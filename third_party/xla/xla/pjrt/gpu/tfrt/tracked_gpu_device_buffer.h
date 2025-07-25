/* Copyright 2025 The OpenXLA Authors.

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

#ifndef XLA_PJRT_GPU_TFRT_TRACKED_GPU_DEVICE_BUFFER_H_
#define XLA_PJRT_GPU_TFRT_TRACKED_GPU_DEVICE_BUFFER_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <type_traits>
#include <utility>

#include "absl/container/inlined_vector.h"
#include "absl/functional/any_invocable.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/types/span.h"
#include "xla/pjrt/gpu/tfrt/gpu_event.h"
#include "xla/pjrt/pjrt_client.h"
#include "xla/service/shaped_buffer.h"
#include "xla/shape.h"
#include "xla/stream_executor/device_memory.h"
#include "xla/stream_executor/device_memory_allocator.h"
#include "xla/tsl/concurrency/async_value_ref.h"
#include "xla/tsl/framework/allocator.h"

namespace xla {
// TODO(b/400541410): Refactor and Merge this with MaybeOwningDeviceMemory.

// GpuDeviceMemory represents either an owned or unowned GPU memory. It
// owns GPU memory if an allocator is provided. When the object goes output of
// scope, it will free the underlying memory if it owns it.
class GpuDeviceMemory {
 public:
  GpuDeviceMemory() = default;
  GpuDeviceMemory(GpuDeviceMemory&& other) = default;
  GpuDeviceMemory& operator=(GpuDeviceMemory&& other) = default;

  // Creates non-owning GPU device memory from a raw data pointer.
  explicit GpuDeviceMemory(stream_executor::DeviceMemoryBase buffer)
      : buffer_(buffer) {}

  // Creates owning GPU device memory from an owned data pointer.
  explicit GpuDeviceMemory(stream_executor::OwningDeviceMemory buffer)
      : owning_buffer_(std::move(buffer)), buffer_(*owning_buffer_) {}

  ShapedBuffer AsShapedBuffer(const Shape& on_device_shape,
                              const PjRtDevice* device) const;

  // Change ownership from owning to non-owning. Used for buffer donation.
  void SetUnOwned();

  // Allocates raw owning memory.
  static absl::StatusOr<GpuDeviceMemory> Allocate(
      se::DeviceMemoryAllocator* allocator, int device_ordinal, size_t size);

  static absl::StatusOr<GpuDeviceMemory> Allocate(
      se::DeviceMemoryAllocator* allocator, int device_ordinal, size_t size,
      int64_t memory_space);

  stream_executor::DeviceMemoryBase buffer() const { return buffer_; }
  size_t size_bytes() const { return buffer_.size(); }
  bool owns_data() const { return !owning_buffer_.is_null(); }

 private:
  stream_executor::OwningDeviceMemory owning_buffer_;
  se::DeviceMemoryBase buffer_;
};

// Class that represents a GPU buffer. It optionally owns the buffer. It also
// tracks the definition and usage of the memory to allow for synchronized usage
// and deletion of GPU memory. This class is thread-compatible.
class TrackedGpuDeviceBuffer {
 public:
  TrackedGpuDeviceBuffer(
      tsl::AsyncValueRef<GpuDeviceMemory> buffer,
      tsl::AsyncValueRef<GpuEvent> definition_event,
      tsl::AsyncValueRef<GpuEvent> ready_event,
      absl::AnyInvocable<void() &&> on_delete_callback = nullptr);

  TrackedGpuDeviceBuffer(TrackedGpuDeviceBuffer&&) = default;
  TrackedGpuDeviceBuffer& operator=(TrackedGpuDeviceBuffer&&) = default;

  ~TrackedGpuDeviceBuffer();

  const tsl::AsyncValueRef<GpuDeviceMemory>& buffer() const { return buffer_; }

  const tsl::AsyncValueRef<GpuEvent>& definition_event() const {
    return definition_event_;
  }

  const tsl::AsyncValueRef<GpuEvent>& ready_event() const {
    return ready_event_;
  }

  // Adds usage events to the buffer. This usage events could be any device
  // buffer related events, e.g. D2H/D2D
  void AddUsageEvents(absl::Span<tsl::AsyncValueRef<GpuEvent>> events);

  // Returns an AsyncValueRef<GpuEvent> that will be ready after all the async
  // values in usage events are ready. If errors occurs, one of the errors will
  // be propagated through the returned async value.
  tsl::AsyncValueRef<GpuEvent> AfterAllUsageEvents();

  // Return the usage events for the buffers. After
  // LockUseAndTransferUsageEvents is called, it is illegal to AddUsageEvent.
  tsl::AsyncValueRef<GpuEvent> LockUseAndTransferUsageEvents();

  // Relinquishes ownership of the buffer's device memory, e.g., after the
  // buffer is passed to a computation that aliases its inputs to outputs.
  void ReleaseDeviceMemory();

  // Change ownership of underlying GpuDeviceMemory from owning to
  // non-owning. Used for buffer donation.
  void SetUnOwned();

  friend class TfrtGpuBuffer;

 private:
  tsl::AsyncValueRef<GpuDeviceMemory> buffer_;

  // The definition event are resolved when the GPU operations that write to the
  // buffers are enqueued to the stream.
  tsl::AsyncValueRef<GpuEvent> definition_event_;

  // The ready event is resolved when the GPU operations that write to the
  // buffers are done executing on the stream.
  tsl::AsyncValueRef<GpuEvent> ready_event_;

  // Usage events are associated with GPU operations that read from the buffers.
  TfrtEventSet usage_events_;

  // An event triggered after this buffer is freed or donated. This event is
  // used to make sure that allocations are sequenced with respect to
  // deallocations in program order.
  tsl::AsyncValueRef<GpuEvent> deallocation_event_;

  // A callback to call when the TrackedGpuDeviceBuffer is about to be
  // destroyed.
  absl::AnyInvocable<void() &&> on_delete_callback_;
};

}  // namespace xla

#endif  // XLA_PJRT_GPU_TFRT_TRACKED_GPU_DEVICE_BUFFER_H_
