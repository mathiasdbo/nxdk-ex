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
#define PROGRESS    "E:\\hrtf3_progress.txt"

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

typedef struct {
    const char *name;
    uint32_t submix;
    bool direct;        /* 2D list, no HRTF: the reference */
    uint32_t w[16];
} test_t;

#define SUB_01  0x1F1F0100u   /* submix slots 0,1 -> bins 0,1; slots 2,3 -> bin 31 (not forwarded) */
#define SUB_23  0x01001F1Fu   /* slots 2,3 -> bins 0,1 */

static const test_t s_tests[] = {
    { "direct (2D, no HRTF): reference", SUB_01, true, { 0 } },
    { "w0=00007F7F (tap0 both), slots 0/1", SUB_01, false, { 0x00007F7Fu } },
    { "w0=00007F7F, slots 2/3", SUB_23, false, { 0x00007F7Fu } },
    { "w0=0000007F (byte 0 only)", SUB_01, false, { 0x0000007Fu } },
    { "w0=00007F00 (byte 1 only)", SUB_01, false, { 0x00007F00u } },
    { "w0=007F0000 (byte 2 only)", SUB_01, false, { 0x007F0000u } },
    { "w0=7F000000 (byte 3 only)", SUB_01, false, { 0x7F000000u } },
    { "w0=0000407F: byte0 127, byte1 64", SUB_01, false, { 0x0000407Fu } },
    { "byte0 of w0 + byte1 of w10 (R tap 20?)", SUB_01, false, { 0x0000007Fu, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00007F00u } },
    { "byte0 of w0 + byte1 of w1 (R tap 2?)", SUB_01, false, { 0x0000007Fu, 0x00007F00u } },
    { "byte0 of w0 + byte1 of w15 (R tap 30?)", SUB_01, false, { 0x0000007Fu, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00007F00u } },
    { "w0=00007F7F, w15 hi=+10.0 (ITD s6.9?)", SUB_01, false, { 0x00007F7Fu, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x14000000u } },
    { "w0=00007F7F, w15 hi=-10.0", SUB_01, false, { 0x00007F7Fu, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xEC000000u } },
    { "w0=00007F7F, w15 hi=+0.5", SUB_01, false, { 0x00007F7Fu, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01000000u } },
};
#define N_TESTS ((int)(sizeof(s_tests) / sizeof(s_tests[0])))

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

static void start_voice(const test_t *t) {
    volatile uint32_t *r = (volatile uint32_t *)apu_vp_debug_voice_record(H);
    uint8_t bins[APU_VP_OUTPUTS];
    uint16_t vols[APU_VP_OUTPUTS];
    uint32_t vbin, fb, vola, volb, volc, w;
    for (w = 0; w < APU_VP_OUTPUTS; w++) {
        bins[w] = (uint8_t)(w & 1u);
        vols[w] = 0xFFFu;
    }
    vols[0] = vols[1] = 0x080u;
    apu_vp_pack_bins(bins, &vbin, &fb);
    apu_vp_pack_volumes(vols, &vola, &volb, &volc);
    for (w = 0; w < 32u; w++) r[w] = 0;
    r[MCPX_VOICE_CFG_VBIN / 4] = vbin;
    r[MCPX_VOICE_CFG_FMT / 4] = apu_vp_format(1, 16, true) | fb;
    r[MCPX_VOICE_CFG_HRTF_TARGET / 4] = t->direct ? 0xFFFFu : 0u;
    r[MCPX_VOICE_CUR_PSL_START / 4] = s_pcm_ba;
    r[MCPX_VOICE_PAR_NEXT / 4] = FRAMES - 1u;
    r[MCPX_VOICE_TAR_VOLA / 4] = vola;
    r[MCPX_VOICE_TAR_VOLB / 4] = volb;
    r[MCPX_VOICE_TAR_VOLC / 4] = volc;
    r[MCPX_VOICE_TAR_PITCH_LINK / 4] = MCPX_APU_LIST_END;
    r[MCPX_VOICE_PAR_STATE / 4] = MCPX_PAR_STATE_ACTIVE_VOICE;
    REG(t->direct ? MCPX_APU_TVL2D : MCPX_APU_TVL3D) = H;
}

static void stop_voice(void) {
    REG(MCPX_APU_TVL2D) = MCPX_APU_LIST_END;
    REG(MCPX_APU_TVL3D) = MCPX_APU_LIST_END;
    Sleep(3);
}

int main(void) {
    int resume, k;
    uint32_t ht_phys = 0, hc_phys = 0;

    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    resume = read_progress();
    if (resume >= N_TESTS) {
        logf_("\n=== round 3: all %d tests already ran (delete %s to run them again)\n", N_TESTS, PROGRESS);
        save_log();
        for (;;) Sleep(1000);
    }
    if (resume < 0) {
        logf_("\n=== round 3: HRTF entry layout, %d tests (fresh start)\n", N_TESTS);
        resume = 0;
    } else {
        logf_("\n=== round 3: resumed; test %d (%s) did not finish (FROZE?)\n", resume, s_tests[resume].name);
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
    logf_("frames %s; columns: L peak / onset, R peak / onset, R-L onset difference (samples)\n",
          gp_delta(20) ? "running" : "STOPPED");
    save_log();

    for (k = resume; k < N_TESTS; k++) {
        const test_t *t = &s_tests[k];
        uint32_t n, i;
        int pl, ol, pr, orr;
        write_progress(k);
        memset(s_ht, 0, ARRAY_BYTES);
        memset(s_hc, 0, ARRAY_BYTES);
        for (i = 0; i < 16u; i++) s_ht[i] = t->w[i];
        REG(0x20ACu) = t->submix;
        start_voice(t);
        Sleep(60);   /* the current filter state follows the target */
        n = capture();
        stop_voice();
        analyse(n, 0, &pl, &ol);
        analyse(n, 1, &pr, &orr);
        logf_("%2d %-40s L %5d @%5d  R %5d @%5d  R-L %5d  (%lu frames)\n", k, t->name, pl, ol, pr, orr,
              (ol >= 0 && orr >= 0) ? orr - ol : 99999, (unsigned long)n);
        logf_("      current +80: %08lx %08lx %08lx %08lx  %08lx %08lx %08lx %08lx\n", (unsigned long)s_hc[32],
              (unsigned long)s_hc[33], (unsigned long)s_hc[34], (unsigned long)s_hc[35], (unsigned long)s_hc[36],
              (unsigned long)s_hc[37], (unsigned long)s_hc[38], (unsigned long)s_hc[39]);
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
