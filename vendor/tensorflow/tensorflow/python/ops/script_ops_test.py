# Copyright 2020 The TensorFlow Authors. All Rights Reserved.
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
"""Tests for script operations."""

from tensorflow.python.eager import def_function
from tensorflow.python.framework import constant_op
from tensorflow.python.framework import dtypes
from tensorflow.python.framework import errors
from tensorflow.python.framework import test_util
from tensorflow.python.ops import gen_script_ops
from tensorflow.python.ops import resource_variable_ops
from tensorflow.python.ops import script_ops
from tensorflow.python.ops.script_ops import numpy_function
from tensorflow.python.platform import test


class NumpyFunctionTest(test.TestCase):

  @test_util.run_in_graph_and_eager_modes
  def test_numpy_arguments(self):

    def plus(a, b):
      return a + b

    actual_result = script_ops.numpy_function(plus, [1, 2], dtypes.int32)
    expect_result = constant_op.constant(3, dtypes.int32)
    self.assertAllEqual(actual_result, expect_result)

  def test_stateless(self):
    call_count = 0

    def plus(a, b):
      nonlocal call_count
      call_count += 1
      return a + b

    @def_function.function
    def numpy_func_stateless(a, b):
      return numpy_function(plus, [a, b], dtypes.int32, stateful=False)

    @def_function.function
    def func_stateless(a, b):
      sum1 = numpy_func_stateless(a, b)
      sum2 = numpy_func_stateless(a, b)
      return sum1 + sum2

    self.evaluate(func_stateless(
        constant_op.constant(1),
        constant_op.constant(2),
    ))

    self.assertIn(call_count, (1, 2))  # as stateless, func may be deduplicated

  def test_stateful(self):
    call_count = 0

    def plus(a, b):
      nonlocal call_count
      call_count += 1
      return a + b

    @def_function.function
    def numpy_func_stateful(a, b):
      return numpy_function(plus, [a, b], dtypes.int32, stateful=True)

    @def_function.function
    def func_stateful(a, b):
      sum1 = numpy_func_stateful(a, b)
      sum2 = numpy_func_stateful(a, b)
      return sum1 + sum2

    self.evaluate(func_stateful(
        constant_op.constant(1),
        constant_op.constant(2),
    ))

    self.assertEqual(call_count,
                     2)  # as stateful, func is guaranteed to execute twice


class PyFunctionTest(test.TestCase):

  @test_util.run_in_graph_and_eager_modes
  def test_variable_arguments(self):

    def plus(a, b):
      return a + b

    v1 = resource_variable_ops.ResourceVariable(1)
    self.evaluate(v1.initializer)

    actual_result = script_ops.eager_py_func(plus, [v1, 2], dtypes.int32)
    expect_result = constant_op.constant(3, dtypes.int32)
    self.assertAllEqual(actual_result, expect_result)

  @test_util.run_in_graph_and_eager_modes
  def test_fail_on_non_utf8_token(self):
    value = constant_op.constant(value=[1, 2])
    token = b"\xb0"
    data_type = [dtypes.int32]
    with self.assertRaises((errors.InternalError, UnicodeDecodeError)):
      self.evaluate(
          gen_script_ops.py_func(input=[value], token=token, Tout=data_type))


if __name__ == "__main__":
  test.main()
