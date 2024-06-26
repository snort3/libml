/* Copyright 2019 The OpenXLA Authors.

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

#include "xla/service/gpu/hlo_algorithm_denylist.h"

#include <cstdlib>
#include <string>

#include "absl/strings/str_cat.h"
#include "xla/stream_executor/dnn.h"
#include "tsl/platform/env.h"
#include "tsl/platform/path.h"
#include "tsl/platform/test.h"

namespace xla {
namespace gpu {
namespace {

class DenylistTest : public testing::Test {
 protected:
  DenylistTest() {
    std::string existing_xla_flags;
    const char* env = std::getenv("XLA_FLAGS");
    if (env != nullptr) {
      existing_xla_flags = absl::StrCat(env, " ");
    }

    tsl::setenv(
        "XLA_FLAGS",
        absl::StrCat(
            existing_xla_flags, "--xla_gpu_algorithm_denylist_path=",
            tsl::io::JoinPath(tsl::testing::XlaSrcRoot(), "service", "gpu",
                              "data", "hlo_algorithm_denylist.pbtxt"))
            .data(),
        /*overwrite=*/true);
  }
};

TEST_F(DenylistTest, DefaultTest) {
  ComputeCapability cc;
  cc.set_major(7);
  cc.set_minor(0);
  CudnnVersion cudnn_version;
  cudnn_version.set_major(7);
  cudnn_version.set_minor(6);
  cudnn_version.set_patch(2);
  auto list = GetDisabledConvAlgorithms(
      cc, cudnn_version, /*blas_version=*/"9000",
      R"((f16[256,112,112,64]{3,2,1,0}, u8[0]{0}) custom-call(f16[256,224,224,4]{3,2,1,0}, f16[7,7,4,64]{2,1,0,3}), window={size=7x7 stride=2x2 pad=3_3x3_3}, dim_labels=b01f_01io->b01f, custom_call_target="__cudnn$convForward", backend_config="{conv_result_scale:1}")");
  EXPECT_THAT(list, testing::UnorderedElementsAre(
                        stream_executor::dnn::AlgorithmDesc{0, true},
                        stream_executor::dnn::AlgorithmDesc{0, false},
                        stream_executor::dnn::AlgorithmDesc{1, true},
                        stream_executor::dnn::AlgorithmDesc{1, false},
                        stream_executor::dnn::AlgorithmDesc{42, true},
                        stream_executor::dnn::AlgorithmDesc{42, false}));
}

TEST_F(DenylistTest, NegativeTest) {
  ComputeCapability cc;
  cc.set_major(7);
  cc.set_minor(0);
  CudnnVersion cudnn_version;
  cudnn_version.set_major(7);
  cudnn_version.set_minor(6);
  cudnn_version.set_minor(2);
  auto list =
      GetDisabledConvAlgorithms(cc, cudnn_version, "9000", R"(invalid hlo)");
  EXPECT_THAT(list, testing::IsEmpty());
}

TEST_F(DenylistTest, NoBlasVersionSet) {
  ComputeCapability cc;
  cc.set_major(7);
  cc.set_minor(0);
  CudnnVersion cudnn_version;
  cudnn_version.set_major(7);
  cudnn_version.set_minor(6);
  cudnn_version.set_patch(2);
  auto list = GetDisabledConvAlgorithms(
      cc, cudnn_version, /*blas_version=*/"120301",
      R"((f16[256,112,112,64]{3,2,1,0}, u8[0]{0}) custom-call(f16[256,224,224,4]{3,2,1,0}, f16[7,7,4,64]{2,1,0,3}), window={size=7x7 stride=2x2 pad=3_3x3_3}, dim_labels=b01f_01io->b01f, custom_call_target="__cudnn$convForward", backend_config="{conv_result_scale:1}")");
  EXPECT_THAT(list, testing::UnorderedElementsAre(
                        stream_executor::dnn::AlgorithmDesc{42, true},
                        stream_executor::dnn::AlgorithmDesc{42, false}));
}

}  // namespace
}  // namespace gpu
}  // namespace xla
