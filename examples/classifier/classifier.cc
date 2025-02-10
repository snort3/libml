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
// classifier.cc author Brandon Stultz <brastult@cisco.com>

#include <iostream>
#include <string>

#include "libml.h"

int main(int argc, char** argv)
{
    if(argc < 3)
    {
        std::cout << "usage: classifier <model> <input>\n";
        return 0;
    }

    std::cout << "Using LibML version " << libml::version() << "\n";

    std::string model_path(argv[1]);

    libml::BinaryClassifier classifier;

    if(!classifier.buildFromFile(model_path))
    {
        std::cout << "error: could not build classifier\n";
        return 1;
    }

    std::string input(argv[2]);
    float output = 0.0;

    if(!classifier.run(input.c_str(), input.length(), output))
    {
        std::cout << "error: could not run classifier\n";
        return 1;
    }

    std::cout << "Results\n"
              << "-------\n"
              << " input: '" << input << "'\n"
              << "output: "  << output*100.0 << "%\n";

    return 0;
}
