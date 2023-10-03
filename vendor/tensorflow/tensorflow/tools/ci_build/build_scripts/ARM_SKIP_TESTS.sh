# Copyright 2019 The TensorFlow Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ==============================================================================
#!/bin/bash
set -x

ARM_SKIP_TESTS="-//tensorflow/lite/... \
-//tensorflow/python:nn_grad_test \
-//tensorflow/python/client:session_list_devices_test \
-//tensorflow/python/data/kernel_tests:iterator_test_cpu \
-//tensorflow/python/data/kernel_tests:iterator_test_gpu \
-//tensorflow/python/eager:forwardprop_test \
-//tensorflow/python/kernel_tests/nn_ops:conv_ops_test \
-//tensorflow/python/kernel_tests/nn_ops:conv2d_backprop_filter_grad_test \
-//tensorflow/python/kernel_tests/nn_ops:atrous_conv2d_test \
-//tensorflow/python/training:server_lib_test"
