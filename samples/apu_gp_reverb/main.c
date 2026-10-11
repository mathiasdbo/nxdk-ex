/*
 * Reverb on the GP, on a console (reverb plan phase 3, lib/openal/docs/GP_REVERB_PLAN.md).
 * Clean-room: our own GP program (lib/openal/dsp/gp_reverb.asm) and our own sounds.
 *
 *   1-2  decay: a click into the send only (no dry), reverb at T60 1 s and 3 s
 *        without damping; energy 0.1-0.2 s after the onset against 0.6-0.7 s
 *        (the host reference gives -29.9 dB and -9.9 dB)
 *   3    cost: end-of-list waits, timeouts and frame rate with the reverb on
 *   4    listening: AC97 on, a noise burst every 2 s (dry + send), the presets
 *        cycle every 8 s (off, room, hall, cave); runs until switched off
 *
 * Log appended to E:\apu_gp_reverb.txt.
 */
#include <hal/debug.h>
#include <hal/video.h>
#include <nxdk/mount.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "apu_ac97.h"
#include "apu_gp.h"
#include "apu_gp_reverb.h"
#include "apu_hardware.h"
#include "apu_mem.h"
#include "apu_reverb.h"
#include "apu_vp.h"
#include "mcpx_apu_regs.h"

#define BAR0        ((uintptr_t)NV_PAPU_BASE)
#define XW(n)       (*(volatile uint32_t *)(BAR0 + MCPX_APU_GP_XMEM + 4u * (n)))
#define LOGFILE     "E:\\apu_gp_reverb.txt"

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

static void save_log(void) {
    FILE *f;
    if (!nxIsDriveMounted('E')) {
        nxMountDrive('E', "\\Device\\Harddisk0\\Partition1\\");
    }
    f = fopen(LOGFILE, "ab");
    if (f) {
        fwrite(s_log + s_saved, 1, s_log_len - s_saved, f);
        fclose(f);
        s_saved = s_log_len;
    }
}

static uint32_t frame_rate(uint32_t ms) {
    uint32_t f0 = apu_gp_frames(BAR0);
    DWORD t0 = GetTickCount(), dt;
    Sleep(ms);
    dt = GetTickCount() - t0;
    return dt ? (((apu_gp_frames(BAR0) - f0) & 0xFFFFFFu) * 1000u + dt / 2u) / dt : 0;
}

/* ---------------------------------------------------------------- reverb control */

static uint8_t *s_scr;

static void reverb_off(void) {
    XW(GP_REVERB_FLAGS) = 0;
    Sleep(3);
}

static void reverb_on(const apu_reverb_params_t *p) {
    static apu_reverb_word_t w[APU_REVERB_MAX_WORDS];
    uint32_t n, i;
    reverb_off();
    n = apu_reverb_layout(p, w);
    for (i = 0; i < n; i++) XW(w[i].addr) = w[i].value;
    memset(s_scr + APU_REVERB_SCRATCH_BASE, 0, apu_reverb_scratch_bytes());
    XW(GP_REVERB_SPMAX) = 0;
    XW(GP_REVERB_TMOUT) = 0;
    XW(GP_REVERB_FLAGS) = 1;
}

/* ---------------------------------------------------------------- sounds */

#define CLICK_FRAMES 96000u     /* one click every 2 s */
#define BURST_FRAMES 96000u     /* one noise burst every 2 s */
static uint32_t s_click_phys, s_burst_phys;

static void play(uint32_t h, uint32_t phys, uint32_t frames, bool dry, bool send) {
    apu_vp_voice_params_t p;
    uint32_t i;
    memset(&p, 0, sizeof(p));
    p.phys = phys;
    p.bytes = frames * 2u;
    p.channels = 1;
    p.bits = 16;
    p.loop = true;
    p.tail_bytes = APU_VP_SILENT_TAIL_BYTES;
    p.hrtf_entry = -1;
    for (i = 0; i < APU_VP_OUTPUTS; i++) {
        p.bins[i] = (uint8_t)(i & 1u);
        p.vols[i] = 0xFFFu;
    }
    if (dry) p.vols[0] = p.vols[1] = 0;
    p.bins[4] = 2;
    p.vols[4] = send ? 0 : 0xFFFu;
    apu_vp_voice_start(BAR0, h, &p);
}

/* ---------------------------------------------------------------- decay */

#define CAP_FRAMES 120000u   /* 2.5 s */
static int16_t s_cap[2 * CAP_FRAMES];

/* Capture from now on; the caller starts the sound right after the discard */
static uint32_t capture(uint32_t frames, void (*start)(void)) {
    uint32_t got = 0;
    DWORD t0 = GetTickCount();
    apu_gp_fifo_discard(BAR0);
    if (start) start();
    while (got < frames && GetTickCount() - t0 < 4000u) {
        got += apu_gp_fifo_read(BAR0, s_cap + 2u * got, frames - got);
        Sleep(2);
    }
    return got;
}

static double energy(uint32_t from, uint32_t to) {
    double e = 0.0;
    uint32_t i;
    for (i = from; i < to; i++) {
        double l = s_cap[2 * i], r = s_cap[2 * i + 1];
        e += l * l + r * r;
    }
    return e;
}

static void click_send(void) {
    play(5, s_click_phys, CLICK_FRAMES, false, true);
}

static void decay_test(float t60) {
    apu_reverb_params_t p;
    uint32_t n, i, onset = 0xFFFFFFFFu, peak = 0;
    apu_reverb_default_params(&p);
    p.decay_time = t60;
    p.hf_ratio = 1.0f;
    p.input_gain = 1.0f;
    p.gain = 1.0f;
    p.input_lowpass = 0.0f;
    reverb_on(&p);
    Sleep(20);
    n = capture(CAP_FRAMES, click_send);
    apu_vp_voice_off(BAR0, 5);
    reverb_off();
    /* the first sample above 1/8 of the peak: the lines start empty, so it is the first click's onset */
    for (i = 0; i < n; i++) {
        uint32_t a = (uint32_t)abs(s_cap[2 * i]);
        if (a > peak) peak = a;
    }
    for (i = 0; i + 36000u < n; i++) {
        if ((uint32_t)abs(s_cap[2 * i]) * 8u > peak) {
            onset = i;
            break;
        }
    }
    if (onset == 0xFFFFFFFFu) {
        logf_("  T60 %d ms: %lu frames, peak %lu, no onset found\n", (int)(t60 * 1000.0f), (unsigned long)n,
              (unsigned long)peak);
        return;
    }
    {
        double e1 = energy(onset + 4800u, onset + 9600u), e2 = energy(onset + 28800u, onset + 33600u);
        int db10 = (e1 > 0.0 && e2 > 0.0) ? (int)(100.0 * log10(e2 / e1)) : -9999;
        logf_("  T60 %d ms: %lu frames, peak %lu, onset at %lu; 0.5 s later %d.%d dB (expected %s)\n",
              (int)(t60 * 1000.0f), (unsigned long)n, (unsigned long)peak, (unsigned long)onset, db10 / 10,
              abs(db10 % 10), t60 < 2.0f ? "-30" : "-10");
    }
}

/* ---------------------------------------------------------------- main */

typedef struct {
    const char *name;
    float t60, hf, gain, pre, diffusion;
} preset_t;

static const preset_t k_presets[] = {
    { "OFF (dry only)", 0, 0, 0, 0, 0 },
    { "ROOM  (0.6 s)", 0.6f, 0.7f, 0.35f, 0.005f, 0.7f },
    { "HALL  (2.5 s)", 2.5f, 0.6f, 0.5f, 0.025f, 0.6f },
    { "CAVE  (5 s)", 5.0f, 0.4f, 0.6f, 0.05f, 0.5f },
};

int main(void) {
    apu_ac97_stats_t st;
    uint32_t round = 0;

    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    logf_("\n=== apu_gp_reverb round 2 (8 combs and 4 allpasses per ear)\n");
    save_log();
    apu_gp_debug_set_program(gp_reverb_words, GP_REVERB_WORDS);
    apu_gp_debug_set_scratch_bytes(APU_REVERB_SCRATCH_BASE + apu_reverb_scratch_bytes());
    apu_vp_debug_set_flags(APU_VP_DBG_NO_AC97);
    if (apu_vp_init(BAR0) != 0) {
        logf_("apu_vp_init failed\n");
        save_log();
        for (;;) Sleep(1000);
    }
    s_scr = (uint8_t *)apu_gp_debug_scratch();
    {
        int16_t *c = (int16_t *)apu_mem_alloc_table(CLICK_FRAMES * 2u + APU_VP_SILENT_TAIL_BYTES, &s_click_phys);
        int16_t *b = (int16_t *)apu_mem_alloc_table(BURST_FRAMES * 2u + APU_VP_SILENT_TAIL_BYTES, &s_burst_phys);
        uint32_t i, seed = 12345u;
        memset(c, 0, CLICK_FRAMES * 2u + APU_VP_SILENT_TAIL_BYTES);
        memset(b, 0, BURST_FRAMES * 2u + APU_VP_SILENT_TAIL_BYTES);
        /* 100 ms in: a new voice ramps its volume up from 0 and swallowed a click at the start */
        c[4800] = 20000;
        c[4801] = 26000;
        c[4802] = 20000;
        for (i = 0; i < 7200u; i++) {   /* 150 ms of noise, fading out */
            int v;
            seed = seed * 1664525u + 1013904223u;
            v = (int)((seed >> 16) & 0xFFFFu) - 32768;
            b[i] = (int16_t)((v * 0.4f) * (1.0f - (float)i / 7200.0f));
        }
    }
    logf_("program loaded %s (%u words), scratch %lu bytes, %lu frames/s\n", apu_gp_program_loaded() ? "yes" : "NO",
          (unsigned)GP_REVERB_WORDS, (unsigned long)apu_gp_scratch_bytes(), (unsigned long)frame_rate(200));
    save_log();

    logf_("1-2 decay (click into the send only, no damping)\n");
    save_log();
    decay_test(1.0f);
    save_log();
    decay_test(3.0f);
    save_log();

    logf_("3 cost\n");
    {
        apu_reverb_params_t p;
        apu_reverb_default_params(&p);
        reverb_on(&p);
        Sleep(300);
        logf_("  reverb on: %lu frames/s, waits last %lu polls, max %lu, timeouts %lu\n",
              (unsigned long)frame_rate(500), (unsigned long)(XW(GP_REVERB_SPINS) & 0xFFFFFFu),
              (unsigned long)(XW(GP_REVERB_SPMAX) & 0xFFFFFFu), (unsigned long)(XW(GP_REVERB_TMOUT) & 0xFFFFFFu));
    }
    save_log();

    logf_("4 listening: a noise burst every 2 s; presets change every 8 s\n");
    if (apu_ac97_start(BAR0) != 0) {
        logf_("  AC97 start failed\n");
        save_log();
        for (;;) Sleep(1000);
    }
    save_log();
    play(5, s_burst_phys, BURST_FRAMES, true, true);
    for (;;) {
        uint32_t k;
        for (k = 0; k < sizeof(k_presets) / sizeof(k_presets[0]); k++) {
            const preset_t *ps = &k_presets[k];
            if (ps->t60 <= 0.0f) {
                reverb_off();
            } else {
                apu_reverb_params_t p;
                apu_reverb_default_params(&p);
                p.decay_time = ps->t60;
                p.hf_ratio = ps->hf;
                p.gain = ps->gain;
                p.pre_delay = ps->pre;
                p.diffusion = ps->diffusion;
                reverb_on(&p);
            }
            debugPrint("reverb %s\n", ps->name);
            Sleep(8000);
        }
        if (++round == 2u) {
            apu_ac97_get_stats(&st);
            logf_("  after 64 s: AC97 underruns %lu, padded %lu; GP waits max %lu polls, timeouts %lu\n",
                  (unsigned long)st.underruns, (unsigned long)st.padded,
                  (unsigned long)(XW(GP_REVERB_SPMAX) & 0xFFFFFFu), (unsigned long)(XW(GP_REVERB_TMOUT) & 0xFFFFFFu));
            save_log();
        }
    }
    return 0;
}
