/*
 * Copyright (C) 2021 Apple Inc. All rights reserved.
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

#pragma once

#include "Connection.h"
#include "StorageAreaIdentifier.h"
#include "StorageAreaMapIdentifier.h"
#include <wtf/WorkQueue.h>

namespace WebCore {
struct ClientOrigin;
struct SecurityOriginData;
}

namespace WebKit {

class MemoryStorageArea;
class StorageAreaBase;
class StorageAreaRegistry;

class LocalStorageManager {
    WTF_MAKE_FAST_ALLOCATED;
public:
    static Vector<WebCore::SecurityOriginData> originsOfLocalStorageData(const String& path);
    static String localStorageFilePath(const String& path, const WebCore::ClientOrigin&);

    LocalStorageManager(const String& path, StorageAreaRegistry&);
    bool isActive() const;
    bool hasDataInMemory() const;
    void clearDataInMemory();
    void clearDataOnDisk();
    void close();
    void handleLowMemoryWarning();
    void syncLocalStorage();
    void connectionClosed(IPC::Connection::UniqueID);

    StorageAreaIdentifier connectToLocalStorageArea(IPC::Connection::UniqueID, StorageAreaMapIdentifier, const WebCore::ClientOrigin&, Ref<WorkQueue>&&);
    StorageAreaIdentifier connectToTransientLocalStorageArea(IPC::Connection::UniqueID, StorageAreaMapIdentifier, const WebCore::ClientOrigin&);
    void disconnectFromStorageArea(IPC::Connection::UniqueID, StorageAreaIdentifier);

private:
    void connectionClosedForLocalStorageArea(IPC::Connection::UniqueID);
    void connectionClosedForTransientStorageArea(IPC::Connection::UniqueID);

    String m_path;
    StorageAreaRegistry& m_registry;
    std::unique_ptr<MemoryStorageArea> m_transientStorageArea;
    std::unique_ptr<StorageAreaBase> m_localStorageArea;
};

} // namespace WebKit


