#ifndef APU_GP_H
#define APU_GP_H

/*
 * GP DSP stage of the MCPX APU (route B of lib/openal/docs/XEMU_VERIFICATION.md):
 * a DSP56300 program, written for this library, that copies the
 * front-left/right mixbins (bins 0 and 1, where the HRTF submixes land) to GP
 * output FIFO 0 every 32-sample frame as interleaved 16-bit stereo. The FIFO is
 * a ring in system RAM, so the CPU can forward the APU's mix to the AC97 codec
 * (apu_gp_fifo_read(), apu_ac97.c).
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
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APU_GP_FIFO_BYTES   (64u * 1024u)   /* output ring: 16384 stereo frames, ~341 ms */
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

/** Frames the GP program has run (its counter at X:$10, 24 bits). */
uint32_t apu_gp_frames(uintptr_t bar0);

/** Channel stride of the output DMA from the next apu_gp_init() on (diagnostics; default 32). */
void apu_gp_debug_set_dsp_step(uint32_t words);

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
