/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */
#pragma once

#include <mutex>

// Serializes emulation of guest LOCK operations whose host address is not
// naturally aligned for the operand width. Such operations use ordinary
// loads/stores because WebAssembly atomics trap on misaligned addresses.
extern std::mutex g_splitLockMutex;
