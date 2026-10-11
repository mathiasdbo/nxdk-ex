/*
 * GP DSP probe, round 1 (reverb plan phase 0, lib/openal/docs/GP_REVERB_PLAN.md).
 * Clean-room: our own GP program (gp_probe.asm, assembled with
 * lib/openal/tools/dsp56asm.py) and our own data; nothing left by other software.
 *
 * Measures what a reverb program on the GP depends on:
 *   1-3  X/Y/P memory seen through the host windows (size, aliasing)
 *   4    the same words read by the DSP core
 *   5-7  scratch DMA: DSP -> circular buffer, circular buffer -> DSP, and both
 *        chained as a delay line (frame stamp read back K frames later)
 *   8    DMA end-of-list flag ($FFFFC5 bit 7) as a completion wait
 *   9    instruction budget: cycles of NOP per frame before frames slow down
 *   10   descriptor budget: DMA blocks per frame
 *   11-12 a voice's output 4 routed to mixbin 2 (send bus), plain and HRTF voice
 *
 * Log appended to E:\apu_gp_probe.txt. Resumable across a freeze through
 * E:\gp_probe_progress.txt: a step that did not finish is reported and skipped.
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
#include "gp_probe.h"

#define BAR0        ((uintptr_t)NV_PAPU_BASE)
#define REG(off)    (*(volatile uint32_t *)(BAR0 + (off)))
#define XW(n)       (*(volatile uint32_t *)(BAR0 + MCPX_APU_GP_XMEM + 4u * (n)))
#define YW(n)       (*(volatile uint32_t *)(BAR0 + 0x36000u + 4u * (n)))
#define PW(n)       (*(volatile uint32_t *)(BAR0 + MCPX_APU_GP_PMEM + 4u * (n)))
#define PROGRESS    "E:\\gp_probe_progress.txt"
#define LOGFILE     "E:\\apu_gp_probe.txt"

/* DMA descriptor control words (xemu's field list; the probe checks them) */
#define EOL         0x004000u
#define CTL_W       0x0009D2u   /* DSP -> memory, offset write-back, circular scratch (0xE), format 2 (24-bit) */
#define CTL_R       0x0009D0u   /* memory -> DSP, same */

/* Scratch layout (byte offsets; 0..$1FFF hold the program image) */
#define SCR_W       0x2000u
#define SCR_R       0x3000u
#define SCR_D       0x4000u
#define SCR_BUDGET  0x8000u     /* + i * 0x1000, i < 64 */
#define SCR_BYTES   (SCR_BUDGET + 64u * 0x1000u)
#define LINE_BYTES  0x1000u

#define DESC_W      0x100u
#define DESC_R      0x108u
#define DESC_BUDGET 0x400u      /* + 8 i */
#define BLOCK_IN    0x340u      /* the read descriptor's DSP block */

static char s_log[48 * 1024];
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

static uint32_t frames_now(void) {
    return XW(0x10u) & 0xFFFFFFu;
}

/* GP frames per second, measured over `ms` */
static uint32_t frame_rate(uint32_t ms) {
    uint32_t f0 = frames_now();
    DWORD t0 = GetTickCount(), dt;
    Sleep(ms);
    dt = GetTickCount() - t0;
    return dt ? (((frames_now() - f0) & 0xFFFFFFu) * 1000u + dt / 2u) / dt : 0;
}

static uint8_t *s_scr;   /* the GP scratch space (virtual) */

static uint32_t pat(uint32_t i) {
    static const uint32_t edge[6] = { 0x800000u, 0x7FFFFFu, 0x000001u, 0xFFFFFFu, 0x123456u, 0xABCDEFu };
    return i < 6u ? edge[i] : ((i * 0x0B1357u + 0x2468ACu) & 0xFFFFFFu);
}

static void set_desc(uint32_t at, uint32_t next, uint32_t ctl, uint32_t count, uint32_t dsp, uint32_t off,
                     uint32_t base, uint32_t size) {
    XW(at + 0u) = next;
    XW(at + 1u) = ctl;
    XW(at + 2u) = count;
    XW(at + 3u) = dsp;
    XW(at + 4u) = off;
    XW(at + 5u) = base;
    XW(at + 6u) = size - 1u;
}

static void chain(uint32_t head) {
    XW(0u) = head;   /* the FIFO descriptor's next pointer */
}

/* ---------------------------------------------------------------- 1-3 */

static void sweep(const char *name, volatile uint32_t *(*at)(uint32_t), uint32_t tag, uint32_t first, uint32_t end,
                  uint32_t skip) {
    uint32_t a, run_start = first, run_kind = 0xFFFFFFFFu, n_ok = 0, runs = 0;
    for (a = first; a < end; a++) {
        if (a != skip) *at(a) = tag | a;
    }
    Sleep(2);
    logf_("  %s:$%04lx-$%04lx (tag %06lx):\n", name, (unsigned long)first, (unsigned long)end - 1u, (unsigned long)tag);
    for (a = first; a <= end; a++) {
        uint32_t kind;
        if (a == end) {
            kind = 0xFFFFFFFEu;
        } else if (a == skip) {
            kind = 3u;
        } else {
            uint32_t v = *at(a) & 0xFFFFFFu;
            if (v == (tag | a)) {
                kind = 0u;
                n_ok++;
            } else if ((v & 0xFF0000u) == tag) {
                kind = 0x10000u + ((v - a) & 0xFFFFu);   /* aliased: reads the word at a + delta */
            } else {
                kind = 1u;
            }
        }
        if (kind != run_kind) {
            if (run_kind != 0xFFFFFFFFu && runs < 24u) {
                const char *k = run_kind == 0u ? "ok" : run_kind == 1u ? "other" : run_kind == 3u ? "skipped" : "alias";
                logf_("    $%04lx-$%04lx %s", (unsigned long)run_start, (unsigned long)a - 1u, k);
                if (run_kind >= 0x10000u) logf_(" (+$%04lx)", (unsigned long)(run_kind & 0xFFFFu));
                if (run_kind == 1u) logf_(" (first value %06lx)", (unsigned long)(*at(run_start) & 0xFFFFFFu));
                logf_("\n");
                runs++;
            }
            run_kind = kind;
            run_start = a;
        }
    }
    logf_("    %lu words ok\n", (unsigned long)n_ok);
}

static volatile uint32_t *x_at(uint32_t a) { return &XW(a); }
static volatile uint32_t *y_at(uint32_t a) { return &YW(a); }
static volatile uint32_t *p_at(uint32_t a) { return &PW(a); }

static void step_sweep_x(void) { sweep("X", x_at, 0x5A0000u, 0x40u, 0x1400u, GP_PROBE_STAMP); }
static void step_sweep_y(void) { sweep("Y", y_at, 0x6B0000u, 0x0u, 0x1000u, 0xFFFFFFFFu); }
static void step_sweep_p(void) { sweep("P", p_at, 0x7C0000u, 0x800u, 0x1000u, 0xFFFFFFFFu); }

/* ---------------------------------------------------------------- 4 */

static void step_core_read(void) {
    static const uint32_t addr[] = { 0x100u, 0x7FFu, 0x800u, 0xBFFu, 0xC00u, 0xFFFu, 0x1000u, 0x13FFu };
    uint32_t i;
    for (i = 0; i < sizeof(addr) / sizeof(addr[0]); i++) {
        uint32_t a = addr[i], wy = 0;
        XW(a) = 0x3C0000u | a;
        if (a < 0x1000u) YW(a) = 0x4D0000u | a;
        XW(GP_PROBE_RX) = 0xEEEEEEu;
        XW(GP_PROBE_RY) = 0xEEEEEEu;
        XW(GP_PROBE_RADDR) = a;
        XW(GP_PROBE_FLAGS) = 4u;
        Sleep(10);
        XW(GP_PROBE_FLAGS) = 0u;
        Sleep(2);
        if (a < 0x1000u) wy = YW(a) & 0xFFFFFFu;
        logf_("  $%04lx: window X %06lx core X %06lx | window Y %06lx core Y %06lx\n", (unsigned long)a,
              (unsigned long)(XW(a) & 0xFFFFFFu), (unsigned long)(XW(GP_PROBE_RX) & 0xFFFFFFu), (unsigned long)wy,
              (unsigned long)(XW(GP_PROBE_RY) & 0xFFFFFFu));
    }
}

/* ---------------------------------------------------------------- 5 */

static void find_pattern(const char *what) {
    uint32_t off, hits = 0;
    for (off = SCR_BYTES - 4u; off >= 0x2000u && hits < 4u; off -= 4u) {
        uint32_t w = *(volatile uint32_t *)(s_scr + off);
        if (w == pat(1) || w == (pat(1) << 8)) {
            if (hits == 0u) logf_("  %s: pattern word 1 found at scratch", what);
            logf_(" %05lx%s", (unsigned long)off, w == pat(1) ? "" : "(<<8)");
            hits++;
        }
    }
    logf_(hits ? "\n" : "  %s: pattern word 1 not found anywhere in scratch\n", what);
}

static void step_dma_write(void) {
    uint32_t i, b, match_lsb = 0, match_msb = 0, f0;
    for (i = 1; i < 32u; i++) XW(GP_PROBE_STAMP + i) = pat(i);
    memset(s_scr + 0x2000u, 0xEE, SCR_BYTES - 0x2000u);
    set_desc(DESC_W, EOL, CTL_W, 32u, GP_PROBE_STAMP, 0u, SCR_W, LINE_BYTES);
    f0 = frames_now();
    chain(DESC_W);
    Sleep(50);
    chain(EOL);
    Sleep(5);
    logf_("  frames %lu, write-back offset %06lx (byte semantics: advances 128 per frame mod 4096)\n",
          (unsigned long)((frames_now() - f0) & 0xFFFFFFu), (unsigned long)(XW(DESC_W + 4u) & 0xFFFFFFu));
    logf_("  block 0 raw:");
    for (i = 0; i < 6u; i++) logf_(" %08lx", (unsigned long)((volatile uint32_t *)(s_scr + SCR_W))[i]);
    logf_("\n  stamps by block:");
    for (b = 0; b < LINE_BYTES / 128u; b++) {
        volatile uint32_t *blk = (volatile uint32_t *)(s_scr + SCR_W + 128u * b);
        uint32_t ok_l = 1, ok_m = 1;
        for (i = 1; i < 32u; i++) {
            if (blk[i] != pat(i)) ok_l = 0;
            if (blk[i] != (pat(i) << 8)) ok_m = 0;
        }
        match_lsb += ok_l;
        match_msb += ok_m;
        logf_(" %lx", (unsigned long)(blk[0] & 0xFFFFFFFFu));
    }
    logf_("\n  blocks with the payload: %lu as 24-bit in bits 23:0, %lu as bits 31:8 (of %lu)\n",
          (unsigned long)match_lsb, (unsigned long)match_msb, (unsigned long)(LINE_BYTES / 128u));
    find_pattern("write");
}

/* ---------------------------------------------------------------- 6 */

static void step_dma_read(void) {
    uint32_t i, k;
    volatile uint32_t *r = (volatile uint32_t *)(s_scr + SCR_R);
    for (i = 0; i < LINE_BYTES / 4u; i++) r[i] = 0x400000u | i;
    for (i = 0; i < 32u; i++) XW(BLOCK_IN + i) = 0;
    set_desc(DESC_R, EOL, CTL_R, 32u, BLOCK_IN, 0u, SCR_R, LINE_BYTES);
    chain(DESC_R);
    for (k = 0; k < 8u; k++) {
        Sleep(3);
        logf_("  X:$340 %06lx  X:$35F %06lx  offset %06lx\n", (unsigned long)(XW(BLOCK_IN) & 0xFFFFFFu),
              (unsigned long)(XW(BLOCK_IN + 31u) & 0xFFFFFFu), (unsigned long)(XW(DESC_R + 4u) & 0xFFFFFFu));
    }
    chain(EOL);
    Sleep(3);
    {
        uint32_t first = XW(BLOCK_IN) & 0xFFFFFFu, bad = 0;
        for (i = 1; i < 32u; i++) {
            if ((XW(BLOCK_IN + i) & 0xFFFFFFu) != ((first + i) & 0xFFFFFFu)) bad++;
        }
        logf_("  last block: first word %06lx, %lu of 31 words not consecutive\n", (unsigned long)first,
              (unsigned long)bad);
    }
}

/* ---------------------------------------------------------------- 7 */

static void delay_line(uint32_t k_frames, bool read_first) {
    uint32_t hist[10], i, other = 0;
    memset(hist, 0, sizeof(hist));
    memset(s_scr + SCR_D, 0, LINE_BYTES);
    for (i = 0; i < 32u; i++) XW(BLOCK_IN + i) = 0;
    /* write at offset 128 K, read at 0: the read trails the write by K blocks */
    if (read_first) {
        set_desc(DESC_R, DESC_W, CTL_R, 32u, BLOCK_IN, 0u, SCR_D, LINE_BYTES);
        set_desc(DESC_W, EOL, CTL_W, 32u, GP_PROBE_STAMP, 128u * k_frames, SCR_D, LINE_BYTES);
        chain(DESC_R);
    } else {
        set_desc(DESC_W, DESC_R, CTL_W, 32u, GP_PROBE_STAMP, 128u * k_frames, SCR_D, LINE_BYTES);
        set_desc(DESC_R, EOL, CTL_R, 32u, BLOCK_IN, 0u, SCR_D, LINE_BYTES);
        chain(DESC_W);
    }
    Sleep(30);
    for (i = 0; i < 60u; i++) {
        uint32_t d = (XW(GP_PROBE_STAMP) - XW(BLOCK_IN)) & 0xFFFFFFu;
        if (d < 10u) hist[d]++; else other++;
        Sleep(1u + (i % 3u));
    }
    chain(EOL);
    Sleep(3);
    logf_("  K=%lu %s: stamp - read-back:", (unsigned long)k_frames, read_first ? "read then write" : "write then read");
    for (i = 0; i < 10u; i++) if (hist[i]) logf_(" %lu:%lu", (unsigned long)i, (unsigned long)hist[i]);
    if (other) logf_(" other:%lu (last X:$340 %06lx)", (unsigned long)other, (unsigned long)(XW(BLOCK_IN) & 0xFFFFFFu));
    logf_("\n");
}

static void step_delay(void) {
    for (uint32_t i = 1; i < 32u; i++) XW(GP_PROBE_STAMP + i) = pat(i);
    delay_line(4u, false);
    delay_line(1u, false);
    delay_line(0u, false);
    delay_line(4u, true);
    delay_line(1u, true);
}

/* ---------------------------------------------------------------- 8 */

static void wait_report(const char *what) {
    XW(GP_PROBE_SPMAX) = 0;
    XW(GP_PROBE_TMOUT) = 0;
    XW(GP_PROBE_SPLIM) = 5000u;
    XW(GP_PROBE_FLAGS) = 1u;
    {
        uint32_t rate = frame_rate(300);
        logf_("  %s: %lu frames/s, polls last %lu max %lu, timeouts %lu, $FFFFC5 before clear %06lx, CONTROL %06lx\n",
              what, (unsigned long)rate, (unsigned long)(XW(GP_PROBE_SPINS) & 0xFFFFFFu),
              (unsigned long)(XW(GP_PROBE_SPMAX) & 0xFFFFFFu), (unsigned long)(XW(GP_PROBE_TMOUT) & 0xFFFFFFu),
              (unsigned long)(XW(GP_PROBE_RAWC5) & 0xFFFFFFu), (unsigned long)(XW(GP_PROBE_DMACTL) & 0xFFFFFFu));
    }
    XW(GP_PROBE_FLAGS) = 0u;
    Sleep(5);
}

static void step_eol(void) {
    chain(EOL);
    wait_report("FIFO only");
    set_desc(DESC_W, DESC_R, CTL_W, 32u, GP_PROBE_STAMP, 512u, SCR_D, LINE_BYTES);
    set_desc(DESC_R, EOL, CTL_R, 32u, BLOCK_IN, 0u, SCR_D, LINE_BYTES);
    chain(DESC_W);
    wait_report("FIFO + write + read");
    chain(EOL);
}

/* ---------------------------------------------------------------- 9 */

static void step_burn(void) {
    static const uint32_t n[] = { 1000u, 4000u, 8000u, 16000u, 24000u, 32000u, 48000u, 64000u, 96000u, 128000u,
                                  192000u, 256000u };
    uint32_t i;
    logf_("  baseline %lu frames/s\n", (unsigned long)frame_rate(300));
    for (i = 0; i < sizeof(n) / sizeof(n[0]); i++) {
        uint32_t rate;
        XW(GP_PROBE_BURN) = n[i];
        XW(GP_PROBE_FLAGS) = 2u;
        Sleep(50);
        rate = frame_rate(300);
        logf_("  rep %6lu nop: %lu frames/s\n", (unsigned long)n[i], (unsigned long)rate);
        save_log();
        if (rate < 1400u) break;
    }
    XW(GP_PROBE_FLAGS) = 0u;
    Sleep(20);
    logf_("  after: %lu frames/s\n", (unsigned long)frame_rate(200));
}

/* ---------------------------------------------------------------- 10 */

static uint32_t budget(uint32_t k, bool mixed) {
    uint32_t i, rate;
    for (i = 0; i < k; i++) {
        uint32_t at = DESC_BUDGET + 8u * i, next = (i + 1u < k) ? at + 8u : EOL;
        if (mixed && (i & 1u)) {
            set_desc(at, next, CTL_R, 32u, BLOCK_IN, 0u, SCR_BUDGET + 0x1000u * i, LINE_BYTES);
        } else {
            set_desc(at, next, CTL_W, 32u, GP_PROBE_STAMP, 0u, SCR_BUDGET + 0x1000u * i, LINE_BYTES);
        }
    }
    XW(GP_PROBE_SPMAX) = 0;
    XW(GP_PROBE_TMOUT) = 0;
    XW(GP_PROBE_SPLIM) = 20000u;
    XW(GP_PROBE_FLAGS) = 1u;
    chain(DESC_BUDGET);
    Sleep(30);
    rate = frame_rate(300);
    logf_("  %2lu %s: %lu frames/s, polls last %lu max %lu, timeouts %lu, offsets first %06lx last %06lx\n",
          (unsigned long)k, mixed ? "write/read" : "writes", (unsigned long)rate,
          (unsigned long)(XW(GP_PROBE_SPINS) & 0xFFFFFFu), (unsigned long)(XW(GP_PROBE_SPMAX) & 0xFFFFFFu),
          (unsigned long)(XW(GP_PROBE_TMOUT) & 0xFFFFFFu), (unsigned long)(XW(DESC_BUDGET + 4u) & 0xFFFFFFu),
          (unsigned long)(XW(DESC_BUDGET + 8u * (k - 1u) + 4u) & 0xFFFFFFu));
    chain(EOL);
    XW(GP_PROBE_FLAGS) = 0u;
    Sleep(10);
    save_log();
    return rate;
}

static void step_budget(void) {
    static const uint32_t k[] = { 8u, 16u, 32u, 48u, 64u };
    uint32_t i;
    for (i = 0; i < 5u; i++) if (budget(k[i], false) < 1400u) break;
    for (i = 1; i < 5u; i++) if (budget(k[i], true) < 1400u) break;
}

/* ---------------------------------------------------------------- 11-12 */

#define TONE_FRAMES 4800u
static uint32_t s_tone_phys;
static int16_t s_cap[2 * 4800];

static void capture_peaks(int *l, int *r) {
    uint32_t got = 0, i;
    DWORD t0 = GetTickCount();
    apu_gp_fifo_discard(BAR0);
    while (got < 4800u && GetTickCount() - t0 < 400u) {
        got += apu_gp_fifo_read(BAR0, s_cap + 2u * got, 4800u - got);
        Sleep(2);
    }
    *l = *r = 0;
    for (i = 0; i < got; i++) {
        int a = abs(s_cap[2 * i]), b = abs(s_cap[2 * i + 1]);
        if (a > *l) *l = a;
        if (b > *r) *r = b;
    }
}

static void send_test(bool hrtf) {
    apu_vp_voice_params_t p;
    uint32_t i, h = 5u;
    int l01, r01, l23, r23;
    memset(&p, 0, sizeof(p));
    p.phys = s_tone_phys;
    p.bytes = TONE_FRAMES * 2u;
    p.channels = 1;
    p.bits = 16;
    p.loop = true;
    p.tail_bytes = APU_VP_SILENT_TAIL_BYTES;
    p.hrtf_entry = hrtf ? 0 : -1;
    for (i = 0; i < APU_VP_OUTPUTS; i++) {
        p.bins[i] = (uint8_t)(i & 1u);
        p.vols[i] = 0xFFFu;
    }
    p.vols[0] = 0;
    if (hrtf) p.vols[2] = 0;      /* the right ear's submix slot */
    p.bins[4] = 2;                /* the send */
    p.vols[4] = 0;
    if (apu_vp_voice_start(BAR0, h, &p) != 0) {
        logf_("  voice start failed\n");
        return;
    }
    Sleep(40);
    XW(3u) = 0x1400u;
    Sleep(5);
    capture_peaks(&l01, &r01);
    XW(3u) = 0x1440u;
    Sleep(5);
    capture_peaks(&l23, &r23);
    XW(3u) = 0x1400u;
    logf_("  %s voice (3D list %s): bins 0/1 peaks %d / %d, bins 2/3 peaks %d / %d\n", hrtf ? "HRTF" : "plain",
          apu_vp_voice_is_hrtf(h) ? "yes" : "no", l01, r01, l23, r23);
    apu_vp_voice_off(BAR0, h);
    Sleep(10);
}

static void step_send_plain(void) { send_test(false); }
static void step_send_hrtf(void) { send_test(true); }

/* ---------------------------------------------------------------- driver */

typedef struct {
    const char *name;
    void (*run)(void);
} step_t;

static const step_t s_steps[] = {
    { "1 X window sweep", step_sweep_x },
    { "2 Y window sweep", step_sweep_y },
    { "3 P window sweep ($800-$FFF)", step_sweep_p },
    { "4 core reads", step_core_read },
    { "5 DMA DSP -> circular scratch", step_dma_write },
    { "6 DMA circular scratch -> DSP", step_dma_read },
    { "7 delay line (write and read chained)", step_delay },
    { "8 end-of-list wait", step_eol },
    { "9 instruction budget", step_burn },
    { "10 descriptor budget", step_budget },
    { "11 send: plain voice output 4 -> bin 2", step_send_plain },
    { "12 send: HRTF voice output 4 -> bin 2", step_send_hrtf },
};
#define N_STEPS ((int)(sizeof(s_steps) / sizeof(s_steps[0])))

int main(void) {
    int resume, k;

    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    resume = read_progress();
    if (resume >= N_STEPS) {
        logf_("\n=== apu_gp_probe round 1: all %d steps already ran (delete %s to run them again)\n", N_STEPS, PROGRESS);
        save_log();
        for (;;) Sleep(1000);
    }
    if (resume < 0) {
        logf_("\n=== apu_gp_probe round 1: %d steps (fresh start)\n", N_STEPS);
        resume = 0;
    } else {
        logf_("\n=== apu_gp_probe round 1: resumed; step %s did not finish (FROZE?)\n", s_steps[resume].name);
        resume++;
    }
    save_log();

    apu_gp_debug_set_program(gp_probe_words, GP_PROBE_WORDS);
    apu_gp_debug_set_scratch_bytes(SCR_BYTES);
    apu_vp_debug_set_flags(APU_VP_DBG_NO_AC97);
    if (apu_vp_init(BAR0) != 0) {
        logf_("apu_vp_init failed\n");
        save_log();
        for (;;) Sleep(1000);
    }
    s_scr = (uint8_t *)apu_gp_debug_scratch();
    {
        int16_t *pcm = (int16_t *)apu_mem_alloc_table(TONE_FRAMES * 2u + APU_VP_SILENT_TAIL_BYTES, &s_tone_phys);
        uint32_t i;
        for (i = 0; i < TONE_FRAMES; i++) pcm[i] = (int16_t)(((i / 50u) & 1u) ? -8000 : 8000);   /* 480 Hz */
        memset(pcm + TONE_FRAMES, 0, APU_VP_SILENT_TAIL_BYTES);
    }
    logf_("program loaded %s, scratch %lu bytes at %p, start retries %lu, %lu frames/s, HRTF %s\n",
          apu_gp_program_loaded() ? "yes" : "NO", (unsigned long)apu_gp_scratch_bytes(), (void *)s_scr,
          (unsigned long)apu_vp_start_retries(), (unsigned long)frame_rate(200),
          apu_vp_hrtf_available() ? "available" : "not available");
    save_log();

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
