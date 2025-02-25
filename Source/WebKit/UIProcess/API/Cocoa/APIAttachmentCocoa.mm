/*
 * Copyright (C) 2018 Apple Inc. All rights reserved.
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

#import "config.h"
#import "APIAttachment.h"

#import "PageClient.h"
#import <WebCore/MIMETypeRegistry.h>
#import <WebCore/SharedBuffer.h>

#if PLATFORM(IOS_FAMILY)
#import <MobileCoreServices/MobileCoreServices.h>
#else
#import <CoreServices/CoreServices.h>
#endif

namespace API {

static WTF::String mimeTypeInferredFromFileExtension(const API::Attachment& attachment)
{
    if (NSString *fileExtension = [(NSString *)attachment.fileName() pathExtension])
        return WebCore::MIMETypeRegistry::mimeTypeForExtension(fileExtension);

    return { };
}

static BOOL isDeclaredOrDynamicTypeIdentifier(NSString *type)
{
ALLOW_DEPRECATED_DECLARATIONS_BEGIN
    return UTTypeIsDeclared((__bridge CFStringRef)type) || UTTypeIsDynamic((__bridge CFStringRef)type);
ALLOW_DEPRECATED_DECLARATIONS_END
}

NSFileWrapper *Attachment::fileWrapper() const
{
    if (m_fileWrapperGenerator && !m_fileWrapper)
        m_fileWrapper = m_fileWrapperGenerator();
    return m_fileWrapper.get();
}

void Attachment::invalidateGeneratedFileWrapper()
{
    ASSERT(m_fileWrapperGenerator);
    m_fileWrapper = nil;
    m_webPage->didInvalidateDataForAttachment(*this);
}

WTF::String Attachment::mimeType() const
{
    NSString *contentType = m_contentType.isEmpty() ? mimeTypeInferredFromFileExtension(*this) : m_contentType;
    if (!contentType.length)
        return nullString();
    if (!isDeclaredOrDynamicTypeIdentifier(contentType))
        return contentType;

ALLOW_DEPRECATED_DECLARATIONS_BEGIN
    return adoptCF(UTTypeCopyPreferredTagWithClass((__bridge CFStringRef)contentType, kUTTagClassMIMEType)).get();
ALLOW_DEPRECATED_DECLARATIONS_END
}

WTF::String Attachment::utiType() const
{
    NSString *contentType = m_contentType.isEmpty() ? mimeTypeInferredFromFileExtension(*this) : m_contentType;
    if (!contentType.length)
        return nullString();
    if (isDeclaredOrDynamicTypeIdentifier(contentType))
        return contentType;

ALLOW_DEPRECATED_DECLARATIONS_BEGIN
    return adoptCF(UTTypeCreatePreferredIdentifierForTag(kUTTagClassMIMEType, (__bridge CFStringRef)contentType, nullptr)).get();
ALLOW_DEPRECATED_DECLARATIONS_END
}

WTF::String Attachment::fileName() const
{
    auto fileWrapper = this->fileWrapper();

    if ([fileWrapper filename].length)
        return [fileWrapper filename];

    return [fileWrapper preferredFilename];
}

void Attachment::setFileWrapperAndUpdateContentType(NSFileWrapper *fileWrapper, NSString *contentType)
{
    if (!contentType.length) {
ALLOW_DEPRECATED_DECLARATIONS_BEGIN
        if (fileWrapper.directory)
            contentType = (NSString *)kUTTypeDirectory;
        else if (fileWrapper.regularFile) {
            if (NSString *pathExtension = (fileWrapper.filename.length ? fileWrapper.filename : fileWrapper.preferredFilename).pathExtension)
                contentType = WebCore::MIMETypeRegistry::mimeTypeForExtension(pathExtension);
            if (!contentType.length)
                contentType = (NSString *)kUTTypeData;
        }
ALLOW_DEPRECATED_DECLARATIONS_END
    }

    setContentType(contentType);
    setFileWrapper(fileWrapper);
}

std::optional<uint64_t> Attachment::fileSizeForDisplay() const
{
    auto fileWrapper = this->fileWrapper();

    if (![fileWrapper isRegularFile]) {
        // FIXME: We should display a size estimate for directory-type file wrappers.
        return std::nullopt;
    }

    if (auto fileSize = [[fileWrapper fileAttributes][NSFileSize] unsignedLongLongValue])
        return fileSize;

    return [fileWrapper regularFileContents].length;
}

RefPtr<WebCore::FragmentedSharedBuffer> Attachment::enclosingImageData() const
{
    if (!m_hasEnclosingImage)
        return nullptr;

    auto fileWrapper = this->fileWrapper();

    if (![fileWrapper isRegularFile])
        return nullptr;

    NSData *data = [fileWrapper regularFileContents];
    if (!data)
        return nullptr;

    return WebCore::SharedBuffer::create(data);
}

NSData *Attachment::enclosingImageNSData() const
{
    if (!m_hasEnclosingImage)
        return nil;

    auto fileWrapper = this->fileWrapper();

    if (![fileWrapper isRegularFile])
        return nil;

    return fileWrapper.regularFileContents;
}

bool Attachment::isEmpty() const
{
    return !m_fileWrapper && !m_fileWrapperGenerator;
}

RefPtr<WebCore::SharedBuffer> Attachment::createSerializedRepresentation() const
{
    auto fileWrapper = this->fileWrapper();

    if (!fileWrapper || !m_webPage)
        return nullptr;

    NSData *serializedData = [NSKeyedArchiver archivedDataWithRootObject:fileWrapper requiringSecureCoding:YES error:nullptr];
    if (!serializedData)
        return nullptr;

    return WebCore::SharedBuffer::create(serializedData);
}

void Attachment::updateFromSerializedRepresentation(Ref<WebCore::SharedBuffer>&& serializedRepresentation, const WTF::String& contentType)
{
    if (!m_webPage)
        return;

    auto serializedData = serializedRepresentation->createNSData();
    if (!serializedData)
        return;

    NSFileWrapper *fileWrapper = [NSKeyedUnarchiver unarchivedObjectOfClasses:m_webPage->pageClient().serializableFileWrapperClasses() fromData:serializedData.get() error:nullptr];
    if (![fileWrapper isKindOfClass:NSFileWrapper.class])
        return;

    setFileWrapperAndUpdateContentType(fileWrapper, contentType);
    m_webPage->updateAttachmentAttributes(*this, [] { });
}

void Attachment::setFileWrapperGenerator(Function<RetainPtr<NSFileWrapper>(void)>&& fileWrapperGenerator)
{
    m_fileWrapperGenerator = WTFMove(fileWrapperGenerator);
    m_fileWrapper = nil;
    m_webPage->didInvalidateDataForAttachment(*this);
}

} // namespace API
