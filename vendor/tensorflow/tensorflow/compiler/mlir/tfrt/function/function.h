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

#ifndef TENSORFLOW_COMPILER_MLIR_TFRT_FUNCTION_FUNCTION_H_
#define TENSORFLOW_COMPILER_MLIR_TFRT_FUNCTION_FUNCTION_H_

#include <string>
#include <unordered_set>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/strings/string_view.h"
#include "mlir/IR/BuiltinTypes.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/tfrt/translate/tfrt_compile_options.h"
#include "tensorflow/core/platform/status.h"
#include "tfrt/bef/bef_buffer.h"  // from @tf_runtime
#include "tfrt/core_runtime/tensor_handle.h"  // from @tf_runtime

namespace tfrt {
class CoreRuntime;
}

namespace mlir {
class ModuleOp;
}

namespace tensorflow {

struct TfrtFunctionCompileOptions : public TfrtCompileOptions {
  // Currently only SavedModel API inference uses the tpu_fuse_ops option
  TfrtFunctionCompileOptions() {
    tpu_fuse_ops = false;
    // TF function in eager execution uses CoreRT native ops as fallback states
    // are not initialized in that code path.
    enable_native_ops = true;
    // Currently grappler is not correctly applied in the eager execution of TF
    // functions, as it may sometimes remove arguments and results.
    enable_grappler = false;
  }

  // If true, use ServingCoreSelector to pick TPU core. Otherwise, obtain core
  // location from assigned device name.
  // Currently we don't use core_selector for training use cases.
  bool tpu_use_core_selector = false;

  // If true, use BundledTransferToTpuOp to transfer variables and input tensors
  // to TPU.
  bool tpu_use_bundled_transfer = false;

  // If true, lower an TF op that's placed on TPU device to be executed with
  // tfrt_fallback.execute.
  // Currently for training use cases we need to lower the op to corert.execute
  // to execute with TPU OpHandler, and with TFRT's native implementation.
  // TODO(b/188940204): remove this config after we clear up the TPU variable
  // implementation.
  bool tpu_lower_to_fallback = false;
  // If true, transfer the result of TPUExecuteOp from TPU to host.
  // Currently for training and Python bulk inference use cases, we don't need
  // to proactively transfer the result to host since the consumer op (or
  // function) of the result may still be on TPU.
  // TODO(b/194081364): remove this option once we unify servo TPU serving
  // result transfer behavior.
  bool tpu_transfer_result_to_host = false;
};

// Compile MLIR generated by tf.function in TF dialect into BEF.
Status CompileTFMLIRToBEF(const TfrtFunctionCompileOptions& options,
                          mlir::ModuleOp module, tfrt::BefBuffer* bef_buffer);

}  // namespace tensorflow

#endif  // TENSORFLOW_COMPILER_MLIR_TFRT_FUNCTION_FUNCTION_H_