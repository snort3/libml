/* Copyright 2022 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/xla/runtime/async_runtime.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <type_traits>
#include <utility>

#include "absl/base/dynamic_annotations.h"
#include "tensorflow/tsl/platform/mem.h"
#include "tfrt/host_context/async_dispatch.h"  // from @tf_runtime
#include "tfrt/host_context/async_value.h"  // from @tf_runtime
#include "tfrt/host_context/async_value_ref.h"  // from @tf_runtime
#include "tfrt/host_context/chain.h"  // from @tf_runtime

// -------------------------------------------------------------------------- //
// Define AsyncToken and AsyncGroup in the mlir::runtime namespace to implement
// opaque structs defined in the MLIR Async Runtime API header file.
// -------------------------------------------------------------------------- //

namespace mlir {
namespace runtime {

using tfrt::AsyncValueOwningRef;
using tfrt::Chain;
using tfrt::MakeAvailableAsyncValueRef;
using tfrt::MakeConstructedAsyncValueRef;
using tfrt::internal::AsyncValueStorage;

using xla::runtime::AsyncRuntimeObject;

using tsl::port::AlignedFree;
using tsl::port::AlignedMalloc;

class AsyncToken : public AsyncRuntimeObject {
 public:
  explicit AsyncToken(unsigned ref_count = 1)
      : AsyncRuntimeObject(ref_count),
        chain_(MakeConstructedAsyncValueRef<Chain>(storage_)) {}

  tfrt::AsyncValue* GetAsyncValue() const { return chain_.AsPtr().value(); }

 private:
  AsyncValueStorage<Chain> storage_;
  AsyncValueOwningRef<Chain> chain_;
};

class AsyncValue : public AsyncRuntimeObject {
 public:
  explicit AsyncValue(size_t size, size_t alignment, unsigned ref_count = 1)
      : AsyncRuntimeObject(ref_count),
        data_storage_(size, alignment),
        chain_(MakeConstructedAsyncValueRef<Chain>(storage_)) {
    // Storage memory will be initialized by the compiled executable.
    ABSL_ANNOTATE_MEMORY_IS_INITIALIZED(GetStorage(), size);
  }

  void* GetStorage() {
    assert(!GetAsyncValue()->IsError() && "unexpected error state");
    if (data_storage_.is_inline)
      return reinterpret_cast<void*>(&data_storage_.inline_buffer[0]);
    return data_storage_.allocated_buffer;
  }

  tfrt::AsyncValue* GetAsyncValue() const { return chain_.AsPtr().value(); }

 private:
  // If the requested async value storage is small, use the inlined storage.
  // Fall back on dynamic allocation if the requested storage size is large.
  struct Storage {
    static const int kSize = 128;  // enough to fit memref descriptor of rank 5
    static const int kAlign = alignof(std::max_align_t);

    Storage(size_t size, size_t alignment)
        : is_inline(CanStoreInline(size, alignment)) {
      if (!is_inline) allocated_buffer = AlignedMalloc(size, alignment);
    }

    ~Storage() {
      if (!is_inline) AlignedFree(allocated_buffer);
    }

    static bool CanStoreInline(size_t size, size_t alignment) {
      assert(absl::has_single_bit(alignment));
      return size <= kSize && alignment <= kAlign;
    }

    bool is_inline;
    union {
      alignas(kAlign) std::array<std::byte, kSize> inline_buffer;
      void* allocated_buffer;
    };
  };

  Storage data_storage_;

  // Async value that tracks value readiness. It becomes available when result
  // is written to the data storage and ready for consumption.
  AsyncValueStorage<Chain> storage_;
  AsyncValueOwningRef<Chain> chain_;
};

class AsyncGroup : public AsyncRuntimeObject {
 public:
  explicit AsyncGroup(int64_t size, unsigned ref_count = 1)
      : AsyncRuntimeObject(ref_count),
        size_(size),
        rank_(0),
        pending_tokens_(size),
        num_errors_(0),
        completed_(size_ == 0 ? MakeAvailableAsyncValueRef<Chain>(storage_)
                              : MakeConstructedAsyncValueRef<Chain>(storage_)) {
    assert(size_ >= 0 && "size can't be negative");
  }

  size_t AddToken(AsyncToken* token) {
    size_t rank = rank_.fetch_add(1, std::memory_order_relaxed);
    assert(rank < size_ && "can't add more tokens than the group size");

    // When token becomes available drop the number of pending tokens and maybe
    // make the group completion async value available.
    token->GetAsyncValue()->AndThen([group = this, token]() {
      // Increment the number of errors in the group.
      if (token->GetAsyncValue()->IsError()) group->num_errors_.fetch_add(1);

      // Pending tokens can't drop below zero.
      assert(group->pending_tokens_ > 0 && "wrong group size");

      // We do track group error state with the number of errors, and never
      // set completion async value state to error.
      if (group->pending_tokens_.fetch_sub(1) == 1)
        group->completed_.AsPtr().SetStateConcrete();
    });

    return rank;
  }

  tfrt::AsyncValue* GetCompletionAsyncValue() const {
    return completed_.AsPtr().value();
  }

  bool IsError() const { return num_errors_.load() != 0; }

 private:
  int64_t size_;
  std::atomic<int64_t> rank_;
  std::atomic<int64_t> pending_tokens_;
  std::atomic<int64_t> num_errors_;

  // Async value that keeps track the group completion, it will become available
  // when the number of pending tokens will drop to zero.
  AsyncValueStorage<Chain> storage_;
  AsyncValueOwningRef<Chain> completed_;
};

}  // namespace runtime
}  // namespace mlir

// -------------------------------------------------------------------------- //

namespace xla {
namespace runtime {

using tfrt::AsyncValue;

namespace {
// Always keep the current active async runtime in a thread local variable.
static thread_local AsyncRuntime async_runtime;

static_assert(std::is_trivially_destructible<AsyncRuntime>::value,
              "AsyncRuntime must be trivially destructible");

static_assert(std::is_trivially_copy_assignable<AsyncRuntime>::value,
              "AsyncRuntime must be trivially copy assignable");

static_assert(std::is_trivially_copy_constructible<AsyncRuntime>::value,
              "AsyncRuntime must be trivially copy constructible");

// This is an arbitrary limitation, to make sure that AsyncRuntime would not
// become expensive to copy unnoticed.
static_assert(sizeof(AsyncRuntime) == 1 * sizeof(void*),
              "AsyncRuntime must only hold one pointer");

}  // namespace

/*static*/ void AsyncRuntime::Set(AsyncRuntime runtime) {
  assert(runtime.runner() != nullptr);
  async_runtime = runtime;
}

/*static*/ AsyncRuntime& AsyncRuntime::GetCurrentRuntime() {
  assert(async_runtime.runner() != nullptr);
  return async_runtime;
}

/*static*/ void* AsyncRuntime::GetStorage(Value* value) {
  return value->GetStorage();
}

/*static*/ AsyncValue* AsyncRuntime::GetAsyncValue(AsyncRuntime::Value* value) {
  return value->GetAsyncValue();
}

/*static*/ AsyncValue* AsyncRuntime::GetAsyncValue(AsyncRuntime::Token* token) {
  return token->GetAsyncValue();
}

/*static*/ AsyncValue* AsyncRuntime::GetAsyncValue(AsyncRuntime::Group* group) {
  return group->GetCompletionAsyncValue();
}

/*static*/ void AsyncRuntime::Await(AsyncValue* awaitable) {
  // Short circuit the trivial case.
  if (awaitable->IsAvailable()) return;
  tfrt::Await({awaitable});
}

/*static*/ void AsyncRuntime::AddRef(AsyncRuntimeObject* obj, unsigned count) {
  assert(count == 1 && "AsyncRuntimeObject can add just one ref");
  obj->AddRef();
}

/*static*/ void AsyncRuntime::DropRef(AsyncRuntimeObject* obj, unsigned count) {
  assert(count == 1 && "AsyncRuntimeObject can drop just one ref");
  obj->DropRef();
}

/*static*/ AsyncRuntimeObject* AsyncRuntime::ToAsyncRuntimeObject(
    AsyncRuntime::Token* token) {
  return static_cast<AsyncRuntimeObject*>(token);
}

/*static*/ AsyncRuntimeObject* AsyncRuntime::ToAsyncRuntimeObject(
    AsyncRuntime::Value* value) {
  return static_cast<AsyncRuntimeObject*>(value);
}

/*static*/ AsyncRuntimeObject* AsyncRuntime::ToAsyncRuntimeObject(
    AsyncRuntime::Group* group) {
  return static_cast<AsyncRuntimeObject*>(group);
}

/*static*/ AsyncRuntime::Token* AsyncRuntime::CreateToken() {
  // AsyncRuntime::Token created with a reference count of 2 because it will be
  // returned to the `async.execute` caller and also will be later on emplaced
  // by the asynchronously executed task. If the caller immediately will drop
  // its reference we must ensure that the token will be alive until the
  // asynchronous operation is completed.
  return new AsyncRuntime::Token(/*ref_count=*/2);
}

/*static*/ void AsyncRuntime::SetAvailable(AsyncRuntime::Token* token) {
  token->GetAsyncValue()->SetStateConcrete();
  // Async tokens created with a ref count `2` to keep token alive until the
  // async task completes. Drop extra reference explicitly when token emplaced.
  DropRef(token);
}

/*static*/ void AsyncRuntime::SetError(AsyncRuntime::Token* token) {
  // TODO(ezhulenev): Construct a better diagnostincs when async runtime API
  // will support passing custom error messages.
  token->GetAsyncValue()->SetError(
      absl::InternalError("<async runtime error>"));
  // Async tokens created with a ref count `2` to keep token alive until the
  // async task completes. Drop extra reference explicitly when token emplaced.
  DropRef(token);
}

/*static*/ bool AsyncRuntime::IsError(AsyncRuntime::Token* token) {
  return token->GetAsyncValue()->IsError();
}

/*static*/ void AsyncRuntime::AwaitToken(AsyncRuntime::Token* token) {
  Await(token->GetAsyncValue());
}

/*static*/ AsyncRuntime::Value* AsyncRuntime::CreateValue(size_t size,
                                                          size_t alignment) {
  // AsyncRuntime::Value created with a reference count of 2 because it will be
  // returned to the `async.execute` caller and also will be later on emplaced
  // by the asynchronously executed task. If the caller immediately will drop
  // its reference we must ensure that the token will be alive until the
  // asynchronous operation is completed.
  return new AsyncRuntime::Value(size, alignment, /*ref_count=*/2);
}

/*static*/ void AsyncRuntime::SetAvailable(AsyncRuntime::Value* value) {
  value->GetAsyncValue()->SetStateConcrete();
  // Async values created with a ref count `2` to keep token alive until the
  // async task completes. Drop extra reference explicitly when token emplaced.
  DropRef(value);
}

/*static*/ void AsyncRuntime::SetError(AsyncRuntime::Value* value) {
  // TODO(ezhulenev): Construct a better diagnostincs when async runtime API
  // will support passing custom error messages.
  value->GetAsyncValue()->SetError(
      absl::InternalError("<async runtime error>"));
  // Async values created with a ref count `2` to keep token alive until the
  // async task completes. Drop extra reference explicitly when token emplaced.
  DropRef(value);
}

/*static*/ bool AsyncRuntime::IsError(AsyncRuntime::Value* value) {
  return value->GetAsyncValue()->IsError();
}

/*static*/ void AsyncRuntime::AwaitValue(AsyncRuntime::Value* value) {
  Await(value->GetAsyncValue());
}

/*static*/ AsyncRuntime::Group* AsyncRuntime::CreateGroup(int64_t size) {
  return new AsyncRuntime::Group(size);
}

/*static*/ size_t AsyncRuntime::AddTokenToGroup(AsyncRuntime::Group* group,
                                                AsyncRuntime::Token* token) {
  return group->AddToken(token);
}

/*static*/ bool AsyncRuntime::IsError(AsyncRuntime::Group* group) {
  return group->IsError();
}

/*static*/ void AsyncRuntime::AwaitGroup(AsyncRuntime::Group* group) {
  Await(group->GetCompletionAsyncValue());
}

}  // namespace runtime
}  // namespace xla
