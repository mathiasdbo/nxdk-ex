#ifndef ALEXT_H
#define ALEXT_H

#include <AL/al.h>
#include <AL/alc.h>

#if defined(__cplusplus)
extern "C" {
#endif

/*
 * ============================================================================
 * AL_EXT_MCFORMATS (Multichannel Audio Buffer Formats)
 * ============================================================================
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

/* Xbox AV Pack Encodings for AL_XBOX_AV_PACK_TYPE Query */
#define AL_XBOX_AV_PACK_SCART                    0x0000
#define AL_XBOX_AV_PACK_HDTV                     0x0001
#define AL_XBOX_AV_PACK_VGA                      0x0002
#define AL_XBOX_AV_PACK_RFU                      0x0003
#define AL_XBOX_AV_PACK_SVIDEO                   0x0004
#define AL_XBOX_AV_PACK_COMPOSITE                0x0005
#define AL_XBOX_AV_PACK_NONE                     0x0007

/*
 * Query low-level Xbox APU hardware and topological status.
 *
 * Adheres to OpenAL 1.1 error semantics:
 * - If param is invalid, sets AL_INVALID_ENUM.
 * - If value is NULL, sets AL_INVALID_VALUE.
 *
 * @param param Hardware query token (e.g. AL_XBOX_HW_VOICE_COUNT, AL_XBOX_AV_PACK_TYPE).
 * @param value Pointer to ALint where the queried integer value will be stored.
 */
AL_API void AL_APIENTRY alXboxGetHardwareStatus(ALenum param, ALint *value);

/* Function Pointer Type Definition */
typedef void (AL_APIENTRY *LPALXBOXGETHARDWARESTATUS)(ALenum param, ALint *value);

#if defined(__cplusplus)
}
#endif

#endif /* ALEXT_H */
