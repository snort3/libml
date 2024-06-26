/* Copyright 2024 The OpenXLA Authors.

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
#include "xla/service/gpu/fusions/transpose_mlir.h"

#include <cstdint>
#include <iterator>
#include <optional>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/container/inlined_vector.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"  // from @llvm-project
#include "mlir/Dialect/Tensor/IR/Tensor.h"  // from @llvm-project
#include "mlir/IR/AffineExpr.h"  // from @llvm-project
#include "mlir/IR/AffineMap.h"  // from @llvm-project
#include "mlir/IR/BuiltinTypes.h"  // from @llvm-project
#include "mlir/IR/ImplicitLocOpBuilder.h"  // from @llvm-project
#include "mlir/IR/TypeRange.h"  // from @llvm-project
#include "mlir/IR/Value.h"  // from @llvm-project
#include "mlir/IR/ValueRange.h"  // from @llvm-project
#include "mlir/Support/LLVM.h"  // from @llvm-project
#include "xla/hlo/ir/hlo_computation.h"
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/hlo/ir/hlo_instructions.h"
#include "xla/mlir/utils/type_util.h"
#include "xla/permutation_util.h"
#include "xla/service/gpu/fusions/mlir/computation_partitioner.h"
#include "xla/service/gpu/fusions/mlir/elemental_hlo_to_mlir.h"
#include "xla/service/gpu/fusions/mlir/ir/xla_gpu_ops.h"
#include "xla/service/gpu/fusions/tiling_util.h"
#include "xla/service/gpu/hlo_fusion_analysis.h"
#include "xla/service/gpu/ir_emission_utils.h"
#include "xla/service/gpu/launch_dimensions.h"
#include "xla/service/gpu/model/indexing_analysis.h"
#include "xla/service/gpu/model/indexing_map.h"
#include "xla/shape.h"
#include "xla/shape_util.h"
#include "xla/util.h"
#include "xla/xla_data.pb.h"

namespace xla {
namespace gpu {
namespace {

using llvm::SmallVector;
using mlir::AffineExpr;
using mlir::AffineMap;
using mlir::MLIRContext;
using mlir::RankedTensorType;
using mlir::Value;
using mlir::ValueRange;
using mlir::func::FuncOp;
using mlir::func::ReturnOp;
using mlir::tensor::ExtractOp;
using mlir::tensor::InsertOp;
using mlir_converter::ApplyAffineMap;
using mlir_converter::CallTargetProvider;
using mlir_converter::PartitionedComputation;

Tiling ComputeTransposeTiling(const TransposeDescription& tiled_transpose) {
  constexpr int kNumRows = 4;
  static_assert(WarpSize() % kNumRows == 0);

  // 3D view over the output shape.
  Vector3 transposed_dims = tiled_transpose.dimensions;
  Vector3 permutation = tiled_transpose.permutation;

  // Note: the supported permutations are their own inverses. Therefore we
  // always use the permutation, even when we want the inverse.
  CHECK((permutation == Vector3{0, 2, 1}) || (permutation == Vector3{2, 1, 0}));

  absl::InlinedVector<int64_t, 4> input_dims{transposed_dims[permutation[0]],
                                             transposed_dims[permutation[1]],
                                             transposed_dims[permutation[2]]};

  // We tile along the minor dimensions pre- and post-transpose.
  absl::InlinedVector<int64_t, 4> tile_sizes{1, 1, 1};
  tile_sizes[permutation[2]] = WarpSize() / kNumRows;
  absl::InlinedVector<int64_t, 4> num_threads{1, 1, WarpSize()};
  num_threads[permutation[2]] = kNumRows;

  return Tiling(input_dims, tile_sizes, num_threads);
}

}  // namespace

MlirTransposeFusion::MlirTransposeFusion(const HloFusionAnalysis& analysis)
    : analysis_(analysis),
      tiling_(ComputeTransposeTiling(analysis.tiled_transpose())) {
  ConstHloInstructionSet transposes_to_tile;
  int index = 0;
  for (auto [root, hero] :
       llvm::zip(analysis_.fusion_roots(), analysis_.fusion_heroes())) {
    if (auto transpose = GetDescriptionForTiledTransposeEmitter(*root, *hero)) {
      transposes_to_tile.insert(hero);
      shmem_transpose_roots_.push_back(root);
      shmem_transpose_root_indices_.push_back(index);
      permutation_ = transpose->permutation;
    } else {
      side_output_roots_.push_back(root);
      side_output_root_indices_.push_back(index);
    }
    ++index;
  }
  shmem_transposes_ = {transposes_to_tile.begin(), transposes_to_tile.end()};
}

std::optional<IndexingMap> MlirTransposeFusion::ComputeThreadIdToOutputIndexing(
    int64_t root_index, MLIRContext* mlir_context) const {
  const auto& hero = analysis_.fusion_hero(root_index).instruction();
  // The block offsets are permuted, but the thread offsets remain the same.
  auto block_offset = GetBlockOffsetsForTiling(tiling_, mlir_context)
                          .getSubMap(std::vector<unsigned>{permutation_.begin(),
                                                           permutation_.end()});
  auto thread_offset = GetThreadOffsetsForTiling(tiling_, mlir_context);
  auto permuted_tiled_shape =
      ShapeUtil::MakeShape(U8, Permute(tiling_.GetShape(), permutation_));

  auto map = ComposeIndexingMaps(
      GetIndexingMapForTiling(
          block_offset, thread_offset, tiling_.GetNumThreadsPerBlock(),
          tiling_.GetNumBlocks(), tiling_.GetThreadTileSize(),
          permuted_tiled_shape.dimensions()),
      GetBitcastMap(permuted_tiled_shape, hero.shape(), mlir_context));
  map.Simplify(GetIndexingMapForInstruction);
  return map;
}

IndexingMap MlirTransposeFusion::ComputeThreadIdToInputIndexing(
    const HloInstruction& hero, MLIRContext* mlir_context) const {
  auto map = ComposeIndexingMaps(
      GetIndexingMapForTiling(tiling_, mlir_context),
      GetBitcastMap(tiling_.GetXlaShape(), hero.operand(0)->shape(),
                    mlir_context));
  map.Simplify(GetIndexingMapForInstruction);
  return map;
}

std::optional<IndexingMap> MlirTransposeFusion::ComputeThreadIdToInputIndexing(
    int64_t root_index, int64_t hero_operand_index,
    MLIRContext* mlir_context) const {
  const auto& hero = analysis_.fusion_hero(root_index).instruction();
  const auto& root = analysis_.fusion_root(root_index).instruction();
  if (!GetDescriptionForTiledTransposeEmitter(root, hero)) {
    // Non-transpose roots are elementwise by definition.
    return ComputeThreadIdToOutputIndexing(root_index, mlir_context);
  }
  return ComputeThreadIdToInputIndexing(hero, mlir_context);
}

LaunchDimensions MlirTransposeFusion::launch_dimensions() const {
  return LaunchDimensions(tiling_.GetNumBlocks(),
                          tiling_.GetNumThreadsPerBlock());
}

// Returns an indexing map with block_x, block_y, block_z set to 0.
IndexingMap GetSharedMemoryWriteIndexingMap(
    const IndexingMap& thread_id_indexing, int loop_dim) {
  auto* mlir_context = thread_id_indexing.GetMLIRContext();

  AffineExpr c0 = mlir::getAffineConstantExpr(0, mlir_context);
  AffineExpr th_x = mlir::getAffineDimExpr(0, mlir_context);
  SmallVector<AffineExpr, 3> tile_sizes(3);
  mlir::bindSymbolsList(mlir_context, llvm::MutableArrayRef(tile_sizes));
  SmallVector<AffineExpr, 3> shared_memory_indices = {
      th_x.floorDiv(32) + 4 * tile_sizes[loop_dim], th_x % 32};
  for (auto [index, range_val] :
       llvm::enumerate(thread_id_indexing.GetRangeVars())) {
    if (range_val.range.NumElements() == 1) {
      shared_memory_indices.insert(shared_memory_indices.begin() + index, c0);
      break;
    }
  }

  IndexingMap shmem_write_indexing{
      AffineMap::get(thread_id_indexing.GetDimensionCount(),
                     thread_id_indexing.GetSymbolCount(), shared_memory_indices,
                     mlir_context),
      thread_id_indexing.GetDimVars(), thread_id_indexing.GetRangeVars(),
      thread_id_indexing.GetRTVars(), thread_id_indexing.GetConstraints()};
  return shmem_write_indexing;
}

// Returns an indexing map with block_x, block_y, block_z set to 0 and swapped
// 2nd and 3rd results.
IndexingMap GetSharedMemoryReadIndexingMap(
    const IndexingMap& thread_id_indexing, Vector3 permutation) {
  IndexingMap write_indexing = GetSharedMemoryWriteIndexingMap(
      thread_id_indexing, /*loop_dim=*/permutation[2]);
  llvm::SmallVector<unsigned, 3> positions;
  absl::c_copy(permutation, std::back_inserter(positions));
  return IndexingMap{write_indexing.GetAffineMap().getSubMap(positions),
                     write_indexing.GetDimVars(), write_indexing.GetRangeVars(),
                     write_indexing.GetRTVars(),
                     write_indexing.GetConstraints()};
}

MlirTransposeFusion::WriteResult MlirTransposeFusion::EmitWriteToShMemMlir(
    mlir::ImplicitLocOpBuilder& builder, FuncOp entry_function,
    const HloFusionInstruction& fusion,
    const PartitionedComputation& root_computation,
    const CallTargetProvider& call_target_provider,
    ValueRange output_args) const {
  std::vector<int64_t> shmem_tensor_size(tiling_.GetBlockTileSize().begin(),
                                         tiling_.GetBlockTileSize().end());
  // Avoid bank conflict.
  ++shmem_tensor_size.back();

  MLIRContext* ctx = builder.getContext();

  WriteResult write_result;
  // Allocate shared memory.
  SmallVector<Value> inits;
  for (auto* transpose : shmem_transposes_) {
    auto elem_type = *ConvertPrimitiveTypeToMlirType(
        transpose->shape().element_type(), builder);
    inits.push_back(builder.create<AllocateSharedOp>(
        RankedTensorType::get(shmem_tensor_size, elem_type)));
  }

  // Add output arguments for side outputs.
  int num_inputs = fusion.fused_instructions_computation()->num_parameters();
  for (int index : side_output_root_indices_) {
    inits.push_back(entry_function.getArgument(num_inputs + index));
  }

  auto tile_indexing = GetIndexingMapForTiling(tiling_, ctx);
  IndexingMap shmem_write_indexing =
      GetSharedMemoryWriteIndexingMap(tile_indexing, permutation_[2]);
  auto body_builder = [&](ValueRange output_tensors, ValueRange dim_values,
                          ValueRange symbol_values) -> SmallVector<Value> {
    auto input_indices = [&](const HloInstruction* instr) {
      auto map = ComposeIndexingMaps(
          tile_indexing,
          GetBitcastMap(tiling_.GetXlaShape(), instr->shape(), ctx));
      return ApplyAffineMap(map.GetAffineMap(), dim_values, symbol_values,
                            builder);
    };
    SmallVector<Value> result_tensors;
    auto shmem_indices = ApplyAffineMap(shmem_write_indexing.GetAffineMap(),
                                        dim_values, symbol_values, builder);
    for (auto [transpose, output] :
         llvm::zip(shmem_transposes_, output_tensors)) {
      // Emit loop that writes subgraphs of transpose operands to shmem.
      auto result_scalar = mlir_converter::ProvideParameter(
          root_computation, transpose,
          /*operand_index=*/0, input_indices(transpose->operand(0)),
          call_target_provider, entry_function, builder)[0];
      result_tensors.push_back(
          builder.create<InsertOp>(result_scalar, output, shmem_indices));
    }

    // Produce all side outputs and then write them.
    SmallVector<Value> side_outputs;
    SmallVector<SmallVector<Value>> side_output_indices;
    auto* root_tuple = fusion.fused_expression_root();
    for (auto root : side_output_roots_) {
      side_output_indices.push_back(input_indices(root));
      side_outputs.append(mlir_converter::ProvideParameter(
          root_computation, root_tuple, root_tuple->operand_index(root),
          side_output_indices.back(), call_target_provider, entry_function,
          builder));
    }

    for (const auto& [value, indices, output] :
         llvm::zip(side_outputs, side_output_indices,
                   output_tensors.take_back(side_output_roots_.size()))) {
      result_tensors.push_back(
          builder.create<InsertOp>(value, output, indices));
    }

    return result_tensors;
  };

  auto written_vector =
      EmitThreadLoopNest(builder, inits, tile_indexing, body_builder);
  ValueRange written = written_vector;
  auto shmem_tensors = written.take_front(shmem_transposes_.size());

  WriteResult result;
  result.shmem_tensors =
      builder
          .create<SyncThreadsOp>(mlir::TypeRange(shmem_tensors), shmem_tensors)
          .getResults();
  result.updated_outputs = output_args;
  for (auto [index, side_output_result] :
       llvm::zip(side_output_root_indices_,
                 written.take_back(side_output_roots_.size()))) {
    result.updated_outputs[index] = side_output_result;
  }
  return result;
}

void MlirTransposeFusion::EmitReadFromShMemMlir(
    mlir::ImplicitLocOpBuilder& builder, FuncOp entry_function,
    const HloFusionInstruction& fusion,
    const mlir_converter::PartitionedComputations& computations,
    const WriteResult& written) const {
  auto* mlir_context = builder.getContext();
  auto output_indexing = *ComputeThreadIdToOutputIndexing(0, mlir_context);
  auto shmem_output_indexing =
      GetSharedMemoryReadIndexingMap(output_indexing, permutation_);
  auto result_tensors = EmitThreadLoopNest(
      builder, written.updated_outputs, output_indexing,
      [&](ValueRange output_tensors, ValueRange dim_values,
          ValueRange symbol_values) -> SmallVector<Value> {
        auto shmem_indices =
            ApplyAffineMap(shmem_output_indexing.GetAffineMap(), dim_values,
                           symbol_values, builder);
        absl::flat_hash_map<const HloInstruction*, llvm::SmallVector<Value>>
            transpose_values;
        for (auto [transpose, shmem] :
             llvm::zip(shmem_transposes_, written.shmem_tensors)) {
          transpose_values[transpose].push_back(
              builder.create<ExtractOp>(shmem, shmem_indices));
        }
        llvm::SmallVector<Value> epilogue_indices = dim_values;
        absl::c_copy(symbol_values, std::back_inserter(epilogue_indices));
        auto result_scalars =
            EmitEpilogue(/*epilogue_index=*/0, computations, entry_function,
                         transpose_values, epilogue_indices, builder);
        SmallVector<Value> results = output_tensors;
        for (auto [root, indexing, root_index] :
             llvm::zip(shmem_transpose_roots_,
                       computations.epilogues().front().root_indexing,
                       shmem_transpose_root_indices_)) {
          llvm::SmallVector<Value> indices =
              ApplyAffineMap(indexing, dim_values, symbol_values, builder);
          results[root_index] = builder.create<InsertOp>(
              result_scalars.at(root).front(), results[root_index], indices);
        }
        return results;
      });

  builder.create<ReturnOp>(result_tensors);
}

std::vector<mlir_converter::EpilogueSpecification>
MlirTransposeFusion::GetEpilogues(const HloFusionInstruction& fusion,
                                  MLIRContext* mlir_context) const {
  return {mlir_converter::EpilogueSpecification::FromOutputIndexing(
      analysis_, shmem_transposes_, shmem_transpose_roots_, *this,
      mlir_context)};
}

absl::Status MlirTransposeFusion::EmitEntryFunction(
    const mlir_converter::PartitionedComputations& computations,
    const mlir_converter::CallTargetProvider& call_targets,
    mlir::func::FuncOp entry_function,
    const HloFusionInstruction& fusion) const {
  const auto& root_computation = computations.FindPartitionedComputation(
      fusion.fused_instructions_computation());
  // Write intermediate results to shmem.
  mlir::ImplicitLocOpBuilder builder(entry_function.getLoc(), entry_function);
  builder.setInsertionPointToStart(entry_function.addEntryBlock());
  auto written = EmitWriteToShMemMlir(
      builder, entry_function, fusion, root_computation, call_targets,
      entry_function.getArguments().take_back(analysis_.fusion_roots().size()));
  // Read intermediate results from shmem and compute epilogues.
  EmitReadFromShMemMlir(builder, entry_function, fusion, computations, written);
  return absl::OkStatus();
}

}  // namespace gpu
}  // namespace xla
