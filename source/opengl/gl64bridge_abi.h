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

// Shared ABI between the 64-bit guest libGL.so.1 (tools/rootfs64/libgl64) and
// the host marshaller (source/opengl/gl64bridge.cpp). This file is the single
// source of truth for:
//   - the private syscall number used to trap from guest to host
//   - the fixed-layout argument block passed across the trap
//   - the function-id enum identifying which GL/GLX entry point is being called
//
// It is plain C (compiles into both the host C++ tree and the guest C library),
// uses only fixed-width integers, and must stay self-contained (no other
// includes) so the guest build needs nothing from the host tree.
//
// Trap ABI (x86-64 Linux `syscall`):
//   RAX = GL64_SYSCALL_NR
//   RDI = function id (one of GL64_fn_*)
//   RSI = guest virtual address of a `struct GL64Args`
//   -> kernel returns the call's result (or 0 for void) in RAX
//
// Argument marshalling is uniform: every wrapper writes its scalar arguments
// into args.a[0..N-1] and traps. Conventions per slot:
//   - integer / enum / boolean args  : zero/sign-extended into the u64 slot
//   - 32-bit float (GLfloat)         : bit-cast to u32, stored in the low half
//   - 64-bit double (GLdouble)       : bit-cast to u64
//   - pointer args (guest buffers)   : the GUEST virtual address, as u64
// The host knows each function's signature from its id and reads the slots
// accordingly. Out-parameters (e.g. glGetIntegerv) are guest addresses the host
// writes back into via KMemory64::memcpyToGuest.

#ifndef __GL64BRIDGE_ABI_H__
#define __GL64BRIDGE_ABI_H__

#include <stdint.h>

// Private syscall number, well outside the Linux x86-64 ABI range (max ~547 as
// of 6.x). 'GL' = 0x474C in the high bits keeps it recognizable in logs and
// collision-free against any real syscall.
#define GL64_SYSCALL_NR  ((uint64_t)0x474C0000ULL)

// Fixed argument block. 16 slots comfortably covers every core-GL entry point
// used for first light (max real arity is well under that). Kept POD with a
// stable layout so guest and host agree byte-for-byte.
#define GL64_MAX_ARGS 16
typedef struct GL64Args {
    uint64_t a[GL64_MAX_ARGS];
} GL64Args;

// Function ids. Append-only — never renumber, the guest libGL and host bridge
// are compiled from this same list. Grouped: GLX/context bootstrap first, then
// core GL needed for a clear+triangle / glxgears.
enum {
    // --- GLX / context bootstrap ---------------------------------------
    GL64_fn_glXQueryVersion = 1,        // (out major*, out minor*) -> Bool
    GL64_fn_glXQueryExtension,          // (out errorBase*, out eventBase*) -> Bool
    GL64_fn_glXQueryExtensionsString,   // () -> const char* (host-owned, see note)
    GL64_fn_glXChooseVisual,            // (attribList*) -> XVisualInfo* (guest sees opaque non-null)
    GL64_fn_glXCreateContext,           // (vis*, share, direct) -> GLXContext (opaque id)
    GL64_fn_glXCreateContextAttribsARB, // (config, share, direct, attribs*) -> GLXContext
    GL64_fn_glXChooseFBConfig,          // (screen, attribs*, out nelements*) -> GLXFBConfig*
    GL64_fn_glXGetFBConfigs,            // (screen, out nelements*) -> GLXFBConfig*
    GL64_fn_glXGetFBConfigAttrib,       // (config, attribute, out value*) -> int
    GL64_fn_glXGetVisualFromFBConfig,   // (config) -> XVisualInfo*
    GL64_fn_glXGetConfig,               // (vis*, attribute, out value*) -> int
    GL64_fn_glXMakeCurrent,             // (drawable, ctx) -> Bool
    GL64_fn_glXMakeContextCurrent,      // (draw, read, ctx) -> Bool
    GL64_fn_glXSwapBuffers,             // (drawable) -> void
    GL64_fn_glXDestroyContext,          // (ctx) -> void
    GL64_fn_glXIsDirect,                // (ctx) -> Bool (always True)
    GL64_fn_glXGetCurrentContext,       // () -> GLXContext
    GL64_fn_glXGetCurrentDrawable,      // () -> GLXDrawable
    GL64_fn_glXQueryServerString,       // (name) -> const char*
    GL64_fn_glXGetClientString,         // (name) -> const char*
    GL64_fn_glXWaitGL,                  // () -> void
    GL64_fn_glXWaitX,                   // () -> void
    GL64_fn_glXSwapIntervalEXT,         // (drawable, interval) -> void

    // --- core GL: state ------------------------------------------------
    GL64_fn_glClearColor = 200,         // (r,g,b,a : float)
    GL64_fn_glClear,                    // (mask)
    GL64_fn_glClearDepth,               // (depth : double)
    GL64_fn_glViewport,                 // (x,y,w,h)
    GL64_fn_glEnable,                   // (cap)
    GL64_fn_glDisable,                  // (cap)
    GL64_fn_glShadeModel,               // (mode)
    GL64_fn_glDepthFunc,                // (func)
    GL64_fn_glCullFace,                 // (mode)
    GL64_fn_glFrontFace,                // (mode)
    GL64_fn_glHint,                     // (target, mode)
    GL64_fn_glFlush,                    // ()
    GL64_fn_glFinish,                   // ()
    GL64_fn_glGetError,                 // () -> GLenum
    GL64_fn_glGetString,               // (name) -> const char*
    GL64_fn_glGetIntegerv,             // (pname, out params*)
    GL64_fn_glGetFloatv,               // (pname, out params*)
    GL64_fn_glColor3f,                  // (r,g,b : float)
    GL64_fn_glColor4f,                  // (r,g,b,a : float)

    // --- core GL: matrices --------------------------------------------
    GL64_fn_glMatrixMode = 260,         // (mode)
    GL64_fn_glLoadIdentity,             // ()
    GL64_fn_glPushMatrix,               // ()
    GL64_fn_glPopMatrix,                // ()
    GL64_fn_glFrustum,                  // (l,r,b,t,n,f : double)
    GL64_fn_glOrtho,                    // (l,r,b,t,n,f : double)
    GL64_fn_glTranslatef,               // (x,y,z : float)
    GL64_fn_glRotatef,                  // (angle,x,y,z : float)
    GL64_fn_glScalef,                   // (x,y,z : float)
    GL64_fn_glMultMatrixf,              // (m* -> 16 floats)

    // --- core GL: lighting / material ---------------------------------
    GL64_fn_glLightfv = 290,            // (light, pname, params* -> up to 4 floats)
    GL64_fn_glLightf,                   // (light, pname, param : float)
    GL64_fn_glMaterialfv,               // (face, pname, params* -> up to 4 floats)
    GL64_fn_glMaterialf,                // (face, pname, param : float)
    GL64_fn_glColorMaterial,            // (face, mode)
    GL64_fn_glNormal3f,                 // (x,y,z : float)

    // --- core GL: immediate-mode geometry -----------------------------
    GL64_fn_glBegin = 320,              // (mode)
    GL64_fn_glEnd,                      // ()
    GL64_fn_glVertex2f,                 // (x,y : float)
    GL64_fn_glVertex3f,                 // (x,y,z : float)

    // === programmable pipeline (GL2/GLES3, for wined3d / Direct3D) =====
    // These are the modern-GL entry points wined3d resolves through
    // glXGetProcAddressARB. They marshal 1:1 onto WebGL2 (GLES3) on the host.
    // Guest pointer args are GUEST virtual addresses; the host reads/writes
    // them via KMemory64. Buffer-relative pointers (VertexAttribPointer
    // `pointer`, DrawElements `indices`) are plain integer OFFSETS into the
    // bound buffer when a VBO/IBO is bound — passed through as u64, no guest
    // read. Handles returned by CreateShader/CreateProgram are real host GL
    // names handed straight back to the guest.

    // --- diagnostics ---
    GL64_fn_traceProc = 400,            // (name* , hit) -> log a glXGetProcAddress resolution

    // --- shaders / programs ---
    GL64_fn_glCreateShader = 410,       // (type) -> GLuint
    GL64_fn_glShaderSource,             // (shader, count, string** , length*) reads guest strings
    GL64_fn_glCompileShader,            // (shader)
    GL64_fn_glGetShaderiv,              // (shader, pname, out params*)
    GL64_fn_glGetShaderInfoLog,         // (shader, bufSize, out length*, out infoLog*)
    GL64_fn_glDeleteShader,             // (shader)
    GL64_fn_glCreateProgram,            // () -> GLuint
    GL64_fn_glAttachShader,             // (program, shader)
    GL64_fn_glDetachShader,             // (program, shader)
    GL64_fn_glBindAttribLocation,       // (program, index, name*)
    GL64_fn_glLinkProgram,              // (program)
    GL64_fn_glGetProgramiv,             // (program, pname, out params*)
    GL64_fn_glGetProgramInfoLog,        // (program, bufSize, out length*, out infoLog*)
    GL64_fn_glUseProgram,               // (program)
    GL64_fn_glDeleteProgram,            // (program)
    GL64_fn_glGetUniformLocation,       // (program, name*) -> GLint
    GL64_fn_glGetAttribLocation,        // (program, name*) -> GLint
    GL64_fn_glValidateProgram,          // (program)

    // --- uniforms ---
    GL64_fn_glUniform1i = 440,          // (loc, v0)
    GL64_fn_glUniform1f,                // (loc, v0:float)
    GL64_fn_glUniform2f,                // (loc, v0,v1:float)
    GL64_fn_glUniform3f,                // (loc, v0,v1,v2:float)
    GL64_fn_glUniform4f,                // (loc, v0,v1,v2,v3:float)
    GL64_fn_glUniform1fv,               // (loc, count, value*)
    GL64_fn_glUniform2fv,               // (loc, count, value*)
    GL64_fn_glUniform3fv,               // (loc, count, value*)
    GL64_fn_glUniform4fv,               // (loc, count, value*)
    GL64_fn_glUniform1iv,               // (loc, count, value*)
    GL64_fn_glUniformMatrix2fv,         // (loc, count, transpose, value*)
    GL64_fn_glUniformMatrix3fv,         // (loc, count, transpose, value*)
    GL64_fn_glUniformMatrix4fv,         // (loc, count, transpose, value*)

    // --- buffers (VBO / IBO) ---
    GL64_fn_glGenBuffers = 470,         // (n, out buffers*)
    GL64_fn_glBindBuffer,               // (target, buffer)
    GL64_fn_glBufferData,               // (target, size, data*, usage) reads guest data
    GL64_fn_glBufferSubData,            // (target, offset, size, data*) reads guest data
    GL64_fn_glDeleteBuffers,            // (n, buffers*)
    GL64_fn_glMapBufferRange,           // unsupported in WebGL2; returns 0 (guest falls back)

    // --- vertex attrib arrays / VAO ---
    GL64_fn_glEnableVertexAttribArray = 490,  // (index)
    GL64_fn_glDisableVertexAttribArray,       // (index)
    GL64_fn_glVertexAttribPointer,            // (index, size, type, normalized, stride, offset)
    GL64_fn_glGenVertexArrays,                // (n, out arrays*)
    GL64_fn_glBindVertexArray,                // (array)
    GL64_fn_glDeleteVertexArrays,             // (n, arrays*)
    GL64_fn_glVertexAttrib4f,                 // (index, x,y,z,w:float)

    // --- draws ---
    GL64_fn_glDrawArrays = 510,         // (mode, first, count)
    GL64_fn_glDrawElements,             // (mode, count, type, indices-offset)
    GL64_fn_glDrawRangeElements,        // (mode, start, end, count, type, offset)

    // --- modern state ---
    GL64_fn_glBlendFunc = 530,          // (sfactor, dfactor)
    GL64_fn_glBlendFuncSeparate,        // (srcRGB, dstRGB, srcA, dstA)
    GL64_fn_glBlendEquation,            // (mode)
    GL64_fn_glBlendEquationSeparate,    // (modeRGB, modeAlpha)
    GL64_fn_glBlendColor,               // (r,g,b,a:float)
    GL64_fn_glColorMask,                // (r,g,b,a:bool)
    GL64_fn_glDepthMask,                // (flag)
    GL64_fn_glStencilFunc,              // (func, ref, mask)
    GL64_fn_glStencilOp,                // (fail, zfail, zpass)
    GL64_fn_glStencilMask,              // (mask)
    GL64_fn_glStencilFuncSeparate,      // (face, func, ref, mask)
    GL64_fn_glStencilOpSeparate,        // (face, fail, zfail, zpass)
    GL64_fn_glStencilMaskSeparate,      // (face, mask)
    GL64_fn_glScissor,                  // (x,y,w,h)
    GL64_fn_glPolygonOffset,            // (factor, units : float)
    GL64_fn_glPolygonMode,              // (face, mode) — no-op on GLES
    GL64_fn_glDepthRange,               // (near, far : double)
    GL64_fn_glLineWidth,                // (width : float)
    GL64_fn_glPixelStorei,              // (pname, param)
    GL64_fn_glSampleCoverage,           // (value:float, invert)

    // --- textures (modern) ---
    GL64_fn_glActiveTexture = 560,      // (texture)
    GL64_fn_glGenTextures,              // (n, out textures*)
    GL64_fn_glBindTexture,              // (target, texture)
    GL64_fn_glDeleteTextures,           // (n, textures*)
    GL64_fn_glTexParameteri,            // (target, pname, param)
    GL64_fn_glTexParameterf,            // (target, pname, param:float)
    GL64_fn_glTexImage2D,               // (target, level, ifmt, w, h, border, fmt, type, pixels*) reads guest pixels
    GL64_fn_glTexSubImage2D,            // (target, level, x, y, w, h, fmt, type, pixels*) reads guest pixels
    GL64_fn_glGenerateMipmap,           // (target)
    GL64_fn_glCompressedTexImage2D,     // (target, level, ifmt, w, h, border, imageSize, data*)

    // --- queries / strings ---
    GL64_fn_glGetStringi = 590,         // (name, index) -> const char* (returns 0; guest stub)
    GL64_fn_glGetShaderSource,          // (shader, bufSize, out length*, out source*) — rarely used

    GL64_fn__MAX
};

#endif // __GL64BRIDGE_ABI_H__
