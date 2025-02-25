/*
 * Copyright (C) 2014 Apple Inc. All rights reserved.
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

#import <WebCore/Timer.h>
#import <wtf/HashSet.h>
#import <wtf/Noncopyable.h>
#import <wtf/OptionSet.h>

namespace WebCore {
class ImageBuffer;
}

namespace WebKit {

class RemoteLayerBackingStore;
class RemoteLayerTreeContext;
class RemoteLayerTreeTransaction;

class RemoteLayerBackingStoreCollection {
    WTF_MAKE_NONCOPYABLE(RemoteLayerBackingStoreCollection);
    WTF_MAKE_FAST_ALLOCATED;
public:
    RemoteLayerBackingStoreCollection(RemoteLayerTreeContext&);
    virtual ~RemoteLayerBackingStoreCollection();

    void backingStoreWasCreated(RemoteLayerBackingStore&);
    void backingStoreWillBeDestroyed(RemoteLayerBackingStore&);

    // Return value indicates whether the backing store needs to be included in the transaction.
    bool backingStoreWillBeDisplayed(RemoteLayerBackingStore&);
    void backingStoreBecameUnreachable(RemoteLayerBackingStore&);

    void willFlushLayers();
    void willCommitLayerTree(RemoteLayerTreeTransaction&);
    void didFlushLayers();

    void tryMarkAllBackingStoreVolatile(CompletionHandler<void(bool)>&&);

    void scheduleVolatilityTimer();

    virtual RefPtr<WebCore::ImageBuffer> allocateBufferForBackingStore(const RemoteLayerBackingStore&);

protected:
    RemoteLayerTreeContext& layerTreeContext() const { return m_layerTreeContext; }

private:
    enum class VolatilityMarkingBehavior : uint8_t {
        IgnoreReachability              = 1 << 0,
        ConsiderTimeSinceLastDisplay    = 1 << 1,
    };
    bool markBackingStoreVolatile(RemoteLayerBackingStore&, OptionSet<VolatilityMarkingBehavior> = { }, MonotonicTime = { });

    bool markAllBackingStoreVolatile(OptionSet<VolatilityMarkingBehavior> liveBackingStoreMarkingBehavior, OptionSet<VolatilityMarkingBehavior> unparentedBackingStoreMarkingBehavior);
    void volatilityTimerFired();

    RemoteLayerTreeContext& m_layerTreeContext;

    HashSet<RemoteLayerBackingStore*> m_liveBackingStore;
    HashSet<RemoteLayerBackingStore*> m_unparentedBackingStore;

    // Only used during a single flush.
    HashSet<RemoteLayerBackingStore*> m_reachableBackingStoreInLatestFlush;

    WebCore::Timer m_volatilityTimer;

    bool m_inLayerFlush { false };
};

} // namespace WebKit
