#ifndef APU_VOICE_MGR_H
#define APU_VOICE_MGR_H

#include <stdint.h>
#include <stdbool.h>
#include "al_source.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ============================================================================
 * Xbox MCPX APU Voice Virtualization & Priority Manager
 * ============================================================================
 * Manages dynamic allocation and preemption of the 64 hardware Voice Processor
 * channels across up to 256 virtual OpenAL sources.
 *
 * Priority Formula:
 *   P = priority_base * (effective_gain / max(distance, 0.1f))
 * where:
 *   priority_base: 3.0f for looping sources, 1.0f for one-shot sources.
 *   effective_gain: src->gain * distance_gain * cone_gain * listener_gain.
 *   If gain or effective_gain < 0.001f, priority is strictly 0.0f.
 *
 * Preemption is mute-then-halt in a single call (no ramp; a real fade needs
 * several frame ticks and is not implemented). See lib/openal/docs/XEMU_VERIFICATION.md.
 * ============================================================================
 */

/**
 * Initialize the voice manager table and reset voice owner bindings.
 */
void apu_voice_mgr_init(void);

/**
 * Shut down the voice manager and release all voice owner bindings.
 */
void apu_voice_mgr_deinit(void);

/**
 * Allocate a hardware voice slot (0..63) for a given source object.
 *
 * Pass 1: Searches for an unused slot or completed one-shot voice.
 * Pass 2: If all 64 slots are busy, performs priority stealing against the
 *         victim with lowest priority (oldest ALsource.play_seq on ties) if
 *         P_src >= P_victim and P_src > 0 (ties favour the newcomer; a silent
 *         source never evicts).
 *
 * @param src Source requesting a hardware voice slot.
 * @return Assigned hardware voice index (0..63) or AL_HW_VOICE_INVALID (-1)
 *         if deferred to virtual standby.
 */
int apu_voice_mgr_allocate(ALsource *src);

/**
 * Release a hardware voice slot bound to a source object.
 * Zeroes the shadow-context master volume/mixbin gains, then halts the voice
 * in the same call (mute-then-halt, no ramp), and clears owner.
 *
 * @param src Source releasing its hardware voice slot.
 */
void apu_voice_mgr_release(ALsource *src);

/**
 * Frame update tick: reaps completed one-shot hardware voices and promotes
 * highest-priority playing virtual standby sources to newly available slots
 * (priorities computed once per tick; ties go to the lowest source id).
 *
 * @param apu_base APU MMIO base address (0 to use configured default).
 */
void apu_voice_mgr_update(uintptr_t apu_base);

/**
 * Get count of active hardware voices currently bound to playing/paused sources.
 *
 * @return Number of hardware voice channels in use (0..64).
 */
uint32_t apu_voice_mgr_get_active_hw_count(void);

/**
 * Get count of virtual sources currently playing in standby (without a hardware voice).
 *
 * @return Number of virtual sources in standby.
 */
uint32_t apu_voice_mgr_get_virtual_standby_count(void);

/**
 * Calculate dynamic preemption priority for a given source.
 *
 * @param src Source to calculate priority for.
 * @return Floating-point priority score (>= 0.0f).
 */
float apu_voice_mgr_calc_priority(const ALsource *src);

/**
 * Retrieve the source ID currently owning a hardware voice slot.
 *
 * @param hw_voice_idx Hardware voice index (0..63).
 * @return Source ID (1..256) owning the slot, or AL_HW_VOICE_INVALID if unassigned.
 */
int apu_voice_mgr_get_owner(uint32_t hw_voice_idx);

#ifdef __cplusplus
}
#endif

#endif /* APU_VOICE_MGR_H */
