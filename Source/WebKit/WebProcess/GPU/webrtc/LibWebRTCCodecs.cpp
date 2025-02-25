/*
 * Copyright (C) 2020-2021 Apple Inc. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "LibWebRTCCodecs.h"

#if USE(LIBWEBRTC) && PLATFORM(COCOA) && ENABLE(GPU_PROCESS)

#include "GPUProcessConnection.h"
#include "LibWebRTCCodecsMessages.h"
#include "LibWebRTCCodecsProxyMessages.h"
#include "Logging.h"
#include "RemoteVideoFrameObjectHeapProxy.h"
#include "RemoteVideoFrameProxy.h"
#include "WebCoreArgumentCoders.h"
#include "WebProcess.h"
#include <WebCore/CVUtilities.h>
#include <WebCore/LibWebRTCMacros.h>
#include <WebCore/PlatformMediaSessionManager.h>
#include <WebCore/RemoteVideoSample.h>
#include <WebCore/RuntimeEnabledFeatures.h>
#include <WebCore/VP9UtilitiesCocoa.h>
#include <webrtc/sdk/WebKit/WebKitDecoder.h>
#include <webrtc/sdk/WebKit/WebKitEncoder.h>
#include <wtf/MainThread.h>

#include <pal/cf/CoreMediaSoftLink.h>

namespace WebKit {
using namespace WebCore;

static webrtc::WebKitVideoDecoder createVideoDecoder(const webrtc::SdpVideoFormat& format)
{
    auto& codecs = WebProcess::singleton().libWebRTCCodecs();
    if (format.name == "H264")
        return codecs.createDecoder(LibWebRTCCodecs::Type::H264);

    if (format.name == "H265")
        return codecs.createDecoder(LibWebRTCCodecs::Type::H265);

    if (format.name == "VP9" && codecs.supportVP9VTB())
        return codecs.createDecoder(LibWebRTCCodecs::Type::VP9);

    return nullptr;
}

static int32_t releaseVideoDecoder(webrtc::WebKitVideoDecoder decoder)
{
    return WebProcess::singleton().libWebRTCCodecs().releaseDecoder(*static_cast<LibWebRTCCodecs::Decoder*>(decoder));
}

static int32_t decodeVideoFrame(webrtc::WebKitVideoDecoder decoder, uint32_t timeStamp, const uint8_t* data, size_t size, uint16_t width,  uint16_t height)
{
    return WebProcess::singleton().libWebRTCCodecs().decodeFrame(*static_cast<LibWebRTCCodecs::Decoder*>(decoder), timeStamp, data, size, width, height);
}

static int32_t registerDecodeCompleteCallback(webrtc::WebKitVideoDecoder decoder, void* decodedImageCallback)
{
    WebProcess::singleton().libWebRTCCodecs().registerDecodeFrameCallback(*static_cast<LibWebRTCCodecs::Decoder*>(decoder), decodedImageCallback);
    return 0;
}

static webrtc::WebKitVideoEncoder createVideoEncoder(const webrtc::SdpVideoFormat& format)
{
    if (format.name == "H264")
        return WebProcess::singleton().libWebRTCCodecs().createEncoder(LibWebRTCCodecs::Type::H264, format.parameters);

    if (format.name == "H265")
        return WebProcess::singleton().libWebRTCCodecs().createEncoder(LibWebRTCCodecs::Type::H265, format.parameters);

    return nullptr;
}

static int32_t releaseVideoEncoder(webrtc::WebKitVideoEncoder encoder)
{
    return WebProcess::singleton().libWebRTCCodecs().releaseEncoder(*static_cast<LibWebRTCCodecs::Encoder*>(encoder));
}

static int32_t initializeVideoEncoder(webrtc::WebKitVideoEncoder encoder, const webrtc::VideoCodec& codec)
{
    return WebProcess::singleton().libWebRTCCodecs().initializeEncoder(*static_cast<LibWebRTCCodecs::Encoder*>(encoder), codec.width, codec.height, codec.startBitrate, codec.maxBitrate, codec.minBitrate, codec.maxFramerate);
}

static inline MediaSample::VideoRotation toMediaSampleVideoRotation(webrtc::VideoRotation rotation)
{
    switch (rotation) {
    case webrtc::kVideoRotation_0:
        return MediaSample::VideoRotation::None;
    case webrtc::kVideoRotation_180:
        return MediaSample::VideoRotation::UpsideDown;
    case webrtc::kVideoRotation_90:
        return MediaSample::VideoRotation::Right;
    case webrtc::kVideoRotation_270:
        return MediaSample::VideoRotation::Left;
    }
    ASSERT_NOT_REACHED();
    return MediaSample::VideoRotation::None;
}

static inline String formatNameFromWebRTCCodecType(webrtc::VideoCodecType type)
{
    switch (type) {
    case webrtc::kVideoCodecH264:
        return "H264"_s;
    case webrtc::kVideoCodecH265:
        return "H265"_s;
    case webrtc::kVideoCodecVP9:
        return "VP9"_s;
    default:
        ASSERT_NOT_REACHED();
        return "H264";
    }
}

static void createRemoteDecoder(LibWebRTCCodecs::Decoder& decoder, IPC::Connection& connection, bool useRemoteFrames)
{
    switch (decoder.type) {
    case LibWebRTCCodecs::Type::H264:
        connection.send(Messages::LibWebRTCCodecsProxy::CreateH264Decoder { decoder.identifier, useRemoteFrames }, 0);
        break;
    case LibWebRTCCodecs::Type::H265:
        connection.send(Messages::LibWebRTCCodecsProxy::CreateH265Decoder { decoder.identifier, useRemoteFrames }, 0);
        break;
    case LibWebRTCCodecs::Type::VP9:
        connection.send(Messages::LibWebRTCCodecsProxy::CreateVP9Decoder { decoder.identifier, useRemoteFrames }, 0);
        break;
    }
}

static int32_t encodeVideoFrame(webrtc::WebKitVideoEncoder encoder, const webrtc::VideoFrame& frame, bool shouldEncodeAsKeyFrame)
{
    return WebProcess::singleton().libWebRTCCodecs().encodeFrame(*static_cast<LibWebRTCCodecs::Encoder*>(encoder), frame, shouldEncodeAsKeyFrame);
}

static int32_t registerEncodeCompleteCallback(webrtc::WebKitVideoEncoder encoder, void* encodedImageCallback)
{
    WebProcess::singleton().libWebRTCCodecs().registerEncodeFrameCallback(*static_cast<LibWebRTCCodecs::Encoder*>(encoder), encodedImageCallback);
    return 0;
}

static void setEncodeRatesCallback(webrtc::WebKitVideoEncoder encoder, const webrtc::VideoEncoder::RateControlParameters& parameters)
{
    uint32_t bitRate = parameters.bitrate.get_sum_kbps();
    uint32_t frameRate = static_cast<uint32_t>(parameters.framerate_fps + 0.5);
    WebProcess::singleton().libWebRTCCodecs().setEncodeRates(*static_cast<LibWebRTCCodecs::Encoder*>(encoder), bitRate, frameRate);
}

Ref<LibWebRTCCodecs> LibWebRTCCodecs::create()
{
    return adoptRef(*new LibWebRTCCodecs);
}

LibWebRTCCodecs::LibWebRTCCodecs()
    : m_queue(WorkQueue::create("LibWebRTCCodecs", WorkQueue::QOS::UserInteractive))
{
}

void LibWebRTCCodecs::ensureGPUProcessConnectionOnMainThreadWithLock()
{
    ASSERT(isMainRunLoop());
    if (m_connection)
        return;

    auto& gpuConnection = WebProcess::singleton().ensureGPUProcessConnection();
    gpuConnection.addClient(*this);
    m_connection = &gpuConnection.connection();
    m_remoteVideoFrameObjectHeapProxy = &gpuConnection.remoteVideoFrameObjectHeapProxy();
    m_connection->addThreadMessageReceiver(Messages::LibWebRTCCodecs::messageReceiverName(), this);

    if (m_loggingLevel)
        m_connection->send(Messages::LibWebRTCCodecsProxy::SetRTCLoggingLevel { *m_loggingLevel }, 0);
}

// May be called on any thread.
void LibWebRTCCodecs::ensureGPUProcessConnectionAndDispatchToThread(Function<void()>&& task)
{
    m_needsGPUProcessConnection = true;

    Locker locker { m_connectionLock };

    // Fast path when we already have a connection.
    if (m_connection) {
        dispatchToThread(WTFMove(task));
        return;
    }

    // We don't have a connection to the GPUProcess yet, we need to hop to the main thread to initiate it.
    m_tasksToDispatchAfterEstablishingConnection.append(WTFMove(task));
    if (m_tasksToDispatchAfterEstablishingConnection.size() != 1)
        return;

    callOnMainRunLoop([this] {
        Locker locker { m_connectionLock };
        ensureGPUProcessConnectionOnMainThreadWithLock();
        for (auto& task : std::exchange(m_tasksToDispatchAfterEstablishingConnection, { }))
            dispatchToThread(WTFMove(task));
    });
}

void LibWebRTCCodecs::gpuProcessConnectionMayNoLongerBeNeeded()
{
    ASSERT(!isMainRunLoop());
    if (m_encoders.isEmpty() && m_decoders.isEmpty())
        m_needsGPUProcessConnection = false;
}

LibWebRTCCodecs::~LibWebRTCCodecs()
{
    ASSERT_NOT_REACHED();
}

void LibWebRTCCodecs::setCallbacks(bool useGPUProcess, bool useRemoteFrames)
{
    ASSERT(isMainRunLoop());

    if (!useGPUProcess) {
        webrtc::setVideoDecoderCallbacks(nullptr, nullptr, nullptr, nullptr);
        webrtc::setVideoEncoderCallbacks(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
        return;
    }

    // Let's create WebProcess libWebRTCCodecs since it may be called from various threads.
    WebProcess::singleton().libWebRTCCodecs().m_useRemoteFrames = useRemoteFrames;

#if ENABLE(VP9)
    WebProcess::singleton().libWebRTCCodecs().setVP9VTBSupport(WebProcess::singleton().ensureGPUProcessConnection().hasVP9HardwareDecoder());
#endif

    webrtc::setVideoDecoderCallbacks(createVideoDecoder, releaseVideoDecoder, decodeVideoFrame, registerDecodeCompleteCallback);
    webrtc::setVideoEncoderCallbacks(createVideoEncoder, releaseVideoEncoder, initializeVideoEncoder, encodeVideoFrame, registerEncodeCompleteCallback, setEncodeRatesCallback);
}

LibWebRTCCodecs::Decoder* LibWebRTCCodecs::createDecoder(Type type)
{
    auto decoder = makeUnique<Decoder>();
    auto* result = decoder.get();
    decoder->identifier = RTCDecoderIdentifier::generateThreadSafe();
    decoder->type = type;

    ensureGPUProcessConnectionAndDispatchToThread([this, decoder = WTFMove(decoder)]() mutable {
        Locker locker { m_connectionLock };
        decoder->connection = m_connection;
        createRemoteDecoder(*decoder, *m_connection, m_useRemoteFrames);

        auto decoderIdentifier = decoder->identifier;
        ASSERT(!m_decoders.contains(decoderIdentifier));
        m_decoders.add(decoderIdentifier, WTFMove(decoder));
    });
    return result;
}

int32_t LibWebRTCCodecs::releaseDecoder(Decoder& decoder)
{
#if ASSERT_ENABLED
    {
        Locker locker { decoder.decodedImageCallbackLock };
        ASSERT(!decoder.decodedImageCallback);
    }
#endif
    ensureGPUProcessConnectionAndDispatchToThread([this, decoderIdentifier = decoder.identifier] {
        ASSERT(m_decoders.contains(decoderIdentifier));
        if (auto decoder = m_decoders.take(decoderIdentifier)) {
            decoder->connection->send(Messages::LibWebRTCCodecsProxy::ReleaseDecoder { decoderIdentifier }, 0);
            gpuProcessConnectionMayNoLongerBeNeeded();
        }
    });
    return 0;
}

int32_t LibWebRTCCodecs::decodeFrame(Decoder& decoder, uint32_t timeStamp, const uint8_t* data, size_t size, uint16_t width, uint16_t height)
{
    Locker locker { m_connectionLock };
    if (!decoder.connection || decoder.hasError) {
        decoder.hasError = false;
        return WEBRTC_VIDEO_CODEC_ERROR;
    }

    if (decoder.type == Type::VP9 && (width || height))
        decoder.connection->send(Messages::LibWebRTCCodecsProxy::SetFrameSize { decoder.identifier, width, height }, 0);

    decoder.connection->send(Messages::LibWebRTCCodecsProxy::DecodeFrame { decoder.identifier, timeStamp, IPC::DataReference { data, size } }, 0);
    return WEBRTC_VIDEO_CODEC_OK;
}

void LibWebRTCCodecs::registerDecodeFrameCallback(Decoder& decoder, void* decodedImageCallback)
{
    Locker locker { decoder.decodedImageCallbackLock };
    decoder.decodedImageCallback = decodedImageCallback;
}

void LibWebRTCCodecs::failedDecoding(RTCDecoderIdentifier decoderIdentifier)
{
    ASSERT(!isMainRunLoop());

    if (auto* decoder = m_decoders.get(decoderIdentifier))
        decoder->hasError = true;
}

void LibWebRTCCodecs::completedDecoding(RTCDecoderIdentifier decoderIdentifier, uint32_t timeStamp, WebCore::RemoteVideoSample&& remoteSample, std::optional<RemoteVideoFrameIdentifier> remoteFrameIdentifier)
{
    ASSERT(!isMainRunLoop());

    RefPtr<RemoteVideoFrameProxy> remoteVideoFrame;
    // We always create RemoteVideoFrameProxy so that we can release the corresponding GPUProcess IOSurface right away if there is no video source.
    if (remoteFrameIdentifier) {
        Locker locker { m_connectionLock };
        RemoteVideoFrameProxy::Properties properties { { *remoteFrameIdentifier, 0 }, remoteSample.time(), remoteSample.mirrored(), remoteSample.rotation(), remoteSample.size(), remoteSample.videoFormat() };
        remoteVideoFrame = RemoteVideoFrameProxy::create(*m_connection, properties, [proxy = m_remoteVideoFrameObjectHeapProxy](auto& frame, auto&& callback) {
            if (!proxy) {
                callback({ });
                return;
            }
            proxy->getVideoFrameBuffer(frame, WTFMove(callback));
        });
    }
    // FIXME: Do error logging.
    auto* decoder = m_decoders.get(decoderIdentifier);
    if (!decoder)
        return;

    if (!decoder->decodedImageCallbackLock.tryLock())
        return;

    Locker locker { AdoptLock, decoder->decodedImageCallbackLock };

    if (!decoder->decodedImageCallback)
        return;

    if (remoteVideoFrame) {
        webrtc::videoDecoderTaskComplete(decoder->decodedImageCallback, timeStamp, remoteSample.time().toDouble(), remoteVideoFrame.leakRef(),
            [](auto* pointer) { return static_cast<RemoteVideoFrameProxy*>(pointer)->pixelBuffer(); },
            [](auto* pointer) { static_cast<RemoteVideoFrameProxy*>(pointer)->deref(); },
            remoteSample.size().width(), remoteSample.size().height());
        return;
    }

    if (!remoteSample.surface())
        return;
    auto pixelBuffer = createCVPixelBuffer(remoteSample.surface()).value_or(nullptr);
    if (!pixelBuffer) {
        ASSERT_NOT_REACHED();
        return;
    }

    webrtc::videoDecoderTaskComplete(decoder->decodedImageCallback, timeStamp, remoteSample.time().toDouble(), pixelBuffer.get());
}

static inline String formatNameFromCodecType(LibWebRTCCodecs::Type type)
{
    switch (type) {
    case LibWebRTCCodecs::Type::H264:
        return "H264"_s;
    case LibWebRTCCodecs::Type::H265:
        return "H265"_s;
    case LibWebRTCCodecs::Type::VP9:
        return "VP9"_s;
    }
}

static inline webrtc::VideoCodecType toWebRTCCodecType(LibWebRTCCodecs::Type type)
{
    switch (type) {
    case LibWebRTCCodecs::Type::H264:
        return webrtc::kVideoCodecH264;
    case LibWebRTCCodecs::Type::H265:
        return webrtc::kVideoCodecH265;
    case LibWebRTCCodecs::Type::VP9:
        return webrtc::kVideoCodecVP9;
    }
}

LibWebRTCCodecs::Encoder* LibWebRTCCodecs::createEncoder(Type type, const std::map<std::string, std::string>& formatParameters)
{
    auto encoder = makeUnique<Encoder>();
    auto* result = encoder.get();
    encoder->identifier = RTCEncoderIdentifier::generateThreadSafe();
    encoder->codecType = toWebRTCCodecType(type);

    Vector<std::pair<String, String>> parameters;
    for (auto& keyValue : formatParameters)
        parameters.append(std::make_pair(String::fromUTF8(keyValue.first.data(), keyValue.first.length()), String::fromUTF8(keyValue.second.data(), keyValue.second.length())));

    ensureGPUProcessConnectionAndDispatchToThread([this, encoder = WTFMove(encoder), type, parameters = WTFMove(parameters)]() mutable {
        {
            Locker locker { m_connectionLock };
            encoder->connection = m_connection;
        }

        encoder->connection->send(Messages::LibWebRTCCodecsProxy::CreateEncoder { encoder->identifier, formatNameFromCodecType(type), parameters, RuntimeEnabledFeatures::sharedFeatures().webRTCH264LowLatencyEncoderEnabled() }, 0);
        encoder->parameters = WTFMove(parameters);

        Locker locker { m_encodersLock };
        auto encoderIdentifier = encoder->identifier;
        ASSERT(!m_encoders.contains(encoderIdentifier));
        m_encoders.add(encoderIdentifier, WTFMove(encoder));
    });
    return result;
}

int32_t LibWebRTCCodecs::releaseEncoder(Encoder& encoder)
{
#if ASSERT_ENABLED
    {
        Locker locker { encoder.encodedImageCallbackLock };
        ASSERT(!encoder.encodedImageCallback);
    }
#endif
    ensureGPUProcessConnectionAndDispatchToThread([this, encoderIdentifier = encoder.identifier] {
        Locker locker { m_encodersLock };
        ASSERT(m_encoders.contains(encoderIdentifier));
        auto encoder = m_encoders.take(encoderIdentifier);
        encoder->connection->send(Messages::LibWebRTCCodecsProxy::ReleaseEncoder { encoderIdentifier }, 0);
        gpuProcessConnectionMayNoLongerBeNeeded();
    });
    return 0;
}

int32_t LibWebRTCCodecs::initializeEncoder(Encoder& encoder, uint16_t width, uint16_t height, unsigned startBitRate, unsigned maxBitRate, unsigned minBitRate, uint32_t maxFrameRate)
{
    ensureGPUProcessConnectionAndDispatchToThread([this, encoderIdentifier = encoder.identifier, width, height, startBitRate, maxBitRate, minBitRate, maxFrameRate]() mutable {
        auto* encoder = m_encoders.get(encoderIdentifier);
        encoder->initializationData = EncoderInitializationData { width, height, startBitRate, maxBitRate, minBitRate, maxFrameRate };
        encoder->connection->send(Messages::LibWebRTCCodecsProxy::InitializeEncoder { encoderIdentifier, width, height, startBitRate, maxBitRate, minBitRate, maxFrameRate }, 0);
    });
    return 0;
}

template<typename Buffer>
bool copySharedVideoFrame(LibWebRTCCodecs::Encoder& encoder, Buffer&& frameBuffer)
{
    return encoder.sharedVideoFrameWriter.write(frameBuffer,
        [&](auto& semaphore) { encoder.connection->send(Messages::LibWebRTCCodecsProxy::SetSharedVideoFrameSemaphore { encoder.identifier, semaphore }, 0); },
        [&](auto& handle) { encoder.connection->send(Messages::LibWebRTCCodecsProxy::SetSharedVideoFrameMemory { encoder.identifier, handle }, 0); }
    );
}

int32_t LibWebRTCCodecs::encodeFrame(Encoder& encoder, const webrtc::VideoFrame& frame, bool shouldEncodeAsKeyFrame)
{
    Locker locker { m_encodersLock };
    if (!encoder.connection)
        return WEBRTC_VIDEO_CODEC_ERROR;

    std::optional<RemoteVideoFrameReadReference> remoteVideoFrameReadReference;
    if (auto provider = webrtc::videoFrameBufferProvider(frame)) {
        auto& mediaSample = *static_cast<MediaSample*>(provider);
        if (is<RemoteVideoFrameProxy>(mediaSample))
            remoteVideoFrameReadReference = downcast<RemoteVideoFrameProxy>(mediaSample).read();
    }

    RetainPtr<CVPixelBufferRef> buffer;
    if (!remoteVideoFrameReadReference) {
        buffer = adoptCF(webrtc::pixelBufferFromFrame(frame));
        if (!buffer) {
            // buffer is not native, we need to copy to shared video frame.
            if (!copySharedVideoFrame(encoder, frame))
                return WEBRTC_VIDEO_CODEC_ERROR;
        }
    }

    auto sample = RemoteVideoSample::create(buffer.get(), MediaTime(frame.timestamp_us() * 1000, 1000000), toMediaSampleVideoRotation(frame.rotation()), RemoteVideoSample::ShouldCheckForIOSurface::No);
    if (buffer && !sample->surface()) {
        // buffer is not IOSurface, we need to copy to shared video frame.
        if (!copySharedVideoFrame(encoder, buffer.get()))
            return WEBRTC_VIDEO_CODEC_ERROR;
    }

    encoder.connection->send(Messages::LibWebRTCCodecsProxy::EncodeFrame { encoder.identifier, *sample, frame.timestamp(), shouldEncodeAsKeyFrame, remoteVideoFrameReadReference }, 0);
    return WEBRTC_VIDEO_CODEC_OK;
}

void LibWebRTCCodecs::registerEncodeFrameCallback(Encoder& encoder, void* encodedImageCallback)
{
    Locker locker { encoder.encodedImageCallbackLock };

    encoder.encodedImageCallback = encodedImageCallback;
}

void LibWebRTCCodecs::setEncodeRates(Encoder& encoder, uint32_t bitRate, uint32_t frameRate)
{
    Locker locker { m_encodersLock };

    if (!encoder.connection) {
        callOnMainRunLoop([encoderIdentifier = encoder.identifier, bitRate, frameRate] {
            WebProcess::singleton().ensureGPUProcessConnection().connection().send(Messages::LibWebRTCCodecsProxy::SetEncodeRates { encoderIdentifier, bitRate, frameRate }, 0);
        });
        return;
    }
    encoder.connection->send(Messages::LibWebRTCCodecsProxy::SetEncodeRates { encoder.identifier, bitRate, frameRate }, 0);
}

void LibWebRTCCodecs::completedEncoding(RTCEncoderIdentifier identifier, IPC::DataReference&& data, const webrtc::WebKitEncodedFrameInfo& info)
{
    ASSERT(!isMainRunLoop());

    // FIXME: Do error logging.
    auto* encoder = m_encoders.get(identifier);
    if (!encoder)
        return;

    if (!encoder->encodedImageCallbackLock.tryLock())
        return;

    Locker locker { AdoptLock, encoder->encodedImageCallbackLock };

    if (!encoder->encodedImageCallback)
        return;

    webrtc::encoderVideoTaskComplete(encoder->encodedImageCallback, encoder->codecType, data.data(), data.size(), info);
}

CVPixelBufferPoolRef LibWebRTCCodecs::pixelBufferPool(size_t width, size_t height, OSType type)
{
    if (!m_pixelBufferPool || m_pixelBufferPoolWidth != width || m_pixelBufferPoolHeight != height) {
        m_pixelBufferPool = nullptr;
        m_pixelBufferPoolWidth = 0;
        m_pixelBufferPoolHeight = 0;
        if (auto pool = WebCore::createIOSurfaceCVPixelBufferPool(width, height, type)) {
            m_pixelBufferPool = WTFMove(*pool);
            m_pixelBufferPoolWidth = width;
            m_pixelBufferPoolHeight = height;
        }
    }
    return m_pixelBufferPool.get();
}

void LibWebRTCCodecs::dispatchToThread(Function<void()>&& callback)
{
    m_queue->dispatch(WTFMove(callback));
}

void LibWebRTCCodecs::gpuProcessConnectionDidClose(GPUProcessConnection&)
{
    ASSERT(isMainRunLoop());
    Locker locker { m_connectionLock };
    std::exchange(m_connection, nullptr)->removeThreadMessageReceiver(Messages::LibWebRTCCodecs::messageReceiverName());
    if (!m_needsGPUProcessConnection)
        return;

    ensureGPUProcessConnectionOnMainThreadWithLock();
    dispatchToThread([this, connection = m_connection]() {
        for (auto& decoder : m_decoders.values()) {
            createRemoteDecoder(*decoder, *connection, m_useRemoteFrames);
            decoder->connection = connection.get();
        }

        // In case we are waiting for GPUProcess, let's end the wait to not deadlock.
        for (auto& encoder : m_encoders.values())
            encoder->sharedVideoFrameWriter.disable();

        Locker locker { m_encodersLock };
        for (auto& encoder : m_encoders.values()) {
            connection->send(Messages::LibWebRTCCodecsProxy::CreateEncoder { encoder->identifier, formatNameFromWebRTCCodecType(encoder->codecType), encoder->parameters, RuntimeEnabledFeatures::sharedFeatures().webRTCH264LowLatencyEncoderEnabled() }, 0);
            if (encoder->initializationData)
                connection->send(Messages::LibWebRTCCodecsProxy::InitializeEncoder { encoder->identifier, encoder->initializationData->width, encoder->initializationData->height, encoder->initializationData->startBitRate, encoder->initializationData->maxBitRate, encoder->initializationData->minBitRate, encoder->initializationData->maxFrameRate }, 0);
            encoder->connection = connection.get();
            encoder->sharedVideoFrameWriter = { };
        }
    });
}

void LibWebRTCCodecs::setLoggingLevel(WTFLogLevel level)
{
    m_loggingLevel = level;

    Locker locker { m_connectionLock };
    if (m_connection)
        m_connection->send(Messages::LibWebRTCCodecsProxy::SetRTCLoggingLevel(level), 0);
}

}

#endif
