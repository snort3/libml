# Copyright 2022 The TensorFlow Authors. All Rights Reserved.
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
"""Tests for SavedModel fingerprinting.

These tests verify that FingerprintDef protobuf is written correctly in
`tf.saved_model.save`.
"""
import os
import shutil

from tensorflow.core.config import flags
from tensorflow.core.protobuf import fingerprint_pb2
from tensorflow.python.eager import def_function
from tensorflow.python.eager import test
from tensorflow.python.framework import dtypes
from tensorflow.python.framework import tensor_spec
from tensorflow.python.lib.io import file_io
from tensorflow.python.saved_model import save
from tensorflow.python.saved_model.pywrap_saved_model import constants
from tensorflow.python.trackable import autotrackable


class FingerprintingTest(test.TestCase):

  def _create_saved_model(self):
    root = autotrackable.AutoTrackable()
    save_dir = os.path.join(self.get_temp_dir(), "saved_model")
    save.save(root, save_dir)
    self.addCleanup(shutil.rmtree, save_dir)
    return save_dir

  def _create_model_with_function(self):
    root = autotrackable.AutoTrackable()
    root.f = def_function.function(lambda x: 2. * x)
    return root

  def _create_model_with_input_signature(self):
    root = autotrackable.AutoTrackable()
    root.f = def_function.function(
        lambda x: 2. * x,
        input_signature=[tensor_spec.TensorSpec(None, dtypes.float32)])
    return root

  def _read_fingerprint(self, filename):
    fingerprint_def = fingerprint_pb2.FingerprintDef()
    with file_io.FileIO(filename, "rb") as f:
      fingerprint_def.ParseFromString(f.read())
    return fingerprint_def

  def setUp(self):
    super().setUp()
    flags.config().saved_model_fingerprinting.reset(True)

  def test_basic_module(self):
    save_dir = self._create_saved_model()
    files = file_io.list_directory_v2(save_dir)

    self.assertLen(files, 4)
    self.assertIn(constants.FINGERPRINT_FILENAME, files)

    fingerprint_def = self._read_fingerprint(
        file_io.join(save_dir, constants.FINGERPRINT_FILENAME))
    # We cannot check this value due to non-determinism in serialization.
    self.assertGreater(fingerprint_def.graph_def_checksum, 0)
    self.assertEqual(fingerprint_def.graph_def_program_hash,
                     14830488309055091319)
    self.assertEqual(fingerprint_def.signature_def_hash, 1050878586713189074)
    # TODO(b/242348400): The checkpoint hash is non-deterministic, so we cannot
    # check its value here.
    self.assertGreater(fingerprint_def.checkpoint_hash, 0)

  def test_model_saved_with_different_signature_options(self):
    model = self._create_model_with_function()
    # Save the model with signatures specified in SaveOptions.
    sig_dir = os.path.join(self.get_temp_dir(), "saved_model")
    save.save(
        model,
        sig_dir,
        signatures=model.f.get_concrete_function(
            tensor_spec.TensorSpec(None, dtypes.float32)))
    # Save the model without signatures.
    no_sig_dir = os.path.join(self.get_temp_dir(), "saved_model2")
    save.save(model, no_sig_dir)
    # Save the model with an input signature specified.
    input_sig_dir = os.path.join(self.get_temp_dir(), "saved_model3")
    save.save(self._create_model_with_input_signature(), input_sig_dir)

    fingerprint_sig = self._read_fingerprint(
        file_io.join(sig_dir, constants.FINGERPRINT_FILENAME))
    fingerprint_no_sig = self._read_fingerprint(
        file_io.join(no_sig_dir, constants.FINGERPRINT_FILENAME))
    fingerprint_input_sig = self._read_fingerprint(
        file_io.join(input_sig_dir, constants.FINGERPRINT_FILENAME))

    # Check that the model saved with different options has different
    # SignatureDef hashes.
    self.assertNotEqual(fingerprint_sig.signature_def_hash,
                        fingerprint_no_sig.signature_def_hash)
    # Check that the model saved with the same concrete function has the same
    # regularized hashes.
    self.assertEqual(fingerprint_sig.graph_def_program_hash,
                     fingerprint_input_sig.graph_def_program_hash)
    self.assertEqual(fingerprint_sig.signature_def_hash,
                     fingerprint_input_sig.signature_def_hash)


if __name__ == "__main__":
  test.main()
