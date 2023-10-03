// RUN: mlir-hlo-opt %s --gml-collapse-materialize-ops | \
// RUN: FileCheck %s --check-prefix="CHECK-COLLAPSE"

// RUN: mlir-hlo-opt %s --gml-collapse-materialize-ops=reverse=true | \
// RUN: FileCheck %s --check-prefix="CHECK-UNCOLLAPSE"

func.func @collapsed(%arg : tensor<?x?xf32>, %i : index, %j : index, %k : index,
    %m : index, %n : index, %a : index, %b : index) -> tensor<4x?xf32> {
  %0 = gml_st.space [1024, %m] : !gml_st.tile<1024x?>
  %1 = gml_st.tile %0 [%i, %j] [4, 128] [2, %a]
      : !gml_st.tile<1024x?> to !gml_st.tile<4x128>
  %2 = gml_st.tile %1 [0, %k] [4, %n] [1, %b]
      : !gml_st.tile<4x128> to !gml_st.tile<4x?>
  %3 = gml_st.materialize %arg[%2] : tensor<?x?xf32>[!gml_st.tile<4x?>]
    to tensor<4x?xf32>
  func.return %3 : tensor<4x?xf32>
}

// CHECK-COLLAPSE-LABEL: @collapsed
// CHECK-COLLAPSE-SAME:  %[[ARG:.*]]: tensor<?x?xf32>, %[[I:.*]]: index, %[[J:.*]]: index, %[[K:.*]]: index, %[[M:.*]]: index, %[[N:.*]]: index, %[[A:.*]]: index, %[[B:.*]]: index
// CHECK-COLLAPSE-DAG:   %[[SPACE:.*]] = gml_st.space [1024, %[[M]]]
// CHECK-COLLAPSE-DAG:   %[[TILE:.*]] = gml_st.tile %[[SPACE]] [%[[I]], %[[J]]] [4, 128] [2, %[[A]]]
// CHECK-COLLAPSE-DAG:   %[[TILE_:.*]] = gml_st.tile %[[TILE]] [0, %[[K]]] [4, %[[N]]] [1, %[[B]]]
// CHECK-COLLAPSE-DAG:   %[[MATERIALIZE:.*]] = gml_st.materialize %[[ARG]][%[[TILE_]]]
// CHECK-COLLAPSE:       return %[[MATERIALIZE]]

// CHECK-UNCOLLAPSE-LABEL: @collapsed
// CHECK-UNCOLLAPSE-SAME:  %[[ARG:.*]]: tensor<?x?xf32>, %[[I:.*]]: index, %[[J:.*]]: index, %[[K:.*]]: index, %[[M:.*]]: index, %[[N:.*]]: index, %[[A:.*]]: index, %[[B:.*]]: index
// CHECK-UNCOLLAPSE-DAG:   %[[SPACE:.*]] = gml_st.space [1024, %[[M]]]
// CHECK-UNCOLLAPSE-DAG:   %[[TILE:.*]] = gml_st.tile %[[SPACE]] [%[[I]], %[[J]]] [4, 128] [2, %[[A]]]
// CHECK-UNCOLLAPSE-DAG:   %[[SPACE:.*]] = gml_st.space [4, 128]
// CHECK-UNCOLLAPSE-DAG:   %[[TILE_:.*]] = gml_st.tile %[[SPACE]] [0, %[[K]]] [4, %[[N]]] [1, %[[B]]]
// CHECK-UNCOLLAPSE-DAG:   %[[MATERIALIZE:.*]] = gml_st.materialize %[[ARG]][%[[TILE]]]
// CHECK-UNCOLLAPSE-DAG:   %[[MATERIALIZE_:.*]] = gml_st.materialize %[[MATERIALIZE]][%[[TILE_]]]
// CHECK-UNCOLLAPSE:       return %[[MATERIALIZE_]]

// -----

func.func @uncollapsed(%arg: tensor<?x?xf32>, %i: index, %j: index, %k: index,
    %m: index, %n: index, %a: index, %b: index) -> tensor<4x?xf32> {
  %0 = gml_st.space [1024, %m] : !gml_st.tile<1024x?>
  %1 = gml_st.tile %0 [%i, %j] [4, 128] [2, %a]
      : !gml_st.tile<1024x?> to !gml_st.tile<4x128>
  %2 = gml_st.space [4, 128] : !gml_st.tile<4x128>
  %3 = gml_st.tile %2 [0, %k] [4, %n] [1, %b]
      : !gml_st.tile<4x128> to !gml_st.tile<4x?>
  %4 = gml_st.materialize %arg[%1] : tensor<?x?xf32>[!gml_st.tile<4x128>] to tensor<4x128xf32>
  %5 = gml_st.materialize %4[%3] : tensor<4x128xf32>[!gml_st.tile<4x?>] to tensor<4x?xf32>
  return %5 : tensor<4x?xf32>
}

// CHECK-COLLAPSE-LABEL: @uncollapsed
// CHECK-COLLAPSE-SAME:  %[[ARG:.*]]: tensor<?x?xf32>, %[[I:.*]]: index, %[[J:.*]]: index, %[[K:.*]]: index, %[[M:.*]]: index, %[[N:.*]]: index, %[[A:.*]]: index, %[[B:.*]]: index
// CHECK-COLLAPSE-DAG:   %[[SPACE:.*]] = gml_st.space [1024, %[[M]]]
// CHECK-COLLAPSE-DAG:   %[[TILE:.*]] = gml_st.tile %[[SPACE]] [%[[I]], %[[J]]] [4, 128] [2, %[[A]]]
// CHECK-COLLAPSE-DAG:   %[[TILE_:.*]] = gml_st.tile %[[TILE]] [0, %[[K]]] [4, %[[N]]] [1, %[[B]]]
// CHECK-COLLAPSE-DAG:   %[[MATERIALIZE:.*]] = gml_st.materialize %[[ARG]][%[[TILE_]]]
// CHECK-COLLAPSE:       return %[[MATERIALIZE]]

// CHECK-UNCOLLAPSE-LABEL: @uncollapsed
// CHECK-UNCOLLAPSE-SAME:  %[[ARG:.*]]: tensor<?x?xf32>, %[[I:.*]]: index, %[[J:.*]]: index, %[[K:.*]]: index, %[[M:.*]]: index, %[[N:.*]]: index, %[[A:.*]]: index, %[[B:.*]]: index
// CHECK-UNCOLLAPSE-DAG:   %[[SPACE:.*]] = gml_st.space [1024, %[[M]]]
// CHECK-UNCOLLAPSE-DAG:   %[[TILE:.*]] = gml_st.tile %[[SPACE]] [%[[I]], %[[J]]] [4, 128] [2, %[[A]]]
// CHECK-UNCOLLAPSE-DAG:   %[[SPACE_:.*]] = gml_st.space [4, 128]
// CHECK-UNCOLLAPSE-DAG:   %[[TILE_:.*]] = gml_st.tile %[[SPACE_]] [0, %[[K]]] [4, %[[N]]] [1, %[[B]]]
// CHECK-UNCOLLAPSE-DAG:   %[[MATERIALIZE:.*]] = gml_st.materialize %[[ARG]][%[[TILE]]]
// CHECK-UNCOLLAPSE-DAG:   %[[MATERIALIZE_:.*]] = gml_st.materialize %[[MATERIALIZE]][%[[TILE_]]]
// CHECK-UNCOLLAPSE:       return %[[MATERIALIZE_]]

// -----

func.func @deep_collapsed(%arg: tensor<?x?xf32>, %i: index, %j: index, %k: index, %m: index, %n: index, %a: index, %b: index) -> tensor<4x?xf32> {
  %0 = gml_st.space [1024, %m] : !gml_st.tile<1024x?>
  %1 = gml_st.tile %0 [%i, %j] [4, 128] [2, %a] : !gml_st.tile<1024x?> to !gml_st.tile<4x128>
  %2 = gml_st.tile %1 [0, %k] [4, %n] [1, %b] : !gml_st.tile<4x128> to !gml_st.tile<4x?>
  %3 = gml_st.tile %2 [0, 0] [4, %n] [1, %b] : !gml_st.tile<4x?> to !gml_st.tile<4x?>
  %4 = gml_st.tile %3 [0, 0] [4, %n] [1, %b] : !gml_st.tile<4x?> to !gml_st.tile<4x?>
  %5 = gml_st.tile %4 [0, 0] [4, %n] [1, %b] : !gml_st.tile<4x?> to !gml_st.tile<4x?>
  %6 = gml_st.tile %5 [0, 0] [4, %n] [1, %b] : !gml_st.tile<4x?> to !gml_st.tile<4x?>
  %7 = gml_st.tile %6 [0, 0] [4, %n] [1, %b] : !gml_st.tile<4x?> to !gml_st.tile<4x?>
  %8 = gml_st.materialize %arg[%7] : tensor<?x?xf32>[!gml_st.tile<4x?>] to tensor<4x?xf32>
  return %8 : tensor<4x?xf32>
}

// CHECK-COLLAPSE-LABEL: @deep_collapsed
// CHECK-COLLAPSE:       gml_st.materialize
// CHECK-COLLAPSE-NOT:   gml_st.materialize
// CHECK-COLLAPSE:       return

// CHECK-UNCOLLAPSE-LABEL: @deep_collapsed
// CHECK-UNCOLLAPSE-SAME:  %[[ARG:.*]]: tensor<?x?xf32>
// CHECK-UNCOLLAPSE-DAG:   %[[M0:.*]] = gml_st.materialize %[[ARG]][%{{.*}}]
// CHECK-UNCOLLAPSE-DAG:   %[[M1:.*]] = gml_st.materialize %[[M0]][%{{.*}}]
// CHECK-UNCOLLAPSE-DAG:   %[[M2:.*]] = gml_st.materialize %[[M1]][%{{.*}}]
// CHECK-UNCOLLAPSE-DAG:   %[[M3:.*]] = gml_st.materialize %[[M2]][%{{.*}}]
// CHECK-UNCOLLAPSE-DAG:   %[[M4:.*]] = gml_st.materialize %[[M3]][%{{.*}}]
// CHECK-UNCOLLAPSE-DAG:   %[[M5:.*]] = gml_st.materialize %[[M4]][%{{.*}}]
// CHECK-UNCOLLAPSE-DAG:   %[[M6:.*]] = gml_st.materialize %[[M5]][%{{.*}}]
// CHECK-UNCOLLAPSE:       return %[[M6]]

// -----

func.func @deep_uncollapsed(%arg: tensor<?x?xf32>, %i: index, %j: index, %k: index, %m: index, %n: index, %a: index, %b: index) -> tensor<4x?xf32> {
  %0 = gml_st.space [1024, %m] : !gml_st.tile<1024x?>
  %1 = gml_st.tile %0 [%i, %j] [4, 128] [2, %a] : !gml_st.tile<1024x?> to !gml_st.tile<4x128>
  %2 = gml_st.space [4, %n] : !gml_st.tile<4x?>
  %3 = gml_st.tile %2 [0, 0] [4, %n] [1, %b] : !gml_st.tile<4x?> to !gml_st.tile<4x?>
  %4 = gml_st.space [4, %n] : !gml_st.tile<4x?>
  %5 = gml_st.tile %4 [0, 0] [4, %n] [1, %b] : !gml_st.tile<4x?> to !gml_st.tile<4x?>
  %6 = gml_st.space [4, %n] : !gml_st.tile<4x?>
  %7 = gml_st.tile %6 [0, 0] [4, %n] [1, %b] : !gml_st.tile<4x?> to !gml_st.tile<4x?>
  %8 = gml_st.space [4, %n] : !gml_st.tile<4x?>
  %9 = gml_st.tile %8 [0, 0] [4, %n] [1, %b] : !gml_st.tile<4x?> to !gml_st.tile<4x?>
  %10 = gml_st.space [4, %n] : !gml_st.tile<4x?>
  %11 = gml_st.tile %10 [0, 0] [4, %n] [1, %b] : !gml_st.tile<4x?> to !gml_st.tile<4x?>
  %12 = gml_st.space [4, 128] : !gml_st.tile<4x128>
  %13 = gml_st.tile %12 [0, %k] [4, %n] [1, %b] : !gml_st.tile<4x128> to !gml_st.tile<4x?>
  %14 = gml_st.materialize %arg[%1] : tensor<?x?xf32>[!gml_st.tile<4x128>] to tensor<4x128xf32>
  %15 = gml_st.materialize %14[%13] : tensor<4x128xf32>[!gml_st.tile<4x?>] to tensor<4x?xf32>
  %16 = gml_st.materialize %15[%11] : tensor<4x?xf32>[!gml_st.tile<4x?>] to tensor<4x?xf32>
  %17 = gml_st.materialize %16[%9] : tensor<4x?xf32>[!gml_st.tile<4x?>] to tensor<4x?xf32>
  %18 = gml_st.materialize %17[%7] : tensor<4x?xf32>[!gml_st.tile<4x?>] to tensor<4x?xf32>
  %19 = gml_st.materialize %18[%5] : tensor<4x?xf32>[!gml_st.tile<4x?>] to tensor<4x?xf32>
  %20 = gml_st.materialize %19[%3] : tensor<4x?xf32>[!gml_st.tile<4x?>] to tensor<4x?xf32>
  return %20 : tensor<4x?xf32>
}

// CHECK-COLLAPSE-LABEL: @deep_uncollapsed
// CHECK-COLLAPSE:       gml_st.materialize
// CHECK-COLLAPSE-NOT:   gml_st.materialize
// CHECK-COLLAPSE:       return

// CHECK-UNCOLLAPSE-LABEL: @deep_uncollapsed
// CHECK-UNCOLLAPSE-SAME:  %[[ARG:.*]]: tensor<?x?xf32>
// CHECK-UNCOLLAPSE-DAG:   %[[M0:.*]] = gml_st.materialize %[[ARG]][%{{.*}}]
// CHECK-UNCOLLAPSE-DAG:   %[[M1:.*]] = gml_st.materialize %[[M0]][%{{.*}}]
// CHECK-UNCOLLAPSE-DAG:   %[[M2:.*]] = gml_st.materialize %[[M1]][%{{.*}}]
// CHECK-UNCOLLAPSE-DAG:   %[[M3:.*]] = gml_st.materialize %[[M2]][%{{.*}}]
// CHECK-UNCOLLAPSE-DAG:   %[[M4:.*]] = gml_st.materialize %[[M3]][%{{.*}}]
// CHECK-UNCOLLAPSE-DAG:   %[[M5:.*]] = gml_st.materialize %[[M4]][%{{.*}}]
// CHECK-UNCOLLAPSE-DAG:   %[[M6:.*]] = gml_st.materialize %[[M5]][%{{.*}}]
// CHECK-UNCOLLAPSE:       return %[[M6]]
