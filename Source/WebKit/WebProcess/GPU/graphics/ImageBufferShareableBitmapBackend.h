/*
 * Copyright (C) 2020-2021 Apple Inc.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#if ENABLE(GPU_PROCESS)

#include "ImageBufferBackendHandle.h"
#include <WebCore/PlatformImageBufferBackend.h>
#include <wtf/IsoMalloc.h>

namespace WebCore {
class ProcessIdentity;
}

namespace WebKit {

class ShareableBitmap;

class ImageBufferShareableBitmapBackend final : public WebCore::PlatformImageBufferBackend {
    WTF_MAKE_ISO_ALLOCATED(ImageBufferShareableBitmapBackend);
    WTF_MAKE_NONCOPYABLE(ImageBufferShareableBitmapBackend);
public:
    static ShareableBitmap::Configuration configuration(const Parameters&);
    static WebCore::IntSize calculateSafeBackendSize(const Parameters&);
    static unsigned calculateBytesPerRow(const Parameters&, const WebCore::IntSize& backendSize);
    static size_t calculateMemoryCost(const Parameters&);

    static std::unique_ptr<ImageBufferShareableBitmapBackend> create(const Parameters&, const WebCore::HostWindow*);
    static std::unique_ptr<ImageBufferShareableBitmapBackend> create(const Parameters&, ImageBufferBackendHandle);

    ImageBufferShareableBitmapBackend(const Parameters&, RefPtr<ShareableBitmap>&&, std::unique_ptr<WebCore::GraphicsContext>&&);

    ImageBufferBackendHandle createImageBufferBackendHandle() const;

    WebCore::GraphicsContext& context() const final { return *m_context; }
    
    WebCore::IntSize backendSize() const final;

    RefPtr<WebCore::NativeImage> copyNativeImage(WebCore::BackingStoreCopy = WebCore::CopyBackingStore) const final;
    RefPtr<WebCore::Image> copyImage(WebCore::BackingStoreCopy = WebCore::CopyBackingStore, WebCore::PreserveResolution = WebCore::PreserveResolution::No) const final;

    std::optional<WebCore::PixelBuffer> getPixelBuffer(const WebCore::PixelBufferFormat& outputFormat, const WebCore::IntRect&) const final;
    void putPixelBuffer(const WebCore::PixelBuffer&, const WebCore::IntRect& srcRect, const WebCore::IntPoint& destPoint, WebCore::AlphaPremultiplication destFormat) final;

    void setOwnershipIdentity(const WebCore::ProcessIdentity&) { }
private:
    unsigned bytesPerRow() const final;

    RefPtr<ShareableBitmap> m_bitmap;
    std::unique_ptr<WebCore::GraphicsContext> m_context;
};

} // namespace WebKit

#endif // ENABLE(GPU_PROCESS)
