#ifndef AL_SOURCE_H
#define AL_SOURCE_H

#include <AL/al.h>
#include <AL/alext.h>
#include <stdbool.h>
#include <stdint.h>
#include "al_buffer.h"
#include "al_stream.h"
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
    ALfloat lfe_gain;
    uint32_t saved_prd_index;        /* Preserved PRD index upon hardware preemption */
    uint32_t saved_sample_pos_frac;   /* Preserved sample position fractional phase upon preemption */
    uint32_t play_seq;               /* Start order (monotonic, set by alSourcePlay); oldest loses priority ties */
    ALint source_type;               /* AL_UNDETERMINED, AL_STATIC (AL_BUFFER) or AL_STREAMING (queued buffers) */
    al_stream_t *stream;             /* queue and ring of a streaming source, NULL otherwise */
    bool has_offset;                 /* offset_frames applies at the next voice start */
    uint32_t offset_frames;          /* AL_*_OFFSET set while not playing, or a preempted voice's position */
    bool direct;                     /* AL_XBOX_DIRECT_MODE: engine gains, no 3D model */
    ALfloat direct_gain[2];          /* AL_XBOX_DIRECT_GAINS: left, right */
    ALfloat xbox_priority;           /* AL_XBOX_PRIORITY: >= 0 engine priority, < 0 the library's */
    ALboolean virtualize;            /* AL_XBOX_VIRTUALIZE */
} ALsource;


/*
 * ============================================================================
 * Internal Source Subsystem and Lifecycle Management Helpers
 * ============================================================================
 */

/**
 * Reset an OpenAL source object to default state.
 *
 * @param src Source to reset.
 */
void al_source_reset(ALsource *src);

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
 * @param base Base MMIO address, or 0 to restore the default: NV_PAPU_BASE
 *             with -DOPENAL_APU_REAL_MMIO, otherwise an internal RAM stand-in
 *             for the voice ACTIVE/PAUSE registers (never the real BAR0).
 */
void al_source_set_apu_base(uintptr_t base);

/**
 * Get current APU base MMIO address used by the source subsystem
 * (the default described at al_source_set_apu_base() when none is set).
 */
uintptr_t al_source_get_apu_base(void);

/**
 * Update hardware master volume words for all playing or paused sources that
 * hold a hardware voice. Called immediately when global listener gain changes.
 */
void al_source_update_all_gains(void);

/**
 * Recompute 3D spatial attenuation, cone gains, and Doppler pitch steps
 * and update hardware voice context registers for all playing or paused
 * sources that hold a hardware voice (one spatial computation per source).
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

/**
 * Perform complete 3D spatial calculation for a given source object (mutable alias).
 *
 * @param src  Source object pointer.
 * @param calc Destination spatial calculation result structure.
 */
void al_source_compute_spatial(ALsource *src, AL_SPATIAL_CALC *calc);

/**
 * Update APU voice manager frame tick (refill streaming sources, detect
 * completed voices, promote virtual sources).
 */
void al_source_update_frame(void);

/**
 * The buffer a source's voice plays: its static buffer, or the ring of a
 * streaming source (NULL if there is nothing to play).
 */
const ALbuffer *al_source_play_buffer(const ALsource *src);

/**
 * True if the source's voice loops in hardware: AL_LOOPING, or a streaming
 * source (its ring always loops; the library ends it). Such a voice never
 * finishes on its own.
 */
bool al_source_voice_loops(const ALsource *src);

/**
 * The source is about to lose its hardware voice (stop, release, preemption):
 * a streaming source's ring stops being scrubbed by the output thread.
 * Call before the voice is stopped or given to another source.
 */
void al_source_voice_lost(ALsource *src);

/**
 * Configure hardware voice context and trigger playback for a source.
 *
 * @param src Source to program into hardware.
 * @param hw_voice_idx Hardware voice slot index (0..63).
 */
void al_source_program_hw_voice(ALsource *src, uint32_t hw_voice_idx);

/*
 * ----------------------------------------------------------------------------
 * Diagnostics
 * ----------------------------------------------------------------------------
 */

/**
 * Number of full spatial computations (al_source_compute_spatial calls) since
 * program start; wraps at 2^32. Profiling and regression-test aid only.
 */
uint32_t al_source_debug_spatial_calc_count(void);

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
