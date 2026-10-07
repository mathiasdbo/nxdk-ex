#ifndef WAV_LOADER_H
#define WAV_LOADER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <AL/al.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    ALenum format;           /* AL_FORMAT_MONO16 or AL_FORMAT_STEREO16 */
    const void *pcm_data;    /* Pointer to raw 16-bit PCM samples in memory */
    ALsizei pcm_size;        /* Total size of PCM data in bytes */
    ALsizei sample_rate;     /* Sampling frequency (e.g., 48000) */
    ALsizei channels;        /* 1 for Mono, 2 for Stereo */
    ALsizei bits_per_sample; /* 16 */
} wav_info_t;

/**
 * Parse an in-memory RIFF WAVE buffer.
 *
 * @param data Pointer to the complete WAV file in memory.
 * @param size Size of the WAV file buffer in bytes.
 * @param out_info Pointer to wav_info_t struct to populate.
 * @return 0 on success, negative error code on failure:
 *         -1: NULL parameter or buffer too small
 *         -2: Invalid RIFF or WAVE signature
 *         -3: Missing or corrupt 'fmt ' chunk
 *         -4: Unsupported audio format (only 16-bit PCM is supported)
 *         -5: Missing or corrupt 'data' chunk
 */
int wav_parse(const void *data, size_t size, wav_info_t *out_info);

/**
 * Load an in-memory WAV buffer directly into an OpenAL buffer object via alBufferData.
 *
 * @param buffer OpenAL buffer ID (allocated via alGenBuffers).
 * @param data Pointer to in-memory WAV file.
 * @param size Size of in-memory WAV file in bytes.
 * @return 0 on success, negative error code on failure.
 */
int wav_load_to_buffer(ALuint buffer, const void *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* WAV_LOADER_H */
