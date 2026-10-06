#include "al_listener.h"
#include "al_source.h"
#include "al_buffer.h"
#include <string.h>
#include <math.h>

/*
 * ============================================================================
 * Global Listener State
 * ============================================================================
 */
static float g_listener_gain = 1.0f;
static float g_listener_pos[3] = {0.0f, 0.0f, 0.0f};
static float g_listener_vel[3] = {0.0f, 0.0f, 0.0f};
static float g_listener_forward[3] = {0.0f, 0.0f, -1.0f};
static float g_listener_up[3] = {0.0f, 1.0f, 0.0f};
static float g_listener_right[3] = {1.0f, 0.0f, 0.0f};
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
    g_listener_forward[0] = 0.0f;
    g_listener_forward[1] = 0.0f;
    g_listener_forward[2] = -1.0f;
    g_listener_up[0] = 0.0f;
    g_listener_up[1] = 1.0f;
    g_listener_up[2] = 0.0f;
    g_listener_right[0] = 1.0f;
    g_listener_right[1] = 0.0f;
    g_listener_right[2] = 0.0f;
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

void al_listener_get_position(float pos[3]) {
    ensure_listener_initialized();
    if (pos) {
        pos[0] = g_listener_pos[0];
        pos[1] = g_listener_pos[1];
        pos[2] = g_listener_pos[2];
    }
}

void al_listener_get_velocity(float vel[3]) {
    ensure_listener_initialized();
    if (vel) {
        vel[0] = g_listener_vel[0];
        vel[1] = g_listener_vel[1];
        vel[2] = g_listener_vel[2];
    }
}

void al_listener_get_basis(float forward[3], float up[3], float right[3]) {
    ensure_listener_initialized();
    if (forward) {
        forward[0] = g_listener_forward[0];
        forward[1] = g_listener_forward[1];
        forward[2] = g_listener_forward[2];
    }
    if (up) {
        up[0] = g_listener_up[0];
        up[1] = g_listener_up[1];
        up[2] = g_listener_up[2];
    }
    if (right) {
        right[0] = g_listener_right[0];
        right[1] = g_listener_right[1];
        right[2] = g_listener_right[2];
    }
}

void al_listener_world_to_local(const float world_pos[3], float local_pos[3]) {
    if (!world_pos || !local_pos) {
        return;
    }
    ensure_listener_initialized();

    /* Relative vector d = P_world - P_listener */
    float dx = world_pos[0] - g_listener_pos[0];
    float dy = world_pos[1] - g_listener_pos[1];
    float dz = world_pos[2] - g_listener_pos[2];

    /* Project onto orthonormal listener basis:
     * x_local = d . R (Right lateral axis: +Right, -Left)
     * y_local = d . U' (Up elevation axis: +Up, -Down)
     * z_local = -d . F (Front depth axis: -Front, +Behind)
     */
    float lx = dx * g_listener_right[0]   + dy * g_listener_right[1]   + dz * g_listener_right[2];
    float ly = dx * g_listener_up[0]      + dy * g_listener_up[1]      + dz * g_listener_up[2];
    float lz = -(dx * g_listener_forward[0] + dy * g_listener_forward[1] + dz * g_listener_forward[2]);

    local_pos[0] = lx;
    local_pos[1] = ly;
    local_pos[2] = lz;
}

/**
 * Validates, normalizes, and orthonormalizes 6-float orientation array:
 * [fx, fy, fz, ux, uy, uz].
 *
 * Algorithm:
 * 1. Normalize forward F = F / ||F||
 * 2. Cross product Right R = normalize(F x U)
 * 3. True orthogonal Up' = R x F
 */
static bool listener_set_orientation(const ALfloat *values) {
    if (!values) {
        return false;
    }

    for (int i = 0; i < 6; i++) {
        if (!isfinite(values[i])) {
            return false;
        }
    }

    float fx = values[0];
    float fy = values[1];
    float fz = values[2];
    float ux = values[3];
    float uy = values[4];
    float uz = values[5];

    /* 1. Normalize Forward: F = F / ||F|| */
    float f_sq = fx * fx + fy * fy + fz * fz;
    if (f_sq < 1e-12f) {
        return false;
    }
    float f_len = sqrtf(f_sq);
    fx /= f_len;
    fy /= f_len;
    fz /= f_len;

    /* 2. Compute Right axis: R = normalize(F x U) */
    float rx = fy * uz - fz * uy;
    float ry = fz * ux - fx * uz;
    float rz = fx * uy - fy * ux;

    float r_sq = rx * rx + ry * ry + rz * rz;
    if (r_sq < 1e-12f) {
        /* F and U are collinear or U is zero length */
        return false;
    }
    float r_len = sqrtf(r_sq);
    rx /= r_len;
    ry /= r_len;
    rz /= r_len;

    /* 3. Compute true orthogonal Up axis: U' = R x F */
    float up_x = ry * fz - rz * fy;
    float up_y = rz * fx - rx * fz;
    float up_z = rx * fy - ry * fx;

    float up_sq = up_x * up_x + up_y * up_y + up_z * up_z;
    if (up_sq < 1e-12f) {
        return false;
    }
    float up_len = sqrtf(up_sq);
    up_x /= up_len;
    up_y /= up_len;
    up_z /= up_len;

    /* Update internal listener orientation basis */
    g_listener_forward[0] = fx;
    g_listener_forward[1] = fy;
    g_listener_forward[2] = fz;

    g_listener_up[0] = up_x;
    g_listener_up[1] = up_y;
    g_listener_up[2] = up_z;

    g_listener_right[0] = rx;
    g_listener_right[1] = ry;
    g_listener_right[2] = rz;

    return true;
}

/*
 * ============================================================================
 * OpenAL 1.1 Listener Configuration APIs
 * ============================================================================
 */

AL_API void AL_APIENTRY alListenerf(ALenum param, ALfloat value) {
    ensure_listener_initialized();

    if (!isfinite(value)) {
        alSetError(AL_INVALID_VALUE);
        return;
    }

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

    if (!isfinite(v1) || !isfinite(v2) || !isfinite(v3)) {
        alSetError(AL_INVALID_VALUE);
        return;
    }

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
            if (!listener_set_orientation(values)) {
                alSetError(AL_INVALID_VALUE);
            }
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
            values[0] = g_listener_forward[0];
            values[1] = g_listener_forward[1];
            values[2] = g_listener_forward[2];
            values[3] = g_listener_up[0];
            values[4] = g_listener_up[1];
            values[5] = g_listener_up[2];
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
            values[0] = (ALint)g_listener_forward[0];
            values[1] = (ALint)g_listener_forward[1];
            values[2] = (ALint)g_listener_forward[2];
            values[3] = (ALint)g_listener_up[0];
            values[4] = (ALint)g_listener_up[1];
            values[5] = (ALint)g_listener_up[2];
            break;

        default:
            alSetError(AL_INVALID_ENUM);
            break;
    }
}
