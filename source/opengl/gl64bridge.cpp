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
#include "../x11wire/xwireserver.h"

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
// <GL/gl.h> alone declares the FFP + GLES2 basics but NOT the FBO/renderbuffer
// entry points (glGenFramebuffers, glBindFramebuffer, glRenderbufferStorage, ...);
// those live in <GL/glext.h> and need GL_GLEXT_PROTOTYPES to get real prototypes
// (not just function-pointer typedefs). They are core in WebGL2/GLES3, so Emscripten
// links them. We render offscreen into an FBO, hence these are required.
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/html5_webgl.h>
#else
#include <SDL_opengl.h>
#endif
#include <vector>
#include <thread>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <string>
#include <mutex>

#ifdef __EMSCRIPTEN__
// Emscripten's -sLEGACY_GL_EMULATION supplies most fixed-function entry points,
// but a few FFP lighting/material scalars are missing from its glemu and don't
// link: glLightf, glMaterialf, glColorMaterial. The *fv (vector) variants DO
// link, so route the scalars through them (1-element param). glColorMaterial has
// no glemu equivalent — Emscripten's FFP shader synthesis already folds glColor
// into the material, so a no-op preserves the visible result. These shims keep
// the bridge's call sites (GL_MT(glLightf(...)) etc.) unchanged.
static inline void bw_glLightf(GLenum light, GLenum pname, GLfloat v) {
    GLfloat p[1] = { v }; glLightfv(light, pname, p);
}
static inline void bw_glMaterialf(GLenum face, GLenum pname, GLfloat v) {
    GLfloat p[1] = { v }; glMaterialfv(face, pname, p);
}
static inline void bw_glColorMaterial(GLenum, GLenum) { /* glemu folds glColor */ }
#define glLightf        bw_glLightf
#define glMaterialf     bw_glMaterialf
#define glColorMaterial bw_glColorMaterial
#endif

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
// Record an immediate-mode call (color/normal/vertex) into g_immOps when inside
// a glBegin..glEnd block so it can be replayed in one hop at glEnd; otherwise
// (a stray color/normal outside glBegin/glEnd) fall back to a direct hop. fn is
// the GL64_fn_* id; a0..a3 are the float args; stmt is the direct GL call.
#define RECORD_IMM(fn, a0, a1, a2, a3, stmt)                       \
    do {                                                          \
        if (g_inImmediate) g_immOps.push_back(ImmOp{(U16)(fn), {(a0),(a1),(a2),(a3)}}); \
        else GL_MT(stmt);                                         \
    } while (0)
#else
// Native: the existing code is already on a GL-capable thread (re-make-current
// handled in ensureContext); run inline.
template <typename Fn>
inline void glOnMain(Fn&& fn) { fn(); }
#define GL_MT(stmt) do { stmt; } while (0)
#define RECORD_IMM(fn, a0, a1, a2, a3, stmt) do { stmt; } while (0)
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

#ifdef __EMSCRIPTEN__
// --- immediate-mode batching ------------------------------------------------
// Every gl* call normally hops to the GL-owning thread via glOnMain (a blocking
// xwireRunOnMainThread round-trip). Inside a glBegin/glEnd block that is one hop
// PER glVertex/glColor/glNormal — for glcube ~26 blocking hops per face group,
// so the cube renders at seconds-per-frame and never looks animated even though
// the guest advances its rotation every frame (glcube.c: angle += 1.5f).
//
// LEGACY_GL_EMULATION only buffers immediate-mode data in JS until glEnd flushes
// it, so the individual calls have no observable GL side effects before glEnd.
// That lets us RECORD them host-side (on the calling thread, under g_glMutex —
// gl64Bridge already holds it, so recording is serialized) and REPLAY the whole
// glBegin..glEnd as a SINGLE glOnMain hop at glEnd. One hop per primitive block
// instead of one per vertex.
struct ImmOp {
    U16   fn;        // GL64_fn_* of the recorded immediate-mode call
    float f[4];      // up to 4 float args (color4f/vertex3f/normal3f/...)
};
bool               g_inImmediate = false;   // between glBegin and glEnd
GLenum             g_immMode     = 0;        // mode passed to glBegin
std::vector<ImmOp> g_immOps;                 // recorded calls, replayed at glEnd

// Replay one recorded immediate-mode op on the GL thread (called inside the
// single glEnd glOnMain hop). Mirrors the per-case GL calls below.
inline void replayImmOp(const ImmOp& op) {
    switch (op.fn) {
        case GL64_fn_glColor3f:  glColor3f(op.f[0], op.f[1], op.f[2]); break;
        case GL64_fn_glColor4f:  glColor4f(op.f[0], op.f[1], op.f[2], op.f[3]); break;
        case GL64_fn_glNormal3f: glNormal3f(op.f[0], op.f[1], op.f[2]); break;
        case GL64_fn_glVertex2f: glVertex2f(op.f[0], op.f[1]); break;
        case GL64_fn_glVertex3f: glVertex3f(op.f[0], op.f[1], op.f[2]); break;
        default: break;
    }
}

// Replay a recorded glBegin..glEnd block, re-emitting the CURRENT color (and
// normal) immediately BEFORE every vertex.
//
// Why: Emscripten's LEGACY_GL_EMULATION (glemu) does NOT implement real OpenGL
// "current attribute" semantics for immediate mode. Each glColor/glNormal/
// glVertex appends one component to a single interleaved temp buffer and bumps
// an attribute counter; at glEnd it derives a UNIFORM per-vertex stride from the
// first occurrence of each attribute and the assumption that every attribute is
// re-specified once per vertex (color, vertex, color, vertex, ...). glcube (like
// most classic GL code) sets the color ONCE per face and then emits 4 vertices
// (color, vertex, vertex, vertex, vertex). glemu then has 6 colors for 24
// vertices, so the per-vertex stride it computed (1 color float + 4 position
// floats = 20 bytes) no longer matches the data actually written, and
// numVertices = 4*vertexCounter/stride is not even integral — the vertices get
// read at the wrong offsets and the cube renders as scrambled rotating
// triangles. (With ASSERTIONS=0 glemu's own "numVertices must be an integer"
// guard is compiled out, so it fails silently.)
//
// Real GL says the current color/normal applies to every subsequent vertex, so
// emitting one color+normal per vertex is semantically identical and gives glemu
// the uniform color-per-vertex layout it requires. Native GL is unaffected
// (this path is WASM-only and the redundant glColor calls are free there too).
inline void replayImmBlock(const std::vector<ImmOp>& ops) {
    bool  haveColor = false, haveNormal = false;
    float col[4]  = {1, 1, 1, 1};
    float norm[3] = {0, 0, 1};
    for (const ImmOp& op : ops) {
        switch (op.fn) {
            case GL64_fn_glColor3f:
                col[0] = op.f[0]; col[1] = op.f[1]; col[2] = op.f[2]; col[3] = 1.0f;
                haveColor = true;
                break;
            case GL64_fn_glColor4f:
                col[0] = op.f[0]; col[1] = op.f[1]; col[2] = op.f[2]; col[3] = op.f[3];
                haveColor = true;
                break;
            case GL64_fn_glNormal3f:
                norm[0] = op.f[0]; norm[1] = op.f[1]; norm[2] = op.f[2];
                haveNormal = true;
                break;
            case GL64_fn_glVertex2f:
            case GL64_fn_glVertex3f:
                // Re-assert the current attributes for THIS vertex so glemu sees
                // one color (and normal) per vertex — a uniform interleaved layout.
                if (haveColor)  glColor4f(col[0], col[1], col[2], col[3]);
                if (haveNormal) glNormal3f(norm[0], norm[1], norm[2]);
                if (op.fn == GL64_fn_glVertex2f) glVertex2f(op.f[0], op.f[1]);
                else                             glVertex3f(op.f[0], op.f[1], op.f[2]);
                break;
            default:
                break;
        }
    }
}
#endif

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
        // This code runs on the platform "main" thread (the PROXY_TO_PTHREAD main
        // pthread). The build transfers #gl64canvas to THIS pthread at startup as an
        // OffscreenCanvas (-sOFFSCREENCANVAS_SUPPORT=1 -sOFFSCREENCANVASES_TO_PTHREAD=
        // '#gl64canvas'), so we can create a REAL, thread-owned WebGL2 context here
        // — no proxyContextToMainThread. That real context gives glemu a usable JS
        // GLctx object (with .createShader etc.) so the FFP→shader synthesis behind
        // glBegin/glEnd works on this thread. renderViaOffscreenBackBuffer lets a
        // single frame be composited across event callbacks and presented via an
        // explicit swap (we glReadPixels our FBO and hand off to the X11 sink).
        attr.renderViaOffscreenBackBuffer = true;
        // A canvas can hold only ONE context. The page's "#canvas" is already
        // owned by SDL's emscripten renderer (SDL_CreateRenderer makes a WebGL
        // context on it), so emscripten_webgl_create_context("#canvas") FAILS.
        // We render offscreen into our own FBO and glReadPixels the result to the
        // present sink — never to a page canvas — so we just need our OWN canvas.
        // The transferred OffscreenCanvas lives in GL.offscreenCanvases['#gl64canvas']
        // ON THIS pthread (its control was transferred off the DOM element by
        // -sOFFSCREENCANVASES_TO_PTHREAD), so create the context here directly.
        g_emCtx = emscripten_webgl_create_context("#gl64canvas", &attr);
        if (!g_emCtx) {
            klog_fmt("gl64: emscripten_webgl_create_context failed (own canvas)");
            return;
        }
        emscripten_webgl_make_context_current(g_emCtx);
        emBuildFbo(g_drawW, g_drawH);
        // LEGACY_GL_EMULATION's immediate-mode state (GLImmediate.matrix /
        // matrixStack / temp vertex buffers / TexEnvJIT) is initialized by
        // GLImmediate.init(), which Emscripten only registers as a
        // Browser.moduleContextCreatedCallbacks hook — fired for contexts made via
        // the SDL/Browser path, NOT for our emscripten_webgl_create_context. Without
        // it, GLImmediate.matrix is still [] when the guest calls glMatrixMode/
        // glLoadIdentity (fnId 260/261), so mat4.identity(undefined) throws "Cannot
        // read properties of undefined". Run it here, on THIS (the GL-owning) thread
        // — the same thread the GL_MT immediate-mode ops will run on — so the matrix
        // stack & temp buffers exist before the first glMatrixMode. Guarded: init()
        // sets GLImmediate.initted, so only run it the first time.
        int gli = EM_ASM_INT({
            // GLImmediate.init() early-returns unless Browser.useWebGL is true (set
            // by the SDL/Browser context path we bypass), AND SDL's own context
            // creation fires init() first — with useWebGL false — so it sets
            // initted=true but leaves matrix/matrixStack/matrixVersion EMPTY. The
            // guest's first glLoadIdentity (fnId 261) then crashes on the empty
            // arrays. Force useWebGL on and re-run init(). init() touches GLctx
            // (getParameter/TexEnvJIT) which can THROW on our proxied context BEFORE
            // it reaches the matrix loop — so if init left the matrices empty, build
            // them directly here (matrixLib is module-scope, no GLctx needed). The
            // matrix stack is all glMatrixMode/glLoadIdentity/glFrustum/glRotate need.
            // Return a status code so the host can log what happened.
            var rc = 0;
            try {
                if (typeof Browser !== 'undefined') Browser.useWebGL = true;
                if (typeof GLImmediate === 'undefined') return 1;
                // GLImmediate.init()'s FIRST line does
                //   GLImmediate.MAX_TEXTURES = Math.min(Module["GL_MAX_TEXTURE_IMAGE_UNITS"]
                //                                       || GLctx.getParameter(...), 28);
                // On our proxied context GLctx is the raw integer handle (no
                // .getParameter), so that throws and init() aborts before building
                // rendererCache/TexEnvJIT/temp buffers. Pre-seed the Module override so
                // init() takes the `||` short-circuit and never touches GLctx — letting
                // it run to completion. 8 texture units is plenty for glcube's FFP.
                if (typeof Module !== 'undefined' && !Module["GL_MAX_TEXTURE_IMAGE_UNITS"])
                    Module["GL_MAX_TEXTURE_IMAGE_UNITS"] = 8;
                if (typeof GLImmediate.init === 'function' &&
                    (!GLImmediate.matrix || GLImmediate.matrix.length === 0)) {
                    GLImmediate.initted = false;
                    try { GLImmediate.init(); rc = 2; } catch (e) { rc = 3; }
                }
                // If init() still threw before building the renderer cache (needed by
                // glEnd's flush: rendererCache.getStaticKeyView()), build it directly.
                if (!GLImmediate.rendererCache && GLImmediate.MapTreeLib) {
                    GLImmediate.rendererCache = GLImmediate.MapTreeLib.create();
                    rc += 40; // marks the rendererCache fallback ran
                }
                // Fallback: build whatever GLImmediate.init() left unset. init() can
                // throw on our proxied context (GLctx.getParameter/TexEnvJIT) AFTER
                // setting initted but BEFORE the matrix loop and the temp-buffer
                // allocations, so both the matrix stack AND the vertex temp buffers
                // can be missing. The matrix stack feeds glMatrixMode/glLoadIdentity/
                // glFrustum/glRotate; the temp buffers (tempData/vertexDataU8) feed
                // glColor/glVertex inside glBegin/glEnd. matrixLib + MAX_TEMP_BUFFER_SIZE
                // are module-scope (no GLctx needed), so we can build all of it here.
                if ((!GLImmediate.matrix || GLImmediate.matrix.length === 0) &&
                    GLImmediate.matrixLib) {
                    var n = 2 + (GLImmediate.MAX_TEXTURES > 0 ? GLImmediate.MAX_TEXTURES : 0);
                    GLImmediate.matrix = [];
                    GLImmediate.matrixStack = [];
                    GLImmediate.matrixVersion = [];
                    for (var i = 0; i < n; i++) {
                        GLImmediate.matrixStack.push([]);
                        GLImmediate.matrixVersion.push(0);
                        var m = GLImmediate.matrixLib.mat4.create();
                        GLImmediate.matrixLib.mat4.identity(m);
                        GLImmediate.matrix.push(m);
                    }
                    rc += 10; // marks the matrix fallback ran
                }
                // Vertex temp buffers for immediate-mode glColor/glVertex (the
                // glBegin/glEnd path). Without these, glColor3f writes to a null
                // vertexDataU8 and crashes ("Cannot set properties of null").
                if (!GLImmediate.vertexDataU8 && typeof GL !== 'undefined' &&
                    GL.MAX_TEMP_BUFFER_SIZE) {
                    GLImmediate.tempData = new Float32Array(GL.MAX_TEMP_BUFFER_SIZE >> 2);
                    GLImmediate.indexData = new Uint16Array(GL.MAX_TEMP_BUFFER_SIZE >> 1);
                    GLImmediate.vertexDataU8 = new Uint8Array(GLImmediate.tempData.buffer);
                    if (!GLImmediate.clientColor)
                        GLImmediate.clientColor = new Float32Array([1, 1, 1, 1]);
                    try {
                        if (typeof GL.generateTempBuffers === 'function')
                            GL.generateTempBuffers(true, GL.currentContext);
                    } catch (e) {}
                    rc += 20; // marks the vertex-buffer fallback ran
                }
                // GL_QUADS index buffer. glcube draws its faces with glBegin(GL_QUADS);
                // WebGL has no GL_QUADS, so glemu's flush (library_glemu.js) triangulates
                // a quad block by binding GL.currentContext.tempQuadIndexBuffer — a
                // precomputed 0 1 2, 0 2 3, 4 5 6, 4 6 7, ... index buffer — and issuing
                // drawElements(TRIANGLES). That buffer is ONLY created by
                // generateTempBuffers(quads=TRUE), which runs from GLImmediate.init().
                // Our bridge makes its OWN context via emscripten_webgl_create_context
                // (NOT the SDL/Browser path), so that context was set up with
                // generateTempBuffers(false) and has NO tempQuadIndexBuffer; and the
                // vbuf fallback above (which DOES pass quads=true) is gated on
                // !vertexDataU8, which an earlier partial SDL-path init may have already
                // set — skipping quad-buffer creation entirely. With tempQuadIndexBuffer
                // undefined, glemu binds element-array 0, so drawElements reads zeroed/
                // garbage indices and the cube's quads come out as scrambled triangles
                // that still transform (rotate) correctly. Build it UNCONDITIONALLY on
                // THIS context, independent of the vertexDataU8 guard. ASSERTIONS=0 in
                // the wasm64-mt link means glemu's own quad asserts are compiled out, so
                // the missing buffer fails silently — hence no error in the console.
                if (typeof GL !== 'undefined' && GL.currentContext &&
                    !GL.currentContext.tempQuadIndexBuffer && GL.MAX_TEMP_BUFFER_SIZE &&
                    typeof GLctx !== 'undefined' && GLctx.createBuffer) {
                    try {
                        var qb = GLctx.createBuffer();
                        var savedEab = GLctx.getParameter(GLctx.ELEMENT_ARRAY_BUFFER_BINDING);
                        GLctx.bindBuffer(GLctx.ELEMENT_ARRAY_BUFFER, qb);
                        var numIndexes = GL.MAX_TEMP_BUFFER_SIZE >> 1;
                        var quadIndexes = new Uint16Array(numIndexes);
                        var qi = 0;
                        var qv = 0;
                        while (1) {
                            quadIndexes[qi++] = qv;     if (qi >= numIndexes) break;
                            quadIndexes[qi++] = qv + 1; if (qi >= numIndexes) break;
                            quadIndexes[qi++] = qv + 2; if (qi >= numIndexes) break;
                            quadIndexes[qi++] = qv;     if (qi >= numIndexes) break;
                            quadIndexes[qi++] = qv + 2; if (qi >= numIndexes) break;
                            quadIndexes[qi++] = qv + 3; if (qi >= numIndexes) break;
                            qv += 4;
                        }
                        GLctx.bufferData(GLctx.ELEMENT_ARRAY_BUFFER, quadIndexes, GLctx.STATIC_DRAW);
                        GLctx.bindBuffer(GLctx.ELEMENT_ARRAY_BUFFER, savedEab || null);
                        GL.currentContext.tempQuadIndexBuffer = qb;
                        rc += 4; // marks the quad-index-buffer fallback ran
                    } catch (e) {}
                }
                return rc + (GLImmediate.matrix ? GLImmediate.matrix.length * 100 : 0);
            } catch (e) { return 99; }
        });
        if (getenv("BW64_GLTRACE"))
            klog_fmt("gl64: GLImmediate init rc=%d (rc%%100: 2=init-ok 3=init-threw "
                     "+4=quadIndexBuf +10=matrix +20=vbuf +40=rendererCache fallback; "
                     "rc/100=matrix.length)", gli);
        klog_fmt("gl64: host GL up (WebGL2) — vendor='%s' renderer='%s' version='%s'",
                 (const char*)glGetString(GL_VENDOR),
                 (const char*)glGetString(GL_RENDERER),
                 (const char*)glGetString(GL_VERSION));
    });
    if (!g_emCtx) { g_glInitFailed = true; klog_fmt("gl64: ensureContext FAILED (emCtx still 0 after glOnMain)"); return false; }
    g_glContext = (void*)1; // sentinel: the rest of the file checks g_glContext
    if (getenv("BW64_GLTRACE"))
        klog_fmt("gl64: ensureContext OK (emCtx=%llu)", (unsigned long long)g_emCtx);
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

#ifdef __EMSCRIPTEN__
// True once any draw has hit our offscreen FBO this frame — lets the swap path
// know a programmable-pipeline frame was produced (vs. the immediate-mode path).
std::atomic<bool> g_glDrew{false};

// Tiny ring of the most recent gl64 fnIds, for annotating nonzero glGetError
// logs (which GL call likely produced the error). Guarded by g_glMutex.
U64 g_lastFnIds[8] = {0};
int g_lastFnIdx = 0;

// Translate wined3d's desktop-GLSL (#version 120/130) shader source into the
// GLSL ES 3.00 dialect WebGL2 requires. WebGL2 rejects `#version 120` outright,
// and the legacy `attribute`/`varying`/`gl_FragData`/`texture2D` keywords don't
// exist in GLSL ES 3.00. wined3d's generated shaders are regular enough that a
// token-level rewrite suffices for the D3D fixed-function + simple-shader path.
// `isFragment` selects the varying direction and the fragment-output handling.
std::string translateGlslToEs300(const std::string& srcIn, bool isFragment) {
    std::string src = srcIn;

    // Replace whole-word `needle` with `repl` (so we don't corrupt identifiers
    // like `attribute_foo`). ASCII word chars = [A-Za-z0-9_].
    auto isWord = [](char c){ return (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'; };
    auto wordReplace = [&](const std::string& needle, const std::string& repl){
        std::string out; out.reserve(src.size());
        size_t i = 0;
        while (i < src.size()) {
            size_t p = src.find(needle, i);
            if (p == std::string::npos) { out.append(src, i, std::string::npos); break; }
            bool lb = (p == 0) || !isWord(src[p-1]);
            bool rb = (p+needle.size() >= src.size()) || !isWord(src[p+needle.size()]);
            out.append(src, i, p - i);
            if (lb && rb) out += repl; else out += needle;
            i = p + needle.size();
        }
        src.swap(out);
    };

    // Strip EVERY existing #version line (we prepend our own). wined3d sometimes
    // emits more than one #version, and a SECOND one further down re-starts the
    // shader — discarding our prepended `precision` line ("No precision specified
    // for float") and itself violating "#version must occur before anything else".
    for (size_t v; (v = src.find("#version")) != std::string::npos; ) {
        size_t eol = src.find('\n', v);
        src.erase(v, (eol == std::string::npos ? src.size() : eol + 1) - v);
    }

    // Legacy texture lookups → unified texture().
    wordReplace("texture2DProj", "textureProj");
    wordReplace("texture2DLod",  "textureLod");
    wordReplace("texture2D",     "texture");
    wordReplace("texture3D",     "texture");
    wordReplace("textureCube",   "texture");
    wordReplace("shadow2DProj",  "textureProj");
    wordReplace("shadow2D",      "texture");

    // wined3d's fixed-function-pipeline shaders use the legacy compatibility
    // built-in varyings (gl_FrontColor/gl_Color, gl_FrontSecondaryColor/
    // gl_SecondaryColor, gl_TexCoord[], gl_FogFragCoord) that GLSL ES 3.00 removed.
    // Rewrite them to user in/out varyings with matching names across stages so the
    // program links. Order matters: do gl_FrontSecondaryColor before gl_SecondaryColor
    // and the Front* before the bare reads.
    auto rewriteFFPVaryings = [&](){
        wordReplace("gl_FrontSecondaryColor", "bw_secondary");
        wordReplace("gl_BackSecondaryColor",  "bw_secondary");
        wordReplace("gl_SecondaryColor",      "bw_secondary");
        wordReplace("gl_FrontColor",          "bw_color");
        wordReplace("gl_BackColor",           "bw_color");
        wordReplace("gl_Color",               "bw_color");
        wordReplace("gl_TexCoord",            "bw_texcoord");
        wordReplace("gl_FogFragCoord",        "bw_fogcoord");
    };
    bool usesColor     = src.find("gl_Color") != std::string::npos || src.find("gl_FrontColor") != std::string::npos || src.find("gl_BackColor") != std::string::npos;
    bool usesSecondary = src.find("gl_SecondaryColor") != std::string::npos || src.find("gl_FrontSecondaryColor") != std::string::npos || src.find("gl_BackSecondaryColor") != std::string::npos;
    bool usesTexCoord  = src.find("gl_TexCoord") != std::string::npos;
    bool usesFog       = src.find("gl_FogFragCoord") != std::string::npos;

    if (isFragment) {
        // varying → in; legacy FFP built-ins → user `in` varyings.
        wordReplace("varying", "in");
        rewriteFFPVaryings();
        bool usesFragData  = src.find("gl_FragData")  != std::string::npos;
        bool usesFragColor = src.find("gl_FragColor") != std::string::npos;
        for (size_t p; (p = src.find("gl_FragData[0]")) != std::string::npos; )
            src.replace(p, std::string("gl_FragData[0]").size(), "bw_FragColor");
        wordReplace("gl_FragColor", "bw_FragColor");
        std::string header = "#version 300 es\nprecision highp float;\nprecision highp int;\n";
        if (usesColor)     header += "in vec4 bw_color;\n";
        if (usesSecondary) header += "in vec4 bw_secondary;\n";
        if (usesTexCoord)  header += "in vec4 bw_texcoord[8];\n";
        if (usesFog)       header += "in float bw_fogcoord;\n";
        if (usesFragData || usesFragColor) header += "out vec4 bw_FragColor;\n";
        src = header + src;
    } else {
        // vertex: attribute → in, varying → out; legacy FFP built-ins → user `out`.
        wordReplace("attribute", "in");
        wordReplace("varying", "out");
        rewriteFFPVaryings();
        std::string header = "#version 300 es\nprecision highp float;\nprecision highp int;\n";
        if (usesColor)     header += "out vec4 bw_color;\n";
        if (usesSecondary) header += "out vec4 bw_secondary;\n";
        if (usesTexCoord)  header += "out vec4 bw_texcoord[8];\n";
        if (usesFog)       header += "out float bw_fogcoord;\n";
        src = header + src;
    }
    return src;
}

// Is this glTexParameter pname valid in WebGL2/GLES3? wined3d also sets several
// desktop-GL-only ones that would raise GL_INVALID_ENUM; we drop those.
bool texParamSupported(GLenum pname) {
    switch (pname) {
        case 0x2800 /*GL_TEXTURE_MAG_FILTER*/:
        case 0x2801 /*GL_TEXTURE_MIN_FILTER*/:
        case 0x2802 /*GL_TEXTURE_WRAP_S*/:
        case 0x2803 /*GL_TEXTURE_WRAP_T*/:
        case 0x8072 /*GL_TEXTURE_WRAP_R*/:
        case 0x813A /*GL_TEXTURE_MIN_LOD*/:
        case 0x813B /*GL_TEXTURE_MAX_LOD*/:
        case 0x813C /*GL_TEXTURE_BASE_LEVEL*/:
        case 0x813D /*GL_TEXTURE_MAX_LEVEL*/:
        case 0x884C /*GL_TEXTURE_COMPARE_MODE*/:
        case 0x884D /*GL_TEXTURE_COMPARE_FUNC*/:
            return true;
        default:
            return false;   // GL_TEXTURE_LOD_BIAS, GENERATE_MIPMAP, BORDER_COLOR, …
    }
}

// Read a NUL-terminated C string from guest memory, capped at `maxLen` bytes.
// Reads in chunks to avoid a per-byte trap; stops at the first NUL.
std::string readGuestCStr(CPU64* cpu, U64 guestAddr, U64 maxLen) {
    std::string out;
    if (!guestAddr) return out;
    char buf[256];
    while (out.size() < maxLen) {
        U64 chunk = sizeof(buf);
        if (chunk > maxLen - out.size()) chunk = maxLen - out.size();
        cpu->memory->memcpyFromGuest(buf, guestAddr + out.size(), chunk);
        for (U64 i = 0; i < chunk; i++) {
            if (buf[i] == 0) { out.append(buf, i); return out; }
        }
        out.append(buf, chunk);
    }
    return out;
}

// Generate/delete host GL object names and write/read the id array to/from the
// guest. n = args.a[0], guest id array = args.a[1].
enum class GenKind { Buffer, Texture, VertexArray };
void genObjects(CPU64* cpu, const GL64Args& args, GenKind kind) {
    GLsizei n = (GLsizei)args.a[0];
    if (n <= 0 || n > 65536) return;
    std::vector<GLuint> ids((size_t)n, 0);
    GLuint* p = ids.data();
    glOnMain([&]{
        switch (kind) {
            case GenKind::Buffer:      glGenBuffers(n, p); break;
            case GenKind::Texture:     glGenTextures(n, p); break;
            case GenKind::VertexArray: glGenVertexArrays(n, p); break;
        }
    });
    cpu->memory->memcpyToGuest(args.a[1], ids.data(), (U64)n * 4);
}
void deleteObjects(CPU64* cpu, const GL64Args& args, GenKind kind) {
    GLsizei n = (GLsizei)args.a[0];
    if (n <= 0 || n > 65536) return;
    std::vector<GLuint> ids((size_t)n, 0);
    cpu->memory->memcpyFromGuest(ids.data(), args.a[1], (U64)n * 4);
    const GLuint* p = ids.data();
    glOnMain([&]{
        switch (kind) {
            case GenKind::Buffer:      glDeleteBuffers(n, p); break;
            case GenKind::Texture:     glDeleteTextures(n, p); break;
            case GenKind::VertexArray: glDeleteVertexArrays(n, p); break;
        }
    });
}

// glGetShaderInfoLog / glGetProgramInfoLog: args = (obj, bufSize, length*, infoLog*).
void writeInfoLog(CPU64* cpu, const GL64Args& args, bool isProgram) {
    GLuint obj = (GLuint)args.a[0];
    GLsizei bufSize = (GLsizei)args.a[1];
    if (bufSize <= 0 || bufSize > (1 << 20)) bufSize = 0;
    std::vector<char> log((size_t)bufSize + 1, 0);
    GLsizei outLen = 0;
    char* lp = log.data();
    glOnMain([&]{
        if (isProgram) glGetProgramInfoLog(obj, bufSize, &outLen, lp);
        else           glGetShaderInfoLog(obj, bufSize, &outLen, lp);
    });
    if (args.a[2]) cpu->memory->writed(args.a[2], (U32)outLen);
    if (args.a[3] && bufSize > 0)
        cpu->memory->memcpyToGuest(args.a[3], log.data(), (U64)outLen + 1);
}

// Bytes a glTexImage2D/glTexSubImage2D pixel buffer occupies for (w,h,fmt,type).
// Covers the formats wined3d uses for texture uploads; unknown → 4 bytes/texel.
size_t texImageBytes(GLsizei w, GLsizei h, GLenum format, GLenum type) {
    if (w <= 0 || h <= 0) return 0;
    int comps;
    switch (format) {
        case GL_RGBA: case 0x80E1 /*GL_BGRA*/: comps = 4; break;
        case GL_RGB:  case 0x80E0 /*GL_BGR*/:  comps = 3; break;
        case 0x1903 /*GL_RED*/: case GL_ALPHA: case GL_LUMINANCE: comps = 1; break;
        case GL_LUMINANCE_ALPHA: case 0x8227 /*GL_RG*/: comps = 2; break;
        case GL_DEPTH_COMPONENT: comps = 1; break;
        default: comps = 4; break;
    }
    int bytesPerComp;
    switch (type) {
        case GL_UNSIGNED_BYTE: case GL_BYTE: bytesPerComp = 1; break;
        case GL_UNSIGNED_SHORT: case GL_SHORT: bytesPerComp = 2; break;
        case 0x8363 /*GL_UNSIGNED_SHORT_5_6_5*/:
        case 0x8033 /*GL_UNSIGNED_SHORT_4_4_4_4*/:
        case 0x8034 /*GL_UNSIGNED_SHORT_5_5_5_1*/:
            // packed 16-bit types fold all components into 2 bytes/texel
            return (size_t)w * h * 2;
        case 0x8035 /*GL_UNSIGNED_INT_8_8_8_8*/:
        case 0x8368 /*GL_UNSIGNED_INT_8_8_8_8_REV*/:
            return (size_t)w * h * 4;
        case GL_UNSIGNED_INT: case GL_INT: case GL_FLOAT: bytesPerComp = 4; break;
        default: bytesPerComp = 1; break;
    }
    return (size_t)w * h * comps * bytesPerComp;
}
#endif // __EMSCRIPTEN__

} // namespace

U64 gl64Bridge(CPU64* cpu, U64 fnId, U64 argsAddr) {
    std::lock_guard<std::recursive_mutex> lk(g_glMutex);

    // Track the most recent fnIds (ring) so a nonzero glGetError can be annotated
    // with the GL call that likely produced it (see GL64_fn_glGetError). Cheap;
    // guarded by g_glMutex (held above).
    g_lastFnIdx = (g_lastFnIdx + 1) & 7;
    g_lastFnIds[g_lastFnIdx] = fnId;
    // BW64_GLTRACE=2 logs every trapped fnId (verbose). NB: getenv is unreliable on
    // guest worker threads in this pthread build (Module.ENV isn't always wired to
    // workers), so this can stay silent even when GL is flowing — it's a best-effort
    // trace, not a correctness signal.
    if (const char* gt = getenv("BW64_GLTRACE")) {
        if (gt[0] == '2')
            klog_fmt("gl64: call fnId=%llu", (unsigned long long)fnId);
    }
    GL64Args args = {};
    if (argsAddr) {
        cpu->memory->memcpyFromGuest(&args, argsAddr, sizeof(args));
    }

    switch (fnId) {
        // fnId 0 is the guest libGL's load-time witness (an __attribute__((constructor))
        // fires gl64_trap(0, NULL) the moment winex11/opengl32 dlopens libGL.so.1 — see
        // tools/rootfs64/libgl64/libgl64.c). It carries no args; just acknowledge it so
        // it doesn't fall to the "unimplemented fn id" default. Proves the guest is on
        // the gl64 bridge before any real GLX call.
        case 0:
            if (getenv("BW64_GLTRACE"))
                klog_fmt("gl64: FIRST trap fnId=0 (guest libGL.so.1 loaded)");
            return 0;

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
            // (config_or_vis, attribute, out value*). Report a real 32-bit RGBA,
            // depth-24, stencil-8, double-buffered, window+pbuffer config so
            // wined3d accepts the pixel format and uses the GL renderer (returning
            // a blanket 1 made it reject the format and fall back to GDI/software,
            // so D3D frames never reached our GL bridge).
            U32 attr = (U32)args.a[1];
            U64 valueAddr = args.a[2];
            U32 v;
            switch (attr) {
                case 1:  v = 1; break;   // GLX_USE_GL
                case 2:  v = 32; break;  // GLX_BUFFER_SIZE (total color bits)
                case 3:  v = 0; break;   // GLX_LEVEL
                case 4:  v = 1; break;   // GLX_RGBA (true)
                case 5:  v = 1; break;   // GLX_DOUBLEBUFFER (true)
                case 6:  v = 0; break;   // GLX_STEREO
                case 7:  v = 0; break;   // GLX_AUX_BUFFERS
                case 8:  v = 8; break;   // GLX_RED_SIZE
                case 9:  v = 8; break;   // GLX_GREEN_SIZE
                case 10: v = 8; break;   // GLX_BLUE_SIZE
                case 11: v = 8; break;   // GLX_ALPHA_SIZE
                case 12: v = 24; break;  // GLX_DEPTH_SIZE
                case 13: v = 8; break;   // GLX_STENCIL_SIZE
                case 14: case 15: case 16: case 17: v = 0; break; // GLX_ACCUM_*_SIZE
                case 0x20: v = 0; break;     // GLX_CONFIG_CAVEAT -> GLX_NONE (0x8000? wine maps 0=none)
                case 0x22: v = 0x8002; break;// GLX_X_VISUAL_TYPE -> GLX_TRUE_COLOR
                case 0x23: v = 0; break;     // GLX_TRANSPARENT_TYPE -> GLX_NONE
                case 0x800B: v = 0x21; break;// GLX_VISUAL_ID (our visual id)
                case 0x8010: v = 1|4; break; // GLX_DRAWABLE_TYPE -> WINDOW_BIT|PBUFFER_BIT
                case 0x8011: v = 1; break;   // GLX_RENDER_TYPE -> GLX_RGBA_BIT
                case 0x8012: v = 1; break;   // GLX_X_RENDERABLE (true)
                case 0x8013: v = 0x5100; break; // GLX_FBCONFIG_ID (opaque, stable)
                case 0x8016: v = 1; break;   // GLX_MAX_PBUFFER_WIDTH-ish (sane nonzero)
                case 0x186a3: v = 0; break;  // GLX_SAMPLE_BUFFERS_ARB
                case 0x186a4: v = 0; break;  // GLX_SAMPLES_ARB
                default: v = 0; break;
            }
            if (valueAddr) cpu->memory->writed(valueAddr, v);
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
            // winex11 calls glXMakeCurrent with a GLXWindow drawable whose XID is
            // distinct from the underlying X window (glXCreateWindow). Resolve it to
            // a real mapped window so the present sink can find it (otherwise wined3d's
            // windowed present has no window to blit/read back against).
            g_currentDrawable = XWireServer::instance().resolveGlxDrawable((U32)args.a[0]);
            // Claim the present slot for this GL app. Its readback frames are
            // tagged with this drawable id (not the X11 window it maps), so the
            // present sink's window filter needs to know the drawable belongs to
            // the foreground app — otherwise every cube frame is dropped and the
            // canvas stays stuck on the previous (GDI) app. A fresh glcube is
            // spawned on each switch-to-GL, so this re-claims correctly; switching
            // to a GDI app clears it again (see XWireServer GDI-adopt paths).
            XWireServer::instance().glPresentDrawable = g_currentDrawable;
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
            if (g_glContext) {
#ifdef __EMSCRIPTEN__
                // Bind our own offscreen FBO (the one with a DEPTH attachment)
                // before clearing, so the whole frame — clear, depth test, draw —
                // targets it. glcube does glClear first each frame, so this is the
                // per-frame entry point. Without it, after the first
                // readbackAndPresent / glemu shader flush leaves Emscripten's
                // renderViaOffscreenBackBuffer FBO bound, subsequent frames clear &
                // depth-test against a target with no usable depth buffer, so
                // GL_DEPTH_TEST silently does nothing and the cube's back faces
                // draw over the front ones — the "abstract overlapping triangles,
                // no occlusion" symptom. readbackAndPresent rebinds it for
                // glReadPixels anyway, so keeping it bound the whole frame is
                // consistent.
                GL_MT({ if (g_emFbo) glBindFramebuffer(GL_FRAMEBUFFER, g_emFbo);
                        glClear((GLbitfield)ai(args,0)); });
#else
                GL_MT(glClear((GLbitfield)ai(args,0)));
#endif
            }
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
            // Log the first handful of NONZERO GL errors with the recent fnId
            // context, so a CreateDevice cap failure (D3DERR_NOTAVAILABLE) that
            // stems from a GL error is visible. g_lastFnIds is a tiny ring updated
            // at the top of gl64Bridge.
            if (err) {
                static std::atomic<int> nlog{0};
                if (nlog.fetch_add(1) < 20)
                    klog_fmt("gl64 GLERR 0x%llx after recent fnIds [%llu %llu %llu %llu]",
                             (unsigned long long)err,
                             (unsigned long long)g_lastFnIds[(g_lastFnIdx+1)&7],
                             (unsigned long long)g_lastFnIds[(g_lastFnIdx+5)&7],
                             (unsigned long long)g_lastFnIds[(g_lastFnIdx+6)&7],
                             (unsigned long long)g_lastFnIds[(g_lastFnIdx+7)&7]);
            }
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
                GLenum pname = (GLenum)ai(args,0);
                GLint v[16] = {0};
                // Some desktop-GL pnames don't exist in WebGL2/GLES3 and make
                // glGetIntegerv raise GL_INVALID_ENUM (leaving v unchanged at 0).
                // wined3d reads these as caps; a 0 (or a leftover GL error it then
                // observes) can make it reject the device (D3DERR_NOTAVAILABLE).
                // Answer the known desktop-only ones with sane fixed values and
                // skip the GL call; for everything else, query then SWALLOW any
                // error so it can't pollute wined3d's own glGetError checks.
                bool handled = true;
                switch (pname) {
                    case 0x0D31 /*GL_MAX_LIGHTS*/:               v[0] = 8; break;
                    case 0x0D32 /*GL_MAX_CLIP_PLANES*/:          v[0] = 6; break;
                    case 0x0D50 /*GL_MAX_PIXEL_MAP_TABLE*/:      v[0] = 256; break;
                    case 0x0B31 /*GL_MAX_LIST_NESTING*/:         v[0] = 64; break;
                    case 0x8073 /*GL_MAX_3D_TEXTURE_SIZE*/:      v[0] = 2048; break;
                    case 0x84E2 /*GL_MAX_TEXTURE_UNITS (deprecated)*/: v[0] = 8; break;
                    case 0x80E9 /*GL_MAX_ELEMENTS_VERTICES*/:    v[0] = 65536; break;
                    case 0x80E8 /*GL_MAX_ELEMENTS_INDICES*/:     v[0] = 65536; break;
                    case 0x864B /*GL_TEXTURE_COMPRESSED?/desktop-only*/: v[0] = 0; break;
                    default: handled = false; break;
                }
                if (handled) { cpu->memory->memcpyToGuest(args.a[1], v, sizeof(GLint)); return 0; }
                GL_MT({ glGetError(); glGetIntegerv(pname, v); glGetError(); }); // read + swallow err
                // Some pnames return multiple ints (GL_VIEWPORT=4, GL_MAX_VIEWPORT_DIMS=2,
                // GL_SCISSOR_BOX=4). Write the right count so wined3d's caps reads are
                // complete (a 1-int write left stale guest memory for the rest).
                int count = 1;
                switch (pname) {
                    case 0x0BA2 /*GL_VIEWPORT*/: case 0x0C10 /*GL_SCISSOR_BOX*/:
                    case 0x0C22 /*GL_COLOR_WRITEMASK*/: count = 4; break;
                    case 0x0D3A /*GL_MAX_VIEWPORT_DIMS*/: case 0x846D /*GL_ALIASED_POINT_SIZE_RANGE*/:
                    case 0x846E /*GL_ALIASED_LINE_WIDTH_RANGE*/: count = 2; break;
                    default: count = 1; break;
                }
                cpu->memory->memcpyToGuest(args.a[1], v, sizeof(GLint) * count);
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
            if (g_glContext) RECORD_IMM(GL64_fn_glColor3f, af(args,0), af(args,1), af(args,2), 0,
                                        glColor3f(af(args,0), af(args,1), af(args,2)));
            return 0;
        case GL64_fn_glColor4f:
            if (g_glContext) RECORD_IMM(GL64_fn_glColor4f, af(args,0), af(args,1), af(args,2), af(args,3),
                                        glColor4f(af(args,0), af(args,1), af(args,2), af(args,3)));
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
            if (g_glContext) RECORD_IMM(GL64_fn_glNormal3f, af(args,0), af(args,1), af(args,2), 0,
                                        glNormal3f(af(args,0), af(args,1), af(args,2)));
            return 0;

        // === core GL: immediate-mode geometry ============================
        // On WASM, glBegin..glEnd is recorded host-side and replayed in ONE
        // glOnMain hop at glEnd (see g_immOps). Native runs each call inline.
        case GL64_fn_glBegin:
            if (g_glContext) {
#ifdef __EMSCRIPTEN__
                g_inImmediate = true;
                g_immMode = (GLenum)ai(args,0);
                g_immOps.clear();
#else
                glBegin((GLenum)ai(args,0));
#endif
            }
            return 0;
        case GL64_fn_glEnd:
            if (g_glContext) {
#ifdef __EMSCRIPTEN__
                // Single cross-thread hop: replay the whole primitive block.
                GLenum mode = g_immMode;
                glOnMain([&]{
                    // Keep drawing targeted at our depth-equipped offscreen FBO.
                    // glemu's LEGACY_GL_EMULATION shader flush (and Emscripten's
                    // offscreen-backbuffer machinery) can rebind the framebuffer
                    // between glClear and here, so re-bind before each primitive
                    // block or the depth test runs against the wrong target and
                    // the cube loses hidden-surface removal.
                    if (g_emFbo) glBindFramebuffer(GL_FRAMEBUFFER, g_emFbo);
                    // GL_QUADS needs an index buffer at the point of consumption.
                    // glemu's flush triangulates a GL_QUADS (mode 7) block by binding
                    // GL.currentContext.tempQuadIndexBuffer (a precomputed
                    // 0 1 2, 0 2 3, 4 5 6,… index buffer) and drawElements(TRIANGLES).
                    // Our bridge's context was created via emscripten_webgl_create_context
                    // (generateTempBuffers(false) path), so that buffer is NOT created at
                    // init (confirmed: the init rc never set the quad-buffer bit). Without
                    // it glemu binds element-array 0 → garbage indices. Build it once here,
                    // on the exact GL.currentContext glemu reads, the first time we draw.
                    if (mode == GL_QUADS) {
                        EM_ASM({
                            try {
                                if (typeof GL === 'undefined' || !GL.currentContext) return;
                                if (GL.currentContext.tempQuadIndexBuffer) return;
                                if (typeof GLctx === 'undefined' || !GLctx.createBuffer ||
                                    !GL.MAX_TEMP_BUFFER_SIZE) return;
                                var qb = GLctx.createBuffer();
                                var saved = GLctx.getParameter(GLctx.ELEMENT_ARRAY_BUFFER_BINDING);
                                GLctx.bindBuffer(GLctx.ELEMENT_ARRAY_BUFFER, qb);
                                var n = GL.MAX_TEMP_BUFFER_SIZE >> 1;
                                var qa = new Uint16Array(n);
                                var qi = 0;
                                var qv = 0;
                                while (1) {
                                    qa[qi++] = qv;     if (qi >= n) break;
                                    qa[qi++] = qv + 1; if (qi >= n) break;
                                    qa[qi++] = qv + 2; if (qi >= n) break;
                                    qa[qi++] = qv;     if (qi >= n) break;
                                    qa[qi++] = qv + 2; if (qi >= n) break;
                                    qa[qi++] = qv + 3; if (qi >= n) break;
                                    qv += 4;
                                }
                                GLctx.bufferData(GLctx.ELEMENT_ARRAY_BUFFER, qa, GLctx.STATIC_DRAW);
                                GLctx.bindBuffer(GLctx.ELEMENT_ARRAY_BUFFER, saved || null);
                                GL.currentContext.tempQuadIndexBuffer = qb;
                            } catch (e) {}
                        });
                    }
                    glBegin(mode);
                    replayImmBlock(g_immOps);
                    glEnd();
                    // TEMP one-shot depth diagnostic (gated on BW64_GLTRACE): report
                    // the actual depth state on the bound FBO at draw time so we can
                    // see WHY hidden-surface removal isn't happening.
                    if (getenv("BW64_GLTRACE")) {
                        static bool reported = false;
                        if (!reported) {
                            reported = true;
                            GLint fbo = -1, dtype = -1, dname = -1, dbits = -1;
                            GLboolean dtest = glIsEnabled(GL_DEPTH_TEST);
                            GLboolean dmask = GL_FALSE;
                            glGetBooleanv(GL_DEPTH_WRITEMASK, &dmask);
                            GLint dfunc = 0; glGetIntegerv(GL_DEPTH_FUNC, &dfunc);
                            glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
                            GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
                            glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER,
                                GL_DEPTH_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &dtype);
                            glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER,
                                GL_DEPTH_ATTACHMENT, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &dname);
                            glGetIntegerv(GL_DEPTH_BITS, &dbits);
                            GLenum err = glGetError();
                            klog_fmt("gl64 DEPTHDBG: boundFbo=%d g_emFbo=%u status=0x%x "
                                     "DEPTH_TEST=%d writemask=%d func=0x%x depthAttachType=0x%x "
                                     "depthAttachName=%d DEPTH_BITS=%d glErr=0x%x",
                                     fbo, (unsigned)g_emFbo, st, (int)dtest, (int)dmask, dfunc,
                                     dtype, dname, dbits, err);
                            // Dump glemu's JS-side matrices. The native cube is
                            // perfect, so the WASM-only suspects are (a) glemu's
                            // matrix tracking and (b) the immediate-mode replay. The
                            // pinwheel looks like the modelview translate (0,0,-6)
                            // isn't applied. mv = GLImmediate.matrix[0] (MODELVIEW),
                            // proj = matrix[1] (PROJECTION). In column-major, a
                            // translate(0,0,-6) puts -6 at index 14. Report key cells
                            // + the active matrix mode so we can see what glemu has.
                            // NB: EM_ASM passes its body through the C preprocessor,
                            // which splits on UNPARENTHESIZED commas — so no `var a,b;`
                            // and no bare `[a,b]` at statement level. Keep commas
                            // inside (), and one declaration per statement.
                            EM_ASM({
                                try {
                                    // GL_QUADS triangulation buffer check: if this is
                                    // undefined while glcube draws GL_QUADS (mode 7),
                                    // glemu binds element-array 0 and the cube comes out
                                    // as scrambled rotating triangles.
                                    if (typeof GL !== 'undefined' && GL.currentContext)
                                        console.log('gl64 QUADDBG: tempQuadIndexBuffer=' +
                                            (GL.currentContext.tempQuadIndexBuffer ? 'present' : 'MISSING') +
                                            ' immMode=' + (typeof GLImmediate !== 'undefined' ? GLImmediate.mode : 'NA'));
                                    if (typeof GLImmediate === 'undefined') { console.log('gl64 MTXDBG: no GLImmediate'); return; }
                                    var m = GLImmediate.matrix;
                                    var mm = GLImmediate.currentMatrix;
                                    var f = function(a, i){ return (a && a[i] !== undefined) ? Number(a[i]).toFixed(3) : 'NA'; };
                                    var mv = m ? m[0] : null;
                                    var pr = m ? m[1] : null;
                                    var s = 'gl64 MTXDBG: currentMatrix=' + mm + ' nMatrices=' + (m ? m.length : 'NA');
                                    s += ' MV[0/5/10/12/13/14/15]=' + f(mv,0) + '/' + f(mv,5) + '/' + f(mv,10) + '/' + f(mv,12) + '/' + f(mv,13) + '/' + f(mv,14) + '/' + f(mv,15);
                                    s += ' PROJ[0/5/10/11/14/15]=' + f(pr,0) + '/' + f(pr,5) + '/' + f(pr,10) + '/' + f(pr,11) + '/' + f(pr,14) + '/' + f(pr,15);
                                    console.log(s);
                                } catch(e){ console.log('gl64 MTXDBG err: ' + e); }
                            });
                        }
                    }
                });
                g_inImmediate = false;
                g_immOps.clear();
#else
                glEnd();
#endif
            }
            return 0;
        case GL64_fn_glVertex2f:
            if (g_glContext) RECORD_IMM(GL64_fn_glVertex2f, af(args,0), af(args,1), 0, 0,
                                        glVertex2f(af(args,0), af(args,1)));
            return 0;
        case GL64_fn_glVertex3f:
            if (g_glContext) RECORD_IMM(GL64_fn_glVertex3f, af(args,0), af(args,1), af(args,2), 0,
                                        glVertex3f(af(args,0), af(args,1), af(args,2)));
            return 0;

        // =================================================================
        // Programmable pipeline (GL2/GLES3) — wined3d / Direct3D path.
        // These map 1:1 onto WebGL2 on the host. Guest-memory reads happen on
        // THIS (calling) thread into host buffers; only the gl* call hops to
        // the GL-owning thread via GL_MT. Buffer-relative pointers
        // (VertexAttribPointer/DrawElements) are integer offsets into the bound
        // buffer — passed through, never dereferenced as guest memory.
        // =================================================================
#ifdef __EMSCRIPTEN__
        case GL64_fn_traceProc: {
            // args[0]=guest name*, args[1]=hit(1)/miss(0). Log under BW64_GLTRACE
            // so we can see exactly which GL entry points wined3d resolves and
            // which ones we don't implement yet (the D3D worklist).
            if (const char* gt = getenv("BW64_GLTRACE")) {
                char name[64] = {0};
                if (args.a[0]) cpu->memory->memcpyFromGuest(name, args.a[0], sizeof(name)-1);
                // gt=="3" → only log misses (the worklist); else log everything.
                bool hit = args.a[1] != 0;
                if (gt[0] != '3' || !hit)
                    klog_fmt("gl64 PROC %s %s", hit ? "HIT " : "MISS", name);
            }
            return 0;
        }

        // --- shaders / programs ---
        case GL64_fn_glCreateShader: {
            if (!ensureContext()) return 0;
            U64 ret = 0; GLenum sty = (GLenum)ai(args,0);
            GL_MT(ret = glCreateShader(sty));
            return ret;
        }
        case GL64_fn_glShaderSource: {
            if (!ensureContext()) return 0;
            GLsizei count = (GLsizei)ai(args,1);
            if (count <= 0 || count > 256) return 0;
            // Read the guest string-pointer array and optional length array, then
            // each guest source string, into host-owned storage.
            std::vector<U64> guestStrPtrs(count);
            cpu->memory->memcpyFromGuest(guestStrPtrs.data(), args.a[2], (U64)count * 8);
            std::vector<S32> guestLens;
            bool haveLens = args.a[3] != 0;
            if (haveLens) {
                guestLens.resize(count);
                cpu->memory->memcpyFromGuest(guestLens.data(), args.a[3], (U64)count * 4);
            }
            std::vector<std::string> srcs(count);
            std::vector<const char*> srcPtrs(count);
            std::vector<GLint>        srcLens(count);
            for (GLsizei i = 0; i < count; i++) {
                std::string& s = srcs[i];
                if (haveLens && guestLens[i] >= 0) {
                    s.resize(guestLens[i]);
                    if (guestLens[i] > 0 && guestStrPtrs[i])
                        cpu->memory->memcpyFromGuest(&s[0], guestStrPtrs[i], (U64)guestLens[i]);
                } else {
                    // NUL-terminated: read the guest C string.
                    s = guestStrPtrs[i] ? readGuestCStr(cpu, guestStrPtrs[i], 1 << 20) : std::string();
                }
                srcPtrs[i] = s.c_str();
                srcLens[i] = (GLint)s.size();
            }
            // Concatenate the source, then translate the legacy desktop-GLSL wined3d
            // emits (#version 120, attribute/varying, gl_FragData/texture2D) into the
            // GLSL ES 3.00 dialect WebGL2 accepts. WebGL2 rejects the raw source
            // outright (`'120': client/version number not supported`), so without
            // this the program never links and the D3D draw produces nothing.
            std::string joined;
            for (GLsizei i = 0; i < count; i++) joined += srcs[i];
            // Detect stage from content (no per-handle state needed): a fragment
            // shader writes gl_FragData/gl_FragColor/gl_FragDepth; a vertex shader
            // writes gl_Position.
            bool isFrag = joined.find("gl_FragData") != std::string::npos ||
                          joined.find("gl_FragColor") != std::string::npos ||
                          joined.find("gl_FragDepth") != std::string::npos ||
                          (joined.find("gl_Position") == std::string::npos &&
                           joined.find("gl_PointSize") == std::string::npos);
            std::string translated = translateGlslToEs300(joined, isFrag);
            const char* tptr = translated.c_str();
            GLint tlen = (GLint)translated.size();
            GLuint sh = (GLuint)ai(args,0);
            GL_MT(glShaderSource(sh, 1, &tptr, &tlen));
            return 0;
        }
        case GL64_fn_glCompileShader:
            if (g_glContext) {
                GLuint sh = (GLuint)ai(args,0);
                glOnMain([&]{
                    // Safety: never call glCompileShader on a handle Emscripten doesn't
                    // have in GL.shaders — that throws a JS TypeError that kills the
                    // worker. With glDeleteShader deferred this shouldn't happen, but
                    // guard unconditionally so a stray case degrades, not crashes.
                    int present = EM_ASM_INT({
                        try { return (typeof GL!=='undefined' && GL.shaders && GL.shaders[$0]) ? 1 : 0; }
                        catch(e){ return 0; }
                    }, (int)sh);
                    if (!present) { klog_fmt("gl64: glCompileShader(%u) — handle not in GL.shaders, skipped", sh); return; }
                    glCompileShader(sh);
                    GLint ok = 0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
                    if (!ok) {
                        char log[1024] = {0}; GLsizei n = 0;
                        glGetShaderInfoLog(sh, sizeof(log)-1, &n, log);
                        klog_fmt("gl64 SHADER COMPILE FAIL (sh=%u): %s", sh, log);
                    }
                });
            }
            return 0;
        case GL64_fn_glGetShaderiv: {
            if (g_glContext && args.a[2]) {
                GLint v = 0; GLuint sh=(GLuint)ai(args,0); GLenum pn=(GLenum)ai(args,1);
                GL_MT(glGetShaderiv(sh, pn, &v));
                cpu->memory->writed(args.a[2], (U32)v);
            }
            return 0;
        }
        case GL64_fn_glGetShaderInfoLog: {
            if (g_glContext) writeInfoLog(cpu, args, /*isProgram*/false);
            return 0;
        }
        case GL64_fn_glDeleteShader:
            // DEFER (no-op): wined3d does create→source→compile→attach→glDeleteShader,
            // but its GL calls reach this bridge slightly reordered across the
            // trap/main-thread hops, so the delete can land BEFORE the matching
            // glCompileShader. Emscripten's glDeleteShader nulls GL.shaders[id], which
            // then makes the deferred glCompileShader(id) throw "parameter 1 is not a
            // WebGLShader" and the whole program fails to link (the D3D triangle never
            // renders). A deleted-but-attached shader is retained by GL until the
            // program links anyway, so skipping the delete is safe; the only cost is a
            // small, bounded shader leak (wined3d creates few shaders). This is THE fix
            // that lets wined3d's GLSL programs compile+link under our bridge.
            return 0;
        case GL64_fn_glCreateProgram: {
            if (!ensureContext()) return 0;
            U64 ret = 0; GL_MT(ret = glCreateProgram());
            return ret;
        }
        case GL64_fn_glAttachShader:
            if (g_glContext) GL_MT(glAttachShader((GLuint)ai(args,0), (GLuint)ai(args,1)));
            return 0;
        case GL64_fn_glDetachShader:
            if (g_glContext) GL_MT(glDetachShader((GLuint)ai(args,0), (GLuint)ai(args,1)));
            return 0;
        case GL64_fn_glBindAttribLocation: {
            if (g_glContext) {
                std::string nm = readGuestCStr(cpu, args.a[2], 256);
                GLuint p=(GLuint)ai(args,0); GLuint idx=(GLuint)ai(args,1);
                const char* c = nm.c_str();
                GL_MT(glBindAttribLocation(p, idx, c));
            }
            return 0;
        }
        case GL64_fn_glLinkProgram:
            if (g_glContext) {
                GLuint pr = (GLuint)ai(args,0);
                glOnMain([&]{
                    glLinkProgram(pr);
                    GLint ok = 0; glGetProgramiv(pr, GL_LINK_STATUS, &ok);
                    if (!ok) {
                        char log[1024] = {0}; GLsizei n = 0;
                        glGetProgramInfoLog(pr, sizeof(log)-1, &n, log);
                        klog_fmt("gl64 PROGRAM LINK FAIL (pr=%u): %s", pr, log);
                    }
                });
            }
            return 0;
        case GL64_fn_glGetProgramiv: {
            if (g_glContext && args.a[2]) {
                GLint v = 0; GLuint p=(GLuint)ai(args,0); GLenum pn=(GLenum)ai(args,1);
                GL_MT(glGetProgramiv(p, pn, &v));
                cpu->memory->writed(args.a[2], (U32)v);
            }
            return 0;
        }
        case GL64_fn_glGetProgramInfoLog: {
            if (g_glContext) writeInfoLog(cpu, args, /*isProgram*/true);
            return 0;
        }
        case GL64_fn_glUseProgram:
            if (g_glContext) { GLuint p=(GLuint)ai(args,0); GL_MT(glUseProgram(p)); }
            return 0;
        case GL64_fn_glDeleteProgram:
            // DEFER (no-op), same reasoning as glDeleteShader: the delete can reach
            // this bridge before a still-pending glUseProgram/glGetUniformLocation on
            // the same handle (reordered across trap/main-thread hops), which would
            // then hit a nulled GL.programs[id]. Bounded leak; wined3d makes few
            // programs. Keeps the program object alive for the whole app.
            return 0;
        case GL64_fn_glGetUniformLocation: {
            if (!g_glContext) return (U64)(S64)-1;
            std::string nm = readGuestCStr(cpu, args.a[1], 256);
            GLuint p=(GLuint)ai(args,0); const char* c = nm.c_str();
            S64 loc = -1; GL_MT(loc = glGetUniformLocation(p, c));
            return (U64)loc;
        }
        case GL64_fn_glGetAttribLocation: {
            if (!g_glContext) return (U64)(S64)-1;
            std::string nm = readGuestCStr(cpu, args.a[1], 256);
            GLuint p=(GLuint)ai(args,0); const char* c = nm.c_str();
            S64 loc = -1; GL_MT(loc = glGetAttribLocation(p, c));
            return (U64)loc;
        }
        case GL64_fn_glValidateProgram:
            if (g_glContext) GL_MT(glValidateProgram((GLuint)ai(args,0)));
            return 0;

        // --- uniforms ---
        case GL64_fn_glUniform1i:
            if (g_glContext) GL_MT(glUniform1i((GLint)ai(args,0), (GLint)ai(args,1)));
            return 0;
        case GL64_fn_glUniform1f:
            if (g_glContext) GL_MT(glUniform1f((GLint)ai(args,0), af(args,1)));
            return 0;
        case GL64_fn_glUniform2f:
            if (g_glContext) GL_MT(glUniform2f((GLint)ai(args,0), af(args,1), af(args,2)));
            return 0;
        case GL64_fn_glUniform3f:
            if (g_glContext) GL_MT(glUniform3f((GLint)ai(args,0), af(args,1), af(args,2), af(args,3)));
            return 0;
        case GL64_fn_glUniform4f:
            if (g_glContext) GL_MT(glUniform4f((GLint)ai(args,0), af(args,1), af(args,2), af(args,3), af(args,4)));
            return 0;
        case GL64_fn_glUniform1fv: case GL64_fn_glUniform2fv:
        case GL64_fn_glUniform3fv: case GL64_fn_glUniform4fv: {
            if (g_glContext && args.a[2]) {
                int comps = fnId==GL64_fn_glUniform1fv?1 : fnId==GL64_fn_glUniform2fv?2 :
                            fnId==GL64_fn_glUniform3fv?3 : 4;
                GLsizei n = (GLsizei)ai(args,1);
                if (n > 0 && n <= 4096) {
                    std::vector<float> v((size_t)n * comps);
                    cpu->memory->memcpyFromGuest(v.data(), args.a[2], (U64)v.size()*4);
                    GLint loc=(GLint)ai(args,0); const float* p=v.data();
                    switch (comps) {
                        case 1: GL_MT(glUniform1fv(loc, n, p)); break;
                        case 2: GL_MT(glUniform2fv(loc, n, p)); break;
                        case 3: GL_MT(glUniform3fv(loc, n, p)); break;
                        default: GL_MT(glUniform4fv(loc, n, p)); break;
                    }
                }
            }
            return 0;
        }
        case GL64_fn_glUniform1iv: {
            if (g_glContext && args.a[2]) {
                GLsizei n = (GLsizei)ai(args,1);
                if (n > 0 && n <= 4096) {
                    std::vector<GLint> v(n);
                    cpu->memory->memcpyFromGuest(v.data(), args.a[2], (U64)n*4);
                    GLint loc=(GLint)ai(args,0); const GLint* p=v.data();
                    GL_MT(glUniform1iv(loc, n, p));
                }
            }
            return 0;
        }
        case GL64_fn_glUniformMatrix2fv:
        case GL64_fn_glUniformMatrix3fv:
        case GL64_fn_glUniformMatrix4fv: {
            if (g_glContext && args.a[3]) {
                int dim = fnId==GL64_fn_glUniformMatrix2fv?2 : fnId==GL64_fn_glUniformMatrix3fv?3 : 4;
                GLsizei n = (GLsizei)ai(args,1);
                if (n > 0 && n <= 1024) {
                    std::vector<float> v((size_t)n * dim * dim);
                    cpu->memory->memcpyFromGuest(v.data(), args.a[3], (U64)v.size()*4);
                    GLint loc=(GLint)ai(args,0); GLboolean tr=(GLboolean)ai(args,2); const float* p=v.data();
                    switch (dim) {
                        case 2: GL_MT(glUniformMatrix2fv(loc, n, tr, p)); break;
                        case 3: GL_MT(glUniformMatrix3fv(loc, n, tr, p)); break;
                        default: GL_MT(glUniformMatrix4fv(loc, n, tr, p)); break;
                    }
                }
            }
            return 0;
        }

        // --- buffers ---
        case GL64_fn_glGenBuffers: {
            if (g_glContext && args.a[1]) genObjects(cpu, args, GenKind::Buffer);
            return 0;
        }
        case GL64_fn_glBindBuffer:
            if (g_glContext) GL_MT(glBindBuffer((GLenum)ai(args,0), (GLuint)ai(args,1)));
            return 0;
        case GL64_fn_glBufferData: {
            if (g_glContext) {
                GLsizeiptr size = (GLsizeiptr)args.a[1];
                std::vector<U8> data;
                const void* dp = nullptr;
                if (args.a[2] && size > 0) {
                    data.resize((size_t)size);
                    cpu->memory->memcpyFromGuest(data.data(), args.a[2], (U64)size);
                    dp = data.data();
                }
                GLenum tgt=(GLenum)ai(args,0); GLenum usage=(GLenum)ai(args,3);
                GL_MT(glBufferData(tgt, size, dp, usage));
            }
            return 0;
        }
        case GL64_fn_glBufferSubData: {
            if (g_glContext && args.a[3]) {
                GLsizeiptr size = (GLsizeiptr)args.a[2];
                if (size > 0) {
                    std::vector<U8> data((size_t)size);
                    cpu->memory->memcpyFromGuest(data.data(), args.a[3], (U64)size);
                    GLenum tgt=(GLenum)ai(args,0); GLintptr off=(GLintptr)args.a[1]; const void* dp=data.data();
                    GL_MT(glBufferSubData(tgt, off, size, dp));
                }
            }
            return 0;
        }
        case GL64_fn_glDeleteBuffers:
            if (g_glContext && args.a[1]) deleteObjects(cpu, args, GenKind::Buffer);
            return 0;
        case GL64_fn_glMapBufferRange:
            return 0; // unsupported in WebGL2; guest wrapper already returns 0

        // --- vertex attrib arrays / VAO ---
        case GL64_fn_glEnableVertexAttribArray:
            if (g_glContext) GL_MT(glEnableVertexAttribArray((GLuint)ai(args,0)));
            return 0;
        case GL64_fn_glDisableVertexAttribArray:
            if (g_glContext) GL_MT(glDisableVertexAttribArray((GLuint)ai(args,0)));
            return 0;
        case GL64_fn_glVertexAttribPointer: {
            // pointer (a[5]) is a byte offset into the bound ARRAY_BUFFER.
            if (g_glContext) {
                GLuint idx=(GLuint)ai(args,0); GLint sz=(GLint)ai(args,1); GLenum ty=(GLenum)ai(args,2);
                GLboolean nm=(GLboolean)ai(args,3); GLsizei st=(GLsizei)ai(args,4);
                const void* off = (const void*)(uintptr_t)args.a[5];
                GL_MT(glVertexAttribPointer(idx, sz, ty, nm, st, off));
            }
            return 0;
        }
        case GL64_fn_glGenVertexArrays:
            if (g_glContext && args.a[1]) genObjects(cpu, args, GenKind::VertexArray);
            return 0;
        case GL64_fn_glBindVertexArray:
            if (g_glContext) GL_MT(glBindVertexArray((GLuint)ai(args,0)));
            return 0;
        case GL64_fn_glDeleteVertexArrays:
            if (g_glContext && args.a[1]) deleteObjects(cpu, args, GenKind::VertexArray);
            return 0;
        case GL64_fn_glVertexAttrib4f:
            if (g_glContext) GL_MT(glVertexAttrib4f((GLuint)ai(args,0), af(args,1), af(args,2), af(args,3), af(args,4)));
            return 0;

        // --- draws --- (bind our depth-equipped offscreen FBO first, like glEnd)
        case GL64_fn_glDrawArrays:
            if (g_glContext) {
                GLenum mode=(GLenum)ai(args,0); GLint first=(GLint)ai(args,1); GLsizei count=(GLsizei)ai(args,2);
                glOnMain([&]{ if (g_emFbo) glBindFramebuffer(GL_FRAMEBUFFER, g_emFbo);
                              glDrawArrays(mode, first, count); });
                g_glDrew = true;
            }
            return 0;
        case GL64_fn_glDrawElements:
            if (g_glContext) {
                GLenum mode=(GLenum)ai(args,0); GLsizei count=(GLsizei)ai(args,1); GLenum type=(GLenum)ai(args,2);
                const void* indices = (const void*)(uintptr_t)args.a[3];
                static std::atomic<bool> once{false};
                if (!once.exchange(true)) klog_fmt("gl64 DRAW: first glDrawElements mode=0x%x count=%d", mode, count);
                glOnMain([&]{ if (g_emFbo) glBindFramebuffer(GL_FRAMEBUFFER, g_emFbo);
                              glDrawElements(mode, count, type, indices); });
                g_glDrew = true;
            }
            return 0;
        case GL64_fn_glDrawRangeElements:
            if (g_glContext) {
                GLenum mode=(GLenum)ai(args,0); GLuint start=(GLuint)ai(args,1); GLuint end=(GLuint)ai(args,2);
                GLsizei count=(GLsizei)ai(args,3); GLenum type=(GLenum)ai(args,4);
                const void* indices = (const void*)(uintptr_t)args.a[5];
                glOnMain([&]{ if (g_emFbo) glBindFramebuffer(GL_FRAMEBUFFER, g_emFbo);
                              glDrawRangeElements(mode, start, end, count, type, indices); });
                g_glDrew = true;
            }
            return 0;

        // --- modern state ---
        case GL64_fn_glBlendFunc:
            if (g_glContext) GL_MT(glBlendFunc((GLenum)ai(args,0), (GLenum)ai(args,1)));
            return 0;
        case GL64_fn_glBlendFuncSeparate:
            if (g_glContext) GL_MT(glBlendFuncSeparate((GLenum)ai(args,0),(GLenum)ai(args,1),(GLenum)ai(args,2),(GLenum)ai(args,3)));
            return 0;
        case GL64_fn_glBlendEquation:
            if (g_glContext) GL_MT(glBlendEquation((GLenum)ai(args,0)));
            return 0;
        case GL64_fn_glBlendEquationSeparate:
            if (g_glContext) GL_MT(glBlendEquationSeparate((GLenum)ai(args,0),(GLenum)ai(args,1)));
            return 0;
        case GL64_fn_glBlendColor:
            if (g_glContext) GL_MT(glBlendColor(af(args,0),af(args,1),af(args,2),af(args,3)));
            return 0;
        case GL64_fn_glColorMask:
            if (g_glContext) GL_MT(glColorMask((GLboolean)ai(args,0),(GLboolean)ai(args,1),(GLboolean)ai(args,2),(GLboolean)ai(args,3)));
            return 0;
        case GL64_fn_glDepthMask:
            if (g_glContext) GL_MT(glDepthMask((GLboolean)ai(args,0)));
            return 0;
        case GL64_fn_glStencilFunc:
            if (g_glContext) GL_MT(glStencilFunc((GLenum)ai(args,0),(GLint)ai(args,1),(GLuint)ai(args,2)));
            return 0;
        case GL64_fn_glStencilOp:
            if (g_glContext) GL_MT(glStencilOp((GLenum)ai(args,0),(GLenum)ai(args,1),(GLenum)ai(args,2)));
            return 0;
        case GL64_fn_glStencilMask:
            if (g_glContext) GL_MT(glStencilMask((GLuint)ai(args,0)));
            return 0;
        case GL64_fn_glStencilFuncSeparate:
            if (g_glContext) GL_MT(glStencilFuncSeparate((GLenum)ai(args,0),(GLenum)ai(args,1),(GLint)ai(args,2),(GLuint)ai(args,3)));
            return 0;
        case GL64_fn_glStencilOpSeparate:
            if (g_glContext) GL_MT(glStencilOpSeparate((GLenum)ai(args,0),(GLenum)ai(args,1),(GLenum)ai(args,2),(GLenum)ai(args,3)));
            return 0;
        case GL64_fn_glStencilMaskSeparate:
            if (g_glContext) GL_MT(glStencilMaskSeparate((GLenum)ai(args,0),(GLuint)ai(args,1)));
            return 0;
        case GL64_fn_glScissor:
            if (g_glContext) GL_MT(glScissor((GLint)ai(args,0),(GLint)ai(args,1),(GLsizei)ai(args,2),(GLsizei)ai(args,3)));
            return 0;
        case GL64_fn_glPolygonOffset:
            if (g_glContext) GL_MT(glPolygonOffset(af(args,0), af(args,1)));
            return 0;
        case GL64_fn_glPolygonMode:
            return 0; // no glPolygonMode in GLES — wireframe not supported, ignore
        case GL64_fn_glDepthRange:
            if (g_glContext) { float n=(float)ad(args,0), f=(float)ad(args,1); GL_MT(glDepthRangef(n, f)); }
            return 0;
        case GL64_fn_glLineWidth:
            if (g_glContext) GL_MT(glLineWidth(af(args,0)));
            return 0;
        case GL64_fn_glPixelStorei:
            if (g_glContext) GL_MT(glPixelStorei((GLenum)ai(args,0), (GLint)ai(args,1)));
            return 0;
        case GL64_fn_glSampleCoverage:
            if (g_glContext) GL_MT(glSampleCoverage(af(args,0), (GLboolean)ai(args,1)));
            return 0;

        // --- textures (modern) ---
        case GL64_fn_glActiveTexture:
            if (g_glContext) GL_MT(glActiveTexture((GLenum)ai(args,0)));
            return 0;
        case GL64_fn_glGenTextures:
            if (g_glContext && args.a[1]) genObjects(cpu, args, GenKind::Texture);
            return 0;
        case GL64_fn_glBindTexture:
            if (g_glContext) GL_MT(glBindTexture((GLenum)ai(args,0), (GLuint)ai(args,1)));
            return 0;
        case GL64_fn_glDeleteTextures:
            if (g_glContext && args.a[1]) deleteObjects(cpu, args, GenKind::Texture);
            return 0;
        // glTexParameter*: WebGL2 accepts only a subset of pnames; wined3d sets
        // several desktop-GL-only ones (GL_TEXTURE_LOD_BIAS, GL_GENERATE_MIPMAP,
        // GL_TEXTURE_BORDER_COLOR, GL_DEPTH_TEXTURE_MODE, …) that raise
        // GL_INVALID_ENUM. Drop those (return success) so they don't pollute the GL
        // error state wined3d inspects during CreateDevice.
        case GL64_fn_glTexParameteri:
            if (g_glContext) {
                GLenum pn = (GLenum)ai(args,1);
                if (texParamSupported(pn)) GL_MT(glTexParameteri((GLenum)ai(args,0), pn, (GLint)ai(args,2)));
            }
            return 0;
        case GL64_fn_glTexParameterf:
            if (g_glContext) {
                GLenum pn = (GLenum)ai(args,1);
                if (texParamSupported(pn)) GL_MT(glTexParameterf((GLenum)ai(args,0), pn, af(args,2)));
            }
            return 0;
        case GL64_fn_glTexImage2D: {
            if (g_glContext) {
                GLenum tgt=(GLenum)ai(args,0); GLint lvl=(GLint)ai(args,1); GLint ifmt=(GLint)ai(args,2);
                GLsizei w=(GLsizei)ai(args,3); GLsizei h=(GLsizei)ai(args,4); GLint bd=(GLint)ai(args,5);
                GLenum fmt=(GLenum)ai(args,6); GLenum type=(GLenum)ai(args,7);
                std::vector<U8> pix; const void* pp=nullptr;
                if (args.a[8]) {
                    size_t bytes = texImageBytes(w, h, fmt, type);
                    if (bytes) { pix.resize(bytes); cpu->memory->memcpyFromGuest(pix.data(), args.a[8], (U64)bytes); pp=pix.data(); }
                }
                GL_MT(glTexImage2D(tgt, lvl, ifmt, w, h, bd, fmt, type, pp));
            }
            return 0;
        }
        case GL64_fn_glTexSubImage2D: {
            if (g_glContext && args.a[8]) {
                GLenum tgt=(GLenum)ai(args,0); GLint lvl=(GLint)ai(args,1); GLint x=(GLint)ai(args,2); GLint y=(GLint)ai(args,3);
                GLsizei w=(GLsizei)ai(args,4); GLsizei h=(GLsizei)ai(args,5); GLenum fmt=(GLenum)ai(args,6); GLenum type=(GLenum)ai(args,7);
                size_t bytes = texImageBytes(w, h, fmt, type);
                if (bytes) {
                    std::vector<U8> pix(bytes); cpu->memory->memcpyFromGuest(pix.data(), args.a[8], (U64)bytes);
                    const void* pp=pix.data();
                    GL_MT(glTexSubImage2D(tgt, lvl, x, y, w, h, fmt, type, pp));
                }
            }
            return 0;
        }
        case GL64_fn_glGenerateMipmap:
            if (g_glContext) GL_MT(glGenerateMipmap((GLenum)ai(args,0)));
            return 0;
        case GL64_fn_glCompressedTexImage2D: {
            if (g_glContext) {
                GLenum tgt=(GLenum)ai(args,0); GLint lvl=(GLint)ai(args,1); GLenum ifmt=(GLenum)ai(args,2);
                GLsizei w=(GLsizei)ai(args,3); GLsizei h=(GLsizei)ai(args,4); GLint bd=(GLint)ai(args,5);
                GLsizei isz=(GLsizei)ai(args,6);
                std::vector<U8> data; const void* dp=nullptr;
                if (args.a[7] && isz>0) { data.resize((size_t)isz); cpu->memory->memcpyFromGuest(data.data(), args.a[7], (U64)isz); dp=data.data(); }
                GL_MT(glCompressedTexImage2D(tgt, lvl, ifmt, w, h, bd, isz, dp));
            }
            return 0;
        }
        case GL64_fn_glGetStringi:
            return 0; // guest wrapper returns "" itself
        case GL64_fn_glGetShaderSource:
            return 0; // not needed

        // === ARB_sync ====================================================
        // wined3d's Present/flush issues a fence then polls glClientWaitSync
        // until it reports signaled. All our GL work runs synchronously on the
        // main thread via glOnMain, so by the time the guest polls, the work is
        // already done — report ALREADY_SIGNALED so the wait loop exits at once.
        // We still create a REAL GLsync (so glGetSynciv/glIsSync are coherent),
        // but never actually block.
        case GL64_fn_glFenceSync: {
            U64 ret = 0;
            if (g_glContext) {
                GLenum cond = (GLenum)ai(args,0); GLbitfield fl = (GLbitfield)ai(args,1);
                GLsync s = 0;
                GL_MT(s = glFenceSync(cond ? cond : GL_SYNC_GPU_COMMANDS_COMPLETE, fl));
                ret = (U64)(uintptr_t)s;
            }
            // Never hand back 0 (guest treats null GLsync as failure → may spin).
            if (!ret) ret = 0x1; // opaque non-null sentinel
            return ret;
        }
        case GL64_fn_glClientWaitSync: {
            // (sync, flags, timeout) -> GLenum. Always signaled for us.
            if (g_glContext) {
                GLsync s = (GLsync)(uintptr_t)ai(args,0);
                // Flush so any pending commands are submitted, then report done.
                if (s && (U64)(uintptr_t)s != 0x1) GL_MT(glFlush());
            }
            return 0x911A; // GL_ALREADY_SIGNALED
        }
        case GL64_fn_glWaitSync:
            return 0; // server-side wait — nothing to do, never blocks
        case GL64_fn_glDeleteSync: {
            if (g_glContext) {
                GLsync s = (GLsync)(uintptr_t)ai(args,0);
                if (s && (U64)(uintptr_t)s != 0x1) GL_MT(glDeleteSync(s));
            }
            return 0;
        }
        case GL64_fn_glIsSync: {
            GLsync s = (GLsync)(uintptr_t)ai(args,0);
            return (s != 0) ? 1 : 0;
        }
        case GL64_fn_glGetSynciv: {
            // (sync, pname, bufSize, length*, values*). Report SIGNALED status.
            GLenum pname = (GLenum)ai(args,1);
            GLsizei bufSize = (GLsizei)ai(args,2);
            U64 lenAddr = args.a[3], valAddr = args.a[4];
            U32 val = 0;
            switch (pname) {
                case 0x9114 /*GL_SYNC_STATUS*/:    val = 0x9119 /*GL_SIGNALED*/; break;
                case 0x9112 /*GL_OBJECT_TYPE*/:    val = 0x9116 /*GL_SYNC_FENCE*/; break;
                case 0x9113 /*GL_SYNC_CONDITION*/: val = 0x9117 /*GL_SYNC_GPU_COMMANDS_COMPLETE*/; break;
                case 0x9115 /*GL_SYNC_FLAGS*/:     val = 0; break;
                default: val = 0; break;
            }
            if (valAddr && bufSize >= 1) cpu->memory->writed(valAddr, val);
            if (lenAddr) cpu->memory->writed(lenAddr, (bufSize >= 1) ? 1 : 0);
            return 0;
        }

        // === occlusion / timer queries ===================================
        // wined3d uses queries for occlusion + event/timestamp tracking. We
        // create real query objects so begin/end are coherent, and report
        // "result available" with a benign result so its state machine advances.
        case GL64_fn_glGenQueries: {
            if (g_glContext && args.a[1]) {
                GLsizei n = (GLsizei)ai(args,0);
                if (n > 0 && n <= 65536) {
                    std::vector<GLuint> ids((size_t)n, 0); GLuint* p = ids.data();
                    GL_MT(glGenQueries(n, p));
                    cpu->memory->memcpyToGuest(args.a[1], ids.data(), (U64)n * 4);
                }
            }
            return 0;
        }
        case GL64_fn_glDeleteQueries: {
            if (g_glContext && args.a[1]) {
                GLsizei n = (GLsizei)ai(args,0);
                if (n > 0 && n <= 65536) {
                    std::vector<GLuint> ids((size_t)n, 0);
                    cpu->memory->memcpyFromGuest(ids.data(), args.a[1], (U64)n * 4);
                    const GLuint* p = ids.data();
                    GL_MT(glDeleteQueries(n, p));
                }
            }
            return 0;
        }
        case GL64_fn_glIsQuery:
            return (ai(args,0) != 0) ? 1 : 0;
        case GL64_fn_glBeginQuery:
            // Many wined3d query targets (TIMESTAMP, etc.) aren't valid in WebGL2;
            // begin/end are best-effort and errors are swallowed by the GLERR path.
            if (g_glContext) GL_MT(glBeginQuery((GLenum)ai(args,0), (GLuint)ai(args,1)));
            return 0;
        case GL64_fn_glEndQuery:
            if (g_glContext) GL_MT(glEndQuery((GLenum)ai(args,0)));
            return 0;
        case GL64_fn_glQueryCounter:
            return 0; // GL_TIMESTAMP — not in WebGL2; ignore
        case GL64_fn_glGetQueryiv: {
            // (target, pname, params*). GL_QUERY_COUNTER_BITS=0, current query=0.
            if (args.a[2]) cpu->memory->writed(args.a[2], 0);
            return 0;
        }
        case GL64_fn_glGetQueryObjectiv:
        case GL64_fn_glGetQueryObjectuiv: {
            // (id, pname, params*). Report RESULT_AVAILABLE=true and RESULT=0/1.
            GLenum pname = (GLenum)ai(args,1);
            U32 v = 0;
            if (pname == 0x8867 /*GL_QUERY_RESULT_AVAILABLE*/) v = 1;
            else if (pname == 0x8866 /*GL_QUERY_RESULT*/)      v = 1; // samples passed
            if (args.a[2]) cpu->memory->writed(args.a[2], v);
            return 0;
        }
        case GL64_fn_glGetQueryObjectui64v: {
            GLenum pname = (GLenum)ai(args,1);
            U64 v = 0;
            if (pname == 0x8867 /*GL_QUERY_RESULT_AVAILABLE*/) v = 1;
            else if (pname == 0x8866 /*GL_QUERY_RESULT*/)      v = 1;
            if (args.a[2]) cpu->memory->writeq(args.a[2], v);
            return 0;
        }
#endif // __EMSCRIPTEN__

        default:
            klog_fmt("gl64: unimplemented fn id %llu", (unsigned long long)fnId);
            return 0;
    }
}

#endif // BOXEDWINE_OPENGL
