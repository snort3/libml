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

// Fuse tf.Op + tf.BiasAdd and legalized to TOSA

#include <climits>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <numeric>

#include "mlir/Dialect/Tosa/IR/TosaOps.h"  // from @llvm-project
#include "mlir/IR/MLIRContext.h"  // from @llvm-project
#include "mlir/Pass/Pass.h"  // from @llvm-project
#include "mlir/Support/LogicalResult.h"  // from @llvm-project
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/lite/quantization/ir/QuantOps.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_ops.h"
#include "tensorflow/compiler/mlir/tosa/transforms/legalize_common.h"
#include "tensorflow/compiler/mlir/tosa/transforms/passes.h"

#define PASS_NAME "tosa-fuse-bias-tf"
#define DEBUG_TYPE PASS_NAME

namespace mlir {
namespace tosa {
namespace {

#define GEN_PASS_DEF_TOSAFUSEBIASTFPASS
#include "tensorflow/compiler/mlir/tosa/transforms/passes.h.inc"

class FuseBiasTF : public impl::TosaFusebiasTFPassBase<FuseBiasTF> {
 public:
  explicit FuseBiasTF() {}
  void runOnOperation() override;
};

struct ConvertTFBiasAddOp : public RewritePattern {
  explicit ConvertTFBiasAddOp(MLIRContext* context)
      : RewritePattern(TF::BiasAddOp::getOperationName(), 1, context) {}
  LogicalResult matchAndRewrite(Operation* op,
                                PatternRewriter& rewriter) const override;
};

// Replaces the following pattern, take conv2d as an example:
//   %1 = tf.Conv2D (%ifm, %filter)
//   %2 = tf.BiasAdd(%1, %bias)
//   with
//   %1 = tosa.conv2d(%ifm, %filter, %bias)
//   This can also be done using the pair ot Pat<> options in
//   tf_optimize_patterns.td
//   However, this explicit code can handle both when the LHS or RHS is the
//   defining conv2d op.
// TODO: support other pattern. e.g. tf.DepthwiseConv2DNative

LogicalResult ConvertTFBiasAddOp::matchAndRewrite(
    Operation* op, PatternRewriter& rewriter) const {
  auto tf_biasadd_op = cast<TF::BiasAddOp>(op);
  auto output_type =
      tf_biasadd_op.getResult().getType().dyn_cast<RankedTensorType>();

  if (!output_type) {
    return rewriter.notifyMatchFailure(op, "output not a ranked tensor");
  }

  auto value = tf_biasadd_op.value();
  auto bias = tf_biasadd_op.bias();
  auto bias_shape = bias.getType().cast<RankedTensorType>().getShape();
  if (bias_shape.size() != 1) {
    return rewriter.notifyMatchFailure(op, "bias tensor must be rank 1");
  }

  if (TF::Conv2DOp tf_conv2d_op =
          llvm::dyn_cast_if_present<TF::Conv2DOp>(value.getDefiningOp())) {
    // Sanity check to confirm rhs() has the expected shape of bias
    auto filter_shape =
        tf_conv2d_op.filter().getType().cast<RankedTensorType>().getShape();

    // Assume the filter shape is [H, W, I, O]
    if (filter_shape.back() != bias_shape.back()) {
      return rewriter.notifyMatchFailure(
          op, "bias dimension must match filter output channels");
    }

    auto result = convertTFConv2DCommon(
        rewriter, op, output_type, tf_conv2d_op.input(), tf_conv2d_op.filter(),
        bias, tf_conv2d_op.strides(), tf_conv2d_op.dilations(),
        tf_conv2d_op.explicit_paddings(), tf_conv2d_op.padding(),
        tf_conv2d_op.data_format());

    if (!result) return failure();

    rewriter.replaceOp(op, {result.value()});

    return success();
  }

  if (TF::Conv3DOp tf_conv3d_op =
          llvm::dyn_cast_if_present<TF::Conv3DOp>(value.getDefiningOp())) {
    // Sanity check to confirm rhs() has the expected shape of bias
    auto filter_shape =
        tf_conv3d_op.filter().getType().cast<RankedTensorType>().getShape();

    // Assume the filter shape is [D, H, W, I, O]
    if (filter_shape.back() != bias_shape.back()) {
      return rewriter.notifyMatchFailure(
          op, "bias dimension must match filter output channels");
    }

    llvm::Optional<Value> result = convertTFConv3DCommon(
        rewriter, op, output_type, tf_conv3d_op.input(), tf_conv3d_op.filter(),
        bias, tf_conv3d_op.strides(), tf_conv3d_op.dilations(),
        tf_conv3d_op.padding(), tf_conv3d_op.data_format());

    if (!result) return failure();

    rewriter.replaceOp(op, {result.value()});

    return success();
  }

  return failure();
}

void FuseBiasTF::runOnOperation() {
  RewritePatternSet patterns(&getContext());
  auto* ctx = &getContext();
  auto func = getOperation();

  // Add the generated patterns to the list.
  patterns.add<ConvertTFBiasAddOp>(ctx);
  (void)applyPatternsAndFoldGreedily(func, std::move(patterns));
}

}  // anonymous namespace

std::unique_ptr<OperationPass<func::FuncOp>> createFuseBiasTFPass() {
  return std::make_unique<FuseBiasTF>();
}

}  // namespace tosa

}  // namespace mlir
