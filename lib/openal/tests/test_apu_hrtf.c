#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>

#include "apu_hrtf.h"

/*
 * Spherical-head HRTF table (apu_hrtf.c): ITD sign and size, head shadow on
 * the far ear, left/right mirror symmetry and the direction -> entry lookup.
 */

static int tap_sum(const int8_t *t) {
    int i, s = 0;
    for (i = 0; i < APU_HRTF_TAPS; i++) s += t[i];
    return s;
}

/* High-frequency content: sum of |first differences| (a sharp onset = bright) */
static int brightness(const int8_t *t) {
    int i, s = abs(t[0]);
    for (i = 1; i < APU_HRTF_TAPS; i++) s += abs(t[i] - t[i - 1]);
    return s;
}

static int entry(int el_index, int az_index) {
    return el_index * APU_HRTF_AZIMUTHS + az_index;
}

int main(void) {
    static apu_hrtf_entry_t table[APU_HRTF_ENTRIES];
    const apu_hrtf_entry_t *front, *right, *left, *back;
    int a, e, i;

    printf("=== Spherical-Head HRTF Table Test ===\n");
    apu_hrtf_build_table(table);
    front = &table[entry(1, 0)];
    right = &table[entry(1, 8)];    /* 90 deg */
    back = &table[entry(1, 16)];    /* 180 deg */
    left = &table[entry(1, 24)];    /* 270 deg */

    printf("[1] Interaural time difference...\n");
    /* Woodworth: r/c (pi/2 + 1) * 48 kHz = 31.4 samples, s6.9 */
    assert(front->itd == 0 && abs(back->itd) <= 1);
    assert(right->itd > 30 * 512 && right->itd < 33 * 512);    /* source right: left ear late */
    assert(left->itd == -right->itd);
    for (e = 0; e < APU_HRTF_ELEVATIONS; e++) {
        for (a = 0; a < APU_HRTF_AZIMUTHS; a++) {
            assert(abs(table[entry(e, a)].itd) <= 42 * 512);   /* VP clamp */
        }
    }
    /* Higher elevation, smaller lateral angle, smaller ITD */
    assert(table[entry(3, 8)].itd < table[entry(1, 8)].itd && table[entry(3, 8)].itd > 0);
    printf("    -> +/-%.1f samples at the sides, 0 in front, shrinks with elevation [PASS]\n",
           right->itd / 512.0);

    printf("[2] Head shadow...\n");
    /* Source on the right: the far (left) ear loses high frequencies, the near ear gains */
    assert(brightness(right->right) > 2 * brightness(right->left));
    assert(brightness(left->left) > 2 * brightness(left->right));
    /* The shelf leaves low frequencies alone: both ears' DC gain stays close
     * (0.5 * 128 = 64 before the 31-tap truncation) */
    for (i = 0; i < APU_HRTF_ENTRIES; i++) {
        int sl = tap_sum(table[i].left), sr = tap_sum(table[i].right);
        assert(sl > 40 && sl <= 70 && sr > 40 && sr <= 70);
    }
    printf("    -> far ear darker, near ear brighter, DC gain kept [PASS]\n");

    printf("[3] Symmetry...\n");
    for (e = 0; e < APU_HRTF_ELEVATIONS; e++) {
        for (a = 1; a < APU_HRTF_AZIMUTHS; a++) {
            const apu_hrtf_entry_t *p = &table[entry(e, a)];
            const apu_hrtf_entry_t *m = &table[entry(e, APU_HRTF_AZIMUTHS - a)];
            assert(memcmp(p->left, m->right, APU_HRTF_TAPS) == 0);
            assert(memcmp(p->right, m->left, APU_HRTF_TAPS) == 0);
            assert(p->itd == -m->itd);
        }
        assert(memcmp(table[entry(e, 0)].left, table[entry(e, 0)].right, APU_HRTF_TAPS) == 0);
    }
    printf("    -> mirrored azimuths swap ears, straight ahead is identical [PASS]\n");

    printf("[4] Direction lookup (listener frame: +x right, +y up, +z behind)...\n");
    {
        float p_front[3] = { 0.0f, 0.0f, -2.0f }, p_right[3] = { 3.0f, 0.0f, 0.0f };
        float p_back[3] = { 0.0f, 0.0f, 5.0f }, p_left[3] = { -1.0f, 0.0f, 0.0f };
        float p_up[3] = { 0.0f, 4.0f, -1.0f }, p_down[3] = { 1.0f, -1.0f, 0.0f };
        float p_zero[3] = { 0.0f, 0.0f, 0.0f };
        assert(apu_hrtf_entry_for_local(p_front) == entry(1, 0));
        assert(apu_hrtf_entry_for_local(p_right) == entry(1, 8));
        assert(apu_hrtf_entry_for_local(p_back) == entry(1, 16));
        assert(apu_hrtf_entry_for_local(p_left) == entry(1, 24));
        assert(apu_hrtf_entry_for_local(p_up) == entry(3, 0));        /* 76 deg -> 60 */
        assert(apu_hrtf_entry_for_local(p_down) == entry(0, 8));      /* -45 deg -> -30 */
        assert(apu_hrtf_entry_for_local(p_zero) == entry(1, 0));
    }
    printf("    -> front/right/back/left/up/down map to the nearest entry [PASS]\n");

    printf("=== All HRTF Table Tests Passed Successfully! ===\n");
    return 0;
}
