#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "apu_ac97.h"
#include "apu_gp.h"
#include "apu_vp.h"
#include "apu_mem.h"
#include "mcpx_apu_regs.h"

/*
 * Zero-copy output (apu_ac97.c, apu_gp.c): apu_vp_init() gives the GP the
 * small ring the codec reads directly, and the arithmetic that phases the
 * codec's buffer list against the GP's write position and keeps the distance.
 * The buffer-list queue itself only runs on a console (samples/openal_engine).
 */

#define BAR0_SIZE 0x80000u
#define RING      APU_GP_FIFO_BYTES_ZC
#define PIECE     APU_AC97_ZC_PIECE_BYTES

static uint8_t *s_bar0;
static uint32_t reg(uint32_t off) { return *(volatile uint32_t *)(s_bar0 + off); }

int main(void) {
    uintptr_t base;
    uint32_t w, p, lead, pieces = RING / PIECE;

    printf("=== AC97 zero-copy output Test ===\n");
    s_bar0 = (uint8_t *)calloc(1, BAR0_SIZE);
    assert(s_bar0);
    base = (uintptr_t)s_bar0;
    assert(apu_mem_init(0) == 0);

    printf("[1] apu_vp_init() sets up the 8 KiB ring...\n");
    assert(apu_vp_init(base) == 0);
    assert(apu_gp_fifo_bytes() == RING);
    assert(reg(MCPX_APU_GPFMAXSGE) == RING / 4096u - 1u);
    assert(reg(MCPX_APU_GPOFBASE0) == 0 && reg(MCPX_APU_GPOFEND0) == RING);
    assert(apu_gp_fifo_phys() != 0 && apu_gp_fifo_phys() % 4096u == 0);
    assert(apu_gp_fifo_write_pos(base) == 0);
    assert(pieces == 16u && pieces <= 256u);   /* s_zc_piece_of[] holds a byte */
    apu_vp_deinit(base);
    assert(apu_gp_fifo_bytes() == 0 && apu_gp_fifo_phys() == 0);
    printf("    -> GPOFEND0 %u, %u pieces of %u bytes [PASS]\n", RING, pieces, PIECE);

    printf("[2] APU_VP_DBG_AC97_COPY keeps the 64 KiB ring...\n");
    apu_vp_debug_set_flags(APU_VP_DBG_AC97_COPY);
    assert(apu_vp_init(base) == 0);
    assert(apu_gp_fifo_bytes() == APU_GP_FIFO_BYTES && reg(MCPX_APU_GPOFEND0) == APU_GP_FIFO_BYTES);
    apu_vp_deinit(base);
    apu_vp_debug_set_flags(0);
    printf("    -> [PASS]\n");

    printf("[3] Ring size bounds...\n");
    apu_gp_set_fifo_bytes(100u);
    assert(apu_gp_init(base) == 0 && apu_gp_fifo_bytes() == 4096u);
    apu_gp_deinit(base);
    apu_gp_set_fifo_bytes(1u << 20);
    assert(apu_gp_init(base) == 0 && apu_gp_fifo_bytes() == APU_GP_FIFO_BYTES);
    apu_gp_deinit(base);
    apu_gp_set_fifo_bytes(RING + 1000u);   /* whole pages */
    assert(apu_gp_init(base) == 0 && apu_gp_fifo_bytes() == RING);
    apu_gp_deinit(base);
    printf("    -> 4 KiB .. 64 KiB in whole pages [PASS]\n");

    printf("[4] The codec starts half a ring behind the GP...\n");
    for (w = 0; w < RING; w += 4u) {
        p = apu_ac97_zc_first_piece(w, RING);
        assert(p < pieces);
        lead = apu_ac97_zc_lead(w, p * PIECE, RING);
        assert(lead >= RING / 2u && lead < RING / 2u + PIECE);   /* half a ring, rounded to a piece */
        assert(apu_ac97_zc_slip(lead, RING) == 0);               /* and no slip at the start */
    }
    assert(apu_ac97_zc_first_piece(RING / 2u, RING) == 0);
    assert(apu_ac97_zc_first_piece(0, RING) == pieces / 2u);
    assert(apu_ac97_zc_first_piece(RING + 4u, RING) == pieces / 2u);   /* GPOFCUR0 beyond the end wraps */
    printf("    -> lead in [%u, %u) bytes for every write position [PASS]\n", RING / 2u, RING / 2u + PIECE);

    printf("[5] Lead and slips...\n");
    assert(apu_ac97_zc_lead(100, 100, RING) == 0);
    assert(apu_ac97_zc_lead(100, 104, RING) == RING - 4u);   /* the reader just behind: almost a ring */
    assert(apu_ac97_zc_lead(0, RING - 4u, RING) == 4u);      /* across the wrap */
    assert(apu_ac97_zc_slip(RING / 4u - 4u, RING) == -1);    /* catching up: play a piece again */
    assert(apu_ac97_zc_slip(RING / 4u, RING) == 0);
    assert(apu_ac97_zc_slip(RING - RING / 4u, RING) == 0);
    assert(apu_ac97_zc_slip(RING - RING / 4u + 4u, RING) == 1);   /* about to be overwritten: skip one */
    /* A slip moves the lead by one piece back towards the middle */
    assert(apu_ac97_zc_slip(RING / 4u - 4u + PIECE, RING) == 0);
    assert(apu_ac97_zc_slip(RING - RING / 4u + 4u - PIECE, RING) == 0);
    printf("    -> middle half of the ring kept, one piece per slip [PASS]\n");

    printf("[6] Host build: no codec, no zero-copy...\n");
    assert(apu_ac97_start(base) == 0 && !apu_ac97_zero_copy());
    {
        apu_ac97_stats_t st;
        apu_ac97_get_stats(&st);
        assert(!st.zero_copy && st.slips == 0 && st.lead_min == 0);
    }
    printf("    -> [PASS]\n");

    free(s_bar0);
    printf("=== AC97 zero-copy output Test: ALL PASS ===\n");
    return 0;
}
