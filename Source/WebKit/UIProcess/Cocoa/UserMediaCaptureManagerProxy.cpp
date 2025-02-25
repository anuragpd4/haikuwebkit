/*
 * Copyright (C) 2017-2020 Apple Inc. All rights reserved.
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
#include "UserMediaCaptureManagerProxy.h"

#if PLATFORM(COCOA) && ENABLE(MEDIA_STREAM)

#include "Connection.h"
#include "RemoteCaptureSampleManagerMessages.h"
#include "RemoteVideoFrameObjectHeap.h"
#include "SharedRingBufferStorage.h"
#include "UserMediaCaptureManagerMessages.h"
#include "UserMediaCaptureManagerProxyMessages.h"
#include "WebCoreArgumentCoders.h"
#include "WebProcessProxy.h"
#include <WebCore/AudioSession.h>
#include <WebCore/AudioUtilities.h>
#include <WebCore/CARingBuffer.h>
#include <WebCore/ImageRotationSessionVT.h>
#include <WebCore/MediaConstraints.h>
#include <WebCore/MediaSampleAVFObjC.h>
#include <WebCore/RealtimeMediaSourceCenter.h>
#include <WebCore/RealtimeVideoSource.h>
#include <WebCore/RemoteVideoSample.h>
#include <WebCore/WebAudioBufferList.h>
#include <wtf/UniqueRef.h>

#define MESSAGE_CHECK(assertion) MESSAGE_CHECK_BASE(assertion, &m_connectionProxy->connection())

namespace WebKit {
using namespace WebCore;

class UserMediaCaptureManagerProxy::SourceProxy
    : private RealtimeMediaSource::Observer
    , private RealtimeMediaSource::AudioSampleObserver
    , private RealtimeMediaSource::VideoSampleObserver {
    WTF_MAKE_FAST_ALLOCATED;
public:
    SourceProxy(RealtimeMediaSourceIdentifier id, Ref<IPC::Connection>&& connection, ProcessIdentity&& resourceOwner, Ref<RealtimeMediaSource>&& source, RefPtr<RemoteVideoFrameObjectHeap>&& videoFrameObjectHeap)
        : m_id(id)
        , m_connection(WTFMove(connection))
        , m_resourceOwner(WTFMove(resourceOwner))
        , m_source(WTFMove(source))
        , m_videoFrameObjectHeap(WTFMove(videoFrameObjectHeap))
    {
        m_source->addObserver(*this);
        switch (m_source->type()) {
        case RealtimeMediaSource::Type::Audio:
        case RealtimeMediaSource::Type::SystemAudio:
            m_source->addAudioSampleObserver(*this);
            break;
        case RealtimeMediaSource::Type::Video:
        case RealtimeMediaSource::Type::Screen:
        case RealtimeMediaSource::Type::Window:
            m_source->addVideoSampleObserver(*this);
            break;
        case RealtimeMediaSource::Type::None:
            ASSERT_NOT_REACHED();
        }
    }

    ~SourceProxy()
    {
        // Make sure the rendering thread is stopped before we proceed with the destruction.
        stop();

        switch (m_source->type()) {
        case RealtimeMediaSource::Type::Audio:
        case RealtimeMediaSource::Type::SystemAudio:
            m_source->removeAudioSampleObserver(*this);
            break;
        case RealtimeMediaSource::Type::Video:
        case RealtimeMediaSource::Type::Screen:
        case RealtimeMediaSource::Type::Window:
            m_source->removeVideoSampleObserver(*this);
            break;
        case RealtimeMediaSource::Type::None:
            ASSERT_NOT_REACHED();
        }
        m_source->removeObserver(*this);

        if (m_ringBuffer)
            static_cast<SharedRingBufferStorage&>(m_ringBuffer->storage()).invalidate();
    }

    RealtimeMediaSource& source() { return m_source; }
    CAAudioStreamDescription& description() { return m_description; }
    int64_t numberOfFrames() { return m_numberOfFrames; }

    void audioUnitWillStart() final
    {
        AudioSession::sharedSession().setCategory(AudioSession::CategoryType::PlayAndRecord, RouteSharingPolicy::Default);
    }

    void start()
    {
        m_shouldReset = true;
        m_isEnded = false;
        m_source->start();
    }

    void stop()
    {
        m_isEnded = true;
        m_source->stop();
    }

    void requestToEnd()
    {
        m_isEnded = true;
        m_source->requestToEnd(*this);
    }

    void setShouldApplyRotation(bool shouldApplyRotation) { m_shouldApplyRotation = true; }

private:
    void sourceStopped() final {
        if (m_source->captureDidFail()) {
            m_connection->send(Messages::UserMediaCaptureManager::CaptureFailed(m_id), 0);
            return;
        }
        m_connection->send(Messages::UserMediaCaptureManager::SourceStopped(m_id), 0);
    }

    void sourceMutedChanged() final {
        m_connection->send(Messages::UserMediaCaptureManager::SourceMutedChanged(m_id, m_source->muted()), 0);
    }

    void sourceSettingsChanged() final {
        m_connection->send(Messages::UserMediaCaptureManager::SourceSettingsChanged(m_id, m_source->settings()), 0);
    }

    // May get called on a background thread.
    void audioSamplesAvailable(const MediaTime& time, const PlatformAudioData& audioData, const AudioStreamDescription& description, size_t numberOfFrames) final {
        if (m_description != description || m_shouldReset) {
            DisableMallocRestrictionsForCurrentThreadScope scope;

            m_shouldReset = false;
            m_writeOffset = 0;
            m_remainingFrameCount = 0;
            m_startTime = time;
            m_captureSemaphore = makeUnique<IPC::Semaphore>();
            ASSERT(description.platformDescription().type == PlatformDescription::CAAudioStreamBasicType);
            m_description = *std::get<const AudioStreamBasicDescription*>(description.platformDescription().description);

            m_frameChunkSize = std::max(WebCore::AudioUtilities::renderQuantumSize, AudioSession::sharedSession().preferredBufferSize());

            // Allocate a ring buffer large enough to contain 2 seconds of audio.
            m_numberOfFrames = m_description.sampleRate() * 2;
            m_ringBuffer.reset();
            m_ringBuffer = makeUnique<CARingBuffer>(makeUniqueRef<SharedRingBufferStorage>(std::bind(&SourceProxy::storageChanged, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3)));
            m_ringBuffer->allocate(m_description.streamDescription(), m_numberOfFrames);
        }

        ASSERT(is<WebAudioBufferList>(audioData));
        m_ringBuffer->store(downcast<WebAudioBufferList>(audioData).list(), numberOfFrames, m_writeOffset);
        m_writeOffset += numberOfFrames;

        size_t framesToSend = numberOfFrames + m_remainingFrameCount;
        size_t signalCount = framesToSend / m_frameChunkSize;
        m_remainingFrameCount = framesToSend - (signalCount * m_frameChunkSize);
        for (unsigned i = 0; i < signalCount; ++i)
            m_captureSemaphore->signal();
    }

    RefPtr<MediaSample> rotateVideoFrameIfNeeded(MediaSample& sample)
    {
        if (m_shouldApplyRotation && sample.videoRotation() != MediaSample::VideoRotation::None) {
            auto pixelBuffer = rotatePixelBuffer(sample);
            return MediaSampleAVFObjC::createImageSample(WTFMove(pixelBuffer), MediaSample::VideoRotation::None, sample.videoMirrored(), sample.presentationTime(), sample.decodeTime());
        }
        return &sample;
    }

    void videoSampleAvailable(MediaSample& sample, VideoSampleMetadata metadata) final
    {
        auto videoSample = rotateVideoFrameIfNeeded(sample);
        if (!videoSample)
            return;

        auto remoteSample = RemoteVideoSample::create(*videoSample);
        if (!remoteSample)
            return;

        if (m_resourceOwner)
            remoteSample->setOwnershipIdentity(m_resourceOwner);

        std::optional<RemoteVideoFrameIdentifier> remoteIdentifier;
        if (m_videoFrameObjectHeap) {
            remoteIdentifier = m_videoFrameObjectHeap->createRemoteVideoFrame(sample);
            remoteSample->clearIOSurface();
        }

        m_connection->send(Messages::RemoteCaptureSampleManager::VideoSampleAvailable(m_id, WTFMove(*remoteSample), remoteIdentifier, metadata), 0);
    }

    RetainPtr<CVPixelBufferRef> rotatePixelBuffer(MediaSample& sample)
    {
        if (!m_rotationSession)
            m_rotationSession = makeUnique<ImageRotationSessionVT>();

        ImageRotationSessionVT::RotationProperties rotation;
        switch (sample.videoRotation()) {
        case MediaSample::VideoRotation::None:
            ASSERT_NOT_REACHED();
            rotation.angle = 0;
            break;
        case MediaSample::VideoRotation::UpsideDown:
            rotation.angle = 180;
            break;
        case MediaSample::VideoRotation::Right:
            rotation.angle = 90;
            break;
        case MediaSample::VideoRotation::Left:
            rotation.angle = 270;
            break;
        }
        return m_rotationSession->rotate(sample, rotation, ImageRotationSessionVT::IsCGImageCompatible::No);
    }

    void storageChanged(SharedMemory* storage, const WebCore::CAAudioStreamDescription& format, size_t frameCount)
    {
        SharedMemory::Handle handle;
        if (storage)
            storage->createHandle(handle, SharedMemory::Protection::ReadOnly);

        // FIXME: Send the actual data size with IPCHandle.
#if OS(DARWIN) || OS(WINDOWS)
        uint64_t dataSize = handle.size();
#else
        uint64_t dataSize = 0;
#endif
        m_connection->send(Messages::RemoteCaptureSampleManager::AudioStorageChanged(m_id, SharedMemory::IPCHandle { WTFMove(handle),  dataSize }, format, frameCount, *m_captureSemaphore, m_startTime, m_frameChunkSize), 0);
    }

    bool preventSourceFromStopping()
    {
        // Do not allow the source to stop if we are still using it.
        return !m_isEnded;
    }

    RealtimeMediaSourceIdentifier m_id;
    WeakPtr<PlatformMediaSessionManager> m_sessionManager;
    Ref<IPC::Connection> m_connection;
    ProcessIdentity m_resourceOwner;
    Ref<RealtimeMediaSource> m_source;
    std::unique_ptr<CARingBuffer> m_ringBuffer;
    CAAudioStreamDescription m_description { };
    int64_t m_numberOfFrames { 0 };
    bool m_isEnded { false };
    std::unique_ptr<ImageRotationSessionVT> m_rotationSession;
    bool m_shouldApplyRotation { false };
    std::unique_ptr<IPC::Semaphore> m_captureSemaphore;
    int64_t m_writeOffset { 0 };
    int64_t m_remainingFrameCount { 0 };
    size_t m_frameChunkSize { 0 };
    MediaTime m_startTime;
    bool m_shouldReset { false };
    RefPtr<RemoteVideoFrameObjectHeap> m_videoFrameObjectHeap;
};

UserMediaCaptureManagerProxy::UserMediaCaptureManagerProxy(UniqueRef<ConnectionProxy>&& connectionProxy)
    : m_connectionProxy(WTFMove(connectionProxy))
{
    m_connectionProxy->addMessageReceiver(Messages::UserMediaCaptureManagerProxy::messageReceiverName(), *this);
}

UserMediaCaptureManagerProxy::~UserMediaCaptureManagerProxy()
{
    m_connectionProxy->removeMessageReceiver(Messages::UserMediaCaptureManagerProxy::messageReceiverName());
}

void UserMediaCaptureManagerProxy::createMediaSourceForCaptureDeviceWithConstraints(RealtimeMediaSourceIdentifier id, const CaptureDevice& device, String&& hashSalt, const MediaConstraints& mediaConstraints, bool shouldUseGPUProcessRemoteFrames, CreateSourceCallback&& completionHandler)
{
    if (!m_connectionProxy->willStartCapture(device.type()))
        return completionHandler(false, "Request is not allowed"_s, RealtimeMediaSourceSettings { }, { }, { }, { }, 0);

    auto* constraints = mediaConstraints.isValid ? &mediaConstraints : nullptr;

    CaptureSourceOrError sourceOrError;
    switch (device.type()) {
    case WebCore::CaptureDevice::DeviceType::Microphone:
        sourceOrError = RealtimeMediaSourceCenter::singleton().audioCaptureFactory().createAudioCaptureSource(device, WTFMove(hashSalt), constraints);
        break;
    case WebCore::CaptureDevice::DeviceType::Camera:
        sourceOrError = RealtimeMediaSourceCenter::singleton().videoCaptureFactory().createVideoCaptureSource(device, WTFMove(hashSalt), constraints);
        if (sourceOrError)
            sourceOrError.captureSource->monitorOrientation(m_orientationNotifier);
        break;
    case WebCore::CaptureDevice::DeviceType::Screen:
    case WebCore::CaptureDevice::DeviceType::Window:
        sourceOrError = RealtimeMediaSourceCenter::singleton().displayCaptureFactory().createDisplayCaptureSource(device, WTFMove(hashSalt), constraints);
        break;
    case WebCore::CaptureDevice::DeviceType::SystemAudio:
    case WebCore::CaptureDevice::DeviceType::Speaker:
    case WebCore::CaptureDevice::DeviceType::Unknown:
        ASSERT_NOT_REACHED();
        break;
    }

    bool succeeded = !!sourceOrError;
    String invalidConstraints;
    RealtimeMediaSourceSettings settings;
    RealtimeMediaSourceCapabilities capabilities;
    Vector<VideoPresetData> presets;
    IntSize size;
    double frameRate = 0;

    if (sourceOrError) {
        auto source = sourceOrError.source();
#if !RELEASE_LOG_DISABLED
        source->setLogger(m_connectionProxy->logger(), LoggerHelper::uniqueLogIdentifier());
#endif
        settings = source->settings();
        capabilities = source->capabilities();
        if (source->isVideoSource()) {
            auto& videoSource = static_cast<RealtimeVideoSource&>(source.get());
            presets = videoSource.presetsData();
            size = videoSource.size();
            frameRate = videoSource.frameRate();
        }

        ASSERT(!m_proxies.contains(id));
        auto proxy = makeUnique<SourceProxy>(id, m_connectionProxy->connection(), ProcessIdentity { m_connectionProxy->resourceOwner() }, WTFMove(source), shouldUseGPUProcessRemoteFrames ? m_connectionProxy->remoteVideoFrameObjectHeap() : nullptr);
        m_proxies.add(id, WTFMove(proxy));
    } else
        invalidConstraints = WTFMove(sourceOrError.errorMessage);

    completionHandler(succeeded, invalidConstraints, WTFMove(settings), WTFMove(capabilities), WTFMove(presets), size, frameRate);
}

void UserMediaCaptureManagerProxy::startProducingData(RealtimeMediaSourceIdentifier id)
{
    auto* proxy = m_proxies.get(id);
    if (!proxy)
        return;

    if (!m_connectionProxy->setCaptureAttributionString()) {
        RELEASE_LOG_ERROR(WebRTC, "Unable to set capture attribution, failing capture.");
        return;
    }

#if ENABLE(APP_PRIVACY_REPORT)
    m_connectionProxy->setTCCIdentity();
#endif
    m_connectionProxy->startProducingData(proxy->source().type());
    proxy->start();
}

void UserMediaCaptureManagerProxy::stopProducingData(RealtimeMediaSourceIdentifier id)
{
    if (auto* proxy = m_proxies.get(id))
        proxy->stop();
}

void UserMediaCaptureManagerProxy::end(RealtimeMediaSourceIdentifier id)
{
    m_proxies.remove(id);
}

void UserMediaCaptureManagerProxy::capabilities(RealtimeMediaSourceIdentifier id, CompletionHandler<void(RealtimeMediaSourceCapabilities&&)>&& completionHandler)
{
    RealtimeMediaSourceCapabilities capabilities;
    if (auto* proxy = m_proxies.get(id))
        capabilities = proxy->source().capabilities();
    completionHandler(WTFMove(capabilities));
}

void UserMediaCaptureManagerProxy::applyConstraints(RealtimeMediaSourceIdentifier id, const WebCore::MediaConstraints& constraints)
{
    auto* proxy = m_proxies.get(id);
    if (!proxy) {
        m_connectionProxy->connection().send(Messages::UserMediaCaptureManager::ApplyConstraintsFailed(id, { }, "Unknown source"_s), 0);
        return;
    }

    auto& source = proxy->source();
    auto result = source.applyConstraints(constraints);
    if (!result)
        m_connectionProxy->connection().send(Messages::UserMediaCaptureManager::ApplyConstraintsSucceeded(id, source.settings()), 0);
    else
        m_connectionProxy->connection().send(Messages::UserMediaCaptureManager::ApplyConstraintsFailed(id, result->badConstraint, result->message), 0);
}

void UserMediaCaptureManagerProxy::clone(RealtimeMediaSourceIdentifier clonedID, RealtimeMediaSourceIdentifier newSourceID)
{
    MESSAGE_CHECK(m_proxies.contains(clonedID));
    MESSAGE_CHECK(!m_proxies.contains(newSourceID));
    if (auto* proxy = m_proxies.get(clonedID))
        m_proxies.add(newSourceID, makeUnique<SourceProxy>(newSourceID, m_connectionProxy->connection(), ProcessIdentity { m_connectionProxy->resourceOwner() }, proxy->source().clone(), m_connectionProxy->remoteVideoFrameObjectHeap()));
}

void UserMediaCaptureManagerProxy::requestToEnd(RealtimeMediaSourceIdentifier sourceID)
{
    if (auto* proxy = m_proxies.get(sourceID))
        proxy->requestToEnd();
}

void UserMediaCaptureManagerProxy::setShouldApplyRotation(RealtimeMediaSourceIdentifier sourceID, bool shouldApplyRotation)
{
    if (auto* proxy = m_proxies.get(sourceID))
        proxy->setShouldApplyRotation(shouldApplyRotation);
}

void UserMediaCaptureManagerProxy::clear()
{
    m_proxies.clear();
}

void UserMediaCaptureManagerProxy::setOrientation(uint64_t orientation)
{
    m_orientationNotifier.orientationChanged(orientation);
}

bool UserMediaCaptureManagerProxy::hasSourceProxies() const
{
    return !m_proxies.isEmpty();
}

}

#undef MESSAGE_CHECK

#endif
