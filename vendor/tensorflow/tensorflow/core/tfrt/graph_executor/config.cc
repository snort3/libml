/* Copyright 2023 The TensorFlow Authors. All Rights Reserved.

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
#include "tensorflow/core/tfrt/graph_executor/config.h"

#include <string>
#include <utility>

namespace tensorflow {
namespace tfrt_stub {

absl::StatusOr<RuntimeConfig> RuntimeConfig::CreateFromProto(
    RuntimeConfigProto proto) {
  RuntimeConfig model_config;
  model_config.proto_ = std::move(proto);
  size_t i = 0;
  for (const auto& any : model_config.proto_.config()) {
    std::string full_name;
    if (!::google::protobuf::Any::ParseAnyTypeUrl(any.type_url(), &full_name)) {
      return absl::InvalidArgumentError(
          absl::StrCat("Invalid Any proto type url: ", any.type_url()));
    }
    model_config.map_[full_name] = i++;
  }
  return model_config;
}

}  // namespace tfrt_stub
}  // namespace tensorflow
