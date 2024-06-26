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
#include "tensorflow/lite/experimental/acceleration/mini_benchmark/validator.h"

#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "tensorflow/lite/acceleration/configuration/configuration_generated.h"
#include "tensorflow/lite/core/acceleration/configuration/delegate_registry.h"
#include "tensorflow/lite/core/acceleration/configuration/stable_delegate_registry.h"
#include "tensorflow/lite/core/api/profiler.h"
#include "tensorflow/lite/core/c/c_api.h"
#include "tensorflow/lite/core/c/c_api_types.h"
#include "tensorflow/lite/core/c/common.h"
#include "tensorflow/lite/core/interpreter.h"
#include "tensorflow/lite/core/interpreter_builder.h"
#include "tensorflow/lite/core/kernels/register.h"
#include "tensorflow/lite/core/subgraph.h"
#include "tensorflow/lite/experimental/acceleration/mini_benchmark/call_register.h"
#include "tensorflow/lite/experimental/acceleration/mini_benchmark/constants.h"
#include "tensorflow/lite/experimental/acceleration/mini_benchmark/decode_jpeg_register.h"
#include "tensorflow/lite/experimental/acceleration/mini_benchmark/status_codes.h"
#include "tensorflow/lite/logger.h"
#include "tensorflow/lite/minimal_logging.h"
#include "tensorflow/lite/mutable_op_resolver.h"
#include "tensorflow/lite/tools/benchmark/register_custom_op.h"
#include "tensorflow/lite/tools/model_loader.h"

#ifndef TEMP_FAILURE_RETRY
#ifdef __ANDROID__
#error "TEMP_FAILURE_RETRY not set although on Android"
#else  // ! defined(__ANDROID__)
#define TEMP_FAILURE_RETRY(exp) exp
#endif  // defined(__ANDROID__)
#endif  // defined(TEMP_FAILURE_RETRY)

namespace tflite {
namespace acceleration {
namespace {
std::unique_ptr<tflite::delegates::DelegatePluginInterface> LoadDelegatePlugin(
    const std::string& name, const tflite::TFLiteSettings& tflite_settings) {
  return tflite::delegates::DelegatePluginRegistry::CreateByName(
      name + "Plugin", tflite_settings);
}

void AppendTensorDataToVector(const TfLiteTensor* tensor,
                              std::vector<std::vector<char>>& output_vector) {
  std::vector<char> char_output(TfLiteTensorByteSize(tensor));
  memcpy(char_output.data(), TfLiteTensorData(tensor),
         TfLiteTensorByteSize(tensor));
  output_vector.emplace_back(std::move(char_output));
}

// Returns whether the tensor is embedded with data.
inline bool HasTensorData(tools::ModelLoader* model_loader,
                          const Subgraph& graph, int index) {
  // TODO(b/247752800): Find a better approach to see if data is embedded,
  // regardless of how the model is loaded.
  const TfLiteTensor* tensor = graph.tensor(index);
  return tensor->allocation != nullptr ||
         (model_loader->type() == tools::ModelLoader::Type::kPipeModelLoader &&
          tensor->data.data != nullptr);
}

constexpr int64_t kMicrosInSecond = 1000 * 1000;
constexpr int64_t kNanosInMicro = 1000;

// CLOCK_BOOTTIME is what Android uses for elapsed time. Wallclock on mobile
// devices can jump due to user actions or network time sync.
int64_t ElapsedTimeMicros() {
  struct timespec ts;
#if defined(__ANDROID__)
  int err = clock_gettime(CLOCK_BOOTTIME, &ts);
#elif defined(_WIN32)
  int err = 1;
#else
  int err = clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
  if (err) {
    return -1;
  }
  return ts.tv_sec * kMicrosInSecond + ts.tv_nsec / kNanosInMicro;
}

class ValidatorProfiler : public ::tflite::Profiler {
 public:
  struct EventData {
    std::string tag;
    int64_t start_time_us = -1;
    int64_t end_time_us = -1;
  };
  const std::vector<EventData>& events() { return events_; }
  uint32_t BeginEvent(const char* tag, EventType event_type,
                      int64_t event_metadata1,
                      int64_t event_metadata2) override {
    if (event_type != EventType::DEFAULT) {
      return 0;
    }
    events_.push_back({tag, ElapsedTimeMicros(), -1});
    return events_.size();
  }
  void EndEvent(uint32_t event_handle) override {
    if (event_handle == 0) {
      return;
    }
    events_[event_handle - 1].end_time_us = ElapsedTimeMicros();
  }

 private:
  std::vector<EventData> events_;
};

}  // namespace

MinibenchmarkStatus Validator::CheckGoldenOutput(Results* results_out) {
  if (!interpreter_ || !model_loader_->GetModel()) {
    return kMinibenchmarkPreconditionNotMet;
  }
  if (validation_entrypoint_->inputs().size() <= 1) {
    return kMinibenchmarkValidationSubgraphHasTooFewInputs;
  }
  if (validation_entrypoint_->inputs().size() >
      validation_entrypoint_->outputs().size()) {
    return kMinibenchmarkValidationSubgraphHasTooFewOutputs;
  }

  // Check if we have golden output embedded or need to run CPU for it.  If
  // embedded, we can skip running it on CPU.
  if (HasTensorData(model_loader_.get(), *validation_entrypoint_,
                    validation_entrypoint_->inputs()[0])) {
    return kMinibenchmarkSuccess;
  }
  TFLITE_LOG_PROD(TFLITE_LOG_INFO,
                  "Running on CPU to get golden output for comparison.");

  // Create the interpreter to run on CPU.
  tflite::InterpreterBuilder(*model_loader_->GetModel(),
                             *resolver_)(&golden_interpreter_);
  if (!golden_interpreter_) {
    return kMinibenchmarkInterpreterBuilderFailed;
  }
  Subgraph* golden_validation_entrypoint =
      golden_interpreter_->subgraph(validation_entrypoint_index_);

  // Run on CPU.
  if (golden_validation_entrypoint->AllocateTensors() != kTfLiteOk) {
    return kMinibenchmarkAllocateTensorsFailed;
  }
  // Set initial golden outputs to 0 to avoid accessing uninitialized memory.
  // Last input is jpeg, skip.
  for (int i = 0; i < golden_validation_entrypoint->inputs().size() - 1; i++) {
    TfLiteTensor* input_tensor = golden_validation_entrypoint->tensor(
        golden_validation_entrypoint->inputs()[i]);
    memset(input_tensor->data.data, 0, input_tensor->bytes);
  }

  if (golden_validation_entrypoint->Invoke() != kTfLiteOk) {
    return kMinibenchmarkInvokeFailed;
  }
  // Copy CPU outputs as golden. Last input is jpeg image data, skip.
  for (int i = 0; i < validation_entrypoint_->inputs().size() - 1; i++) {
    TfLiteTensor* input_tensor =
        validation_entrypoint_->tensor(validation_entrypoint_->inputs()[i]);
    TfLiteTensor* golden_output_tensor = golden_validation_entrypoint->tensor(
        golden_validation_entrypoint->outputs()[i]);
    if (input_tensor->bytes != golden_output_tensor->bytes) {
      return kMinibenchmarkValidationSubgraphInputsDontMatchOutputs;
    }

    memcpy(input_tensor->data.data, golden_output_tensor->data.data,
           golden_output_tensor->bytes);
  }

  return kMinibenchmarkSuccess;
}

MinibenchmarkStatus Validator::LoadDelegate() {
  if (!compute_settings_) {
    return kMinibenchmarkPreconditionNotMet;
  }
  if (opaque_delegate_) {
    // An opaque delegate is created already.
    return kMinibenchmarkSuccess;
  }

  // Create delegate plugin and delegate.
  Delegate which_delegate = Delegate_NONE;
  bool is_stable_delegate_path_provided = false;
  auto tflite_settings = compute_settings_->tflite_settings();
  if (tflite_settings) {
    which_delegate = compute_settings_->tflite_settings()->delegate();
    if (tflite_settings->stable_delegate_loader_settings()) {
      is_stable_delegate_path_provided =
          tflite_settings->stable_delegate_loader_settings()->delegate_path() &&
          !tflite_settings->stable_delegate_loader_settings()
               ->delegate_path()
               ->str()
               .empty();
    }
  }
  std::string delegate_name;
  if (is_stable_delegate_path_provided && which_delegate == Delegate_GPU) {
    // Load GPU plugin from GpuModulePlugin when delegate_path is provided.
    // This is a workaround before StableDelegate is supported.
    delegate_name = "GpuModule";
  } else if (is_stable_delegate_path_provided) {
    // When a stable delegate shared library path is provided, the stable
    // delegate plugin loads symbols from the shared library to initialize the
    // delegates.
    delegate_name = "StableDelegate";
  } else {
    switch (which_delegate) {
      case Delegate_NONE:
        // Skip creating delegate if running on CPU.
        return kMinibenchmarkSuccess;
      case Delegate_NNAPI:
        delegate_name = "Nnapi";
        break;
      case Delegate_GPU:
        delegate_name = "Gpu";
        break;
      case Delegate_XNNPACK:
        delegate_name = "XNNPack";
        break;
      case Delegate_EDGETPU:
        delegate_name = "EdgeTpu";
        break;
      default:
        return kMinibenchmarkDelegateNotSupported;
    }
  }

  TFLITE_LOG_PROD(TFLITE_LOG_INFO, "Running mini-benchmark on %s",
                  delegate_name.c_str());
  if (!(delegate_plugin_ = LoadDelegatePlugin(
            delegate_name, *compute_settings_->tflite_settings()))) {
    return kMinibenchmarkDelegatePluginNotFound;
  }
  if (!(delegate_ = delegate_plugin_->Create())) {
    return kMinibenchmarkDelegateCreateFailed;
  }
  return kMinibenchmarkSuccess;
}

MinibenchmarkStatus Validator::LoadOpaqueDelegate() {
  if (!compute_settings_) {
    return kMinibenchmarkPreconditionNotMet;
  }

  // Create delegate plugin and delegate.
  bool is_stable_delegate_name_provided = false;
  auto tflite_settings = compute_settings_->tflite_settings();
  if (!tflite_settings) {
    return kMinibenchmarkSuccess;
  }
  auto stable_delegate_settings =
      tflite_settings->stable_delegate_loader_settings();
  is_stable_delegate_name_provided =
      stable_delegate_settings && stable_delegate_settings->delegate_name() &&
      !stable_delegate_settings->delegate_name()->str().empty();
  if (!is_stable_delegate_name_provided) {
    // It is optional to have an opaque delegate.
    return kMinibenchmarkSuccess;
  }
  // When a stable delegate name is provided, we load symbols from the stable
  // delegate registry to initialize the delegates.
  std::string delegate_name = stable_delegate_settings->delegate_name()->str();
  TFLITE_LOG_PROD(TFLITE_LOG_INFO, "Running mini-benchmark on %s",
                  delegate_name.c_str());

  const TfLiteStableDelegate* stable_delegate =
      delegates::StableDelegateRegistry::RetrieveStableDelegate(delegate_name);
  if (!stable_delegate) {
    TFLITE_LOG_PROD(TFLITE_LOG_ERROR,
                    "Failed to load stable delegate plugin %s",
                    delegate_name.c_str());
    return kMinibenchmarkDelegatePluginNotFound;
  }
  const TfLiteOpaqueDelegatePlugin* delegate_plugin =
      stable_delegate->delegate_plugin;
  opaque_delegate_ = TfLiteOpaqueDelegatePtr(
      delegate_plugin->create(tflite_settings), delegate_plugin->destroy);
  return kMinibenchmarkSuccess;
}

MinibenchmarkStatus Validator::CreateInterpreter(int* delegate_error_out,
                                                 int* delegated_kernels_out) {
  if (!delegate_error_out || !delegated_kernels_out ||
      !model_loader_->GetModel()) {
    return kMinibenchmarkPreconditionNotMet;
  }

  if (interpreter_) {
    // Already done.
    return kMinibenchmarkSuccess;
  }

  *delegate_error_out = 0;
  // Create interpreter with the delegate.
  if (compute_settings_->tflite_settings() &&
      compute_settings_->tflite_settings()->disable_default_delegates()) {
    resolver_ = std::make_unique<
        ::tflite::ops::builtin::BuiltinOpResolverWithoutDefaultDelegates>();
  } else {
    resolver_ = std::make_unique<::tflite::ops::builtin::BuiltinOpResolver>();
  }
  resolver_->AddCustom("validation/call",
                       ::tflite::acceleration::ops::Register_CALL(), 1);
  resolver_->AddCustom(
      "validation/decode_jpeg",
      ::tflite::acceleration::decode_jpeg_kernel::Register_DECODE_JPEG(), 1);

  RegisterSelectedOps(resolver_.get());

  tflite::InterpreterBuilder builder(*model_loader_->GetModel(), *resolver_);
  // Add delegate if not running on CPU.
  if (delegate_ != nullptr) {
    builder.AddDelegate(delegate_.get());
  }
  if (opaque_delegate_ != nullptr) {
    builder.AddDelegate(opaque_delegate_.get());
  }
  TfLiteStatus status = builder(&interpreter_);
  if (!interpreter_) {
    // Return delegate error number if not null.
    *delegate_error_out =
        delegate_plugin_ ? delegate_plugin_->GetDelegateErrno(delegate_.get())
                         : 0;

    TFLITE_LOG_PROD(TFLITE_LOG_ERROR,
                    "Creating Interpreter failed with error code %d.", status);
    return kMinibenchmarkInterpreterBuilderFailed;
  }
  main_model_ = interpreter_->subgraph(0);
  validation_entrypoint_index_ = -1;
  for (int i = 0; i < interpreter_->subgraphs_size(); i++) {
    Subgraph* subgraph = interpreter_->subgraph(i);
    if (subgraph->GetName() == kValidationGraphName) {
      validation_entrypoint_index_ = i;
      validation_entrypoint_ = subgraph;
    } else if (subgraph->GetName() == "VALIDATION:metrics") {
      has_accuracy_validation_ = true;
    }
  }
  if (!validation_entrypoint_) {
    return kMinibenchmarkValidationSubgraphNotFound;
  }
  // Check if validation input exists and prefilled.
  if (validation_entrypoint_->inputs().empty()) {
    return kMinibenchmarkValidationSubgraphHasTooFewInputs;
  }
  if (!HasTensorData(model_loader_.get(), *validation_entrypoint_,
                     validation_entrypoint_->inputs().back())) {
    return kMinibenchmarkValidationInputMissing;
  }
  if (validation_entrypoint_->AllocateTensors() != kTfLiteOk) {
    return kMinibenchmarkAllocateTensorsFailed;
  }

  // Check if the model is actually going to execute on the delegate.
  // For now just give a warning, with the exception of NNAPI SL mini
  // benchmark. Can consider changing to error in other contexts. The logic is
  // copy/pasted from benchmark_tflite_model.cc
  // TODO(b/232085640): Replace this logic with Subgraph::IsFullyDelegated()
  // after making that function public.
  absl::flat_hash_set<int> checked_node_ids;
  int num_delegated_kernels = 0;
  for (int i = 0; i < interpreter_->execution_plan().size(); ++i) {
    int node_id = interpreter_->execution_plan()[i];
    if (checked_node_ids.find(node_id) != checked_node_ids.end()) {
      continue;
    }
    const TfLiteNode& node =
        interpreter_->node_and_registration(node_id)->first;
    if (node.delegate != nullptr) {
      num_delegated_kernels++;
      checked_node_ids.insert(node_id);
    }
  }
  *delegated_kernels_out = num_delegated_kernels;
  bool fully_delegated = (num_delegated_kernels == 1 &&
                          interpreter_->execution_plan().size() == 1);
  if (!fully_delegated) {
    TFLITE_LOG_PROD(TFLITE_LOG_WARNING,
                    "The model will be %s executed by the delegate.",
                    num_delegated_kernels > 0 ? "partially" : "not");
  }

  return kMinibenchmarkSuccess;
}

Validator::Status Validator::RunValidation(Results* results_out) {
  BenchmarkStage stage = BenchmarkStage_INITIALIZATION;
  if (!results_out) {
    return Validator::Status{kMinibenchmarkPreconditionNotMet, stage};
  }
  if (!model_loader_) {
    return Validator::Status{kMinibenchmarkModelReadFailed, stage};
  }
  if (!model_loader_->Init()) {
    return Validator::Status{kMinibenchmarkModelInitFailed, stage};
  }

#define MB_RETURN_IF_ERROR(s, bs)                                      \
  {                                                                    \
    MinibenchmarkStatus c = (s);                                       \
    if (c != kMinibenchmarkSuccess) return Validator::Status{c, (bs)}; \
  }

  // The lifetime of the delegate must be at least as long as the lifetime of
  // any Interpreter.
  int64_t delegate_load_start_time_us = ElapsedTimeMicros();
  MB_RETURN_IF_ERROR(LoadOpaqueDelegate(), stage);
  MB_RETURN_IF_ERROR(LoadDelegate(), stage);
  MB_RETURN_IF_ERROR(CreateInterpreter(&results_out->delegate_error,
                                       &results_out->delegated_kernels),
                     stage);
  int64_t delegate_load_end_time_us = ElapsedTimeMicros();

  ValidatorProfiler profiler;
  stage = BenchmarkStage_INFERENCE;

  if (has_accuracy_validation_) {
    MB_RETURN_IF_ERROR(CheckGoldenOutput(results_out), stage);
  }

  main_model_->SetProfiler(&profiler, 0);
  TfLiteStatus status = validation_entrypoint_->Invoke();
  main_model_->SetProfiler(nullptr, 0);
  if (status != kTfLiteOk) {
    MB_RETURN_IF_ERROR(kMinibenchmarkInvokeFailed, stage);
  }

  int model_output_size = main_model_->outputs().size();
  if (has_accuracy_validation_) {
    // Accuracy metrics.
    const std::string kMetricPrefix = "metrics/";
    const std::string kOk("ok");
    for (int i = model_output_size;
         i < validation_entrypoint_->outputs().size(); i++) {
      TfLiteTensor* tensor =
          validation_entrypoint_->tensor(validation_entrypoint_->outputs()[i]);
      std::string name = tensor->name;
      if (name.find(kMetricPrefix) != 0) {  // NOLINT
        continue;
      }
      name = name.substr(kMetricPrefix.size());
      if (kOk == name) {
        results_out->ok = *(tensor->data.b);
      } else {
        std::vector<float> values;
        int count = 1;
        for (int j = 0; j < tensor->dims->size; j++) {
          count *= tensor->dims->data[j];
        }
        values.reserve(count);
        for (int j = 0; j < count; j++) {
          values.push_back(tensor->data.f[j]);
          TFLITE_LOG_PROD(TFLITE_LOG_INFO, "  %s %.4f", name.c_str(),
                          tensor->data.f[j]);
        }
        results_out->metrics[name] = values;
      }
    }

    TFLITE_LOG_PROD(TFLITE_LOG_INFO, "  accuracy: %s",
                    results_out->ok ? "ok" : "not ok");
  } else {
    // Model output.
    results_out->actual_inference_output.clear();
    results_out->actual_inference_output.reserve(model_output_size);
    for (int i = 0; i < model_output_size; i++) {
      AppendTensorDataToVector(
          validation_entrypoint_->tensor(validation_entrypoint_->outputs()[i]),
          results_out->actual_inference_output);
    }
  }
  // Performance metrics.
  results_out->delegate_prep_time_us =
      (delegate_load_end_time_us == -1 || delegate_load_start_time_us == -1)
          ? -1
          : delegate_load_end_time_us - delegate_load_start_time_us;
  TFLITE_LOG_PROD(TFLITE_LOG_INFO, "  Delegate preparation took %d us",
                  static_cast<int>(results_out->delegate_prep_time_us));
  for (const auto& e : profiler.events()) {
    if (e.tag == "Invoke" && e.start_time_us != -1 && e.end_time_us != -1) {
      results_out->execution_time_us.push_back(e.end_time_us - e.start_time_us);
      TFLITE_LOG_PROD(TFLITE_LOG_INFO, "  Inference took %d us",
                      static_cast<int>(e.end_time_us - e.start_time_us));
    }
  }
#undef MB_RETURN_IF_ERROR
  return Validator::Status{kMinibenchmarkSuccess};
}

int64_t Validator::BootTimeMicros() { return ElapsedTimeMicros(); }
int64_t Validator::WallTimeMicros() {
  struct timespec ts;
#ifndef _WIN32
  int err = clock_gettime(CLOCK_REALTIME, &ts);
#else   // _WIN32
  int err = 1;
#endif  // !_WIN32
  if (err) {
    return -1;
  }
  return ts.tv_sec * kMicrosInSecond + ts.tv_nsec / kNanosInMicro;
}

}  // namespace acceleration
}  // namespace tflite
