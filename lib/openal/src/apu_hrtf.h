#ifndef APU_HRTF_H
#define APU_HRTF_H

/*
 * HRTF table for the MCPX Voice Processor (lib/openal/docs/XEMU_VERIFICATION.md
 * section 4.8): 128 entries, each two 31-tap int8 FIRs (coefficient = value /
 * 128) and one interaural time difference in signed s6.9 samples (positive
 * delays the left ear).
 *
 * The table is computed, not measured: a spherical-head model from the
 * published literature, so it carries no third-party data.
 *   - Head shadow per ear: Brown & Duda, "A structural model for binaural
 *     sound synthesis", IEEE Trans. Speech Audio Proc. 6(5), 1998: a
 *     first-order shelf H(s) = (1 + a(t) s / 2w0) / (1 + s / 2w0) with
 *     w0 = c / r and a(t) = (1 + amin/2) + (1 - amin/2) cos(t / tmin * 180 deg),
 *     amin = 0.1, tmin = 150 deg, t = angle between the source and the ear axis.
 *   - ITD: Woodworth, r/c (p + sin p), p = lateral angle.
 * There is no pinna model, so elevation is only conveyed through the ear
 * angle (cone of confusion), not through spectral notches.
 *
 * Entries: 32 azimuths (11.25 deg steps, 0 = front, positive = right) x
 * 4 elevations (-30, 0, 30, 60 deg); entry = elevation_index * 32 + azimuth_index.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APU_HRTF_ENTRIES     128
#define APU_HRTF_TAPS        31
#define APU_HRTF_AZIMUTHS    32
#define APU_HRTF_ELEVATIONS  4

typedef struct {
    int8_t left[APU_HRTF_TAPS];
    int8_t right[APU_HRTF_TAPS];
    int16_t itd;            /* s6.9 samples at 48 kHz; > 0 delays the left ear */
} apu_hrtf_entry_t;

/** Elevation of each elevation index, degrees. */
extern const float apu_hrtf_elevations_deg[APU_HRTF_ELEVATIONS];

/** Fill the 128-entry table from the spherical-head model. */
void apu_hrtf_build_table(apu_hrtf_entry_t table[APU_HRTF_ENTRIES]);

/** Build one entry for a direction (azimuth: 0 front, + right; elevation: + up; degrees). */
void apu_hrtf_build_entry(float azimuth_deg, float elevation_deg, apu_hrtf_entry_t *out);

/**
 * Nearest table entry for a source position in the listener frame of
 * al_listener_world_to_local(): +x right, +y up, +z BEHIND the listener.
 */
int apu_hrtf_entry_for_local(const float local_pos[3]);

#ifdef __cplusplus
}
#endif

#endif /* APU_HRTF_H */
