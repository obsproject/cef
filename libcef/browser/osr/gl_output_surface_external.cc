// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cef/libcef/browser/osr/gl_output_surface_external.h"

#include <stdint.h>

#include "base/bind.h"
#include "components/viz/common/resources/resource_format_utils.h"
#include "components/viz/service/display/output_surface_client.h"
#include "components/viz/service/display/output_surface_frame.h"
#include "gpu/GLES2/gl2extchromium.h"
#include "gpu/command_buffer/client/context_support.h"
#include "gpu/command_buffer/client/gles2_interface.h"
#include "gpu/command_buffer/client/gpu_memory_buffer_manager.h"
#include "gpu/command_buffer/client/shared_image_interface.h"
#include "gpu/command_buffer/common/shared_image_usage.h"
#include "ui/gfx/gpu_memory_buffer.h"
#include "ui/gfx/swap_result.h"
#include "ui/gl/gl_utils.h"

#if defined (OS_WIN) && !defined(ARCH_CPU_ARM_FAMILY)
#include "ui/gl/gl_image_dxgi.h"
#endif

namespace viz {

class ExternalImageData {
 public:
  ExternalImageData(gpu::gles2::GLES2Interface* gl,
                    const gpu::Capabilities& capabilities)
      : gl_(gl) {
    texture_target_ = gpu::GetBufferTextureTarget(
        gfx::BufferUsage::SCANOUT, gfx::BufferFormat::RGBA_8888, capabilities);
  }
  ~ExternalImageData() {
    if (bound_ && fbo_) {
      UnbindTexture();
    }
    if (texture_id_) {
      gl_->DeleteTextures(1, &texture_id_);
    }

    if (image_id_) {
      gl_->DestroyImageCHROMIUM(image_id_);
    }

    buffer_.reset();
  }

  void Create(gfx::Size size,
              gfx::ColorSpace color_space,
              gpu::GpuMemoryBufferManager* manager) {
    size_ = size;
    color_space_ = color_space;
    buffer_ = manager->CreateGpuMemoryBuffer(size, gfx::BufferFormat::RGBA_8888,
                                             gfx::BufferUsage::SCANOUT,
                                             gpu::kNullSurfaceHandle,
                                             nullptr);
    if (!buffer_) {
      LOG(ERROR) << "failed to allocate GPU memory buffer";
      return;
    }
    buffer_->SetColorSpace(color_space);

    image_id_ = gl_->CreateImageCHROMIUM(buffer_->AsClientBuffer(),
                                         size.width(), size.height(), GL_RGBA);
    if (!image_id_) {
      buffer_ = 0;
      LOG(ERROR) << "could not create image chromium: " << gl_->GetError();
      return;
    }

    gl_->GenTextures(1, &texture_id_);
  }

  gfx::GpuMemoryBufferHandle GetHandle() {
    if (!buffer_) {
      return gfx::GpuMemoryBufferHandle();
    }
    return buffer_->CloneHandle();
  }

  void BindTexture(uint32_t fbo) {
    if (bound_) {
      CHECK_EQ(fbo_, fbo);
      // Multi-pass rendering like HTMLCanvasElement with blur-filter triggers
      // multiple calls to GLOutputSurfaceExternal::BindFramebuffer. The only
      // thing we need to do in this case is to make sure the FBO is bound.
      gl_->BindFramebuffer(GL_FRAMEBUFFER, fbo);
      return;
    }

    if (!texture_id_ || !image_id_) {
      return;
    }

    gl_->BindTexture(texture_target_, texture_id_);
    gl_->BindTexImage2DCHROMIUM(texture_target_, image_id_);
    gl_->SetColorSpaceMetadataCHROMIUM(
        texture_id_, reinterpret_cast<GLcolorSpace>(&color_space_));

    gl_->BindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl_->FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              texture_target_, texture_id_, 0);

    fbo_ = fbo;
    bound_ = true;
  }

  void UnbindTexture() {
    if (!texture_id_ || !image_id_ || !fbo_ || !bound_)
      return;

    gl_->BindTexture(texture_target_, texture_id_);
    gl_->ReleaseTexImage2DCHROMIUM(texture_target_, image_id_);

    gl_->Flush();
    bound_ = false;
    fbo_ = 0;
  }

  gpu::gles2::GLES2Interface* gl_;

  gfx::Size size_;
  gfx::ColorSpace color_space_;
  uint32_t texture_target_ = 0;
  uint32_t texture_id_ = 0;
  uint32_t image_id_ = 0;
  uint32_t fbo_ = 0;
  bool bound_ = false;
  std::unique_ptr<gfx::GpuMemoryBuffer> buffer_;
};

GLOutputSurfaceExternal::GLOutputSurfaceExternal(
    scoped_refptr<VizProcessContextProvider> context_provider,
    gpu::GpuMemoryBufferManager* gpu_memory_buffer_manager,
    mojo::Remote<viz::mojom::ExternalRendererUpdater> external_renderer_updater)
    : GLOutputSurface(context_provider, gpu::kNullSurfaceHandle),
      gpu_memory_buffer_manager_(gpu_memory_buffer_manager),
      external_renderer_updater_(std::move(external_renderer_updater)) {
       capabilities_.uses_default_gl_framebuffer = false;
  }

GLOutputSurfaceExternal::~GLOutputSurfaceExternal() {
  DiscardBackbuffer();
}

void GLOutputSurfaceExternal::EnsureBackbuffer() {
  if (size_.IsEmpty())
    return;

  if (!available_surfaces_.empty()) {
    current_surface_ = std::move(available_surfaces_.back());
    available_surfaces_.pop_back();
    return;
  }

  gpu::gles2::GLES2Interface* gl = context_provider_->ContextGL();

  const int max_texture_size =
      context_provider_->ContextCapabilities().max_texture_size;
  gfx::Size texture_size(std::min(size_.width(), max_texture_size),
                         std::min(size_.height(), max_texture_size));

  current_surface_ = std::make_unique<ExternalImageData>(
      gl, context_provider_->ContextCapabilities());
  current_surface_->Create(texture_size, color_space_,
                           gpu_memory_buffer_manager_);

  if (!fbo_) {
    gl->GenFramebuffers(1, &fbo_);
  }
}

void GLOutputSurfaceExternal::DiscardBackbuffer() {
  displayed_surface_.reset();
  displaying_surface_.reset();
  current_surface_.reset();
  for (auto& surface : in_flight_surfaces_)
    surface = nullptr;
  available_surfaces_.clear();

  gpu::gles2::GLES2Interface* gl = context_provider_->ContextGL();

  if (fbo_) {
    gl->BindFramebuffer(GL_FRAMEBUFFER, fbo_);
    gl->DeleteFramebuffers(1, &fbo_);
    fbo_ = 0;
  }

  gl->Flush();
}

void GLOutputSurfaceExternal::BindFramebuffer() {
  if (!current_surface_) {
    EnsureBackbuffer();
  }
  if (current_surface_) {
    current_surface_->BindTexture(fbo_);
  } else {
    LOG(ERROR) << "No surface available to bind";
  }
}

void GLOutputSurfaceExternal::Reshape(const ReshapeParams& params) {
  const gfx::Size& size = params.size;
  const gfx::ColorSpace& color_space = params.color_space;
  if (size_ == size && color_space_ == color_space) {
    return;
  }
  size_ = size;
  color_space_ = color_space;
  DiscardBackbuffer();
}

void GLOutputSurfaceExternal::SwapBuffers(OutputSurfaceFrame frame) {
  DCHECK(frame.size == size_);

  gpu::gles2::GLES2Interface* gl = context_provider_->ContextGL();

  gl->Flush();
  current_surface_->UnbindTexture();

  gpu::SyncToken sync_token;
  gl->GenUnverifiedSyncTokenCHROMIUM(sync_token.GetData());
  context_provider_->ContextSupport()->SignalSyncToken(
      sync_token, base::BindOnce(&GLOutputSurfaceExternal::OnSyncWaitComplete,
                                 weak_ptr_factory_.GetWeakPtr(),
                                 std::move(frame.latency_info)));
}

void GLOutputSurfaceExternal::OnSyncWaitComplete(
    std::vector<ui::LatencyInfo> latency_info) {
  gfx::GpuMemoryBufferHandle handle = current_surface_->GetHandle();

  in_flight_surfaces_.push_back(std::move(current_surface_));

  if (handle.type != gfx::GpuMemoryBufferType::EMPTY_BUFFER) {
    external_renderer_updater_->OnAfterFlip(
        std::move(handle), gfx::Rect(size_),
        base::BindOnce(&GLOutputSurfaceExternal::OnAfterSwap,
                       weak_ptr_factory_.GetWeakPtr(),
                       std::move(latency_info)));
  } else {
    OnAfterSwap(latency_info);
  }
}

void GLOutputSurfaceExternal::OnAfterSwap(
    std::vector<ui::LatencyInfo> latency_info) {
  if (in_flight_surfaces_.front()) {
    if (displayed_surface_) {
      available_surfaces_.push_back(std::move(displayed_surface_));
    }
    if (displaying_surface_) {
      displayed_surface_ = std::move(displaying_surface_);
    }
    displaying_surface_ = std::move(in_flight_surfaces_.front());
  }
  in_flight_surfaces_.pop_front();

  latency_tracker()->OnGpuSwapBuffersCompleted(latency_info);
  // Swap timings are not available since for offscreen there is no Swap, just a
  // SignalSyncToken. We use base::TimeTicks::Now() as an overestimate.
  auto now = base::TimeTicks::Now();
  client()->DidReceiveSwapBuffersAck({.swap_start = now}, gfx::GpuFenceHandle());
  client()->DidReceivePresentationFeedback(gfx::PresentationFeedback(
      now, base::Milliseconds(16), /*flags=*/0));

  if (needs_swap_size_notifications())
    client()->DidSwapWithSize(size_);
}

}  // namespace viz
