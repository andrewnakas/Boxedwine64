/*
 *  Copyright (C) 2012-2025  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

// Guest libSDL2-2.0.so.0 for the 64-bit Boxedwine wine path (M30): just the
// joystick slice wine's winebus.sys SDL backend dlopens. It exposes the
// browser's first gamepad (navigator.getGamepads()[0], pushed into the host by
// the launcher) as one plain SDL joystick; winebus turns it into a HID device
// that dinput, xinput's HID path and winmm's joyGetPos all read.
//
// The host state comes from a private syscall (PAD64_SYSCALL_NR in
// source/kernel/syscall64.cpp). SDL_WaitEventTimeout polls it and diffs
// against the last state to synthesize SDL joystick events. No game-controller
// mapping, no haptics (those entry points report "unsupported").
//
// Freestanding like libgl64.c: no libc headers or DT_NEEDED.

typedef unsigned char       Uint8;
typedef signed short        Sint16;
typedef unsigned short      Uint16;
typedef signed int          Sint32;
typedef unsigned int        Uint32;
typedef unsigned long long  Uint64;

#define API __attribute__((visibility("default")))

// the compiler may lower struct copies to these; no libc to provide them
__attribute__((visibility("hidden"))) void* memcpy(void* d, const void* s, unsigned long n) {
    unsigned char* dd = (unsigned char*)d; const unsigned char* ss = (const unsigned char*)s;
    while (n--) *dd++ = *ss++;
    return d;
}
__attribute__((visibility("hidden"))) void* memset(void* d, int v, unsigned long n) {
    unsigned char* dd = (unsigned char*)d;
    while (n--) *dd++ = (unsigned char)v;
    return d;
}
#define PAD64_SYSCALL_NR 0x50414400ULL

enum {
    SDL_JOYAXISMOTION = 0x600, SDL_JOYHATMOTION = 0x602, SDL_JOYBUTTONDOWN = 0x603,
    SDL_JOYBUTTONUP = 0x604, SDL_JOYDEVICEADDED = 0x605, SDL_JOYDEVICEREMOVED = 0x606,
    SDL_USEREVENT = 0x8000,
};

typedef union SDL_Event { Uint32 type; Uint8 raw[56]; } SDL_Event;
typedef struct SDL_JoystickGUID { Uint8 data[16]; } SDL_JoystickGUID;
typedef struct SDL_Joystick { int open; } SDL_Joystick;

struct PadState { Sint32 connected, naxes, nbuttons; Uint32 buttons; Sint16 axes[8]; };

static inline long sys3(long nr, long a, long b, long c) {
    long ret;
    register long rdx __asm__("rdx") = c;
    __asm__ __volatile__("syscall" : "=a"(ret) : "a"(nr), "D"(a), "S"(b), "r"(rdx) : "rcx", "r11", "memory");
    return ret;
}

static void pad_query(struct PadState* s) {
    s->connected = 0;
    sys3((long)PAD64_SYSCALL_NR, 1, (long)s, 0);
}

static Uint32 now_ms(void) {
    struct { long sec, nsec; } ts = {0, 0};
    sys3(228 /*clock_gettime*/, 1 /*CLOCK_MONOTONIC*/, (long)&ts, 0);
    return (Uint32)(ts.sec * 1000 + ts.nsec / 1000000);
}

static void sleep_ms(int ms) {
    struct { long sec, nsec; } ts = {ms / 1000, (long)(ms % 1000) * 1000000};
    sys3(35 /*nanosleep*/, (long)&ts, 0, 0);
}

static void copy_event(SDL_Event* d, const SDL_Event* s) { for (int i = 0; i < 56; i++) d->raw[i] = s->raw[i]; }

// ---- event queue (winebus's thread + SDL_PushEvent callers) ----
static SDL_Event g_queue[128];
static unsigned g_head, g_tail;
static volatile int g_lock;
static void lock(void) { while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE)) {} }
static void unlock(void) { __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE); }

static void push(const SDL_Event* e) {
    lock();
    if (g_tail - g_head < 128) copy_event(&g_queue[g_tail++ % 128], e);
    unlock();
}
static int pop(SDL_Event* e) {
    int got = 0;
    lock();
    if (g_head != g_tail) { if (e) copy_event(e, &g_queue[g_head % 128]); g_head++; got = 1; }
    unlock();
    return got;
}

static struct PadState g_last;      // state already reported to winebus
static SDL_Joystick g_joy;

static void make(SDL_Event* e, Uint32 type) {
    for (int i = 0; i < 56; i++) e->raw[i] = 0;
    e->type = type;
    *(Uint32*)(e->raw + 4) = now_ms();
    *(Sint32*)(e->raw + 8) = 0;      // device index / instance id 0
}

// Diff the host gamepad against what winebus has seen; queue the changes.
static void poll_host(void) {
    struct PadState s;
    SDL_Event e;
    pad_query(&s);
    if (s.connected && !g_last.connected) {
        g_last = s;
        g_last.buttons = 0;
        for (int i = 0; i < 8; i++) g_last.axes[i] = 0;
        make(&e, SDL_JOYDEVICEADDED);
        push(&e);
    } else if (!s.connected && g_last.connected) {
        g_last.connected = 0;
        make(&e, SDL_JOYDEVICEREMOVED);
        push(&e);
        return;
    }
    if (!s.connected || !g_joy.open) return;
    for (int i = 0; i < g_last.naxes && i < 8; i++) {
        if (s.axes[i] != g_last.axes[i]) {
            make(&e, SDL_JOYAXISMOTION);
            e.raw[12] = (Uint8)i;
            *(Sint16*)(e.raw + 16) = s.axes[i];
            push(&e);
            g_last.axes[i] = s.axes[i];
        }
    }
    for (int i = 0; i < g_last.nbuttons && i < 32; i++) {
        Uint32 bit = 1u << i;
        if ((s.buttons ^ g_last.buttons) & bit) {
            make(&e, (s.buttons & bit) ? SDL_JOYBUTTONDOWN : SDL_JOYBUTTONUP);
            e.raw[12] = (Uint8)i;
            e.raw[13] = (s.buttons & bit) ? 1 : 0;
            push(&e);
        }
    }
    g_last.buttons = s.buttons;
}

API int SDL_Init(Uint32 flags) { (void)flags; return 0; }
API const char* SDL_GetError(void) { return ""; }
API Uint32 SDL_GetTicks(void) { return now_ms(); }
API Uint32 SDL_RegisterEvents(int n) { (void)n; return SDL_USEREVENT; }
API int SDL_PushEvent(SDL_Event* e) { if (e) push(e); return 1; }

API int SDL_WaitEventTimeout(SDL_Event* event, int timeout) {
    Uint32 start = now_ms();
    for (;;) {
        if (pop(event)) return 1;
        poll_host();
        if (pop(event)) return 1;
        if (timeout >= 0 && (int)(now_ms() - start) >= timeout) return 0;
        sleep_ms(timeout >= 0 && timeout < 10 ? (timeout > 0 ? timeout : 1) : 10);
    }
}

// ---- joystick ----
API int SDL_JoystickEventState(int state) { (void)state; return 1; }
API SDL_Joystick* SDL_JoystickOpen(int index) { if (index != 0 || !g_last.connected) return 0; g_joy.open = 1; return &g_joy; }
API void SDL_JoystickClose(SDL_Joystick* j) { if (j) j->open = 0; }
API Sint32 SDL_JoystickInstanceID(SDL_Joystick* j) { (void)j; return 0; }
API const char* SDL_JoystickName(SDL_Joystick* j) { (void)j; return "Boxedwine Gamepad"; }
API int SDL_JoystickNumAxes(SDL_Joystick* j) { (void)j; return g_last.naxes; }
API int SDL_JoystickNumButtons(SDL_Joystick* j) { (void)j; return g_last.nbuttons; }
API int SDL_JoystickNumBalls(SDL_Joystick* j) { (void)j; return 0; }
API int SDL_JoystickNumHats(SDL_Joystick* j) { (void)j; return 0; }
API Sint16 SDL_JoystickGetAxis(SDL_Joystick* j, int axis) { (void)j; return (axis >= 0 && axis < 8) ? g_last.axes[axis] : 0; }
API Uint8 SDL_JoystickGetHat(SDL_Joystick* j, int hat) { (void)j; (void)hat; return 0; }
API Uint16 SDL_JoystickGetVendor(SDL_Joystick* j) { (void)j; return 0x1209; }    // pid.codes test VID
API Uint16 SDL_JoystickGetProduct(SDL_Joystick* j) { (void)j; return 0x0001; }
API Uint16 SDL_JoystickGetProductVersion(SDL_Joystick* j) { (void)j; return 1; }
API int SDL_JoystickGetType(SDL_Joystick* j) { (void)j; return 0; }             // SDL_JOYSTICK_TYPE_UNKNOWN
API const char* SDL_JoystickGetSerial(SDL_Joystick* j) { (void)j; return 0; }
API SDL_JoystickGUID SDL_JoystickGetGUID(SDL_Joystick* j) {
    SDL_JoystickGUID g;
    (void)j;
    for (int i = 0; i < 16; i++) g.data[i] = 0;
    g.data[0] = 0x03;                    // SDL_HARDWARE_BUS_USB
    g.data[4] = 0x09; g.data[5] = 0x12;  // vendor 0x1209 (LE)
    g.data[8] = 0x01;                    // product 0x0001
    return g;
}
API void SDL_JoystickGetGUIDString(SDL_JoystickGUID guid, char* out, int len) {
    static const char hex[] = "0123456789abcdef";
    int o = 0;
    for (int i = 0; i < 16 && o + 2 < len; i++) { out[o++] = hex[guid.data[i] >> 4]; out[o++] = hex[guid.data[i] & 15]; }
    if (len > 0) out[o < len ? o : len - 1] = 0;
}
API int SDL_JoystickRumble(SDL_Joystick* j, Uint16 lo, Uint16 hi, Uint32 ms) { (void)j; (void)lo; (void)hi; (void)ms; return -1; }
API int SDL_JoystickRumbleTriggers(SDL_Joystick* j, Uint16 l, Uint16 r, Uint32 ms) { (void)j; (void)l; (void)r; (void)ms; return -1; }
API int SDL_JoystickIsHaptic(SDL_Joystick* j) { (void)j; return 0; }

// ---- game controller: never claimed (winebus uses the joystick path) ----
API int SDL_IsGameController(int index) { (void)index; return 0; }
API void* SDL_GameControllerOpen(int index) { (void)index; return 0; }
API void SDL_GameControllerClose(void* c) { (void)c; }
API Sint16 SDL_GameControllerGetAxis(void* c, int a) { (void)c; (void)a; return 0; }
API Uint8 SDL_GameControllerGetButton(void* c, int b) { (void)c; (void)b; return 0; }
API const char* SDL_GameControllerName(void* c) { (void)c; return 0; }
API int SDL_GameControllerEventState(int state) { (void)state; return 0; }
API int SDL_GameControllerAddMapping(const char* m) { (void)m; return 0; }

// ---- haptics: unsupported ----
API void* SDL_HapticOpenFromJoystick(SDL_Joystick* j) { (void)j; return 0; }
API void SDL_HapticClose(void* h) { (void)h; }
API unsigned SDL_HapticQuery(void* h) { (void)h; return 0; }
API int SDL_HapticNewEffect(void* h, void* e) { (void)h; (void)e; return -1; }
API int SDL_HapticUpdateEffect(void* h, int id, void* e) { (void)h; (void)id; (void)e; return -1; }
API int SDL_HapticRunEffect(void* h, int id, Uint32 it) { (void)h; (void)id; (void)it; return -1; }
API int SDL_HapticStopEffect(void* h, int id) { (void)h; (void)id; return -1; }
API void SDL_HapticDestroyEffect(void* h, int id) { (void)h; (void)id; }
API int SDL_HapticGetEffectStatus(void* h, int id) { (void)h; (void)id; return -1; }
API int SDL_HapticSetGain(void* h, int g) { (void)h; (void)g; return -1; }
API int SDL_HapticPause(void* h) { (void)h; return -1; }
API int SDL_HapticUnpause(void* h) { (void)h; return -1; }
API int SDL_HapticStopAll(void* h) { (void)h; return -1; }
API int SDL_HapticRumbleSupported(void* h) { (void)h; return 0; }
API int SDL_HapticRumbleInit(void* h) { (void)h; return -1; }
API int SDL_HapticRumblePlay(void* h, float s, Uint32 ms) { (void)h; (void)s; (void)ms; return -1; }
API int SDL_HapticRumbleStop(void* h) { (void)h; return -1; }
