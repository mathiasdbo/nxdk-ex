/*
 * Console probe, three questions, safest first (the log is saved after every step):
 *
 *   B  AL_LOOPING on a playing voice: can its loop markers be rewritten live?
 *      B1 a new loop start (LBO; the VP writes the top byte of that dword back)
 *      B2 "release": loop -> play on to the end (EBO, then LBO, into the silent tail)
 *      B3 one-shot -> loop (LBO, then EBO, well ahead of the play position)
 *   C  HRTF without front-end methods: point the HRTF target/current address
 *      registers (0x2038/0x203C) at our own zeroed arrays and start voices
 *      below handle 64 with a CFG_HRTF_TARGET; does the voice advance, is
 *      anything mixed?
 *   D  Reset: freeze the APU (handle 129, XEMU_VERIFICATION.md 8.1b) unless C
 *      already did, then try to bring it back: deinit/init, the PCI command
 *      register, and a PCI power-management D3hot -> D0 cycle.
 *
 * The AC97 output is off (the probe reads the GP FIFO itself). Log: E:\apu_probe2.txt
 */
#include <hal/debug.h>
#include <hal/video.h>
#include <math.h>
#include <nxdk/mount.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include "apu_gp.h"
#include "apu_hardware.h"
#include "apu_mem.h"
#include "apu_vp.h"
#include "mcpx_apu_regs.h"

#define BAR0        ((uintptr_t)NV_PAPU_BASE)
#define REG(off)    (*(volatile uint32_t *)(BAR0 + (off)))
#define FRAMES      20000u
#define TAIL        (APU_VP_SILENT_TAIL_BYTES / 2u)

static char s_log[64 * 1024];
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

static void save_log(void) {
    FILE *f;
    if (!nxIsDriveMounted('E')) {
        nxMountDrive('E', "\\Device\\Harddisk0\\Partition1\\");
    }
    f = fopen("E:\\apu_probe2.txt", "wb");
    if (f) {
        fwrite(s_log, 1, s_log_len, f);
        fclose(f);
    }
}

static uint32_t s_pcm_phys;

static bool frames_run(void) {
    uint32_t g0 = apu_gp_frames(BAR0);
    Sleep(20);
    return apu_gp_frames(BAR0) != g0;
}

static volatile uint32_t *rec(uint32_t h) {
    return (volatile uint32_t *)apu_vp_debug_voice_record(h);
}

static uint32_t cbo(uint32_t h) {
    return rec(h)[MCPX_VOICE_PAR_OFFSET / 4] & 0xFFFFFFu;
}

static void params(apu_vp_voice_params_t *p, bool loop) {
    int i;
    memset(p, 0, sizeof(*p));
    p->phys = s_pcm_phys;
    p->bytes = FRAMES * 2u;
    p->channels = 1;
    p->bits = 16;
    p->loop = loop;
    p->pitch = 0;
    for (i = 0; i < APU_VP_OUTPUTS; i++) {
        p->bins[i] = (uint8_t)(i & 1);
        p->vols[i] = 0xFFFu;
    }
    p->vols[0] = p->vols[1] = 0x100u;
    p->hrtf_entry = -1;
    p->tail_bytes = APU_VP_SILENT_TAIL_BYTES;
}

/* Peak |sample| of the GP output (bins 0/1) over `ms` */
static void fifo_peak(uint32_t ms, int *pl, int *pr) {
    static int16_t buf[2 * 512];
    DWORD t0 = GetTickCount();
    int l = 0, r = 0;
    apu_gp_fifo_discard(BAR0);
    while (GetTickCount() - t0 < ms) {
        uint32_t n = apu_gp_fifo_read(BAR0, buf, 512u), i;
        for (i = 0; i < n; i++) {
            int a = buf[2 * i] < 0 ? -buf[2 * i] : buf[2 * i];
            int b = buf[2 * i + 1] < 0 ? -buf[2 * i + 1] : buf[2 * i + 1];
            if (a > l) l = a;
            if (b > r) r = b;
        }
        Sleep(2);
    }
    *pl = l;
    *pr = r;
}

/* Sample a voice's CBO for `ms`: min/max, and the smallest value seen right after a wrap */
static void watch(uint32_t h, uint32_t ms, uint32_t *mn, uint32_t *mx, uint32_t *after_wrap, uint32_t *wraps) {
    DWORD t0 = GetTickCount();
    uint32_t last = cbo(h);
    *mn = *mx = last;
    *after_wrap = 0xFFFFFFu;
    *wraps = 0;
    while (GetTickCount() - t0 < ms) {
        uint32_t c = cbo(h);
        if (c < *mn) *mn = c;
        if (c > *mx) *mx = c;
        if (c < last) {
            (*wraps)++;
            if (c < *after_wrap) *after_wrap = c;
        }
        last = c;
        Sleep(1);
    }
}

/* ---------------- B: live loop-marker writes ---------------- */

static void probe_looping(void) {
    apu_vp_voice_params_t p;
    uint32_t mn, mx, aw, wr;
    volatile uint32_t *r;

    logf_("-- B1: looping voice [1000, 9000), then LBO := 4000 while it plays\n");
    params(&p, true);
    p.loop_start = 1000;
    p.loop_end = 9000;
    apu_vp_voice_start(BAR0, 64, &p);
    Sleep(300);
    r = rec(64);
    watch(64, 400, &mn, &mx, &aw, &wr);
    logf_("   before: CBO %lu..%lu, after-wrap min %lu, %lu wraps; LBO dword %08lx EBO %08lx\n", (unsigned long)mn,
          (unsigned long)mx, (unsigned long)aw, (unsigned long)wr, (unsigned long)r[MCPX_VOICE_CUR_PSH_SAMPLE / 4],
          (unsigned long)r[MCPX_VOICE_PAR_NEXT / 4]);
    r[MCPX_VOICE_CUR_PSH_SAMPLE / 4] = (r[MCPX_VOICE_CUR_PSH_SAMPLE / 4] & 0xFF000000u) | 4000u;
    watch(64, 600, &mn, &mx, &aw, &wr);
    logf_("   after:  CBO %lu..%lu, after-wrap min %lu, %lu wraps; LBO dword %08lx (expect low 24 bits 4000=0fa0)\n",
          (unsigned long)mn, (unsigned long)mx, (unsigned long)aw, (unsigned long)wr,
          (unsigned long)r[MCPX_VOICE_CUR_PSH_SAMPLE / 4]);
    logf_("   frames %s\n", frames_run() ? "running" : "STOPPED");
    save_log();

    logf_("-- B2: release: EBO := end of the silent tail, then LBO := end of the data (play on, then the tail)\n");
    r[MCPX_VOICE_PAR_NEXT / 4] = FRAMES - 1u + TAIL;
    r[MCPX_VOICE_CUR_PSH_SAMPLE / 4] = (r[MCPX_VOICE_CUR_PSH_SAMPLE / 4] & 0xFF000000u) | FRAMES;
    watch(64, 700, &mn, &mx, &aw, &wr);
    logf_("   CBO %lu..%lu, after-wrap min %lu, %lu wraps, now %lu (expect it to reach %lu..%lu and stay)\n",
          (unsigned long)mn, (unsigned long)mx, (unsigned long)aw, (unsigned long)wr, (unsigned long)cbo(64),
          (unsigned long)FRAMES, (unsigned long)(FRAMES - 1u + TAIL));
    logf_("   frames %s\n", frames_run() ? "running" : "STOPPED");
    apu_vp_voice_off(BAR0, 64);
    save_log();
    if (!frames_run()) return;

    logf_("-- B3: one-shot, then LBO := 1000 and EBO := 8999 while CBO is still small (start looping)\n");
    params(&p, false);
    apu_vp_voice_start(BAR0, 65, &p);
    Sleep(20);
    r = rec(65);
    {
        uint32_t c = cbo(65);
        if (c + 3000u < 9000u) {
            r[MCPX_VOICE_CUR_PSH_SAMPLE / 4] = (r[MCPX_VOICE_CUR_PSH_SAMPLE / 4] & 0xFF000000u) | 1000u;
            r[MCPX_VOICE_PAR_NEXT / 4] = 8999u;
            logf_("   written at CBO %lu\n", (unsigned long)c);
        } else {
            logf_("   CBO already %lu: not written\n", (unsigned long)c);
        }
    }
    watch(65, 800, &mn, &mx, &aw, &wr);
    logf_("   CBO %lu..%lu, after-wrap min %lu, %lu wraps (expect it to stay in 1000..8999)\n", (unsigned long)mn,
          (unsigned long)mx, (unsigned long)aw, (unsigned long)wr);
    logf_("   frames %s\n", frames_run() ? "running" : "STOPPED");
    apu_vp_voice_off(BAR0, 65);
    save_log();
}

/* ---------------- C: HRTF through the address registers ---------------- */

static bool probe_hrtf(void) {
    static const struct {
        const char *tag;
        uint32_t handle, target;
        bool impulse;   /* target record 0: unit impulse at tap 0 for both ears */
    } v[] = {
        { "C1 handle 0, target 0xFFFF (baseline)", 0, 0xFFFFu, false },
        { "C2 handle 1, target 0, impulse filter", 1, 0, true },
        { "C3 handle 2, target 1, zero filter", 2, 1, false },
        { "C4 handle 3, target 2, impulse filter", 3, 2, true },
    };
    uint32_t ht_phys = 0, hc_phys = 0, old_ht = REG(0x2038), old_hc = REG(0x203C), k;
    uint32_t *ht = (uint32_t *)apu_mem_alloc_table(0x10000u, &ht_phys);
    uint32_t *hc = (uint32_t *)apu_mem_alloc_table(0x10000u, &hc_phys);
    if (!ht || !hc) {
        logf_("-- C: allocation failed\n");
        return true;
    }
    /* Guess: 64-byte records in the SET_HRIR method encoding (15 dwords of
     * L0 R0 L1 R1 ..., then L30 R30 ITD); record n at n * 64 */
    for (k = 0; k < 4u; k++) {
        if (k == 0 || k == 2) {
            ht[k * 16u] = 0x00007F7Fu;   /* tap 0: L = R = 127 */
        }
    }
    logf_("-- C: HRTF arrays: was 2038=%08lx 203c=%08lx, now ours %08lx / %08lx\n", (unsigned long)old_ht,
          (unsigned long)old_hc, (unsigned long)ht_phys, (unsigned long)hc_phys);
    REG(0x2038) = ht_phys;
    REG(0x203C) = hc_phys;
    for (k = 0; k < sizeof(v) / sizeof(v[0]); k++) {
        apu_vp_voice_params_t p;
        uint32_t c0, c1;
        int pl, pr;
        volatile uint32_t *r;
        params(&p, true);
        apu_vp_voice_start(BAR0, v[k].handle, &p);
        r = rec(v[k].handle);
        r[MCPX_VOICE_CFG_HRTF_TARGET / 4] = v[k].target;
        Sleep(50);
        c0 = cbo(v[k].handle);
        fifo_peak(150, &pl, &pr);
        c1 = cbo(v[k].handle);
        logf_("   %s: CBO %lu -> %lu (%s), GP peak L %d R %d, record 0x1C %08lx\n", v[k].tag, (unsigned long)c0,
              (unsigned long)c1, c1 != c0 ? "advances" : "STUCK", pl, pr, (unsigned long)r[0x1C / 4]);
        apu_vp_voice_off(BAR0, v[k].handle);
        if (!frames_run()) {
            logf_("   frames STOPPED after %s\n", v[k].tag);
            save_log();
            return false;
        }
        {
            uint32_t w, nz = 0;
            for (w = 0; w < 0x10000u / 4u; w++) nz += hc[w] ? 1u : 0u;
            logf_("      current array: %lu non-zero dwords (the VP writes its state there?)\n", (unsigned long)nz);
        }
        save_log();
    }
    REG(0x2038) = old_ht;
    REG(0x203C) = old_hc;
    return true;
}

/* ---------------- D: freeze, then reset ---------------- */

static uint32_t pci_rd(uint32_t off) {
    uint32_t v = 0;
    HalReadWritePCISpace(MCPX_APU_PCI_BUS, MCPX_APU_PCI_SLOT, off, &v, 4, FALSE);
    return v;
}

static void pci_wr(uint32_t off, uint32_t v) {
    HalReadWritePCISpace(MCPX_APU_PCI_BUS, MCPX_APU_PCI_SLOT, off, &v, 4, TRUE);
}

static bool reinit_and_check(const char *tag) {
    bool ok;
    apu_vp_deinit(BAR0);
    Sleep(10);
    if (apu_vp_init(BAR0) != 0) {
        logf_("   %s: apu_vp_init failed\n", tag);
        save_log();
        return false;
    }
    Sleep(30);
    ok = frames_run();
    logf_("   %s: GP program %s, frames %s, SECTL %08lx FECTL %08lx\n", tag,
          apu_gp_program_loaded() ? "loaded" : "NOT LOADED", ok ? "RUNNING AGAIN" : "still stopped",
          (unsigned long)REG(MCPX_APU_SECTL), (unsigned long)REG(MCPX_APU_FECTL));
    save_log();
    return ok;
}

static void probe_reset(bool frozen) {
    uint32_t i, cap = 0, cmd, bar0;

    logf_("-- D: PCI config (non-zero dwords):");
    for (i = 0; i < 0x100u; i += 4u) {
        uint32_t v = pci_rd(i);
        if (v) logf_(" %02lx=%08lx", (unsigned long)i, (unsigned long)v);
    }
    logf_("\n");
    if (pci_rd(0x04) & (1u << 20)) {   /* status: capability list */
        uint32_t p = pci_rd(0x34) & 0xFCu, guard = 0;
        while (p && guard++ < 16u) {
            uint32_t h = pci_rd(p);
            logf_("   capability %02lx at %02lx: %08lx\n", (unsigned long)(h & 0xFFu), (unsigned long)p, (unsigned long)h);
            if ((h & 0xFFu) == 0x01u) cap = p;   /* power management */
            p = (h >> 8) & 0xFCu;
        }
    } else {
        logf_("   no capability list\n");
    }
    save_log();

    if (!frozen) {
        logf_("-- D0: freeze the APU (a voice on handle 129)\n");
        {
            apu_vp_voice_params_t p;
            volatile uint32_t *r;
            uint32_t w;
            params(&p, true);
            apu_vp_voice_start(BAR0, 70, &p);   /* a valid record to copy */
            Sleep(20);
            r = rec(129);
            for (w = 0; w < 32u; w++) r[w] = rec(70)[w];
            r[MCPX_VOICE_TAR_PITCH_LINK / 4] = (r[MCPX_VOICE_TAR_PITCH_LINK / 4] & 0xFFFF0000u) | (REG(MCPX_APU_TVL2D) & 0xFFFFu);
            r[MCPX_VOICE_PAR_STATE / 4] = MCPX_PAR_STATE_ACTIVE_VOICE;
            REG(MCPX_APU_TVL2D) = 129;
            Sleep(50);
        }
        logf_("   frames %s, FECTL %08lx FEDECMETH %08lx FEDECPARAM %08lx\n", frames_run() ? "running (did not freeze)" : "STOPPED",
              (unsigned long)REG(MCPX_APU_FECTL), (unsigned long)REG(MCPX_APU_FEDECMETH),
              (unsigned long)REG(MCPX_APU_FEDECPARAM));
        save_log();
    }

    logf_("-- D1: deinit + init only (failed in earlier rounds)\n");
    if (reinit_and_check("D1")) return;

    logf_("-- D2: PCI command: memory and bus master off for 20 ms, then on\n");
    cmd = pci_rd(0x04);
    pci_wr(0x04, cmd & ~0x6u);
    Sleep(20);
    pci_wr(0x04, cmd);
    if (reinit_and_check("D2")) return;

    if (cap) {
        uint32_t pmcsr = pci_rd(cap + 4u);
        logf_("-- D3: power management D3hot -> D0 (PMCSR %08lx)\n", (unsigned long)pmcsr);
        bar0 = pci_rd(0x10);
        cmd = pci_rd(0x04);
        save_log();
        pci_wr(cap + 4u, (pmcsr & ~3u) | 3u);
        Sleep(20);
        logf_("   in D3: PMCSR %08lx\n", (unsigned long)pci_rd(cap + 4u));
        pci_wr(cap + 4u, pmcsr & ~3u);
        Sleep(20);
        logf_("   back in D0: PMCSR %08lx, BAR0 %08lx (was %08lx), command %08lx (was %08lx)\n",
              (unsigned long)pci_rd(cap + 4u), (unsigned long)pci_rd(0x10), (unsigned long)bar0,
              (unsigned long)pci_rd(0x04), (unsigned long)cmd);
        pci_wr(0x10, bar0);
        pci_wr(0x04, cmd);
        if (reinit_and_check("D3")) return;
    } else {
        logf_("-- D3: no power-management capability: skipped\n");
    }
    logf_("   no reset brought the frames back\n");
    save_log();
}

int main(void) {
    bool alive;
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    logf_("apu_probe2: live loop markers, HRTF via 0x2038/0x203C, APU reset\n");
    {
        uint32_t i, ph;
        int16_t *pcm = (int16_t *)apu_mem_alloc_table(FRAMES * 2u + APU_VP_SILENT_TAIL_BYTES, &ph);
        for (i = 0; i < FRAMES; i++) pcm[i] = (int16_t)(6000.0f * sinf(6.2831853f * 440.0f * (float)i / 48000.0f));
        s_pcm_phys = ph;
    }
    apu_vp_debug_set_flags(APU_VP_DBG_NO_AC97);
    if (apu_vp_init(BAR0) != 0) {
        logf_("apu_vp_init failed\n");
        save_log();
        for (;;) Sleep(1000);
    }
    Sleep(50);
    logf_("init: frames %s, front end %s\n", frames_run() ? "running" : "STOPPED",
          apu_vp_hrtf_available() ? "works" : "stalled (console)");
    save_log();

    probe_looping();
    alive = frames_run();
    if (alive) {
        alive = probe_hrtf();
    }
    probe_reset(!alive);
    logf_("done\n");
    save_log();
    for (;;) Sleep(1000);
    return 0;
}
