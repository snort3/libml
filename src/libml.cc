//--------------------------------------------------------------------------
// Copyright (C) 2023-2023 Cisco and/or its affiliates. All rights reserved.
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License Version 2 as published
// by the Free Software Foundation.  You may not use, modify or distribute
// this program under any other version of the GNU General Public License.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//--------------------------------------------------------------------------
// libml.cc author Brandon Stultz <brastult@cisco.com>

#include <cstdint>
#include <utility>

#include "tensorflow/lite/interpreter.h"
#include "tensorflow/lite/kernels/kernel_util.h"
#include "tensorflow/lite/kernels/register.h"
#include "tensorflow/lite/logger.h"
#include "tensorflow/lite/model.h"

#include "libml.h"
#include "util.h"
#include "version.h"

using namespace tflite;

const char* libml_version()
{ return VERSION; }

BinaryClassifier::BinaryClassifier() = default;
BinaryClassifier::~BinaryClassifier() = default;

bool BinaryClassifier::build(std::string in)
{
    interpreter.reset();

    LoggerOptions::SetMinimumLogSeverity(TFLITE_LOG_ERROR);

    src = std::move(in);

    model = FlatBufferModel::VerifyAndBuildFromBuffer(src.data(),
        src.size());

    if(model == nullptr)
        return false;

    ops::builtin::BuiltinOpResolver resolver;

    std::unique_ptr<Interpreter> check;

    InterpreterBuilder builder(*model, resolver);

    if(builder(&check) != kTfLiteOk)
        return false;

    if(check->inputs().size() != 1 &&
        check->outputs().size() != 1)
        return false;

    const TfLiteTensor* input_tensor = check->input_tensor(0);
    const TfLiteTensor* output_tensor = check->output_tensor(0);

    if(input_tensor->type != kTfLiteFloat32 &&
        output_tensor->type != kTfLiteFloat32)
        return false;

    int64_t sz = NumElements(input_tensor);

    if(sz <= 0)
        return false;

    input_size = (size_t)sz;

    if(NumElements(output_tensor) != 1)
        return false;

    if(check->AllocateTensors() != kTfLiteOk)
        return false;

    interpreter = std::move(check);
    return true;
}

bool BinaryClassifier::buildFromFile(const std::string& path)
{
    std::string data;

    if(!readFile(path, data))
    {
        interpreter.reset();
        return false;
    }

    return build(std::move(data));
}

bool BinaryClassifier::run(const char* buffer,
    size_t buffer_size, float& output)
{
    if(buffer_size == 0)
        return false;

    if(interpreter == nullptr)
        return false;

    if(interpreter->ResetVariableTensors() != kTfLiteOk)
        return false;

    if(buffer_size > input_size)
        buffer_size = input_size;

    size_t pad_size = input_size - buffer_size;

    float* input = interpreter->typed_input_tensor<float>(0);

    for(size_t i = 0; i < pad_size; i++)
        input[i] = (float)0;

    for(size_t i = 0; i < buffer_size; i++)
    {
        uint8_t byte = (uint8_t)buffer[i];
        input[pad_size + i] = (float)byte;
    }

    if(interpreter->Invoke() != kTfLiteOk)
        return false;

    output = *interpreter->typed_output_tensor<float>(0);
    return true;
}
