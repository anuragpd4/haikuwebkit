/*
 * Copyright (C) 2009-2021 Apple Inc. All rights reserved.
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

#import "config.h"

#if ENABLE(WEBGL)
#import "GraphicsContextGLCocoa.h"

#import "ANGLEUtilities.h"
#import "ANGLEUtilitiesCocoa.h"
#import "CVUtilities.h"
#import "ExtensionsGLANGLE.h"
#import "GraphicsContextGLIOSurfaceSwapChain.h"
#import "GraphicsContextGLOpenGLManager.h"
#import "Logging.h"
#import "ProcessIdentity.h"
#import "RuntimeApplicationChecks.h"
#import <CoreGraphics/CGBitmapContext.h>
#import <Metal/Metal.h>
#import <pal/spi/cocoa/MetalSPI.h>
#import <wtf/BlockObjCExceptions.h>
#import <wtf/darwin/WeakLinking.h>
#import <wtf/text/CString.h>

#if PLATFORM(IOS_FAMILY)
#import "WebCoreThread.h"
#endif

#if ENABLE(VIDEO)
#include "GraphicsContextGLCVCocoa.h"
#include "MediaPlayerPrivate.h"
#endif

#if ENABLE(MEDIA_STREAM)
#include "ImageRotationSessionVT.h"
#include "MediaSampleAVFObjC.h"
#endif

WTF_WEAK_LINK_FORCE_IMPORT(EGL_Initialize);

namespace WebCore {

bool platformIsANGLEAvailable()
{
    // The ANGLE is weak linked in full, and the EGL_Initialize is explicitly weak linked above
    // so that we can detect the case where ANGLE is not present.
    return !!EGL_Initialize;
}

// In isCurrentContextPredictable() == true case this variable is accessed in single-threaded manner.
// In isCurrentContextPredictable() == false case this variable is accessed from multiple threads but always sequentially
// and it always contains nullptr and nullptr is always written to it.
static GraphicsContextGLANGLE* currentContext;

static bool isCurrentContextPredictable()
{
    static bool value = isInWebProcess() || isInGPUProcess();
    return value;
}

#if ASSERT_ENABLED
// Returns true if we have volatile context extension for the particular API or
// if the particular API is not used.
static bool checkVolatileContextSupportIfDeviceExists(EGLDisplay display, const char* deviceContextVolatileExtension,
    const char* deviceContextExtension, EGLint deviceContextType)
{
    const char *clientExtensions = EGL_QueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    if (clientExtensions && strstr(clientExtensions, deviceContextVolatileExtension))
        return true;
    EGLDeviceEXT device = EGL_NO_DEVICE_EXT;
    if (!EGL_QueryDisplayAttribEXT(display, EGL_DEVICE_EXT, reinterpret_cast<EGLAttrib*>(&device)))
        return true;
    if (device == EGL_NO_DEVICE_EXT)
        return true;
    const char* deviceExtensions = EGL_QueryDeviceStringEXT(device, EGL_EXTENSIONS);
    if (!deviceExtensions || !strstr(deviceExtensions, deviceContextExtension))
        return true;
    void* deviceContext = nullptr;
    if (!EGL_QueryDeviceAttribEXT(device, deviceContextType, reinterpret_cast<EGLAttrib*>(&deviceContext)))
        return true;
    return !deviceContext;
}
#endif

static bool platformSupportsMetal(bool isWebGL2)
{
    auto device = adoptNS(MTLCreateSystemDefaultDevice());

    if (device) {
#if PLATFORM(IOS_FAMILY) && !PLATFORM(IOS_FAMILY_SIMULATOR)
        // A8 devices (iPad Mini 4, iPad Air 2) cannot use WebGL2 via Metal.
        // This check can be removed once they are no longer supported.
        if (isWebGL2)
            return [device supportsFamily:MTLGPUFamilyApple3];
#else
        UNUSED_PARAM(isWebGL2);
#endif
        return true;
    }
    
    return false;
}

static EGLDisplay initializeEGLDisplay(const GraphicsContextGLAttributes& attrs)
{
    if (!platformIsANGLEAvailable()) {
        WTFLogAlways("Failed to load ANGLE shared library.");
        return EGL_NO_DISPLAY;
    }

    EGLint majorVersion = 0;
    EGLint minorVersion = 0;
    EGLDisplay display;

    Vector<EGLint> displayAttributes;

    // FIXME: This should come in from the GraphicsContextGLAttributes.
    bool shouldInitializeWithVolatileContextSupport = !isCurrentContextPredictable();
    if (shouldInitializeWithVolatileContextSupport) {
        // For WK1 type APIs we need to set "volatile platform context" for specific
        // APIs, since client code will be able to override the thread-global context
        // that ANGLE expects.
        displayAttributes.append(EGL_PLATFORM_ANGLE_DEVICE_CONTEXT_VOLATILE_EAGL_ANGLE);
        displayAttributes.append(EGL_TRUE);
        displayAttributes.append(EGL_PLATFORM_ANGLE_DEVICE_CONTEXT_VOLATILE_CGL_ANGLE);
        displayAttributes.append(EGL_TRUE);
    }

    LOG(WebGL, "Attempting to use ANGLE's %s backend.", attrs.useMetal ? "Metal" : "OpenGL");
    EGLNativeDisplayType nativeDisplay = GraphicsContextGLANGLE::defaultDisplay;
    if (attrs.useMetal) {
        displayAttributes.append(EGL_PLATFORM_ANGLE_TYPE_ANGLE);
        displayAttributes.append(EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE);
        // These properties are defined for EGL_ANGLE_power_preference as EGLContext attributes,
        // but Metal backend uses EGLDisplay attributes.
        auto powerPreference = attrs.effectivePowerPreference();
        if (powerPreference == GraphicsContextGLAttributes::PowerPreference::LowPower) {
            displayAttributes.append(EGL_POWER_PREFERENCE_ANGLE);
            displayAttributes.append(EGL_LOW_POWER_ANGLE);
        } else if (powerPreference == GraphicsContextGLAttributes::PowerPreference::HighPerformance) {
            displayAttributes.append(EGL_POWER_PREFERENCE_ANGLE);
            displayAttributes.append(EGL_HIGH_POWER_ANGLE);
        }
    } else
        nativeDisplay = GraphicsContextGLANGLE::defaultOpenGLDisplay;

    displayAttributes.append(EGL_NONE);
    display = EGL_GetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, reinterpret_cast<void*>(nativeDisplay), displayAttributes.data());

    if (EGL_Initialize(display, &majorVersion, &minorVersion) == EGL_FALSE) {
        LOG(WebGL, "EGLDisplay Initialization failed.");
        return EGL_NO_DISPLAY;
    }
    LOG(WebGL, "ANGLE initialised Major: %d Minor: %d", majorVersion, minorVersion);
    if (shouldInitializeWithVolatileContextSupport) {
        ASSERT(checkVolatileContextSupportIfDeviceExists(display, "EGL_ANGLE_platform_device_context_volatile_eagl", "EGL_ANGLE_device_eagl", EGL_EAGL_CONTEXT_ANGLE));
        ASSERT(checkVolatileContextSupportIfDeviceExists(display, "EGL_ANGLE_platform_device_context_volatile_cgl", "EGL_ANGLE_device_cgl", EGL_CGL_CONTEXT_ANGLE));
    }
    return display;
}

static const unsigned statusCheckThreshold = 5;

#if PLATFORM(MAC) || PLATFORM(MACCATALYST)
static bool needsEAGLOnMac()
{
#if PLATFORM(MACCATALYST) && CPU(ARM64)
    return true;
#else
    return false;
#endif
}
#endif

RefPtr<GraphicsContextGLCocoa> GraphicsContextGLCocoa::create(GraphicsContextGLAttributes&& attributes, ProcessIdentity&& resourceOwner)
{
    auto context = adoptRef(*new GraphicsContextGLCocoa(WTFMove(attributes), WTFMove(resourceOwner)));
    if (!context->isValid())
        return nullptr;
    return context;
}

GraphicsContextGLCocoa::GraphicsContextGLCocoa(GraphicsContextGLAttributes&& creationAttributes, ProcessIdentity&& resourceOwner)
    :
#if PLATFORM(COCOA) && HAVE(TASK_IDENTITY_TOKEN)
    GraphicsContextGLANGLE(WTFMove(creationAttributes), resourceOwner)
#else
    GraphicsContextGLANGLE(WTFMove(creationAttributes))
#endif
{
    if (!isValid())
        return;
    // FIXME: Move this to initializer list once m_resourceOwner moves to GraphicsContextGLCocoa.
    m_resourceOwner = WTFMove(resourceOwner);

#if PLATFORM(MAC)
    auto attributes = contextAttributes();
    if (!attributes.useMetal && attributes.effectivePowerPreference() == GraphicsContextGLPowerPreference::HighPerformance)
        m_switchesGPUOnDisplayReconfiguration = true;
#endif
}

GraphicsContextGLCocoa::~GraphicsContextGLCocoa() = default;

IOSurface* GraphicsContextGLCocoa::displayBuffer()
{
    return m_swapChain.displayBuffer().surface.get();
}

void GraphicsContextGLCocoa::markDisplayBufferInUse()
{
    return m_swapChain.markDisplayBufferInUse();
}

// FIXME: Below is functionality that should be moved to GraphicsContextGLCocoa to simplify the base class.

#if PLATFORM(COCOA) && HAVE(TASK_IDENTITY_TOKEN)
GraphicsContextGLANGLE::GraphicsContextGLANGLE(GraphicsContextGLAttributes attrs, const ProcessIdentity& resourceOwner)
#else
GraphicsContextGLANGLE::GraphicsContextGLANGLE(GraphicsContextGLAttributes attrs)
#endif
    : GraphicsContextGL(attrs)
{
    m_isForWebGL2 = attrs.webGLVersion == GraphicsContextGLWebGLVersion::WebGL2;
    if (attrs.useMetal && !platformSupportsMetal(m_isForWebGL2)) {
        attrs.useMetal = false;
        setContextAttributes(attrs);
    }

#if ENABLE(WEBXR)
    if (attrs.xrCompatible) {
        // FIXME: It's almost certain that any connected headset will require the high-power GPU,
        // which is the same GPU we need this context to use. However, this is not guaranteed, and
        // there is also the chance that there are multiple GPUs. Given that you can request the
        // GraphicsContextGL before initializing the WebXR session, we'll need some way to
        // migrate the context to the appropriate GPU when the code here does not work.
        LOG(WebGL, "WebXR compatible context requested. This will also trigger a request for the high-power GPU.");
        attrs.forceRequestForHighPerformanceGPU = true;
        setContextAttributes(attrs);
    }
#endif

    m_displayObj = initializeEGLDisplay(attrs);
    if (!m_displayObj)
        return;

#if PLATFORM(MAC)
    if (!attrs.useMetal) {
        // For OpenGL, EGL_ANGLE_power_preference is used. The context is initialized with the
        // default, low-power device. For high-performance contexts, we request the high-performance
        // GPU in setContextVisibility. When the request is fullfilled by the system, we get the
        // display reconfiguration callback. Upon this, we update the CGL contexts inside ANGLE.
        const char *displayExtensions = EGL_QueryString(m_displayObj, EGL_EXTENSIONS);
        bool supportsPowerPreference = strstr(displayExtensions, "EGL_ANGLE_power_preference");
        if (!supportsPowerPreference) {
            attrs.forceRequestForHighPerformanceGPU = false;
            if (attrs.powerPreference == GraphicsContextGLPowerPreference::HighPerformance) {
                attrs.powerPreference = GraphicsContextGLPowerPreference::Default;
            }
            setContextAttributes(attrs);
        }
    }
#endif

    EGLint configAttributes[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0,
        EGL_STENCIL_SIZE, 0,
        EGL_NONE
    };
    EGLint numberConfigsReturned = 0;
    EGL_ChooseConfig(m_displayObj, configAttributes, &m_configObj, 1, &numberConfigsReturned);
    if (numberConfigsReturned != 1) {
        LOG(WebGL, "EGLConfig Initialization failed.");
        return;
    }
    LOG(WebGL, "Got EGLConfig");

    EGL_BindAPI(EGL_OPENGL_ES_API);
    if (EGL_GetError() != EGL_SUCCESS) {
        LOG(WebGL, "Unable to bind to OPENGL_ES_API");
        return;
    }

    Vector<EGLint> eglContextAttributes;
    if (m_isForWebGL2) {
        eglContextAttributes.append(EGL_CONTEXT_CLIENT_VERSION);
        eglContextAttributes.append(3);
    } else {
        eglContextAttributes.append(EGL_CONTEXT_CLIENT_VERSION);
        eglContextAttributes.append(2);
        // ANGLE will upgrade the context to ES3 automatically unless this is specified.
        eglContextAttributes.append(EGL_CONTEXT_OPENGL_BACKWARDS_COMPATIBLE_ANGLE);
        eglContextAttributes.append(EGL_FALSE);
    }
    eglContextAttributes.append(EGL_CONTEXT_WEBGL_COMPATIBILITY_ANGLE);
    eglContextAttributes.append(EGL_TRUE);

    // WebGL requires that all resources are cleared at creation.
    eglContextAttributes.append(EGL_ROBUST_RESOURCE_INITIALIZATION_ANGLE);
    eglContextAttributes.append(EGL_TRUE);

    // WebGL doesn't allow client arrays.
    eglContextAttributes.append(EGL_CONTEXT_CLIENT_ARRAYS_ENABLED_ANGLE);
    eglContextAttributes.append(EGL_FALSE);
    // WebGL doesn't allow implicit creation of objects on bind.
    eglContextAttributes.append(EGL_CONTEXT_BIND_GENERATES_RESOURCE_CHROMIUM);
    eglContextAttributes.append(EGL_FALSE);

#if PLATFORM(COCOA) && HAVE(TASK_IDENTITY_TOKEN)
    auto displayExtensions = EGL_QueryString(m_displayObj, EGL_EXTENSIONS);
    bool supportsOwnershipIdentity = strstr(displayExtensions, "EGL_ANGLE_metal_create_context_ownership_identity");
    // FIXME: Use m_resourceOwner once it moves to GraphicsContextGLCocoa.
    if (attrs.useMetal && resourceOwner && supportsOwnershipIdentity) {
        eglContextAttributes.append(EGL_CONTEXT_METAL_OWNERSHIP_IDENTITY_ANGLE);
        eglContextAttributes.append(resourceOwner.taskIdToken());
    }
#endif

    eglContextAttributes.append(EGL_NONE);

    m_contextObj = EGL_CreateContext(m_displayObj, m_configObj, EGL_NO_CONTEXT, eglContextAttributes.data());
    if (m_contextObj == EGL_NO_CONTEXT || !makeCurrent(m_displayObj, m_contextObj)) {
        LOG(WebGL, "EGLContext Initialization failed.");
        return;
    }
    LOG(WebGL, "Got EGLContext");

    if (m_isForWebGL2)
        GL_Enable(GraphicsContextGL::PRIMITIVE_RESTART_FIXED_INDEX);

    Vector<ASCIILiteral, 4> requiredExtensions;
    if (m_isForWebGL2) {
        // For WebGL 2.0 occlusion queries to work.
        requiredExtensions.append("GL_EXT_occlusion_query_boolean"_s);
    }
#if PLATFORM(MAC) || PLATFORM(MACCATALYST)
    if (!needsEAGLOnMac()) {
        // For IOSurface-backed textures.
        if (!attrs.useMetal)
            requiredExtensions.append("GL_ANGLE_texture_rectangle"_s);
        // For creating the EGL surface from an IOSurface.
        requiredExtensions.append("GL_EXT_texture_format_BGRA8888"_s);
    }
#endif // PLATFORM(MAC) || PLATFORM(MACCATALYST)
#if ENABLE(WEBXR) && !PLATFORM(IOS_FAMILY_SIMULATOR)
    if (contextAttributes().xrCompatible)
        requiredExtensions.append("GL_OES_EGL_image"_s);
#endif
    if (m_isForWebGL2)
        requiredExtensions.append("GL_ANGLE_framebuffer_multisample"_s);
    ExtensionsGL& extensions = getExtensions();
    for (auto& extension : requiredExtensions) {
        if (!extensions.supports(extension)) {
            LOG(WebGL, "Missing required extension. %s", extension.characters());
            return;
        }
        extensions.ensureEnabled(extension);
    }
    if (contextAttributes().useMetal) {
        // GraphicsContextGLANGLE uses sync objects to throttle display on Metal implementations.
        // OpenGL sync objects are not signaling upon completion on Catalina-era drivers, so
        // OpenGL cannot use this method of throttling. OpenGL drivers typically implement
        // some sort of internal throttling.
        if (extensions.supports("GL_ARB_sync"_s)) {
            m_useFenceSyncForDisplayRateLimit = true;
            extensions.ensureEnabled("GL_ARB_sync"_s);
        }
    }
    validateAttributes();
    attrs = contextAttributes(); // They may have changed during validation.

    // Create the texture that will be used for the framebuffer.
    GLenum textureTarget = drawingBufferTextureTarget();

    GL_GenTextures(1, &m_texture);
    GL_BindTexture(textureTarget, m_texture);
    GL_TexParameteri(textureTarget, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    GL_TexParameteri(textureTarget, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    GL_TexParameteri(textureTarget, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    GL_TexParameteri(textureTarget, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GL_BindTexture(textureTarget, 0);

    GL_GenFramebuffers(1, &m_fbo);
    GL_BindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    m_state.boundDrawFBO = m_state.boundReadFBO = m_fbo;

    if (!attrs.antialias && (attrs.stencil || attrs.depth))
        GL_GenRenderbuffers(1, &m_depthStencilBuffer);

    // If necessary, create another framebuffer for the multisample results.
    if (attrs.antialias) {
        GL_GenFramebuffers(1, &m_multisampleFBO);
        GL_BindFramebuffer(GL_FRAMEBUFFER, m_multisampleFBO);
        m_state.boundDrawFBO = m_state.boundReadFBO = m_multisampleFBO;
        GL_GenRenderbuffers(1, &m_multisampleColorBuffer);
        if (attrs.stencil || attrs.depth)
            GL_GenRenderbuffers(1, &m_multisampleDepthStencilBuffer);
    } else if (attrs.preserveDrawingBuffer) {
        // If necessary, create another texture to handle preserveDrawingBuffer:true without
        // antialiasing.
        GL_GenTextures(1, &m_preserveDrawingBufferTexture);
        GL_BindTexture(GL_TEXTURE_2D, m_preserveDrawingBufferTexture);
        GL_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        GL_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        GL_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        GL_TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        GL_BindTexture(GL_TEXTURE_2D, 0);
        // Create an FBO with which to perform BlitFramebuffer from one texture to the other.
        GL_GenFramebuffers(1, &m_preserveDrawingBufferFBO);
    }

    GL_ClearColor(0, 0, 0, 0);

    LOG(WebGL, "Created a GraphicsContextGLANGLE (%p).", this);
}

GraphicsContextGLANGLE::~GraphicsContextGLANGLE()
{
    GraphicsContextGLOpenGLManager::sharedManager().removeContext(this);
    if (makeContextCurrent()) {
        if (m_texture)
            GL_DeleteTextures(1, &m_texture);
        if (m_multisampleColorBuffer)
            GL_DeleteRenderbuffers(1, &m_multisampleColorBuffer);
        if (m_multisampleDepthStencilBuffer)
            GL_DeleteRenderbuffers(1, &m_multisampleDepthStencilBuffer);
        if (m_multisampleFBO)
            GL_DeleteFramebuffers(1, &m_multisampleFBO);
        if (m_depthStencilBuffer)
            GL_DeleteRenderbuffers(1, &m_depthStencilBuffer);
        if (m_fbo)
            GL_DeleteFramebuffers(1, &m_fbo);
        if (m_preserveDrawingBufferTexture)
            GL_DeleteTextures(1, &m_preserveDrawingBufferTexture);
        if (m_preserveDrawingBufferFBO)
            GL_DeleteFramebuffers(1, &m_preserveDrawingBufferFBO);
        // If fences are not enabled, this loop will not execute.
        for (auto& fence : m_frameCompletionFences)
            fence.reset();
    } else {
        for (auto& fence : m_frameCompletionFences)
            fence.abandon();
    }
    if (m_displayBufferPbuffer)
        EGL_DestroySurface(m_displayObj, m_displayBufferPbuffer);
    auto recycledBuffer = m_swapChain.recycleBuffer();
    if (recycledBuffer.handle)
        EGL_DestroySurface(m_displayObj, recycledBuffer.handle);
    auto contentsHandle = m_swapChain.detachClient();
    if (contentsHandle)
        EGL_DestroySurface(m_displayObj, contentsHandle);
    if (m_contextObj) {
        makeCurrent(m_displayObj, EGL_NO_CONTEXT);
        EGL_DestroyContext(m_displayObj, m_contextObj);
    }
    ASSERT(currentContext != this);
    m_drawingBufferTextureTarget = -1;
    LOG(WebGL, "Destroyed a GraphicsContextGLANGLE (%p).", this);
}

bool GraphicsContextGLCocoa::isValid() const
{
    return m_texture;
}

bool GraphicsContextGLANGLE::makeContextCurrent()
{
    if (!m_contextObj)
        return false;
    // If there is no drawing buffer, we failed to allocate one during preparing for display.
    // The exception is the case when the context is used before reshaping.
    if (!m_displayBufferBacking && !getInternalFramebufferSize().isEmpty())
        return false;
    if (currentContext == this)
        return true;
    // Calling MakeCurrent is important to set volatile platform context. See initializeEGLDisplay().
    if (!EGL_MakeCurrent(m_displayObj, EGL_NO_SURFACE, EGL_NO_SURFACE, m_contextObj))
        return false;
    if (isCurrentContextPredictable())
        currentContext = this;
    return true;
}

void GraphicsContextGLANGLE::checkGPUStatus()
{
    if (m_failNextStatusCheck) {
        LOG(WebGL, "Pretending the GPU has reset (%p). Lose the context.", this);
        m_failNextStatusCheck = false;
        forceContextLost();
        makeCurrent(m_displayObj, EGL_NO_CONTEXT);
        return;
    }

    // Only do the check every statusCheckThreshold calls.
    if (m_statusCheckCount)
        return;

    m_statusCheckCount = (m_statusCheckCount + 1) % statusCheckThreshold;

    // FIXME: check via KHR_robustness.
}

void GraphicsContextGLCocoa::setContextVisibility(bool isVisible)
{
#if PLATFORM(MAC)
    if (!m_switchesGPUOnDisplayReconfiguration)
        return;
    if (isVisible)
        m_highPerformanceGPURequest = ScopedHighPerformanceGPURequest::acquire();
    else
        m_highPerformanceGPURequest = { };
#else
    UNUSED_PARAM(isVisible);
#endif
}

#if PLATFORM(MAC)
void GraphicsContextGLCocoa::updateContextOnDisplayReconfiguration()
{
    if (m_switchesGPUOnDisplayReconfiguration)
        EGL_HandleGPUSwitchANGLE(m_displayObj);
    dispatchContextChangedNotification();
}
#endif

bool GraphicsContextGLANGLE::reshapeDisplayBufferBacking()
{
    ASSERT(!getInternalFramebufferSize().isEmpty());
    // Reset the current backbuffer now before allocating a new one in order to slightly reduce memory pressure.
    if (m_displayBufferBacking) {
        m_displayBufferBacking.reset();
        EGL_ReleaseTexImage(m_displayObj, m_displayBufferPbuffer, EGL_BACK_BUFFER);
        EGL_DestroySurface(m_displayObj, m_displayBufferPbuffer);
        m_displayBufferPbuffer = EGL_NO_SURFACE;
    }
    // Reset the future recycled buffer now, because it most likely will not be reusable at the time it will be reused.
    auto recycledBuffer = m_swapChain.recycleBuffer();
    if (recycledBuffer.handle)
        EGL_DestroySurface(m_displayObj, recycledBuffer.handle);
    recycledBuffer.surface.reset();
    return allocateAndBindDisplayBufferBacking();
}

bool GraphicsContextGLANGLE::allocateAndBindDisplayBufferBacking()
{
    ASSERT(!getInternalFramebufferSize().isEmpty());
    auto backing = IOSurface::create(getInternalFramebufferSize(), DestinationColorSpace::SRGB());
    if (!backing)
        return false;
    if (m_resourceOwner)
        backing->setOwnershipIdentity(m_resourceOwner);
    backing->migrateColorSpaceToProperties();

    const bool usingAlpha = contextAttributes().alpha;
    const auto size = getInternalFramebufferSize();
    const EGLint surfaceAttributes[] = {
        EGL_WIDTH, size.width(),
        EGL_HEIGHT, size.height(),
        EGL_IOSURFACE_PLANE_ANGLE, 0,
        EGL_TEXTURE_TARGET, EGLDrawingBufferTextureTargetForDrawingTarget(drawingBufferTextureTarget()),
        EGL_TEXTURE_INTERNAL_FORMAT_ANGLE, usingAlpha ? GL_BGRA_EXT : GL_RGB,
        EGL_TEXTURE_FORMAT, EGL_TEXTURE_RGBA,
        EGL_TEXTURE_TYPE_ANGLE, GL_UNSIGNED_BYTE,
        // Only has an effect on the iOS Simulator.
        EGL_IOSURFACE_USAGE_HINT_ANGLE, EGL_IOSURFACE_WRITE_HINT_ANGLE,
        EGL_NONE, EGL_NONE
    };
    EGLSurface pbuffer = EGL_CreatePbufferFromClientBuffer(m_displayObj, EGL_IOSURFACE_ANGLE, backing->surface(), m_configObj, surfaceAttributes);
    if (!pbuffer)
        return false;
    return bindDisplayBufferBacking(WTFMove(backing), pbuffer);
}

bool GraphicsContextGLANGLE::bindDisplayBufferBacking(std::unique_ptr<IOSurface> backing, void* pbuffer)
{
    GCGLenum textureTarget = drawingBufferTextureTarget();
    ScopedRestoreTextureBinding restoreBinding(drawingBufferTextureTargetQueryForDrawingTarget(textureTarget), textureTarget, textureTarget != TEXTURE_RECTANGLE_ARB);
    GL_BindTexture(textureTarget, m_texture);
    if (!EGL_BindTexImage(m_displayObj, pbuffer, EGL_BACK_BUFFER)) {
        EGL_DestroySurface(m_displayObj, pbuffer);
        return false;
    }
    m_displayBufferPbuffer = pbuffer;
    m_displayBufferBacking = WTFMove(backing);
    return true;
}

bool GraphicsContextGLANGLE::makeCurrent(PlatformGraphicsContextGLDisplay display, PlatformGraphicsContextGL context)
{
    currentContext = nullptr;
    return EGL_MakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context);
}

void* GraphicsContextGLCocoa::createPbufferAndAttachIOSurface(GCGLenum target, PbufferAttachmentUsage usage, GCGLenum internalFormat, GCGLsizei width, GCGLsizei height, GCGLenum type, IOSurfaceRef surface, GCGLuint plane)
{
    if (target != GraphicsContextGLANGLE::drawingBufferTextureTarget()) {
        LOG(WebGL, "Unknown texture target %d.", static_cast<int>(target));
        return nullptr;
    }

    auto usageHint = [&] () -> EGLint {
        if (usage == PbufferAttachmentUsage::Read)
            return EGL_IOSURFACE_READ_HINT_ANGLE;
        if (usage == PbufferAttachmentUsage::Write)
            return EGL_IOSURFACE_WRITE_HINT_ANGLE;
        return EGL_IOSURFACE_READ_HINT_ANGLE | EGL_IOSURFACE_WRITE_HINT_ANGLE;
    }();

    return WebCore::createPbufferAndAttachIOSurface(m_displayObj, m_configObj, target, usageHint, internalFormat, width, height, type, surface, plane);
}

void GraphicsContextGLCocoa::destroyPbufferAndDetachIOSurface(void* handle)
{
    WebCore::destroyPbufferAndDetachIOSurface(m_displayObj, handle);
}

#if !PLATFORM(IOS_FAMILY_SIMULATOR)
void* GraphicsContextGLCocoa::attachIOSurfaceToSharedTexture(GCGLenum target, IOSurface* surface)
{
    constexpr EGLint emptyAttributes[] = { EGL_NONE };

    // Create a MTLTexture out of the IOSurface.
    // FIXME: We need to use the same device that ANGLE is using, which might not be the default.

    RetainPtr<MTLSharedTextureHandle> handle = adoptNS([[MTLSharedTextureHandle alloc] initWithIOSurface:surface->surface() label:@"WebXR"]);
    if (!handle) {
        LOG(WebGL, "Unable to create a MTLSharedTextureHandle from the IOSurface in attachIOSurfaceToTexture.");
        return nullptr;
    }

    if (!handle.get().device) {
        LOG(WebGL, "MTLSharedTextureHandle does not have a Metal device in attachIOSurfaceToTexture.");
        return nullptr;
    }

    auto texture = adoptNS([handle.get().device newSharedTextureWithHandle:handle.get()]);
    if (!texture) {
        LOG(WebGL, "Unable to create a MTLSharedTexture from the texture handle in attachIOSurfaceToTexture.");
        return nullptr;
    }

    // FIXME: Does the texture have the correct usage mode?

    // Create an EGLImage out of the MTLTexture
    auto display = platformDisplay();
    auto eglImage = EGL_CreateImageKHR(display, EGL_NO_CONTEXT, EGL_METAL_TEXTURE_ANGLE, reinterpret_cast<EGLClientBuffer>(texture.get()), emptyAttributes);
    if (!eglImage) {
        LOG(WebGL, "Unable to create an EGLImage from the Metal handle in attachIOSurfaceToTexture.");
        return nullptr;
    }

    // Tell the currently bound texture to use the EGLImage.
    GL_EGLImageTargetTexture2DOES(target, eglImage);

    return eglImage;
}

void GraphicsContextGLCocoa::detachIOSurfaceFromSharedTexture(void* handle)
{
    auto display = platformDisplay();
    EGL_DestroyImageKHR(display, handle);
}
#endif

void GraphicsContextGLANGLE::prepareForDisplay()
{
    if (m_layerComposited)
        return;
    if (!makeContextCurrent())
        return;
    prepareTextureImpl();

    // The IOSurface will be used from other graphics subsystem, so flush GL commands.
    GL_Flush();

    auto recycledBuffer = m_swapChain.recycleBuffer();

    EGL_ReleaseTexImage(m_displayObj, m_displayBufferPbuffer, EGL_BACK_BUFFER);
    m_swapChain.present({ WTFMove(m_displayBufferBacking), m_displayBufferPbuffer });
    m_displayBufferPbuffer = EGL_NO_SURFACE;

    bool hasNewBacking = false;
    if (recycledBuffer.surface && recycledBuffer.surface->size() == getInternalFramebufferSize()) {
        hasNewBacking = bindDisplayBufferBacking(WTFMove(recycledBuffer.surface), recycledBuffer.handle);
        recycledBuffer.handle = nullptr;
    }
    recycledBuffer.surface.reset();
    if (recycledBuffer.handle)
        EGL_DestroySurface(m_displayObj, recycledBuffer.handle);

    // Error will be handled by next call to makeContextCurrent() which will notice lack of display buffer.
    if (!hasNewBacking)
        allocateAndBindDisplayBufferBacking();

    markLayerComposited();

    if (m_useFenceSyncForDisplayRateLimit) {
        bool success = waitAndUpdateOldestFrame();
        UNUSED_VARIABLE(success); // FIXME: implement context lost.
    }
}


#if ENABLE(VIDEO)
GraphicsContextGLCV* GraphicsContextGLCocoa::asCV()
{
    if (!m_cv)
        m_cv = GraphicsContextGLCVCocoa::create(*this);
    return m_cv.get();
}
#endif

std::optional<PixelBuffer> GraphicsContextGLANGLE::readCompositedResults()
{
    auto& displayBuffer = m_swapChain.displayBuffer();
    if (!displayBuffer.surface || !displayBuffer.handle)
        return std::nullopt;
    if (displayBuffer.surface->size() != getInternalFramebufferSize())
        return std::nullopt;
    // Note: We are using GL to read the IOSurface. At the time of writing, there are no convinient
    // functions to convert the IOSurface pixel data to ImageData. The image data ends up being
    // drawn to a ImageBuffer, but at the time there's no functions to construct a NativeImage
    // out of an IOSurface in such a way that drawing the NativeImage would be guaranteed leave
    // the IOSurface be unrefenced after the draw call finishes.
    ScopedTexture texture;
    GCGLenum textureTarget = drawingBufferTextureTarget();
    ScopedRestoreTextureBinding restoreBinding(drawingBufferTextureTargetQueryForDrawingTarget(drawingBufferTextureTarget()), textureTarget, textureTarget != TEXTURE_RECTANGLE_ARB);
    GL_BindTexture(textureTarget, texture);
    if (!EGL_BindTexImage(m_displayObj, displayBuffer.handle, EGL_BACK_BUFFER))
        return std::nullopt;
    GL_TexParameteri(textureTarget, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    ScopedFramebuffer fbo;
    ScopedRestoreReadFramebufferBinding fboBinding(m_isForWebGL2, m_state.boundReadFBO, fbo);
    GL_FramebufferTexture2D(fboBinding.framebufferTarget(), GL_COLOR_ATTACHMENT0, textureTarget, texture, 0);
    ASSERT(GL_CheckFramebufferStatus(fboBinding.framebufferTarget()) == GL_FRAMEBUFFER_COMPLETE);

    auto result = readPixelsForPaintResults();

    EGLBoolean releaseOk = EGL_ReleaseTexImage(m_displayObj, displayBuffer.handle, EGL_BACK_BUFFER);
    ASSERT_UNUSED(releaseOk, releaseOk);
    return result;
}

#if ENABLE(MEDIA_STREAM)
RefPtr<MediaSample> GraphicsContextGLCocoa::paintCompositedResultsToMediaSample()
{
    auto &displayBuffer = m_swapChain.displayBuffer();
    if (!displayBuffer.surface || !displayBuffer.handle)
        return nullptr;
    if (displayBuffer.surface->size() != getInternalFramebufferSize())
        return nullptr;
    // Display surface is not marked in use since we will mirror and rotate it explicitly.
    auto pixelBuffer = createCVPixelBuffer(displayBuffer.surface->surface());
    if (!pixelBuffer)
        return nullptr;
    // Mirror and rotate the pixel buffer explicitly, as WebRTC encoders cannot mirror.
    if (!m_mediaSampleRotationSession)
        m_mediaSampleRotationSession = makeUnique<ImageRotationSessionVT>(ImageRotationSessionVT::RotationProperties { true, false, 180 }, getInternalFramebufferSize(), ImageRotationSessionVT::IsCGImageCompatible::No);
    auto mediaSamplePixelBuffer = m_mediaSampleRotationSession->rotate(pixelBuffer->get());
    if (!mediaSamplePixelBuffer)
        return nullptr;
    if (m_resourceOwner)
        setOwnershipIdentityForCVPixelBuffer(mediaSamplePixelBuffer.get(), m_resourceOwner);
    return MediaSampleAVFObjC::createImageSample(WTFMove(mediaSamplePixelBuffer), MediaSampleAVFObjC::VideoRotation::None, false);
}
#endif

void GraphicsContextGLANGLE::platformReleaseThreadResources()
{
    currentContext = nullptr;
}

#if ENABLE(VIDEO)
bool GraphicsContextGLCocoa::copyTextureFromMedia(MediaPlayer& player, PlatformGLObject outputTexture, GCGLenum outputTarget, GCGLint level, GCGLenum internalFormat, GCGLenum format, GCGLenum type, bool premultiplyAlpha, bool flipY)
{
    auto videoFrame = player.videoFrameForCurrentTime();
    if (!videoFrame)
        return false;

    auto contextCV = asCV();
    if (!contextCV)
        return false;

    UNUSED_VARIABLE(premultiplyAlpha);
    ASSERT_UNUSED(outputTarget, outputTarget == GraphicsContextGL::TEXTURE_2D);
    return contextCV->copyVideoSampleToTexture(*videoFrame, outputTexture, level, internalFormat, format, type, GraphicsContextGL::FlipY(flipY));
}
#endif

PlatformGraphicsContextGLDisplay GraphicsContextGLANGLE::platformDisplay() const
{
    return m_displayObj;
}

PlatformGraphicsContextGLConfig GraphicsContextGLANGLE::platformConfig() const
{
    return m_configObj;
}

RefPtr<GraphicsLayerContentsDisplayDelegate> GraphicsContextGLCocoa::layerContentsDisplayDelegate()
{
    return nullptr;
}

void GraphicsContextGLCocoa::invalidateKnownTextureContent(GCGLuint texture)
{
    if (m_cv)
        m_cv->invalidateKnownTextureContent(texture);
}

}

#endif // ENABLE(WEBGL)
