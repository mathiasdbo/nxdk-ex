/*
 * EFX routing and filter probe (lib/openal/docs/EFX_PLAN.md, task phase8-00).
 * Clean-room: our own tones, noise and register writes; the library's copy GP
 * program. A mixbin pair n/n+1 is captured by pointing the FIFO descriptor's
 * DSP address (X:3) at $1400 + 32n and reading the GP output ring.
 *
 *   1  send bins: outputs 4/5 -> bins 2k/2k+1 (k = 1..4) on a plain, an HRTF
 *      and a stereo voice (left only, then right only): RMS of bins 0..9
 *   2  bin 30: the HRTF spare submix slots on bin 31 (today) and on bin 30
 *   3  live re-route: CFG_VBIN / CFG_FMT bins rewritten on a playing voice
 *   4  VP filter: CFG_MISC FMODE x TAR_FCA/FCB on white noise, 10 bands
 *
 * Log appended to E:\apu_efx_probe.txt. Resumable across a freeze through
 * E:\efx_probe_progress.txt.
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
#define XW(n)       (*(volatile uint32_t *)(BAR0 + MCPX_APU_GP_XMEM + 4u * (n)))
#define PROGRESS    "E:\\efx_probe_progress.txt"
#define LOGFILE     "E:\\apu_efx_probe.txt"

#define RATE        48000u
#define TONE_AMP    8000.0f     /* -12 dBFS */
#define CAP_FRAMES  16384u

static char s_log[64 * 1024];
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

static void wc_flush(void) {
    __asm__ __volatile__("lock; addl $0,(%%esp)" ::: "memory");
}

static uint32_t frames_now(void) {
    return XW(0x10u) & 0xFFFFFFu;
}

static uint32_t frame_rate(uint32_t ms) {
    uint32_t f0 = frames_now();
    DWORD t0 = GetTickCount(), dt;
    Sleep(ms);
    dt = GetTickCount() - t0;
    return dt ? (((frames_now() - f0) & 0xFFFFFFu) * 1000u + dt / 2u) / dt : 0;
}

/* ---------------------------------------------------------------- signals and voices */

static uint32_t s_tone_phys, s_left_phys, s_right_phys, s_noise_phys;
static int16_t s_cap[2 * CAP_FRAMES];
static uint32_t s_h = 0xFFFFu;

/* One second of a 1 kHz tone from the sample index (no phase drift) */
static float tone(uint32_t i) {
    return TONE_AMP * sinf(2.0f * 3.14159265f * 1000.0f * (float)(i % RATE) / (float)RATE);
}

static void make_signals(void) {
    uint32_t i, seed = 12345u, tail = APU_VP_SILENT_TAIL_BYTES;
    int16_t *t = (int16_t *)apu_mem_alloc_table(RATE * 2u + tail, &s_tone_phys);
    int16_t *l = (int16_t *)apu_mem_alloc_table(RATE * 4u + tail, &s_left_phys);
    int16_t *r = (int16_t *)apu_mem_alloc_table(RATE * 4u + tail, &s_right_phys);
    int16_t *n = (int16_t *)apu_mem_alloc_table(RATE * 2u + tail, &s_noise_phys);
    memset(t, 0, RATE * 2u + tail);
    memset(l, 0, RATE * 4u + tail);
    memset(r, 0, RATE * 4u + tail);
    memset(n, 0, RATE * 2u + tail);
    for (i = 0; i < RATE; i++) {
        int16_t v = (int16_t)tone(i);
        t[i] = v;
        l[2u * i] = v;          /* stereo, left only */
        r[2u * i + 1u] = v;     /* stereo, right only */
        seed = seed * 1664525u + 1013904223u;
        n[i] = (int16_t)((int32_t)(seed >> 16) % 8001 - 4000) * 2;   /* white, about -12 dBFS peak */
    }
}

static void voice_stop(void) {
    if (s_h != 0xFFFFu) {
        apu_vp_voice_off(BAR0, s_h);
        Sleep(5);
    }
}

/* bins/vols: 8 entries; vol 0 = unity, 0xFFF = mute */
static int voice_play(uint32_t phys, int channels, int hrtf_entry, const uint8_t bins[8], const uint16_t vols[8]) {
    apu_vp_voice_params_t p;
    int rc;
    voice_stop();
    if (s_h == 0xFFFFu) s_h = apu_vp_alloc_handle(BAR0);
    memset(&p, 0, sizeof(p));
    p.phys = phys;
    p.bytes = RATE * 2u * (uint32_t)channels;
    p.channels = channels;
    p.bits = 16;
    p.loop = true;
    p.tail_bytes = APU_VP_SILENT_TAIL_BYTES;
    p.hrtf_entry = hrtf_entry;
    memcpy(p.bins, bins, 8);
    memcpy(p.vols, vols, 8 * sizeof(uint16_t));
    rc = apu_vp_voice_start(BAR0, s_h, &p);
    if (rc != 0) logf_("  voice start failed (%d)\n", rc);
    Sleep(30);
    return rc;
}

static volatile uint32_t *record(uint32_t off) {
    return (volatile uint32_t *)(apu_vp_debug_voice_record(s_h) + off);
}

/* Capture `frames` of bins n (left) and n+1 (right) into s_cap */
static uint32_t capture_pair(uint32_t n, uint32_t frames) {
    uint32_t got = 0;
    DWORD t0;
    XW(3u) = 0x1400u + 32u * n;
    Sleep(5);
    apu_gp_fifo_discard(BAR0);
    t0 = GetTickCount();
    while (got < frames && GetTickCount() - t0 < 2000u) {
        got += apu_gp_fifo_read(BAR0, s_cap + 2u * got, frames - got);
        Sleep(2);
    }
    XW(3u) = 0x1400u;
    return got;
}

static uint32_t rms(uint32_t frames, uint32_t ch) {
    double acc = 0.0;
    uint32_t i;
    if (!frames) return 0;
    for (i = 0; i < frames; i++) {
        double v = s_cap[2u * i + ch];
        acc += v * v;
    }
    return (uint32_t)(sqrt(acc / frames) + 0.5);
}

/* RMS of bins 0..9 and 30..31 as one line */
static void log_bins(const char *what, bool with30) {
    uint32_t n, k;
    logf_("  %-26s", what);
    for (k = 0; k < (with30 ? 6u : 5u); k++) {
        uint32_t pair = (k < 5u) ? 2u * k : 30u;
        n = capture_pair(pair, 4800u);
        logf_(" %2lu:%5lu %2lu:%5lu", (unsigned long)pair, (unsigned long)rms(n, 0), (unsigned long)(pair + 1u),
              (unsigned long)rms(n, 1));
    }
    logf_("\n");
}

/* ---------------------------------------------------------------- 1 send bins */

static void dry_outputs(uint8_t bins[8], uint16_t vols[8], bool hrtf) {
    uint32_t i;
    for (i = 0; i < 8u; i++) {
        bins[i] = (uint8_t)(i & 1u);
        vols[i] = 0xFFFu;
    }
    vols[0] = 0;
    if (hrtf) vols[2] = 0;   /* left ear slot 0 via output 0, right ear slot 2 via output 2 */
    else vols[1] = 0;
}

static void step_sends(void) {
    static const struct { const char *name; int kind; } kinds[4] = {
        { "plain mono", 0 }, { "HRTF mono", 1 }, { "stereo, left only", 2 }, { "stereo, right only", 3 },
    };
    uint32_t k, j;
    logf_("  tone RMS is %lu; each line: bin:RMS\n", (unsigned long)(TONE_AMP / 1.41421356f + 0.5f));
    for (j = 0; j < 4u; j++) {
        for (k = 1; k <= 4u; k++) {
            uint8_t bins[8];
            uint16_t vols[8];
            char what[48];
            int kind = kinds[j].kind;
            dry_outputs(bins, vols, kind == 1);
            bins[4] = (uint8_t)(2u * k);
            bins[5] = (uint8_t)(2u * k + 1u);
            vols[4] = vols[5] = 0;
            if (voice_play(kind == 0 || kind == 1 ? s_tone_phys : (kind == 2 ? s_left_phys : s_right_phys),
                           kind >= 2 ? 2 : 1, kind == 1 ? 0 : -1, bins, vols) != 0) {
                continue;
            }
            snprintf(what, sizeof(what), "%s, send %lu/%lu", kinds[j].name, (unsigned long)(2u * k),
                     (unsigned long)(2u * k + 1u));
            log_bins(what, false);
        }
    }
    voice_stop();
}

/* ---------------------------------------------------------------- 2 bin 30 */

static void step_bin30(void) {
    uint8_t bins[8];
    uint16_t vols[8];
    uint32_t saved = REG(MCPX_APU_HRTF_SUBMIXES);
    dry_outputs(bins, vols, true);
    vols[1] = vols[3] = 0;   /* the spare slots carry the ears too */
    if (voice_play(s_tone_phys, 1, 0, bins, vols) != 0) return;
    logf_("  0x20AC = %08lx (today)\n", (unsigned long)saved);
    log_bins("HRTF, spare slots on 31", true);
    REG(MCPX_APU_HRTF_SUBMIXES) = 0u | (30u << 8) | (1u << 16) | (30u << 24);
    Sleep(30);
    logf_("  0x20AC = %08lx\n", (unsigned long)REG(MCPX_APU_HRTF_SUBMIXES));
    log_bins("HRTF, spare slots on 30", true);
    vols[1] = vols[3] = 0xFFFu;   /* as the library plays them */
    voice_play(s_tone_phys, 1, 0, bins, vols);
    log_bins("HRTF, spare muted, on 30", true);
    REG(MCPX_APU_HRTF_SUBMIXES) = saved;
    Sleep(10);
    voice_stop();
}

/* ---------------------------------------------------------------- 3 live re-route */

/* Largest step between neighbouring samples of channel ch */
static uint32_t max_step(uint32_t frames, uint32_t ch) {
    uint32_t i, m = 0;
    for (i = 1; i < frames; i++) {
        int d = abs(s_cap[2u * i + ch] - s_cap[2u * (i - 1u) + ch]);
        if ((uint32_t)d > m) m = (uint32_t)d;
    }
    return m;
}

/* Capture pair `pair` while `rewrite` runs about 100 ms in */
static uint32_t capture_across(uint32_t pair, void (*rewrite)(void)) {
    uint32_t got = 0;
    bool done = false;
    DWORD t0;
    XW(3u) = 0x1400u + 32u * pair;
    Sleep(5);
    apu_gp_fifo_discard(BAR0);
    t0 = GetTickCount();
    while (got < 9600u && GetTickCount() - t0 < 2000u) {
        got += apu_gp_fifo_read(BAR0, s_cap + 2u * got, 9600u - got);
        if (!done && got >= 4800u) {
            rewrite();
            done = true;
        }
        Sleep(2);
    }
    XW(3u) = 0x1400u;
    return got;
}

static uint32_t s_vbin_new, s_fmt_new;
static void rewrite_vbin(void) { *record(MCPX_VOICE_CFG_VBIN) = s_vbin_new; wc_flush(); }
static void rewrite_fmt(void) { *record(MCPX_VOICE_CFG_FMT) = s_fmt_new; wc_flush(); }

static void step_reroute(void) {
    uint8_t bins[8];
    uint16_t vols[8];
    uint32_t vbin, fmt_bins, n, tone_step;
    dry_outputs(bins, vols, false);
    bins[4] = 2; bins[5] = 3; vols[4] = vols[5] = 0;
    bins[6] = 6; bins[7] = 7; vols[6] = vols[7] = 0;
    if (voice_play(s_tone_phys, 1, -1, bins, vols) != 0) return;
    n = capture_pair(0, 4800u);
    tone_step = max_step(n, 0);
    logf_("  the tone's own largest step: %lu\n", (unsigned long)tone_step);
    log_bins("before (4/5 -> 2/3, 6/7 -> 6/7)", false);

    /* outputs 4/5 -> bins 4/5 through CFG_VBIN, dry pair watched across it */
    bins[4] = 4; bins[5] = 5;
    apu_vp_pack_bins(bins, &vbin, &fmt_bins);
    s_vbin_new = vbin;
    logf_("  CFG_VBIN %08lx -> %08lx\n", (unsigned long)*record(MCPX_VOICE_CFG_VBIN), (unsigned long)vbin);
    n = capture_across(0, rewrite_vbin);
    logf_("    dry bins 0/1 across the write: largest step %lu / %lu\n", (unsigned long)max_step(n, 0),
          (unsigned long)max_step(n, 1));
    Sleep(20);
    logf_("    record CFG_VBIN now %08lx\n", (unsigned long)*record(MCPX_VOICE_CFG_VBIN));
    log_bins("after VBIN (4/5 -> 4/5?)", false);

    /* outputs 6/7 -> bins 8/9 through the CFG_FMT bin bits, the new pair watched */
    bins[6] = 8; bins[7] = 9;
    apu_vp_pack_bins(bins, &vbin, &fmt_bins);
    s_fmt_new = (*record(MCPX_VOICE_CFG_FMT) & ~0x3FFu) | fmt_bins;
    logf_("  CFG_FMT %08lx -> %08lx\n", (unsigned long)*record(MCPX_VOICE_CFG_FMT), (unsigned long)s_fmt_new);
    n = capture_across(8, rewrite_fmt);
    {
        double acc = 0.0;
        uint32_t i, half = (n < 4800u) ? n : 4800u;
        for (i = half; i < n; i++) acc += (double)s_cap[2u * i] * s_cap[2u * i];
        logf_("    bins 8/9 across the write: RMS before %lu, after %lu, largest step %lu\n",
              (unsigned long)rms(half, 0), (unsigned long)(n > half ? sqrt(acc / (n - half)) + 0.5 : 0.0),
              (unsigned long)max_step(n, 0));
    }
    Sleep(20);
    log_bins("after FMT (6/7 -> 8/9?)", false);

    /* the same rewrite with the voice restarted: the reference */
    voice_play(s_tone_phys, 1, -1, bins, vols);
    log_bins("restarted with the new bins", false);
    voice_stop();
}

/* ---------------------------------------------------------------- 4 VP filter */

#define BANDS 10
static const float s_band_hz[BANDS] = { 100, 250, 500, 1000, 2000, 4000, 6000, 8000, 12000, 16000 };
#define GBLOCK 512u

/* Average Goertzel power of the left channel per band over `frames` */
static void band_power(uint32_t frames, double out[BANDS]) {
    uint32_t b, blk, i, blocks = frames / GBLOCK;
    for (b = 0; b < BANDS; b++) {
        double c = 2.0 * cos(2.0 * 3.14159265358979 * s_band_hz[b] / RATE), acc = 0.0;
        for (blk = 0; blk < blocks; blk++) {
            double s1 = 0.0, s2 = 0.0;
            for (i = 0; i < GBLOCK; i++) {
                double s = s_cap[2u * (blk * GBLOCK + i)] + c * s1 - s2;
                s2 = s1;
                s1 = s;
            }
            acc += s1 * s1 + s2 * s2 - c * s1 * s2;
        }
        out[b] = blocks ? acc / blocks : 0.0;
    }
}

static double s_ref[BANDS];

static void log_filter(const char *what) {
    double p[BANDS];
    uint32_t b, n = capture_pair(0, CAP_FRAMES);
    band_power(n, p);
    logf_("  %-30s RMS %5lu:", what, (unsigned long)rms(n, 0));
    for (b = 0; b < BANDS; b++) {
        int db10 = (p[b] > 0.0 && s_ref[b] > 0.0) ? (int)lrint(100.0 * log10(p[b] / s_ref[b])) : -999;
        logf_(" %4d", db10);
    }
    logf_("\n");
}

static void step_filter(void) {
    static const int32_t cut[] = { 0, -0x1000, -0x2000, -0x3000, -0x4000, -0x6000 };
    static const uint32_t qs[] = { 0u, 0x4000u };
    uint8_t bins[8];
    uint16_t vols[8];
    uint32_t m, c, q, b, n;
    char what[64];
    dry_outputs(bins, vols, false);
    if (voice_play(s_noise_phys, 1, -1, bins, vols) != 0) return;
    n = capture_pair(0, CAP_FRAMES);
    band_power(n, s_ref);
    logf_("  white noise, FMODE 0: RMS %lu; levels below in 0.1 dB against it at", (unsigned long)rms(n, 0));
    for (b = 0; b < BANDS; b++) logf_(" %lu", (unsigned long)s_band_hz[b]);
    logf_(" Hz\n  record: CFG_MISC %08lx TAR_FCA %08lx TAR_FCB %08lx\n", (unsigned long)*record(MCPX_VOICE_CFG_MISC),
          (unsigned long)*record(MCPX_VOICE_TAR_FCA), (unsigned long)*record(MCPX_VOICE_TAR_FCB));
    for (m = 0; m < 4u; m++) {
        for (q = 0; q < 2u; q++) {
            for (c = 0; c < sizeof(cut) / sizeof(cut[0]); c++) {
                uint32_t fc = (qs[q] << 16) | ((uint32_t)cut[c] & 0xFFFFu);
                if (m == 0u && (c != 2u || q != 0u)) continue;   /* mode 0 once, with a cutoff set */
                *record(MCPX_VOICE_CFG_MISC) = (*record(MCPX_VOICE_CFG_MISC) & ~0x30000u) | (m << 16);
                *record(MCPX_VOICE_TAR_FCA) = fc;
                *record(MCPX_VOICE_TAR_FCB) = fc;
                wc_flush();
                Sleep(30);
                snprintf(what, sizeof(what), "FMODE %lu cut %s%04lx Q %04lx", (unsigned long)m, cut[c] < 0 ? "-" : "+",
                         (unsigned long)(cut[c] < 0 ? -cut[c] : cut[c]), (unsigned long)qs[q]);
                log_filter(what);
            }
        }
        save_log();
    }
    /* back to no filter, then the same with the voice restarted after the write */
    *record(MCPX_VOICE_CFG_MISC) &= ~0x30000u;
    *record(MCPX_VOICE_TAR_FCA) = 0;
    *record(MCPX_VOICE_TAR_FCB) = 0;
    wc_flush();
    Sleep(30);
    log_filter("FMODE 0 again (live)");
    voice_stop();
}

/* ---------------------------------------------------------------- driver */

typedef struct {
    const char *name;
    void (*run)(void);
} step_t;

static const step_t s_steps[] = {
    { "1 send bins (outputs 4/5)", step_sends },
    { "2 HRTF spare slots on bin 30", step_bin30 },
    { "3 live re-route (CFG_VBIN, CFG_FMT bins)", step_reroute },
    { "4 VP filter (FMODE, TAR_FCA/FCB)", step_filter },
};
#define N_STEPS ((int)(sizeof(s_steps) / sizeof(s_steps[0])))

int main(void) {
    int resume, k;

    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    resume = read_progress();
    if (resume >= N_STEPS) {
        logf_("\n=== apu_efx_probe: all steps ran (delete %s to run them again); running them again\n", PROGRESS);
        resume = 0;
    } else if (resume < 0) {
        logf_("\n=== apu_efx_probe: %d steps (fresh start)\n", N_STEPS);
        resume = 0;
    } else {
        logf_("\n=== apu_efx_probe: resumed; step %s did not finish (FROZE?)\n", s_steps[resume].name);
        resume++;
    }
    save_log();

    apu_vp_debug_set_flags(APU_VP_DBG_NO_AC97 | APU_VP_DBG_AC97_COPY);   /* the 64 KiB ring, read by the CPU */
    if (apu_vp_init(BAR0) != 0) {
        logf_("apu_vp_init failed\n");
        save_log();
        for (;;) Sleep(1000);
    }
    make_signals();
    logf_("GP program loaded %s, HRTF %s, front end %s, start retries %lu, %lu frames/s\n",
          apu_gp_program_loaded() ? "yes" : "NO", apu_vp_hrtf_available() ? "yes" : "no",
          apu_vp_front_end_ok() ? "yes (xemu)" : "no (console)", (unsigned long)apu_vp_start_retries(),
          (unsigned long)frame_rate(200));
    save_log();

    for (k = resume; k < N_STEPS; k++) {
        uint32_t rate;
        write_progress(k);
        logf_("%s\n", s_steps[k].name);
        save_log();
        s_steps[k].run();
        rate = frame_rate(100);
        logf_("  frames/s %lu\n", (unsigned long)rate);
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
