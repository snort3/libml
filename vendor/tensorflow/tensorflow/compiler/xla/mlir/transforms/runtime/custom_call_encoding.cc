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

#include "tensorflow/compiler/xla/mlir/transforms/runtime/custom_call_encoding.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "llvm/ADT/TypeSwitch.h"
#include "mlir/Conversion/LLVMCommon/MemRefBuilder.h"  // from @llvm-project
#include "mlir/Dialect/Arith/IR/Arith.h"  // from @llvm-project
#include "mlir/Dialect/Func/IR/FuncOps.h"  // from @llvm-project
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"  // from @llvm-project
#include "mlir/Dialect/LLVMIR/LLVMTypes.h"  // from @llvm-project
#include "mlir/IR/Attributes.h"  // from @llvm-project
#include "mlir/IR/Builders.h"  // from @llvm-project
#include "mlir/IR/BuiltinAttributes.h"  // from @llvm-project
#include "mlir/IR/BuiltinTypes.h"  // from @llvm-project
#include "mlir/IR/Types.h"  // from @llvm-project
#include "mlir/IR/Value.h"  // from @llvm-project
#include "mlir/Support/LogicalResult.h"  // from @llvm-project
#include "tensorflow/compiler/xla/mlir/ir/runtime/rt_dialect.h"
#include "tensorflow/compiler/xla/runtime/custom_call.h"
#include "tensorflow/compiler/xla/runtime/tracing.h"
#include "tensorflow/compiler/xla/runtime/type_id.h"

namespace Eigen {
struct half;
}  // namespace Eigen

namespace xla {
namespace runtime {

using namespace mlir;  // NOLINT
using arith::ConstantOp;
using func::FuncOp;

using llvm::ArrayRef;

//===----------------------------------------------------------------------===//
// Custom call arguments encoding.
//===----------------------------------------------------------------------===//

using EncodedArg = CustomCallArgEncodingSet::Encoded;

FailureOr<EncodedArg> CustomCallArgEncodingSet::Encode(Globals &g,
                                                       ImplicitLocOpBuilder &b,
                                                       Value value,
                                                       Value converted) const {
  for (auto &encoding : encodings_)
    if (succeeded(encoding->Match(value, converted)))
      return encoding->Encode(g, b, value, converted);
  return failure();
}

//===----------------------------------------------------------------------===//
// Custom call results encoding.
//===----------------------------------------------------------------------===//

using EncodedRet = CustomCallRetEncodingSet::Encoded;

FailureOr<EncodedRet> CustomCallRetEncodingSet::Encode(Globals &g,
                                                       ImplicitLocOpBuilder &b,
                                                       Type type,
                                                       Type converted) const {
  for (auto &encoding : encodings_)
    if (succeeded(encoding->Match(type, converted)))
      return encoding->Encode(g, b, type, converted);
  return failure();
}

FailureOr<Value> CustomCallRetEncodingSet::Decode(ImplicitLocOpBuilder &b,
                                                  Type type, Type converted,
                                                  LLVM::AllocaOp alloca) const {
  for (auto &encoding : encodings_) {
    if (succeeded(encoding->Match(type, converted)))
      return encoding->Decode(b, type, converted, alloca);
  }
  return failure();
}

//===----------------------------------------------------------------------===//
// Custom call attributes encoding.
//===----------------------------------------------------------------------===//

using EncodedAttr = CustomCallAttrEncodingSet::Encoded;

FailureOr<EncodedAttr> CustomCallAttrEncodingSet::Encode(
    mlir::SymbolTable &sym_table, Globals &g, ImplicitLocOpBuilder &b,
    std::string_view name, Attribute attr) const {
  for (auto &encoding : encodings_)
    if (succeeded(encoding->Match(sym_table, name, attr)))
      return encoding->Encode(sym_table, g, b, name, attr);
  return failure();
}

//===----------------------------------------------------------------------===//
// A set of helper functions for packing primitive attributes.
//===----------------------------------------------------------------------===//

Value PackTypeId(Globals &g, ImplicitLocOpBuilder &b, TypeID type_id) {
  auto global = g.GetOrCreate(b, type_id);
  return Globals::AddrOf(b, global);
}

Value PackString(Globals &g, ImplicitLocOpBuilder &b, std::string_view strref,
                 std::string_view symbol_base) {
  MLIRContext *ctx = b.getContext();
  int64_t size = strref.size();

  // Encoded string type: !llvm.struct<(i64, !llvm.ptr<array<i8 x len>>)>.
  Type ptr = LLVM::LLVMPointerType::get(ctx);
  Type type = LLVM::LLVMStructType::getLiteral(ctx, {b.getI64Type(), ptr});

  // Global constant initializer for the encoded string structure
  auto init = [&](ImplicitLocOpBuilder &ib, Attribute) {
    // String size and pointer to a null-terminated string.
    Value num_elements = ib.create<ConstantOp>(ib.getI64IntegerAttr(size));
    Value str = Globals::AddrOf(ib, g.GetOrCreate(b, strref, "__rt_str"));

    // Store size and pointer into the struct.
    Value encoded = ib.create<LLVM::UndefOp>(type);
    encoded = ib.create<LLVM::InsertValueOp>(encoded, num_elements, 0);
    encoded = ib.create<LLVM::InsertValueOp>(encoded, str, 1);
    ib.create<LLVM::ReturnOp>(encoded);
  };

  auto value = b.getStringAttr(strref);
  auto global = g.GetOrCreate(b, value, type, symbol_base, init);
  return Globals::AddrOf(b, global);
}

// Packs scalar attribute as a global constant. Returns `!llvm.ptr<AttrType>`.
Value PackScalarAttribute(Globals &g, ImplicitLocOpBuilder &b, Attribute value,
                          std::string_view symbol_base) {
  auto global = g.GetOrCreate(b, value, symbol_base);
  return Globals::AddrOf(b, global);
}

// Reshape dense elements as a one-dimensional array.
static mlir::DenseElementsAttr Flatten(DenseIntOrFPElementsAttr dense) {
  ShapedType shaped_type = dense.getType();
  ShapedType new_shaped_type = shaped_type.cloneWith(
      {shaped_type.getNumElements()}, dense.getElementType());
  return dense.reshape(new_shaped_type);
}

//===----------------------------------------------------------------------===//
// A set of helper functions for packing dense and array-like attributes.
//===----------------------------------------------------------------------===//

// Packs dense elements attribute as a global constant. Returns
// `!llvm.ptr<EncodedDenseElements>`.
static Value PackDenseElementsAttribute(Globals &g, ImplicitLocOpBuilder &b,
                                        Attribute value,
                                        std::string_view symbol_base) {
  MLIRContext *ctx = b.getContext();
  DenseIntOrFPElementsAttr dense = value.cast<DenseIntOrFPElementsAttr>();

  Type ptr = LLVM::LLVMPointerType::get(ctx);

  // Store dense elements data as !llvm.array<element_type x num_elements>
  Type array_type =
      LLVM::LLVMArrayType::get(dense.getElementType(), dense.getNumElements());

  // Encoded array type: !llvm.struct<(i64, !llvm.ptr)>.
  //
  // We use the same type for encoding dense elements attributes as the type for
  // encoding arrays attributes, so that at run time we can safely reinterpret
  // cast pointers to dense elements attributes (shaped tensors) as pointers to
  // flat array attributes.
  //
  // See `PackArrayAttribute` defined below.
  Type encoded_arr_type =
      LLVM::LLVMStructType::getLiteral(ctx, {b.getI64Type(), ptr});

  int64_t rank = dense.getType().getRank();
  ArrayRef<int64_t> shape = dense.getType().getShape();
  Type shape_arr_type = LLVM::LLVMArrayType::get(b.getI64Type(), rank);

  // Encoded dense elements type:
  //   !llvm.struct<encoded_arr_type, i64, array<i64, rank>
  Type type = LLVM::LLVMStructType::getLiteral(
      ctx, {encoded_arr_type, b.getI64Type(), shape_arr_type});

  // Global constant initializer for the encoded array structure.
  auto init = [&](ImplicitLocOpBuilder &ib, Attribute) {
    Value num_elements =
        ib.create<ConstantOp>(b.getI64IntegerAttr(dense.getNumElements()));
    Value data_ptr = Globals::AddrOf(
        ib, g.GetOrCreate(b, Flatten(dense), array_type, symbol_base));

    // Create the encoded array struct.
    Value encoded_arr = ib.create<LLVM::UndefOp>(encoded_arr_type);
    encoded_arr = ib.create<LLVM::InsertValueOp>(encoded_arr, num_elements, 0);
    encoded_arr = ib.create<LLVM::InsertValueOp>(encoded_arr, data_ptr, 1);

    // Get rank and shape.
    Value rank_value = ib.create<ConstantOp>(b.getI64IntegerAttr(rank));
    Value shape_value = ib.create<LLVM::UndefOp>(shape_arr_type);

    // Store each dimension size into shape_value.
    for (int i = 0; i < rank; i++) {
      Value dim = ib.create<ConstantOp>(ib.getI64IntegerAttr(shape[i]));
      shape_value = ib.create<LLVM::InsertValueOp>(shape_value, dim, i);
    }

    // Store the encoded_arr, rank, and shape into the struct.
    Value encoded = ib.create<LLVM::UndefOp>(type);
    encoded = ib.create<LLVM::InsertValueOp>(encoded, encoded_arr, 0);
    encoded = ib.create<LLVM::InsertValueOp>(encoded, rank_value, 1);
    encoded = ib.create<LLVM::InsertValueOp>(encoded, shape_value, 2);
    ib.create<LLVM::ReturnOp>(encoded);
  };

  auto global = g.GetOrCreate(b, value, type, symbol_base, init);
  return Globals::AddrOf(b, global);
}

// Create a global for the data array in an EncodedArray.
// Returns `!llvm.ptr<array<element_type x size>>
static Value CreateGlobalFromArray(Globals &g, ImplicitLocOpBuilder &b,
                                   ArrayAttr array, Type element_type,
                                   std::string_view symbol_base) {
  Type arr_type = LLVM::LLVMArrayType::get(element_type, array.size());

  auto init = [&](ImplicitLocOpBuilder &ib, Attribute) {
    Value data = ib.create<LLVM::UndefOp>(arr_type);
    for (int i = 0; i < array.size(); i++) {
      Value value = ib.create<ConstantOp>(array[i]);
      data = ib.create<LLVM::InsertValueOp>(data, value, i);
    }
    ib.create<LLVM::ReturnOp>(data);
  };

  auto global = g.GetOrCreate(b, array, arr_type, symbol_base, init);
  return Globals::AddrOf(b, global);
}

// Packs array attribute as a global constant. Returns `!llvm.ptr<EncodedArr>`.
static Value PackArrayAttribute(Globals &g, ImplicitLocOpBuilder &b,
                                ArrayAttr array, Type element_type,
                                std::string_view symbol_base) {
  MLIRContext *ctx = b.getContext();

  int64_t size = array.size();
  Type ptr = LLVM::LLVMPointerType::get(ctx);

  // Encoded array type: !llvm.struct<(i64, !llvm.ptr)>.
  Type type = LLVM::LLVMStructType::getLiteral(ctx, {b.getI64Type(), ptr});

  // Global constant initializer for the encoded array structure
  auto init = [&](ImplicitLocOpBuilder &ib, Attribute) {
    // Array size and the pointer to data.
    Value num_elements = ib.create<ConstantOp>(b.getI64IntegerAttr(size));
    Value data = CreateGlobalFromArray(g, b, array, element_type, symbol_base);

    // Store size and values into the struct.
    Value encoded = ib.create<LLVM::UndefOp>(type);
    encoded = ib.create<LLVM::InsertValueOp>(encoded, num_elements, 0);
    encoded = ib.create<LLVM::InsertValueOp>(encoded, data, 1);

    ib.create<LLVM::ReturnOp>(encoded);
  };

  auto global = g.GetOrCreate(b, array, type, symbol_base, init);
  return Globals::AddrOf(b, global);
}

template <typename T, typename AttrType, typename ArrayType>
static Value FillDataFromDenseArrayAttr(
    ImplicitLocOpBuilder &b, AttrType (ImplicitLocOpBuilder::*get_attr)(T),
    ArrayType array, Value data) {
  ArrayRef<T> array_ref = array.asArrayRef();
  for (int i = 0; i < array_ref.size(); i++) {
    Value value = b.create<ConstantOp>((b.*get_attr)(array_ref[i]));
    data = b.create<LLVM::InsertValueOp>(data, value, i);
  }
  return data;
}

static Value CreateGlobalFromDenseArray(Globals &g, ImplicitLocOpBuilder &b,
                                        DenseArrayAttr base_array,
                                        Type arr_type,
                                        std::string_view symbol_base) {
  auto init = [&](ImplicitLocOpBuilder &ib, Attribute) {
    Value data = ib.create<LLVM::UndefOp>(arr_type);
    llvm::TypeSwitch<DenseArrayAttr>(base_array)
        .Case([&](DenseI8ArrayAttr attr) {
          data = FillDataFromDenseArrayAttr<int8_t, IntegerAttr>(
              b, &ImplicitLocOpBuilder::getI8IntegerAttr, attr, data);
        })
        .Case([&](DenseI16ArrayAttr attr) {
          data = FillDataFromDenseArrayAttr<int16_t, IntegerAttr>(
              b, &ImplicitLocOpBuilder::getI16IntegerAttr, attr, data);
        })
        .Case([&](DenseI32ArrayAttr attr) {
          data = FillDataFromDenseArrayAttr<int32_t, IntegerAttr>(
              b, &ImplicitLocOpBuilder::getI32IntegerAttr, attr, data);
        })
        .Case([&](DenseI64ArrayAttr attr) {
          data = FillDataFromDenseArrayAttr<int64_t, IntegerAttr>(
              b, &ImplicitLocOpBuilder::getI64IntegerAttr, attr, data);
        })
        .Case([&](DenseF32ArrayAttr attr) {
          data = FillDataFromDenseArrayAttr<float, FloatAttr>(
              b, &ImplicitLocOpBuilder::getF32FloatAttr, attr, data);
        })
        .Case([&](DenseF64ArrayAttr attr) {
          data = FillDataFromDenseArrayAttr<double, FloatAttr>(
              b, &ImplicitLocOpBuilder::getF64FloatAttr, attr, data);
        })
        .Default([&](DenseArrayAttr attr) {
          assert(false && "unsupported DenseArrayAttr element type");
        });
    ib.create<LLVM::ReturnOp>(data);
  };

  auto global = g.GetOrCreate(b, base_array, arr_type, symbol_base, init);
  return Globals::AddrOf(b, global);
}

static Value PackDenseArrayAttribute(Globals &g, ImplicitLocOpBuilder &b,
                                     Attribute value,
                                     std::string_view symbol_base) {
  MLIRContext *ctx = b.getContext();

  DenseArrayAttr base_array = value.cast<DenseArrayAttr>();
  int64_t size = base_array.size();

  Type ptr = LLVM::LLVMPointerType::get(ctx);

  // Stored array type: !llvm.array<element_type x size>
  Type element_type = base_array.getType().getElementType();
  Type arr_type = LLVM::LLVMArrayType::get(element_type, size);

  // Encoded array type: !llvm.struct<(i64, !llvm.ptr)>.
  Type type = LLVM::LLVMStructType::getLiteral(ctx, {b.getI64Type(), ptr});

  // Global constant initializer for the encoded array structure
  auto init = [&](ImplicitLocOpBuilder &ib, Attribute) {
    // Array size and values.
    Value num_elements = ib.create<ConstantOp>(b.getI64IntegerAttr(size));
    Value data =
        CreateGlobalFromDenseArray(g, ib, base_array, arr_type, symbol_base);

    // Store size and values into the struct.
    Value encoded = ib.create<LLVM::UndefOp>(type);
    encoded = ib.create<LLVM::InsertValueOp>(encoded, num_elements, 0);
    encoded = ib.create<LLVM::InsertValueOp>(encoded, data, 1);

    ib.create<LLVM::ReturnOp>(encoded);
  };

  auto global = g.GetOrCreate(b, value, type, symbol_base, init);
  return Globals::AddrOf(b, global);
}

static Value PackEmptyArrayAttribute(Globals &g, ImplicitLocOpBuilder &b,
                                     Attribute value,
                                     std::string_view symbol_base) {
  MLIRContext *ctx = b.getContext();

  Type ptr = LLVM::LLVMPointerType::get(ctx);

  // Encoded array type: !llvm.struct<(i64, !llvm.ptr)>.
  Type type = LLVM::LLVMStructType::getLiteral(ctx, {b.getI64Type(), ptr});

  // Global constant initializer for the encoded array structure
  auto init = [&](ImplicitLocOpBuilder &ib, Attribute) {
    // Array size and the pointer to data.
    Value num_elements = ib.create<ConstantOp>(b.getI64IntegerAttr(0));
    Value data = ib.create<LLVM::NullOp>(ptr);

    // Store size and values into the struct.
    Value encoded = ib.create<LLVM::UndefOp>(type);
    encoded = ib.create<LLVM::InsertValueOp>(encoded, num_elements, 0);
    encoded = ib.create<LLVM::InsertValueOp>(encoded, data, 1);

    ib.create<LLVM::ReturnOp>(encoded);
  };

  auto global = g.GetOrCreate(b, value, type, symbol_base, init);
  return Globals::AddrOf(b, global);
}

//===----------------------------------------------------------------------===//
// Packing primitive values on the stack.
//===----------------------------------------------------------------------===//

// Returns the parent function operation for the given value.
static FuncOp GetParentFunc(Value value) {
  Block *parent_block = value.getParentBlock();
  Operation *parent_op = parent_block->getParentOp();

  return isa<FuncOp>(parent_op) ? cast<FuncOp>(parent_op)
                                : parent_op->getParentOfType<FuncOp>();
}

// Packs value on the stack. Returns allocation holding the value.
static LLVM::AllocaOp PackValue(ImplicitLocOpBuilder &b, Value value) {
  Type ptr = LLVM::LLVMPointerType::get(b.getContext());

  // Always create an `alloca` in the parent function entry block.
  // See: https://llvm.org/docs/Frontend/PerformanceTips.html#use-of-allocas
  LLVM::AllocaOp mem = [&]() -> LLVM::AllocaOp {
    Block &block = GetParentFunc(value).getBody().front();
    OpBuilder::InsertionGuard guard(b);
    b.setInsertionPointToStart(&block);
    Value one = b.create<ConstantOp>(b.getI32IntegerAttr(1));
    return b.create<LLVM::AllocaOp>(ptr, value.getType(), one, 0);
  }();

  b.create<LLVM::StoreOp>(value, mem);

  return mem;
}

//===----------------------------------------------------------------------===//
// A helper class to create global constants in the module.
//===----------------------------------------------------------------------===//

LLVM::GlobalOp Globals::Find(Key key) {
  auto it = globals_.find(key);
  if (it != globals_.end()) return it->second;
  return nullptr;
}

LLVM::GlobalOp Globals::GetOrCreate(ImplicitLocOpBuilder &b,
                                    std::string_view strref,
                                    std::string_view symbol_base) {
  // Create an std::string to get a null terminated sequence of characters.
  std::string str(strref);

  // Create a string reference that captures the null terminator.
  std::string_view ref(str.data(), str.size() + 1);
  StringAttr attr = b.getStringAttr(ref);
  Type arr = LLVM::LLVMArrayType::get(b.getI8Type(), ref.size());
  return GetOrCreate(b, attr, arr, symbol_base);
}

LLVM::GlobalOp Globals::GetOrCreate(ImplicitLocOpBuilder &b, TypedAttr attr,
                                    std::string_view symbol_base) {
  return GetOrCreate(b, attr, attr.getType(), symbol_base);
}

LLVM::GlobalOp Globals::GetOrCreate(ImplicitLocOpBuilder &b,
                                    mlir::TypeID type_id) {
  std::string_view name = type_id_names_.FindTypeIDSymbolName(type_id);
  assert(!name.empty() && "cannot find the symbol name of type_id");
  return GetOrCreate(b, IntegerAttr(), b.getI64Type(), name, /*initialize=*/{},
                     LLVM::Linkage::External);
}

LLVM::GlobalOp Globals::GetOrCreate(ImplicitLocOpBuilder &b, Attribute attr,
                                    Type type, std::string_view symbol_base,
                                    GlobalInitializer initialize,
                                    LLVM::Linkage linkage) {
  if (!initialize) {
    return *TryGetOrCreate(b, attr, type, symbol_base, /*initialize=*/{},
                           linkage);
  }

  return *TryGetOrCreate(
      b, attr, type, symbol_base,
      [&](ImplicitLocOpBuilder &b, Attribute) {
        return (initialize(b, attr), success());
      },
      linkage);
}

mlir::FailureOr<mlir::LLVM::GlobalOp> Globals::TryGetOrCreate(
    mlir::ImplicitLocOpBuilder &b, mlir::Attribute attr, mlir::Type type,
    std::string_view symbol_base, FailureOrGlobalInitializer initialize,
    mlir::LLVM::Linkage linkage) {
  // We assume that this triple uniquely identifies the global value and the
  // global initializer always produces the same value for given inputs.
  Key key(attr, type, b.getStringAttr(symbol_base));

  // Check if global value already exists ...
  if (auto global = Find(key)) return global;

  // ... otherwise create a new one.
  OpBuilder::InsertionGuard guard(b);
  b.setInsertionPointToStart(module_.getBody());

  // If the initialize function is not provided, create constant directly.
  if (!initialize) {
    auto global = b.create<LLVM::GlobalOp>(type, /*isConstant=*/true, linkage,
                                           symbol_base, attr);
    return (sym_table_.insert(global), globals_[key] = global);
  }

  // Create an uninitialized global.
  auto global = b.create<LLVM::GlobalOp>(type, /*isConstant=*/true, linkage,
                                         symbol_base, nullptr);

  // Call user-provided global initializer.
  mlir::Region &region = global.getInitializerRegion();
  mlir::Block *block = b.createBlock(&region);

  b.setInsertionPointToStart(block);
  if (failed(initialize(b, attr))) return failure();

  return (sym_table_.insert(global), globals_[key] = global);
}

/*static*/ Value Globals::AddrOf(ImplicitLocOpBuilder &b,
                                 LLVM::GlobalOp global) {
  return b.create<LLVM::AddressOfOp>(LLVM::LLVMPointerType::get(b.getContext()),
                                     global.getSymName());
}

//===----------------------------------------------------------------------===//
// Helper functions for encoding attributes and values for custom calls.
//===----------------------------------------------------------------------===//

static bool IsAnyOf(unsigned width, ArrayRef<unsigned> supported) {
  return llvm::any_of(supported, [&](unsigned w) { return w == width; });
}

static bool IsSupportedScalarType(Type type) {
  if (auto idx = type.dyn_cast<mlir::IndexType>()) return true;

  if (auto i = type.dyn_cast<mlir::IntegerType>())
    return i.isUnsigned() ? IsAnyOf(i.getWidth(), {8, 16, 32, 64})
                          : IsAnyOf(i.getWidth(), {1, 8, 16, 32, 64});

  if (auto fp = type.dyn_cast<mlir::FloatType>())
    return IsAnyOf(fp.getWidth(), {16, 32, 64});

  return false;
}

static bool IsSupportedScalarAttribute(Attribute attr) {
  if (auto typed = attr.dyn_cast<TypedAttr>())
    return IsSupportedScalarType(typed.getType());
  return false;
}

static TypeID ScalarRuntimeTypeId(Type type) {
  if (type.isIndex()) return TypeID::get<Tagged<int64_t>>();

  if (type.isUnsignedInteger(8)) return TypeID::get<Tagged<uint8_t>>();
  if (type.isUnsignedInteger(16)) return TypeID::get<Tagged<uint16_t>>();
  if (type.isUnsignedInteger(32)) return TypeID::get<Tagged<uint32_t>>();
  if (type.isUnsignedInteger(64)) return TypeID::get<Tagged<uint64_t>>();

  if (type.isInteger(1)) return TypeID::get<Tagged<bool>>();
  if (type.isInteger(8)) return TypeID::get<Tagged<int8_t>>();
  if (type.isInteger(16)) return TypeID::get<Tagged<int16_t>>();
  if (type.isInteger(32)) return TypeID::get<Tagged<int32_t>>();
  if (type.isInteger(64)) return TypeID::get<Tagged<int64_t>>();

  if (type.isBF16()) return TypeID::get<Tagged<Eigen::bfloat16>>();
  if (type.isF16()) return TypeID::get<Tagged<Eigen::half>>();
  if (type.isF32()) return TypeID::get<Tagged<float>>();
  if (type.isF64()) return TypeID::get<Tagged<double>>();

  assert(false && "unsupported type id");
  return TypeID::getFromOpaquePointer(reinterpret_cast<void *>(0xDEADBEEF));
}

static PrimitiveType ScalarPrimitiveType(Type type) {
  // Unsigned integer types.
  if (type.isUnsignedInteger(8)) return PrimitiveType::U8;
  if (type.isUnsignedInteger(16)) return PrimitiveType::U16;
  if (type.isUnsignedInteger(32)) return PrimitiveType::U32;
  if (type.isUnsignedInteger(64)) return PrimitiveType::U64;

  // Signed integer types.
  if (type.isInteger(1)) return PrimitiveType::PRED;
  if (type.isInteger(8)) return PrimitiveType::S8;
  if (type.isInteger(16)) return PrimitiveType::S16;
  if (type.isInteger(32)) return PrimitiveType::S32;
  if (type.isInteger(64)) return PrimitiveType::S64;

  // Floating point types.
  if (type.isF16()) return PrimitiveType::F16;
  if (type.isF32()) return PrimitiveType::F32;
  if (type.isF64()) return PrimitiveType::F64;
  if (type.isBF16()) return PrimitiveType::BF16;

  // Complex types.
  if (auto complex = type.dyn_cast<ComplexType>()) {
    if (complex.getElementType().isF32()) return PrimitiveType::C64;
    if (complex.getElementType().isF64()) return PrimitiveType::C128;
  }

  assert(false && "unsupported type id");
  return PrimitiveType::PRIMITIVE_TYPE_INVALID;
}

static TypeID ArrayRuntimeTypeId(Type elem_type) {
  if (elem_type.isInteger(8)) return TypeID::get<Tagged<ArrayRef<int8_t>>>();
  if (elem_type.isInteger(16)) return TypeID::get<Tagged<ArrayRef<int16_t>>>();
  if (elem_type.isInteger(32)) return TypeID::get<Tagged<ArrayRef<int32_t>>>();
  if (elem_type.isInteger(64)) return TypeID::get<Tagged<ArrayRef<int64_t>>>();
  if (elem_type.isF32()) return TypeID::get<Tagged<ArrayRef<float>>>();
  if (elem_type.isF64()) return TypeID::get<Tagged<ArrayRef<double>>>();

  assert(false && "unsupported type id");
  return TypeID::getFromOpaquePointer(reinterpret_cast<void *>(0xDEADBEEF));
}

static TypeID DenseElementsRuntimeTypeId(Type elem_type) {
  if (elem_type.isInteger(32))
    return TypeID::get<Tagged<CustomCall::TensorRef<int32_t>>>();
  if (elem_type.isInteger(64))
    return TypeID::get<Tagged<CustomCall::TensorRef<int64_t>>>();
  if (elem_type.isF32())
    return TypeID::get<Tagged<CustomCall::TensorRef<float>>>();
  if (elem_type.isF64())
    return TypeID::get<Tagged<CustomCall::TensorRef<double>>>();

  assert(false && "unsupported type id");
  return TypeID::getFromOpaquePointer(reinterpret_cast<void *>(0xDEADBEEF));
}

//===----------------------------------------------------------------------===//
// Custom call attributes encoding.
//===----------------------------------------------------------------------===//

LogicalResult StringAttrEncoding::Match(mlir::SymbolTable &,
                                        std::string_view name,
                                        Attribute attr) const {
  return success(attr.isa<StringAttr>());
}

FailureOr<EncodedAttr> StringAttrEncoding::Encode(mlir::SymbolTable &,
                                                  Globals &g,
                                                  ImplicitLocOpBuilder &b,
                                                  std::string_view name,
                                                  Attribute attr) const {
  auto str = attr.cast<StringAttr>();

  Encoded encoded;
  encoded.name = PackString(g, b, name, kAttrName);
  encoded.type_id = PackTypeId(g, b, TypeID::get<Tagged<std::string_view>>());
  encoded.value = PackString(g, b, str.getValue(), kAttrValue);
  return encoded;
}

//===----------------------------------------------------------------------===//

LogicalResult ScalarAttrEncoding::Match(mlir::SymbolTable &,
                                        std::string_view name,
                                        Attribute attr) const {
  return success(IsSupportedScalarAttribute(attr));
}

FailureOr<EncodedAttr> ScalarAttrEncoding::Encode(mlir::SymbolTable &,
                                                  Globals &g,
                                                  ImplicitLocOpBuilder &b,
                                                  std::string_view name,
                                                  Attribute attr) const {
  Type type = attr.cast<TypedAttr>().getType();

  Encoded encoded;
  encoded.name = PackString(g, b, name, kAttrName);
  encoded.type_id = PackTypeId(g, b, ScalarRuntimeTypeId(type));
  encoded.value = PackScalarAttribute(g, b, attr, kAttrValue);

  return encoded;
}

//===----------------------------------------------------------------------===//

LogicalResult DenseElementsAttrEncoding::Match(mlir::SymbolTable &,
                                               std::string_view name,
                                               Attribute attr) const {
  if (auto dense = attr.dyn_cast<DenseIntOrFPElementsAttr>())
    return success(IsSupportedScalarType(dense.getElementType()));
  return failure();
}

FailureOr<EncodedAttr> DenseElementsAttrEncoding::Encode(
    mlir::SymbolTable &, Globals &g, ImplicitLocOpBuilder &b,
    std::string_view name, Attribute attr) const {
  auto dense = attr.cast<DenseIntOrFPElementsAttr>();
  Type elem_type = dense.getType().getElementType();

  Encoded encoded;
  encoded.name = PackString(g, b, name, kAttrName);
  encoded.type_id = PackTypeId(g, b, DenseElementsRuntimeTypeId(elem_type));
  encoded.value = PackDenseElementsAttribute(g, b, attr, kAttrValue);

  return encoded;
}

//===----------------------------------------------------------------------===//

LogicalResult ArrayAttrEncoding::Match(mlir::SymbolTable &,
                                       std::string_view name,
                                       Attribute attr) const {
  if (auto array = attr.dyn_cast<ArrayAttr>();
      array && !array.empty() && array[0].isa<TypedAttr>()) {
    return success(IsSupportedScalarAttribute(array[0]));
  }
  return failure();
}

FailureOr<EncodedAttr> ArrayAttrEncoding::Encode(mlir::SymbolTable &,
                                                 Globals &g,
                                                 ImplicitLocOpBuilder &b,
                                                 std::string_view name,
                                                 Attribute attr) const {
  ArrayAttr array = attr.dyn_cast<ArrayAttr>();
  Type elem_type = array[0].cast<TypedAttr>().getType();

  // We only support array attributes with elements of same type.
  bool all_of_same_type = llvm::all_of(array, [&](Attribute attr) {
    auto typed = attr.dyn_cast<TypedAttr>();
    return typed && typed.getType() == elem_type;
  });
  if (!all_of_same_type) return failure();

  Encoded encoded;
  encoded.name = PackString(g, b, name, kAttrName);
  encoded.type_id = PackTypeId(g, b, ArrayRuntimeTypeId(elem_type));
  encoded.value = PackArrayAttribute(g, b, array, elem_type, kAttrValue);

  return encoded;
}

//===----------------------------------------------------------------------===//

LogicalResult DenseArrayAttrEncoding::Match(mlir::SymbolTable &,
                                            std::string_view name,
                                            Attribute attr) const {
  if (auto array = attr.dyn_cast<DenseArrayAttr>()) {
    return success();
  }
  return failure();
}

FailureOr<EncodedAttr> DenseArrayAttrEncoding::Encode(mlir::SymbolTable &,
                                                      Globals &g,
                                                      ImplicitLocOpBuilder &b,
                                                      std::string_view name,
                                                      Attribute attr) const {
  Type elem_type = attr.cast<DenseArrayAttr>().getType().getElementType();

  Encoded encoded;
  encoded.name = PackString(g, b, name, kAttrName);
  encoded.type_id = PackTypeId(g, b, ArrayRuntimeTypeId(elem_type));
  encoded.value = PackDenseArrayAttribute(g, b, attr, kAttrValue);

  return encoded;
}

//===----------------------------------------------------------------------===//

LogicalResult EmptyArrayAttrEncoding::Match(mlir::SymbolTable &,
                                            std::string_view name,
                                            Attribute attr) const {
  if (auto array = attr.dyn_cast<ArrayAttr>(); array && array.empty()) {
    return success();
  }
  return failure();
}

FailureOr<EncodedAttr> EmptyArrayAttrEncoding::Encode(mlir::SymbolTable &,
                                                      Globals &g,
                                                      ImplicitLocOpBuilder &b,
                                                      std::string_view name,
                                                      Attribute attr) const {
  Encoded encoded;
  encoded.name = PackString(g, b, name, kAttrName);
  encoded.type_id = PackTypeId(g, b, TypeID::get<Tagged<EmptyArrayRef>>());
  encoded.value = PackEmptyArrayAttribute(g, b, attr, kAttrValue);

  return encoded;
}

//===----------------------------------------------------------------------===//

LogicalResult SymbolRefAttrEncoding::Match(mlir::SymbolTable &sym_table,
                                           std::string_view name,
                                           Attribute attr) const {
  if (auto ref = attr.dyn_cast<FlatSymbolRefAttr>()) {
    auto exported = sym_table.lookup<func::FuncOp>(ref.getValue());
    return success(exported && exported->hasAttr(kExportedAttrName));
  }
  return failure();
}

FailureOr<EncodedAttr> SymbolRefAttrEncoding::Encode(
    mlir::SymbolTable &sym_table, Globals &g, ImplicitLocOpBuilder &b,
    std::string_view name, Attribute attr) const {
  // Get the exported function ordinal.
  auto ref = attr.cast<FlatSymbolRefAttr>();
  auto func = sym_table.lookup<func::FuncOp>(ref.getValue());
  auto ordinal = func->getAttrOfType<IntegerAttr>(kExportedAttrName);
  assert(ordinal.getType().isSignlessInteger(32));

  // Encode exported function ordinal as a scalar constant with function ordinal
  // type id.
  auto type_id = TypeID::get<Tagged<CustomCall::FunctionOrdinal>>();

  Encoded encoded;
  encoded.name = PackString(g, b, name, kAttrName);
  encoded.type_id = PackTypeId(g, b, type_id);
  encoded.value = PackScalarAttribute(g, b, ordinal, kAttrValue);

  return encoded;
}

//===----------------------------------------------------------------------===//
// Encoding for collection of attributes.
//===----------------------------------------------------------------------===//

FailureOr<Value> EncodeAttributes(mlir::SymbolTable &sym_table, Globals &g,
                                  ImplicitLocOpBuilder &b,
                                  const CustomCallAttrEncodingSet &encoding,
                                  std::string_view symbol_base,
                                  ArrayRef<NamedAttribute> attrs) {
  using EncodedAttr =
      std::pair<std::string_view, CustomCallAttrEncoding::Encoded>;

  // In addition to encoded attributes we encode the number of attributes.
  int64_t n_attrs = attrs.size();

  // We store encoded attribute as `!llvm.array<ptr<i8> x len>`.
  Type ptr = LLVM::LLVMPointerType::get(b.getContext());
  Type type = LLVM::LLVMArrayType::get(ptr, 1 + n_attrs * 3);

  // Global initializer that encodes attributes as pointers.
  auto init = [&](ImplicitLocOpBuilder &ib, Attribute) -> LogicalResult {
    // Try to encode each individual attribute.
    llvm::SmallVector<EncodedAttr> encoded_attrs;
    for (auto &attr : attrs) {
      auto encoded = encoding.Encode(sym_table, g, b, attr.getName().getValue(),
                                     attr.getValue());
      if (failed(encoded)) return failure();
      encoded_attrs.emplace_back(attr.getName().getValue(), *encoded);
    }

    // Prepare an array for encoding attributes.
    Value arr = b.create<LLVM::UndefOp>(type);
    auto insert_value = [&](Value value, int64_t offset) {
      arr = b.create<LLVM::InsertValueOp>(arr, value, offset);
    };

    // Insert the number of encoded attributes.
    Attribute num_attrs = b.getI64IntegerAttr(n_attrs);
    Value size = PackScalarAttribute(g, b, num_attrs, "__rt_num_attrs");
    insert_value(size, 0);

    // Insert encoded attributes into the allocated storage.
    for (auto &pair : llvm::enumerate(encoded_attrs)) {
      CustomCallAttrEncoding::Encoded encoded = pair.value().second;
      int64_t offset = 1 + pair.index() * 3;

      insert_value(encoded.name, offset + 0);
      insert_value(encoded.type_id, offset + 1);
      insert_value(encoded.value, offset + 2);
    }

    // Return attributes array from the global initializer block.
    b.create<LLVM::ReturnOp>(arr);

    return success();
  };

  // Put all attributes in a dictionary attribute, so we can rely use it as a
  // part of the `Globals` cache key.
  auto attrs_map = DictionaryAttr::get(b.getContext(), attrs);
  auto global = g.TryGetOrCreate(b, attrs_map, type, symbol_base, init);
  if (failed(global)) return failure();

  // Return an address of global encoding attributes.
  return Globals::AddrOf(b, *global);
}

//===----------------------------------------------------------------------===//
// Custom call arguments encodings.
//===----------------------------------------------------------------------===//

LogicalResult ScalarArgEncoding::Match(Value value, Value converted) const {
  return success(IsSupportedScalarType(value.getType()));
}

FailureOr<EncodedArg> ScalarArgEncoding::Encode(Globals &g,
                                                ImplicitLocOpBuilder &b,
                                                Value value,
                                                Value converted) const {
  Type type = converted.getType();

  Encoded encoded;
  encoded.type_id = PackTypeId(g, b, ScalarRuntimeTypeId(type));
  encoded.value = PackValue(b, converted);

  return encoded;
}

//===----------------------------------------------------------------------===//

static bool IsOpaqueValue(Value value) {
  return value.getType().isa<OpaqueType>();
}

OpaqueArgEncoding::OpaqueArgEncoding()
    : OpaqueArgEncoding(IsOpaqueValue, TypeID::get<Tagged<void *>>()) {}

OpaqueArgEncoding::OpaqueArgEncoding(std::function<bool(Value)> match,
                                     TypeID type_id)
    : match_(std::move(match)), type_id_(type_id) {}

LogicalResult OpaqueArgEncoding::Match(Value value, Value converted) const {
  if (auto ptr = converted.getType().dyn_cast<LLVM::LLVMPointerType>())
    return success(match_(value));
  return failure();
}

FailureOr<EncodedArg> OpaqueArgEncoding::Encode(Globals &g,
                                                ImplicitLocOpBuilder &b,
                                                Value value,
                                                Value converted) const {
  Encoded encoded;
  encoded.type_id = PackTypeId(g, b, type_id_);
  encoded.value = PackValue(b, converted);
  return encoded;
}

//===----------------------------------------------------------------------===//

static LLVM::LLVMStructType GetEncodeMemRefType(ImplicitLocOpBuilder &b,
                                                MemRefType memref_ty) {
  MLIRContext *ctx = b.getContext();

  // Encode sizes together with strides as a single array.
  int64_t sizes_and_strides_size = 2 * memref_ty.getRank();

  // Encoded memref type: !llvm.struct<(i8, i8, ptr<i8>, array<... x i64>)>.
  Type i8 = b.getI8Type();
  Type ptr = LLVM::LLVMPointerType::get(ctx);
  Type arr = LLVM::LLVMArrayType::get(b.getI64Type(), sizes_and_strides_size);
  return LLVM::LLVMStructType::getLiteral(ctx, {i8, i8, ptr, arr});
}

// Encodes memref as LLVM struct value:
//
//   { i8: dtype, i8: rank, ptr<i8>: data,
//     array<2*rank x i64>: sizes_and_strides }
//
// This is a type erased version of the MLIR memref descriptor without base
// pointer. We pack sizes and strides as a single array member, so that on
// the runtime side we can read it back using C flexible array member.
// If the descriptor value is null, we only encode statically known info: dtype,
// rank, and dims, otherwise we also encode dynamic info
static Value EncodeMemRef(ImplicitLocOpBuilder &b, MemRefType memref_ty,
                          Value descriptor) {
  Location loc = b.getLoc();

  auto type = GetEncodeMemRefType(b, memref_ty);

  // Helper to unpack MLIR strided memref descriptor value.
  std::optional<MemRefDescriptor> desc = std::nullopt;
  if (descriptor) {
    desc = MemRefDescriptor(descriptor);
  }

  PrimitiveType element_dtype = ScalarPrimitiveType(memref_ty.getElementType());

  // Create values for filling encoded memref struct.
  Value dtype = b.create<ConstantOp>(
      b.getI8IntegerAttr(static_cast<uint8_t>(element_dtype)));
  Value rank = b.create<ConstantOp>(b.getI8IntegerAttr(memref_ty.getRank()));

  auto i64 = [&](int64_t i) { return b.getI64IntegerAttr(i); };

  // Get the statically known strides and offset from the memref type.
  llvm::SmallVector<int64_t> strides;
  int64_t memref_offset;
  if (failed(getStridesAndOffset(memref_ty, strides, memref_offset)))
    strides.resize(memref_ty.getRank(), ShapedType::kDynamicStrideOrOffset);

  // Build encoded memref sizes + strides: !llvm.array<... x i64>
  Value payload = b.create<LLVM::UndefOp>(type.getBody()[3]);
  for (unsigned i = 0; i < memref_ty.getRank(); ++i) {
    int64_t dim_size = memref_ty.getDimSize(i);
    int64_t stride_size = strides[i];

    Value dim = ShapedType::isDynamic(dim_size) && desc.has_value()
                    ? desc->size(b, loc, i)
                    : b.create<ConstantOp>(i64(dim_size));

    Value stride =
        ShapedType::isDynamicStrideOrOffset(stride_size) && desc.has_value()
            ? desc->stride(b, loc, i)
            : b.create<ConstantOp>(i64(stride_size));

    auto stride_pos = memref_ty.getRank() + i;

    payload = b.create<LLVM::InsertValueOp>(payload, dim, i);
    payload = b.create<LLVM::InsertValueOp>(payload, stride, stride_pos);
  }

  // Construct encoded memref value.
  Value memref = b.create<LLVM::UndefOp>(type);
  memref = b.create<LLVM::InsertValueOp>(memref, dtype, 0);
  memref = b.create<LLVM::InsertValueOp>(memref, rank, 1);
  memref = b.create<LLVM::InsertValueOp>(memref, payload, 3);

  // Previous values almost always are known at compile time, and inserting
  // dynamic values into the struct after all statically know values leads to a
  // better canonicalization and cleaner final LLVM IR.
  if (desc.has_value()) {
    auto ptr = LLVM::LLVMPointerType::get(b.getContext());
    Value data = b.create<LLVM::BitcastOp>(ptr, desc->alignedPtr(b, loc));
    memref = b.create<LLVM::InsertValueOp>(memref, data, 2);
  }

  return memref;
}

LogicalResult MemrefArgEncoding::Match(Value value, Value converted) const {
  return success(value.getType().isa<MemRefType>());
}

FailureOr<EncodedArg> MemrefArgEncoding::Encode(Globals &g,
                                                ImplicitLocOpBuilder &b,
                                                Value value,
                                                Value converted) const {
  auto memref_type = value.getType().cast<MemRefType>();

  // If memref has non-identity layout we use `StridedMemrefView` to
  // distinguish it from the default row-major memref.
  auto type_id = memref_type.getLayout().isIdentity()
                     ? TypeID::get<Tagged<MemrefView>>()
                     : TypeID::get<Tagged<StridedMemrefView>>();

  Encoded encoded;
  encoded.type_id = PackTypeId(g, b, type_id);
  encoded.value = PackValue(b, EncodeMemRef(b, memref_type, converted));

  return encoded;
}

//===----------------------------------------------------------------------===//
// Custom call results encodings.
//===----------------------------------------------------------------------===//

LogicalResult ScalarRetEncoding::Match(Type type, Type converted) const {
  return success(IsSupportedScalarType(type));
}

FailureOr<EncodedRet> ScalarRetEncoding::Encode(Globals &g,
                                                ImplicitLocOpBuilder &b,
                                                Type type,
                                                Type converted) const {
  Encoded encoded;
  encoded.type_id = PackTypeId(g, b, ScalarRuntimeTypeId(converted));

  Type ptr = LLVM::LLVMPointerType::get(b.getContext());
  Value one = b.create<ConstantOp>(b.getI32IntegerAttr(1));
  encoded.value = b.create<LLVM::AllocaOp>(ptr, converted, one, 0);

  return encoded;
}

FailureOr<Value> ScalarRetEncoding::Decode(ImplicitLocOpBuilder &b, Type type,
                                           Type converted,
                                           LLVM::AllocaOp alloca) const {
  return Value{b.create<LLVM::LoadOp>(converted, alloca)};
}

//===----------------------------------------------------------------------===//

static bool IsOpaqueType(Type type) { return type.isa<OpaqueType>(); }

OpaqueRetEncoding::OpaqueRetEncoding()
    : OpaqueRetEncoding(IsOpaqueType, TypeID::get<Tagged<void *>>()) {}

OpaqueRetEncoding::OpaqueRetEncoding(std::function<bool(Type)> match,
                                     TypeID type_id)
    : match_(std::move(match)), type_id_(type_id) {}

LogicalResult OpaqueRetEncoding::Match(Type type, Type converted) const {
  if (auto ptr = converted.dyn_cast<LLVM::LLVMPointerType>())
    return success(match_(type));
  return failure();
}

FailureOr<EncodedRet> OpaqueRetEncoding::Encode(Globals &g,
                                                ImplicitLocOpBuilder &b,
                                                Type value,
                                                Type converted) const {
  Encoded encoded;
  encoded.type_id = PackTypeId(g, b, type_id_);

  Type ptr = LLVM::LLVMPointerType::get(b.getContext());
  Value one = b.create<ConstantOp>(b.getI32IntegerAttr(1));
  encoded.value = b.create<LLVM::AllocaOp>(ptr, converted, one, 0);

  return encoded;
}

FailureOr<Value> OpaqueRetEncoding::Decode(ImplicitLocOpBuilder &b, Type type,
                                           Type converted,
                                           LLVM::AllocaOp alloca) const {
  return Value{b.create<LLVM::LoadOp>(converted, alloca)};
}

//===----------------------------------------------------------------------===//

LogicalResult MemrefRetEncoding::Match(Type type, Type converted) const {
  return success(type.isa<MemRefType>() &&
                 converted.isa<LLVM::LLVMStructType>());
}

FailureOr<EncodedRet> MemrefRetEncoding::Encode(Globals &g,
                                                ImplicitLocOpBuilder &b,
                                                Type type,
                                                Type converted) const {
  auto memref_ty = type.cast<MemRefType>();

  // We assume custom calls can only return row-major memrefs, may need to add
  // PermutedMemref support in the future.
  auto type_id = TypeID::get<Tagged<MemrefView>>();

  Encoded encoded;
  encoded.type_id = PackTypeId(g, b, type_id);
  // No memref descriptor for result, we only encode compile time known info:
  // dtype, rank, dims
  encoded.value =
      PackValue(b, EncodeMemRef(b, memref_ty, /*descriptor=*/nullptr));

  return encoded;
}

// Convert EncodedMemRef back to llvm MemRef descriptor, e.g.,
//   !llvm.struct<(i8, i8, ptr, array<2 x i64>)>
//     --->>> (note that memref descriptor still uses typed LLVM pointers)
//   !llvm.struct<(ptr<f32>, ptr<f32>, i64, array<1 x i64>, array<1 x i64>)>
FailureOr<Value> MemrefRetEncoding::Decode(ImplicitLocOpBuilder &b, Type type,
                                           Type converted,
                                           LLVM::AllocaOp alloca) const {
  Location loc = b.getLoc();
  auto memref_type = cast<MemRefType>(type);
  auto memref_desc = MemRefDescriptor::undef(b, loc, converted);

  // TODO(ezhulenev): Add support for returning dynamically shaped memrefs.
  if (!memref_type.hasStaticShape()) return failure();

  Type ptr = LLVM::LLVMPointerType::get(b.getContext());
  LLVM::LLVMStructType encoded = GetEncodeMemRefType(b, memref_type);

  Value c0 = b.create<ConstantOp>(b.getI64IntegerAttr(0));
  Value c2 = b.create<ConstantOp>(b.getI64IntegerAttr(2));

  // Fill memref descriptor pointers and offset.
  Value gep = b.create<LLVM::GEPOp>(ptr, encoded, alloca, ValueRange({c0, c2}));
  Value data_ptr = b.create<LLVM::BitcastOp>(memref_desc.getElementPtrType(),
                                             b.create<LLVM::LoadOp>(ptr, gep));
  memref_desc.setAllocatedPtr(b, loc, data_ptr);
  memref_desc.setAlignedPtr(b, loc, data_ptr);
  memref_desc.setConstantOffset(b, loc, 0);

  // Get the statically known strides and offset from the memref type.
  SmallVector<int64_t> strides;
  int64_t memref_offset;
  if (failed(getStridesAndOffset(memref_type, strides, memref_offset))) {
    return failure();
  }

  // Fill memref descriptor dimensions and strides.
  for (unsigned i = 0; i < memref_type.getRank(); ++i) {
    memref_desc.setConstantSize(b, loc, i, memref_type.getDimSize(i));
    memref_desc.setConstantStride(b, loc, i, strides[i]);
  }

  auto casted =
      b.create<UnrealizedConversionCastOp>(memref_type, Value(memref_desc));
  return casted.getResult(0);
}

//===----------------------------------------------------------------------===//
// Default encodings for arguments, attributes, and results
//===----------------------------------------------------------------------===//

CustomCallAttrEncodingSet DefaultAttrEncodings() {
  CustomCallAttrEncodingSet encodings;
  encodings
      .Add<StringAttrEncoding, ScalarAttrEncoding, DenseElementsAttrEncoding,
           ArrayAttrEncoding, DenseArrayAttrEncoding, EmptyArrayAttrEncoding,
           SymbolRefAttrEncoding>();

  encodings.Add<AggregateAttrEncoding<HloTraceAttr, HloTrace>>(
      encodings, AggregateAttrDef<HloTraceAttr>()
                     .Add("hlo_op", &HloTraceAttr::getHloOp)
                     .Add("module", &HloTraceAttr::getModule)
                     .Add("program_id", &HloTraceAttr::getProgramId));

  return encodings;
}

CustomCallArgEncodingSet DefaultArgEncodings() {
  CustomCallArgEncodingSet encodings;
  encodings.Add<ScalarArgEncoding, OpaqueArgEncoding, MemrefArgEncoding>();
  return encodings;
}

CustomCallRetEncodingSet DefaultRetEncodings() {
  CustomCallRetEncodingSet encodings;
  encodings.Add<ScalarRetEncoding, OpaqueRetEncoding, MemrefRetEncoding>();
  return encodings;
}

}  // namespace runtime
}  // namespace xla
