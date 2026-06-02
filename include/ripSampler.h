/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#ifndef __RIP_SAMPLER_H__
#define __RIP_SAMPLER_H__

// BW64_RIPSAMPLE — an env-gated statistical profiler that finds a wine64 guest
// thread spinning in GUEST code during the GUI boot and attributes the hot RIP
// to a module (DSO/PE x86_64-unix .so) + offset.
//
// The motivating bug: in the failing notepad-GUI boots the main window never
// paints — the X-request stream freezes, the log stops growing, and host CPU
// sits at ~one core because a wine client thread is busy-looping in its OWN
// code (it is NOT blocked on the emulator; our X-wire answers requests fine).
// Because that thread makes NO syscalls, the syscall-driven BW64_MEMSTATS
// instrumentation is structurally blind to it. This sampler runs on a separate
// host thread, snapshots every live CPU64's {rip, instructionCount} every
// ~30ms, and flags the thread whose instructionCount keeps climbing while its
// rip stays in a tiny window — then prints "file=<dso>.so+0x<off>".
//
// Everything here is a no-op unless BW64_RIPSAMPLE is set in the environment,
// so it costs nothing in normal runs (and never perturbs the selftest).

#include <string>

class CPU64;

// True iff BW64_RIPSAMPLE is set (env read once, cached). All other entry
// points early-out on this, so callers can also gate their argument prep.
bool ripSamplerEnabled();

// Claim a registry slot for the calling guest thread (one host thread per guest
// thread in the MT build). Returns a slot handle to pass to Publish/Release, or
// -1 when disabled / no slot free. The first successful acquire lazily spawns
// the background sampler thread.
int  ripSamplerAcquireSlot(int pid, int tid);

// Publish the live CPU64 the calling thread is about to run. Must be called on
// EVERY platformThread loop iteration after re-fetching thread->cpu64, because
// execve() frees the old CPU64 and installs a fresh one; republishing keeps the
// slot pointing at the pointer the loop is actually running. No-op on slot==-1.
void ripSamplerPublish(int slot, CPU64* cpu);

// Release the slot as the thread exits (before its CPU64 is torn down).
void ripSamplerReleaseSlot(int slot);

// Record a loaded module segment [lo, lo+len) -> path for pid, so a sampled rip
// can be resolved to file+offset. Called from the file-backed mmap path (DSO/PE
// segments) and the ELF loader (the initial wine64/ld.so/glibc images).
void ripSamplerNoteModule(int pid, U64 lo, U64 len, const std::string& path);

// Drop all module ranges for pid (execve replaces the address space — stale
// ranges would mis-attribute the next image's RIPs).
void ripSamplerClearPid(int pid);

#endif // __RIP_SAMPLER_H__
