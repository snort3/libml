// Test vectorization of gml_st.parallel and gml_st.for loops.
// RUN: mlir-hlo-opt %s --vectorize-gml-st-loops | \
// RUN: FileCheck %s

#map0 = affine_map<(d0, d1)[s0, s1] -> (d0 * s1 + s0 + d1)>
#map1 = affine_map<(d0, d1) -> (d0, d1)>

// CHECK-LABEL: @parallel_with_tiles(
func.func @parallel_with_tiles(
    %arg0: memref<?x?xf32>, %arg1: memref<?x?xf32>, %arg2: memref<?x?xf32>)
    -> memref<?x?xf32> {
  %c4 = arith.constant 4 : index
  %c1 = arith.constant 1 : index
  %c0 = arith.constant 0 : index
  %0 = memref.dim %arg0, %c0 : memref<?x?xf32>
  %1 = memref.dim %arg0, %c1 : memref<?x?xf32>
  gml_st.parallel (%arg3, %arg4) = (%c0, %c0) to (%0, %1) step (%c4, %c1) {
    %6 = memref.subview %arg2[%arg3, %arg4] [4, 1] [1, 1]
      : memref<?x?xf32> to memref<4x1xf32, #map0>
    %7 = memref.subview %arg1[%arg3, %arg4] [4, 1] [1, 1]
      : memref<?x?xf32> to memref<4x1xf32, #map0>
    %8 = memref.subview %arg0[%arg3, %arg4] [4, 1] [1, 1]
      : memref<?x?xf32> to memref<4x1xf32, #map0>
    linalg.generic {indexing_maps = [#map1, #map1, #map1],
                    iterator_types = ["parallel", "parallel"]}
                    ins(%8, %7 : memref<4x1xf32, #map0>, memref<4x1xf32, #map0>)
                    outs(%6 : memref<4x1xf32, #map0>) {
    ^bb0(%arg5: f32, %arg6: f32, %arg7: f32):
      %9 = arith.addf %arg5, %arg6 : f32
      linalg.yield %9 : f32
    }
    gml_st.set_yield
  }
  func.return %arg2 : memref<?x?xf32>
}
// CHECK-NOT: linalg.generic
// CHECK: %[[LHS:.*]] = vector.transfer_read {{%.*}}[%c0, %c0]
// CHECK: %[[RHS:.*]] = vector.transfer_read {{%.*}}[%c0, %c0]
// CHECK: %[[ADD:.*]] = arith.addf %[[LHS]], %[[RHS]] : vector<4x1xf32>
// CHECK: vector.transfer_write %[[ADD]], {{%.*}}[%c0, %c0]

// CHECK-LABEL: @for_with_tiles(
func.func @for_with_tiles(
    %arg0: memref<?x?xf32>, %arg1: memref<?x?xf32>, %arg2: memref<?x?xf32>)
    -> memref<?x?xf32> {
  %c4 = arith.constant 4 : index
  %c1 = arith.constant 1 : index
  %c0 = arith.constant 0 : index
  %0 = memref.dim %arg0, %c0 : memref<?x?xf32>
  %1 = memref.dim %arg0, %c1 : memref<?x?xf32>
  gml_st.for (%arg3, %arg4) = (%c0, %c0) to (%0, %1) step (%c4, %c1) {
    %6 = memref.subview %arg2[%arg3, %arg4] [4, 1] [1, 1]
      : memref<?x?xf32> to memref<4x1xf32, #map0>
    %7 = memref.subview %arg1[%arg3, %arg4] [4, 1] [1, 1]
      : memref<?x?xf32> to memref<4x1xf32, #map0>
    %8 = memref.subview %arg0[%arg3, %arg4] [4, 1] [1, 1]
      : memref<?x?xf32> to memref<4x1xf32, #map0>
    linalg.generic {indexing_maps = [#map1, #map1, #map1],
                    iterator_types = ["parallel", "parallel"]}
                    ins(%8, %7 : memref<4x1xf32, #map0>, memref<4x1xf32, #map0>)
                    outs(%6 : memref<4x1xf32, #map0>) {
    ^bb0(%arg5: f32, %arg6: f32, %arg7: f32):
      %9 = arith.addf %arg5, %arg6 : f32
      linalg.yield %9 : f32
    }
    gml_st.set_yield
  }
  func.return %arg2 : memref<?x?xf32>
}
// CHECK-NOT: linalg.generic
// CHECK: %[[LHS:.*]] = vector.transfer_read {{%.*}}[%c0, %c0]
// CHECK: %[[RHS:.*]] = vector.transfer_read {{%.*}}[%c0, %c0]
// CHECK: %[[ADD:.*]] = arith.addf %[[LHS]], %[[RHS]] : vector<4x1xf32>
// CHECK: vector.transfer_write %[[ADD]], {{%.*}}[%c0, %c0]

#map3 = affine_map<(d0) -> (d0)>

// CHECK-LABEL: @parallel_on_tensor(
func.func @parallel_on_tensor(
    %arg0: tensor<?xf32>, %arg1: tensor<?xf32>, %arg2: tensor<?xf32>)
    -> tensor<?xf32> {
  %c4 = arith.constant 4 : index
  %c1 = arith.constant 1 : index
  %c0 = arith.constant 0 : index
  %0 = tensor.dim %arg0, %c0 : tensor<?xf32>
  %space = gml_st.space [%0] : !gml_st.tile<?>
  %2 = gml_st.parallel (%i) = (%c0) to (%0) step (%c4) {
    %tile = gml_st.tile %space [%i] [4] [1] : !gml_st.tile<?> to !gml_st.tile<4>
    %6 = gml_st.materialize %arg0[%tile]
      : tensor<?xf32>[!gml_st.tile<4>] to tensor<4xf32>
    %7 = gml_st.materialize %arg1[%tile]
      : tensor<?xf32>[!gml_st.tile<4>] to tensor<4xf32>
    %8 = gml_st.materialize %arg2[%tile]
      : tensor<?xf32>[!gml_st.tile<4>] to tensor<4xf32>
    %9 = linalg.generic {indexing_maps = [#map3, #map3, #map3],
                        iterator_types = ["parallel"]}
                        ins(%6, %7 : tensor<4xf32>, tensor<4xf32>)
                        outs(%8 : tensor<4xf32>) {
    ^bb0(%arg5: f32, %arg6: f32, %arg7: f32):
      %10 = arith.addf %arg5, %arg6 : f32
      linalg.yield %10 : f32
    } -> tensor<4xf32>
    gml_st.set_yield %9 into %arg2[%tile]
      : tensor<4xf32> into tensor<?xf32>[!gml_st.tile<4>]
  } : tensor<?xf32>
  func.return %2 : tensor<?xf32>
}
// CHECK-NOT: linalg.generic
// CHECK: %[[LHS:.*]] = vector.transfer_read {{%.*}}[%c0]
// CHECK: %[[RHS:.*]] = vector.transfer_read {{%.*}}[%c0]
// CHECK: %[[ADD:.*]] = arith.addf %[[LHS]], %[[RHS]] : vector<4xf32>
// CHECK: vector.transfer_write %[[ADD]], {{%.*}}[%c0]
