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

#include "xla/stream_executor/gpu/gpu_timer.h"

#include <cmath>
#include <cstdlib>
#include <optional>
#include <random>
#include <string_view>
#include <utility>

#include "absl/base/const_init.h"
#include "absl/base/thread_annotations.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"
#include "absl/utility/utility.h"
#include "xla/stream_executor/gpu/gpu_driver.h"
#include "xla/stream_executor/gpu/gpu_executor.h"
#include "xla/stream_executor/gpu/gpu_semaphore.h"
#include "xla/stream_executor/gpu/gpu_stream.h"
#include "xla/stream_executor/gpu/gpu_timer_kernel.h"
#include "xla/stream_executor/gpu/gpu_types.h"
#include "xla/stream_executor/stream.h"
#include "tsl/platform/errors.h"
#include "tsl/platform/statusor.h"

namespace stream_executor {
namespace gpu {

namespace {

bool return_random_durations = false;

absl::Duration RandomDuration() {
  static absl::Mutex mu(absl::kConstInit);
  static std::mt19937 rng ABSL_GUARDED_BY(mu);
  std::uniform_real_distribution<float> distribution(10, 1000);
  absl::MutexLock l(&mu);
  return absl::Microseconds(distribution(rng));
}

bool ShouldLaunchDelayKernel() {
  // Only launch the delay kernel if CUDA_LAUNCH_BLOCKING is not set to 1.
  static bool value = [] {
    const char* blocking = std::getenv("CUDA_LAUNCH_BLOCKING");
    return !blocking || std::string_view{blocking} != "1";
  }();
  return value;
}

}  // namespace

/*deprecated*/ /*static*/ absl::StatusOr<GpuTimer> GpuTimer::Create(
    GpuStream* stream) {
  // This deprecated factory does not launch the delay kernel and may lead to
  // reduced measurement accuracy.
  GpuExecutor* parent = stream->parent();
  GpuContext* context = parent->gpu_context();
  GpuEventHandle start_event;
  TF_RETURN_IF_ERROR(GpuDriver::InitEvent(context, &start_event,
                                          GpuDriver::EventFlags::kDefault));
  GpuEventHandle stop_event;
  TF_RETURN_IF_ERROR(GpuDriver::InitEvent(context, &stop_event,
                                          GpuDriver::EventFlags::kDefault));
  CHECK(start_event != nullptr && stop_event != nullptr);
  TF_RETURN_IF_ERROR(GpuDriver::RecordEvent(parent->gpu_context(), start_event,
                                            stream->gpu_stream()));
  return absl::StatusOr<GpuTimer>{absl::in_place, parent, start_event,
                                  stop_event, stream};
}

/*deprecated*/ /*static*/ absl::StatusOr<std::optional<GpuTimer>>
GpuTimer::CreateIfNeeded(GpuStream* stream, bool is_needed) {
  // This deprecated factory does not launch the delay kernel and may lead to
  // reduced measurement accuracy.
  if (is_needed) {
    TF_ASSIGN_OR_RETURN(GpuTimer t, GpuTimer::Create(stream));
    return {std::make_optional(std::move(t))};
  }
  return std::nullopt;
}

/*static*/ absl::StatusOr<GpuTimer> GpuTimer::Create(Stream* real_stream,
                                                     bool use_delay_kernel) {
  GpuStream* stream = AsGpuStream(real_stream);
  GpuExecutor* parent = stream->parent();
  GpuContext* context = parent->gpu_context();
  GpuEventHandle start_event;
  TF_RETURN_IF_ERROR(GpuDriver::InitEvent(context, &start_event,
                                          GpuDriver::EventFlags::kDefault));
  GpuEventHandle stop_event;
  TF_RETURN_IF_ERROR(GpuDriver::InitEvent(context, &stop_event,
                                          GpuDriver::EventFlags::kDefault));
  CHECK(start_event != nullptr && stop_event != nullptr);
  GpuSemaphore semaphore{};
  if (!use_delay_kernel) {
    LOG(WARNING)
        << "Skipping the delay kernel, measurement accuracy will be reduced";
  }

  if (use_delay_kernel && ShouldLaunchDelayKernel()) {
    TF_ASSIGN_OR_RETURN(bool is_supported, DelayKernelIsSupported(stream));

    if (is_supported) {
      TF_ASSIGN_OR_RETURN(semaphore, LaunchDelayKernel(real_stream));
    }
  }

  // The start event goes after the delay kernel in the stream
  TF_RETURN_IF_ERROR(GpuDriver::RecordEvent(parent->gpu_context(), start_event,
                                            stream->gpu_stream()));
  return absl::StatusOr<GpuTimer>{absl::in_place, parent, start_event,
                                  stop_event,     stream, std::move(semaphore)};
}

/*static*/ absl::StatusOr<std::optional<GpuTimer>> GpuTimer::CreateIfNeeded(
    Stream* stream, bool use_delay_kernel, bool is_needed) {
  if (is_needed) {
    TF_ASSIGN_OR_RETURN(GpuTimer t, GpuTimer::Create(stream, use_delay_kernel));
    return {std::make_optional(std::move(t))};
  }
  return std::nullopt;
}

/*static*/ void GpuTimer::ReturnRandomDurationsForTesting() {
  return_random_durations = true;
}

GpuTimer::~GpuTimer() {
  GpuContext* context = parent_->gpu_context();
  if (semaphore_ && !is_stopped_) {
    // Signal the delay kernel that it can exit
    *semaphore_ = GpuSemaphoreState::kRelease;
    // Wait for the delay kernel to exit before destroying the value that it is
    // watching.
    absl::Status status =
        GpuDriver::SynchronizeStream(context, stream_->gpu_stream());
    if (!status.ok()) {
      LOG(ERROR) << status;
    }
  }
  if (start_event_ != nullptr) {
    absl::Status status = GpuDriver::DestroyEvent(context, &start_event_);
    if (!status.ok()) {
      LOG(ERROR) << status;
    }
  }
  if (stop_event_ != nullptr) {
    absl::Status status = GpuDriver::DestroyEvent(context, &stop_event_);
    if (!status.ok()) {
      LOG(ERROR) << status;
    }
  }
}

absl::StatusOr<absl::Duration> GpuTimer::GetElapsedDuration() {
  if (is_stopped_) {
    return absl::InternalError("Measuring inactive timer");
  }
  TF_RETURN_IF_ERROR(GpuDriver::RecordEvent(parent_->gpu_context(), stop_event_,
                                            stream_->gpu_stream()));
  // If we launched the delay kernel then check if it already timed out.
  if (semaphore_) {
    if (*semaphore_ == GpuSemaphoreState::kTimedOut) {
      // The delay kernel did not achieve the intended result.
      LOG(ERROR) << "Delay kernel timed out: measured time has sub-optimal "
                    "accuracy. There may be a missing warmup execution, please "
                    "investigate in Nsight Systems.";
    } else {
      // Signal that the kernel can exit
      *semaphore_ = GpuSemaphoreState::kRelease;
    }
  }
  float elapsed_milliseconds = NAN;
  if (!GpuDriver::GetEventElapsedTime(parent_->gpu_context(),
                                      &elapsed_milliseconds, start_event_,
                                      stop_event_)) {
    return absl::InternalError("Error stopping the timer");
  }
  is_stopped_ = true;
  if (return_random_durations) {
    return RandomDuration();
  }
  return absl::Milliseconds(elapsed_milliseconds);
}

}  // namespace gpu
}  // namespace stream_executor
