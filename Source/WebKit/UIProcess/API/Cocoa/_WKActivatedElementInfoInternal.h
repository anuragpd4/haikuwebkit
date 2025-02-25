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

#if PLATFORM(IOS_FAMILY)
#import "InteractionInformationAtPosition.h"
#endif
#import <WebKit/_WKActivatedElementInfo.h>

#import <WebCore/IntPoint.h>

namespace WebKit {
    class ShareableBitmap;
}

@interface _WKActivatedElementInfo ()

#if PLATFORM(IOS_FAMILY)
+ (instancetype)activatedElementInfoWithInteractionInformationAtPosition:(const WebKit::InteractionInformationAtPosition&)information userInfo:(NSDictionary *)userInfo;
- (instancetype)_initWithInteractionInformationAtPosition:(const WebKit::InteractionInformationAtPosition&)information userInfo:(NSDictionary *)userInfo;
#endif
- (instancetype)_initWithType:(_WKActivatedElementType)type URL:(NSURL *)url imageURL:(NSURL *)imageURL location:(const WebCore::IntPoint&)location title:(NSString *)title ID:(NSString *)ID rect:(CGRect)rect image:(WebKit::ShareableBitmap*)image imageMIMEType:(NSString *)imageMIMEType;
- (instancetype)_initWithType:(_WKActivatedElementType)type URL:(NSURL *)url imageURL:(NSURL *)imageURL location:(const WebCore::IntPoint&)location title:(NSString *)title ID:(NSString *)ID rect:(CGRect)rect image:(WebKit::ShareableBitmap*)image imageMIMEType:(NSString *)imageMIMEType userInfo:(NSDictionary *)userInfo;

@property (nonatomic, readonly) NSString *imageMIMEType;
@property (nonatomic, readonly) WebCore::IntPoint _interactionLocation;

@end
