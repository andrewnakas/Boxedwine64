#!/usr/bin/env python3
"""Apply the Boxedwine64 doom.exe patch set to a fresh doomgeneric checkout.

Implements PATCH #1 (window creation), #2 (timer/pacing/draw), #3 (mouse) as
documented in tools/rootfs64/games/build-games.sh, plus the M5 sound hookup
(FEATURE_SOUND + the i_winsound.c waveOut module, copied in by the caller).

Usage: python3 doom_apply_patches.py <doomgeneric/doomgeneric dir>
Every replacement is exact-match; the script dies loudly if upstream drifted.
"""
import sys
import os

d = sys.argv[1]


def patch(path, subs):
    p = os.path.join(d, path)
    s = open(p).read()
    for old, new in subs:
        if old not in s:
            sys.exit(f"ANCHOR MISSING in {path}:\n{old[:120]}")
        s = s.replace(old, new, 1)
    open(p, 'w').write(s)
    print(f"patched {path} ({len(subs)} edits)")


# ---- PATCH #1 + #2 + #3: doomgeneric_win.c ----
patch('doomgeneric_win.c', [
    # 1: real hInstance + fixed position (wine + WM-less XWire rejects the defaults)
    ("wc.hInstance = 0;",
     "wc.hInstance = GetModuleHandle(NULL);"),
    ("HWND hwnd = CreateWindowExA(0, windowClassName, windowTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, 0, 0, 0, 0);",
     "HWND hwnd = CreateWindowExA(0, windowClassName, windowTitle, WS_OVERLAPPEDWINDOW, 0, 0, rect.right - rect.left, rect.bottom - rect.top, 0, 0, GetModuleHandle(NULL), 0);"),
    # 2c: no SwapBuffers (drags the window into the GLX path; GDI blit must stay GDI)
    ("\tSwapBuffers(s_Hdc);\n", ""),
    # 2a/2b: QPC-backed real clock + no-op sleep + per-frame pacing
    ("""void DG_SleepMs(uint32_t ms)
{
	Sleep(ms);
}

uint32_t DG_GetTicksMs()
{
	return GetTickCount();
}""",
     """// Real wall-clock ms via QPC with a monotonic floor (GetTickCount was stuck
// under the emulator; a +1-per-call floor ran the game unpaced). See
// tools/rootfs64/games/build-games.sh PATCH #2.
static uint32_t dg_realMs(void)
{
	static LARGE_INTEGER s_freq;
	static uint32_t s_last = 0;
	LARGE_INTEGER now;
	uint32_t ms;
	if (s_freq.QuadPart == 0)
	{
		QueryPerformanceFrequency(&s_freq);
		if (s_freq.QuadPart == 0) s_freq.QuadPart = 1000;
	}
	QueryPerformanceCounter(&now);
	ms = (uint32_t)((now.QuadPart * 1000) / s_freq.QuadPart);
	if (ms <= s_last) ms = s_last; else s_last = ms;
	return ms;
}

void DG_SleepMs(uint32_t ms)
{
	(void)ms; // never block the TryRunTics spin; the QPC clock advances alone
}

uint32_t DG_GetTicksMs()
{
	return dg_realMs();
}

// Cap the outer frame loop to ~35 fps (called at the top of doomgeneric_Tick)
// without re-introducing the blocking-sleep stall inside TryRunTics.
void DG_PaceFrame(void)
{
	static uint32_t s_lastFrame = 0;
	const uint32_t frameMs = 1000 / 35;
	while (dg_realMs() - s_lastFrame < frameMs)
	{
		// busy-poll the real clock
	}
	s_lastFrame = dg_realMs();
}
"""),
    # 3: accumulator + DG_GetMouse must sit ABOVE wndProc (which references them)
    ("static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)",
     """// ---- PATCH #3: mouse accumulator (fed by wndProc, drained by i_input.c) ----
static int s_MouseButtons = 0;
static int s_MouseDX = 0;
static int s_MouseDY = 0;

int DG_GetMouse(int* buttons, int* dx, int* dy)
{
	*buttons = s_MouseButtons;
	*dx = s_MouseDX;
	*dy = s_MouseDY;
	s_MouseDX = 0;
	s_MouseDY = 0;
	return 1;
}

static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)"""),
    # 3: wndProc mouse cases (relative-from-center movement + button bits)
    ("""	case WM_KEYDOWN:
		addKeyToQueue(1, wParam);
		break;
	case WM_KEYUP:
		addKeyToQueue(0, wParam);
		break;""",
     """	case WM_KEYDOWN:
		addKeyToQueue(1, wParam);
		break;
	case WM_KEYUP:
		addKeyToQueue(0, wParam);
		break;
	case WM_MOUSEMOVE:
	{
		RECT rc;
		POINT center;
		int x = (int)(short)LOWORD(lParam);
		int y = (int)(short)HIWORD(lParam);
		GetClientRect(hwnd, &rc);
		center.x = (rc.right - rc.left) / 2;
		center.y = (rc.bottom - rc.top) / 2;
		s_MouseDX += x - center.x;
		s_MouseDY += y - center.y;
		break;
	}
	case WM_LBUTTONDOWN: s_MouseButtons |= 1; break;
	case WM_LBUTTONUP:   s_MouseButtons &= ~1; break;
	case WM_RBUTTONDOWN: s_MouseButtons |= 2; break;
	case WM_RBUTTONUP:   s_MouseButtons &= ~2; break;
	case WM_MBUTTONDOWN: s_MouseButtons |= 4; break;
	case WM_MBUTTONUP:   s_MouseButtons &= ~4; break;"""),
])

# ---- doomgeneric.h: declare the new DG_ entry points ----
patch('doomgeneric.h', [
    ("void DG_SetWindowTitle(const char * title);",
     """void DG_SetWindowTitle(const char * title);
void DG_PaceFrame(void);
int DG_GetMouse(int* buttons, int* dx, int* dy);"""),
])

# ---- PATCH #2d: d_main.c — graphics up BEFORE the first TryRunTics; frame pacing;
# unconditional display ----
patch('d_main.c', [
    ("""void doomgeneric_Tick()
{
    // frame syncronous IO operations
    I_StartFrame ();

    TryRunTics (); // will run at least one tic

    S_UpdateSounds (players[consoleplayer].mo);// move positional sounds

    // Update display, next frame, with current state.
    if (screenvisible)
    {
        D_Display ();
    }
}""",
     """void doomgeneric_Tick()
{
    DG_PaceFrame(); // ~35 fps outer cap (PATCH #2a2) — keeps input paced

    // frame syncronous IO operations
    I_StartFrame ();

    TryRunTics (); // will run at least one tic

    S_UpdateSounds (players[consoleplayer].mo);// move positional sounds

    // Update display, next frame, with current state (unconditional: the
    // WM-less XWire server never flips screenvisible).
    screenvisible = true;
    D_Display ();
}"""),
    ("""    main_loop_started = true;

    TryRunTics();

    I_SetWindowTitle(gamedescription);
    I_GraphicsCheckCommandLine();
    I_SetGrabMouseCallback(D_GrabMouseCallback);
    I_InitGraphics();
    I_EnableLoadingDisk();""",
     """    main_loop_started = true;

    // Graphics BEFORE the first TryRunTics (PATCH #2d): the first tic spin can
    // take a while under the interpreter; bring the window up first.
    I_SetWindowTitle(gamedescription);
    I_GraphicsCheckCommandLine();
    I_SetGrabMouseCallback(D_GrabMouseCallback);
    I_InitGraphics();
    I_EnableLoadingDisk();

    TryRunTics();"""),
])

# d_main.c needs the DG_PaceFrame declaration
patch('d_main.c', [
    ('#include "doomgeneric.h"', '#include "doomgeneric.h"')
    if '#include "doomgeneric.h"' in open(os.path.join(d, 'd_main.c')).read()
    else ('#include "d_main.h"', '#include "d_main.h"\n#include "doomgeneric.h"'),
])

# ---- PATCH #3: i_video.c usemouse + i_input.c mouse events ----
patch('i_video.c', [
    ("int usemouse = 0;", "int usemouse = 1;"),
])

patch('i_input.c', [
    ("""	while (DG_GetKey(&pressed, &key))""",
     """	{
		// PATCH #3: hand DOOM one mouse sample per frame as an ev_mouse
		int mb, mdx, mdy;
		if (DG_GetMouse(&mb, &mdx, &mdy) && (mb || mdx || mdy))
		{
			event_t mev;
			mev.type = ev_mouse;
			mev.data1 = mb;
			mev.data2 = mdx * 4;
			mev.data3 = -mdy * 4;
			D_PostEvent(&mev);
		}
	}
	while (DG_GetKey(&pressed, &key))"""),
])

patch('i_sound.c', [
    ("""#if defined(FEATURE_SOUND) && !defined(__DJGPP__)
#include <SDL_mixer.h>
#endif""",
     "// SDL_mixer include dropped: the win build's DG_sound_module is waveOut-based"),
])

print("all patches applied")
