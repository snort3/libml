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
#include "tensorflow/compiler/mlir/tfrt/ir/mlrt/tf_mlrt_tpu_ops.h"

#include "mlir/IR/Builders.h"  // from @llvm-project
#include "mlir/IR/BuiltinTypes.h"  // from @llvm-project
#include "mlir/IR/DialectImplementation.h"  // from @llvm-project
#include "mlir/IR/OpDefinition.h"  // from @llvm-project
#include "mlir/IR/TypeUtilities.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_ops.h"
#include "tensorflow/compiler/mlir/tfrt/ir/mlrt/mlrt_dialect.h"
#include "tensorflow/compiler/mlir/tfrt/ir/mlrt/tf_mlrt_ops.h"

namespace tensorflow {
namespace tf_mlrt_tpu {

TensorflowMlrtTpuDialect::TensorflowMlrtTpuDialect(mlir::MLIRContext *context)
    : mlir::Dialect(/*name=*/"tf_mlrt_tpu", context,
                    mlir::TypeID::get<TensorflowMlrtTpuDialect>()) {
  addOperations<
#define GET_OP_LIST
#include "tensorflow/compiler/mlir/tfrt/ir/mlrt/tf_mlrt_tpu_ops.cpp.inc"
      >();
}

}  // namespace tf_mlrt_tpu
}  // namespace tensorflow

//===----------------------------------------------------------------------===//
// TableGen'd op method definitions
//===----------------------------------------------------------------------===//

#define GET_OP_CLASSES
#include "tensorflow/compiler/mlir/tfrt/ir/mlrt/tf_mlrt_tpu_ops.cpp.inc"
