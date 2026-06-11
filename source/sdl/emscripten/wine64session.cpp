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

// wine64session.cpp — "persistent wine session" for the browser (wasm64-mt).
//
// The roadmap item: load a new app into the SAME running wine, instead of
// reloading the page and re-booting wine64 per app. The pieces that make this
// possible already exist in Boxedwine:
//
//   * The emscripten main loop (source/sdl/emscripten/mainloop.cpp) runs via
//     emscripten_set_main_loop and only stops when SDL_QUIT is posted —
//     postQuit() does that when platformThreadCount hits 0 (the last guest
//     thread exited). So as long as ONE guest process stays alive, the kernel,
//     the X11 wire server, the GL context and the wine prefix all stay resident.
//
//   * KProcess::create()->startProcess(...) spawns a brand-new guest process
//     into the live kernel and scheduleThread()s it (platformThreadCount++),
//     exactly as the first app was launched in StartUpArgs::apply().
//
// So a persistent session is:
//   1. Boot wine64's wineserver as a long-lived FOREGROUND process
//      (`wineserver64 -f -p`): -f = don't fork/daemonize (the in-emulator
//      daemonize path is unreliable — see wine64-launcher's ?boot=1 note), -p =
//      persist forever. This is the process that keeps platformThreadCount > 0
//      between apps, so the loop never quits and the prefix stays warm.
//   2. Spawn each app as `wine64 <app>` against that running server, via
//      bw64_spawn() below, with NO page reload.
//
// THREADING. The spawn must run on the main-loop thread (it mutates kernel
// structures the loop also touches and calls KThread::setCurrentThread). JS calls
// the exported bw64_spawn() — under PROXY_TO_PTHREAD that runs on the BROWSER
// main thread, where Atomics.wait is forbidden, so we must NOT block there. So
// bw64_spawn just enqueues the request behind an atomic_flag spinlock (no OS
// wait) and returns; the main loop drains the queue each tick via
// bw64SessionDrainSpawns() and does the real startProcess there.

#if defined(__EMSCRIPTEN__) && defined(BOXEDWINE_MULTI_THREADED) && defined(BOXEDWINE_GUEST_X64)

#include <emscripten/emscripten.h>
#include "boxedwine.h"
#include "kprocess.h"
#include "ksystem.h"
#include "kthread.h"
#include "ksignal.h" // K_SIGTERM
#include "../../io/fs.h"      // Fs::getNodeFromLocalPath / addFileNode (VFS registration)
#include "../../io/fsnode.h"

#include <atomic>
#include <vector>
#include <string>

// --- captured session context -------------------------------------------------
// StartUpArgs::apply() (the boot path) fills these once, so later spawns reuse
// the EXACT working dir / uids / env the first (booted) process ran with. Reusing
// the captured boot env is important: a spawned app must get the identical wine
// environment (HOME, WINEPREFIX, WINESERVER, WINEDLLPATH, PATH, USER, …) the boot
// app got, or wine inside it can't find the prefix / wineserver socket and hangs
// after connecting to X11 (observed: glcube stalled with only envc=6 vs the boot
// app's envc=16). So the bridge ignores any env passed from JS and always uses
// g_sessionCtx.env.
struct Wine64SessionCtx {
    std::atomic<bool> valid{false};
    BString workingDir;
    std::vector<BString> env;
    int userId = 0;
    int groupId = 0;
    int effectiveUserId = 0;
    int effectiveGroupId = 0;
};
static Wine64SessionCtx g_sessionCtx;

// A queued in-session command: either spawn an app or kill a process. Both run
// on the main-loop thread, in order, so a "kill old app then spawn new app"
// switch is sequenced correctly.
struct Wine64Req {
    enum Kind { Spawn, Kill, RegisterFile } kind = Spawn;
    std::vector<BString> argv;   // Spawn
    std::vector<BString> env;    // Spawn
    U32 pid = 0;                 // Kill
    BString path;                // RegisterFile (guest local path, e.g. /home/username/foo.exe)
};

// Pending requests, guarded by an atomic_flag spinlock. A spinlock (busy CAS, no
// futex) is used deliberately: bw64_spawn()/bw64_kill() run on the browser main
// thread, where a blocking lock (Atomics.wait) is illegal. The critical section
// is tiny (a vector move), so the spin is negligible.
static std::vector<Wine64Req> g_pending;
static std::atomic_flag g_pendingLock = ATOMIC_FLAG_INIT;

// The pid of the most recently spawned app, published for JS so it can ask us to
// kill the previous app when the user switches (close-previous-on-switch).
static std::atomic<U32> g_lastSpawnedPid{0};

static void pendingLockAcquire() { while (g_pendingLock.test_and_set(std::memory_order_acquire)) {} }
static void pendingLockRelease() { g_pendingLock.clear(std::memory_order_release); }

// Called from StartUpArgs::apply() right before doMainLoop so the spawn bridge
// has the same launch parameters (incl. the FULL env) the boot process used.
void bw64SessionRememberContext(const BString& workingDir, const std::vector<BString>& env,
                                int userId, int groupId,
                                int effectiveUserId, int effectiveGroupId, U32 bootPid) {
    g_sessionCtx.workingDir = workingDir;
    g_sessionCtx.env = env;
    g_sessionCtx.userId = userId;
    g_sessionCtx.groupId = groupId;
    g_sessionCtx.effectiveUserId = effectiveUserId;
    g_sessionCtx.effectiveGroupId = effectiveGroupId;
    // Seed the "current app" pid with the boot app, so the FIRST switch closes
    // notepad (the booted app) and not nothing. Later bridge spawns update it.
    g_lastSpawnedPid.store(bootPid, std::memory_order_release);
    g_sessionCtx.valid.store(true, std::memory_order_release);
    klog_fmt("wine64session: context captured (cwd=%s uid=%d bootPid=%d)",
             workingDir.c_str(), userId, (int)bootPid);
}

// JS hands argv and env as '\n'-joined strings (newlines never appear in a
// program path or a wine env value we set, and this avoids quoting headaches).
static std::vector<BString> splitLines(const char* joined) {
    std::vector<BString> out;
    if (!joined) return out;
    std::string s(joined);
    size_t start = 0;
    while (start <= s.size()) {
        size_t nl = s.find('\n', start);
        std::string piece = (nl == std::string::npos) ? s.substr(start) : s.substr(start, nl - start);
        if (!piece.empty()) out.push_back(BString::copy(piece.c_str()));
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return out;
}

// Defined in source/x11wire/xwireserver.cpp (calls XWireServer::resetForAppSwitch);
// forward-declared here to avoid pulling the whole XWire header into this bridge.
void bw64ResetPresentForSwitch();
// Defined in source/x11wire/xwireserver.cpp: erase a killed app's host-side X
// presence (connections, windows, pixmaps) + wipe the canvas if it owned it.
void bw64DropAppPresenceByPid(uint32_t pid);
// Defined in source/x11wire/xwireserver.cpp: the guest pid that owns the window
// being switched away from (captured when the switch armed), one-shot consume.
uint32_t bw64TakeOutgoingAppPid();

// The actual spawn — runs on the main-loop thread (see THREADING note).
static void doSpawn(const Wine64Req& req) {
    if (req.argv.empty()) {
        klog("wine64session: spawn with empty argv — ignoring");
        return;
    }
    klog_nonewline("wine64session: spawning ");
    for (auto& a : req.argv) klog_nonewline_fmt("\"%s\" ", a.c_str());
    klog_nonewline("\n");

    // startProcess() sets the calling thread's "current thread" to the new
    // process's first thread. On the main-loop thread that current-thread slot
    // is whatever the previous spawn/boot left; save and restore it so the loop
    // keeps a consistent view (mirrors ChangeThread in kthread.cpp).
    KThread* saved = KThread::currentThread();
    {
        KProcessPtr process = KProcess::create();
        // Always use the captured BOOT env (not req.env) so a spawned app gets
        // the identical wine environment the boot app had — see Wine64SessionCtx.
        KThread* thread = process->startProcess(g_sessionCtx.workingDir, req.argv, g_sessionCtx.env,
                                                g_sessionCtx.userId, g_sessionCtx.groupId,
                                                g_sessionCtx.effectiveUserId,
                                                g_sessionCtx.effectiveGroupId);
        if (!thread) {
            klog("wine64session: startProcess FAILED");
        } else {
            g_lastSpawnedPid.store(process->id, std::memory_order_release);
            klog_fmt("wine64session: spawned pid=%d", (int)process->id);
        }
    }
    KThread::setCurrentThread(saved);
}

// Kill a previously-spawned app (close-previous-on-switch). Runs on the
// main-loop thread.
//
// HOW (and why not a signal): delivering a guest SIGTERM means running wine's
// guest signal handler mid-session — the path that crashed the runtime
// (WebAssembly.Exception + wineserver protocol errors) when kill-on-switch was
// first attempted. Instead this terminates the process KERNEL-SIDE, the same
// way exit_group does: mark every thread `terminating` and wake any wait it's
// parked in. Each thread's own platform pthread then unwinds and the LAST one
// runs the full process cleanup in KProcess::deleteThread (fds close — so the
// persistent wineserver sees its client die, like any crashed Windows app —
// RAM released, process erased). CPU-bound threads that never block notice the
// flag via the periodic check in CPU64::run(). Deliberately NON-blocking: we
// don't wait for the threads to finish dying (terminateOtherThread's wait loop
// would wedge the main loop if a thread were stuck), we just flag + wake and
// let them exit on their own pthreads.
//
// The host-side X presence (its connections/windows on our wire server) is
// dropped immediately — the dying process never tells the X server anything.
static bool terminatePid(U32 pid, const char* why) {
    if (!pid) return false;
    KProcessPtr process = KSystem::getProcess(pid);
    if (!process || process->terminated) {
        klog_fmt("wine64session: kill pid=%d (%s) — already gone", (int)pid, why);
        bw64DropAppPresenceByPid(pid);  // in case its X state still lingers
        return false;
    }
    // Never kill the session's plumbing, whatever pid bookkeeping claims: the
    // wineserver (pinned persistent — killing it ends the session) or wine's
    // service processes. The X-derived pid below SHOULD always be a real app,
    // but a guard here is cheap and the failure mode without it is fatal.
    BString cmd = process->commandLine;
    if (cmd.contains("wineserver") || cmd.contains("services.exe") ||
        cmd.contains("winedevice") || cmd.contains("plugplay") ||
        cmd.contains("rpcss") || cmd.contains("explorer.exe")) {
        klog_fmt("wine64session: REFUSING to kill pid=%d (%s) — system process (%s)",
                 (int)pid, cmd.c_str(), why);
        return false;
    }
    klog_fmt("wine64session: killing previous app pid=%d (%s) [%s]", (int)pid,
             cmd.c_str(), why);
    process->iterateThreads([](KThread* thread) {
        thread->terminating = true;
        // Wake the thread if it's parked in a guest wait so it sees the flag
        // (same wake terminateOtherThread uses, minus the blocking wait).
        const std::shared_ptr<BoxedWineCondition> cond = thread->waitingCond;
        if (cond) {
            cond->lock();
            cond->signal();
            cond->unlock();
        }
        return true;
    });
    bw64DropAppPresenceByPid(pid);
    return true;
}

static void doKill(U32 pid) {
    // The pid JS tracked (the spawn root). For an in-session spawn this IS the
    // app; for the boot app it's usually a relay ancestor that already exited.
    terminatePid(pid, "launcher-tracked pid");
    // The pid that owns the window being switched away from, per the X server —
    // authoritative even when wine relayed the app through intermediate
    // processes (observed: the BOOT notepad's GUI lived in a different pid than
    // the boot spawn, so killing only the tracked pid left it running hidden).
    U32 xpid = bw64TakeOutgoingAppPid();
    if (xpid && xpid != pid) {
        terminatePid(xpid, "X window owner");
    }
}

// Register a file that JS just wrote directly into the Emscripten MEMFS (e.g. an
// uploaded .exe) into Boxedwine's VFS, so newly-spawned wine processes can find
// it. WHY THIS IS NEEDED: FsNode::loadChildren() scans a directory's native
// contents exactly ONCE (gated by hasLoadedChildrenFromFileSystem) and caches the
// child nodes; the prefix dir /home/username is scanned at boot. A later
// Module.FS.writeFile() lands in MEMFS but is invisible to that cache, so wine's
// path resolver returns c0000135 ("cannot find binary"). Here we look up the
// parent dir node and, if the child isn't already registered, add a node for it
// (its native path = parent's native path + the remote-encoded name), matching
// what loadChildren() would have done. Runs on the main-loop thread.
static void doRegisterFile(const BString& guestPath) {
    if (guestPath.isEmpty()) return;
    // Split into parent dir + leaf name.
    int slash = guestPath.lastIndexOf('/');
    if (slash < 0) { klog_fmt("wine64session: registerFile bad path '%s'", guestPath.c_str()); return; }
    BString parentPath = (slash == 0) ? B("/") : guestPath.substr(0, slash);
    BString name = guestPath.substr(slash + 1);
    if (name.isEmpty()) return;

    std::shared_ptr<FsNode> parent = Fs::getNodeFromLocalPath(B(""), parentPath, false);
    if (!parent) {
        klog_fmt("wine64session: registerFile parent '%s' not in VFS", parentPath.c_str());
        return;
    }
    // Force the parent's children to be loaded first (so getChildByName is valid
    // and we don't double-register what loadChildren already saw).
    if (parent->getChildByName(name)) {
        klog_fmt("wine64session: registerFile '%s' already present — ok", guestPath.c_str());
        return;
    }
    // Native path = parent's native path + the remote-encoded leaf name (mirrors
    // FsNode::loadChildren's localName->remoteName mapping for on-disk names).
    BString remoteName = name;
    Fs::localNameToRemote(remoteName);
    BString nativePath = parent->nativePath.stringByApppendingPath(remoteName);
    Fs::addFileNode(guestPath, B(""), nativePath, false, parent);
    klog_fmt("wine64session: registerFile '%s' -> native '%s' (registered)",
             guestPath.c_str(), nativePath.c_str());
}

// Drain queued requests. MUST be called from the main-loop thread (mainloop.cpp
// calls it once per tick). Swaps the queue out under the spinlock, then runs the
// requests outside the lock so kernel ops can take their own locks freely.
void bw64SessionDrainSpawns() {
    if (!g_sessionCtx.valid.load(std::memory_order_acquire)) return;
    std::vector<Wine64Req> todo;
    pendingLockAcquire();
    if (!g_pending.empty()) todo.swap(g_pending);
    pendingLockRelease();
    for (auto& req : todo) {
        if (req.kind == Wine64Req::Kill) doKill(req.pid);
        else if (req.kind == Wine64Req::RegisterFile) doRegisterFile(req.path);
        else doSpawn(req);
    }
}

// --- exported JS entry points -------------------------------------------------
// bw64_spawn: queue `wine64 <app>` to be launched into the running kernel.
// Called via Module.ccall("bw64_spawn","number",["string","string"],[argv,env]).
// Returns 1 if queued (session up), 0 if the session isn't ready yet. NON-
// blocking — safe to call from the browser main thread.
extern "C" EMSCRIPTEN_KEEPALIVE int bw64_spawn(const char* argvJoined, const char* envJoined) {
    if (!g_sessionCtx.valid.load(std::memory_order_acquire)) return 0;
    Wine64Req req;
    req.kind = Wine64Req::Spawn;
    req.argv = splitLines(argvJoined);
    req.env = splitLines(envJoined);
    if (req.argv.empty()) return 0;
    pendingLockAcquire();
    g_pending.push_back(std::move(req));
    pendingLockRelease();
    return 1;
}

// bw64_register_file: register a file JS already wrote into MEMFS (e.g. an
// uploaded .exe at guest path /home/username/foo.exe) into Boxedwine's VFS, so the
// app spawned right after can find it. Queue it (drained on the main-loop thread,
// in order) so it runs BEFORE the spawn JS queues next. NON-blocking; returns 1 if
// queued. Called via Module.ccall("bw64_register_file","number",["string"],[path]).
extern "C" EMSCRIPTEN_KEEPALIVE int bw64_register_file(const char* guestPath) {
    if (!g_sessionCtx.valid.load(std::memory_order_acquire) || !guestPath || !guestPath[0]) return 0;
    Wine64Req req;
    req.kind = Wine64Req::RegisterFile;
    req.path = BString::copy(guestPath);
    pendingLockAcquire();
    g_pending.push_back(std::move(req));
    pendingLockRelease();
    return 1;
}

// bw64_kill: queue a kernel-side terminate of a previously-spawned app pid
// (app switch kills the outgoing app). The launcher queues the NEW app's spawn
// first, then this — the FIFO drain keeps them ordered, so a failed spawn can
// still be retried against a live old app. NON-blocking. Returns 1 if queued.
extern "C" EMSCRIPTEN_KEEPALIVE int bw64_kill(int pid) {
    if (!g_sessionCtx.valid.load(std::memory_order_acquire) || pid <= 0) return 0;
    Wine64Req req;
    req.kind = Wine64Req::Kill;
    req.pid = (U32)pid;
    pendingLockAcquire();
    g_pending.push_back(std::move(req));
    pendingLockRelease();
    return 1;
}

// bw64_last_spawned_pid: the pid of the most recently spawned app, so JS can
// remember which process to kill on the next switch.
extern "C" EMSCRIPTEN_KEEPALIVE int bw64_last_spawned_pid() {
    return (int)g_lastSpawnedPid.load(std::memory_order_acquire);
}

// bw64_reset_present: clear the XWire base window so the next app's first window
// becomes the presented base (instead of compositing onto the closed app's
// larger window). Safe to call from the browser main thread — it locks the
// XWire registry mutex internally and touches no per-thread kernel state.
extern "C" EMSCRIPTEN_KEEPALIVE void bw64_reset_present() {
    bw64ResetPresentForSwitch();
}

// Whether the persistent session is up (context captured == boot completed).
extern "C" EMSCRIPTEN_KEEPALIVE int bw64_session_ready() {
    return g_sessionCtx.valid.load(std::memory_order_acquire) ? 1 : 0;
}

#endif
