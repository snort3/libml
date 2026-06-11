# LibML

## Description

LibML is a library for loading, configuring, and running machine learning models in production. It provides a simple high-level API for C++ applications. LibML uses TensorFlow Lite with XNNPACK acceleration for low latency inference on the CPU.

LibML is the inference engine behind [SnortML](https://blog.snort.org/2024/03/talos-launching-new-machine-learning.html), the machine learning based exploit detector in Snort 3, but it is a standalone library and can be used by any C++ application. All of its dependencies (TensorFlow Lite, XNNPACK, abseil, flatbuffers, and friends) are vendored in this repository, so no external ML framework installation is required.

## Install

```sh
./configure.sh
cd build
sudo make -j$(nproc) install
```

Useful `configure.sh` options:

* `--prefix=<path>` - install to a custom location
* `--builddir=<path>` - build in a custom directory (default: `build`)
* `--debug` - build with symbols
* `--test` - also build the unit tests (requires CppUTest)

Both shared and static libraries are built, along with pkg-config files (`libml.pc` and `libml_static.pc`) for consumers.

## Build Dependencies

* CMake
* C++ Compiler (C++17)
* CppUTest (only when configured with `--test`)

## API Overview

The public API lives in a single header, `libml.h`, under the `libml` namespace.

### `const char* libml::version()`

Returns the LibML version string.

### `libml::BinaryClassifier`

A binary classifier wraps a single TensorFlow Lite model and produces one probability per input buffer.

```c++
libml::BinaryClassifier classifier;

if(!classifier.buildFromFile(model_path))
    return 1;

float output = 0.0;

if(!classifier.run(input, input_size, output))
    return 1;

std::cout << "output: " << output*100.0 << "%\n";
```

* `bool build(std::string model_data)` - builds the classifier from an in-memory TFLite flatbuffer. The classifier takes ownership of the buffer.
* `bool buildFromFile(const std::string& path)` - reads the model file and calls `build()`.
* `bool run(const char* buffer, size_t size, float& output)` - runs inference on a byte buffer and stores the result in `output`. Returns `false` if the classifier was not built or the buffer is empty.

Input handling during `run()`:

* Each input byte is converted to a `float` and written into the model input tensor.
* If the buffer is shorter than the model input size, the input is left-padded with zeros, so the data always ends at the last element of the tensor. Train your models with the same alignment (see `examples/classifier/train.py`).
* If the buffer is longer than the model input size, it is truncated.
* If the model metadata sets the `lowercase` flag, ASCII input bytes are lowercased before inference.

### `libml::BinaryClassifierSet`

A classifier set holds several models of different input sizes and picks the best one for each input.

```c++
std::vector<std::string> models; // one TFLite flatbuffer per entry

libml::BinaryClassifierSet classifiers;

if(!classifiers.build(std::move(models)))
    return 1;

float output = 0.0;

if(!classifiers.run(input, input_size, output))
    return 1;
```

* `build()` constructs one classifier per model. If two models have the same input size, the later one replaces the earlier one. The set is kept sorted by input size.
* `run()` selects the smallest model whose input size fits the whole buffer and runs it. If the buffer is larger than every model, the largest model is used (and the input is truncated).

This is useful in production where you want a cheap, small model for short inputs and a larger model only for inputs that need it.

## Model Requirements

LibML accepts models in the TensorFlow Lite flatbuffer format. A model must satisfy these constraints, which are verified at build time:

* exactly one input tensor and one output tensor
* both tensors have type `float32`
* the output tensor has exactly one element (the probability)
* the input tensor has a fixed, nonzero size

Any network architecture that satisfies this contract works, whether it is an LSTM, a CNN, a transformer, or a plain dense network.

## Model Metadata

A model may optionally carry a flatbuffer metadata entry named `LIBML_METADATA` (see `src/metadata_schema.fbs`):

```text
table Metadata {
    lowercase:bool = false;
}
```

When `lowercase` is true, LibML lowercases ASCII input bytes before inference, which lets you train on lowercased data and stay case-insensitive at runtime.

## Training a Model

`examples/classifier/train.py` is a complete, minimal example of producing a LibML-compatible model with the TensorFlow Keras API. It:

1. URL-decodes example HTTP query parameters and labels them (attack or not)
2. Encodes each example as a fixed-length byte sequence, left-padded with zeros to `maxlen`
3. Builds a small network (embedding layer, LSTM, dense sigmoid output)
4. Trains it with binary cross-entropy loss
5. Converts the trained model to a TFLite flatbuffer with `tf.lite.TFLiteConverter` and writes `classifier.model`

```sh
cd examples/classifier
python3 -m venv venv
source venv/bin/activate
pip install tensorflow
./train.py
deactivate
```

The same recipe scales to real datasets: keep the input encoding (bytes to floats, zero left-padding) and the single sigmoid output, and swap in your own data and architecture.

## Running the Example

The `classifier` example binary is built by default and can be tried immediately with one of the bundled test models:

```sh
$ ./build/examples/classifier/classifier src/test/models/256.model "foo=1%27%20or%201=1--"
Using LibML version 2.0.0
Results
-------
 input: 'foo=1%27%20or%201=1--'
output: 97.6309%
```

It prints the model output as a percentage for the given input string. A model produced by `train.py` (`classifier.model`) works the same way.

## Running the Tests

```sh
./configure.sh --test
cd build
make -j$(nproc)
ctest
```

The tests in `src/test` exercise single classifiers, lowercase metadata handling, and classifier sets against small pre-built models.

## Using LibML with Snort 3 (SnortML)

Snort 3 (3.1.82.0 and later) integrates LibML through two modules. Build Snort with LibML installed and configure:

```lua
-- load the trained model (global)
snort_ml_engine = { http_param_model = 'snort_ml.model' }

-- enable the inspector (policy)
snort_ml =
{
    uri_depth = -1,          -- bytes of HTTP URI query to inspect (-1 = unlimited)
    client_body_depth = 100, -- bytes of HTTP POST body to inspect (0 = none)
}
```

The `snort_ml_engine` module loads the model and instantiates classifiers. The `snort_ml` inspector subscribes to HTTP request data, feeds the URI query and optionally the POST body to the classifier, and raises the builtin alert with GID 411, SID 1 when the classifier output indicates an exploit. A `http_param_model` trained by Talos ships in the Lightweight Security Package (LSP) for registered users.

## Further Reading

* [Talos launching new machine learning-based exploit detection engine](https://blog.snort.org/2024/03/talos-launching-new-machine-learning.html) - the SnortML announcement, with a walkthrough of the model contract and a training example
* [SnortML training video](https://blog.snort.org/2024/08/watch-snortml-training-video.html)
* [SnortML reference](https://docs.snort.org/misc/snort_ml) - inspector and engine configuration in the Snort 3 manual
* [SnortML: Machine Learning-based Exploit Detection (Cisco Secure Firewall)](https://secure.cisco.com/secure-firewall/docs/snortml-machine-learning-based-exploit-detection)
* [TensorFlow Lite converter documentation](https://www.tensorflow.org/lite/models/convert) - for producing models from Keras or SavedModel formats
