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
 * default: the frame loop dsp/gp_loop.asm at P:0 and kernel 0, the reverb
 * (dsp/gp_kfreeverb.asm), at P:$100 (the copy program while the kernel is off).
 * The program must end each frame like the copy program and keep the frame
 * counter at X:$10 (apu_vp_init checks it). The mailbox, the burn and the
 * reverb need the default image.
 */
void apu_gp_debug_set_program(const uint32_t *words, uint32_t count);

/**
 * Size of the GP scratch space from the next apu_gp_init(), in bytes (rounded up
 * to 4 KiB, at least 0x2000, at most 2 MiB; 0 = what the program needs: the
 * reverb's delay lines for the default program). Offsets 0..$1FFF hold the
 * bootstrap image; the rest is free for the program's DMA (zeroed at init).
 */
void apu_gp_debug_set_scratch_bytes(uint32_t bytes);

/** Size of the scratch space in use (0 before apu_gp_init()). */
uint32_t apu_gp_scratch_bytes(void);

/**
 * Mailbox of the frame loop (EFX_PLAN.md 3.4): the `n` (address, value) X words
 * (at most 63) land together at the start of one frame. A timed post still
 * waiting (apu_gp_kernel_params_at) is let through first; then this waits for
 * the previous post to be applied (at most ~20 ms). Returns 0, 1 if the GP took
 * no frames (the words were written directly), -1 without the default image.
 */
int apu_gp_mailbox(uintptr_t bar0, const apu_reverb_word_t *w, uint32_t n);

/** A mailbox post has not been applied yet (a timed one waiting for its frame, or the next frame). */
bool apu_gp_mailbox_pending(uintptr_t bar0);

/** Host builds (no GP): run `n` frames of the loop's frame counter and timed mailbox (tests). */
void apu_gp_debug_host_frames(uintptr_t bar0, uint32_t n);

/** Frames from an empty mailbox post to the loop's acknowledgement (diagnostics; 0xFFFFFFFF on failure). */
uint32_t apu_gp_debug_mailbox_frames(uintptr_t bar0);

/** Cycles of nop the loop adds to every frame while the kernel runs (diagnostics: headroom; 0 = none). */
void apu_gp_debug_set_burn(uintptr_t bar0, uint32_t cycles);

/**
 * The reverb, kernel 0 of the default image (apu_reverb.h). `on` with new
 * parameters: if only gains change they go through the mailbox (one frame); a
 * new pre-delay (or switching on) stops the kernel for a frame, lays the delay
 * lines out again and empties them. Switching on stops a custom kernel and puts
 * the reverb's code back in P memory. Returns -1 if the GP runs another program.
 */
int apu_gp_reverb_set(uintptr_t bar0, const struct apu_reverb_params_s *p, bool on);

/** The reverb is running. */
bool apu_gp_reverb_on(void);

/*
 * Custom kernels (AL_XBOX_gp_program, EFX_PLAN.md 3.6; the ABI in dsp/gp_loop.asm).
 * An image is APU_GP_IMAGE_HEADER words (magic, ABI version, scratch bytes,
 * DMA descriptors a frame) and the code loaded at P:$100, as
 * tools/dsp56asm.py --image writes it.
 */
#define APU_GP_IMAGE_MAGIC            0x47504Bu   /* 'GPK' */
#define APU_GP_IMAGE_ABI              1u
#define APU_GP_IMAGE_HEADER           4u
#define APU_GP_KERNEL_MAX_WORDS       0x700u      /* P:$100..$7FF */
#define APU_GP_KERNEL_MAX_DESCRIPTORS 60u
#define APU_GP_KERNEL_PARAMS          32u

typedef struct {
    const uint32_t *code;       /* the words for P:$100 on */
    uint32_t words;
    uint32_t scratch_bytes;     /* from scratch byte $2000 */
    uint32_t descriptors;
} apu_gp_kernel_t;

/**
 * Check an image: magic, ABI, code size, 24-bit words, descriptors, and scratch
 * within apu_gp_kernel_scratch_max(). Fills `out` (pointing into `image`).
 * Returns 0, or -1 if the image is not usable.
 */
int apu_gp_kernel_parse(const uint32_t *image, uint32_t count, apu_gp_kernel_t *out);

/** Scratch bytes a kernel may use: the open device's space above $2000, else what the next apu_gp_init() allocates. */
uint32_t apu_gp_kernel_scratch_max(void);

/**
 * Run kernel `k` in place of the reverb (`on`), or stop it. A kernel already
 * running with the same code only gets its changed parameters, through the
 * mailbox (one frame). Otherwise the kernel stops for two frames, the code is
 * written to P:$100 (read back; on a mismatch the GP restarts from the
 * bootstrap image, which holds it too), the kernel's X is set up (ABI) and its
 * scratch zeroed, and it starts. Returns 0, -1 if the GP cannot run it.
 */
int apu_gp_kernel_set(uintptr_t bar0, const apu_gp_kernel_t *k, const uint32_t params[APU_GP_KERNEL_PARAMS], bool on);

/**
 * The running kernel `k` (same code) gets new parameters at the start of GP
 * frame `frame` (apu_gp_frames(); a frame already past means the next one):
 * the changed words, all in that frame. One such post waits at a time.
 * Returns 0, 1 while an earlier post is still waiting (try again later), -1 if
 * `k` is not the kernel running.
 */
int apu_gp_kernel_params_at(uintptr_t bar0, const apu_gp_kernel_t *k, const uint32_t params[APU_GP_KERNEL_PARAMS],
                            uint32_t frame);

/** A custom kernel is running. */
bool apu_gp_kernel_on(void);

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
