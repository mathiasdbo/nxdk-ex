/*
 * HRTF on the real MCPX APU, for listening (headphones work best).
 *
 * A burst of noise (4 a second, easy to localize) moves around the listener.
 * Each pattern plays twice, first through the APU's HRTF stage
 * (AL_XBOX_HRTF enabled), then with volume panning (disabled), so the two can
 * be compared:
 *   1  a full circle in the horizontal plane, 8 s
 *   2  front, right, behind, left: 2 s each
 *   3  up and down in front (elevation -30 .. +60 deg)
 * then it repeats. The screen and E:\openal_hrtf.txt show the pattern, the
 * mode, and whether the source's voice really runs on the HRTF stage (its
 * VP handle, the 3D list and its HRTF entry).
 *
 * The HRTF table is the library's own spherical-head model (src/apu_hrtf.c),
 * written to the APU in its measured encoding (samples/apu_hrtf_probe).
 */
#include <hal/debug.h>
#include <hal/video.h>
#include <math.h>
#include <nxdk/mount.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>

#include "al_source.h"
#include "apu_ac97.h"
#include "apu_voice.h"
#include "apu_vp.h"
#include "mcpx_apu_regs.h"

#define RATE    48000
#define BURST   12000u        /* 250 ms: 120 ms of noise, then silence */

static char s_log[48 * 1024];
static size_t s_log_len;

static void logf_(const char *fmt, ...) {
    char line[256];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if ((size_t)n >= sizeof(line)) n = (int)sizeof(line) - 1;
    if (s_log_len + (size_t)n > sizeof(s_log)) {
        memmove(s_log, s_log + sizeof(s_log) / 2u, s_log_len - sizeof(s_log) / 2u);
        s_log_len -= sizeof(s_log) / 2u;
    }
    memcpy(s_log + s_log_len, line, (size_t)n);
    s_log_len += (size_t)n;
    debugPrint("%s", line);
}

static void save_log(void) {
    FILE *f;
    if (!nxIsDriveMounted('E')) {
        nxMountDrive('E', "\\Device\\Harddisk0\\Partition1\\");
    }
    f = fopen("E:\\openal_hrtf.txt", "wb");
    if (f) {
        fwrite(s_log, 1, s_log_len, f);
        fclose(f);
    }
}

static int16_t s_pcm[BURST];

static void make_burst(void) {
    uint32_t i, r = 12345u;
    float lp = 0.0f;
    for (i = 0; i < BURST; i++) {
        float x = 0.0f;
        if (i < 5760u) {   /* 120 ms of noise with a soft attack and release */
            float env = (i < 240u) ? (float)i / 240.0f : ((i > 5280u) ? (float)(5760u - i) / 480.0f : 1.0f);
            r = r * 1664525u + 1013904223u;
            x = ((float)(int32_t)(r >> 8) / 8388608.0f - 1.0f);
            lp += 0.6f * (x - lp);   /* a little low-pass: less hiss, still broadband */
            x = lp * env;
        }
        s_pcm[i] = (int16_t)(x * 14000.0f);
    }
}

/* Where the source is for pattern p at time t (seconds into the pattern); false when it is over */
static bool position(int p, float t, float pos[3], const char **what) {
    const float r = 2.0f;
    float az, el;
    switch (p) {
    case 0:   /* circle: azimuth 0 (front) -> 360, clockwise seen from above (right first) */
        if (t >= 8.0f) return false;
        az = t / 8.0f * 6.2831853f;
        el = 0.0f;
        *what = "circle";
        break;
    case 1: { /* front, right, behind, left */
        static const char *names[4] = { "front", "right", "behind", "left" };
        int k = (int)(t / 2.0f);
        if (k >= 4) return false;
        az = (float)k * 1.5707963f;
        el = 0.0f;
        *what = names[k];
        break;
    }
    default:  /* elevation sweep in front: -30 -> +60 -> -30 deg */
        if (t >= 8.0f) return false;
        az = 0.0f;
        el = (t < 4.0f) ? (-30.0f + 22.5f * t) : (60.0f - 22.5f * (t - 4.0f));
        el *= 0.017453293f;
        *what = "up/down";
        break;
    }
    /* OpenAL: -z is in front of the default listener, +x right, +y up */
    pos[0] = r * sinf(az) * cosf(el);
    pos[1] = r * sinf(el);
    pos[2] = -r * cosf(az) * cosf(el);
    return true;
}

int main(void) {
    ALCdevice *dev;
    ALCcontext *ctx;
    ALuint buf, src;
    ALint hrtf_hw = 0;
    DWORD t_log = 0, t_save;

    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    logf_("openal_hrtf: HRTF on the APU vs volume panning (use headphones)\n");
    dev = alcOpenDevice(NULL);
    if (!dev) {
        logf_("alcOpenDevice failed\n");
        save_log();
        for (;;) Sleep(1000);
    }
    ctx = alcCreateContext(dev, NULL);
    alcMakeContextCurrent(ctx);
    alXboxGetHardwareStatus(AL_XBOX_HRTF_ACTIVE, &hrtf_hw);
    logf_("backend: %s; HRTF stage %s; front end %s\n", apu_voice_hw_backend() ? "MCPX APU" : "null (no sound)",
          hrtf_hw ? "available" : "NOT available", apu_vp_front_end_ok() ? "works (xemu)" : "stalled (console)");

    make_burst();
    alGenBuffers(1, &buf);
    alBufferData(buf, AL_FORMAT_MONO16, s_pcm, (ALsizei)sizeof(s_pcm), RATE);
    alGenSources(1, &src);
    alSourcei(src, AL_BUFFER, (ALint)buf);
    alSourcei(src, AL_LOOPING, AL_TRUE);
    alSourcef(src, AL_REFERENCE_DISTANCE, 2.0f);   /* constant level on the 2 m path */
    t_save = GetTickCount();

    for (;;) {
        int p, mode;
        for (p = 0; p < 3; p++) {
            for (mode = 0; mode < 2; mode++) {
                DWORD t0;
                const char *what = "";
                float pos[3];
                if (mode == 0) alEnable(AL_XBOX_HRTF);
                else alDisable(AL_XBOX_HRTF);
                position(p, 0.0f, pos, &what);
                alSource3f(src, AL_POSITION, pos[0], pos[1], pos[2]);
                alSourcePlay(src);   /* a fresh voice: the HRTF setting applies at play */
                logf_("-- pattern %d, %s\n", p + 1, mode == 0 ? "HRTF" : "volume panning");
                t0 = GetTickCount();
                for (;;) {
                    float t = (float)(GetTickCount() - t0) / 1000.0f;
                    if (!position(p, t, pos, &what)) break;
                    alSource3f(src, AL_POSITION, pos[0], pos[1], pos[2]);
                    alXboxUpdateVoices();
                    if (GetTickCount() - t_log >= 500u) {
                        ALsource *s = al_source_get(src);
                        uint32_t h = (s && s->hw_voice_idx >= 0) ? apu_voice_hw_handle((uint32_t)s->hw_voice_idx) : 0xFFFFu;
                        const volatile uint32_t *r = (h < APU_VP_MAX_HANDLES)
                                                         ? (const volatile uint32_t *)apu_vp_debug_voice_record(h)
                                                         : NULL;
                        apu_ac97_stats_t as;
                        apu_ac97_get_stats(&as);
                        t_log = GetTickCount();
                        logf_("   %-7s pos %5ld %5ld %5ld cm  handle %3lu %s entry %4lx  und %lu pad %lu\n", what,
                              (long)(pos[0] * 100.0f), (long)(pos[1] * 100.0f), (long)(pos[2] * 100.0f),
                              (unsigned long)h, (h < APU_VP_MAX_HANDLES && apu_vp_voice_is_hrtf(h)) ? "HRTF " : "plain",
                              r ? (unsigned long)(r[MCPX_VOICE_CFG_HRTF_TARGET / 4] & 0xFFFFu) : 0xFFFFul,
                              (unsigned long)as.underruns, (unsigned long)as.padded);
                    }
                    if (GetTickCount() - t_save >= 5000u) {
                        t_save = GetTickCount();
                        save_log();
                    }
                    Sleep(16);
                }
                alSourceStop(src);
                Sleep(300);
            }
        }
    }
    return 0;
}
