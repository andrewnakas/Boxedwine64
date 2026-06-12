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

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include "boxedwine.h"
#include "knativesystem.h"

#ifdef BOXEDWINE_MULTI_THREADED
#include "knativethread.h"
#ifdef BOXEDWINE_GUEST_X64
#include "../../x11wire/xwirepresent.h"
#endif

U32 getNextTimer();
void runTimers();

#ifdef BOXEDWINE_GUEST_X64
// Persistent in-browser wine session (wine64session.cpp): drain any app-launch
// requests JS queued via bw64_spawn and start them on THIS (main-loop) thread.
void bw64SessionDrainSpawns();
#endif

extern std::atomic<int> platformThreadCount;

static U32 lastTitleUpdate = 0;
static thread_local bool isMainThread;

extern int allocatedRamPages;

bool isMainthread() {
    return isMainThread;
}

static BString getSize(int pages)
{
    pages *= 4;
    if (pages < 2048) {
        return BString::valueOf(pages) + B("KB");
    }
    if (pages < 2048 * 1024) {
        return BString::valueOf(pages / 1024) + B("MB");
    }
    return BString::valueOf(pages / 1024 / 1024) + B("GB");
}
extern int allocatedRamPages;
void mainloop() {
    isMainThread = true;
#ifdef BOXEDWINE_GUEST_X64
        // LEGACY_GL_EMULATION registers GL.newRenderingFrameStarted() as a
        // preMainLoop hook that runs HERE, on this main-loop thread, every frame.
        // It indexes GL.currentContext's temp vertex buffers — but the gl64 bridge
        // creates its WebGL2 context with proxyContextToMainThread, so on this
        // thread GL.currentContext is the raw integer handle (GL.currentContextIsProxied
        // = true), not a context object. Its only guard is `if (!GL.currentContext)`,
        // which the nonzero integer passes, so it then does
        // `(integer).tempVertexBufferCounters1[i] = 0` -> "Cannot set properties of
        // undefined", which kills this main loop and deadlocks every glOnMain. Patch
        // the hook IN THIS THREAD's GL instance (the one the hook actually uses) to
        // also bail for a proxied context. Once (static guard); near-zero cost after.
        static bool framePatched = false;
        if (!framePatched) {
            framePatched = true;
            EM_ASM({
                try {
                    if (typeof GL !== 'undefined' && GL.newRenderingFrameStarted &&
                        !GL.newRenderingFrameStarted.__bw64Patched) {
                        var orig = GL.newRenderingFrameStarted;
                        GL.newRenderingFrameStarted = function() {
                            if (GL.currentContextIsProxied ||
                                !GL.currentContext ||
                                typeof GL.currentContext !== 'object') return;
                            // Our gl64 WebGL2 context is created via
                            // emscripten_webgl_create_context (NOT the SDL/Browser
                            // path), so glemu's per-context temp-vertex-buffer arrays
                            // don't exist on it. When a wined3d GLSL draw leaves OUR
                            // context current, orig() then does
                            // `currentContext.tempVertexBufferCounters1[i]=0` →
                            // "Cannot read/set properties of undefined", which throws
                            // out of the main loop and DEADLOCKS every later glOnMain
                            // (the D3D Present stall: one draw, then total silence).
                            // Bail when those arrays are absent.
                            if (!GL.currentContext.tempVertexBufferCounters1) return;
                            // Belt-and-suspenders: never let a throw from glemu's
                            // frame hook escape the main loop (that would deadlock
                            // every glOnMain forever).
                            try { return orig.apply(this, arguments); } catch (e) {}
                        };
                        GL.newRenderingFrameStarted.__bw64Patched = true;
                    }
                } catch (e) {}
            });
        }
        // Present the in-process X11 wire server's latest frame AND drain work the
        // gl64 bridge deferred to the main thread (WebGL context creation, FBO
        // setup, glReadPixels). The gl64 trap runs on a guest worker thread that
        // can't touch the thread-affine WebGL context, so it enqueues closures via
        // xwireRunOnMainThread and BLOCKS until drainMainThreadWork() (inside
        // tickXWirePresent) runs them here. Without this call those jobs never run
        // and the guest's glXCreateContext hangs forever (black canvas).
        tickXWirePresent();
        // Persistent wine session: start any apps JS asked for (bw64_spawn) here
        // on the main-loop thread, against the already-running wineserver/prefix.
        bw64SessionDrainSpawns();
#endif
        U32 t = KSystem::getMilliesSinceStart();
        U32 nextTimer = getNextTimer();
        if (nextTimer == 0) {
            runTimers();
        }

        if (lastTitleUpdate + 5000 < t) {
            lastTitleUpdate = t;
            BString title;
            if (KSystem::title.length()) {
                title = KSystem::title;
            } else {
                title = B("BoxedWine " BOXEDWINE_VERSION_DISPLAY);
            }

            title.append(" ");
            title.append(getSize(allocatedRamPages));
	    emscripten_set_window_title(title.c_str());
        }
        if (!KNativeSystem::getCurrentInput()->processEvents()) {
            KNativeSystem::cleanup();
        }
}

void waitForProcessToFinish(const std::shared_ptr<KProcess>& process, KThread* thread) {
    BOXEDWINE_CRITICAL_SECTION_WITH_CONDITION(KSystem::processesCond);
    while (!process->isTerminated()) {
        BOXEDWINE_CONDITION_WAIT(KSystem::processesCond);
    }
}

#else

static U32 lastTitleUpdate = 0;
static bool mainLoopTimingConfigured = false;

bool isMainthread() {
    return true;
}

void mainloop() {
    if (!mainLoopTimingConfigured) {
        emscripten_set_main_loop_timing(EM_TIMING_SETTIMEOUT, 1);
        mainLoopTimingConfigured = true;
    }
    U64 startTime = KSystem::getMicroCounter();
    U32 t;
    U32 count=0;
    BString mipsTitle;
    while (1) {
        bool ran = runSlice();

        KNativeSystem::tick();
        if (!KNativeSystem::getCurrentInput()->processEvents()) {
            KNativeSystem::cleanup();
            return;
        }
        t = KSystem::getMilliesSinceStart();                
        if (lastTitleUpdate+1000 < t) {
            lastTitleUpdate = t;
	    mipsTitle = B("BoxedWine ");
	    mipsTitle.append(getMIPS());
	    mipsTitle.append(" MIPS");
	    emscripten_set_window_title(mipsTitle.c_str());           
        }
        if (!ran) {
            break;
        }
        U64 diff = KSystem::getMicroCounter() - startTime;

        if (diff>10000 || KNativeSystem::getScreen()->presentedSinceLastCheck()) {
            if (diff > 100000) {
                klog_fmt("ran main loop in %dms", (U32)diff / 1000);
            }
            break;
        }
    };
}

#endif

bool doMainLoop() {
    EM_ASM(
#ifndef SDL2
            SDL.defaults.copyOnLock = false;
            SDL.defaults.discardOnLock = true;
#endif
            //SDL.defaults.opaqueFrontBuffer = false;
    );
    emscripten_set_main_loop(mainloop, 0, 1);
    return false;
}
#endif
