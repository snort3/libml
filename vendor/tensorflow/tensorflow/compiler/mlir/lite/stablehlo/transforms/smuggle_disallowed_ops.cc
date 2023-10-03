/* Copyright 2021 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/mlir/lite/stablehlo/transforms/smuggle_disallowed_ops.h"

#include <utility>
#include <vector>

#include "llvm/ADT/StringRef.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"  // from @llvm-project
#include "mlir/IR/PatternMatch.h"  // from @llvm-project
#include "mlir/IR/Value.h"  // from @llvm-project
#include "mlir/Transforms/DialectConversion.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_ops.h"
#include "tensorflow/compiler/xla/mlir_hlo/include/mlir-hlo/Dialect/mhlo/IR/hlo_ops.h"

namespace mlir {
namespace TFL {
namespace mhlo {

namespace {

LogicalResult SmuggleOp(Operation* op, PatternRewriter& rewriter) {
  auto call_target =
      rewriter.getNamedAttr("call_target_name", op->getName().getIdentifier());
  auto custom_call = rewriter.create<mlir::mhlo::CustomCallOp>(
      op->getLoc(), op->getResultTypes(), op->getOperands(),
      ArrayRef<NamedAttribute>{call_target});
  rewriter.replaceOp(op, custom_call.getResults());
  return success();
}

}  // namespace

class SmuggleTFResizeBilinearOpPattern
    : public OpRewritePattern<TF::ResizeBilinearOp> {
 public:
  using OpRewritePattern<TF::ResizeBilinearOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(TF::ResizeBilinearOp op,
                                PatternRewriter& rewriter) const override {
    return SmuggleOp(op, rewriter);
  }
};

class SmuggleDisallowedOpsPass
    : public PassWrapper<SmuggleDisallowedOpsPass,
                         OperationPass<func::FuncOp>> {
 public:
  StringRef getArgument() const final { return "smuggle-disallowed-ops-pass"; }
  StringRef getDescription() const final {
    return "Smuggle disallowed ops via mhlo.custom_calls";
  }

  void runOnOperation() override {
    RewritePatternSet patterns(&getContext());
    patterns.add<SmuggleTFResizeBilinearOpPattern>(&getContext());

    ConversionTarget target(getContext());
    target.addIllegalOp<TF::ResizeBilinearOp>();
    target.addLegalDialect<mlir::mhlo::MhloDialect>();
    if (failed(applyPartialConversion(getOperation(), target,
                                      std::move(patterns)))) {
      signalPassFailure();
    }
  }
};

std::unique_ptr<Pass> CreateSmuggleDisallowedOpsPass() {
  return std::make_unique<SmuggleDisallowedOpsPass>();
}

static PassRegistration<SmuggleDisallowedOpsPass> pass;

}  // namespace mhlo
}  // namespace TFL
}  // namespace mlir
