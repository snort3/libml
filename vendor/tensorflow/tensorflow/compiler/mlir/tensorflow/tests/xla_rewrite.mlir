// RUN: tf-opt %s -tf-xla-rewrite | FileCheck %s

// -----

// CHECK-LABEL: func.func @convert_partitioned_call
func.func @convert_partitioned_call(%arg0: tensor<i32>) -> tensor<i32> {
  // CHECK: "tf.XlaLaunch"(%arg0) {_xla_compile_device_type = "CPU", device = "/device:CPU:0", function = @pcall_func, operand_segment_sizes = array<i32: 0, 1, 0>} : (tensor<i32>) -> tensor<i32>
  %0 = "tf.PartitionedCall"(%arg0) {_xla_compile_device_type = "CPU", config = "", config_proto = "", device = "/device:CPU:0", executor_type = "", f = @pcall_func} : (tensor<i32>) -> (tensor<i32>)
  func.return %0 : tensor<i32>
}

func.func @pcall_func(%arg0: tensor<i32>) -> tensor<i32> {
  func.return %arg0 : tensor<i32>
}

// -----

// CHECK-LABEL: func.func @convert_partitioned_call_with_resources
func.func @convert_partitioned_call_with_resources(%arg0: tensor<!tf_type.resource>, %arg1: tensor<i32>) -> tensor<i32> {
  // CHECK: "tf.XlaLaunch"(%arg1, %arg0) {_xla_compile_device_type = "CPU", device = "/device:CPU:0", function = @pcall_func_with_resources, operand_segment_sizes = array<i32: 0, 1, 1>} : (tensor<i32>, tensor<!tf_type.resource>) -> tensor<i32>
  %0 = "tf.PartitionedCall"(%arg0, %arg1) {_xla_compile_device_type = "CPU", config = "", config_proto = "", device = "/device:CPU:0", executor_type = "", f = @pcall_func_with_resources} : (tensor<!tf_type.resource>, tensor<i32>) -> (tensor<i32>)
  func.return %0 : tensor<i32>
}

func.func @pcall_func_with_resources(%arg0 : tensor<!tf_type.resource>, %arg1: tensor<i32>) -> tensor<i32> {
  func.return %arg1 : tensor<i32>
}

// -----

// CHECK-LABEL: func.func @convert_stateful_partitioned_call
func.func @convert_stateful_partitioned_call(%arg0: tensor<i32>) -> tensor<i32> {
  // CHECK: "tf.XlaLaunch"(%arg0) {_xla_compile_device_type = "CPU", device = "/device:CPU:0", function = @stateful_pcall_func, operand_segment_sizes = array<i32: 0, 1, 0>} : (tensor<i32>) -> tensor<i32>
  %0 = "tf.StatefulPartitionedCall"(%arg0) {_xla_compile_device_type = "CPU", config = "", config_proto = "", device = "/device:CPU:0", executor_type = "", f = @stateful_pcall_func} : (tensor<i32>) -> (tensor<i32>)
  func.return %0 : tensor<i32>
}

func.func @stateful_pcall_func(%arg0: tensor<i32>) -> tensor<i32> {
  func.return %arg0 : tensor<i32>
}

// -----

// CHECK-LABEL: func.func @convert_stateful_partitioned_call_with_resources
func.func @convert_stateful_partitioned_call_with_resources(%arg0: tensor<!tf_type.resource>, %arg1: tensor<i32>) -> tensor<i32> {
  // CHECK: "tf.XlaLaunch"(%arg1, %arg0) {_xla_compile_device_type = "CPU", device = "/device:CPU:0", function = @stateful_pcall_func_with_resources, operand_segment_sizes = array<i32: 0, 1, 1>} : (tensor<i32>, tensor<!tf_type.resource>) -> tensor<i32>
  %0 = "tf.StatefulPartitionedCall"(%arg0, %arg1) {_xla_compile_device_type = "CPU", config = "", config_proto = "", device = "/device:CPU:0", executor_type = "", f = @stateful_pcall_func_with_resources} : (tensor<!tf_type.resource>, tensor<i32>) -> (tensor<i32>)
  func.return %0 : tensor<i32>
}

func.func @stateful_pcall_func_with_resources(%arg0 : tensor<!tf_type.resource>, %arg1: tensor<i32>) -> tensor<i32> {
  func.return %arg1 : tensor<i32>
}
