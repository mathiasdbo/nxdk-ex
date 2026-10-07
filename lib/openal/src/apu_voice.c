#include "apu_voice.h"
#include <string.h>

/*
 * ============================================================================
 * Internal State of Voice Processor Subsystem
 * ============================================================================
 */
static uintptr_t s_apu_base = NV_PAPU_BASE;
static NVAPU_VOICE_CONTEXT_3D *s_voice_contexts = NULL;
static uint32_t s_voice_contexts_phys = 0;
static bool s_subsystem_initialized = false;
static uint32_t s_stop_calls = 0;

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

    /* Reset all 64 contexts in the array */
    for (i = 0; i < NV_PAPU_NUM_3D_VOICES; ++i) {
        apu_voice_context_reset(&s_voice_contexts[i]);
    }

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

    s_subsystem_initialized = true;
    return 0;
}

void apu_voice_subsystem_deinit(uintptr_t apu_base)
{
    uintptr_t base = (apu_base != 0) ? apu_base : s_apu_base;

    /* Stop all running voices immediately */
    apu_write32(base, NV_PAPU_VP_ACTIVE_0, 0u);
    apu_write32(base, NV_PAPU_VP_ACTIVE_1, 0u);
    apu_write32(base, NV_PAPU_VP_PAUSE_0, 0u);
    apu_write32(base, NV_PAPU_VP_PAUSE_1, 0u);

    /* Assert reset to silence the Voice Processor core */
    apu_write32(base, NV_PAPU_VP_CONTROL, NV_PAPU_VP_CONTROL_RESET);

    s_voice_contexts = NULL;
    s_voice_contexts_phys = 0u;
    s_subsystem_initialized = false;
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
    if (index >= NV_PAPU_NUM_3D_VOICES || ctx == NULL || s_voice_contexts == NULL) {
        return -1;
    }

    memcpy(&s_voice_contexts[index], ctx, sizeof(NVAPU_VOICE_CONTEXT_3D));
    return 0;
}

NVAPU_VOICE_CONTEXT_3D *apu_voice_get_context(uint32_t index)
{
    if (index >= NV_PAPU_NUM_3D_VOICES || s_voice_contexts == NULL) {
        return NULL;
    }

    return &s_voice_contexts[index];
}

int apu_voice_trigger(uintptr_t apu_base, uint32_t index)
{
    uintptr_t base;

    if (index >= NV_PAPU_NUM_3D_VOICES) {
        return -1;
    }

    base = (apu_base != 0) ? apu_base : s_apu_base;

    /* Update internal context active status if registered */
    if (s_voice_contexts != NULL) {
        s_voice_contexts[index].active = 1u;
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

    if (index >= NV_PAPU_NUM_3D_VOICES) {
        return -1;
    }

    s_stop_calls++;

    base = (apu_base != 0) ? apu_base : s_apu_base;

    /* Clear internal context active status if registered */
    if (s_voice_contexts != NULL) {
        s_voice_contexts[index].active = 0u;
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

    if (index >= NV_PAPU_NUM_3D_VOICES) {
        return -1;
    }

    base = (apu_base != 0) ? apu_base : s_apu_base;

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

    if (index >= NV_PAPU_NUM_3D_VOICES) {
        return 0;
    }

    base = (apu_base != 0) ? apu_base : s_apu_base;

    if (index < 32u) {
        return (apu_read32(base, NV_PAPU_VP_ACTIVE_0) & (1u << index)) ? 1 : 0;
    } else {
        return (apu_read32(base, NV_PAPU_VP_ACTIVE_1) & (1u << (index - 32u))) ? 1 : 0;
    }
}

uint32_t apu_voice_debug_stop_count(void)
{
    return s_stop_calls;
}
