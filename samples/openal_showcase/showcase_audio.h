#ifndef SHOWCASE_AUDIO_H
#define SHOWCASE_AUDIO_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Software rendering of the OpenAL sources to the AC97 stereo output.
 *
 * The library's default (null) backend computes the spatial model but drives
 * no hardware, so nothing is heard. This mixer makes that model audible: it
 * walks the library's sources and, for every source that holds a voice slot,
 * resamples its buffer with the source pitch and the Doppler factor, applies
 * the library's Q14 elevation biquad, delays each ear by the library's ITD
 * taps and folds the library's 5.1 pan gains down to stereo, scaled by the
 * effective gain. 2D (stereo) buffers play straight to left/right.
 *
 * When a one-shot buffer runs out, the mixer clears the voice's ACTIVE bit in
 * the null backend's register stand-in, as the voice processor would, so the
 * library's voice manager reaps the source and frees the slot.
 *
 * It runs only while the library reports the null backend; with a real APU
 * backend the APU would play the sources itself.
 */

#define SHOWCASE_AUDIO_RATE         48000
#define SHOWCASE_AUDIO_CHUNK_FRAMES 512

/**
 * Start AC97 playback (Xbox) and reset the mixer.
 * @return 0 on success, negative if audio is unavailable.
 */
int showcase_audio_init(void);

/**
 * Mix ahead so the output never runs dry. Call once per main-loop frame,
 * after showcase_app_update().
 */
void showcase_audio_update(void);

/**
 * Stop playback and release the mixer's buffers.
 */
void showcase_audio_shutdown(void);

/**
 * Mix the current OpenAL state into interleaved stereo 16-bit frames
 * (the core of showcase_audio_update; exposed for host tests).
 * @param out    Destination, 2 * frames samples.
 * @param frames Number of stereo frames.
 */
void showcase_audio_mix(int16_t *out, uint32_t frames);

/**
 * Sources mixed in the last chunk, and chunks the AC97 output had to fill
 * with silence because the main loop had not mixed ahead (underruns).
 */
void showcase_audio_get_stats(uint32_t *voices, uint32_t *underruns);

/**
 * True when the mixer is rendering (null backend and output started).
 */
bool showcase_audio_active(void);

#ifdef __cplusplus
}
#endif

#endif /* SHOWCASE_AUDIO_H */
