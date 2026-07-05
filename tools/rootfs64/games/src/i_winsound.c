//
// i_winsound.c — waveOut (winmm) sound module for the doomgeneric win32 build
// running under Boxedwine64's wine (M5). Replaces the dummy driver: DOOM's sfx
// now reach winmm → winealsa → libasound(plug) → pcm_oss → /dev/dsp →
// KDspAudio → SDL/WebAudio in the browser.
//
// Design: an 8-slot software mixer producing S16 stereo at 11025 Hz (the rate
// nearly every DOOM sfx lump uses — cheap under the interpreter), pumped
// through a ring of preprepared WAVEHDRs. Update() runs every tic (35 Hz) on
// the game thread: any buffer waveOut has finished is remixed and requeued
// (CALLBACK_NULL + WHDR_DONE polling — no callback thread, which keeps the
// wine surface minimal). Music is a no-op module (MIDI needs a synth we don't
// have; sfx are the gameplay win).
//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
// (chocolate-doom / doomgeneric lineage — GPLv2, like the rest of this build)
//
// windows.h's rpcndr.h typedefs `boolean`, clashing with doomtype.h's enum —
// rename the windows one out of the way for this TU.
#define boolean rpcndr_win_boolean
#include <windows.h>
#include <mmsystem.h>
#undef boolean
#include <stdio.h>
#include <string.h>

#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"
#include "deh_str.h"

// Config-bound externs that normally live in i_sdlsound.c (i_sound.c binds
// them under FEATURE_SOUND); harmless here — we never resample via SRC.
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

#define MIX_RATE 11025
#define MIX_CHANNELS 8
#define NUM_BUFFERS 6
// ~1/8 s per buffer: 6 buffers ≈ 0.75 s of cushion against tic jitter.
#define BUF_FRAMES (MIX_RATE / 8)

typedef struct
{
    boolean active;
    const unsigned char* samples; // u8 mono sample data (past the DMX header)
    unsigned int length;          // usable sample count
    unsigned int pos1616;         // 16.16 fixed-point read cursor
    unsigned int step1616;        // 16.16 step (lump rate / MIX_RATE)
    int leftvol, rightvol;        // 0..127 each, panning applied
    void* lumpcache;              // W_CacheLumpNum result, released on stop
    int lumpnum;
} mixchan_t;

static mixchan_t channels[MIX_CHANNELS];
static HWAVEOUT s_hwo = NULL;
static WAVEHDR s_hdrs[NUM_BUFFERS];
static short s_bufs[NUM_BUFFERS][BUF_FRAMES * 2];
static boolean s_prefix = true;

static void ChanStop(mixchan_t* c)
{
    if (c->active && c->lumpcache)
    {
        W_ReleaseLumpNum(c->lumpnum);
    }
    c->active = false;
    c->lumpcache = NULL;
}

static void MixInto(short* out, int frames)
{
    int f, ch;

    memset(out, 0, (size_t)frames * 2 * sizeof(short));
    for (ch = 0; ch < MIX_CHANNELS; ch++)
    {
        mixchan_t* c = &channels[ch];
        int l, r;
        if (!c->active)
            continue;
        l = c->leftvol;
        r = c->rightvol;
        for (f = 0; f < frames; f++)
        {
            unsigned int idx = c->pos1616 >> 16;
            int s, accl, accr;
            if (idx >= c->length)
            {
                ChanStop(c);
                break;
            }
            s = ((int)c->samples[idx] - 128) << 8; // u8 -> s16 centered
            accl = out[f * 2] + (s * l) / 127;
            accr = out[f * 2 + 1] + (s * r) / 127;
            if (accl > 32767) accl = 32767; else if (accl < -32768) accl = -32768;
            if (accr > 32767) accr = 32767; else if (accr < -32768) accr = -32768;
            out[f * 2] = (short)accl;
            out[f * 2 + 1] = (short)accr;
            c->pos1616 += c->step1616;
        }
    }
}

static void PumpBuffers(void)
{
    int i;
    if (!s_hwo)
        return;
    for (i = 0; i < NUM_BUFFERS; i++)
    {
        if (s_hdrs[i].dwFlags & WHDR_DONE)
        {
            MixInto(s_bufs[i], BUF_FRAMES);
            s_hdrs[i].dwFlags &= ~WHDR_DONE;
            waveOutWrite(s_hwo, &s_hdrs[i], sizeof(WAVEHDR));
        }
    }
}

static boolean I_Win_InitSound(boolean use_sfx_prefix)
{
    WAVEFORMATEX wf;
    MMRESULT r;
    int i;

    s_prefix = use_sfx_prefix;
    ZeroMemory(&wf, sizeof(wf));
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = MIX_RATE;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = MIX_RATE * 4;
    r = waveOutOpen(&s_hwo, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL);
    if (r != MMSYSERR_NOERROR)
    {
        printf("i_winsound: waveOutOpen failed (%u) — sfx disabled\n", (unsigned)r);
        s_hwo = NULL;
        return false;
    }
    for (i = 0; i < NUM_BUFFERS; i++)
    {
        ZeroMemory(&s_hdrs[i], sizeof(WAVEHDR));
        s_hdrs[i].lpData = (LPSTR)s_bufs[i];
        s_hdrs[i].dwBufferLength = BUF_FRAMES * 2 * sizeof(short);
        waveOutPrepareHeader(s_hwo, &s_hdrs[i], sizeof(WAVEHDR));
        memset(s_bufs[i], 0, sizeof(s_bufs[i]));
        waveOutWrite(s_hwo, &s_hdrs[i], sizeof(WAVEHDR)); // prefill: silence
    }
    printf("i_winsound: waveOut mixer up (%d Hz stereo, %d ch)\n", MIX_RATE, MIX_CHANNELS);
    return true;
}

static void I_Win_ShutdownSound(void)
{
    int i;
    if (!s_hwo)
        return;
    for (i = 0; i < MIX_CHANNELS; i++)
        ChanStop(&channels[i]);
    waveOutReset(s_hwo);
    for (i = 0; i < NUM_BUFFERS; i++)
        waveOutUnprepareHeader(s_hwo, &s_hdrs[i], sizeof(WAVEHDR));
    waveOutClose(s_hwo);
    s_hwo = NULL;
}

static int I_Win_GetSfxLumpNum(sfxinfo_t* sfx)
{
    char namebuf[9];
    if (sfx->link != NULL)
        sfx = sfx->link;
    if (s_prefix)
        snprintf(namebuf, sizeof(namebuf), "ds%s", DEH_String(sfx->name));
    else
        snprintf(namebuf, sizeof(namebuf), "%s", DEH_String(sfx->name));
    return W_GetNumForName(namebuf);
}

static void I_Win_UpdateSound(void)
{
    PumpBuffers();
}

static void SetVolPan(mixchan_t* c, int vol, int sep)
{
    // sep: 0 (hard left) .. 254 (hard right), 128 = center; vol 0..127.
    if (sep < 0) sep = 0;
    if (sep > 254) sep = 254;
    c->leftvol = vol * (254 - sep) / 254;
    c->rightvol = vol * sep / 254;
}

static void I_Win_UpdateSoundParams(int handle, int vol, int sep)
{
    if (handle < 0 || handle >= MIX_CHANNELS)
        return;
    SetVolPan(&channels[handle], vol, sep);
}

static int I_Win_StartSound(sfxinfo_t* sfxinfo, int channel, int vol, int sep)
{
    mixchan_t* c;
    unsigned char* data;
    unsigned int lumplen, length;
    int samplerate, lumpnum;

    if (!s_hwo || channel < 0 || channel >= MIX_CHANNELS)
        return -1;
    lumpnum = sfxinfo->lumpnum;
    data = W_CacheLumpNum(lumpnum, PU_STATIC);
    lumplen = W_LumpLength(lumpnum);
    // DMX header: 03 00, rate LE16, length LE32, then 16 pad bytes each side.
    if (lumplen < 8 + 32 || data[0] != 0x03 || data[1] != 0x00)
    {
        W_ReleaseLumpNum(lumpnum);
        return -1;
    }
    samplerate = (data[3] << 8) | data[2];
    length = (unsigned)((data[7] << 24) | (data[6] << 16) | (data[5] << 8) | data[4]);
    if (length > lumplen - 8 || length <= 48)
    {
        W_ReleaseLumpNum(lumpnum);
        return -1;
    }
    c = &channels[channel];
    ChanStop(c);
    c->samples = data + 8 + 16;
    c->length = length - 32;
    c->pos1616 = 0;
    c->step1616 = (unsigned int)(((unsigned long long)samplerate << 16) / MIX_RATE);
    if (c->step1616 == 0)
        c->step1616 = 1 << 16;
    SetVolPan(c, vol, sep);
    c->lumpcache = data;
    c->lumpnum = lumpnum;
    c->active = true;
    return channel;
}

static void I_Win_StopSound(int handle)
{
    if (handle < 0 || handle >= MIX_CHANNELS)
        return;
    ChanStop(&channels[handle]);
}

static boolean I_Win_SoundIsPlaying(int handle)
{
    if (handle < 0 || handle >= MIX_CHANNELS)
        return false;
    return channels[handle].active;
}

static void I_Win_PrecacheSounds(sfxinfo_t* sounds, int num_sounds)
{
    // Lumps are cached on StartSound; nothing to do.
    (void)sounds; (void)num_sounds;
}

static snddevice_t sound_win_devices[] =
{
    SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS, SNDDEVICE_WAVEBLASTER,
    SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32,
};

sound_module_t DG_sound_module =
{
    sound_win_devices,
    arrlen(sound_win_devices),
    I_Win_InitSound,
    I_Win_ShutdownSound,
    I_Win_GetSfxLumpNum,
    I_Win_UpdateSound,
    I_Win_UpdateSoundParams,
    I_Win_StartSound,
    I_Win_StopSound,
    I_Win_SoundIsPlaying,
    I_Win_PrecacheSounds,
};

// ---- Music: intentionally silent (MIDI needs a synth; sfx are the win) ----
static boolean I_WinMus_Init(void) { return true; }
static void I_WinMus_Shutdown(void) {}
static void I_WinMus_SetVolume(int volume) { (void)volume; }
static void I_WinMus_Pause(void) {}
static void I_WinMus_Resume(void) {}
static void* I_WinMus_Register(void* data, int len) { (void)data; (void)len; return (void*)1; }
static void I_WinMus_Unregister(void* handle) { (void)handle; }
static void I_WinMus_Play(void* handle, boolean looping) { (void)handle; (void)looping; }
static void I_WinMus_Stop(void) {}
static boolean I_WinMus_Playing(void) { return false; }
static void I_WinMus_Poll(void) {}

static snddevice_t music_win_devices[] =
{
    SNDDEVICE_ADLIB, SNDDEVICE_SB, SNDDEVICE_GENMIDI, SNDDEVICE_AWE32,
};

music_module_t DG_music_module =
{
    music_win_devices,
    arrlen(music_win_devices),
    I_WinMus_Init,
    I_WinMus_Shutdown,
    I_WinMus_SetVolume,
    I_WinMus_Pause,
    I_WinMus_Resume,
    I_WinMus_Register,
    I_WinMus_Unregister,
    I_WinMus_Play,
    I_WinMus_Stop,
    I_WinMus_Playing,
    I_WinMus_Poll,
};
