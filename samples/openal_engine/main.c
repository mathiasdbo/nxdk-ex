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

#include "apu_ac97.h"
#include "apu_gp.h"
#include "apu_hardware.h"
#include "apu_voice.h"
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
    }
    check_apu("start");

    phase_loop_points();
    phase_direct();
    phase_offsets();
    phase_pitch();
    phase_budget();
    phase_churn();
    logf_("done\n");
    save_log();
    for (;;) {
        alXboxUpdateVoices();
        Sleep(100);
    }
    return 0;
}
