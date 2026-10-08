/*
 * OpenAL on the MCPX APU Voice Processor (route A, stage 2 of
 * lib/openal/docs/XEMU_VERIFICATION.md section 8.2).
 *
 * The OpenAL library is built into this sample with -DOPENAL_APU_REAL_MMIO,
 * so alcOpenDevice() runs the corrected VP bring-up (apu_vp.c) and every
 * source plays on a hardware VP voice. Only the OpenAL API is used here.
 *
 * Audible on xemu with "Real-time DSP processing" off (its default): xemu
 * then taps the VP output. xemu's VP tap ignores the mixbins (each voice is
 * heard at its loudest output), so left/right panning is not audible there;
 * distance attenuation and pitch are.
 *
 * On a real console the front-end method queue does not run, so the library
 * writes the voice records itself, mono sources are panned by volumes (no
 * HRTF table without methods), the GP program copies mixbins 0/1 to a FIFO
 * and alXboxUpdateVoices() forwards it to the AC97.
 */
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <hal/debug.h>
#include <hal/video.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <windows.h>

#include "al_source.h"
#include "apu_ac97.h"
#include "apu_gp.h"
#include "apu_hardware.h"
#include "apu_vp.h"
#include "apu_voice.h"
#include "apu_hrtf.h"

#define RATE 48000
#define PI_F 3.14159265f

static int16_t s_tone[RATE];          /* 1 s, loops seamlessly at 440 Hz */
static int16_t s_ping[RATE / 4];      /* 250 ms decaying one-shot */

typedef struct {
    const char *name;
    DWORD ms;
} phase_t;

static const phase_t s_phases[] = {
    { "Loop at 2 m (unity pitch)",              3000 },
    { "Distance sweep 1 m .. 30 m (volume)",    6000 },
    { "Pitch sweep 0.5x .. 2x (AL_PITCH)",      6000 },
    { "Orbit around the listener",              5000 },
    { "Chord: three voices at once",            4000 },
    { "One-shot ping x3 (must end AL_STOPPED)", 4000 },
};
#define NUM_PHASES ((int)(sizeof(s_phases) / sizeof(s_phases[0])))

static const char *state_name(ALint s) {
    switch (s) {
        case AL_INITIAL: return "INITIAL";
        case AL_PLAYING: return "PLAYING";
        case AL_PAUSED:  return "PAUSED";
        case AL_STOPPED: return "STOPPED";
        default:         return "?";
    }
}

static void show_source(const char *label, ALuint id) {
    ALint state = 0;
    ALsource *s;
    alGetSourcei(id, AL_SOURCE_STATE, &state);
    s = al_source_get(id);
    if (s && s->hw_voice_idx >= 0) {
        uint32_t h = apu_voice_hw_handle((uint32_t)s->hw_voice_idx);
        const uint8_t *rec = apu_vp_debug_voice_record(h);
        uint32_t hrtf = rec ? (*(const uint32_t *)(rec + 0x1C) & 0xFFFFu) : 0xFFFFu;   /* CFG_HRTF_TARGET */
        debugPrint("%-6s %-8s VP handle %3lu  CBO %6lu  ACTIVE %d  ", label, state_name(state),
                   (unsigned long)h, (unsigned long)apu_vp_voice_position(h), apu_vp_voice_active(h) ? 1 : 0);
        if (hrtf == 0xFFFFu) {
            debugPrint("HRTF -\n");
        } else {
            debugPrint("HRTF %3lu (az %3lu el %+d)\n", (unsigned long)hrtf,
                       (unsigned long)((hrtf % APU_HRTF_AZIMUTHS) * 360u / APU_HRTF_AZIMUTHS),
                       (int)apu_hrtf_elevations_deg[hrtf / APU_HRTF_AZIMUTHS]);
        }
    } else {
        debugPrint("%-6s %-8s (no voice)\n", label, state_name(state));
    }
}

int main(void) {
    ALCdevice *dev;
    ALCcontext *ctx;
    ALuint buf_tone, buf_ping, src[3];
    ALint backend = -1;
    int i, phase = 0, pings = 0, pings_stopped = 0;
    DWORD phase_start, last_show = 0;
    ALint last_ping_state = AL_INITIAL;

    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);

    for (i = 0; i < RATE; i++) {
        s_tone[i] = (int16_t)(8000.0f * sinf(2.0f * PI_F * 440.0f * (float)i / RATE));
    }
    for (i = 0; i < RATE / 4; i++) {
        float t = (float)i / RATE;
        s_ping[i] = (int16_t)(12000.0f * expf(-t * 18.0f) * sinf(2.0f * PI_F * 880.0f * t));
    }

    dev = alcOpenDevice(NULL);
    if (!dev) {
        debugPrint("alcOpenDevice failed\n");
        for (;;) Sleep(1000);
    }
    ctx = alcCreateContext(dev, NULL);
    alcMakeContextCurrent(ctx);
    alXboxGetHardwareStatus(AL_XBOX_BACKEND, &backend);

    alGenBuffers(1, &buf_tone);
    alBufferData(buf_tone, AL_FORMAT_MONO16, s_tone, (ALsizei)sizeof(s_tone), RATE);
    alGenBuffers(1, &buf_ping);
    alBufferData(buf_ping, AL_FORMAT_MONO16, s_ping, (ALsizei)sizeof(s_ping), RATE);
    alGenSources(3, src);
    for (i = 0; i < 3; i++) {
        alSourcei(src[i], AL_BUFFER, (ALint)buf_tone);
        alSourcei(src[i], AL_LOOPING, AL_TRUE);
        alSourcef(src[i], AL_REFERENCE_DISTANCE, 1.0f);
        alSourcef(src[i], AL_MAX_DISTANCE, 30.0f);
    }
    alListener3f(AL_POSITION, 0.0f, 0.0f, 0.0f);

    alSource3f(src[0], AL_POSITION, 0.0f, 0.0f, -2.0f);
    alSourcePlay(src[0]);
    phase_start = GetTickCount();

    for (;;) {
        DWORD now = GetTickCount();
        float t = (float)(now - phase_start) / 1000.0f;
        float u = t * 1000.0f / (float)s_phases[phase].ms;   /* 0..1 through the phase */

        if (now - phase_start >= s_phases[phase].ms) {
            /* Next phase: reset to one looping source */
            phase = (phase + 1) % NUM_PHASES;
            phase_start = now;
            alSourceStop(src[1]);
            alSourceStop(src[2]);
            alSourcef(src[0], AL_PITCH, 1.0f);
            alSource3f(src[0], AL_POSITION, 0.0f, 0.0f, -2.0f);
            alSourcei(src[0], AL_LOOPING, AL_TRUE);
            alSourcei(src[0], AL_BUFFER, (ALint)buf_tone);
            if (phase == 4) {
                alSourcef(src[1], AL_PITCH, 1.2599f);      /* major third */
                alSourcef(src[2], AL_PITCH, 1.4983f);      /* fifth */
                alSource3f(src[1], AL_POSITION, -1.5f, 0.0f, -2.0f);
                alSource3f(src[2], AL_POSITION, 1.5f, 0.0f, -2.0f);
                alSourcePlay(src[1]);
                alSourcePlay(src[2]);
            }
            if (phase == 5) {
                alSourceStop(src[0]);
                alSourcei(src[0], AL_LOOPING, AL_FALSE);
                alSourcei(src[0], AL_BUFFER, (ALint)buf_ping);
                pings = pings_stopped = 0;
                last_ping_state = AL_INITIAL;
            } else {
                alSourcePlay(src[0]);
            }
            continue;
        }

        switch (phase) {
            case 1: {   /* 1 m -> 30 m -> 1 m */
                float d = 1.0f + 29.0f * (0.5f - 0.5f * cosf(2.0f * PI_F * u));
                alSource3f(src[0], AL_POSITION, 0.0f, 0.0f, -d);
                break;
            }
            case 2:     /* 0.5x -> 2x -> 0.5x, exponential */
                alSourcef(src[0], AL_PITCH, powf(2.0f, -cosf(2.0f * PI_F * u)));
                break;
            case 3:
                alSource3f(src[0], AL_POSITION, 3.0f * sinf(2.0f * PI_F * u), 0.0f, -3.0f * cosf(2.0f * PI_F * u));
                break;
            case 5: {   /* a ping every 1.2 s; each must end on its own */
                ALint st = AL_INITIAL;
                alGetSourcei(src[0], AL_SOURCE_STATE, &st);
                if (st == AL_STOPPED && last_ping_state == AL_PLAYING) {
                    pings_stopped++;
                }
                last_ping_state = st;
                if (pings < 3 && t >= 0.2f + 1.2f * (float)pings) {
                    alSourcePlay(src[0]);
                    last_ping_state = AL_PLAYING;
                    pings++;
                }
                break;
            }
            default:
                break;
        }

        /* Frame tick: reaps finished voices and services the VP idle trap */
        alXboxUpdateVoices();

        if (now - last_show >= 150) {
            last_show = now;
            debugClearScreen();
            debugPrint("OpenAL on the MCPX APU Voice Processor (route A)\n\n");
            debugPrint("Backend: %s   Renderer: %s\n",
                       backend == AL_XBOX_BACKEND_MMIO ? "MMIO (real APU)" : "NULL (software model)",
                       alGetString(AL_RENDERER));
            {
                apu_ac97_stats_t as;
                apu_ac97_get_stats(&as);
                apu_vp_fe_stats_t fs;
                apu_vp_get_fe_stats(&fs);
                debugPrint("HRTF %s   GP frames %lu   AC97 %s\n",
                           apu_vp_hrtf_available() ? "on" : "off (volume panning)",
                           (unsigned long)apu_gp_frames(NV_PAPU_BASE),
                           apu_ac97_active() ? "fed from the GP FIFO" : "not used");
                debugPrint("AC97 %lu buffers, %lu padded, %lu underruns   FE msgs %lu (last %08lx=%08lx)\n",
                           (unsigned long)as.chunks, (unsigned long)as.padded, (unsigned long)as.underruns,
                           (unsigned long)fs.events, (unsigned long)fs.last_meth, (unsigned long)fs.last_param);
            }
            {
                volatile uint32_t *r = (volatile uint32_t *)NV_PAPU_BASE;
                debugPrint("GP program %s  P0 %06lx  GPSADDR %08lx  SECTL %02lx\n",
                           apu_gp_program_loaded() ? "loaded" : "NOT LOADED",
                           (unsigned long)(r[0x3A000 / 4] & 0xFFFFFFu), (unsigned long)r[0x2040 / 4],
                           (unsigned long)r[0x2000 / 4]);
                debugPrint("XGSCNT %08lx TVL2D %04lx FIFO %06lx FEPIOQ %08lx\n", (unsigned long)r[0x200C / 4],
                           (unsigned long)(r[0x2054 / 4] & 0xFFFFu), (unsigned long)(r[0x302C / 4] & 0xFFFFFFu),
                           (unsigned long)r[0x1340 / 4]);
            }
            debugPrint("Phase %d/%d: %s\n\n", phase + 1, NUM_PHASES, s_phases[phase].name);
            show_source("src 1", src[0]);
            show_source("src 2", src[1]);
            show_source("src 3", src[2]);
            if (phase == 5) {
                debugPrint("\npings started %d, ended on their own %d\n", pings, pings_stopped);
            }
            debugPrint("\nReal console: VP -> GP FIFO -> AC97, panning in stereo.\n"
                       "xemu (real-time DSP off): VP tap, panning not heard.\n");
        }
        Sleep(5);
    }
    return 0;
}
