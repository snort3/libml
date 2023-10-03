/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/xla/translate/hlo_to_mhlo/mlir_hlo_builder.h"

#include <string>

#include "llvm/Support/raw_ostream.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"  // from @llvm-project
#include "mlir/IR/Attributes.h"  // from @llvm-project
#include "mlir/IR/Builders.h"  // from @llvm-project
#include "mlir/IR/BuiltinOps.h"  // from @llvm-project
#include "mlir/IR/Dialect.h"  // from @llvm-project
#include "mlir/IR/Location.h"  // from @llvm-project
#include "mlir/IR/MLIRContext.h"  // from @llvm-project
#include "tensorflow/compiler/xla/mlir_hlo/include/mlir-hlo/Dialect/mhlo/IR/hlo_ops.h"
#include "tensorflow/compiler/xla/service/hlo_module.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/translate/hlo_to_mhlo/hlo_function_importer.h"
#include "tensorflow/tsl/lib/core/status_test_util.h"

namespace xla {

namespace {

static void ExpectHasSubstr(absl::string_view s, absl::string_view expected) {
  EXPECT_TRUE(absl::StrContains(s, expected))
      << s << " does not contain " << expected;
}

class XlaBuilderTest : public ::testing::Test {
 protected:
  XlaBuilderTest()
      : name_(SetupTest()),
        module_(mlir::ModuleOp::create(mlir::UnknownLoc::get(&context_))),
        builder_(&module_->getBodyRegion()),
        xla_builder_(name_, builder_, module_->getLoc(),
                     /*build_functions=*/true) {
    context_.loadDialect<mlir::mhlo::MhloDialect>();
  }

  std::string SetupTest() {
    return ::testing::UnitTest::GetInstance()->current_test_info()->name();
  }

  // Retuns the MLIR op string representation of the given XlaOp.
  std::string GetMlirOpString(XlaOp xla_op) {
    std::string str;
    llvm::raw_string_ostream ostream{str};
    xla_builder_.GetValue(xla_op).print(ostream);
    ostream.flush();
    return str;
  }

  std::string GetMlirOpString(mlir::func::FuncOp func_op) {
    std::string func_op_mlir_str;
    llvm::raw_string_ostream ostream{func_op_mlir_str};
    func_op.print(ostream);
    return func_op_mlir_str;
  }

  std::string name_;
  mlir::MLIRContext context_;
  mlir::OwningOpRef<mlir::ModuleOp> module_;
  mlir::OpBuilder builder_;
  MlirHloBuilder xla_builder_;
};

TEST_F(XlaBuilderTest, CreateToken) {
  auto token = CreateToken(&xla_builder_);
  auto str = GetMlirOpString(token);

  TF_ASSERT_OK(xla_builder_.GetCurrentStatus());

  ExpectHasSubstr(GetMlirOpString(token), R"(mhlo.create_token : !mhlo.token)");
}

TEST_F(XlaBuilderTest, Infeed) {
  auto token = CreateToken(&xla_builder_);
  auto infeed = InfeedWithToken(token, ShapeUtil::MakeShape(F32, {4, 8}), "");

  TF_ASSERT_OK(xla_builder_.GetCurrentStatus());
  ExpectHasSubstr(
      GetMlirOpString(infeed),
      R"("mhlo.tuple"(%1#0, %1#1) : (tensor<4x8xf32>, !mhlo.token) -> tuple<tensor<4x8xf32>)");
}

TEST_F(XlaBuilderTest, Outfeed) {
  auto outfeed_shape = ShapeUtil::MakeShape(F32, {4, 8});
  auto data = ConstantLiteral(
      &xla_builder_,
      LiteralUtil::CreateFromDimensions(F32, outfeed_shape.dimensions()));
  auto token = CreateToken(&xla_builder_);
  auto outfeed = OutfeedWithToken(data, token, outfeed_shape, "");

  TF_ASSERT_OK(xla_builder_.GetCurrentStatus());
  ExpectHasSubstr(
      GetMlirOpString(outfeed),
      R"("mhlo.outfeed"(%0, %1) {outfeed_config = ""} : (tensor<4x8xf32>, !mhlo.token) -> !mhlo.token)");
}

TEST_F(XlaBuilderTest, ConcatInDim) {
  auto data0 = ConstantLiteral(
      &xla_builder_, LiteralUtil::CreateFromDimensions(F32, {2, 4, 5}));
  auto data1 = ConstantLiteral(
      &xla_builder_, LiteralUtil::CreateFromDimensions(F32, {2, 6, 5}));
  auto concat = ConcatInDim(&xla_builder_, {data0, data1}, 1);

  TF_ASSERT_OK(xla_builder_.GetCurrentStatus());
  ExpectHasSubstr(
      GetMlirOpString(concat),
      R"("mhlo.concatenate"(%0, %1) {dimension = 1 : i64} : (tensor<2x4x5xf32>, tensor<2x6x5xf32>) -> tensor<2x10x5xf32>)");
}

TEST_F(XlaBuilderTest, Tuple) {
  auto data0 = ConstantLiteral(&xla_builder_,
                               LiteralUtil::CreateFromDimensions(F32, {3, 7}));
  auto data1 = ConstantLiteral(&xla_builder_,
                               LiteralUtil::CreateFromDimensions(F32, {}));
  auto tuple = Tuple(&xla_builder_, {data0, data1});

  TF_ASSERT_OK(xla_builder_.GetCurrentStatus());
  ExpectHasSubstr(
      GetMlirOpString(tuple),
      R"("mhlo.tuple"(%0, %1) : (tensor<3x7xf32>, tensor<f32>) -> tuple<tensor<3x7xf32>, tensor<f32>>)");
}

TEST_F(XlaBuilderTest, GetTupleElement) {
  auto data0 = ConstantLiteral(&xla_builder_,
                               LiteralUtil::CreateFromDimensions(F32, {3, 7}));
  auto data1 = ConstantLiteral(&xla_builder_,
                               LiteralUtil::CreateFromDimensions(F32, {}));
  auto tuple_data = Tuple(&xla_builder_, {data0, data1});
  auto gte = GetTupleElement(tuple_data, 1);

  TF_ASSERT_OK(xla_builder_.GetCurrentStatus());
  ExpectHasSubstr(
      GetMlirOpString(gte),
      R"("mhlo.get_tuple_element"(%2) {index = 1 : i32} : (tuple<tensor<3x7xf32>, tensor<f32>>) -> tensor<f32>)");
}

TEST_F(XlaBuilderTest, Slice) {
  auto data = ConstantLiteral(&xla_builder_,
                              LiteralUtil::CreateFromDimensions(F32, {3, 7}));
  auto slice = Slice(data, {0, 1}, {2, 5}, {1, 1});

  TF_ASSERT_OK(xla_builder_.GetCurrentStatus());
  ExpectHasSubstr(
      GetMlirOpString(slice),
      R"("mhlo.slice"(%0) {limit_indices = dense<[2, 5]> : tensor<2xi64>, start_indices = dense<[0, 1]> : tensor<2xi64>, strides = dense<1> : tensor<2xi64>} : (tensor<3x7xf32>) -> tensor<2x4xf32>)");
}

TEST_F(XlaBuilderTest, Pad) {
  auto data = ConstantLiteral(&xla_builder_,
                              LiteralUtil::CreateFromDimensions(F32, {3, 7}));
  auto zero = ConstantLiteral(&xla_builder_, LiteralUtil::Zero(F32));

  PaddingConfig padding_config;
  auto* dims0 = padding_config.add_dimensions();
  dims0->set_edge_padding_low(1);
  dims0->set_interior_padding(0);
  dims0->set_edge_padding_high(2);
  auto* dims1 = padding_config.add_dimensions();
  dims1->set_edge_padding_low(3);
  dims1->set_interior_padding(1);
  dims1->set_edge_padding_high(0);
  auto pad = Pad(data, zero, padding_config);

  TF_ASSERT_OK(xla_builder_.GetCurrentStatus());
  ExpectHasSubstr(
      GetMlirOpString(pad),
      R"("mhlo.pad"(%0, %1) {edge_padding_high = dense<[2, 0]> : tensor<2xi64>, edge_padding_low = dense<[1, 3]> : tensor<2xi64>, interior_padding = dense<[0, 1]> : tensor<2xi64>} : (tensor<3x7xf32>, tensor<f32>) -> tensor<6x16xf32>)");
}

TEST_F(XlaBuilderTest, CustomCallWithComputation) {
  // Create a test comparator computation to use as the custom computation.
  // auto cmp_builder = main_func_builder.CreateSubBuilder("test_comparator");
  auto cmp_builder = xla_builder_.CreateSubBuilder("test_comparator");
  auto p0 = Parameter(cmp_builder.get(), 0,
                      xla::ShapeUtil::MakeScalarShape(xla::F32), "p0");
  auto p1 = Parameter(cmp_builder.get(), 1,
                      xla::ShapeUtil::MakeScalarShape(xla::F32), "p1");
  auto gt = Gt(p0, p1);
  StatusOr<Shape> output_shape_or = cmp_builder->GetShape(gt);
  TF_ASSERT_OK(output_shape_or.status());
  XlaComputation test_comparator = cmp_builder->BuildAndNoteError();

  // Finally, add the CustomCallOp (with computation) to the module.
  auto custom_call = CustomCallWithComputation(
      &xla_builder_, "test_call_target", {}, test_comparator,
      output_shape_or.value(),
      "{\"option1\": foo, \"option2\": bar, \"option3\": \"baz\"}");

  TF_ASSERT_OK(xla_builder_.GetCurrentStatus());

  ExpectHasSubstr(
      GetMlirOpString(custom_call),
      R"("mhlo.custom_call"() {api_version = 1 : i32, backend_config = "{\22option1\22: foo, \22option2\22: bar, \22option3\22: \22baz\22}", call_target_name = "test_call_target", called_computations = [@test_comparator.4], has_side_effect = false} : () -> tensor<i1>)");

  // We should also expect there to be a new function added for the comparator.
  auto actual_func_op = module_->lookupSymbol<mlir::func::FuncOp>(
      test_comparator.proto().computations().at(0).name());
  EXPECT_TRUE(actual_func_op);
  EXPECT_EQ(
      GetMlirOpString(actual_func_op),
      R"(func.func private @test_comparator.4(%arg0: tensor<f32>, %arg1: tensor<f32>) -> tensor<i1> {
  %1 = mhlo.compare  GT, %arg0, %arg1 : (tensor<f32>, tensor<f32>) -> tensor<i1>
  return %1 : tensor<i1>
})");
}

}  // namespace
}  // namespace xla
