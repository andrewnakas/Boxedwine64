/* joytest.c -- M30 joystick probe: every second print what winmm
 * (joyGetNumDevs / joyGetDevCaps / joyGetPosEx) and dinput8 (EnumDevices of
 * game controllers) see. Output goes to stderr as "joytest: ..." lines.
 *
 * Build: i686-w64-mingw32-gcc -O2 -o joytest.exe joytest.c -lwinmm -ldinput8 -ldxguid -mwindows
 */
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <mmsystem.h>
#include <dinput.h>
#include <stdio.h>

static int g_found;

static BOOL CALLBACK enum_cb(const DIDEVICEINSTANCEA *inst, void *ctx)
{
    (void)ctx;
    fprintf(stderr, "joytest: dinput device '%s' type 0x%lx\n", inst->tszProductName, (unsigned long)inst->dwDevType);
    g_found++;
    return DIENUM_CONTINUE;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    IDirectInput8A *di = NULL;
    HRESULT hr = DirectInput8Create(inst, DIRECTINPUT_VERSION, &IID_IDirectInput8A, (void **)&di, NULL);
    int i;

    fprintf(stderr, "joytest: DirectInput8Create hr=0x%08lx\n", (unsigned long)hr);
    for (i = 0; i < 600; i++) {
        JOYINFOEX ji = { sizeof(ji), JOY_RETURNALL };
        JOYCAPSA caps;
        MMRESULT mr = joyGetPosEx(JOYSTICKID1, &ji);
        if (mr == JOYERR_NOERROR && joyGetDevCapsA(JOYSTICKID1, &caps, sizeof(caps)) == JOYERR_NOERROR)
            fprintf(stderr, "joytest: winmm '%s' axes=%u buttons=%u x=%lu y=%lu btn=0x%lx\n", caps.szPname,
                    caps.wNumAxes, caps.wNumButtons, ji.dwXpos, ji.dwYpos, ji.dwButtons);
        else
            fprintf(stderr, "joytest: winmm numdevs=%u joyGetPosEx=%u\n", joyGetNumDevs(), mr);
        if (di) {
            g_found = 0;
            IDirectInput8_EnumDevices(di, DI8DEVCLASS_GAMECTRL, enum_cb, NULL, DIEDFL_ATTACHEDONLY);
            if (!g_found) fprintf(stderr, "joytest: dinput sees no game controllers\n");
        }
        fflush(stderr);
        Sleep(1000);
    }
    return 0;
}
