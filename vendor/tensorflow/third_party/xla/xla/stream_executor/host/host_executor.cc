/* Copyright 2016 The OpenXLA Authors.

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

// Implementation of HostExecutor class [of those methods not defined in the
// class declaration].
#include "xla/stream_executor/host/host_executor.h"

#include <stdint.h>
#include <string.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "absl/functional/any_invocable.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/notification.h"
#include "absl/types/span.h"
#include "xla/stream_executor/device_description.h"
#include "xla/stream_executor/device_memory.h"
#include "xla/stream_executor/event.h"
#include "xla/stream_executor/event_interface.h"
#include "xla/stream_executor/host/host_execution_engine.h"
#include "xla/stream_executor/host/host_kernel.h"
#include "xla/stream_executor/host/host_stream.h"
#include "xla/stream_executor/kernel_spec.h"
#include "xla/stream_executor/launch_dim.h"
#include "xla/stream_executor/stream_executor.h"
#include "xla/stream_executor/stream_executor_interface.h"
#include "tsl/platform/mem.h"
#include "tsl/platform/profile_utils/cpu_utils.h"
#include "tsl/platform/statusor.h"

namespace stream_executor {
namespace host {

HostStream* AsHostStream(Stream* stream) {
  DCHECK(stream != nullptr);
  return dynamic_cast<HostStream*>(stream->implementation());
}

absl::Status HostExecutor::Init() { return absl::OkStatus(); }

absl::StatusOr<std::unique_ptr<Kernel>> HostExecutor::CreateKernel() {
  return std::make_unique<HostKernel>();
}

absl::Status HostExecutor::GetKernel(const MultiKernelLoaderSpec& spec,
                                     Kernel* kernel) {
  HostKernel* host_kernel = AsHostKernel(kernel);
  host_kernel->SetArity(spec.arity());

  VLOG(3) << "GetKernel on kernel " << kernel << " : " << kernel->name();

  if (spec.has_llvm_host_kernel()) {
    const LlvmHostKernel& llvm_host_kernel = spec.llvm_host_kernel();
    const absl::string_view name = llvm_host_kernel.kernel_name();
    const absl::string_view entry = llvm_host_kernel.entrypoint();
    const absl::string_view ir = llvm_host_kernel.ir();
    const absl::Span<const std::string> options = llvm_host_kernel.options();

    TF_ASSIGN_OR_RETURN(
        auto execution_engine,
        LlvmExecutionEngine::CreateFromLlvmIr(name, entry, ir, options));
    host_kernel->SetExecutionEngine(std::move(execution_engine));
    return absl::OkStatus();
  } else if (false /* TODO(tsilytskyi): Implement CppHostKernel */) {
    // host_kernel->SetExecutionEngine(std::make_unique<CppExecutionEngine>());
  } else {
    return absl::InternalError("No method of loading host kernel provided");
  }

  return absl::UnimplementedError("Not Implemented");
}

absl::Status HostExecutor::Launch(Stream* stream, const ThreadDim& thread_dims,
                                  const BlockDim& block_dims,
                                  const Kernel& kernel,
                                  const KernelArgs& args) {
  // const HostKernel* host_kernel = AsHostKernel(&kernel);

  // TODO(tsilytskyi): convert args into proper format
  // host_kernel->Launch(thread_dims, args);
  return absl::UnimplementedError("Not Implemented");
}

bool HostExecutor::DeviceMemoryUsage(int64_t* free, int64_t* total) const {
  tsl::port::MemoryInfo mem_info = tsl::port::GetMemoryInfo();
  *free = (mem_info.free != INT64_MAX) ? mem_info.free : -1;
  *total = (mem_info.total != INT64_MAX) ? mem_info.total : -1;
  return true;
}

DeviceMemoryBase HostExecutor::Allocate(uint64_t size, int64_t memory_space) {
  CHECK_EQ(memory_space, 0);
  // Use a minimum alignment of 64 bytes to be friendly to AVX512 code.
  // This should probably be kept in sync with
  // tsl::Allocator::kAllocatorAlignment.
  return DeviceMemoryBase(
      tsl::port::AlignedMalloc(size, /*minimum_alignment=*/64), size);
}

void HostExecutor::Deallocate(DeviceMemoryBase* mem) {
  tsl::port::AlignedFree(mem->opaque());
}

absl::Status HostExecutor::SynchronousMemZero(DeviceMemoryBase* location,
                                              uint64_t size) {
  memset(location->opaque(), 0, size);
  return absl::OkStatus();
}

absl::Status HostExecutor::Memcpy(Stream* stream, void* host_dst,
                                  const DeviceMemoryBase& gpu_src,
                                  uint64_t size) {
  // Enqueue the [asynchronous] memcpy on the stream (HostStream) associated
  // with the HostExecutor.
  void* src_mem = const_cast<void*>(gpu_src.opaque());
  AsHostStream(stream)->EnqueueTask(
      [host_dst, src_mem, size]() { memcpy(host_dst, src_mem, size); });
  return absl::OkStatus();
}

absl::Status HostExecutor::Memcpy(Stream* stream, DeviceMemoryBase* gpu_dst,
                                  const void* host_src, uint64_t size) {
  void* dst_mem = gpu_dst->opaque();
  // Enqueue the [asynchronous] memcpy on the stream (HostStream) associated
  // with the HostExecutor.
  AsHostStream(stream)->EnqueueTask(
      [dst_mem, host_src, size]() { memcpy(dst_mem, host_src, size); });
  return absl::OkStatus();
}

bool HostExecutor::MemcpyDeviceToDevice(Stream* stream,
                                        DeviceMemoryBase* gpu_dst,
                                        const DeviceMemoryBase& gpu_src,
                                        uint64_t size) {
  void* dst_mem = gpu_dst->opaque();
  void* src_mem = const_cast<void*>(gpu_src.opaque());
  // Enqueue this [asynchronous] "device-to-device" (i.e., host-to-host, given
  // the nature of the HostExecutor) memcpy  on the stream (HostStream)
  // associated with the HostExecutor.
  AsHostStream(stream)->EnqueueTask(
      [src_mem, dst_mem, size]() { memcpy(dst_mem, src_mem, size); });
  return true;
}

absl::Status HostExecutor::MemZero(Stream* stream, DeviceMemoryBase* location,
                                   uint64_t size) {
  void* gpu_mem = location->opaque();
  // Enqueue the [asynchronous] memzero on the stream (HostStream) associated
  // with the HostExecutor.
  AsHostStream(stream)->EnqueueTask(
      [gpu_mem, size]() { memset(gpu_mem, 0, size); });
  return absl::OkStatus();
}

absl::Status HostExecutor::Memset(Stream* stream, DeviceMemoryBase* location,
                                  uint8 pattern, uint64_t size) {
  void* gpu_mem = location->opaque();
  // Enqueue the [asynchronous] memzero on the stream (HostStream) associated
  // with the HostExecutor.
  AsHostStream(stream)->EnqueueTask(
      [gpu_mem, size, pattern]() { memset(gpu_mem, pattern, size); });
  return absl::OkStatus();
}

absl::Status HostExecutor::Memset32(Stream* stream, DeviceMemoryBase* location,
                                    uint32_t pattern, uint64_t size) {
  void* gpu_mem = location->opaque();
  // Enqueue the [asynchronous] memzero on the stream (HostStream) associated
  // with the HostExecutor.
  AsHostStream(stream)->EnqueueTask(
      [gpu_mem, size, pattern]() { memset(gpu_mem, pattern, size); });
  return absl::OkStatus();
}

absl::Status HostExecutor::SynchronousMemcpy(DeviceMemoryBase* gpu_dst,
                                             const void* host_src,
                                             uint64_t size) {
  memcpy(gpu_dst->opaque(), host_src, size);
  return absl::OkStatus();
}

absl::Status HostExecutor::SynchronousMemcpy(void* host_dst,
                                             const DeviceMemoryBase& gpu_src,
                                             uint64_t size) {
  memcpy(host_dst, gpu_src.opaque(), size);
  return absl::OkStatus();
}

bool HostExecutor::HostCallback(
    Stream* stream, absl::AnyInvocable<absl::Status() &&> callback) {
  AsHostStream(stream)->EnqueueTaskWithStatus(std::move(callback));
  return true;
}

void HostExecutor::DeallocateStream(Stream* stream) {}

bool HostExecutor::CreateStreamDependency(Stream* dependent, Stream* other) {
  auto event = std::make_shared<absl::Notification>();
  AsHostStream(other)->EnqueueTask([event]() { event->Notify(); });
  AsHostStream(dependent)->EnqueueTask(
      [event]() { event->WaitForNotification(); });
  return true;
}

class HostEvent : public EventInterface {
 public:
  HostEvent() : notification_(std::make_shared<absl::Notification>()) {}

  std::shared_ptr<absl::Notification>& notification() { return notification_; }

 private:
  // We use a std::shared_ptr here because the client may delete the HostEvent
  // object while there are still RecordEvent and WaitForEvent callbacks pending
  // on a stream.
  std::shared_ptr<absl::Notification> notification_;
};

std::unique_ptr<EventInterface> HostExecutor::CreateEventImplementation() {
  return std::unique_ptr<EventInterface>(new HostEvent());
}

static HostEvent* AsHostEvent(Event* event) {
  DCHECK(event != nullptr);
  return static_cast<HostEvent*>(event->implementation());
}

absl::Status HostExecutor::AllocateEvent(Event* /*event*/) {
  return absl::OkStatus();
}

absl::Status HostExecutor::DeallocateEvent(Event* /*event*/) {
  return absl::OkStatus();
}

absl::Status HostExecutor::RecordEvent(Stream* stream, Event* event) {
  std::shared_ptr<absl::Notification> notification =
      AsHostEvent(event)->notification();
  AsHostStream(stream)->EnqueueTask([notification]() {
    CHECK(!notification->HasBeenNotified());
    notification->Notify();
  });
  return absl::OkStatus();
}

absl::Status HostExecutor::WaitForEvent(Stream* stream, Event* event) {
  std::shared_ptr<absl::Notification> notification =
      AsHostEvent(event)->notification();
  AsHostStream(stream)->EnqueueTask(
      [notification]() { notification->WaitForNotification(); });
  return absl::OkStatus();
}

Event::Status HostExecutor::PollForEventStatus(Event* event) {
  absl::Notification& notification = *AsHostEvent(event)->notification();
  return notification.HasBeenNotified() ? Event::Status::kComplete
                                        : Event::Status::kPending;
}

absl::Status HostExecutor::BlockHostUntilDone(Stream* stream) {
  return AsHostStream(stream)->BlockUntilDone();
}

absl::StatusOr<std::unique_ptr<DeviceDescription>>
HostExecutor::CreateDeviceDescription(int device_ordinal) {
  internal::DeviceDescriptionBuilder builder;

  builder.set_device_address_bits(64);

  // TODO(rspringer): How to report a value that's based in reality but that
  // doesn't result in thrashing or other badness? 4GiB chosen arbitrarily.
  builder.set_device_memory_size(static_cast<uint64_t>(4) * 1024 * 1024 * 1024);

  float cycle_counter_frequency = static_cast<float>(
      tsl::profile_utils::CpuUtils::GetCycleCounterFrequency());
  builder.set_clock_rate_ghz(cycle_counter_frequency / 1e9);

  builder.set_name("Host");
  builder.set_platform_version("Default Version");

  return builder.Build();
}

absl::StatusOr<std::unique_ptr<Stream>> HostExecutor::CreateStream(
    std::optional<std::variant<StreamPriority, int>> priority) {
  return std::make_unique<Stream>(this, std::make_unique<HostStream>());
}

}  // namespace host
}  // namespace stream_executor
