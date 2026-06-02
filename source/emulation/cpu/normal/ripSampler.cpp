/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "boxedwine.h"

// The sampler only exists in the multi-threaded x86-64 guest build — it relies
// on each guest thread driving its own CPU64 on its own host thread (so a
// background thread can sample their per-thread {rip, instructionCount}). In any
// other build configuration every entry point is an inert stub so callers don't
// need their own #ifdefs.
#if defined(BOXEDWINE_MULTI_THREADED) && defined(BOXEDWINE_GUEST_X64)

#include "ripSampler.h"
#include "cpu64.h"
#include "ksystem.h"

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <cstdlib>

// ---------------------------------------------------------------------------
// Tunables (overridable via env so a run can be retargeted without a rebuild).
// ---------------------------------------------------------------------------
static const int  SAMPLE_INTERVAL_MS = 30;   // sampler wake cadence

// A thread is "busy" in a tick if it advanced more than this many instructions
// since the previous sample. ~2M insn / 30ms is a tiny fraction of the interp's
// throughput but far above the handful of instructions a genuinely-idle (but
// not-yet-blocked) thread retires between samples.
static U64 g_spinInsnThreshold = 2000000ULL;

// The accumulated rip window over consecutive busy ticks must stay narrower than
// this for the thread to count as a tight spin (one loop body, not normal code
// making forward progress through many functions).
static U64 g_spinWindow = 4096ULL;

// Consecutive busy+narrow ticks required before we report (debounce against a
// transient hot loop that's actually making progress). 4 * 30ms ~= 120ms.
static const U32 SPIN_TICKS_REQUIRED = 4;

// ---------------------------------------------------------------------------
// Env gate — read once.
// ---------------------------------------------------------------------------
bool ripSamplerEnabled() {
    static const bool on = []() {
        bool e = std::getenv("BW64_RIPSAMPLE") != nullptr;
        if (e) {
            if (const char* t = std::getenv("BW64_RIPSAMPLE_THRESH"))
                g_spinInsnThreshold = std::strtoull(t, nullptr, 0);
            if (const char* w = std::getenv("BW64_RIPSAMPLE_WINDOW"))
                g_spinWindow = std::strtoull(w, nullptr, 0);
        }
        return e;
    }();
    return on;
}

// ---------------------------------------------------------------------------
// Live-CPU64 registry. One slot per guest host-thread. The sampler reads
// slot.cpu (an atomic pointer, so it never tears) and then reads cpu->rip /
// cpu->instructionCount RACILY — those are plain U64s written only by the
// owning thread. A stale/torn sample over a 30ms window is harmless for a
// statistical profiler; we only ever ask "did rip stay in a narrow window while
// instructionCount climbed", which a one-sample-old value answers just as well.
// ---------------------------------------------------------------------------
#define RIP_MAX_SLOTS 256
struct RipSlot {
    std::atomic<CPU64*> cpu{nullptr};
    int pid = -1;
    int tid = -1;
};
static RipSlot     g_slots[RIP_MAX_SLOTS];
static std::mutex  g_slotMutex;       // guards acquire/release scans only

// Sampler-thread-private per-slot state (never touched by guest threads).
struct LastSample {
    U64  rip = 0;
    U64  icount = 0;
    U64  ripLo = 0;
    U64  ripHi = 0;
    U32  spinTicks = 0;
    bool reported = false;
};
static LastSample g_last[RIP_MAX_SLOTS];

// ---------------------------------------------------------------------------
// Module-range table: per-pid [lo,hi) -> path, so a sampled rip resolves to
// "<basename>+0x<off>". Populated at module-load time, queried off the hot path.
// ---------------------------------------------------------------------------
struct ModRange { U64 lo; U64 hi; std::string path; };
static std::map<int, std::vector<ModRange>> g_modRanges;
static std::mutex g_modMutex;

void ripSamplerNoteModule(int pid, U64 lo, U64 len, const std::string& path) {
    if (!ripSamplerEnabled() || len == 0) return;
    std::lock_guard<std::mutex> lk(g_modMutex);
    g_modRanges[pid].push_back({lo, lo + len, path});
}

void ripSamplerClearPid(int pid) {
    if (!ripSamplerEnabled()) return;
    std::lock_guard<std::mutex> lk(g_modMutex);
    g_modRanges.erase(pid);
}

static bool ripSamplerResolve(int pid, U64 rip, std::string& outBase, U64& outOff) {
    std::lock_guard<std::mutex> lk(g_modMutex);
    auto it = g_modRanges.find(pid);
    if (it == g_modRanges.end()) return false;
    // Most-recently-mapped wins on overlap (later MAP_FIXED sub-segments of a DSO
    // re-cover earlier whole-span reservations) — scan newest-first.
    for (auto r = it->second.rbegin(); r != it->second.rend(); ++r) {
        if (rip >= r->lo && rip < r->hi) {
            size_t slash = r->path.find_last_of('/');
            outBase = (slash == std::string::npos) ? r->path : r->path.substr(slash + 1);
            outOff = rip - r->lo;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// The background sampler thread.
// ---------------------------------------------------------------------------
static void ripSamplerLoop() {
    // Heartbeat/summary mode (BW64_RIPSAMPLE_HB): once per ~2s, dump EVERY live
    // slot's per-tick instructionCount delta + current rip+module, regardless of
    // the spin latch. This is the ground-truth probe: if a thread burns CPU but
    // its instructionCount is NOT climbing, the spin is NOT in the guest
    // interpreter loop (it's host-side — our syscall/epoll/futex handling — or
    // the thread is blocked), which changes the whole diagnosis. Also confirms
    // the sampler thread is alive and slots are populated.
    static const bool hb = std::getenv("BW64_RIPSAMPLE_HB") != nullptr;
    U64 hbAccumMs = 0;
    klog_fmt("RIPSAMPLE: sampler thread started (threshold=%llu window=%llu hb=%d)",
             (unsigned long long)g_spinInsnThreshold, (unsigned long long)g_spinWindow, (int)hb);
    while (!KSystem::shutingDown) {
        std::this_thread::sleep_for(std::chrono::milliseconds(SAMPLE_INTERVAL_MS));
        hbAccumMs += SAMPLE_INTERVAL_MS;
        bool emitHb = hb && hbAccumMs >= 2000;
        if (emitHb) hbAccumMs = 0;
        for (int i = 0; i < RIP_MAX_SLOTS; i++) {
            CPU64* c = g_slots[i].cpu.load(std::memory_order_relaxed);
            if (!c) { g_last[i].spinTicks = 0; g_last[i].reported = false; continue; }

            U64 rip = c->rip;                 // racy sample — see registry note
            U64 ic  = c->instructionCount;
            LastSample& L = g_last[i];
            U64 dIns = ic - L.icount;

            if (emitHb) {
                std::string hbBase; U64 hbOff = 0;
                bool res = ripSamplerResolve(g_slots[i].pid, rip, hbBase, hbOff);
                klog_fmt("RIPSAMPLE HB slot=%d pid=%d tid=%d dIns=%llu rip=0x%llx %s%s0x%llx",
                         i, g_slots[i].pid, g_slots[i].tid, (unsigned long long)dIns,
                         (unsigned long long)rip,
                         res ? hbBase.c_str() : "(unmapped)", res ? "+" : " @",
                         (unsigned long long)(res ? hbOff : rip));
            }

            L.rip = rip;
            L.icount = ic;

            bool busy = dIns > g_spinInsnThreshold;
            if (!busy) {
                L.spinTicks = 0;
                L.reported = false;
                continue;
            }

            // Accumulate the rip window across consecutive busy ticks.
            if (L.spinTicks == 0) { L.ripLo = rip; L.ripHi = rip; }
            else { if (rip < L.ripLo) L.ripLo = rip; if (rip > L.ripHi) L.ripHi = rip; }
            L.spinTicks++;

            U64 span = L.ripHi - L.ripLo;
            if (span >= g_spinWindow) {
                // Wandered out of a tight loop — a real hot path making progress.
                // Re-seed the window on the current rip and clear the latch.
                L.ripLo = rip; L.ripHi = rip; L.spinTicks = 1; L.reported = false;
                continue;
            }

            if (L.spinTicks >= SPIN_TICKS_REQUIRED && !L.reported) {
                L.reported = true;
                int pid = g_slots[i].pid;
                int tid = g_slots[i].tid;
                std::string base;
                U64 off = 0;
                if (ripSamplerResolve(pid, L.ripLo, base, off)) {
                    klog_fmt("RIPSAMPLE SPIN pid=%d tid=%d insn/tick=%llu "
                             "window=[0x%llx,0x%llx] span=%llu file=%s+0x%llx rip=0x%llx",
                             pid, tid, (unsigned long long)dIns,
                             (unsigned long long)L.ripLo, (unsigned long long)L.ripHi,
                             (unsigned long long)span, base.c_str(),
                             (unsigned long long)off, (unsigned long long)rip);
                } else {
                    klog_fmt("RIPSAMPLE SPIN pid=%d tid=%d insn/tick=%llu "
                             "window=[0x%llx,0x%llx] span=%llu (unmapped) rip=0x%llx",
                             pid, tid, (unsigned long long)dIns,
                             (unsigned long long)L.ripLo, (unsigned long long)L.ripHi,
                             (unsigned long long)span, (unsigned long long)rip);
                }
            }
        }
    }
}

static std::once_flag g_startOnce;
static void ripSamplerEnsureStarted() {
    std::call_once(g_startOnce, []() {
        std::thread(ripSamplerLoop).detach();
    });
}

// ---------------------------------------------------------------------------
// Registry register / publish / release (called from platformThread).
// ---------------------------------------------------------------------------
int ripSamplerAcquireSlot(int pid, int tid) {
    if (!ripSamplerEnabled()) return -1;
    std::lock_guard<std::mutex> lk(g_slotMutex);
    for (int i = 0; i < RIP_MAX_SLOTS; i++) {
        if (g_slots[i].cpu.load(std::memory_order_relaxed) == nullptr && g_slots[i].pid == -1) {
            g_slots[i].pid = pid;
            g_slots[i].tid = tid;
            g_last[i] = LastSample{};
            ripSamplerEnsureStarted();
            return i;
        }
    }
    return -1;
}

void ripSamplerPublish(int slot, CPU64* cpu) {
    if (slot < 0) return;
    g_slots[slot].cpu.store(cpu, std::memory_order_relaxed);
}

void ripSamplerReleaseSlot(int slot) {
    if (slot < 0) return;
    std::lock_guard<std::mutex> lk(g_slotMutex);
    g_slots[slot].cpu.store(nullptr, std::memory_order_relaxed);
    g_slots[slot].pid = -1;
    g_slots[slot].tid = -1;
    g_last[slot] = LastSample{};
}

#else // not (MULTI_THREADED && GUEST_X64) — inert stubs

#include "ripSampler.h"
class CPU64;
bool ripSamplerEnabled() { return false; }
int  ripSamplerAcquireSlot(int, int) { return -1; }
void ripSamplerPublish(int, CPU64*) {}
void ripSamplerReleaseSlot(int) {}
void ripSamplerNoteModule(int, U64, U64, const std::string&) {}
void ripSamplerClearPid(int) {}

#endif
