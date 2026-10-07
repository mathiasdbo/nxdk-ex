#ifndef ALEXT_H
#define ALEXT_H

#include <AL/al.h>
#include <AL/alc.h>

#if defined(__cplusplus)
extern "C" {
#endif

/*
 * ============================================================================
 * AL_EXT_MCFORMATS (Multichannel Audio Buffer Formats) - RESERVED
 * ============================================================================
 * The tokens below are reserved for a future multichannel implementation.
 * alBufferData() does NOT accept them yet (AL_INVALID_ENUM), so the library
 * does not advertise AL_EXT_MCFORMATS in alGetString(AL_EXTENSIONS) and
 * alIsExtensionPresent("AL_EXT_MCFORMATS") returns AL_FALSE.
 */
#define AL_FORMAT_QUAD8                          0x1204
#define AL_FORMAT_QUAD16                         0x1205
#define AL_FORMAT_51CHN8                         0x120A
#define AL_FORMAT_51CHN16                        0x120B

/*
 * ============================================================================
 * AL_XBOX_hardware_status (Xbox MCPX APU Hardware Query Extension)
 * ============================================================================
 */
#define AL_XBOX_HW_VOICE_COUNT                   0x7001
#define AL_XBOX_AV_PACK_TYPE                     0x7002
#define AL_XBOX_DOLBY_DIGITAL_ACTIVE             0x7003
#define AL_XBOX_VP_BASE_PHYS                     0x7004
#define AL_XBOX_LFE_GAIN                         0x7005
#define AL_XBOX_BACKEND                          0x7006

/*
 * Xbox AV Pack Encodings for AL_XBOX_AV_PACK_TYPE Query
 *
 * Library-private token values. They are chosen to equal the raw SMC
 * AV-pack register codes (SMBus slave 0x20 in the 8-bit form HalReadSMBusValue
 * takes, i.e. xemu's 7-bit 0x10 at hw/xbox/xbox.c:300; register 0x04) used by
 * xemu (hw/xbox/smbus_xbox_smc.c: SCART 0, HDTV 1, VGA 2, RFU 3, SVIDEO 4,
 * COMPOSITE 6, NONE 7). A raw code the SMC does not define is reported as
 * AL_XBOX_AV_PACK_NONE; if the SMBus read itself fails the library assumes a
 * standard pack and reports AL_XBOX_AV_PACK_COMPOSITE. See
 * lib/openal/docs/XEMU_VERIFICATION.md.
 */
#define AL_XBOX_AV_PACK_SCART                    0x0000
#define AL_XBOX_AV_PACK_HDTV                     0x0001
#define AL_XBOX_AV_PACK_VGA                      0x0002
#define AL_XBOX_AV_PACK_RFU                      0x0003
#define AL_XBOX_AV_PACK_SVIDEO                   0x0004
#define AL_XBOX_AV_PACK_COMPOSITE                0x0006
#define AL_XBOX_AV_PACK_NONE                     0x0007

/*
 * Backend identifiers for the AL_XBOX_BACKEND query.
 *
 * AL_XBOX_BACKEND_NULL: the device runs on the software model (a zeroed,
 *   BAR0-sized RAM region, or a test/mock MMIO base). No real APU register
 *   is touched. This is the DEFAULT. It produces NO SOUND, and voices only
 *   complete if something clears their ACTIVE bit (a test harness does; the
 *   RAM stand-in never does), so alXboxUpdateVoices() is effectively inert on it.
 * AL_XBOX_BACKEND_MMIO: the device base equals the real BAR0 address
 *   (NV_PAPU_BASE, 0xFE800000) and the library drives the hardware directly.
 *   Only available when the library is built with -DOPENAL_APU_REAL_MMIO.
 *
 * The real-MMIO path is opt-in because the register model is not
 * xemu-conformant (see lib/openal/docs/XEMU_VERIFICATION.md): on xemu the device
 * bring-up can disturb or abort the emulator. alGetString(AL_RENDERER)
 * also names the active backend. The backend belongs to an open device:
 * when no device is open the query reports AL_XBOX_BACKEND_NULL in every
 * build, including -DOPENAL_APU_REAL_MMIO.
 */
#define AL_XBOX_BACKEND_NULL                     0
#define AL_XBOX_BACKEND_MMIO                     1

/*
 * Query low-level Xbox APU hardware and topological status.
 *
 * Adheres to OpenAL 1.1 error semantics:
 * - If param is invalid, sets AL_INVALID_ENUM.
 * - If value is NULL, sets AL_INVALID_VALUE.
 *
 * AL_XBOX_VP_BASE_PHYS and AL_XBOX_DOLBY_DIGITAL_ACTIVE are answered from
 * cached software state and return 0 when no device is open; no APU
 * register is read.
 *
 * @param param Hardware query token (e.g. AL_XBOX_HW_VOICE_COUNT, AL_XBOX_AV_PACK_TYPE).
 * @param value Pointer to ALint where the queried integer value will be stored.
 */
AL_API void AL_APIENTRY alXboxGetHardwareStatus(ALenum param, ALint *value);

/* Function Pointer Type Definition */
typedef void (AL_APIENTRY *LPALXBOXGETHARDWARESTATUS)(ALenum param, ALint *value);

/*
 * ============================================================================
 * AL_XBOX_update (Voice Frame Tick)
 * ============================================================================
 */

/*
 * Run one voice-manager frame tick. Applications should call this once per
 * frame (alcProcessContext() on a valid context performs the same tick).
 *
 * The tick reaps finished one-shot voices (their sources become AL_STOPPED)
 * and promotes virtual-standby sources into freed hardware voice slots.
 * Looping sources are never reaped. It does nothing when no device is open.
 */
AL_API void AL_APIENTRY alXboxUpdateVoices(void);

/* Function Pointer Type Definition */
typedef void (AL_APIENTRY *LPALXBOXUPDATEVOICES)(void);

#if defined(__cplusplus)
}
#endif

#endif /* ALEXT_H */
