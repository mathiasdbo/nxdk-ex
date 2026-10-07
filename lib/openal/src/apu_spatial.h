#ifndef APU_SPATIAL_H
#define APU_SPATIAL_H

#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include "al_listener.h"
#include "apu_hardware.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ============================================================================
 * Woodworth ITD & Circular Buffer Constants
 * ============================================================================
 * In 3D spatial mode (mode_3d = 1), each of the 64 voices has a dedicated
 * circular ITD delay buffer in contiguous RAM:
 *   - 256 bytes per voice (128 16-bit delay samples)
 *   - 128-byte hardware alignment requirement
 *   - Total pool: 64 voices * 256 bytes = 16384 bytes (16 KB)
 * ============================================================================
 */
#define NV_PAPU_ITD_BUFFER_SIZE_PER_VOICE   256u
#define NV_PAPU_ITD_BUFFER_ALIGN            128u
#define NV_PAPU_ITD_POOL_SIZE               (NV_PAPU_NUM_3D_VOICES * NV_PAPU_ITD_BUFFER_SIZE_PER_VOICE) /* 16384 bytes */

/**
 * 3D Spatial Calculation Result Structure.
 * Holds calculated listener-relative coordinates, distance, attenuation gains,
 * Doppler pitch multipliers, and Woodworth ITD delay taps for an OpenAL source.
 */
typedef struct AL_SPATIAL_CALC {
    float local_pos[3];       /**< Source position in listener local frame: right, up, front */
    float distance;           /**< Euclidean distance between source and listener */
    float distance_gain;      /**< Distance attenuation gain factor [0.0..1.0+] */
    float cone_gain;          /**< Directional sound cone gain factor [0.0..1.0] */
    float effective_gain;     /**< Combined clamped gain multiplied by listener gain */
    float doppler_pitch;      /**< Doppler pitch multiplier (>1 approaching, <1 receding) */
    uint16_t itd_delay_left;  /**< ITD left ear delay tap in samples [0..64] */
    uint16_t itd_delay_right; /**< ITD right ear delay tap in samples [0..64] */
} AL_SPATIAL_CALC;

/**
 * Spatial calculation parameters alias for hardware/DSP subsystem.
 */
typedef AL_SPATIAL_CALC AL_SPATIAL_PARAMS;

/*
 * ============================================================================
 * Global Distance Model & Doppler State Functions
 * ============================================================================
 */

/**
 * Set the global OpenAL distance attenuation model.
 *
 * @param model Distance model enum (e.g. AL_INVERSE_DISTANCE_CLAMPED, AL_NONE, etc.).
 */
void apu_spatial_set_distance_model(ALenum model);

/**
 * Get the current global OpenAL distance attenuation model.
 *
 * @return Active distance model enum.
 */
ALenum apu_spatial_get_distance_model(void);

/**
 * Set the global Doppler shift scaling factor.
 *
 * @param factor Non-negative Doppler factor multiplier (default 1.0f).
 */
void apu_spatial_set_doppler_factor(float factor);

/**
 * Get the current global Doppler factor.
 *
 * @return Current Doppler factor multiplier.
 */
float apu_spatial_get_doppler_factor(void);

/**
 * Set the global speed of sound for Doppler shift calculations.
 *
 * @param speed Speed of sound in units per second (default 343.3f m/s).
 */
void apu_spatial_set_speed_of_sound(float speed);

/**
 * Get the current global speed of sound.
 *
 * @return Speed of sound value.
 */
float apu_spatial_get_speed_of_sound(void);

/**
 * Reset all spatial calculation state parameters (distance model, Doppler factor, speed of sound)
 * to their default specification values.
 */
void apu_spatial_reset(void);

/*
 * ============================================================================
 * Spatial Calculation Helpers
 * ============================================================================
 */

/**
 * Calculate distance attenuation gain for any supported OpenAL distance model.
 *
 * @param distance Euclidean distance between source and listener.
 * @param ref_dist Reference distance at which gain is 1.0f.
 * @param max_dist Maximum distance for clamped models.
 * @param rolloff  Rolloff factor controlling attenuation rate.
 * @param model    Active OpenAL distance model enum.
 * @return Attenuation gain multiplier (>= 0.0f).
 */
float apu_calc_distance_gain(float distance, float ref_dist, float max_dist, float rolloff, ALenum model);

/**
 * Calculate directional sound cone attenuation gain.
 *
 * @param source_pos   Source world 3D coordinates.
 * @param source_dir   Source direction vector.
 * @param listener_pos Listener world 3D coordinates.
 * @param inner_deg    Inner cone full angle in degrees [0..360].
 * @param outer_deg    Outer cone full angle in degrees [0..360].
 * @param outer_gain   Gain multiplier outside outer cone [0.0..1.0].
 * @return Cone attenuation gain multiplier [0.0..1.0].
 */
float apu_calc_cone_gain(const float source_pos[3], const float source_dir[3], const float listener_pos[3],
                         float inner_deg, float outer_deg, float outer_gain);

/**
 * Calculate Doppler pitch shift multiplier based on line-of-sight velocity projections.
 *
 * @param source_pos      Source 3D position vector.
 * @param source_vel      Source 3D velocity vector.
 * @param listener_pos    Listener 3D position vector.
 * @param listener_vel    Listener 3D velocity vector.
 * @param doppler_factor  Global Doppler factor multiplier.
 * @param speed_of_sound  Global speed of sound.
 * @return Frequency multiplier (> 1.0 approaching, < 1.0 receding, 1.0 stationary).
 */
float apu_calc_doppler_pitch(const float source_pos[3], const float source_vel[3],
                            const float listener_pos[3], const float listener_vel[3],
                            float doppler_factor, float speed_of_sound);

/*
 * ============================================================================
 * Woodworth Interaural Time Difference (ITD) Engine
 * ============================================================================
 */

/**
 * Calculate the Woodworth Interaural Time Difference (ITD) delay in samples at 48 kHz.
 *
 * Formula: Delay = round(31.0f * (0.55f * |x_rel| + 0.45f * |x_rel|^3))
 * Clamps |x_rel| to [0.0f, 1.0f] and Delay to [0, 64].
 *
 * @param x_rel Normalized relative horizontal offset (x_local / distance).
 * @return ITD delay in samples [0..64].
 */
uint16_t apu_calc_itd_delay_samples(float x_rel);

/**
 * Calculate left and right ear ITD delay taps based on relative horizontal angle.
 *
 * Ear Tap Assignment:
 *   - If x_rel > 0 (Source to the right): delay_left = Delay, delay_right = 0
 *   - If x_rel < 0 (Source to the left):  delay_left = 0,     delay_right = Delay
 *   - If x_rel == 0 (Source centered):    delay_left = 0,     delay_right = 0
 *
 * @param x_rel       Normalized relative horizontal offset (x_local / distance).
 * @param delay_left  Pointer to store left ear delay tap in samples (0..64).
 * @param delay_right Pointer to store right ear delay tap in samples (0..64).
 */
void apu_calc_itd_taps(float x_rel, uint16_t *delay_left, uint16_t *delay_right);

/*
 * ============================================================================
 * ITD Circular Buffer Subsystem API
 * ============================================================================
 */

/**
 * Initialize the hardware ITD circular buffer subsystem pool.
 * Allocates 16 KB (64 voices * 256 bytes) contiguous physical RAM aligned to 128 bytes
 * and zeroes the memory pool.
 *
 * @return 0 on success, negative error code on failure.
 */
int apu_itd_subsystem_init(void);

/**
 * Deinitialize the hardware ITD circular buffer pool and free physical memory.
 */
void apu_itd_subsystem_deinit(void);

/**
 * Retrieve the 32-bit physical RAM DMA address of a voice's ITD delay buffer.
 *
 * @param voice_index Voice hardware slot index (0..63).
 * @return Physical 128-byte aligned address, or 0 if uninitialized or invalid voice.
 */
uint32_t apu_itd_get_voice_buffer_phys(uint32_t voice_index);

/**
 * Retrieve the virtual memory pointer to a voice's ITD delay buffer.
 *
 * @param voice_index Voice hardware slot index (0..63).
 * @return Virtual address pointer, or NULL if uninitialized or invalid voice.
 */
void *apu_itd_get_voice_buffer_virt(uint32_t voice_index);

#ifdef __cplusplus
}
#endif

#endif /* APU_SPATIAL_H */
