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

#include "tensorflow/core/tfrt/fallback/cost_recorder.h"

#include <string>

#include "absl/container/flat_hash_map.h"
#include "tensorflow/core/platform/env.h"
#include "tensorflow/core/platform/mutex.h"
#include "tensorflow/core/platform/status.h"
#include "tensorflow/core/tfrt/fallback/op_cost_map.pb.h"
#include "tensorflow/core/util/env_var.h"

namespace tensorflow {
namespace tfrt_stub {

void CostRecorder::RecordCost(int64_t op_key, const uint64_t execution_time) {
  mutex_lock l(op_cost_map_mutex_);
  op_cost_map_[op_key].first += execution_time;
  op_cost_map_[op_key].second += 1;
}

size_t CostRecorder::size() {
  tf_shared_lock l(op_cost_map_mutex_);
  return op_cost_map_.size();
}

Status CostRecorder::WriteToFile() {
  OpCostMapProto op_cost_map_proto;
  {
    tf_shared_lock l(op_cost_map_mutex_);
    for (const auto& [op_key, op_cost] : op_cost_map_) {
      uint64_t avg_op_cost = op_cost.first / op_cost.second;
      (*op_cost_map_proto.mutable_op_cost_map())[op_key] = avg_op_cost;
    }
  }

  std::string measured_cost_path;
  TF_RETURN_IF_ERROR(ReadStringFromEnvVar("TF_TFRT_MEASURED_COST_PATH", "",
                                          &measured_cost_path));
  return tensorflow::WriteTextProto(tensorflow::Env::Default(),
                                    measured_cost_path, op_cost_map_proto);
}

}  // namespace tfrt_stub
}  // namespace tensorflow
