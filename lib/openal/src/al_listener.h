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
 * Orientation = (0, 0, -1) [at], (0, 1, 0) [up].
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
