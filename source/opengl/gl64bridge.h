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

// Entry point from ksyscall64. fnId = RDI, argsAddr = RSI (guest VA of a
// GL64Args). Returns the call's result (or 0 for void) to be placed in RAX.
U64 gl64Bridge(CPU64* cpu, U64 fnId, U64 argsAddr);

#endif // __GL64BRIDGE_H__
