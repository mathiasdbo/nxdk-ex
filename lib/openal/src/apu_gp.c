#include "apu_gp.h"
#include "apu_mem.h"
#include "apu_platform.h"
#include "mcpx_apu_regs.h"
#include <string.h>

/* DSP56300 words, see apu_gp.h */
const uint32_t apu_gp_program[APU_GP_PROGRAM_WORDS] = {
    0x08F484u, 0x000001u,   /* movep #1,x:$FFFFC4  (frame complete) */
    0x0A8581u, 0x000002u,   /* jclr #1,x:$FFFFC5,$2 (wait for START_FRAME) */
    0x08F485u, 0x000002u,   /* movep #2,x:$FFFFC5  (acknowledge) */
    0x08F494u, 0x000000u,   /* movep #0,x:$FFFFD4  (NEXT_BLOCK) */
    0x08F496u, 0x000001u,   /* movep #1,x:$FFFFD6  (CONTROL = start) */
    0x000008u,              /* inc a */
    0x501000u,              /* move a0,x:$10       (frame counter; 01dd0ddd W0aaaaaa, a0 = register 8) */
    0x0C0000u,              /* jmp $0000 */
};

#define FRAME_COUNTER_X   0x10u

/* DMA descriptor at X:0 */
#define DESC_EOL          0x004000u   /* next pointer: end of list */
#define DESC_INTERLEAVE   0x000001u
#define DESC_TO_MEMORY    0x000002u
#define DESC_BUF_FIFO0    (0u << 5)
#define DESC_FMT_16BIT    (1u << 10)  /* 24-bit sample >> 8 */
/* Distance in DSP words between the channels of an interleaved transfer
 * (control bits 23:14). xemu ignores it and steps by the block count; a real
 * console needs it: with 0 every channel reads bin 0 (apu_probe rounds 20/21) */
#define DESC_DSP_STEP(n)  ((uint32_t)(n) << 14)

static uint32_t s_dsp_step = APU_GP_FRAME;   /* apu_gp_debug_set_dsp_step() */

void apu_gp_debug_set_dsp_step(uint32_t words) {
    s_dsp_step = words;
}
#define MIXBIN_X_BASE     0x1400u     /* bin n at X:$1400 + 32 n */

#define SCRATCH_BYTES     (0x800u * 4u)   /* bootstrap image: P:0..$7FF */
#define FIFO_FRAMES       (APU_GP_FIFO_BYTES / 4u)

static uint32_t *s_scratch;  static uint32_t s_scratch_phys;
static uint32_t *s_scratch_sge; static uint32_t s_scratch_sge_phys;
static uint8_t *s_fifo;      static uint32_t s_fifo_phys;
static uint32_t *s_fifo_sge; static uint32_t s_fifo_sge_phys;
static uint32_t s_read;       /* byte offset of the next unread frame */
static uint32_t s_frames_read;
static uint32_t s_resyncs;
static bool s_ready;
static bool s_loaded;

static inline void reg_wr(uintptr_t bar0, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(bar0 + off) = v;
}

static inline uint32_t reg_rd(uintptr_t bar0, uint32_t off) {
    return *(volatile uint32_t *)(bar0 + off);
}

/*
 * On hardware, check that the bootstrap put our program in P memory (the P
 * window is readable while the core runs) and retry the load if not; the host
 * test's plain-memory BAR0 has no bootstrap, so there it is assumed.
 */
static bool program_loaded(uintptr_t bar0) {
#if defined(OPENAL_TARGET_XBOX)
    uint32_t attempt, i, spin;
    for (attempt = 0; attempt < 3u; attempt++) {
        bool ok = true;
        for (spin = 0; spin < 2000u; spin++) {
            (void)reg_rd(bar0, MCPX_APU_GPRST);      /* ~2 ms for the bootstrap DMA */
        }
        for (i = 0; i < APU_GP_PROGRAM_WORDS; i++) {
            if ((reg_rd(bar0, MCPX_APU_GP_PMEM + 4u * i) & 0xFFFFFFu) != apu_gp_program[i]) {
                ok = false;
            }
        }
        if (ok) {
            return true;
        }
        reg_wr(bar0, MCPX_APU_GPRST, 0);
        reg_wr(bar0, MCPX_APU_GPRST, 3u);
    }
    return false;
#else
    (void)bar0;
    return true;
#endif
}

static void free_all(void) {
    if (s_scratch) apu_mem_free_phys(s_scratch);
    if (s_scratch_sge) apu_mem_free_phys(s_scratch_sge);
    if (s_fifo) apu_mem_free_phys(s_fifo);
    if (s_fifo_sge) apu_mem_free_phys(s_fifo_sge);
    s_scratch = s_scratch_sge = s_fifo_sge = NULL;
    s_fifo = NULL;
}

int apu_gp_init(uintptr_t bar0) {
    static const uint32_t desc[7] = {
        DESC_EOL,
        DESC_INTERLEAVE | DESC_TO_MEMORY | DESC_BUF_FIFO0 | DESC_FMT_16BIT | DESC_DSP_STEP(APU_GP_FRAME),
        (APU_GP_FRAME << 4) | (2u - 1u),     /* 32 frames, 2 channels */
        MIXBIN_X_BASE,                       /* bins 0 and 1 */
        0u, 0u, 0u,                          /* scratch offset/base/size: unused for FIFOs */
    };
    uint32_t i, pages;

    if (s_ready) {
        return 0;
    }
    s_scratch = (uint32_t *)apu_mem_alloc_table(SCRATCH_BYTES, &s_scratch_phys);
    s_scratch_sge = (uint32_t *)apu_mem_alloc_table(4096, &s_scratch_sge_phys);
    s_fifo = (uint8_t *)apu_mem_alloc_table(APU_GP_FIFO_BYTES, &s_fifo_phys);
    s_fifo_sge = (uint32_t *)apu_mem_alloc_table(4096, &s_fifo_sge_phys);
    if (!s_scratch || !s_scratch_sge || !s_fifo || !s_fifo_sge) {
        free_all();
        return -1;
    }

    /* Bootstrap image: P:0..$7FF as little-endian 32-bit words, top byte 0; the
     * rest stays 0 (NOP), the program never runs into it */
    memset(s_scratch, 0, SCRATCH_BYTES);
    for (i = 0; i < sizeof(apu_gp_program) / sizeof(apu_gp_program[0]); i++) {
        s_scratch[i] = apu_gp_program[i] & 0xFFFFFFu;
    }
    memset(s_fifo, 0, APU_GP_FIFO_BYTES);

    /* SGE tables: one entry per 4 KiB page (dword 0 = physical page) */
    memset(s_scratch_sge, 0, 4096);
    pages = SCRATCH_BYTES / 4096u;
    for (i = 0; i < pages; i++) {
        s_scratch_sge[2u * i] = s_scratch_phys + i * 4096u;
    }
    memset(s_fifo_sge, 0, 4096);
    for (i = 0; i < APU_GP_FIFO_BYTES / 4096u; i++) {
        s_fifo_sge[2u * i] = s_fifo_phys + i * 4096u;
    }

    /* Hold the GP in reset while it is configured */
    reg_wr(bar0, MCPX_APU_GPRST, 0);

    /* Step 8: table bases and highest valid entries (inclusive) */
    reg_wr(bar0, MCPX_APU_GPSADDR, s_scratch_sge_phys);
    reg_wr(bar0, MCPX_APU_GPSMAXSGE, pages - 1u);
    reg_wr(bar0, MCPX_APU_GPFADDR, s_fifo_sge_phys);
    reg_wr(bar0, MCPX_APU_GPFMAXSGE, APU_GP_FIFO_BYTES / 4096u - 1u);

    /* Step 15: output FIFO 0, base <= cur < end */
    reg_wr(bar0, MCPX_APU_GPOFBASE0, 0);
    reg_wr(bar0, MCPX_APU_GPOFEND0, APU_GP_FIFO_BYTES);
    reg_wr(bar0, MCPX_APU_GPOFCUR0, 0);
    s_read = 0;
    s_frames_read = 0;
    s_resyncs = 0;

    /* Step 14: 0 -> 3 runs the bootstrap (P:0..$7FF from scratch offset 0) */
    reg_wr(bar0, MCPX_APU_GPRST, 3u);
    s_loaded = program_loaded(bar0);

    /* The descriptor lives in X memory. On hardware the X window ignores writes
     * while the core is held in reset (RST = 0), so it is written after the
     * release; the program waits for the first START_FRAME, and frame
     * generation only starts after this returns. */
    for (i = 0; i < 7u; i++) {
        reg_wr(bar0, MCPX_APU_GP_XMEM + 4u * i,
               (i == 1u) ? ((desc[1] & ~DESC_DSP_STEP(0x3FFu)) | DESC_DSP_STEP(s_dsp_step)) : desc[i]);
    }
    reg_wr(bar0, MCPX_APU_GP_XMEM + 4u * FRAME_COUNTER_X, 0);
    s_ready = true;
    return 0;
}

void apu_gp_deinit(uintptr_t bar0) {
    if (!s_ready) {
        return;
    }
    reg_wr(bar0, MCPX_APU_GPRST, 0);
    s_ready = false;
    free_all();
}

bool apu_gp_ready(void) {
    return s_ready;
}

bool apu_gp_program_loaded(void) {
    return s_ready && s_loaded;
}

static uint32_t fifo_cur(uintptr_t bar0) {
    return (reg_rd(bar0, MCPX_APU_GPOFCUR0) & 0xFFFFFFu) % APU_GP_FIFO_BYTES;
}

uint32_t apu_gp_fifo_available(uintptr_t bar0) {
    if (!s_ready) {
        return 0;
    }
    return ((fifo_cur(bar0) + APU_GP_FIFO_BYTES - s_read) % APU_GP_FIFO_BYTES) / 4u;
}

uint32_t apu_gp_fifo_read(uintptr_t bar0, int16_t *out, uint32_t frames) {
    uint32_t avail, n, i;

    if (!s_ready || !out) {
        return 0;
    }
    avail = apu_gp_fifo_available(bar0);
    if (avail > FIFO_FRAMES - FIFO_FRAMES / 8u) {
        /* The writer is about to lap the reader (the reader stalled): jump
         * close behind the writer and continue from there */
        s_read = (fifo_cur(bar0) + APU_GP_FIFO_BYTES / 2u) % APU_GP_FIFO_BYTES;   /* half a ring behind */
        s_resyncs++;
        avail = apu_gp_fifo_available(bar0);
    }
    n = (frames < avail) ? frames : avail;
    for (i = 0; i < n; i++) {
        const int16_t *src = (const int16_t *)(s_fifo + s_read);
        out[2u * i] = src[0];
        out[2u * i + 1u] = src[1];
        s_read = (s_read + 4u) % APU_GP_FIFO_BYTES;
    }
    s_frames_read += n;
    return n;
}

void apu_gp_fifo_discard(uintptr_t bar0) {
    if (s_ready) {
        s_read = fifo_cur(bar0) & ~3u;
    }
}

uint32_t apu_gp_frames(uintptr_t bar0) {
    return s_ready ? (reg_rd(bar0, MCPX_APU_GP_XMEM + 4u * FRAME_COUNTER_X) & 0xFFFFFFu) : 0;
}

void apu_gp_get_stats(uintptr_t bar0, apu_gp_stats_t *out) {
    if (!out) {
        return;
    }
    out->fifo_cur = s_ready ? fifo_cur(bar0) : 0;
    out->frames_read = s_frames_read;
    out->resyncs = s_resyncs;
}

uint8_t *apu_gp_debug_fifo(void) {
    return s_ready ? s_fifo : NULL;
}

const uint32_t *apu_gp_debug_scratch(void) {
    return s_ready ? s_scratch : NULL;
}
