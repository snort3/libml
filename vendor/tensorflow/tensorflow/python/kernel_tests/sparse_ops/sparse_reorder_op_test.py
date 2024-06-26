# Copyright 2015 The TensorFlow Authors. All Rights Reserved.
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
"""Tests for SparseReorder."""

from absl.testing import parameterized
import numpy as np

from tensorflow.python.eager import def_function
from tensorflow.python.framework import dtypes
from tensorflow.python.framework import errors
from tensorflow.python.framework import sparse_tensor
from tensorflow.python.framework import tensor_spec
from tensorflow.python.framework import test_util
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import gen_sparse_ops
from tensorflow.python.ops import gradient_checker
from tensorflow.python.ops import sparse_ops
import tensorflow.python.ops.sparse_grad  # pylint: disable=unused-import
from tensorflow.python.platform import test


class SparseReorderTest(test.TestCase, parameterized.TestCase):

  def _SparseTensorPlaceholder(self):
    return sparse_tensor.SparseTensor(
        array_ops.placeholder(dtypes.int64),
        array_ops.placeholder(dtypes.float64),
        array_ops.placeholder(dtypes.int64))

  def _SparseTensorValue_5x6(self, permutation, dtype=dtypes.float64):
    ind = np.array([[0, 0], [1, 0], [1, 3], [1, 4], [3, 2],
                    [3, 3]]).astype(np.int64)
    val = np.array([0, 10, 13, 14, 32, 33]).astype(dtype.as_numpy_dtype)

    ind = ind[permutation]
    val = val[permutation]

    shape = np.array([5, 6]).astype(np.int64)
    return sparse_tensor.SparseTensorValue(ind, val, shape)

  def testStaticShapeInfoPreserved(self):
    sp_input = sparse_tensor.SparseTensor.from_value(
        self._SparseTensorValue_5x6(np.arange(6)))
    self.assertAllEqual((5, 6), sp_input.get_shape())
    sp_output = sparse_ops.sparse_reorder(sp_input)
    self.assertAllEqual((5, 6), sp_output.get_shape())

  @parameterized.named_parameters([
      ("FullyStatic", [5, 6], None, [5, 6]),
      ("Partial1", [None, 6], None, [None, 6]),
      ("Partial2", [5, None], None, [5, None]),
      ("RankFromStaticDenseShape", [None, None], None, [None, None]),
      ("RankFromDenseShapeDim", None, [2], [None, None]),
      ("FullyDynamic", None, [None], None),
  ])
  def testStaticShapeInfoPreservedInFunction(self,
                                             static_dense_shape,
                                             dense_shape_shape,
                                             expected_output_shape):

    @def_function.function
    def func(indices, values, dense_shape):
      if static_dense_shape:
        dense_shape = [
            static_dim or dense_shape[i]
            for i, static_dim in enumerate(static_dense_shape)
        ]
      sp_input = sparse_tensor.SparseTensor(indices, values, dense_shape)
      sp_output = sparse_ops.sparse_reorder(sp_input)
      self.assertEqual(expected_output_shape, sp_output.get_shape())
      return sp_output

    _ = func.get_concrete_function(
        tensor_spec.TensorSpec([6, 2], dtypes.int64),
        tensor_spec.TensorSpec([6], dtypes.float64),
        tensor_spec.TensorSpec(dense_shape_shape, dtypes.int64))

  def testAlreadyInOrder(self):
    with self.session() as sess:
      input_val = self._SparseTensorValue_5x6(np.arange(6))
      sp_output = sparse_ops.sparse_reorder(input_val)

      output_val = self.evaluate(sp_output)
      self.assertAllEqual(output_val.indices, input_val.indices)
      self.assertAllEqual(output_val.values, input_val.values)
      self.assertAllEqual(output_val.dense_shape, input_val.dense_shape)

  @test_util.run_deprecated_v1
  def testFeedAlreadyInOrder(self):
    with self.session() as sess:
      sp_input = self._SparseTensorPlaceholder()
      input_val = self._SparseTensorValue_5x6(np.arange(6))
      sp_output = sparse_ops.sparse_reorder(sp_input)

      output_val = sess.run(sp_output, {sp_input: input_val})
      self.assertAllEqual(output_val.indices, input_val.indices)
      self.assertAllEqual(output_val.values, input_val.values)
      self.assertAllEqual(output_val.dense_shape, input_val.dense_shape)

  @parameterized.parameters(dtypes.bfloat16, dtypes.float64)
  def testOutOfOrder(self, dtype):
    expected_output_val = self._SparseTensorValue_5x6(np.arange(6), dtype)
    with self.session() as sess:
      for _ in range(5):  # To test various random permutations
        input_val = self._SparseTensorValue_5x6(np.random.permutation(6), dtype)
        sp_output = sparse_ops.sparse_reorder(input_val)

        output_val = self.evaluate(sp_output)
        self.assertAllEqual(output_val.indices, expected_output_val.indices)
        self.assertAllEqual(output_val.values, expected_output_val.values)
        self.assertAllEqual(output_val.dense_shape,
                            expected_output_val.dense_shape)

  @test_util.run_deprecated_v1
  def testFeedOutOfOrder(self):
    expected_output_val = self._SparseTensorValue_5x6(np.arange(6))
    with self.session() as sess:
      for _ in range(5):  # To test various random permutations
        sp_input = self._SparseTensorPlaceholder()
        input_val = self._SparseTensorValue_5x6(np.random.permutation(6))
        sp_output = sparse_ops.sparse_reorder(sp_input)

        output_val = sess.run(sp_output, {sp_input: input_val})
        self.assertAllEqual(output_val.indices, expected_output_val.indices)
        self.assertAllEqual(output_val.values, expected_output_val.values)
        self.assertAllEqual(output_val.dense_shape,
                            expected_output_val.dense_shape)

  @test_util.run_deprecated_v1
  def testGradients(self):
    with self.session():
      for _ in range(5):  # To test various random permutations
        input_val = self._SparseTensorValue_5x6(np.random.permutation(6))
        sp_input = sparse_tensor.SparseTensor(input_val.indices,
                                              input_val.values,
                                              input_val.dense_shape)
        sp_output = sparse_ops.sparse_reorder(sp_input)

        err = gradient_checker.compute_gradient_error(
            sp_input.values,
            input_val.values.shape,
            sp_output.values,
            input_val.values.shape,
            x_init_value=input_val.values)
        self.assertLess(err, 1e-11)

  def testShapeOverflow(self):
    # Test case for GitHub issue 45392
    sp_input = sparse_tensor.SparseTensor(
        indices=[[0, 0, 0, 0, 0, 0]],
        values=[0.0],
        dense_shape=[4096, 4096, 4096, 4096, 4096, 4096])
    self.assertAllEqual((4096, 4096, 4096, 4096, 4096, 4096),
                        sp_input.get_shape())
    sp_output = sparse_ops.sparse_reorder(sp_input)
    self.assertAllEqual((4096, 4096, 4096, 4096, 4096, 4096),
                        sp_output.get_shape())

  def testInvalidSparseInput(self):
    with self.assertRaisesRegex(
        (ValueError, errors.InvalidArgumentError),
        "Number of elements .* do not match",
    ):
      self.evaluate(
          gen_sparse_ops.sparse_reorder(
              input_indices=[[0, 0, 0]],
              input_values=[0, 1, 2],
              input_shape=[3, 3],
          )
      )


if __name__ == "__main__":
  test.main()
