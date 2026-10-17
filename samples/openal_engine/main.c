/*
 * The engine-facing OpenAL features on the real MCPX APU (phase B of
 * lib/openal/docs/XASH3D_INTEGRATION_PLAN.md), one phase each:
 *   1  loop points: a rising sweep once (the intro), then a steady tone loops
 *   2  direct gains: a tone panned hard left -> right -> left by the "engine"
 *   3  offsets: four one-second tones (C E G C'); play from 2 s, seek back to
 *      1 s every 1.5 s; the reported AL_SEC_OFFSET is logged
 *   4  pitch range: an 11.025 kHz 8-bit tone at pitch 0.25 .. 4; the playback
 *      rate measured from AL_SAMPLE_OFFSET is logged against the expected one
 *   5  voice budget: 80 sources with engine priorities: 64 get voices, the
 *      rest wait or stop; then restarts at random priorities
 *   6  buffer churn: 600 buffers of random sizes created, played and deleted
 *      (the APU's sample space must be reclaimed)
 *   7-10 CPU time and the output callback, static buffers, ADPCM, streamed ADPCM
 *  11  zero-copy output: the codec reads the GP ring; copy mode under an output callback
 * E:\openal_engine_N.txt (a new N each run) gets the results. The APU frame counter is checked
 * after every phase: if it stops, the log says so.
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

#include "al_buffer.h"
#include "apu_ac97.h"
#include "apu_gp.h"
#include "apu_hardware.h"
#include "apu_voice.h"
#include "apu_vp.h"
#include "mcpx_apu_regs.h"

#define TWO_PI 6.28318531f

static char s_log[64 * 1024];
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

static void save_log(void) {
    FILE *f;
    if (!nxIsDriveMounted('E')) {
        nxMountDrive('E', "\\Device\\Harddisk0\\Partition1\\");
    }
    /* One file per run: E:\openal_engine_1.txt, _2, ... (the first free name at the first save) */
    {
        static char name[40];
        if (!name[0]) {
            int n;
            for (n = 1; n < 100; n++) {
                FILE *t;
                snprintf(name, sizeof(name), "E:\\openal_engine_%d.txt", n);
                t = fopen(name, "rb");
                if (!t) {
                    break;
                }
                fclose(t);
            }
        }
        f = fopen(name, "wb");
    }
    if (f) {
        fwrite(s_log, 1, s_log_len, f);
        fclose(f);
    }
}

/* Keep the library ticking while waiting */
static void run_for(DWORD ms) {
    DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < ms) {
        alXboxUpdateVoices();
        Sleep(16);
    }
}

static uint32_t s_gp_last;

static void check_apu(const char *phase) {
    uint32_t gp = apu_gp_frames((uintptr_t)NV_PAPU_BASE);
    apu_ac97_stats_t as;
    ALint free_v = -1, pages = -1, err = alGetError();
    apu_ac97_get_stats(&as);
    alXboxGetHardwareStatus(AL_XBOX_FREE_VOICES, &free_v);
    alXboxGetHardwareStatus(AL_XBOX_SAMPLE_PAGES_USED, &pages);
    logf_("   [%s] APU %s (GP %lu), AC97 und %lu pad %lu, free voices %ld, sample pages %ld, AL error 0x%x\n", phase,
          gp != s_gp_last ? "running" : "STALLED", (unsigned long)gp, (unsigned long)as.underruns,
          (unsigned long)as.padded, (long)free_v, (long)pages, (unsigned)err);
    s_gp_last = gp;
    save_log();
}

static ALuint make_buffer(const int16_t *pcm, uint32_t frames, ALsizei rate) {
    ALuint b;
    alGenBuffers(1, &b);
    alBufferData(b, AL_FORMAT_MONO16, pcm, (ALsizei)(frames * 2u), rate);
    return b;
}

static ALuint make_source(ALuint buf) {
    ALuint s;
    alGenSources(1, &s);
    alSourcei(s, AL_BUFFER, (ALint)buf);
    alSourcei(s, AL_SOURCE_RELATIVE, AL_TRUE);
    return s;
}

static void free_source(ALuint s, ALuint b) {
    alSourceStop(s);
    alSourcei(s, AL_BUFFER, 0);
    alDeleteSources(1, &s);
    alDeleteBuffers(1, &b);
}

static int16_t s_pcm[4 * 48000];

/* 1: intro sweep 0.6 s (200 -> 800 Hz), then 0.25 s of 440 Hz as the loop */
static void phase_loop_points(void) {
    const uint32_t intro = 28800, loop = 12000;
    float ph = 0.0f;
    uint32_t i;
    ALuint b, s;
    ALint lp[2];
    for (i = 0; i < intro + loop; i++) {
        float f = (i < intro) ? 200.0f + 600.0f * (float)i / (float)intro : 440.0f;
        ph += TWO_PI * f / 48000.0f;
        s_pcm[i] = (int16_t)(7000.0f * sinf(ph));
    }
    b = make_buffer(s_pcm, intro + loop, 48000);
    lp[0] = (ALint)intro;
    lp[1] = (ALint)(intro + loop);
    alBufferiv(b, AL_LOOP_POINTS_SOFT, lp);
    s = make_source(b);
    alSourcei(s, AL_LOOPING, AL_TRUE);
    logf_("-- 1 loop points: sweep once, then 440 Hz steady (6 s)\n");
    alSourcePlay(s);
    {
        int k;
        for (k = 0; k < 6; k++) {
            ALint off = 0;
            run_for(1000);
            alGetSourcei(s, AL_SAMPLE_OFFSET, &off);
            logf_("   t %d s: offset %ld (%s)\n", k + 1, (long)off, off >= (ALint)intro ? "in the loop" : "INTRO AGAIN?");
        }
    }
    free_source(s, b);
    check_apu("loop points");
}

/* 2: the engine pans a tone with direct gains */
static void phase_direct(void) {
    uint32_t i;
    ALuint b, s;
    int k;
    for (i = 0; i < 48000u; i++) s_pcm[i] = (int16_t)(7000.0f * sinf(TWO_PI * 330.0f * (float)i / 48000.0f));
    b = make_buffer(s_pcm, 48000u, 48000);
    s = make_source(b);
    alSourcei(s, AL_LOOPING, AL_TRUE);
    logf_("-- 2 direct gains: left -> right -> left (6 s)\n");
    {
        const ALfloat g0[2] = { 1.0f, 0.0f };
        alSourcefv(s, AL_XBOX_DIRECT_GAINS, g0);
    }
    alSourcePlay(s);
    for (k = 0; k <= 360; k++) {
        float x = (k <= 180) ? (float)k / 180.0f : (float)(360 - k) / 180.0f;   /* 0 = left, 1 = right */
        ALfloat g[2];
        g[0] = cosf(x * 1.5707963f);
        g[1] = sinf(x * 1.5707963f);
        alSourcefv(s, AL_XBOX_DIRECT_GAINS, g);
        alXboxUpdateVoices();
        Sleep(16);
    }
    free_source(s, b);
    check_apu("direct gains");
}

/* 3: offsets */
static void phase_offsets(void) {
    static const float notes[4] = { 261.6f, 329.6f, 392.0f, 523.3f };
    uint32_t i;
    ALuint b, s;
    int k;
    for (i = 0; i < 4u * 48000u; i++) {
        s_pcm[i] = (int16_t)(7000.0f * sinf(TWO_PI * notes[i / 48000u] * (float)(i % 48000u) / 48000.0f));
    }
    b = make_buffer(s_pcm, 4u * 48000u, 48000);
    s = make_source(b);
    logf_("-- 3 offsets: play from 2.0 s (G), seek to 1.0 s (E) every 1.5 s\n");
    alSourcef(s, AL_SEC_OFFSET, 2.0f);
    alSourcePlay(s);
    for (k = 0; k < 12; k++) {
        ALfloat sec = -1.0f;
        if (k % 6 == 5) {
            alSourcef(s, AL_SEC_OFFSET, 1.0f);
        }
        run_for(250);
        alGetSourcef(s, AL_SEC_OFFSET, &sec);
        logf_("   %4d ms: AL_SEC_OFFSET %ld ms\n", (k + 1) * 250, (long)(sec * 1000.0f));   /* nxdk printf has no %f */
    }
    free_source(s, b);
    check_apu("offsets");
}

/* 4: pitch range on an 11.025 kHz 8-bit tone */
static void phase_pitch(void) {
    static const float pitches[] = { 0.25f, 0.5f, 1.0f, 2.0f, 2.55f, 4.0f };
    static uint8_t u8[11025];
    uint32_t i, k;
    ALuint b, s;
    for (i = 0; i < 11025u; i++) u8[i] = (uint8_t)(128 + (int)(50.0f * sinf(TWO_PI * 441.0f * (float)i / 11025.0f)));
    alGenBuffers(1, &b);
    alBufferData(b, AL_FORMAT_MONO8, u8, 11025, 11025);
    s = make_source(b);
    alSourcei(s, AL_LOOPING, AL_TRUE);
    logf_("-- 4 pitch range: 11.025 kHz mono8, 441 Hz tone\n");
    alSourcePlay(s);
    for (k = 0; k < sizeof(pitches) / sizeof(pitches[0]); k++) {
        ALint p0 = 0, p1 = 0;
        DWORD t0, t1;
        alSourcef(s, AL_PITCH, pitches[k]);
        run_for(300);
        alGetSourcei(s, AL_SAMPLE_OFFSET, &p0);
        t0 = GetTickCount();
        run_for(500);
        alGetSourcei(s, AL_SAMPLE_OFFSET, &p1);
        t1 = GetTickCount();
        {
            int32_t adv = (int32_t)p1 - (int32_t)p0;
            float expect = 11025.0f * pitches[k] * (float)(t1 - t0) / 1000.0f;
            while ((float)adv < expect - 11025.0f / 2.0f) adv += 11025;   /* wrapped around the loop */
            logf_("   pitch x%ld/100: %ld frames in %lu ms, expected %ld (measured/expected %ld/1000)\n",
                  (long)(pitches[k] * 100.0f), (long)adv, (unsigned long)(t1 - t0), (long)expect,
                  (long)(1000.0f * (float)adv / expect));
        }
    }
    free_source(s, b);
    check_apu("pitch");
}

/* 5: more sources than hardware voices, with engine priorities */
static void phase_budget(void) {
    enum { N = 140 };   /* more than the hardware voices (64 on xemu, 120 on a console) */
    static ALuint src[N];
    uint32_t i;
    ALuint b;
    ALint hw = 64;
    int k, voiced = 0, waiting = 0, stopped = 0;
    for (i = 0; i < 24000u; i++) s_pcm[i] = (int16_t)(1500.0f * sinf(TWO_PI * (200.0f + 3.0f * (float)(i % 7)) * (float)i / 48000.0f));
    b = make_buffer(s_pcm, 24000u, 48000);
    alXboxGetHardwareStatus(AL_XBOX_HW_VOICE_COUNT, &hw);
    logf_("-- 5 voice budget: %d looping sources on %ld hardware voices, priority = index; odd ones may not wait\n", N,
          (long)hw);
    alGenSources(N, src);
    for (i = 0; i < N; i++) {
        alSourcei(src[i], AL_BUFFER, (ALint)b);
        alSourcei(src[i], AL_LOOPING, AL_TRUE);
        alSourcei(src[i], AL_SOURCE_RELATIVE, AL_TRUE);
        alSourcef(src[i], AL_GAIN, 0.2f);
        alSourcef(src[i], AL_XBOX_PRIORITY, (ALfloat)i);
        alSourcei(src[i], AL_XBOX_VIRTUALIZE, (i & 1u) ? AL_FALSE : AL_TRUE);
        alSourcef(src[i], AL_PITCH, 0.8f + 0.005f * (float)i);
        alSourcePlay(src[i]);
    }
    run_for(1000);
    for (i = 0; i < N; i++) {
        ALint st = 0, hv = 0;
        alGetSourcei(src[i], AL_SOURCE_STATE, &st);
        alGetSourcei(src[i], AL_XBOX_HAS_VOICE, &hv);
        if (hv) voiced++;
        else if (st == AL_PLAYING) waiting++;
        else stopped++;
    }
    logf_("   %d with a voice, %d waiting, %d stopped (expect %ld / %ld / %ld)\n", voiced, waiting, stopped, (long)hw,
          (long)((N - hw) / 2), (long)((N - hw) / 2));
    check_apu("budget");
    for (k = 0; k < 100; k++) {   /* 10 s of restarts at random priorities */
        uint32_t j = (uint32_t)rand() % N;
        alSourcef(src[j], AL_XBOX_PRIORITY, (ALfloat)(rand() % 100));
        alSourcePlay(src[j]);
        run_for(100);
    }
    voiced = 0;
    for (i = 0; i < N; i++) {
        ALint hv = 0;
        alGetSourcei(src[i], AL_XBOX_HAS_VOICE, &hv);
        voiced += hv ? 1 : 0;
    }
    logf_("   after 100 restarts: %d with a voice\n", voiced);
    for (i = 0; i < N; i++) {
        alSourceStop(src[i]);
        alSourcei(src[i], AL_BUFFER, 0);
    }
    alDeleteSources(N, src);
    alDeleteBuffers(1, &b);
    run_for(100);
    check_apu("budget churn");
}

/* 6: buffer churn: the sample space is reclaimed */
static void phase_churn(void) {
    enum { LIVE = 24 };
    static ALuint bufs[LIVE], srcs[LIVE];
    int k, fails = 0;
    ALint pages_max = 0;
    for (k = 0; k < 48000; k++) s_pcm[k] = (int16_t)(2500.0f * sinf(TWO_PI * 600.0f * (float)k / 48000.0f));
    logf_("-- 6 buffer churn: 600 buffers of 2..90 KB played and deleted (20 s)\n");
    memset(bufs, 0, sizeof(bufs));
    memset(srcs, 0, sizeof(srcs));
    for (k = 0; k < 600; k++) {
        int slot = k % LIVE;
        uint32_t frames = 1000u + (uint32_t)(rand() % 44000);
        ALint st = 0, pages = 0;
        if (srcs[slot]) {
            free_source(srcs[slot], bufs[slot]);
        }
        bufs[slot] = make_buffer(s_pcm, frames, 48000);
        srcs[slot] = make_source(bufs[slot]);
        alSourcef(srcs[slot], AL_GAIN, 0.15f);
        alSourcePlay(srcs[slot]);
        alGetSourcei(srcs[slot], AL_SOURCE_STATE, &st);
        if (st != AL_PLAYING) fails++;
        alXboxGetHardwareStatus(AL_XBOX_SAMPLE_PAGES_USED, &pages);
        if (pages > pages_max) pages_max = pages;
        alXboxUpdateVoices();
        Sleep(33);
    }
    for (k = 0; k < LIVE; k++) {
        if (srcs[k]) free_source(srcs[k], bufs[k]);
    }
    run_for(100);
    logf_("   %d starts did not play, most sample pages in use %ld of 2048\n", fails, (long)pages_max);
    check_apu("churn");
}

/* 7: AL_XBOX_stats and AL_XBOX_output_callback: CPU time of the output thread
 * and the voice tick, and a callback that sees (and for 2 s halves) the mix */
static volatile uint32_t s_cb_calls, s_cb_frames, s_cb_peak;
static volatile int s_cb_halve;

static void AL_APIENTRY output_cb(ALshort *samples, ALsizei frames, ALvoid *user) {
    ALsizei i;
    uint32_t peak = s_cb_peak;
    (void)user;
    for (i = 0; i < 2 * frames; i++) {
        uint32_t a = (uint32_t)(samples[i] < 0 ? -samples[i] : samples[i]);
        if (a > peak) peak = a;
        if (s_cb_halve) samples[i] = (ALshort)(samples[i] / 2);
    }
    s_cb_peak = peak;
    s_cb_frames += (uint32_t)frames;
    s_cb_calls++;
}

static void stats_window(const char *what, uint32_t ms) {
    ALXBOXSTATS a, b;
    uint32_t c0 = s_cb_calls, f0 = s_cb_frames;
    double hz;
    memset(&a, 0, sizeof(a));
    a.size = sizeof(a);
    b = a;
    s_cb_peak = 0;
    alXboxGetStats(&a);
    run_for(ms);
    alXboxGetStats(&b);
    hz = b.cycle_hz ? (double)b.cycle_hz : 733333333.0;
    /* per second of the window, in microseconds of CPU (nxdk printf has no %f) */
    logf_("   %s: clock %lu Hz; output thread %lu passes, %lu us/s (longest pass %lu us, gap %lu ms), callback %lu us/s; "
          "voice tick %lu calls, %lu us/s (longest %lu us); AC97 +%lu buffers, und %lu pad %lu; callback %lu calls, "
          "%lu frames, peak %lu\n",
          what, (unsigned long)b.cycle_hz, (unsigned long)(b.output_passes - a.output_passes),
          (unsigned long)((double)(b.output_cycles - a.output_cycles) * 1e6 / hz * 1000.0 / ms),
          (unsigned long)((double)b.output_max_cycles * 1e6 / hz), (unsigned long)b.output_max_gap_ms,
          (unsigned long)((double)(b.output_hook_cycles - a.output_hook_cycles) * 1e6 / hz * 1000.0 / ms),
          (unsigned long)(b.update_calls - a.update_calls),
          (unsigned long)((double)(b.update_cycles - a.update_cycles) * 1e6 / hz * 1000.0 / ms),
          (unsigned long)((double)b.update_max_cycles * 1e6 / hz), (unsigned long)(b.ac97_buffers - a.ac97_buffers),
          (unsigned long)b.ac97_underruns, (unsigned long)b.ac97_padded, (unsigned long)(s_cb_calls - c0),
          (unsigned long)(s_cb_frames - f0), (unsigned long)s_cb_peak);
}

/* Smoothness of the left channel: a 480 Hz sine of amplitude A has
 * |x[n+1] - 2 x[n] + x[n-1]| <= A (2 pi 480 / 48000)^2, about 15 here, and the
 * one-second buffer holds a whole number of cycles (no jump at the loop); a
 * stale cache line (data from an earlier lap of the ring) shows as a jump */
static volatile uint32_t s_glitches, s_glitch_samples;
static int s_g1, s_g2;

static void AL_APIENTRY glitch_cb(ALshort *samples, ALsizei frames, ALvoid *user) {
    ALsizei i;
    uint32_t g = 0;
    (void)user;
    for (i = 0; i < frames; i++) {
        int x = samples[2 * i], d = x - 2 * s_g1 + s_g2;
        if (d > 300 || d < -300) g++;
        s_g2 = s_g1;
        s_g1 = x;
    }
    s_glitches += g;
    s_glitch_samples += (uint32_t)frames;
}

static void glitch_window(const char *what, uint32_t ms) {
    s_glitches = 0;
    s_glitch_samples = 0;
    alXboxOutputCallback(glitch_cb, NULL);
    run_for(ms);
    alXboxOutputCallback(NULL, NULL);
    logf_("   %s: %lu jumps in %lu samples\n", what, (unsigned long)s_glitches, (unsigned long)s_glitch_samples);
}

static void phase_stats(void) {
    float ph = 0.0f;
    uint32_t i;
    ALuint b, s;
    for (i = 0; i < 48000u; i++) {
        ph += TWO_PI * 480.0f / 48000.0f;
        s_pcm[i] = (int16_t)(8000.0f * sinf(ph));
    }
    b = make_buffer(s_pcm, 48000u, 48000);
    s = make_source(b);
    alSourcei(s, AL_LOOPING, AL_TRUE);
    logf_("-- 7 CPU time (AL_XBOX_stats), the GP ring (cached by default) and the output callback "
          "(480 Hz; the 6th window is quieter)\n");
    alSourcePlay(s);
    stats_window("cached    ", 2000);      /* the default: cached ring, 32-bit reads */
    glitch_window("cached ring, jump check", 3000);
    apu_gp_debug_set_fifo_cached(false);
    apu_gp_debug_set_fifo_read(2);
    stats_window("uncached64", 2000);      /* the previous default */
    glitch_window("uncached ring, jump check", 2000);
    apu_gp_debug_set_fifo_read(0);
    apu_gp_debug_set_fifo_cached(true);
    alXboxOutputCallback(output_cb, NULL);
    stats_window("callback  ", 2000);
    s_cb_halve = 1;
    stats_window("halving   ", 2000);
    s_cb_halve = 0;
    alXboxOutputCallback(NULL, NULL);
    stats_window("removed   ", 2000);
    free_source(s, b);
    check_apu("stats");
}

/* 8: AL_EXT_STATIC_BUFFER. An output callback measures the left channel of
 * the final mix: loud samples, peak, rising zero crossings (frequency), jumps
 * (a sine's second difference is small) and silent gaps between loud parts. */
typedef struct {
    uint32_t frames, loud, peak, crossings, jumps, gaps, quiet_run;
    int prev1, prev2;
    bool seen_loud;
} mix_scan_t;

static mix_scan_t s_scan;
static volatile int s_scan_on;

static void AL_APIENTRY scan_cb(ALshort *samples, ALsizei frames, ALvoid *user) {
    ALsizei i;
    mix_scan_t *m = &s_scan;
    (void)user;
    if (!s_scan_on) return;
    for (i = 0; i < frames; i++) {
        int x = samples[2 * i], a = x < 0 ? -x : x, d = x - 2 * m->prev1 + m->prev2;
        if (a > 300) {
            if (m->seen_loud && m->quiet_run >= 48u) m->gaps++;   /* 1 ms of silence inside the sound */
            m->seen_loud = true;
            m->quiet_run = 0;
            m->loud++;
        } else {
            m->quiet_run++;
        }
        if ((uint32_t)a > m->peak) m->peak = (uint32_t)a;
        if (m->prev1 < 0 && x >= 0) m->crossings++;
        if (m->seen_loud && a > 300 && (d > 400 || d < -400)) m->jumps++;
        m->prev2 = m->prev1;
        m->prev1 = x;
    }
    m->frames += (uint32_t)frames;
}

static void scan_start(void) {
    s_scan_on = 0;
    memset(&s_scan, 0, sizeof(s_scan));
    s_scan_on = 1;
}

static void scan_log(const char *what) {
    s_scan_on = 0;
    logf_("   %s: %lu frames, %lu loud, peak %lu, %lu Hz, %lu jumps, %lu gaps\n", what, (unsigned long)s_scan.frames,
          (unsigned long)s_scan.loud, (unsigned long)s_scan.peak,
          (unsigned long)(s_scan.frames ? (uint64_t)s_scan.crossings * 48000u / s_scan.frames : 0),
          (unsigned long)s_scan.jumps, (unsigned long)s_scan.gaps);
}

/* A sine of `hz` with a whole number of cycles per `frames` (exact phase from the index) */
static void fill_tone(int16_t *dst, uint32_t frames, float hz) {
    uint32_t i;
    for (i = 0; i < frames; i++) {
        dst[i] = (int16_t)(8000.0f * sinf(TWO_PI * hz * (float)(i % 48000u) / 48000.0f));
    }
}

/* Breaks in the physical pages of a static buffer's list (0 = contiguous) */
static uint32_t page_breaks(ALuint b) {
    const ALbuffer *pb = al_buffer_get(b);
    uint32_t i, n = 0;
    for (i = 1; pb && i + 1u < pb->page_count; i++) {
        if (pb->pages[i] != pb->pages[i - 1u] + 4096u) n++;
    }
    return n;
}

static void phase_static(void) {
    const uint32_t frames = 48000u;
    uint8_t *raw = (uint8_t *)malloc(frames * 2u + 4096u);
    int16_t *tone = (int16_t *)(void *)(raw + 0x2A6);      /* any offset */
    uint8_t *raw2 = NULL;
    ALuint b, b2, s, q[5];
    ALint st = 0, held = 0, ref = 0;
    uint32_t i;

    logf_("-- 8 static buffers (AL_EXT_STATIC_BUFFER): the application's memory played in place\n");
    if (!raw) {
        logf_("   malloc failed\n");
        return;
    }
    fill_tone(tone, frames, 480.0f);
    alXboxOutputCallback(scan_cb, NULL);

    /* a: the same 1 s tone copied and static, looping 2 s each */
    b = make_buffer(tone, frames, 48000);
    s = make_source(b);
    alSourcei(s, AL_LOOPING, AL_TRUE);
    alSourcePlay(s);
    run_for(200);
    scan_start();
    run_for(2000);
    scan_log("copied 480 Hz ");
    free_source(s, b);
    alGenBuffers(1, &b2);
    alBufferDataStatic(b2, AL_FORMAT_MONO16, tone, (ALsizei)(frames * 2u), 48000);
    alXboxGetHardwareStatus(AL_XBOX_BUFFER_BYTES, &held);
    alXboxGetHardwareStatus(AL_XBOX_STATIC_BUFFER_BYTES, &ref);
    logf_("   static buffer: %lu pages (%lu breaks in physical contiguity), library holds %ld bytes, references %ld; "
          "AL error 0x%x\n", (unsigned long)al_buffer_get(b2)->page_count, (unsigned long)page_breaks(b2), (long)held,
          (long)ref, alGetError());
    s = make_source(b2);
    alSourcei(s, AL_LOOPING, AL_TRUE);
    alSourcePlay(s);
    run_for(200);
    scan_start();
    run_for(2000);
    scan_log("static 480 Hz ");
    alSourceStop(s);
    alSourcei(s, AL_BUFFER, 0);

    /* b: fresh memory written just before, no cache write-back: does the APU see it? */
    raw2 = (uint8_t *)malloc(frames * 2u + 4096u);
    if (raw2) {
        int16_t *t2 = (int16_t *)(void *)(raw2 + 0x100);
        al_buffer_debug_set_static_flush(false);
        fill_tone(t2, frames, 960.0f);
        alBufferDataStatic(b2, AL_FORMAT_MONO16, t2, (ALsizei)(frames * 2u), 48000);
        al_buffer_debug_set_static_flush(true);
        alSourcei(s, AL_BUFFER, (ALint)b2);
        alSourcePlay(s);
        run_for(200);
        scan_start();
        run_for(2000);
        scan_log("no flush 960Hz");
        alSourceStop(s);
        alSourcei(s, AL_BUFFER, 0);
    }

    /* c: a word's middle: 0.25 s to 0.5 s of the tone, one-shot */
    alBufferDataStatic(b2, AL_FORMAT_MONO16, tone + 12000, 24000, 48000);
    alSourcei(s, AL_LOOPING, AL_FALSE);
    alSourcei(s, AL_BUFFER, (ALint)b2);
    scan_start();
    alSourcePlay(s);
    {
        DWORD t0 = GetTickCount(), t_stop = 0;
        while (GetTickCount() - t0 < 600u) {
            alGetSourcei(s, AL_SOURCE_STATE, &st);
            if (st != AL_PLAYING && t_stop == 0) t_stop = GetTickCount() - t0;
            alXboxUpdateVoices();
            Sleep(1);
        }
        scan_log("sub-range 0.25 s (12000 frames expected loud)");
        logf_("   AL_STOPPED reported after %lu ms (250 ms of data)\n", (unsigned long)t_stop);
    }
    alSourcei(s, AL_BUFFER, 0);
    alDeleteSources(1, &s);
    alDeleteBuffers(1, &b2);

    /* d: a sentence: five consecutive 0.2 s pieces of the tone queued on one source */
    alGenBuffers(5, q);
    for (i = 0; i < 5u; i++) {
        alBufferDataStatic(q[i], AL_FORMAT_MONO16, tone + 9600u * i, 9600 * 2, 48000);
    }
    alGenSources(1, &s);
    alSourcei(s, AL_SOURCE_RELATIVE, AL_TRUE);
    alSourceQueueBuffers(s, 5, q);
    scan_start();
    alSourcePlay(s);
    run_for(1300);
    scan_log("5 queued pieces (48000 loud, 0 gaps expected)");
    alGetSourcei(s, AL_SOURCE_STATE, &st);
    logf_("   queue state after 1.3 s: %s\n", st == AL_STOPPED ? "AL_STOPPED" : "still playing");
    alSourceStop(s);
    alSourcei(s, AL_BUFFER, 0);
    alDeleteSources(1, &s);
    alDeleteBuffers(5, q);

    alXboxOutputCallback(NULL, NULL);
    free(raw);
    free(raw2);
    check_apu("static");
}

/* 9: AL_XBOX_adpcm and the lower pitch floor, through OpenAL */
static const int16_t k_ima_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
    118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767
};

static uint8_t ima_code(int *pred, int *index, int target) {
    static const int8_t adj[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };
    int step = k_ima_step[*index], diff = target - *pred, delta = step >> 3;
    uint8_t n = 0;
    if (diff < 0) { n = 8; diff = -diff; }
    if (diff >= step) { n |= 4; diff -= step; delta += step; }
    if (diff >= step >> 1) { n |= 2; diff -= step >> 1; delta += step >> 1; }
    if (diff >= step >> 2) { n |= 1; delta += step >> 2; }
    *pred += (n & 8) ? -delta : delta;
    if (*pred > 32767) *pred = 32767;
    if (*pred < -32768) *pred = -32768;
    *index += adj[n & 7];
    if (*index < 0) *index = 0;
    if (*index > 88) *index = 88;
    return n;
}

/* Xbox ADPCM: 64 samples per 36-byte block and channel (the header holds the
 * first; the 64th code repeats the next block's header) */
static void xbox_adpcm_encode(uint8_t *dst, const int16_t *src, uint32_t blocks, int ch) {
    int pred[2] = { 0, 0 }, index[2] = { 0, 0 }, c, k;
    uint32_t b, i;
    for (b = 0; b < blocks; b++) {
        const int16_t *in = src + (size_t)b * 64u * (uint32_t)ch;
        for (c = 0; c < ch; c++) {
            pred[c] = in[c];
            *dst++ = (uint8_t)(in[c] & 0xFF);
            *dst++ = (uint8_t)((uint16_t)in[c] >> 8);
            *dst++ = (uint8_t)index[c];
            *dst++ = 0;
        }
        for (i = 0; i < 8u; i++) {
            for (c = 0; c < ch; c++) {
                for (k = 0; k < 4; k++) {
                    uint32_t s1 = 1u + i * 8u + (uint32_t)k * 2u;
                    uint8_t lo = ima_code(&pred[c], &index[c], in[s1 * (uint32_t)ch + (uint32_t)c]);
                    uint8_t hi = ima_code(&pred[c], &index[c], in[(s1 + 1u) * (uint32_t)ch + (uint32_t)c]);
                    *dst++ = (uint8_t)(lo | (hi << 4));
                }
            }
        }
    }
}

static void phase_adpcm(void) {
    const uint32_t blocks = 750u;                 /* 48000 samples: 1 s */
    int16_t *pcm = (int16_t *)malloc((blocks * 64u + 2u) * 2u * sizeof(int16_t));
    uint8_t *mono = (uint8_t *)malloc(blocks * 36u), *stereo = (uint8_t *)malloc(blocks * 72u + 0x40);
    ALuint b, s;
    ALint st = 0, off0 = 0, off1 = 0;
    uint32_t i;

    logf_("-- 9 Xbox ADPCM buffers (decoded by the APU) and the pitch floor\n");
    if (!pcm || !mono || !stereo) {
        logf_("   malloc failed\n");
        return;
    }
    alXboxOutputCallback(scan_cb, NULL);
    /* a: mono, copied, looping */
    for (i = 0; i < blocks * 64u + 1u; i++) pcm[i] = (int16_t)(8000.0f * sinf(TWO_PI * 480.0f * (float)(i % 48000u) / 48000.0f));
    xbox_adpcm_encode(mono, pcm, blocks, 1);
    alGenBuffers(1, &b);
    alBufferData(b, AL_FORMAT_MONO_XBOX_ADPCM, mono, (ALsizei)(blocks * 36u), 48000);
    alGetBufferi(b, AL_BITS, &st);
    logf_("   mono ADPCM buffer: %u bytes, AL_BITS %ld, AL error 0x%x\n", (unsigned)(blocks * 36u), (long)st, alGetError());
    s = make_source(b);
    alSourcei(s, AL_LOOPING, AL_TRUE);
    alSourcePlay(s);
    run_for(200);
    scan_start();
    run_for(2000);
    scan_log("ADPCM mono 480 Hz, copied ");
    /* c: the same as a one-shot: it ends after 1 s */
    alSourceStop(s);
    alSourcei(s, AL_LOOPING, AL_FALSE);
    scan_start();
    alSourcePlay(s);
    {
        DWORD t0 = GetTickCount(), t_stop = 0;
        while (GetTickCount() - t0 < 1300u) {
            alGetSourcei(s, AL_SOURCE_STATE, &st);
            if (st != AL_PLAYING && t_stop == 0) t_stop = GetTickCount() - t0;
            alXboxUpdateVoices();
            Sleep(1);
        }
        scan_log("ADPCM one-shot 1 s (48000 frames)");
        logf_("   AL_STOPPED after %lu ms\n", (unsigned long)t_stop);
    }
    free_source(s, b);

    /* b: stereo (480 Hz left, 960 Hz right), static, looping; the scan reads the left */
    for (i = 0; i < blocks * 64u + 1u; i++) {
        pcm[2u * i] = (int16_t)(8000.0f * sinf(TWO_PI * 480.0f * (float)(i % 48000u) / 48000.0f));
        pcm[2u * i + 1u] = (int16_t)(8000.0f * sinf(TWO_PI * 960.0f * (float)(i % 48000u) / 48000.0f));
    }
    xbox_adpcm_encode(stereo + 0x14, pcm, blocks, 2);
    alGenBuffers(1, &b);
    alBufferDataStatic(b, AL_FORMAT_STEREO_XBOX_ADPCM, stereo + 0x14, (ALsizei)(blocks * 72u), 48000);
    s = make_source(b);
    alSourcei(s, AL_LOOPING, AL_TRUE);
    alSourcePlay(s);
    run_for(200);
    scan_start();
    run_for(2000);
    scan_log("ADPCM stereo, static, left");
    free_source(s, b);

    /* d: pitch 1/64 (below the old 1/16 floor): a 4800 Hz PCM tone plays at 75 Hz */
    for (i = 0; i < 48000u; i++) pcm[i] = (int16_t)(8000.0f * sinf(TWO_PI * 4800.0f * (float)i / 48000.0f));
    b = make_buffer(pcm, 48000u, 48000);
    s = make_source(b);
    alSourcei(s, AL_LOOPING, AL_TRUE);
    alSourcef(s, AL_PITCH, 1.0f / 64.0f);
    alSourcePlay(s);
    run_for(300);
    alGetSourcei(s, AL_SAMPLE_OFFSET, &off0);
    scan_start();
    run_for(2000);
    alGetSourcei(s, AL_SAMPLE_OFFSET, &off1);
    scan_log("PCM 4800 Hz at pitch 1/64 (75 Hz)");
    logf_("   AL_SAMPLE_OFFSET %ld -> %ld in 2 s (1500 expected)\n", (long)off0, (long)off1);
    free_source(s, b);

    alXboxOutputCallback(NULL, NULL);
    free(pcm);
    free(mono);
    free(stereo);
    check_apu("adpcm");
}

/* 10: Xbox ADPCM streamed: a 480 Hz tone in 192-block chunks (12288 samples,
 * 256 ms), four queued and refilled as they finish, like a music player */
static void phase_adpcm_stream(void) {
    enum { CHUNK_BLOCKS = 192, CHUNKS = 16, NQ = 4 };
    const uint32_t blocks = CHUNK_BLOCKS * CHUNKS;          /* about 4 s */
    int16_t *pcm = (int16_t *)malloc((blocks * 64u + 1u) * sizeof(int16_t));
    uint8_t *adpcm = (uint8_t *)malloc(blocks * 36u);
    ALuint bufs[NQ], s;
    ALint st = 0, starved_state = 0;
    uint32_t i, next = 0, requeued = 0;
    DWORD t0;

    logf_("-- 10 Xbox ADPCM streaming: 256 ms chunks, 4 queued, refilled for 3 s\n");
    if (!pcm || !adpcm) {
        logf_("   malloc failed\n");
        return;
    }
    for (i = 0; i < blocks * 64u + 1u; i++) pcm[i] = (int16_t)(8000.0f * sinf(TWO_PI * 480.0f * (float)(i % 48000u) / 48000.0f));
    xbox_adpcm_encode(adpcm, pcm, blocks, 1);
    alGenBuffers(NQ, bufs);
    for (i = 0; i < NQ; i++, next++) {
        alBufferData(bufs[i], AL_FORMAT_MONO_XBOX_ADPCM, adpcm + next * CHUNK_BLOCKS * 36u, CHUNK_BLOCKS * 36, 48000);
    }
    alGenSources(1, &s);
    alSourcei(s, AL_SOURCE_RELATIVE, AL_TRUE);
    alSourceQueueBuffers(s, NQ, bufs);
    logf_("   queued %d ADPCM chunks, AL error 0x%x\n", NQ, alGetError());
    alXboxOutputCallback(scan_cb, NULL);
    alSourcePlay(s);
    Sleep(100);
    scan_start();
    t0 = GetTickCount();
    while (GetTickCount() - t0 < 3000u) {
        ALint done = 0;
        alGetSourcei(s, AL_BUFFERS_PROCESSED, &done);
        while (done-- > 0) {
            ALuint b;
            alSourceUnqueueBuffers(s, 1, &b);
            alBufferData(b, AL_FORMAT_MONO_XBOX_ADPCM, adpcm + (next % CHUNKS) * CHUNK_BLOCKS * 36u, CHUNK_BLOCKS * 36,
                         48000);
            alSourceQueueBuffers(s, 1, &b);
            next++;
            requeued++;
        }
        alGetSourcei(s, AL_SOURCE_STATE, &st);
        if (st != AL_PLAYING) starved_state++;
        alXboxUpdateVoices();
        Sleep(16);
    }
    scan_log("ADPCM stream 480 Hz");
    logf_("   %lu chunks refilled, source not playing in %ld checks, AL error 0x%x\n", (unsigned long)requeued,
          (long)starved_state, alGetError());
    alXboxOutputCallback(NULL, NULL);
    alSourceStop(s);
    alSourcei(s, AL_BUFFER, 0);
    alDeleteSources(1, &s);
    alDeleteBuffers(NQ, bufs);
    free(pcm);
    free(adpcm);
    check_apu("adpcm stream");
}

/* 11: zero-copy output. The codec reads the GP ring directly; an output
 * callback needs the samples on the CPU, so installing one switches to the
 * copy mode (the A/B of the output thread's cost) and removing it switches
 * back. Not measurable on the CPU: listen for clicks in the 480 Hz tone. */
static void AL_APIENTRY noop_cb(ALshort *samples, ALsizei frames, ALvoid *user) {
    (void)samples;
    (void)frames;
    (void)user;
}

static uint32_t s_zc_t;   /* seconds into phase 11 */

static void zc_slice(const char *what) {
    ALXBOXSTATS a, b;
    double hz;
    memset(&a, 0, sizeof(a));
    a.size = sizeof(a);
    b = a;
    alXboxGetStats(&a);
    run_for(10000);
    alXboxGetStats(&b);
    s_zc_t += 10u;
    hz = b.cycle_hz ? (double)b.cycle_hz : 733333333.0;
    logf_("   %3lu s %s (%s): output %lu us/s, %lu passes, longest %lu us; entries +%lu; lead %lu..%lu now %lu frames; "
          "slips %lu und %lu pad %lu\n",
          (unsigned long)s_zc_t, what, b.output_zero_copy ? "zero-copy" : "copy",
          (unsigned long)((double)(b.output_cycles - a.output_cycles) * 1e6 / hz / 10.0),
          (unsigned long)(b.output_passes - a.output_passes), (unsigned long)((double)b.output_max_cycles * 1e6 / hz),
          (unsigned long)(b.ac97_buffers - a.ac97_buffers), (unsigned long)b.output_lead_min,
          (unsigned long)b.output_lead_max, (unsigned long)b.output_lead_last, (unsigned long)b.output_slips,
          (unsigned long)b.ac97_underruns, (unsigned long)b.ac97_padded);
    save_log();
}

static void phase_zero_copy(void) {
    float ph = 0.0f;
    uint32_t i;
    ALuint b, s;
    ALXBOXSTATS st;
    for (i = 0; i < 48000u; i++) {
        ph += TWO_PI * 480.0f / 48000.0f;
        s_pcm[i] = (int16_t)(8000.0f * sinf(ph));
    }
    b = make_buffer(s_pcm, 48000u, 48000);
    s = make_source(b);
    alSourcei(s, AL_LOOPING, AL_TRUE);
    memset(&st, 0, sizeof(st));
    st.size = sizeof(st);
    alXboxGetStats(&st);
    logf_("-- 11 zero-copy output: 60 s zero-copy, 60 s copy under a no-op callback, 300 s zero-copy (480 Hz, listen "
          "for clicks); now %s\n", st.output_zero_copy ? "zero-copy" : "COPY (zero-copy not active)");
    alSourcePlay(s);
    s_zc_t = 0;
    for (i = 0; i < 6u; i++) zc_slice("zero-copy");
    alXboxOutputCallback(noop_cb, NULL);
    for (i = 0; i < 6u; i++) zc_slice("callback ");
    alXboxOutputCallback(NULL, NULL);
    for (i = 0; i < 30u; i++) zc_slice("zero-copy");
    free_source(s, b);
    check_apu("zero-copy");
}

int main(void) {
    ALCdevice *dev;
    ALCcontext *ctx;

    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    logf_("openal_engine: loop points, direct gains, offsets, pitch, voice budget, buffer churn\n");
    dev = alcOpenDevice(NULL);
    if (!dev) {
        logf_("alcOpenDevice failed\n");
        save_log();
        for (;;) Sleep(1000);
    }
    ctx = alcCreateContext(dev, NULL);
    if (ctx && !apu_ac97_active()) {
        /* xemu: its front end works, so the library leaves the output to the
         * emulated APU and the AC97 path never runs. Start again the way a
         * console does (apu_clock_probe does the same) */
        logf_("AC97 inactive (front end works: xemu?), reopening with APU_VP_DBG_NO_FE_PROBE\n");
        alcDestroyContext(ctx);
        alcCloseDevice(dev);
        apu_vp_debug_set_flags(APU_VP_DBG_NO_FE_PROBE);
        dev = alcOpenDevice(NULL);
        ctx = dev ? alcCreateContext(dev, NULL) : NULL;
    }
    alcMakeContextCurrent(ctx);
    logf_("backend: %s; extensions: %s\n", apu_voice_hw_backend() ? "MCPX APU" : "null (no sound)",
          (const char *)alGetString(AL_EXTENSIONS));
    srand(1234);
    s_gp_last = 0;
    {   /* did the APU start? (state left by a previous program shows here) */
        volatile uint32_t *r = (volatile uint32_t *)NV_PAPU_BASE;
        uint32_t g0 = apu_gp_frames((uintptr_t)NV_PAPU_BASE);
        Sleep(100);
        logf_("   init: GP program %s, GP frames +%lu in 100 ms, SECTL %08lx FECTL %08lx FEDECMETH %08lx FEDECPARAM %08lx "
              "FETFORCE1 %08lx TVL2D %04lx XGSCNT %08lx\n",
              apu_gp_program_loaded() ? "loaded" : "NOT LOADED",
              (unsigned long)((apu_gp_frames((uintptr_t)NV_PAPU_BASE) - g0) & 0xFFFFFFu),
              (unsigned long)r[MCPX_APU_SECTL / 4u], (unsigned long)r[MCPX_APU_FECTL / 4u],
              (unsigned long)r[MCPX_APU_FEDECMETH / 4u], (unsigned long)r[MCPX_APU_FEDECPARAM / 4u],
              (unsigned long)r[MCPX_APU_FETFORCE1 / 4u], (unsigned long)(r[MCPX_APU_TVL2D / 4u] & 0xFFFFu),
              (unsigned long)r[MCPX_APU_XGSCNT / 4u]);
        logf_("   frame-start retries since boot: %lu\n", (unsigned long)apu_vp_start_retries());
    }
    check_apu("start");

#if defined(ENGINE_ONLY_ZERO_COPY)   /* make ENGINE_PHASES=11: phase 11 alone (a quick xemu run) */
    phase_zero_copy();
#else
    phase_loop_points();
    phase_direct();
    phase_offsets();
    phase_pitch();
    phase_budget();
    phase_churn();
    phase_stats();
    phase_static();
    phase_adpcm();
    phase_adpcm_stream();
    phase_zero_copy();
#endif
    logf_("done\n");
    save_log();
    for (;;) {
        alXboxUpdateVoices();
        Sleep(100);
    }
    return 0;
}
