/* Copyright 2020 The OpenXLA Authors.

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

#ifndef XLA_BACKENDS_PROFILER_GPU_CUPTI_BUFFER_EVENTS_H_
#define XLA_BACKENDS_PROFILER_GPU_CUPTI_BUFFER_EVENTS_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <list>
#include <memory>
#include <string>
#include <utility>

#include "absl/container/fixed_array.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/node_hash_set.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "tsl/platform/mutex.h"
#include "tsl/platform/thread_annotations.h"
#include "tsl/profiler/utils/buffer_pool.h"

namespace xla {
namespace profiler {

struct MemcpyDetails {
  // The amount of data copied for memcpy events.
  size_t num_bytes;
  // The destination device for peer-2-peer communication (memcpy). The source
  // device is implicit: it's the current device.
  uint32_t destination;
  // Whether or not the memcpy is asynchronous.
  bool async;
  // This contains CUpti_ActivityMemcpyKind for activity event (on device).
  // For events from other CuptiTracerEventSource, it is always 0.
  int8_t copy_kind;
  // CUpti_ActivityMemoryKind of source.
  int8_t src_mem_kind;
  // CUpti_ActivityMemoryKind of destination.
  int8_t dst_mem_kind;

  // ID of the hardware channel on which this operation ran.
  uint32_t channel_id = -1;
  // CUpti_ChannelType of the channel above.
  int8_t channel_type = 0;  // CUPTI_CHANNEL_TYPE_INVALID
};

struct MemAllocDetails {
  // Size of memory to be written over in bytes.
  size_t num_bytes;
  // The CUpti_ActivityMemoryKind value for this activity event.
  int8_t mem_kind;
  // The virtual address of allocation. 0 if it is a free operation.
  uint64_t address;
};

using MemFreeDetails = MemAllocDetails;

// Memory residency contains details read from CUpti_ActivityMemory type. This
// is populated in the CUPTI tracer encounters a CUPTI_ACTIVITY_KIND_MEMORY
// event. The start of this even corresponse to a cudaMalloc, and the end
// corresponds to a cudaFree.
using MemoryResidencyDetails = MemAllocDetails;

// cudaHostRegister
struct HostRegisterDetails {
  size_t num_bytes;
  uint64_t address;
  unsigned int flags;
};

// cudaHostUnregister
struct HostUnregisterDetails {
  uint64_t address;
};

struct MemsetDetails {
  // Size of memory to be written over in bytes.
  size_t num_bytes;
  // The CUpti_ActivityMemoryKind value for this activity event.
  int8_t mem_kind;
  // Whether or not the memset is asynchronous.
  bool async;

  // ID of the hardware channel on which this operation ran.
  uint32_t channel_id = -1;
  // CUpti_ChannelType of the channel above.
  int8_t channel_type = 0;  // CUPTI_CHANNEL_TYPE_INVALID
};

struct KernelDetails {
  // The number of registers used in this kernel.
  uint32_t registers_per_thread;
  // The amount of shared memory space used by a thread block.
  uint32_t static_shared_memory_usage;
  // The amount of dynamic memory space used by a thread block.
  uint32_t dynamic_shared_memory_usage;
  // X-dimension of a thread block.
  uint32_t block_x;
  // Y-dimension of a thread block.
  uint32_t block_y;
  // Z-dimension of a thread block.
  uint32_t block_z;
  // X-dimension of a grid.
  uint32_t grid_x;
  // Y-dimension of a grid.
  uint32_t grid_y;
  // Z-dimension of a grid.
  uint32_t grid_z;

  // ID of the hardware channel on which this operation ran.
  uint32_t channel_id = -1;
  // CUpti_ChannelType of the channel above.
  int8_t channel_type = 0;  // CUPTI_CHANNEL_TYPE_INVALID
};

inline std::string ToXStat(const KernelDetails& kernel_info,
                           double occupancy_pct) {
  return absl::StrCat(
      "regs:", kernel_info.registers_per_thread,
      " static_shared:", kernel_info.static_shared_memory_usage,
      " dynamic_shared:", kernel_info.dynamic_shared_memory_usage,
      " grid:", kernel_info.grid_x, ",", kernel_info.grid_y, ",",
      kernel_info.grid_z, " block:", kernel_info.block_x, ",",
      kernel_info.block_y, ",", kernel_info.block_z,
      " occ_pct:", occupancy_pct);
}

// Gets the name of the CUpti_ActivityMemoryKind value.
absl::string_view GetMemoryKindName(int8_t memory_kind);

enum class CuptiTracerEventType {
  Unsupported = 0,
  Kernel = 1,
  MemcpyH2D = 2,
  MemcpyD2H = 3,
  MemcpyD2D = 4,
  MemcpyP2P = 5,
  MemcpyOther = 6,
  MemoryAlloc = 7,
  Overhead = 8,
  UnifiedMemory = 9,
  MemoryFree = 10,
  Memset = 11,
  MemoryResidency = 12,
  HostRegister = 13,
  HostUnregister = 14,
  Generic = 100,
};

const char* GetTraceEventTypeName(const CuptiTracerEventType& type);

enum class CuptiTracerEventSource {
  Invalid = 0,
  DriverCallback = 1,
  Activity = 2,
  // Maybe consider adding runtime callback and metric api in the future.
};

struct CuptiTracerEvent {
  static constexpr uint32_t kInvalidThreadId =
      std::numeric_limits<uint32_t>::max();
  static constexpr uint32_t kInvalidCorrelationId =
      std::numeric_limits<uint32_t>::max();
  static constexpr uint64_t kInvalidContextId =
      std::numeric_limits<uint64_t>::max();
  static constexpr uint64_t kInvalidStreamId =
      std::numeric_limits<uint64_t>::max();
  CuptiTracerEventType type = CuptiTracerEventType::Unsupported;
  CuptiTracerEventSource source = CuptiTracerEventSource::Invalid;
  // Although CUpti_CallbackData::functionName is persistent, however
  // CUpti_ActivityKernel4::name is not persistent, therefore we need a copy of
  // it.
  std::string name;
  // This points to strings in AnnotationMap, which should outlive the point
  // where serialization happens.
  absl::string_view annotation;
  absl::string_view nvtx_range;
  uint64_t start_time_ns = 0;
  uint64_t end_time_ns = 0;
  uint32_t device_id = 0;
  uint32_t correlation_id = kInvalidCorrelationId;
  uint32_t thread_id = kInvalidThreadId;
  int64_t context_id = kInvalidContextId;
  int64_t stream_id = kInvalidStreamId;
  union {
    // For Memcpy API and activities. `type` must be Memcpy*.
    MemcpyDetails memcpy_info;
    // Used for MemAlloc API. `type` must be MemoryAlloc.
    MemAllocDetails memalloc_info;
    // Used for kernel activities. `type` must be Kernel.
    KernelDetails kernel_info;
    // Used for MemFree activities. `type` must be MemoryFree.
    MemFreeDetails memfree_info;
    // Used for cuMemHostRegister.  `type` must be HostRegister.
    HostRegisterDetails host_register_info;
    // Used for cuMemHostUnregister.  `type` must be HostUnregister.
    HostUnregisterDetails host_unregister_info;
    // Used for Memset API and activities. `type` must be Memset.
    MemsetDetails memset_info;
    // Used for Memory residency activities. `type` must be MemoryResidency.
    MemoryResidencyDetails memory_residency_info;
  };
};

class AnnotationMap {
 public:
  struct AnnotationInfo {
    absl::string_view annotation;
    absl::string_view nvtx_range;
  };

  explicit AnnotationMap(uint64_t max_size, uint32_t num_gpus)
      : max_size_(max_size), per_device_map_(num_gpus) {}
  void Add(uint32_t device_id, uint32_t correlation_id,
           absl::string_view annotation, absl::string_view nvtx_range);
  AnnotationInfo LookUp(uint32_t device_id, uint32_t correlation_id);

 private:
  struct PerDeviceAnnotationMap {
    // The population/consumption of annotations might happen from multiple
    // callback/activity api related threads.
    tsl::mutex mutex;
    // Annotation tends to be repetitive, use a hash_set to store the strings,
    // an use the reference to the string in the map.
    absl::node_hash_set<std::string> annotations TF_GUARDED_BY(mutex);
    absl::node_hash_set<std::string> nvtx_ranges TF_GUARDED_BY(mutex);
    absl::flat_hash_map<uint32_t, AnnotationInfo> correlation_map
        TF_GUARDED_BY(mutex);
  };
  const uint64_t max_size_;
  absl::FixedArray<PerDeviceAnnotationMap> per_device_map_;

  AnnotationMap(const AnnotationMap&) = delete;
  void operator=(const AnnotationMap&) = delete;
};

struct CuptiEventCollectorDelegate {
  AnnotationMap& annotation_map;
  std::function<void(CuptiTracerEvent&&)> receive;
  explicit CuptiEventCollectorDelegate(
      AnnotationMap& p_annotation_map,
      std::function<void(CuptiTracerEvent&&)> p_receive)
      : annotation_map(p_annotation_map), receive(std::move(p_receive)) {}
};

class CuptiActivityBufferManager {
 public:
  struct ActivityBufferAndSize {
    std::unique_ptr<uint8_t, std::function<void(uint8_t*)>> buffer;
    size_t size;  // size in bytes for the events filled by CUPTI.
    explicit ActivityBufferAndSize(uint8_t* p = nullptr, size_t sz = 0);
  };

  explicit CuptiActivityBufferManager(size_t buffer_size_in_bytes)
      : buffer_pool_(buffer_size_in_bytes) {}

  size_t GetBufferSizeInBytes() { return buffer_pool_.GetBufferSizeInBytes(); }

  uint8_t* GetOrCreateBuffer() { return buffer_pool_.GetOrCreateBuffer(); }

  void ReclaimBuffer(uint8_t* p) { buffer_pool_.ReclaimBuffer(p); }

  void CacheCuptiFilledActivityBuffer(uint8_t* p, size_t sz) {
    tsl::mutex_lock lock(buffer_mutex_);
    cached_buffers_.emplace_back(p, sz);
  }

  void AddCachedActivityEventsTo(CuptiEventCollectorDelegate& receiver,
                                 size_t max_activity_event_count,
                                 size_t& dropped_activity_event_count);

 private:
  tsl::profiler::BufferPool buffer_pool_;
  tsl::mutex buffer_mutex_;
  std::list<ActivityBufferAndSize> cached_buffers_ TF_GUARDED_BY(buffer_mutex_);
};

}  // namespace profiler
}  // namespace xla

#endif  // XLA_BACKENDS_PROFILER_GPU_CUPTI_BUFFER_EVENTS_H_
