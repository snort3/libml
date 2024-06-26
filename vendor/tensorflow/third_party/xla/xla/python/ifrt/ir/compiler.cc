/* Copyright 2023 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "xla/python/ifrt/ir/compiler.h"

#include <memory>

namespace xla {
namespace ifrt {

char IfrtIRProgram::ID = 0;
char IfrtIRCompileOptions::ID = 0;

absl::StatusOr<std::unique_ptr<IfrtIRCompileOptions>> GetIfrtIRCompileOptions(
    std::unique_ptr<CompileOptions> options) {
  if (!llvm::isa<IfrtIRCompileOptions>(options.get())) {
    return absl::InvalidArgumentError("options must be IfrtIRCompileOptions");
  }
  return std::unique_ptr<IfrtIRCompileOptions>(
      static_cast<IfrtIRCompileOptions*>(options.release()));
}

}  // namespace ifrt
}  // namespace xla
