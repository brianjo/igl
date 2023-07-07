/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "OpenGLTextureAccessor.h"
#include "ITextureAccessor.h"
#include "igl/Texture.h"
#include "igl/opengl/Device.h"
#include "igl/opengl/Framebuffer.h"
#include "igl/opengl/IContext.h"
#include "igl/opengl/Texture.h"

#if defined(IGL_CMAKE_BUILD)
#include <igl/IGLSafeC.h>
#else
#include <secure_lib/secure_string.h>
#endif

namespace iglu {
namespace textureaccessor {

OpenGLTextureAccessor::OpenGLTextureAccessor(std::shared_ptr<igl::ITexture> texture,
                                             igl::IDevice& device) :
  ITextureAccessor(std::move(texture)) {
  // glReadPixels requires a that the texture be attached to a framebuffer
  igl::FramebufferDesc framebufferDesc;
  framebufferDesc.colorAttachments[0].texture = texture_;
  frameBuffer_ = device.createFramebuffer(framebufferDesc, nullptr);

  auto& oglTexture = static_cast<igl::opengl::Texture&>(*texture_);
  const auto dimensions = oglTexture.getDimensions();
  textureWidth_ = dimensions.width;
  textureHeight_ = dimensions.height;

  textureBytesPerImage_ = oglTexture.getProperties().getBytesPerRange(oglTexture.getFullRange());
  latestBytesRead_.resize(textureBytesPerImage_);

  auto& context = oglTexture.getContext();
  const auto& deviceFeatures = context.deviceFeatures();
  asyncReadbackSupported_ =
      deviceFeatures.hasInternalFeature(igl::opengl::InternalFeatures::PixelBufferObject) &&
      deviceFeatures.hasInternalFeature(igl::opengl::InternalFeatures::Sync) &&
      deviceFeatures.hasFeature(igl::DeviceFeatures::MapBufferRange);

  if (asyncReadbackSupported_) {
    // Create PBO and allocate size
    context.genBuffers((GLsizei)1, &pboId_);
    context.bindBuffer(GL_PIXEL_PACK_BUFFER, pboId_);
    context.bufferData(GL_PIXEL_PACK_BUFFER, textureBytesPerImage_, nullptr, GL_STREAM_READ);
    context.bindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  }
};

void OpenGLTextureAccessor::requestBytes(igl::ICommandQueue& commandQueue,
                                         std::shared_ptr<igl::ITexture> texture) {
  if (texture) {
    IGL_ASSERT(textureWidth_ == texture->getDimensions().width &&
               textureHeight_ == texture->getDimensions().height);
    texture_ = std::move(texture);
    frameBuffer_->updateDrawable(texture_);
  }

  if (asyncReadbackSupported_) {
    auto& glTexture = static_cast<igl::opengl::Texture&>(*texture_);
    auto& context = glTexture.getContext();

    auto oglFrameBuffer = static_cast<igl::opengl::Framebuffer*>(&(*frameBuffer_));
    oglFrameBuffer->bindBuffer();

    const auto& properties = glTexture.getProperties();
    context.pixelStorei(GL_PACK_ALIGNMENT,
                        glTexture.getAlignment(properties.getBytesPerRow(textureWidth_), 0));

    // Start transferring from framebuffer -> PBO
    context.bindBuffer(GL_PIXEL_PACK_BUFFER, pboId_);
    context.readPixels(0, 0, textureWidth_, textureHeight_, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    context.bindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    sync_ = context.fenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

    status_ = RequestStatus::InProgress;
    return;
  }

  // Async readback not supported
  const auto range = igl::TextureRangeDesc::new2D(0, 0, textureWidth_, textureHeight_);
  frameBuffer_->copyBytesColorAttachment(commandQueue, 0, latestBytesRead_.data(), range);

  status_ = RequestStatus::Ready;
}

RequestStatus OpenGLTextureAccessor::getRequestStatus() {
  if (asyncReadbackSupported_ && status_ == RequestStatus::InProgress) {
    auto& texture = static_cast<igl::opengl::Texture&>(*texture_);
    auto& context = texture.getContext();
    // If a read is in progress, check whether it has completed
    int result;
    int valuesLength;
    context.getSynciv(sync_, GL_SYNC_STATUS, 1, &valuesLength, &result);
    IGL_ASSERT(valuesLength == 1);
    status_ = result == GL_SIGNALED ? RequestStatus::Ready : RequestStatus::InProgress;
    if (status_ == RequestStatus::Ready) {
      context.deleteSync(sync_);
      sync_ = nullptr;
    }
  }
  return status_;
};

std::vector<unsigned char>& OpenGLTextureAccessor::getBytes() {
  if (asyncReadbackSupported_ && status_ == RequestStatus::InProgress) {
    auto& texture = static_cast<igl::opengl::Texture&>(*texture_);
    auto& context = texture.getContext();
    context.bindBuffer(GL_PIXEL_PACK_BUFFER, pboId_);
    auto bytes =
        context.mapBufferRange(GL_PIXEL_PACK_BUFFER, 0, textureBytesPerImage_, GL_MAP_READ_BIT);
    checked_memcpy_robust(latestBytesRead_.data(),
                          latestBytesRead_.size(),
                          bytes,
                          textureBytesPerImage_,
                          textureBytesPerImage_);
    context.unmapBuffer(GL_PIXEL_PACK_BUFFER);
    context.bindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    status_ = RequestStatus::Ready;
    if (sync_ != nullptr) {
      context.deleteSync(sync_);
      sync_ = nullptr;
    }
  }
  return latestBytesRead_;
}

} // namespace textureaccessor
} // namespace iglu
