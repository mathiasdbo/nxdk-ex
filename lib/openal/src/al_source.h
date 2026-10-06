#ifndef AL_SOURCE_H
#define AL_SOURCE_H

#include <AL/al.h>
#include <stdbool.h>
#include <stdint.h>
#include "al_buffer.h"
#include "apu_voice.h"
#include "apu_hardware.h"
#include "apu_spatial.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Maximum number of concurrent OpenAL sources supported.
 */
#define AL_MAX_SOURCES 256

/**
 * Sentinel value indicating no hardware voice is currently bound.
 */
#define AL_HW_VOICE_INVALID (-1)

/**
 * Internal OpenAL 1.1 Source Object.
 */
typedef struct ALsource {
    ALuint id;
    bool in_use;
    ALint state; /* AL_INITIAL, AL_PLAYING, AL_PAUSED, AL_STOPPED */
    ALbuffer *buffer;
    int hw_voice_idx;
    ALboolean looping;
    ALfloat pitch;
    ALfloat gain;
    ALfloat min_gain;
    ALfloat max_gain;
    ALfloat position[3];
    ALfloat velocity[3];
    ALfloat direction[3];
    ALboolean source_relative;
    ALfloat reference_distance;
    ALfloat max_distance;
    ALfloat rolloff_factor;
    ALfloat cone_inner_angle;
    ALfloat cone_outer_angle;
    ALfloat cone_outer_gain;
} ALsource;

/*
 * ============================================================================
 * Internal Source Subsystem and Lifecycle Management Helpers
 * ============================================================================
 */

/**
 * Initialize the OpenAL source subsystem pool and voice tracker.
 */
void al_source_init_subsystem(void);

/**
 * Clean up all source allocations, halt active voices, and reset subsystem pool.
 */
void al_source_cleanup_subsystem(void);

/**
 * Retrieve an active source object pointer by its ID name.
 *
 * @param id OpenAL source ID (1..256).
 * @return Pointer to ALsource if valid and in use, NULL otherwise.
 */
ALsource *al_source_get(ALuint id);

/**
 * Override the APU base MMIO address used by the source subsystem (host testing support).
 *
 * @param base Base MMIO address, or 0 to restore default NV_PAPU_BASE.
 */
void al_source_set_apu_base(uintptr_t base);

/**
 * Get current APU base MMIO address used by the source subsystem.
 */
uintptr_t al_source_get_apu_base(void);

/**
 * Update hardware master volume words for all active playing sources.
 * Called immediately when global listener gain changes.
 */
void al_source_update_all_gains(void);

/**
 * Recompute 3D spatial attenuation, cone gains, and Doppler pitch steps
 * and update hardware voice context registers for all active playing sources.
 * Called immediately when listener position/velocity/orientation or global models change.
 */
void al_source_update_all_spatial(void);

/**
 * Perform complete 3D spatial calculation for a given source object.
 *
 * @param src  Source object pointer.
 * @param calc Destination spatial calculation result structure.
 */
void al_source_calc_spatial(const ALsource *src, AL_SPATIAL_CALC *calc);

/*
 * ============================================================================
 * OpenAL 1.1 Source API Prototypes
 * ============================================================================
 */
AL_API void AL_APIENTRY alGenSources(ALsizei n, ALuint *sources);
AL_API void AL_APIENTRY alDeleteSources(ALsizei n, const ALuint *sources);
AL_API ALboolean AL_APIENTRY alIsSource(ALuint source);

AL_API void AL_APIENTRY alSourcef(ALuint source, ALenum param, ALfloat value);
AL_API void AL_APIENTRY alSource3f(ALuint source, ALenum param, ALfloat v1, ALfloat v2, ALfloat v3);
AL_API void AL_APIENTRY alSourcefv(ALuint source, ALenum param, const ALfloat *values);
AL_API void AL_APIENTRY alSourcei(ALuint source, ALenum param, ALint value);
AL_API void AL_APIENTRY alSource3i(ALuint source, ALenum param, ALint v1, ALint v2, ALint v3);
AL_API void AL_APIENTRY alSourceiv(ALuint source, ALenum param, const ALint *values);

AL_API void AL_APIENTRY alGetSourcef(ALuint source, ALenum param, ALfloat *value);
AL_API void AL_APIENTRY alGetSource3f(ALuint source, ALenum param, ALfloat *v1, ALfloat *v2, ALfloat *v3);
AL_API void AL_APIENTRY alGetSourcefv(ALuint source, ALenum param, ALfloat *values);
AL_API void AL_APIENTRY alGetSourcei(ALuint source, ALenum param, ALint *value);
AL_API void AL_APIENTRY alGetSource3i(ALuint source, ALenum param, ALint *v1, ALint *v2, ALint *v3);
AL_API void AL_APIENTRY alGetSourceiv(ALuint source, ALenum param, ALint *values);

AL_API void AL_APIENTRY alSourcePlay(ALuint source);
AL_API void AL_APIENTRY alSourcePause(ALuint source);
AL_API void AL_APIENTRY alSourceStop(ALuint source);
AL_API void AL_APIENTRY alSourceRewind(ALuint source);

AL_API void AL_APIENTRY alSourcePlayv(ALsizei n, const ALuint *sources);
AL_API void AL_APIENTRY alSourcePausev(ALsizei n, const ALuint *sources);
AL_API void AL_APIENTRY alSourceStopv(ALsizei n, const ALuint *sources);
AL_API void AL_APIENTRY alSourceRewindv(ALsizei n, const ALuint *sources);

AL_API void AL_APIENTRY alSourceQueueBuffers(ALuint source, ALsizei nb, const ALuint *buffers);
AL_API void AL_APIENTRY alSourceUnqueueBuffers(ALuint source, ALsizei nb, ALuint *buffers);

#ifdef __cplusplus
}
#endif

#endif /* AL_SOURCE_H */
