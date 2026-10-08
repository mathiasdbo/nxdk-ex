/*
 * MCPX APU Voice Processor bring-up, stage 1 of route A in
 * lib/openal/docs/XEMU_VERIFICATION.md (section 8.2): one PCM16 mono voice,
 * no DSP code. Follows the bring-up sequence of section 7.2, steps 1-13 and
 * 17-24, using the register model in lib/openal/src/mcpx_apu_regs.h.
 *
 * Audible on xemu with "Real-time DSP processing" off (the default): the
 * emulator then taps the Voice Processor output directly. On a real console
 * the VP only fills mixbins, which the GP/EP DSPs must turn into output, so
 * this stage is silent there; it still reports whether the VP ran.
 *
 * The test cycles through: a one-shot at 48 kHz, the same buffer pitched up
 * a fifth, and a loop an octave down that is switched off with VOICE_OFF.
 * For each it checks CBO progress, ACTIVE_VOICE, the notifier byte and the
 * idle-voice trap / unlink.
 */
#include <hal/debug.h>
#include <hal/video.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include "mcpx_apu_regs.h"

#define VOICE        64u                       /* plain voice: handles >= 64 skip HRTF */
#define MAX_HANDLES  256u
#define RATE         48000u
#define TONE_FRAMES  48000u                    /* 1 s */
#define TAIL_FRAMES  64u                       /* trailing silence (xemu drops a one-shot's last frame) */
#define BUF_FRAMES   (TONE_FRAMES + TAIL_FRAMES)

static volatile uint8_t *s_bar0;
static uint32_t s_bar0_phys;
static uint32_t s_pci_id;

static uint8_t *s_voices;      static uint32_t s_voices_phys;
static uint32_t *s_sge;        static uint32_t s_sge_phys;
static uint8_t *s_ssl;         static uint32_t s_ssl_phys;
static uint8_t *s_notify;      static uint32_t s_notify_phys;
static int16_t *s_pcm;         static uint32_t s_pcm_phys;

static uint32_t s_traps;
static uint32_t s_unlinks;

static inline void reg_wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(s_bar0 + off) = v; }
static inline uint32_t reg_rd(uint32_t off) { return *(volatile uint32_t *)(s_bar0 + off); }
static inline void method(uint32_t m, uint32_t v) { reg_wr(MCPX_APU_METHOD_BASE + m, v); }

static inline volatile uint32_t *voice_dw(uint32_t h, uint32_t off) {
    return (volatile uint32_t *)(s_voices + h * MCPX_VOICE_SIZE + off);
}

static void *alloc_phys(uint32_t size, uint32_t *phys) {
    void *p = MmAllocateContiguousMemoryEx(size, 0, 0x03FFAFFF, 4096,
                                           PAGE_READWRITE | PAGE_WRITECOMBINE);
    if (p) {
        memset(p, 0, size);
        *phys = (uint32_t)MmGetPhysicalAddress(p);
    }
    return p;
}

/* Step 1: find BAR0 through PCI config space (0:5.0) */
static bool find_apu(void) {
    uint32_t bar0 = 0;
    HalReadWritePCISpace(MCPX_APU_PCI_BUS, MCPX_APU_PCI_SLOT, 0x00, &s_pci_id, 4, FALSE);
    HalReadWritePCISpace(MCPX_APU_PCI_BUS, MCPX_APU_PCI_SLOT, 0x10, &bar0, 4, FALSE);
    bar0 &= ~0xFu;
    if (s_pci_id != MCPX_APU_PCI_ID || bar0 == 0) {
        bar0 = MCPX_APU_BAR0_DEFAULT;
    }
    s_bar0_phys = bar0;
    /* The kernel maps the 0xFD000000+ device range one to one */
    if (bar0 >= 0xFD000000u) {
        s_bar0 = (volatile uint8_t *)bar0;
    } else {
        s_bar0 = (volatile uint8_t *)MmMapIoSpace(bar0, MCPX_APU_BAR0_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    }
    return s_bar0 != NULL;
}

static bool bring_up(void) {
    uint32_t pages = (BUF_FRAMES * 2u + 4095u) / 4096u, k, i;

    /* Step 4: zero-filled tables and the sample buffer */
    s_voices = alloc_phys(MAX_HANDLES * MCPX_VOICE_SIZE, &s_voices_phys);
    s_sge = alloc_phys(4096, &s_sge_phys);
    s_ssl = alloc_phys(4096, &s_ssl_phys);
    s_notify = alloc_phys(16u * (2u + 4u * MAX_HANDLES), &s_notify_phys);
    s_pcm = alloc_phys(pages * 4096u, &s_pcm_phys);
    if (!s_voices || !s_sge || !s_ssl || !s_notify || !s_pcm || pages > 512u) {
        return false;
    }

    /* 440 Hz with 10 ms fades, then silence */
    for (i = 0; i < TONE_FRAMES; i++) {
        float env = 1.0f;
        if (i < 480u) env = (float)i / 480.0f;
        if (i > TONE_FRAMES - 480u) env = (float)(TONE_FRAMES - i) / 480.0f;
        s_pcm[i] = (int16_t)(9000.0f * env * sinf(2.0f * 3.14159265f * 440.0f * (float)i / (float)RATE));
    }

    /* Step 18: one SGE entry per 4 KiB page of the (contiguous) buffer */
    for (k = 0; k < pages; k++) {
        s_sge[2u * k] = s_pcm_phys + k * 4096u;
        s_sge[2u * k + 1u] = 0;
    }

    /* Step 6: quiesce */
    reg_wr(MCPX_APU_SECTL, 0);
    reg_wr(MCPX_APU_FECTL, 0);
    reg_wr(MCPX_APU_GPRST, 0);
    reg_wr(MCPX_APU_EPRST, 0);
    reg_wr(MCPX_APU_IEN, 0);
    reg_wr(MCPX_APU_ISTS, 0xFFFFFFFFu);

    /* Step 7: table bases */
    reg_wr(MCPX_APU_VPVADDR, s_voices_phys);
    reg_wr(MCPX_APU_VPSGEADDR, s_sge_phys);
    reg_wr(MCPX_APU_VPSSLADDR, s_ssl_phys);

    /* Step 9: empty voice lists */
    reg_wr(MCPX_APU_TVL2D, MCPX_APU_LIST_END);
    reg_wr(MCPX_APU_TVL3D, MCPX_APU_LIST_END);
    reg_wr(MCPX_APU_TVLMP, MCPX_APU_LIST_END);

    /* Steps 10-11: idle-voice trap and notifier area (interrupts stay off: we poll) */
    reg_wr(MCPX_APU_FETFORCE1, MCPX_APU_FETFORCE1_SE2FE_IDLE_VOICE);
    reg_wr(MCPX_APU_FENADDR, s_notify_phys);

    /* Step 17: start frame generation */
    reg_wr(MCPX_APU_SECTL, 0x00000008u);
    return true;
}

/* Remove voice h from whichever list holds it (step 23) */
static void unlink_voice(uint32_t h) {
    static const uint32_t heads[3] = { MCPX_APU_TVL2D, MCPX_APU_TVL3D, MCPX_APU_TVLMP };
    uint32_t next = *voice_dw(h, MCPX_VOICE_TAR_PITCH_LINK) & 0xFFFFu;
    int l;

    for (l = 0; l < 3; l++) {
        uint32_t cur = reg_rd(heads[l]) & 0xFFFFu, guard = 0;
        if (cur == h) {
            reg_wr(heads[l], next);
            s_unlinks++;
            return;
        }
        while (cur != MCPX_APU_LIST_END && cur < MAX_HANDLES && guard++ < MAX_HANDLES) {
            volatile uint32_t *link = voice_dw(cur, MCPX_VOICE_TAR_PITCH_LINK);
            uint32_t n = *link & 0xFFFFu;
            if (n == h) {
                *link = (*link & 0xFFFF0000u) | next;   /* keep the pitch bits */
                s_unlinks++;
                return;
            }
            cur = n;
        }
    }
}

/* Step 23: service the idle-voice trap */
static void service_trap(void) {
    if (reg_rd(MCPX_APU_FECTL) & MCPX_APU_FECTL_FEMETHMODE) {
        uint32_t h = reg_rd(MCPX_APU_FEDECPARAM) & 0xFFFFu;   /* read before any other method */
        s_traps++;
        if (h < MAX_HANDLES && !(*voice_dw(h, MCPX_VOICE_PAR_STATE) & MCPX_PAR_STATE_ACTIVE_VOICE)) {
            unlink_voice(h);
        }
        reg_wr(MCPX_APU_FECTL, 0);
        reg_wr(MCPX_APU_ISTS, MCPX_APU_ISTS_FETINTSTS);
    }
}

static bool voice_linked(uint32_t h) {
    static const uint32_t heads[3] = { MCPX_APU_TVL2D, MCPX_APU_TVL3D, MCPX_APU_TVLMP };
    int l;
    for (l = 0; l < 3; l++) {
        uint32_t cur = reg_rd(heads[l]) & 0xFFFFu, guard = 0;
        while (cur != MCPX_APU_LIST_END && cur < MAX_HANDLES && guard++ < MAX_HANDLES) {
            if (cur == h) {
                return true;
            }
            cur = *voice_dw(cur, MCPX_VOICE_TAR_PITCH_LINK) & 0xFFFFu;
        }
    }
    return false;
}

static uint8_t *notifier_byte(uint32_t h) {
    return s_notify + 16u * (2u + 4u * h) + 15u;
}

/* Steps 19-20: configure and start a buffer voice. pitch = 4.12 log2 of the rate ratio */
static void voice_start(uint32_t h, int16_t pitch, bool loop) {
    *notifier_byte(h) = 0;
    method(MCPX_METH_SET_CURRENT_VOICE, h);
    method(MCPX_METH_CFG_VBIN, 0x00000020u);            /* V0BIN = 0, V1BIN = 1 */
    method(MCPX_METH_CFG_FMT, MCPX_FMT_SAMPLE_S16 | MCPX_FMT_CONTAINER_B16 | (loop ? MCPX_FMT_LOOP : 0u));
    method(MCPX_METH_CFG_ENV0, 0);
    method(MCPX_METH_CFG_ENVA, 0);
    method(MCPX_METH_CFG_ENV1, 0);
    method(MCPX_METH_CFG_ENVF, 0);
    method(MCPX_METH_CFG_MISC, 0);
    method(MCPX_METH_TAR_HRTF, 0x0000FFFFu);
    method(MCPX_METH_TAR_VOLA, 0x000F000Fu);            /* outputs 0, 1 at 0 dB */
    method(MCPX_METH_TAR_VOLB, 0xFFFFFFFFu);            /* outputs 2-7 muted */
    method(MCPX_METH_TAR_VOLC, 0xFFFFFFFFu);
    method(MCPX_METH_LFO_ENV, 0);
    method(MCPX_METH_TAR_FCA, 0);
    method(MCPX_METH_TAR_FCB, 0);
    method(MCPX_METH_TAR_PITCH, (uint32_t)(uint16_t)pitch << 16);
    method(MCPX_METH_CFG_BUF_BASE, s_pcm_phys & 0xFFFu); /* page 0 of the SGE space */
    method(MCPX_METH_CFG_BUF_LBO, 0);
    method(MCPX_METH_CFG_BUF_EBO, (loop ? TONE_FRAMES : BUF_FRAMES) - 1u);
    method(MCPX_METH_SET_ANTECEDENT_VOICE, MCPX_ANTECEDENT_LIST_2D_TOP);
    method(MCPX_METH_VOICE_ON, h);
}

typedef struct {
    const char *name;
    int16_t pitch;
    bool loop;
    uint32_t max_cbo;
    bool cbo_moved, went_inactive, notified, unlinked, done, pass;
} test_step_t;

static test_step_t s_steps[3] = {
    { "one-shot 440 Hz (pitch 0)",           0,     false, 0, false, false, false, false, false, false },
    { "one-shot +7 semitones (pitch 2389)",  2389,  false, 0, false, false, false, false, false, false },
    { "loop -1 octave, VOICE_OFF at 3 s",    -4096, true,  0, false, false, false, false, false, false },
};

static void show(int step, uint32_t elapsed_ms, uint32_t round) {
    uint32_t state = *voice_dw(VOICE, MCPX_VOICE_PAR_STATE);
    int i;

    debugClearScreen();
    debugPrint("MCPX APU Voice Processor bring-up (route A, stage 1)\n\n");
    debugPrint("PCI 0:5.0 id %08lx  BAR0 %08lx\n", (unsigned long)s_pci_id, (unsigned long)s_bar0_phys);
    debugPrint("SECTL %08lx  FECTL %08lx  ISTS %08lx  TVL2D %04lx\n",
               (unsigned long)reg_rd(MCPX_APU_SECTL), (unsigned long)reg_rd(MCPX_APU_FECTL),
               (unsigned long)reg_rd(MCPX_APU_ISTS), (unsigned long)(reg_rd(MCPX_APU_TVL2D) & 0xFFFFu));
    debugPrint("voice %u: CBO %6lu  ACTIVE %d  PAUSED %d  linked %d  notifier %u\n",
               VOICE, (unsigned long)(*voice_dw(VOICE, MCPX_VOICE_PAR_OFFSET) & 0xFFFFFFu),
               (state & MCPX_PAR_STATE_ACTIVE_VOICE) ? 1 : 0, (state & MCPX_PAR_STATE_PAUSED) ? 1 : 0,
               voice_linked(VOICE) ? 1 : 0, (unsigned)*notifier_byte(VOICE));
    debugPrint("traps serviced %lu  unlinks %lu  round %lu\n\n",
               (unsigned long)s_traps, (unsigned long)s_unlinks, (unsigned long)round);

    for (i = 0; i < 3; i++) {
        test_step_t *t = &s_steps[i];
        debugPrint("%s %-36s ", (i == step) ? ">" : " ", t->name);
        if (!t->done && i != step) {
            debugPrint("-\n");
            continue;
        }
        debugPrint("CBO max %6lu %s%s%s%s %s\n", (unsigned long)t->max_cbo,
                   t->cbo_moved ? "moved " : "",
                   t->went_inactive ? "off " : "",
                   t->notified ? "notified " : "",
                   t->unlinked ? "unlinked" : "",
                   t->done ? (t->pass ? "PASS" : "FAIL") : "...");
    }
    debugPrint("\nstep time %lu ms\n", (unsigned long)elapsed_ms);
    debugPrint("\nHeard on xemu with real-time DSP off. A real console needs\n"
               "GP/EP firmware to output the mixbins (later stage).\n");
}

int main(void) {
    uint32_t round = 0;

    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);

    if (!find_apu() || !bring_up()) {
        debugPrint("APU bring-up failed (BAR0 %08lx)\n", (unsigned long)s_bar0_phys);
        for (;;) Sleep(1000);
    }

    for (;;) {
        int step;
        round++;
        for (step = 0; step < 3; step++) {
            test_step_t *t = &s_steps[step];
            DWORD start = GetTickCount(), last_show = 0;
            bool off_sent = false;

            t->max_cbo = 0;
            t->cbo_moved = t->went_inactive = t->notified = t->unlinked = t->done = t->pass = false;
            voice_start(VOICE, t->pitch, t->loop);

            for (;;) {
                DWORD now = GetTickCount() - start;
                uint32_t cbo = *voice_dw(VOICE, MCPX_VOICE_PAR_OFFSET) & 0xFFFFFFu;
                bool active = (*voice_dw(VOICE, MCPX_VOICE_PAR_STATE) & MCPX_PAR_STATE_ACTIVE_VOICE) != 0;

                service_trap();
                if (cbo > t->max_cbo) t->max_cbo = cbo;
                if (cbo > 0) t->cbo_moved = true;
                if (t->loop && !off_sent && now > 3000) {
                    method(MCPX_METH_VOICE_OFF, VOICE);     /* step 24 */
                    off_sent = true;
                }
                if (!active && (t->cbo_moved || now > 200)) t->went_inactive = true;
                if (*notifier_byte(VOICE)) t->notified = true;
                if (t->went_inactive && !voice_linked(VOICE)) t->unlinked = true;

                if (now - last_show >= 100) {
                    show(step, now, round);
                    last_show = now;
                }
                if ((t->went_inactive && t->unlinked) || now > 8000) {
                    break;
                }
                Sleep(2);
            }

            t->done = true;
            t->pass = t->cbo_moved && t->went_inactive && t->unlinked &&
                      (t->loop || t->max_cbo > TONE_FRAMES / 2u);
            show(step, GetTickCount() - start, round);
            Sleep(700);
        }
    }
    return 0;
}
