// RUN: mlir-hlo-opt %s -split-input-file -verify-diagnostics


func.func @materialize_rank_mismatch(%tensor: tensor<?x?xf32>,
                                     %tile: !gml_st.tile<4>) {
  // expected-error @+1 {{expected source rank = 2 to match tile rank = 1}}
  %0 = gml_st.materialize %tensor[%tile]
     : tensor<?x?xf32>[!gml_st.tile<4>] to tensor<4xf32>
}

// -----

func.func @materialize_inferred_type_mismatch(%tensor: tensor<?x?xf32>,
                                              %tile: !gml_st.tile<?x4>) {
  // expected-error @+1 {{expected result type = 'tensor<4x?xf32>' to match the inferred type = 'tensor<?x4xf32>}}
  %0 = gml_st.materialize %tensor[%tile]
     : tensor<?x?xf32>[!gml_st.tile<?x4>] to tensor<4x?xf32>
}

// -----

func.func @materialize_scalar_with_dynamic_tile(
    %tensor: tensor<?x?xf32>, %tile: !gml_st.tile<?x2>) {
  // expected-error @+1 {{expected tile type '!gml_st.tile<?x2>' to have a single element shape}}
  %0 = gml_st.materialize %tensor[%tile]
     : tensor<?x?xf32>[!gml_st.tile<?x2>] to f32
}

// -----

func.func @materialize_scalar_with_nonsingle_element_tile(
    %tensor: tensor<?x?xf32>, %tile: !gml_st.tile<1x2>) {
  // expected-error @+1 {{expected tile type '!gml_st.tile<1x2>' to have a single element shape}}
  %0 = gml_st.materialize %tensor[%tile]
     : tensor<?x?xf32>[!gml_st.tile<1x2>] to f32
}

// -----

func.func @materialize_scalar_element_type_mismatch(
    %tensor: tensor<?x?xf32>, %tile: !gml_st.tile<1x1>) {
  // expected-error @+1 {{expected the result type 'i32' to match source element type 'f32'}}
  %0 = gml_st.materialize %tensor[%tile]
     : tensor<?x?xf32>[!gml_st.tile<1x1>] to i32
}

// -----

#map0 = affine_map<(d0) -> (24, -d0 + 192)>
#map1 = affine_map<(d0, d1)[s0] -> (d0 * 192 + s0 + d1)>
#map2 = affine_map<(d0) -> (16, -d0 + 192)>

func.func private @foo(%A: memref<192x192xf32>, %B: memref<192x192xf32>,
                  %C: memref<192x192xf32>) -> ()

func.func @loop_incorrent_num_yield_operands(%A: memref<192x192xf32>,
    %B: memref<192x192xf32>, %C: memref<192x192xf32>,
    %C_tensor: tensor<192x192xf32>) {
  %c24 = arith.constant 24 : index
  %c0 = arith.constant 0 : index
  %c192 = arith.constant 192 : index
  %0 = gml_st.loop (%i, %j) = (%c0, %c0) to (%c192, %c192)
      step (%c24, %c24)
      ins (%A_ = %A: memref<192x192xf32>, %B_ = %B: memref<192x192xf32>)
      outs (%CT_ = %C_tensor: tensor<192x192xf32>,
            %C_ = %C: memref<192x192xf32>) {
        func.call @foo(%A_, %B_, %C_)
          : (memref<192x192xf32>, memref<192x192xf32>, memref<192x192xf32>)-> ()
    // expected-error @+1 {{expected number of tensor output args = 1 to match the number of yield operands = 0}}
    gml_st.yield
  }
  func.return
}

// -----

#map0 = affine_map<(d0) -> (24, -d0 + 192)>
#map1 = affine_map<(d0, d1)[s0] -> (d0 * 192 + s0 + d1)>
#map2 = affine_map<(d0) -> (16, -d0 + 192)>

func.func private @foo(%A: memref<192x192xf32>, %B: memref<192x192xf32>,
                  %C: memref<192x192xf32>) -> tensor<f32>

func.func @loop_incorrent_yield_operand_type(%A: memref<192x192xf32>,
    %B: memref<192x192xf32>, %C: memref<192x192xf32>,
    %C_tensor: tensor<192x192xf32>) {
  %c24 = arith.constant 24 : index
  %c0 = arith.constant 0 : index
  %c192 = arith.constant 192 : index
  %0 = gml_st.loop (%i, %j) = (%c0, %c0) to (%c192, %c192)
      step (%c24, %c24)
      ins (%A_ = %A: memref<192x192xf32>, %B_ = %B: memref<192x192xf32>)
      outs (%CT_ = %C_tensor: tensor<192x192xf32>,
            %C_ = %C: memref<192x192xf32>) {
        %1 = func.call @foo(%A_, %B_, %C_)
          : (memref<192x192xf32>, memref<192x192xf32>, memref<192x192xf32>)-> tensor<f32>
    // expected-error @+1 {{expected yield operand 0 with type = 'tensor<f32>' to match output arg type = 'tensor<192x192xf32>}}
    gml_st.yield %1 : tensor<f32>
  }
  func.return
}

// -----

func.func private @foo(%A: memref<192x192xf32>, %B: memref<192x192xf32>,
                  %C: memref<192x192xf32>) -> ()

func.func @loop_incorrent_iterator_types_count(%A: memref<192x192xf32>,
    %B: memref<192x192xf32>, %C: memref<192x192xf32>,
    %C_tensor: tensor<192x192xf32>) {
  %c24 = arith.constant 24 : index
  %c0 = arith.constant 0 : index
  %c192 = arith.constant 192 : index
  // expected-error @+1 {{expected iterator types array attribute size = 1 to match the number of loops = 2}}
  %0 = "gml_st.loop"(%c0, %c0, %c192, %c192, %c24, %c24, %A, %B, %C_tensor, %C) ({
    ^bb0(%arg4: index, %arg5: index, %A_: memref<192x192xf32>,
         %B_: memref<192x192xf32>, %CT_: tensor<192x192xf32>,
         %C_: memref<192x192xf32>):
      func.call @foo(%A_, %B_, %C_)
          : (memref<192x192xf32>, memref<192x192xf32>, memref<192x192xf32>)-> ()
      gml_st.yield %CT_ : tensor<192x192xf32>
    }) {
      iterator_types = [#gml_st.iterator_type<parallel>],
      operand_segment_sizes = array<i32: 2, 2, 2, 2, 2>
    } : (index, index, index, index, index, index, memref<192x192xf32>,
      memref<192x192xf32>, tensor<192x192xf32>, memref<192x192xf32>
    ) -> tensor<192x192xf32>
  func.return
}

// -----

func.func private @foo(%A: memref<100xf32>) -> ()

func.func @loop_incorrent_block_arg_type(%A: memref<192xf32>) {
  %c0 = arith.constant 0 : index
  %c192 = arith.constant 192 : index
  %c24 = arith.constant 24 : index
  // expected-error @+1 {{expected output arg 0 with type = 'memref<192xf32>' to match region arg 1 type = 'memref<100xf32>'}}
  "gml_st.loop"(%c0, %c192, %c24, %A) ({
    ^bb0(%arg4: index, %A_: memref<100xf32>):
      func.call @foo(%A_) : (memref<100xf32>)-> ()
      gml_st.yield
    }) {
      iterator_types = [#gml_st.iterator_type<parallel>],
      operand_segment_sizes = array<i32: 1, 1, 1, 0, 1>
    } : (index, index, index, memref<192xf32>) -> ()
  func.return
}

// -----

func.func @space_op_different_rank() -> !gml_st.tile<32x32> {
  // expected-error@+1 {{expected 2 size values}}
  %0 = gml_st.space [64] : !gml_st.tile<32x32>
  func.return %0 : !gml_st.tile<32x32>
}

// -----

func.func @space_op_dynamic_static_mismatch(%size : index) -> !gml_st.tile<64x32> {
  // expected-error@+1 {{'gml_st.space' op inferred type(s) '!gml_st.tile<64x?>' are incompatible with return type(s) of operation '!gml_st.tile<64x32>'}}
  %0 = gml_st.space [64, %size] : !gml_st.tile<64x32>
  func.return %0 : !gml_st.tile<64x32>
}

// -----

func.func @space_op_mismatch_shapes_and_static_shapes() -> !gml_st.tile<5x?> {
  // expected-error@+1 {{expected 1 dynamic size values}}
  %0 = "gml_st.space"() {static_sizes = [5, -1]} : () -> !gml_st.tile<5x?>
  func.return %0 : !gml_st.tile<5x?>
}

// -----

func.func @tile_op_mismatch_sizes_and_static_sizes(%i: index) {
  %0 = gml_st.space [64, 32] : !gml_st.tile<64x32>
  // expected-error@+1 {{expected 0 dynamic size values}}
  %1 = "gml_st.tile"(%0, %i) { static_offsets = [0, 0], static_sizes = [1, 1], static_strides = [1, 1], operand_segment_sizes = array<i32: 1, 0, 1, 0> } : (!gml_st.tile<64x32>, index) -> !gml_st.tile<?x?>
  func.return
}

// -----

func.func @tile_op_mismatch_offsets_and_static_offsets(%i: index) -> !gml_st.tile<8x8> {
  %0 = gml_st.space [64, 32] : !gml_st.tile<64x32>
  // expected-error@+1 {{expected 0 dynamic offset values}}
  %1 = "gml_st.tile"(%0, %i) {static_offsets = [0, 0], static_sizes = [8, 8], static_strides = [1, 1], operand_segment_sizes = array<i32: 1, 1, 0, 0>} : (!gml_st.tile<64x32>, index) -> !gml_st.tile<8x8>
  func.return %1 : !gml_st.tile<8x8>
}

// -----

func.func @tile_op_mismatch_strides_and_static_strides(%i: index)  -> !gml_st.tile<8x8> {
  %0 = gml_st.space [64, 32] : !gml_st.tile<64x32>
  // expected-error@+1 {{expected 0 dynamic stride values}}
  %1 = "gml_st.tile"(%0, %i) {static_offsets = [0, 0], static_sizes = [8, 8], static_strides = [1, 1], operand_segment_sizes = array<i32: 1, 0, 0, 1>} : (!gml_st.tile<64x32>, index) -> !gml_st.tile<8x8>
  func.return %1 : !gml_st.tile<8x8>
}

// -----

func.func @tile_op_negative_static_size(%i: index)  -> !gml_st.tile<?x?> {
  %0 = gml_st.space [64, 32] : !gml_st.tile<64x32>
  // expected-error@+1 {{'gml_st.tile' op expected size = -2 to be non-negative}}
  %1 = "gml_st.tile"(%0, %i) {static_offsets = [0, 0], static_sizes = [-1, -2], static_strides = [1, 1], operand_segment_sizes = array<i32: 1, 0, 1, 0>} : (!gml_st.tile<64x32>, index) -> !gml_st.tile<?x?>
  func.return %1 : !gml_st.tile<?x?>
}

// -----

func.func @tile_op_negative_static_stride(%i: index)  -> !gml_st.tile<?x8> {
  %0 = gml_st.space [64, 32] : !gml_st.tile<64x32>
  // expected-error@+1 {{'gml_st.tile' op expected stride = -2 to be non-negative}}
  %1 = "gml_st.tile"(%0, %i) {static_offsets = [0, 0], static_sizes = [-1, 8], static_strides = [1, -2], operand_segment_sizes = array<i32: 1, 0, 1, 0>} : (!gml_st.tile<64x32>, index) -> !gml_st.tile<?x8>
  func.return %1 : !gml_st.tile<?x8>
}

// -----

func.func @tile_op_negative_static_offset(%i: index)  -> !gml_st.tile<?x8> {
  %0 = gml_st.space [64, 32] : !gml_st.tile<64x32>
  // expected-error@+1 {{'gml_st.tile' op expected offset = -2 to be non-negative}}
  %1 = "gml_st.tile"(%0, %i) {static_offsets = [0, -2], static_sizes = [-1, 8], static_strides = [1, 1], operand_segment_sizes = array<i32: 1, 0, 1, 0>} : (!gml_st.tile<64x32>, index) -> !gml_st.tile<?x8>
  func.return %1 : !gml_st.tile<?x8>
}

// -----

func.func @tile_op_offset_out_of_bounds(%i: index) -> !gml_st.tile<?x?> {
  %0 = gml_st.space [64, 32] : !gml_st.tile<64x32>
  // expected-error@+1 {{'gml_st.tile' op offset = 32 is out of bounds for argument dimension size = 32}}
  %1 = "gml_st.tile"(%0, %i, %i) {static_offsets = [0, 32], static_sizes = [-1, -1], static_strides = [1, 1], operand_segment_sizes = array<i32: 1, 0, 2, 0>} : (!gml_st.tile<64x32>, index, index) -> !gml_st.tile<?x?>
  func.return %1 : !gml_st.tile<?x?>
}

// -----

func.func @tile_op_offset_out_of_bounds_considering_size_and_stride() -> !gml_st.tile<8x8> {
  %0 = gml_st.space [64, 32] : !gml_st.tile<64x32>
  // expected-error@+1 {{'gml_st.tile' op offset = 25 size = 8 stride = 1 causes access out of bounds at 32 for argument dimension size = 32}}
  %1 = gml_st.tile %0 [56, 25] [8, 8] [1, 1] : !gml_st.tile<64x32> to !gml_st.tile<8x8>
  func.return %1 : !gml_st.tile<8x8>
}

// -----

func.func @tile_op_offset_out_of_bounds_considering_size_and_stride(%i: index) {
  %0 = gml_st.space [64, 32] : !gml_st.tile<64x32>
  // expected-error@+1 {{'gml_st.tile' op size = 9 stride = 4 causes access out of bounds for argument dimension size = 32}}
  %1 = gml_st.tile %0 [%i, %i] [8, 9] [8, 4] : !gml_st.tile<64x32> to !gml_st.tile<8x9>
  func.return %1 : !gml_st.tile<8x9>
}

// -----

func.func @for_loop_wrong_yield_target(
    %arg: tensor<8xf32>, %output: tensor<f32>) -> tensor<f32> {
  %c0 = arith.constant 0 : index
  %c4 = arith.constant 4 : index
  %c8 = arith.constant 8 : index
  %space = gml_st.space [8] : !gml_st.tile<8>
  %space_0 = gml_st.space [] : !gml_st.tile<>

  %sum = gml_st.for (%i) = (%c0) to (%c8) step (%c4)
      outs(%out_ = %output : tensor<f32>) {
    %tile = gml_st.tile %space [%i] [4] [1]
      : !gml_st.tile<8> to !gml_st.tile<4>
    %arg_sub = gml_st.materialize %arg[%tile]
      : tensor<8xf32>[!gml_st.tile<4>] to tensor<4xf32>
    %out_sub = gml_st.materialize %out_[%space_0]
      : tensor<f32>[!gml_st.tile<>] to tensor<f32>

    %result_sub = linalg.dot
        ins(%arg_sub, %arg_sub : tensor<4xf32>, tensor<4xf32>)
        outs(%out_sub : tensor<f32>) -> tensor<f32>

    // expected-error@+1 {{'gml_st.set_yield' op expected output block argument 0 to match set_yield destination}}
    gml_st.set_yield %result_sub into %output[%space_0]
      : tensor<f32> into tensor<f32>[!gml_st.tile<>]
  } : tensor<f32>
  func.return %sum : tensor<f32>
}

// -----

func.func @yield_with_accumulator_mismatched_type(
    %arg: tensor<8xf32>, %output: tensor<f32>) -> tensor<f32> {
  %c0 = arith.constant 0 : index
  %c4 = arith.constant 4 : index
  %c8 = arith.constant 8 : index

  %space_1d = gml_st.space [8] : !gml_st.tile<8>
  %space_0d = gml_st.space [] : !gml_st.tile<>

  %sum = gml_st.parallel (%i) = (%c0) to (%c8) step (%c4) {
    %tile = gml_st.tile %space_1d [%i] [4] [1]
      : !gml_st.tile<8> to !gml_st.tile<4>
    %arg_sub = gml_st.materialize %arg[%tile]
      : tensor<8xf32>[!gml_st.tile<4>] to tensor<4xf32>
    %out_sub = gml_st.materialize %output[%space_0d]
      : tensor<f32>[!gml_st.tile<>] to tensor<f32>

    %result_sub = linalg.dot
       ins(%arg_sub, %arg_sub : tensor<4xf32>, tensor<4xf32>)
       outs(%out_sub : tensor<f32>) -> tensor<f32>

    // expected-error@+1 {{'gml_st.set_yield' op expected accumulator region to have 2 arguments of type 'tensor<f32>'}}
    gml_st.set_yield %result_sub into %output[%space_0d]
      acc (%in, %out: memref<f32>) {
        gml_st.yield %in : memref<f32>
      }: tensor<f32> into tensor<f32>[!gml_st.tile<>]
  } : tensor<f32>
  func.return %sum : tensor<f32>
}
