#include "al_listener.h"
#include "al_source.h"
#include "al_buffer.h"
#include <string.h>

/*
 * ============================================================================
 * Global Listener State
 * ============================================================================
 */
static float g_listener_gain = 1.0f;
static float g_listener_pos[3] = {0.0f, 0.0f, 0.0f};
static float g_listener_vel[3] = {0.0f, 0.0f, 0.0f};
static float g_listener_ori[6] = {0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f};
static bool s_listener_initialized = false;

/*
 * ============================================================================
 * Internal Listener Helpers
 * ============================================================================
 */

void al_listener_init(void) {
    g_listener_gain = 1.0f;
    g_listener_pos[0] = 0.0f;
    g_listener_pos[1] = 0.0f;
    g_listener_pos[2] = 0.0f;
    g_listener_vel[0] = 0.0f;
    g_listener_vel[1] = 0.0f;
    g_listener_vel[2] = 0.0f;
    g_listener_ori[0] = 0.0f;  /* At vector x */
    g_listener_ori[1] = 0.0f;  /* At vector y */
    g_listener_ori[2] = -1.0f; /* At vector z */
    g_listener_ori[3] = 0.0f;  /* Up vector x */
    g_listener_ori[4] = 1.0f;  /* Up vector y */
    g_listener_ori[5] = 0.0f;  /* Up vector z */
    s_listener_initialized = true;
}

void al_listener_reset(void) {
    al_listener_init();
}

static void ensure_listener_initialized(void) {
    if (!s_listener_initialized) {
        al_listener_init();
    }
}

float al_listener_get_gain(void) {
    ensure_listener_initialized();
    return g_listener_gain;
}

/*
 * ============================================================================
 * OpenAL 1.1 Listener Configuration APIs
 * ============================================================================
 */

AL_API void AL_APIENTRY alListenerf(ALenum param, ALfloat value) {
    ensure_listener_initialized();

    switch (param) {
        case AL_GAIN:
            if (value < 0.0f) {
                alSetError(AL_INVALID_VALUE);
                return;
            }
            g_listener_gain = value;
            /* Real-time sync: immediately rescale all active playing voices */
            al_source_update_all_gains();
            break;

        default:
            alSetError(AL_INVALID_ENUM);
            break;
    }
}

AL_API void AL_APIENTRY alListener3f(ALenum param, ALfloat v1, ALfloat v2, ALfloat v3) {
    ensure_listener_initialized();

    switch (param) {
        case AL_POSITION:
            g_listener_pos[0] = v1;
            g_listener_pos[1] = v2;
            g_listener_pos[2] = v3;
            break;

        case AL_VELOCITY:
            g_listener_vel[0] = v1;
            g_listener_vel[1] = v2;
            g_listener_vel[2] = v3;
            break;

        default:
            alSetError(AL_INVALID_ENUM);
            break;
    }
}

AL_API void AL_APIENTRY alListenerfv(ALenum param, const ALfloat *values) {
    if (!values) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    ensure_listener_initialized();

    switch (param) {
        case AL_GAIN:
            alListenerf(param, values[0]);
            break;

        case AL_POSITION:
        case AL_VELOCITY:
            alListener3f(param, values[0], values[1], values[2]);
            break;

        case AL_ORIENTATION:
            memcpy(g_listener_ori, values, sizeof(g_listener_ori));
            break;

        default:
            alSetError(AL_INVALID_ENUM);
            break;
    }
}

AL_API void AL_APIENTRY alListeneri(ALenum param, ALint value) {
    alListenerf(param, (ALfloat)value);
}

AL_API void AL_APIENTRY alListener3i(ALenum param, ALint v1, ALint v2, ALint v3) {
    alListener3f(param, (ALfloat)v1, (ALfloat)v2, (ALfloat)v3);
}

AL_API void AL_APIENTRY alListeneriv(ALenum param, const ALint *values) {
    if (!values) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    ensure_listener_initialized();

    switch (param) {
        case AL_GAIN:
            alListenerf(param, (ALfloat)values[0]);
            break;

        case AL_POSITION:
        case AL_VELOCITY:
            alListener3f(param, (ALfloat)values[0], (ALfloat)values[1], (ALfloat)values[2]);
            break;

        case AL_ORIENTATION: {
            ALfloat ori[6];
            for (int i = 0; i < 6; i++) {
                ori[i] = (ALfloat)values[i];
            }
            alListenerfv(param, ori);
            break;
        }

        default:
            alSetError(AL_INVALID_ENUM);
            break;
    }
}

/*
 * ============================================================================
 * OpenAL 1.1 Listener Query APIs
 * ============================================================================
 */

AL_API void AL_APIENTRY alGetListenerf(ALenum param, ALfloat *value) {
    if (!value) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    ensure_listener_initialized();

    switch (param) {
        case AL_GAIN:
            *value = g_listener_gain;
            break;

        default:
            alSetError(AL_INVALID_ENUM);
            break;
    }
}

AL_API void AL_APIENTRY alGetListener3f(ALenum param, ALfloat *v1, ALfloat *v2, ALfloat *v3) {
    if (!v1 || !v2 || !v3) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    ensure_listener_initialized();

    switch (param) {
        case AL_POSITION:
            *v1 = g_listener_pos[0];
            *v2 = g_listener_pos[1];
            *v3 = g_listener_pos[2];
            break;

        case AL_VELOCITY:
            *v1 = g_listener_vel[0];
            *v2 = g_listener_vel[1];
            *v3 = g_listener_vel[2];
            break;

        default:
            alSetError(AL_INVALID_ENUM);
            break;
    }
}

AL_API void AL_APIENTRY alGetListenerfv(ALenum param, ALfloat *values) {
    if (!values) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    ensure_listener_initialized();

    switch (param) {
        case AL_GAIN:
            values[0] = g_listener_gain;
            break;

        case AL_POSITION:
            values[0] = g_listener_pos[0];
            values[1] = g_listener_pos[1];
            values[2] = g_listener_pos[2];
            break;

        case AL_VELOCITY:
            values[0] = g_listener_vel[0];
            values[1] = g_listener_vel[1];
            values[2] = g_listener_vel[2];
            break;

        case AL_ORIENTATION:
            memcpy(values, g_listener_ori, sizeof(g_listener_ori));
            break;

        default:
            alSetError(AL_INVALID_ENUM);
            break;
    }
}

AL_API void AL_APIENTRY alGetListeneri(ALenum param, ALint *value) {
    if (!value) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    ALfloat fval = 0.0f;
    alGetListenerf(param, &fval);
    *value = (ALint)fval;
}

AL_API void AL_APIENTRY alGetListener3i(ALenum param, ALint *v1, ALint *v2, ALint *v3) {
    if (!v1 || !v2 || !v3) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    ALfloat f1 = 0.0f, f2 = 0.0f, f3 = 0.0f;
    alGetListener3f(param, &f1, &f2, &f3);
    *v1 = (ALint)f1;
    *v2 = (ALint)f2;
    *v3 = (ALint)f3;
}

AL_API void AL_APIENTRY alGetListeneriv(ALenum param, ALint *values) {
    if (!values) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    ensure_listener_initialized();

    switch (param) {
        case AL_GAIN:
            values[0] = (ALint)g_listener_gain;
            break;

        case AL_POSITION:
            values[0] = (ALint)g_listener_pos[0];
            values[1] = (ALint)g_listener_pos[1];
            values[2] = (ALint)g_listener_pos[2];
            break;

        case AL_VELOCITY:
            values[0] = (ALint)g_listener_vel[0];
            values[1] = (ALint)g_listener_vel[1];
            values[2] = (ALint)g_listener_vel[2];
            break;

        case AL_ORIENTATION:
            for (int i = 0; i < 6; i++) {
                values[i] = (ALint)g_listener_ori[i];
            }
            break;

        default:
            alSetError(AL_INVALID_ENUM);
            break;
    }
}
