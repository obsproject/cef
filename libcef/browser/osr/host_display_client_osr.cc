// Copyright 2019 The Chromium Embedded Framework Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "libcef/browser/osr/host_display_client_osr.h"

#include <utility>

#include "cef/libcef/browser/osr/external_renderer_updater.mojom.h"
#include "libcef/browser/osr/render_widget_host_view_osr.h"

#include "base/memory/shared_memory_mapping.h"
#include "components/viz/common/resources/resource_format.h"
#include "components/viz/common/resources/resource_sizes.h"
#include "mojo/public/cpp/system/platform_handle.h"
#include "services/viz/privileged/mojom/compositing/layered_window_updater.mojom.h"
#include "skia/ext/platform_canvas.h"
#include "third_party/skia/include/core/SkColor.h"
#include "third_party/skia/include/core/SkRect.h"
#include "third_party/skia/src/core/SkDevice.h"
#include "ui/gfx/skia_util.h"

#if BUILDFLAG(IS_WIN)
#include "skia/ext/skia_utils_win.h"
#endif

class CefExternalRendererUpdaterOSR
    : public viz::mojom::ExternalRendererUpdater {
 public:
  CefExternalRendererUpdaterOSR(
      CefRenderWidgetHostViewOSR* const view,
      mojo::PendingReceiver<viz::mojom::ExternalRendererUpdater> receiver);
  ~CefExternalRendererUpdaterOSR() override;

  // viz::mojom::ExternalRendererUpdater implementation.
  void OnAfterFlip(gfx::GpuMemoryBufferHandle handle,
                   bool new_texture,
                   const gfx::Rect& damage_rect,
                   OnAfterFlipCallback callback) override;

 private:
  CefRenderWidgetHostViewOSR* const view_;
  mojo::Receiver<viz::mojom::ExternalRendererUpdater> receiver_;

  CefExternalRendererUpdaterOSR(const CefExternalRendererUpdaterOSR&);
  void operator=(const CefExternalRendererUpdaterOSR&);
};

CefExternalRendererUpdaterOSR::CefExternalRendererUpdaterOSR(
    CefRenderWidgetHostViewOSR* const view,
    mojo::PendingReceiver<viz::mojom::ExternalRendererUpdater> receiver)
    : view_(view), receiver_(this, std::move(receiver)) {}

CefExternalRendererUpdaterOSR::~CefExternalRendererUpdaterOSR() = default;

void CefExternalRendererUpdaterOSR::OnAfterFlip(
    gfx::GpuMemoryBufferHandle handle,
    bool new_texture,
    const gfx::Rect& damage_rect,
    OnAfterFlipCallback callback) {
#if defined (OS_WIN) && !defined(ARCH_CPU_ARM_FAMILY)
  HANDLE nthandle = nullptr;
  if (new_texture)
    nthandle = handle.dxgi_handle.Get();
  view_->OnAcceleratedPaint2(damage_rect, nthandle, new_texture);
#elif defined(OS_MAC)
  view_->OnAcceleratedPaint2(damage_rect,
                             (void*)(uintptr_t)(handle.io_surface.get()),
                             new_texture);
#else
  view_->OnAcceleratedPaint2(damage_rect, nullptr, false);
#endif
  std::move(callback).Run();
}

class CefLayeredWindowUpdaterOSR : public viz::mojom::LayeredWindowUpdater {
 public:
  CefLayeredWindowUpdaterOSR(
      CefRenderWidgetHostViewOSR* const view,
      mojo::PendingReceiver<viz::mojom::LayeredWindowUpdater> receiver);

  CefLayeredWindowUpdaterOSR(const CefLayeredWindowUpdaterOSR&) = delete;
  CefLayeredWindowUpdaterOSR& operator=(const CefLayeredWindowUpdaterOSR&) =
      delete;

  ~CefLayeredWindowUpdaterOSR() override;

  void SetActive(bool active);
  const void* GetPixelMemory() const;
  gfx::Size GetPixelSize() const;

  // viz::mojom::LayeredWindowUpdater implementation.
  void OnAllocatedSharedMemory(const gfx::Size& pixel_size,
                               base::UnsafeSharedMemoryRegion region) override;
  void Draw(const gfx::Rect& damage_rect, DrawCallback draw_callback) override;

 private:
  CefRenderWidgetHostViewOSR* const view_;
  mojo::Receiver<viz::mojom::LayeredWindowUpdater> receiver_;
  bool active_ = false;
  base::WritableSharedMemoryMapping shared_memory_;
  gfx::Size pixel_size_;
};

CefLayeredWindowUpdaterOSR::CefLayeredWindowUpdaterOSR(
    CefRenderWidgetHostViewOSR* const view,
    mojo::PendingReceiver<viz::mojom::LayeredWindowUpdater> receiver)
    : view_(view), receiver_(this, std::move(receiver)) {}

CefLayeredWindowUpdaterOSR::~CefLayeredWindowUpdaterOSR() = default;

void CefLayeredWindowUpdaterOSR::SetActive(bool active) {
  active_ = active;
}

const void* CefLayeredWindowUpdaterOSR::GetPixelMemory() const {
  return shared_memory_.memory();
}

gfx::Size CefLayeredWindowUpdaterOSR::GetPixelSize() const {
  return pixel_size_;
}

void CefLayeredWindowUpdaterOSR::OnAllocatedSharedMemory(
    const gfx::Size& pixel_size,
    base::UnsafeSharedMemoryRegion region) {
  // Make sure |pixel_size| is sane.
  size_t expected_bytes;
  bool size_result = viz::ResourceSizes::MaybeSizeInBytes(
      pixel_size, viz::ResourceFormat::RGBA_8888, &expected_bytes);
  if (!size_result)
    return;

  pixel_size_ = pixel_size;
  shared_memory_ = region.Map();
  DCHECK(shared_memory_.IsValid());
}

void CefLayeredWindowUpdaterOSR::Draw(const gfx::Rect& damage_rect,
                                      DrawCallback draw_callback) {
  if (active_) {
    const void* memory = GetPixelMemory();
    if (memory) {
      view_->OnPaint(damage_rect, pixel_size_, memory);
    } else {
      LOG(WARNING) << "Failed to read pixels";
    }
  }

  std::move(draw_callback).Run();
}

CefHostDisplayClientOSR::CefHostDisplayClientOSR(
    CefRenderWidgetHostViewOSR* const view,
    gfx::AcceleratedWidget widget,
    bool use_proxy_output)
    : viz::HostDisplayClient(widget),
      view_(view),
      use_proxy_output_(use_proxy_output) {}

CefHostDisplayClientOSR::~CefHostDisplayClientOSR() {}

void CefHostDisplayClientOSR::SetActive(bool active) {
  active_ = active;
  if (layered_window_updater_) {
    layered_window_updater_->SetActive(active_);
  }
}

const void* CefHostDisplayClientOSR::GetPixelMemory() const {
  return layered_window_updater_ ? layered_window_updater_->GetPixelMemory()
                                 : nullptr;
}

gfx::Size CefHostDisplayClientOSR::GetPixelSize() const {
  return layered_window_updater_ ? layered_window_updater_->GetPixelSize()
                                 : gfx::Size{};
}

void CefHostDisplayClientOSR::UseProxyOutputDevice(
    UseProxyOutputDeviceCallback callback) {
  std::move(callback).Run(use_proxy_output_);
}

void CefHostDisplayClientOSR::CreateLayeredWindowUpdater(
    mojo::PendingReceiver<viz::mojom::LayeredWindowUpdater> receiver) {
  layered_window_updater_ =
      std::make_unique<CefLayeredWindowUpdaterOSR>(view_, std::move(receiver));
  layered_window_updater_->SetActive(active_);
}

void CefHostDisplayClientOSR::CreateExternalRendererUpdater(
    mojo::PendingReceiver<viz::mojom::ExternalRendererUpdater> receiver) {
  external_renderer_updater_ = std::make_unique<CefExternalRendererUpdaterOSR>(
      view_, std::move(receiver));
}

#if BUILDFLAG(IS_LINUX)
void CefHostDisplayClientOSR::DidCompleteSwapWithNewSize(
    const gfx::Size& size) {}
#endif
