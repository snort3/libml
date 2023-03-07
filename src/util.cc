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
// util.cc author Brandon Stultz <brastult@cisco.com>

#include <fstream>
#include <iostream>

bool readFile(const std::string& path, std::string& buffer)
{
    std::ifstream file(path, std::ios::binary);

    if(!file.is_open())
        return false;

    file.seekg(0, std::ios::end);
    size_t size = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);

    if(size == 0)
    {
        buffer = {};
        return true;
    }

    buffer.resize(size);
    file.read(&buffer[0], std::streamsize(size));
    return true;
}
