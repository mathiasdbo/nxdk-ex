/*
 * HRTF on a console without front-end methods (clean-room: our own data and
 * the public xemu field list only; no state left by DirectSound or games).
 *
 * Found so far (log of earlier rounds in E:\apu_hrtf_probe.txt):
 *   round 1  no voice-record bit engages the HRTF stage on the 2D list
 *            (FMT bit 12 doubles a voice's output; MISC FMODE 1/3 is its filter)
 *   round 2  a voice on the 3D (or multipass) list with CFG_HRTF_TARGET != 0xFFFF
 *            runs the HRTF stage: the filter comes from the HRTF target array
 *            (register 0x2038; 64-byte entries indexed by the target), the VP keeps
 *            its state in the HRTF current array (0x203C; 128 bytes per voice
 *            handle), and 0x20AC routes the HRTF output to four mixbins
 *            (bins at 4:0, 12:8, 20:16, 28:24, the xemu SET_HRTF_SUBMIXES layout).
 *            A zero filter silenced the voice, a tap-0 impulse passed it.
 *
 * Round 3: the layout of an entry. A click (one spike every 100 ms) plays
 * through entry 0; for each test the entry's 16 dwords and the submix bins
 * are set, 150 ms of GP output (bins 0/1) are captured, and each channel's
 * peak and click onset (sample index) are logged. The onset difference
 * between the channels shows tap delays and the ITD.
 *
 * Resumable across a freeze (E:\hrtf3_progress.txt). Log appended to E:\apu_hrtf_probe.txt.
 */
#include <hal/debug.h>
#include <hal/video.h>
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
#define FRAMES      4800u         /* the click repeats every 100 ms */
#define H           1u
#define ARRAY_BYTES 0x10000u
#define CAPTURE     7200u         /* 150 ms of stereo frames */
#define PROGRESS    "E:\\hrtf5_progress.txt"

static char s_log[32 * 1024];
static size_t s_log_len, s_saved;

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

/* ---- tests: entry 0's 16 dwords in the SET_HRIR order guess
 *      (dword k = L[2k] | R[2k] << 8 | L[2k+1] << 16 | R[2k+1] << 24 for k < 15,
 *       dword 15 = L[30] | R[30] << 8 | ITD << 16) ---- */


/* ---- measurement ---- */

static uint32_t s_pcm_ba;
static uint32_t *s_ht, *s_hc;
static int16_t s_cap[2 * CAPTURE];

static uint32_t gp_delta(uint32_t ms) {
    uint32_t g0 = apu_gp_frames(BAR0);
    Sleep(ms);
    return (apu_gp_frames(BAR0) - g0) & 0xFFFFFFu;
}

static uint32_t capture(void) {
    uint32_t got = 0;
    DWORD t0 = GetTickCount();
    apu_gp_fifo_discard(BAR0);
    while (got < CAPTURE && GetTickCount() - t0 < 400u) {
        got += apu_gp_fifo_read(BAR0, s_cap + 2u * got, CAPTURE - got);
        Sleep(2);
    }
    return got;
}

/* Peak of channel c, and the first index reaching half of it */
static void analyse(uint32_t n, int c, int *peak, int *onset) {
    uint32_t i;
    int p = 0;
    for (i = 0; i < n; i++) {
        int a = s_cap[2 * i + c] < 0 ? -s_cap[2 * i + c] : s_cap[2 * i + c];
        if (a > p) p = a;
    }
    *peak = p;
    *onset = -1;
    if (p < 64) return;
    for (i = 0; i < n; i++) {
        int a = s_cap[2 * i + c] < 0 ? -s_cap[2 * i + c] : s_cap[2 * i + c];
        if (a * 2 >= p) {
            *onset = (int)i;
            return;
        }
    }
}

/*
 * Round 4 results (console): ear A (bytes 0/2, "left") feeds HRTF submix slots
 * 0 and 1, ear B (bytes 1/3, "right") slots 2 and 3 (each slot scaled by the
 * voice output volume of the same number); taps: dword k holds taps 2k, 2k+1
 * of each ear (ear B tap 20 arrived 20 samples after ear A tap 0); a negative
 * ITD delays ear B. Coefficients are NOT linear: 0x7F gave 0.969 of the direct
 * level (current 0x7C0) but 0x40 only 0.0625 (current 0x080). Fits an 8-bit
 * float: sign 7, exponent 6:4, mantissa 3:0, magnitude (16 + m) << (e - 1),
 * full scale 2048. Round 5 checks that over many codes (and negative ones),
 * whether handles 64 and above run HRTF, and a live change of the target.
 */
#define SUB(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define X 31u
#define STEREO_SUB SUB(0, X, 1, X)   /* ear A -> bin 0 (left), ear B -> bin 1 (right) */

typedef struct {
    const char *name;
    uint32_t code;      /* ear A tap 0 of entry 0 (byte 0 of dword 0) */
    uint32_t handle;
    int switch_to;      /* >= 0: after the capture, set CFG_HRTF_TARGET to it and capture again */
} test_t;

static const test_t s_tests[] = {
    { "code 01 (e0 m1: 1/2048?)", 0x01u, H, -1 },
    { "code 0F (e0 m15: 15?)", 0x0Fu, H, -1 },
    { "code 10 (e1 m0: 16?)", 0x10u, H, -1 },
    { "code 11 (e1 m1: 17?)", 0x11u, H, -1 },
    { "code 1F (e1 m15: 31?)", 0x1Fu, H, -1 },
    { "code 20 (e2 m0: 32?)", 0x20u, H, -1 },
    { "code 30 (e3 m0: 64?)", 0x30u, H, -1 },
    { "code 3F (e3 m15: 124?)", 0x3Fu, H, -1 },
    { "code 48 (e4 m8: 192?)", 0x48u, H, -1 },
    { "code 50 (e5 m0: 256?)", 0x50u, H, -1 },
    { "code 60 (e6 m0: 512?)", 0x60u, H, -1 },
    { "code 70 (e7 m0: 1024?)", 0x70u, H, -1 },
    { "code 7F (e7 m15: 1984)", 0x7Fu, H, -1 },
    { "code 81 (negative 1?)", 0x81u, H, -1 },
    { "code C0 (negative 128?)", 0xC0u, H, -1 },
    { "code FF (negative 1984?)", 0xFFu, H, -1 },
    { "handle 70 (>= 64), code 7F", 0x7Fu, 70u, -1 },
    { "handle 127, code 7F", 0x7Fu, 127u, -1 },
    { "handle 1: entry 0 (ear A) -> live to entry 1 (ear B)", 0x7Fu, H, 1 },
};
#define N_TESTS ((int)(sizeof(s_tests) / sizeof(s_tests[0])))

/* Signed sample of channel c at its peak */
static int signed_peak(uint32_t n, int c) {
    uint32_t i;
    int best = 0;
    for (i = 0; i < n; i++) {
        int v = s_cap[2 * i + c];
        if ((v < 0 ? -v : v) > (best < 0 ? -best : best)) best = v;
    }
    return best;
}

static void start_voice(uint32_t h) {
    volatile uint32_t *r = (volatile uint32_t *)apu_vp_debug_voice_record(h);
    uint8_t bins[APU_VP_OUTPUTS];
    uint16_t vols[APU_VP_OUTPUTS];
    uint32_t vbin, fb, vola, volb, volc, w;
    for (w = 0; w < APU_VP_OUTPUTS; w++) {
        bins[w] = (uint8_t)(w & 1u);
        vols[w] = 0xFFFu;
    }
    vols[0] = vols[2] = 0x080u;   /* slot 0 = ear A, slot 2 = ear B */
    apu_vp_pack_bins(bins, &vbin, &fb);
    apu_vp_pack_volumes(vols, &vola, &volb, &volc);
    for (w = 0; w < 32u; w++) r[w] = 0;
    r[MCPX_VOICE_CFG_VBIN / 4] = vbin;
    r[MCPX_VOICE_CFG_FMT / 4] = apu_vp_format(1, 16, true) | fb;
    r[MCPX_VOICE_CFG_HRTF_TARGET / 4] = 0;
    r[MCPX_VOICE_CUR_PSL_START / 4] = s_pcm_ba;
    r[MCPX_VOICE_PAR_NEXT / 4] = FRAMES - 1u;
    r[MCPX_VOICE_TAR_VOLA / 4] = vola;
    r[MCPX_VOICE_TAR_VOLB / 4] = volb;
    r[MCPX_VOICE_TAR_VOLC / 4] = volc;
    r[MCPX_VOICE_TAR_PITCH_LINK / 4] = MCPX_APU_LIST_END;
    r[MCPX_VOICE_PAR_STATE / 4] = MCPX_PAR_STATE_ACTIVE_VOICE;
    REG(MCPX_APU_TVL3D) = h;
}

static void stop_voice(void) {
    REG(MCPX_APU_TVL2D) = MCPX_APU_LIST_END;
    REG(MCPX_APU_TVL3D) = MCPX_APU_LIST_END;
    Sleep(3);
}

static void measure(const char *tag, uint32_t h) {
    uint32_t n = capture();
    int pl, ol, pr, orr;
    const uint32_t *c = s_hc + h * 32u;   /* the voice's 128-byte current record */
    analyse(n, 0, &pl, &ol);
    analyse(n, 1, &pr, &orr);
    logf_("   %-6s L %5d (signed %6d) R %5d (signed %6d)  current %08lx %08lx\n", tag, pl, signed_peak(n, 0), pr,
          signed_peak(n, 1), (unsigned long)c[0], (unsigned long)c[1]);
}

int main(void) {
    int resume, k;
    uint32_t ht_phys = 0, hc_phys = 0;

    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    resume = read_progress();
    if (resume >= N_TESTS) {
        logf_("\n=== round 5: all %d tests already ran (delete %s to run them again)\n", N_TESTS, PROGRESS);
        save_log();
        for (;;) Sleep(1000);
    }
    if (resume < 0) {
        logf_("\n=== round 5: coefficient code, handles >= 64, live target change; %d tests (fresh start)\n", N_TESTS);
        resume = 0;
    } else {
        logf_("\n=== round 5: resumed; test %d (%s) did not finish (FROZE?)\n", resume, s_tests[resume].name);
        resume++;
    }
    save_log();

    {
        uint32_t ph, i;
        int16_t *pcm = (int16_t *)apu_mem_alloc_table(FRAMES * 2u + APU_VP_SILENT_TAIL_BYTES, &ph);
        apu_vp_voice_params_t p;
        for (i = 0; i < FRAMES; i++) pcm[i] = 0;
        pcm[0] = 20000;   /* the click */
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
    REG(0x20ACu) = STEREO_SUB;
    logf_("frames %s; direct click level 15883 (round 3); expect peak = 15883 * magnitude / 2048\n",
          gp_delta(20) ? "running" : "STOPPED");
    save_log();

    for (k = resume; k < N_TESTS; k++) {
        const test_t *t = &s_tests[k];
        write_progress(k);
        memset(s_ht, 0, ARRAY_BYTES);
        memset(s_hc, 0, ARRAY_BYTES);
        s_ht[0] = t->code;          /* entry 0: ear A tap 0 */
        s_ht[16] = 0x00007F00u;     /* entry 1: ear B tap 0 = 0x7F */
        logf_("%2d %s\n", k, t->name);
        start_voice(t->handle);
        Sleep(60);
        measure("", t->handle);
        if (t->switch_to >= 0) {
            ((volatile uint32_t *)apu_vp_debug_voice_record(t->handle))[MCPX_VOICE_CFG_HRTF_TARGET / 4] = (uint32_t)t->switch_to;
            Sleep(60);
            measure("after", t->handle);
        }
        stop_voice();
        if (!gp_delta(20)) {
            logf_("   frames STOPPED after test %d: power-cycle and run again to continue\n", k);
            save_log();
            for (;;) Sleep(1000);
        }
        save_log();
    }
    write_progress(N_TESTS);
    logf_("done\n");
    save_log();
    for (;;) Sleep(1000);
    return 0;
}
