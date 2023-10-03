/* Copyright 2022 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/xla/mlir/transforms/runtime/compilation_pipeline_cpu.h"

#include <utility>

#include "mlir/Conversion/AffineToStandard/AffineToStandard.h"  // from @llvm-project
#include "mlir/Conversion/AsyncToLLVM/AsyncToLLVM.h"  // from @llvm-project
#include "mlir/Conversion/ComplexToLLVM/ComplexToLLVM.h"  // from @llvm-project
#include "mlir/Conversion/FuncToLLVM/ConvertFuncToLLVMPass.h"  // from @llvm-project
#include "mlir/Conversion/LinalgToLLVM/LinalgToLLVM.h"  // from @llvm-project
#include "mlir/Conversion/MathToLLVM/MathToLLVM.h"  // from @llvm-project
#include "mlir/Conversion/MathToLibm/MathToLibm.h"  // from @llvm-project
#include "mlir/Conversion/MemRefToLLVM/MemRefToLLVM.h"  // from @llvm-project
#include "mlir/Conversion/ReconcileUnrealizedCasts/ReconcileUnrealizedCasts.h"  // from @llvm-project
#include "mlir/Conversion/SCFToControlFlow/SCFToControlFlow.h"  // from @llvm-project
#include "mlir/Conversion/VectorToLLVM/ConvertVectorToLLVM.h"  // from @llvm-project
#include "mlir/Dialect/Affine/IR/AffineOps.h"  // from @llvm-project
#include "mlir/Dialect/Arith/IR/Arith.h"  // from @llvm-project
#include "mlir/Dialect/Arith/Transforms/Passes.h"  // from @llvm-project
#include "mlir/Dialect/Async/IR/Async.h"  // from @llvm-project
#include "mlir/Dialect/Async/Passes.h"  // from @llvm-project
#include "mlir/Dialect/ControlFlow/IR/ControlFlow.h"  // from @llvm-project
#include "mlir/Dialect/Func/IR/FuncOps.h"  // from @llvm-project
#include "mlir/Dialect/Linalg/Passes.h"  // from @llvm-project
#include "mlir/Dialect/Math/IR/Math.h"  // from @llvm-project
#include "mlir/Dialect/MemRef/IR/MemRef.h"  // from @llvm-project
#include "mlir/Dialect/MemRef/Transforms/Passes.h"  // from @llvm-project
#include "mlir/Dialect/SCF/IR/SCF.h"  // from @llvm-project
#include "mlir/Pass/Pass.h"  // from @llvm-project
#include "mlir/Target/LLVMIR/Dialect/AMX/AMXToLLVMIRTranslation.h"  // from @llvm-project
#include "mlir/Target/LLVMIR/Dialect/ArmNeon/ArmNeonToLLVMIRTranslation.h"  // from @llvm-project
#include "mlir/Target/LLVMIR/Dialect/ArmSVE/ArmSVEToLLVMIRTranslation.h"  // from @llvm-project
#include "mlir/Target/LLVMIR/Dialect/LLVMIR/LLVMToLLVMIRTranslation.h"  // from @llvm-project
#include "mlir/Target/LLVMIR/Dialect/X86Vector/X86VectorToLLVMIRTranslation.h"  // from @llvm-project
#include "mlir/Transforms/Passes.h"  // from @llvm-project
#include "tensorflow/compiler/xla/mlir/transforms/math/passes.h"
#include "tensorflow/compiler/xla/mlir/transforms/memref/passes.h"
#include "tensorflow/compiler/xla/mlir/transforms/runtime/compiler.h"
#include "tensorflow/compiler/xla/mlir/transforms/runtime/custom_call_encoding.h"
#include "tensorflow/compiler/xla/mlir/transforms/runtime/passes.h"

namespace xla {
namespace runtime {

void RegisterDefaultXlaCpuRuntimeDialects(DialectRegistry& dialects) {
  // Register MLIR dialects supported by the compiled executables.
  dialects->insert<mlir::AffineDialect, mlir::arith::ArithDialect,
                   mlir::async::AsyncDialect, mlir::cf::ControlFlowDialect,
                   mlir::linalg::LinalgDialect, mlir::math::MathDialect,
                   mlir::memref::MemRefDialect, mlir::scf::SCFDialect,
                   mlir::func::FuncDialect, mlir::tensor::TensorDialect,
                   mlir::vector::VectorDialect, RuntimeDialect>();

  // Register MLIR dialects that can be translated to LLVM IR.
  mlir::registerArmNeonDialectTranslation(*dialects);
  mlir::registerAMXDialectTranslation(*dialects);
  mlir::registerArmSVEDialectTranslation(*dialects);
  mlir::registerLLVMDialectTranslation(*dialects);
  mlir::registerX86VectorDialectTranslation(*dialects);
}

void CreateDefaultXlaCpuRuntimeCompilationPipeline(
    PassManager& passes, const CpuPipelineOptions& opts) {
  // Convert entry function to the XLA entrypoint.
  passes->addPass(CreateExportRuntimeFunctionsPass());
  passes->addPass(CreateConvertCustomCallsPass());
  passes->addPass(CreateConvertAssertsPass());

  passes->addPass(mlir::createInlinerPass());
  passes->addPass(mlir::createCanonicalizerPass());
  passes->addPass(mlir::createCSEPass());

  // Optimize operations from the math dialect before outlining compute regions
  // into functions to see all constant operands.
  passes->addNestedPass<mlir::func::FuncOp>(
      xla::runtime::CreateMathOptimizationPass(opts.math_avx2));

  // Convert all linalg operations to parallel loops.
  passes->addNestedPass<mlir::func::FuncOp>(
      mlir::createConvertLinalgToParallelLoopsPass());
  // Canonicalize generated scf.parallel operations to remove single iterations.
  passes->addPass(mlir::createCanonicalizerPass());

  // TODO(ecg,ezhulenev): add missing conversion of scf.parallel to async work.

  // Lower from high level async operations to async runtime.
  passes->addPass(mlir::createAsyncToAsyncRuntimePass());

  // Add async.runtime reference counting operations.
  passes->addPass(mlir::createAsyncRuntimePolicyBasedRefCountingPass());

  // Expand math operations into std/arith dialect operations.
  passes->addNestedPass<mlir::func::FuncOp>(
      mlir::arith::createArithExpandOpsPass());
  passes->addNestedPass<mlir::func::FuncOp>(
      mlir::memref::createExpandOpsPass());

  // Add alignment attribute to all memref allocations.
  passes->addNestedPass<mlir::func::FuncOp>(
      xla::runtime::CreateAlignedAllocationsPass(opts.alignment));

  // Lower everything down to LLVM dialect.
  passes->addPass(mlir::createConvertLinalgToLLVMPass());
  passes->addPass(mlir::createLowerAffinePass());
  passes->addPass(mlir::createConvertSCFToCFPass());

  // Convert runtime operations and custom calls to LLVM dialect.
  const CompilationPipelineOptions& copts = opts.common_options;
  ConvertRuntimeToLLvmOpts rt_to_llvm_opts = {
      copts.populate_type_id_names, copts.populate_type_conversions,
      copts.populate_arg_encodings, copts.populate_ret_encodings,
      copts.populate_attr_encodings};
  passes->addPass(CreateConvertRuntimeToLLVMPass(std::move(rt_to_llvm_opts)));

  // Convert async dialect to LLVM once everything else is in the LLVM dialect.
  passes->addPass(mlir::createConvertAsyncToLLVMPass());

  {
    mlir::OpPassManager& fpm = passes->nest<mlir::func::FuncOp>();
    fpm.addPass(mlir::createConvertMathToLLVMPass());
  }
  passes->addPass(mlir::createConvertMathToLibmPass());

  // Convert everything else to LLVM dialect.
  mlir::LowerVectorToLLVMOptions vector_to_llvm_opts;
  if (opts.math_avx2) vector_to_llvm_opts.enableX86Vector();
  passes->addPass(mlir::createConvertVectorToLLVMPass(vector_to_llvm_opts));
  passes->addPass(mlir::createMemRefToLLVMConversionPass());
  passes->addPass(mlir::createConvertFuncToLLVMPass());
  passes->addPass(mlir::createReconcileUnrealizedCastsPass());

  // Prepare module for translation to LLVM.
  passes->addPass(mlir::createCanonicalizerPass());
  passes->addPass(mlir::createCSEPass());
}

static void CreateDefaultCpuPipeline(mlir::OpPassManager& pm) {
  CpuPipelineOptions opts;
  PassManager passes(&pm);
  CreateDefaultXlaCpuRuntimeCompilationPipeline(passes, opts);
}

static mlir::PassPipelineRegistration<> kXlaRuntimePipeline(
    "xla-runtime-default-cpu-pipeline",
    "Default XLA-CPU runtime compilation pipeline", CreateDefaultCpuPipeline);

}  // namespace runtime
}  // namespace xla
