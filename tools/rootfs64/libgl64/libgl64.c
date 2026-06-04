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
    GL64_fn_glVertex3f
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
    // so it returns 0 and we hand back our own stable strings. Good enough for
    // wined3d/version probes during first light.
    static const GLubyte* vendor   = (const GLubyte*)"Boxedwine64";
    static const GLubyte* renderer = (const GLubyte*)"Boxedwine64 GL (host passthrough)";
    static const GLubyte* version  = (const GLubyte*)"2.1 Boxedwine64";
    static const GLubyte* slv      = (const GLubyte*)"1.20";
    static const GLubyte* exts     = (const GLubyte*)"";
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
API void glGetIntegerv(GLenum pname, GLint* params) {
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
};
#undef E
#define NPROCS (sizeof(g_procs)/sizeof(g_procs[0]))

API GLproc glXGetProcAddressARB(const GLubyte* name) {
    if (!name) return 0;
    for (unsigned i = 0; i < NPROCS; i++) {
        if (strcmp(g_procs[i].name, (const char*)name) == 0) return g_procs[i].fn;
    }
    // Unknown gl* function: hand back a no-op so opengl32's dispatch fills its
    // table with a callable pointer (returning 0 would make wine think the
    // driver is broken and disable OpenGL).
    return gl64_noop;
}
API GLproc glXGetProcAddress(const GLubyte* name) { return glXGetProcAddressARB(name); }

// Full ALL_WGL_FUNCS coverage so wine's init_opengl dlsym loop succeeds.
#include "libgl64_stubs.h"
