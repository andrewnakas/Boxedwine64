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
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include "boxedwine.h"

// gl64bridge.cpp replaces this legacy 32-bit GL marshaling for the x64 guest,
// and these files include the native GL header (GLH) which the WASM/WebGL2 build
// does not provide -- so compile them out when building the 64-bit guest.
#if defined(BOXEDWINE_OPENGL) && !defined(BOXEDWINE_GUEST_X64)
#include GLH
#include "glcommon.h"

extern BHashTable<BString, void*> glFunctionMap;

const char* glIsLoaded[GL_FUNC_COUNT];

void glExtensionsLoaded() {


}
#endif
