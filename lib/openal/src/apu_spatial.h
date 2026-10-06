#ifndef APU_SPATIAL_H
#define APU_SPATIAL_H

#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include "al_listener.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 3D Spatial Calculation Result Structure.
 * Holds calculated listener-relative coordinates, distance, attenuation gains,
 * and Doppler pitch multipliers for an OpenAL source.
 */
typedef struct AL_SPATIAL_CALC {
    float local_pos[3];    /**< Source position in listener local frame: right, up, front */
    float distance;        /**< Euclidean distance between source and listener */
    float distance_gain;   /**< Distance attenuation gain factor [0.0..1.0+] */
    float cone_gain;       /**< Directional sound cone gain factor [0.0..1.0] */
    float effective_gain;  /**< Combined clamped gain multiplied by listener gain */
    float doppler_pitch;   /**< Doppler pitch multiplier (>1 approaching, <1 receding) */
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

#ifdef __cplusplus
}
#endif

#endif /* APU_SPATIAL_H */
