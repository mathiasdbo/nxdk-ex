/*
 * Are the APU's 48 kHz frame clock and the AC97 codec's clock locked?
 * (specs/task-phase7-00-clock-probe.md, the gate for the zero-copy output:
 * once the codec reads the GP's output ring directly, a drift between the two
 * clocks makes one side overtake the other.)
 *
 * The library runs as usual (copy path, output thread). A looping 480 Hz tone
 * plays, so a glitch is also audible. Once a second:
 *   gp  stereo frames the APU generated: GP frame counter (X:$10, 24 bits) x 32
 *   ac  stereo frames the codec played: buffers queued x 512 minus what is still
 *       queued, from CIV, LVI and PICB (16-bit samples left in the current buffer)
 *   d   gp - ac: the GP ring's backlog plus the AC97 queue. Locked clocks keep
 *       it within a buffer of where it started; a drift moves it steadily.
 * Each clock is also timed against the performance counter, so a drift shows
 * which side it is on. At the end: the least-squares slope of d in ppm and a
 * verdict. E:\apu_clock_probe_N.txt (a new N each run) gets the log.
 */
#include <hal/audio.h>
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

#include "apu_ac97.h"
#include "apu_gp.h"
#include "apu_hardware.h"
#include "apu_voice.h"
#include "apu_vp.h"

#define PROBE_SECONDS 420u   /* 7 minutes, as long as rounds G-J */
#define CHUNK         APU_AC97_CHUNK_FRAMES
#define GLITCH_FRAMES 256    /* a jump of d between two samples that no drift explains (100 ppm = 4.8 frames/s) */

#define AC97_PO_CIV   0x114u
#define AC97_PO_LVI   0x115u
#define AC97_PO_PICB  0x118u

extern AC97_DEVICE ac97Device;   /* nxdk hal/audio.c */

static char s_log[96 * 1024];
static size_t s_log_len;

static void logf_(const char *fmt, ...) {
    char line[256];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    if ((size_t)n >= sizeof(line)) {
        n = (int)sizeof(line) - 1;
    }
    if (s_log_len + (size_t)n < sizeof(s_log)) {
        memcpy(s_log + s_log_len, line, (size_t)n);
        s_log_len += (size_t)n;
    }
    debugPrint("%s", line);
}

/* v with `dec` decimals (0-3) as text: nxdk's printf has no %f. Eight results can be in use at once */
static const char *fx(double v, int dec) {
    static char buf[8][32];
    static unsigned k;
    static const char *const fmt[4] = { "%s%lld", "%s%lld.%01lld", "%s%lld.%02lld", "%s%lld.%03lld" };
    char *o = buf[k++ & 7u];
    long long m = 1, x;
    int i;
    if (dec < 0) dec = 0;
    if (dec > 3) dec = 3;
    for (i = 0; i < dec; i++) m *= 10;
    x = (long long)(fabs(v) * (double)m + 0.5);
    if (dec == 0) {
        snprintf(o, 32, fmt[0], v < 0 ? "-" : "", x);
    } else {
        snprintf(o, 32, fmt[dec], v < 0 ? "-" : "", x / m, x % m);
    }
    return o;
}

static void save_log(void) {
    static char name[40];
    FILE *f;
    if (!nxIsDriveMounted('E')) {
        nxMountDrive('E', "\\Device\\Harddisk0\\Partition1\\");
    }
    /* One file per run: E:\apu_clock_probe_1.txt, _2, ... (the first free name at the first save) */
    if (!name[0]) {
        int n;
        for (n = 1; n < 100; n++) {
            FILE *t;
            snprintf(name, sizeof(name), "E:\\apu_clock_probe_%d.txt", n);
            t = fopen(name, "rb");
            if (!t) {
                break;
            }
            fclose(t);
        }
    }
    f = fopen(name, "wb");
    if (f) {
        fwrite(s_log, 1, s_log_len, f);
        fclose(f);
    }
}

typedef struct {
    uint64_t gp;        /* stereo frames the APU generated (unwrapped) */
    uint64_t ac;        /* stereo frames the codec played */
    uint32_t picb;      /* raw PICB (16-bit samples left in the current buffer) */
    uint32_t tries;     /* reads until the sample was consistent */
    LARGE_INTEGER qpc;
} sample_t;

static uint32_t s_gp_raw_last;
static uint64_t s_gp_frames;   /* unwrapped GP frame count */
static uint32_t s_picb_bad;

/*
 * One consistent reading. The output thread moves LVI and the buffer count
 * while this runs (it queues, then counts), and the codec moves CIV and PICB:
 * read the count and CIV on both sides of the rest and retry if either moved.
 */
static void take_sample(sample_t *s) {
    volatile uint8_t *pb = (volatile uint8_t *)ac97Device.mmio;
    uintptr_t bar0 = (uintptr_t)NV_PAPU_BASE;
    apu_ac97_stats_t a0, a1;
    uint32_t civ0, civ1, lvi, picb, gp, tries = 0;

    do {
        tries++;
        apu_ac97_get_stats(&a0);
        civ0 = pb[AC97_PO_CIV] & 31u;
        lvi = pb[AC97_PO_LVI] & 31u;
        picb = *(volatile uint16_t *)(pb + AC97_PO_PICB);
        gp = apu_gp_frames(bar0);
        QueryPerformanceCounter(&s->qpc);
        civ1 = pb[AC97_PO_CIV] & 31u;
        apu_ac97_get_stats(&a1);
    } while ((civ0 != civ1 || a0.chunks != a1.chunks) && tries < 50u);

    s_gp_frames += (gp - s_gp_raw_last) & 0xFFFFFFu;
    s_gp_raw_last = gp;
    if (picb > 2u * CHUNK) {
        s_picb_bad++;
    }
    s->gp = s_gp_frames * APU_GP_FRAME;
    /* Queued: the primed silence and every buffer since; still to play: the
     * buffers after the current one up to LVI, and the rest of the current one */
    s->ac = (uint64_t)(a0.chunks + APU_AC97_PRIME) * CHUNK
          - (uint64_t)((lvi - civ0) & 31u) * CHUNK - picb / 2u;
    s->picb = picb;
    s->tries = tries;
}

/* PICB counts down within a buffer: a few quick readings must not go up unless CIV moved */
static void check_picb(void) {
    volatile uint8_t *pb = (volatile uint8_t *)ac97Device.mmio;
    uint32_t i, ups = 0, prev_civ = pb[AC97_PO_CIV] & 31u;
    uint32_t prev = *(volatile uint16_t *)(pb + AC97_PO_PICB);
    for (i = 0; i < 2000u; i++) {
        uint32_t civ = pb[AC97_PO_CIV] & 31u, p = *(volatile uint16_t *)(pb + AC97_PO_PICB);
        if ((pb[AC97_PO_CIV] & 31u) != civ) {
            continue;   /* the buffer changed during the reading */
        }
        if (civ == prev_civ && p > prev) {
            ups++;
        }
        prev = p;
        prev_civ = civ;
    }
    logf_("   PICB check: %lu increases within a buffer in 2000 reads (%s)\n", (unsigned long)ups,
          ups ? "PICB_BAD" : "counts down");
    if (ups) {
        s_picb_bad++;
    }
}

static ALuint s_buf, s_src;
static int16_t s_tone[48000];

static void start_tone(void) {
    uint32_t i;
    for (i = 0; i < 48000u; i++) {
        s_tone[i] = (int16_t)(6000.0f * sinf(6.28318531f * 480.0f * (float)i / 48000.0f));   /* 480 cycles: loops seamlessly */
    }
    alGenBuffers(1, &s_buf);
    alBufferData(s_buf, AL_FORMAT_MONO16, s_tone, (ALsizei)sizeof(s_tone), 48000);
    alGenSources(1, &s_src);
    alSourcei(s_src, AL_BUFFER, (ALint)s_buf);
    alSourcei(s_src, AL_SOURCE_RELATIVE, AL_TRUE);
    alSourcei(s_src, AL_LOOPING, AL_TRUE);
    alSourcePlay(s_src);
}

/* Keep the library ticking for `ms` */
static void run_for(DWORD ms) {
    DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < ms) {
        alXboxUpdateVoices();
        Sleep(16);
    }
}

int main(void) {
    ALCdevice *dev;
    ALCcontext *ctx;
    LARGE_INTEGER freq;
    sample_t s0, s, prev;
    double sx = 0, sy = 0, sxx = 0, sxy = 0, qhz;
    int64_t d0, d, d_prev = 0, dmin, dmax;
    uint32_t n = 0, glitches = 0, sec;
    apu_ac97_stats_t a;

    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    logf_("apu_clock_probe: APU frame clock vs AC97 codec clock, %u s\n", PROBE_SECONDS);
    dev = alcOpenDevice(NULL);
    ctx = dev ? alcCreateContext(dev, NULL) : NULL;
    if (ctx && !apu_ac97_active()) {
        /* xemu: its front end works, so the library leaves the output to the
         * emulated APU and never starts the AC97 copy. Start again the way a
         * console does, so there is an AC97 queue to measure */
        logf_("   AC97 inactive (front end works: xemu?), reopening with APU_VP_DBG_NO_FE_PROBE\n");
        alcDestroyContext(ctx);
        alcCloseDevice(dev);
        apu_vp_debug_set_flags(APU_VP_DBG_NO_FE_PROBE);
        dev = alcOpenDevice(NULL);
        ctx = dev ? alcCreateContext(dev, NULL) : NULL;
    }
    if (!ctx || !apu_voice_hw_backend() || !apu_ac97_active()) {
        logf_("no APU output (device %p, context %p, AC97 %s)\n", (void *)dev, (void *)ctx,
              apu_ac97_active() ? "active" : "inactive");
        save_log();
        for (;;) Sleep(1000);
    }
    alcMakeContextCurrent(ctx);
    QueryPerformanceFrequency(&freq);
    qhz = (double)freq.QuadPart;
    apu_ac97_get_stats(&a);
    logf_("   output thread %s, performance counter %s Hz\n", a.threaded ? "on" : "OFF", fx(qhz, 0));
    start_tone();
    run_for(2000);   /* let the start settle: the queue fills, the GP backlog stabilises */
    check_picb();

    s_gp_raw_last = apu_gp_frames((uintptr_t)NV_PAPU_BASE);
    s_gp_frames = 0;
    take_sample(&s0);
    prev = s0;
    d0 = (int64_t)s0.gp - (int64_t)s0.ac;
    dmin = dmax = 0;
    logf_("   start: gp %llu ac %llu d %lld frames\n", (unsigned long long)s0.gp, (unsigned long long)s0.ac,
          (long long)d0);
    logf_("    t   d-d0  gp_Hz     ac_Hz     und pad tries\n");
    save_log();

    for (sec = 1; sec <= PROBE_SECONDS; sec++) {
        double t, gp_hz, ac_hz, dt;
        run_for(1000);
        take_sample(&s);
        apu_ac97_get_stats(&a);
        d = ((int64_t)s.gp - (int64_t)s.ac) - d0;
        dt = (double)(s.qpc.QuadPart - prev.qpc.QuadPart) / qhz;
        gp_hz = (double)(s.gp - prev.gp) / dt;   /* the GP counts in 32-frame steps: +-32 Hz per second */
        ac_hz = (double)(s.ac - prev.ac) / dt;
        /* Every sample goes into the fit: a jump can be the codec catching up
         * (xemu's AC97 plays in bursts), and leaving those out would keep only
         * the ramps between them and show a drift that is not there */
        t = (double)(s.qpc.QuadPart - s0.qpc.QuadPart) / qhz;
        sx += t;
        sy += (double)d;
        sxx += t * t;
        sxy += t * (double)d;
        n++;
        if (d < dmin) dmin = d;
        if (d > dmax) dmax = d;
        if (llabs(d - d_prev) > GLITCH_FRAMES) {
            glitches++;   /* an underrun, a padded buffer, a burst of the codec or a torn reading */
            logf_("   %3u JUMP d by %lld frames to %lld\n", sec, (long long)(d - d_prev), (long long)d);
        }
        d_prev = d;
        if (sec <= 10u || sec % 10u == 0u) {
            logf_("   %3u %6lld  %s  %s  %lu %lu %lu\n", sec, (long long)d, fx(gp_hz, 2), fx(ac_hz, 2),
                  (unsigned long)a.underruns, (unsigned long)a.padded, (unsigned long)s.tries);
        }
        if (sec % 30u == 0u) {
            save_log();
        }
        prev = s;
    }

    {
        double total = (double)(s.qpc.QuadPart - s0.qpc.QuadPart) / qhz;
        double gp_hz = (double)(s.gp - s0.gp) / total, ac_hz = (double)(s.ac - s0.ac) / total;
        double slope = (n > 2u) ? ((double)n * sxy - sx * sy) / ((double)n * sxx - sx * sx) : 0.0;   /* frames/s */
        double ppm = slope / 48000.0 * 1e6;
        double ends = (double)d / total;   /* end to end, frames/s: a check on the fit */
        bool locked = fabs(ppm) < 2.0 && s_picb_bad == 0u;
        apu_ac97_get_stats(&a);
        logf_("== %s s: GP %s Hz, codec %s Hz (performance counter as the reference)\n", fx(total, 1), fx(gp_hz, 3),
              fx(ac_hz, 3));
        logf_("== d - d0: slope %s frames/s = %s ppm (end to end %s ppm), range %lld..%lld frames, %lu jumps, "
              "%lu underruns, %lu padded, PICB bad %lu\n",
              fx(slope, 3), fx(ppm, 2), fx(ends / 48000.0 * 1e6, 2), (long long)dmin, (long long)dmax,
              (unsigned long)glitches, (unsigned long)a.underruns, (unsigned long)a.padded, (unsigned long)s_picb_bad);
        if (locked && dmax - dmin < 64) {
            logf_("== LOCKED\n");
        } else if (locked) {
            /* no drift, but the codec's position moves in bursts: the zero-copy phase margin must cover the range */
            logf_("== LOCKED, BURSTY (d spans %lld frames)\n", (long long)(dmax - dmin));
        } else {
            logf_("== DRIFT %s ppm (one 128-frame slip every %s s)\n", fx(ppm, 2),
                  fx(fabs(slope) > 1e-9 ? 128.0 / fabs(slope) : 0.0, 0));
        }
    }
    logf_("done\n");
    save_log();
    for (;;) {
        alXboxUpdateVoices();
        Sleep(100);
    }
    return 0;
}
