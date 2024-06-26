/* Copyright 2017 The OpenXLA Authors.

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

#ifndef XLA_SERVICE_CPU_ELEMENTAL_IR_EMITTER_H_
#define XLA_SERVICE_CPU_ELEMENTAL_IR_EMITTER_H_

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Value.h"
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/service/cpu/ir_emitter.h"
#include "xla/service/elemental_ir_emitter.h"
#include "xla/statusor.h"

namespace xla {
namespace cpu {

class CpuElementalIrEmitter : public ElementalIrEmitter {
 public:
  CpuElementalIrEmitter(const HloModuleConfig& module_config,
                        IrEmitter* ir_emitter, llvm::Module* module)
      : ElementalIrEmitter(module, ir_emitter->b()),
        hlo_module_config_(module_config),
        ir_emitter_(ir_emitter) {}

 protected:
  absl::StatusOr<llvm::Value*> EmitAtan2(PrimitiveType prim_type,
                                         llvm::Value* lhs, llvm::Value* rhs,
                                         absl::string_view name) override;
  absl::StatusOr<llvm::Value*> EmitTanh(PrimitiveType prim_type,
                                        llvm::Value* value) override;
  absl::StatusOr<llvm::Value*> EmitErf(PrimitiveType prim_type,
                                       llvm::Value* value) override;

  absl::StatusOr<std::vector<llvm::Value*>> EmitThreadLocalCall(
      const HloComputation& callee, absl::Span<llvm::Value* const> parameters,
      absl::string_view name, bool is_reducer) override {
    return ir_emitter_->EmitThreadLocalCall(callee, parameters, name,
                                            is_reducer);
  }

  bool fast_min_max() override {
    return hlo_module_config_.debug_options().xla_cpu_enable_fast_min_max();
  }

  const HloModuleConfig& hlo_module_config_;
  IrEmitter* ir_emitter_;
};

}  // namespace cpu
}  // namespace xla

#endif  // XLA_SERVICE_CPU_ELEMENTAL_IR_EMITTER_H_
