/*
 * Copyright (C) 2020 Apple Inc. All rights reserved.
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
#include "RemoteRealtimeMediaSourceProxy.h"

#if PLATFORM(COCOA) && ENABLE(MEDIA_STREAM)

#include "GPUProcessConnection.h"
#include "SharedRingBufferStorage.h"
#include "UserMediaCaptureManager.h"
#include "UserMediaCaptureManagerMessages.h"
#include "UserMediaCaptureManagerProxyMessages.h"
#include "WebCoreArgumentCoders.h"
#include "WebProcess.h"
#include <WebCore/MediaConstraints.h>
#include <WebCore/RealtimeMediaSource.h>
#include <WebCore/RealtimeMediaSourceCenter.h>
#include <WebCore/WebAudioBufferList.h>

namespace WebKit {
using namespace WebCore;

RemoteRealtimeMediaSourceProxy::~RemoteRealtimeMediaSourceProxy()
{
    failApplyConstraintCallbacks("Source terminated"_s);
}

IPC::Connection* RemoteRealtimeMediaSourceProxy::connection()
{
    ASSERT(isMainRunLoop());
#if ENABLE(GPU_PROCESS)
    if (m_shouldCaptureInGPUProcess)
        return &WebProcess::singleton().ensureGPUProcessConnection().connection();
#endif
    return WebProcess::singleton().parentProcessConnection();
}

void RemoteRealtimeMediaSourceProxy::startProducingData()
{
    connection()->send(Messages::UserMediaCaptureManagerProxy::StartProducingData { m_identifier }, 0);
}

void RemoteRealtimeMediaSourceProxy::stopProducingData()
{
    connection()->send(Messages::UserMediaCaptureManagerProxy::StopProducingData { m_identifier }, 0);
}

void RemoteRealtimeMediaSourceProxy::createRemoteMediaSource(const String& deviceIDHashSalt, CreateCallback&& callback, bool shouldUseRemoteFrame)
{
    connection()->sendWithAsyncReply(Messages::UserMediaCaptureManagerProxy::CreateMediaSourceForCaptureDeviceWithConstraints(identifier(), m_device, deviceIDHashSalt, m_constraints, shouldUseRemoteFrame), WTFMove(callback));
}

void RemoteRealtimeMediaSourceProxy::applyConstraints(const MediaConstraints& constraints, RealtimeMediaSource::ApplyConstraintsHandler&& completionHandler)
{
    m_pendingApplyConstraintsCallbacks.append(WTFMove(completionHandler));
    // FIXME: Use sendAsyncWithReply.
    connection()->send(Messages::UserMediaCaptureManagerProxy::ApplyConstraints { m_identifier, constraints }, 0);
}

void RemoteRealtimeMediaSourceProxy::applyConstraintsSucceeded()
{
    auto callback = m_pendingApplyConstraintsCallbacks.takeFirst();
    callback({ });
}

void RemoteRealtimeMediaSourceProxy::applyConstraintsFailed(String&& failedConstraint, String&& errorMessage)
{
    auto callback = m_pendingApplyConstraintsCallbacks.takeFirst();
    callback(RealtimeMediaSource::ApplyConstraintsError { WTFMove(failedConstraint), WTFMove(errorMessage) });
}

void RemoteRealtimeMediaSourceProxy::failApplyConstraintCallbacks(const String& errorMessage)
{
    auto callbacks = WTFMove(m_pendingApplyConstraintsCallbacks);
    while (!callbacks.isEmpty())
        callbacks.takeFirst()(RealtimeMediaSource::ApplyConstraintsError { { }, errorMessage });
}

void RemoteRealtimeMediaSourceProxy::hasEnded()
{
    connection()->send(Messages::UserMediaCaptureManagerProxy::End { m_identifier }, 0);
}

void RemoteRealtimeMediaSourceProxy::whenReady(CompletionHandler<void(String)>&& callback)
{
    if (m_isReady)
        return callback(WTFMove(m_errorMessage));
    m_callback = WTFMove(callback);
}

void RemoteRealtimeMediaSourceProxy::setAsReady()
{
    ASSERT(!m_isReady);
    m_isReady = true;
    if (m_callback)
        m_callback({ });
}

void RemoteRealtimeMediaSourceProxy::didFail(String&& errorMessage)
{
    m_isReady = true;
    m_errorMessage = WTFMove(errorMessage);
    if (m_callback)
        m_callback(m_errorMessage);
}

}

#endif
