/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include <string>
#include <utility>

#include "absl/strings/str_replace.h"
#include "tensorflow/compiler/xla/service/gpu/gemm_rewriter.h"
#include "tensorflow/compiler/xla/service/gpu/gpu_executable.h"
#include "tensorflow/compiler/xla/service/gpu/stream_executor_util.h"
#include "tensorflow/compiler/xla/service/gpu/tests/gpu_codegen_test.h"
#include "tensorflow/compiler/xla/service/hlo_instruction.h"
#include "tensorflow/compiler/xla/service/hlo_module.h"
#include "tensorflow/compiler/xla/service/hlo_module_config.h"
#include "tensorflow/compiler/xla/service/hlo_parser.h"
#include "tensorflow/compiler/xla/service/pattern_matcher.h"
#include "tensorflow/compiler/xla/service/pattern_matcher_gmock.h"
#include "tensorflow/compiler/xla/statusor.h"
#include "tensorflow/compiler/xla/stream_executor/lib/statusor.h"
#include "tensorflow/compiler/xla/tests/filecheck.h"
#include "tensorflow/compiler/xla/tests/hlo_test_base.h"
#include "tensorflow/compiler/xla/xla.pb.h"
#include "tensorflow/tsl/lib/core/status_test_util.h"
#include "tensorflow/tsl/platform/test.h"

namespace xla {
namespace gpu {

namespace {

namespace m = ::xla::match;

class GemmRewriteTest : public GpuCodegenTest {
 public:
  se::CudaComputeCapability GetCudaComputeCapability() {
    return backend()
        .default_stream_executor()
        ->GetDeviceDescription()
        .cuda_compute_capability();
  }
};

TEST_F(GemmRewriteTest, CheckCustomCallTarget) {
  const char* hlo_text = R"(
HloModule SimpleGemm

ENTRY AddDotsFunc {
  x = f32[2,3] parameter(0)
  y = f32[3,4] parameter(1)
  ROOT dot_a = f32[2,4] dot(x, y), lhs_contracting_dims={1}, rhs_contracting_dims={0}
}

)";

  DebugOptions debug_options = GetDebugOptionsForTest();
  if (debug_options.xla_gpu_enable_cublaslt()) {
    MatchOptimizedHlo(hlo_text,
                      R"(; CHECK: custom_call_target="__cublas$lt$matmul")");
  } else {
    MatchOptimizedHlo(hlo_text,
                      R"(; CHECK: custom_call_target="__cublas$gemm")");
  }
}

TEST_F(GemmRewriteTest, TestBatchedAutotuning) {
  if (GetCudaComputeCapability().IsAtLeast(se::CudaComputeCapability::AMPERE)) {
    GTEST_SKIP()
        << "There is no autotuning starting with the Nvidia Ampere generation";
  }
  const char* hlo_text = R"(
HloModule ComplexDotMultipleNonContracting

ENTRY %test {
  %lhs = f32[7,17,10,13]{3,2,1,0} parameter(0)
  %rhs = f32[7,9,10,13,6]{4,3,2,1,0} parameter(1)
  ROOT %dot = f32[10,7,17,9,6]{4,3,2,1,0} dot(%lhs, %rhs), lhs_batch_dims={2,0}, rhs_batch_dims={2,0}, lhs_contracting_dims={3}, rhs_contracting_dims={3}
}

)";

  MatchOptimizedHlo(hlo_text,
                    R"(
; CHECK: selected_algorithm
      )");
}

TEST_F(GemmRewriteTest, SimpleRewriteDeterministic) {
  const char* hlo_text = R"(
HloModule SimpleGemm

ENTRY AddDotsFunc {
  x = f32[128,128] parameter(0)
  y = f32[128,128] parameter(1)
  ROOT dot_a = f32[128,128] dot(x, y), lhs_contracting_dims={1}, rhs_contracting_dims={0}
}
)";

  ErrorSpec error_spec = [&] {
    DebugOptions debug_options = GetDebugOptionsForTest();
    if (debug_options.xla_gpu_enable_cublaslt()) {
      return ErrorSpec{1e-3, 1e-5};
    } else {
      return ErrorSpec{1e-5, 1e-5};
    }
  }();

  auto get_module = [&]() {
    HloModuleConfig config;
    DebugOptions debug_options = GetDebugOptionsForTest();
    debug_options.set_xla_gpu_deterministic_ops(true);
    config.set_debug_options(debug_options);
    return ParseAndReturnVerifiedModule(hlo_text, config);
  };

  TF_ASSERT_OK_AND_ASSIGN(
      std::unique_ptr<HloModule> optimized_module,
      backend().compiler()->RunHloPasses(
          *get_module(), backend().default_stream_executor(),
          backend().default_stream_executor()->GetAllocator()));

  StatusOr<bool> filecheck_result = RunFileCheck(optimized_module->ToString(),
                                                 R"(
; CHECK:    custom_call_target="__cublas${{(lt\$matmul|gemm)}}"
    )");
  TF_ASSERT_OK(filecheck_result.status());
  EXPECT_TRUE(filecheck_result.value());
  EXPECT_TRUE(RunAndCompare(*get_module(), error_spec));
}

TEST_F(GemmRewriteTest, BF16GemmCodeGen) {
  const char* hlo_text = R"(
HloModule bf16codegendgemm

ENTRY bf16gemm {
  %parameter.1 = bf16[3]{0} parameter(0)
  %parameter.2 = bf16[3]{0} parameter(1)
  ROOT %dot.3 = bf16[] dot(bf16[3]{0} %parameter.1, bf16[3]{0} %parameter.2), lhs_contracting_dims={0}, rhs_contracting_dims={0}, operand_precision={highest,highest}
}
  )";

  MatchOptimizedHlo(hlo_text, R"(
; CHECK:  [[P1:%[^ ]+]] = bf16[3]{0} parameter(1)
; CHECK:  [[INSTR_1:%[^ ]+]] = f32[3]{0} convert([[P1]])
; CHECK:  [[P0:%[^ ]+]] = bf16[3]{0} parameter(0)
; CHECK:  [[INSTR_3:%[^ ]+]] = f32[3]{0} convert([[P0]])
; CHECK:  [[INSTR_4:%[^ ]+]] = f32[3]{0} multiply([[INSTR_1]], [[INSTR_3]])
; CHECK:  [[INSTR_5:%[^ ]+]] = f32[] constant(0)
; CHECK:  [[INSTR_6:%[^ ]+]] = f32[] reduce([[INSTR_4]], [[INSTR_5]]), dimensions={0}, to_apply=[[INSTR_7:%[^ ]+]]
; CHECK:  ROOT [[INSTR_8:%[^ ]+]] = bf16[] convert([[INSTR_6]])
  )");

  EXPECT_TRUE(RunAndCompare(hlo_text, ErrorSpec{1e-5, 1e-5}));
}

TEST_F(GemmRewriteTest, BF16Transpose) {
  const char* hlo_text = R"(
HloModule broadcast

ENTRY broadcast {
  p = bf16[9] parameter(0)
  ROOT out = bf16[1,9] broadcast(p), dimensions={1}
}
)";

  MatchOptimizedHlo(hlo_text, R"(
; CHECK: bf16[1,9]{1,0} bitcast
; CHECK: bf16[1,9]{1,0} copy
)");

  EXPECT_TRUE(RunAndCompare(hlo_text, ErrorSpec{1e-5, 1e-5}));
}

// A test fixture class for tests which should have similar results with legacy
// cublas and cublasLt
class ParameterizedGemmRewriteTest
    : public GemmRewriteTest,
      public ::testing::WithParamInterface<bool> {
 public:
  ParameterizedGemmRewriteTest() {
    const bool kUsingCublasLt = GetParam();
    replacements_[kCustomCallTargetPlaceholder] =
        kUsingCublasLt ? "__cublas$lt$matmul" : "__cublas$gemm";
  }
  DebugOptions GetDebugOptionsForTest() override {
    DebugOptions debug_options = GemmRewriteTest::GetDebugOptionsForTest();
    debug_options.set_xla_gpu_enable_cublaslt(GetParam());
    return debug_options;
  }
  void MatchOptimizedHlo(absl::string_view hlo, const absl::string_view pattern,
                         bool print_operand_shape = false) {
    GemmRewriteTest::MatchOptimizedHlo(
        hlo, absl::StrReplaceAll(pattern, replacements_), print_operand_shape);
  }

 private:
  static constexpr const char* kCustomCallTargetPlaceholder{
      "<<CUBLAS_CUSTOM_CALL_TARGET_PLACEHOLDER>>"};
  absl::flat_hash_map<absl::string_view, absl::string_view> replacements_;
};

TEST_P(ParameterizedGemmRewriteTest, Simple) {
  const char* hlo_text = R"(
HloModule test

ENTRY test {
  x = f32[2,3] parameter(0)
  y = f32[3,4] parameter(1)
  ROOT dot_a = f32[2,4] dot(x, y), lhs_contracting_dims={1}, rhs_contracting_dims={0}
}

)";

  EXPECT_TRUE(RunAndCompare(hlo_text, ErrorSpec{1e-5, 1e-5}));
  MatchOptimizedHlo(hlo_text,
                    R"(
; CHECK-LABEL: ENTRY %test (x: f32[2,3], y: f32[3,4]) -> f32[2,4] {
; CHECK-NEXT:    [[P0:%[^ ]+]] = f32[2,3]{1,0} parameter(0)
; CHECK-NEXT:    [[P1:%[^ ]+]] = f32[3,4]{1,0} parameter(1)
; CHECK-NEXT:    ROOT [[OUT:%[^ ]+]] = f32[2,4]{1,0} custom-call([[P0]], [[P1]]),
; CHECK:           custom_call_target="<<CUBLAS_CUSTOM_CALL_TARGET_PLACEHOLDER>>",
; CHECK:           backend_config="{
; CHECK-DAG:         \"alpha_real\":1
; CHECK-DAG:         \"alpha_imag\":0
; CHECK-DAG:         \"beta\":0
; CHECK-DAG:         \"dot_dimension_numbers\":{
; CHECK-DAG:           \"lhs_contracting_dimensions\":[\"1\"]
; CHECK-DAG:           \"rhs_contracting_dimensions\":[\"0\"]
; CHECK-DAG:           \"lhs_batch_dimensions\":[]
; CHECK-DAG:           \"rhs_batch_dimensions\":[]
; CHECK-DAG:         }
; CHECK-DAG:         \"precision_config\":{
; CHECK-DAG:           \"operand_precision\":[\"DEFAULT\",\"DEFAULT\"]
; CHECK-DAG:         }
; CHECK-DAG:         \"epilogue\":\"DEFAULT\"
; CHECK:           }"
)");
}

TEST_P(ParameterizedGemmRewriteTest, SimpleRewrite) {
  const char* hlo_text = R"(
HloModule SimpleGemm

ENTRY AddDotsFunc {
  x = f32[2,3] parameter(0)
  y = f32[3,4] parameter(1)
  ROOT dot_a = f32[2,4] dot(x, y), lhs_contracting_dims={1}, rhs_contracting_dims={0}
}

)";

  EXPECT_TRUE(RunAndCompare(hlo_text, ErrorSpec{1e-5, 1e-5}));
  MatchOptimizedHlo(hlo_text,
                    R"(
; CHECK-LABEL: ENTRY %AddDotsFunc (x: f32[2,3], y: f32[3,4]) -> f32[2,4] {
; CHECK-NEXT:    [[P0:%[^ ]+]] = f32[2,3]{1,0} parameter(0)
; CHECK-NEXT:    [[P1:%[^ ]+]] = f32[3,4]{1,0} parameter(1)
; CHECK-NEXT:    ROOT [[OUT:%[^ ]+]] = f32[2,4]{1,0} custom-call([[P0]], [[P1]]),
; CHECK:           custom_call_target="<<CUBLAS_CUSTOM_CALL_TARGET_PLACEHOLDER>>",
; CHECK:           backend_config="{
; CHECK-DAG:         \"alpha_real\":1
; CHECK-DAG:         \"alpha_imag\":0
; CHECK-DAG:         \"beta\":0
; CHECK-DAG:         \"dot_dimension_numbers\":{
; CHECK-DAG:           \"lhs_contracting_dimensions\":[\"1\"]
; CHECK-DAG:           \"rhs_contracting_dimensions\":[\"0\"]
; CHECK-DAG:           \"lhs_batch_dimensions\":[]
; CHECK-DAG:           \"rhs_batch_dimensions\":[]
; CHECK-DAG:         }
; CHECK-DAG:         \"precision_config\":{
; CHECK-DAG:           \"operand_precision\":[\"DEFAULT\",\"DEFAULT\"]
; CHECK-DAG:         }
; CHECK-DAG:         \"epilogue\":\"DEFAULT\"
; CHECK:           }"
)");
}

TEST_P(ParameterizedGemmRewriteTest, MultipleContractingDims) {
  const char* hlo_text = R"(
HloModule MultipleContractingCheckGemm

ENTRY AddDotsFunc {
  x = f32[3,4,2] parameter(0)
  y = f32[3,4,5] parameter(1)
  ROOT dot_a = f32[2,5] dot(x, y), lhs_contracting_dims={0,1}, rhs_contracting_dims={0,1}
}

)";

  EXPECT_TRUE(RunAndCompare(hlo_text, ErrorSpec{1e-5, 1e-5}));
  MatchOptimizedHlo(hlo_text,
                    R"(
; CHECK-NOT:     copy
;
; CHECK-LABEL: ENTRY %AddDotsFunc (x: f32[3,4,2], y: f32[3,4,5]) -> f32[2,5] {
; CHECK-NEXT:    [[P0:%[^ ]+]] = f32[3,4,2]{2,1,0} parameter(0)
; CHECK-DAG:     [[P1:%[^ ]+]] = f32[3,4,5]{2,1,0} parameter(1)
; CHECK-DAG:     [[BITCAST0:%[^ ]+]] = f32[2,12]{0,1} bitcast([[P0]])
; CHECK-DAG:     [[BITCAST1:%[^ ]+]] = f32[12,5]{1,0} bitcast([[P1]])
; CHECK-NEXT:    ROOT [[OUT:%[^ ]+]] = f32[2,5]{1,0} custom-call([[BITCAST0]], [[BITCAST1]]),
; CHECK:           custom_call_target="<<CUBLAS_CUSTOM_CALL_TARGET_PLACEHOLDER>>",
; CHECK:           backend_config="{
; CHECK-DAG:         \"alpha_real\":1
; CHECK-DAG:         \"alpha_imag\":0
; CHECK-DAG:         \"beta\":0
; CHECK-DAG:         \"dot_dimension_numbers\":{
; CHECK-DAG:           \"lhs_contracting_dimensions\":[\"1\"]
; CHECK-DAG:           \"rhs_contracting_dimensions\":[\"0\"]
; CHECK-DAG:           \"lhs_batch_dimensions\":[]
; CHECK-DAG:           \"rhs_batch_dimensions\":[]
; CHECK-DAG:         }
; CHECK-DAG:         \"precision_config\":{
; CHECK-DAG:           \"operand_precision\":[\"DEFAULT\",\"DEFAULT\"]
; CHECK-DAG:         }
; CHECK-DAG:         \"epilogue\":\"DEFAULT\"
; CHECK:           }"
)");
}

TEST_P(ParameterizedGemmRewriteTest, ArgTransposeFoldCheck) {
  const char* hlo_text = R"(
HloModule ArgTransposeFoldGemm

ENTRY AddDotsFunc {
  x = f32[3,2] parameter(0)
  y = f32[3,4] parameter(1)
  x_transposed = f32[2,3] transpose(x), dimensions={1, 0}
  ROOT dot_a = f32[2,4] dot(x_transposed, y), lhs_contracting_dims={1}, rhs_contracting_dims={0}
}

)";

  EXPECT_TRUE(RunAndCompare(hlo_text, ErrorSpec{1e-5, 1e-5}));
  MatchOptimizedHlo(hlo_text,
                    R"(
; CHECK-LABEL: ENTRY %AddDotsFunc (x: f32[3,2], y: f32[3,4]) -> f32[2,4] {
; CHECK-NEXT:    [[P0:%[^ ]+]] = f32[3,2]{1,0} parameter(0)
; CHECK-NEXT:    [[P1:%[^ ]+]] = f32[3,4]{1,0} parameter(1)
; CHECK-NEXT:    ROOT [[OUT:%[^ ]+]] = f32[2,4]{1,0} custom-call([[P0]], [[P1]]),
; CHECK:           custom_call_target="<<CUBLAS_CUSTOM_CALL_TARGET_PLACEHOLDER>>",
; CHECK:           backend_config="{
; CHECK-DAG:         \"alpha_real\":1
; CHECK-DAG:         \"alpha_imag\":0
; CHECK-DAG:         \"beta\":0
; CHECK-DAG:         \"dot_dimension_numbers\":{
; CHECK-DAG:           \"lhs_contracting_dimensions\":[\"0\"]
; CHECK-DAG:           \"rhs_contracting_dimensions\":[\"0\"]
; CHECK-DAG:           \"lhs_batch_dimensions\":[]
; CHECK-DAG:           \"rhs_batch_dimensions\":[]
; CHECK-DAG:         }
; CHECK-DAG:         \"precision_config\":{
; CHECK-DAG:           \"operand_precision\":[\"DEFAULT\",\"DEFAULT\"]
; CHECK-DAG:         }
; CHECK-DAG:         \"epilogue\":\"DEFAULT\"
; CHECK:           }"
)");
}

TEST_P(ParameterizedGemmRewriteTest, BatchedArgRowColTransposeFoldCheck) {
  const char* hlo_text = R"(
HloModule BatchedArgRowColTransposeFoldGemm

ENTRY AddDotsFunc {
  x = f32[5,3,2] parameter(0)
  y = f32[5,3,4] parameter(1)
  x_transposed = f32[5,2,3] transpose(x), dimensions={0, 2, 1}
  ROOT dot_a = f32[5,2,4] dot(x_transposed, y), lhs_contracting_dims={2}, rhs_contracting_dims={1}, lhs_batch_dims={0}, rhs_batch_dims={0}
}

)";

  EXPECT_TRUE(RunAndCompare(hlo_text, ErrorSpec{1e-5, 1e-5}));
  MatchOptimizedHlo(hlo_text,
                    R"(
; CHECK-LABEL: ENTRY %AddDotsFunc (x: f32[5,3,2], y: f32[5,3,4]) -> f32[5,2,4] {
; CHECK-NEXT:    [[P0:%[^ ]+]] = f32[5,3,2]{2,1,0} parameter(0)
; CHECK-NEXT:    [[P1:%[^ ]+]] = f32[5,3,4]{2,1,0} parameter(1)
; CHECK-NEXT:    ROOT [[OUT:%[^ ]+]] = f32[5,2,4]{2,1,0} custom-call([[P0]], [[P1]]),
; CHECK:           custom_call_target="<<CUBLAS_CUSTOM_CALL_TARGET_PLACEHOLDER>>",
; CHECK:           backend_config="{
; CHECK-DAG:         \"alpha_real\":1
; CHECK-DAG:         \"alpha_imag\":0
; CHECK-DAG:         \"beta\":0
; CHECK-DAG:         \"dot_dimension_numbers\":{
; CHECK-DAG:           \"lhs_contracting_dimensions\":[\"1\"]
; CHECK-DAG:           \"rhs_contracting_dimensions\":[\"1\"]
; CHECK-DAG:           \"lhs_batch_dimensions\":[\"0\"]
; CHECK-DAG:           \"rhs_batch_dimensions\":[\"0\"]
; CHECK-DAG:         }
; CHECK-DAG:         \"precision_config\":{
; CHECK-DAG:           \"operand_precision\":[\"DEFAULT\",\"DEFAULT\"]
; CHECK-DAG:         }
; CHECK-DAG:         \"epilogue\":\"DEFAULT\"
; CHECK:           }"
)");
}

TEST_P(ParameterizedGemmRewriteTest, BatchRowTransposeFoldCheck) {
  const char* hlo_text = R"(
HloModule BatchRowTransposeFoldCheck

ENTRY AddDotsFunc {
  x = f32[2,5,3] parameter(0)
  y = f32[5,3,4] parameter(1)
  x_transposed = f32[5,2,3] transpose(x), dimensions={1, 0, 2}
  ROOT dot_a = f32[5,2,4] dot(x_transposed, y), lhs_contracting_dims={2}, rhs_contracting_dims={1}, lhs_batch_dims={0}, rhs_batch_dims={0}
}

)";

  EXPECT_TRUE(RunAndCompare(hlo_text, ErrorSpec{1e-5, 1e-5}));
  MatchOptimizedHlo(hlo_text,
                    R"(
; CHECK-LABEL: ENTRY %AddDotsFunc (x: f32[2,5,3], y: f32[5,3,4]) -> f32[5,2,4] {
; CHECK-NEXT:    [[P0:%[^ ]+]] = f32[2,5,3]{2,1,0} parameter(0)
; CHECK-NEXT:    [[P1:%[^ ]+]] = f32[5,3,4]{2,1,0} parameter(1)
; CHECK-NEXT:    ROOT [[OUT:%[^ ]+]] = f32[5,2,4]{2,1,0} custom-call([[P0]], [[P1]]),
; CHECK:           custom_call_target="<<CUBLAS_CUSTOM_CALL_TARGET_PLACEHOLDER>>",
; CHECK:           backend_config="{
; CHECK-DAG:         \"alpha_real\":1
; CHECK-DAG:         \"alpha_imag\":0
; CHECK-DAG:         \"beta\":0
; CHECK-DAG:         \"dot_dimension_numbers\":{
; CHECK-DAG:           \"lhs_contracting_dimensions\":[\"2\"]
; CHECK-DAG:           \"rhs_contracting_dimensions\":[\"1\"]
; CHECK-DAG:           \"lhs_batch_dimensions\":[\"1\"]
; CHECK-DAG:           \"rhs_batch_dimensions\":[\"0\"]
; CHECK-DAG:         }
; CHECK-DAG:         \"precision_config\":{
; CHECK-DAG:           \"operand_precision\":[\"DEFAULT\",\"DEFAULT\"]
; CHECK-DAG:         }
; CHECK-DAG:         \"epilogue\":\"DEFAULT\"
; CHECK:           }"
)");
}

TEST_P(ParameterizedGemmRewriteTest, BatchFromMinorDimTransposeIsNotFolded) {
  const char* hlo_text = R"(
HloModule BatchFromMinorDimTransposeDoesntFold

ENTRY AddDotsFunc {
  x = f32[3,2,5] parameter(0)
  y = f32[5,3,4] parameter(1)
  x_transposed = f32[5,2,3] transpose(x), dimensions={2, 1, 0}
  ROOT dot_a = f32[5,2,4] dot(x_transposed, y), lhs_contracting_dims={2}, rhs_contracting_dims={1}, lhs_batch_dims={0}, rhs_batch_dims={0}
}

)";

  EXPECT_TRUE(RunAndCompare(hlo_text, ErrorSpec{1e-5, 1e-5}));
  MatchOptimizedHlo(hlo_text,
                    R"(
; CHECK-LABEL: ENTRY %AddDotsFunc (x: f32[3,2,5], y: f32[5,3,4]) -> f32[5,2,4] {
; CHECK-NEXT:    [[P0:%[^ ]+]] = f32[3,2,5]{2,1,0} parameter(0)
; CHECK-DAG:     [[P1:%[^ ]+]] = f32[5,3,4]{2,1,0} parameter(1)
; CHECK-DAG:     [[FUSION:%[^ ]+]] = f32[5,2,3]{2,1,0} fusion([[P0]])
; CHECK-NEXT:    ROOT [[OUT:%[^ ]+]] = f32[5,2,4]{2,1,0} custom-call([[FUSION]], [[P1]]),
; CHECK:           custom_call_target="<<CUBLAS_CUSTOM_CALL_TARGET_PLACEHOLDER>>",
; CHECK:           backend_config="{
; CHECK-DAG:         \"alpha_real\":1
; CHECK-DAG:         \"alpha_imag\":0
; CHECK-DAG:         \"beta\":0
; CHECK-DAG:         \"dot_dimension_numbers\":{
; CHECK-DAG:           \"lhs_contracting_dimensions\":[\"2\"]
; CHECK-DAG:           \"rhs_contracting_dimensions\":[\"1\"]
; CHECK-DAG:           \"lhs_batch_dimensions\":[\"0\"]
; CHECK-DAG:           \"rhs_batch_dimensions\":[\"0\"]
; CHECK-DAG:         }
; CHECK-DAG:         \"precision_config\":{
; CHECK-DAG:           \"operand_precision\":[\"DEFAULT\",\"DEFAULT\"]
; CHECK-DAG:         }
; CHECK-DAG:         \"epilogue\":\"DEFAULT\"
; CHECK:           }"
)");
}

TEST_P(ParameterizedGemmRewriteTest, LargeBatch) {
  const char* hlo_text = R"(
HloModule BatchedArgRowColTransposeFoldGemm

ENTRY AddDotsFunc {
  x = f32[20000,4,3,2] parameter(0)
  y = f32[20000,4,3,4] parameter(1)
  ROOT dot_a = f32[20000,4,2,4] dot(x, y), lhs_contracting_dims={2}, rhs_contracting_dims={2}, lhs_batch_dims={0,1}, rhs_batch_dims={0,1}
}

)";

  // Batch sizes larger than 2^16-1 are not supported by cublasLt. Ensure that
  // the custom_call_target is __cublas$gemm.
  EXPECT_TRUE(RunAndCompare(hlo_text, ErrorSpec{1e-5, 1e-5}));
  MatchOptimizedHlo(hlo_text,
                    R"(
; CHECK-LABEL: ENTRY %AddDotsFunc (x: f32[20000,4,3,2], y: f32[20000,4,3,4]) -> f32[20000,4,2,4] {
; CHECK-NEXT:    [[P0:%[^ ]+]] = f32[20000,4,3,2]{3,2,1,0} parameter(0)
; CHECK-NEXT:    [[P1:%[^ ]+]] = f32[20000,4,3,4]{3,2,1,0} parameter(1)
; CHECK-NEXT:    ROOT [[OUT:%[^ ]+]] = f32[20000,4,2,4]{3,2,1,0} custom-call([[P0]], [[P1]]),
; CHECK:           custom_call_target="__cublas$gemm",
; CHECK:           backend_config="{
; CHECK-DAG:         \"alpha_real\":1
; CHECK-DAG:         \"alpha_imag\":0
; CHECK-DAG:         \"beta\":0
; CHECK-DAG:         \"dot_dimension_numbers\":{
; CHECK-DAG:           \"lhs_contracting_dimensions\":[\"2\"]
; CHECK-DAG:           \"rhs_contracting_dimensions\":[\"2\"]
; CHECK-DAG:           \"lhs_batch_dimensions\":[\"0\",\"1\"]
; CHECK-DAG:           \"rhs_batch_dimensions\":[\"0\",\"1\"]
; CHECK-DAG:         }
; CHECK-DAG:         \"precision_config\":{
; CHECK-DAG:           \"operand_precision\":[\"DEFAULT\",\"DEFAULT\"]
; CHECK-DAG:         }
; CHECK:           }"
)");
}

TEST_P(ParameterizedGemmRewriteTest, InstrTransposeFoldCheck) {
  const char* hlo_text = R"(
HloModule InstrTransposeFoldGemm

ENTRY AddDotsFunc {
  x = f32[2,3] parameter(0)
  y = f32[3,4] parameter(1)
  dot_a = f32[2,4] dot(x, y), lhs_contracting_dims={1}, rhs_contracting_dims={0}
  ROOT out = f32[4,2] transpose(dot_a), dimensions={1, 0}
}

)";

  EXPECT_TRUE(RunAndCompare(hlo_text, ErrorSpec{1e-5, 1e-5}));
  MatchOptimizedHlo(hlo_text,
                    R"(
; CHECK-LABEL: ENTRY %AddDotsFunc (x: f32[2,3], y: f32[3,4]) -> f32[4,2] {
; CHECK-NEXT:    [[P1:%[^ ]+]] = f32[3,4]{1,0} parameter(1)
; CHECK-NEXT:    [[P0:%[^ ]+]] = f32[2,3]{1,0} parameter(0)
; CHECK-NEXT:    ROOT [[OUT:%[^ ]+]] = f32[4,2]{1,0} custom-call([[P1]], [[P0]]),
; CHECK:           custom_call_target="<<CUBLAS_CUSTOM_CALL_TARGET_PLACEHOLDER>>",
; CHECK:           backend_config="{
; CHECK-DAG:         \"alpha_real\":1
; CHECK-DAG:         \"alpha_imag\":0
; CHECK-DAG:         \"beta\":0
; CHECK-DAG:         \"dot_dimension_numbers\":{
; CHECK-DAG:           \"lhs_contracting_dimensions\":[\"0\"]
; CHECK-DAG:           \"rhs_contracting_dimensions\":[\"1\"]
; CHECK-DAG:           \"lhs_batch_dimensions\":[]
; CHECK-DAG:           \"rhs_batch_dimensions\":[]
; CHECK-DAG:         }
; CHECK-DAG:         \"precision_config\":{
; CHECK-DAG:           \"operand_precision\":[\"DEFAULT\",\"DEFAULT\"]
; CHECK-DAG:         }
; CHECK-DAG:         \"epilogue\":\"DEFAULT\"
; CHECK:           }"
)");
}

TEST_P(ParameterizedGemmRewriteTest, BatchedInstrLayoutTransposed) {
  const char* hlo_text = R"(
HloModule BatchedInstrLayoutCheck

ENTRY AddDotsFunc {
  x = f32[5,2,3] parameter(0)
  y = f32[5,3,4] parameter(1)
  dot_a = f32[5,2,4] dot(x, y), lhs_contracting_dims={2}, rhs_contracting_dims={1}, lhs_batch_dims={0}, rhs_batch_dims={0}
  ROOT out = f32[2,5,4] transpose(dot_a), dimensions={1, 0, 2}
}

)";

  EXPECT_TRUE(RunAndCompare(hlo_text, ErrorSpec{1e-5, 1e-5}));
  MatchOptimizedHlo(hlo_text,
                    R"(
; CHECK-LABEL: ENTRY %AddDotsFunc (x: f32[5,2,3], y: f32[5,3,4]) -> f32[2,5,4] {
; CHECK-NEXT:    [[P0:%[^ ]+]] = f32[5,2,3]{2,1,0} parameter(0)
; CHECK-NEXT:    [[P1:%[^ ]+]] = f32[5,3,4]{2,1,0} parameter(1)
; CHECK-NEXT:    [[GEMM:%[^ ]+]] = f32[5,2,4]{2,0,1} custom-call([[P0]], [[P1]]),
; CHECK:           custom_call_target="<<CUBLAS_CUSTOM_CALL_TARGET_PLACEHOLDER>>",
; CHECK:           backend_config="{
; CHECK-DAG:         \"alpha_real\":1
; CHECK-DAG:         \"alpha_imag\":0
; CHECK-DAG:         \"beta\":0
; CHECK-DAG:         \"dot_dimension_numbers\":{
; CHECK-DAG:           \"lhs_contracting_dimensions\":[\"2\"]
; CHECK-DAG:           \"rhs_contracting_dimensions\":[\"1\"]
; CHECK-DAG:           \"lhs_batch_dimensions\":[\"0\"]
; CHECK-DAG:           \"rhs_batch_dimensions\":[\"0\"]
; CHECK-DAG:         }
; CHECK-DAG:         \"precision_config\":{
; CHECK-DAG:           \"operand_precision\":[\"DEFAULT\",\"DEFAULT\"]
; CHECK-DAG:         }
; CHECK-DAG:         \"epilogue\":\"DEFAULT\"
; CHECK:           }"
; CHECK-NEXT:    ROOT [[OUT:%[^ ]+]] = f32[2,5,4]{2,1,0} bitcast([[GEMM]])
)");
}

TEST_P(ParameterizedGemmRewriteTest, BatchedInstrLayoutBatchNotInMinorDim) {
  const char* hlo_text = R"(
HloModule BatchedInstrLayoutBatchNotInMinorDim

ENTRY AddDotsFunc {
  x = f32[5,2,3] parameter(0)
  y = f32[5,3,4] parameter(1)
  dot_a = f32[5,2,4] dot(x, y), lhs_contracting_dims={2}, rhs_contracting_dims={1}, lhs_batch_dims={0}, rhs_batch_dims={0}
  ROOT out = f32[2,4,5] transpose(dot_a), dimensions={1, 2, 0}
}

)";

  EXPECT_TRUE(RunAndCompare(hlo_text, ErrorSpec{1e-5, 1e-5}));
  MatchOptimizedHlo(hlo_text,
                    R"(
; CHECK-LABEL: ENTRY %AddDotsFunc (x: f32[5,2,3], y: f32[5,3,4]) -> f32[2,4,5] {
; CHECK-NEXT:    [[P0:%[^ ]+]] = f32[5,2,3]{2,1,0} parameter(0)
; CHECK-NEXT:    [[P1:%[^ ]+]] = f32[5,3,4]{2,1,0} parameter(1)
; CHECK-NEXT:    [[GEMM:%[^ ]+]] = f32[5,2,4]{2,1,0} custom-call([[P0]], [[P1]]),
; CHECK:           custom_call_target="<<CUBLAS_CUSTOM_CALL_TARGET_PLACEHOLDER>>",
; CHECK:           backend_config="{
; CHECK-DAG:         \"alpha_real\":1
; CHECK-DAG:         \"alpha_imag\":0
; CHECK-DAG:         \"beta\":0
; CHECK-DAG:         \"dot_dimension_numbers\":{
; CHECK-DAG:           \"lhs_contracting_dimensions\":[\"2\"]
; CHECK-DAG:           \"rhs_contracting_dimensions\":[\"1\"]
; CHECK-DAG:           \"lhs_batch_dimensions\":[\"0\"]
; CHECK-DAG:           \"rhs_batch_dimensions\":[\"0\"]
; CHECK-DAG:         }
; CHECK-DAG:         \"precision_config\":{
; CHECK-DAG:           \"operand_precision\":[\"DEFAULT\",\"DEFAULT\"]
; CHECK-DAG:         }
; CHECK-DAG:         \"epilogue\":\"DEFAULT\"
; CHECK:           }"
; CHECK-NEXT:    ROOT [[OUT:%[^ ]+]] = f32[2,4,5]{2,1,0} [[OP:[^ ]+]]([[GEMM]])
)");
}

TEST_P(ParameterizedGemmRewriteTest, AlphaSimpleRewrite) {
  const char* hlo_text = R"(
HloModule AlphaSimpleRewrite

ENTRY AddDotsFunc {
  x = f32[2,2] parameter(0)
  y = f32[2,2] parameter(1)
  k = f32[] constant(3.0)
  k_broadcast = f32[2, 2] broadcast(k), dimensions={}
  dot_a = f32[2,2] dot(x, y), lhs_contracting_dims={1}, rhs_contracting_dims={0}
  ROOT dot_a_multiplied = f32[2, 2] multiply(dot_a, k_broadcast)
}

)";

  EXPECT_TRUE(RunAndCompare(hlo_text, ErrorSpec{1e-5, 1e-5}));
  MatchOptimizedHlo(hlo_text,
                    R"(
; CHECK-LABEL: ENTRY %AddDotsFunc (x: f32[2,2], y: f32[2,2]) -> f32[2,2] {
; CHECK-NEXT:    [[P0:%[^ ]+]] = f32[2,2]{1,0} parameter(0)
; CHECK-NEXT:    [[P1:%[^ ]+]] = f32[2,2]{1,0} parameter(1)
; CHECK-NEXT:    ROOT [[OUT:%[^ ]+]] = f32[2,2]{1,0} custom-call([[P0]], [[P1]]),
; CHECK:           custom_call_target="<<CUBLAS_CUSTOM_CALL_TARGET_PLACEHOLDER>>",
; CHECK:           backend_config="{
; CHECK-DAG:         \"alpha_real\":3
; CHECK-DAG:         \"alpha_imag\":0
; CHECK-DAG:         \"beta\":0
; CHECK-DAG:         \"dot_dimension_numbers\":{
; CHECK-DAG:           \"lhs_contracting_dimensions\":[\"1\"]
; CHECK-DAG:           \"rhs_contracting_dimensions\":[\"0\"]
; CHECK-DAG:           \"lhs_batch_dimensions\":[]
; CHECK-DAG:           \"rhs_batch_dimensions\":[]
; CHECK-DAG:         }
; CHECK-DAG:         \"precision_config\":{
; CHECK-DAG:           \"operand_precision\":[\"DEFAULT\",\"DEFAULT\"]
; CHECK-DAG:         }
; CHECK-DAG:         \"epilogue\":\"DEFAULT\"
; CHECK:           }"
)");
}

TEST_P(ParameterizedGemmRewriteTest, ComplexAlphaSimpleRewrite) {
  const char* hlo_text = R"(
HloModule ComplexAlphaSimpleRewrite

ENTRY AddDotsFunc {
  x = c64[2,2] parameter(0)
  y = c64[2,2] parameter(1)
  k = c64[] constant((3.0, 3.0))
  k_broadcast = c64[2, 2] broadcast(k), dimensions={}
  dot_a = c64[2,2] dot(x, y), lhs_contracting_dims={1}, rhs_contracting_dims={0}
  ROOT dot_a_multiplied = c64[2, 2] multiply(dot_a, k_broadcast)
}

)";

  DebugOptions debug_options = GetDebugOptionsForTest();
  if (!debug_options.xla_gpu_enable_cublaslt()) {
    EXPECT_TRUE(RunAndCompare(hlo_text, ErrorSpec{1e-5, 1e-5}));
  } else {
    EXPECT_TRUE(RunAndCompare(hlo_text, ErrorSpec{1e-5, 1e-2}));
  }
  MatchOptimizedHlo(hlo_text,
                    R"(
; CHECK-LABEL: ENTRY %AddDotsFunc (x: c64[2,2], y: c64[2,2]) -> c64[2,2] {
; CHECK-NEXT:    [[P0:%[^ ]+]] = c64[2,2]{1,0} parameter(0)
; CHECK-NEXT:    [[P1:%[^ ]+]] = c64[2,2]{1,0} parameter(1)
; CHECK-NEXT:    ROOT [[OUT:%[^ ]+]] = c64[2,2]{1,0} custom-call([[P0]], [[P1]]),
; CHECK:           custom_call_target="<<CUBLAS_CUSTOM_CALL_TARGET_PLACEHOLDER>>",
; CHECK:           backend_config="{
; CHECK-DAG:         \"alpha_real\":3
; CHECK-DAG:         \"alpha_imag\":3
; CHECK-DAG:         \"beta\":0
; CHECK-DAG:         \"dot_dimension_numbers\":{
; CHECK-DAG:           \"lhs_contracting_dimensions\":[\"1\"]
; CHECK-DAG:           \"rhs_contracting_dimensions\":[\"0\"]
; CHECK-DAG:           \"lhs_batch_dimensions\":[]
; CHECK-DAG:           \"rhs_batch_dimensions\":[]
; CHECK-DAG:         }
; CHECK-DAG:         \"precision_config\":{
; CHECK-DAG:           \"operand_precision\":[\"DEFAULT\",\"DEFAULT\"]
; CHECK-DAG:         }
; CHECK-DAG:         \"epilogue\":\"DEFAULT\"
; CHECK:           }"
)");
}

TEST_P(ParameterizedGemmRewriteTest, AlphaMultipleUsersNoRewrite) {
  const char* hlo_text = R"(
HloModule AlphaMultipleUsersNoRewrite

ENTRY AddDotsFunc {
  x = f32[2,2] parameter(0)
  y = f32[2,2] parameter(1)
  k = f32[] constant(3.0)
  k_broadcast = f32[2, 2] broadcast(k), dimensions={}
  dot_a = f32[2,2] dot(x, y), lhs_contracting_dims={1}, rhs_contracting_dims={0}
  dot_a_multiplied = f32[2, 2] multiply(dot_a, k_broadcast)
  ROOT out = f32[2,2] add(dot_a_multiplied, dot_a)
}

)";

  EXPECT_TRUE(RunAndCompare(hlo_text, ErrorSpec{1e-5, 1e-5}));
  MatchOptimizedHlo(hlo_text,
                    R"(
; CHECK:    {{[^ ]+}} = f32[2,2]{1,0} custom-call({{[^,]+}}, {{[^)]+}}),
; CHECK:           custom_call_target="<<CUBLAS_CUSTOM_CALL_TARGET_PLACEHOLDER>>",
; CHECK:           backend_config="{
; CHECK-DAG:         \"alpha_real\":1
; CHECK-DAG:         \"alpha_imag\":0
; CHECK-DAG:         \"beta\":0
; CHECK-DAG:         \"dot_dimension_numbers\":{
; CHECK-DAG:           \"lhs_contracting_dimensions\":[\"1\"]
; CHECK-DAG:           \"rhs_contracting_dimensions\":[\"0\"]
; CHECK-DAG:           \"lhs_batch_dimensions\":[]
; CHECK-DAG:           \"rhs_batch_dimensions\":[]
; CHECK-DAG:         }
; CHECK-DAG:         \"precision_config\":{
; CHECK-DAG:           \"operand_precision\":[\"DEFAULT\",\"DEFAULT\"]
; CHECK-DAG:         }
; CHECK-DAG:         \"epilogue\":\"DEFAULT\"
; CHECK:           }"
)");
}

TEST_P(ParameterizedGemmRewriteTest, AlphaVectorNoRewrite) {
  const char* hlo_text = R"(
HloModule AlphaVectorNoRewrite

ENTRY AddDotsFunc {
  x = f32[2,2] parameter(0)
  y = f32[2,2] parameter(1)
  alpha = f32[2] constant({1, 2})
  alpha_broadcast = f32[2,2] broadcast(alpha), dimensions={1}
  dot = f32[2,2] dot(x, y), lhs_contracting_dims={1}, rhs_contracting_dims={0}
  ROOT dot_a_multiplied = f32[2, 2] multiply(dot, alpha_broadcast)
}
)";

  EXPECT_TRUE(RunAndCompare(hlo_text, ErrorSpec{1e-5, 1e-5}));
  MatchOptimizedHlo(hlo_text,
                    R"(
; CHECK-LABEL: ENTRY %AddDotsFunc (x: f32[2,2], y: f32[2,2]) -> f32[2,2] {
; CHECK-NEXT:    [[P0:%[^ ]+]] = f32[2,2]{1,0} parameter(0)
; CHECK-NEXT:    [[P1:%[^ ]+]] = f32[2,2]{1,0} parameter(1)
; CHECK-NEXT:    [[OUT:%[^ ]+]] = f32[2,2]{1,0} custom-call([[P0]], [[P1]]),
; CHECK:           custom_call_target="<<CUBLAS_CUSTOM_CALL_TARGET_PLACEHOLDER>>",
; CHECK:           backend_config="{
; CHECK-DAG:         \"alpha_real\":1
; CHECK-DAG:         \"alpha_imag\":0
; CHECK-DAG:         \"beta\":0
; CHECK-DAG:         \"dot_dimension_numbers\":{
; CHECK-DAG:           \"lhs_contracting_dimensions\":[\"1\"]
; CHECK-DAG:           \"rhs_contracting_dimensions\":[\"0\"]
; CHECK-DAG:           \"lhs_batch_dimensions\":[]
; CHECK-DAG:           \"rhs_batch_dimensions\":[]
; CHECK-DAG:         }
; CHECK-DAG:         \"precision_config\":{
; CHECK-DAG:           \"operand_precision\":[\"DEFAULT\",\"DEFAULT\"]
; CHECK-DAG:         }
; CHECK-DAG:         \"epilogue\":\"DEFAULT\"
; CHECK:           }"
)");
}

TEST_P(ParameterizedGemmRewriteTest, BF16Gemm) {
  const char* hlo_text = R"(
HloModule bf16gemm

ENTRY bf16gemm {
  %parameter.1 = bf16[12,4]{1,0} parameter(0)
  %parameter.2 = bf16[4,8]{1,0} parameter(1)
  ROOT %dot.8 = bf16[12,8] dot(bf16[12,4] %parameter.1, bf16[4,8] %parameter.2), lhs_contracting_dims={1}, rhs_contracting_dims={0}
}
  )";
  EXPECT_TRUE(RunAndCompare(hlo_text, ErrorSpec{1e-5, 1e-5}));

  if (GetCudaComputeCapability().IsAtLeast(se::CudaComputeCapability::AMPERE)) {
    MatchOptimizedHlo(hlo_text,
                      R"(
; CHECK: bf16[16,8]{1,0} custom-call(bf16[16,8]{1,0} {{.*}}, bf16[8,8]{1,0} {{.*}}), custom_call_target="<<CUBLAS_CUSTOM_CALL_TARGET_PLACEHOLDER>>"
  )",
                      /*print_operand_shape=*/true);
  } else {
    MatchOptimizedHlo(hlo_text,
                      R"(
; CHECK: bf16[12,8]{1,0} custom-call(bf16[12,4]{1,0} [[P0:%[^ ]+]], bf16[4,8]{1,0} [[P1:%[^ ]+]]), custom_call_target="<<CUBLAS_CUSTOM_CALL_TARGET_PLACEHOLDER>>"
  )",
                      /*print_operand_shape=*/true);
  }
}

TEST_P(ParameterizedGemmRewriteTest, BF16GemmStrided) {
  const char* hlo_text = R"(
HloModule bf16gemm

ENTRY bf16gemm {
  %parameter.1 = bf16[3,3,4] parameter(0)
  %parameter.2 = bf16[3,3,2] parameter(1)
  ROOT %dot.3 = bf16[3,4,2]{2,1,0} dot(bf16[3,3,4]{2,1,0} %parameter.1, bf16[3,3,2]{2,1,0} %parameter.2), lhs_batch_dims={0}, lhs_contracting_dims={1}, rhs_batch_dims={0}, rhs_contracting_dims={1}, operand_precision={highest,highest}
}

  )";
  EXPECT_TRUE(RunAndCompare(hlo_text, ErrorSpec{1e-5, 1e-5}));

  if (GetCudaComputeCapability().IsAtLeast(se::CudaComputeCapability::AMPERE)) {
    MatchOptimizedHlo(hlo_text,
                      R"(
    ; CHECK: bf16[3,8,8]{2,1,0} custom-call(bf16[3,8,8]{2,1,0} {{.*}}, bf16[3,8,8]{2,1,0} {{.*}}), custom_call_target="<<CUBLAS_CUSTOM_CALL_TARGET_PLACEHOLDER>>"
    )",
                      /*print_operand_shape=*/true);
  } else {
    MatchOptimizedHlo(hlo_text,
                      R"(
    ; CHECK: ROOT [[OUT:%[^ ]+]] = bf16[3,4,2]{2,1,0} custom-call(bf16[3,3,4]{2,1,0} [[A:%[^ ]+]], bf16[3,3,2]{2,1,0} [[B:%[^ ]+]]), custom_call_target="<<CUBLAS_CUSTOM_CALL_TARGET_PLACEHOLDER>>"
    )",
                      /*print_operand_shape=*/true);
  }
}

TEST_P(ParameterizedGemmRewriteTest, Int8Gemm) {
  const char* hlo_text = R"(
HloModule int8gemm

ENTRY int8gemm {
  %parameter.1 = s8[12,4]{1,0} parameter(0)
  %parameter.2 = s8[4,8]{1,0} parameter(1)
  ROOT %dot.8 = s32[12,8] dot(s8[12,4] %parameter.1, s8[4,8] %parameter.2), lhs_contracting_dims={1}, rhs_contracting_dims={0}
}
  )";
  EXPECT_TRUE(RunAndCompare(hlo_text, ErrorSpec{1e-5, 1e-5}));

  if (GetCudaComputeCapability().IsAtLeast(se::CudaComputeCapability::VOLTA)) {
    MatchOptimizedHlo(hlo_text,
                      R"(
; CHECK: s32[12,8]{1,0} custom-call(s8[12,4]{1,0} [[A:%[^ ]+]], s8[4,8]{0,1} [[B:%[^ ]+]]), custom_call_target="__cublas$gemm"
  )",
                      /*print_operand_shape=*/true);
  } else {
    MatchOptimizedHlo(hlo_text,
                      R"(
; CHECK: s32[12,8]{1,0} dot(s32[12,4]{1,0} [[A:%[^ ]+]], s32[4,8]{1,0} [[B:%[^ ]+]]), lhs_contracting_dims={1}, rhs_contracting_dims={0}

  )",
                      /*print_operand_shape=*/true);
  }
}

TEST_P(ParameterizedGemmRewriteTest, Int8GemmNoAlphaRewrite) {
  const char* hlo_text = R"(
HloModule int8gemm

ENTRY int8gemm {
  %parameter.1 = s8[12,4]{1,0} parameter(0)
  %parameter.2 = s8[4,8]{1,0} parameter(1)
  k = s32[] constant(2)
  k_broadcast = s32[12,8] broadcast(k), dimensions={}
  %dot.8 = s32[12,8] dot(s8[12,4] %parameter.1, s8[4,8] %parameter.2), lhs_contracting_dims={1}, rhs_contracting_dims={0}
  ROOT dot_multiplied = s32[12,8] multiply(%dot.8, k_broadcast)
}
  )";
  EXPECT_TRUE(RunAndCompare(hlo_text, ErrorSpec{1e-5, 1e-5}));

  if (GetCudaComputeCapability().IsAtLeast(se::CudaComputeCapability::VOLTA)) {
    MatchOptimizedHlo(hlo_text,
                      R"(
; CHECK: s32[12,8]{1,0} custom-call(s8[12,4]{1,0} [[A:%[^ ]+]], s8[4,8]{0,1} [[B:%[^ ]+]]),
; CHECK:           custom_call_target="__cublas$gemm",
; CHECK:           backend_config="{
; CHECK-DAG:       \"alpha_real\":1
; CHECK-DAG:       \"alpha_imag\":0
  )",
                      /*print_operand_shape=*/true);
  } else {
    MatchOptimizedHlo(hlo_text,
                      R"(
; CHECK: s32[12,8]{1,0} dot(s32[12,4]{1,0} [[A:%[^ ]+]], s32[4,8]{1,0} [[B:%[^ ]+]]), lhs_contracting_dims={1}, rhs_contracting_dims={0}

  )",
                      /*print_operand_shape=*/true);
  }
}

TEST_P(ParameterizedGemmRewriteTest, Int8GemmNoBetaRewrite) {
  const char* hlo_text = R"(
HloModule int8gemm

ENTRY int8gemm {
  %parameter.1 = s8[12,4]{1,0} parameter(0)
  %parameter.2 = s8[4,8]{1,0} parameter(1)
  bias = s32[12,8] parameter(2)
  %dot.8 = s32[12,8] dot(s8[12,4] %parameter.1, s8[4,8] %parameter.2), lhs_contracting_dims={1}, rhs_contracting_dims={0}
  ROOT out = s32[12,8] add(%dot.8, bias)
}
  )";
  EXPECT_TRUE(RunAndCompare(hlo_text, ErrorSpec{1e-5, 1e-5}));

  if (GetCudaComputeCapability().IsAtLeast(se::CudaComputeCapability::VOLTA)) {
    MatchOptimizedHlo(hlo_text,
                      R"(
; CHECK: s32[12,8]{1,0} custom-call(s8[12,4]{1,0} [[A:%[^ ]+]], s8[4,8]{0,1} [[B:%[^ ]+]]),
; CHECK:           custom_call_target="__cublas$gemm",
; CHECK:           backend_config="{
; CHECK-DAG:       \"alpha_real\":1
; CHECK-DAG:       \"alpha_imag\":0
; CHECK-DAG:       \"beta\":0
  )",
                      /*print_operand_shape=*/true);
  } else {
    MatchOptimizedHlo(hlo_text,
                      R"(
; CHECK: s32[12,8]{1,0} dot(s32[12,4]{1,0} [[A:%[^ ]+]], s32[4,8]{1,0} [[B:%[^ ]+]]), lhs_contracting_dims={1}, rhs_contracting_dims={0}

  )",
                      /*print_operand_shape=*/true);
  }
}

TEST_P(ParameterizedGemmRewriteTest, Int8GemmNotMultipleOfFour) {
  const char* hlo_text = R"(
HloModule int8gemm

ENTRY int8gemm {
  %parameter.1 = s8[13,4]{1,0} parameter(0)
  %parameter.2 = s8[4,9]{1,0} parameter(1)
  ROOT %dot.9 = s32[13,9] dot(s8[13,4] %parameter.1, s8[4,9] %parameter.2), lhs_contracting_dims={1}, rhs_contracting_dims={0}
}
  )";
  EXPECT_TRUE(RunAndCompare(hlo_text, ErrorSpec{1e-5, 1e-5}));

  if (GetCudaComputeCapability().IsAtLeast(se::CudaComputeCapability::VOLTA)) {
    MatchOptimizedHlo(hlo_text,
                      R"(
; CHECK: s32[16,12]{1,0} custom-call(s8[16,4]{1,0} [[A:%[^ ]+]], s8[4,12]{0,1} [[B:%[^ ]+]]), custom_call_target="__cublas$gemm"
  )",
                      /*print_operand_shape=*/true);
  } else {
    MatchOptimizedHlo(hlo_text,
                      R"(
; CHECK: s32[13,9]{1,0} dot(s32[13,4]{1,0} [[A:%[^ ]+]], s32[4,9]{1,0} [[B:%[^ ]+]]), lhs_contracting_dims={1}, rhs_contracting_dims={0}

  )",
                      /*print_operand_shape=*/true);
  }
}

INSTANTIATE_TEST_SUITE_P(CublasTestsBothLegacyAndLt,
                         ParameterizedGemmRewriteTest, ::testing::Bool());

// A test fixture class for tests which are specific to legacy cublas
class LegacyCublasGemmRewriteTest : public GemmRewriteTest {
 public:
  DebugOptions GetDebugOptionsForTest() override {
    DebugOptions debug_options = GemmRewriteTest::GetDebugOptionsForTest();
    debug_options.set_xla_gpu_enable_cublaslt(false);
    return debug_options;
  }
};

TEST_F(LegacyCublasGemmRewriteTest, AlphaBetaRewrite) {
  const char* hlo_text = R"(
HloModule NonZeroAlphaBeta

ENTRY AddDotsFunc {
  x = f32[2,2] parameter(0)
  y = f32[2,2] parameter(1)
  bias = f32[2,2] parameter(2)
  k = f32[] constant(3.0)
  k_broadcast = f32[2, 2] broadcast(k), dimensions={}
  dot_a = f32[2,2] dot(x, y), lhs_contracting_dims={1}, rhs_contracting_dims={0}
  dot_a_multiplied = f32[2, 2] multiply(dot_a, k_broadcast)
  ROOT out = f32[2,2] add(dot_a_multiplied, bias)
}

)";

  EXPECT_TRUE(RunAndCompare(hlo_text, ErrorSpec{1e-5, 1e-5}));
  MatchOptimizedHlo(hlo_text,
                    R"(
; CHECK-LABEL: ENTRY %AddDotsFunc (x: f32[2,2], y: f32[2,2], bias: f32[2,2]) -> f32[2,2] {
; CHECK-DAG:     [[X:%[^ ]+]] = f32[2,2]{1,0} parameter(0)
; CHECK-DAG:     [[Y:%[^ ]+]] = f32[2,2]{1,0} parameter(1)
; CHECK-DAG:     [[BIAS:%[^ ]+]] = f32[2,2]{1,0} parameter(2)
; CHECK-DAG:     [[BIAS_COPY:%[^ ]+]] = f32[2,2]{1,0} copy([[BIAS]])
; CHECK-NEXT:    ROOT [[OUT:%[^ ]+]] = f32[2,2]{1,0} custom-call([[X]], [[Y]], [[BIAS_COPY]]),
; CHECK:           custom_call_target="__cublas$gemm",
; CHECK:           output_to_operand_aliasing={{{{}: \(2, {}\)}}},
; CHECK:           backend_config="{
; CHECK-DAG:         \"alpha_real\":3
; CHECK-DAG:         \"alpha_imag\":0
; CHECK-DAG:         \"beta\":1
; CHECK-DAG:         \"dot_dimension_numbers\":{
; CHECK-DAG:           \"lhs_contracting_dimensions\":[\"1\"]
; CHECK-DAG:           \"rhs_contracting_dimensions\":[\"0\"]
; CHECK-DAG:           \"lhs_batch_dimensions\":[]
; CHECK-DAG:           \"rhs_batch_dimensions\":[]
; CHECK-DAG:         }
; CHECK-DAG:         \"precision_config\":{
; CHECK-DAG:           \"operand_precision\":[\"DEFAULT\",\"DEFAULT\"]
; CHECK-DAG:         }
; CHECK-DAG:         \"epilogue\":\"DEFAULT\"
; CHECK:           }"
)");
}

TEST_F(LegacyCublasGemmRewriteTest, BiasMultipleUsersNoOverwrite) {
  const char* hlo_text = R"(
HloModule BiasMultipleUsersNoOverwrite

ENTRY AddDotsFunc {
  x = f32[2,2] parameter(0)
  y = f32[2,2] parameter(1)
  bias = f32[2,2] parameter(2)
  k = f32[] constant(3.0)
  k_broadcast = f32[2, 2] broadcast(k), dimensions={}
  dot_a = f32[2,2] dot(x, y), lhs_contracting_dims={1}, rhs_contracting_dims={0}
  dot_a_multiplied = f32[2, 2] multiply(dot_a, k_broadcast)
  biased_out = f32[2,2] add(dot_a_multiplied, bias)
  ROOT out = f32[2,2] add(biased_out, bias)
}
)";

  EXPECT_TRUE(RunAndCompare(hlo_text, ErrorSpec{1e-5, 1e-5}));
  MatchOptimizedHlo(hlo_text,
                    R"(
; CHECK-LABEL: ENTRY %AddDotsFunc (x: f32[2,2], y: f32[2,2], bias: f32[2,2]) -> f32[2,2] {
; CHECK-DAG:     [[P0:%[^ ]+]] = f32[2,2]{1,0} parameter(0)
; CHECK-DAG:     [[P1:%[^ ]+]] = f32[2,2]{1,0} parameter(1)
; CHECK-NEXT:    [[GEMM:%[^ ]+]] = f32[2,2]{1,0} custom-call([[P0]], [[P1]]),
; CHECK:           custom_call_target="__cublas$gemm",
; CHECK:           backend_config="{
; CHECK-DAG:         \"alpha_real\":3
; CHECK-DAG:         \"alpha_imag\":0
; CHECK-DAG:         \"beta\":0
; CHECK-DAG:         \"dot_dimension_numbers\":{
; CHECK-DAG:           \"lhs_contracting_dimensions\":[\"1\"]
; CHECK-DAG:           \"rhs_contracting_dimensions\":[\"0\"]
; CHECK-DAG:           \"lhs_batch_dimensions\":[]
; CHECK-DAG:           \"rhs_batch_dimensions\":[]
; CHECK-DAG:         }
; CHECK-DAG:         \"precision_config\":{
; CHECK-DAG:           \"operand_precision\":[\"DEFAULT\",\"DEFAULT\"]
; CHECK-DAG:         }
; CHECK-DAG:         \"epilogue\":\"DEFAULT\"
; CHECK:           }"
)");
}

TEST_F(LegacyCublasGemmRewriteTest, LargerBiasMultipleUsersNoRewrite) {
  const char* hlo_text = R"(
HloModule LargerBiasMultipleUsersNoRewrite

ENTRY AddDotsFunc {
  x = f32[1024,1024] parameter(0)
  y = f32[1024,1024] parameter(1)
  bias = f32[1024,1024] parameter(2)
  dot_a = f32[1024,1024] dot(x, y), lhs_contracting_dims={1}, rhs_contracting_dims={0}
  biased_out = f32[1024,1024] add(dot_a, bias)
  ROOT out = f32[1024,1024] add(biased_out, bias)
}

)";

  EXPECT_TRUE(RunAndCompare(hlo_text, ErrorSpec{1e-5, 1e-5}));
  MatchOptimizedHlo(hlo_text,
                    R"(
; CHECK-LABEL: ENTRY %AddDotsFunc (x: f32[1024,1024], y: f32[1024,1024], bias: f32[1024,1024]) -> f32[1024,1024] {
; CHECK-DAG:     [[P0:%[^ ]+]] = f32[1024,1024]{1,0} parameter(0)
; CHECK-DAG:     [[P1:%[^ ]+]] = f32[1024,1024]{1,0} parameter(1)
; CHECK-NEXT:    [[GEMM:%[^ ]+]] = f32[1024,1024]{1,0} custom-call([[P0]], [[P1]]),
; CHECK:           custom_call_target="__cublas$gemm",
; CHECK:           backend_config="{
; CHECK-DAG:         \"alpha_real\":1
; CHECK-DAG:         \"alpha_imag\":0
; CHECK-DAG:         \"beta\":0
; CHECK-DAG:         \"dot_dimension_numbers\":{
; CHECK-DAG:           \"lhs_contracting_dimensions\":[\"1\"]
; CHECK-DAG:           \"rhs_contracting_dimensions\":[\"0\"]
; CHECK-DAG:           \"lhs_batch_dimensions\":[]
; CHECK-DAG:           \"rhs_batch_dimensions\":[]
; CHECK-DAG:         }
; CHECK-DAG:         \"precision_config\":{
; CHECK-DAG:           \"operand_precision\":[\"DEFAULT\",\"DEFAULT\"]
; CHECK-DAG:         }
; CHECK-DAG:         \"epilogue\":\"DEFAULT\"
; CHECK:           }"
)");
}

TEST_F(LegacyCublasGemmRewriteTest, BF16GemmWithBias) {
  const char* hlo_text = R"(
HloModule BF16GemmWithBias

ENTRY BF16GemmWithBias {
  x = bf16[8,8]{1,0} parameter(0)
  y = bf16[8,8]{1,0} parameter(1)
  dot.5 = bf16[8,8]{1,0} dot(x, y), lhs_contracting_dims={1}, rhs_contracting_dims={0}
  bias = bf16[8,8]{1,0} parameter(2)
  ROOT add.6 = bf16[8,8]{1,0} add(dot.5, bias)
}
  )";

  EXPECT_TRUE(RunAndCompare(hlo_text, ErrorSpec{1e-3, 1e-3}));
  MatchOptimizedHlo(hlo_text,
                    R"(
; CHECK-LABEL: ENTRY %BF16GemmWithBias (x: bf16[8,8], y: bf16[8,8], bias: bf16[8,8]) -> bf16[8,8] {
; CHECK-DAG:    [[X:%[^ ]+]] = bf16[8,8]{1,0} parameter(0)
; CHECK-DAG:    [[Y:%[^ ]+]] = bf16[8,8]{1,0} parameter(1)
; CHECK-DAG:    [[BIAS:%[^ ]+]] = bf16[8,8]{1,0} parameter(2)
; CHECK-DAG:    [[BIAS_COPY:%[^ ]+]] = bf16[8,8]{1,0} copy([[BIAS]])
; CHECK-NEXT:   ROOT [[GEMM:%[^ ]+]] = bf16[8,8]{1,0} custom-call([[X]], [[Y]], [[BIAS_COPY]]),
; CHECK:           custom_call_target="__cublas$gemm",
; CHECK:           output_to_operand_aliasing={{{{}: \(2, {}\)}}},
; CHECK:           backend_config="{
; CHECK-DAG:         \"alpha_real\":1
; CHECK-DAG:         \"alpha_imag\":0
; CHECK-DAG:         \"beta\":1
; CHECK-DAG:         \"dot_dimension_numbers\":{
; CHECK-DAG:           \"lhs_contracting_dimensions\":[\"1\"]
; CHECK-DAG:           \"rhs_contracting_dimensions\":[\"0\"]
; CHECK-DAG:           \"lhs_batch_dimensions\":[]
; CHECK-DAG:           \"rhs_batch_dimensions\":[]
; CHECK-DAG:         }
; CHECK-DAG:         \"precision_config\":{
; CHECK-DAG:           \"operand_precision\":[\"DEFAULT\",\"DEFAULT\"]
; CHECK-DAG:         }
; CHECK-DAG:         \"epilogue\":\"DEFAULT\"
; CHECK:           }"
)");
}

TEST_F(LegacyCublasGemmRewriteTest, MatrixBias) {
  const char* hlo_text = R"(
HloModule test

ENTRY test {
  x = f32[2,3] parameter(0)
  y = f32[3,4] parameter(1)
  z = f32[2,4] parameter(2)
  dot_a = f32[2,4] dot(x, y), lhs_contracting_dims={1}, rhs_contracting_dims={0}
  ROOT out = f32[2,4] add(dot_a, z)
}

)";

  EXPECT_TRUE(RunAndCompare(hlo_text, ErrorSpec{1e-5, 1e-5}));
  MatchOptimizedHlo(hlo_text,
                    R"(
; CHECK-LABEL: ENTRY %test (x: f32[2,3], y: f32[3,4], z: f32[2,4]) -> f32[2,4] {
; CHECK-NEXT:    [[P0:%[^ ]+]] = f32[2,3]{1,0} parameter(0)
; CHECK-NEXT:    [[P1:%[^ ]+]] = f32[3,4]{1,0} parameter(1)
; CHECK-NEXT:    [[BIAS:%[^ ]+]] = f32[2,4]{1,0} parameter(2)
; CHECK-NEXT:    [[BIAS_COPY:%[^ ]+]] = f32[2,4]{1,0} copy([[BIAS]])
; CHECK-NEXT:    ROOT [[GEMM:%[^ ]+]] = f32[2,4]{1,0} custom-call([[P0]], [[P1]], [[BIAS_COPY]]),
; CHECK:           custom_call_target="__cublas$gemm",
; CHECK:           output_to_operand_aliasing={{{{}: \(2, {}\)}}},
; CHECK:           backend_config="{
; CHECK-DAG:         \"alpha_real\":1
; CHECK-DAG:         \"alpha_imag\":0
; CHECK-DAG:         \"beta\":1
; CHECK-DAG:         \"dot_dimension_numbers\":{
; CHECK-DAG:           \"lhs_contracting_dimensions\":[\"1\"]
; CHECK-DAG:           \"rhs_contracting_dimensions\":[\"0\"]
; CHECK-DAG:           \"lhs_batch_dimensions\":[]
; CHECK-DAG:           \"rhs_batch_dimensions\":[]
; CHECK-DAG:         }
; CHECK-DAG:         \"precision_config\":{
; CHECK-DAG:           \"operand_precision\":[\"DEFAULT\",\"DEFAULT\"]
; CHECK-DAG:         }
; CHECK-DAG:         \"epilogue\":\"DEFAULT\"
; CHECK:           }"
)");
}

TEST_F(LegacyCublasGemmRewriteTest, MatrixBiasWhereBiasIsNotAParameter) {
  const char* hlo_text = R"(
HloModule test

ENTRY test {
  w = f32[2,3] parameter(0)
  x = f32[3,4] parameter(1)
  first_dot = f32[2,4] dot(w, x), lhs_contracting_dims={1}, rhs_contracting_dims={0}
  y = f32[2,3] parameter(2)
  z = f32[3,4] parameter(3)
  second_dot = f32[2,4] dot(y, z), lhs_contracting_dims={1}, rhs_contracting_dims={0}
  ROOT out = f32[2,4] add(second_dot, first_dot)
}

)";

  EXPECT_TRUE(RunAndCompare(hlo_text, ErrorSpec{1e-5, 1e-5}));
  MatchOptimizedHlo(hlo_text,
                    R"(
; CHECK-LABEL: ENTRY %test (w: f32[2,3], x: f32[3,4], y: f32[2,3], z: f32[3,4]) -> f32[2,4] {
; CHECK-DAG:     [[P0:%[^ ]+]] = f32[2,3]{1,0} parameter(0)
; CHECK-DAG:     [[P1:%[^ ]+]] = f32[3,4]{1,0} parameter(1)
; CHECK-DAG:     [[P2:%[^ ]+]] = f32[2,3]{1,0} parameter(2)
; CHECK-DAG:     [[P3:%[^ ]+]] = f32[3,4]{1,0} parameter(3)
; CHECK-NEXT:    [[FIRST_GEMM:%[^ ]+]] = f32[2,4]{1,0} custom-call([[P0]], [[P1]]),
; CHECK:           custom_call_target="__cublas$gemm",
; CHECK:           backend_config="{
; CHECK-DAG:         \"alpha_real\":1
; CHECK-DAG:         \"alpha_imag\":0
; CHECK-DAG:         \"beta\":0
; CHECK-DAG:         \"dot_dimension_numbers\":{
; CHECK-DAG:           \"lhs_contracting_dimensions\":[\"1\"]
; CHECK-DAG:           \"rhs_contracting_dimensions\":[\"0\"]
; CHECK-DAG:           \"lhs_batch_dimensions\":[]
; CHECK-DAG:           \"rhs_batch_dimensions\":[]
; CHECK-DAG:         }
; CHECK-DAG:         \"precision_config\":{
; CHECK-DAG:           \"operand_precision\":[\"DEFAULT\",\"DEFAULT\"]
; CHECK-DAG:         }
; CHECK-DAG:         \"epilogue\":\"DEFAULT\"
; CHECK:           }"
; CHECK-NEXT:    ROOT [[SECOND_GEMM:%[^ ]+]] = f32[2,4]{1,0} custom-call([[P2]], [[P3]], [[FIRST_GEMM]]),
; CHECK:           custom_call_target="__cublas$gemm",
; CHECK:           output_to_operand_aliasing={{{{}: \(2, {}\)}}},
; CHECK:           backend_config="{
; CHECK-DAG:         \"alpha_real\":1
; CHECK-DAG:         \"alpha_imag\":0
; CHECK-DAG:         \"beta\":1
; CHECK-DAG:         \"dot_dimension_numbers\":{
; CHECK-DAG:           \"lhs_contracting_dimensions\":[\"1\"]
; CHECK-DAG:           \"rhs_contracting_dimensions\":[\"0\"]
; CHECK-DAG:           \"lhs_batch_dimensions\":[]
; CHECK-DAG:           \"rhs_batch_dimensions\":[]
; CHECK-DAG:         }
; CHECK-DAG:         \"precision_config\":{
; CHECK-DAG:           \"operand_precision\":[\"DEFAULT\",\"DEFAULT\"]
; CHECK-DAG:         }
; CHECK-DAG:         \"epilogue\":\"DEFAULT\"
; CHECK:           }"
)");
}

TEST_F(LegacyCublasGemmRewriteTest, MergeBitcastAndAdd) {
  const char* hlo_text = R"(
HloModule test
ENTRY test {
  x = f32[2,2] parameter(0)
  y = f32[2,2] parameter(1)
  bias = f32[4] parameter(2)
  dot = f32[2,2] dot(x, y), lhs_contracting_dims={1}, rhs_contracting_dims={0}
  ROOT out = f32[4] add(f32[4] bitcast(dot), bias)
}
)";

  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo_text));
  GemmRewriter pass(GetCudaComputeCapability());
  TF_ASSERT_OK_AND_ASSIGN(bool changed, this->RunHloPass(&pass, module.get()));
  EXPECT_TRUE(changed);

  EXPECT_THAT(
      module->entry_computation()->root_instruction(),
      GmockMatch(
          m::Bitcast(
              m::CustomCall("__cublas$gemm", m::Parameter(0), m::Parameter(1),
                            m::Bitcast(m::Parameter(2)).WithShape(F32, {2, 2})))
              .WithShape(F32, {4})));
}

TEST_F(LegacyCublasGemmRewriteTest, FoldConstantBias) {
  const char* hlo_text = R"(
HloModule test
ENTRY test {
  x = f32[2,2] parameter(0)
  y = f32[2,2] parameter(1)
  bias = f32[2,2] broadcast(f32[2] constant({0, 0})), dimensions={0}

  dot1 = f32[2,2] dot(x, y), lhs_contracting_dims={1}, rhs_contracting_dims={0}
  bias1 = f32[2,2] broadcast(f32[2] constant({0, 0})), dimensions={0}
  sum1 = add(dot1, bias1)

  dot2 = f32[2,2] dot(x, y), lhs_contracting_dims={1}, rhs_contracting_dims={0}
  sum2 = add(dot2, f32[2,2] reshape(bias))

  dot3 = f32[2,2] dot(x, y), lhs_contracting_dims={1}, rhs_contracting_dims={0}
  bias3 = f32[2,2] transpose(bias), dimensions={1,0}
  sum3 = add(dot3, bias3)

  dot4 = f32[2,2] dot(x, y), lhs_contracting_dims={1}, rhs_contracting_dims={0}
  sum4 = add(dot4, f32[2,2] bitcast(bias))

  ROOT root = tuple(sum1, sum2, sum3, sum4)
}
)";

  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo_text));
  GemmRewriter pass(GetCudaComputeCapability());
  TF_ASSERT_OK_AND_ASSIGN(bool changed, this->RunHloPass(&pass, module.get()));
  SCOPED_TRACE(module->ToString());
  EXPECT_TRUE(changed);

  EXPECT_THAT(
      module->entry_computation()->root_instruction(),
      GmockMatch(m::Tuple(
          m::CustomCall(m::Parameter(0), m::Parameter(1), m::Constant()),
          m::CustomCall(m::Parameter(0), m::Parameter(1), m::Constant()),
          m::CustomCall(m::Parameter(0), m::Parameter(1), m::Constant()),
          m::CustomCall(m::Parameter(0), m::Parameter(1), m::Constant()))));
}

// A test fixture class for tests which are specific to cublasLt
class CublasLtGemmRewriteTest : public GemmRewriteTest {
 public:
  DebugOptions GetDebugOptionsForTest() override {
    DebugOptions debug_options = GemmRewriteTest::GetDebugOptionsForTest();
    debug_options.set_xla_gpu_enable_cublaslt(true);
    return debug_options;
  }
};

TEST_F(CublasLtGemmRewriteTest, AlphaBetaRewrite) {
  const char* hlo_text = R"(
HloModule NonZeroAlphaBeta

ENTRY AddDotsFunc {
  x = f32[2,2] parameter(0)
  y = f32[2,2] parameter(1)
  bias = f32[2,2] parameter(2)
  k = f32[] constant(3.0)
  k_broadcast = f32[2, 2] broadcast(k), dimensions={}
  dot_a = f32[2,2] dot(x, y), lhs_contracting_dims={1}, rhs_contracting_dims={0}
  dot_a_multiplied = f32[2, 2] multiply(dot_a, k_broadcast)
  ROOT out = f32[2,2] add(dot_a_multiplied, bias)
}

)";

  EXPECT_TRUE(RunAndCompare(hlo_text, ErrorSpec{1e-5, 1e-5}));
  MatchOptimizedHlo(hlo_text,
                    R"(
; CHECK-LABEL: ENTRY %AddDotsFunc (x: f32[2,2], y: f32[2,2], bias: f32[2,2]) -> f32[2,2] {
; CHECK-DAG:     [[X:%[^ ]+]] = f32[2,2]{1,0} parameter(0)
; CHECK-DAG:     [[Y:%[^ ]+]] = f32[2,2]{1,0} parameter(1)
; CHECK-DAG:     [[BIAS:%[^ ]+]] = f32[2,2]{1,0} parameter(2)
; CHECK-NEXT:    ROOT [[OUT:%[^ ]+]] = f32[2,2]{1,0} custom-call([[X]], [[Y]], [[BIAS]]),
; CHECK:           custom_call_target="__cublas$lt$matmul",
; CHECK:           backend_config="{
; CHECK-DAG:         \"alpha_real\":3
; CHECK-DAG:         \"alpha_imag\":0
; CHECK-DAG:         \"beta\":1
; CHECK-DAG:         \"dot_dimension_numbers\":{
; CHECK-DAG:           \"lhs_contracting_dimensions\":[\"1\"]
; CHECK-DAG:           \"rhs_contracting_dimensions\":[\"0\"]
; CHECK-DAG:           \"lhs_batch_dimensions\":[]
; CHECK-DAG:           \"rhs_batch_dimensions\":[]
; CHECK-DAG:         }
; CHECK-DAG:         \"precision_config\":{
; CHECK-DAG:           \"operand_precision\":[\"DEFAULT\",\"DEFAULT\"]
; CHECK-DAG:         }
; CHECK-DAG:         \"epilogue\":\"DEFAULT\"
; CHECK:           }"
)");
}

TEST_F(CublasLtGemmRewriteTest, BiasMultipleUsersNoOverwrite) {
  const char* hlo_text = R"(
HloModule BiasMultipleUsersNoOverwrite

ENTRY AddDotsFunc {
  x = f32[2,2] parameter(0)
  y = f32[2,2] parameter(1)
  bias = f32[2,2] parameter(2)
  k = f32[] constant(3.0)
  k_broadcast = f32[2, 2] broadcast(k), dimensions={}
  dot_a = f32[2,2] dot(x, y), lhs_contracting_dims={1}, rhs_contracting_dims={0}
  dot_a_multiplied = f32[2, 2] multiply(dot_a, k_broadcast)
  biased_out = f32[2,2] add(dot_a_multiplied, bias)
  ROOT out = f32[2,2] add(biased_out, bias)
}
)";

  EXPECT_TRUE(RunAndCompare(hlo_text, ErrorSpec{1e-5, 1e-5}));
  MatchOptimizedHlo(hlo_text,
                    R"(
; CHECK-LABEL: ENTRY %AddDotsFunc (x: f32[2,2], y: f32[2,2], bias: f32[2,2]) -> f32[2,2] {
; CHECK-DAG:     [[P0:%[^ ]+]] = f32[2,2]{1,0} parameter(0)
; CHECK-DAG:     [[P1:%[^ ]+]] = f32[2,2]{1,0} parameter(1)
; CHECK-DAG:     [[BIAS:%[^ ]+]] = f32[2,2]{1,0} parameter(2)
; CHECK-NEXT:    [[GEMM:%[^ ]+]] = f32[2,2]{1,0} custom-call([[P0]], [[P1]], [[BIAS]]),
; CHECK:           custom_call_target="__cublas$lt$matmul",
; CHECK-NOT:       output_to_operand_aliasing
; CHECK:           backend_config="{
; CHECK-DAG:         \"alpha_real\":3
; CHECK-DAG:         \"alpha_imag\":0
; CHECK-DAG:         \"beta\":1
; CHECK-DAG:         \"dot_dimension_numbers\":{
; CHECK-DAG:           \"lhs_contracting_dimensions\":[\"1\"]
; CHECK-DAG:           \"rhs_contracting_dimensions\":[\"0\"]
; CHECK-DAG:           \"lhs_batch_dimensions\":[]
; CHECK-DAG:           \"rhs_batch_dimensions\":[]
; CHECK-DAG:         }
; CHECK-DAG:         \"precision_config\":{
; CHECK-DAG:           \"operand_precision\":[\"DEFAULT\",\"DEFAULT\"]
; CHECK-DAG:         }
; CHECK-DAG:         \"epilogue\":\"DEFAULT\"
; CHECK:           }"
)");
}

TEST_F(CublasLtGemmRewriteTest, LargerBiasMultipleUsersNoRewrite) {
  const char* hlo_text = R"(
HloModule LargerBiasMultipleUsersNoRewrite

ENTRY AddDotsFunc {
  x = f32[1024,1024] parameter(0)
  y = f32[1024,1024] parameter(1)
  bias = f32[1024,1024] parameter(2)
  dot_a = f32[1024,1024] dot(x, y), lhs_contracting_dims={1}, rhs_contracting_dims={0}
  biased_out = f32[1024,1024] add(dot_a, bias)
  ROOT out = f32[1024,1024] add(biased_out, bias)
}

)";

  EXPECT_TRUE(RunAndCompare(hlo_text, ErrorSpec{1e-5, 1e-5}));
  MatchOptimizedHlo(hlo_text,
                    R"(
; CHECK-LABEL: ENTRY %AddDotsFunc (x: f32[1024,1024], y: f32[1024,1024], bias: f32[1024,1024]) -> f32[1024,1024] {
; CHECK-DAG:     [[P0:%[^ ]+]] = f32[1024,1024]{1,0} parameter(0)
; CHECK-DAG:     [[P1:%[^ ]+]] = f32[1024,1024]{1,0} parameter(1)
; CHECK-DAG:     [[BIAS:%[^ ]+]] = f32[1024,1024]{1,0} parameter(2)
; CHECK-NEXT:    [[GEMM:%[^ ]+]] = f32[1024,1024]{1,0} custom-call([[P0]], [[P1]], [[BIAS]]),
; CHECK:           custom_call_target="__cublas$lt$matmul",
; CHECK:           backend_config="{
; CHECK-DAG:         \"alpha_real\":1
; CHECK-DAG:         \"alpha_imag\":0
; CHECK-DAG:         \"beta\":1
; CHECK-DAG:         \"dot_dimension_numbers\":{
; CHECK-DAG:           \"lhs_contracting_dimensions\":[\"1\"]
; CHECK-DAG:           \"rhs_contracting_dimensions\":[\"0\"]
; CHECK-DAG:           \"lhs_batch_dimensions\":[]
; CHECK-DAG:           \"rhs_batch_dimensions\":[]
; CHECK-DAG:         }
; CHECK-DAG:         \"precision_config\":{
; CHECK-DAG:           \"operand_precision\":[\"DEFAULT\",\"DEFAULT\"]
; CHECK-DAG:         }
; CHECK-DAG:         \"epilogue\":\"DEFAULT\"
; CHECK:           }"
; CHECK-NEXT:    ROOT [[OUT:%[^ ]+]] = f32[1024,1024]{1,0} add([[GEMM]], [[BIAS]])
)");
}

TEST_F(CublasLtGemmRewriteTest, BF16GemmWithBias) {
  const char* hlo_text = R"(
HloModule BF16GemmWithBias

ENTRY BF16GemmWithBias {
  x = bf16[8,8]{1,0} parameter(0)
  y = bf16[8,8]{1,0} parameter(1)
  dot.5 = bf16[8,8]{1,0} dot(x, y), lhs_contracting_dims={1}, rhs_contracting_dims={0}
  bias = bf16[8,8]{1,0} parameter(2)
  ROOT add.6 = bf16[8,8]{1,0} add(dot.5, bias)
}
  )";

  EXPECT_TRUE(RunAndCompare(hlo_text, ErrorSpec{1e-3, 1e-3}));
  MatchOptimizedHlo(hlo_text,
                    R"(
; CHECK-LABEL: ENTRY %BF16GemmWithBias (x: bf16[8,8], y: bf16[8,8], bias: bf16[8,8]) -> bf16[8,8] {
; CHECK-DAG:    [[X:%[^ ]+]] = bf16[8,8]{1,0} parameter(0)
; CHECK-DAG:    [[Y:%[^ ]+]] = bf16[8,8]{1,0} parameter(1)
; CHECK-DAG:    [[BIAS:%[^ ]+]] = bf16[8,8]{1,0} parameter(2)
; CHECK-NEXT:   ROOT [[GEMM:%[^ ]+]] = bf16[8,8]{1,0} custom-call([[X]], [[Y]], [[BIAS]]),
; CHECK:           custom_call_target="__cublas$lt$matmul",
; CHECK:           backend_config="{
; CHECK-DAG:         \"alpha_real\":1
; CHECK-DAG:         \"alpha_imag\":0
; CHECK-DAG:         \"beta\":1
; CHECK-DAG:         \"dot_dimension_numbers\":{
; CHECK-DAG:           \"lhs_contracting_dimensions\":[\"1\"]
; CHECK-DAG:           \"rhs_contracting_dimensions\":[\"0\"]
; CHECK-DAG:           \"lhs_batch_dimensions\":[]
; CHECK-DAG:           \"rhs_batch_dimensions\":[]
; CHECK-DAG:         }
; CHECK-DAG:         \"precision_config\":{
; CHECK-DAG:           \"operand_precision\":[\"DEFAULT\",\"DEFAULT\"]
; CHECK-DAG:         }
; CHECK-DAG:         \"epilogue\":\"DEFAULT\"
; CHECK:           }"
)");
}

TEST_F(CublasLtGemmRewriteTest, MatrixBias) {
  const char* hlo_text = R"(
HloModule test

ENTRY test {
  x = f32[2,3] parameter(0)
  y = f32[3,4] parameter(1)
  z = f32[2,4] parameter(2)
  dot_a = f32[2,4] dot(x, y), lhs_contracting_dims={1}, rhs_contracting_dims={0}
  ROOT out = f32[2,4] add(dot_a, z)
}

)";

  EXPECT_TRUE(RunAndCompare(hlo_text, ErrorSpec{1e-5, 1e-5}));
  MatchOptimizedHlo(hlo_text,
                    R"(
; CHECK-LABEL: ENTRY %test (x: f32[2,3], y: f32[3,4], z: f32[2,4]) -> f32[2,4] {
; CHECK-NEXT:    [[P0:%[^ ]+]] = f32[2,3]{1,0} parameter(0)
; CHECK-NEXT:    [[P1:%[^ ]+]] = f32[3,4]{1,0} parameter(1)
; CHECK-NEXT:    [[P2:%[^ ]+]] = f32[2,4]{1,0} parameter(2)
; CHECK-NEXT:    ROOT [[GEMM:%[^ ]+]] = f32[2,4]{1,0} custom-call([[P0]], [[P1]], [[P2]]),
; CHECK:           custom_call_target="__cublas$lt$matmul",
; CHECK:           backend_config="{
; CHECK-DAG:         \"alpha_real\":1
; CHECK-DAG:         \"alpha_imag\":0
; CHECK-DAG:         \"beta\":1
; CHECK-DAG:         \"dot_dimension_numbers\":{
; CHECK-DAG:           \"lhs_contracting_dimensions\":[\"1\"]
; CHECK-DAG:           \"rhs_contracting_dimensions\":[\"0\"]
; CHECK-DAG:           \"lhs_batch_dimensions\":[]
; CHECK-DAG:           \"rhs_batch_dimensions\":[]
; CHECK-DAG:         }
; CHECK-DAG:         \"precision_config\":{
; CHECK-DAG:           \"operand_precision\":[\"DEFAULT\",\"DEFAULT\"]
; CHECK-DAG:         }
; CHECK-DAG:         \"epilogue\":\"DEFAULT\"
; CHECK:           }"
)");
}

TEST_F(CublasLtGemmRewriteTest, MatrixBiasWhereBiasIsNotAParameter) {
  const char* hlo_text = R"(
HloModule test

ENTRY test {
  w = f32[2,3] parameter(0)
  x = f32[3,4] parameter(1)
  first_dot = f32[2,4] dot(w, x), lhs_contracting_dims={1}, rhs_contracting_dims={0}
  y = f32[2,3] parameter(2)
  z = f32[3,4] parameter(3)
  second_dot = f32[2,4] dot(y, z), lhs_contracting_dims={1}, rhs_contracting_dims={0}
  ROOT out = f32[2,4] add(second_dot, first_dot)
}

)";

  EXPECT_TRUE(RunAndCompare(hlo_text, ErrorSpec{1e-5, 1e-5}));
  MatchOptimizedHlo(hlo_text,
                    R"(
; CHECK-LABEL: ENTRY %test (w: f32[2,3], x: f32[3,4], y: f32[2,3], z: f32[3,4]) -> f32[2,4] {
; CHECK-DAG:     [[P0:%[^ ]+]] = f32[2,3]{1,0} parameter(0)
; CHECK-DAG:     [[P1:%[^ ]+]] = f32[3,4]{1,0} parameter(1)
; CHECK-DAG:     [[P2:%[^ ]+]] = f32[2,3]{1,0} parameter(2)
; CHECK-DAG:     [[P3:%[^ ]+]] = f32[3,4]{1,0} parameter(3)
; CHECK-NEXT:    [[FIRST_GEMM:%[^ ]+]] = f32[2,4]{1,0} custom-call([[P0]], [[P1]]),
; CHECK:           custom_call_target="__cublas$lt$matmul",
; CHECK:           backend_config="{
; CHECK-DAG:         \"alpha_real\":1
; CHECK-DAG:         \"alpha_imag\":0
; CHECK-DAG:         \"beta\":0
; CHECK-DAG:         \"dot_dimension_numbers\":{
; CHECK-DAG:           \"lhs_contracting_dimensions\":[\"1\"]
; CHECK-DAG:           \"rhs_contracting_dimensions\":[\"0\"]
; CHECK-DAG:           \"lhs_batch_dimensions\":[]
; CHECK-DAG:           \"rhs_batch_dimensions\":[]
; CHECK-DAG:         }
; CHECK-DAG:         \"precision_config\":{
; CHECK-DAG:           \"operand_precision\":[\"DEFAULT\",\"DEFAULT\"]
; CHECK-DAG:         }
; CHECK-DAG:         \"epilogue\":\"DEFAULT\"
; CHECK:           }"
; CHECK-NEXT:    ROOT [[SECOND_GEMM:%[^ ]+]] = f32[2,4]{1,0} custom-call([[P2]], [[P3]], [[FIRST_GEMM]]),
; CHECK:           custom_call_target="__cublas$lt$matmul",
; CHECK:           output_to_operand_aliasing={{{{}: \(2, {}\)}}},
; CHECK:           backend_config="{
; CHECK-DAG:         \"alpha_real\":1
; CHECK-DAG:         \"alpha_imag\":0
; CHECK-DAG:         \"beta\":1
; CHECK-DAG:         \"dot_dimension_numbers\":{
; CHECK-DAG:           \"lhs_contracting_dimensions\":[\"1\"]
; CHECK-DAG:           \"rhs_contracting_dimensions\":[\"0\"]
; CHECK-DAG:           \"lhs_batch_dimensions\":[]
; CHECK-DAG:           \"rhs_batch_dimensions\":[]
; CHECK-DAG:         }
; CHECK-DAG:         \"precision_config\":{
; CHECK-DAG:           \"operand_precision\":[\"DEFAULT\",\"DEFAULT\"]
; CHECK-DAG:         }
; CHECK-DAG:         \"epilogue\":\"DEFAULT\"
; CHECK:           }"
)");
}

TEST_F(CublasLtGemmRewriteTest, VectorBias) {
  const char* hlo_text = R"(
HloModule test

ENTRY test {
  x = f32[2,3] parameter(0)
  y = f32[3,4] parameter(1)
  z = f32[4] parameter(2)
  dot_a = f32[2,4] dot(x, y), lhs_contracting_dims={1}, rhs_contracting_dims={0}
  z_bcast = f32[2,4] broadcast(z), dimensions={1}
  ROOT out = f32[2,4] add(dot_a, z_bcast)
}

)";

  EXPECT_TRUE(RunAndCompare(hlo_text, ErrorSpec{1e-5, 1e-5}));
  MatchOptimizedHlo(hlo_text,
                    R"(
; CHECK-LABEL: ENTRY %test (x: f32[2,3], y: f32[3,4], z: f32[4]) -> f32[2,4] {
; CHECK-NEXT:    [[P0:%[^ ]+]] = f32[2,3]{1,0} parameter(0)
; CHECK-NEXT:    [[P1:%[^ ]+]] = f32[3,4]{1,0} parameter(1)
; CHECK-NEXT:    [[P2:%[^ ]+]] = f32[4]{0} parameter(2)
; CHECK-NEXT:    ROOT [[OUT:%[^ ]+]] = f32[2,4]{1,0} custom-call([[P0]], [[P1]], [[P2]]),
; CHECK:           custom_call_target="__cublas$lt$matmul",
; CHECK:           backend_config="{
; CHECK-DAG:         \"alpha_real\":1
; CHECK-DAG:         \"alpha_imag\":0
; CHECK-DAG:         \"beta\":0
; CHECK-DAG:         \"dot_dimension_numbers\":{
; CHECK-DAG:           \"lhs_contracting_dimensions\":[\"1\"]
; CHECK-DAG:           \"rhs_contracting_dimensions\":[\"0\"]
; CHECK-DAG:           \"lhs_batch_dimensions\":[]
; CHECK-DAG:           \"rhs_batch_dimensions\":[]
; CHECK-DAG:         }
; CHECK-DAG:         \"precision_config\":{
; CHECK-DAG:           \"operand_precision\":[\"DEFAULT\",\"DEFAULT\"]
; CHECK-DAG:         }
; CHECK-DAG:         \"epilogue\":\"BIAS\"
; CHECK:           }"
)");
}

TEST_F(CublasLtGemmRewriteTest, VectorBiasTransposed) {
  const char* hlo_text = R"(
HloModule test

ENTRY test {
  x = f32[2,3] parameter(0)
  y = f32[3,4] parameter(1)
  z = f32[2] parameter(2)
  dot_a = f32[2,4] dot(x, y), lhs_contracting_dims={1}, rhs_contracting_dims={0}
  z_bcast = f32[2,4] broadcast(z), dimensions={0}
  add = f32[2,4] add(dot_a, z_bcast)
  ROOT out = f32[4,2] transpose(add), dimensions={1,0}
}

)";

  EXPECT_TRUE(RunAndCompare(hlo_text, ErrorSpec{1e-5, 1e-5}));
  MatchOptimizedHlo(hlo_text,
                    R"(
; CHECK-LABEL: ENTRY %test (x: f32[2,3], y: f32[3,4], z: f32[2]) -> f32[4,2] {
; CHECK-NEXT:    [[P0:%[^ ]+]] = f32[2,3]{1,0} parameter(0)
; CHECK-NEXT:    [[P1:%[^ ]+]] = f32[3,4]{1,0} parameter(1)
; CHECK-NEXT:    [[P2:%[^ ]+]] = f32[2]{0} parameter(2)
; CHECK-NEXT:    [[MATMUL:%[^ ]+]] = f32[2,4]{0,1} custom-call([[P0]], [[P1]], [[P2]]),
; CHECK:           custom_call_target="__cublas$lt$matmul",
; CHECK:           backend_config="{
; CHECK-DAG:         \"alpha_real\":1
; CHECK-DAG:         \"alpha_imag\":0
; CHECK-DAG:         \"beta\":0
; CHECK-DAG:         \"dot_dimension_numbers\":{
; CHECK-DAG:           \"lhs_contracting_dimensions\":[\"1\"]
; CHECK-DAG:           \"rhs_contracting_dimensions\":[\"0\"]
; CHECK-DAG:           \"lhs_batch_dimensions\":[]
; CHECK-DAG:           \"rhs_batch_dimensions\":[]
; CHECK-DAG:         }
; CHECK-DAG:         \"precision_config\":{
; CHECK-DAG:           \"operand_precision\":[\"DEFAULT\",\"DEFAULT\"]
; CHECK-DAG:         }
; CHECK-DAG:         \"epilogue\":\"BIAS\"
; CHECK:           }"
; CHECK-NEXT:    ROOT [[OUT:%[^ ]+]] = f32[4,2]{1,0} bitcast([[MATMUL]])
)");
}

TEST_F(CublasLtGemmRewriteTest, VectorBiasIncorrectAxisFusedAsMatrix) {
  const char* hlo_text = R"(
HloModule test

ENTRY test {
  x = f32[2,3] parameter(0)
  y = f32[3,4] parameter(1)
  z = f32[2] parameter(2)
  dot_a = f32[2,4] dot(x, y), lhs_contracting_dims={1}, rhs_contracting_dims={0}
  z_bcast = f32[2,4] broadcast(z), dimensions={0}
  ROOT out = f32[2,4] add(dot_a, z_bcast)
}

)";

  EXPECT_TRUE(RunAndCompare(hlo_text, ErrorSpec{1e-5, 1e-5}));
  MatchOptimizedHlo(hlo_text,
                    R"(
; CHECK-LABEL: ENTRY %test (x: f32[2,3], y: f32[3,4], z: f32[2]) -> f32[2,4] {
; CHECK-NEXT:    [[P0:%[^ ]+]] = f32[2,3]{1,0} parameter(0)
; CHECK-NEXT:    [[P1:%[^ ]+]] = f32[3,4]{1,0} parameter(1)
; CHECK-NEXT:    [[P2:%[^ ]+]] = f32[2]{0} parameter(2)
; CHECK-NEXT:    [[P2_BCAST:%[^ ]+]] = f32[2,4]{1,0} broadcast([[P2]]), dimensions={0}
; CHECK-NEXT:    ROOT [[OUT:%[^ ]+]] = f32[2,4]{1,0} custom-call([[P0]], [[P1]], [[P2_BCAST]]),
; CHECK:           custom_call_target="__cublas$lt$matmul",
; CHECK:           backend_config="{
; CHECK-DAG:         \"alpha_real\":1
; CHECK-DAG:         \"alpha_imag\":0
; CHECK-DAG:         \"beta\":1
; CHECK-DAG:         \"dot_dimension_numbers\":{
; CHECK-DAG:           \"lhs_contracting_dimensions\":[\"1\"]
; CHECK-DAG:           \"rhs_contracting_dimensions\":[\"0\"]
; CHECK-DAG:           \"lhs_batch_dimensions\":[]
; CHECK-DAG:           \"rhs_batch_dimensions\":[]
; CHECK-DAG:         }
; CHECK-DAG:         \"precision_config\":{
; CHECK-DAG:           \"operand_precision\":[\"DEFAULT\",\"DEFAULT\"]
; CHECK-DAG:         }
; CHECK-DAG:         \"epilogue\":\"DEFAULT\"
; CHECK:           }"
)");
}

TEST_F(CublasLtGemmRewriteTest, VectorBiasThenMatrixBias) {
  const char* hlo_text = R"(
HloModule test

ENTRY test {
  x = f32[2,3] parameter(0)
  y = f32[3,4] parameter(1)
  z = f32[4] parameter(2)
  z2 = f32[2,4] parameter(3)
  dot_a = f32[2,4] dot(x, y), lhs_contracting_dims={1}, rhs_contracting_dims={0}
  z_bcast = f32[2,4] broadcast(z), dimensions={1}
  add0 = f32[2,4] add(dot_a, z_bcast)
  ROOT add1 = f32[2,4] add(add0, z2)
}

)";

  EXPECT_TRUE(RunAndCompare(hlo_text, ErrorSpec{1e-5, 1e-5}));
  MatchOptimizedHlo(hlo_text,
                    R"(
; CHECK-LABEL: ENTRY %test (x: f32[2,3], y: f32[3,4], z: f32[4], z2: f32[2,4]) -> f32[2,4] {
; CHECK-DAG:     [[P0:%[^ ]+]] = f32[2,3]{1,0} parameter(0)
; CHECK-DAG:     [[P1:%[^ ]+]] = f32[3,4]{1,0} parameter(1)
; CHECK-DAG:     [[VECTOR_BIAS:%[^ ]+]] = f32[4]{0} parameter(2)
; CHECK-DAG:     [[MATRIX_BIAS:%[^ ]+]] = f32[2,4]{1,0} parameter(3)
; CHECK-NEXT:    ROOT [[OUT:%[^ ]+]] = f32[2,4]{1,0} custom-call([[P0]], [[P1]], [[MATRIX_BIAS]], [[VECTOR_BIAS]]),
; CHECK:           custom_call_target="__cublas$lt$matmul",
; CHECK:           backend_config="{
; CHECK-DAG:         \"alpha_real\":1
; CHECK-DAG:         \"alpha_imag\":0
; CHECK-DAG:         \"beta\":1
; CHECK-DAG:         \"dot_dimension_numbers\":{
; CHECK-DAG:           \"lhs_contracting_dimensions\":[\"1\"]
; CHECK-DAG:           \"rhs_contracting_dimensions\":[\"0\"]
; CHECK-DAG:           \"lhs_batch_dimensions\":[]
; CHECK-DAG:           \"rhs_batch_dimensions\":[]
; CHECK-DAG:         }
; CHECK-DAG:         \"precision_config\":{
; CHECK-DAG:           \"operand_precision\":[\"DEFAULT\",\"DEFAULT\"]
; CHECK-DAG:         }
; CHECK-DAG:         \"epilogue\":\"BIAS\"
; CHECK:           }"
)");
}

TEST_F(CublasLtGemmRewriteTest, MergeBitcastAndAdd) {
  const char* hlo_text = R"(
HloModule test
ENTRY test {
  x = f32[2,2] parameter(0)
  y = f32[2,2] parameter(1)
  bias = f32[4] parameter(2)
  dot = f32[2,2] dot(x, y), lhs_contracting_dims={1}, rhs_contracting_dims={0}
  ROOT out = f32[4] add(f32[4] bitcast(dot), bias)
}
)";

  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo_text));
  GemmRewriter pass(GetCudaComputeCapability());
  TF_ASSERT_OK_AND_ASSIGN(bool changed, this->RunHloPass(&pass, module.get()));
  EXPECT_TRUE(changed);

  EXPECT_THAT(module->entry_computation()->root_instruction(),
              GmockMatch(m::Add(m::Bitcast(m::CustomCall("__cublas$lt$matmul",
                                                         m::Parameter(0),
                                                         m::Parameter(1))
                                               .WithShape(F32, {2, 2}))
                                    .WithShape(F32, {4}),
                                m::Parameter(2))
                             .WithShape(F32, {4})));
}

TEST_F(CublasLtGemmRewriteTest, FoldConstantBias) {
  const char* hlo_text = R"(
HloModule test
ENTRY test {
  x = f32[2,2] parameter(0)
  y = f32[2,2] parameter(1)
  bias = f32[2,2] broadcast(f32[2] constant({0, 0})), dimensions={0}

  dot1 = f32[2,2] dot(x, y), lhs_contracting_dims={1}, rhs_contracting_dims={0}
  bias1 = f32[2,2] broadcast(f32[2] constant({0, 0})), dimensions={0}
  sum1 = add(dot1, bias1)

  dot2 = f32[2,2] dot(x, y), lhs_contracting_dims={1}, rhs_contracting_dims={0}
  sum2 = add(dot2, f32[2,2] reshape(bias))

  dot3 = f32[2,2] dot(x, y), lhs_contracting_dims={1}, rhs_contracting_dims={0}
  bias3 = f32[2,2] transpose(bias), dimensions={1,0}
  sum3 = add(dot3, bias3)

  dot4 = f32[2,2] dot(x, y), lhs_contracting_dims={1}, rhs_contracting_dims={0}
  sum4 = add(dot4, f32[2,2] bitcast(bias))

  ROOT root = tuple(sum1, sum2, sum3, sum4)
}
)";

  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> module,
                          ParseAndReturnVerifiedModule(hlo_text));
  GemmRewriter pass(GetCudaComputeCapability());
  TF_ASSERT_OK_AND_ASSIGN(bool changed, this->RunHloPass(&pass, module.get()));
  SCOPED_TRACE(module->ToString());
  EXPECT_TRUE(changed);

  EXPECT_THAT(
      module->entry_computation()->root_instruction(),
      GmockMatch(m::Tuple(
          m::CustomCall(m::Parameter(0), m::Parameter(1), m::Constant()),
          m::CustomCall(m::Parameter(0), m::Parameter(1), m::Constant()),
          m::CustomCall(m::Parameter(0), m::Parameter(1), m::Constant()),
          m::CustomCall(m::Parameter(0), m::Parameter(1), m::Constant()))));
}

class GemmRewriteAllocationTest : public GpuCodegenTest {
 public:
  void CheckNumberOfAllocations(const std::string& hlo,
                                int expected_number_of_allocations) {
    TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<HloModule> optimized_module,
                            GetOptimizedModule(hlo));
    TF_ASSERT_OK_AND_ASSIGN(
        std::unique_ptr<Executable> executable,
        backend().compiler()->RunBackend(
            std::move(optimized_module), backend().default_stream_executor(),
            backend().default_stream_executor()->GetAllocator()));
    GpuExecutable* gpu_executable =
        static_cast<GpuExecutable*>(executable.get());
    absl::Span<const BufferAllocation> allocations =
        gpu_executable->GetAllocations();
    CHECK_EQ(allocations.size(), expected_number_of_allocations);
  }
};

TEST_F(GemmRewriteAllocationTest, SharedBufferAssignment) {
  const char* hlo_text = R"(
HloModule SharedBufferAssignment

ENTRY AddDotsFunc {
  x = f32[2,2] parameter(0)
  y = f32[2,2] parameter(1)
  bias = f32[2,2] add(x, y)
  dot = f32[2,2] dot(x, y), lhs_contracting_dims={1}, rhs_contracting_dims={0}
  ROOT out = f32[2,2] add(dot, bias)
}

)";

  // Bias should be fused into the multiplication.
  CheckNumberOfAllocations(hlo_text, 3);
  EXPECT_TRUE(RunAndCompare(hlo_text, ErrorSpec{1e-5, 1e-5}));
}

}  // namespace
}  // namespace gpu
}  // namespace xla
