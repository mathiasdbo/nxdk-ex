/*
 * Driver-level stress test of the MCPX VP backend (lib/openal/src/apu_vp.c),
 * without OpenAL on top: one-shot pings started and stopped through the
 * apu_vp_* API (3 starts every 16 ms, the oldest stopped beyond a target).
 * This is the test that found the hardware voice limit: the VP reports any
 * handle above 127 as idle and stops all frames for good (APU_VP_HW_HANDLES,
 * lib/openal/docs/XEMU_VERIFICATION.md 8.1b). It now runs the full driver path
 * as a regression test. The log (E:\apu_vp_stress.txt) records whether GP
 * frames kept running and, on a stall, the driver's operation trace, the
 * records involved, the registers and the front end's response to the ways of
 * releasing an idle-voice trap.
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

#include "apu_gp.h"
#include "apu_hardware.h"
#include "apu_mem.h"
#include "apu_vp.h"
#include "mcpx_apu_regs.h"

#define PING_FRAMES 12000u
#define BAR0        ((uintptr_t)NV_PAPU_BASE)

static char s_log[64 * 1024];
static size_t s_log_len;

static void logf_(const char *fmt, ...) {
    va_list ap;
    int n;
    char line[512];
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
    f = fopen("E:\\apu_vp_stress.txt", "wb");
    if (f) {
        fwrite(s_log, 1, s_log_len, f);
        fclose(f);
    }
}

static uint32_t s_rng = 0x1234567u;
static uint32_t rnd(void) { s_rng = s_rng * 1664525u + 1013904223u; return s_rng >> 8; }

static uint32_t s_ping_phys;
static uint32_t s_q[256], s_qh, s_qn;   /* playing handles, oldest first */

static void stop_all(void) {
    while (s_qn) {
        apu_vp_voice_off(BAR0, s_q[s_qh]);
        s_qh = (s_qh + 1u) % 256u;
        s_qn--;
    }
}

static void fill_params(apu_vp_voice_params_t *p) {
    int i;
    memset(p, 0, sizeof(*p));
    p->phys = s_ping_phys;
    p->bytes = PING_FRAMES * 2u;
    p->channels = 1;
    p->bits = 16;
    p->loop = false;
    p->pitch = (int16_t)((int32_t)(rnd() % 8192u) - 4096);   /* 0.5x .. 2x */
    for (i = 0; i < APU_VP_OUTPUTS; i++) {
        p->bins[i] = (uint8_t)(i & 1);
        p->vols[i] = 0xFFFu;
    }
    p->vols[0] = (uint16_t)(0x600u + rnd() % 0x300u);
    p->vols[1] = (uint16_t)(0x600u + rnd() % 0x300u);
    p->hrtf_entry = -1;
    p->tail_bytes = APU_VP_SILENT_TAIL_BYTES;
}


#define REG(off)    (*(volatile uint32_t *)(BAR0 + (off)))
#define XGSCNT_REG  0x200Cu

/* Stall report: last operations, the records they touched, SGE, registers, GP X memory */
static void dump_stall(const apu_vp_trace_t *tr, uint32_t n) {
    static uint8_t seen[256];
    const uint32_t *sge = apu_vp_debug_sge();
    volatile uint32_t *r = (volatile uint32_t *)BAR0;
    static const uint32_t rng[][2] = { { 0x1000, 0x1018 }, { 0x1100, 0x1160 }, { 0x1300, 0x1344 },
                                       { 0x1500, 0x1510 }, { 0x2000, 0x20FC }, { 0x3000, 0x307C } };
    uint32_t j, a, q, cnt = 0;

    for (j = (n > 40u) ? n - 40u : 0u; j < n; j++) {
        logf_("    %c %3u %08lx %08lx\n", tr[j].op, (unsigned)tr[j].h, (unsigned long)tr[j].a, (unsigned long)tr[j].t);
    }
    memset(seen, 0, sizeof(seen));
    for (j = (n > 40u) ? n - 40u : 0u; j < n; j++) {
        uint32_t h = tr[j].h, w;
        const volatile uint32_t *rec;
        if (tr[j].op == 'Z' || h >= 256u || seen[h]) continue;
        seen[h] = 1;
        rec = (const volatile uint32_t *)apu_vp_debug_voice_record(h);
        if (!rec) continue;
        logf_("  record %3lu:", (unsigned long)h);
        for (w = 0; w < 32u; w++) logf_("%s%08lx", (w % 8u) ? " " : "\n    ", (unsigned long)rec[w]);
        logf_("\n");
    }
    logf_("  SGE 0..7:");
    for (j = 0; j < 8u; j++) logf_(" %08lx/%08lx", (unsigned long)sge[2u * j], (unsigned long)sge[2u * j + 1u]);
    logf_("\n  registers (non-zero) 1000-1018, 1100-1160, 1300-1344, 1500-1510, 2000-20fc, 3000-307c:");
    for (q = 0; q < 6u; q++) {
        for (a = rng[q][0]; a <= rng[q][1]; a += 4u) {
            uint32_t v = r[a / 4u];
            if (v) {
                logf_("%s%04lx=%08lx", (cnt % 6u) ? " " : "\n    ", (unsigned long)a, (unsigned long)v);
                cnt++;
            }
        }
    }
    logf_("\n  GP X:0..7:");
    for (j = 0; j < 8u; j++) logf_(" %06lx", (unsigned long)(r[(0x30000u + 4u * j) / 4u] & 0xFFFFFFu));
    logf_("  X:$10 %06lx\n", (unsigned long)(r[0x30040u / 4u] & 0xFFFFFFu));
}

/*
 * After a stall: the front end holds an SE2FE_IDLE_VOICE message (FECTL bit 15,
 * FEDECMETH 0x8000, FEDECPARAM = handle) when no method was sent before. Try
 * the ways software might release it, one at a time, and log whether GP
 * frames resume after each.
 */
static void fe_state(const char *step) {
    uint32_t g0 = apu_gp_frames(BAR0), g1;
    Sleep(10);
    g1 = apu_gp_frames(BAR0);
    logf_("  %-34s GP +%-3lu FECTL %08lx METH %08lx PARAM %08lx ISTS %08lx TVL2D %04lx FETF1 %08lx\n", step,
          (unsigned long)((g1 - g0) & 0xFFFFFFu), (unsigned long)REG(MCPX_APU_FECTL),
          (unsigned long)REG(MCPX_APU_FEDECMETH), (unsigned long)REG(MCPX_APU_FEDECPARAM),
          (unsigned long)REG(MCPX_APU_ISTS), (unsigned long)(REG(MCPX_APU_TVL2D) & 0xFFFFu),
          (unsigned long)REG(MCPX_APU_FETFORCE1));
}

static bool frames_run(void) {
    uint32_t g0 = apu_gp_frames(BAR0);
    Sleep(10);
    return apu_gp_frames(BAR0) != g0;
}

/* Take handle h off the 2D list by editing records directly */
static void raw_unlink(uint32_t h) {
    volatile uint32_t *hr = (volatile uint32_t *)apu_vp_debug_voice_record(h);
    uint32_t next = hr[0x7C / 4] & 0xFFFFu, cur = REG(MCPX_APU_TVL2D) & 0xFFFFu, guard = 0;
    if (cur == h) {
        REG(MCPX_APU_TVL2D) = next;
        return;
    }
    while (cur < 256u && guard++ < 256u) {
        volatile uint32_t *l = &((volatile uint32_t *)apu_vp_debug_voice_record(cur))[0x7C / 4];
        if ((*l & 0xFFFFu) == h) {
            *l = (*l & 0xFFFF0000u) | next;
            return;
        }
        cur = *l & 0xFFFFu;
    }
}

static void try_recover(void) {
    uint32_t h = REG(MCPX_APU_FEDECPARAM) & 0xFFFFu, step;
    logf_("  recovery attempts (idle handle %lu):\n", (unsigned long)h);
    fe_state("0 nothing");
    for (step = 1; step <= 8u; step++) {
        const char *name = "";
        switch (step) {
        case 1: name = "1 ISTS = all";               REG(MCPX_APU_ISTS) = 0xFFFFFFFFu; break;
        case 2: name = "2 FECTL &= ~0x8000";         REG(MCPX_APU_FECTL) = REG(MCPX_APU_FECTL) & ~0x8000u; break;
        case 3: name = "3 FECTL |= 0x8000 (w1c?)";   REG(MCPX_APU_FECTL) = REG(MCPX_APU_FECTL) | 0x8000u; break;
        case 4: name = "4 unlink idle voice, FECTL 0"; if (h < 256u) raw_unlink(h);
                REG(MCPX_APU_FECTL) = 0; REG(MCPX_APU_ISTS) = 0xFFFFFFFFu; break;
        case 5: name = "5 FETFORCE1 = 0x8000";       REG(MCPX_APU_FETFORCE1) = 0x8000u; REG(MCPX_APU_FECTL) = 0; break;
        case 6: name = "6 FECTL 0x80 then 0";        REG(MCPX_APU_FECTL) = 0x80u; REG(MCPX_APU_FECTL) = 0; break;
        case 7: name = "7 FECTL 0xE0 then 0";        REG(MCPX_APU_FECTL) = 0xE0u; REG(MCPX_APU_FECTL) = 0; break;
        case 8: name = "8 SECTL 0 then 0x0F";        REG(MCPX_APU_SECTL) = 0; Sleep(2); REG(MCPX_APU_SECTL) = 0x0Fu; break;
        }
        fe_state(name);
        if (frames_run() && frames_run()) {
            uint32_t g0 = apu_gp_frames(BAR0);
            Sleep(500);
            logf_("  RESUMED after step %lu: GP +%lu in 500 ms\n", (unsigned long)step,
                  (unsigned long)((apu_gp_frames(BAR0) - g0) & 0xFFFFFFu));
            fe_state("after 500 ms");
            return;
        }
    }
    logf_("  no step resumed the frames\n");
}

/* Playing voices whose CBO is in the silent tail right now (what the end scan unlinks) */
static uint32_t count_in_tail(void) {
    uint32_t i, c = 0;
    for (i = 0; i < s_qn; i++) {
        if (apu_vp_voice_position(s_q[(s_qh + i) % 256u]) >= PING_FRAMES) c++;
    }
    return c;
}

/* Optional handle range for run() (s_hi = 0: the driver's allocator). A handle
 * stopped in the last two ticks is not reused. */
static uint32_t s_lo, s_hi, s_rr;
static uint8_t s_busy[256];
static uint32_t s_stop_tick[256];

static uint32_t range_alloc(uint32_t tick) {
    uint32_t n = s_hi - s_lo + 1u, k, h;
    for (k = 0; k < n; k++) {
        h = s_lo + (s_rr + k) % n;
        if (s_busy[h] || (s_stop_tick[h] && tick < s_stop_tick[h] + 2u)) continue;
        s_rr = (h - s_lo + 1u) % n;
        return h;
    }
    return 0xFFFFu;
}

/* Start 3 voices per tick through the driver, stop the oldest beyond `target` */
static bool run(const char *tag, uint32_t target, uint32_t tick_ms, uint32_t ms) {
    uint32_t t0 = GetTickCount(), ticks = 0, starts = 0, fails = 0, tail_seen = 0;
    uint32_t g_last = apu_gp_frames(BAR0), x_last = REG(XGSCNT_REG);
    apu_vp_voice_params_t p;

    while (GetTickCount() - t0 < ms) {
        uint32_t k;
        for (k = 0; k < 3u; k++) {
            uint32_t h = s_hi ? range_alloc(ticks + 1u) : apu_vp_alloc_handle(BAR0);
            if (h == 0xFFFFu) {
                fails++;
                break;
            }
            fill_params(&p);
            if (apu_vp_voice_start(BAR0, h, &p) != 0) {
                fails++;
                continue;
            }
            s_q[(s_qh + s_qn) % 256u] = h;
            s_qn++;
            starts++;
            s_busy[h] = 1;
        }
        tail_seen += count_in_tail();
        while (s_qn > target) {
            s_busy[s_q[s_qh]] = 0;
            s_stop_tick[s_q[s_qh]] = ticks + 1u;
            apu_vp_voice_off(BAR0, s_q[s_qh]);
            s_qh = (s_qh + 1u) % 256u;
            s_qn--;
        }
        apu_vp_service(BAR0);   /* end scan and AC97 forwarding, as an application would every frame */
        Sleep(tick_ms);
        ticks++;
        if (apu_gp_frames(BAR0) != g_last) {
            g_last = apu_gp_frames(BAR0);
            x_last = REG(XGSCNT_REG);
        } else if (REG(XGSCNT_REG) - x_last > 512u) {
            static apu_vp_trace_t tr[256];
            uint32_t n = apu_vp_debug_trace(tr, 256u);
            logf_("%-46s STALLED after %lu ticks, %lu starts, %lu playing, %lu in-tail sightings\n", tag, ticks, starts,
                  s_qn, tail_seen);
            dump_stall(tr, n);
            try_recover();
            save_log();
            return false;
        }
    }
    logf_("%-46s ok: %lu ticks, %lu starts, %lu fails, %lu playing, %lu in-tail sightings\n", tag, ticks, starts,
          fails, s_qn, tail_seen);
    stop_all();
    save_log();
    return true;
}

/* The ping, with the silent tail al_buffer.c appends, 0x20 into a page as in the frozen runs */
static bool make_ping(void) {
    uint32_t page_phys = 0, i;
    uint8_t *mem;
    int16_t *ping;
    mem = (uint8_t *)apu_mem_alloc_table(PING_FRAMES * 2u + APU_VP_SILENT_TAIL_BYTES + 0x20u, &page_phys);
    if (!mem) {
        return false;
    }
    ping = (int16_t *)(mem + 0x20u);
    s_ping_phys = page_phys + 0x20u;
    for (i = 0; i < PING_FRAMES; i++) {
        float t = (float)i / 48000.0f;
        ping[i] = (int16_t)(12000.0f * expf(-t * 18.0f) * sinf(2.0f * 3.14159265f * 880.0f * t));
    }
    memset((uint8_t *)ping + PING_FRAMES * 2u, 0, APU_VP_SILENT_TAIL_BYTES);
    return true;
}

int main(void) {
    /*
     * The D1 pattern (3 starts per 16 ms, stop the oldest beyond 40) froze the
     * APU at tick 23 every run: the VP reports handle 129 (and, it seems, any
     * handle above 127) as idle (SE2FE_IDLE_VOICE) and stops for good, with
     * the driver and with raw record writes alike; handles 64..127 ran clean.
     * The driver now keeps hardware voices below APU_VP_HW_HANDLES. This run
     * checks the full driver path (fe_probe, end scan, FE check, AC97 out),
     * safest first; a frozen APU needs a reboot, so the run ends at a stall:
     *   F1  D1, 40 playing, 8 s
     *   F2  60 playing: 64 handles minus the quarantine, so starts may fail
     * (Handles 0..63 as plain voices neither froze nor played: they need the
     * HRTF stage. range_alloc() stays for trying other handle ranges.)
     */
    static const struct {
        const char *tag;
        uint32_t lo, hi, target, ms;
    } v[] = {
        { "F1: driver, 40 playing:", 0, 0, 40, 8000 },
        { "F2: driver, 60 playing:", 0, 0, 60, 8000 },
    };
    uint32_t k;

    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    logf_("apu_vp_stress: handles below 128 (D1 pattern, seed 0x1234567)\n");
    if (!make_ping()) {
        logf_("buffer allocation failed\n");
        save_log();
        for (;;) Sleep(1000);
    }
    for (k = 0; k < sizeof(v) / sizeof(v[0]); k++) {
        uint32_t g0;
        apu_vp_debug_set_flags(0);
        apu_gp_debug_set_dsp_step(32);
        s_lo = v[k].lo;
        s_hi = v[k].hi;
        s_rr = 0;
        memset(s_busy, 0, sizeof(s_busy));
        memset(s_stop_tick, 0, sizeof(s_stop_tick));
        if (apu_vp_init(BAR0) != 0) {
            logf_("%s apu_vp_init failed\n", v[k].tag);
            break;
        }
        g0 = apu_gp_frames(BAR0);
        Sleep(50);
        logf_("-- %s GP program %s, GP frames +%lu in 50 ms after init, front end %s\n", v[k].tag,
              apu_gp_program_loaded() ? "loaded" : "NOT LOADED", (unsigned long)((apu_gp_frames(BAR0) - g0) & 0xFFFFFFu),
              apu_vp_hrtf_available() ? "works" : "stalled (real console)");
        s_rng = 0x1234567u;
        s_qh = s_qn = 0;
        if (!run(v[k].tag, v[k].target, 16, v[k].ms)) {
            break;   /* frozen: only a reboot brings the APU back */
        }
        stop_all();
        apu_vp_deinit(BAR0);
        Sleep(20);
    }
    logf_("done\n");
    save_log();
    for (;;) Sleep(1000);
    return 0;
}
