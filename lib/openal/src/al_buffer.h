#ifndef AL_BUFFER_H
#define AL_BUFFER_H

#include <AL/al.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "apu_mem.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Maximum number of concurrent OpenAL buffers supported.
 */
#define AL_MAX_BUFFERS 2048   /* an engine precaches hundreds of sounds (Xash3D: up to 2048) */

/**
 * Internal OpenAL 1.1 Buffer Object.
 */
typedef struct ALbuffer {
    ALuint id;
    bool in_use;
    ALenum format;
    ALsizei frequency;
    ALsizei size;
    ALint channels;
    ALint bits;
    void *data_virt;
    uint32_t data_phys;
    NVAPU_PRD_ENTRY *prd_table_virt;
    uint32_t prd_table_phys;
    uint32_t prd_count;
    int ref_count;
    ALint loop_start;   /* AL_LOOP_POINTS_SOFT, in sample frames (whole buffer by default) */
    ALint loop_end;
    /* alBufferDataStatic: data_virt is the application's memory (not owned) and
     * data_phys 0; the voice maps `pages` (the physical page of each 4 KiB of the
     * data, then the library's own copy of the last partial page with the silent
     * tail, `tail_virt`) with the first byte at page_offset */
    bool is_static;
    uint32_t *pages;
    uint32_t page_count;
    uint32_t page_offset;
    void *tail_virt;
    uint32_t map_key;
    uint32_t held_bytes;   /* library memory this buffer holds (AL_XBOX_BUFFER_BYTES) */
} ALbuffer;

/*
 * ============================================================================
 * Internal Buffer Subsystem and Lifecycle Management Helpers
 * ============================================================================
 */

/**
 * Initialize the OpenAL buffer subsystem pool.
 */
void al_buffer_init_subsystem(void);

/**
 * Clean up all buffer allocations and reset the subsystem pool.
 */
void al_buffer_cleanup_subsystem(void);

/**
 * Retrieve an active buffer object pointer by its ID name.
 *
 * @param id OpenAL buffer ID (1..256).
 * @return Pointer to ALbuffer if valid and in use, NULL otherwise.
 */
ALbuffer *al_buffer_get(ALuint id);

/**
 * Increment the reference count of a buffer (attached to a source/queue).
 *
 * @param buf Pointer to ALbuffer object.
 */
void al_buffer_retain(ALbuffer *buf);

/**
 * Decrement the reference count of a buffer (detached from a source/queue).
 *
 * @param buf Pointer to ALbuffer object.
 */
void al_buffer_release(ALbuffer *buf);

/**
 * Internal error recording helpers.
 */
void alSetError(ALenum error);
void al_set_error(ALenum error);

/*
 * ============================================================================
 * OpenAL 1.1 Buffer API Prototypes
 * ============================================================================
 */
AL_API void AL_APIENTRY alGenBuffers(ALsizei n, ALuint *buffers);
AL_API void AL_APIENTRY alDeleteBuffers(ALsizei n, const ALuint *buffers);
AL_API ALboolean AL_APIENTRY alIsBuffer(ALuint buffer);
AL_API void AL_APIENTRY alBufferData(ALuint buffer, ALenum format, const ALvoid *data, ALsizei size, ALsizei freq);
AL_API void AL_APIENTRY alGetBufferi(ALuint buffer, ALenum param, ALint *value);
AL_API void AL_APIENTRY alGetBufferf(ALuint buffer, ALenum param, ALfloat *value);
AL_API void AL_APIENTRY alGetBufferiv(ALuint buffer, ALenum param, ALint *values);
AL_API void AL_APIENTRY alGetBufferfv(ALuint buffer, ALenum param, ALfloat *values);

/** Library memory held for buffer data (copies, silent tails, page lists), in bytes. */
uint32_t al_buffer_held_bytes(void);

/** Application memory the static buffers point at, in bytes. */
uint32_t al_buffer_static_bytes(void);

/** Write the CPU cache back before the APU reads application memory (alBufferDataStatic); default on. */
void al_buffer_debug_set_static_flush(bool on);

#ifdef __cplusplus
}
#endif

#endif /* AL_BUFFER_H */
