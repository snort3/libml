/* Copyright 2021 The OpenXLA Authors.

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

#ifndef XLA_BACKENDS_PROFILER_GPU_MOCK_CUPTI_H_
#define XLA_BACKENDS_PROFILER_GPU_MOCK_CUPTI_H_

#include <stddef.h>
#include <stdint.h>

#include <cstdint>

#include "xla/backends/profiler/gpu/cupti_interface.h"
#include "tsl/platform/test.h"

namespace xla {
namespace profiler {

// A mock object automatically generated by gmock_gen.py.
class MockCupti : public xla::profiler::CuptiInterface {
 public:
  MOCK_METHOD(CUptiResult, ActivityDisable, (CUpti_ActivityKind kind),
              (override));
  MOCK_METHOD(CUptiResult, ActivityEnable, (CUpti_ActivityKind kind),
              (override));
  MOCK_METHOD(CUptiResult, ActivityFlushAll, (uint32_t flag), (override));
  MOCK_METHOD(CUptiResult, ActivityGetNextRecord,
              (uint8_t* buffer, size_t valid_buffer_size_bytes,
               CUpti_Activity** record),
              (override));
  MOCK_METHOD(CUptiResult, ActivityGetNumDroppedRecords,
              (CUcontext context, uint32_t stream_id, size_t* dropped),
              (override));
  MOCK_METHOD(CUptiResult, ActivityConfigureUnifiedMemoryCounter,
              (CUpti_ActivityUnifiedMemoryCounterConfig * config,
               uint32_t count),
              (override));
  MOCK_METHOD(CUptiResult, ActivityRegisterCallbacks,
              (CUpti_BuffersCallbackRequestFunc func_buffer_requested,
               CUpti_BuffersCallbackCompleteFunc func_buffer_completed),
              (override));
  MOCK_METHOD(CUptiResult, ActivityUsePerThreadBuffer, (), (override));
  MOCK_METHOD(CUptiResult, GetDeviceId, (CUcontext context, uint32_t* deviceId),
              (override));
  MOCK_METHOD(CUptiResult, GetTimestamp, (uint64_t* timestamp), (override));
  MOCK_METHOD(CUptiResult, Finalize, (), (override));
  MOCK_METHOD(CUptiResult, EnableCallback,
              (uint32_t enable, CUpti_SubscriberHandle subscriber,
               CUpti_CallbackDomain domain, CUpti_CallbackId cbid),
              (override));
  MOCK_METHOD(CUptiResult, EnableDomain,
              (uint32_t enable, CUpti_SubscriberHandle subscriber,
               CUpti_CallbackDomain domain),
              (override));
  MOCK_METHOD(CUptiResult, Subscribe,
              (CUpti_SubscriberHandle * subscriber, CUpti_CallbackFunc callback,
               void* userdata),
              (override));
  MOCK_METHOD(CUptiResult, Unsubscribe, (CUpti_SubscriberHandle subscriber),
              (override));
  MOCK_METHOD(CUptiResult, GetResultString,
              (CUptiResult result, const char** str), (override));

  MOCK_METHOD(CUptiResult, GetContextId,
              (CUcontext context, uint32_t* context_id), (override));

  MOCK_METHOD(CUptiResult, GetStreamIdEx,
              (CUcontext context, CUstream stream, uint8_t per_thread_stream,
               uint32_t* stream_id),
              (override));

  MOCK_METHOD(void, CleanUp, (), (override));
  MOCK_METHOD(bool, Disabled, (), (const, override));
};

}  // namespace profiler
}  // namespace xla

#endif  // XLA_BACKENDS_PROFILER_GPU_MOCK_CUPTI_H_