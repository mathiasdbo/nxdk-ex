/*
 * MCPX APU register probe for real hardware (round 27, batched; rounds 1-2 notes below).
 *
 * Round 1 (E:\apu_probe.txt from a console) showed:
 *   - method writes land in the front end's PIO queue (BAR0 0x1400-0x14FF,
 *     32 entries of {0x000A0000 | method, value}), but after the first one
 *     the front end stops decoding them: PIO_FREE falls 0x80 -> 0 and never
 *     drains, FEDECMETH/FECV stay at the first method;
 *   - the console left SECTL = 0x00000007 and FECTL = 0x0007138F, and the
 *     stage-1 bring-up overwrote them with 0x08 / 0 (clearing bits xemu
 *     does not model);
 *   - XCNTMODE 1 (SECTL 0x08) runs the sample counter, 2 and 3 do not.
 *
 * Round 2 keeps those bits. For each SECTL/FECTL variant it checks whether
 * the queued methods drain and a new method is decoded; the first variant that
 * works is used for a full voice start (CBO, ACTIVE, list link). The log goes
 * to E:\apu_probe27.txt and the screen.
 */
#include <hal/debug.h>
#include <hal/video.h>
#include <nxdk/mount.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include "mcpx_apu_regs.h"

#define PIO_FREE     0x010u
#define VOICE        64u
#define LOG_BYTES    (64u * 1024u)

static volatile uint8_t *s_bar0;
static uint32_t s_bar0_phys;
static char s_log[LOG_BYTES];
static size_t s_log_len;

static uint8_t *s_voices; static uint32_t s_voices_phys;
static uint32_t *s_sge;   static uint32_t s_sge_phys;
static uint8_t *s_ssl;    static uint32_t s_ssl_phys;
static uint8_t *s_notify; static uint32_t s_notify_phys;
static int16_t *s_pcm;    static uint32_t s_pcm_phys;

static void logf_(const char *fmt, ...) {
    va_list ap;
    char line[256];
    int n;
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    debugPrint("%s", line);
    if (s_log_len + (size_t)n < LOG_BYTES) {
        memcpy(s_log + s_log_len, line, (size_t)n);
        s_log_len += (size_t)n;
    }
}

static void save_log(void) {
    FILE *f;
    if (!nxIsDriveMounted('E')) {
        nxMountDrive('E', "\\Device\\Harddisk0\\Partition1\\");
    }
    f = fopen("E:\\apu_probe27.txt", "wb");
    if (f) {
        fwrite(s_log, 1, s_log_len, f);
        fclose(f);
        debugPrint("[log saved to E:\\apu_probe27.txt]\n");
    } else {
        debugPrint("[could not write E:\\apu_probe27.txt]\n");
    }
}

static inline uint32_t rd(uint32_t off) { return *(volatile uint32_t *)(s_bar0 + off); }
static inline void wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(s_bar0 + off) = v; }
static inline void meth(uint32_t m, uint32_t v) { wr(MCPX_APU_METHOD_BASE + m, v); }
static inline uint32_t vdw(uint32_t h, uint32_t off) { return *(volatile uint32_t *)(s_voices + h * 0x80u + off); }
static inline uint32_t pio_free(void) { return rd(MCPX_APU_METHOD_BASE + PIO_FREE); }

static void *alloc_phys(uint32_t size, uint32_t *phys) {
    void *p = MmAllocateContiguousMemoryEx(size, 0, 0x03FFAFFF, 0x4000, PAGE_READWRITE | PAGE_NOCACHE);
    if (p) {
        memset(p, 0, size);
        *phys = (uint32_t)MmGetPhysicalAddress(p);
    }
    return p;
}

static void fe_state(const char *tag) {
    logf_("  %s: FECTL %08lx SECTL %08lx FECV %04lx FEDEC %03lx/%08lx PIO_FREE %02lx FE[1338] %08lx [133c] %08lx [1340] %08lx ISTS %08lx\n",
          tag, rd(MCPX_APU_FECTL), rd(MCPX_APU_SECTL), rd(MCPX_APU_FECV) & 0xFFFFu, rd(MCPX_APU_FEDECMETH),
          rd(MCPX_APU_FEDECPARAM), pio_free() & 0xFFu, rd(0x1338u), rd(0x133Cu), rd(0x1340u), rd(MCPX_APU_ISTS));
}

static void xgscnt_rate(const char *tag) {
    uint32_t a = rd(MCPX_APU_XGSCNT);
    Sleep(100);
    logf_("  XGSCNT %s: %08lx -> %08lx in 100 ms (delta %lu)\n", tag, a, rd(MCPX_APU_XGSCNT),
          (unsigned long)(rd(MCPX_APU_XGSCNT) - a));
}

/* Does the front end drain its queue and decode a new method under the current settings? */
static bool method_test(uint32_t probe_voice) {
    uint32_t before = pio_free() & 0xFFu;
    DWORD t0 = GetTickCount();
    meth(MCPX_METH_SET_CURRENT_VOICE, probe_voice);
    while (GetTickCount() - t0 < 50u) {
        if ((rd(MCPX_APU_FECV) & 0xFFFFu) == probe_voice && (pio_free() & 0xFFu) >= 0x7Cu) break;
    }
    logf_("  method SET_CURRENT_VOICE(%lu): PIO_FREE %02lx -> %02lx, FECV %04lx, FEDEC %03lx/%08lx after %lu ms\n",
          (unsigned long)probe_voice, before, pio_free() & 0xFFu, rd(MCPX_APU_FECV) & 0xFFFFu,
          rd(MCPX_APU_FEDECMETH), rd(MCPX_APU_FEDECPARAM), (unsigned long)(GetTickCount() - t0));
    return (rd(MCPX_APU_FECV) & 0xFFFFu) == probe_voice;
}

static void voice_state(const char *tag) {
    logf_("    %s: TVL2D %04lx CVL2D %04lx  rec STATE %08lx CBO %08lx EBO %08lx LINK %08lx FMT %08lx\n",
          tag, rd(MCPX_APU_TVL2D) & 0xFFFFu, rd(0x2058u) & 0xFFFFu, vdw(VOICE, 0x54u), vdw(VOICE, 0x58u),
          vdw(VOICE, 0x5Cu), vdw(VOICE, 0x7Cu), vdw(VOICE, 0x04u));
}

static void start_voice_and_watch(void) {
    meth(MCPX_METH_SET_CURRENT_VOICE, VOICE);
    meth(MCPX_METH_CFG_VBIN, 0x20u);
    meth(MCPX_METH_CFG_FMT, MCPX_FMT_SAMPLE_S16 | MCPX_FMT_CONTAINER_B16 | MCPX_FMT_LOOP);
    meth(MCPX_METH_CFG_ENV0, 0);
    meth(MCPX_METH_CFG_ENVA, 0);
    meth(MCPX_METH_CFG_ENV1, 0);
    meth(MCPX_METH_CFG_ENVF, 0);
    meth(MCPX_METH_CFG_MISC, 0);
    meth(MCPX_METH_TAR_HRTF, 0xFFFFu);
    meth(MCPX_METH_TAR_VOLA, 0x000F000Fu);
    meth(MCPX_METH_TAR_VOLB, 0xFFFFFFFFu);
    meth(MCPX_METH_TAR_VOLC, 0xFFFFFFFFu);
    meth(MCPX_METH_LFO_ENV, 0);
    meth(MCPX_METH_TAR_FCA, 0);
    meth(MCPX_METH_TAR_FCB, 0);
    meth(MCPX_METH_TAR_PITCH, 0);
    meth(MCPX_METH_CFG_BUF_BASE, s_pcm_phys & 0xFFFu);
    meth(MCPX_METH_CFG_BUF_LBO, 0);
    meth(MCPX_METH_CFG_BUF_EBO, 47999u);
    meth(MCPX_METH_SET_ANTECEDENT_VOICE, MCPX_ANTECEDENT_LIST_2D_TOP);
    meth(MCPX_METH_VOICE_ON, VOICE);
    fe_state("after VOICE_ON");
    voice_state("t=0");
    Sleep(10);
    voice_state("t=10ms");
    Sleep(90);
    voice_state("t=100ms");
    Sleep(400);
    voice_state("t=500ms");
    fe_state("t=500ms");
}

/* Front-end register blocks, for decoding new event codes */
static void fe_blocks(const char *tag) {
    uint32_t off;
    logf_("  %s FE regs:", tag);
    for (off = 0x1100u; off <= 0x1160u; off += 4u) {
        if (((off - 0x1100u) / 4u) % 8u == 0u) logf_("\n   [%04lx]", (unsigned long)off);
        logf_(" %08lx", rd(off));
    }
    for (off = 0x1300u; off <= 0x1344u; off += 4u) {
        if (((off - 0x1300u) / 4u) % 8u == 0u) logf_("\n   [%04lx]", (unsigned long)off);
        logf_(" %08lx", rd(off));
    }
    logf_("\n");
}

/* Try the ways a trap could be cleared; report FECTL after each */
static void clear_trap(void) {
    uint32_t f = rd(MCPX_APU_FECTL);
    logf_("  clear trap: FECTL %08lx ISTS %08lx", f, rd(MCPX_APU_ISTS));
    wr(MCPX_APU_FECTL, (f & ~0xE0u) | 0x8000u);        /* write bit 15 back (write-1-to-clear?) */
    logf_(" | w1c15 -> %08lx", rd(MCPX_APU_FECTL));
    if (rd(MCPX_APU_FECTL) & 0x8000u) {
        wr(MCPX_APU_FECTL, f & ~0x80E0u);              /* write bit 15 as 0 */
        logf_(" | w0 -> %08lx", rd(MCPX_APU_FECTL));
    }
    wr(MCPX_APU_ISTS, 0xFFFFFFFFu);
    logf_(" | ISTS cleared -> FECTL %08lx ISTS %08lx\n", rd(MCPX_APU_FECTL), rd(MCPX_APU_ISTS));
}

/* Queue pointers: [0x1340] byte2 = put, byte1 = get, byte0 = pending (round 3) */
static void queue_ptrs(const char *tag) {
    uint32_t q = rd(0x1340u);
    logf_("  %s: queue put %02lx get %02lx pending %lu, PIO_FREE %02lx, FEDEC %03lx/%08lx, FEMEMADDR %08lx\n", tag,
          (q >> 16) & 0xFFu, (q >> 8) & 0xFFu, q & 0xFFu, pio_free() & 0xFFu,
          rd(MCPX_APU_FEDECMETH), rd(MCPX_APU_FEDECPARAM), rd(0x1324u));
}

static void configure_voice_methods(void) {
    meth(MCPX_METH_SET_CURRENT_VOICE, VOICE);
    meth(MCPX_METH_CFG_VBIN, 0x20u);
    meth(MCPX_METH_CFG_FMT, MCPX_FMT_SAMPLE_S16 | MCPX_FMT_CONTAINER_B16 | MCPX_FMT_LOOP);
    meth(MCPX_METH_CFG_ENV0, 0);
    meth(MCPX_METH_CFG_ENVA, 0);
    meth(MCPX_METH_CFG_ENV1, 0);
    meth(MCPX_METH_CFG_ENVF, 0);
    meth(MCPX_METH_CFG_MISC, 0);
    meth(MCPX_METH_TAR_HRTF, 0xFFFFu);
    meth(MCPX_METH_TAR_VOLA, 0x000F000Fu);
    meth(MCPX_METH_TAR_VOLB, 0xFFFFFFFFu);
    meth(MCPX_METH_TAR_VOLC, 0xFFFFFFFFu);
    meth(MCPX_METH_LFO_ENV, 0);
    meth(MCPX_METH_TAR_FCA, 0);
    meth(MCPX_METH_TAR_FCB, 0);
    meth(MCPX_METH_TAR_PITCH, 0);
    meth(MCPX_METH_CFG_BUF_BASE, s_pcm_phys & 0xFFFu);
    meth(MCPX_METH_CFG_BUF_LBO, 0);
    meth(MCPX_METH_CFG_BUF_EBO, 47999u);
}

/* Minimal DSP image: end every frame (movep #1,x:$FFFFC4), loop (jmp $0) */
static const uint32_t s_dsp_halt_loop[3] = { 0x08F484u, 0x000001u, 0x0C0000u };

static uint32_t *s_gp_scratch, *s_gp_sge, *s_ep_scratch, *s_ep_sge;
static uint32_t s_gp_scratch_phys, s_gp_sge_phys, s_ep_scratch_phys, s_ep_sge_phys;

static void load_dsp(bool gp) {
    uint32_t **scratch = gp ? &s_gp_scratch : &s_ep_scratch, **sge = gp ? &s_gp_sge : &s_ep_sge;
    uint32_t *sp = gp ? &s_gp_scratch_phys : &s_ep_scratch_phys, *gp_phys = gp ? &s_gp_sge_phys : &s_ep_sge_phys;
    uint32_t i;
    if (!*scratch) {
        *scratch = (uint32_t *)alloc_phys(0x2000u, sp);
        *sge = (uint32_t *)alloc_phys(4096u, gp_phys);
    }
    for (i = 0; i < 3u; i++) (*scratch)[i] = s_dsp_halt_loop[i];
    (*sge)[0] = *sp;
    (*sge)[2] = *sp + 4096u;
    if (gp) {
        wr(MCPX_APU_GPRST, 0);
        wr(MCPX_APU_GPSADDR, *gp_phys);
        wr(MCPX_APU_GPSMAXSGE, 1u);
        wr(MCPX_APU_GPRST, 3u);
    } else {
        wr(MCPX_APU_EPRST, 0);
        wr(0x2048u, *gp_phys);      /* EPSADDR */
        wr(0x20DCu, 1u);            /* EPSMAXSGE */
        wr(MCPX_APU_EPRST, 3u);
    }
    logf_("  %s loaded: RST %08lx, P[0..2] %06lx %06lx %06lx\n", gp ? "GP" : "EP",
          rd(gp ? MCPX_APU_GPRST : MCPX_APU_EPRST),
          rd((gp ? 0x3A000u : 0x5A000u) + 0u), rd((gp ? 0x3A000u : 0x5A000u) + 4u), rd((gp ? 0x3A000u : 0x5A000u) + 8u));
}

static void watch_voice(const char *tag, int steps) {
    int s;
    for (s = 0; s < steps; s++) {
        Sleep(100);
        logf_("    %s +%d00ms: TVL2D %04lx CVL2D %04lx STATE %08lx CBO %06lx XGSCNT %08lx q %08lx\n", tag, s + 1,
              rd(MCPX_APU_TVL2D) & 0xFFFFu, rd(0x2058u) & 0xFFFFu, vdw(VOICE, 0x54u), vdw(VOICE, 0x58u) & 0xFFFFFFu,
              rd(MCPX_APU_XGSCNT), rd(0x1340u));
    }
}

static void fe_line(const char *tag) {
    uint32_t q = rd(0x1340u);
    logf_("  %s: FECTL %08lx queue put %02lx get %02lx pending %lu PIO_FREE %02lx FEDEC %04lx/%08lx [133c] %08lx ISTS %08lx\n",
          tag, rd(MCPX_APU_FECTL), (q >> 16) & 0xFFu, (q >> 8) & 0xFFu, q & 0xFFu, pio_free() & 0xFFu,
          rd(MCPX_APU_FEDECMETH), rd(MCPX_APU_FEDECPARAM), rd(0x133Cu), rd(MCPX_APU_ISTS));
}

static uint32_t pending(void) { return rd(0x1340u) & 0xFFu; }

/* Send one method; report whether the front end consumed it */
static bool step(const char *name, uint32_t m, uint32_t v) {
    uint32_t q0 = rd(0x1340u);
    meth(m, v);
    Sleep(10);
    logf_("  %-28s %03lx=%08lx: get %02lx->%02lx pending %lu FEDEC %08lx/%08lx FECTL %08lx\n", name,
          (unsigned long)m, (unsigned long)v, (q0 >> 8) & 0xFFu, (rd(0x1340u) >> 8) & 0xFFu, pending(),
          rd(MCPX_APU_FEDECMETH), rd(MCPX_APU_FEDECPARAM), rd(MCPX_APU_FECTL));
    return pending() == 0u;
}

/*
 * Load our halt-each-frame loop through the bootstrap (round 7 finding: on
 * hardware, releasing the DSP to RST 3 copies P:0..$7FF from scratch memory
 * through the scratch SGE table, as on xemu). The scratch image and its SGE
 * table are ours and 16 KiB aligned, so nothing else can be loaded.
 */
static uint32_t *s_dsp_image[2], *s_dsp_sge[2];
static uint32_t s_dsp_image_phys[2], s_dsp_sge_phys[2];

static bool load_dsp_bootstrap(int which) {          /* 0 = GP, 1 = EP */
    uint32_t rst = which ? MCPX_APU_EPRST : MCPX_APU_GPRST;
    uint32_t sadr = which ? 0x2048u : MCPX_APU_GPSADDR;      /* EPSADDR / GPSADDR */
    uint32_t smax = which ? 0x20DCu : MCPX_APU_GPSMAXSGE;    /* EPSMAXSGE / GPSMAXSGE */
    uint32_t pmem = which ? 0x5A000u : 0x3A000u;
    uint32_t i;
    bool ok = true;

    if (!s_dsp_image[which]) {
        s_dsp_image[which] = (uint32_t *)alloc_phys(0x4000u, &s_dsp_image_phys[which]);   /* 0x800 words = 8 KiB used */
        s_dsp_sge[which] = (uint32_t *)alloc_phys(0x4000u, &s_dsp_sge_phys[which]);
    }
    memset(s_dsp_image[which], 0, 0x4000u);
    for (i = 0; i < 3u; i++) {
        s_dsp_image[which][i] = s_dsp_halt_loop[i];
    }
    s_dsp_sge[which][0] = s_dsp_image_phys[which];
    s_dsp_sge[which][2] = s_dsp_image_phys[which] + 4096u;

    wr(rst, 0u);
    wr(sadr, s_dsp_sge_phys[which]);
    wr(smax, 1u);
    logf_("  %s: scratch %08lx sge %08lx, %sSADDR readback %08lx MAXSGE %08lx\n", which ? "EP" : "GP",
          s_dsp_image_phys[which], s_dsp_sge_phys[which], which ? "EP" : "GP", rd(sadr), rd(smax));
    wr(rst, 3u);
    Sleep(5);
    logf_("  %s released: RST %08lx P[0..3] %06lx %06lx %06lx %06lx\n", which ? "EP" : "GP", rd(rst),
          rd(pmem), rd(pmem + 4u), rd(pmem + 8u), rd(pmem + 12u));
    for (i = 0; i < 3u; i++) {
        if ((rd(pmem + 4u * i) & 0xFFFFFFu) != s_dsp_halt_loop[i]) ok = false;
    }
    if (!ok) {
        /* Not our program: hold it in reset rather than run anything else */
        wr(rst, 0u);
        logf_("  %s: program memory is not ours, held in reset\n", which ? "EP" : "GP");
    }
    return ok;
}

/* Frame-counting DSP program: X:$10 = frames completed (proves the frame handshake) */
static const uint32_t s_dsp_counter[9] = {
    0x60F400u, 0x000000u,   /* move #0,r0 */
    0x000008u,              /* inc a                       (P:2, loop) */
    0x0A7088u, 0x000010u,   /* move a0,x:(r0+$10) */
    0x08F484u, 0x000001u,   /* movep #1,x:$FFFFC4 (end of frame) */
    0x0C0002u,              /* jmp $2 */
    0x000000u,
};

static bool load_counter(int which) {
    uint32_t rst = which ? MCPX_APU_EPRST : MCPX_APU_GPRST;
    uint32_t sadr = which ? 0x2048u : MCPX_APU_GPSADDR;
    uint32_t smax = which ? 0x20DCu : MCPX_APU_GPSMAXSGE;
    uint32_t pmem = which ? 0x5A000u : 0x3A000u;
    uint32_t i;
    bool ok = true;

    if (!s_dsp_image[which]) {
        s_dsp_image[which] = (uint32_t *)alloc_phys(0x4000u, &s_dsp_image_phys[which]);
        s_dsp_sge[which] = (uint32_t *)alloc_phys(0x4000u, &s_dsp_sge_phys[which]);
    }
    memset(s_dsp_image[which], 0, 0x4000u);
    for (i = 0; i < 9u; i++) s_dsp_image[which][i] = s_dsp_counter[i];
    s_dsp_sge[which][0] = s_dsp_image_phys[which];
    s_dsp_sge[which][2] = s_dsp_image_phys[which] + 4096u;
    wr(rst, 0u);
    wr(sadr, s_dsp_sge_phys[which]);
    wr(smax, 1u);
    wr(rst, 3u);
    Sleep(5);
    for (i = 0; i < 8u; i++) {
        if ((rd(pmem + 4u * i) & 0xFFFFFFu) != s_dsp_counter[i]) ok = false;
    }
    logf_("  %s: P[0..7] %06lx %06lx %06lx %06lx %06lx %06lx %06lx %06lx -> %s\n", which ? "EP" : "GP",
          rd(pmem), rd(pmem + 4u), rd(pmem + 8u), rd(pmem + 12u), rd(pmem + 16u), rd(pmem + 20u),
          rd(pmem + 24u), rd(pmem + 28u), ok ? "ours" : "NOT ours, held in reset");
    if (!ok) wr(rst, 0u);
    return ok;
}

static uint32_t gp_frames(void) { return rd(MCPX_APU_GP_XMEM + 0x40u); }
static uint32_t ep_frames(void) { return rd(0x50000u + 0x40u); }

/* Did the queue move since the last check? */
static uint32_t s_last_get;
static bool moved(const char *after) {
    uint32_t get = (rd(0x1340u) >> 8) & 0xFFu;
    bool m = get != s_last_get;
    logf_("    after %-34s get %02lx pending %lu FEDEC %08lx/%08lx FECTL %08lx ISTS %08lx %s\n", after,
          get, pending(), rd(MCPX_APU_FEDECMETH), rd(MCPX_APU_FEDECPARAM), rd(MCPX_APU_FECTL),
          rd(MCPX_APU_ISTS), m ? "<== QUEUE MOVED" : "");
    s_last_get = get;
    return m;
}

/* ---- round 11: front-end single-step kicks ---- */

static uint32_t q_get(void) { return (rd(0x1340u) >> 8) & 0xFFu; }

/* One FECTL write; how many queue entries did it consume? (ring of 32) */
static uint32_t kick_write(const char *name, uint32_t v) {
    uint32_t g0 = q_get(), adv;
    wr(MCPX_APU_FECTL, v);
    Sleep(5);
    adv = (q_get() - g0) & 31u;
    logf_("    %-22s wrote %08lx read %08lx: advanced %lu, pending %lu, FEDEC %03lx=%08lx ISTS %08lx\n", name, v,
          rd(MCPX_APU_FECTL), adv, pending(), rd(MCPX_APU_FEDECMETH) & 0xFFFu, rd(MCPX_APU_FEDECPARAM),
          rd(MCPX_APU_ISTS));
    return adv;
}

/* Known-good kick from round 10: TRAPPED then HALTED */
static void kick(void) {
    uint32_t f = rd(MCPX_APU_FECTL) & ~0x1FE0u;
    wr(MCPX_APU_FECTL, f | 0xE0u);
    wr(MCPX_APU_FECTL, f | 0x80u);
}

static uint32_t s_sent, s_kicks, s_stuck;

/* Push a method, then kick the front end until it has consumed it */
static void smeth(uint32_t m, uint32_t v) {
    int t;
    meth(m, v);
    s_sent++;
    for (t = 0; t < 6 && pending() != 0u; t++) {
        kick();
        s_kicks++;
    }
    if (pending() != 0u) {
        s_stuck++;
        logf_("    method %03lx=%08lx still pending after %d kicks (pending %lu)\n", m, v, t, pending());
    }
}

static const uint32_t s_voice_cfg[][2] = {
    { MCPX_METH_SET_CURRENT_VOICE, VOICE },
    { MCPX_METH_CFG_VBIN, 0x20u },
    { MCPX_METH_CFG_FMT, MCPX_FMT_SAMPLE_S16 | MCPX_FMT_CONTAINER_B16 | MCPX_FMT_LOOP },
    { MCPX_METH_CFG_ENV0, 0 }, { MCPX_METH_CFG_ENVA, 0 }, { MCPX_METH_CFG_ENV1, 0 }, { MCPX_METH_CFG_ENVF, 0 },
    { MCPX_METH_CFG_MISC, 0 },
    { MCPX_METH_TAR_HRTF, 0xFFFFu },
    { MCPX_METH_TAR_VOLA, 0x000F000Fu }, { MCPX_METH_TAR_VOLB, 0xFFFFFFFFu }, { MCPX_METH_TAR_VOLC, 0xFFFFFFFFu },
    { MCPX_METH_LFO_ENV, 0 }, { MCPX_METH_TAR_FCA, 0 }, { MCPX_METH_TAR_FCB, 0 }, { MCPX_METH_TAR_PITCH, 0 },
    { MCPX_METH_CFG_BUF_BASE, 0 },  /* filled in with the PCM offset */
    { MCPX_METH_CFG_BUF_LBO, 0 },
    { MCPX_METH_CFG_BUF_EBO, 47999u },
};

static void dump_record(const char *tag) {
    uint32_t i;
    logf_("  voice %u record (%s):", VOICE, tag);
    for (i = 0; i < 0x80u; i += 4u) logf_("%s%08lx", (i % 32u) ? " " : "\n    ", vdw(VOICE, i));
    logf_("\n");
}

/* ---- round 13: complete-then-wait DSP frame loop, then the front end in free-running mode ---- */

/*
 * Frame counter in the order measured on silicon (xemu PR 3047): a latched start stays invisible in
 * x:$FFFFC5 until the program writes frame complete (x:$FFFFC4 = 1), which releases exactly one. So:
 * signal complete, wait for START_FRAME (bit 1 of x:$FFFFC5), acknowledge it (write 2), count.
 *   jclr #n,x:pp,xxxx = 0000 1010 10pp pppp 1S0b bbbb (S = 0 for X), then the target
 */
static const uint32_t s_dsp_synced[12] = {
    0x60F400u, 0x000000u,   /* P:0  move #0,r0 */
    0x08F484u, 0x000001u,   /* P:2  movep #1,x:$FFFFC4      frame complete */
    0x0A8581u, 0x000004u,   /* P:4  jclr #1,x:$FFFFC5,$4   wait for START_FRAME */
    0x08F485u, 0x000002u,   /* P:6  movep #2,x:$FFFFC5      acknowledge it */
    0x000008u,              /* P:8  inc a */
    0x0A7088u, 0x000010u,   /* P:9  move a0,x:(r0+$10) */
    0x0C0002u,              /* P:B  jmp $2 */
};

static bool load_prog(int which, const uint32_t *prog, uint32_t n) {
    uint32_t rst = which ? MCPX_APU_EPRST : MCPX_APU_GPRST;
    uint32_t sadr = which ? 0x2048u : MCPX_APU_GPSADDR;
    uint32_t smax = which ? 0x20DCu : MCPX_APU_GPSMAXSGE;
    uint32_t pmem = which ? 0x5A000u : 0x3A000u;
    uint32_t i;
    bool ok = true;

    if (!s_dsp_image[which]) {
        s_dsp_image[which] = (uint32_t *)alloc_phys(0x4000u, &s_dsp_image_phys[which]);
        s_dsp_sge[which] = (uint32_t *)alloc_phys(0x4000u, &s_dsp_sge_phys[which]);
    }
    memset(s_dsp_image[which], 0, 0x4000u);
    for (i = 0; i < n; i++) s_dsp_image[which][i] = prog[i];
    s_dsp_sge[which][0] = s_dsp_image_phys[which];
    s_dsp_sge[which][2] = s_dsp_image_phys[which] + 4096u;
    wr(rst, 0u);
    wr(sadr, s_dsp_sge_phys[which]);
    wr(smax, 1u);
    wr(rst, 3u);
    Sleep(5);
    for (i = 0; i < n; i++) {
        if ((rd(pmem + 4u * i) & 0xFFFFFFu) != prog[i]) ok = false;
    }
    logf_("  %s: P[0..11] read back %s\n", which ? "EP" : "GP", ok ? "ours" : "NOT ours, held in reset");
    if (!ok) wr(rst, 0u);
    return ok;
}

static void count_frames(const char *tag) {
    uint32_t g0 = gp_frames(), e0 = ep_frames(), x0 = rd(MCPX_APU_XGSCNT);
    Sleep(100);
    logf_("  %-34s GP %lu EP %lu frames, XGSCNT +%lu in 100 ms\n", tag, (gp_frames() - g0) & 0xFFFFFFu,
          (ep_frames() - e0) & 0xFFFFFFu, rd(MCPX_APU_XGSCNT) - x0);
}

/* Push the voice setup without kicks; report how far the queue drains on its own */
static uint32_t push_free(const char *tag) {
    uint32_t i, g0 = q_get(), n = sizeof(s_voice_cfg) / sizeof(s_voice_cfg[0]);
    for (i = 0; i < n; i++) {
        uint32_t v = s_voice_cfg[i][1];
        if (s_voice_cfg[i][0] == MCPX_METH_CFG_BUF_BASE) v = s_pcm_phys & 0xFFFu;
        meth(s_voice_cfg[i][0], v);
    }
    Sleep(50);
    logf_("  %-34s %lu sent, %lu consumed, pending %lu, FECTL %08lx FEDEC %08lx=%08lx ISTS %08lx\n", tag, n,
          (q_get() - g0) & 31u, pending(), rd(MCPX_APU_FECTL), rd(MCPX_APU_FEDECMETH),
          rd(MCPX_APU_FEDECPARAM), rd(MCPX_APU_ISTS));
    return pending();
}

static bool record_written(void) {
    uint32_t i;
    for (i = 0; i < 0x80u; i += 4u) {
        if (vdw(VOICE, i)) return true;
    }
    return false;
}


/* ---- round 14: FE register diffs around the stall, FECTL bit matrix with frames running ---- */

#define SNAP_FIRST 0x1000u
#define SNAP_WORDS ((0x1600u - SNAP_FIRST) / 4u)
static uint32_t s_snap[3][SNAP_WORDS];

static void snap(int k) {
    uint32_t i;
    for (i = 0; i < SNAP_WORDS; i++) {
        uint32_t off = SNAP_FIRST + 4u * i;
        s_snap[k][i] = (off >= 0x1400u && off < 0x1500u) ? 0u : rd(off);   /* skip the queue mirror */
    }
}

static void snap_diff(const char *tag, int a, int b) {
    uint32_t i, n = 0;
    logf_("  changed registers %s:", tag);
    for (i = 0; i < SNAP_WORDS; i++) {
        if (s_snap[a][i] != s_snap[b][i]) {
            logf_("%s%04lx %08lx->%08lx", (n % 3u) ? "  " : "\n    ", SNAP_FIRST + 4u * i, s_snap[a][i],
                  s_snap[b][i]);
            n++;
        }
    }
    logf_("%s\n", n ? "" : " none");
}

static void snap_dump(int k) {
    uint32_t i, n = 0;
    logf_("  non-zero registers 1000-15fc (queue mirror skipped):");
    for (i = 0; i < SNAP_WORDS; i++) {
        if (s_snap[k][i]) {
            logf_("%s%04lx=%08lx", (n % 5u) ? " " : "\n    ", SNAP_FIRST + 4u * i, s_snap[k][i]);
            n++;
        }
    }
    logf_("\n");
}

static uint32_t try_fectl(const char *tag, uint32_t v) {
    uint32_t g0 = q_get(), adv;
    wr(MCPX_APU_FECTL, v);
    Sleep(15);
    adv = (q_get() - g0) & 31u;
    logf_("    %-26s wrote %08lx read %08lx: consumed %lu pending %lu FEDEC %08lx=%08lx%s\n", tag, v,
          rd(MCPX_APU_FECTL), adv, pending(), rd(MCPX_APU_FEDECMETH), rd(MCPX_APU_FEDECPARAM),
          adv ? "  <==" : "");
    return adv;
}


/* ---- round 15: what the FE does through FEMEMADDR ---- */

#define MAGIC_WORDS 64u
#define FILL        0xA5A5A5A5u
static uint32_t *s_magic;
static uint32_t s_magic_phys;

static void magic_fill(uint32_t v) {
    uint32_t i;
    for (i = 0; i < MAGIC_WORDS; i++) s_magic[i] = v;
}

/* Print the words that differ from the fill value */
static void magic_dump(const char *tag, uint32_t fill) {
    uint32_t i, n = 0;
    logf_("  %-30s FEMEMADDR %08lx (+%03lx), changed words:", tag, rd(0x1324u), rd(0x1324u) - s_magic_phys);
    for (i = 0; i < MAGIC_WORDS; i++) {
        if (s_magic[i] != fill) {
            logf_("%s[%02lx]=%08lx", (n % 4u) ? " " : "\n    ", 4u * i, s_magic[i]);
            n++;
        }
    }
    logf_("%s\n", n ? "" : " none");
}


/* ---- round 16: bypass the front end, write the voice record and the list head directly ---- */

static volatile uint32_t *vrec(uint32_t h) { return (volatile uint32_t *)(s_voices + h * 0x80u); }

/* What SET_CURRENT_VOICE + the CFG/TAR methods + VOICE_ON would leave in the record (xemu vp.c) */
static void build_record(uint32_t h, uint32_t state) {
    volatile uint32_t *r = vrec(h);
    uint32_t i;
    for (i = 0; i < 32u; i++) r[i] = 0;
    r[0x00 / 4] = 0u | (1u << 5);                                   /* VBIN: out0 -> bin 0, out1 -> bin 1 */
    r[0x04 / 4] = (1u << 28) | (1u << 30) | (1u << 25);             /* S16 in B16 containers, loop */
    r[0x1C / 4] = 0xFFFFu;                                          /* no HRTF target */
    r[0x20 / 4] = s_pcm_phys & 0xFFFu;                              /* BA: offset in the SGE-linear space */
    r[0x24 / 4] = 0u;                                               /* LBO */
    r[0x58 / 4] = 0u;                                               /* CBO */
    r[0x5C / 4] = 47999u;                                           /* EBO */
    r[0x60 / 4] = 0x000F000Fu;                                      /* vol0 = vol1 = 0 dB, vol6/7 low nibble */
    r[0x64 / 4] = 0xFFFFFFFFu;                                      /* vol2/3 mute */
    r[0x68 / 4] = 0xFFFFFFFFu;                                      /* vol4/5 mute */
    r[0x7C / 4] = 0xFFFFu;                                          /* pitch 0 (x1), end of list */
    r[0x54 / 4] = state;                                            /* PAR_STATE last */
}

static void watch_direct(const char *tag, int steps) {
    int s;
    uint32_t c0 = vrec(VOICE)[0x58 / 4] & 0xFFFFFFu;
    for (s = 0; s < steps; s++) {
        Sleep(100);
        logf_("    %s +%d00ms: CBO %06lx STATE %08lx TVL2D %04lx CVL2D %04lx NVL2D %04lx FECTL %08lx ISTS %08lx"
              " FEDEC %08lx=%08lx q %08lx\n", tag, s + 1, vrec(VOICE)[0x58 / 4] & 0xFFFFFFu, vrec(VOICE)[0x54 / 4],
              rd(MCPX_APU_TVL2D) & 0xFFFFu, rd(0x2058u) & 0xFFFFu, rd(0x205Cu) & 0xFFFFu, rd(MCPX_APU_FECTL),
              rd(MCPX_APU_ISTS), rd(MCPX_APU_FEDECMETH), rd(MCPX_APU_FEDECPARAM), rd(0x1340u));
    }
    logf_("  %s: CBO moved %s (%06lx -> %06lx)\n", tag, ((vrec(VOICE)[0x58 / 4] & 0xFFFFFFu) != c0) ? "YES" : "no",
          c0, vrec(VOICE)[0x58 / 4] & 0xFFFFFFu);
}

static void dump_rec(const char *tag) {
    uint32_t i;
    logf_("  voice %u record (%s):", VOICE, tag);
    for (i = 0; i < 32u; i++) logf_("%s%08lx", (i % 8u) ? " " : "\n    ", vrec(VOICE)[i]);
    logf_("\n");
}


/* ---- round 17: VP voice -> mixbins -> GP DMA -> output FIFO 0 -> CPU -> AC97, plus a WAV capture ---- */

#include <hal/audio.h>

/*
 * GP frame loop: frame complete, wait for START_FRAME, acknowledge, start the DMA described at X:0
 * (mixbins 0 and 1 -> FIFO 0, interleaved 16-bit), count frames at X:$10.
 */
static const uint32_t s_gp_out[14] = {
    0x08F484u, 0x000001u,   /* P:0  movep #1,x:$FFFFC4      frame complete */
    0x0A8581u, 0x000002u,   /* P:2  jclr #1,x:$FFFFC5,$2   wait for START_FRAME */
    0x08F485u, 0x000002u,   /* P:4  movep #2,x:$FFFFC5      acknowledge */
    0x08F494u, 0x000000u,   /* P:6  movep #0,x:$FFFFD4      DMA NEXT_BLOCK = X:0 */
    0x08F496u, 0x000001u,   /* P:8  movep #1,x:$FFFFD6      DMA CONTROL = start */
    0x000008u,              /* P:A  inc a */
    0x501000u,              /* P:B  move a0,x:$10 */
    0x0C0000u,              /* P:D  jmp $0 */
};

static const uint32_t s_gp_desc[7] = {
    0x004000u,                          /* next: end of list */
    0x000001u | 0x000002u | (1u << 10), /* interleave, to memory, FIFO 0, 16-bit */
    (32u << 4) | 1u,                    /* 32 frames, 2 channels */
    0x001400u,                          /* X:$1400 = mixbin 0, bin 1 follows */
    0u, 0u, 0u,
};

#define RING_BYTES   0x10000u
#define CAP_FRAMES   96000u                 /* 2 s */
#define CHUNK_FR     2048u                  /* 42.7 ms per AC97 buffer */
#define OUT_SLOTS    16u
#define OUT_AHEAD    6u

static uint8_t *s_ring;   static uint32_t s_ring_phys;
static uint32_t *s_ring_sge; static uint32_t s_ring_sge_phys;
static int16_t s_cap[CAP_FRAMES * 2u];
static uint32_t s_cap_n;
static int16_t *s_out[OUT_SLOTS];
static uint32_t s_out_next, s_fill, s_underruns, s_rd, s_resync;

static uint32_t ring_cur(void) { return (rd(MCPX_APU_GPOFCUR0) & 0xFFFFFFu) % RING_BYTES; }

static volatile uint8_t *ac97(void) {
    extern AC97_DEVICE ac97Device;
    return (volatile uint8_t *)ac97Device.mmio;
}

static void out_frame(int16_t l, int16_t r) {
    int16_t *c = s_out[s_out_next % OUT_SLOTS];
    c[2u * s_fill] = l;
    c[2u * s_fill + 1u] = r;
    if (++s_fill == CHUNK_FR) {
        volatile uint8_t *pb = ac97();
        uint32_t t = GetTickCount();
        /* never overrun the descriptor list: wait while OUT_AHEAD chunks are queued (at most 500 ms) */
        while (((pb[0x115] - pb[0x114]) & 31u) >= OUT_AHEAD && !(pb[0x116] & 1u) && GetTickCount() - t < 500u) {
        }
        XAudioProvideSamples((unsigned char *)c, (unsigned short)(CHUNK_FR * 4u), 0);
        s_out_next++;
        s_fill = 0;
        if (pb[0x116] & 1u) {
            s_underruns++;
            XAudioPlay();
        }
    }
}

/* Forward everything the GP wrote since the last call */
static uint32_t forward(void) {
    uint32_t cur = ring_cur(), n = 0;
    uint32_t avail = ((cur + RING_BYTES - s_rd) % RING_BYTES) / 4u;
    if (avail > (RING_BYTES / 4u) * 7u / 8u) {
        s_rd = (cur + RING_BYTES / 2u) % RING_BYTES;
        s_resync++;
        avail = ((cur + RING_BYTES - s_rd) % RING_BYTES) / 4u;
    }
    while (n < avail) {
        const int16_t *f = (const int16_t *)(s_ring + s_rd);
        if (s_cap_n < CAP_FRAMES) {
            s_cap[2u * s_cap_n] = f[0];
            s_cap[2u * s_cap_n + 1u] = f[1];
            s_cap_n++;
        }
        out_frame(f[0], f[1]);
        s_rd = (s_rd + 4u) % RING_BYTES;
        n++;
    }
    return n;
}

static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }

static void save_wav(void) {
    FILE *f = fopen("E:\\apu_capture.wav", "wb");
    uint32_t bytes = s_cap_n * 4u;
    if (!f) {
        logf_("  could not write E:\\apu_capture.wav\n");
        return;
    }
    fwrite("RIFF", 1, 4, f); put32(f, 36u + bytes); fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16u); put16(f, 1u); put16(f, 2u); put32(f, 48000u); put32(f, 48000u * 4u); put16(f, 4u); put16(f, 16u);
    fwrite("data", 1, 4, f); put32(f, bytes);
    fwrite(s_cap, 4, s_cap_n, f);
    fclose(f);
    logf_("  wrote E:\\apu_capture.wav (%lu frames)\n", s_cap_n);
}

static void analyse_capture(void) {
    uint32_t i, cross_l = 0, cross_r = 0, nz = 0;
    int32_t peak_l = 0, peak_r = 0;
    for (i = 0; i < s_cap_n; i++) {
        int32_t l = s_cap[2u * i], r = s_cap[2u * i + 1u];
        if (l || r) nz++;
        if (l > peak_l) peak_l = l;
        if (-l > peak_l) peak_l = -l;
        if (r > peak_r) peak_r = r;
        if (-r > peak_r) peak_r = -r;
        if (i && ((s_cap[2u * (i - 1u)] < 0) != (l < 0))) cross_l++;
        if (i && ((s_cap[2u * (i - 1u) + 1u] < 0) != (r < 0))) cross_r++;
    }
    logf_("  capture: %lu frames, %lu non-zero, peak L %ld R %ld, zero crossings L %lu R %lu"
          " (440 Hz -> ~%lu)\n", s_cap_n, nz, peak_l, peak_r, cross_l, cross_r, s_cap_n * 880u / 48000u);
    logf_("  first frames:");
    for (i = 0; i < 12u && i < s_cap_n; i++) logf_(" %d/%d", s_cap[2u * i], s_cap[2u * i + 1u]);
    logf_("\n");
}


/* ---- round 27: exact replay of the apu_vp_stress D1 sequence that froze the driver ---- */

#define PING_FRAMES  12000u
#define TAIL_FRAMES  128u
#define PING_OFF     0x20u            /* the ping starts 0x20 into its first page, as in the driver run */

static uint32_t r_rng = 0x1234567u;
static uint32_t r_rnd(void) { r_rng = r_rng * 1664525u + 1013904223u; return r_rng >> 8; }

/* the driver's bookkeeping, reimplemented */
static uint8_t r_listed[256];
static uint32_t r_retired_at[256];
static uint8_t r_retired[256];
static uint32_t r_alloc_next;
static uint32_t r_q[256], r_qh, r_qn;
static uint32_t r_ops;
static bool r_check_end = true;      /* scan for ended one-shots after every stop, as apu_vp_service() does */
static bool r_ended_by_age;          /* variant: only unlink ended voices from the tail */

static uint32_t r_alloc(void) {
    uint32_t now = rd(MCPX_APU_XGSCNT), k, h;
    for (k = 0; k < 192u; k++) {
        h = 64u + (r_alloc_next + k) % 192u;
        if (r_listed[h]) continue;
        if (r_retired[h] && now - r_retired_at[h] < 64u) continue;
        r_alloc_next = (h - 64u + 1u) % 192u;
        return h;
    }
    return 0xFFFFu;
}

static void r_unlink(uint32_t h) {
    uint32_t next = vrec(h)[0x7C / 4] & 0xFFFFu, cur, guard = 0;
    if (!r_listed[h]) return;
    r_listed[h] = 0;
    r_retired_at[h] = rd(MCPX_APU_XGSCNT);
    r_retired[h] = 1;
    cur = rd(MCPX_APU_TVL2D) & 0xFFFFu;
    if (cur == h) {
        wr(MCPX_APU_TVL2D, next);
        return;
    }
    while (cur != 0xFFFFu && cur < 256u && guard++ < 256u) {
        volatile uint32_t *link = &vrec(cur)[0x7C / 4];
        if ((*link & 0xFFFFu) == h) {
            *link = (*link & 0xFFFF0000u) | next;
            return;
        }
        cur = *link & 0xFFFFu;
    }
}

/* apu_vp_service(): unlink listed voices whose CBO passed the data, in handle order */
static void r_service(void) {
    uint32_t h;
    for (h = 0; h < 256u; h++) {
        if (r_listed[h] && (vrec(h)[0x58 / 4] & 0xFFFFFFu) >= PING_FRAMES) {
            r_unlink(h);
        }
    }
}

/* apu_vp_voice_start() for the ping, with the driver's packing */
static void r_start(uint32_t h, int16_t pitch, uint16_t v0, uint16_t v1) {
    volatile uint32_t *r = vrec(h);
    uint32_t i;
    r[0x54 / 4] = 0;
    for (i = 0; i < 32u; i++) r[i] = 0;
    r[0x00 / 4] = 0u | (1u << 5) | (0u << 10) | (1u << 16) | (0u << 21) | (1u << 26);   /* bins i & 1 */
    r[0x04 / 4] = (1u << 28) | (1u << 30) | (1u << 25) | (0u | (1u << 5));               /* S16 B16 loop, out6/7 bins */
    r[0x1C / 4] = 0xFFFFu;
    r[0x20 / 4] = PING_OFF;                                                              /* SGE entry 0 */
    r[0x24 / 4] = PING_FRAMES;
    r[0x58 / 4] = 0;
    r[0x5C / 4] = PING_FRAMES - 1u + TAIL_FRAMES;
    r[0x60 / 4] = ((uint32_t)v0 << 4) | ((uint32_t)v1 << 20) | 0xFu | (0xFu << 16);
    r[0x64 / 4] = 0xFFFFFFFFu;
    r[0x68 / 4] = 0xFFFFFFFFu;
    r[0x7C / 4] = ((uint32_t)(uint16_t)pitch << 16) | 0xFFFFu;
    r[0x54 / 4] = 1u << 21;
    {   /* link at the 2D head */
        uint32_t head = rd(MCPX_APU_TVL2D) & 0xFFFFu;
        r[0x7C / 4] = (r[0x7C / 4] & 0xFFFF0000u) | head;
        wr(MCPX_APU_TVL2D, h);
    }
    r_listed[h] = 1;
}

static void r_reset(void) {
    uint32_t h;
    wr(MCPX_APU_TVL2D, 0xFFFFu);
    for (h = 0; h < 256u; h++) r_listed[h] = 0;
    r_qh = r_qn = 0;
    Sleep(5);
}

/* D1: 3 starts per 16 ms tick, stop the oldest beyond 40 (each stop runs the service) */
static bool replay(const char *tag, uint32_t seed, uint32_t ms) {
    uint32_t t0 = GetTickCount(), ticks = 0, starts = 0, g_last = gp_frames(), x_last = rd(MCPX_APU_XGSCNT);
    r_rng = seed;
    r_reset();
    while (GetTickCount() - t0 < ms) {
        uint32_t k;
        for (k = 0; k < 3u; k++) {
            uint32_t h = r_alloc();
            int16_t pitch;
            uint16_t v0, v1;
            if (h == 0xFFFFu) break;
            pitch = (int16_t)((int32_t)(r_rnd() % 8192u) - 4096);
            v0 = (uint16_t)(0x600u + r_rnd() % 0x300u);
            v1 = (uint16_t)(0x600u + r_rnd() % 0x300u);
            r_start(h, pitch, v0, v1);
            r_q[(r_qh + r_qn) % 256u] = h;
            r_qn++;
            starts++;
        }
        while (r_qn > 40u) {
            uint32_t h = r_q[r_qh];
            r_qh = (r_qh + 1u) % 256u;
            r_qn--;
            r_unlink(h);
            if (r_check_end) r_service();
        }
        Sleep(16);
        ticks++;
        if (gp_frames() != g_last) {
            g_last = gp_frames();
            x_last = rd(MCPX_APU_XGSCNT);
        } else if (rd(MCPX_APU_XGSCNT) - x_last > 512u) {
            logf_("  %-46s STALLED after %lu ticks, %lu starts\n", tag, ticks, starts);
            save_log();
            return false;
        }
    }
    logf_("  %-46s ok: %lu ticks, %lu starts\n", tag, ticks, starts);
    r_reset();
    save_log();
    return true;
}

int main(void) {
    uint32_t pci_id = 0, bar0 = 0, i;
    uint32_t sectl0;
    bool gp_ok;

    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    logf_("MCPX APU probe, round 27 (exact replay of the driver sequence that froze)\n");

    HalReadWritePCISpace(MCPX_APU_PCI_BUS, MCPX_APU_PCI_SLOT, 0x00, &pci_id, 4, FALSE);
    HalReadWritePCISpace(MCPX_APU_PCI_BUS, MCPX_APU_PCI_SLOT, 0x10, &bar0, 4, FALSE);
    s_bar0_phys = bar0 & ~0xFu;
    if (pci_id != MCPX_APU_PCI_ID || s_bar0_phys == 0) {
        s_bar0_phys = MCPX_APU_BAR0_DEFAULT;
    }
    s_bar0 = (volatile uint8_t *)s_bar0_phys;
    sectl0 = rd(MCPX_APU_SECTL);

    s_magic = (uint32_t *)alloc_phys(0x4000u, &s_magic_phys);
    s_voices = alloc_phys(256u * 0x80u, &s_voices_phys);
    s_sge = alloc_phys(2048u * 8u, &s_sge_phys);
    s_ssl = alloc_phys(0x4000u, &s_ssl_phys);
    s_notify = alloc_phys(16u * (2u + 4u * 256u), &s_notify_phys);
    s_pcm = alloc_phys(8u * 4096u, &s_pcm_phys);
    s_ring = alloc_phys(RING_BYTES, &s_ring_phys);
    s_ring_sge = (uint32_t *)alloc_phys(0x4000u, &s_ring_sge_phys);
    if (!s_magic || !s_voices || !s_sge || !s_ssl || !s_notify || !s_pcm || !s_ring || !s_ring_sge) {
        logf_("allocation failed\n");
        save_log();
        for (;;) Sleep(1000);
    }
    wr(0x1324u, s_magic_phys);
    {
        int16_t *ping = (int16_t *)((uint8_t *)s_pcm + PING_OFF);
        for (i = 0; i < PING_FRAMES; i++) {
            float t = (float)i / 48000.0f;
            ping[i] = (int16_t)(12000.0f * expf(-t * 18.0f) * sinf(2.0f * 3.14159265f * 880.0f * t));
        }
    }
    for (i = 0; i < 7u; i++) s_sge[2u * i] = s_pcm_phys + i * 4096u;   /* only the pages the driver maps */
    for (i = 0; i < RING_BYTES / 4096u; i++) s_ring_sge[2u * i] = s_ring_phys + i * 4096u;

    /* the library's bring-up */
    wr(MCPX_APU_SECTL, 0);
    wr(MCPX_APU_FECTL, 0);
    wr(MCPX_APU_IEN, 0);
    wr(MCPX_APU_ISTS, 0xFFFFFFFFu);
    wr(MCPX_APU_VPVADDR, s_voices_phys);
    wr(MCPX_APU_VPSGEADDR, s_sge_phys);
    wr(MCPX_APU_VPSSLADDR, s_ssl_phys);
    wr(MCPX_APU_TVL2D, 0xFFFFu);
    wr(MCPX_APU_TVL3D, 0xFFFFu);
    wr(MCPX_APU_TVLMP, 0xFFFFu);
    wr(MCPX_APU_FETFORCE1, 0);
    wr(MCPX_APU_FENADDR, s_notify_phys);
    wr(MCPX_APU_EPRST, 0u);
    wr(MCPX_APU_GPFADDR, s_ring_sge_phys);
    wr(MCPX_APU_GPFMAXSGE, RING_BYTES / 4096u - 1u);
    wr(MCPX_APU_GPOFBASE0, 0u);
    wr(MCPX_APU_GPOFEND0, RING_BYTES);
    wr(MCPX_APU_GPOFCUR0, 0u);
    gp_ok = load_prog(0, s_gp_out, 14u);
    if (!gp_ok) {
        logf_("-- GP not running our code; stopping here\n");
        goto out;
    }
    for (i = 0; i < 7u; i++) wr(MCPX_APU_GP_XMEM + 4u * i, s_gp_desc[i]);
    wr(MCPX_APU_SECTL, 0x0Fu);
    Sleep(20);

    logf_("-- replay of apu_vp_stress D1 (seed 0x1234567: the driver froze at tick 23)\n");
    /* safest first: each variant that survives tells which ingredient is needed */
    r_check_end = false;
    replay("R1: same seed, never unlink ended voices:", 0x1234567u, 4000);
    r_check_end = true;
    replay("R2: same seed, exactly as the driver:", 0x1234567u, 4000);
    replay("R3: other seed, as the driver:", 0x7654321u, 4000);

out:
    wr(MCPX_APU_TVL2D, 0xFFFFu);
    wr(MCPX_APU_SECTL, sectl0 & ~0x18u);
    wr(MCPX_APU_GPRST, 0u);
    wr(MCPX_APU_EPRST, 0u);
    logf_("done (lists empty, DSPs in reset, frames off)\n");
    save_log();
    for (;;) Sleep(1000);
    return 0;
}
