#include "apu_hrtf.h"
#include <math.h>
#include <string.h>

#define HRTF_RATE        48000.0f
#define HEAD_RADIUS_M    0.0875f
#define SPEED_OF_SOUND   343.0f
#define DEG              0.017453292f
#define ITD_MAX_SAMPLES  42.0f          /* the VP clamps to +/-42 samples */
/* FIR scale: the near-ear shelf rises to +6 dB at high frequencies, so the
 * taps are scaled by 0.5 to stay inside int8 (value / 128) */
#define FIR_SCALE        0.5f

const float apu_hrtf_elevations_deg[APU_HRTF_ELEVATIONS] = { -30.0f, 0.0f, 30.0f, 60.0f };

/* Unit vector for azimuth (0 front, + right) and elevation (+ up): x right, y up, z front */
static void direction(float az_deg, float el_deg, float v[3]) {
    float az = az_deg * DEG, el = el_deg * DEG;
    v[0] = sinf(az) * cosf(el);
    v[1] = sinf(el);
    v[2] = cosf(az) * cosf(el);
}

/*
 * Head-shadow FIR for the ear on side `side` (-1 left, +1 right): the
 * Brown-Duda shelf discretised with the bilinear transform, its impulse
 * response truncated to 31 taps with a half-Hann taper over the tail.
 */
static void shadow_fir(const float dir[3], float side, int8_t taps[APU_HRTF_TAPS]) {
    const float amin = 0.1f, tmin = 150.0f;
    const float w0 = SPEED_OF_SOUND / HEAD_RADIUS_M;
    float cos_t = dir[0] * side;                    /* ear axis is +/- x */
    float t_deg, alpha, k, b0, b1, a1, x_prev = 0.0f, y_prev = 0.0f;
    int n;

    if (cos_t > 1.0f) cos_t = 1.0f;
    if (cos_t < -1.0f) cos_t = -1.0f;
    t_deg = acosf(cos_t) / DEG;
    alpha = (1.0f + amin * 0.5f) + (1.0f - amin * 0.5f) * cosf(t_deg / tmin * 180.0f * DEG);

    /* H(s) = (1 + alpha s / 2w0) / (1 + s / 2w0), s = 2 fs (z - 1) / (z + 1) */
    k = 2.0f * HRTF_RATE / (2.0f * w0);
    b0 = (1.0f + alpha * k) / (1.0f + k);
    b1 = (1.0f - alpha * k) / (1.0f + k);
    a1 = (1.0f - k) / (1.0f + k);

    for (n = 0; n < APU_HRTF_TAPS; n++) {
        float x = (n == 0) ? 1.0f : 0.0f;
        float y = b0 * x + b1 * x_prev - a1 * y_prev;
        float taper = 1.0f, q;
        if (n >= APU_HRTF_TAPS / 2) {
            float u = (float)(n - APU_HRTF_TAPS / 2) / (float)(APU_HRTF_TAPS - APU_HRTF_TAPS / 2);
            taper = 0.5f + 0.5f * cosf(u * 3.14159265f);
        }
        q = y * taper * FIR_SCALE * 128.0f;
        q = (q < 0.0f) ? q - 0.5f : q + 0.5f;
        if (q > 127.0f) q = 127.0f;
        if (q < -128.0f) q = -128.0f;
        taps[n] = (int8_t)q;
        x_prev = x;
        y_prev = y;
    }
}

void apu_hrtf_build_entry(float azimuth_deg, float elevation_deg, apu_hrtf_entry_t *out) {
    float dir[3], lateral, itd;

    direction(azimuth_deg, elevation_deg, dir);
    shadow_fir(dir, -1.0f, out->left);
    shadow_fir(dir, +1.0f, out->right);

    /* Woodworth on the lateral angle; a source on the right reaches the
     * right ear first, so the left ear is delayed (positive) */
    lateral = asinf(dir[0] > 1.0f ? 1.0f : (dir[0] < -1.0f ? -1.0f : dir[0]));
    itd = (HEAD_RADIUS_M / SPEED_OF_SOUND) * (lateral + sinf(lateral)) * HRTF_RATE;
    if (itd > ITD_MAX_SAMPLES) itd = ITD_MAX_SAMPLES;
    if (itd < -ITD_MAX_SAMPLES) itd = -ITD_MAX_SAMPLES;
    out->itd = (int16_t)(itd * 512.0f + (itd < 0.0f ? -0.5f : 0.5f));
}

void apu_hrtf_build_table(apu_hrtf_entry_t table[APU_HRTF_ENTRIES]) {
    int e, a;
    for (e = 0; e < APU_HRTF_ELEVATIONS; e++) {
        for (a = 0; a < APU_HRTF_AZIMUTHS; a++) {
            apu_hrtf_build_entry((float)a * (360.0f / APU_HRTF_AZIMUTHS), apu_hrtf_elevations_deg[e],
                                 &table[e * APU_HRTF_AZIMUTHS + a]);
        }
    }
}

int apu_hrtf_entry_for_local(const float local_pos[3]) {
    /* Local frame: +x right, +y up, +z behind (front is -z) */
    float x = local_pos[0], y = local_pos[1], front = -local_pos[2];
    float horiz = sqrtf(x * x + front * front);
    float az, el, best_d = 1e9f;
    int a, e, best_e = 1;

    if (horiz < 1e-6f && fabsf(y) < 1e-6f) {
        return 1 * APU_HRTF_AZIMUTHS;        /* at the head: straight ahead, level */
    }
    az = atan2f(x, front) / DEG;             /* -180..180, + right */
    el = atan2f(y, horiz) / DEG;
    if (az < 0.0f) {
        az += 360.0f;
    }
    a = (int)(az / (360.0f / APU_HRTF_AZIMUTHS) + 0.5f) % APU_HRTF_AZIMUTHS;
    for (e = 0; e < APU_HRTF_ELEVATIONS; e++) {
        float d = fabsf(el - apu_hrtf_elevations_deg[e]);
        if (d < best_d) {
            best_d = d;
            best_e = e;
        }
    }
    return best_e * APU_HRTF_AZIMUTHS + a;
}
