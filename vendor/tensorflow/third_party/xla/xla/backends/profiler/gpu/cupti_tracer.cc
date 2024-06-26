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

#include "xla/backends/profiler/gpu/cupti_tracer.h"

#include "absl/cleanup/cleanup.h"
#include "absl/container/flat_hash_set.h"
#include "third_party/gpus/cuda/extras/CUPTI/include/cupti_activity.h"
#include "third_party/gpus/cuda/extras/CUPTI/include/generated_nvtx_meta.h"
#include "third_party/gpus/cuda/include/cuda.h"
#include "xla/backends/profiler/gpu/cupti_collector.h"
#include "xla/backends/profiler/gpu/nvtx_utils.h"
#include "tsl/platform/env.h"
#include "tsl/platform/errors.h"
#include "tsl/platform/host_info.h"
#include "tsl/platform/logging.h"
#include "tsl/platform/macros.h"
#include "tsl/profiler/backends/cpu/annotation_stack.h"

namespace xla {
namespace profiler {

namespace {

using absl::OkStatus;
using tsl::Env;
using tsl::profiler::AnnotationStack;

static thread_local int internalCuCall = 0;

// Temporary disable cupti api tracing for this thread during the life scope of
// this class. Used for the API calls that initiated by us.
class CuptiApiTracingDisabler {
 public:
  CuptiApiTracingDisabler() { internalCuCall++; }
  ~CuptiApiTracingDisabler() { internalCuCall--; }
};

absl::Status ToStatus(CUptiResult result) {
  if (result == CUPTI_SUCCESS) {
    return OkStatus();
  }
  const char *str = nullptr;
  cuptiGetResultString(result, &str);
  return tsl::errors::Unavailable("CUPTI error: ", str ? str : "<unknown>");
}

absl::Status ToStatus(CUresult result) {
  if (result == CUDA_SUCCESS) {
    return OkStatus();
  }
  const char *str = nullptr;
  cuGetErrorName(result, &str);
  return tsl::errors::Unavailable("CUDA error: ", str ? str : "<unknown>");
}

inline void LogIfError(const absl::Status &status) {
  if (status.ok()) return;
  LOG(ERROR) << status.message();
}

// CUPTI_ERROR_INSUFFICIENT_PRIVILEGES is introduced at CUDA 10.1.
#if CUDA_VERSION <= 10000
#define CUPTI_ERROR_INSUFFICIENT_PRIVILEGES 35
#endif

#define RETURN_IF_CUPTI_ERROR(expr)                                         \
  do {                                                                      \
    CUptiResult status = expr;                                              \
    if (ABSL_PREDICT_FALSE(status != CUPTI_SUCCESS)) {                      \
      const char *errstr = "";                                              \
      cupti_interface_->GetResultString(status, &errstr);                   \
      LOG(ERROR) << "function " << #expr << "failed with error " << errstr; \
      if (status == CUPTI_ERROR_INSUFFICIENT_PRIVILEGES) {                  \
        return tsl::errors::PermissionDenied("CUPTI need root access!");    \
      } else {                                                              \
        return tsl::errors::Internal("CUPTI call error", errstr);           \
      }                                                                     \
    }                                                                       \
  } while (false)

size_t Bytes2D(const CUDA_MEMCPY2D *p) { return p->Height * p->WidthInBytes; }

size_t Bytes3D(const CUDA_MEMCPY3D *p) {
  return p->Depth * p->Height * p->WidthInBytes;
}

template <typename CudaMemcpy>
CuptiTracerEventType MemcpyKind(const CudaMemcpy *p) {
  if (p->srcMemoryType == CU_MEMORYTYPE_HOST &&
      p->dstMemoryType == CU_MEMORYTYPE_DEVICE) {
    return CuptiTracerEventType::MemcpyH2D;
  }
  if (p->srcMemoryType == CU_MEMORYTYPE_DEVICE &&
      p->dstMemoryType == CU_MEMORYTYPE_HOST) {
    return CuptiTracerEventType::MemcpyD2H;
  }
  if (p->srcMemoryType == CU_MEMORYTYPE_DEVICE &&
      p->dstMemoryType == CU_MEMORYTYPE_DEVICE) {
    return CuptiTracerEventType::MemcpyD2D;
  }
  return CuptiTracerEventType::Unsupported;
}

std::tuple<size_t /*bytes*/, CuptiTracerEventType, bool /*async*/>
DecodeDriverMemcpy(CUpti_CallbackId cbid, const void *params) {
  switch (cbid) {
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyHtoD_v2: {
      const auto *p = reinterpret_cast<const cuMemcpyHtoD_v2_params *>(params);
      return std::make_tuple(p->ByteCount, CuptiTracerEventType::MemcpyH2D,
                             false);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyHtoDAsync_v2: {
      const auto *p =
          reinterpret_cast<const cuMemcpyHtoDAsync_v2_params *>(params);
      return std::make_tuple(p->ByteCount, CuptiTracerEventType::MemcpyH2D,
                             true);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoH_v2: {
      const auto *p = reinterpret_cast<const cuMemcpyDtoH_v2_params *>(params);
      return std::make_tuple(p->ByteCount, CuptiTracerEventType::MemcpyD2H,
                             false);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoHAsync_v2: {
      const auto *p =
          reinterpret_cast<const cuMemcpyDtoHAsync_v2_params *>(params);
      return std::make_tuple(p->ByteCount, CuptiTracerEventType::MemcpyD2H,
                             true);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoD_v2: {
      const auto *p = reinterpret_cast<const cuMemcpyDtoD_v2_params *>(params);
      return std::make_tuple(p->ByteCount, CuptiTracerEventType::MemcpyD2D,
                             false);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoDAsync_v2: {
      const auto *p =
          reinterpret_cast<const cuMemcpyDtoDAsync_v2_params *>(params);
      return std::make_tuple(p->ByteCount, CuptiTracerEventType::MemcpyD2D,
                             true);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpy: {
      const auto *p = reinterpret_cast<const cuMemcpy_params *>(params);
      return std::make_tuple(p->ByteCount, CuptiTracerEventType::MemcpyOther,
                             false);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyAsync: {
      const auto *p = reinterpret_cast<const cuMemcpyAsync_params *>(params);
      return std::make_tuple(p->ByteCount, CuptiTracerEventType::MemcpyOther,
                             true);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpy2D_v2: {
      const auto *p = reinterpret_cast<const cuMemcpy2D_v2_params *>(params);
      return std::make_tuple(Bytes2D(p->pCopy), MemcpyKind(p->pCopy), false);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpy2DAsync_v2: {
      const auto *p =
          reinterpret_cast<const cuMemcpy2DAsync_v2_params *>(params);
      return std::make_tuple(Bytes2D(p->pCopy), MemcpyKind(p->pCopy), true);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpy3D_v2: {
      const auto *p = reinterpret_cast<const cuMemcpy3D_v2_params *>(params);
      return std::make_tuple(Bytes3D(p->pCopy), MemcpyKind(p->pCopy), true);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpy3DAsync_v2: {
      const auto *p =
          reinterpret_cast<const cuMemcpy3DAsync_v2_params *>(params);
      return std::make_tuple(Bytes3D(p->pCopy), MemcpyKind(p->pCopy), true);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyPeer: {
      const auto *p2p_params =
          reinterpret_cast<const cuMemcpyPeer_params *>(params);
      return std::make_tuple(p2p_params->ByteCount,
                             CuptiTracerEventType::MemcpyP2P, false);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyPeerAsync: {
      const auto *p2p_params =
          reinterpret_cast<const cuMemcpyPeerAsync_params *>(params);
      return std::make_tuple(p2p_params->ByteCount,
                             CuptiTracerEventType::MemcpyP2P, true);
    }
    default: {
      LOG(ERROR) << "Unsupported memcpy activity observed: " << cbid;
      return std::make_tuple(0, CuptiTracerEventType::Unsupported, false);
    }
  }
}

std::tuple<size_t /*bytes*/, CuptiTracerEventType, bool /*async*/>
DecodeDriverMemset(CUpti_CallbackId cbid, const void *params) {
  switch (cbid) {
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD8_v2: {
      const auto *p = reinterpret_cast<const cuMemsetD8_v2_params *>(params);
      return std::make_tuple(p->N, CuptiTracerEventType::Memset, false);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD16_v2: {
      const auto *p = reinterpret_cast<const cuMemsetD16_v2_params *>(params);
      return std::make_tuple(p->N, CuptiTracerEventType::Memset, false);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD32_v2: {
      const auto *p = reinterpret_cast<const cuMemsetD32_v2_params *>(params);
      return std::make_tuple(p->N, CuptiTracerEventType::Memset, false);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD2D8_v2: {
      const auto *p = reinterpret_cast<const cuMemsetD2D8_v2_params *>(params);
      return std::make_tuple(p->dstPitch * p->Height,
                             CuptiTracerEventType::Memset, false);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD2D16_v2: {
      const auto *p = reinterpret_cast<const cuMemsetD2D16_v2_params *>(params);
      return std::make_tuple(p->dstPitch * p->Height,
                             CuptiTracerEventType::Memset, false);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD2D32_v2: {
      const auto *p = reinterpret_cast<const cuMemsetD2D32_v2_params *>(params);
      return std::make_tuple(p->dstPitch * p->Height,
                             CuptiTracerEventType::Memset, false);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD8Async: {
      const auto *p = reinterpret_cast<const cuMemsetD8Async_params *>(params);
      return std::make_tuple(p->N, CuptiTracerEventType::Memset, true);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD16Async: {
      const auto *p = reinterpret_cast<const cuMemsetD16Async_params *>(params);
      return std::make_tuple(p->N, CuptiTracerEventType::Memset, true);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD32Async: {
      const auto *p = reinterpret_cast<const cuMemsetD32Async_params *>(params);
      return std::make_tuple(p->N, CuptiTracerEventType::Memset, true);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD2D8Async: {
      const auto *p =
          reinterpret_cast<const cuMemsetD2D8Async_params *>(params);
      return std::make_tuple(p->dstPitch * p->Height,
                             CuptiTracerEventType::Memset, true);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD2D16Async: {
      const auto *p =
          reinterpret_cast<const cuMemsetD2D16Async_params *>(params);
      return std::make_tuple(p->dstPitch * p->Height,
                             CuptiTracerEventType::Memset, true);
    }
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD2D32Async: {
      const auto *p =
          reinterpret_cast<const cuMemsetD2D32Async_params *>(params);
      return std::make_tuple(p->dstPitch * p->Height,
                             CuptiTracerEventType::Memset, true);
    }
    default: {
      LOG(ERROR) << "Unsupported memset activity observed: " << cbid;
      return std::make_tuple(0, CuptiTracerEventType::Unsupported, false);
    }
  }
}

// Cupti callback corresponding to a driver or runtime API. This global function
// is invoked twice for each API: at entry and at exit. The cbdata
// parameter is guaranteed by Cupti to be thread-safe. Most invocations are
// dropped to the floor and entry/exit is tracked for the APIs we deem
// performance-relevant.
void CUPTIAPI ApiCallback(void *user_data, CUpti_CallbackDomain domain,
                          CUpti_CallbackId cbid,
                          const CUpti_CallbackData *cbdata) {
  CuptiTracer *tracer = reinterpret_cast<CuptiTracer *>(user_data);
  tracer->HandleCallback(domain, cbid, cbdata).IgnoreError();
}

// Callback which is invoked when an empty buffer is requested by CUPTI.
// Allocates an empty aligned-memory buffer. The buffer is used by CUPTI as a
// ring buffer where device maintains activity profiles that have been
// collected.
void CUPTIAPI RequestCuptiActivityBuffer(uint8_t **buffer, size_t *size,
                                         size_t *maxNumRecords) {
  CuptiTracer::GetCuptiTracerSingleton()->RequestActivityBuffer(buffer, size);
  VLOG(3) << "Requested CUPTI Buffer, buffer=" << std::hex
          << reinterpret_cast<uintptr_t>(*buffer) << std::dec
          << " size=" << *size;
  // Request CUPTI to fill as many records as possible in the buffer.
  *maxNumRecords = 0;
}

// Callback which is invoked when a buffer containing activity records is
// available from CUPTI. Processes the buffer after reading activity records
// from it.
void CUPTIAPI ProcessCuptiActivityBuffer(CUcontext context, uint32_t stream_id,
                                         uint8_t *buffer, size_t size,
                                         size_t valid_size) {
  VLOG(3) << "Processing CUPTI Buffer, buffer:" << std::hex
          << reinterpret_cast<uintptr_t>(buffer) << std::dec
          << " size: " << size << " valid_size: " << valid_size;
  VLOG(3) << "Activity profile for stream " << stream_id;

  absl::Status status =
      CuptiTracer::GetCuptiTracerSingleton()->ProcessActivityBuffer(
          context, stream_id, buffer, valid_size);
  if (!status.ok()) {
    LOG(ERROR) << status;
  }
}

void AddKernelEventUponApiExit(CuptiTraceCollector *collector,
                               uint32_t device_id,
                               const CUpti_CallbackData *cbdata,
                               uint64_t start_time, uint64_t end_time) {
  CuptiTracerEvent event{};
  event.type = CuptiTracerEventType::Kernel;
  event.source = CuptiTracerEventSource::DriverCallback;
  event.name = cbdata->symbolName ? cbdata->symbolName : cbdata->functionName;
  event.start_time_ns = start_time;
  event.end_time_ns = end_time;
  event.thread_id = Env::Default()->GetCurrentThreadId();
  event.device_id = device_id;
  event.context_id = cbdata->contextUid;
  event.correlation_id = cbdata->correlationId;
  VLOG(3) << "Cuda Kernel launch API exit. name=" << event.name;
  collector->AddEvent(std::move(event));
}

// Performs the actual callback for both normal and P2P memcpy operations.
CuptiTracerEvent PopulateMemcpyCallbackEvent(
    CuptiTracerEventType type, const CUpti_CallbackData *cbdata,
    size_t num_bytes, uint32_t src_device, uint32_t dst_device, bool async,
    uint64_t start_time, uint64_t end_time) {
  CuptiTracerEvent event{};
  event.type = type;
  event.source = CuptiTracerEventSource::DriverCallback;
  event.start_time_ns = start_time;
  event.end_time_ns = end_time;
  event.thread_id = Env::Default()->GetCurrentThreadId();
  event.device_id = src_device;
  event.context_id = cbdata->contextUid;
  event.correlation_id = cbdata->correlationId;
  event.memcpy_info.num_bytes = num_bytes;
  event.memcpy_info.destination = dst_device;
  event.memcpy_info.async = async;
  // These are not populated during callback for API activities.
  event.memcpy_info.copy_kind = CUPTI_ACTIVITY_MEMCPY_KIND_UNKNOWN;
  event.memcpy_info.dst_mem_kind = CUPTI_ACTIVITY_MEMORY_KIND_UNKNOWN;
  event.memcpy_info.src_mem_kind = CUPTI_ACTIVITY_MEMORY_KIND_UNKNOWN;
  return event;
}

void AddNormalMemcpyEventUponApiExit(CuptiTraceCollector *collector,
                                     uint32_t device_id, CUpti_CallbackId cbid,
                                     const CUpti_CallbackData *cbdata,
                                     uint64_t start_time, uint64_t end_time) {
  size_t num_bytes;
  CuptiTracerEventType type;
  bool async;
  std::tie(num_bytes, type, async) =
      DecodeDriverMemcpy(cbid, cbdata->functionParams);

  VLOG(3) << "Cuda Memcpy API exit. sz=" << num_bytes;
  CuptiTracerEvent event =
      PopulateMemcpyCallbackEvent(type, cbdata, num_bytes, device_id, device_id,
                                  async, start_time, end_time);
  collector->AddEvent(std::move(event));
}

void AddCuMemsetEventUponApiExit(CuptiTraceCollector *collector,
                                 uint32_t device_id, CUpti_CallbackId cbid,
                                 const CUpti_CallbackData *cbdata,
                                 uint64_t start_time, uint64_t end_time) {
  // We are casting all variants of cuMemset to cuMemsetD8 for accessing the
  // first member attribute, a CUdeviceptr.
  const auto *params =
      static_cast<const cuMemsetD8_v2_params *>(cbdata->functionParams);
  size_t num_bytes;
  bool async;
  CuptiTracerEventType type;
  std::tie(num_bytes, type, async) =
      DecodeDriverMemset(cbid, cbdata->functionParams);

  CuptiTracerEvent event{};
  event.type = type;
  event.source = CuptiTracerEventSource::DriverCallback;
  event.start_time_ns = start_time;
  event.end_time_ns = end_time;
  event.thread_id = Env::Default()->GetCurrentThreadId();
  event.device_id = device_id;
  event.context_id = cbdata->contextUid;
  event.correlation_id = cbdata->correlationId;
  event.memset_info.num_bytes = num_bytes;
  // memset_info.kind cannot be determined from API.
  event.memset_info.async = async;
  VLOG(3) << "Cuda Memset API exit."
          << " dptr=" << reinterpret_cast<void *>(params->dstDevice)
          << " sz=" << num_bytes;
  collector->AddEvent(std::move(event));
}

void AddP2PMemcpyEventUponApiExit(CuptiTraceCollector *collector,
                                  CuptiInterface *cupti_interface,
                                  uint32_t device_id, CUpti_CallbackId cbid,
                                  const CUpti_CallbackData *cbdata,
                                  uint64_t start_time, uint64_t end_time) {
  size_t num_bytes;
  CuptiTracerEventType type;
  bool async;
  std::tie(num_bytes, type, async) =
      DecodeDriverMemcpy(cbid, cbdata->functionParams);

  uint32_t dst_device = -1, src_device = -1;
  const auto *p2p_params =
      static_cast<const cuMemcpyPeer_params *>(cbdata->functionParams);
  cupti_interface->GetDeviceId(p2p_params->srcContext, &src_device);
  cupti_interface->GetDeviceId(p2p_params->dstContext, &dst_device);
  VLOG(3) << "Cuda P2P Memcpy API exit, src: " << src_device
          << " dst: " << dst_device << " size:" << num_bytes;
  CuptiTracerEvent event =
      PopulateMemcpyCallbackEvent(type, cbdata, num_bytes, src_device,
                                  dst_device, async, start_time, end_time);
  collector->AddEvent(std::move(event));
}

void AddCuMemAllocEventUponApiExit(CuptiTraceCollector *collector,
                                   uint32_t device_id, CUpti_CallbackId cbid,
                                   const CUpti_CallbackData *cbdata,
                                   uint64_t start_time, uint64_t end_time) {
  const auto *params =
      static_cast<const cuMemAlloc_v2_params *>(cbdata->functionParams);
  const void *dptr = reinterpret_cast<void *>(*params->dptr);
  CuptiTracerEvent event{};
  event.type = CuptiTracerEventType::MemoryAlloc;
  event.source = CuptiTracerEventSource::DriverCallback;
  event.name = cbdata->functionName;
  event.start_time_ns = start_time;
  event.end_time_ns = end_time;
  event.thread_id = Env::Default()->GetCurrentThreadId();
  event.device_id = device_id;
  event.context_id = cbdata->contextUid;
  event.correlation_id = cbdata->correlationId;
  event.memalloc_info.address = reinterpret_cast<uintptr_t>(dptr);
  event.memalloc_info.num_bytes = params->bytesize;
  VLOG(3) << "Cuda MemAlloc API exit." << " dptr=" << dptr
          << " sz=" << params->bytesize;
  collector->AddEvent(std::move(event));
}

void AddCuMemAllocPitchEventUponApiExit(
    CuptiTraceCollector *collector, uint32_t device_id, CUpti_CallbackId cbid,
    const CUpti_CallbackData *cbdata, uint64_t start_time, uint64_t end_time) {
  const auto *params =
      static_cast<const cuMemAllocPitch_v2_params *>(cbdata->functionParams);
  const void *dptr = reinterpret_cast<void *>(*params->dptr);
  CuptiTracerEvent event{};
  event.type = CuptiTracerEventType::MemoryAlloc;
  event.source = CuptiTracerEventSource::DriverCallback;
  event.name = cbdata->functionName;
  event.start_time_ns = start_time;
  event.end_time_ns = end_time;
  event.thread_id = Env::Default()->GetCurrentThreadId();
  event.device_id = device_id;
  event.context_id = cbdata->contextUid;
  event.correlation_id = cbdata->correlationId;
  const size_t size_in_bytes = *params->pPitch * params->Height;
  event.memalloc_info.address = reinterpret_cast<uintptr_t>(dptr);
  event.memalloc_info.num_bytes = size_in_bytes;
  VLOG(3) << "Cuda MemAllocPitch API exit." << " dptr=" << dptr
          << " sz=" << size_in_bytes;
  collector->AddEvent(std::move(event));
}

void AddCuMemAllocManagedEventUponApiExit(
    CuptiTraceCollector *collector, uint32_t device_id, CUpti_CallbackId cbid,
    const CUpti_CallbackData *cbdata, uint64_t start_time, uint64_t end_time) {
  const auto *params =
      static_cast<const cuMemAllocManaged_params *>(cbdata->functionParams);
  const void *dptr = reinterpret_cast<void *>(*params->dptr);
  CuptiTracerEvent event{};
  event.type = CuptiTracerEventType::MemoryAlloc;
  event.source = CuptiTracerEventSource::DriverCallback;
  event.name = cbdata->functionName;
  event.start_time_ns = start_time;
  event.end_time_ns = end_time;
  event.thread_id = Env::Default()->GetCurrentThreadId();
  event.device_id = device_id;
  event.context_id = cbdata->contextUid;
  event.correlation_id = cbdata->correlationId;
  event.memalloc_info.address = reinterpret_cast<uintptr_t>(dptr);
  event.memalloc_info.num_bytes = params->bytesize;
  VLOG(3) << "Cuda MemAllocManaged API exit." << " dptr=" << dptr
          << " sz=" << params->bytesize;
  collector->AddEvent(std::move(event));
}

void AddCuMemAllocHostEventUponApiExit(CuptiTraceCollector *collector,
                                       uint32_t device_id,
                                       CUpti_CallbackId cbid,
                                       const CUpti_CallbackData *cbdata,
                                       uint64_t start_time, uint64_t end_time) {
  const auto *params =
      static_cast<const cuMemAllocHost_v2_params *>(cbdata->functionParams);
  CuptiTracerEvent event{};
  event.type = CuptiTracerEventType::MemoryAlloc;
  event.source = CuptiTracerEventSource::DriverCallback;
  event.name = cbdata->functionName;
  event.start_time_ns = start_time;
  event.end_time_ns = end_time;
  event.thread_id = Env::Default()->GetCurrentThreadId();
  event.device_id = device_id;
  event.context_id = cbdata->contextUid;
  event.correlation_id = cbdata->correlationId;
  event.memalloc_info.address = reinterpret_cast<uintptr_t>(*params->pp);
  event.memalloc_info.num_bytes = params->bytesize;
  VLOG(3) << "Cuda MemAllocHost API exit." << " pp=" << *params->pp
          << " sz=" << params->bytesize;
  collector->AddEvent(std::move(event));
}

void AddCuMemHostAllocEventUponApiExit(CuptiTraceCollector *collector,
                                       uint32_t device_id,
                                       CUpti_CallbackId cbid,
                                       const CUpti_CallbackData *cbdata,
                                       uint64_t start_time, uint64_t end_time) {
  const auto *params =
      static_cast<const cuMemHostAlloc_params *>(cbdata->functionParams);
  CuptiTracerEvent event{};
  event.type = CuptiTracerEventType::MemoryAlloc;
  event.source = CuptiTracerEventSource::DriverCallback;
  event.name = cbdata->functionName;
  event.start_time_ns = start_time;
  event.end_time_ns = end_time;
  event.thread_id = Env::Default()->GetCurrentThreadId();
  event.device_id = device_id;
  event.context_id = cbdata->contextUid;
  event.correlation_id = cbdata->correlationId;
  event.memalloc_info.address = reinterpret_cast<uintptr_t>(*params->pp);
  event.memalloc_info.num_bytes = params->bytesize;
  VLOG(3) << "Cuda MemHostAlloc API exit." << " pp=" << *params->pp
          << " sz=" << params->bytesize << " Flags=" << params->Flags;
  collector->AddEvent(std::move(event));
}

void AddCuMemFreeEventUponApiExit(CuptiTraceCollector *collector,
                                  uint32_t device_id, CUpti_CallbackId cbid,
                                  const CUpti_CallbackData *cbdata,
                                  uint64_t start_time, uint64_t end_time) {
  const auto *params =
      static_cast<const cuMemFree_v2_params *>(cbdata->functionParams);
  const void *dptr = reinterpret_cast<void *>(params->dptr);
  CuptiTracerEvent event{};
  event.type = CuptiTracerEventType::MemoryFree;
  event.source = CuptiTracerEventSource::DriverCallback;
  event.name = cbdata->functionName;
  event.start_time_ns = start_time;
  event.end_time_ns = end_time;
  event.thread_id = Env::Default()->GetCurrentThreadId();
  event.device_id = device_id;
  event.context_id = cbdata->contextUid;
  event.correlation_id = cbdata->correlationId;
  event.memfree_info.address = reinterpret_cast<uintptr_t>(dptr);
  VLOG(3) << "Cuda MemFree API exit." << " dptr=" << dptr;
  collector->AddEvent(std::move(event));
}

void AddCuMemFreeHostEventUponApiExit(CuptiTraceCollector *collector,
                                      uint32_t device_id, CUpti_CallbackId cbid,
                                      const CUpti_CallbackData *cbdata,
                                      uint64_t start_time, uint64_t end_time) {
  const auto *params =
      static_cast<const cuMemFreeHost_params *>(cbdata->functionParams);
  CuptiTracerEvent event{};
  event.type = CuptiTracerEventType::MemoryFree;
  event.source = CuptiTracerEventSource::DriverCallback;
  event.name = cbdata->functionName;
  event.start_time_ns = start_time;
  event.end_time_ns = end_time;
  event.thread_id = Env::Default()->GetCurrentThreadId();
  event.device_id = device_id;
  event.context_id = cbdata->contextUid;
  event.correlation_id = cbdata->correlationId;
  event.memfree_info.address = reinterpret_cast<uintptr_t>(params->p);
  VLOG(3) << "Cuda MemFreeHost API exit." << " p=" << params->p;
  collector->AddEvent(std::move(event));
}

void AddCuMemHostRegisterEventUponApiExit(
    CuptiTraceCollector *collector, uint32_t device_id, CUpti_CallbackId cbid,
    const CUpti_CallbackData *cbdata, uint64_t start_time, uint64_t end_time) {
  const auto *params =
      static_cast<const cuMemHostRegister_v2_params *>(cbdata->functionParams);
  CuptiTracerEvent event{};
  event.type = CuptiTracerEventType::HostRegister;
  event.source = CuptiTracerEventSource::DriverCallback;
  event.name = cbdata->functionName;
  event.start_time_ns = start_time;
  event.end_time_ns = end_time;
  event.thread_id = Env::Default()->GetCurrentThreadId();
  event.device_id = device_id;
  event.context_id = cbdata->contextUid;
  event.correlation_id = cbdata->correlationId;
  event.host_register_info.address = reinterpret_cast<uintptr_t>(params->p);
  event.host_register_info.num_bytes = params->bytesize;
  event.host_register_info.flags = params->Flags;
  VLOG(3) << "Cuda HostRegister API exit." << " p=" << params->p
          << " bytesize=" << params->bytesize << " flags=" << params->Flags;
  collector->AddEvent(std::move(event));
}

void AddCuMemHostUnregisterEventUponApiExit(
    CuptiTraceCollector *collector, uint32_t device_id, CUpti_CallbackId cbid,
    const CUpti_CallbackData *cbdata, uint64_t start_time, uint64_t end_time) {
  const auto *params =
      static_cast<const cuMemHostUnregister_params *>(cbdata->functionParams);
  CuptiTracerEvent event{};
  event.type = CuptiTracerEventType::HostUnregister;
  event.source = CuptiTracerEventSource::DriverCallback;
  event.name = cbdata->functionName;
  event.start_time_ns = start_time;
  event.end_time_ns = end_time;
  event.thread_id = Env::Default()->GetCurrentThreadId();
  event.device_id = device_id;
  event.context_id = cbdata->contextUid;
  event.correlation_id = cbdata->correlationId;
  event.host_unregister_info.address = reinterpret_cast<uintptr_t>(params->p);
  VLOG(3) << "Cuda HostUnregister API exit." << " p=" << params->p;
  collector->AddEvent(std::move(event));
}

void AddGenericEventUponApiExit(CuptiTraceCollector *collector,
                                uint32_t device_id, CUpti_CallbackId cbid,
                                const CUpti_CallbackData *cbdata,
                                uint64_t start_time, uint64_t end_time) {
  CuptiTracerEvent event{};
  event.type = CuptiTracerEventType::Generic;
  event.source = CuptiTracerEventSource::DriverCallback;
  event.name = cbdata->functionName;
  event.start_time_ns = start_time;
  event.end_time_ns = end_time;
  event.thread_id = Env::Default()->GetCurrentThreadId();
  event.device_id = device_id;
  event.context_id = cbdata->contextUid;
  event.correlation_id = cbdata->correlationId;
  VLOG(3) << "Observed generic API exit." << " name=" << cbdata->functionName;
  collector->AddEvent(std::move(event));
}

// This hook uses cupti activity api to measure device side activities.
class CuptiDriverApiHookWithActivityApi : public CuptiDriverApiHook {
 public:
  CuptiDriverApiHookWithActivityApi(const CuptiTracerOptions &option,
                                    CuptiInterface *cupti_interface,
                                    CuptiTraceCollector *collector)
      : option_(option),
        cupti_interface_(cupti_interface),
        collector_(collector) {}

  absl::Status OnDriverApiEnter(int device_id, CUpti_CallbackDomain domain,
                                CUpti_CallbackId cbid,
                                const CUpti_CallbackData *cbdata) override {
    // Stash away the current Cupti timestamp into cbdata.
    *cbdata->correlationData =
        option_.required_callback_api_events ? CuptiTracer::GetTimestamp() : 0;
    return OkStatus();
  }
  absl::Status OnDriverApiExit(int device_id, CUpti_CallbackDomain domain,
                               CUpti_CallbackId cbid,
                               const CUpti_CallbackData *cbdata) override {
    // If we are not collecting CPU events from Callback API, we can return now.
    if (!option_.required_callback_api_events) {
      return OkStatus();
    }

    // Grab timestamp for API exit. API entry timestamp saved in cbdata.
    uint64_t end_tsc = CuptiTracer::GetTimestamp();
    uint64_t start_tsc = *cbdata->correlationData;
    TrackContext(cbid, cbdata->context);
    return AddDriverApiCallbackEvent(collector_, cupti_interface_, device_id,
                                     start_tsc, end_tsc, domain, cbid, cbdata);
  }
  absl::Status SyncAndFlush() override {
    if (option_.sync_devices_before_stop) {
      CuptiApiTracingDisabler disabler;
      absl::MutexLock lock(&mutex_);
      for (auto &ctx : contexts_) {
        cuCtxPushCurrent(ctx);
        cuCtxSynchronize();  // Ignore error here for best effort.
        CUcontext current;
        cuCtxPopCurrent(&current);
      }
    }
    return OkStatus();
  }

 private:
  void TrackContext(CUpti_CallbackId cbid, CUcontext ctx) {
    if (!option_.sync_devices_before_stop) return;
    if (ctx == nullptr) return;
    absl::MutexLock lock(&mutex_);
    if (cbid == CUPTI_DRIVER_TRACE_CBID_cuCtxDestroy_v2 ||
        cbid == CUPTI_DRIVER_TRACE_CBID_cuCtxDestroy) {
      contexts_.erase(ctx);
    } else {
      contexts_.emplace(ctx);
    }
  }

  const CuptiTracerOptions option_;
  CuptiInterface *cupti_interface_;
  CuptiTraceCollector *collector_;
  absl::Mutex mutex_;
  absl::flat_hash_set<CUcontext> contexts_ TF_GUARDED_BY(mutex_);

  CuptiDriverApiHookWithActivityApi(const CuptiDriverApiHookWithActivityApi &) =
      delete;
  void operator=(const CuptiDriverApiHookWithActivityApi &) = delete;
};

/*static*/ std::string ErrorWithHostname(absl::string_view error_message) {
  return absl::StrCat(tsl::port::Hostname(), ": ", error_message);
}

}  // namespace

/*static*/ absl::Status CuptiDriverApiHook::AddDriverApiCallbackEvent(
    CuptiTraceCollector *collector, CuptiInterface *cupti_interface,
    int device_id, uint64_t start_tsc, uint64_t end_tsc,
    CUpti_CallbackDomain domain, CUpti_CallbackId cbid,
    const CUpti_CallbackData *cbdata) {
  switch (cbid) {
    case CUPTI_DRIVER_TRACE_CBID_cuLaunchKernel:
#if CUDA_VERSION >= 11080  // CUDA 11.8
    case CUPTI_DRIVER_TRACE_CBID_cuLaunchKernelEx:
#endif  // CUDA_VERSION >= 11080
    case CUPTI_DRIVER_TRACE_CBID_cuLaunchCooperativeKernel:
    case CUPTI_DRIVER_TRACE_CBID_cuLaunchCooperativeKernelMultiDevice:
      AddKernelEventUponApiExit(collector, device_id, cbdata, start_tsc,
                                end_tsc);
      break;
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpy:
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyAsync:
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyHtoD_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyHtoDAsync_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoH_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoHAsync_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoD_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoDAsync_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyAtoH_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyAtoHAsync_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyAtoD_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyDtoA_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyAtoA_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpy2D_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpy2DUnaligned_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpy2DAsync_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpy3D_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpy3DAsync_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyHtoA_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyHtoAAsync_v2:
      // This would be the place to populate the memcpy API activity's src and
      // dst memory kind by casting cbdata->functionParams. However, we are not
      // doing that because that will incur significant overhead to get the
      // memory aperture of each argument.
      AddNormalMemcpyEventUponApiExit(collector, device_id, cbid, cbdata,
                                      start_tsc, end_tsc);
      break;
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyPeer:
    case CUPTI_DRIVER_TRACE_CBID_cuMemcpyPeerAsync:
      AddP2PMemcpyEventUponApiExit(collector, cupti_interface, device_id, cbid,
                                   cbdata, start_tsc, end_tsc);
      break;
    case CUPTI_DRIVER_TRACE_CBID_cuMemAlloc_v2:
      AddCuMemAllocEventUponApiExit(collector, device_id, cbid, cbdata,
                                    start_tsc, end_tsc);
      break;
    case CUPTI_DRIVER_TRACE_CBID_cuMemAllocPitch_v2:
      AddCuMemAllocPitchEventUponApiExit(collector, device_id, cbid, cbdata,
                                         start_tsc, end_tsc);
      break;
    case CUPTI_DRIVER_TRACE_CBID_cuMemAllocManaged:
      AddCuMemAllocManagedEventUponApiExit(collector, device_id, cbid, cbdata,
                                           start_tsc, end_tsc);
      break;
    case CUPTI_DRIVER_TRACE_CBID_cuMemAllocHost_v2:
      AddCuMemAllocHostEventUponApiExit(collector, device_id, cbid, cbdata,
                                        start_tsc, end_tsc);
      break;
    case CUPTI_DRIVER_TRACE_CBID_cuMemHostAlloc:
      AddCuMemHostAllocEventUponApiExit(collector, device_id, cbid, cbdata,
                                        start_tsc, end_tsc);
      break;
    case CUPTI_DRIVER_TRACE_CBID_cuMemFree_v2:
      AddCuMemFreeEventUponApiExit(collector, device_id, cbid, cbdata,
                                   start_tsc, end_tsc);
      break;
    case CUPTI_DRIVER_TRACE_CBID_cuMemFreeHost:
      AddCuMemFreeHostEventUponApiExit(collector, device_id, cbid, cbdata,
                                       start_tsc, end_tsc);
      break;
    case CUPTI_DRIVER_TRACE_CBID_cuMemHostRegister_v2:
      AddCuMemHostRegisterEventUponApiExit(collector, device_id, cbid, cbdata,
                                           start_tsc, end_tsc);
      break;
    case CUPTI_DRIVER_TRACE_CBID_cuMemHostUnregister:
      AddCuMemHostUnregisterEventUponApiExit(collector, device_id, cbid, cbdata,
                                             start_tsc, end_tsc);
      break;
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD8_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD16_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD32_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD2D8_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD2D16_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD2D32_v2:
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD8Async:
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD16Async:
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD32Async:
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD2D8Async:
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD2D16Async:
    case CUPTI_DRIVER_TRACE_CBID_cuMemsetD2D32Async:
      AddCuMemsetEventUponApiExit(collector, device_id, cbid, cbdata, start_tsc,
                                  end_tsc);
      break;
    default:
      AddGenericEventUponApiExit(collector, device_id, cbid, cbdata, start_tsc,
                                 end_tsc);
      break;
  }
  return OkStatus();
}

const char *GetTraceEventTypeName(const CuptiTracerEventType &type) {
  // Do not use a default so that this gives a build error when
  // CuptiTracerEventType is extended but this is not.
  switch (type) {
    case CuptiTracerEventType::MemcpyH2D:
      return "MemcpyH2D";
    case CuptiTracerEventType::MemcpyD2H:
      return "MemcpyD2H";
    case CuptiTracerEventType::MemcpyD2D:
      return "MemcpyD2D";
    case CuptiTracerEventType::MemcpyP2P:
      return "MemcpyP2P";
    case CuptiTracerEventType::MemcpyOther:
      return "MemcpyOther";
    case CuptiTracerEventType::Kernel:
      return "Compute";
    case CuptiTracerEventType::MemoryAlloc:
      return "MemoryAlloc";
    case CuptiTracerEventType::MemoryFree:
      return "MemoryFree";
    case CuptiTracerEventType::Memset:
      return "Memset";
    case CuptiTracerEventType::Overhead:
      return "Overhead";
    case CuptiTracerEventType::UnifiedMemory:
      return "UnifiedMemory";
    case CuptiTracerEventType::Generic:
      return "Generic";
    case CuptiTracerEventType::MemoryResidency:
      return "MemoryResidency";
    case CuptiTracerEventType::HostRegister:
      return "HostRegister";
    case CuptiTracerEventType::HostUnregister:
      return "HostUnregister";
    case CuptiTracerEventType::Unsupported:
      return "";
  }
}

CuptiTracer::CuptiTracer(CuptiInterface *cupti_interface)
    : num_gpus_(NumGpus()), cupti_interface_(cupti_interface) {}

/* static */ CuptiTracer *CuptiTracer::GetCuptiTracerSingleton() {
  static auto *singleton = new CuptiTracer(GetCuptiInterface());
  return singleton;
}

bool CuptiTracer::IsAvailable() const {
  return NumGpus() && !activity_tracing_enabled_ && !api_tracing_enabled_;
}

int CuptiTracer::NumGpus() {
  static int num_gpus = []() -> int {
    if (cuInit(0) != CUDA_SUCCESS) {
      return 0;
    }
    int gpu_count;
    if (cuDeviceGetCount(&gpu_count) != CUDA_SUCCESS) {
      return 0;
    }
    LOG(INFO) << "Profiler found " << gpu_count << " GPUs";
    return gpu_count;
  }();
  return num_gpus;
}

void CuptiTracer::Enable(const CuptiTracerOptions &option,
                         CuptiTraceCollector *collector) {
  option_ = option;
  collector_ = collector;

  cupti_driver_api_hook_.reset(new CuptiDriverApiHookWithActivityApi(
      option, cupti_interface_, collector));

  absl::Status status = EnableApiTracing();
  need_root_access_ |= status.code() == tsl::error::PERMISSION_DENIED;
  if (!status.ok()) return;

  EnableActivityTracing().IgnoreError();
  tsl::profiler::AnnotationStack::Enable(true);
}

void CuptiTracer::Disable() {
  DisableApiTracing().IgnoreError();
  DisableActivityTracing().IgnoreError();
  cupti_interface_->CleanUp();
  Finalize().IgnoreError();
  cupti_driver_api_hook_->SyncAndFlush().IgnoreError();

  collector_->OnTracerCachedActivityBuffers(std::move(activity_buffers_));
  if (cupti_dropped_activity_event_count_ > 0) {
    collector_->OnEventsDropped("Activity Event dropped by Cupti Lib:",
                                cupti_dropped_activity_event_count_);
  }
  if (num_activity_events_in_dropped_buffer_ > 0) {
    collector_->OnEventsDropped("Activity Event dropped in dropped buffer:",
                                num_activity_events_in_dropped_buffer_);
  }

  collector_->Flush();
  collector_ = nullptr;
  option_.reset();
  cupti_driver_api_hook_.reset();
  tsl::profiler::AnnotationStack::Enable(false);
}

absl::Status CuptiTracer::EnableApiTracing() {
  if (api_tracing_enabled_) return OkStatus();

  VLOG(1) << "Enable subscriber";
  // Subscribe can return CUPTI_ERROR_MAX_LIMIT_REACHED.
  // The application which calls CUPTI APIs cannot be used with Nvidia tools
  // like nvprof, Nvidia Visual Profiler, Nsight Compute, Nsight Systems.
  RETURN_IF_CUPTI_ERROR(cupti_interface_->Subscribe(
      &subscriber_, (CUpti_CallbackFunc)ApiCallback, this));
  api_tracing_enabled_ = true;

  if (!option_->cbids_selected.empty()) {
    for (auto cbid : option_->cbids_selected) {
      RETURN_IF_CUPTI_ERROR(cupti_interface_->EnableCallback(
          1 /* ENABLE */, subscriber_, CUPTI_CB_DOMAIN_DRIVER_API, cbid));
    }
  } else {  // select all callback ids.
    RETURN_IF_CUPTI_ERROR(cupti_interface_->EnableDomain(
        1 /* ENABLE */, subscriber_, CUPTI_CB_DOMAIN_DRIVER_API));
  }

  if (option_->enable_nvtx_tracking) {
    RETURN_IF_CUPTI_ERROR(cupti_interface_->EnableDomain(
        1 /* ENABLE */, subscriber_, CUPTI_CB_DOMAIN_NVTX));
  }
  return OkStatus();
}

absl::Status CuptiTracer::DisableApiTracing() {
  if (!api_tracing_enabled_) return OkStatus();

  api_tracing_enabled_ = false;

  if (!option_->cbids_selected.empty()) {
    for (auto cbid : option_->cbids_selected) {
      RETURN_IF_CUPTI_ERROR(cupti_interface_->EnableCallback(
          0 /* DISABLE */, subscriber_, CUPTI_CB_DOMAIN_DRIVER_API, cbid));
    }
  } else {
    RETURN_IF_CUPTI_ERROR(cupti_interface_->EnableDomain(
        0 /* DISABLE */, subscriber_, CUPTI_CB_DOMAIN_DRIVER_API));
  }

  if (option_->enable_nvtx_tracking) {
    RETURN_IF_CUPTI_ERROR(cupti_interface_->EnableDomain(
        0 /* DISABLE */, subscriber_, CUPTI_CB_DOMAIN_NVTX));
  }

  VLOG(1) << "Disable subscriber";
  RETURN_IF_CUPTI_ERROR(cupti_interface_->Unsubscribe(subscriber_));
  return OkStatus();
}

absl::Status CuptiTracer::EnableActivityTracing() {
  if (activity_tracing_enabled_) return OkStatus();
  PrepareActivityStart();
  if (!option_->activities_selected.empty()) {
    // Initialize callback functions for Cupti Activity API.
    VLOG(1) << "Registering CUPTI activity callbacks";
    if (auto err = cupti_interface_->ActivityUsePerThreadBuffer();
        err != CUPTI_SUCCESS) {
      LOG(WARNING) << "Fail to use per-thread activity buffer, cupti trace "
                      "overhead may be big. CUPTI ERROR CODE:"
                   << err;
    }
    RETURN_IF_CUPTI_ERROR(cupti_interface_->ActivityRegisterCallbacks(
        RequestCuptiActivityBuffer, ProcessCuptiActivityBuffer));

    VLOG(1) << "Enabling activity tracing for "
            << option_->activities_selected.size() << " activities";
    for (auto activity : option_->activities_selected) {
      VLOG(1) << "Enabling activity tracing for: " << activity;
      if (activity == CUPTI_ACTIVITY_KIND_UNIFIED_MEMORY_COUNTER) {
        ConfigureActivityUnifiedMemoryCounter(true);
      }
      RETURN_IF_CUPTI_ERROR(cupti_interface_->ActivityEnable(activity));
    }
  }
  activity_tracing_enabled_ = true;
  return OkStatus();
}

absl::Status CuptiTracer::DisableActivityTracing() {
  if (activity_tracing_enabled_) {
    VLOG(1) << "Disabling activity tracing for "
            << option_->activities_selected.size() << " activities";
    for (auto activity : option_->activities_selected) {
      VLOG(1) << "Disabling activity tracing for: " << activity;
      if (activity == CUPTI_ACTIVITY_KIND_UNIFIED_MEMORY_COUNTER) {
        ConfigureActivityUnifiedMemoryCounter(false);
      }
      RETURN_IF_CUPTI_ERROR(cupti_interface_->ActivityDisable(activity));
    }
    option_->activities_selected.clear();

    VLOG(1) << "Flushing CUPTI activity buffer";
    RETURN_IF_CUPTI_ERROR(
        cupti_interface_->ActivityFlushAll(CUPTI_ACTIVITY_FLAG_FLUSH_FORCED));
    LOG(INFO) << "CUPTI activity buffer flushed";
  }
  activity_tracing_enabled_ = false;
  return OkStatus();
}

absl::Status CuptiTracer::Finalize() {
  if (option_->cupti_finalize) {
    VLOG(1) << "CuptiFinalize";
    RETURN_IF_CUPTI_ERROR(cupti_interface_->Finalize());
  }
  return OkStatus();
}

/*static*/ uint64_t CuptiTracer::GetTimestamp() {
  uint64_t tsc;
  CuptiInterface *cupti_interface = GetCuptiInterface();
  if (cupti_interface && cupti_interface->GetTimestamp(&tsc) == CUPTI_SUCCESS) {
    return tsc;
  }
  // Return 0 on error. If an activity timestamp is 0, the activity will be
  // dropped during time normalization.
  return 0;
}

absl::Status CuptiTracer::HandleNVTXCallback(CUpti_CallbackId cbid,
                                             const CUpti_CallbackData *cbdata) {
  const CUpti_NvtxData *pdata =
      reinterpret_cast<const CUpti_NvtxData *>(cbdata);
  if (cbid == CUPTI_CBID_NVTX_nvtxDomainRangePushEx) {
    const nvtxDomainRangePushEx_params *params =
        reinterpret_cast<const nvtxDomainRangePushEx_params *>(
            pdata->functionParams);
    // TODO(profiler): The messageType is actually NVTX_MESSAGE_TYPE_REGISTERED
    // (which is 3), However it seems to me that we can not get the registered
    // string from nvtxDomainRegisterStringA_params. If we reinterpret the
    // payload as ascii, it happen to work.
    NVTXRangeTracker::EnterRange(params->core.eventAttrib->message.ascii);
  } else if (cbid == CUPTI_CBID_NVTX_nvtxDomainRangePop) {
    NVTXRangeTracker::ExitRange();
  }
  return OkStatus();
}

absl::Status CuptiTracer::HandleCallback(CUpti_CallbackDomain domain,
                                         CUpti_CallbackId cbid,
                                         const CUpti_CallbackData *cbdata) {
  if (!api_tracing_enabled_) return OkStatus();    // already unsubscribed.
  if (!cupti_driver_api_hook_) return OkStatus();  // already unsubscribed.
  if (domain == CUPTI_CB_DOMAIN_NVTX) return HandleNVTXCallback(cbid, cbdata);
  if (domain != CUPTI_CB_DOMAIN_DRIVER_API) return OkStatus();
  if (internalCuCall) return OkStatus();

  if (cbdata->context == nullptr) {
    // API callback is called before any CUDA context is created.
    // This is expected to be rare, and we ignore this case.
    VLOG(3) << "API callback received before creation of CUDA context\n";
    return tsl::errors::Internal("cutpi callback without context");
  }

  // Grab a correct device ID.
  uint32_t device_id = -1;
  RETURN_IF_CUPTI_ERROR(
      cupti_interface_->GetDeviceId(cbdata->context, &device_id));
  if (device_id >= num_gpus_) {
    return tsl::errors::Internal("Invalid device id:", device_id);
  }

  if (cbdata->callbackSite == CUPTI_API_ENTER) {
    TF_RETURN_IF_ERROR(cupti_driver_api_hook_->OnDriverApiEnter(
        device_id, domain, cbid, cbdata));
  } else if (cbdata->callbackSite == CUPTI_API_EXIT) {
    // Set up the map from correlation id to annotation string.
    const auto &annotation = AnnotationStack::Get();
    if (!annotation.empty()) {
      if (cbid ==
          CUPTI_DRIVER_TRACE_CBID_cuLaunchCooperativeKernelMultiDevice) {
        // Kernels are launched on different devices by this API call, therefore
        // we need to populate per device annotation map respectively.
        for (int i = 0; i < num_gpus_; ++i) {
          collector_->annotation_map()->Add(i, cbdata->correlationId,
                                            annotation, "");
        }
      } else {
        absl::string_view nvtx_range = NVTXRangeTracker::CurrentRange();
        collector_->annotation_map()->Add(device_id, cbdata->correlationId,
                                          annotation, nvtx_range);
      }
    }

    TF_RETURN_IF_ERROR(cupti_driver_api_hook_->OnDriverApiExit(
        device_id, domain, cbid, cbdata));
  }
  return OkStatus();
}

void CuptiTracer::ConfigureActivityUnifiedMemoryCounter(bool enable) {
  CUpti_ActivityUnifiedMemoryCounterConfig config[2];
  // By experiments, currently only measurements from these two activities are
  // trustworthy. Others like GPU page fault may be problematic.
  config[0].kind =
      CUPTI_ACTIVITY_UNIFIED_MEMORY_COUNTER_KIND_BYTES_TRANSFER_HTOD;
  config[1].kind =
      CUPTI_ACTIVITY_UNIFIED_MEMORY_COUNTER_KIND_BYTES_TRANSFER_DTOH;

  for (size_t i = 0; i < 2; i++) {
    config[i].enable = enable;
  }

  CUptiResult res;

  res = cupti_interface_->ActivityConfigureUnifiedMemoryCounter(config, 2);
  if (res == CUPTI_ERROR_UM_PROFILING_NOT_SUPPORTED) {
    LOG(ERROR) << "Unified memory is not supported on the "
                  "underlying platform.\n";
  } else if (res == CUPTI_ERROR_UM_PROFILING_NOT_SUPPORTED_ON_DEVICE) {
    LOG(ERROR) << "Unified memory is not supported on the device.\n";
  } else if (res == CUPTI_ERROR_UM_PROFILING_NOT_SUPPORTED_ON_NON_P2P_DEVICES) {
    LOG(ERROR) << "Unified memory is not supported on the "
                  "non-P2P multi-gpu setup.\n";
  } else if (res != CUPTI_SUCCESS) {
    const char *errstr = "";
    cuptiGetResultString(res, &errstr);
    LOG(ERROR) << "Error while enabling unified memory profiling: " << errstr;
  } else {
    VLOG(1) << "Configuring Unified memory profiling: " << res;
  }
}

void CuptiTracer::RequestActivityBuffer(uint8_t **buffer, size_t *size) {
  *buffer = activity_buffers_->GetOrCreateBuffer();
  if (*buffer == nullptr) {
    LOG(WARNING)
        << "CUPTI Buffer not allocated, activity records will be dropped";
    *size = 0;
    return;
  }
  *size = activity_buffers_->GetBufferSizeInBytes();
}

static size_t CountCuptiActivityEvent(uint8_t *buffer, size_t size) {
  size_t total_event_count = 0;
  if (size == 0 || buffer == nullptr) return total_event_count;
  CuptiInterface *cupti_interface = GetCuptiInterface();
  CUpti_Activity *record = nullptr;
  while (true) {
    if (cupti_interface->ActivityGetNextRecord(buffer, size, &record) ==
        CUPTI_SUCCESS) {
      ++total_event_count;
    } else {
      break;
    }
  }
  return total_event_count;
}

absl::Status CuptiTracer::ProcessActivityBuffer(CUcontext context,
                                                uint32_t stream_id,
                                                uint8_t *buffer, size_t size) {
  absl::Cleanup buffer_cleanup = [&]() {
    if (buffer) activity_buffers_->ReclaimBuffer(buffer);
  };
  if (size == 0 || buffer == nullptr) {
    return OkStatus();
  }
  if (!activity_tracing_enabled_) {
    LOG(WARNING) << "CUPTI activity buffer is reclaimed after flush.";
    return OkStatus();
  }
  if (cupti_interface_->Disabled()) return tsl::errors::Internal("Disabled.");

  // Report dropped records.
  size_t dropped = 0;
  if (cupti_interface_->ActivityGetNumDroppedRecords(
          context, stream_id, &dropped) == CUPTI_SUCCESS) {
    cupti_dropped_activity_event_count_ += dropped;
  }

  size_t event_count_in_buffer = CountCuptiActivityEvent(buffer, size);
  auto max_activity_event_count =
      collector_->GetOptions().max_activity_api_events;
  if (max_activity_event_count > 0 &&
      num_activity_events_in_cached_buffer_ >= max_activity_event_count) {
    LOG(WARNING) << "Already too many activity events, drop the buffer of "
                 << size << "bytes of event to reuse.";
    num_activity_events_in_dropped_buffer_ += event_count_in_buffer;
    // buffer will be return to the pool
    return OkStatus();
  }
  num_activity_events_in_cached_buffer_ += event_count_in_buffer;

  // When cupti activity buffer is required to flush, save the buffer and its
  // valid size some where. All the saved activity buffer will be handled
  // after the profiling is stopped.
  VLOG(3) << "Caching CUPTI activity buffer of size:" << size;
  activity_buffers_->CacheCuptiFilledActivityBuffer(buffer, size);
  buffer = nullptr;  // So cleanup will not free it as it was saved already

  return OkStatus();
}

/*static*/ std::string CuptiTracer::ErrorIfAny() {
  if (CuptiTracer::NumGpus() == 0) {
    return ErrorWithHostname("No GPU detected.");
  } else if (CuptiTracer::GetCuptiTracerSingleton()->NeedRootAccess()) {
    return ErrorWithHostname(
        "Insufficient privilege to run libcupti (you need root permission).");
  } else if (CuptiTracer::GetTimestamp() == 0) {
    return ErrorWithHostname(
        "Failed to load libcupti (is it installed and accessible?)");
  }
  return "";
}

void CuptiTracer::PrepareActivityStart() {
  activity_buffers_ =
      std::make_unique<CuptiActivityBufferManager>(kBufferSizeInBytes);
  cupti_dropped_activity_event_count_ = 0;
  num_activity_events_in_cached_buffer_ = 0;
  num_activity_events_in_dropped_buffer_ = 0;
}

}  // namespace profiler
}  // namespace xla
