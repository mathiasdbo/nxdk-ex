#include "apu_gp.h"
#include "apu_gp_reverb.h"
#include "apu_reverb.h"
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
#define SCRATCH_MAX_BYTES (512u * 4096u)  /* one 4 KiB SGE table page: 512 entries */
#define CLEAR_X_END       0x40u           /* X:$10..$3F (counter, program parameters) start at 0 */
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
/* The program: dsp/gp_reverb.asm, the copy program until its reverb is switched on */
static const uint32_t *s_image = gp_reverb_words;    /* apu_gp_debug_set_program() */
static uint32_t s_image_words = GP_REVERB_WORDS;
static uint32_t s_scratch_req;                       /* apu_gp_debug_set_scratch_bytes(); 0 = what the image needs */
static uint32_t s_scratch_bytes = SCRATCH_BYTES;     /* in use */
static bool s_reverb_on;
static uint32_t s_reverb_pre;                        /* pre-delay line length laid out */

void apu_gp_debug_set_program(const uint32_t *words, uint32_t count) {
    if (words && count > 0u && count <= SCRATCH_BYTES / 4u) {
        s_image = words;
        s_image_words = count;
    } else {
        s_image = gp_reverb_words;
        s_image_words = GP_REVERB_WORDS;
    }
}

void apu_gp_debug_set_scratch_bytes(uint32_t bytes) {
    if (bytes == 0u) {
        s_scratch_req = 0;
        return;
    }
    bytes = (bytes + 4095u) & ~4095u;
    if (bytes < SCRATCH_BYTES) bytes = SCRATCH_BYTES;
    if (bytes > SCRATCH_MAX_BYTES) bytes = SCRATCH_MAX_BYTES;
    s_scratch_req = bytes;
}

uint32_t apu_gp_scratch_bytes(void) {
    return s_ready ? s_scratch_bytes : 0;
}

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
        for (i = 0; i < s_image_words; i++) {
            if ((reg_rd(bar0, MCPX_APU_GP_PMEM + 4u * i) & 0xFFFFFFu) != (s_image[i] & 0xFFFFFFu)) {
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
    s_scratch_bytes = s_scratch_req ? s_scratch_req
                    : (s_image == gp_reverb_words ? (APU_REVERB_SCRATCH_BASE + apu_reverb_scratch_bytes() + 4095u) & ~4095u
                                                  : SCRATCH_BYTES);
    s_reverb_on = false;
    s_scratch = (uint32_t *)apu_mem_alloc_table(s_scratch_bytes, &s_scratch_phys);
    s_scratch_sge = (uint32_t *)apu_mem_alloc_table(4096, &s_scratch_sge_phys);
    s_fifo = (uint8_t *)apu_mem_alloc_table(APU_GP_FIFO_BYTES, &s_fifo_phys);
    s_fifo_sge = (uint32_t *)apu_mem_alloc_table(4096, &s_fifo_sge_phys);
    if (!s_scratch || !s_scratch_sge || !s_fifo || !s_fifo_sge) {
        free_all();
        return -1;
    }

    /* Bootstrap image: P:0..$7FF as little-endian 32-bit words, top byte 0; the
     * rest stays 0 (NOP), the program never runs into it */
    memset(s_scratch, 0, s_scratch_bytes);
    for (i = 0; i < s_image_words; i++) {
        s_scratch[i] = s_image[i] & 0xFFFFFFu;
    }
    memset(s_fifo, 0, APU_GP_FIFO_BYTES);

    /* SGE tables: one entry per 4 KiB page (dword 0 = physical page) */
    memset(s_scratch_sge, 0, 4096);
    pages = s_scratch_bytes / 4096u;
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
    for (i = FRAME_COUNTER_X; i < CLEAR_X_END; i++) {
        reg_wr(bar0, MCPX_APU_GP_XMEM + 4u * i, 0);   /* frame counter and the program's parameters */
    }
    s_ready = true;
    return 0;
}

void apu_gp_deinit(uintptr_t bar0) {
    if (!s_ready) {
        return;
    }
    reg_wr(bar0, MCPX_APU_GPRST, 0);
    s_ready = false;
    s_reverb_on = false;
    free_all();
}

/* ---------------------------------------------------------------- reverb */

#define REVERB_FLAGS_X  0x11u   /* gp_reverb.asm FLAGS, bit 0 */

/* Let the frame in progress finish its DMA chains (on the host nothing runs) */
static void wait_frames(uintptr_t bar0, uint32_t n) {
#if defined(OPENAL_TARGET_XBOX)
    uint32_t f0 = apu_gp_frames(bar0), spin;
    for (spin = 0; spin < 40000u; spin++) {   /* about 20 ms of register reads */
        if (((apu_gp_frames(bar0) - f0) & 0xFFFFFFu) >= n) {
            return;
        }
    }
#else
    (void)bar0;
    (void)n;
#endif
}

static void write_words(uintptr_t bar0, const apu_reverb_word_t *w, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n && i < APU_REVERB_MAX_WORDS; i++) {
        reg_wr(bar0, MCPX_APU_GP_XMEM + 4u * w[i].addr, w[i].value);
    }
}

int apu_gp_reverb_set(uintptr_t bar0, const apu_reverb_params_t *p, bool on) {
    static apu_reverb_word_t words[APU_REVERB_MAX_WORDS];
    uint32_t len[APU_REVERB_LINES], n;

    if (!s_ready || s_image != gp_reverb_words || !p) {
        return -1;
    }
    if (!on) {
        if (s_reverb_on) {
            reg_wr(bar0, MCPX_APU_GP_XMEM + 4u * REVERB_FLAGS_X, 0);
            s_reverb_on = false;
        }
        return 0;
    }
    apu_reverb_line_lengths(p, len);
    if (s_reverb_on && len[0] == s_reverb_pre) {
        /* Same delay lines: only the gains change, word by word while it runs */
        write_words(bar0, words, apu_reverb_coefficients(p, words));
        return 0;
    }
    if (s_reverb_on) {
        reg_wr(bar0, MCPX_APU_GP_XMEM + 4u * REVERB_FLAGS_X, 0);
        s_reverb_on = false;
    }
    /* Off for a frame so no chain is in flight, then the layout, empty lines, on */
    wait_frames(bar0, 2u);
    n = apu_reverb_layout(p, words);
    write_words(bar0, words, n);
    memset((uint8_t *)s_scratch + APU_REVERB_SCRATCH_BASE, 0, apu_reverb_scratch_bytes());
    reg_wr(bar0, MCPX_APU_GP_XMEM + 4u * REVERB_FLAGS_X, 1u);
    s_reverb_on = true;
    s_reverb_pre = len[0];
    return 0;
}

bool apu_gp_reverb_on(void) {
    return s_ready && s_reverb_on;
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
