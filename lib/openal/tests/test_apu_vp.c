#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "apu_vp.h"
#include "apu_mem.h"
#include "mcpx_apu_regs.h"

/*
 * MCPX Voice Processor driver (apu_vp.c) against a plain-memory BAR0: voice
 * records are checked in the voice array, and the last value written to each
 * method offset (HRTF only) in the method window. Encodings are checked against the worked values in
 * lib/openal/docs/XEMU_VERIFICATION.md (sections 4.6, 4.7 and 7.2).
 */

#define BAR0_SIZE 0x80000u

static uint8_t *s_bar0;

static uint32_t reg(uint32_t off) { return *(volatile uint32_t *)(s_bar0 + off); }
static void set_reg(uint32_t off, uint32_t v) { *(volatile uint32_t *)(s_bar0 + off) = v; }
static uint32_t meth(uint32_t m) { return reg(MCPX_APU_METHOD_BASE + m); }

static uint32_t voice_dw(uint32_t h, uint32_t off) {
    return *(uint32_t *)(apu_vp_debug_voice_record(h) + off);
}

static void set_voice_dw(uint32_t h, uint32_t off, uint32_t v) {
    *(uint32_t *)(apu_vp_debug_voice_record(h) + off) = v;
}

int main(void) {
    uintptr_t base;
    void *buf_a, *buf_b;
    uint32_t phys_a = 0, phys_b = 0;
    apu_vp_voice_params_t p;
    uint32_t vola, volb, volc, vbin, fmt_bins, ba_a, ba_b;
    uint16_t vols[APU_VP_OUTPUTS];
    uint8_t bins[APU_VP_OUTPUTS] = { 0, 1, 2, 3, 4, 5, 0, 0 };
    int i;

    printf("=== MCPX APU Voice Processor Driver (route A) Test ===\n");

    /* 1. Encodings */
    printf("[1] Pitch, volume, routing and format encodings...\n");
    assert(apu_vp_pitch_from_ratio(1.0f) == 0);
    assert(apu_vp_pitch_from_ratio(2.0f) == 4096);
    assert(apu_vp_pitch_from_ratio(0.5f) == -4096);
    assert(apu_vp_pitch_from_ratio(44100.0f / 48000.0f) == -501);    /* 0xFE0B */
    assert(apu_vp_pitch_from_ratio(32000.0f / 48000.0f) == -2396);   /* 0xF6A4 */
    assert(apu_vp_pitch_from_ratio(8000.0f / 48000.0f) == -10588);   /* 0xD6A4 */
    assert((uint16_t)apu_vp_pitch_from_ratio(44100.0f / 48000.0f) == 0xFE0Bu);
    assert(apu_vp_pitch_from_ratio(0.0f) == -32768);

    assert(apu_vp_atten_from_gain(1.0f) == 0);
    assert(apu_vp_atten_from_gain(2.0f) == 0);                      /* cannot amplify */
    assert(apu_vp_atten_from_gain(0.5f) == 0x181);
    assert(apu_vp_atten_from_gain(0.7071f) == 0x0C1);
    assert(apu_vp_atten_from_gain(0.0f) == 0xFFF);
    assert(apu_vp_atten_from_gain(1e-4f) == 0xFFF);

    for (i = 0; i < APU_VP_OUTPUTS; i++) vols[i] = 0xFFF;
    vols[0] = vols[1] = 0;
    apu_vp_pack_volumes(vols, &vola, &volb, &volc);
    assert(vola == 0x000F000Fu && volb == 0xFFFFFFFFu && volc == 0xFFFFFFFFu);
    for (i = 0; i < APU_VP_OUTPUTS; i++) vols[i] = (uint16_t)(0x100 * (i + 1) + i);
    apu_vp_pack_volumes(vols, &vola, &volb, &volc);
    /* outputs 6 and 7 are split across the low nibbles */
    assert(((vola >> 4) & 0xFFF) == vols[0] && (vola >> 20) == vols[1]);
    assert(((volb >> 4) & 0xFFF) == vols[2] && (volb >> 20) == vols[3]);
    assert(((volc >> 4) & 0xFFF) == vols[4] && (volc >> 20) == vols[5]);
    assert(((vola & 0xF) | ((volb & 0xF) << 4) | ((volc & 0xF) << 8)) == vols[6]);
    assert((((vola >> 16) & 0xF) | (((volb >> 16) & 0xF) << 4) | (((volc >> 16) & 0xF) << 8)) == vols[7]);

    apu_vp_pack_bins(bins, &vbin, &fmt_bins);
    assert(vbin == (0u | (1u << 5) | (2u << 10) | (3u << 16) | (4u << 21) | (5u << 26)));
    assert(fmt_bins == 0);
    bins[0] = 0; bins[1] = 1; bins[2] = bins[3] = bins[4] = bins[5] = 0;
    apu_vp_pack_bins(bins, &vbin, &fmt_bins);
    assert(vbin == 0x20u);                                          /* step 19: V0BIN 0, V1BIN 1 */

    assert(apu_vp_format(1, 16, false) == 0x50000000u);
    assert(apu_vp_format(1, 16, true) == (0x50000000u | (1u << 25)));
    assert(apu_vp_format(2, 16, false) == (0x50000000u | (1u << 27) | (1u << 16)));
    assert(apu_vp_format(1, 8, false) == 0u);
    printf("    -> Encodings match the report's worked values [PASS]\n");

    /* 2. Bring-up */
    printf("[2] Bring-up register sequence...\n");
    s_bar0 = (uint8_t *)calloc(1, BAR0_SIZE);
    assert(s_bar0 != NULL);
    base = (uintptr_t)s_bar0;
    assert(apu_mem_init(0) == 0);
    set_reg(MCPX_APU_SECTL, 0x1234);
    assert(apu_vp_init(base) == 0);
    assert(apu_vp_ready());
    assert(apu_vp_hrtf_available());                                /* FEPIOQ reads 0: methods run (xemu) */
    assert(reg(MCPX_APU_SECTL) == MCPX_APU_SECTL_RUN);
    assert(reg(MCPX_APU_TVL2D) == 0xFFFFu && reg(MCPX_APU_TVL3D) == 0xFFFFu && reg(MCPX_APU_TVLMP) == 0xFFFFu);
    assert(reg(MCPX_APU_FETFORCE1) == 0x8000u);
    assert(reg(MCPX_APU_VPVADDR) != 0 && reg(MCPX_APU_VPSGEADDR) != 0 && reg(MCPX_APU_VPSSLADDR) != 0);
    assert(reg(MCPX_APU_FENADDR) != 0);
    assert(reg(MCPX_APU_IEN) == 0 && reg(MCPX_APU_EPRST) == 0 && reg(MCPX_APU_FECTL) == 0);
    assert(reg(MCPX_APU_GPRST) == 3u);                         /* GP stage released (apu_gp.c) */
    printf("    -> Lists empty, idle trap on, notifiers set, frames started [PASS]\n");

    /* 3. Voice start: the record is written directly and linked at the 2D head */
    printf("[3] Voice record and start...\n");
    buf_a = apu_mem_alloc_phys(20000, 8, &phys_a);
    buf_b = apu_mem_alloc_phys(5000, 8, &phys_b);
    assert(buf_a && buf_b);

    memset(&p, 0, sizeof(p));
    p.phys = phys_a;
    p.bytes = 20000;
    p.channels = 1;
    p.bits = 16;
    p.loop = false;
    p.pitch = -501;
    for (i = 0; i < APU_VP_OUTPUTS; i++) { p.bins[i] = (uint8_t)(i < 6 ? i : 0); p.vols[i] = 0xFFF; }
    p.vols[0] = 0x181;
    set_reg(MCPX_APU_METHOD_BASE + MCPX_METH_VOICE_ON, 0xDEAD);     /* no voice method may be written */
    assert(apu_vp_voice_start(base, 70, &p) == 0);
    assert(meth(MCPX_METH_VOICE_ON) == 0xDEAD);

    assert(voice_dw(70, MCPX_VOICE_CFG_VBIN) == (0u | (1u << 5) | (2u << 10) | (3u << 16) | (4u << 21) | (5u << 26)));
    assert(voice_dw(70, MCPX_VOICE_CFG_FMT) == 0x50000000u);
    assert(voice_dw(70, MCPX_VOICE_CFG_ENV0) == 0);
    assert(voice_dw(70, MCPX_VOICE_CFG_HRTF_TARGET) == 0xFFFFu);
    assert(voice_dw(70, MCPX_VOICE_TAR_PITCH_LINK) == 0xFE0BFFFFu);  /* pitch, end of list */
    assert(voice_dw(70, MCPX_VOICE_PAR_NEXT) == 10000u - 1u);        /* EBO: 20000 bytes of mono S16 */
    assert(voice_dw(70, MCPX_VOICE_PAR_OFFSET) == 0);                 /* CBO */
    assert(voice_dw(70, MCPX_VOICE_CUR_PSH_SAMPLE) == 0);             /* LBO */
    assert(((voice_dw(70, MCPX_VOICE_TAR_VOLA) >> 4) & 0xFFF) == 0x181);
    assert(voice_dw(70, MCPX_VOICE_PAR_STATE) == MCPX_PAR_STATE_ACTIVE_VOICE);
    assert(reg(MCPX_APU_TVL2D) == 70);
    assert(apu_vp_voice_active(70));
    ba_a = voice_dw(70, MCPX_VOICE_CUR_PSL_START);
    assert((ba_a & 0xFFFu) == (phys_a & 0xFFFu));
    {
        const uint32_t *sge = apu_vp_debug_sge();
        uint32_t pages = ((phys_a + 20000u - 1u) >> 12) - (phys_a >> 12) + 1u, k;
        for (k = 0; k < pages; k++) {
            assert(sge[2u * ((ba_a >> 12) + k)] == (phys_a & ~0xFFFu) + k * 4096u);
        }
    }
    printf("    -> Record fields as the methods would leave them, SGE maps the pages, voice linked [PASS]\n");

    /* 4. Mapping cache and list order: 72 -> 71 -> 70 */
    printf("[4] SGE mapping reuse and list linking...\n");
    assert(apu_vp_voice_start(base, 71, &p) == 0);
    assert(voice_dw(71, MCPX_VOICE_CUR_PSL_START) == ba_a);
    p.phys = phys_b;
    p.bytes = 5000;
    p.channels = 2;
    assert(apu_vp_voice_start(base, 72, &p) == 0);
    ba_b = voice_dw(72, MCPX_VOICE_CUR_PSL_START);
    assert(ba_b != ba_a && (ba_b & 0xFFFu) == (phys_b & 0xFFFu));
    assert(voice_dw(72, MCPX_VOICE_PAR_NEXT) == 5000u / 4u - 1u);    /* stereo S16 frames */
    assert(voice_dw(72, MCPX_VOICE_CFG_FMT) & MCPX_FMT_STEREO);
    assert(reg(MCPX_APU_TVL2D) == 72);
    assert((voice_dw(72, MCPX_VOICE_TAR_PITCH_LINK) & 0xFFFFu) == 71);
    assert((voice_dw(71, MCPX_VOICE_TAR_PITCH_LINK) & 0xFFFFu) == 70);
    assert((voice_dw(70, MCPX_VOICE_TAR_PITCH_LINK) & 0xFFFFu) == 0xFFFFu);
    /* Restarting a playing handle relinks it once, at the head */
    assert(apu_vp_voice_start(base, 71, &p) == 0);
    assert(reg(MCPX_APU_TVL2D) == 71);
    assert((voice_dw(71, MCPX_VOICE_TAR_PITCH_LINK) & 0xFFFFu) == 72);
    assert((voice_dw(72, MCPX_VOICE_TAR_PITCH_LINK) & 0xFFFFu) == 70);
    printf("    -> Mapped pages reused, voices chained at the head, restart without a cycle [PASS]\n");

    /* 5. Live update: targets and pitch, next field kept */
    printf("[5] Live pitch/volume update...\n");
    vols[0] = 0x0C1;
    for (i = 1; i < APU_VP_OUTPUTS; i++) vols[i] = 0xFFF;
    apu_vp_voice_update(base, 72, 4096, vols, -1);
    assert(voice_dw(72, MCPX_VOICE_TAR_PITCH_LINK) == (0x10000000u | 70u));
    assert(((voice_dw(72, MCPX_VOICE_TAR_VOLA) >> 4) & 0xFFF) == 0x0C1);
    printf("    -> Pitch and volume targets rewritten, list link untouched [PASS]\n");

    /* 6. Ended one-shot in the middle and at the head; xemu trap release */
    printf("[6] Ended voices and the idle-voice trap...\n");
    set_voice_dw(72, MCPX_VOICE_PAR_STATE, 0);                       /* the VP ended 72 (middle) */
    apu_vp_service(base);
    assert(reg(MCPX_APU_TVL2D) == 71);
    assert((voice_dw(71, MCPX_VOICE_TAR_PITCH_LINK) & 0xFFFFu) == 70);
    assert(!apu_vp_voice_active(72) && apu_vp_voice_active(71));
    set_voice_dw(71, MCPX_VOICE_PAR_STATE, 0);                       /* then 71 (head), trapped on xemu */
    set_reg(MCPX_APU_FECTL, 0xE0);
    set_reg(MCPX_APU_FEDECPARAM, 71);
    assert(apu_vp_service(base) == 1);
    assert(reg(MCPX_APU_FECTL) == 0 && reg(MCPX_APU_ISTS) == MCPX_APU_ISTS_FETINTSTS);
    assert(reg(MCPX_APU_TVL2D) == 70);
    assert(apu_vp_service(base) == 0);                               /* nothing trapped */
    /* An active voice reported by a trap stays linked */
    set_reg(MCPX_APU_FECTL, 0xE0);
    set_reg(MCPX_APU_FEDECPARAM, 70);
    assert(apu_vp_service(base) == 1 && reg(MCPX_APU_TVL2D) == 70);
    printf("    -> Ended voices unlinked from middle and head, active one kept [PASS]\n");

    printf("[6b] HRTF table upload, submixes and per-voice entry...\n");
    {
        static apu_hrtf_entry_t table[APU_HRTF_ENTRIES];
        const apu_hrtf_entry_t *last;
        uint32_t w;

        /* apu_vp_init() loaded the table: the last entry's writes are left in the method window */
        apu_hrtf_build_table(table);
        last = &table[APU_HRTF_ENTRIES - 1];
        assert(meth(MCPX_METH_SET_CURRENT_HRTF_ENTRY) == APU_HRTF_ENTRIES - 1);
        w = meth(MCPX_METH_SET_HRIR + 4u * 14u);
        assert((int8_t)(w & 0xFF) == last->left[28] && (int8_t)((w >> 8) & 0xFF) == last->right[28]);
        assert((int8_t)((w >> 16) & 0xFF) == last->left[29] && (int8_t)(w >> 24) == last->right[29]);
        w = meth(MCPX_METH_SET_HRIR_X);
        assert((int8_t)(w & 0xFF) == last->left[30] && (int8_t)((w >> 8) & 0xFF) == last->right[30]);
        assert((int16_t)(w >> 16) == last->itd);
        assert(meth(MCPX_METH_SET_HRTF_SUBMIXES) == 0x03020100u);  /* front L/R, surround L/R */
        assert(meth(MCPX_METH_SET_HRTF_HEADROOM) == 0);

        /* Handles below 64 get the entry in the record and latched by method (xemu) */
        p.phys = phys_a;
        p.bytes = 20000;
        p.channels = 1;
        p.hrtf_entry = 40;
        assert(apu_vp_voice_start(base, 5, &p) == 0);
        assert(voice_dw(5, MCPX_VOICE_CFG_HRTF_TARGET) == 40u);
        assert(meth(MCPX_METH_SET_CURRENT_VOICE) == 5u && meth(MCPX_METH_TAR_HRTF) == 40u);
        assert(apu_vp_voice_start(base, 75, &p) == 0);
        assert(voice_dw(75, MCPX_VOICE_CFG_HRTF_TARGET) == 0xFFFFu);
        p.hrtf_entry = -1;
        assert(apu_vp_voice_start(base, 6, &p) == 0);
        assert(voice_dw(6, MCPX_VOICE_CFG_HRTF_TARGET) == 0xFFFFu);

        apu_vp_voice_update(base, 5, 0, vols, 72);
        assert(voice_dw(5, MCPX_VOICE_CFG_HRTF_TARGET) == 72u && meth(MCPX_METH_TAR_HRTF) == 72u);
        apu_vp_voice_update(base, 75, 0, vols, 12);                 /* plain voice: no latch */
        assert(voice_dw(75, MCPX_VOICE_CFG_HRTF_TARGET) == 0xFFFFu && meth(MCPX_METH_TAR_HRTF) == 72u);
    }
    printf("    -> 128 entries uploaded, submixes set, entries latched only below handle 64 [PASS]\n");

    printf("[7] Pause, off and teardown...\n");
    /* list now: 6 -> 75 -> 5 -> 70 */
    set_voice_dw(75, MCPX_VOICE_PAR_OFFSET, 1234);
    apu_vp_voice_pause(base, 75, true);
    assert((voice_dw(6, MCPX_VOICE_TAR_PITCH_LINK) & 0xFFFFu) == 5u);
    assert(apu_vp_voice_active(75) && apu_vp_voice_position(75) == 1234u);
    apu_vp_service(base);                                            /* paused is not ended */
    assert(apu_vp_voice_active(75));
    apu_vp_voice_pause(base, 75, false);
    assert(reg(MCPX_APU_TVL2D) == 75 && (voice_dw(75, MCPX_VOICE_TAR_PITCH_LINK) & 0xFFFFu) == 6u);
    assert(apu_vp_voice_position(75) == 1234u);
    apu_vp_voice_off(base, 75);
    assert(reg(MCPX_APU_TVL2D) == 6 && voice_dw(75, MCPX_VOICE_PAR_STATE) == 0 && !apu_vp_voice_active(75));
    /* A frame in flight writing ACTIVE back does not resurrect it */
    set_voice_dw(75, MCPX_VOICE_PAR_STATE, MCPX_PAR_STATE_ACTIVE_VOICE);
    assert(!apu_vp_voice_active(75));
    apu_vp_deinit(base);
    assert(!apu_vp_ready());
    assert(reg(MCPX_APU_SECTL) == 0 && reg(MCPX_APU_TVL2D) == 0xFFFFu);
    assert(apu_vp_voice_start(base, 70, &p) != 0);                   /* not initialised */
    printf("    -> Pause keeps the position, off unlinks, lists emptied on teardown [PASS]\n");

    /* 8. Real console: the front end runs one method and stalls */
    printf("[8] Stalled front end (real console)...\n");
    memset(s_bar0, 0, BAR0_SIZE);
    set_reg(MCPX_APU_FEPIOQ, 0x00191901u);                           /* one method stuck in the queue */
    assert(apu_vp_init(base) == 0);
    assert(!apu_vp_hrtf_available());
    assert(reg(MCPX_APU_FETFORCE1) == 0);                            /* ended voices must not halt frames */
    assert(meth(MCPX_METH_SET_HRIR_X) == 0 && meth(MCPX_METH_SET_HRTF_SUBMIXES) == 0);   /* no HRTF methods */
    assert(reg(MCPX_APU_FECTL) == 0);                                /* left free-running after the steps */
    assert(reg(MCPX_APU_SECTL) == MCPX_APU_SECTL_RUN && reg(MCPX_APU_GPRST) == 3u);
    p.phys = phys_a;
    p.bytes = 20000;
    p.channels = 1;
    p.hrtf_entry = 40;
    assert(apu_vp_voice_start(base, 5, &p) == 0);
    assert(voice_dw(5, MCPX_VOICE_CFG_HRTF_TARGET) == 0xFFFFu);      /* no HRTF without the table */
    assert(meth(MCPX_METH_TAR_HRTF) == 0 && reg(MCPX_APU_TVL2D) == 5);
    /* A one-shot never reaches its end on hardware: it loops over the silent tail */
    p.loop = false;
    p.tail_bytes = APU_VP_SILENT_TAIL_BYTES;                        /* mono S16: 128 frames */
    assert(apu_vp_voice_start(base, 77, &p) == 0);
    assert(voice_dw(77, MCPX_VOICE_CFG_FMT) & MCPX_FMT_LOOP);
    assert(voice_dw(77, MCPX_VOICE_CUR_PSH_SAMPLE) == 10000u);      /* LBO: first silent frame */
    assert(voice_dw(77, MCPX_VOICE_PAR_NEXT) == 10000u + 128u - 1u);  /* EBO: last silent frame */
    set_voice_dw(77, MCPX_VOICE_PAR_OFFSET, 9999);
    apu_vp_service(base);
    assert(apu_vp_voice_active(77) && reg(MCPX_APU_TVL2D) == 77);
    set_voice_dw(77, MCPX_VOICE_PAR_OFFSET, 10003);                 /* into the silence */
    assert(!apu_vp_voice_active(77));
    apu_vp_service(base);
    assert(reg(MCPX_APU_TVL2D) == 5 && voice_dw(77, MCPX_VOICE_PAR_STATE) == 0);
    p.loop = true;                                                   /* loops keep their real end */
    assert(apu_vp_voice_start(base, 78, &p) == 0);
    assert(voice_dw(78, MCPX_VOICE_CUR_PSH_SAMPLE) == 0 && voice_dw(78, MCPX_VOICE_PAR_NEXT) == 9999u);
    apu_vp_voice_off(base, 78);
    p.loop = false;
    p.tail_bytes = 0;
    {
        apu_vp_fe_stats_t fs;
        set_reg(MCPX_APU_FECTL, 0x0007808Fu);                         /* a front-end message is pending */
        set_reg(MCPX_APU_FEDECMETH, 0x8008u);
        set_reg(MCPX_APU_FEDECPARAM, 1u);
        apu_vp_service(base);
        apu_vp_get_fe_stats(&fs);
        assert(fs.events == 1 && fs.last_meth == 0x8008u && fs.last_param == 1u);
        assert(reg(MCPX_APU_FECTL) == 0x0007800Fu);                   /* bit 15 written back, mode free-running */
    }
    apu_vp_deinit(base);
    printf("    -> HRTF off, voices still play from their records [PASS]\n");

    apu_mem_free_phys(buf_a);
    apu_mem_free_phys(buf_b);
    apu_mem_shutdown();
    free(s_bar0);

    printf("=== All MCPX VP Driver Tests Passed Successfully! ===\n");
    return 0;
}
