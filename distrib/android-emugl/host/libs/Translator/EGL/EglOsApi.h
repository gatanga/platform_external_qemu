/*
* Copyright (C) 2011 The Android Open Source Project
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/
#ifndef EGL_OS_API_H
#define EGL_OS_API_H

#include <EGL/egl.h>

#include <list>

#define PBUFFER_MAX_WIDTH  32767
#define PBUFFER_MAX_HEIGHT 32767
#define PBUFFER_MAX_PIXELS 32767*32767

class EglConfig;

namespace EglOS {

// Base class used to wrap various GL Surface types.
class Surface {
public:
    typedef enum {
        WINDOW = 0,
        PBUFFER = 1,
        PIXMAP,
    } SurfaceType;

    explicit Surface(SurfaceType type) : mType(type) {}

    virtual ~Surface() {}

    SurfaceType type() const { return mType; }

protected:
    SurfaceType mType;
};

// An interface class for engine-specific implementation of a GL context.
class Context {
public:
    Context() {}
    virtual ~Context() {}
};

// Base class used to wrap engine-specific pixel format descriptors.
class PixelFormat {
public:
    PixelFormat() {}

    virtual ~PixelFormat() {}

    virtual PixelFormat* clone() = 0;
};

// Pbuffer description.
// |width| and |height| are its dimensions.
// |largest| is set to ask the largest pixek buffer (see GLX_LARGEST_PBUFFER).
// |format| is one of EGL_TEXTURE_RGB or EGL_TEXTURE_RGBA
// |target| is one of EGL_TEXTURE_2D or EGL_NO_TEXTURE.
// |hasMipmap| is true if the Pbuffer has mipmaps.
struct PbufferInfo {
    EGLint width;
    EGLint height;
    EGLint largest;
    EGLint format;
    EGLint target;
    EGLint hasMipmap;
};

// A class to model the engine-specific implementation of a GL display
// connection.
class Display {
public:
    Display() {}
    virtual ~Display() {}

    virtual bool release() = 0;

    virtual void queryConfigs(int renderableType,
                              std::list<EglConfig*>& listOut) = 0;

    virtual bool isValidNativeWin(Surface* win) = 0;
    virtual bool isValidNativeWin(EGLNativeWindowType win) = 0;
    virtual bool isValidNativePixmap(Surface* pix) = 0;

    virtual bool checkWindowPixelFormatMatch(EGLNativeWindowType win,
                                             const EglConfig* config,
                                             unsigned int* width,
                                             unsigned int* height) = 0;

    virtual bool checkPixmapPixelFormatMatch(EGLNativePixmapType pix,
                                             const EglConfig* config,
                                             unsigned int* width,
                                             unsigned int* height) = 0;

    virtual Context* createContext(
            const EglConfig* config, Context* sharedContext) = 0;

    virtual bool destroyContext(Context* context) = 0;

    virtual Surface* createPbufferSurface(
            const EglConfig* config, const PbufferInfo* info) = 0;

    virtual bool releasePbuffer(Surface* pb) = 0;

    virtual bool makeCurrent(Surface* read,
                             Surface* draw,
                             Context* context) = 0;

    virtual void swapBuffers(Surface* srfc) = 0;

    virtual void swapInterval(Surface* win, int interval) = 0;
};

// An interface class to model a specific underlying GL graphics subsystem
// or engine. Use getHost() to retrieve the implementation for the current
// host.
class Engine {
public:
    Engine() {}
    virtual ~Engine() {}

    // Return a Display instance to the default display / window.
    virtual Display* getDefaultDisplay() = 0;

    // Convert a platform-specific display type (e.g. a Windows HWND) into
    // the corresponding Display instance. This will return NULL for engines
    // that are not tied to the host platform (e.g. software renderers like
    // OSMesa).
    virtual Display* getInternalDisplay(EGLNativeDisplayType dpy) = 0;

    // Create a new window surface. |wnd| is a host-specific window handle
    // (e.g. a Windows HWND). A software renderer would always return NULL
    // here.
    virtual Surface* createWindowSurface(EGLNativeWindowType wnd) = 0;

    // Create a new pixmap surface. |pix| is a host-specific pixmap handle
    // (e.g. a Windows HBITMAP). A software renderer would always return NULL.
    virtual Surface* createPixmapSurface(EGLNativePixmapType pix) = 0;

    // Wait for host graphics command completion. This is only useful on X11
    // to gall glXwaitX(), ignored on other platforms or by software
    // engines.
    virtual void wait() = 0;

    // Retrieve the implementation for the current host. This can be called
    // multiple times, and will initialize the engine on first call.
    static Engine* getHostInstance();
};

}  // namespace EglOS

#endif
