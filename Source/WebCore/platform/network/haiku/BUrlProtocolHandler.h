/*
    Copyright (C) 2008 Nokia Corporation and/or its subsidiary(-ies)

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.

    You should have received a copy of the GNU Library General Public License
    along with this library; see the file COPYING.LIB.  If not, write to
    the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
    Boston, MA 02110-1301, USA.
*/
#ifndef BURLPROTOCOLHANDLER_H
#define BURLPROTOCOLHANDLER_H

#include "HaikuFormDataStream.h"
#include "ResourceRequest.h"
#include "ResourceError.h"

#include <support/Locker.h>
#include <Messenger.h>
#include <private/netservices/HttpRequest.h>
#include <UrlProtocolAsynchronousListener.h>

class BFile;

namespace WebCore {

class NetworkingContext;
class NetworkStorageSession;
class ResourceHandle;
class ResourceResponse;

class BUrlProtocolHandler;
class BUrlRequestWrapper;

class BUrlRequestWrapper : public ThreadSafeRefCounted<BUrlRequestWrapper>, public BPrivate::Network::BUrlProtocolAsynchronousListener, public BDataIO {
public:
    static RefPtr<BUrlRequestWrapper> create(BUrlProtocolHandler*, NetworkStorageSession*, ResourceRequest&);
    virtual ~BUrlRequestWrapper();

    void abort();

    bool isValid() const { return m_request; };

    // BUrlProtocolListener hooks
    void HeadersReceived(BPrivate::Network::BUrlRequest* caller) override;
    void UploadProgress(BPrivate::Network::BUrlRequest* caller, off_t bytesSent, off_t bytesTotal) override;
    void RequestCompleted(BPrivate::Network::BUrlRequest* caller, bool success) override;
    bool CertificateVerificationFailed(BPrivate::Network::BUrlRequest* caller, BCertificate&, const char* message) override;

    // BDataIO
    ssize_t Write(const void*, size_t) override;

private:
    BUrlRequestWrapper(BUrlProtocolHandler*, NetworkStorageSession*, ResourceRequest&);

private:
    BUrlProtocolHandler* m_handler { nullptr };
    BPrivate::Network::BUrlRequest* m_request { nullptr };

    bool m_didReceiveData { false };

    // This lock is in charge of two things:
    // - Whether data can be received.
    // - Synchronizing cancellation via m_handler.
    BLocker m_receiveMutex;
};

class BUrlProtocolHandler {
public:
    explicit BUrlProtocolHandler(ResourceHandle *handle);
    virtual ~BUrlProtocolHandler();

    void abort();

    bool isValid() const { return m_request && m_request->isValid(); }

private:
    void didFail(const ResourceError& error);
    void willSendRequest(const ResourceResponse& response);
    void continueAfterWillSendRequest(ResourceRequest&& request);
    bool didReceiveAuthenticationChallenge(const ResourceResponse& response);
    void didReceiveResponse(ResourceResponse&& response);
    void didReceiveBuffer(Ref<SharedBuffer>&&);
    void didSendData(ssize_t bytesSent, ssize_t bytesTotal);
    void didFinishLoading();
    bool didReceiveInvalidCertificate(BCertificate&, const char* message);

private:
    friend class BUrlRequestWrapper;

    ResourceRequest m_resourceRequest;
    ResourceHandle* m_resourceHandle;
    RefPtr<BUrlRequestWrapper> m_request;

    unsigned m_redirectionTries { 0 };
    unsigned m_authenticationTries { 0 };
};

}

#endif // BURLPROTOCOLHANDLER_H
