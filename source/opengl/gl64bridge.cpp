/*
 *  Copyright (C) 2012-2025  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

// Host side of the 64-bit OpenGL bridge — see gl64bridge.h / gl64bridge_abi.h.
//
// Design (first light): the existing main window is owned by an SDL_Renderer
// (xwirepresentSDL), and SDL2 does not allow a hand-rolled GL context to share
// a window with a renderer. So instead of rendering ONTO the main window, we
// render OFFSCREEN into a hidden GL window's default framebuffer, then on
// glXSwapBuffers we glReadPixels the result, convert RGBA->BGRX and push it
// through the existing X11-wire presentation sink (submitFrame). That reuses the
// proven present pipeline and keeps all native-GL state on the guest's GL
// thread.

#include "boxedwine.h"

#ifdef BOXEDWINE_OPENGL

#include "gl64bridge.h"
#include "gl64bridge_abi.h"
#include "cpu64.h"
#include "kmemory64.h"
#include "../x11wire/xwirepresent.h"

#include <SDL.h>
#ifdef __EMSCRIPTEN__
// WASM backend: the host GL context is WebGL2 (GLES3) with NO fixed-function
// pipeline of its own — Emscripten's -sLEGACY_GL_EMULATION glemu layer
// synthesizes shaders for the FFP calls (glBegin/glEnd, matrix stack, lighting,
// glColor/glVertex) and exposes them as real linkable symbols through <GL/gl.h>.
// We render OFFSCREEN into an app-created FBO and glReadPixels it back through
// the X11-wire present sink, so we never composite to the page canvas directly.
//
// WebGL contexts are thread-affine (emsdk libwebgl.js stamps the creating
// thread; make-current fails cross-thread), and this build has no OffscreenCanvas
// / OFFSCREEN_FRAMEBUFFER escape hatch — so a guest worker thread cannot hold a
// usable context. The gl64 trap runs on a guest thread, therefore every GL call
// is marshaled to the platform MAIN thread (which owns GL and runs the present
// loop) via xwireRunOnMainThread. Guest-memory reads/writes stay on the calling
// thread; only the gl* calls hop.
#include <GL/gl.h>
#include <emscripten/html5.h>
#include <emscripten/html5_webgl.h>
#else
#include <SDL_opengl.h>
#endif
#include <vector>
#include <thread>
#include <atomic>
#include <cstring>
#include <mutex>

// ---------------------------------------------------------------------------
// Host GL context + offscreen target. One per process for first light (wine
// renders from a single GL thread). All access is from the guest GL thread
// inside the syscall handler, serialized by g_glMutex so a stray second guest
// thread can't corrupt the lazily-built state.
// ---------------------------------------------------------------------------
namespace {
std::recursive_mutex g_glMutex;

#ifdef __EMSCRIPTEN__
// On WASM the "context" is a WebGL2 handle owned by the main thread, and the
// drawable is an app FBO we read back. g_glContext doubles as the "GL is up"
// flag (non-null == ready) so the rest of the file's `if (g_glContext)` guards
// keep working unchanged.
EMSCRIPTEN_WEBGL_CONTEXT_HANDLE g_emCtx = 0;     // WebGL2 context (main thread)
GLuint        g_emFbo     = 0;            // offscreen render target
GLuint        g_emColorTex = 0;
GLuint        g_emDepthRb  = 0;
void*         g_glContext = nullptr;      // sentinel only; real ctx is g_emCtx
#else
SDL_Window*   g_hiddenWindow = nullptr;   // hidden, GL-capable
SDL_GLContext g_glContext    = nullptr;
#endif
bool          g_glInitFailed = false;

#ifdef __EMSCRIPTEN__
// Run a closure on the platform MAIN thread (which owns the WebGL context),
// blocking until it completes. The gl64 trap runs on a guest worker thread that
// cannot touch WebGL, so all GL work hops here. xwireRunOnMainThread runs `fn`
// inline if already on the main thread, else enqueues + waits for the main
// loop's drainMainThreadWork(). Before each op we re-make the gl64 context
// current — the SDL_Renderer present (tickMainThread) leaves its own context
// current, so we must reclaim ours every time.
template <typename Fn>
inline void glOnMain(Fn&& fn) {
    xwireRunOnMainThread([&]{
        if (g_emCtx) emscripten_webgl_make_context_current(g_emCtx);
        fn();
    });
}
// Run a GL statement on the main thread. Used to wrap the inline gl* calls
// scattered through the switch so they execute on the context-owning thread.
#define GL_MT(stmt) glOnMain([&]{ stmt; })
#else
// Native: the existing code is already on a GL-capable thread (re-make-current
// handled in ensureContext); run inline.
template <typename Fn>
inline void glOnMain(Fn&& fn) { fn(); }
#define GL_MT(stmt) do { stmt; } while (0)
#endif

// Current drawable as the guest sees it (the X window id winex11 passed to
// glXMakeCurrent). We present readback frames against this id.
U32 g_currentDrawable = 0;
int g_drawW = 640, g_drawH = 480;

// Monotonic opaque ids handed back to the guest for GLXContext/XVisualInfo/
// GLXFBConfig. The guest treats them as opaque; the host never dereferences
// them (single context for first light).
U64 g_nextOpaqueId = 0x5000;

// Readback scratch buffers.
std::vector<U8> g_rgba;   // glReadPixels output (RGBA, bottom-up)
std::vector<U8> g_bgrx;   // converted, top-down, for submitFrame

#ifndef __EMSCRIPTEN__
// Track which host thread currently holds the GL context. An SDL/Apple GL
// context is current PER THREAD; the guest may issue GL calls from a different
// host thread than the one glXMakeCurrent ran on (KThread64 = one host thread
// per guest thread). Calling glViewport/glClear with no current context hangs or
// faults in the Apple Metal-GL layer. So re-make-current whenever the calling
// thread changes. (WASM uses a single main-thread context — see glOnMain.)
static std::atomic<std::thread::id> g_glCurrentThread{};
#endif

#ifdef __EMSCRIPTEN__
// (Re)create the offscreen FBO color/depth attachments at the current drawable
// size. MUST run on the GL-owning main thread (caller wraps it).
void emBuildFbo(int w, int h) {
    if (!g_emFbo)     glGenFramebuffers(1, &g_emFbo);
    if (!g_emColorTex) glGenTextures(1, &g_emColorTex);
    if (!g_emDepthRb)  glGenRenderbuffers(1, &g_emDepthRb);

    glBindTexture(GL_TEXTURE_2D, g_emColorTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glBindRenderbuffer(GL_RENDERBUFFER, g_emDepthRb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h);

    glBindFramebuffer(GL_FRAMEBUFFER, g_emFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_emColorTex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_emDepthRb);
}
#endif

// Ensure the hidden GL context exists and is current ON THE CALLING THREAD.
// Returns false if GL is unavailable (then the bridge degrades to no-ops so the
// guest keeps running).
bool ensureContext() {
#ifdef __EMSCRIPTEN__
    if (g_emCtx) return true;
    if (g_glInitFailed) return false;
    // Create the WebGL2 context + offscreen FBO on the platform main thread; the
    // context is thread-affine and must live where present() runs. glemu's FFP
    // shaders need WebGL2, so request majorVersion 2.
    glOnMain([&]{
        EmscriptenWebGLContextAttributes attr;
        emscripten_webgl_init_context_attributes(&attr);
        attr.majorVersion = 2;
        attr.minorVersion = 0;
        attr.alpha = true;
        attr.depth = true;
        attr.stencil = false;
        attr.antialias = false;
        attr.enableExtensionsByDefault = true;
        // Render into our own FBO, so the target canvas selector is irrelevant;
        // "#canvas" is the page canvas the SDL renderer also uses, but we never
        // present to it from this context — we glReadPixels and hand off.
        g_emCtx = emscripten_webgl_create_context("#canvas", &attr);
        if (!g_emCtx) {
            klog_fmt("gl64: emscripten_webgl_create_context failed");
            return;
        }
        emscripten_webgl_make_context_current(g_emCtx);
        emBuildFbo(g_drawW, g_drawH);
        klog_fmt("gl64: host GL up (WebGL2) — vendor='%s' renderer='%s' version='%s'",
                 (const char*)glGetString(GL_VENDOR),
                 (const char*)glGetString(GL_RENDERER),
                 (const char*)glGetString(GL_VERSION));
    });
    if (!g_emCtx) { g_glInitFailed = true; return false; }
    g_glContext = (void*)1; // sentinel: the rest of the file checks g_glContext
    return true;
#else
    if (g_glContext) {
        std::thread::id me = std::this_thread::get_id();
        if (g_glCurrentThread.load() != me) {
            SDL_GL_MakeCurrent(g_hiddenWindow, g_glContext);
            g_glCurrentThread.store(me);
        }
        return true;
    }
    if (g_glInitFailed) {
        return false;
    }
    // SDL video is already initialized by the platform layer for the main
    // window; creating a second hidden window is fine.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);

    // macOS requires SDL_CreateWindow / NSWindow on the MAIN thread, but we run
    // on a guest thread (whoever called glXCreateContext). Defer the window
    // creation to the main loop and block until it's made. The GL CONTEXT is
    // thread-affine, so we create + make it current on THIS (the gl64) thread
    // after the window exists — the window just has to be born on the main one.
    xwireRunOnMainThread([&]{
        g_hiddenWindow = SDL_CreateWindow("bw64-gl", 0, 0, g_drawW, g_drawH,
                                          SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    });
    if (!g_hiddenWindow) {
        klog_fmt("gl64: SDL_CreateWindow(GL) failed: %s", SDL_GetError());
        g_glInitFailed = true;
        return false;
    }
    g_glContext = SDL_GL_CreateContext(g_hiddenWindow);
    if (!g_glContext) {
        klog_fmt("gl64: SDL_GL_CreateContext failed: %s", SDL_GetError());
        SDL_DestroyWindow(g_hiddenWindow);
        g_hiddenWindow = nullptr;
        g_glInitFailed = true;
        return false;
    }
    SDL_GL_MakeCurrent(g_hiddenWindow, g_glContext);
    g_glCurrentThread.store(std::this_thread::get_id());
    klog_fmt("gl64: host GL up — vendor='%s' renderer='%s' version='%s'",
             (const char*)glGetString(GL_VENDOR),
             (const char*)glGetString(GL_RENDERER),
             (const char*)glGetString(GL_VERSION));
    return true;
#endif // __EMSCRIPTEN__
}

// Resize the GL target to match the guest drawable.
void resizeTarget(int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (w == g_drawW && h == g_drawH && g_glContext) return;
    g_drawW = w; g_drawH = h;
#ifdef __EMSCRIPTEN__
    // Recreate the offscreen FBO attachments at the new size on the GL thread.
    if (g_emCtx) glOnMain([&]{ emBuildFbo(w, h); });
#else
    if (g_hiddenWindow) {
        // SDL_SetWindowSize touches the NSWindow → must run on the macOS main
        // thread, same as SDL_CreateWindow. Off-thread it hangs/crashes (this was
        // the glViewport stall). Defer + block.
        xwireRunOnMainThread([&]{ SDL_SetWindowSize(g_hiddenWindow, w, h); });
    }
#endif
}

// Read the rendered framebuffer and present it through the X11-wire sink.
// On WASM this whole body runs on the GL-owning main thread (caller wraps it via
// glOnMain at the glXSwapBuffers case); the glReadPixels targets our offscreen
// FBO, which is left bound after rendering.
void readbackAndPresent() {
    if (!g_glContext || !g_currentDrawable || !g_xwirePresentSink) {
        return;
    }
    int w = g_drawW, h = g_drawH;
    if (w <= 0 || h <= 0) return;
    size_t pixels = (size_t)w * h;
    g_rgba.resize(pixels * 4);
    g_bgrx.resize(pixels * 4);

#ifdef __EMSCRIPTEN__
    if (g_emFbo) glBindFramebuffer(GL_FRAMEBUFFER, g_emFbo);
#endif
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, g_rgba.data());

    // glReadPixels is bottom-up RGBA; the sink wants top-down BGRX (X11 ZPixmap
    // on a 0x00RRGGBB TrueColor visual == byte order B,G,R,X little-endian).
    for (int y = 0; y < h; y++) {
        const U8* src = g_rgba.data() + (size_t)(h - 1 - y) * w * 4;
        U8* dst = g_bgrx.data() + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            dst[x*4+0] = src[x*4+2]; // B
            dst[x*4+1] = src[x*4+1]; // G
            dst[x*4+2] = src[x*4+0]; // R
            dst[x*4+3] = 0;          // X
        }
    }
    g_xwirePresentSink->onWindowMapped(g_currentDrawable, (U16)w, (U16)h);
    g_xwirePresentSink->submitFrame(g_currentDrawable, (U16)w, (U16)h,
                                    g_bgrx.data(), (U32)w * 4);
}

// --- argument decoders ------------------------------------------------------
// Args were packed by the guest per gl64bridge_abi.h conventions.
inline U64   ai(const GL64Args& a, int i)  { return a.a[i]; }
inline float af(const GL64Args& a, int i)  { U32 u = (U32)a.a[i]; float f; memcpy(&f, &u, 4); return f; }
inline double ad(const GL64Args& a, int i) { U64 u = a.a[i]; double d; memcpy(&d, &u, 8); return d; }

// Read `count` floats from a guest buffer (params* for glLightfv etc.).
void readFloats(CPU64* cpu, U64 guestAddr, float* out, int count) {
    if (!guestAddr) { for (int i=0;i<count;i++) out[i]=0; return; }
    cpu->memory->memcpyFromGuest(out, guestAddr, (U64)count * 4);
}

} // namespace

U64 gl64Bridge(CPU64* cpu, U64 fnId, U64 argsAddr) {
    std::lock_guard<std::recursive_mutex> lk(g_glMutex);

    // BW64_GLTRACE: confirm the guest opengl32/winex11 is actually issuing the
    // private gl64 trap (and which fn ids). If a GL app reaches a window but no
    // GLTRACE line ever prints, the guest-side GL dll is NOT the gl64 build —
    // it's trying the stock GLX path the wire server doesn't implement, so it
    // never gets here. First hit + per-id-once keeps it cheap.
    if (const char* gt = getenv("BW64_GLTRACE")) {
        static std::atomic<bool> announced{false};
        if (!announced.exchange(true))
            klog_fmt("gl64: FIRST trap — guest IS using the gl64 bridge (fnId=%llu)",
                     (unsigned long long)fnId);
        // BW64_GLTRACE=2: log EVERY call (fnId) — find the last GL call before a
        // hang/crash. Noisy; only for pinpointing a stall.
        if (gt[0] == '2')
            klog_fmt("gl64: call fnId=%llu", (unsigned long long)fnId);
    }
    GL64Args args = {};
    if (argsAddr) {
        cpu->memory->memcpyFromGuest(&args, argsAddr, sizeof(args));
    }

    switch (fnId) {
        // === GLX / context bootstrap =====================================
        case GL64_fn_glXQueryVersion: {
            // args[0]=out major*, args[1]=out minor* -> write 1.4, return True
            if (args.a[0]) cpu->memory->writed(args.a[0], 1);
            if (args.a[1]) cpu->memory->writed(args.a[1], 4);
            return 1;
        }
        case GL64_fn_glXQueryExtension: {
            if (args.a[0]) cpu->memory->writed(args.a[0], 0); // errorBase
            if (args.a[1]) cpu->memory->writed(args.a[1], 0); // eventBase
            return 1;
        }
        case GL64_fn_glXIsDirect:
            return 1;
        case GL64_fn_glXChooseVisual:
        case GL64_fn_glXGetVisualFromFBConfig:
            // Hand back an opaque non-null id; guest never dereferences it.
            return ++g_nextOpaqueId;
        case GL64_fn_glXChooseFBConfig:
        case GL64_fn_glXGetFBConfigs: {
            // args[last-1]=out nelements*. Report exactly one config.
            // glXChooseFBConfig(screen, attribs, nelem*) -> a[2]=nelem
            // glXGetFBConfigs(screen, nelem*)            -> a[1]=nelem
            U64 nelemAddr = (fnId == GL64_fn_glXChooseFBConfig) ? args.a[2] : args.a[1];
            if (nelemAddr) cpu->memory->writed(nelemAddr, 1);
            return ++g_nextOpaqueId; // pointer to (opaque) config array
        }
        case GL64_fn_glXGetFBConfigAttrib:
        case GL64_fn_glXGetConfig: {
            // (config_or_vis, attribute, out value*) -> write a sane default, 0
            U64 valueAddr = args.a[2];
            if (valueAddr) cpu->memory->writed(valueAddr, 1);
            return 0; // Success
        }
        case GL64_fn_glXCreateContext:
        case GL64_fn_glXCreateContextAttribsARB:
            ensureContext();
            return ++g_nextOpaqueId; // opaque GLXContext
        case GL64_fn_glXMakeCurrent:
        case GL64_fn_glXMakeContextCurrent: {
            // glXMakeCurrent(drawable, ctx)            -> a[0]=drawable
            // glXMakeContextCurrent(draw, read, ctx)   -> a[0]=draw
            if (!ensureContext()) return 0;
            g_currentDrawable = (U32)args.a[0];
#ifndef __EMSCRIPTEN__
            SDL_GL_MakeCurrent(g_hiddenWindow, g_glContext);
#endif
            // WASM: the context is re-made-current per op inside glOnMain.
            return 1;
        }
        case GL64_fn_glXSwapBuffers:
            if (g_glContext) {
                // glFlush + readback both touch GL — run them together on the
                // GL-owning thread so the FBO is bound and flushed in one hop.
                glOnMain([&]{ glFlush(); readbackAndPresent(); });
            }
            return 0;
        case GL64_fn_glXDestroyContext:
            return 0; // keep the single context alive for first light
        case GL64_fn_glXGetCurrentContext:
            return g_glContext ? g_nextOpaqueId : 0;
        case GL64_fn_glXGetCurrentDrawable:
            return g_currentDrawable;
        case GL64_fn_glXWaitGL:
            if (g_glContext) GL_MT(glFinish());
            return 0;
        case GL64_fn_glXWaitX:
        case GL64_fn_glXSwapIntervalEXT:
            return 0;
        case GL64_fn_glXQueryExtensionsString:
        case GL64_fn_glXQueryServerString:
        case GL64_fn_glXGetClientString:
            // String returns are not needed for first light (winex11 tolerates
            // empty); returning 0 makes the guest wrapper hand back "".
            return 0;

        // === core GL: state ==============================================
        // Each gl* call is wrapped in GL_MT so it executes on the GL-owning
        // thread (the platform main thread on WASM; inline natively).
        case GL64_fn_glClearColor:
            if (ensureContext()) GL_MT(glClearColor(af(args,0), af(args,1), af(args,2), af(args,3)));
            return 0;
        case GL64_fn_glClear:
            if (g_glContext) GL_MT(glClear((GLbitfield)ai(args,0)));
            return 0;
        case GL64_fn_glClearDepth:
            if (g_glContext) GL_MT(glClearDepth(ad(args,0)));
            return 0;
        case GL64_fn_glViewport:
            if (ensureContext()) {
                resizeTarget((int)ai(args,2), (int)ai(args,3));
                GL_MT(glViewport((GLint)ai(args,0), (GLint)ai(args,1), (GLsizei)ai(args,2), (GLsizei)ai(args,3)));
            }
            return 0;
        case GL64_fn_glEnable:
            if (g_glContext) GL_MT(glEnable((GLenum)ai(args,0)));
            return 0;
        case GL64_fn_glDisable:
            if (g_glContext) GL_MT(glDisable((GLenum)ai(args,0)));
            return 0;
        case GL64_fn_glShadeModel:
            if (g_glContext) GL_MT(glShadeModel((GLenum)ai(args,0)));
            return 0;
        case GL64_fn_glDepthFunc:
            if (g_glContext) GL_MT(glDepthFunc((GLenum)ai(args,0)));
            return 0;
        case GL64_fn_glCullFace:
            if (g_glContext) GL_MT(glCullFace((GLenum)ai(args,0)));
            return 0;
        case GL64_fn_glFrontFace:
            if (g_glContext) GL_MT(glFrontFace((GLenum)ai(args,0)));
            return 0;
        case GL64_fn_glHint:
            if (g_glContext) GL_MT(glHint((GLenum)ai(args,0), (GLenum)ai(args,1)));
            return 0;
        case GL64_fn_glFlush:
            if (g_glContext) GL_MT(glFlush());
            return 0;
        case GL64_fn_glFinish:
            if (g_glContext) GL_MT(glFinish());
            return 0;
        case GL64_fn_glGetError: {
            if (!g_glContext) return 0;
            U64 err = 0;
            GL_MT(err = glGetError());
            return err;
        }
        case GL64_fn_glGetString: {
            // Return value is a const char* — guest can't use a host pointer.
            // For first light the guest wrapper falls back to its own static
            // strings when we return 0.
            return 0;
        }
        case GL64_fn_glGetIntegerv: {
            if (g_glContext && args.a[1]) {
                GLint v[16] = {0};
                GL_MT(glGetIntegerv((GLenum)ai(args,0), v)); // read on GL thread
                cpu->memory->memcpyToGuest(args.a[1], v, sizeof(GLint)); // 1 value (enough for first light)
            }
            return 0;
        }
        case GL64_fn_glGetFloatv: {
            if (g_glContext && args.a[1]) {
                GLfloat v[16] = {0};
                GL_MT(glGetFloatv((GLenum)ai(args,0), v)); // read on GL thread
                cpu->memory->memcpyToGuest(args.a[1], v, sizeof(GLfloat));
            }
            return 0;
        }
        case GL64_fn_glColor3f:
            if (g_glContext) GL_MT(glColor3f(af(args,0), af(args,1), af(args,2)));
            return 0;
        case GL64_fn_glColor4f:
            if (g_glContext) GL_MT(glColor4f(af(args,0), af(args,1), af(args,2), af(args,3)));
            return 0;

        // === core GL: matrices ===========================================
        case GL64_fn_glMatrixMode:
            if (g_glContext) GL_MT(glMatrixMode((GLenum)ai(args,0)));
            return 0;
        case GL64_fn_glLoadIdentity:
            if (g_glContext) GL_MT(glLoadIdentity());
            return 0;
        case GL64_fn_glPushMatrix:
            if (g_glContext) GL_MT(glPushMatrix());
            return 0;
        case GL64_fn_glPopMatrix:
            if (g_glContext) GL_MT(glPopMatrix());
            return 0;
        case GL64_fn_glFrustum:
            if (g_glContext) GL_MT(glFrustum(ad(args,0), ad(args,1), ad(args,2), ad(args,3), ad(args,4), ad(args,5)));
            return 0;
        case GL64_fn_glOrtho:
            if (g_glContext) GL_MT(glOrtho(ad(args,0), ad(args,1), ad(args,2), ad(args,3), ad(args,4), ad(args,5)));
            return 0;
        case GL64_fn_glTranslatef:
            if (g_glContext) GL_MT(glTranslatef(af(args,0), af(args,1), af(args,2)));
            return 0;
        case GL64_fn_glRotatef:
            if (g_glContext) GL_MT(glRotatef(af(args,0), af(args,1), af(args,2), af(args,3)));
            return 0;
        case GL64_fn_glScalef:
            if (g_glContext) GL_MT(glScalef(af(args,0), af(args,1), af(args,2)));
            return 0;
        case GL64_fn_glMultMatrixf: {
            // readFloats reads guest memory — keep it on the calling thread, then
            // hand the local matrix to the GL call on the GL thread.
            if (g_glContext) { float m[16]; readFloats(cpu, args.a[0], m, 16); GL_MT(glMultMatrixf(m)); }
            return 0;
        }

        // === core GL: lighting / material ================================
        case GL64_fn_glLightfv: {
            if (g_glContext) { float p[4]; readFloats(cpu, args.a[2], p, 4); GL_MT(glLightfv((GLenum)ai(args,0), (GLenum)ai(args,1), p)); }
            return 0;
        }
        case GL64_fn_glLightf:
            if (g_glContext) GL_MT(glLightf((GLenum)ai(args,0), (GLenum)ai(args,1), af(args,2)));
            return 0;
        case GL64_fn_glMaterialfv: {
            if (g_glContext) { float p[4]; readFloats(cpu, args.a[2], p, 4); GL_MT(glMaterialfv((GLenum)ai(args,0), (GLenum)ai(args,1), p)); }
            return 0;
        }
        case GL64_fn_glMaterialf:
            if (g_glContext) GL_MT(glMaterialf((GLenum)ai(args,0), (GLenum)ai(args,1), af(args,2)));
            return 0;
        case GL64_fn_glColorMaterial:
            if (g_glContext) GL_MT(glColorMaterial((GLenum)ai(args,0), (GLenum)ai(args,1)));
            return 0;
        case GL64_fn_glNormal3f:
            if (g_glContext) GL_MT(glNormal3f(af(args,0), af(args,1), af(args,2)));
            return 0;

        // === core GL: immediate-mode geometry ============================
        case GL64_fn_glBegin:
            if (g_glContext) GL_MT(glBegin((GLenum)ai(args,0)));
            return 0;
        case GL64_fn_glEnd:
            if (g_glContext) GL_MT(glEnd());
            return 0;
        case GL64_fn_glVertex2f:
            if (g_glContext) GL_MT(glVertex2f(af(args,0), af(args,1)));
            return 0;
        case GL64_fn_glVertex3f:
            if (g_glContext) GL_MT(glVertex3f(af(args,0), af(args,1), af(args,2)));
            return 0;

        default:
            klog_fmt("gl64: unimplemented fn id %llu", (unsigned long long)fnId);
            return 0;
    }
}

#endif // BOXEDWINE_OPENGL
