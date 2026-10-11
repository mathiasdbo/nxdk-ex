/*
 * HRTF on a console without front-end methods: what switches a voice into the
 * VP's HRTF stage? (Clean-room: only our own data and the public xemu field
 * list; no state left by DirectSound or games.)
 *
 * Set-up: the driver runs (AC97 off, the probe reads the GP FIFO itself). The
 * HRTF target/current address registers (0x2038/0x203C) point at our own
 * arrays: the target array all zero (a zero filter: an engaged HRTF stage
 * should make the voice silent), the current array filled with 0x5A5A5A5A (the
 * VP writing its filter state there would show).
 *
 * Each test writes a fresh record for a looping tone on handle 1 with
 * CFG_HRTF_TARGET = 0 plus one candidate bit or field value, links it, and
 * measures for 60 ms: CBO advance, GP output peak (bins 0/1), and how many
 * current-array dwords changed. A change in the last two is the signal.
 *
 * A candidate may freeze the APU: progress goes to E:\hrtf_progress.txt before
 * every test; after a power cycle the probe resumes past the test that froze
 * and logs it. Delete that file to start over. Log: E:\apu_hrtf_probe.txt (appended).
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
#define FRAMES      20000u
#define H           1u            /* the probe voice: an HRTF-capable handle (below 64) */
#define ARRAY_BYTES 0x10000u
#define SENTINEL    0x5A5A5A5Au

static char s_log[48 * 1024];
static size_t s_log_len;

static void logf_(const char *fmt, ...) {
    char line[300];
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

/* Append the new part of the log */
static size_t s_saved;
static void save_log(void) {
    FILE *f;
    mount_e();
    f = fopen("E:\\apu_hrtf_probe.txt", "ab");
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
    f = fopen("E:\\hrtf_progress.txt", "rb");
    if (f) {
        if (fscanf(f, "%d", &v) != 1) v = -1;
        fclose(f);
    }
    return v;
}

static void write_progress(int v) {
    FILE *f;
    mount_e();
    f = fopen("E:\\hrtf_progress.txt", "wb");
    if (f) {
        fprintf(f, "%d\n", v);
        fclose(f);
    }
}

/* ---- the tests ---- */

typedef struct {
    const char *name;
    uint32_t off;     /* record dword to modify (0xFFFF: none) */
    uint32_t value;   /* OR-ed into it */
    uint32_t target;  /* CFG_HRTF_TARGET */
} test_t;

#define N_TESTS 64
static test_t s_tests[N_TESTS];
static int s_n;

static void add(const char *name, uint32_t off, uint32_t value, uint32_t target) {
    if (s_n < N_TESTS) {
        s_tests[s_n].name = name;
        s_tests[s_n].off = off;
        s_tests[s_n].value = value;
        s_tests[s_n].target = target;
        s_n++;
    }
}

static const char *s_bitname[32] = { "b0", "b1", "b2", "b3", "b4", "b5", "b6", "b7", "b8", "b9", "b10", "b11",
                                     "b12", "b13", "b14", "b15", "b16", "b17", "b18", "b19", "b20", "b21",
                                     "b22", "b23", "b24", "b25", "b26", "b27", "b28", "b29", "b30", "b31" };

static void build_tests(void) {
    uint32_t b;
    add("baseline, target 0xFFFF", 0xFFFFu, 0, 0xFFFFu);
    add("baseline, target 0", 0xFFFFu, 0, 0);
    for (b = 10; b <= 15; b++) add(s_bitname[b], MCPX_VOICE_CFG_FMT, 1u << b, 0);           /* FMT 10-15 (HEADROOM 13-15) */
    add("FMODE 1", MCPX_VOICE_CFG_ENV1 + 8u, 1u << 16, 0);                                 /* CFG_MISC 0x18 */
    add("FMODE 2", MCPX_VOICE_CFG_ENV1 + 8u, 2u << 16, 0);
    add("FMODE 3", MCPX_VOICE_CFG_ENV1 + 8u, 3u << 16, 0);
    for (b = 12; b <= 15; b++) add(s_bitname[b], MCPX_VOICE_CFG_ENV1 + 8u, 1u << b, 0);    /* MISC 12-15 */
    for (b = 18; b <= 31; b++) add(s_bitname[b], MCPX_VOICE_CFG_ENV1 + 8u, 1u << b, 0);    /* MISC 18-31 */
    for (b = 16; b <= 31; b++) add(s_bitname[b], MCPX_VOICE_CFG_HRTF_TARGET, 1u << b, 0);  /* HRTF_TARGET 16-31 */
    add(s_bitname[15], MCPX_VOICE_CFG_VBIN, 1u << 15, 0);
    add(s_bitname[31], MCPX_VOICE_CFG_VBIN, 1u << 31, 0);
}

static const char *field_name(uint32_t off) {
    switch (off) {
    case MCPX_VOICE_CFG_VBIN: return "VBIN";
    case MCPX_VOICE_CFG_FMT: return "FMT";
    case MCPX_VOICE_CFG_ENV1 + 8u: return "MISC";
    case MCPX_VOICE_CFG_HRTF_TARGET: return "HRTF_TARGET";
    default: return "-";
    }
}

/* ---- measurement ---- */

static uint32_t s_pcm_ba;
static uint32_t *s_ht, *s_hc;

static uint32_t gp_delta(uint32_t ms) {
    uint32_t g0 = apu_gp_frames(BAR0);
    Sleep(ms);
    return (apu_gp_frames(BAR0) - g0) & 0xFFFFFFu;
}

static void fifo_peak(uint32_t ms, int *pl, int *pr) {
    static int16_t buf[2 * 512];
    DWORD t0 = GetTickCount();
    int l = 0, r = 0;
    apu_gp_fifo_discard(BAR0);
    while (GetTickCount() - t0 < ms) {
        uint32_t n = apu_gp_fifo_read(BAR0, buf, 512u), i;
        for (i = 0; i < n; i++) {
            int a = buf[2 * i] < 0 ? -buf[2 * i] : buf[2 * i];
            int c = buf[2 * i + 1] < 0 ? -buf[2 * i + 1] : buf[2 * i + 1];
            if (a > l) l = a;
            if (c > r) r = c;
        }
        Sleep(2);
    }
    *pl = l;
    *pr = r;
}

static void reset_arrays(void) {
    uint32_t i;
    memset(s_ht, 0, ARRAY_BYTES);
    for (i = 0; i < ARRAY_BYTES / 4u; i++) s_hc[i] = SENTINEL;
}

/* A fresh looping-tone record on handle H, linked at the 2D list head */
static void start_voice(const test_t *t) {
    volatile uint32_t *r = (volatile uint32_t *)apu_vp_debug_voice_record(H);
    uint8_t bins[APU_VP_OUTPUTS];
    uint16_t vols[APU_VP_OUTPUTS];
    uint32_t vbin, fb, vola, volb, volc, w;
    for (w = 0; w < APU_VP_OUTPUTS; w++) {
        bins[w] = (uint8_t)(w & 1u);
        vols[w] = 0xFFFu;
    }
    vols[0] = vols[1] = 0x100u;
    apu_vp_pack_bins(bins, &vbin, &fb);
    apu_vp_pack_volumes(vols, &vola, &volb, &volc);
    for (w = 0; w < 32u; w++) r[w] = 0;
    r[MCPX_VOICE_CFG_VBIN / 4] = vbin;
    r[MCPX_VOICE_CFG_FMT / 4] = apu_vp_format(1, 16, true) | fb;
    r[MCPX_VOICE_CFG_HRTF_TARGET / 4] = t->target;
    if (t->off != 0xFFFFu) r[t->off / 4] |= t->value;
    r[MCPX_VOICE_CUR_PSL_START / 4] = s_pcm_ba;
    r[MCPX_VOICE_PAR_NEXT / 4] = FRAMES - 1u;
    r[MCPX_VOICE_TAR_VOLA / 4] = vola;
    r[MCPX_VOICE_TAR_VOLB / 4] = volb;
    r[MCPX_VOICE_TAR_VOLC / 4] = volc;
    r[MCPX_VOICE_TAR_PITCH_LINK / 4] = MCPX_APU_LIST_END;
    r[MCPX_VOICE_PAR_STATE / 4] = MCPX_PAR_STATE_ACTIVE_VOICE;
    REG(MCPX_APU_TVL2D) = H;   /* the only voice on the list */
}

static void stop_voice(void) {
    REG(MCPX_APU_TVL2D) = MCPX_APU_LIST_END;
    Sleep(3);   /* past any frame still holding it */
}

int main(void) {
    int resume, k;
    uint32_t ht_phys = 0, hc_phys = 0;

    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    build_tests();
    resume = read_progress();
    if (resume >= s_n) {
        logf_("\n=== apu_hrtf_probe: all %d tests already ran (delete E:\\hrtf_progress.txt to run them again)\n", s_n);
        save_log();
        for (;;) Sleep(1000);
    }
    if (resume < 0) {
        logf_("\n=== apu_hrtf_probe: %d candidates for the HRTF enable (fresh start)\n", s_n);
        resume = 0;
    } else {
        logf_("\n=== apu_hrtf_probe: resumed; test %d (%s %s) did not finish (FROZE?)\n", resume,
              field_name(s_tests[resume].off), s_tests[resume].name);
        resume++;
    }
    save_log();

    {   /* the tone: a mapping made by the driver gives its BA */
        uint32_t ph, i;
        int16_t *pcm = (int16_t *)apu_mem_alloc_table(FRAMES * 2u + APU_VP_SILENT_TAIL_BYTES, &ph);
        apu_vp_voice_params_t p;
        for (i = 0; i < FRAMES; i++) pcm[i] = (int16_t)(6000.0f * sinf(6.2831853f * 440.0f * (float)i / 48000.0f));
        apu_vp_debug_set_flags(APU_VP_DBG_NO_AC97);
        if (apu_vp_init(BAR0) != 0) {
            logf_("apu_vp_init failed\n");
            save_log();
            for (;;) Sleep(1000);
        }
        memset(&p, 0, sizeof(p));
        p.phys = ph;
        p.bytes = FRAMES * 2u;
        p.channels = 1;
        p.bits = 16;
        p.loop = true;
        p.hrtf_entry = -1;
        p.tail_bytes = APU_VP_SILENT_TAIL_BYTES;
        for (i = 0; i < APU_VP_OUTPUTS; i++) p.vols[i] = 0xFFFu;
        apu_vp_voice_start(BAR0, 100, &p);
        s_pcm_ba = ((volatile uint32_t *)apu_vp_debug_voice_record(100))[MCPX_VOICE_CUR_PSL_START / 4] & 0xFFFFFFu;
        apu_vp_voice_off(BAR0, 100);
        Sleep(5);
    }
    s_ht = (uint32_t *)apu_mem_alloc_table(ARRAY_BYTES, &ht_phys);
    s_hc = (uint32_t *)apu_mem_alloc_table(ARRAY_BYTES, &hc_phys);
    REG(0x2038) = ht_phys;
    REG(0x203C) = hc_phys;
    logf_("frames %s; HRTF arrays at %08lx / %08lx; columns: CBO advance, GP peak L/R, current-array dwords changed\n",
          gp_delta(20) ? "running" : "STOPPED", (unsigned long)ht_phys, (unsigned long)hc_phys);
    save_log();

    for (k = resume; k < s_n; k++) {
        const test_t *t = &s_tests[k];
        uint32_t c0, c1, i, changed = 0, first = 0xFFFFFFFFu;
        int pl, pr;
        write_progress(k);
        reset_arrays();
        start_voice(t);
        Sleep(20);
        c0 = ((volatile uint32_t *)apu_vp_debug_voice_record(H))[MCPX_VOICE_PAR_OFFSET / 4] & 0xFFFFFFu;
        fifo_peak(60, &pl, &pr);
        c1 = ((volatile uint32_t *)apu_vp_debug_voice_record(H))[MCPX_VOICE_PAR_OFFSET / 4] & 0xFFFFFFu;
        for (i = 0; i < ARRAY_BYTES / 4u; i++) {
            if (s_hc[i] != SENTINEL) {
                if (!changed) first = i * 4u;
                changed++;
            }
        }
        stop_voice();
        logf_("%2d %-11s %-26s CBO +%5ld  peak %5d/%5d  cur %4lu\n", k, field_name(t->off), t->name,
              (long)(int32_t)(c1 - c0), pl, pr, (unsigned long)changed);
        if (changed) {
            logf_("      first changed dword at +%05lx: %08lx %08lx %08lx %08lx\n", (unsigned long)first,
                  (unsigned long)s_hc[first / 4u], (unsigned long)s_hc[first / 4u + 1u],
                  (unsigned long)s_hc[first / 4u + 2u], (unsigned long)s_hc[first / 4u + 3u]);
        }
        if (!gp_delta(20)) {
            logf_("   frames STOPPED after test %d: power-cycle and run again to continue\n", k);
            save_log();
            for (;;) Sleep(1000);
        }
        save_log();
    }
    write_progress(s_n);   /* all done: the next run resumes past the end */
    logf_("done (delete E:\\hrtf_progress.txt to run the list again)\n");
    save_log();
    for (;;) Sleep(1000);
    return 0;
}
