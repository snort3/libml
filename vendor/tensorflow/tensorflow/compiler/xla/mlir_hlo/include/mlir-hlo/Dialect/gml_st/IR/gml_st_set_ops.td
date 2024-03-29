/* Copyright 2022 The TensorFlow Authors. All Rights Reserved.

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

// This is the operation definition file for ST ops.

#ifndef GML_ST_SET_OPS
#define GML_ST_SET_OPS

include "mlir/Interfaces/InferTypeOpInterface.td"
include "mlir/Interfaces/ViewLikeInterface.td"
include "mlir/IR/AttrTypeBase.td"
include "mlir/IR/OpBase.td"
include "mlir-hlo/Dialect/gml_st/IR/gml_st_ops_base.td"
include "mlir-hlo/Dialect/gml_st/transforms/compose_set_interface.td"

// Base class of all subset types.
class GMLST_Set<string name> : TypeDef<GmlSt_Dialect, name> { }

def GMLST_TileType : GMLST_Set<"Tile"> {
  let mnemonic = "tile";
  let summary = "Type that represents a tile of an index space.";
  let parameters = (ins ArrayRefParameter<"int64_t">:$shape);
  let assemblyFormat = "`<` custom<ShapeTypeDimensionsList>($shape) `>`";
  let extraClassDeclaration = [{
    unsigned getRank() const { return getShape().size(); }
    bool hasStaticShape() const {
      return llvm::none_of(getShape(), ShapedType::isDynamic);
    }
    int64_t getNumElements() const {
      return ShapedType::getNumElements(getShape());
    }
  }];
}

// Whether a type is a subset type.
def IsSetTypePred : Or<[GMLST_TileType.predicate]>;

def AnySet : Type<IsSetTypePred, "subset type">;

def GMLST_SpaceOp : GMLST_Op<"space", [Pure,
    DeclareOpInterfaceMethods<InferTypeOpInterface>]> {
  let arguments = (ins Variadic<Index>:$dynamic_sizes,
                       I64ArrayAttr:$static_sizes);
  let results = (outs GMLST_TileType:$result);
  let assemblyFormat = [{
    custom<DynamicIndexList>($dynamic_sizes, $static_sizes,
                               "ShapedType::kDynamicSize")
    attr-dict `:` qualified(type($result))
  }];
  let builders = [
    OpBuilder<(ins "ArrayRef<OpFoldResult>":$sizes,
                    CArg<"ArrayRef<NamedAttribute>", "{}">:$attrs)>,
  ];
  let extraClassDeclaration = [{
    unsigned getNumDynamicEntriesUpToIdx(unsigned idx);
    mlir::Value getDynamicSize(unsigned idx);
  }];
  let hasVerifier = 1;
}


def GMLST_TileOp : GMLST_Op<"tile", [
    Pure,
    AttrSizedOperandSegments,
    OffsetSizeAndStrideOpInterface,
    DeclareOpInterfaceMethods<InferTypeOpInterface>,
    DeclareOpInterfaceMethods<ComposeSetInterface>]> {
  let arguments = (ins GMLST_TileType:$superset,
                       Variadic<Index>:$offsets,
                       Variadic<Index>:$sizes,
                       Variadic<Index>:$strides,
                       I64ArrayAttr:$static_offsets,
                       I64ArrayAttr:$static_sizes,
                       I64ArrayAttr:$static_strides);
  let results = (outs GMLST_TileType:$result);
  let assemblyFormat = [{
    $superset
    custom<DynamicIndexList>($offsets, $static_offsets,
                               "ShapedType::kDynamicStrideOrOffset")
    custom<DynamicIndexList>($sizes, $static_sizes,
                               "ShapedType::kDynamicSize")
    custom<DynamicIndexList>($strides, $static_strides,
                               "ShapedType::kDynamicStrideOrOffset")
    attr-dict `:` qualified(type($superset)) `to` qualified(type($result))
  }];
  let builders = [
   OpBuilder<(ins "Value":$superset, "ArrayRef<OpFoldResult>":$offsets,
      "ArrayRef<OpFoldResult>":$sizes, "ArrayRef<OpFoldResult>":$strides,
      CArg<"ArrayRef<NamedAttribute>", "{}">:$attrs)>,
  ];
  let extraClassDeclaration = [{
    /// Return the expected rank of each of the`static_offsets`, `static_sizes`
    /// and `static_strides` attributes.
    std::array<unsigned, 3> getArrayAttrMaxRanks() {
      unsigned rank = getSuperset().getType().cast<TileType>().getRank();
      return {rank, rank, rank};
    }
    /// Return the number of leading operands before the `offsets`, `sizes` and
    /// and `strides` operands.
    static unsigned getOffsetSizeAndStrideStartOperandIndex() { return 1; }
  }];
  let hasVerifier = 1;
}

#endif // GML_ST_SET_OPS
