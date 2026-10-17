/*
 * GP DSP probe (reverb plan, lib/openal/docs/GP_REVERB_PLAN.md). Clean-room:
 * our own GP program (gp_probe.asm, assembled with lib/openal/tools/dsp56asm.py)
 * and our own data; nothing left by other software.
 *
 * Round 1 (phase 0, results in XEMU_VERIFICATION.md 8.1b): X RAM $000-$BFF
 * ($C00-$FFF mirror the mixbins), Y $000-$7FF, P $000-$FFF; circular scratch
 * DMA works both ways with byte offsets and write-back, and as a delay line
 * (stamp read back exactly K frames later); the end-of-list flag works as a
 * wait; 64000 NOP cycles and 64 descriptors per frame keep full rate; output 4
 * of plain and HRTF voices reaches mixbin 2. But a linear block with
 * dsp_step 0 moved one DSP word 32 times, and format 2 keeps a word in bits 31:8.
 *
 * Round 2:
 *   1-3  linear blocks with dsp_step 1: DSP -> scratch (formats 2 and 6) and back
 *   4-5  phase 2: an echo on the GP. A click voice plays into bins 0/1 and the
 *        send bin 2; the program adds wet * line to the output and feeds
 *        bin 2 + fb * line back into a DMA delay line of D samples
 *        (D = 12000 and 12007: whole and partial frames)
 *   6    listening: AC97 output on, the echo switches on and off every 4 s
 *
 * Log appended to E:\apu_gp_probe.txt. Resumable across a freeze through
 * E:\gp_probe2_progress.txt.
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

#include "apu_ac97.h"
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
#define PROGRESS    "E:\\gp_probe2_progress.txt"
#define LOGFILE     "E:\\apu_gp_probe.txt"

/* DMA descriptor control words */
#define EOL         0x004000u
#define STEP1       (1u << 14)  /* dsp_step 1: the DSP address advances one word per item */
#define CTL_W       (0x0009D2u | STEP1)   /* DSP -> memory, offset write-back, circular scratch, format 2 */
#define CTL_W6      (0x0019D2u | STEP1)   /* the same with format 6 */
#define CTL_R       (0x0009D0u | STEP1)   /* memory -> DSP, format 2 */

/* Scratch layout (byte offsets; 0..$1FFF hold the program image) */
#define SCR_W       0x2000u
#define SCR_R       0x3000u
#define SCR_LINE    0x10000u
#define LINE_MAX    0x10000u    /* 16384 words: delays up to 341 ms */
#define SCR_BYTES   (SCR_LINE + LINE_MAX)
#define LINE_BYTES  0x1000u

#define DESC_W      0x100u
#define DESC_R      0x108u
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

static void dma_write(uint32_t ctl, bool msb) {
    uint32_t i, b, match = 0, f0, first_bad = 0xFFFFFFFFu;
    for (i = 1; i < 32u; i++) XW(GP_PROBE_STAMP + i) = pat(i);
    memset(s_scr + SCR_W, 0xEE, LINE_BYTES);
    set_desc(DESC_W, EOL, ctl, 32u, GP_PROBE_STAMP, 0u, SCR_W, LINE_BYTES);
    f0 = frames_now();
    chain(DESC_W);
    Sleep(50);
    chain(EOL);
    Sleep(5);
    logf_("  control %06lx: frames %lu, offset %06lx; block 0 raw:", (unsigned long)ctl,
          (unsigned long)((frames_now() - f0) & 0xFFFFFFu), (unsigned long)(XW(DESC_W + 4u) & 0xFFFFFFu));
    for (i = 0; i < 6u; i++) logf_(" %08lx", (unsigned long)((volatile uint32_t *)(s_scr + SCR_W))[i]);
    for (b = 0; b < LINE_BYTES / 128u; b++) {
        volatile uint32_t *blk = (volatile uint32_t *)(s_scr + SCR_W + 128u * b);
        uint32_t ok = 1;
        for (i = 1; i < 32u; i++) {
            uint32_t want = msb ? (pat(i) << 8) : pat(i);
            if (blk[i] != want) {
                ok = 0;
                if (first_bad == 0xFFFFFFFFu) first_bad = b * 32u + i;
            }
        }
        match += ok;
    }
    logf_("\n    blocks holding words 1-31 as bits %s: %lu of %lu", msb ? "31:8" : "23:0", (unsigned long)match,
          (unsigned long)(LINE_BYTES / 128u));
    if (first_bad != 0xFFFFFFFFu) {
        uint32_t w = first_bad;
        logf_(" (first mismatch at word %lu: %08lx, wanted %06lx)", (unsigned long)w,
              (unsigned long)((volatile uint32_t *)(s_scr + SCR_W))[w], (unsigned long)pat(w % 32u));
    }
    logf_("\n");
}

static void step_write2(void) { dma_write(CTL_W, true); }
static void step_write6(void) { dma_write(CTL_W6, false); }

static void step_read(void) {
    uint32_t i, k, bad = 0, first;
    volatile uint32_t *r = (volatile uint32_t *)(s_scr + SCR_R);
    for (i = 0; i < LINE_BYTES / 4u; i++) r[i] = (0x400000u | i) << 8;
    for (i = 0; i < 32u; i++) XW(BLOCK_IN + i) = 0;
    set_desc(DESC_R, EOL, CTL_R, 32u, BLOCK_IN, 0u, SCR_R, LINE_BYTES);
    chain(DESC_R);
    for (k = 0; k < 4u; k++) {
        Sleep(3);
        logf_("  X:$340 %06lx  X:$35F %06lx  offset %06lx\n", (unsigned long)(XW(BLOCK_IN) & 0xFFFFFFu),
              (unsigned long)(XW(BLOCK_IN + 31u) & 0xFFFFFFu), (unsigned long)(XW(DESC_R + 4u) & 0xFFFFFFu));
    }
    chain(EOL);
    Sleep(3);
    first = XW(BLOCK_IN) & 0xFFFFFFu;
    for (i = 1; i < 32u; i++) {
        if ((XW(BLOCK_IN + i) & 0xFFFFFFu) != ((first + i) & 0xFFFFFFu)) bad++;
    }
    logf_("  last block: first word %06lx, %lu of 31 words not consecutive\n", (unsigned long)first, (unsigned long)bad);
}

/* ---------------------------------------------------------------- 4-6 */

#define CLICK_FRAMES 48000u   /* one click per second */
#define CAP_FRAMES   60000u
static uint32_t s_click_phys;
static int16_t s_cap[2 * CAP_FRAMES];
static const uint32_t s_voice = 5u;

static void click_on(void) {
    apu_vp_voice_params_t p;
    uint32_t i;
    memset(&p, 0, sizeof(p));
    p.phys = s_click_phys;
    p.bytes = CLICK_FRAMES * 2u;
    p.channels = 1;
    p.bits = 16;
    p.loop = true;
    p.tail_bytes = APU_VP_SILENT_TAIL_BYTES;
    p.hrtf_entry = -1;
    for (i = 0; i < APU_VP_OUTPUTS; i++) {
        p.bins[i] = (uint8_t)(i & 1u);
        p.vols[i] = 0xFFFu;
    }
    p.vols[0] = p.vols[1] = 0;    /* dry: both channels */
    p.bins[4] = 2;                /* the send */
    p.vols[4] = 0;
    apu_vp_voice_start(BAR0, s_voice, &p);
}

static void echo_on(uint32_t d, uint32_t wet, uint32_t fb) {
    uint32_t i;
    for (i = 0; i < 0x80u; i++) XW(0x200u + i) = 0;   /* staging, DIN, LOUT */
    memset(s_scr + SCR_LINE, 0, LINE_MAX);
    /* chain A: the line block written d samples ago -> DIN */
    set_desc(DESC_R, EOL, CTL_R, 32u, GP_PROBE_DIN, 0u, SCR_LINE, LINE_MAX);
    /* chain B: LOUT -> the line, then the FIFO descriptor at X:0 */
    set_desc(DESC_W, 0u, CTL_W, 32u, GP_PROBE_LOUT, 4u * d, SCR_LINE, LINE_MAX);
    XW(GP_PROBE_WET) = wet;
    XW(GP_PROBE_FB) = fb;
    XW(GP_PROBE_SPLIM) = 4000u;
    XW(GP_PROBE_SPMAX) = 0;
    XW(GP_PROBE_TMOUT) = 0;
    XW(GP_PROBE_HEADB) = DESC_W;
    XW(GP_PROBE_HEADA) = DESC_R;
    XW(3u) = GP_PROBE_OUTL;       /* the FIFO sends the staging area */
    XW(GP_PROBE_FLAGS) = 8u;
}

static void echo_off(void) {
    XW(GP_PROBE_FLAGS) = 0u;
    XW(GP_PROBE_HEADA) = 0u;
    XW(GP_PROBE_HEADB) = 0u;
    XW(3u) = 0x1400u;
}

static uint32_t capture(uint32_t frames) {
    uint32_t got = 0;
    DWORD t0 = GetTickCount();
    apu_gp_fifo_discard(BAR0);
    while (got < frames && GetTickCount() - t0 < 2000u) {
        got += apu_gp_fifo_read(BAR0, s_cap + 2u * got, frames - got);
        Sleep(2);
    }
    return got;
}

/* Clicks and echoes in the left channel: local peaks above 300, 400 samples apart */
static void list_peaks(uint32_t n) {
    uint32_t i, found = 0, last = 0;
    logf_("    peaks (sample: L R):");
    for (i = 0; i < n && found < 12u; i++) {
        int v = abs(s_cap[2 * i]);
        uint32_t j, end = (i + 200u < n) ? i + 200u : n;
        bool is_max = v > 300;
        if (!is_max || (found && i - last < 400u)) continue;
        for (j = (i > 200u ? i - 200u : 0); j < end && is_max; j++) {
            if (abs(s_cap[2 * j]) > v) is_max = false;
        }
        if (!is_max) continue;
        logf_(" %lu: %d %d", (unsigned long)i, s_cap[2 * i], s_cap[2 * i + 1]);
        found++;
        last = i;
    }
    logf_("%s\n", found ? "" : " none");
}

static void echo_test(uint32_t d) {
    uint32_t n, rate;
    echo_on(d, 0x400000u, 0x400000u);   /* wet 0.5, feedback 0.5 */
    Sleep(1500);                         /* the line fills */
    n = capture(CAP_FRAMES);
    rate = frame_rate(200);
    logf_("  D = %lu samples, wet 0.5, feedback 0.5: %lu frames captured, %lu frames/s, waits: last %lu polls, max %lu, "
          "timeouts %lu\n", (unsigned long)d, (unsigned long)n, (unsigned long)rate,
          (unsigned long)(XW(GP_PROBE_SPINS) & 0xFFFFFFu), (unsigned long)(XW(GP_PROBE_SPMAX) & 0xFFFFFFu),
          (unsigned long)(XW(GP_PROBE_TMOUT) & 0xFFFFFFu));
    list_peaks(n);
    echo_off();
    Sleep(20);
    n = capture(CAP_FRAMES);
    logf_("  echo off:\n");
    list_peaks(n);
}

static void step_echo_whole(void) { echo_test(12000u); }
static void step_echo_part(void) { echo_test(12007u); }

#define LISTEN_STEP 5   /* index of step_listen in s_steps */

static void step_listen(void) {
    apu_ac97_stats_t st;
    uint32_t round = 0;
    if (apu_ac97_start(BAR0) != 0) {
        logf_("  AC97 start failed\n");
        return;
    }
    logf_("  AC97 on: a click every second; echo (250 ms, feedback 0.5) on for 4 s, off for 4 s, repeating\n");
    write_progress(LISTEN_STEP + 1);   /* the loop never ends: mark the round done */
    save_log();
    for (;;) {
        echo_on(12000u, 0x400000u, 0x400000u);
        debugPrint("echo ON\n");
        Sleep(4000);
        echo_off();
        debugPrint("echo off\n");
        Sleep(4000);
        if (++round == 4u) {
            apu_ac97_get_stats(&st);
            logf_("  after 32 s: AC97 underruns %lu, padded buffers %lu\n", (unsigned long)st.underruns,
                  (unsigned long)st.padded);
            save_log();
        }
    }
}

/* ---------------------------------------------------------------- driver */

typedef struct {
    const char *name;
    void (*run)(void);
} step_t;

static const step_t s_steps[] = {
    { "1 DMA DSP -> scratch, dsp_step 1, format 2", step_write2 },
    { "2 DMA DSP -> scratch, dsp_step 1, format 6", step_write6 },
    { "3 DMA scratch -> DSP, dsp_step 1, format 2", step_read },
    { "4 echo, delay 12000 samples", step_echo_whole },
    { "5 echo, delay 12007 samples", step_echo_part },
    { "6 listening (runs until switched off)", step_listen },
};
#define N_STEPS ((int)(sizeof(s_steps) / sizeof(s_steps[0])))

int main(void) {
    int resume, k;

    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    resume = read_progress();
    if (resume >= N_STEPS) {
        logf_("\n=== apu_gp_probe round 2: all steps ran (delete %s to run them again); listening only\n", PROGRESS);
        resume = LISTEN_STEP;
    } else if (resume < 0) {
        logf_("\n=== apu_gp_probe round 2: %d steps (fresh start)\n", N_STEPS);
        resume = 0;
    } else {
        logf_("\n=== apu_gp_probe round 2: resumed; step %s did not finish (FROZE?)\n", s_steps[resume].name);
        resume++;
    }
    save_log();

    apu_gp_debug_set_program(gp_probe_words, GP_PROBE_WORDS);
    apu_gp_debug_set_scratch_bytes(SCR_BYTES);
    apu_vp_debug_set_flags(APU_VP_DBG_NO_AC97 | APU_VP_DBG_AC97_COPY);   /* the 64 KiB ring, copy output */
    if (apu_vp_init(BAR0) != 0) {
        logf_("apu_vp_init failed\n");
        save_log();
        for (;;) Sleep(1000);
    }
    s_scr = (uint8_t *)apu_gp_debug_scratch();
    {
        int16_t *pcm = (int16_t *)apu_mem_alloc_table(CLICK_FRAMES * 2u + APU_VP_SILENT_TAIL_BYTES, &s_click_phys);
        memset(pcm, 0, CLICK_FRAMES * 2u + APU_VP_SILENT_TAIL_BYTES);
        pcm[0] = 12000;
        pcm[1] = 16000;
        pcm[2] = 12000;
    }
    logf_("program loaded %s (%u words), scratch %lu bytes, start retries %lu, %lu frames/s\n",
          apu_gp_program_loaded() ? "yes" : "NO", (unsigned)GP_PROBE_WORDS, (unsigned long)apu_gp_scratch_bytes(),
          (unsigned long)apu_vp_start_retries(), (unsigned long)frame_rate(200));
    save_log();
    click_on();
    Sleep(50);

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
