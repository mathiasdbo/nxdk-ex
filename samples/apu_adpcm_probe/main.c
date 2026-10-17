/*
 * ADPCM voices and the pitch range on a console (round 1). Clean-room: our own
 * IMA ADPCM encoder (the standard algorithm) and our own test tones; the voice
 * format fields come from xemu's public VP model (container size 2 = ADPCM,
 * 36-byte blocks per channel, CBO/EBO in samples).
 *
 *   1-2  mono ADPCM, a 480 Hz tone encoded with 65 samples per block (the
 *        header sample + 64) and with 64 (the next header repeats the last
 *        sample): the layout the VP decodes plays at 480 Hz without jumps,
 *        the other about 1.5 % off
 *   3    stereo ADPCM, 480 Hz left and 960 Hz right (channel interleave)
 *   4    pitch range: a PCM tone at 1/16 down to 1/256 and up to 16 times
 *        48 kHz; the real rate from the voice position, frames still running
 *
 * Log appended to E:\apu_adpcm_probe.txt; resumable across a freeze
 * (E:\adpcm_probe_progress.txt).
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

#include "apu_gp.h"
#include "apu_hardware.h"
#include "apu_mem.h"
#include "apu_vp.h"
#include "mcpx_apu_regs.h"

#define BAR0        ((uintptr_t)NV_PAPU_BASE)
#define REG(off)    (*(volatile uint32_t *)(BAR0 + (off)))
#define PROGRESS    "E:\\adpcm_probe_progress.txt"
#define LOGFILE     "E:\\apu_adpcm_probe.txt"
#define H           5u            /* the probe's voice handle */
#define TWO_PI      6.28318530718f

static char s_log[16 * 1024];
static size_t s_log_len, s_saved;

static void logf_(const char *fmt, ...) {
    char line[320];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n > 0 && s_log_len + (size_t)n < sizeof(s_log)) {
        memcpy(s_log + s_log_len, line, (size_t)n);
        s_log_len += (size_t)n;
    }
    debugPrint("%s", line);
}

static void mount_e(void) {
    if (!nxIsDriveMounted('E')) {
        nxMountDrive('E', "\\Device\\Harddisk0\\Partition1\\");
    }
}

static void save_log(void) {
    FILE *f;
    mount_e();
    f = fopen(LOGFILE, "ab");
    if (f) {
        fwrite(s_log + s_saved, 1, s_log_len - s_saved, f);
        fclose(f);
        s_saved = s_log_len;
    }
}

static int read_progress(void) {
    FILE *f;
    int v = -1;
    mount_e();
    f = fopen(PROGRESS, "rb");
    if (f) {
        if (fscanf(f, "%d", &v) != 1) v = -1;
        fclose(f);
    }
    return v;
}

static void write_progress(int v) {
    FILE *f;
    mount_e();
    f = fopen(PROGRESS, "wb");
    if (f) {
        fprintf(f, "%d\n", v);
        fclose(f);
    }
}

static uint32_t frame_rate(uint32_t ms) {
    uint32_t f0 = apu_gp_frames(BAR0);
    DWORD t0 = GetTickCount(), dt;
    Sleep(ms);
    dt = GetTickCount() - t0;
    return dt ? (((apu_gp_frames(BAR0) - f0) & 0xFFFFFFu) * 1000u + dt / 2u) / dt : 0;
}

/* ---------------------------------------------------------------- IMA ADPCM encoder (standard algorithm) */

static const int16_t k_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
    118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767
};
static const int8_t k_index_adj[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

typedef struct {
    int pred, index;
} ima_state_t;

static uint8_t ima_nibble(ima_state_t *st, int target) {
    int step = k_step[st->index], diff = target - st->pred, delta = step >> 3;
    uint8_t n = 0;
    if (diff < 0) {
        n = 8;
        diff = -diff;
    }
    if (diff >= step) { n |= 4; diff -= step; delta += step; }
    if (diff >= step >> 1) { n |= 2; diff -= step >> 1; delta += step >> 1; }
    if (diff >= step >> 2) { n |= 1; delta += step >> 2; }
    st->pred += (n & 8) ? -delta : delta;
    if (st->pred > 32767) st->pred = 32767;
    if (st->pred < -32768) st->pred = -32768;
    st->index += k_index_adj[n & 7];
    if (st->index < 0) st->index = 0;
    if (st->index > 88) st->index = 88;
    return n;
}

/*
 * Encode `blocks` blocks of `channels` (1 or 2) from `src` (interleaved),
 * each block covering `span` input frames: frame 0 goes in the header, frames
 * 1..64 in the nibbles. span 65: blocks are disjoint; span 64: the next block's
 * header repeats this block's last sample.
 */
static void ima_encode(uint8_t *dst, const int16_t *src, uint32_t blocks, int channels, uint32_t span) {
    ima_state_t st[2] = { { 0, 0 }, { 0, 0 } };
    uint32_t b, i;
    int c;
    for (b = 0; b < blocks; b++) {
        const int16_t *in = src + (size_t)b * span * (uint32_t)channels;
        for (c = 0; c < channels; c++) {
            st[c].pred = in[c];
            dst[0] = (uint8_t)(in[c] & 0xFF);
            dst[1] = (uint8_t)((uint16_t)in[c] >> 8);
            dst[2] = (uint8_t)st[c].index;
            dst[3] = 0;
            dst += 4;
        }
        /* 8 chunks; each channel's chunk holds 8 samples, low nibble first */
        for (i = 0; i < 8u; i++) {
            for (c = 0; c < channels; c++) {
                int k;
                for (k = 0; k < 4; k++) {
                    uint32_t s1 = 1u + i * 8u + (uint32_t)k * 2u;
                    uint8_t lo = ima_nibble(&st[c], in[s1 * (uint32_t)channels + (uint32_t)c]);
                    uint8_t hi = ima_nibble(&st[c], in[(s1 + 1u) * (uint32_t)channels + (uint32_t)c]);
                    *dst++ = (uint8_t)(lo | (hi << 4));
                }
            }
        }
    }
}

/* ---------------------------------------------------------------- voice and capture */

static uint32_t map_ba(uint32_t phys, uint32_t bytes) {
    /* Map the data through a plain voice start, read its BA, stop it */
    apu_vp_voice_params_t p;
    uint32_t i, ba;
    memset(&p, 0, sizeof(p));
    p.phys = phys;
    p.bytes = bytes & ~3u;
    p.channels = 1;
    p.bits = 16;
    p.loop = true;
    p.hrtf_entry = -1;
    for (i = 0; i < APU_VP_OUTPUTS; i++) p.vols[i] = 0xFFFu;
    apu_vp_voice_start(BAR0, 100, &p);
    ba = ((volatile uint32_t *)apu_vp_debug_voice_record(100))[MCPX_VOICE_CUR_PSL_START / 4] & 0xFFFFFFu;
    apu_vp_voice_off(BAR0, 100);
    Sleep(5);
    return ba;
}

/* A looping voice on handle H: fmt (CFG_FMT without bins), EBO = samples - 1, pitch field */
static void start_voice(uint32_t ba, uint32_t fmt, uint32_t samples, int16_t pitch, bool stereo) {
    volatile uint32_t *r = (volatile uint32_t *)apu_vp_debug_voice_record(H);
    uint8_t bins[APU_VP_OUTPUTS];
    uint16_t vols[APU_VP_OUTPUTS];
    uint32_t vbin, fb, vola, volb, volc, w;
    for (w = 0; w < APU_VP_OUTPUTS; w++) {
        bins[w] = (uint8_t)(w & 1u);
        vols[w] = 0xFFFu;
    }
    vols[0] = vols[1] = 0;      /* mono: both channels; stereo: left -> bin 0, right -> bin 1 */
    (void)stereo;
    apu_vp_pack_bins(bins, &vbin, &fb);
    apu_vp_pack_volumes(vols, &vola, &volb, &volc);
    for (w = 0; w < 32u; w++) r[w] = 0;
    r[MCPX_VOICE_CFG_VBIN / 4] = vbin;
    r[MCPX_VOICE_CFG_FMT / 4] = fmt | fb;
    r[MCPX_VOICE_CFG_HRTF_TARGET / 4] = 0xFFFFu;
    r[MCPX_VOICE_CUR_PSL_START / 4] = ba;
    r[MCPX_VOICE_PAR_NEXT / 4] = samples - 1u;
    r[MCPX_VOICE_TAR_VOLA / 4] = vola;
    r[MCPX_VOICE_TAR_VOLB / 4] = volb;
    r[MCPX_VOICE_TAR_VOLC / 4] = volc;
    r[MCPX_VOICE_TAR_PITCH_LINK / 4] = ((uint32_t)(uint16_t)pitch << 16) | MCPX_APU_LIST_END;
    r[MCPX_VOICE_PAR_STATE / 4] = MCPX_PAR_STATE_ACTIVE_VOICE;
    REG(MCPX_APU_TVL2D) = H;
}

static void stop_voice(void) {
    REG(MCPX_APU_TVL2D) = MCPX_APU_LIST_END;
    Sleep(5);
}

static uint32_t cbo(void) {
    return ((volatile uint32_t *)apu_vp_debug_voice_record(H))[MCPX_VOICE_PAR_OFFSET / 4] & 0xFFFFFFu;
}

#define CAP 48000u
static int16_t s_cap[2 * CAP];

static uint32_t capture(void) {
    uint32_t got = 0;
    DWORD t0 = GetTickCount();
    apu_gp_fifo_discard(BAR0);
    while (got < CAP && GetTickCount() - t0 < 2000u) {
        got += apu_gp_fifo_read(BAR0, s_cap + 2u * got, CAP - got);
        Sleep(2);
    }
    return got;
}

/* Channel c: peak, frequency from rising zero crossings, jumps of the second difference */
static void analyse(const char *what, uint32_t n, int c) {
    uint32_t i, peak = 0, cross = 0, jumps = 0, limit;
    for (i = 0; i < n; i++) {
        int x = s_cap[2 * i + c], a = x < 0 ? -x : x;
        if ((uint32_t)a > peak) peak = (uint32_t)a;
    }
    /* a sine of amplitude A and frequency f: |second difference| <= A (2 pi f / 48000)^2; allow 4x */
    limit = peak / 8u + 40u;
    for (i = 2; i < n; i++) {
        int x = s_cap[2 * i + c], x1 = s_cap[2 * (i - 1) + c], x2 = s_cap[2 * (i - 2) + c], d = x - 2 * x1 + x2;
        if (x1 < 0 && x >= 0) cross++;
        if ((uint32_t)(d < 0 ? -d : d) > limit) jumps++;
    }
    /* frequency in tenths of Hz */
    logf_("   %s: peak %lu, %lu.%lu Hz, %lu jumps in %lu samples\n", what, (unsigned long)peak,
          (unsigned long)((uint64_t)cross * 480000u / (n ? n : 1u) / 10u),
          (unsigned long)((uint64_t)cross * 480000u / (n ? n : 1u) % 10u), (unsigned long)jumps, (unsigned long)n);
}

/* ---------------------------------------------------------------- tests */

#define FMT_ADPCM   (2u << 30)
#define FMT_S16     (1u << 28)
#define FMT_STEREO  (1u << 27)
#define FMT_LOOP    (1u << 25)
#define FMT_PCM16   ((1u << 30) | (1u << 28))
#define BLOCKS      750u          /* 64 * 750 = 48000 samples: one second at 48 kHz */

static int16_t *s_src;            /* 2 channels x 65 * BLOCKS frames of tone */
static uint8_t *s_adpcm;
static uint32_t s_adpcm_phys;

static void make_tone(int channels, float hz_l, float hz_r, uint32_t frames) {
    uint32_t i;
    for (i = 0; i < frames; i++) {
        s_src[(size_t)i * (uint32_t)channels] = (int16_t)(12000.0f * sinf(TWO_PI * hz_l * (float)(i % 48000u) / 48000.0f));
        if (channels == 2) {
            s_src[(size_t)i * 2u + 1u] = (int16_t)(12000.0f * sinf(TWO_PI * hz_r * (float)(i % 48000u) / 48000.0f));
        }
    }
}

static void adpcm_test(const char *what, uint32_t span, int channels) {
    uint32_t bytes = BLOCKS * 36u * (uint32_t)channels, ba, n;
    make_tone(channels, 480.0f, 960.0f, BLOCKS * span + 1u);
    ima_encode(s_adpcm, s_src, BLOCKS, channels, span);
    ba = map_ba(s_adpcm_phys, bytes);
    start_voice(ba, FMT_ADPCM | FMT_S16 | FMT_LOOP | (channels == 2 ? (FMT_STEREO | (1u << 16)) : 0u),
                BLOCKS * 64u, 0, channels == 2);
    Sleep(100);
    n = capture();
    {
        uint32_t c0 = cbo();
        Sleep(500);
        logf_("  %s (%lu bytes, EBO %lu): CBO %lu -> %lu in 500 ms\n", what, (unsigned long)bytes,
              (unsigned long)(BLOCKS * 64u - 1u), (unsigned long)c0, (unsigned long)cbo());
    }
    analyse("left ", n, 0);
    if (channels == 2) analyse("right", n, 1);
    stop_voice();
}

static void step_adpcm65(void) { adpcm_test("mono, 65 samples per block", 65u, 1); }
static void step_adpcm64(void) { adpcm_test("mono, 64 samples per block", 64u, 1); }
static void step_adpcm_stereo(void) {
    /* the span that step 1/2 found is unknown here: try 64 (xemu) and log both channels */
    adpcm_test("stereo, 64 per block, 480 L / 960 R", 64u, 2);
}

static void step_pitch(void) {
    static const struct { int16_t field; const char *name; } k[] = {
        { 0, "1x" }, { -16384, "1/16" }, { -20480, "1/32" }, { -24576, "1/64" }, { -28672, "1/128" },
        { -32768, "1/256" }, { 8192, "4x" }, { 12288, "8x" }, { 16384, "16x" },
    };
    uint32_t i, ba;
    make_tone(1, 480.0f, 0.0f, 48000u);
    ba = map_ba(s_adpcm_phys, 96000u);
    memcpy(s_adpcm, s_src, 96000u);
    for (i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        uint32_t total = 0, prev, t, rate;
        DWORD t0;
        start_voice(ba, FMT_PCM16 | FMT_LOOP, 48000u, k[i].field, false);
        Sleep(50);
        prev = cbo();
        t0 = GetTickCount();
        while (GetTickCount() - t0 < 1000u) {
            Sleep(5);
            t = cbo();
            total += (t >= prev) ? t - prev : t + 48000u - prev;   /* the loop wraps at 48000 */
            prev = t;
        }
        rate = frame_rate(100);
        stop_voice();
        logf_("  pitch field %6d (%5s): %lu samples/s (expected %lu), frames %lu/s\n", (int)k[i].field, k[i].name,
              (unsigned long)total, (unsigned long)(48000.0f * powf(2.0f, (float)k[i].field / 4096.0f) + 0.5f),
              (unsigned long)rate);
        save_log();
        if (rate < 1400u) {
            logf_("  frames slowed: stopping the pitch sweep\n");
            break;
        }
    }
}

typedef struct {
    const char *name;
    void (*run)(void);
} step_t;

static const step_t s_steps[] = {
    { "1 ADPCM mono, 65-sample blocks", step_adpcm65 },
    { "2 ADPCM mono, 64-sample blocks", step_adpcm64 },
    { "3 ADPCM stereo", step_adpcm_stereo },
    { "4 pitch range", step_pitch },
};
#define N_STEPS ((int)(sizeof(s_steps) / sizeof(s_steps[0])))

int main(void) {
    int resume, k;
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    resume = read_progress();
    if (resume >= N_STEPS) {
        logf_("\n=== apu_adpcm_probe round 1: all steps ran (delete %s to run them again)\n", PROGRESS);
        save_log();
        for (;;) Sleep(1000);
    }
    if (resume < 0) {
        logf_("\n=== apu_adpcm_probe round 1: %d steps (fresh start)\n", N_STEPS);
        resume = 0;
    } else {
        logf_("\n=== apu_adpcm_probe round 1: resumed; step %s did not finish (FROZE?)\n", s_steps[resume].name);
        resume++;
    }
    save_log();
    apu_vp_debug_set_flags(APU_VP_DBG_NO_AC97 | APU_VP_DBG_AC97_COPY);   /* the 64 KiB ring, copy output */
    if (apu_vp_init(BAR0) != 0) {
        logf_("apu_vp_init failed\n");
        save_log();
        for (;;) Sleep(1000);
    }
    s_src = (int16_t *)malloc((BLOCKS * 65u + 64u) * 2u * sizeof(int16_t));
    s_adpcm = (uint8_t *)apu_mem_alloc_table(96000u + 4096u, &s_adpcm_phys);   /* ADPCM up to 54000 bytes, PCM 96000 */
    if (!s_src || !s_adpcm) {
        logf_("allocation failed\n");
        save_log();
        for (;;) Sleep(1000);
    }
    logf_("frames %lu/s\n", (unsigned long)frame_rate(200));
    for (k = resume; k < N_STEPS; k++) {
        uint32_t rate;
        write_progress(k);
        logf_("%s\n", s_steps[k].name);
        save_log();
        s_steps[k].run();
        rate = frame_rate(100);
        if (rate < 1400u) {
            logf_("  frames at %lu/s after step %s: power-cycle and run again to continue\n", (unsigned long)rate,
                  s_steps[k].name);
            save_log();
            for (;;) Sleep(1000);
        }
        save_log();
    }
    write_progress(N_STEPS);
    logf_("done\n");
    save_log();
    for (;;) Sleep(1000);
    return 0;
}
