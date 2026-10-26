#ifndef APU_AC97_H
#define APU_AC97_H

/*
 * Sends the APU mix (GP output FIFO 0, apu_gp.c: interleaved 16-bit stereo in
 * a ring in RAM) to the AC97 codec. On a real console this is how the VP/GP
 * output becomes audible (verified with samples/apu_probe round 17). Two modes:
 *
 * Zero-copy (the default): the AC97's buffer lists (PCM out and S/PDIF) point
 * straight into the GP's ring, cut into APU_AC97_ZC_PIECE_BYTES pieces; the 32
 * entries form a queue of pieces (twice round an 8 KiB ring, ~85 ms). The codec
 * starts half a ring behind the GP's write position (~21 ms) and the two
 * clocks are locked on a console (samples/apu_clock_probe: 0.02 ppm), so the
 * CPU never touches the samples: each pass refills the entries played since
 * the last one and checks the distance between the GP and the codec. If it
 * leaves the middle half of the ring, a piece is played twice or skipped
 * (stats.slips). No interrupts are used.
 *
 * Copy: the CPU copies the ring into its own AC97 buffers. Used while an
 * output hook is installed (it needs the samples on the CPU) and with
 * apu_ac97_set_zero_copy(false). Under a hook the ring stays the 8 KiB one,
 * so a pump gap of more than ~37 ms (7/8 of it) loses audio, where the 64 KiB
 * ring (APU_VP_DBG_AC97_COPY) bridged ~300 ms: the thread pumps every 4-6 ms,
 * a pump from apu_vp_service() alone needs a frame rate above ~30 fps. No XAudio callback is used (it stops after
 * the first underrun, see samples/openal_showcase/showcase_audio.c).
 *
 * Both are kept going by a pump thread that runs every few milliseconds
 * (APU_AC97_PUMP_MS), so the sound keeps playing while the game does not call
 * the library (loading screens, long frames); the thread also runs the
 * library's hook (al_stream_scrub_tick). The thread is the only code that
 * touches the AC97 and the GP FIFO read position; apu_ac97_pump() from
 * apu_vp_service() does nothing while it runs. Without the thread
 * (apu_ac97_set_thread(false) before the start) the pump runs from
 * apu_vp_service() every frame, as before.
 *
 * On the host build every function is a no-op.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APU_AC97_CHUNK_FRAMES 512u   /* 10.7 ms per AC97 buffer */
#define APU_AC97_PRIME        4u     /* copy mode: silent buffers queued at start, the GP-to-codec lead (~43 ms) */
#define APU_AC97_AHEAD        8u     /* copy mode: most buffers queued ahead of the one playing (~85 ms) */
#define APU_AC97_PUMP_MS      4u     /* pump thread period */
/* The pump thread's stack. CreateThread's 0 would give it the XBE's main
 * stack size (1 MiB with Xash3D's link): a pass and the thread hook need a
 * few KiB. Raise it for an output callback that needs more. */
#ifndef APU_AC97_PUMP_STACK
#define APU_AC97_PUMP_STACK   (128u * 1024u)
#endif
#define APU_AC97_HOLD_WINDOW  96u    /* samples: a held voice is released half a pass (2 ms) before its frame at most */
#define APU_AC97_ZC_PIECE_BYTES 512u /* zero-copy: one buffer-list entry, 128 frames (2.67 ms) */

typedef struct {
    uint32_t chunks;        /* buffers handed to the AC97 */
    uint32_t underruns;     /* times the AC97 DMA had run dry and was restarted */
    uint32_t padded;        /* buffers completed with silence because the GP fell behind */
    uint32_t pumps;         /* pump passes (thread or apu_vp_service()) */
    uint32_t max_gap_ms;    /* longest time between two pump passes */
    bool threaded;          /* the pump thread is running */
    uint64_t cycles;        /* CPU cycles in pump passes, the thread hook and the output hook included */
    uint32_t max_cycles;    /* longest pass */
    uint64_t hook_cycles;   /* of which in the output hook */
    uint64_t fifo_cycles;   /* of which reading the GP's mix (apu_gp_fifo_read) */
    uint64_t queue_cycles;  /* of which copying blocks to the codec's buffers and queueing them */
    uint64_t thread_hook_cycles;   /* of which in the thread hook (scrubbing played stream data) */
    uint32_t cycle_hz;      /* rate of the cycle counter, measured since the start (0 until 10 ms have passed) */
    bool zero_copy;         /* the codec reads the GP ring directly */
    uint32_t slips;         /* zero-copy: pieces played twice or skipped to keep the distance */
    uint32_t lead_min;      /* zero-copy: frames the GP is ahead of the codec, least, most and last seen */
    uint32_t lead_max;
    uint32_t lead_last;
} apu_ac97_stats_t;

/*
 * Output hook: called with each block of the mix (APU_AC97_CHUNK_FRAMES
 * interleaved 16-bit stereo frames at 48 kHz, in cached memory) before it is
 * queued on the AC97; it may change the samples in place. It runs in the pump
 * thread (or from apu_ac97_pump() without the thread), so it must be quick and
 * must not call the library. NULL removes it.
 *
 * The hook needs the copy mode: installing one while zero-copy output runs
 * restarts the output in copy mode (a short gap), removing it goes back.
 */
typedef void (*apu_ac97_output_fn)(int16_t *samples, uint32_t frames, void *user);
void apu_ac97_set_output_hook(apu_ac97_output_fn fn, void *user);

/** Zero-copy output (default true) while no output hook is installed: from the next apu_ac97_start() or hook
 *  change on. apu_vp_init() sets it with the ring size (off with APU_VP_DBG_AC97_COPY). */
void apu_ac97_set_zero_copy(bool on);

/** Use the pump thread (default true) from the next apu_ac97_start() on. */
void apu_ac97_set_thread(bool on);

/** Function the pump thread also runs on every pass (al_stream_scrub_tick), or NULL. */
void apu_ac97_set_thread_hook(void (*fn)(void));

/**
 * Start AC97 output fed from the GP FIFO (drops the FIFO backlog first).
 * @return 0 on success, negative on allocation failure; 0 and inactive on the host.
 */
int apu_ac97_start(uintptr_t bar0);

/** Queue whatever the GP has written since the last call (non-blocking; nothing while the pump thread runs). */
void apu_ac97_pump(uintptr_t bar0);

/**
 * Stop the AC97 PCM-out DMA at once, without touching the driver's state
 * (shutdown notification: the program is ending, its buffers are about to be
 * freed and must not be read any more).
 */
void apu_ac97_halt_dma(void);

/** Stop the pump thread, pause the AC97 output and free the buffers. */
void apu_ac97_stop(void);

bool apu_ac97_active(void);

/** The output runs in zero-copy mode. */
bool apu_ac97_zero_copy(void);

/** The output thread runs (a console): it releases held voices (apu_vp_voice_hold). */
bool apu_ac97_threaded(void);
void apu_ac97_get_stats(apu_ac97_stats_t *out);

/*
 * Zero-copy arithmetic (byte offsets in a ring of `ring` bytes; exposed for the
 * host tests).
 *   first_piece: the piece the codec starts with, half a ring behind the
 *                GP's write position `w`.
 *   lead:        bytes the GP has written that the codec has not read.
 *   slip:        -1 to play a piece again (the codec is catching up with the
 *                GP), +1 to skip one (the GP is about to overwrite unread
 *                data), 0 while the lead is in the middle half of the ring.
 */
uint32_t apu_ac97_zc_first_piece(uint32_t w, uint32_t ring);
uint32_t apu_ac97_zc_lead(uint32_t w, uint32_t r, uint32_t ring);
int apu_ac97_zc_slip(uint32_t lead, uint32_t ring);

#ifdef __cplusplus
}
#endif

#endif /* APU_AC97_H */
