#ifndef AL_LISTENER_H
#define AL_LISTENER_H

#include <AL/al.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ============================================================================
 * Internal Listener Subsystem Helpers
 * ============================================================================
 */

/**
 * Initialize or reset global OpenAL listener state to defaults:
 * Gain = 1.0f, Position = (0, 0, 0), Velocity = (0, 0, 0),
 * Orientation = (0, 0, -1) [forward], (0, 1, 0) [up], (1, 0, 0) [right].
 */
void al_listener_init(void);

/**
 * Reset listener state back to default values.
 */
void al_listener_reset(void);

/**
 * Retrieve current global listener gain multiplier.
 *
 * @return Listener gain value (>= 0.0f).
 */
float al_listener_get_gain(void);

/**
 * Retrieve current global listener position in world coordinates.
 *
 * @param pos Destination 3-element float array (x, y, z).
 */
void al_listener_get_position(float pos[3]);

/**
 * Retrieve current global listener velocity in world coordinates.
 *
 * @param vel Destination 3-element float array (vx, vy, vz).
 */
void al_listener_get_velocity(float vel[3]);

/**
 * Retrieve current listener orthonormal basis vectors (Forward, Up, Right).
 *
 * @param forward Destination 3-element float array (may be NULL).
 * @param up      Destination 3-element float array (may be NULL).
 * @param right   Destination 3-element float array (may be NULL).
 */
void al_listener_get_basis(float forward[3], float up[3], float right[3]);

/**
 * Transform a 3D coordinate from world space into listener local coordinates.
 *
 * Coordinate Convention:
 * - d = P_world - P_listener
 * - x_local = d . R (Right axis: +Right, -Left)
 * - y_local = d . U' (Up axis: +Up, -Down)
 * - z_local = -d . F (Front/Depth axis: -Front, +Behind)
 *
 * Safe for in-place transformation where world_pos == local_pos.
 *
 * @param world_pos Source world 3D position (x, y, z).
 * @param local_pos Destination listener-relative 3D position (x, y, z).
 */
void al_listener_world_to_local(const float world_pos[3], float local_pos[3]);

/*
 * ============================================================================
 * OpenAL 1.1 Listener API Prototypes
 * ============================================================================
 */
AL_API void AL_APIENTRY alListenerf(ALenum param, ALfloat value);
AL_API void AL_APIENTRY alListener3f(ALenum param, ALfloat v1, ALfloat v2, ALfloat v3);
AL_API void AL_APIENTRY alListenerfv(ALenum param, const ALfloat *values);
AL_API void AL_APIENTRY alListeneri(ALenum param, ALint value);
AL_API void AL_APIENTRY alListener3i(ALenum param, ALint v1, ALint v2, ALint v3);
AL_API void AL_APIENTRY alListeneriv(ALenum param, const ALint *values);

AL_API void AL_APIENTRY alGetListenerf(ALenum param, ALfloat *value);
AL_API void AL_APIENTRY alGetListener3f(ALenum param, ALfloat *v1, ALfloat *v2, ALfloat *v3);
AL_API void AL_APIENTRY alGetListenerfv(ALenum param, ALfloat *values);
AL_API void AL_APIENTRY alGetListeneri(ALenum param, ALint *value);
AL_API void AL_APIENTRY alGetListener3i(ALenum param, ALint *v1, ALint *v2, ALint *v3);
AL_API void AL_APIENTRY alGetListeneriv(ALenum param, ALint *values);

#ifdef __cplusplus
}
#endif

#endif /* AL_LISTENER_H */
