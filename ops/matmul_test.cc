// Copyright 2023 Google LLC
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// End to end test of MatMul, comparing against a reference implementation.

#include "hwy/detect_compiler_arch.h"
#ifndef HWY_DISABLED_TARGETS
// Exclude HWY_SCALAR due to 2x bf16 -> f32, and Armv7 NEON because we require
// double-precision support.
#if HWY_ARCH_ARM_V7
#define HWY_DISABLED_TARGETS (HWY_SCALAR | HWY_NEON)
#else
#define HWY_DISABLED_TARGETS HWY_SCALAR
#endif
#endif

#include <stddef.h>
#include <stdio.h>

#include <memory>

#include "compression/compress.h"
#include "compression/shared.h"
#include "ops/matmul.h"
#include "util/allocator.h"
#include "util/basics.h"
#include "util/threading.h"
#include "hwy/base.h"
#include "hwy/contrib/thread_pool/thread_pool.h"
#include "hwy/timer.h"

// clang-format off
#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "ops/matmul_test.cc"  // NOLINT
// clang-format on
#include "hwy/foreach_target.h"  // IWYU pragma: keep
#include "hwy/highway.h"
// After highway.h
#include "compression/compress-inl.h"
#include "ops/dot-inl.h"
#include "ops/matmul-inl.h"
#include "hwy/tests/test_util-inl.h"

HWY_BEFORE_NAMESPACE();
namespace gcpp {
// For running TestBatchSizes only once. Defined within HWY_ONCE.
extern int64_t first_target;

namespace HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

using FloatPtr = hwy::AlignedFreeUniquePtr<float[]>;

template <typename MatT>
using MatStoragePtr = std::unique_ptr<MatStorageT<MatT>>;

// Generates inputs: deterministic, within max SfpStream range.
template <typename MatT>
MatStoragePtr<MatT> GenerateMat(const Extents2D extents,
                                hwy::ThreadPool& pool) {
  gcpp::CompressWorkingSet ws;
  auto mat =
      std::make_unique<MatStorageT<MatT>>("mat", extents.rows, extents.cols);
  FloatPtr content = hwy::AllocateAligned<float>(mat->NumElements());
  HWY_ASSERT(content);
  const float scale = SfpStream::kMax / (mat->NumElements());
  pool.Run(0, extents.rows, [&](const size_t r, size_t /*thread*/) {
    for (size_t c = 0; c < extents.cols; c++) {
      float f = static_cast<float>(r * extents.cols + c) * scale;
      if ((r + c) & 1) f = -f;  // Also generate some negative values.
      content[r * extents.cols + c] = f;
    }
  });

  CompressScaled(content.get(), mat->NumElements(), ws, *mat, pool);
  mat->set_scale(0.6f);  // Arbitrary value, different from 1.
  return mat;
}

// extents describes the transposed matrix.
template <typename MatT>
MatStoragePtr<MatT> GenerateTransposedMat(const Extents2D extents,
                                          hwy::ThreadPool& pool) {
  gcpp::CompressWorkingSet ws;
  auto mat =
      std::make_unique<MatStorageT<MatT>>("trans", extents.rows, extents.cols);
  FloatPtr content = hwy::AllocateAligned<float>(mat->NumElements());
  const float scale = SfpStream::kMax / (mat->NumElements());
  pool.Run(0, extents.rows, [&](const size_t r, size_t /*thread*/) {
    for (size_t c = 0; c < extents.cols; c++) {
      float f = static_cast<float>(c * extents.rows + r) * scale;
      if ((r + c) & 1) f = -f;  // Also generate some negative values.
      content[r * extents.cols + c] = f;
    }
  });

  CompressScaled(content.get(), mat->NumElements(), ws, *mat, pool);
  // Arbitrary value, different from 1, must match GenerateMat.
  mat->set_scale(0.6f);
  return mat;
}

// Returns 1-norm, used for estimating tolerable numerical differences.
double MaxRowAbsSum(const float* HWY_RESTRICT a, const Extents2D& extents) {
  double max_row_abs_sum = 0.0;
  for (size_t r = 0; r < extents.rows; r++) {
    const float* row = a + r * extents.cols;
    double row_abs_sum = 0.0;
    for (size_t c = 0; c < extents.cols; c++) {
      row_abs_sum += hwy::ScalarAbs(row[c]);
    }
    max_row_abs_sum = HWY_MAX(max_row_abs_sum, row_abs_sum);
  }
  return max_row_abs_sum;
}

// Returns the maximum absolute value of `a`.
float MaxAbs(const float* HWY_RESTRICT a, const Extents2D& extents) {
  float max_abs = 0.0f;
  for (size_t c = 0; c < extents.cols; c++) {
    for (size_t r = 0; r < extents.rows; r++) {
      max_abs = HWY_MAX(max_abs, hwy::ScalarAbs(a[r * extents.cols + c]));
    }
  }
  return max_abs;
}

// B is already transposed.
template <typename MatTA, typename MatTB>
void AssertClose(const ConstMat<MatTA>& A, const ConstMat<MatTB>& B,
                 const RowPtrF& C_slow, const RowPtrF& C) {
  const hn::ScalableTag<float> df;
  const size_t num_a = A.extents.Area();
  const size_t num_b = B.extents.Area();
  HWY_ASSERT(num_a % hn::Lanes(df) == 0);  // for DecompressAndZeroPad
  HWY_ASSERT(num_b % hn::Lanes(df) == 0);  // for DecompressAndZeroPad
  FloatPtr a = hwy::AllocateAligned<float>(num_a);
  FloatPtr b_trans = hwy::AllocateAligned<float>(num_b);
  HWY_ASSERT(a && b_trans);
  HWY_ASSERT(A.ofs == 0 && B.ofs == 0);
  DecompressAndZeroPad(df, MakeSpan(A.ptr, num_a), 0, a.get(), num_a);
  DecompressAndZeroPad(df, MakeSpan(B.ptr, num_b), 0, b_trans.get(), num_b);

  // MatMul rounds inputs to BF16, so error is proportional to the max input
  // magnitude, but also to f32 accumulation of rows in A and B.
  const double norm = MaxRowAbsSum(a.get(), A.Extents()) *
                      MaxRowAbsSum(b_trans.get(), B.Extents());
  const float max_abs =
      MaxAbs(a.get(), A.Extents()) * MaxAbs(b_trans.get(), B.Extents());
  const double eps_bf16 = hwy::ConvertScalarTo<double>(hwy::Epsilon<BF16>());
  const double eps_f32 = hwy::ConvertScalarTo<double>(hwy::Epsilon<float>());
  double tolerance = 8 * norm * eps_f32;
  // Dot() also rounds F32,BF16 to BF16, but not with F32,F32, so increase the
  // tolerance there.
  if (IsF32<MatTA>() && IsF32<MatTB>()) {
    tolerance += 4 * max_abs * eps_bf16;
  }
  EXPECT_GE(tolerance, 1E-4);
  if (tolerance > 4.0) {
    fprintf(stderr, "WARN: high tolerance %f norm %f maxabs %f\n", tolerance,
            norm, max_abs);
  }

  for (size_t r = 0; r < A.extents.rows; r++) {
    const float* expected_row = C_slow.Row(r);
    const float* actual_row = C.Row(r);
    for (size_t c = 0; c < B.extents.rows; c++) {
      const double expected_value = static_cast<double>(expected_row[c]);
      const double actual_value = static_cast<double>(actual_row[c]);

      if (!(expected_value - tolerance <= actual_value &&
            actual_value <= expected_value + tolerance)) {
        fprintf(stderr,
                "(%zu,%zu): expected %f, actual %f, norm %f maxabs %f "
                "tolerance %f\n",
                r, c, expected_value, actual_value, norm, max_abs, tolerance);
        return;
      }
    }
  }
}

// B is already transposed.
template <typename MatTA, typename MatTB>
HWY_INLINE void MatMulSlow(const ConstMat<MatTA> A, const ConstMat<MatTB> B,
                           const float* HWY_RESTRICT add_row, MatMulEnv& env,
                           const RowPtrF& C) {
  // MatTA can be any Packed except NuqStream because it uses pointer
  // arithmetic, because it is the second argument to Dot, which does not
  // support a v_ofs.
  static_assert(sizeof(MatTA) >= sizeof(BF16), "A matrix must be BF16/f32");
  const float scale = A.scale * B.scale;

  const hn::ScalableTag<float> df;  // lane type is ignored
  const PackedSpan<const MatTB> b_span =
      MakeSpan(B.ptr, B.ofs + B.extents.Area());
  const IndexRange all_rows_c(0, A.Extents().rows);
  const IndexRange all_cols_c(0, C.Cols());

  NestedPools& pools = env.Pools();
  hwy::ThreadPool& all_packages = pools.AllPackages();
  const IndexRangePartition get_row_c =
      StaticPartition(all_rows_c, all_packages.NumWorkers(), 1);
  ParallelizeOneRange(
      get_row_c, all_packages,
      [&](const IndexRange& rows_c, size_t package_idx) HWY_ATTR {
        hwy::ThreadPool& all_clusters = pools.AllClusters(package_idx);
        const size_t multiple = Allocator::Alignment() / sizeof(MatTB);
        const IndexRangePartition get_col_c =
            StaticPartition(all_cols_c, all_clusters.NumWorkers(), multiple);
        ParallelizeOneRange(
            get_col_c, all_clusters,
            [&](const IndexRange& cols_c, size_t cluster_idx) HWY_ATTR {
              for (size_t r : rows_c) {
                float* HWY_RESTRICT C_row = C.Row(r);
                for (size_t c : cols_c) {
                  const float add = add_row ? add_row[c] : 0.0f;
                  C_row[c] =
                      add + scale * Dot(df, b_span, c * B.extents.cols,
                                        A.ptr + A.Row(r), A.extents.cols);
                }
              }
            });
      });
}

void PrintSpeed(const char* algo, const Extents2D& A_extents,
                const Extents2D& B_extents, double elapsed) {
  const size_t num_b = B_extents.Area();
  // 2x because of FMA.
  fprintf(stderr, "                     %10s: %f seconds, %.1f GFLOPS.\n", algo,
          elapsed, 2 * 1E-9 * A_extents.rows * num_b / elapsed);
}

template <typename MatTA, typename MatTB = MatTA>
void TestMatMul(size_t rows_ac, size_t cols_a_rows_b, size_t cols_bc, bool add,
                MatMulEnv& env) {
  hwy::ThreadPool& pool = env.Pool();
  const bool want_bench = cols_bc > 2000;  // avoid spam for small matrices
  fprintf(stderr, "TestMatMul %lu, %lu, %lu, add=%d, MatTA=%s, MatTB=%s\n",
          rows_ac, cols_a_rows_b, cols_bc, add, TypeName<MatTA>(),
          TypeName<MatTB>());

  const Extents2D A_extents(rows_ac, cols_a_rows_b);
  const Extents2D B_extents(cols_bc, cols_a_rows_b);  // already transposed
  const Extents2D C_extents(rows_ac, cols_bc);

  MatStoragePtr<MatTA> a = GenerateMat<MatTA>(A_extents, pool);
  MatStoragePtr<MatTB> b_trans = GenerateTransposedMat<MatTB>(B_extents, pool);
  RowVectorBatch<float> c_slow_batch(C_extents);
  RowVectorBatch<float> c_batch(C_extents);
  HWY_ASSERT(a && b_trans);

  std::unique_ptr<MatStorageT<float>> add_storage;
  if (add) {
    add_storage = GenerateMat<float>(Extents2D(1, cols_bc), pool);
    HWY_ASSERT(add_storage);
    add_storage->set_scale(1.0f);
  }

  const auto A = ConstMatFromWeights(*a);
  const auto B = ConstMatFromWeights(*b_trans);
  const float* add_row = add ? add_storage->data_scale1() : nullptr;
  const RowPtrF C_slow = RowPtrFromBatch(c_slow_batch);
  const RowPtrF C = RowPtrFromBatch(c_batch);

  const double start_slow = hwy::platform::Now();
  MatMulSlow(A, B, add_row, env, C_slow);
  if (want_bench) {
    PrintSpeed("MatMulSlow", A_extents, B_extents,
               hwy::platform::Now() - start_slow);
  }

  double min_elapsed = hwy::HighestValue<double>();
  for (int rep = 0; rep < (want_bench ? 3 : 1); ++rep) {
    const double start_tiled = hwy::platform::Now();
    MatMul(A, B, add_row, env, C);
    min_elapsed = HWY_MIN(min_elapsed, hwy::platform::Now() - start_tiled);
  }
  if (want_bench) {
    PrintSpeed("MatMul", A_extents, B_extents, min_elapsed);
  }

  AssertClose(A, B, C_slow, C);
}

using F32 = float;
using SFP = SfpStream;

// Sweep batch_size for a single input type and Highway target, to verify the
// row partitioning.
void TestBatchSizes() {
  if (first_target == 0) first_target = HWY_TARGET;
  if (HWY_TARGET != first_target) return;

  for (size_t max_packages : {1, 2}) {
    const size_t max_threads = 0;  // no limit
    NestedPools pools(max_threads, Tristate::kDefault,
                      BoundedSlice(0, max_packages));
#if GEMMA_DISABLE_TOPOLOGY
    if (max_packages == 2) break;  // we only have one package
#else
    // If less than the limit, we have already tested all num_packages.
    if (pools.Topology().FullTopology().packages.size() < max_packages) break;
#endif
    fprintf(stderr, "TestBatchSizes %zu: %s %s\n", max_packages,
            pools.TopologyString(), pools.PinString());

    Tristate use_spinning = Tristate::kDefault;
    pools.MaybeStartSpinning(use_spinning);
    Allocator::Init(pools.Topology());
    MatMulEnv env(pools);

    for (size_t batch_size = 1; batch_size <= 3 * kRegRows; ++batch_size) {
      TestMatMul<F32, F32>(batch_size, 256, 256, /*add=*/false, env);
    }
    pools.MaybeStopSpinning(use_spinning);
  }
}

void TestAllMatMul() {
  // Skip EMU128 (10x slower than SSE4 for SFP) and older x86.
  if (HWY_TARGET == HWY_EMU128 || HWY_TARGET == HWY_SSE4 ||
      HWY_TARGET == HWY_SSSE3 || HWY_TARGET == HWY_SSE2) {
    return;
  }

  NestedPools pools(0);  // no limits
  Tristate use_spinning = Tristate::kDefault;
  pools.MaybeStartSpinning(use_spinning);
  Allocator::Init(pools.Topology());
  MatMulEnv env(pools);

  // Sizes seen in gemma_test 2B.
  TestMatMul<F32>(1, 2048, 512, /*add=*/false, env);
  TestMatMul<F32>(1, 2048, 2048, /*add=*/false, env);
  TestMatMul<F32>(1, 2048, 16384, /*add=*/false, env);
  TestMatMul<F32>(1, 16384, 2048, /*add=*/false, env);
  TestMatMul<F32>(1, 2048, 256000, /*add=*/false, env);
  TestMatMul<F32>(5, 2048, 512, /*add=*/false, env);
  TestMatMul<F32>(5, 2048, 2048, /*add=*/false, env);
  TestMatMul<F32>(5, 2048, 16384, /*add=*/false, env);
  TestMatMul<F32>(5, 16384, 2048, /*add=*/false, env);

  // medium-sized square
  TestMatMul<F32>(512, 512, 512, /*add=*/false, env);
  TestMatMul<BF16>(512, 512, 512, /*add=*/true, env);
  TestMatMul<F32, BF16>(512, 512, 512, /*add=*/false, env);
  TestMatMul<BF16, F32>(512, 512, 512, /*add=*/true, env);
  TestMatMul<F32, SFP>(512, 512, 512, /*add=*/false, env);
  TestMatMul<BF16, SFP>(512, 512, 512, /*add=*/true, env);

  // minimal non-square test. kColsARowsB must be at least 2 vectors.
  TestMatMul<F32>(35, 128, 32, /*add=*/false, env);
  TestMatMul<BF16>(34, 128, 32, /*add=*/true, env);
  TestMatMul<F32, BF16>(33, 128, 32, /*add=*/false, env);
  TestMatMul<BF16, F32>(33, 128, 32, /*add=*/true, env);
  TestMatMul<F32, SFP>(31, 128, 32, /*add=*/false, env);
  TestMatMul<BF16, SFP>(29, 128, 32, /*add=*/true, env);
  TestMatMul<F32>(4, 128, 32, /*add=*/true, env);
  TestMatMul<BF16>(4, 128, 32, /*add=*/false, env);
  TestMatMul<F32, BF16>(4, 128, 32, /*add=*/true, env);
  TestMatMul<BF16, F32>(4, 128, 32, /*add=*/false, env);
  TestMatMul<F32, SFP>(4, 128, 32, /*add=*/true, env);
  TestMatMul<BF16, SFP>(4, 128, 32, /*add=*/false, env);
  TestMatMul<F32>(3, 128, 32, /*add=*/false, env);
  TestMatMul<BF16>(3, 128, 32, /*add=*/true, env);
  TestMatMul<F32, BF16>(3, 128, 32, /*add=*/false, env);
  TestMatMul<BF16, F32>(3, 128, 32, /*add=*/true, env);
  TestMatMul<F32, SFP>(3, 128, 32, /*add=*/false, env);
  TestMatMul<BF16, SFP>(3, 128, 32, /*add=*/true, env);
  TestMatMul<F32>(2, 128, 64, /*add=*/true, env);
  TestMatMul<BF16>(2, 128, 64, /*add=*/false, env);
  TestMatMul<F32, BF16>(2, 128, 64, /*add=*/true, env);
  TestMatMul<BF16, F32>(2, 128, 64, /*add=*/false, env);
  TestMatMul<F32, SFP>(2, 128, 64, /*add=*/true, env);
  TestMatMul<BF16, SFP>(2, 128, 64, /*add=*/false, env);
  TestMatMul<F32>(1, 128, 32, /*add=*/false, env);
  TestMatMul<BF16>(1, 128, 32, /*add=*/true, env);
  TestMatMul<F32, BF16>(1, 128, 32, /*add=*/false, env);
  TestMatMul<BF16, F32>(1, 128, 32, /*add=*/true, env);
  TestMatMul<F32, SFP>(1, 128, 32, /*add=*/false, env);
  TestMatMul<BF16, SFP>(1, 128, 32, /*add=*/true, env);
}

// NOLINTNEXTLINE(google-readability-namespace-comments)
}  // namespace HWY_NAMESPACE
}  // namespace gcpp
HWY_AFTER_NAMESPACE();

#if HWY_ONCE

namespace gcpp {
int64_t first_target = 0;  // none run yet
HWY_BEFORE_TEST(MatMulTest);
HWY_EXPORT_AND_TEST_P(MatMulTest, TestBatchSizes);
HWY_EXPORT_AND_TEST_P(MatMulTest, TestAllMatMul);
HWY_AFTER_TEST();

}  // namespace gcpp

#endif
