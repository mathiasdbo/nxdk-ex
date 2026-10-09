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
#include <nxdk/mount.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
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

#define STRESS_SOURCES 96             /* more than the 64 voice slots: forces stealing */

/*
 * Log: one status line per second, written to E:\openal_vp.txt every 5 s and
 * as soon as the GP frame counter stops (so a freeze leaves a record).
 */
#define LOG_BYTES (96u * 1024u)
static char s_log[LOG_BYTES];
static size_t s_log_len;

static void log_line(const char *fmt, ...) {
    va_list ap;
    int n;
    if (s_log_len >= LOG_BYTES - 256u) {
        return;   /* full: keep the start, which has the first freeze */
    }
    va_start(ap, fmt);
    n = vsnprintf(s_log + s_log_len, LOG_BYTES - s_log_len, fmt, ap);
    va_end(ap);
    if (n > 0) s_log_len += (size_t)n;
}

static void log_save(void) {
    FILE *f;
    if (!nxIsDriveMounted('E')) {
        nxMountDrive('E', "\\Device\\Harddisk0\\Partition1\\");
    }
    f = fopen("E:\\openal_vp.txt", "wb");
    if (f) {
        fwrite(s_log, 1, s_log_len, f);
        fclose(f);
    }
}

static uint32_t lcg(uint32_t *s) {
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

/* Library slots whose VP voice is playing */
static uint32_t active_hw_voices(void) {
    uint32_t i, n = 0;
    for (i = 0; i < 64u; i++) {
        if (apu_vp_voice_active(apu_voice_hw_handle(i))) n++;
    }
    return n;
}

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
    { "Stress: 96 one-shot sources (voice stealing)", 10000 },
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
    static ALuint stress[STRESS_SOURCES];
    ALint backend = -1;
    int i, phase = 0, pings = 0, pings_stopped = 0;
    DWORD phase_start, last_show = 0, last_frames_check = 0;
    ALint last_ping_state = AL_INITIAL;
    uint32_t stress_next = 0, stress_starts = 0, rng = 12345u, gp_last = 0, frozen_at = 0;
    DWORD start_tick, last_log = 0, last_save = 0;

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
    alGenSources(STRESS_SOURCES, stress);
    for (i = 0; i < STRESS_SOURCES; i++) {
        float a = 2.0f * PI_F * (float)i / STRESS_SOURCES;
        alSourcei(stress[i], AL_BUFFER, (ALint)buf_ping);
        alSourcei(stress[i], AL_LOOPING, AL_FALSE);
        alSourcef(stress[i], AL_GAIN, 0.25f);
        alSource3f(stress[i], AL_POSITION, 4.0f * sinf(a), 0.0f, -4.0f * cosf(a));
    }
    alListener3f(AL_POSITION, 0.0f, 0.0f, 0.0f);

    alSource3f(src[0], AL_POSITION, 0.0f, 0.0f, -2.0f);
    alSourcePlay(src[0]);
    phase_start = GetTickCount();
    start_tick = phase_start;
    log_line("openal_vp log: HRTF %s, GP program %s, GPSADDR %08lx\n", apu_vp_hrtf_available() ? "on" : "off",
             apu_gp_program_loaded() ? "loaded" : "NOT LOADED",
             (unsigned long)((volatile uint32_t *)NV_PAPU_BASE)[0x2040 / 4]);

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
            for (i = 0; i < STRESS_SOURCES; i++) {
                alSourceStop(stress[i]);
            }
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
            } else if (phase == 6) {
                alSourceStop(src[0]);
                stress_starts = 0;
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
            case 6: {   /* start up to 3 idle stress sources per frame, random pitch 0.5x .. 2x */
                int started = 0, tries;
                for (tries = 0; tries < STRESS_SOURCES && started < 3; tries++) {
                    ALint st = AL_INITIAL;
                    ALuint s = stress[stress_next];
                    stress_next = (stress_next + 1u) % STRESS_SOURCES;
                    alGetSourcei(s, AL_SOURCE_STATE, &st);
                    if (st != AL_PLAYING) {
                        alSourcef(s, AL_PITCH, 0.5f + 1.5f * (float)(lcg(&rng) & 0xFFFFu) / 65535.0f);
                        alSourcePlay(s);
                        stress_starts++;
                        started++;
                    }
                }
                break;
            }
            default:
                break;
        }

        /* Frame tick: reaps finished voices and services the VP idle trap */
        alXboxUpdateVoices();

        /* Freeze detector: the GP frame counter must move (1500 per second) */
        if (now - last_frames_check >= 500) {
            uint32_t g = apu_gp_frames(NV_PAPU_BASE);
            bool just_froze = false;
            if (last_frames_check && g == gp_last && !frozen_at) {
                frozen_at = now ? now : 1;
                just_froze = true;
            }
            gp_last = g;
            last_frames_check = now;
            /* status line every second (and at the freeze) */
            if (just_froze || (now / 1000u) != (last_log / 1000u)) {
                apu_ac97_stats_t as;
                apu_vp_fe_stats_t fs;
                volatile uint32_t *r = (volatile uint32_t *)NV_PAPU_BASE;
                apu_ac97_get_stats(&as);
                apu_vp_get_fe_stats(&fs);
                log_line("t %6lu ms phase %d: GP %7lu XGSCNT %08lx TVL2D %04lx voices %2lu starts %5lu "
                         "AC97 %6lu pad %4lu und %3lu FE %lu (%08lx=%08lx) FECTL %08lx ISTS %08lx%s\n",
                         (unsigned long)(now - start_tick), phase + 1, (unsigned long)g, (unsigned long)r[0x200C / 4],
                         (unsigned long)(r[0x2054 / 4] & 0xFFFFu), (unsigned long)active_hw_voices(),
                         (unsigned long)stress_starts, (unsigned long)as.chunks, (unsigned long)as.padded,
                         (unsigned long)as.underruns, (unsigned long)fs.events, (unsigned long)fs.last_meth,
                         (unsigned long)fs.last_param, (unsigned long)r[0x1100 / 4], (unsigned long)r[0x1000 / 4],
                         just_froze ? "   <== GP FRAMES STOPPED" : "");
                last_log = now;
            }
            if (just_froze) {
                /* The 2D list as the hardware sees it, each voice's record, then the driver's last operations */
                static apu_vp_trace_t tr[256];
                uint32_t n, j, h = ((volatile uint32_t *)NV_PAPU_BASE)[0x2054 / 4] & 0xFFFFu, guard = 0;
                log_line("  2D list from TVL2D:\n");
                while (h != 0xFFFFu && h < 256u && guard++ < 256u) {
                    const volatile uint32_t *rec = (const volatile uint32_t *)apu_vp_debug_voice_record(h);
                    if (!rec) break;
                    log_line("    h %3lu FMT %08lx BA %06lx LBO %06lx CBO %06lx EBO %06lx STATE %08lx VOLA %08lx "
                             "PITCH_LINK %08lx CUR %08lx %08lx\n",
                             (unsigned long)h, (unsigned long)rec[0x04 / 4], (unsigned long)rec[0x20 / 4],
                             (unsigned long)rec[0x24 / 4], (unsigned long)(rec[0x58 / 4] & 0xFFFFFFu),
                             (unsigned long)(rec[0x5C / 4] & 0xFFFFFFu), (unsigned long)rec[0x54 / 4],
                             (unsigned long)rec[0x60 / 4], (unsigned long)rec[0x7C / 4], (unsigned long)rec[0x38 / 4],
                             (unsigned long)rec[0x3C / 4]);
                    h = rec[0x7C / 4] & 0xFFFFu;
                }
                n = apu_vp_debug_trace(tr, 256u);
                log_line("  last %lu driver operations (op handle a XGSCNT):\n", (unsigned long)n);
                for (j = 0; j < n; j++) {
                    log_line("    %c %3u %08lx %08lx\n", tr[j].op, (unsigned)tr[j].h, (unsigned long)tr[j].a,
                             (unsigned long)tr[j].t);
                }
            }
            if (just_froze || now - last_save >= 5000u) {
                log_save();
                last_save = now;
            }
        }

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
            if (phase == 6) {
                ALint playing = 0, st, j;
                for (j = 0; j < STRESS_SOURCES; j++) {
                    alGetSourcei(stress[j], AL_SOURCE_STATE, &st);
                    if (st == AL_PLAYING) playing++;
                }
                debugPrint("\nstress: %lu starts, %ld of %d sources playing, %lu VP voices active\n",
                           (unsigned long)stress_starts, (long)playing, STRESS_SOURCES,
                           (unsigned long)active_hw_voices());
            }
            if (frozen_at) {
                debugPrint("\n*** GP FRAMES STOPPED (phase %d, %lu starts) ***\n", phase + 1,
                           (unsigned long)stress_starts);
            }
            debugPrint("\nReal console: VP -> GP FIFO -> AC97, panning in stereo.\n"
                       "xemu (real-time DSP off): VP tap, panning not heard.\n");
        }
        Sleep(5);
    }
    return 0;
}
