#!/usr/bin/env python3
#--------------------------------------------------------------------------
# Copyright (C) 2024-2025 Cisco and/or its affiliates. All rights reserved.
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License Version 2 as published
# by the Free Software Foundation.  You may not use, modify or distribute
# this program under any other version of the GNU General Public License.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License along
# with this program; if not, write to the Free Software Foundation, Inc.,
# 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
#--------------------------------------------------------------------------
# train.py author Brandon Stultz <brastult@cisco.com>

# python3 -m venv venv
# source venv/bin/activate
# pip install tensorflow
# ./train.py
# deactivate

import os
os.environ['TF_CPP_MIN_LOG_LEVEL'] = '3'

import numpy as np
import tensorflow as tf
from tensorflow.keras import layers
from urllib.parse import unquote_to_bytes

# example data
data = [
    { 'str':'foo=1', 'attack':0 },
    { 'str':'foo=1%27%20or%201=1%2D%2D', 'attack':1 }
]

#
# Prepare Data
#

maxlen = 1024

X = []
Y = []

def decode_query(str):
    return unquote_to_bytes(str.replace('+',' '))

for item in data:
    arr = decode_query(item['str'])[:maxlen]
    arrlen = len(arr)
    seq = [0] * maxlen
    for i in range(arrlen):
        seq[maxlen - arrlen + i] = arr[i]
    X.append(seq)
    Y.append(item['attack'])

#
# Build Model (Simple LSTM)
#

model = tf.keras.Sequential([
    layers.Input(shape=(maxlen,), batch_size=1),
    layers.Embedding(256, 32),
    layers.LSTM(16),
    layers.Dense(1, activation='sigmoid')])

model.compile(loss='binary_crossentropy', optimizer='adam', metrics=['accuracy'])

model.summary()

#
# Train Model
#

model.fit(np.asarray(X).astype(np.float32),
          np.asarray(Y).astype(np.float32),
          epochs=100, batch_size=1)

#
# Save Model
#

export_archive = tf.keras.export.ExportArchive()
export_archive.track(model)
export_archive.add_endpoint(
    name='serve',
    fn=model.call,
    input_signature=[tf.TensorSpec(shape=(1, maxlen), dtype=tf.float32)],
)
export_archive.write_out('model')

converter = tf.lite.TFLiteConverter.from_saved_model('model')

classifier_model = converter.convert()

with open('classifier.model', 'wb') as f:
    f.write(classifier_model)
