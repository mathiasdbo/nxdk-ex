#include "apu_gp.h"
#include "apu_gp_kfreeverb.h"
#include "apu_gp_loop.h"
#include "apu_reverb.h"
#include "apu_mem.h"
#include "apu_platform.h"
#include "mcpx_apu_regs.h"
#include <string.h>
#if defined(OPENAL_TARGET_XBOX)
#include <xboxkrnl/xboxkrnl.h>
#endif

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

static uint32_t *s_scratch;  static uint32_t s_scratch_phys;
static uint32_t *s_scratch_sge; static uint32_t s_scratch_sge_phys;
static uint8_t *s_fifo;      static uint32_t s_fifo_phys;
static uint32_t s_fifo_req = APU_GP_FIFO_BYTES;    /* apu_gp_set_fifo_bytes() */
static uint32_t s_fifo_bytes = APU_GP_FIFO_BYTES;  /* in use */
static uint32_t *s_fifo_sge; static uint32_t s_fifo_sge_phys;
static uint32_t s_read;       /* byte offset of the next unread frame */
static uint32_t s_frames_read;
static uint32_t s_resyncs;
static bool s_ready;
static bool s_loaded;
/*
 * The program: the frame loop (dsp/gp_loop.asm) at P:0; the copy program until
 * a slot runs a kernel, which the slots load into P:$100..$7FF (apu_gp_kernel_set,
 * apu_gp_reverb_set). s_image NULL means this default image.
 */
typedef char apu_gp_image_check[(GP_LOOP_WORDS <= GP_LOOP_KERNEL &&
                                 GP_LOOP_MBOX + 2u * GP_LOOP_MB_MAX <= GP_LOOP_SLOTX &&
                                 GP_LOOP_CHAIN < GP_LOOP_KX_BASE &&
                                 GP_KFREEVERB_K_SCRATCH_BYTES + APU_REVERB_SCRATCH_BASE <= APU_GP_SCRATCH_DEFAULT) ? 1 : -1];
static const uint32_t *s_image;                      /* apu_gp_debug_set_program(); NULL = the default */
static uint32_t s_image_words;
static uint32_t s_mb_seq;                            /* the last mailbox sequence number posted */

static const uint32_t *image_words(uint32_t *count) {
    if (s_image) {
        *count = s_image_words;
        return s_image;
    }
    *count = GP_LOOP_WORDS;
    return gp_loop_words;
}
static uint32_t s_scratch_req;                       /* apu_gp_debug_set_scratch_bytes(); 0 = what the image needs */
static uint32_t s_scratch_bytes = SCRATCH_BYTES;     /* in use */
static uint32_t s_image_end;                         /* P words the bootstrap image holds (program_loaded checks them) */
static uint32_t s_idle_base;                         /* START_FRAME polls of a frame without a kernel */
static uint32_t s_code_restarts;                     /* GP restarts because the P window did not take a kernel */
#define IDLE_NONE 0x7FFFFFu                          /* X:IDLMIN reset value */

static void loop_x_init(uintptr_t bar0);
static void slots_reset(void);

/* What apu_gp_init() allocates: room for the kernels for the default program */
static uint32_t scratch_bytes_for_init(void) {
    if (s_scratch_req) {
        return s_scratch_req;
    }
    return (s_image == NULL) ? APU_GP_SCRATCH_DEFAULT : SCRATCH_BYTES;
}

void apu_gp_debug_set_program(const uint32_t *words, uint32_t count) {
    if (words && count > 0u && count <= SCRATCH_BYTES / 4u) {
        s_image = words;
        s_image_words = count;
    } else {
        s_image = NULL;
        s_image_words = 0;
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
    uint32_t attempt, i, spin, words = s_image_end;
    const uint32_t *image = s_scratch;                   /* the bootstrap image (P:0 at scratch offset 0) */
    for (attempt = 0; attempt < 3u; attempt++) {
        bool ok = true;
        for (spin = 0; spin < 2000u; spin++) {
            (void)reg_rd(bar0, MCPX_APU_GPRST);      /* ~2 ms for the bootstrap DMA */
        }
        for (i = 0; i < words; i++) {
            if ((reg_rd(bar0, MCPX_APU_GP_PMEM + 4u * i) & 0xFFFFFFu) != (image[i] & 0xFFFFFFu)) {
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
    (void)s_image_end;
    return true;
#endif
}

/* The loop's X words after a (re)start: both FIFO descriptors, counters, mailbox, idle meter */
static void loop_x_init(uintptr_t bar0) {
    static const uint32_t desc[7] = {
        DESC_EOL,
        DESC_INTERLEAVE | DESC_TO_MEMORY | DESC_BUF_FIFO0 | DESC_FMT_16BIT | DESC_DSP_STEP(APU_GP_FRAME),
        (APU_GP_FRAME << 4) | (2u - 1u),     /* 32 frames, 2 channels */
        MIXBIN_X_BASE,                       /* bins 0 and 1 */
        0u, 0u, 0u,                          /* scratch offset/base/size: unused for FIFOs */
    };
    uint32_t i;
    for (i = 0; i < 7u; i++) {
        uint32_t v = (i == 1u) ? ((desc[1] & ~DESC_DSP_STEP(0x3FFu)) | DESC_DSP_STEP(s_dsp_step)) : desc[i];
        reg_wr(bar0, MCPX_APU_GP_XMEM + 4u * i, v);
        /* the mix descriptor: the same from OUT L/R, for every kernel */
        reg_wr(bar0, MCPX_APU_GP_XMEM + 4u * (GP_LOOP_FIFO2 + i), (i == 3u) ? GP_LOOP_OUTL : v);
    }
    for (i = FRAME_COUNTER_X; i < CLEAR_X_END; i++) {
        reg_wr(bar0, MCPX_APU_GP_XMEM + 4u * i, 0);   /* frame counter, loop words (mailbox 0/0, FLAGS 0), OUT L/R */
    }
    for (i = GP_LOOP_SLOTX; i <= GP_LOOP_CHAIN; i++) {
        reg_wr(bar0, MCPX_APU_GP_XMEM + 4u * i, 0);   /* the slot table */
    }
    slots_reset();
    reg_wr(bar0, MCPX_APU_GP_XMEM + 4u * GP_LOOP_K_WHEAD, GP_LOOP_FIFO2);
    reg_wr(bar0, MCPX_APU_GP_XMEM + 4u * GP_LOOP_IDLE, 0);
    reg_wr(bar0, MCPX_APU_GP_XMEM + 4u * GP_LOOP_IDLMIN, IDLE_NONE);
    s_mb_seq = 0;
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
    uint32_t i, pages;

    if (s_ready) {
        return 0;
    }
    s_scratch_bytes = scratch_bytes_for_init();
    s_fifo_bytes = s_fifo_req;
    s_scratch = (uint32_t *)apu_mem_alloc_table(s_scratch_bytes, &s_scratch_phys);
    s_scratch_sge = (uint32_t *)apu_mem_alloc_table(4096, &s_scratch_sge_phys);
    s_fifo = (uint8_t *)apu_mem_alloc_table(s_fifo_bytes, &s_fifo_phys);
    s_fifo_sge = (uint32_t *)apu_mem_alloc_table(4096, &s_fifo_sge_phys);
    if (!s_scratch || !s_scratch_sge || !s_fifo || !s_fifo_sge) {
        free_all();
        return -1;
    }

    /* Bootstrap image: P:0..$7FF as little-endian 32-bit words, top byte 0; the
     * rest stays 0 (NOP), the program never runs into it */
    memset(s_scratch, 0, s_scratch_bytes);
    {
        uint32_t words;
        const uint32_t *image = image_words(&words);
        for (i = 0; i < words; i++) {
            s_scratch[i] = image[i] & 0xFFFFFFu;
        }
        s_image_end = words;
    }
    memset(s_fifo, 0, s_fifo_bytes);
#if defined(OPENAL_TARGET_XBOX)
    /* Read the ring through the CPU cache: the APU's DMA writes are seen by the
     * cache (openal_engine phase 7: no stale data over 3 s, against 8.3 ms of
     * CPU per second for 64-bit uncached reads, 5.0 ms cached). Zeroed while
     * still uncached, so no dirty line can later be written back over the
     * GP's output; after this the CPU only reads it. */
    MmSetAddressProtect(s_fifo, s_fifo_bytes, PAGE_READWRITE);
#endif

    /* SGE tables: one entry per 4 KiB page (dword 0 = physical page) */
    memset(s_scratch_sge, 0, 4096);
    pages = s_scratch_bytes / 4096u;
    for (i = 0; i < pages; i++) {
        s_scratch_sge[2u * i] = s_scratch_phys + i * 4096u;
    }
    memset(s_fifo_sge, 0, 4096);
    for (i = 0; i < s_fifo_bytes / 4096u; i++) {
        s_fifo_sge[2u * i] = s_fifo_phys + i * 4096u;
    }

    /* Hold the GP in reset while it is configured */
    reg_wr(bar0, MCPX_APU_GPRST, 0);

    /* Step 8: table bases and highest valid entries (inclusive) */
    reg_wr(bar0, MCPX_APU_GPSADDR, s_scratch_sge_phys);
    reg_wr(bar0, MCPX_APU_GPSMAXSGE, pages - 1u);
    reg_wr(bar0, MCPX_APU_GPFADDR, s_fifo_sge_phys);
    reg_wr(bar0, MCPX_APU_GPFMAXSGE, s_fifo_bytes / 4096u - 1u);

    /* Step 15: output FIFO 0, base <= cur < end */
    reg_wr(bar0, MCPX_APU_GPOFBASE0, 0);
    reg_wr(bar0, MCPX_APU_GPOFEND0, s_fifo_bytes);
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
    loop_x_init(bar0);
    s_ready = true;
    return 0;
}

void apu_gp_deinit(uintptr_t bar0) {
    if (!s_ready) {
        return;
    }
    reg_wr(bar0, MCPX_APU_GPRST, 0);
    s_ready = false;
    slots_reset();
    free_all();
}

/* ---------------------------------------------------------------- mailbox */

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

#define XREG(n) (MCPX_APU_GP_XMEM + 4u * (n))
#define PREG(n) (MCPX_APU_GP_PMEM + 4u * (n))

static void idle_calibrate(uintptr_t bar0);

/* The loop has applied every mailbox posted so far (about 20 ms of register reads at most) */
static bool mailbox_idle(uintptr_t bar0) {
#if defined(OPENAL_TARGET_XBOX)
    uint32_t spin;
    for (spin = 0; spin < 40000u; spin++) {
        if ((reg_rd(bar0, XREG(GP_LOOP_MB_ACK)) & 0xFFFFFFu) == s_mb_seq) {
            return true;
        }
    }
    return false;
#else
    (void)bar0;
    return true;
#endif
}

int apu_gp_mailbox(uintptr_t bar0, const apu_reverb_word_t *w, uint32_t n) {
    uint32_t i;
    if (!s_ready || s_image != NULL || n > GP_LOOP_MB_MAX || (n && !w)) {
        return -1;
    }
    if (!mailbox_idle(bar0)) {
        /* The GP is not taking frames: nothing would apply it */
        write_words(bar0, w, n);
        return 1;
    }
    for (i = 0; i < n; i++) {
        reg_wr(bar0, XREG(GP_LOOP_MBOX + 2u * i), w[i].addr);
        reg_wr(bar0, XREG(GP_LOOP_MBOX + 2u * i + 1u), w[i].value & 0xFFFFFFu);
    }
    reg_wr(bar0, XREG(GP_LOOP_MB_N), n);
    s_mb_seq = (s_mb_seq + 1u) & 0xFFFFFFu;
    reg_wr(bar0, XREG(GP_LOOP_MB_SEQ), s_mb_seq);   /* last: the loop takes the pairs at its next frame */
#if !defined(OPENAL_TARGET_XBOX)
    /* No GP on the host: apply it the way the loop would */
    write_words(bar0, w, n);
    reg_wr(bar0, XREG(GP_LOOP_MB_ACK), s_mb_seq);
#endif
    return 0;
}

uint32_t apu_gp_debug_mailbox_frames(uintptr_t bar0) {
#if defined(OPENAL_TARGET_XBOX)
    uint32_t f0, spin;
    if (!s_ready || s_image != NULL || !mailbox_idle(bar0)) {
        return 0xFFFFFFFFu;
    }
    f0 = apu_gp_frames(bar0);
    if (apu_gp_mailbox(bar0, NULL, 0) != 0) {
        return 0xFFFFFFFFu;
    }
    for (spin = 0; spin < 40000u; spin++) {
        if ((reg_rd(bar0, XREG(GP_LOOP_MB_ACK)) & 0xFFFFFFu) == s_mb_seq) {
            return (apu_gp_frames(bar0) - f0) & 0xFFFFFFu;
        }
    }
    return 0xFFFFFFFFu;
#else
    (void)bar0;
    return 0;
#endif
}

void apu_gp_debug_set_burn(uintptr_t bar0, uint32_t cycles) {
    if (s_ready && s_image == NULL) {
        reg_wr(bar0, XREG(GP_LOOP_BURN), cycles & 0xFFFFu);
    }
}

/* ---------------------------------------------------------------- kernel images */

#define GP_P_END          0x800u      /* the kernels' P: GP_LOOP_KERNEL ($100) .. $7FF */
#define GP_X_END          0xC00u      /* the kernels' X: GP_LOOP_KX_BASE ($E0) .. $BFF */
#define GP_Y_END          0x800u
#define SLOT_INPUT_BIN(k) (2u + 2u * (k))   /* slot k reads bins 2+2k / 3+2k (EFX_PLAN.md 3.2) */
#define FLAG_LEGACY       0x10u       /* X:FLAGS bit 4: an ABI 1 kernel at P:$100 */
#define FLAG_LOWPASS      0x20u       /* X:FLAGS bit 5: the output lowpass (the reverb's) */
#define JMP_SHORT         0x0C0000u   /* jmp xxx (12-bit address): a trampoline, as the loop's own jmps */
#define SCRATCH_ALIGN     4096u
#define RELOC_P           0u          /* reloc kinds (apu_gp.h) */
#define RELOC_P12         1u
#define RELOC_X           2u

static uint32_t round_scratch(uint32_t bytes) {
    return (bytes + SCRATCH_ALIGN - 1u) & ~(SCRATCH_ALIGN - 1u);
}

uint32_t apu_gp_kernel_scratch_max(void) {
    uint32_t bytes = s_ready ? s_scratch_bytes : scratch_bytes_for_init();
    return (bytes > APU_REVERB_SCRATCH_BASE) ? bytes - APU_REVERB_SCRATCH_BASE : 0u;
}

static int parse_image(const uint32_t *image, uint32_t count, apu_gp_kernel_t *out, bool check_scratch) {
    uint32_t i, head, n = 0;
    if (!image || !out || count <= APU_GP_IMAGE_HEADER || image[0] != APU_GP_IMAGE_MAGIC ||
        (image[1] != 1u && image[1] != 2u)) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    head = APU_GP_IMAGE_HEADER;
    if (image[1] == 2u) {
        if (count <= APU_GP_IMAGE_HEADER2 || image[6] >= count - APU_GP_IMAGE_HEADER2) {
            return -1;
        }
        n = image[6];
        head = APU_GP_IMAGE_HEADER2 + n;
        if (image[4] < APU_GP_KERNEL_MIN_XWORDS || image[4] > APU_GP_KERNEL_MAX_XWORDS ||
            image[5] > APU_GP_KERNEL_MAX_YWORDS) {
            return -1;
        }
        out->x_words = image[4];
        out->y_words = image[5];
        out->relocs = image + APU_GP_IMAGE_HEADER2;
        out->reloc_count = n;
    }
    if (count - head > APU_GP_KERNEL_MAX_WORDS || image[3] > APU_GP_KERNEL_MAX_DESCRIPTORS ||
        (check_scratch && image[2] > apu_gp_kernel_scratch_max())) {
        return -1;
    }
    for (i = head; i < count; i++) {
        if (image[i] > 0xFFFFFFu) {
            return -1;
        }
    }
    out->code = image + head;
    out->words = count - head;
    out->scratch_bytes = image[2];
    out->descriptors = image[3];
    out->abi = image[1];
    /* Relocs inside the code; a short jump's 12-bit field stays in the kernel, so adding a P base never carries */
    for (i = 0; i < n; i++) {
        uint32_t r = out->relocs[i], off = r & 0xFFFu;
        if (r > 0x3FFFu || off >= out->words ||
            (((r >> 12) & 3u) == RELOC_P12 && (out->code[off] & 0xFFFu) >= out->words)) {
            return -1;
        }
    }
    return 0;
}

int apu_gp_kernel_parse(const uint32_t *image, uint32_t count, apu_gp_kernel_t *out) {
    return parse_image(image, count, out, true);
}

void apu_gp_kernel_relocate(const apu_gp_kernel_t *k, uint32_t pbase, uint32_t xbase, uint32_t ybase, uint32_t *out) {
    uint32_t i;
    for (i = 0; i < k->words; i++) {
        out[i] = k->code[i] & 0xFFFFFFu;
    }
    for (i = 0; k->abi == 2u && i < k->reloc_count; i++) {
        uint32_t off = k->relocs[i] & 0xFFFu, kind = (k->relocs[i] >> 12) & 3u, w = out[off];
        if (kind == RELOC_P12) {
            w = (w & ~0xFFFu) | ((w + pbase) & 0xFFFu);
        } else {
            w += (kind == RELOC_P) ? pbase : ((kind == RELOC_X) ? xbase : ybase);
        }
        out[off] = w & 0xFFFFFFu;
    }
}

const apu_gp_kernel_t *apu_gp_reverb_kernel(void) {
    static apu_gp_kernel_t k;
    static bool ok;
    if (!ok) {
        ok = parse_image(gp_kfreeverb_image, GP_KFREEVERB_IMAGE_WORDS, &k, false) == 0;
    }
    return ok ? &k : NULL;
}

/* ---------------------------------------------------------------- slots */

enum { SLOT_OFF, SLOT_KERNEL, SLOT_REVERB };

/* What a slot runs and where (P words, X block, Y words, scratch bytes) */
typedef struct {
    uint8_t kind;
    uint8_t abi;
    uint32_t p, words;
    uint32_t x, xwords;
    uint32_t y, ywords;
    uint32_t scr, scr_bytes;        /* scr_bytes rounded to 4 KiB */
    uint32_t descriptors;
    uint32_t params[APU_GP_KERNEL_PARAMS];
    uint32_t reverb_pre;            /* SLOT_REVERB: the pre-delay laid out */
} gp_slot_t;

static gp_slot_t s_slot[APU_GP_SLOTS];
static uint32_t s_flags;            /* X:FLAGS as last written */
static uint32_t s_reloc_buf[APU_GP_KERNEL_MAX_WORDS];

typedef struct {
    uint32_t base, size;
} gp_range_t;

/* The lowest base in [lo, hi) with `size` words (or bytes) clear of `used`, aligned; 0xFFFFFFFF if none */
static uint32_t first_fit(const gp_range_t *used, uint32_t n, uint32_t lo, uint32_t hi, uint32_t size, uint32_t align) {
    uint32_t base = lo, i;
    bool moved = true;
    while (moved) {
        moved = false;
        for (i = 0; i < n; i++) {
            if (size && used[i].size && base < used[i].base + used[i].size && used[i].base < base + size) {
                base = (used[i].base + used[i].size + align - 1u) & ~(align - 1u);
                moved = true;
            }
        }
    }
    return (base + size <= hi) ? base : 0xFFFFFFFFu;
}

/*
 * Place kernel `k` for slot `slot` beside the slots of `slots` that run
 * something (first fit in each space, no compaction): fills the slot's ranges,
 * leaves its kind alone. False if a budget is full or an ABI 1 kernel would
 * not run alone.
 */
static bool place(gp_slot_t *slots, uint32_t slot, const apu_gp_kernel_t *k) {
    gp_range_t p[APU_GP_SLOTS], x[APU_GP_SLOTS], y[APU_GP_SLOTS], scr[APU_GP_SLOTS];
    gp_slot_t *s = &slots[slot];
    uint32_t i, n = 0, desc = k->descriptors;
    uint32_t scr_end = APU_REVERB_SCRATCH_BASE + apu_gp_kernel_scratch_max();
    for (i = 0; i < APU_GP_SLOTS; i++) {
        const gp_slot_t *o = &slots[i];
        if (i == slot || o->kind == SLOT_OFF) {
            continue;
        }
        if (k->abi == 1u || o->abi == 1u) {
            return false;   /* an ABI 1 kernel owns the whole GP */
        }
        p[n].base = o->p;     p[n].size = o->words;
        x[n].base = o->x;     x[n].size = o->xwords;
        y[n].base = o->y;     y[n].size = o->ywords;
        scr[n].base = o->scr; scr[n].size = o->scr_bytes;
        desc += o->descriptors;
        n++;
    }
    if (desc > APU_GP_KERNEL_MAX_DESCRIPTORS) {
        return false;
    }
    s->abi = (uint8_t)k->abi;
    s->words = k->words;
    s->descriptors = k->descriptors;
    s->scr_bytes = round_scratch(k->scratch_bytes);
    if (k->abi == 1u) {
        s->p = GP_LOOP_KERNEL;
        s->x = GP_LOOP_KX_BASE;
        s->xwords = GP_X_END - GP_LOOP_KX_BASE;
        s->y = 0;
        s->ywords = GP_Y_END;
        s->scr = APU_REVERB_SCRATCH_BASE;
        return s->scr + s->scr_bytes <= scr_end;
    }
    s->xwords = k->x_words;
    s->ywords = k->y_words;
    s->p = first_fit(p, n, GP_LOOP_KERNEL, GP_P_END, s->words, 1u);
    s->x = first_fit(x, n, GP_LOOP_KX_BASE, GP_X_END, s->xwords, 1u);
    s->y = first_fit(y, n, 0u, GP_Y_END, s->ywords, 1u);
    s->scr = first_fit(scr, n, APU_REVERB_SCRATCH_BASE, scr_end, s->scr_bytes, SCRATCH_ALIGN);
    return s->p != 0xFFFFFFFFu && s->x != 0xFFFFFFFFu && s->y != 0xFFFFFFFFu && s->scr != 0xFFFFFFFFu;
}

bool apu_gp_slot_holds(uint32_t slot, const apu_gp_kernel_t *k) {
    const gp_slot_t *s;
    uint32_t i;
    if (slot >= APU_GP_SLOTS) {
        return false;
    }
    s = &s_slot[slot];
    if (!k || s->kind == SLOT_OFF) {
        return !k && s->kind == SLOT_OFF;
    }
    if (!s_ready || s->abi != k->abi || s->words != k->words || s->descriptors != k->descriptors ||
        s->scr_bytes != round_scratch(k->scratch_bytes) ||
        (k->abi == 2u && (s->xwords != k->x_words || s->ywords != k->y_words))) {
        return false;
    }
    apu_gp_kernel_relocate(k, s->p, s->x, s->y, s_reloc_buf);
    for (i = 0; i < k->words; i++) {
        if (s_scratch[s->p + i] != s_reloc_buf[i]) {   /* the bootstrap image mirrors P */
            return false;
        }
    }
    return true;
}

bool apu_gp_kernels_fit(const apu_gp_kernel_t *const set[APU_GP_SLOTS]) {
    gp_slot_t sim[APU_GP_SLOTS];
    uint32_t i, kernels = 0, abi1 = 0, p = 0, x = 0, y = 0, scr = 0, desc = 0;
    for (i = 0; i < APU_GP_SLOTS; i++) {
        const apu_gp_kernel_t *k = set[i];
        if (!k) {
            continue;
        }
        kernels++;
        abi1 += (k->abi == 1u) ? 1u : 0u;
        p += k->words;
        x += (k->abi == 1u) ? APU_GP_KERNEL_MAX_XWORDS : k->x_words;
        y += (k->abi == 1u) ? APU_GP_KERNEL_MAX_YWORDS : k->y_words;
        scr += round_scratch(k->scratch_bytes);
        desc += k->descriptors;
    }
    if ((abi1 && kernels > 1u) || p > APU_GP_KERNEL_MAX_WORDS || x > APU_GP_KERNEL_MAX_XWORDS ||
        y > APU_GP_KERNEL_MAX_YWORDS || scr > apu_gp_kernel_scratch_max() || desc > APU_GP_KERNEL_MAX_DESCRIPTORS) {
        return false;
    }
    if (!s_ready || s_image != NULL) {
        return true;   /* loaded in slot order into an empty GP: the sums are the whole story */
    }
    /* The loader's own placement: kernels that stay keep their ranges, the rest go around them */
    memcpy(sim, s_slot, sizeof(sim));
    for (i = 0; i < APU_GP_SLOTS; i++) {
        if (!set[i] || !apu_gp_slot_holds(i, set[i])) {
            sim[i].kind = SLOT_OFF;
        }
    }
    for (i = 0; i < APU_GP_SLOTS; i++) {
        if (set[i] && sim[i].kind == SLOT_OFF) {
            if (!place(sim, i, set[i])) {
                return false;
            }
            sim[i].kind = SLOT_KERNEL;
        }
    }
    return true;
}

/* Every slot empty (a GP start or restart: the loop's X words were reset) */
static void slots_reset(void) {
    memset(s_slot, 0, sizeof(s_slot));
    s_flags = 0;
}

static void flags_write(uintptr_t bar0) {
    reg_wr(bar0, XREG(GP_LOOP_FLAGS), s_flags);
}

static uint32_t slot_flag(uint32_t slot) {
    return (s_slot[slot].abi == 1u) ? FLAG_LEGACY : (1u << slot);
}

void apu_gp_slot_stop(uintptr_t bar0, uint32_t slot) {
    uint32_t i;
    if (!s_ready || slot >= APU_GP_SLOTS || s_slot[slot].kind == SLOT_OFF) {
        return;
    }
    s_flags &= ~slot_flag(slot);
    s_slot[slot].kind = SLOT_OFF;
    s_flags &= ~FLAG_LOWPASS;
    for (i = 0; i < APU_GP_SLOTS; i++) {
        if (s_slot[i].kind == SLOT_REVERB) {
            s_flags |= FLAG_LOWPASS;
        }
    }
    flags_write(bar0);   /* the loop skips it from its next frame; its ranges are free */
}

static void slot_start(uintptr_t bar0, uint32_t slot) {
    s_flags |= slot_flag(slot) | ((s_slot[slot].kind == SLOT_REVERB) ? FLAG_LOWPASS : 0u);
    flags_write(bar0);
}

/* The idle polls of a frame without a kernel: the 0 % of apu_gp_load() */
static void idle_calibrate(uintptr_t bar0) {
    uint32_t v = reg_rd(bar0, XREG(GP_LOOP_IDLE)) & 0xFFFFFFu;
    if (v) {
        s_idle_base = v;
    }
}

/*
 * Code words for P:p.. (and slot k's trampoline, ABI 2) with that slot off:
 * into the bootstrap image and through the P window, then read back. If the
 * window did not take them, restart the GP, whose bootstrap then loads the
 * image; the restart resets the loop, so every other slot stops.
 */
static int load_code(uintptr_t bar0, uint32_t slot, const gp_slot_t *s, const uint32_t *code) {
    uint32_t i, tramp = GP_LOOP_TRAMP0 + slot;
    bool ok = true;
    for (i = 0; i < s->words; i++) {
        s_scratch[s->p + i] = code[i];
        reg_wr(bar0, PREG(s->p + i), code[i]);
    }
    if (s->abi == 2u) {
        s_scratch[tramp] = JMP_SHORT | s->p;
        reg_wr(bar0, PREG(tramp), JMP_SHORT | s->p);
    }
    if (s->p + s->words > s_image_end) {
        s_image_end = s->p + s->words;
    }
#if defined(OPENAL_TARGET_XBOX)
    for (i = 0; i < s->words; i++) {
        if ((reg_rd(bar0, PREG(s->p + i)) & 0xFFFFFFu) != code[i]) {
            ok = false;
        }
    }
    if (s->abi == 2u && (reg_rd(bar0, PREG(tramp)) & 0xFFFFFFu) != (JMP_SHORT | s->p)) {
        ok = false;
    }
    if (!ok) {
        gp_slot_t keep = s_slot[slot];   /* the restart empties every slot but the one loading */
        s_code_restarts++;
        reg_wr(bar0, MCPX_APU_GPRST, 0);
        reg_wr(bar0, MCPX_APU_GPRST, 3u);
        s_loaded = program_loaded(bar0);
        loop_x_init(bar0);
        s_slot[slot] = keep;
        ok = s_loaded;
    }
#else
    (void)slot;
#endif
    return ok ? 0 : -1;
}

/*
 * Load kernel `k` into slot `slot` (stopped first; the others keep running):
 * place it, write its code and trampoline, zero its X block and scratch, write
 * the ABI words and the slot table entry. The caller switches it on.
 */
static int slot_load(uintptr_t bar0, uint32_t slot, const apu_gp_kernel_t *k, const uint32_t *params, uint8_t kind) {
    gp_slot_t *s = &s_slot[slot];
    uint32_t i, kparam;
    apu_gp_slot_stop(bar0, slot);
    wait_frames(bar0, 2u);   /* nothing stopped (this slot or one before) is still in its frame */
    if ((s_flags & ~FLAG_LOWPASS) == 0u) {
        idle_calibrate(bar0);
    }
    if (!place(s_slot, slot, k)) {
        return -1;
    }
    apu_gp_kernel_relocate(k, s->p, s->x, s->y, s_reloc_buf);
    if (load_code(bar0, slot, s, s_reloc_buf) != 0) {
        return -1;
    }
    /* The ABI: X block zeroed, then its words; no chains until the kernel sets them */
    for (i = s->x; i < s->x + s->xwords; i++) {
        reg_wr(bar0, XREG(i), 0);
    }
    if (k->abi == 1u) {
        reg_wr(bar0, XREG(GP_LOOP_KIN), MIXBIN_X_BASE + 32u * SLOT_INPUT_BIN(slot));
        reg_wr(bar0, XREG(GP_LOOP_KSCR), s->scr);
        reg_wr(bar0, XREG(GP_LOOP_KSCRN), k->scratch_bytes);
        reg_wr(bar0, XREG(GP_LOOP_K_RHEAD), 0);
        reg_wr(bar0, XREG(GP_LOOP_K_WHEAD), GP_LOOP_FIFO2);
        kparam = GP_LOOP_KPARAM;
    } else {
        reg_wr(bar0, XREG(s->x), MIXBIN_X_BASE + 32u * SLOT_INPUT_BIN(slot));
        reg_wr(bar0, XREG(s->x + 1u), s->scr);
        reg_wr(bar0, XREG(s->x + 2u), k->scratch_bytes);
        reg_wr(bar0, XREG(GP_LOOP_SLOTX + slot), s->x);
        kparam = s->x + APU_GP_KERNEL_PARAM0;
    }
    for (i = 0; i < APU_GP_KERNEL_PARAMS; i++) {
        s->params[i] = params ? (params[i] & 0xFFFFFFu) : 0u;
        reg_wr(bar0, XREG(kparam + i), s->params[i]);
    }
    memset((uint8_t *)s_scratch + s->scr, 0, k->scratch_bytes);
    s->kind = kind;
    return 0;
}

/* X address of parameter n of the kernel in `slot` */
static uint32_t param_addr(const gp_slot_t *s, uint32_t n) {
    return ((s->abi == 1u) ? GP_LOOP_KPARAM : s->x + APU_GP_KERNEL_PARAM0) + n;
}

int apu_gp_kernel_set(uintptr_t bar0, uint32_t slot, const apu_gp_kernel_t *k,
                      const uint32_t params[APU_GP_KERNEL_PARAMS], bool on) {
    apu_reverb_word_t w[APU_GP_KERNEL_PARAMS];
    gp_slot_t *s;
    uint32_t i, n = 0;

    if (!s_ready || s_image != NULL || slot >= APU_GP_SLOTS) {
        return -1;
    }
    s = &s_slot[slot];
    if (!on) {
        if (s->kind == SLOT_KERNEL) {
            apu_gp_slot_stop(bar0, slot);
        }
        return 0;
    }
    if (!k || !params || k->scratch_bytes > apu_gp_kernel_scratch_max()) {
        return -1;
    }
    if (s->kind == SLOT_KERNEL && apu_gp_slot_holds(slot, k)) {
        /* The same kernel: only the parameters that changed, in one frame */
        for (i = 0; i < APU_GP_KERNEL_PARAMS; i++) {
            if ((params[i] & 0xFFFFFFu) != s->params[i]) {
                w[n].addr = (uint16_t)param_addr(s, i);
                w[n].value = params[i] & 0xFFFFFFu;
                s->params[i] = w[n].value;
                n++;
            }
        }
        if (n && apu_gp_mailbox(bar0, w, n) < 0) {
            write_words(bar0, w, n);
        }
        return 0;
    }
    if (slot_load(bar0, slot, k, params, SLOT_KERNEL) != 0) {
        return -1;
    }
    slot_start(bar0, slot);
    return 0;
}

bool apu_gp_kernel_on(uint32_t slot) {
    uint32_t i;
    for (i = 0; s_ready && i < APU_GP_SLOTS; i++) {
        if ((slot == APU_GP_SLOTS || slot == i) && s_slot[i].kind == SLOT_KERNEL) {
            return true;
        }
    }
    return false;
}

bool apu_gp_debug_slot(uint32_t slot, uint32_t *p, uint32_t *x, uint32_t *y, uint32_t *scratch) {
    const gp_slot_t *s;
    if (!s_ready || slot >= APU_GP_SLOTS || s_slot[slot].kind == SLOT_OFF) {
        return false;
    }
    s = &s_slot[slot];
    if (p) *p = s->p;
    if (x) *x = s->x;
    if (y) *y = s->y;
    if (scratch) *scratch = s->scr;
    return true;
}

/* ---------------------------------------------------------------- reverb */

int apu_gp_reverb_set(uintptr_t bar0, uint32_t slot, const apu_reverb_params_t *p, bool on) {
    static apu_reverb_word_t words[APU_REVERB_MAX_WORDS];
    uint32_t len[APU_REVERB_LINES], n;
    const apu_gp_kernel_t *k = apu_gp_reverb_kernel();
    gp_slot_t *s;

    if (!s_ready || s_image != NULL || !p || !k || slot >= APU_GP_SLOTS) {
        return -1;
    }
    s = &s_slot[slot];
    if (!on) {
        if (s->kind == SLOT_REVERB) {
            apu_gp_slot_stop(bar0, slot);
        }
        return 0;
    }
    apu_reverb_line_lengths(p, len);
    if (s->kind == SLOT_REVERB && len[0] == s->reverb_pre) {
        /* Same delay lines: only the gains change, all in one frame (mailbox) */
        n = apu_reverb_coefficients(p, s->x, words);
        if (apu_gp_mailbox(bar0, words, n) < 0) {
            write_words(bar0, words, n);
        }
        return 0;
    }
    if (s->kind == SLOT_REVERB) {
        /* A new pre-delay: off for two frames so no chain is in flight, then a new layout, empty lines */
        s_flags &= ~slot_flag(slot);
        flags_write(bar0);
        wait_frames(bar0, 2u);
        memset((uint8_t *)s_scratch + s->scr, 0, k->scratch_bytes);
    } else if (slot_load(bar0, slot, k, NULL, SLOT_REVERB) != 0) {
        return -1;
    }
    n = apu_reverb_layout(p, s->x, s->scr, words);
    write_words(bar0, words, n);
    s->reverb_pre = len[0];
    slot_start(bar0, slot);
    return 0;
}

bool apu_gp_reverb_on(void) {
    uint32_t i;
    for (i = 0; s_ready && i < APU_GP_SLOTS; i++) {
        if (s_slot[i].kind == SLOT_REVERB) {
            return true;
        }
    }
    return false;
}

uint32_t apu_gp_debug_code_restarts(void) {
    return s_code_restarts;
}

static uint32_t load_pct(uint32_t idle) {
    if (s_idle_base == 0u || idle >= s_idle_base) {
        return 0;
    }
    return (uint32_t)(((uint64_t)(s_idle_base - idle) * 100u) / s_idle_base);
}

void apu_gp_load(uintptr_t bar0, uint32_t *last_pct, uint32_t *peak_pct) {
    uint32_t idle, low, last = 0, peak = 0;
    if (s_ready && s_image == NULL) {
        idle = reg_rd(bar0, XREG(GP_LOOP_IDLE)) & 0xFFFFFFu;
        low = reg_rd(bar0, XREG(GP_LOOP_IDLMIN)) & 0xFFFFFFu;
        if (peak_pct) {
            reg_wr(bar0, XREG(GP_LOOP_IDLMIN), IDLE_NONE);   /* the peak restarts */
        }
        if ((s_flags & ~FLAG_LOWPASS) == 0u) {
            idle_calibrate(bar0);
        }
        last = load_pct(idle);
        peak = (low == IDLE_NONE) ? last : load_pct(low);
        if (peak < last) {
            peak = last;
        }
    }
    if (last_pct) *last_pct = last;
    if (peak_pct) *peak_pct = peak;
}

bool apu_gp_ready(void) {
    return s_ready;
}

bool apu_gp_program_loaded(void) {
    return s_ready && s_loaded;
}

static uint32_t fifo_cur(uintptr_t bar0) {
    return (reg_rd(bar0, MCPX_APU_GPOFCUR0) & 0xFFFFFFu) % s_fifo_bytes;
}

uint32_t apu_gp_fifo_available(uintptr_t bar0) {
    if (!s_ready) {
        return 0;
    }
    return ((fifo_cur(bar0) + s_fifo_bytes - s_read) % s_fifo_bytes) / 4u;
}

/* How the ring is read. Uncached, a console's output thread spent 23.6 ms of
 * CPU per second with 16-bit reads, 13.4 with 32-bit and 8.3 with 64-bit ones;
 * the ring is now cached (apu_gp_init), where plain 32-bit reads are enough */
static uint32_t s_fifo_read_mode = 0u;   /* apu_gp_debug_set_fifo_read() */

void apu_gp_debug_set_fifo_read(uint32_t mode) {
    s_fifo_read_mode = mode;
}

void apu_gp_debug_set_fifo_cached(bool on) {
#if defined(OPENAL_TARGET_XBOX)
    if (s_fifo) {
        MmSetAddressProtect(s_fifo, s_fifo_bytes, on ? PAGE_READWRITE : (PAGE_READWRITE | PAGE_NOCACHE));
    }
#else
    (void)on;
#endif
}

/* Copy n stereo frames out of the ring (no wrap inside) */
static void copy_frames(uint32_t *dst, const volatile uint32_t *src, uint32_t n) {
    uint32_t i = 0;
    if (s_fifo_read_mode == 1u) {
        /* one 16-bit read per sample (the old reader, for comparison) */
        const volatile uint16_t *s16 = (const volatile uint16_t *)src;
        uint16_t *d16 = (uint16_t *)dst;
        for (i = 0; i < 2u * n; i++) d16[i] = s16[i];
        return;
    }
#if defined(OPENAL_TARGET_XBOX)
    if (s_fifo_read_mode == 0u) {
        /* Every line is a fresh APU write, so every read misses (~400 ns a
         * 32-byte line on a console, one after the other): ask for all of
         * them first and the misses overlap */
        const char *p = (const char *)(const void *)src;
        const char *e = (const char *)(const void *)(src + n);
        for (; p < e; p += 32) {
            __asm__ __volatile__("prefetchnta (%0)" : : "r"(p));
        }
    }
    if (s_fifo_read_mode == 2u) {
        /* 8-byte MMX reads: two frames per bus transaction */
        for (; i + 2u <= n; i += 2u) {
            __asm__ __volatile__("movq (%0), %%mm0\n\tmovq %%mm0, (%1)" : : "r"(src + i), "r"(dst + i) : "mm0", "memory");
        }
        __asm__ __volatile__("emms");
    }
#endif
    for (; i < n; i++) dst[i] = src[i];
}

uint32_t apu_gp_fifo_read(uintptr_t bar0, int16_t *out, uint32_t frames) {
    uint32_t avail, n, i;

    if (!s_ready || !out) {
        return 0;
    }
    avail = apu_gp_fifo_available(bar0);
    if (avail > (s_fifo_bytes / 4u) - (s_fifo_bytes / 4u) / 8u) {
        /* The writer is about to lap the reader (the reader stalled): jump
         * close behind the writer and continue from there */
        s_read = (fifo_cur(bar0) + s_fifo_bytes / 2u) % s_fifo_bytes;   /* half a ring behind */
        s_resyncs++;
        avail = apu_gp_fifo_available(bar0);
    }
    n = (frames < avail) ? frames : avail;
    /* The ring is uncached memory, and on the console each read of it is a bus
     * transaction: read whole frames (32 bits), not single samples */
    for (i = 0; i < n;) {
        uint32_t idx = s_read / 4u, seg = (s_fifo_bytes / 4u) - idx;
        if (seg > n - i) seg = n - i;
        copy_frames((uint32_t *)(void *)(out + 2u * i), (const volatile uint32_t *)(const void *)s_fifo + idx, seg);
        i += seg;
        s_read = (s_read + 4u * seg) % s_fifo_bytes;
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

void apu_gp_set_fifo_bytes(uint32_t bytes) {
    bytes &= ~4095u;
    if (bytes < 4096u) bytes = 4096u;
    if (bytes > APU_GP_FIFO_BYTES) bytes = APU_GP_FIFO_BYTES;
    s_fifo_req = bytes;
}

uint32_t apu_gp_fifo_bytes(void) {
    return s_ready ? s_fifo_bytes : 0;
}

uint32_t apu_gp_fifo_phys(void) {
    return s_ready ? s_fifo_phys : 0;
}

uint32_t apu_gp_fifo_write_pos(uintptr_t bar0) {
    return s_ready ? fifo_cur(bar0) : 0;
}
