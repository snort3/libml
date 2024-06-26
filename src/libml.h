//--------------------------------------------------------------------------
// Copyright (C) 2023-2024 Cisco and/or its affiliates. All rights reserved.
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
// libml.h author Brandon Stultz <brastult@cisco.com>

#pragma once

#include <cstddef>
#include <memory>
#include <string>

const char* libml_version();

namespace tflite::impl
{
    class FlatBufferModel;
    class Interpreter;
}

class BinaryClassifier
{
public:
    BinaryClassifier();
    ~BinaryClassifier();

    bool build(std::string);
    bool buildFromFile(const std::string&);

    bool run(const char*, size_t, float&);

private:
    std::string src;
    size_t input_size = 0;
    std::unique_ptr<tflite::impl::FlatBufferModel> model;
    std::unique_ptr<tflite::impl::Interpreter> interpreter;
};
