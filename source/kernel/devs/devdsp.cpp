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

#include "kscheduler.h"
#include "../../io/fsvirtualopennode.h"
#include "oss.h"
#ifdef BOXEDWINE_GUEST_X64
#include "kmemory64.h"
#endif
#include <algorithm>
#include <math.h>
#include <string.h>
#include "kdspaudio.h"

#ifdef __EMSCRIPTEN__
// 48000 like native: SDL2's emscripten audio feeds WebAudio, which resamples
// any rate natively — the old 11025 cap predates that and broke the M5 ALSA
// bridge (pcm_oss requests 22050/44100, sees the clamped write-back, -EINVAL,
// and winealsa's stream create fails => waveOutOpen NOTENABLED).
static U32 dspMaxOutputFreq = 48000;
static const U32 DSP_DEFAULT_FRAGMENT_SIZE = 1024;
#else
static U32 dspMaxOutputFreq = 48000;
static const U32 DSP_DEFAULT_FRAGMENT_SIZE = 4096;
#endif
static const U32 DSP_DEFAULT_FRAGMENT_COUNT = 8;

class DevDsp : public FsVirtualOpenNode {
public:
    DevDsp(const std::shared_ptr<FsNode>& node, U32 flags) : FsVirtualOpenNode(node, flags) {                
        this->audio = KDspAudio::createDspAudio();
        this->freq = 11025;
        this->channels = 1;
        this->format = AFMT_U8;
        this->audio->setFragmentSize(DSP_DEFAULT_FRAGMENT_SIZE);
    } 
    virtual ~DevDsp() {this->audio->closeAudio();}

    // From FsOpenNode
    bool setLength(S64 length) override;
    U32 ioctl(KThread* thread, U32 request) override;
    U32 ioctl64(U32 request, U64 argAddr, KMemory64* mem) override;
    U32 readNative(U8* buffer, U32 len) override;
    U32 writeNative(U8* buffer, U32 len) override;
    void waitForEvents(BOXEDWINE_CONDITION& parentCondition, U32 events) override;
    bool isWriteReady() override;

    std::shared_ptr<KDspAudio> audio;
    U32 freq;
    U32 channels;
    U32 format;
    U32 fragmentCount = DSP_DEFAULT_FRAGMENT_COUNT;
    U32 bytesWritten = 0;
    U32 lastOutputBlocks = 0;

private:
    U32 getEffectiveBufferCapacity();
    U32 getUsedBufferSize();
    U32 getAvailableBufferSize();
};


void dspShutdown() {
    KDspAudio::shutdown();
}

void dspSetMaxOutputFreq(U32 freq) {
#ifdef __EMSCRIPTEN__
    if (freq == 11025 || freq == 22050) {
        dspMaxOutputFreq = freq;
    } else {
        kwarn_fmt("Unsupported Emscripten audio frequency %d, using %d", freq, dspMaxOutputFreq);
    }
#else
    dspMaxOutputFreq = freq;
#endif
}

bool DevDsp::setLength(S64 len) {
    return false;
}

U32 DevDsp::readNative(U8* buffer, U32 len){
    return 0;
}

U32 DevDsp::writeNative(U8* buffer, U32 len) {    
    if (!this->audio->isOpen()) {
        this->audio->openAudio(this->format, this->freq, this->channels);
    }
    U32 result = this->audio->writeAudio(buffer, len);
    if ((S32)result > 0) {
        this->bytesWritten += result;
    }
    return result;
}

U32 DevDsp::getEffectiveBufferCapacity() {
    U32 capacity = this->audio->getBufferCapacity();
    U32 fragmentCapacity = this->audio->getFragmentSize() * this->fragmentCount;
    return std::min(capacity, fragmentCapacity ? fragmentCapacity : capacity);
}

U32 DevDsp::getUsedBufferSize() {
    return std::min(this->audio->getBufferSize(), this->getEffectiveBufferCapacity());
}

U32 DevDsp::getAvailableBufferSize() {
    return this->getEffectiveBufferCapacity() - this->getUsedBufferSize();
}

bool DevDsp::isWriteReady() {
    return this->getAvailableBufferSize() >= this->audio->getFragmentSize();
}

// 64-bit-guest OSS ioctls (M5 sound bridge). alsa-lib's pcm_oss plugin (the
// wine winealsa -> libasound -> /dev/dsp route) drives the device with the
// classic OSS ioctl set; the 32-bit ioctl() above can't serve a 64-bit guest
// because it reads/writes the arg through the 32-bit KMemory. This mirrors the
// same state logic with the arg accessed через the guest-64 address space.
// Unknown requests klog once per value so the next missing ioctl is visible.
U32 DevDsp::ioctl64(U32 request, U64 argAddr, KMemory64* mem) {
    bool write = (request & 0x80000000) != 0;
    switch (request & 0xFFFF) {
    case 0x5000: // SNDCTL_DSP_RESET
        this->audio->closeAudio();
        this->freq = 8000;
        this->channels = 1;
        this->format = AFMT_U8;
        this->audio->setFragmentSize(DSP_DEFAULT_FRAGMENT_SIZE);
        this->fragmentCount = DSP_DEFAULT_FRAGMENT_COUNT;
        this->bytesWritten = 0;
        this->lastOutputBlocks = 0;
        return 0;
    case 0x5001: // SNDCTL_DSP_SYNC — drain; our sink paces via the SDL callback,
        return 0; // pending bytes keep playing, so report done immediately.
    case 0x5002: { // SNDCTL_DSP_SPEED
        U32 oldFreq = this->freq;
        this->freq = std::min(mem->readd(argAddr), dspMaxOutputFreq);
        if (oldFreq != this->freq) {
            this->audio->closeAudio();
        }
        if (write) mem->writed(argAddr, this->freq);
        return 0;
    }
    case 0x5003: { // SNDCTL_DSP_STEREO
        U32 fmt = mem->readd(argAddr);
        if (fmt != (U32)(this->channels - 1)) {
            this->audio->closeAudio();
        }
        this->channels = (fmt == 1) ? 2 : 1;
        if (write) mem->writed(argAddr, this->channels - 1);
        return 0;
    }
    case 0x5005: { // SNDCTL_DSP_SETFMT
        U32 fmt = mem->readd(argAddr);
        if (fmt != AFMT_QUERY && fmt != this->format) {
            this->audio->closeAudio();
        }
        switch (fmt) {
        case AFMT_QUERY: break;
        case AFMT_S16_LE: this->format = AFMT_S16_LE; break;
        case AFMT_S16_BE: this->format = AFMT_S16_BE; break;
        case AFMT_S8:     this->format = AFMT_S8; break;
        case AFMT_U16_LE: this->format = AFMT_U16_LE; break;
        case AFMT_U16_BE: this->format = AFMT_U16_BE; break;
        case AFMT_FLOAT:  this->format = AFMT_S16_LE; break;
        default:          this->format = AFMT_U8; break;
        }
        if (write) mem->writed(argAddr, this->format);
        return 0;
    }
    case 0x5006: { // SOUND_PCM_WRITE_CHANNELS
        U32 ch = mem->readd(argAddr);
        if (ch != this->channels) {
            this->audio->closeAudio();
        }
        this->channels = (ch == 1) ? 1 : 2;
        if (write) mem->writed(argAddr, this->channels);
        return 0;
    }
    case 0x500A: { // SNDCTL_DSP_SETFRAGMENT
        U32 value = mem->readd(argAddr);
        U32 shift = value & 0xFFFF;
        U32 count = value >> 16;
        if (shift > 15) shift = 15;
        this->audio->setFragmentSize(1 << shift);
        this->fragmentCount = std::clamp(count, (U32)2, (U32)64);
        return 0;
    }
    case 0x500B: // SNDCTL_DSP_GETFMTS
        mem->writed(argAddr, AFMT_U8 | AFMT_S16_LE | AFMT_S16_BE | AFMT_S8 | AFMT_U16_LE | AFMT_U16_BE);
        return 0;
    case 0x500C: { // SNDCTL_DSP_GETOSPACE (audio_buf_info)
        U32 capacity = this->getEffectiveBufferCapacity();
        U32 used = this->getUsedBufferSize();
        U32 available = capacity - used;
        U32 frag = this->audio->getFragmentSize();
        mem->writed(argAddr, frag ? available / frag : 0);      // fragments
        mem->writed(argAddr + 4, frag ? capacity / frag : 0);   // fragstotal
        mem->writed(argAddr + 8, frag);                         // fragsize
        mem->writed(argAddr + 12, available);                   // bytes
        return 0;
    }
    case 0x500D: { // SNDCTL_DSP_GETISPACE — no capture; report an empty buffer
        U32 frag = this->audio->getFragmentSize();
        mem->writed(argAddr, 0);
        mem->writed(argAddr + 4, this->fragmentCount);
        mem->writed(argAddr + 8, frag);
        mem->writed(argAddr + 12, 0);
        return 0;
    }
    case 0x500F: // SNDCTL_DSP_GETCAPS
        mem->writed(argAddr, DSP_CAP_TRIGGER);
        return 0;
    case 0x5010: // SNDCTL_DSP_SETTRIGGER — playback runs freely; accept.
        return 0;
    case 0x5012: { // SNDCTL_DSP_GETOPTR (count_info)
        U32 fragmentSize = this->audio->getFragmentSize();
        U32 currentBlocks = fragmentSize ? this->bytesWritten / fragmentSize : 0;
        U32 blocks = currentBlocks - this->lastOutputBlocks;
        this->lastOutputBlocks = currentBlocks;
        U32 capacity = this->getEffectiveBufferCapacity();
        mem->writed(argAddr, this->bytesWritten);
        mem->writed(argAddr + 4, blocks);
        mem->writed(argAddr + 8, capacity ? this->bytesWritten % capacity : 0);
        return 0;
    }
    case 0x5016: // SNDCTL_DSP_SETDUPLEX
        return (U32)-K_EINVAL;
    case 0x5017: // SNDCTL_DSP_GETODELAY
        mem->writed(argAddr, this->getUsedBufferSize());
        return 0;
    default:
        klog_fmt("DevDsp::ioctl64: unimplemented OSS ioctl 0x%x (len=%d dir=%c%c)",
                 request & 0xFFFF, (request >> 16) & 0x3FFF,
                 (request & 0x40000000) ? 'R' : '-', write ? 'W' : '-');
        return (U32)-K_ENOTTY;
    }
}

U32 DevDsp::ioctl(KThread* thread, U32 request) {
    U32 len = (request >> 16) & 0x3FFF;
    CPU* cpu = thread->cpu;
    KMemory* memory = thread->memory;

    //BOOL read = request & 0x40000000;
    bool write = (request & 0x80000000)!=0;

    switch (request & 0xFFFF) {
    case 0x5000: // SNDCTL_DSP_RESET
        this->audio->closeAudio();
        this->freq = 8000;
        this->channels = 1;
        this->format = AFMT_U8;
        this->audio->setFragmentSize(DSP_DEFAULT_FRAGMENT_SIZE);
        this->fragmentCount = DSP_DEFAULT_FRAGMENT_COUNT;
        this->bytesWritten = 0;
        this->lastOutputBlocks = 0;
        return 0;
    case 0x5002: { // SNDCTL_DSP_SPEED 
        if (len!=4) {
            kpanic("SNDCTL_DSP_SPEED was expecting a len of 4");
        }
		U32 oldFreq = this->freq;
		this->freq = std::min(memory->readd(IOCTL_ARG1), dspMaxOutputFreq);
        if (oldFreq != this->freq) {
            this->audio->closeAudio();
        }
		if (write)
            memory->writed(IOCTL_ARG1, this->freq);
        return 0;
    }
    case 0x5003: { // SNDCTL_DSP_STEREO
        if (len!=4) {
            kpanic("SNDCTL_DSP_STEREO was expecting a len of 4");
        }
        U32 fmt = memory->readd(IOCTL_ARG1);
        if (fmt != (U32)(this->channels - 1)) {
            this->audio->closeAudio();
        }
        if (fmt == 0) {
            this->channels = 1;
        } else if (fmt == 1) {
            this->channels = 2;
        } else {
            kpanic_fmt("SNDCTL_DSP_STEREO wasn't expecting a value of %d", fmt);
        }
        if (write)
            memory->writed(IOCTL_ARG1, this->channels - 1);
        return 0;
    }
    case 0x5005: { // SNDCTL_DSP_SETFMT 
        if (len!=4) {
            kpanic("SNDCTL_DSP_SETFMT was expecting a len of 4");
        }
        U32 fmt = memory->readd(IOCTL_ARG1);
		if (fmt != AFMT_QUERY && fmt != this->format) {
            this->audio->closeAudio();
        }
        switch (fmt) {
        case AFMT_QUERY:
            break;
        case AFMT_MU_LAW:
        case AFMT_A_LAW:
        case AFMT_IMA_ADPCM:
        case AFMT_U8:
			this->format = AFMT_U8;
            break;
        case AFMT_S16_LE:
			this->format = AFMT_S16_LE;
            break;
        case AFMT_S16_BE:
			this->format = AFMT_S16_BE;
            break;
        case AFMT_S8:
			this->format = AFMT_S8;
            break;
        case AFMT_U16_LE:
			this->format = AFMT_U16_LE;
            break;
        case AFMT_U16_BE:
			this->format = AFMT_U16_BE;
            break;
        case AFMT_MPEG:
			this->format = AFMT_U8;
            break;
        case AFMT_FLOAT:
            this->format = AFMT_S16_LE;
            break;
        }
        if (write)
            memory->writed(IOCTL_ARG1, this->format);
		else if (this->format != fmt) {
            kpanic("SNDCTL_DSP_SETFMT dspFmt!=fmt and can't write result");
        }
        return 0;
        }
    case 0x5006: {// SOUND_PCM_WRITE_CHANNELS
        U32 channels = memory->readd(IOCTL_ARG1);
		if (channels != this->channels) {
            this->audio->closeAudio();
        }
        if (channels==1) {
            this->channels = 1;
        } else if (channels == 2) {
            this->channels = 2;
        } else {
            this->channels = 2;
        }
        if (write)
            memory->writed(IOCTL_ARG1, this->channels);
        return 0;
        }
    case 0x500A: { // SNDCTL_DSP_SETFRAGMENT
        U32 value = memory->readd(IOCTL_ARG1);
        U32 shift = value & 0xFFFF;
        U32 count = value >> 16;
        if (shift > 15) {
            shift = 15;
        }
        this->audio->setFragmentSize(1 << shift);
        this->fragmentCount = std::clamp(count, (U32)2, (U32)64);
        return 0;
    }
    case 0x500B: // SNDCTL_DSP_GETFMTS
        memory->writed(IOCTL_ARG1, AFMT_U8 | AFMT_S16_LE | AFMT_S16_BE | AFMT_S8 | AFMT_U16_LE | AFMT_U16_BE);
        return 0;

		//typedef struct audio_buf_info {
		//	int fragments;     /* # of available fragments (partially usend ones not counted) */
		//	int fragstotal;    /* Total # of fragments allocated */
		//	int fragsize;      /* Size of a fragment in bytes */
		//
		//	int bytes;         /* Available space in bytes (includes partially used fragments) */
		//	/* Note! 'bytes' could be more than fragments*fragsize */
		//} audio_buf_info;

    case 0x500C: // SNDCTL_DSP_GETOSPACE
    {
        U32 capacity = this->getEffectiveBufferCapacity();
        U32 used = this->getUsedBufferSize();
        U32 available = capacity - used;
        memory->writed(IOCTL_ARG1, available / this->audio->getFragmentSize()); // fragments
        memory->writed(IOCTL_ARG1 + 4, capacity / this->audio->getFragmentSize());
        memory->writed(IOCTL_ARG1 + 8, this->audio->getFragmentSize());
        memory->writed(IOCTL_ARG1 + 12, available);
        return 0;
    }
    case 0x500F: // SNDCTL_DSP_GETCAPS
        memory->writed(IOCTL_ARG1, DSP_CAP_TRIGGER);
        return 0;
    case 0x5010: // SNDCTL_DSP_SETTRIGGER
        /*
        if (readd(IOCTL_ARG1) & PCM_ENABLE_OUTPUT) {
            if (sdlSoundEnabled) {
                SDL_PauseAudio(0);
            }
			this->data->pauseAtLen = 0xFFFFFFFF;
        } else {            
			this->data->pauseAtLen = (U32)audioBuffer.size();
			if (this->data->pauseAtLen == 0) {
                if (sdlSoundEnabled) {
                    SDL_PauseAudio(0);
                }
            }
        }
        */
        klog("DevDsp::ioctl was not expecting SNDCTL_DSP_SETTRIGGER");
        return 0;
    case 0x5012: { // SNDCTL_DSP_GETOPTR
        U32 fragmentSize = this->audio->getFragmentSize();
        U32 currentBlocks = fragmentSize ? this->bytesWritten / fragmentSize : 0;
        U32 blocks = currentBlocks - this->lastOutputBlocks;
        this->lastOutputBlocks = currentBlocks;
        U32 capacity = this->getEffectiveBufferCapacity();
        memory->writed(IOCTL_ARG1, this->bytesWritten); // Total # of bytes written
        memory->writed(IOCTL_ARG1 + 4, blocks); // # of fragment transitions since last time
        memory->writed(IOCTL_ARG1 + 8, capacity ? this->bytesWritten % capacity : 0); // Current DMA pointer value
        return 0;
    }
    case 0x5016: // SNDCTL_DSP_SETDUPLEX
        return -K_EINVAL;
    case 0x5017: // SNDCTL_DSP_GETODELAY
        memory->writed(IOCTL_ARG1, this->getUsedBufferSize());
        return 0;
    case 0x580C: // SNDCTL_ENGINEINFO
        if (write) {
            U32 p = IOCTL_ARG1;
            p+=4; // int dev; /* Audio device number */
            memory->strcpy(p, "BoxedWine audio"); p+=64; // oss_devname_t name;
            memory->writed(p, 0); p+=4; // int busy; /* 0, OPEN_READ, OPEN_WRITE or OPEN_READWRITE */
            memory->writed(p, -1); p+=4; // int pid;
            memory->writed(p, PCM_CAP_OUTPUT); p+=4; // int caps;			/* PCM_CAP_INPUT, PCM_CAP_OUTPUT */
            memory->writed(p, 0); p+=4; // int iformats
            memory->writed(p, AFMT_U8 | AFMT_S16_LE | AFMT_S16_BE | AFMT_S8 | AFMT_U16_BE); p+=4; // int oformats;
            memory->writed(p, 0); p+=4; // int magic;			/* Reserved for internal use */
            memory->strcpy(p, ""); p+=64; // oss_cmd_t cmd;		/* Command using the device (if known) */
            memory->writed(p, 0); p+=4; // int card_number;
            memory->writed(p, 0); p+=4; // int port_number;
            memory->writed(p, 0); p+=4; // int mixer_dev;
            memory->writed(p, 0); p+=4; // int legacy_device;		/* Obsolete field. Replaced by devnode */
            memory->writed(p, 1); p+=4; // int enabled;			/* 1=enabled, 0=device not ready at this moment */
            memory->writed(p, 0); p+=4; // int flags;			/* For internal use only - no practical meaning */
            memory->writed(p, 11025); p += 4; // int min_rate
            memory->writed(p, dspMaxOutputFreq); p+=4; // max_rate;	/* Sample rate limits */
            memory->writed(p, 1); p+=4; // int min_channels
            memory->writed(p, 2); p+=4; // max_channels;	/* Number of channels supported */
            memory->writed(p, 0); p+=4; // int binding;			/* DSP_BIND_FRONT, etc. 0 means undefined */
            memory->writed(p, 0); p+=4; // int rate_source;
            memory->strcpy(p, ""); p+=32; // oss_handle_t handle;
            memory->writed(p, 0); p+=4; // unsigned int nrates
            for (int i=0;i<20;i++) {
                memory->writed(p, 0); p+=4; // rates[20];	/* Please read the manual before using these */
            }
            memory->strcpy(p, ""); p+=64; // oss_longname_t song_name;	/* Song name (if given) */
            memory->strcpy(p, ""); p+=16; // oss_label_t label;		/* Device label (if given) */
            memory->writed(p, -1); p+=4; // int latency;			/* In usecs, -1=unknown */
            memory->strcpy(p, "/dev/dsp"); p+=16; // oss_devnode_t devnode;	/* Device special file name (absolute path) */
            memory->writed(p, 0); p+=4; // int next_play_engine;		/* Read the documentation for more info */
            memory->writed(p, 0); // int next_rec_engine;		/* Read the documentation for more info */
            return 0;
        }        
    }
    return -K_ENODEV;
}

void DevDsp::waitForEvents(BOXEDWINE_CONDITION& parentCondition, U32 events) {
    if (events & K_POLLOUT) {
        if (this->isWriteReady()) {
            BOXEDWINE_CONDITION_SIGNAL_ALL(parentCondition);
        }
    }
}

FsOpenNode* openDevDsp(const std::shared_ptr<FsNode>& node, U32 flags, U32 data) {
    return new DevDsp(node, flags);
}
