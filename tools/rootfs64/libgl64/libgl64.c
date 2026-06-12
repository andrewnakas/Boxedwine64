/*
 *  Copyright (C) 2012-2025  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

// Guest libGL.so.1 for the 64-bit Boxedwine wine path. wine's winex11.drv
// dlopen("libGL.so.1") and resolves glXGetProcAddressARB + the core GLX entry
// points; opengl32 then resolves every gl* function through glXGetProcAddressARB.
// Each wrapper here packs its arguments into a GL64Args block and traps to the
// host (Boxedwine64's kernel) via a private syscall (see gl64bridge_abi.h /
// source/opengl/gl64bridge.cpp), where the call is replayed on a real macOS GL
// context. This is the 64-bit analogue of the 32-bit int-0x99 GL shim.
//
// Self-contained: no GL/GLX headers required. We declare just enough typedefs to
// export the right symbol names with C linkage.

// Freestanding: no libc headers (the build cross-compiles to x86_64-linux from
// macOS, which has no Linux libc headers/objects). We define the few fixed-width
// types and string/mem helpers we need locally so the .so has no libc DT_NEEDED.
typedef unsigned char       uint8_t;
typedef unsigned int        uint32_t;
typedef unsigned long long  uint64_t;
typedef unsigned long       uintptr_t;

static void* gl_memcpy(void* d, const void* s, unsigned long n) {
    unsigned char* dd = (unsigned char*)d; const unsigned char* ss = (const unsigned char*)s;
    while (n--) *dd++ = *ss++; return d;
}
static void* gl_memset(void* d, int v, unsigned long n) {
    unsigned char* dd = (unsigned char*)d; while (n--) *dd++ = (unsigned char)v; return d;
}
static int gl_strcmp(const char* a, const char* b) {
    while (*a && (*a == *b)) { a++; b++; } return (int)((unsigned char)*a) - (int)((unsigned char)*b);
}
#define memcpy gl_memcpy
#define memset gl_memset
#define strcmp gl_strcmp

// ---- ABI shared with the host (copy kept in sync with source/opengl) -------
#define GL64_SYSCALL_NR  ((uint64_t)0x474C0000ULL)
#define GL64_MAX_ARGS 16
typedef struct GL64Args { uint64_t a[GL64_MAX_ARGS]; } GL64Args;

enum {
    GL64_fn_glXQueryVersion = 1,
    GL64_fn_glXQueryExtension,
    GL64_fn_glXQueryExtensionsString,
    GL64_fn_glXChooseVisual,
    GL64_fn_glXCreateContext,
    GL64_fn_glXCreateContextAttribsARB,
    GL64_fn_glXChooseFBConfig,
    GL64_fn_glXGetFBConfigs,
    GL64_fn_glXGetFBConfigAttrib,
    GL64_fn_glXGetVisualFromFBConfig,
    GL64_fn_glXGetConfig,
    GL64_fn_glXMakeCurrent,
    GL64_fn_glXMakeContextCurrent,
    GL64_fn_glXSwapBuffers,
    GL64_fn_glXDestroyContext,
    GL64_fn_glXIsDirect,
    GL64_fn_glXGetCurrentContext,
    GL64_fn_glXGetCurrentDrawable,
    GL64_fn_glXQueryServerString,
    GL64_fn_glXGetClientString,
    GL64_fn_glXWaitGL,
    GL64_fn_glXWaitX,
    GL64_fn_glXSwapIntervalEXT,

    GL64_fn_glClearColor = 200,
    GL64_fn_glClear,
    GL64_fn_glClearDepth,
    GL64_fn_glViewport,
    GL64_fn_glEnable,
    GL64_fn_glDisable,
    GL64_fn_glShadeModel,
    GL64_fn_glDepthFunc,
    GL64_fn_glCullFace,
    GL64_fn_glFrontFace,
    GL64_fn_glHint,
    GL64_fn_glFlush,
    GL64_fn_glFinish,
    GL64_fn_glGetError,
    GL64_fn_glGetString,
    GL64_fn_glGetIntegerv,
    GL64_fn_glGetFloatv,
    GL64_fn_glColor3f,
    GL64_fn_glColor4f,

    GL64_fn_glMatrixMode = 260,
    GL64_fn_glLoadIdentity,
    GL64_fn_glPushMatrix,
    GL64_fn_glPopMatrix,
    GL64_fn_glFrustum,
    GL64_fn_glOrtho,
    GL64_fn_glTranslatef,
    GL64_fn_glRotatef,
    GL64_fn_glScalef,
    GL64_fn_glMultMatrixf,

    GL64_fn_glLightfv = 290,
    GL64_fn_glLightf,
    GL64_fn_glMaterialfv,
    GL64_fn_glMaterialf,
    GL64_fn_glColorMaterial,
    GL64_fn_glNormal3f,

    GL64_fn_glBegin = 320,
    GL64_fn_glEnd,
    GL64_fn_glVertex2f,
    GL64_fn_glVertex3f,

    // === programmable pipeline (must match source/opengl/gl64bridge_abi.h) ===
    GL64_fn_traceProc = 400,

    GL64_fn_glCreateShader = 410,
    GL64_fn_glShaderSource,
    GL64_fn_glCompileShader,
    GL64_fn_glGetShaderiv,
    GL64_fn_glGetShaderInfoLog,
    GL64_fn_glDeleteShader,
    GL64_fn_glCreateProgram,
    GL64_fn_glAttachShader,
    GL64_fn_glDetachShader,
    GL64_fn_glBindAttribLocation,
    GL64_fn_glLinkProgram,
    GL64_fn_glGetProgramiv,
    GL64_fn_glGetProgramInfoLog,
    GL64_fn_glUseProgram,
    GL64_fn_glDeleteProgram,
    GL64_fn_glGetUniformLocation,
    GL64_fn_glGetAttribLocation,
    GL64_fn_glValidateProgram,

    GL64_fn_glUniform1i = 440,
    GL64_fn_glUniform1f,
    GL64_fn_glUniform2f,
    GL64_fn_glUniform3f,
    GL64_fn_glUniform4f,
    GL64_fn_glUniform1fv,
    GL64_fn_glUniform2fv,
    GL64_fn_glUniform3fv,
    GL64_fn_glUniform4fv,
    GL64_fn_glUniform1iv,
    GL64_fn_glUniformMatrix2fv,
    GL64_fn_glUniformMatrix3fv,
    GL64_fn_glUniformMatrix4fv,

    GL64_fn_glGenBuffers = 470,
    GL64_fn_glBindBuffer,
    GL64_fn_glBufferData,
    GL64_fn_glBufferSubData,
    GL64_fn_glDeleteBuffers,
    GL64_fn_glMapBufferRange,

    GL64_fn_glEnableVertexAttribArray = 490,
    GL64_fn_glDisableVertexAttribArray,
    GL64_fn_glVertexAttribPointer,
    GL64_fn_glGenVertexArrays,
    GL64_fn_glBindVertexArray,
    GL64_fn_glDeleteVertexArrays,
    GL64_fn_glVertexAttrib4f,

    GL64_fn_glDrawArrays = 510,
    GL64_fn_glDrawElements,
    GL64_fn_glDrawRangeElements,

    GL64_fn_glBlendFunc = 530,
    GL64_fn_glBlendFuncSeparate,
    GL64_fn_glBlendEquation,
    GL64_fn_glBlendEquationSeparate,
    GL64_fn_glBlendColor,
    GL64_fn_glColorMask,
    GL64_fn_glDepthMask,
    GL64_fn_glStencilFunc,
    GL64_fn_glStencilOp,
    GL64_fn_glStencilMask,
    GL64_fn_glStencilFuncSeparate,
    GL64_fn_glStencilOpSeparate,
    GL64_fn_glStencilMaskSeparate,
    GL64_fn_glScissor,
    GL64_fn_glPolygonOffset,
    GL64_fn_glPolygonMode,
    GL64_fn_glDepthRange,
    GL64_fn_glLineWidth,
    GL64_fn_glPixelStorei,
    GL64_fn_glSampleCoverage,

    GL64_fn_glActiveTexture = 560,
    GL64_fn_glGenTextures,
    GL64_fn_glBindTexture,
    GL64_fn_glDeleteTextures,
    GL64_fn_glTexParameteri,
    GL64_fn_glTexParameterf,
    GL64_fn_glTexImage2D,
    GL64_fn_glTexSubImage2D,
    GL64_fn_glGenerateMipmap,
    GL64_fn_glCompressedTexImage2D,

    GL64_fn_glGetStringi = 590,
    GL64_fn_glGetShaderSource
};

// ---- the trap ---------------------------------------------------------------
static inline uint64_t gl64_trap(uint64_t fnId, GL64Args* args) {
    uint64_t ret;
    register uint64_t rdi __asm__("rdi") = fnId;
    register uint64_t rsi __asm__("rsi") = (uint64_t)(uintptr_t)args;
    __asm__ __volatile__(
        "syscall"
        : "=a"(ret)
        : "a"(GL64_SYSCALL_NR), "r"(rdi), "r"(rsi)
        : "rcx", "r11", "memory");
    return ret;
}

// Load-time witness: fire a gl64 trap with fnId=0 the moment winex11/opengl32
// dlopens this libGL. The host bridge logs it under BW64_GLTRACE ("gl64: FIRST
// trap fnId=0"). If that line never prints, the guest never loaded THIS libGL —
// the GL-disable / ChoosePixelFormat=0 failure is in the dlopen, not downstream.
__attribute__((constructor))
static void gl64_loaded_witness(void) {
    gl64_trap(0 /* sentinel: libGL loaded */, 0);
}

// Bit-cast helpers: floats/doubles travel as their raw bits in u64 slots.
static inline uint64_t F2U(float f)  { uint32_t u; memcpy(&u, &f, 4); return (uint64_t)u; }
static inline uint64_t D2U(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }

#define API __attribute__((visibility("default")))

// wine's winex11 calls XFree()/free() on the XVisualInfo* and GLXFBConfig* arrays
// returned by glXChooseVisual / glX*FBConfig* (it expects real Xlib-allocated,
// freeable memory). Returning a static/.bss pointer makes that free() abort with
// "free(): invalid pointer". This .so is freestanding (no libc linked) but is
// dlopen'd into a process that HAS glibc, so the dynamic linker resolves malloc
// from the guest libc at load time. Declare it; never free here (wine owns it).
extern void* malloc(unsigned long size);

// ---- GL minimal typedefs (match the platform C ABI) -------------------------
typedef unsigned int   GLenum;
typedef unsigned int   GLbitfield;
typedef int            GLint;
typedef int            GLsizei;
typedef unsigned char  GLboolean;
typedef unsigned char  GLubyte;
typedef float          GLfloat;
typedef float          GLclampf;
typedef double         GLdouble;
typedef double         GLclampd;
typedef void           GLvoid;

typedef void* GLXContext;
typedef void* GLXFBConfig;
typedef unsigned long  XID;
typedef XID            GLXDrawable;
typedef struct _XDisplay Display;
typedef struct { void* visual; XID visualid; int screen; int depth; int c_class;
                 unsigned long red_mask, green_mask, blue_mask;
                 int colormap_size; int bits_per_rgb; } XVisualInfo;

// ===========================================================================
// GLX entry points
// ===========================================================================
API int glXQueryVersion(Display* dpy, int* major, int* minor) {
    GL64Args a = {{0}}; a.a[0]=(uint64_t)(uintptr_t)major; a.a[1]=(uint64_t)(uintptr_t)minor;
    return (int)gl64_trap(GL64_fn_glXQueryVersion, &a);
}
API GLboolean glXQueryExtension(Display* dpy, int* errorBase, int* eventBase) {
    GL64Args a = {{0}}; a.a[0]=(uint64_t)(uintptr_t)errorBase; a.a[1]=(uint64_t)(uintptr_t)eventBase;
    return (GLboolean)gl64_trap(GL64_fn_glXQueryExtension, &a);
}
API const char* glXQueryExtensionsString(Display* dpy, int screen) {
    // Advertise the extensions winex11 cares about for context creation. Returned
    // from a static buffer in the guest so winex11 can strstr() it directly.
    static const char* s = "GLX_ARB_create_context GLX_ARB_create_context_profile "
                           "GLX_EXT_create_context_es2_profile GLX_ARB_get_proc_address";
    (void)gl64_trap(GL64_fn_glXQueryExtensionsString, 0);
    return s;
}
API const char* glXQueryServerString(Display* dpy, int screen, int name) {
    static const char* vendor = "Boxedwine64";
    static const char* version = "1.4";
    static const char* exts = "GLX_ARB_create_context GLX_ARB_create_context_profile";
    if (name == 1) return vendor;      // GLX_VENDOR
    if (name == 2) return version;     // GLX_VERSION
    return exts;                       // GLX_EXTENSIONS
}
API const char* glXGetClientString(Display* dpy, int name) {
    return glXQueryServerString(dpy, 0, name);
}
API XVisualInfo* glXChooseVisual(Display* dpy, int screen, int* attribList) {
    GL64Args a = {{0}}; a.a[0]=(uint64_t)(uintptr_t)attribList;
    (void)gl64_trap(GL64_fn_glXChooseVisual, &a);
    // Heap-allocate: wine XFree()s this. (See the malloc note above.)
    XVisualInfo* vi = (XVisualInfo*)malloc(sizeof(XVisualInfo));
    if (!vi) return 0;
    memset(vi, 0, sizeof(*vi));
    vi->depth = 24; vi->c_class = 4 /*TrueColor*/; vi->bits_per_rgb = 8;
    vi->red_mask = 0xff0000; vi->green_mask = 0x00ff00; vi->blue_mask = 0x0000ff;
    vi->visualid = 0x21; vi->colormap_size = 256;
    return vi;
}
API GLXContext glXCreateContext(Display* dpy, XVisualInfo* vis, GLXContext share, int direct) {
    GL64Args a = {{0}}; a.a[0]=(uint64_t)(uintptr_t)vis; a.a[1]=(uint64_t)(uintptr_t)share; a.a[2]=(uint64_t)direct;
    return (GLXContext)(uintptr_t)gl64_trap(GL64_fn_glXCreateContext, &a);
}
API GLXContext glXCreateContextAttribsARB(Display* dpy, GLXFBConfig config, GLXContext share, int direct, const int* attribs) {
    GL64Args a = {{0}}; a.a[0]=(uint64_t)(uintptr_t)config; a.a[1]=(uint64_t)(uintptr_t)share;
    a.a[2]=(uint64_t)direct; a.a[3]=(uint64_t)(uintptr_t)attribs;
    return (GLXContext)(uintptr_t)gl64_trap(GL64_fn_glXCreateContextAttribsARB, &a);
}
API GLXContext glXCreateNewContext(Display* dpy, GLXFBConfig config, int renderType, GLXContext share, int direct) {
    GL64Args a = {{0}}; a.a[0]=(uint64_t)(uintptr_t)config; a.a[1]=(uint64_t)(uintptr_t)share; a.a[2]=(uint64_t)direct;
    return (GLXContext)(uintptr_t)gl64_trap(GL64_fn_glXCreateContext, &a);
}
// glXChooseFBConfig/glXGetFBConfigs return an array wine XFree()s — so malloc a
// 1-element array of opaque config ids (NOT a static, which wine would invalid-
// free). The element is an opaque id the host bridge tracks; winex11's [i] read
// stays in this guest buffer.
API GLXFBConfig* glXChooseFBConfig(Display* dpy, int screen, const int* attribs, int* nelements) {
    GL64Args a = {{0}}; a.a[0]=(uint64_t)screen; a.a[1]=(uint64_t)(uintptr_t)attribs; a.a[2]=(uint64_t)(uintptr_t)nelements;
    uint64_t id = gl64_trap(GL64_fn_glXChooseFBConfig, &a);
    GLXFBConfig* arr = (GLXFBConfig*)malloc(sizeof(GLXFBConfig));
    if (!arr) { if (nelements) *nelements = 0; return 0; }
    arr[0] = (GLXFBConfig)(uintptr_t)id;
    if (nelements) *nelements = 1;
    return arr;
}
API GLXFBConfig* glXGetFBConfigs(Display* dpy, int screen, int* nelements) {
    GL64Args a = {{0}}; a.a[0]=(uint64_t)screen; a.a[1]=(uint64_t)(uintptr_t)nelements;
    uint64_t id = gl64_trap(GL64_fn_glXGetFBConfigs, &a);
    GLXFBConfig* arr = (GLXFBConfig*)malloc(sizeof(GLXFBConfig));
    if (!arr) { if (nelements) *nelements = 0; return 0; }
    arr[0] = (GLXFBConfig)(uintptr_t)id;
    if (nelements) *nelements = 1;
    return arr;
}
API int glXGetFBConfigAttrib(Display* dpy, GLXFBConfig config, int attribute, int* value) {
    GL64Args a = {{0}}; a.a[0]=(uint64_t)(uintptr_t)config; a.a[1]=(uint64_t)attribute; a.a[2]=(uint64_t)(uintptr_t)value;
    return (int)gl64_trap(GL64_fn_glXGetFBConfigAttrib, &a);
}
API XVisualInfo* glXGetVisualFromFBConfig(Display* dpy, GLXFBConfig config) {
    return glXChooseVisual(dpy, 0, 0);
}
API int glXGetConfig(Display* dpy, XVisualInfo* vis, int attribute, int* value) {
    GL64Args a = {{0}}; a.a[0]=(uint64_t)(uintptr_t)vis; a.a[1]=(uint64_t)attribute; a.a[2]=(uint64_t)(uintptr_t)value;
    return (int)gl64_trap(GL64_fn_glXGetConfig, &a);
}
API GLboolean glXMakeCurrent(Display* dpy, GLXDrawable drawable, GLXContext ctx) {
    GL64Args a = {{0}}; a.a[0]=(uint64_t)drawable; a.a[1]=(uint64_t)(uintptr_t)ctx;
    return (GLboolean)gl64_trap(GL64_fn_glXMakeCurrent, &a);
}
API GLboolean glXMakeContextCurrent(Display* dpy, GLXDrawable draw, GLXDrawable read, GLXContext ctx) {
    GL64Args a = {{0}}; a.a[0]=(uint64_t)draw; a.a[1]=(uint64_t)read; a.a[2]=(uint64_t)(uintptr_t)ctx;
    return (GLboolean)gl64_trap(GL64_fn_glXMakeContextCurrent, &a);
}
API void glXSwapBuffers(Display* dpy, GLXDrawable drawable) {
    GL64Args a = {{0}}; a.a[0]=(uint64_t)drawable;
    (void)gl64_trap(GL64_fn_glXSwapBuffers, &a);
}
API void glXDestroyContext(Display* dpy, GLXContext ctx) {
    GL64Args a = {{0}}; a.a[0]=(uint64_t)(uintptr_t)ctx;
    (void)gl64_trap(GL64_fn_glXDestroyContext, &a);
}
API GLboolean glXIsDirect(Display* dpy, GLXContext ctx) {
    return (GLboolean)gl64_trap(GL64_fn_glXIsDirect, 0);
}
API GLXContext glXGetCurrentContext(void) {
    return (GLXContext)(uintptr_t)gl64_trap(GL64_fn_glXGetCurrentContext, 0);
}
API GLXDrawable glXGetCurrentDrawable(void) {
    return (GLXDrawable)gl64_trap(GL64_fn_glXGetCurrentDrawable, 0);
}
API void glXWaitGL(void) { (void)gl64_trap(GL64_fn_glXWaitGL, 0); }
API void glXWaitX(void)  { (void)gl64_trap(GL64_fn_glXWaitX, 0); }
API void glXSwapIntervalEXT(Display* dpy, GLXDrawable d, int interval) {
    GL64Args a = {{0}}; a.a[0]=(uint64_t)d; a.a[1]=(uint64_t)interval;
    (void)gl64_trap(GL64_fn_glXSwapIntervalEXT, &a);
}
API int glXSwapIntervalMESA(unsigned int interval) { return 0; }
API int glXSwapIntervalSGI(int interval) { return 0; }

// ===========================================================================
// core GL
// ===========================================================================
API void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf al) {
    GL64Args a = {{0}}; a.a[0]=F2U(r); a.a[1]=F2U(g); a.a[2]=F2U(b); a.a[3]=F2U(al);
    (void)gl64_trap(GL64_fn_glClearColor, &a);
}
API void glClear(GLbitfield mask) {
    GL64Args a = {{0}}; a.a[0]=mask; (void)gl64_trap(GL64_fn_glClear, &a);
}
API void glClearDepth(GLclampd depth) {
    GL64Args a = {{0}}; a.a[0]=D2U(depth); (void)gl64_trap(GL64_fn_glClearDepth, &a);
}
API void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    GL64Args a = {{0}}; a.a[0]=(uint64_t)(uint32_t)x; a.a[1]=(uint64_t)(uint32_t)y; a.a[2]=(uint64_t)(uint32_t)w; a.a[3]=(uint64_t)(uint32_t)h;
    (void)gl64_trap(GL64_fn_glViewport, &a);
}
API void glEnable(GLenum c)  { GL64Args a={{0}}; a.a[0]=c; (void)gl64_trap(GL64_fn_glEnable,&a); }
API void glDisable(GLenum c) { GL64Args a={{0}}; a.a[0]=c; (void)gl64_trap(GL64_fn_glDisable,&a); }
API void glShadeModel(GLenum m){ GL64Args a={{0}}; a.a[0]=m; (void)gl64_trap(GL64_fn_glShadeModel,&a); }
API void glDepthFunc(GLenum f){ GL64Args a={{0}}; a.a[0]=f; (void)gl64_trap(GL64_fn_glDepthFunc,&a); }
API void glCullFace(GLenum m) { GL64Args a={{0}}; a.a[0]=m; (void)gl64_trap(GL64_fn_glCullFace,&a); }
API void glFrontFace(GLenum m){ GL64Args a={{0}}; a.a[0]=m; (void)gl64_trap(GL64_fn_glFrontFace,&a); }
API void glHint(GLenum t, GLenum m){ GL64Args a={{0}}; a.a[0]=t; a.a[1]=m; (void)gl64_trap(GL64_fn_glHint,&a); }
API void glFlush(void) { (void)gl64_trap(GL64_fn_glFlush,0); }
API void glFinish(void){ (void)gl64_trap(GL64_fn_glFinish,0); }
API GLenum glGetError(void){ return (GLenum)gl64_trap(GL64_fn_glGetError,0); }
API const GLubyte* glGetString(GLenum name) {
    // Host returns the real string only as a host pointer (unusable in-guest),
    // so it returns 0 and we hand back our own stable strings.
    static const GLubyte* vendor   = (const GLubyte*)"Boxedwine64";
    static const GLubyte* renderer = (const GLubyte*)"Boxedwine64 GL (WebGL2)";
    // GL 2.1 + a rich ARB extension list so wined3d's GLSL renderer backend
    // activates (it picks GLSL when ARB_shader_objects + GLSL 1.20 are present)
    // and so it sees the VBO / FBO / multitexture / NPOT / depth-texture caps it
    // needs to emit a draw. With an EMPTY extension string wined3d concluded the
    // driver had no usable feature set and never issued DrawPrimitive (the clear
    // showed, the triangle never did). These all map onto WebGL2/GLES3 features.
    static const GLubyte* version  = (const GLubyte*)"2.1 Boxedwine64";
    static const GLubyte* slv      = (const GLubyte*)"1.20";
    // The monolithic GL_EXTENSIONS string (legacy/compat path). Must list the same
    // extensions as g_extList[] below (the core-profile glGetStringi path) so both
    // wined3d code paths see the same feature set.
    static const GLubyte* exts     = (const GLubyte*)
        "GL_ARB_multitexture GL_ARB_texture_env_combine GL_ARB_texture_env_dot3 "
        "GL_ARB_vertex_buffer_object GL_ARB_pixel_buffer_object "
        "GL_ARB_vertex_program GL_ARB_fragment_program "
        "GL_ARB_shader_objects GL_ARB_shading_language_100 "
        "GL_ARB_vertex_shader GL_ARB_fragment_shader "
        "GL_ARB_framebuffer_object GL_EXT_framebuffer_object GL_EXT_framebuffer_blit "
        "GL_ARB_depth_texture GL_ARB_shadow GL_ARB_texture_non_power_of_two "
        "GL_ARB_texture_rectangle GL_ARB_texture_cube_map GL_ARB_texture_float "
        "GL_ARB_half_float_pixel GL_ARB_occlusion_query GL_ARB_point_sprite "
        "GL_ARB_draw_buffers GL_EXT_blend_minmax GL_EXT_blend_color "
        "GL_EXT_blend_func_separate GL_EXT_blend_equation_separate "
        "GL_EXT_stencil_two_side GL_ARB_stencil_two_side "
        "GL_EXT_texture_sRGB GL_ARB_texture_compression GL_EXT_texture_compression_s3tc "
        "GL_NV_texture_shader GL_ARB_map_buffer_range";
    (void)gl64_trap(GL64_fn_glGetString, 0);
    switch (name) {
        case 0x1F00: return vendor;    // GL_VENDOR
        case 0x1F01: return renderer;  // GL_RENDERER
        case 0x1F02: return version;   // GL_VERSION
        case 0x1F03: return exts;      // GL_EXTENSIONS
        case 0x8B8C: return slv;       // GL_SHADING_LANGUAGE_VERSION
        default:     return exts;
    }
}

// The SAME extension set as the monolithic string above, as an indexable array
// for the GL 3.0+ core-profile enumeration path: glGetIntegerv(GL_NUM_EXTENSIONS)
// + glGetStringi(GL_EXTENSIONS, i). wined3d uses THIS path (it queries
// GL_NUM_EXTENSIONS), so an empty glGetStringi made it find no usable extensions
// and never issue a draw, even though the monolithic string was populated.
static const char* const g_extList[] = {
    "GL_ARB_multitexture","GL_ARB_texture_env_combine","GL_ARB_texture_env_dot3",
    "GL_ARB_vertex_buffer_object","GL_ARB_pixel_buffer_object",
    "GL_ARB_vertex_program","GL_ARB_fragment_program",
    "GL_ARB_shader_objects","GL_ARB_shading_language_100",
    "GL_ARB_vertex_shader","GL_ARB_fragment_shader",
    "GL_ARB_framebuffer_object","GL_EXT_framebuffer_object","GL_EXT_framebuffer_blit",
    "GL_ARB_depth_texture","GL_ARB_shadow","GL_ARB_texture_non_power_of_two",
    "GL_ARB_texture_rectangle","GL_ARB_texture_cube_map","GL_ARB_texture_float",
    "GL_ARB_half_float_pixel","GL_ARB_occlusion_query","GL_ARB_point_sprite",
    "GL_ARB_draw_buffers","GL_EXT_blend_minmax","GL_EXT_blend_color",
    "GL_EXT_blend_func_separate","GL_EXT_blend_equation_separate",
    "GL_EXT_stencil_two_side","GL_ARB_stencil_two_side",
    "GL_EXT_texture_sRGB","GL_ARB_texture_compression","GL_EXT_texture_compression_s3tc",
    "GL_NV_texture_shader","GL_ARB_map_buffer_range",
};
#define G_EXT_COUNT ((int)(sizeof(g_extList)/sizeof(g_extList[0])))

API void glGetIntegerv(GLenum pname, GLint* params) {
    // GL_NUM_EXTENSIONS: answer with OUR curated count guest-side. If we forwarded
    // to the host it would return WebGL2's own count (54), and wined3d would then
    // glGetStringi() through 54 host extensions that our guest can't name — so it
    // would see none of the features it needs. Keeping the count + the names in
    // sync (both from g_extList) is what lets wined3d's GLSL renderer come up.
    if (pname == 0x821D /*GL_NUM_EXTENSIONS*/) { if (params) params[0] = G_EXT_COUNT; return; }
    GL64Args a = {{0}}; a.a[0]=pname; a.a[1]=(uint64_t)(uintptr_t)params;
    (void)gl64_trap(GL64_fn_glGetIntegerv, &a);
}
API void glGetFloatv(GLenum pname, GLfloat* params) {
    GL64Args a = {{0}}; a.a[0]=pname; a.a[1]=(uint64_t)(uintptr_t)params;
    (void)gl64_trap(GL64_fn_glGetFloatv, &a);
}
API void glColor3f(GLfloat r,GLfloat g,GLfloat b){ GL64Args a={{0}}; a.a[0]=F2U(r);a.a[1]=F2U(g);a.a[2]=F2U(b); (void)gl64_trap(GL64_fn_glColor3f,&a); }
API void glColor4f(GLfloat r,GLfloat g,GLfloat b,GLfloat al){ GL64Args a={{0}}; a.a[0]=F2U(r);a.a[1]=F2U(g);a.a[2]=F2U(b);a.a[3]=F2U(al); (void)gl64_trap(GL64_fn_glColor4f,&a); }

API void glMatrixMode(GLenum m){ GL64Args a={{0}}; a.a[0]=m; (void)gl64_trap(GL64_fn_glMatrixMode,&a); }
API void glLoadIdentity(void){ (void)gl64_trap(GL64_fn_glLoadIdentity,0); }
API void glPushMatrix(void){ (void)gl64_trap(GL64_fn_glPushMatrix,0); }
API void glPopMatrix(void){ (void)gl64_trap(GL64_fn_glPopMatrix,0); }
API void glFrustum(GLdouble l,GLdouble r,GLdouble b,GLdouble t,GLdouble n,GLdouble f){
    GL64Args a={{0}}; a.a[0]=D2U(l);a.a[1]=D2U(r);a.a[2]=D2U(b);a.a[3]=D2U(t);a.a[4]=D2U(n);a.a[5]=D2U(f);
    (void)gl64_trap(GL64_fn_glFrustum,&a);
}
API void glOrtho(GLdouble l,GLdouble r,GLdouble b,GLdouble t,GLdouble n,GLdouble f){
    GL64Args a={{0}}; a.a[0]=D2U(l);a.a[1]=D2U(r);a.a[2]=D2U(b);a.a[3]=D2U(t);a.a[4]=D2U(n);a.a[5]=D2U(f);
    (void)gl64_trap(GL64_fn_glOrtho,&a);
}
API void glTranslatef(GLfloat x,GLfloat y,GLfloat z){ GL64Args a={{0}}; a.a[0]=F2U(x);a.a[1]=F2U(y);a.a[2]=F2U(z); (void)gl64_trap(GL64_fn_glTranslatef,&a); }
API void glRotatef(GLfloat ang,GLfloat x,GLfloat y,GLfloat z){ GL64Args a={{0}}; a.a[0]=F2U(ang);a.a[1]=F2U(x);a.a[2]=F2U(y);a.a[3]=F2U(z); (void)gl64_trap(GL64_fn_glRotatef,&a); }
API void glScalef(GLfloat x,GLfloat y,GLfloat z){ GL64Args a={{0}}; a.a[0]=F2U(x);a.a[1]=F2U(y);a.a[2]=F2U(z); (void)gl64_trap(GL64_fn_glScalef,&a); }
API void glMultMatrixf(const GLfloat* m){ GL64Args a={{0}}; a.a[0]=(uint64_t)(uintptr_t)m; (void)gl64_trap(GL64_fn_glMultMatrixf,&a); }

API void glLightfv(GLenum light,GLenum pname,const GLfloat* params){ GL64Args a={{0}}; a.a[0]=light;a.a[1]=pname;a.a[2]=(uint64_t)(uintptr_t)params; (void)gl64_trap(GL64_fn_glLightfv,&a); }
API void glLightf(GLenum light,GLenum pname,GLfloat param){ GL64Args a={{0}}; a.a[0]=light;a.a[1]=pname;a.a[2]=F2U(param); (void)gl64_trap(GL64_fn_glLightf,&a); }
API void glMaterialfv(GLenum face,GLenum pname,const GLfloat* params){ GL64Args a={{0}}; a.a[0]=face;a.a[1]=pname;a.a[2]=(uint64_t)(uintptr_t)params; (void)gl64_trap(GL64_fn_glMaterialfv,&a); }
API void glMaterialf(GLenum face,GLenum pname,GLfloat param){ GL64Args a={{0}}; a.a[0]=face;a.a[1]=pname;a.a[2]=F2U(param); (void)gl64_trap(GL64_fn_glMaterialf,&a); }
API void glColorMaterial(GLenum face,GLenum mode){ GL64Args a={{0}}; a.a[0]=face;a.a[1]=mode; (void)gl64_trap(GL64_fn_glColorMaterial,&a); }
API void glNormal3f(GLfloat x,GLfloat y,GLfloat z){ GL64Args a={{0}}; a.a[0]=F2U(x);a.a[1]=F2U(y);a.a[2]=F2U(z); (void)gl64_trap(GL64_fn_glNormal3f,&a); }

API void glBegin(GLenum m){ GL64Args a={{0}}; a.a[0]=m; (void)gl64_trap(GL64_fn_glBegin,&a); }
API void glEnd(void){ (void)gl64_trap(GL64_fn_glEnd,0); }
API void glVertex2f(GLfloat x,GLfloat y){ GL64Args a={{0}}; a.a[0]=F2U(x);a.a[1]=F2U(y); (void)gl64_trap(GL64_fn_glVertex2f,&a); }
API void glVertex3f(GLfloat x,GLfloat y,GLfloat z){ GL64Args a={{0}}; a.a[0]=F2U(x);a.a[1]=F2U(y);a.a[2]=F2U(z); (void)gl64_trap(GL64_fn_glVertex3f,&a); }

// ===========================================================================
// Programmable pipeline (GL2 / GLES3) — the modern entry points wined3d needs.
// Extra typedefs the FFP block above didn't require.
// ===========================================================================
typedef unsigned int   GLuint;
typedef char           GLchar;
typedef long           GLsizeiptr;   // 64-bit on x86_64-linux
typedef long           GLintptr;
typedef unsigned int   GLuintptr;

// --- shaders / programs ---
API GLuint glCreateShader(GLenum type){ GL64Args a={{0}}; a.a[0]=type; return (GLuint)gl64_trap(GL64_fn_glCreateShader,&a); }
API void glShaderSource(GLuint sh, GLsizei count, const GLchar* const* string, const GLint* length){
    GL64Args a={{0}}; a.a[0]=sh; a.a[1]=(uint64_t)(uint32_t)count;
    a.a[2]=(uint64_t)(uintptr_t)string; a.a[3]=(uint64_t)(uintptr_t)length;
    (void)gl64_trap(GL64_fn_glShaderSource,&a);
}
API void glCompileShader(GLuint sh){ GL64Args a={{0}}; a.a[0]=sh; (void)gl64_trap(GL64_fn_glCompileShader,&a); }
API void glGetShaderiv(GLuint sh, GLenum pname, GLint* params){ GL64Args a={{0}}; a.a[0]=sh; a.a[1]=pname; a.a[2]=(uint64_t)(uintptr_t)params; (void)gl64_trap(GL64_fn_glGetShaderiv,&a); }
API void glGetShaderInfoLog(GLuint sh, GLsizei bufSize, GLsizei* length, GLchar* infoLog){
    GL64Args a={{0}}; a.a[0]=sh; a.a[1]=(uint64_t)(uint32_t)bufSize; a.a[2]=(uint64_t)(uintptr_t)length; a.a[3]=(uint64_t)(uintptr_t)infoLog;
    (void)gl64_trap(GL64_fn_glGetShaderInfoLog,&a);
}
API void glDeleteShader(GLuint sh){ GL64Args a={{0}}; a.a[0]=sh; (void)gl64_trap(GL64_fn_glDeleteShader,&a); }
API GLuint glCreateProgram(void){ return (GLuint)gl64_trap(GL64_fn_glCreateProgram,0); }
API void glAttachShader(GLuint p, GLuint sh){ GL64Args a={{0}}; a.a[0]=p; a.a[1]=sh; (void)gl64_trap(GL64_fn_glAttachShader,&a); }
API void glDetachShader(GLuint p, GLuint sh){ GL64Args a={{0}}; a.a[0]=p; a.a[1]=sh; (void)gl64_trap(GL64_fn_glDetachShader,&a); }
API void glBindAttribLocation(GLuint p, GLuint index, const GLchar* name){ GL64Args a={{0}}; a.a[0]=p; a.a[1]=index; a.a[2]=(uint64_t)(uintptr_t)name; (void)gl64_trap(GL64_fn_glBindAttribLocation,&a); }
API void glLinkProgram(GLuint p){ GL64Args a={{0}}; a.a[0]=p; (void)gl64_trap(GL64_fn_glLinkProgram,&a); }
API void glGetProgramiv(GLuint p, GLenum pname, GLint* params){ GL64Args a={{0}}; a.a[0]=p; a.a[1]=pname; a.a[2]=(uint64_t)(uintptr_t)params; (void)gl64_trap(GL64_fn_glGetProgramiv,&a); }
API void glGetProgramInfoLog(GLuint p, GLsizei bufSize, GLsizei* length, GLchar* infoLog){
    GL64Args a={{0}}; a.a[0]=p; a.a[1]=(uint64_t)(uint32_t)bufSize; a.a[2]=(uint64_t)(uintptr_t)length; a.a[3]=(uint64_t)(uintptr_t)infoLog;
    (void)gl64_trap(GL64_fn_glGetProgramInfoLog,&a);
}
API void glUseProgram(GLuint p){ GL64Args a={{0}}; a.a[0]=p; (void)gl64_trap(GL64_fn_glUseProgram,&a); }
API void glDeleteProgram(GLuint p){ GL64Args a={{0}}; a.a[0]=p; (void)gl64_trap(GL64_fn_glDeleteProgram,&a); }
API GLint glGetUniformLocation(GLuint p, const GLchar* name){ GL64Args a={{0}}; a.a[0]=p; a.a[1]=(uint64_t)(uintptr_t)name; return (GLint)gl64_trap(GL64_fn_glGetUniformLocation,&a); }
API GLint glGetAttribLocation(GLuint p, const GLchar* name){ GL64Args a={{0}}; a.a[0]=p; a.a[1]=(uint64_t)(uintptr_t)name; return (GLint)gl64_trap(GL64_fn_glGetAttribLocation,&a); }
API void glValidateProgram(GLuint p){ GL64Args a={{0}}; a.a[0]=p; (void)gl64_trap(GL64_fn_glValidateProgram,&a); }

// --- uniforms ---
API void glUniform1i(GLint l, GLint v0){ GL64Args a={{0}}; a.a[0]=(uint64_t)(uint32_t)l; a.a[1]=(uint64_t)(uint32_t)v0; (void)gl64_trap(GL64_fn_glUniform1i,&a); }
API void glUniform1f(GLint l, GLfloat v0){ GL64Args a={{0}}; a.a[0]=(uint64_t)(uint32_t)l; a.a[1]=F2U(v0); (void)gl64_trap(GL64_fn_glUniform1f,&a); }
API void glUniform2f(GLint l, GLfloat v0, GLfloat v1){ GL64Args a={{0}}; a.a[0]=(uint64_t)(uint32_t)l; a.a[1]=F2U(v0); a.a[2]=F2U(v1); (void)gl64_trap(GL64_fn_glUniform2f,&a); }
API void glUniform3f(GLint l, GLfloat v0, GLfloat v1, GLfloat v2){ GL64Args a={{0}}; a.a[0]=(uint64_t)(uint32_t)l; a.a[1]=F2U(v0); a.a[2]=F2U(v1); a.a[3]=F2U(v2); (void)gl64_trap(GL64_fn_glUniform3f,&a); }
API void glUniform4f(GLint l, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3){ GL64Args a={{0}}; a.a[0]=(uint64_t)(uint32_t)l; a.a[1]=F2U(v0); a.a[2]=F2U(v1); a.a[3]=F2U(v2); a.a[4]=F2U(v3); (void)gl64_trap(GL64_fn_glUniform4f,&a); }
API void glUniform1fv(GLint l, GLsizei n, const GLfloat* v){ GL64Args a={{0}}; a.a[0]=(uint64_t)(uint32_t)l; a.a[1]=(uint64_t)(uint32_t)n; a.a[2]=(uint64_t)(uintptr_t)v; (void)gl64_trap(GL64_fn_glUniform1fv,&a); }
API void glUniform2fv(GLint l, GLsizei n, const GLfloat* v){ GL64Args a={{0}}; a.a[0]=(uint64_t)(uint32_t)l; a.a[1]=(uint64_t)(uint32_t)n; a.a[2]=(uint64_t)(uintptr_t)v; (void)gl64_trap(GL64_fn_glUniform2fv,&a); }
API void glUniform3fv(GLint l, GLsizei n, const GLfloat* v){ GL64Args a={{0}}; a.a[0]=(uint64_t)(uint32_t)l; a.a[1]=(uint64_t)(uint32_t)n; a.a[2]=(uint64_t)(uintptr_t)v; (void)gl64_trap(GL64_fn_glUniform3fv,&a); }
API void glUniform4fv(GLint l, GLsizei n, const GLfloat* v){ GL64Args a={{0}}; a.a[0]=(uint64_t)(uint32_t)l; a.a[1]=(uint64_t)(uint32_t)n; a.a[2]=(uint64_t)(uintptr_t)v; (void)gl64_trap(GL64_fn_glUniform4fv,&a); }
API void glUniform1iv(GLint l, GLsizei n, const GLint* v){ GL64Args a={{0}}; a.a[0]=(uint64_t)(uint32_t)l; a.a[1]=(uint64_t)(uint32_t)n; a.a[2]=(uint64_t)(uintptr_t)v; (void)gl64_trap(GL64_fn_glUniform1iv,&a); }
API void glUniformMatrix2fv(GLint l, GLsizei n, GLboolean tr, const GLfloat* v){ GL64Args a={{0}}; a.a[0]=(uint64_t)(uint32_t)l; a.a[1]=(uint64_t)(uint32_t)n; a.a[2]=tr; a.a[3]=(uint64_t)(uintptr_t)v; (void)gl64_trap(GL64_fn_glUniformMatrix2fv,&a); }
API void glUniformMatrix3fv(GLint l, GLsizei n, GLboolean tr, const GLfloat* v){ GL64Args a={{0}}; a.a[0]=(uint64_t)(uint32_t)l; a.a[1]=(uint64_t)(uint32_t)n; a.a[2]=tr; a.a[3]=(uint64_t)(uintptr_t)v; (void)gl64_trap(GL64_fn_glUniformMatrix3fv,&a); }
API void glUniformMatrix4fv(GLint l, GLsizei n, GLboolean tr, const GLfloat* v){ GL64Args a={{0}}; a.a[0]=(uint64_t)(uint32_t)l; a.a[1]=(uint64_t)(uint32_t)n; a.a[2]=tr; a.a[3]=(uint64_t)(uintptr_t)v; (void)gl64_trap(GL64_fn_glUniformMatrix4fv,&a); }

// --- buffers ---
API void glGenBuffers(GLsizei n, GLuint* buffers){ GL64Args a={{0}}; a.a[0]=(uint64_t)(uint32_t)n; a.a[1]=(uint64_t)(uintptr_t)buffers; (void)gl64_trap(GL64_fn_glGenBuffers,&a); }
API void glBindBuffer(GLenum target, GLuint buffer){ GL64Args a={{0}}; a.a[0]=target; a.a[1]=buffer; (void)gl64_trap(GL64_fn_glBindBuffer,&a); }
API void glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage){ GL64Args a={{0}}; a.a[0]=target; a.a[1]=(uint64_t)size; a.a[2]=(uint64_t)(uintptr_t)data; a.a[3]=usage; (void)gl64_trap(GL64_fn_glBufferData,&a); }
API void glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data){ GL64Args a={{0}}; a.a[0]=target; a.a[1]=(uint64_t)offset; a.a[2]=(uint64_t)size; a.a[3]=(uint64_t)(uintptr_t)data; (void)gl64_trap(GL64_fn_glBufferSubData,&a); }
API void glDeleteBuffers(GLsizei n, const GLuint* buffers){ GL64Args a={{0}}; a.a[0]=(uint64_t)(uint32_t)n; a.a[1]=(uint64_t)(uintptr_t)buffers; (void)gl64_trap(GL64_fn_glDeleteBuffers,&a); }
API void* glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access){ (void)target;(void)offset;(void)length;(void)access; return 0; }

// --- vertex attrib arrays / VAO ---
API void glEnableVertexAttribArray(GLuint index){ GL64Args a={{0}}; a.a[0]=index; (void)gl64_trap(GL64_fn_glEnableVertexAttribArray,&a); }
API void glDisableVertexAttribArray(GLuint index){ GL64Args a={{0}}; a.a[0]=index; (void)gl64_trap(GL64_fn_glDisableVertexAttribArray,&a); }
API void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void* ptr){
    GL64Args a={{0}}; a.a[0]=index; a.a[1]=(uint64_t)(uint32_t)size; a.a[2]=type; a.a[3]=norm; a.a[4]=(uint64_t)(uint32_t)stride; a.a[5]=(uint64_t)(uintptr_t)ptr;
    (void)gl64_trap(GL64_fn_glVertexAttribPointer,&a);
}
API void glGenVertexArrays(GLsizei n, GLuint* arrays){ GL64Args a={{0}}; a.a[0]=(uint64_t)(uint32_t)n; a.a[1]=(uint64_t)(uintptr_t)arrays; (void)gl64_trap(GL64_fn_glGenVertexArrays,&a); }
API void glBindVertexArray(GLuint array){ GL64Args a={{0}}; a.a[0]=array; (void)gl64_trap(GL64_fn_glBindVertexArray,&a); }
API void glDeleteVertexArrays(GLsizei n, const GLuint* arrays){ GL64Args a={{0}}; a.a[0]=(uint64_t)(uint32_t)n; a.a[1]=(uint64_t)(uintptr_t)arrays; (void)gl64_trap(GL64_fn_glDeleteVertexArrays,&a); }
API void glVertexAttrib4f(GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w){ GL64Args a={{0}}; a.a[0]=index; a.a[1]=F2U(x); a.a[2]=F2U(y); a.a[3]=F2U(z); a.a[4]=F2U(w); (void)gl64_trap(GL64_fn_glVertexAttrib4f,&a); }

// --- draws ---
API void glDrawArrays(GLenum mode, GLint first, GLsizei count){ GL64Args a={{0}}; a.a[0]=mode; a.a[1]=(uint64_t)(uint32_t)first; a.a[2]=(uint64_t)(uint32_t)count; (void)gl64_trap(GL64_fn_glDrawArrays,&a); }
API void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices){ GL64Args a={{0}}; a.a[0]=mode; a.a[1]=(uint64_t)(uint32_t)count; a.a[2]=type; a.a[3]=(uint64_t)(uintptr_t)indices; (void)gl64_trap(GL64_fn_glDrawElements,&a); }
API void glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices){ GL64Args a={{0}}; a.a[0]=mode; a.a[1]=start; a.a[2]=end; a.a[3]=(uint64_t)(uint32_t)count; a.a[4]=type; a.a[5]=(uint64_t)(uintptr_t)indices; (void)gl64_trap(GL64_fn_glDrawRangeElements,&a); }

// --- modern state ---
API void glBlendFunc(GLenum s, GLenum d){ GL64Args a={{0}}; a.a[0]=s; a.a[1]=d; (void)gl64_trap(GL64_fn_glBlendFunc,&a); }
API void glBlendFuncSeparate(GLenum sR, GLenum dR, GLenum sA, GLenum dA){ GL64Args a={{0}}; a.a[0]=sR; a.a[1]=dR; a.a[2]=sA; a.a[3]=dA; (void)gl64_trap(GL64_fn_glBlendFuncSeparate,&a); }
API void glBlendEquation(GLenum m){ GL64Args a={{0}}; a.a[0]=m; (void)gl64_trap(GL64_fn_glBlendEquation,&a); }
API void glBlendEquationSeparate(GLenum mR, GLenum mA){ GL64Args a={{0}}; a.a[0]=mR; a.a[1]=mA; (void)gl64_trap(GL64_fn_glBlendEquationSeparate,&a); }
API void glBlendColor(GLfloat r, GLfloat g, GLfloat b, GLfloat al){ GL64Args a={{0}}; a.a[0]=F2U(r); a.a[1]=F2U(g); a.a[2]=F2U(b); a.a[3]=F2U(al); (void)gl64_trap(GL64_fn_glBlendColor,&a); }
API void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean al){ GL64Args a={{0}}; a.a[0]=r; a.a[1]=g; a.a[2]=b; a.a[3]=al; (void)gl64_trap(GL64_fn_glColorMask,&a); }
API void glDepthMask(GLboolean f){ GL64Args a={{0}}; a.a[0]=f; (void)gl64_trap(GL64_fn_glDepthMask,&a); }
API void glStencilFunc(GLenum func, GLint ref, GLuint mask){ GL64Args a={{0}}; a.a[0]=func; a.a[1]=(uint64_t)(uint32_t)ref; a.a[2]=mask; (void)gl64_trap(GL64_fn_glStencilFunc,&a); }
API void glStencilOp(GLenum f, GLenum zf, GLenum zp){ GL64Args a={{0}}; a.a[0]=f; a.a[1]=zf; a.a[2]=zp; (void)gl64_trap(GL64_fn_glStencilOp,&a); }
API void glStencilMask(GLuint mask){ GL64Args a={{0}}; a.a[0]=mask; (void)gl64_trap(GL64_fn_glStencilMask,&a); }
API void glStencilFuncSeparate(GLenum face, GLenum func, GLint ref, GLuint mask){ GL64Args a={{0}}; a.a[0]=face; a.a[1]=func; a.a[2]=(uint64_t)(uint32_t)ref; a.a[3]=mask; (void)gl64_trap(GL64_fn_glStencilFuncSeparate,&a); }
API void glStencilOpSeparate(GLenum face, GLenum f, GLenum zf, GLenum zp){ GL64Args a={{0}}; a.a[0]=face; a.a[1]=f; a.a[2]=zf; a.a[3]=zp; (void)gl64_trap(GL64_fn_glStencilOpSeparate,&a); }
API void glStencilMaskSeparate(GLenum face, GLuint mask){ GL64Args a={{0}}; a.a[0]=face; a.a[1]=mask; (void)gl64_trap(GL64_fn_glStencilMaskSeparate,&a); }
API void glScissor(GLint x, GLint y, GLsizei w, GLsizei h){ GL64Args a={{0}}; a.a[0]=(uint64_t)(uint32_t)x; a.a[1]=(uint64_t)(uint32_t)y; a.a[2]=(uint64_t)(uint32_t)w; a.a[3]=(uint64_t)(uint32_t)h; (void)gl64_trap(GL64_fn_glScissor,&a); }
API void glPolygonOffset(GLfloat factor, GLfloat units){ GL64Args a={{0}}; a.a[0]=F2U(factor); a.a[1]=F2U(units); (void)gl64_trap(GL64_fn_glPolygonOffset,&a); }
API void glPolygonMode(GLenum face, GLenum mode){ GL64Args a={{0}}; a.a[0]=face; a.a[1]=mode; (void)gl64_trap(GL64_fn_glPolygonMode,&a); }
API void glDepthRange(GLclampd n, GLclampd f){ GL64Args a={{0}}; a.a[0]=D2U(n); a.a[1]=D2U(f); (void)gl64_trap(GL64_fn_glDepthRange,&a); }
API void glLineWidth(GLfloat w){ GL64Args a={{0}}; a.a[0]=F2U(w); (void)gl64_trap(GL64_fn_glLineWidth,&a); }
API void glPixelStorei(GLenum pname, GLint param){ GL64Args a={{0}}; a.a[0]=pname; a.a[1]=(uint64_t)(uint32_t)param; (void)gl64_trap(GL64_fn_glPixelStorei,&a); }
API void glSampleCoverage(GLfloat value, GLboolean invert){ GL64Args a={{0}}; a.a[0]=F2U(value); a.a[1]=invert; (void)gl64_trap(GL64_fn_glSampleCoverage,&a); }

// --- textures ---
API void glActiveTexture(GLenum texture){ GL64Args a={{0}}; a.a[0]=texture; (void)gl64_trap(GL64_fn_glActiveTexture,&a); }
API void glGenTextures(GLsizei n, GLuint* textures){ GL64Args a={{0}}; a.a[0]=(uint64_t)(uint32_t)n; a.a[1]=(uint64_t)(uintptr_t)textures; (void)gl64_trap(GL64_fn_glGenTextures,&a); }
API void glBindTexture(GLenum target, GLuint texture){ GL64Args a={{0}}; a.a[0]=target; a.a[1]=texture; (void)gl64_trap(GL64_fn_glBindTexture,&a); }
API void glDeleteTextures(GLsizei n, const GLuint* textures){ GL64Args a={{0}}; a.a[0]=(uint64_t)(uint32_t)n; a.a[1]=(uint64_t)(uintptr_t)textures; (void)gl64_trap(GL64_fn_glDeleteTextures,&a); }
API void glTexParameteri(GLenum target, GLenum pname, GLint param){ GL64Args a={{0}}; a.a[0]=target; a.a[1]=pname; a.a[2]=(uint64_t)(uint32_t)param; (void)gl64_trap(GL64_fn_glTexParameteri,&a); }
API void glTexParameterf(GLenum target, GLenum pname, GLfloat param){ GL64Args a={{0}}; a.a[0]=target; a.a[1]=pname; a.a[2]=F2U(param); (void)gl64_trap(GL64_fn_glTexParameterf,&a); }
API void glTexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border, GLenum fmt, GLenum type, const void* pixels){
    GL64Args a={{0}}; a.a[0]=target; a.a[1]=(uint64_t)(uint32_t)level; a.a[2]=(uint64_t)(uint32_t)ifmt; a.a[3]=(uint64_t)(uint32_t)w; a.a[4]=(uint64_t)(uint32_t)h; a.a[5]=(uint64_t)(uint32_t)border; a.a[6]=fmt; a.a[7]=type; a.a[8]=(uint64_t)(uintptr_t)pixels;
    (void)gl64_trap(GL64_fn_glTexImage2D,&a);
}
API void glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, const void* pixels){
    GL64Args a={{0}}; a.a[0]=target; a.a[1]=(uint64_t)(uint32_t)level; a.a[2]=(uint64_t)(uint32_t)x; a.a[3]=(uint64_t)(uint32_t)y; a.a[4]=(uint64_t)(uint32_t)w; a.a[5]=(uint64_t)(uint32_t)h; a.a[6]=fmt; a.a[7]=type; a.a[8]=(uint64_t)(uintptr_t)pixels;
    (void)gl64_trap(GL64_fn_glTexSubImage2D,&a);
}
API void glGenerateMipmap(GLenum target){ GL64Args a={{0}}; a.a[0]=target; (void)gl64_trap(GL64_fn_glGenerateMipmap,&a); }
API void glCompressedTexImage2D(GLenum target, GLint level, GLenum ifmt, GLsizei w, GLsizei h, GLint border, GLsizei imageSize, const void* data){
    GL64Args a={{0}}; a.a[0]=target; a.a[1]=(uint64_t)(uint32_t)level; a.a[2]=ifmt; a.a[3]=(uint64_t)(uint32_t)w; a.a[4]=(uint64_t)(uint32_t)h; a.a[5]=(uint64_t)(uint32_t)border; a.a[6]=(uint64_t)(uint32_t)imageSize; a.a[7]=(uint64_t)(uintptr_t)data;
    (void)gl64_trap(GL64_fn_glCompressedTexImage2D,&a);
}
API const GLubyte* glGetStringi(GLenum name, GLuint index){
    // GL 3.0+ extension enumeration: return the i-th name from our curated list
    // (the same set as the monolithic GL_EXTENSIONS string). wined3d walks this
    // to decide which renderer features are available.
    if (name == 0x1F03 /*GL_EXTENSIONS*/ && (int)index < G_EXT_COUNT)
        return (const GLubyte*)g_extList[index];
    return (const GLubyte*)"";
}

// ===========================================================================
// glXGetProcAddressARB — opengl32/winex11 resolve every gl*/glX* through here.
// Return our own wrapper for names we implement, or a harmless no-op stub for
// the rest (so an unimplemented call is silently ignored rather than crashing).
// ===========================================================================
typedef void (*GLproc)(void);

static void gl64_noop(void) { /* unimplemented GL entry — ignore for first light */ }

struct procEntry { const char* name; GLproc fn; };
#define E(n) { #n, (GLproc)n }
static const struct procEntry g_procs[] = {
    E(glXQueryVersion), E(glXQueryExtension), E(glXQueryExtensionsString),
    E(glXQueryServerString), E(glXGetClientString), E(glXChooseVisual),
    E(glXCreateContext), E(glXCreateContextAttribsARB), E(glXCreateNewContext),
    E(glXChooseFBConfig), E(glXGetFBConfigs), E(glXGetFBConfigAttrib),
    E(glXGetVisualFromFBConfig), E(glXGetConfig), E(glXMakeCurrent),
    E(glXMakeContextCurrent), E(glXSwapBuffers), E(glXDestroyContext),
    E(glXIsDirect), E(glXGetCurrentContext), E(glXGetCurrentDrawable),
    E(glXWaitGL), E(glXWaitX), E(glXSwapIntervalEXT), E(glXSwapIntervalMESA),
    E(glXSwapIntervalSGI),
    E(glClearColor), E(glClear), E(glClearDepth), E(glViewport), E(glEnable),
    E(glDisable), E(glShadeModel), E(glDepthFunc), E(glCullFace), E(glFrontFace),
    E(glHint), E(glFlush), E(glFinish), E(glGetError), E(glGetString),
    E(glGetIntegerv), E(glGetFloatv), E(glColor3f), E(glColor4f),
    E(glMatrixMode), E(glLoadIdentity), E(glPushMatrix), E(glPopMatrix),
    E(glFrustum), E(glOrtho), E(glTranslatef), E(glRotatef), E(glScalef),
    E(glMultMatrixf), E(glLightfv), E(glLightf), E(glMaterialfv), E(glMaterialf),
    E(glColorMaterial), E(glNormal3f), E(glBegin), E(glEnd), E(glVertex2f),
    E(glVertex3f),
    // --- programmable pipeline (wined3d / Direct3D) ---
    E(glCreateShader), E(glShaderSource), E(glCompileShader), E(glGetShaderiv),
    E(glGetShaderInfoLog), E(glDeleteShader), E(glCreateProgram), E(glAttachShader),
    E(glDetachShader), E(glBindAttribLocation), E(glLinkProgram), E(glGetProgramiv),
    E(glGetProgramInfoLog), E(glUseProgram), E(glDeleteProgram), E(glGetUniformLocation),
    E(glGetAttribLocation), E(glValidateProgram),
    E(glUniform1i), E(glUniform1f), E(glUniform2f), E(glUniform3f), E(glUniform4f),
    E(glUniform1fv), E(glUniform2fv), E(glUniform3fv), E(glUniform4fv), E(glUniform1iv),
    E(glUniformMatrix2fv), E(glUniformMatrix3fv), E(glUniformMatrix4fv),
    E(glGenBuffers), E(glBindBuffer), E(glBufferData), E(glBufferSubData),
    E(glDeleteBuffers), E(glMapBufferRange),
    E(glEnableVertexAttribArray), E(glDisableVertexAttribArray), E(glVertexAttribPointer),
    E(glGenVertexArrays), E(glBindVertexArray), E(glDeleteVertexArrays), E(glVertexAttrib4f),
    E(glDrawArrays), E(glDrawElements), E(glDrawRangeElements),
    E(glBlendFunc), E(glBlendFuncSeparate), E(glBlendEquation), E(glBlendEquationSeparate),
    E(glBlendColor), E(glColorMask), E(glDepthMask),
    E(glStencilFunc), E(glStencilOp), E(glStencilMask),
    E(glStencilFuncSeparate), E(glStencilOpSeparate), E(glStencilMaskSeparate),
    E(glScissor), E(glPolygonOffset), E(glPolygonMode), E(glDepthRange),
    E(glLineWidth), E(glPixelStorei), E(glSampleCoverage),
    E(glActiveTexture), E(glGenTextures), E(glBindTexture), E(glDeleteTextures),
    E(glTexParameteri), E(glTexParameterf), E(glTexImage2D), E(glTexSubImage2D),
    E(glGenerateMipmap), E(glCompressedTexImage2D), E(glGetStringi),
};
#undef E
#define NPROCS (sizeof(g_procs)/sizeof(g_procs[0]))

API GLproc glXGetProcAddressARB(const GLubyte* name) {
    if (!name) return 0;
    for (unsigned i = 0; i < NPROCS; i++) {
        if (strcmp(g_procs[i].name, (const char*)name) == 0) {
            // hit=1: a real wrapper. Trace so BW64_GLTRACE shows wined3d's
            // resolved-and-implemented set.
            GL64Args a = {{0}}; a.a[0] = (uint64_t)(uintptr_t)name; a.a[1] = 1;
            (void)gl64_trap(GL64_fn_traceProc, &a);
            return g_procs[i].fn;
        }
    }
    // Unknown gl* function: trace it (hit=0) so the host log names exactly which
    // modern-GL entry point wined3d wanted that we don't implement yet — the
    // worklist for D3D. Then hand back a no-op so opengl32's dispatch fills its
    // table with a callable pointer (returning 0 would make wine think the
    // driver is broken and disable OpenGL).
    GL64Args a = {{0}}; a.a[0] = (uint64_t)(uintptr_t)name; a.a[1] = 0;
    (void)gl64_trap(GL64_fn_traceProc, &a);
    return gl64_noop;
}
API GLproc glXGetProcAddress(const GLubyte* name) { return glXGetProcAddressARB(name); }

// Full ALL_WGL_FUNCS coverage so wine's init_opengl dlsym loop succeeds.
#include "libgl64_stubs.h"
