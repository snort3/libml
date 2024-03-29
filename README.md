# LibML

## Description

LibML is a library for loading, configuring, and running machine learning models in production. It provides a simple high-level API for C++ applications. LibML uses TensorFlow with XNNPACK acceleration for low latency inference.

## Install

```sh
./configure.sh
cd build
sudo make -j$(nproc) install
```

## Examples

### Binary Classifier

```c++
BinaryClassifier classifier;

if(!classifier.buildFromFile(model_path))
    return 1;

float output = 0.0;

if(!classifier.run(input, input_size, output))
    return 1;

std::cout << "output: " << output << "%\n";
```

## Build Dependencies

* CMake
* C++ Compiler
