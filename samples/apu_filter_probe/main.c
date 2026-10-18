/*
 * VP voice filter calibration (lib/openal/docs/EFX_PLAN.md 3.7, the probe before
 * AL_FILTER_LOWPASS). Clean-room: our own multi-tone signal and register writes;
 * the library's copy GP program. Bins 0/1 are read from the GP output ring.
 *
 * The signal is 25 tones on a 50 Hz grid (50 Hz .. 20 kHz), each a whole number
 * of cycles in 960 samples, so a Goertzel per tone over 960-sample blocks reads
 * each tone's level exactly; every response is in 0.1 dB against the same voice
 * with FMODE 0. TAR_FCA / TAR_FCB = Q (31:16) | cutoff (15:0, s4.12).
 *
 *   1  Q sweep: FMODE 1, cutoff -0x2000, Q 0x2000 .. 0xFFFF (which Q is flat?)
 *   2  cutoff sweep: FMODE 1 at Q 0xB505, cutoff -0x8000 .. +0x1800 (the scale,
 *      and whether positive cutoffs go above 8 kHz)
 *   3  FCA or FCB: modes 1, 2, 3 with only one of them set
 *   4  FMODE 2 with FCB: cutoff sweep at Q 0xB505 and 0x4000
 *   5  other voices: an HRTF voice and a stereo voice with FMODE 1
 *   6  a cutoff change on a playing voice: the largest sample step across it
 * Round 2 (the default; PROBE_ROUND1 for the steps above):
 *   7  the top of the range: cutoffs -0x800 .. -1 (where does it switch off?)
 *   8  HRTF voices: cutoffs -0x800 .. -0x5000 (their offset against plain voices)
 *
 * Log appended to E:\apu_filter_probe.txt.
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
#define XW(n)       (*(volatile uint32_t *)(BAR0 + MCPX_APU_GP_XMEM + 4u * (n)))
#define LOGFILE     "E:\\apu_filter_probe.txt"

#define RATE        48000u
#define BLOCK       960u            /* 50 Hz grid */
#define CAP_FRAMES  (BLOCK * 40u)   /* 0.8 s */
#define NT          25
#define TONE_AMP    320.0           /* each tone; the sum peaks near 5000 */
#define QBW         0xB505u         /* sqrt(2) if 0x8000 = 1 (the damping guess) */

static const uint32_t s_hz[NT] = { 50, 100, 150, 200, 300, 400, 500, 650, 800, 1000, 1250, 1600, 2000,
                                   2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500, 14000, 16000, 18000, 20000 };

/* ---------------------------------------------------------------- log */

static char s_log[96 * 1024];
static size_t s_log_len, s_saved;

static void logf_(const char *fmt, ...) {
    char line[400];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if ((size_t)n >= sizeof(line)) n = (int)sizeof(line) - 1;
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
    f = fopen(LOGFILE, "ab");
    if (f) {
        fwrite(s_log + s_saved, 1, s_log_len - s_saved, f);
        fclose(f);
        s_saved = s_log_len;
    }
}

static void wc_flush(void) {
    __asm__ __volatile__("lock; addl $0,(%%esp)" ::: "memory");
}

static uint32_t frame_rate(uint32_t ms) {
    uint32_t f0 = XW(0x10u) & 0xFFFFFFu;
    DWORD t0 = GetTickCount(), dt;
    Sleep(ms);
    dt = GetTickCount() - t0;
    return dt ? ((((XW(0x10u) & 0xFFFFFFu) - f0) & 0xFFFFFFu) * 1000u + dt / 2u) / dt : 0;
}

/* nxdk's pdclib lrint() is an assert(0) stub */
static int round_i(double v) {
    return (int)(v < 0.0 ? v - 0.5 : v + 0.5);
}

/* ---------------------------------------------------------------- signal and voice */

static uint32_t s_mono_phys, s_stereo_phys;
static int16_t s_cap[2 * CAP_FRAMES];
static uint32_t s_h = 0xFFFFu;
static double s_ref[2][NT];

static void make_signals(void) {
    uint32_t i, k, tail = APU_VP_SILENT_TAIL_BYTES;
    int16_t *m = (int16_t *)apu_mem_alloc_table(RATE * 2u + tail, &s_mono_phys);
    int16_t *s = (int16_t *)apu_mem_alloc_table(RATE * 4u + tail, &s_stereo_phys);
    double ph[NT];
    uint32_t seed = 777u;
    memset(m, 0, RATE * 2u + tail);
    memset(s, 0, RATE * 4u + tail);
    for (k = 0; k < NT; k++) {   /* fixed random phases keep the sum's peak low */
        seed = seed * 1664525u + 1013904223u;
        ph[k] = (double)(seed >> 8) / 16777216.0 * 6.283185307;
    }
    for (i = 0; i < RATE; i++) {
        double v = 0.0;
        for (k = 0; k < NT; k++) v += TONE_AMP * sin(6.283185307 * s_hz[k] * (double)i / RATE + ph[k]);
        m[i] = (int16_t)round_i(v);
        s[2u * i] = s[2u * i + 1u] = m[i];
    }
}

static void voice_stop(void) {
    if (s_h != 0xFFFFu) {
        apu_vp_voice_off(BAR0, s_h);
        Sleep(5);
    }
}

typedef enum { V_PLAIN, V_HRTF, V_STEREO } vkind_t;

static int voice_play(vkind_t kind) {
    apu_vp_voice_params_t p;
    uint32_t i;
    int rc;
    voice_stop();
    if (s_h == 0xFFFFu) s_h = apu_vp_alloc_handle(BAR0);
    memset(&p, 0, sizeof(p));
    p.phys = kind == V_STEREO ? s_stereo_phys : s_mono_phys;
    p.channels = kind == V_STEREO ? 2 : 1;
    p.bytes = RATE * 2u * (uint32_t)p.channels;
    p.bits = 16;
    p.loop = true;
    p.tail_bytes = APU_VP_SILENT_TAIL_BYTES;
    p.hrtf_entry = kind == V_HRTF ? 0 : -1;    /* entry 0: straight ahead */
    for (i = 0; i < 8u; i++) {
        p.bins[i] = (uint8_t)(i & 1u);
        p.vols[i] = 0xFFFu;
    }
    p.vols[0] = 0;
    if (kind == V_HRTF) p.vols[2] = 0;      /* left ear slot 0 via output 0, right ear slot 2 via output 2 */
    else p.vols[1] = 0;
    rc = apu_vp_voice_start(BAR0, s_h, &p);
    if (rc != 0) logf_("  voice start failed (%d)\n", rc);
    Sleep(30);
    return rc;
}

static volatile uint32_t *record(uint32_t off) {
    return (volatile uint32_t *)(apu_vp_debug_voice_record(s_h) + off);
}

static void set_filter(uint32_t mode, uint32_t fca, uint32_t fcb) {
    *record(MCPX_VOICE_CFG_MISC) = (*record(MCPX_VOICE_CFG_MISC) & ~0x30000u) | (mode << 16);
    *record(MCPX_VOICE_TAR_FCA) = fca;
    *record(MCPX_VOICE_TAR_FCB) = fcb;
    wc_flush();
    Sleep(40);
}

static uint32_t fc(uint32_t q, int32_t cut) {
    return (q << 16) | ((uint32_t)cut & 0xFFFFu);
}

static uint32_t capture(uint32_t frames) {
    uint32_t got = 0;
    DWORD t0;
    apu_gp_fifo_discard(BAR0);
    t0 = GetTickCount();
    while (got < frames && GetTickCount() - t0 < 3000u) {
        got += apu_gp_fifo_read(BAR0, s_cap + 2u * got, frames - got);
        Sleep(2);
    }
    return got;
}

/* Per-tone power of channel ch, averaged over 960-sample blocks */
static void tones(uint32_t frames, uint32_t ch, double out[NT]) {
    uint32_t k, blk, i, blocks = frames / BLOCK;
    for (k = 0; k < NT; k++) {
        double c = 2.0 * cos(6.283185307179586 * s_hz[k] / RATE), acc = 0.0;
        for (blk = 0; blk < blocks; blk++) {
            double s1 = 0.0, s2 = 0.0;
            for (i = 0; i < BLOCK; i++) {
                double s = s_cap[2u * (blk * BLOCK + i) + ch] + c * s1 - s2;
                s2 = s1;
                s1 = s;
            }
            acc += s1 * s1 + s2 * s2 - c * s1 * s2;
        }
        out[k] = blocks ? acc / blocks : 0.0;
    }
}

static void reference(void) {
    uint32_t n;
    set_filter(0, 0, 0);
    n = capture(CAP_FRAMES);
    tones(n, 0, s_ref[0]);
    tones(n, 1, s_ref[1]);
}

/* One line: the response of channel ch in 0.1 dB at every tone */
static void measure(const char *what, uint32_t ch) {
    double p[NT];
    uint32_t k, n = capture(CAP_FRAMES);
    char line[400];
    int len = snprintf(line, sizeof(line), "  %-24s", what);
    tones(n, ch, p);
    for (k = 0; k < NT && len < (int)sizeof(line) - 8; k++) {
        int d = (p[k] > 0.0 && s_ref[ch][k] > 0.0) ? round_i(100.0 * log10(p[k] / s_ref[ch][k])) : -999;
        len += snprintf(line + len, sizeof(line) - (size_t)len, " %4d", d);
    }
    logf_("%s\n", line);
}

static void header(void) {
    char line[400];
    uint32_t k;
    int len = snprintf(line, sizeof(line), "  %-24s", "Hz:");
    for (k = 0; k < NT; k++) {
        uint32_t f = s_hz[k];
        len += f >= 1000u ? snprintf(line + len, sizeof(line) - (size_t)len, " %3luk", (unsigned long)(f / 1000u))
                          : snprintf(line + len, sizeof(line) - (size_t)len, " %4lu", (unsigned long)f);
    }
    logf_("%s\n", line);
}

static void cut_name(char *b, size_t n, const char *pre, uint32_t q, int32_t cut) {
    snprintf(b, n, "%s Q %04lx c %s%04lx", pre, (unsigned long)q, cut < 0 ? "-" : "+", (unsigned long)(cut < 0 ? -cut : cut));
}

/* ---------------------------------------------------------------- steps */

static void step_q(void) {
    static const uint32_t qs[] = { 0x2000u, 0x4000u, 0x6000u, 0x8000u, 0xA000u, QBW, 0xC000u, 0xE000u, 0xFFFFu };
    uint32_t i;
    char what[48];
    logf_("1 Q sweep, FMODE 1, cutoff -0x2000 on FCA and FCB\n");
    header();
    for (i = 0; i < sizeof(qs) / sizeof(qs[0]); i++) {
        set_filter(1, fc(qs[i], -0x2000), fc(qs[i], -0x2000));
        cut_name(what, sizeof(what), "F1", qs[i], -0x2000);
        measure(what, 0);
    }
}

static void step_cut(void) {
    int32_t c;
    char what[48];
    logf_("2 cutoff sweep, FMODE 1, Q %04x on FCA and FCB\n", QBW);
    header();
    for (c = -0x8000; c <= 0x1800; c += 0x800) {
        set_filter(1, fc(QBW, c), fc(QBW, c));
        cut_name(what, sizeof(what), "F1", QBW, c);
        measure(what, 0);
    }
    for (c = 0x2000; c <= 0x7000; c += 0x1000) {   /* the top of the range */
        set_filter(1, fc(QBW, c), fc(QBW, c));
        cut_name(what, sizeof(what), "F1", QBW, c);
        measure(what, 0);
    }
}

static void step_ab(void) {
    uint32_t m, q;
    static const uint32_t qs[2] = { QBW, 0x4000u };
    char what[48];
    logf_("3 FCA or FCB alone (the other 0), cutoff -0x2000\n");
    header();
    for (m = 1; m <= 3u; m++) {
        for (q = 0; q < 2u; q++) {
            set_filter(m, fc(qs[q], -0x2000), 0);
            snprintf(what, sizeof(what), "F%lu FCA only Q %04lx", (unsigned long)m, (unsigned long)qs[q]);
            measure(what, 0);
            set_filter(m, 0, fc(qs[q], -0x2000));
            snprintf(what, sizeof(what), "F%lu FCB only Q %04lx", (unsigned long)m, (unsigned long)qs[q]);
            measure(what, 0);
        }
    }
    /* mode 3 with different cutoffs on the two */
    set_filter(3, fc(QBW, -0x1000), fc(QBW, -0x3000));
    measure("F3 A -1000 B -3000", 0);
    set_filter(3, fc(QBW, -0x3000), fc(QBW, -0x1000));
    measure("F3 A -3000 B -1000", 0);
}

static void step_mode2(void) {
    static const int32_t cuts[] = { -0x4000, -0x2000, 0, 0x1000 };
    static const uint32_t qs[] = { QBW, 0x4000u, 0xFFFFu };
    uint32_t q, c;
    char what[48];
    logf_("4 FMODE 2, cutoff and Q on FCA and FCB\n");
    header();
    for (q = 0; q < 3u; q++) {
        for (c = 0; c < 4u; c++) {
            set_filter(2, fc(qs[q], cuts[c]), fc(qs[q], cuts[c]));
            cut_name(what, sizeof(what), "F2", qs[q], cuts[c]);
            measure(what, 0);
        }
    }
}

static void step_voices(void) {
    char what[48];
    logf_("5 other voices, FMODE 1 Q %04x cutoff -0x2000 (each against itself with FMODE 0)\n", QBW);
    header();
    if (voice_play(V_HRTF) == 0) {
        reference();
        set_filter(1, fc(QBW, -0x2000), fc(QBW, -0x2000));
        measure("HRTF left", 0);
        measure("HRTF right", 1);
    }
    if (voice_play(V_STEREO) == 0) {
        reference();
        set_filter(1, fc(QBW, -0x2000), fc(QBW, -0x2000));
        snprintf(what, sizeof(what), "stereo left");
        measure(what, 0);
        measure("stereo right", 1);
    }
}

/* Largest step between neighbouring samples of the left channel */
static uint32_t max_step(uint32_t frames) {
    uint32_t i, m = 0;
    for (i = 1; i < frames; i++) {
        int d = s_cap[2u * i] - s_cap[2u * (i - 1u)];
        if (d < 0) d = -d;
        if ((uint32_t)d > m) m = (uint32_t)d;
    }
    return m;
}

static void step_live(void) {
    uint32_t got = 0, n, before;
    DWORD t0;
    logf_("6 a live cutoff change: -0x1000 -> -0x4000 while playing (plain voice)\n");
    if (voice_play(V_PLAIN) != 0) return;
    set_filter(1, fc(QBW, -0x1000), fc(QBW, -0x1000));
    n = capture(CAP_FRAMES);
    before = max_step(n);
    apu_gp_fifo_discard(BAR0);
    t0 = GetTickCount();
    while (got < CAP_FRAMES && GetTickCount() - t0 < 3000u) {
        got += apu_gp_fifo_read(BAR0, s_cap + 2u * got, CAP_FRAMES - got);
        if (got > CAP_FRAMES / 2u && got < CAP_FRAMES / 2u + 2000u) {
            *record(MCPX_VOICE_TAR_FCA) = fc(QBW, -0x4000);
            *record(MCPX_VOICE_TAR_FCB) = fc(QBW, -0x4000);
            wc_flush();
        }
        Sleep(2);
    }
    logf_("  largest step: steady at -0x1000 %lu, across the change %lu\n", (unsigned long)before,
          (unsigned long)max_step(got));
}

static void step_top(void) {
    static const int32_t cuts[] = { -0x800, -0x700, -0x600, -0x500, -0x400, -0x300, -0x200, -0x100, -0x80, -0x40, -1 };
    uint32_t i;
    char what[48];
    logf_("7 the top of the range, FMODE 1, Q %04x (plain voice)\n", QBW);
    header();
    for (i = 0; i < sizeof(cuts) / sizeof(cuts[0]); i++) {
        set_filter(1, fc(QBW, cuts[i]), 0);
        cut_name(what, sizeof(what), "F1", QBW, cuts[i]);
        measure(what, 0);
    }
}

static void step_hrtf(void) {
    static const int32_t cuts[] = { -0x800, -0x1000, -0x1800, -0x2000, -0x2800, -0x3000, -0x4000, -0x5000 };
    uint32_t i;
    char what[48];
    logf_("8 HRTF voice, FMODE 1, Q %04x (against itself with FMODE 0)\n", QBW);
    header();
    if (voice_play(V_HRTF) != 0) return;
    reference();
    for (i = 0; i < sizeof(cuts) / sizeof(cuts[0]); i++) {
        set_filter(1, fc(QBW, cuts[i]), 0);
        cut_name(what, sizeof(what), "HRTF", QBW, cuts[i]);
        measure(what, 0);
    }
}

/* ---------------------------------------------------------------- main */

int main(void) {
    uint32_t rate;
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    logf_("\n=== apu_filter_probe round 2: 25 tones 50 Hz .. 20 kHz, levels in 0.1 dB against FMODE 0\n");
    save_log();
    apu_vp_debug_set_flags(APU_VP_DBG_NO_AC97 | APU_VP_DBG_AC97_COPY);   /* the 64 KiB ring, read by the CPU */
    if (apu_vp_init(BAR0) != 0) {
        logf_("apu_vp_init failed\n");
        save_log();
        for (;;) Sleep(1000);
    }
    make_signals();
    logf_("GP program loaded %s, front end %s, %lu frames/s\n", apu_gp_program_loaded() ? "yes" : "NO",
          apu_vp_front_end_ok() ? "yes (xemu)" : "no (console)", (unsigned long)frame_rate(200));
    if (voice_play(V_PLAIN) != 0) {
        save_log();
        for (;;) Sleep(1000);
    }
    reference();
    {
        double l = 0.0;
        uint32_t k;
        for (k = 0; k < NT; k++) l += s_ref[0][k];
        logf_("reference: plain voice, FMODE 0; tone power sum %lu (x1000)\n", (unsigned long)(l / 1000.0));
    }
    save_log();
#ifdef PROBE_ROUND1
    step_q();
    save_log();
    step_cut();
    save_log();
    step_ab();
    save_log();
    step_mode2();
    save_log();
    step_voices();
    save_log();
    step_live();
#else
    step_top();
    save_log();
    step_hrtf();
#endif
    voice_stop();
    rate = frame_rate(100);
    logf_("done, %lu frames/s\n", (unsigned long)rate);
    save_log();
    for (;;) Sleep(1000);
    return 0;
}
