#include "apu_spatial.h"
#include "apu_mem.h"
#include <string.h>
#include <math.h>

/*
 * ============================================================================
 * Internal State of Spatial and ITD Subsystem
 * ============================================================================
 */
static ALenum s_distance_model = AL_INVERSE_DISTANCE_CLAMPED;
static float s_doppler_factor = 1.0f;
static float s_speed_of_sound = 343.3f;

/* Hardware ITD Circular Buffer State (16 KB pool for 64 voices) */
static void *s_itd_virt = NULL;
static uint32_t s_itd_phys = 0;
static bool s_itd_initialized = false;

/*
 * ============================================================================
 * Global Distance Model & Doppler Configuration
 * ============================================================================
 */

void apu_spatial_set_distance_model(ALenum model) {
    s_distance_model = model;
}

ALenum apu_spatial_get_distance_model(void) {
    return s_distance_model;
}

void apu_spatial_set_doppler_factor(float factor) {
    s_doppler_factor = factor;
}

float apu_spatial_get_doppler_factor(void) {
    return s_doppler_factor;
}

void apu_spatial_set_speed_of_sound(float speed) {
    s_speed_of_sound = speed;
}

float apu_spatial_get_speed_of_sound(void) {
    return s_speed_of_sound;
}

void apu_spatial_reset(void) {
    s_distance_model = AL_INVERSE_DISTANCE_CLAMPED;
    s_doppler_factor = 1.0f;
    s_speed_of_sound = 343.3f;
}

/*
 * ============================================================================
 * Distance Attenuation Models Math (OpenAL 1.1 Specification Section 3.4)
 * ============================================================================
 */

float apu_calc_distance_gain(float distance, float ref_dist, float max_dist, float rolloff, ALenum model) {
    if (distance < 0.0f) {
        distance = 0.0f;
    }
    if (ref_dist <= 0.0f || rolloff == 0.0f) {
        return 1.0f;
    }
    if (max_dist < ref_dist) {
        max_dist = ref_dist;
    }

    switch (model) {
        case AL_NONE:
            return 1.0f;

        case AL_INVERSE_DISTANCE: {
            float denom = ref_dist + rolloff * (distance - ref_dist);
            if (denom <= 0.0f) {
                return 1.0f;
            }
            return ref_dist / denom;
        }

        case AL_INVERSE_DISTANCE_CLAMPED: {
            float d_clamp = distance;
            if (d_clamp < ref_dist) d_clamp = ref_dist;
            if (d_clamp > max_dist) d_clamp = max_dist;
            float denom = ref_dist + rolloff * (d_clamp - ref_dist);
            if (denom <= 0.0f) {
                return 1.0f;
            }
            return ref_dist / denom;
        }

        case AL_LINEAR_DISTANCE: {
            if (max_dist <= ref_dist) {
                return 1.0f;
            }
            float d = distance;
            if (d > max_dist) d = max_dist;
            float gain = 1.0f - rolloff * ((d - ref_dist) / (max_dist - ref_dist));
            if (gain < 0.0f) gain = 0.0f;
            return gain;
        }

        case AL_LINEAR_DISTANCE_CLAMPED: {
            if (max_dist <= ref_dist) {
                return 1.0f;
            }
            float d_clamp = distance;
            if (d_clamp < ref_dist) d_clamp = ref_dist;
            if (d_clamp > max_dist) d_clamp = max_dist;
            float gain = 1.0f - rolloff * ((d_clamp - ref_dist) / (max_dist - ref_dist));
            if (gain < 0.0f) gain = 0.0f;
            return gain;
        }

        case AL_EXPONENT_DISTANCE: {
            if (distance <= 0.0f) {
                return 1.0f;
            }
            return powf(distance / ref_dist, -rolloff);
        }

        case AL_EXPONENT_DISTANCE_CLAMPED: {
            float d_clamp = distance;
            if (d_clamp < ref_dist) d_clamp = ref_dist;
            if (d_clamp > max_dist) d_clamp = max_dist;
            if (d_clamp <= 0.0f) {
                return 1.0f;
            }
            return powf(d_clamp / ref_dist, -rolloff);
        }

        default:
            return 1.0f;
    }
}

/*
 * ============================================================================
 * Directional Cone Attenuation Math
 * ============================================================================
 */

float apu_calc_cone_gain(const float source_pos[3], const float source_dir[3], const float listener_pos[3],
                         float inner_deg, float outer_deg, float outer_gain) {
    if (!source_pos || !source_dir || !listener_pos) {
        return 1.0f;
    }

    /* Omnidirectional if direction is zero or angles are full sphere */
    if (source_dir[0] == 0.0f && source_dir[1] == 0.0f && source_dir[2] == 0.0f) {
        return 1.0f;
    }
    if (inner_deg >= 360.0f && outer_deg >= 360.0f) {
        return 1.0f;
    }

    float lx = listener_pos[0] - source_pos[0];
    float ly = listener_pos[1] - source_pos[1];
    float lz = listener_pos[2] - source_pos[2];
    float dist = sqrtf(lx * lx + ly * ly + lz * lz);
    if (dist == 0.0f) {
        return 1.0f;
    }

    float dir_len = sqrtf(source_dir[0] * source_dir[0] +
                          source_dir[1] * source_dir[1] +
                          source_dir[2] * source_dir[2]);
    if (dir_len == 0.0f) {
        return 1.0f;
    }

    float dot = source_dir[0] * lx + source_dir[1] * ly + source_dir[2] * lz;
    float cos_theta = dot / (dir_len * dist);
    if (cos_theta > 1.0f) cos_theta = 1.0f;
    if (cos_theta < -1.0f) cos_theta = -1.0f;

    float theta_deg = acosf(cos_theta) * (180.0f / 3.14159265358979323846f);

    if (theta_deg <= inner_deg) {
        return 1.0f;
    }
    if (theta_deg >= outer_deg) {
        return outer_gain;
    }
    if (outer_deg <= inner_deg) {
        return outer_gain;
    }

    float factor = (theta_deg - inner_deg) / (outer_deg - inner_deg);
    return 1.0f - (1.0f - outer_gain) * factor;
}

/*
 * ============================================================================
 * Doppler Shift Pitch Multiplier Calculation
 * ============================================================================
 */

float apu_calc_doppler_pitch(const float source_pos[3], const float source_vel[3],
                            const float listener_pos[3], const float listener_vel[3],
                            float doppler_factor, float speed_of_sound) {
    if (doppler_factor == 0.0f || speed_of_sound <= 0.0f) {
        return 1.0f;
    }
    if (!source_pos || !source_vel || !listener_pos || !listener_vel) {
        return 1.0f;
    }

    /* Vector from source to listener (line of sight) */
    float slx = listener_pos[0] - source_pos[0];
    float sly = listener_pos[1] - source_pos[1];
    float slz = listener_pos[2] - source_pos[2];
    float dist = sqrtf(slx * slx + sly * sly + slz * slz);
    if (dist == 0.0f) {
        return 1.0f;
    }

    /* Normalized direction from source to listener */
    float ux = slx / dist;
    float uy = sly / dist;
    float uz = slz / dist;

    /* Scalar velocity projections along line of sight */
    float vls = listener_vel[0] * ux + listener_vel[1] * uy + listener_vel[2] * uz;
    float vss = source_vel[0] * ux + source_vel[1] * uy + source_vel[2] * uz;

    /* Clamping per OpenAL 1.1 Specification Section 3.5.2 */
    float max_v = speed_of_sound / doppler_factor;
    if (vls > max_v) vls = max_v;
    if (vss >= max_v * 0.999f) vss = max_v * 0.999f;

    float num = speed_of_sound - doppler_factor * vls;
    float denom = speed_of_sound - doppler_factor * vss;
    if (denom <= 0.0f) {
        return 1.0f;
    }

    float factor = num / denom;
    if (factor < 0.0f) factor = 0.0f;
    return factor;
}

/*
 * ============================================================================
 * Woodworth Interaural Time Difference (ITD) Engine
 * ============================================================================
 */

uint16_t apu_calc_itd_delay_samples(float x_rel) {
    float abs_x = fabsf(x_rel);
    if (isnan(abs_x)) {
        abs_x = 0.0f;
    }
    if (abs_x > 1.0f) {
        abs_x = 1.0f;
    }

    /*
     * Woodworth delay model at 48 kHz:
     * Delay = round(31.0 * (0.55 * |x_rel| + 0.45 * |x_rel|^3))
     */
    float delay_f = 31.0f * (0.55f * abs_x + 0.45f * abs_x * abs_x * abs_x);
    int delay = (int)roundf(delay_f);

    /* Clamp to hardware delay engine range [0, 64] */
    if (delay < 0) {
        delay = 0;
    } else if (delay > 64) {
        delay = 64;
    }

    return (uint16_t)delay;
}

void apu_calc_itd_taps(float x_rel, uint16_t *delay_left, uint16_t *delay_right) {
    if (!delay_left || !delay_right) {
        return;
    }

    if (isnan(x_rel) || fabsf(x_rel) < 1e-6f) {
        *delay_left = 0;
        *delay_right = 0;
        return;
    }

    uint16_t delay = apu_calc_itd_delay_samples(x_rel);

    if (x_rel > 0.0f) {
        /*
         * Sound source to the right:
         * Direct wave reaches right ear first.
         * Left ear path is longer -> Left ear tap delayed.
         */
        *delay_left = delay;
        *delay_right = 0;
    } else if (x_rel < 0.0f) {
        /*
         * Sound source to the left:
         * Direct wave reaches left ear first.
         * Right ear path is longer -> Right ear tap delayed.
         */
        *delay_left = 0;
        *delay_right = delay;
    } else {
        *delay_left = 0;
        *delay_right = 0;
    }
}

/*
 * ============================================================================
 * HRTF Elevation & Pinna Spectral Filter Engine (Q14 Biquad)
 * ============================================================================
 */

float apu_calc_elevation_cutoff(float y_rel, float z_rel) {
    if (isnan(y_rel)) {
        y_rel = 0.0f;
    }
    if (isnan(z_rel)) {
        z_rel = 0.0f;
    }

    if (y_rel < -1.0f) {
        y_rel = -1.0f;
    } else if (y_rel > 1.0f) {
        y_rel = 1.0f;
    }

    if (z_rel < -1.0f) {
        z_rel = -1.0f;
    } else if (z_rel > 1.0f) {
        z_rel = 1.0f;
    }

    float f_base = 14000.0f + 4000.0f * y_rel;
    float fc;
    if (z_rel > 0.0f) {
        fc = f_base - 4000.0f * z_rel;
    } else {
        fc = f_base;
    }

    if (fc < 1000.0f) {
        fc = 1000.0f;
    } else if (fc > 20000.0f) {
        fc = 20000.0f;
    }

    return fc;
}

static int16_t float_to_q14(float val) {
    float scaled = roundf(val * 16384.0f);
    if (scaled > 32767.0f) {
        return 32767;
    }
    if (scaled < -32768.0f) {
        return -32768;
    }
    return (int16_t)scaled;
}

void apu_calc_butterworth_lowpass_q14(float cutoff_hz, float sample_rate, APU_BIQUAD_COEFFS_Q14 *out_coeffs) {
    if (!out_coeffs) {
        return;
    }

    if (isnan(cutoff_hz) || cutoff_hz <= 0.0f) {
        apu_get_biquad_passthrough_q14(out_coeffs);
        return;
    }

    if (sample_rate <= 0.0f) {
        sample_rate = 48000.0f;
    }

    /* Guard cutoff against Nyquist frequency limit */
    float nyquist = sample_rate * 0.5f;
    if (cutoff_hz >= nyquist) {
        cutoff_hz = nyquist * 0.999f;
    }

    float w0 = 2.0f * 3.14159265358979323846f * (cutoff_hz / sample_rate);
    float alpha = sinf(w0) / 1.4142135623730951f; /* sin(w0) / sqrt(2) */
    float a0 = 1.0f + alpha;

    float cos_w0 = cosf(w0);
    float b0 = (1.0f - cos_w0) / (2.0f * a0);
    float b1 = (1.0f - cos_w0) / a0;
    float b2 = b0;
    float a1 = (-2.0f * cos_w0) / a0;
    float a2 = (1.0f - alpha) / a0;

    out_coeffs->b0 = float_to_q14(b0);
    out_coeffs->b1 = float_to_q14(b1);
    out_coeffs->b2 = float_to_q14(b2);
    out_coeffs->a1 = float_to_q14(a1);
    out_coeffs->a2 = float_to_q14(a2);
}

void apu_get_biquad_passthrough_q14(APU_BIQUAD_COEFFS_Q14 *out_coeffs) {
    if (!out_coeffs) {
        return;
    }
    out_coeffs->b0 = 16384;
    out_coeffs->b1 = 0;
    out_coeffs->b2 = 0;
    out_coeffs->a1 = 0;
    out_coeffs->a2 = 0;
}

/*
 * ============================================================================
 * ITU-R BS.775 5.1 Equal-Power Panning Engine
 * ============================================================================
 * Speaker Azimuth Angles:
 *   - Center (C):          0.0 rad (0 deg)
 *   - Front Right (FR):    pi/6 rad (30 deg)
 *   - Surround Right (SR): 11*pi/18 rad (110 deg)
 *   - Surround Left (SL):  25*pi/18 rad (250 deg)
 *   - Front Left (FL):     11*pi/6 rad (330 deg)
 * ============================================================================
 */

#ifndef APU_PI
#define APU_PI       3.14159265358979323846f
#endif
#define APU_TWO_PI   (2.0f * APU_PI)
#define APU_HALF_PI  (0.5f * APU_PI)

#define APU_AZIMUTH_C   (0.0f)
#define APU_AZIMUTH_FR  (APU_PI / 6.0f)
#define APU_AZIMUTH_SR  (11.0f * APU_PI / 18.0f)
#define APU_AZIMUTH_SL  (25.0f * APU_PI / 18.0f)
#define APU_AZIMUTH_FL  (11.0f * APU_PI / 6.0f)

static uint8_t float_to_gain8(float g) {
    if (isnan(g) || g <= 0.0f) {
        return 0;
    }
    float scaled = roundf(g * 255.0f);
    if (scaled >= 255.0f) {
        return 255;
    }
    return (uint8_t)scaled;
}

void apu_calc_panning_51(float x_local, float z_local, float lfe_factor, APU_PAN_GAINS_51 *out_gains) {
    if (!out_gains) {
        return;
    }

    /* Clamp LFE factor to [0.0f, 1.0f] */
    float lfe = lfe_factor;
    if (isnan(lfe) || lfe < 0.0f) {
        lfe = 0.0f;
    } else if (lfe > 1.0f) {
        lfe = 1.0f;
    }
    out_gains->lfe = lfe;

    if (isnan(x_local) || isnan(z_local)) {
        out_gains->fl = 0.5f;
        out_gains->fr = 0.5f;
        out_gains->sl = 0.5f;
        out_gains->sr = 0.5f;
        out_gains->c  = 0.0f;
        return;
    }

    float r = sqrtf(x_local * x_local + z_local * z_local);
    if (r < 0.00001f) {
        /* Evenly distributed across FL, FR, SL, SR (0.5f each), C = 0.0f */
        out_gains->fl = 0.5f;
        out_gains->fr = 0.5f;
        out_gains->sl = 0.5f;
        out_gains->sr = 0.5f;
        out_gains->c  = 0.0f;
        return;
    }

    /* Azimuth angle theta = atan2f(x_local, -z_local). Wrap negative to [0, 2*pi) */
    float theta = atan2f(x_local, -z_local);
    if (theta < 0.0f) {
        theta += APU_TWO_PI;
    }
    while (theta >= APU_TWO_PI) {
        theta -= APU_TWO_PI;
    }
    if (theta < 0.0f) {
        theta = 0.0f;
    }

    out_gains->fl = 0.0f;
    out_gains->fr = 0.0f;
    out_gains->sl = 0.0f;
    out_gains->sr = 0.0f;
    out_gains->c  = 0.0f;

    /* Pairwise equal-power panning */
    if (theta >= APU_AZIMUTH_C && theta < APU_AZIMUTH_FR) {
        /* Sector 0: [0, FR] (C to FR) */
        float phi = (theta - APU_AZIMUTH_C) / (APU_AZIMUTH_FR - APU_AZIMUTH_C);
        if (phi < 0.0f) phi = 0.0f; else if (phi > 1.0f) phi = 1.0f;
        out_gains->c  = cosf(phi * APU_HALF_PI);
        out_gains->fr = sinf(phi * APU_HALF_PI);
    } else if (theta >= APU_AZIMUTH_FR && theta < APU_AZIMUTH_SR) {
        /* Sector 1: [FR, SR] (FR to SR) */
        float phi = (theta - APU_AZIMUTH_FR) / (APU_AZIMUTH_SR - APU_AZIMUTH_FR);
        if (phi < 0.0f) phi = 0.0f; else if (phi > 1.0f) phi = 1.0f;
        out_gains->fr = cosf(phi * APU_HALF_PI);
        out_gains->sr = sinf(phi * APU_HALF_PI);
    } else if (theta >= APU_AZIMUTH_SR && theta < APU_AZIMUTH_SL) {
        /* Sector 2: [SR, SL] (SR to SL, Rear) */
        float phi = (theta - APU_AZIMUTH_SR) / (APU_AZIMUTH_SL - APU_AZIMUTH_SR);
        if (phi < 0.0f) phi = 0.0f; else if (phi > 1.0f) phi = 1.0f;
        out_gains->sr = cosf(phi * APU_HALF_PI);
        out_gains->sl = sinf(phi * APU_HALF_PI);
    } else if (theta >= APU_AZIMUTH_SL && theta < APU_AZIMUTH_FL) {
        /* Sector 3: [SL, FL] (SL to FL) */
        float phi = (theta - APU_AZIMUTH_SL) / (APU_AZIMUTH_FL - APU_AZIMUTH_SL);
        if (phi < 0.0f) phi = 0.0f; else if (phi > 1.0f) phi = 1.0f;
        out_gains->sl = cosf(phi * APU_HALF_PI);
        out_gains->fl = sinf(phi * APU_HALF_PI);
    } else {
        /* Sector 4: [FL, 2*pi) (FL to C) */
        float phi = (theta - APU_AZIMUTH_FL) / (APU_TWO_PI - APU_AZIMUTH_FL);
        if (phi < 0.0f) phi = 0.0f; else if (phi > 1.0f) phi = 1.0f;
        out_gains->fl = cosf(phi * APU_HALF_PI);
        out_gains->c  = sinf(phi * APU_HALF_PI);
    }
}

void apu_calc_mixbin_gains_51(const APU_PAN_GAINS_51 *pan_gains, uint8_t out_mixbins[16]) {
    if (!pan_gains || !out_mixbins) {
        return;
    }
    memset(out_mixbins, 0, 16);
    out_mixbins[0] = float_to_gain8(pan_gains->fl);
    out_mixbins[1] = float_to_gain8(pan_gains->fr);
    out_mixbins[2] = float_to_gain8(pan_gains->sl);
    out_mixbins[3] = float_to_gain8(pan_gains->sr);
    out_mixbins[4] = float_to_gain8(pan_gains->c);
    out_mixbins[5] = float_to_gain8(pan_gains->lfe);
}

/*
 * ============================================================================
 * ITD Circular Buffer Subsystem API
 * ============================================================================
 */


int apu_itd_subsystem_init(void) {
    if (s_itd_initialized && s_itd_virt != NULL) {
        return 0;
    }

    s_itd_virt = apu_mem_alloc_phys(NV_PAPU_ITD_POOL_SIZE, NV_PAPU_ITD_BUFFER_ALIGN, &s_itd_phys);
    if (!s_itd_virt) {
        s_itd_phys = 0;
        s_itd_initialized = false;
        return -1;
    }

    memset(s_itd_virt, 0, NV_PAPU_ITD_POOL_SIZE);
    s_itd_initialized = true;
    return 0;
}

void apu_itd_subsystem_deinit(void) {
    if (s_itd_virt != NULL) {
        apu_mem_free_phys(s_itd_virt);
        s_itd_virt = NULL;
        s_itd_phys = 0;
    }
    s_itd_initialized = false;
}

uint32_t apu_itd_get_voice_buffer_phys(uint32_t voice_index) {
    if (voice_index >= NV_PAPU_NUM_3D_VOICES || !s_itd_initialized || s_itd_phys == 0) {
        return 0;
    }
    return s_itd_phys + (voice_index * NV_PAPU_ITD_BUFFER_SIZE_PER_VOICE);
}

void *apu_itd_get_voice_buffer_virt(uint32_t voice_index) {
    if (voice_index >= NV_PAPU_NUM_3D_VOICES || !s_itd_initialized || s_itd_virt == NULL) {
        return NULL;
    }
    return (void *)((uintptr_t)s_itd_virt + (voice_index * NV_PAPU_ITD_BUFFER_SIZE_PER_VOICE));
}
