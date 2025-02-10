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
// libml.h author Brandon Stultz <brastult@cisco.com>

#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace tflite::impl
{
    class FlatBufferModel;
    class Interpreter;
}

namespace libml
{

const char* version();

class BinaryClassifier
{
public:
    BinaryClassifier();
    ~BinaryClassifier();

    BinaryClassifier(BinaryClassifier&) = delete;

    BinaryClassifier(BinaryClassifier&&) noexcept;
    BinaryClassifier& operator=(BinaryClassifier&&) noexcept;

    bool build(std::string);
    bool buildFromFile(const std::string&);

    bool run(const char*, size_t, float&);

    friend void swap(BinaryClassifier& l, BinaryClassifier& r) noexcept
    {
        std::swap(l.src, r.src);
        std::swap(l.input_size, r.input_size);
        std::swap(l.lowercase, r.lowercase);

        std::swap(l.model, r.model);
        std::swap(l.interpreter, r.interpreter);
    }

    friend class BinaryClassifierSet;

private:
    std::string src;
    size_t input_size = 0;
    bool lowercase = false;

    std::unique_ptr<tflite::impl::FlatBufferModel> model;
    std::unique_ptr<tflite::impl::Interpreter> interpreter;
};

class BinaryClassifierSet
{
public:
    bool build(std::vector<std::string>);

    bool run(const char*, size_t, float&);

private:
    std::vector<BinaryClassifier> classifiers;
};

}
