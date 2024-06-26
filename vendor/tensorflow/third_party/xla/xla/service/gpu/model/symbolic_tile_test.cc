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

#include "xla/service/gpu/model/symbolic_tile.h"

#include <optional>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "xla/service/gpu/model/affine_map_printer.h"
#include "xla/service/gpu/model/indexing_analysis.h"
#include "xla/service/gpu/model/indexing_test_utils.h"
#include "tsl/platform/test.h"

namespace xla {
namespace gpu {
namespace {

using ::testing::ExplainMatchResult;
using ::testing::Optional;
using ::testing::StrEq;

MATCHER_P4(MatchSymbolicTileWithRtVars, offset_map_string, size_map_string,
           stride_map_string, rt_vars_string,
           absl::StrCat(negation
                            ? "equals "
                            : "doesn't equal symbolic tile with offset_map_ ",
                        offset_map_string, " and size_map_ ", size_map_string,
                        " and stride_map_ ", stride_map_string, "and rt_vars_ ",
                        rt_vars_string)) {
  AffineMapPrinter printer;
  return ExplainMatchResult(StrEq(offset_map_string),
                            printer.ToString(arg.offset_map()),
                            result_listener) &&
         ExplainMatchResult(StrEq(size_map_string),
                            printer.ToString(arg.size_map()),
                            result_listener) &&
         ExplainMatchResult(StrEq(stride_map_string),
                            printer.ToString(arg.stride_map()),
                            result_listener) &&
         // Strip whitespace, so we don't need to add trailing newlines.
         ExplainMatchResult(StrEq(absl::StripAsciiWhitespace(rt_vars_string)),
                            absl::StripAsciiWhitespace(arg.RtVarsToString()),
                            result_listener);
}

MATCHER_P3(MatchSymbolicTile, offset_map_string, size_map_string,
           stride_map_string,
           absl::StrCat(negation
                            ? "equals "
                            : "doesn't equal symbolic tile with offset_map_ ",
                        offset_map_string, " and size_map_ ", size_map_string,
                        " and stride_map_ ", stride_map_string)) {
  return ExplainMatchResult(
      MatchSymbolicTileWithRtVars(offset_map_string, size_map_string,
                                  stride_map_string, ""),
      arg, result_listener);
}

using SymbolicTileTest = IndexingTestBase;

TEST_F(SymbolicTileTest, CanPropagateTileFromDotOutputToInputs) {
  auto input_indexing = GetOutputToInputIndexing(ParseAndGetRoot(R"(
    HloModule m
    ENTRY e {
      p0 = f32[11, 17, 19] parameter(0)
      p1 = f32[11, 19, 23] parameter(1)
      ROOT dot = f32[11, 17, 23] dot(p0, p1),
        lhs_batch_dims={0}, rhs_batch_dims={0},
        lhs_contracting_dims={2}, rhs_contracting_dims={1}
    }
  )"));

  EXPECT_THAT(
      SymbolicTile::FromIndexingMap(*input_indexing.indexing_maps[0].begin()),
      Optional(MatchSymbolicTile("()[s0, s1, s2] -> (0, 0, 0)",
                                 "()[s0, s1, s2] -> (s0, s1, 19)",
                                 "()[s0, s1, s2] -> (1, 1, 1)")));

  EXPECT_THAT(
      SymbolicTile::FromIndexingMap(*input_indexing.indexing_maps[1].begin()),
      Optional(MatchSymbolicTile("()[s0, s1, s2] -> (0, 0, 0)",
                                 "()[s0, s1, s2] -> (s0, 19, s2)",
                                 "()[s0, s1, s2] -> (1, 1, 1)")));
}

TEST_F(SymbolicTileTest, CanPropagateTileThroughTrivialReshape) {
  auto input_indexing = GetOutputToInputIndexing(ParseAndGetRoot(R"(
    HloModule m
    ENTRY e {
      p0 = f32[11, 17, 19] parameter(0)
      ROOT reshape = f32[1, 11, 17, 19] reshape(p0)
    }
  )"));

  EXPECT_THAT(
      SymbolicTile::FromIndexingMap(*input_indexing.indexing_maps[0].begin()),
      Optional(MatchSymbolicTile("()[s0, s1, s2, s3] -> (0, 0, 0)",
                                 "()[s0, s1, s2, s3] -> (s1, s2, s3)",
                                 "()[s0, s1, s2, s3] -> (1, 1, 1)")));
}

TEST_F(SymbolicTileTest,
       CanPropagateTileThroughNonTrivialMergeReshapeFromOutputToInput) {
  auto input_indexing = GetOutputToInputIndexing(ParseAndGetRoot(R"(
    HloModule m
    ENTRY e {
      p0 = f32[1,8,6,4]{3,2,1,0} parameter(0)
      ROOT bitcast = f32[48,4]{1,0} bitcast(p0)
    }
  )"));

  EXPECT_THAT(
      SymbolicTile::FromIndexingMap(*input_indexing.indexing_maps[0].begin()),
      Optional(MatchSymbolicTile(
          "()[s0, s1] -> (0, 0, 0, 0)",
          "()[s0, s1] -> "
          "(1, (s0 + 5) floordiv 6, s0 - ((s0 - 1) floordiv 6) * 6, s1)",
          "()[s0, s1] -> (0, 1, 1, 1)")));
}

TEST_F(SymbolicTileTest, FailsToPropagateTileThroughNonTrivialReshape) {
  auto input_indexing = GetOutputToInputIndexing(ParseAndGetRoot(R"(
    HloModule m
    ENTRY e {
      p0 = f32[12, 4, 19] parameter(0)
      ROOT reshape = f32[4, 12, 19] reshape(p0)
    }
  )"));

  EXPECT_EQ(
      SymbolicTile::FromIndexingMap(*input_indexing.indexing_maps[0].begin()),
      std::nullopt);
}

TEST_F(SymbolicTileTest, CanPropagateTileThroughElementwiseOp) {
  auto input_indexing = GetOutputToInputIndexing(ParseAndGetRoot(R"(
    HloModule m
    ENTRY e {
      p0 = f32[150] parameter(0)
      p1 = f32[150] parameter(1)
      ROOT add = f32[150] add(p0, p1)
    }
  )"));

  EXPECT_THAT(
      SymbolicTile::FromIndexingMap(*input_indexing.indexing_maps[0].begin()),
      Optional(MatchSymbolicTile("()[s0] -> (0)", "()[s0] -> (s0)",
                                 "()[s0] -> (1)")));
}

TEST_F(SymbolicTileTest, CanPropagateTileFromBroadcastOutputToInput) {
  auto input_indexing = GetOutputToInputIndexing(ParseAndGetRoot(R"(
    HloModule m
    ENTRY e {
      p0 = f32[150] parameter(0)
      ROOT broadcast = f32[157,150] broadcast(p0), dimensions={1}
    }
  )"));

  EXPECT_THAT(
      SymbolicTile::FromIndexingMap(*input_indexing.indexing_maps[0].begin()),
      Optional(MatchSymbolicTile("()[s0, s1] -> (0)", "()[s0, s1] -> (s1)",
                                 "()[s0, s1] -> (1)")));
}

TEST_F(SymbolicTileTest, CanPropagateTileFromReduceOutputToInput) {
  auto input_indexing = GetOutputToInputIndexing(ParseAndGetRoot(R"(
    HloModule m
    max {
      p0 = f32[] parameter(0)
      p1 = f32[] parameter(1)
      ROOT max = f32[] maximum(p0, p1)
    }

    ENTRY e {
      p0 = f32[125,150] parameter(0)
      c0 = f32[] constant(-inf)
      ROOT reduce = f32[150] reduce(p0, c0), dimensions={0}, to_apply=max
    }
  )"));

  EXPECT_THAT(
      SymbolicTile::FromIndexingMap(*input_indexing.indexing_maps[0].begin()),
      Optional(MatchSymbolicTile("()[s0] -> (0, 0)", "()[s0] -> (125, s0)",
                                 "()[s0] -> (1, 1)")));
}

TEST_F(SymbolicTileTest, CanPropagateTileThroughReverse) {
  auto input_indexing = GetOutputToInputIndexing(ParseAndGetRoot(R"(
    HloModule m
    ENTRY e {
      p0 = f32[179] parameter(0)
      ROOT reverse = f32[179] reverse(p0), dimensions={0}
    }
  )"));

  EXPECT_THAT(
      SymbolicTile::FromIndexingMap(*input_indexing.indexing_maps[0].begin()),
      Optional(MatchSymbolicTile("()[s0] -> (-s0 + 179)", "()[s0] -> (s0)",
                                 "()[s0] -> (1)")));
}

TEST_F(SymbolicTileTest, CanPropagateTileFromSliceOutputToInput) {
  auto input_indexing = GetOutputToInputIndexing(ParseAndGetRoot(R"(
    HloModule m
    ENTRY e {
      p0 = f32[120,142] parameter(0)
      ROOT slice = f32[10,21] slice(p0), slice={[40:60:2], [20:104:4]}
    }
  )"));

  EXPECT_THAT(
      SymbolicTile::FromIndexingMap(*input_indexing.indexing_maps[0].begin()),
      Optional(MatchSymbolicTile("()[s0, s1] -> (40, 20)",
                                 "()[s0, s1] -> (s0, s1)",
                                 "()[s0, s1] -> (2, 4)")));
}

TEST_F(SymbolicTileTest, CanPropagateTileThroughTranspose) {
  auto input_indexing = GetOutputToInputIndexing(ParseAndGetRoot(R"(
    HloModule m
    ENTRY e {
      p0 = f32[21,10] parameter(0)
      ROOT transpose = f32[10,21] transpose(p0), dimensions={1,0}
    }
  )"));

  EXPECT_THAT(
      SymbolicTile::FromIndexingMap(*input_indexing.indexing_maps[0].begin()),
      Optional(MatchSymbolicTile("()[s0, s1] -> (0, 0)",
                                 "()[s0, s1] -> (s1, s0)",
                                 "()[s0, s1] -> (1, 1)")));
}

TEST_F(SymbolicTileTest, CanPropagateTileThroughConcatenate) {
  // TODO(b/325488844): Add additional concat test cases with constraints.
  auto input_indexing = GetOutputToInputIndexing(ParseAndGetRoot(R"(
    HloModule m
    ENTRY e {
      p0 = f32[2,5,7] parameter(0)
      p1 = f32[2,11,7] parameter(1)
      p2 = f32[2,17,7] parameter(2)
      ROOT concat = f32[2,33,7] concatenate(p0, p1, p2), dimensions={1}
    }
  )"));

  EXPECT_THAT(
      SymbolicTile::FromIndexingMap(*input_indexing.indexing_maps[0].begin()),
      Optional(MatchSymbolicTile("()[s0, s1, s2] -> (0, 0, 0)",
                                 "()[s0, s1, s2] -> (s0, s1, s2)",
                                 "()[s0, s1, s2] -> (1, 1, 1)")));
  EXPECT_THAT(
      SymbolicTile::FromIndexingMap(*input_indexing.indexing_maps[1].begin()),
      Optional(MatchSymbolicTile("()[s0, s1, s2] -> (0, -5, 0)",
                                 "()[s0, s1, s2] -> (s0, s1, s2)",
                                 "()[s0, s1, s2] -> (1, 1, 1)")));
  EXPECT_THAT(
      SymbolicTile::FromIndexingMap(*input_indexing.indexing_maps[2].begin()),
      Optional(MatchSymbolicTile("()[s0, s1, s2] -> (0, -16, 0)",
                                 "()[s0, s1, s2] -> (s0, s1, s2)",
                                 "()[s0, s1, s2] -> (1, 1, 1)")));
}

TEST_F(SymbolicTileTest, CanPropagateTileThroughPadOpWithoutInteriorPadding) {
  // TODO(b/325488844): Add pad tests with defined constraints on tile input.
  auto input_indexing = GetOutputToInputIndexing(ParseAndGetRoot(R"(
    HloModule m
    ENTRY e {
      p0 = f32[4, 4] parameter(0)
      p1 = f32[] parameter(1)
      ROOT pad = f32[8,8] pad(p0, p1), padding=2_2_0x1_3_0
    }
  )"));

  EXPECT_THAT(
      SymbolicTile::FromIndexingMap(*input_indexing.indexing_maps[0].begin()),
      Optional(MatchSymbolicTile("()[s0, s1] -> (-2, -1)",
                                 "()[s0, s1] -> (s0, s1)",
                                 "()[s0, s1] -> (1, 1)")));
}

TEST_F(SymbolicTileTest, CanPropagateTileThroughDynamicSlice) {
  auto input_indexing = GetOutputToInputIndexing(ParseAndGetRoot(R"(
    HloModule m
    ENTRY e {
      %src = s32[2,2,258] parameter(0)
      %of1 = s32[] parameter(1)
      %of2 = s32[] parameter(2)
      %of3 = s32[] parameter(3)
      ROOT %ds = s32[1,2,32] dynamic-slice(s32[2,2,258] %src,
        s32[] %of1, s32[] %of2, s32[] %of3),
        dynamic_slice_sizes={1, 2, 32}
    }
  )"));

  ASSERT_EQ(input_indexing.indexing_maps.size(), 4);

  EXPECT_THAT(
      SymbolicTile::FromIndexingMap(*input_indexing.indexing_maps[0].begin()),
      // s0, s1, s2: tile sizes
      // s3, s4: runtime parameters
      // Note: We don't have s0 in the size map's rhs, because the first dim
      // of the tile size can only be 1. The second offset is optimized to 0,
      // because that is the only possible value.
      Optional(MatchSymbolicTileWithRtVars(
          "()[s0, s1, s2, s3, s4] -> (s3, 0, s4)",
          "()[s0, s1, s2] -> (1, s1, s2)", "()[s0, s1, s2] -> (0, 1, 1)",
          R"(
s3 in [0, 1]
  hlo: %of1 = s32[] parameter(1)
  (d0, d1, d2) -> ()
s4 in [0, 226]
  hlo: %of3 = s32[] parameter(3)
  (d0, d1, d2) -> ()
)")));

  for (int i = 1; i <= 3; i++) {
    EXPECT_THAT(
        SymbolicTile::FromIndexingMap(*input_indexing.indexing_maps[i].begin()),
        Optional(MatchSymbolicTile("()[s0, s1, s2] -> ()",
                                   "()[s0, s1, s2] -> ()",
                                   "()[s0, s1, s2] -> ()")));
  }
}

TEST_F(SymbolicTileTest, CanPropagateTileThroughDynamicUpdateSlice) {
  auto input_indexing = GetOutputToInputIndexing(ParseAndGetRoot(R"(
    HloModule m
    ENTRY e {
      %src = s32[20,30] parameter(0)
      %upd = s32[5,10] parameter(1)
      %of1 = s32[] parameter(2)
      %of2 = s32[] parameter(3)
      ROOT %dus = s32[20,30] dynamic-update-slice(
          s32[20,30] %src, s32[5,10] %upd, s32[] %of1, s32[] %of2)
    }
  )"));

  ASSERT_EQ(input_indexing.indexing_maps.size(), 4);

  EXPECT_THAT(
      SymbolicTile::FromIndexingMap(*input_indexing.indexing_maps[0].begin()),
      // s0, s1: tile sizes
      // s2, s3: runtime parameters
      Optional(MatchSymbolicTile("()[s0, s1] -> (0, 0)",
                                 "()[s0, s1] -> (s0, s1)",
                                 "()[s0, s1] -> (1, 1)")));
  EXPECT_THAT(
      SymbolicTile::FromIndexingMap(*input_indexing.indexing_maps[1].begin()),
      // s0, s1: tile sizes
      // s2, s3: runtime parameters
      Optional(MatchSymbolicTileWithRtVars("()[s0, s1, s2, s3] -> (-s2, -s3)",
                                           "()[s0, s1] -> (s0, s1)",
                                           "()[s0, s1] -> (1, 1)",
                                           R"(
s2 in [0, 15]
  hlo: %of1 = s32[] parameter(2)
  (d0, d1) -> ()
s3 in [0, 20]
  hlo: %of2 = s32[] parameter(3)
  (d0, d1) -> ()
)")));
  for (int i = 2; i <= 3; i++) {
    EXPECT_THAT(
        SymbolicTile::FromIndexingMap(*input_indexing.indexing_maps[i].begin()),
        Optional(MatchSymbolicTile("()[s0, s1] -> ()", "()[s0, s1] -> ()",
                                   "()[s0, s1] -> ()")));
  }
}

TEST_F(SymbolicTileTest, CanPropagateTileThroughGather) {
  auto input_indexing = GetOutputToInputIndexing(ParseAndGetRoot(R"(
    HloModule m
    ENTRY main {
      operand = f32[33,76,70] parameter(0)
      indices = s32[1806,2] parameter(1)
      ROOT r = f32[1806,7,8,4] gather(operand, indices), offset_dims={1,2,3},
                                 collapsed_slice_dims={}, start_index_map={0,1},
                                 index_vector_dim=1, slice_sizes={7,8,4}
    }
  )"));

  ASSERT_EQ(input_indexing.indexing_maps.size(), 2);

  EXPECT_THAT(
      SymbolicTile::FromIndexingMap(*input_indexing.indexing_maps[0].begin()),
      // s0, s1, s2, s3: tile sizes
      // s4, s5: runtime parameters
      Optional(MatchSymbolicTileWithRtVars(
          "()[s0, s1, s2, s3, s4, s5] -> (s4, s5, 0)",
          "()[s0, s1, s2, s3] -> (s1, s2, s3)",
          "()[s0, s1, s2, s3] -> (1, 1, 1)",
          R"(
s4 in [0, 26]
  hlo: %indices = s32[1806,2]{1,0} parameter(1)
  (d0, d1, d2, d3) -> (d0, 0)
s5 in [0, 68]
  hlo: %indices = s32[1806,2]{1,0} parameter(1)
  (d0, d1, d2, d3) -> (d0, 1)
)")));

  EXPECT_THAT(
      SymbolicTile::FromIndexingMap(*input_indexing.indexing_maps[1].begin()),
      Optional(MatchSymbolicTile("()[s0, s1, s2, s3] -> (0, 0)",
                                 "()[s0, s1, s2, s3] -> (s0, 2)",
                                 "()[s0, s1, s2, s3] -> (1, 1)")));
}

TEST_F(SymbolicTileTest, CanPropagateTileThroughSplitReshapeOfReverse) {
  // A split reshape of a reverse creates a negative unit stride atop a
  // floordiv.
  auto input_indexing = GetOutputToInputIndexing(ParseAndGetRoot(R"(
    HloModule m
    computation {
      p0 = f32[1,8,6,4]{3,2,1,0} parameter(0)
      reverse = f32[1,8,6,4]{3,2,1,0} reverse(p0), dimensions={1,2}
      ROOT bitcast = f32[48,4]{1,0} bitcast(reverse)
    }

    ENTRY e {
      p0 = f32[1,8,6,4]{3,2,1,0} parameter(0)
      ROOT fusion = f32[48,4]{1,0} fusion(p0), kind=kLoop, calls=computation
    }
  )"));

  // TODO(b/331257678): the expected expressions should be simplified.
  EXPECT_THAT(
      SymbolicTile::FromIndexingMap(*input_indexing.indexing_maps[0].begin()),
      Optional(MatchSymbolicTile(
          "()[s0, s1] -> (0, -((s0 + 5) floordiv 6) + 8, "
          "-(s0 - ((s0 - 1) floordiv 6) * 6) + 6, 0)",
          "()[s0, s1] -> "
          "(1, (s0 + 5) floordiv 6, s0 - ((s0 - 1) floordiv 6) * 6, s1)",
          "()[s0, s1] -> (0, 1, 1, 1)")));
}

TEST_F(SymbolicTileTest,
       FailsGracefullyAtPropagatingTileThroughSliceOfSplitReshape) {
  // TODO(b/326998704): constraints should allow us to unblock this use case.
  // A slice of a split reshape creates a non-unit stride atop a floordiv.
  auto input_indexing = GetOutputToInputIndexing(ParseAndGetRoot(R"(
    HloModule m
    computation {
      p0 = f32[1,8,6,4]{3,2,1,0} parameter(0)
      bitcast = f32[48,4]{1,0} bitcast(p0)
      ROOT slice = f32[5,2]{1,0} slice(bitcast), slice={[18:43:5], [0:4:2]}
    }

    ENTRY e {
      p0 = f32[1,8,6,4]{3,2,1,0} parameter(0)
      ROOT fusion = f32[5,2]{1,0} fusion(p0), kind=kLoop, calls=computation
    }
  )"));

  EXPECT_EQ(
      SymbolicTile::FromIndexingMap(*input_indexing.indexing_maps[0].begin()),
      std::nullopt);
}

TEST_F(SymbolicTileTest,
       FailsGracefullyAtPropagatingTileThroughMisalignedSliceOfSplitReshape) {
  // TODO(b/326998704): constraints should allow us to unblock part of this use
  // case.
  // TODO(b/331257678): handling correctly cases where offsets don't get
  // simplified away perfectly will allow us to unblock part of this use case.
  auto input_indexing = GetOutputToInputIndexing(ParseAndGetRoot(R"(
    HloModule m
    computation {
      p0 = f32[1,8,6,4]{3,2,1,0} parameter(0)
      bitcast = f32[48,4]{1,0} bitcast(p0)
      ROOT slice = f32[5,2]{1,0} slice(bitcast), slice={[20:45:5], [0:4:2]}
    }

    ENTRY e {
      p0 = f32[1,8,6,4]{3,2,1,0} parameter(0)
      ROOT fusion = f32[5,2]{1,0} fusion(p0), kind=kLoop, calls=computation
    }
  )"));

  EXPECT_EQ(
      SymbolicTile::FromIndexingMap(*input_indexing.indexing_maps[0].begin()),
      std::nullopt);
}

TEST_F(SymbolicTileTest,
       FailsGracefullyAtPropagatingTileThroughSliceOfSplitReshapeOnTranspose) {
  // TODO(b/326998704): constraints should allow us to unblock this use case.
  // A slice of a split reshape creates a non-unit stride atop a floordiv.
  auto input_indexing = GetOutputToInputIndexing(ParseAndGetRoot(R"(
    HloModule m
    computation {
      p0 = f32[1,6,8,4]{3,2,1,0} parameter(0)
      transpose = f32[1,8,6,4]{3,2,1,0} transpose(p0), dimensions={0,2,1,3}
      bitcast = f32[48,4]{1,0} bitcast(transpose)
      ROOT slice = f32[5,2]{1,0} slice(bitcast), slice={[18:43:5], [0:4:2]}
    }

    ENTRY e {
      p0 = f32[1,6,8,4]{3,2,1,0} parameter(0)
      ROOT fusion = f32[5,2]{1,0} fusion(p0), kind=kLoop, calls=computation
    }
  )"));

  EXPECT_EQ(
      SymbolicTile::FromIndexingMap(*input_indexing.indexing_maps[0].begin()),
      std::nullopt);
}

TEST_F(SymbolicTileTest,
       FailsGracefullyAtPropagatingTileThroughSliceOfSplitReshapeOfReverse) {
  // TODO(b/326998704): constraints should allow us to unblock this use case.
  // A slice of a split reshape of a reverse creates a negative non-unit stride
  // atop a floordiv.
  auto input_indexing = GetOutputToInputIndexing(ParseAndGetRoot(R"(
    HloModule m
    computation {
      p0 = f32[1,8,6,4]{3,2,1,0} parameter(0)
      reverse = f32[1,8,6,4]{3,2,1,0} reverse(p0), dimensions={1,2}
      bitcast = f32[48,4]{1,0} bitcast(reverse)
      ROOT slice = f32[5,2]{1,0} slice(bitcast), slice={[18:43:5], [0:4:2]}
    }

    ENTRY e {
      p0 = f32[1,8,6,4]{3,2,1,0} parameter(0)
      ROOT fusion = f32[5,2]{1,0} fusion(p0), kind=kLoop, calls=computation
    }
  )"));

  EXPECT_EQ(
      SymbolicTile::FromIndexingMap(*input_indexing.indexing_maps[0].begin()),
      std::nullopt);
}

TEST_F(SymbolicTileTest,
       FailsGracefullyAtPropagatingTileThroughReductionOfConcatenation) {
  // TODO(b/330906085): concatenating across a reduction dimension needs to be
  // handled to unblock this.
  auto input_indexing = GetOutputToInputIndexing(ParseAndGetRoot(R"(
    HloModule m
    max_computation {
      p0 = f32[] parameter(0)
      p1 = f32[] parameter(1)
      ROOT maximum = f32[] maximum(p0, p1)
    }

    computation {
      p0 = f32[10,8]{1,0} parameter(0)
      p1 = f32[20,8]{1,0} parameter(1)
      concatenate = f32[30,8]{1,0} concatenate(p0, p1), dimensions={0}
      neg_inf = f32[] constant(-inf)
      ROOT reduce = f32[8] reduce(concatenate, neg_inf), dimensions={0},
        to_apply=max_computation
    }

    ENTRY e {
      p0 = f32[10,8]{1,0} parameter(0)
      p1 = f32[20,8]{1,0} parameter(1)
      ROOT fusion = f32[8] fusion(p0, p1), kind=kLoop, calls=computation
    }
  )"));

  EXPECT_EQ(
      SymbolicTile::FromIndexingMap(*input_indexing.indexing_maps[1].begin()),
      std::nullopt);
}

}  // namespace
}  // namespace gpu
}  // namespace xla
