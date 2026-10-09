#ifndef APU_AC97_H
#define APU_AC97_H

/*
 * Forwards the APU mix (GP output FIFO 0, apu_gp.c) to the AC97 codec through
 * nxdk's XAudio driver. On a real console this is how the VP/GP output becomes
 * audible: the GP writes interleaved 16-bit stereo into a ring in RAM and the
 * CPU queues it on the AC97 PCM-out DMA (verified with samples/apu_probe round
 * 17). The descriptor list is kept filled from apu_ac97_pump(), called from
 * apu_vp_service() every frame; no XAudio callback is used (it stops after the
 * first underrun, see samples/openal_showcase/showcase_audio.c).
 *
 * On the host build every function is a no-op.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APU_AC97_CHUNK_FRAMES 512u   /* 10.7 ms per AC97 buffer */
#define APU_AC97_PRIME        4u     /* silent buffers queued at start: the GP-to-codec lead (~43 ms) */
#define APU_AC97_AHEAD        8u     /* most buffers queued ahead of the one playing (~85 ms) */

typedef struct {
    uint32_t chunks;        /* buffers handed to the AC97 */
    uint32_t underruns;     /* times the AC97 DMA had run dry and was restarted */
    uint32_t padded;        /* buffers completed with silence because the GP fell behind */
} apu_ac97_stats_t;

/**
 * Start AC97 output fed from the GP FIFO (drops the FIFO backlog first).
 * @return 0 on success, negative on allocation failure; 0 and inactive on the host.
 */
int apu_ac97_start(uintptr_t bar0);

/** Queue whatever the GP has written since the last call (non-blocking). */
void apu_ac97_pump(uintptr_t bar0);

/** Pause the AC97 output and free the buffers. */
void apu_ac97_stop(void);

bool apu_ac97_active(void);
void apu_ac97_get_stats(apu_ac97_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* APU_AC97_H */
