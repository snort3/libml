//--------------------------------------------------------------------------
// Copyright (C) 2023-2025 Cisco and/or its affiliates. All rights reserved.
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

#include <algorithm>
#include <cstdint>
#include <utility>

#include "absl/strings/ascii.h"
#include "flatbuffers/flatbuffers.h"
#include "tensorflow/lite/interpreter.h"
#include "tensorflow/lite/kernels/kernel_util.h"
#include "tensorflow/lite/kernels/register.h"
#include "tensorflow/lite/logger.h"
#include "tensorflow/lite/model.h"

#include "libml.h"
#include "metadata_schema_generated.h"
#include "util.h"
#include "version.h"

using namespace libml;

static constexpr char MetadataBufferName[] = "LIBML_METADATA";

const char* libml::version()
{ return VERSION; }

BinaryClassifier::BinaryClassifier() = default;
BinaryClassifier::~BinaryClassifier() = default;

BinaryClassifier::BinaryClassifier(BinaryClassifier&& other) noexcept
{
    swap(*this, other);
}

BinaryClassifier& BinaryClassifier::operator=(
    BinaryClassifier&& other) noexcept
{
    swap(*this, other);
    return *this;
}

bool BinaryClassifier::build(std::string in)
{
    interpreter.reset();

    tflite::LoggerOptions::SetMinimumLogSeverity(
        tflite::TFLITE_LOG_ERROR);

    src = std::move(in);

    model = tflite::FlatBufferModel::VerifyAndBuildFromBuffer(
        src.data(), src.size());

    if(!model)
        return false;

    auto md_map = model->ReadAllMetadata();
    auto search = md_map.find(MetadataBufferName);

    if(search != md_map.end())
    {
        const std::string& val = search->second;
        const uint8_t* buf = (const uint8_t*)val.data();
        size_t len = val.size();

        flatbuffers::Verifier::Options opts;
        auto verifier = flatbuffers::Verifier(buf, len, opts);

        if(!VerifyMetadataBuffer(verifier))
            return false;

        const Metadata* md = GetMetadata(buf);

        if(!md)
            return false;

        lowercase = md->lowercase();
    }

    tflite::ops::builtin::BuiltinOpResolver resolver;

    std::unique_ptr<tflite::Interpreter> check;

    tflite::InterpreterBuilder builder(*model, resolver);

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

    int64_t sz = tflite::NumElements(input_tensor);

    if(sz <= 0)
        return false;

    input_size = (size_t)sz;

    if(tflite::NumElements(output_tensor) != 1)
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
    if(!interpreter || buffer_size == 0)
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
        uint8_t byte = uint8_t(lowercase ?
            absl::ascii_tolower((unsigned char)buffer[i]) : buffer[i]);

        input[pad_size + i] = (float)byte;
    }

    if(interpreter->Invoke() != kTfLiteOk)
        return false;

    output = *interpreter->typed_output_tensor<float>(0);
    return true;
}

bool BinaryClassifierSet::build(std::vector<std::string> models)
{
    classifiers.clear();

    if(models.empty())
        return false;

    std::vector<BinaryClassifier> vec;

    for(auto& model : models)
    {
        BinaryClassifier classifier;

        if(!classifier.build(std::move(model)))
            return false;

        auto it = std::find_if(vec.begin(), vec.end(),
            [&](const BinaryClassifier& c)
            { return c.input_size == classifier.input_size; });

        if(it != vec.end())
        {
            *it = std::move(classifier);
            continue;
        }

        vec.push_back(std::move(classifier));
    }

    std::sort(vec.begin(), vec.end(),
        [](const BinaryClassifier& l, const BinaryClassifier& r)
        { return l.input_size < r.input_size; });

    classifiers = std::move(vec);
    return true;
}

bool BinaryClassifierSet::run(const char* buffer,
    size_t buffer_size, float& output)
{
    if(classifiers.empty() || buffer_size == 0)
        return false;

    for(auto& classifier : classifiers)
    {
        if(classifier.input_size >= buffer_size)
            return classifier.run(buffer, buffer_size, output);
    }

    return classifiers.back().run(buffer, buffer_size, output);
}
