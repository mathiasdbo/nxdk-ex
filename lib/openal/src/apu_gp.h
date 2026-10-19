#ifndef APU_GP_H
#define APU_GP_H

/*
 * GP DSP stage of the MCPX APU (route B of lib/openal/docs/XEMU_VERIFICATION.md):
 * a DSP56300 program, written for this library, that copies the
 * front-left/right mixbins (bins 0 and 1, where the HRTF submixes land) to GP
 * output FIFO 0 every 32-sample frame as interleaved 16-bit stereo. The FIFO is
 * a ring in system RAM, which the AC97 codec plays in place (zero-copy) or the
 * CPU copies to it (apu_gp_fifo_read(), apu_ac97.c).
 *
 *   P:0 08F484 000001   movep #1,x:$FFFFC4   frame complete (releases one latched start)
 *   P:2 0A8581 000002   jclr #1,x:$FFFFC5,$2 wait for START_FRAME
 *   P:4 08F485 000002   movep #2,x:$FFFFC5   acknowledge it
 *   P:6 08F494 000000   movep #0,x:$FFFFD4   DMA NEXT_BLOCK = descriptor at X:0
 *   P:8 08F496 000001   movep #1,x:$FFFFD6   DMA CONTROL = start (runs the chain)
 *   P:A 000008          inc a
 *   P:B 501000          move a0,x:$10        frame counter (apu_gp_frames())
 *   P:C 0C0000          jmp $0000
 *
 * (The probes used 0A7088 000010 for the counter. xemu decodes that as
 * jclr #8,x:ea,... and runs off the end of the image; 501000 is the X:aa
 * parallel move, 01dd0ddd W0aaaaaa.)
 *
 *   X:0  descriptor: next = end of list, control = interleave | DSP->memory |
 *        FIFO 0 | 16-bit | channel stride 32 words (dsp_step, bits 23:14),
 *        count = 32 frames x 2 channels, source X:$1400. Without the stride a
 *        real console puts bin 0 on both channels (apu_probe rounds 20/21).
 *
 * Verified on a retail console (samples/apu_probe round 17): the frame order
 * (complete, then wait) is the one measured on silicon in xemu PR 3047; a loop
 * that waits first never sees a start. The 440 Hz test voice came out of the
 * AC97 bit-exact.
 */

#include <stdbool.h>
#include "apu_reverb.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APU_GP_FIFO_BYTES   (64u * 1024u)   /* largest (and default) output ring: 16384 stereo frames, ~341 ms */
#define APU_GP_FIFO_BYTES_ZC (8u * 1024u)   /* the ring the AC97 reads directly (apu_ac97.c zero-copy): ~43 ms, the codec ~21 ms behind */
#define APU_GP_FRAME        32u             /* samples per APU frame */

typedef struct {
    uint32_t fifo_cur;       /* GPOFCUR0: byte offset the DSP writes next */
    uint32_t frames_read;    /* stereo frames handed out by apu_gp_fifo_read() */
    uint32_t resyncs;        /* times the reader was more than the ring behind or ahead */
} apu_gp_stats_t;

/**
 * Load the GP program and DMA descriptor, set up the output FIFO ring and
 * release the GP from reset (bring-up steps 8, 14 and 15). Call before frame
 * generation starts.
 * @return 0 on success, negative on allocation failure.
 */
int apu_gp_init(uintptr_t bar0);

/**
 * Size of the output ring from the next apu_gp_init(), in bytes (whole 4 KiB
 * pages, 4 KiB .. APU_GP_FIFO_BYTES; default APU_GP_FIFO_BYTES).
 */
void apu_gp_set_fifo_bytes(uint32_t bytes);

/** Size of the ring in use, its physical address (contiguous), and the byte offset the DSP writes next (0 before apu_gp_init()). */
uint32_t apu_gp_fifo_bytes(void);
uint32_t apu_gp_fifo_phys(void);
uint32_t apu_gp_fifo_write_pos(uintptr_t bar0);

/** Hold the GP in reset and free its memory. */
void apu_gp_deinit(uintptr_t bar0);

/** True between apu_gp_init() and apu_gp_deinit(). */
bool apu_gp_ready(void);

/** On hardware: the GP's P memory held our program after the bootstrap (always true on the host). */
bool apu_gp_program_loaded(void);

/** Stereo frames written by the GP and not yet read. */
uint32_t apu_gp_fifo_available(uintptr_t bar0);

/**
 * Copy up to `frames` interleaved stereo int16 frames of the APU mix.
 * @return frames copied.
 */
uint32_t apu_gp_fifo_read(uintptr_t bar0, int16_t *out, uint32_t frames);

/** Drop everything written so far: the next read starts at the DSP's write position. */
void apu_gp_fifo_discard(uintptr_t bar0);

void apu_gp_get_stats(uintptr_t bar0, apu_gp_stats_t *out);

/** How apu_gp_fifo_read() reads the ring (diagnostics): 0 32-bit reads (default), 1 16-bit, 2 64-bit MMX. */
void apu_gp_debug_set_fifo_read(uint32_t mode);

/** Map the output ring cached (true, the default on a console) or uncached (false): a probe of the APU's DMA coherence. */
void apu_gp_debug_set_fifo_cached(bool on);

/** Frames the GP program has run (its counter at X:$10, 24 bits). */
uint32_t apu_gp_frames(uintptr_t bar0);

/** Channel stride of the output DMA from the next apu_gp_init() on (diagnostics; default 32). */
void apu_gp_debug_set_dsp_step(uint32_t words);

/**
 * Program image for the next apu_gp_init() (diagnostics and alternative
 * programs): `count` DSP words loaded at P:0, at most 0x800. NULL restores the
 * default: the frame loop dsp/gp_loop.asm at P:0 (the copy program while no
 * slot runs a kernel); the kernels are loaded into P:$100..$7FF later.
 * The program must end each frame like the copy program and keep the frame
 * counter at X:$10 (apu_vp_init checks it). The mailbox, the burn, the reverb
 * and the kernels need the default image.
 */
void apu_gp_debug_set_program(const uint32_t *words, uint32_t count);

/**
 * Size of the GP scratch space from the next apu_gp_init(), in bytes (rounded up
 * to 4 KiB, at least 0x2000, at most 2 MiB; 0 = APU_GP_SCRATCH_DEFAULT for the
 * default program). Offsets 0..$1FFF hold the bootstrap image; the rest is
 * the kernels' (zeroed at init).
 */
#define APU_GP_SCRATCH_DEFAULT (512u * 1024u)   /* the reverb's lines (180 KiB) and two 128 KiB echoes */
void apu_gp_debug_set_scratch_bytes(uint32_t bytes);

/** Size of the scratch space in use (0 before apu_gp_init()). */
uint32_t apu_gp_scratch_bytes(void);

/**
 * Mailbox of the frame loop (EFX_PLAN.md 3.4): the `n` (address, value) X words
 * (at most GP_LOOP_MB_MAX, 56) land together at the start of one frame. Waits
 * for the previous post to be applied first (at most ~20 ms). Returns 0, 1 if
 * the GP took no frames (the words were written directly), -1 without the
 * default image.
 */
int apu_gp_mailbox(uintptr_t bar0, const apu_reverb_word_t *w, uint32_t n);

/** Frames from an empty mailbox post to the loop's acknowledgement (diagnostics; 0xFFFFFFFF on failure). */
uint32_t apu_gp_debug_mailbox_frames(uintptr_t bar0);

/** Cycles of nop the loop adds to every frame while the kernel runs (diagnostics: headroom; 0 = none). */
void apu_gp_debug_set_burn(uintptr_t bar0, uint32_t cycles);

/*
 * Effect slots (EFX_PLAN.md 3.2-3.3, GP_KERNELS.md): the loop runs up to
 * APU_GP_SLOTS kernels at once, one per slot, in slot order; slot k reads the
 * bin pair 2+2k / 3+2k. Each kernel gets its own P range (P:$100..$7FF), X
 * block (X:$E0..$BFF), Y range and scratch, placed first fit (no compaction:
 * a freed range is reused when something fits in it). The budgets are shared:
 * P, X, Y, scratch, and 60 DMA descriptors a frame in all.
 *
 * An image, as tools/dsp56asm.py --image writes it:
 *   ABI 2 (relocatable): magic, 2, scratch bytes, descriptors, X words, Y
 *     words, reloc count, the relocs, then the code assembled at P:0 with
 *     KX = KY = 0. A reloc is one word: bits 11:0 the code offset, 13:12 the
 *     kind (0 a P extension word, 1 the 12-bit address field of a short
 *     jmp / jsr / jCC, 2 an X extension word, 3 a Y extension word); the
 *     loader adds the slot's P, X or Y base.
 *   ABI 1: magic, 1, scratch bytes, descriptors, then the code for P:$100, its
 *     X from $E0 and all of Y: it runs only alone.
 */
#define APU_GP_SLOTS                  4u
#define APU_GP_IMAGE_MAGIC            0x47504Bu   /* 'GPK' */
#define APU_GP_IMAGE_ABI              2u          /* the current ABI; 1 is still accepted */
#define APU_GP_IMAGE_HEADER           4u          /* ABI 1 */
#define APU_GP_IMAGE_HEADER2          7u          /* ABI 2, before the relocs */
#define APU_GP_KERNEL_MAX_WORDS       0x700u      /* P:$100..$7FF */
#define APU_GP_KERNEL_MAX_XWORDS      0xB20u      /* X:$E0..$BFF */
#define APU_GP_KERNEL_MAX_YWORDS      0x800u
#define APU_GP_KERNEL_MIN_XWORDS      0x28u       /* the ABI words: KIN .. KPARAM + 31 */
#define APU_GP_KERNEL_MAX_DESCRIPTORS 60u
#define APU_GP_KERNEL_PARAMS          32u
#define APU_GP_KERNEL_PARAM0          8u          /* ABI 2: X:KX + 8 + n is parameter n */

typedef struct {
    const uint32_t *code;       /* ABI 2: assembled at P:0; ABI 1: the words for P:$100 on */
    uint32_t words;
    uint32_t scratch_bytes;
    uint32_t descriptors;
    uint32_t abi;               /* 1 or 2 */
    uint32_t x_words;           /* ABI 2: the X block from KX, the ABI words included */
    uint32_t y_words;           /* ABI 2: Y words from KY */
    const uint32_t *relocs;     /* ABI 2 */
    uint32_t reloc_count;
} apu_gp_kernel_t;

/**
 * Check an image: magic, ABI, code size, 24-bit words, descriptors, scratch
 * within apu_gp_kernel_scratch_max(), and for ABI 2 the X / Y sizes and the
 * relocs (offsets inside the code). Fills `out` (pointing into `image`).
 * Returns 0, or -1 if the image is not usable.
 */
int apu_gp_kernel_parse(const uint32_t *image, uint32_t count, apu_gp_kernel_t *out);

/** Scratch bytes the kernels may use: the open device's space above $2000, else what the next apu_gp_init() allocates. */
uint32_t apu_gp_kernel_scratch_max(void);

/** `code` relocated for P / X / Y bases (ABI 2; an ABI 1 kernel is copied as it is). `out`: k->words words. */
void apu_gp_kernel_relocate(const apu_gp_kernel_t *k, uint32_t pbase, uint32_t xbase, uint32_t ybase, uint32_t *out);

/** The reverb's kernel (dsp/gp_kfreeverb.asm, ABI 2), for budget checks. */
const apu_gp_kernel_t *apu_gp_reverb_kernel(void);

/**
 * Do the kernels `set` (one per slot, NULL for an empty slot) fit the GP
 * together? The sums of P, X, Y, scratch and descriptors, an ABI 1 kernel
 * only alone, and while the GP runs, the loader's own first fit: a slot whose
 * kernel stays keeps its ranges, the others are placed around them in slot
 * order.
 */
bool apu_gp_kernels_fit(const apu_gp_kernel_t *const set[APU_GP_SLOTS]);

/** Slot `slot` runs kernel `k` (its code, relocated where it is, and its sizes); NULL: the slot is empty. */
bool apu_gp_slot_holds(uint32_t slot, const apu_gp_kernel_t *k);

/** Stop whatever slot `slot` runs and free its ranges. */
void apu_gp_slot_stop(uintptr_t bar0, uint32_t slot);

/**
 * The reverb (apu_reverb.h) in slot `slot`. `on` with new parameters: if only
 * gains change they go through the mailbox (one frame); a new pre-delay (or
 * switching on) stops the slot for two frames, loads the kernel if the slot
 * ran something else, lays the delay lines out again and empties them. Its
 * output lowpass acts on the whole mix. Off stops the slot if it runs the
 * reverb. Returns -1 if the GP runs another program or the budgets are full.
 */
int apu_gp_reverb_set(uintptr_t bar0, uint32_t slot, const struct apu_reverb_params_s *p, bool on);

/** A slot runs the reverb. */
bool apu_gp_reverb_on(void);

/**
 * Run kernel `k` in slot `slot` (`on`), or stop the slot's kernel. A kernel
 * already running there with the same code only gets its changed parameters,
 * through the mailbox (one frame). Otherwise the slot stops for two frames
 * (the other slots keep running), the kernel is placed (first fit), its code
 * relocated and written through the P window for its range only (read back;
 * on a mismatch the GP restarts from the bootstrap image, which holds it too,
 * and the other slots stop), its trampoline pointed at it, its X block and
 * scratch zeroed and its ABI words written, and it starts. An ABI 1 kernel
 * needs every other slot empty. Returns 0, -1 if the GP cannot run it.
 */
int apu_gp_kernel_set(uintptr_t bar0, uint32_t slot, const apu_gp_kernel_t *k,
                      const uint32_t params[APU_GP_KERNEL_PARAMS], bool on);

/** Slot `slot` runs a custom kernel (APU_GP_SLOTS: any slot). */
bool apu_gp_kernel_on(uint32_t slot);

/** Where slot `slot`'s kernel is (diagnostics, tests): P, X and Y bases and scratch byte offset; false if empty. */
bool apu_gp_debug_slot(uint32_t slot, uint32_t *p, uint32_t *x, uint32_t *y, uint32_t *scratch);

/**
 * How much of the GP's frame the effect takes, in percent (100: frames late),
 * the last frame's and the most since the last call asking for the peak
 * (`peak_pct` not NULL). Measured from the
 * loop's START_FRAME polls against a frame without the kernel (taken when an
 * effect starts); 0 on the host or before an effect has run.
 */
void apu_gp_load(uintptr_t bar0, uint32_t *last_pct, uint32_t *peak_pct);

/** Times a kernel load restarted the GP because the P window did not take the code (diagnostics). */
uint32_t apu_gp_debug_code_restarts(void);

/** Output ring and bootstrap scratch image (host tests). */
uint8_t *apu_gp_debug_fifo(void);
const uint32_t *apu_gp_debug_scratch(void);

#define APU_GP_PROGRAM_WORDS 13u

/** The GP program words (for tests). */
extern const uint32_t apu_gp_program[APU_GP_PROGRAM_WORDS];

#ifdef __cplusplus
}
#endif

#endif /* APU_GP_H */
