/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <Metal/Metal.h>
#include <igl/Buffer.h>
#include <igl/metal/Device.h>

namespace igl {
namespace metal {

class BufferSynchronizationManager;

class Buffer : public igl::IBuffer {
 public:
  Buffer(id<MTLBuffer> value,
         MTLResourceOptions options,
         BufferDesc::BufferAPIHint requestedApiHints,
         BufferDesc::BufferAPIHint acceptedApiHints);

  Result upload(const void* data, const BufferRange& range) override;

  void* map(const BufferRange& range, Result* outResult) override;
  void unmap() override;

  BufferDesc::BufferAPIHint requestedApiHints() const noexcept override;
  BufferDesc::BufferAPIHint acceptedApiHints() const noexcept override;
  ResourceStorage storage() const noexcept override;

  size_t getSizeInBytes() const override;
  [[nodiscard]] uint64_t gpuAddress(size_t offset) const override;

  IGL_INLINE virtual id<MTLBuffer> get() {
    return mtlBuffers_[0];
  }

 protected:
  MTLResourceOptions resourceOptions_;
  std::vector<id<MTLBuffer>> mtlBuffers_;
  BufferDesc::BufferAPIHint requestedApiHints_;
  BufferDesc::BufferAPIHint acceptedApiHints_;
};

// Manages a ring of buffers.
// At a given time frame, all upload, map and get methods operate on the buffer
// indexed by the current inFlight buffer index
class RingBuffer final : public Buffer {
 public:
  RingBuffer(std::vector<id<MTLBuffer>> ringBuffers,
             MTLResourceOptions options,
             std::shared_ptr<const BufferSynchronizationManager> syncManager,
             BufferDesc::BufferAPIHint requestedApiHints);

  Result upload(const void* data, const BufferRange& range) override;

  void* map(const BufferRange& range, Result* outResult) override;
  void unmap() override;

  IGL_INLINE id<MTLBuffer> get() override;

 private:
  int lastUpdatedBufferIdx_ = -1;
  std::shared_ptr<const BufferSynchronizationManager> syncManager_;
};

} // namespace metal
} // namespace igl
