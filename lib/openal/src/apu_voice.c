#include "apu_voice.h"
#include "apu_vp.h"
#include "apu_hrtf.h"
#include <math.h>
#include <string.h>

/*
 * ============================================================================
 * Internal State of Voice Processor Subsystem
 * ============================================================================
 */
static uintptr_t s_apu_base = NV_PAPU_BASE;
static NVAPU_VOICE_CONTEXT_3D *s_voice_contexts = NULL;
static uint32_t s_voice_contexts_phys = 0;
static uint32_t s_stop_calls = 0;

/*
 * Voice Processor backend. When the session runs on the real BAR0 (only
 * possible with -DOPENAL_APU_REAL_MMIO), the calls below drive the MCPX VP
 * through apu_vp.c; slot n is VP handle APU_VP_HANDLE_BASE + n. Otherwise
 * (null backend, host mocks) they keep updating the software model.
 */
static bool s_vp_hw = false;

typedef struct {
    uint32_t phys;          /* contiguous data; with pages: the first byte's offset in pages[0] */
    const uint32_t *pages;  /* static buffers: physical page list (apu_voice_bind_pages) */
    uint32_t page_count;
    uint32_t map_key;
    uint32_t bytes;
    int channels;
    int bits;
    uint32_t loop_start;    /* loop region of a looping voice (loop_end 0 = whole buffer) */
    uint32_t loop_end;
    uint32_t start_frame;   /* first frame of the next trigger */
} voice_buffer_t;

static voice_buffer_t s_voice_buffer[APU_VOICE_MAX_SLOTS];

/* Direct mode (AL_XBOX_DIRECT_GAINS): the engine's left/right gains replace the pan */
typedef struct {
    bool on;
    float left, right;
} voice_direct_t;

static voice_direct_t s_voice_direct[APU_VOICE_MAX_SLOTS];

/* Effect sends of each slot (apu_voice_set_sends): absolute linear gains */
static float s_voice_send[APU_VOICE_MAX_SLOTS][APU_VOICE_SENDS];
static float s_voice_lowpass[APU_VOICE_MAX_SLOTS];   /* gain at 5 kHz; 1 = no filter */

/* Console: send n leaves on outputs 4 + 2n (left or mono) and 5 + 2n (right
 * or mono) for the GP kernel's input pair, bins 2/3 (dsp/gp_kfreeverb.asm KIN) */
#define SEND_OUT     4
#define SEND_BIN_L   2u
#define SEND_BIN_R   3u

/* HRTF: the slot plays as an HRTF voice (its last start); AL_XBOX_HRTF (on by default) */
static bool s_slot_hrtf[APU_VOICE_MAX_SLOTS];
static bool s_hrtf_enabled = true;

/* Software model: play position of each slot (apu_voice_debug_set_position) */
static uint32_t s_model_pos[APU_VOICE_MAX_SLOTS];

/* VP handle each slot last started (mono 3D sources: slot n -> HRTF handle n;
 * everything else: APU_VP_HANDLE_BASE + n), and each slot's source direction */
static uint32_t s_voice_handle[APU_VOICE_MAX_SLOTS];

/* Slot that last started each VP handle. Handles are reused across slots, so a
 * slot only stops, pauses or updates its handle while it still owns it */
static uint16_t s_handle_slot[APU_VP_MAX_HANDLES];

static bool owns_handle(uint32_t index) {
    uint32_t h = s_voice_handle[index];
    return h < APU_VP_MAX_HANDLES && s_handle_slot[h] == index;
}
static float s_voice_local[APU_VOICE_MAX_SLOTS][3];

static uint32_t vp_handle(uint32_t index) {
    return s_voice_handle[index];
}

static void vp_params_core(uint32_t index, const NVAPU_VOICE_CONTEXT_3D *ctx, bool hrtf, int16_t *pitch,
                           uint8_t bins[APU_VP_OUTPUTS], uint16_t vols[APU_VP_OUTPUTS]) {
    float master = (float)ctx->master_vol_left / 65535.0f;
    int i;
    *pitch = apu_vp_pitch_from_ratio((float)ctx->pitch_step / 65536.0f);
    if (s_voice_direct[index].on) {
        /* Direct mode: output 0 -> bin 0 (left), output 1 -> bin 1 (right) at
         * the engine's gains, full float precision into the 12-bit attenuation */
        for (i = 0; i < APU_VP_OUTPUTS; i++) {
            bins[i] = (uint8_t)(i & 1);
            vols[i] = 0xFFFu;
        }
        vols[0] = apu_vp_atten_from_gain(master * s_voice_direct[index].left);
        vols[1] = apu_vp_atten_from_gain(master * s_voice_direct[index].right);
        return;
    }
    for (i = 0; i < APU_VP_OUTPUTS; i++) {
        bins[i] = (uint8_t)((i < 6) ? i : 0);
    }
    if (hrtf && !apu_vp_front_end_ok()) {
        /* Console HRTF voice: the left ear feeds HRTF submix slot 0 through
         * output 0, the right ear slot 2 through output 2 (apu_vp.c routes them
         * to bins 0 and 1); the filter makes the direction, both ears get the
         * source's level. Outputs 1 and 3 would feed the unused slots. */
        float fl = ctx->mixbin_gain[0] / 255.0f, fr = ctx->mixbin_gain[1] / 255.0f;
        float sl = ctx->mixbin_gain[2] / 255.0f, sr = ctx->mixbin_gain[3] / 255.0f;
        float c = ctx->mixbin_gain[4] / 255.0f;
        float level = sqrtf(fl * fl + fr * fr + sl * sl + sr * sr + c * c);
        if (level > 1.0f) level = 1.0f;
        for (i = 0; i < APU_VP_OUTPUTS; i++) {
            bins[i] = (uint8_t)(i & 1);
            vols[i] = 0xFFFu;
        }
        vols[0] = vols[2] = apu_vp_atten_from_gain(master * level);
        return;
    }
    if (!apu_vp_front_end_ok()) {
        /* Real console: the GP forwards only bins 0/1 (stereo), so the 5.1 pan
         * is folded on the CPU into two outputs (power sum of FL+SL+C/-3 dB+
         * LFE/-6 dB per side); output 0 plays the left channel of a stereo
         * voice and output 1 the right. The other six are muted, which keeps
         * the VP's per-voice mixing work down. */
        float fl = ctx->mixbin_gain[0] / 255.0f, fr = ctx->mixbin_gain[1] / 255.0f;
        float sl = ctx->mixbin_gain[2] / 255.0f, sr = ctx->mixbin_gain[3] / 255.0f;
        float c = ctx->mixbin_gain[4] / 255.0f, lfe = ctx->mixbin_gain[5] / 255.0f;
        float shared = 0.5f * c * c + 0.25f * lfe * lfe;
        for (i = 0; i < APU_VP_OUTPUTS; i++) {
            bins[i] = (uint8_t)(i & 1);
            vols[i] = 0xFFFu;
        }
        vols[0] = apu_vp_atten_from_gain(master * sqrtf(fl * fl + sl * sl + shared));
        vols[1] = apu_vp_atten_from_gain(master * sqrtf(fr * fr + sr * sr + shared));
        return;
    }
    if (hrtf) {
        /* HRTF voice: outputs 0-3 go to the HRTF submixes (front pair 0/1,
         * surround pair 2/3) and the HRTF filter makes left/right; the pan's
         * equal-power front and rear shares set the two pairs. Output 5 keeps LFE. */
        float fl = ctx->mixbin_gain[0] / 255.0f, fr = ctx->mixbin_gain[1] / 255.0f;
        float sl = ctx->mixbin_gain[2] / 255.0f, sr = ctx->mixbin_gain[3] / 255.0f;
        float c = ctx->mixbin_gain[4] / 255.0f, lfe = ctx->mixbin_gain[5] / 255.0f;
        float front = sqrtf(fl * fl + fr * fr + c * c), rear = sqrtf(sl * sl + sr * sr);
        vols[0] = vols[1] = apu_vp_atten_from_gain(master * front);
        vols[2] = vols[3] = apu_vp_atten_from_gain(master * rear);
        vols[4] = 0xFFFu;
        vols[5] = apu_vp_atten_from_gain(master * lfe);
        vols[6] = vols[7] = 0xFFFu;
        return;
    }
    /* Output i feeds mixbin i (FL, FR, SL, SR, C, LFE in this library's order) */
    for (i = 0; i < APU_VP_OUTPUTS; i++) {
        vols[i] = (i < 6) ? apu_vp_atten_from_gain(master * (float)ctx->mixbin_gain[i] / 255.0f) : 0xFFFu;
    }
}

/* The voice's pitch, bins and volumes; on a console also its effect sends:
 * send n on outputs 4 + 2n / 5 + 2n to bins 2 / 3, each at half the send's
 * gain (a stereo voice's left and right, or the mono sound twice: their sum,
 * which a mono-input kernel reads, is the send) */
static void vp_params_from_context(uint32_t index, const NVAPU_VOICE_CONTEXT_3D *ctx, bool hrtf, int16_t *pitch,
                                   uint8_t bins[APU_VP_OUTPUTS], uint16_t vols[APU_VP_OUTPUTS]) {
    uint32_t n;
    vp_params_core(index, ctx, hrtf, pitch, bins, vols);
    if (!apu_vp_front_end_ok()) {
        for (n = 0; n < APU_VOICE_SENDS; n++) {
            float g = s_voice_send[index][n];
            uint16_t v = (g > 0.0f) ? apu_vp_atten_from_gain(0.5f * g) : 0xFFFu;
            bins[SEND_OUT + 2u * n] = (uint8_t)SEND_BIN_L;
            bins[SEND_OUT + 2u * n + 1u] = (uint8_t)SEND_BIN_R;
            vols[SEND_OUT + 2u * n] = vols[SEND_OUT + 2u * n + 1u] = v;
        }
    }
}

/*
 * ----------------------------------------------------------------------------
 * Voice Subsystem Lifecycle
 * ----------------------------------------------------------------------------
 */

int apu_voice_subsystem_init(uintptr_t apu_base, void *context_array_virt, uint32_t context_array_phys)
{
    uint32_t i;
    uint32_t control_val;

    if (context_array_virt == NULL || context_array_phys == 0) {
        return -1;
    }

    s_apu_base = (apu_base != 0) ? apu_base : NV_PAPU_BASE;
    s_voice_contexts = (NVAPU_VOICE_CONTEXT_3D *)context_array_virt;
    s_voice_contexts_phys = context_array_phys;

    /* Reset the 64 contexts every caller provides (the device's array holds
     * APU_VOICE_MAX_SLOTS; the rest are reset below for the hardware backend) */
    for (i = 0; i < NV_PAPU_NUM_3D_VOICES; ++i) {
        apu_voice_context_reset(&s_voice_contexts[i]);
    }
    memset(s_voice_buffer, 0, sizeof(s_voice_buffer));
    memset(s_voice_local, 0, sizeof(s_voice_local));
    memset(s_voice_direct, 0, sizeof(s_voice_direct));
    for (i = 0; i < APU_VOICE_MAX_SLOTS; ++i) {
        s_voice_handle[i] = 0xFFFFu;   /* none yet: never turn off a handle another slot may hold */
        s_voice_lowpass[i] = 1.0f;
    }
    for (i = 0; i < APU_VP_MAX_HANDLES; ++i) {
        s_handle_slot[i] = 0xFFFFu;
    }

#ifdef OPENAL_APU_REAL_MMIO
    if (s_apu_base == NV_PAPU_BASE) {
        /* Real hardware: the corrected VP bring-up replaces the register writes below,
         * which target a register map that does not exist (XEMU_VERIFICATION.md C8.1) */
        if (apu_vp_init(s_apu_base) != 0) {
            return -2;
        }
        for (i = NV_PAPU_NUM_3D_VOICES; i < APU_VOICE_MAX_SLOTS; ++i) {
            apu_voice_context_reset(&s_voice_contexts[i]);   /* alc_context.c allocates all of them */
        }
        s_vp_hw = true;
        return 0;
    }
#endif

    /* Program hardware Voice Processor base physical address */
    apu_write32(s_apu_base, NV_PAPU_VP_BASE_ADDR, s_voice_contexts_phys);

    /*
     * Configure NV_PAPU_VP_CONTROL:
     * - Bit 0 (RESET): 0 (Clear hardware reset)
     * - Bit 1 (ENABLE): 1 (Enable Voice Processor engine)
     * - Bit 2 (MODE_3D): 1 (Enable 64 3D Voices mode with ITD/HRTF)
     */
    control_val = NV_PAPU_VP_CONTROL_ENABLE | NV_PAPU_VP_CONTROL_MODE_3D;
    apu_write32(s_apu_base, NV_PAPU_VP_CONTROL, control_val);

    /* Clear active and pause bitmasks for all 64 voices */
    apu_write32(s_apu_base, NV_PAPU_VP_ACTIVE_0, 0u);
    apu_write32(s_apu_base, NV_PAPU_VP_ACTIVE_1, 0u);
    apu_write32(s_apu_base, NV_PAPU_VP_PAUSE_0, 0u);
    apu_write32(s_apu_base, NV_PAPU_VP_PAUSE_1, 0u);

    return 0;
}

void apu_voice_subsystem_deinit(uintptr_t apu_base)
{
    uintptr_t base = (apu_base != 0) ? apu_base : s_apu_base;

    if (s_vp_hw) {
        apu_vp_deinit(base);
        s_vp_hw = false;
        s_voice_contexts = NULL;
        s_voice_contexts_phys = 0u;
        return;
    }

    /* Stop all running voices immediately */
    apu_write32(base, NV_PAPU_VP_ACTIVE_0, 0u);
    apu_write32(base, NV_PAPU_VP_ACTIVE_1, 0u);
    apu_write32(base, NV_PAPU_VP_PAUSE_0, 0u);
    apu_write32(base, NV_PAPU_VP_PAUSE_1, 0u);

    /* Assert reset to silence the Voice Processor core */
    apu_write32(base, NV_PAPU_VP_CONTROL, NV_PAPU_VP_CONTROL_RESET);

    s_voice_contexts = NULL;
    s_voice_contexts_phys = 0u;
}

void apu_voice_context_reset(NVAPU_VOICE_CONTEXT_3D *ctx)
{
    if (ctx == NULL) {
        return;
    }

    memset(ctx, 0, sizeof(NVAPU_VOICE_CONTEXT_3D));

    /* Initialize to default 3D rendering mode and unity pitch */
    ctx->format = NVAPU_VOICE_FORMAT_PCM16;
    ctx->channels = NVAPU_VOICE_CHANNELS_MONO;
    ctx->loop_mode = NVAPU_VOICE_LOOP_OFF;
    ctx->mode_3d = 1u;
    ctx->active = 0u;
    ctx->pitch_step = APU_PITCH_STEP_UNITY;
    ctx->hrtf_b0 = 16384; /* Default Q14 identity passthrough (b0 = 1.0) */
}

int apu_voice_setup(uint32_t index, const NVAPU_VOICE_CONTEXT_3D *ctx)
{
    if (index >= APU_VOICE_MAX_SLOTS || ctx == NULL || s_voice_contexts == NULL) {
        return -1;
    }

    memcpy(&s_voice_contexts[index], ctx, sizeof(NVAPU_VOICE_CONTEXT_3D));
    return 0;
}

NVAPU_VOICE_CONTEXT_3D *apu_voice_get_context(uint32_t index)
{
    if (index >= APU_VOICE_MAX_SLOTS || s_voice_contexts == NULL) {
        return NULL;
    }

    return &s_voice_contexts[index];
}

int apu_voice_trigger(uintptr_t apu_base, uint32_t index)
{
    uintptr_t base;

    if (index >= APU_VOICE_MAX_SLOTS) {
        return -1;
    }

    base = (apu_base != 0) ? apu_base : s_apu_base;

    /* Update internal context active status if registered */
    if (s_voice_contexts != NULL) {
        s_voice_contexts[index].active = 1u;
    }
    s_model_pos[index] = s_voice_buffer[index].start_frame;

    if (s_vp_hw) {
        const NVAPU_VOICE_CONTEXT_3D *ctx = &s_voice_contexts[index];
        const voice_buffer_t *vb = &s_voice_buffer[index];
        apu_vp_voice_params_t p;

        memset(&p, 0, sizeof(p));
        p.phys = vb->phys;
        p.pages = vb->pages;
        p.page_count = vb->page_count;
        p.map_key = vb->map_key;
        p.bytes = vb->bytes;
        p.channels = vb->channels;
        p.bits = vb->bits;
        p.loop = ctx->loop_mode != 0u;
        p.tail_bytes = apu_vp_tail_bytes(vb->channels, vb->bits);   /* al_buffer.c pads every buffer with silence */
        p.loop_start = vb->loop_start;
        p.loop_end = vb->loop_end;
        p.start_frame = vb->start_frame;
        /* Mono buffers are 3D sources: an HRTF voice with the table entry for
         * the source direction, where the HRTF stage is usable (on xemu the
         * handle is the slot, below 64; on a console any fresh handle). Stereo,
         * direct mode, AL_XBOX_HRTF off, or no HRTF: plain voice panned by volumes. */
        bool want_hrtf = vb->channels == 1 && apu_vp_hrtf_available() && !s_voice_direct[index].on && s_hrtf_enabled;
        if (want_hrtf && !apu_vp_front_end_ok() && apu_vp_hrtf_voices() >= APU_VP_HRTF_MAX_VOICES) {
            want_hrtf = false;   /* the VP's HRTF budget is used up: pan this one by volume */
        }
        s_slot_hrtf[index] = want_hrtf;
        if (want_hrtf && apu_vp_front_end_ok()) {
            if (owns_handle(index) && vp_handle(index) != index) {
                apu_vp_voice_off(base, vp_handle(index));   /* its previous plain voice */
            }
            s_voice_handle[index] = index;
            p.hrtf_entry = apu_hrtf_entry_for_local(s_voice_local[index]);
        } else {
            /* A fresh handle each start: the slot's previous voice may still be
             * in the VP's current frame, and its record must not be rewritten */
            uint32_t h = apu_vp_alloc_handle(base);
            if (owns_handle(index)) {
                apu_vp_voice_off(base, vp_handle(index));
            }
            if (h == 0xFFFFu) {
                s_voice_contexts[index].active = 0u;
                return -2;
            }
            s_voice_handle[index] = h;
            p.hrtf_entry = want_hrtf ? apu_hrtf_entry_for_local(s_voice_local[index]) : -1;
        }
        s_handle_slot[vp_handle(index)] = (uint16_t)index;
        vp_params_from_context(index, ctx, p.hrtf_entry >= 0, &p.pitch, p.bins, p.vols);
        p.fmode = apu_vp_lowpass(s_voice_lowpass[index], &p.fc);
        if (apu_vp_voice_start(base, vp_handle(index), &p) != 0) {
            s_voice_contexts[index].active = 0u;
            return -2;
        }
        return 0;
    }

    /* Set active bitmask in hardware register */
    if (index < 32u) {
        uint32_t current = apu_read32(base, NV_PAPU_VP_ACTIVE_0);
        apu_write32(base, NV_PAPU_VP_ACTIVE_0, current | (1u << index));
    } else {
        uint32_t current = apu_read32(base, NV_PAPU_VP_ACTIVE_1);
        apu_write32(base, NV_PAPU_VP_ACTIVE_1, current | (1u << (index - 32u)));
    }

    return 0;
}

int apu_voice_stop(uintptr_t apu_base, uint32_t index)
{
    uintptr_t base;

    if (index >= APU_VOICE_MAX_SLOTS) {
        return -1;
    }

    s_stop_calls++;

    base = (apu_base != 0) ? apu_base : s_apu_base;

    /* Clear internal context active status if registered */
    if (s_voice_contexts != NULL) {
        s_voice_contexts[index].active = 0u;
    }

    if (s_vp_hw) {
        if (owns_handle(index)) {
            apu_vp_voice_off(base, vp_handle(index));
        }
        return 0;
    }

    /* Clear active bitmask in hardware register */
    if (index < 32u) {
        uint32_t current = apu_read32(base, NV_PAPU_VP_ACTIVE_0);
        apu_write32(base, NV_PAPU_VP_ACTIVE_0, current & ~(1u << index));
    } else {
        uint32_t current = apu_read32(base, NV_PAPU_VP_ACTIVE_1);
        apu_write32(base, NV_PAPU_VP_ACTIVE_1, current & ~(1u << (index - 32u)));
    }

    return 0;
}

int apu_voice_pause(uintptr_t apu_base, uint32_t index, int pause)
{
    uintptr_t base;

    if (index >= APU_VOICE_MAX_SLOTS) {
        return -1;
    }

    base = (apu_base != 0) ? apu_base : s_apu_base;

    if (s_vp_hw) {
        if (owns_handle(index)) {
            apu_vp_voice_pause(base, vp_handle(index), pause != 0);
        }
        return 0;
    }

    if (index < 32u) {
        uint32_t current = apu_read32(base, NV_PAPU_VP_PAUSE_0);
        uint32_t mask = 1u << index;
        apu_write32(base, NV_PAPU_VP_PAUSE_0, pause ? (current | mask) : (current & ~mask));
    } else {
        uint32_t current = apu_read32(base, NV_PAPU_VP_PAUSE_1);
        uint32_t mask = 1u << (index - 32u);
        apu_write32(base, NV_PAPU_VP_PAUSE_1, pause ? (current | mask) : (current & ~mask));
    }

    return 0;
}

int apu_voice_is_active(uintptr_t apu_base, uint32_t index)
{
    uintptr_t base;

    if (index >= APU_VOICE_MAX_SLOTS) {
        return 0;
    }

    base = (apu_base != 0) ? apu_base : s_apu_base;

    if (s_vp_hw) {
        /* apu_vp_voice_active() already treats a one-shot past its data as
         * ended; the service runs once per frame from apu_voice_service() */
        (void)base;
        return (owns_handle(index) && apu_vp_voice_active(vp_handle(index))) ? 1 : 0;
    }

    if (index < 32u) {
        return (apu_read32(base, NV_PAPU_VP_ACTIVE_0) & (1u << index)) ? 1 : 0;
    } else {
        return (apu_read32(base, NV_PAPU_VP_ACTIVE_1) & (1u << (index - 32u))) ? 1 : 0;
    }
}

void apu_voice_bind_buffer(uint32_t index, uint32_t phys, uint32_t bytes, int channels, int bits)
{
    if (index >= APU_VOICE_MAX_SLOTS) {
        return;
    }
    s_voice_buffer[index].phys = phys;
    s_voice_buffer[index].pages = NULL;
    s_voice_buffer[index].page_count = 0;
    s_voice_buffer[index].map_key = 0;
    s_voice_buffer[index].bytes = bytes;
    s_voice_buffer[index].channels = channels;
    s_voice_buffer[index].bits = bits;
    s_voice_buffer[index].loop_start = 0;
    s_voice_buffer[index].loop_end = 0;
    s_voice_buffer[index].start_frame = 0;
}

void apu_voice_bind_pages(uint32_t index, uint32_t offset, const uint32_t *pages, uint32_t page_count,
                          uint32_t map_key, uint32_t bytes, int channels, int bits)
{
    if (index >= APU_VOICE_MAX_SLOTS) {
        return;
    }
    apu_voice_bind_buffer(index, offset, bytes, channels, bits);
    s_voice_buffer[index].pages = pages;
    s_voice_buffer[index].page_count = page_count;
    s_voice_buffer[index].map_key = map_key;
}

void apu_voice_bind_loop(uint32_t index, uint32_t loop_start, uint32_t loop_end, uint32_t start_frame)
{
    if (index >= APU_VOICE_MAX_SLOTS) {
        return;
    }
    s_voice_buffer[index].loop_start = loop_start;
    s_voice_buffer[index].loop_end = loop_end;
    s_voice_buffer[index].start_frame = start_frame;
}

void apu_voice_set_looping(uintptr_t apu_base, uint32_t index, bool loop)
{
    if (index >= APU_VOICE_MAX_SLOTS) {
        return;
    }
    if (s_voice_contexts != NULL) {
        s_voice_contexts[index].loop_mode = loop ? NVAPU_VOICE_LOOP_ON : NVAPU_VOICE_LOOP_OFF;
    }
    if (s_vp_hw && owns_handle(index)) {
        (void)apu_vp_voice_set_loop((apu_base != 0) ? apu_base : s_apu_base, vp_handle(index), loop,
                                    s_voice_buffer[index].loop_start, s_voice_buffer[index].loop_end);
    }
}

void apu_voice_set_direct(uint32_t index, bool on, float left, float right)
{
    if (index >= APU_VOICE_MAX_SLOTS) {
        return;
    }
    s_voice_direct[index].on = on;
    s_voice_direct[index].left = left;
    s_voice_direct[index].right = right;
}

void apu_voice_set_sends(uint32_t index, const float sends[APU_VOICE_SENDS])
{
    uint32_t n;
    if (index >= APU_VOICE_MAX_SLOTS || sends == NULL) {
        return;
    }
    for (n = 0; n < APU_VOICE_SENDS; n++) {
        float g = sends[n];
        s_voice_send[index][n] = (g > 0.0f) ? ((g < 1.0f) ? g : 1.0f) : 0.0f;
    }
}

void apu_voice_set_lowpass(uint32_t index, float gain_hf)
{
    if (index >= APU_VOICE_MAX_SLOTS) {
        return;
    }
    s_voice_lowpass[index] = (gain_hf > 0.0f) ? ((gain_hf < 1.0f) ? gain_hf : 1.0f) : 0.0f;
}

float apu_voice_debug_lowpass(uint32_t index)
{
    return (index < APU_VOICE_MAX_SLOTS) ? s_voice_lowpass[index] : 1.0f;
}

void apu_voice_set_direction(uint32_t index, const float local_pos[3])
{
    if (index >= APU_VOICE_MAX_SLOTS || local_pos == NULL) {
        return;
    }
    memcpy(s_voice_local[index], local_pos, sizeof(s_voice_local[index]));
}

void apu_voice_commit(uintptr_t apu_base, uint32_t index)
{
    int16_t pitch;
    uint8_t bins[APU_VP_OUTPUTS];
    uint16_t vols[APU_VP_OUTPUTS];

    if (!s_vp_hw || index >= APU_VOICE_MAX_SLOTS || s_voice_contexts == NULL ||
        !s_voice_contexts[index].active || !owns_handle(index)) {
        return;
    }
    {
        uint32_t h = vp_handle(index);
        int entry = s_slot_hrtf[index] ? apu_hrtf_entry_for_local(s_voice_local[index]) : -1;
        uint32_t fc, fmode = apu_vp_lowpass(s_voice_lowpass[index], &fc);
        /* the bins were set at start and do not change with the pan */
        vp_params_from_context(index, &s_voice_contexts[index], entry >= 0, &pitch, bins, vols);
        apu_vp_voice_update((apu_base != 0) ? apu_base : s_apu_base, h, pitch, vols, entry);
        apu_vp_voice_set_filter((apu_base != 0) ? apu_base : s_apu_base, h, fmode, fc);
    }
}

void apu_voice_service(uintptr_t apu_base)
{
    if (s_vp_hw) {
        apu_vp_service((apu_base != 0) ? apu_base : s_apu_base);
    }
}

uint32_t apu_voice_hw_handle(uint32_t index)
{
    return (index < APU_VOICE_MAX_SLOTS && owns_handle(index)) ? vp_handle(index) : 0xFFFFu;
}

bool apu_voice_hw_backend(void)
{
    return s_vp_hw;
}

uint32_t apu_voice_debug_stop_count(void)
{
    return s_stop_calls;
}

uint32_t apu_voice_get_position(uint32_t index)
{
    if (index >= APU_VOICE_MAX_SLOTS) {
        return 0;
    }
    if (s_vp_hw) {
        return owns_handle(index) ? apu_vp_voice_position(vp_handle(index)) : 0u;
    }
    return s_model_pos[index];
}

void apu_voice_debug_set_position(uint32_t index, uint32_t frames)
{
    if (index < APU_VOICE_MAX_SLOTS) {
        s_model_pos[index] = frames;
    }
}

uint32_t apu_voice_slot_count(void)
{
    /* A console (no HRTF stage) plays plain voices on handles 0..127 */
    return (s_vp_hw && !apu_vp_front_end_ok() && apu_vp_plain_handles() >= APU_VP_HW_HANDLES) ? APU_VOICE_HW_SLOTS
                                                                                         : NV_PAPU_NUM_3D_VOICES;
}

void apu_voice_set_hrtf_enabled(bool on)
{
    s_hrtf_enabled = on;
}

bool apu_voice_hrtf_enabled(void)
{
    return s_hrtf_enabled;
}

bool apu_voice_hrtf_active(void)
{
    return s_vp_hw && s_hrtf_enabled && apu_vp_hrtf_available();
}
