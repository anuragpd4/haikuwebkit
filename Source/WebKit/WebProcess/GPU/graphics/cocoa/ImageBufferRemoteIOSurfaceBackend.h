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

#if HAVE(IOSURFACE)

#include "ImageBufferBackendHandle.h"
#include <WebCore/GraphicsContext.h>
#include <WebCore/ImageBufferBackend.h>
#include <wtf/IsoMalloc.h>

namespace WebKit {

class ImageBufferRemoteIOSurfaceBackend final : public WebCore::ImageBufferBackend {
    WTF_MAKE_ISO_ALLOCATED(ImageBufferRemoteIOSurfaceBackend);
    WTF_MAKE_NONCOPYABLE(ImageBufferRemoteIOSurfaceBackend);
public:
    static WebCore::IntSize calculateSafeBackendSize(const Parameters&);
    static size_t calculateMemoryCost(const Parameters&);
    static size_t calculateExternalMemoryCost(const Parameters&);

    static std::unique_ptr<ImageBufferRemoteIOSurfaceBackend> create(const Parameters&, ImageBufferBackendHandle);

    ImageBufferRemoteIOSurfaceBackend(const Parameters& parameters, ImageBufferBackendHandle&& handle)
        : ImageBufferBackend(parameters)
        , m_handle(WTFMove(handle))
    {
    }

    ImageBufferBackendHandle createImageBufferBackendHandle() const;

    static constexpr bool isOriginAtBottomLeftCorner = true;
    static constexpr bool canMapBackingStore = false;
    static constexpr WebCore::RenderingMode renderingMode = WebCore::RenderingMode::Accelerated;

    WebCore::GraphicsContext& context() const final;

private:
    WebCore::IntSize backendSize() const final;
    RefPtr<WebCore::NativeImage> copyNativeImage(WebCore::BackingStoreCopy) const final;
    RefPtr<WebCore::Image> copyImage(WebCore::BackingStoreCopy, WebCore::PreserveResolution) const final;
    void draw(WebCore::GraphicsContext&, const WebCore::FloatRect& destRect, const WebCore::FloatRect& srcRect, const WebCore::ImagePaintingOptions&) final;
    void drawPattern(WebCore::GraphicsContext&, const WebCore::FloatRect& destRect, const WebCore::FloatRect& srcRect, const WebCore::AffineTransform& patternTransform, const WebCore::FloatPoint& phase, const WebCore::FloatSize& spacing, const WebCore::ImagePaintingOptions&) final;
    String toDataURL(const String& mimeType, std::optional<double> quality, WebCore::PreserveResolution) const final;
    Vector<uint8_t> toData(const String& mimeType, std::optional<double> quality) const final;
    std::optional<WebCore::PixelBuffer> getPixelBuffer(const WebCore::PixelBufferFormat& outputFormat, const WebCore::IntRect&) const final;
    void putPixelBuffer(const WebCore::PixelBuffer&, const WebCore::IntRect& srcRect, const WebCore::IntPoint& destPoint, WebCore::AlphaPremultiplication destFormat) final;

    bool originAtBottomLeftCorner() const final { return isOriginAtBottomLeftCorner; }

    unsigned bytesPerRow() const final;

    ImageBufferBackendHandle m_handle;
};

} // namespace WebKit

#endif // HAVE(IOSURFACE)
