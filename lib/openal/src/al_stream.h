#ifndef AL_STREAM_H
#define AL_STREAM_H

/*
 * Buffer queueing (OpenAL 1.1 streaming sources) on the APU.
 *
 * A streaming source plays one voice that loops over a ring in APU memory, and
 * the library copies the queued buffers into the ring ahead of the play
 * position. The voice never reaches an end and its record is never rewritten
 * while it plays, so this adds no new way to stop the APU (a voice that ends
 * on a real console stops all frame processing, XEMU_VERIFICATION.md 8.1b).
 *
 *   stream position   frames since the start of the queue (wraps at 2^32)
 *   read_pos          position the voice has played up to (from its CBO)
 *   write_pos         position up to which queued data is in the ring
 *   ring index        (position - base) mod AL_STREAM_RING_FRAMES, where base
 *                     is read_pos when the voice (re)started at ring index 0
 *
 * Data is written at most AL_STREAM_LEAD_FRAMES ahead of the play position, and
 * the rest of the ring is kept silent: if the application stops calling the
 * library, the voice plays the data it has, then silence, and only replays old
 * data after a whole ring (AL_STREAM_RING_FRAMES) without an update.
 *
 * This module only keeps the queue and the ring. al_source.c owns the source
 * state and the voice; it reports the voice's ring position through
 * al_stream_update() once per frame.
 */

#include <AL/al.h>
#include <stdbool.h>
#include <stdint.h>
#include "al_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AL_STREAM_MAX_STREAMS   32u      /* streaming sources at once */
#define AL_STREAM_MAX_QUEUE     64u      /* buffers queued on one source */
#define AL_STREAM_RING_FRAMES   16384u   /* power of two: 341 ms at 48 kHz, 1.49 s at 11 kHz */
#define AL_STREAM_LEAD_FRAMES   8192u    /* most frames of data written ahead of the play position */

typedef struct al_stream al_stream_t;

/** Free every stream and ring (subsystem cleanup). */
void al_stream_shutdown(void);

/** A new, empty stream, or NULL if all AL_STREAM_MAX_STREAMS are in use. */
al_stream_t *al_stream_create(void);

/** Release every queued buffer and free the stream. */
void al_stream_destroy(al_stream_t *s);

/**
 * Append buffers to the queue (alSourceQueueBuffers). Every buffer must have
 * the format and frequency of the first one with data.
 * @return AL_NO_ERROR, AL_INVALID_NAME (an id is not a buffer),
 *         AL_INVALID_OPERATION (format mismatch, or the queue is full) or
 *         AL_OUT_OF_MEMORY (no ring); nothing is queued on an error.
 */
ALenum al_stream_queue(al_stream_t *s, ALsizei n, const ALuint *ids);

/**
 * Remove the n oldest processed buffers (alSourceUnqueueBuffers) and store their ids.
 * @return AL_NO_ERROR or AL_INVALID_VALUE (fewer than n are processed).
 */
ALenum al_stream_unqueue(al_stream_t *s, ALsizei n, ALuint *ids);

uint32_t al_stream_queued(const al_stream_t *s);
uint32_t al_stream_processed(const al_stream_t *s);

/** Total frames of data in the queue. */
uint32_t al_stream_total_frames(const al_stream_t *s);

/**
 * The ring as a buffer the voice layer can play (format, frequency, physical
 * address and size), or NULL before a buffer with data has been queued.
 */
const ALbuffer *al_stream_ring_buffer(const al_stream_t *s);

/** Id of the queued buffer being played (AL_BUFFER), 0 if none. */
ALuint al_stream_current_buffer(const al_stream_t *s);

/** Back to the start of the queue, nothing processed (alSourcePlay from INITIAL/STOPPED, alSourceRewind). */
void al_stream_rewind(al_stream_t *s);

/** Mark every queued buffer processed (alSourceStop, or the end of the data). */
void al_stream_finish(al_stream_t *s);

/**
 * A voice starts playing the ring at index 0 from the current read position:
 * fill the ring for it. Call before the voice is triggered.
 */
void al_stream_voice_start(al_stream_t *s, bool looping);

/**
 * Advance by the voice's ring position (CBO, 0..AL_STREAM_RING_FRAMES-1), mark
 * the buffers it finished as processed and refill the ring.
 * @return true when a non-looping stream has played all its data.
 */
bool al_stream_update(al_stream_t *s, uint32_t ring_pos, bool looping);

/** Times the play position caught up with the queued data (the application queued too late). */
uint32_t al_stream_starved(const al_stream_t *s);

/*
 * Output-thread scrubbing. While armed, al_stream_scrub_tick() (run by the AC97
 * pump thread, apu_ac97.c) overwrites with silence the ring data the voice on
 * `slot` has played, so a stream the game stops servicing (a loading screen)
 * plays out its data and then silence, never its old ring content. Arm after
 * the voice is triggered; disarm before its voice or ring changes (it waits
 * for a scrub pass in progress).
 */
void al_stream_arm(al_stream_t *s, uint32_t slot);
void al_stream_disarm(al_stream_t *s);
void al_stream_scrub_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* AL_STREAM_H */
