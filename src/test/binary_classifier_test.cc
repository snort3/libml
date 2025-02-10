//--------------------------------------------------------------------------
// Copyright (C) 2024-2025 Cisco and/or its affiliates. All rights reserved.
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
// binary_classifier_test.cc author Brandon Stultz <brastult@cisco.com>

#include <string>
#include <vector>

#include "libml.h"
#include "util.h"

#include "CppUTest/CommandLineTestRunner.h"
#include "CppUTest/TestHarness.h"

TEST_GROUP(binary_classifier_test_group) {};

TEST(binary_classifier_test_group, model_check)
{
    libml::BinaryClassifier classifier;

    CHECK(classifier.buildFromFile("models/256.model"));

    std::string input = "foo=bar' or 1=1;--";
    float output = 0.0;

    CHECK(classifier.run(input.c_str(), input.length(), output));

    CHECK(output > 0.95);
}

TEST(binary_classifier_test_group, lowercase_model_check)
{
    libml::BinaryClassifier classifier;

    CHECK(classifier.buildFromFile("models/lowercase.model"));

    std::string input = "foo=bar' or 1=1;--";
    float output = 0.0;

    CHECK(classifier.run(input.c_str(), input.length(), output));

    CHECK(output > 0.95);
}

TEST(binary_classifier_test_group, classifier_set_check)
{
    std::vector<std::string> models(3);

    CHECK(readFile("models/256.model", models[0]));
    CHECK(readFile("models/512.model", models[1]));
    CHECK(readFile("models/1024.model", models[2]));

    libml::BinaryClassifierSet classifiers;

    CHECK(classifiers.build(models));

    std::string input = "foo=bar' or 1=1;--";
    float output = 0.0;

    CHECK(classifiers.run(input.c_str(), input.length(), output));

    CHECK(output > 0.95);
}

int main(int argc, char* argv[])
{
    MemoryLeakWarningPlugin::turnOffNewDeleteOverloads();
    return CommandLineTestRunner::RunAllTests(argc, argv);
}
