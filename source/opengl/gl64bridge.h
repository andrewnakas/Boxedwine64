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

// Host side of the 64-bit OpenGL bridge. The guest libGL.so.1 traps into the
// kernel via GL64_SYSCALL_NR; ksyscall64 forwards here. We marshal the fixed
// GL64Args block to native macOS OpenGL, rendering into an offscreen host GL
// context and presenting completed frames through the existing X11-wire
// presentation sink (no SDL-renderer/GL-context conflict on the main window).

#ifndef __GL64BRIDGE_H__
#define __GL64BRIDGE_H__

class CPU64;
class KMemory;

// Guest-memory access the bridge needs, abstracted so 64-bit (KMemory64) AND
// 32-bit (KMemory) guest processes share ONE bridge (M23). ptrSize is the
// guest pointer width, for the few calls that read arrays of guest pointers.
struct GLGuestMem {
    virtual ~GLGuestMem() {}
    virtual void memcpyFromGuest(void* dst, U64 src, U64 len) = 0;
    virtual void memcpyToGuest(U64 dst, const void* src, U64 len) = 0;
    virtual void writed(U64 addr, U32 value) = 0;
    virtual void writeq(U64 addr, U64 value) = 0;
    U32 ptrSize = 8;
};

// Entry point from ksyscall64. fnId = RDI, argsAddr = RSI (guest VA of a
// GL64Args). Returns the call's result (or 0 for void) to be placed in RAX.
U64 gl64Bridge(CPU64* cpu, U64 fnId, U64 argsAddr);

// Entry point from the 32-bit ksyscall (EAX = GL64_SYSCALL_NR via int 0x80 from
// the i386 libgl32 shim): fnId = EBX, argsAddr = ECX. Same GL64Args block (u64
// slots); the 64-bit result goes back in EDX:EAX.
U64 gl32Bridge(KMemory* memory, U32 fnId, U32 argsAddr);

#endif // __GL64BRIDGE_H__
