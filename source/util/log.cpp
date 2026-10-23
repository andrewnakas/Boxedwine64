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
#include "knativethread.h"
#include "knativesystem.h"

#include <stdio.h>

#ifdef BOXEDWINE_MSVC
#include <Windows.h>
#endif

static BOXEDWINE_MUTEX logMutex;

void kpanic(const char* msg) {
    internal_kpanic(BString::copy(msg));
}

void internal_kpanic(BString msg) {
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(logMutex);
    if (KSystem::logFile.isOpen()) {
        KSystem::logFile.write(msg);
    }
    fwrite(msg.c_str(), 1, msg.length(), stderr);
    fflush(stderr);
#if defined(BOXEDWINE_MSVC) && defined(_DEBUG)
    OutputDebugStringA(msg.c_str());
#endif
    if (KSystem::videoOption == VIDEO_NORMAL) {
#ifndef _TEST
        KNativeSystem::exit(msg.c_str(), 1);
#endif
    } else {
        KNativeThread::sleep(5000);
    }
    exit(1);
}

void kwarn(const char* msg) {
    internal_log(BString::copy(msg) + "\n", stdout);
}

void klog(const char* msg) {
    internal_log(BString::copy(msg) + "\n", stdout);
}

void klog_nonewline(const char* msg) {
    internal_log(BString::copy(msg), stdout);
}

void internal_log(BString msg, FILE* f) {
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(logMutex);
    if (KSystem::logFile.isOpen()) {
        KSystem::logFile.write(msg);
    }
    fwrite(msg.c_str(), 1, msg.length(), f);
    fflush(f);
#if defined(BOXEDWINE_MSVC) && defined(_DEBUG)
    OutputDebugStringA(msg.c_str());
#endif
}

// ---- unknown-instruction recover policy (ported from Winbox 2b504cf99) ----
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <set>
static std::atomic<unsigned long long> g_recoverSkippedInstructions{0};
static std::atomic<unsigned long long> g_recoverLoggedRips{0};
static std::atomic<unsigned long long> g_recoverSigillFallbacks{0};
static std::mutex g_recoverRipMutex;
static std::set<std::pair<unsigned, U64>> g_recoverLoggedRipSet;

bool bw64RecoverEnabled() {
    const char* value = std::getenv("BW64_RECOVER");
    if (value && value[0] == '0') return false;
    if (value && value[0] == '1') return true;
    // Opt-in: skipping an unknown instruction keeps an app alive but can
    // silently corrupt its results; the default is an honest SIGILL.
    return false;
}

void bw64RecoverNoteSkippedInstruction(bool loggedRip) {
    g_recoverSkippedInstructions.fetch_add(1, std::memory_order_relaxed);
    if (loggedRip) g_recoverLoggedRips.fetch_add(1, std::memory_order_relaxed);
}

void bw64RecoverNoteSigillFallback() {
    g_recoverSigillFallbacks.fetch_add(1, std::memory_order_relaxed);
}

bool bw64RecoverClaimRip(unsigned architecture, U64 rip) {
    std::lock_guard<std::mutex> lock(g_recoverRipMutex);
    const auto key = std::make_pair(architecture, rip);
    if (g_recoverLoggedRipSet.find(key) != g_recoverLoggedRipSet.end() ||
        g_recoverLoggedRipSet.size() >= 200) return false;
    g_recoverLoggedRipSet.insert(key);
    return true;
}
