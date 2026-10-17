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
#define AL_XBOX_FREE_VOICES                      0x7007   /* hardware voices not held by a source */
#define AL_XBOX_SAMPLE_PAGES_USED                0x7008   /* 4 KiB pages of the APU's sample space in use */
#define AL_XBOX_SAMPLE_PAGES_TOTAL               0x7009   /* size of that space (2048 pages, 8 MiB) */
#define AL_XBOX_HRTF_ACTIVE                      0x700A   /* mono 3D sources are playing through the HRTF stage */
#define AL_XBOX_REVERB_ACTIVE                    0x700B   /* the GP reverb is running (AL_XBOX_reverb) */
#define AL_XBOX_BUFFER_BYTES                     0x700C   /* library memory held for buffer data (copies, tails) */
#define AL_XBOX_STATIC_BUFFER_BYTES              0x700D   /* application memory the static buffers play */

/*
 * ============================================================================
 * AL_XBOX_hrtf (the APU's HRTF stage)
 * ============================================================================
 *
 * alEnable / alDisable / alIsEnabled(AL_XBOX_HRTF): mono sources positioned in
 * 3D play through the APU's HRTF filter (a computed spherical-head model, see
 * src/apu_hrtf.h) instead of volume panning. On by default; a change applies
 * at each source's next play. Works on a real console and on xemu; sources in
 * direct mode (AL_XBOX_DIRECT_GAINS) and stereo sources never use it. HRTF is
 * meant for headphones; on TV speakers volume panning may sound better.
 */
#define AL_XBOX_HRTF                             0x7015

/*
 * ============================================================================
 * AL_XBOX_adpcm (Xbox ADPCM buffers, decoded by the APU)
 * ============================================================================
 *
 * alBufferData / alBufferDataStatic with these formats take Xbox ADPCM: IMA
 * ADPCM in 36-byte blocks per channel, each holding 64 samples (a 4-byte header
 * with the first sample and step index, then 32 bytes of 4-bit codes; the 64th
 * code is not played, the next block's header carries that sample). Stereo
 * interleaves the two headers, then 4-byte chunks left, right, left, ...
 * (the layout of .xwav / WAVE_FORMAT_XBOX_ADPCM, 0x0069). The size must be a
 * whole number of blocks. The APU decodes it at no CPU cost.
 *   - frames: 64 per block; AL_SAMPLE_OFFSET and loop points in frames;
 *     AL_BYTE_OFFSET reads and sets the start of a block; AL_BITS reports 4
 *   - static buffers and streaming queues work; a queue moves whole blocks
 *     (seeks and restarts land on a block start)
 */
#define AL_FORMAT_MONO_XBOX_ADPCM                0x7030
#define AL_FORMAT_STEREO_XBOX_ADPCM              0x7031

/*
 * ============================================================================
 * AL_EXT_STATIC_BUFFER (same entry point as Apple's and OpenAL Soft's)
 * ============================================================================
 *
 * alBufferDataStatic(buffer, format, data, size, freq): like alBufferData, but
 * the buffer plays `data` where it is instead of copying it. The memory:
 *   - is ordinary RAM (malloc, a sound pool, part of a larger file in memory);
 *     no alignment or physical contiguity needed, any byte offset works, so a
 *     buffer can point into the middle of a sound (a word's start and end);
 *   - must stay allocated, unmoved and unchanged until the buffer is deleted or
 *     given other data (AL_INVALID_OPERATION protects buffers in use as usual).
 *     After changing the bytes, call alBufferDataStatic again.
 * The library copies only the last partial 4 KiB page (it needs a silent tail
 * behind the data) and writes the CPU cache back to RAM so the APU reads the
 * application's latest data. alXboxGetHardwareStatus(AL_XBOX_BUFFER_BYTES)
 * reports the library's own buffer memory, AL_XBOX_STATIC_BUFFER_BYTES the
 * application memory referenced.
 */
AL_API void AL_APIENTRY alBufferDataStatic(ALuint buffer, ALenum format, const ALvoid *data, ALsizei size,
                                           ALsizei freq);
typedef void (AL_APIENTRY *LPALBUFFERDATASTATIC)(ALuint buffer, ALenum format, const ALvoid *data, ALsizei size,
                                                 ALsizei freq);

/*
 * ============================================================================
 * AL_XBOX_reverb (a reverb on the APU's GP DSP)
 * ============================================================================
 *
 * alEnable / alDisable / alIsEnabled(AL_XBOX_REVERB): one room reverb for the
 * whole mix, computed by the APU's GP DSP (no CPU time). Off by default. Each
 * source sends part of its sound to it:
 *   alSourcef(src, AL_XBOX_REVERB_SEND, 0..1)   default 0 (dry only)
 * The send follows the source's gain (AL_GAIN, distance, direct gains), is
 * taken before panning and HRTF, and works for every source kind.
 *
 * The room: alXboxReverbPreset(AL_XBOX_REVERB_PRESET_*), then optionally
 * alXboxReverbf(param, value) with
 *   AL_XBOX_REVERB_DECAY_TIME     0.1 .. 20 s     time to fall by 60 dB
 *   AL_XBOX_REVERB_HF_RATIO       0.1 .. 1        high-frequency decay (1: as bright as the low end)
 *   AL_XBOX_REVERB_GAIN           0 .. 1          reverb level in the output
 *   AL_XBOX_REVERB_PRE_DELAY      0 .. 0.1 s      gap before the reverb starts (at least 0.67 ms)
 *   AL_XBOX_REVERB_DIFFUSION      0 .. 1          echo density of the tail
 *   AL_XBOX_REVERB_INPUT_GAIN     0 .. 1          headroom of the reverb's input
 *   AL_XBOX_REVERB_INPUT_LOWPASS  0 .. 0.95       muffles what goes into the reverb
 *   AL_XBOX_REVERB_OUTPUT_LOWPASS 0 .. 0.98       muffles the whole mix, dry sound included
 *                                                 (underwater; 0 leaves the mix untouched)
 * Gains change at once; a new pre-delay restarts the reverb (its tail is cut).
 * alXboxGetReverbf reads a value back. Out-of-range values: AL_INVALID_VALUE.
 * Runs on a real console (AL_XBOX_REVERB_ACTIVE says when); the settings are
 * kept when no device is open and applied when one is.
 */
#define AL_XBOX_REVERB                           0x7016
#define AL_XBOX_REVERB_SEND                      0x7017

#define AL_XBOX_REVERB_DECAY_TIME                0x7020
#define AL_XBOX_REVERB_HF_RATIO                  0x7021
#define AL_XBOX_REVERB_GAIN                      0x7022
#define AL_XBOX_REVERB_PRE_DELAY                 0x7023
#define AL_XBOX_REVERB_DIFFUSION                 0x7024
#define AL_XBOX_REVERB_INPUT_GAIN                0x7025
#define AL_XBOX_REVERB_INPUT_LOWPASS             0x7026
#define AL_XBOX_REVERB_OUTPUT_LOWPASS            0x7027

#define AL_XBOX_REVERB_PRESET_GENERIC            0
#define AL_XBOX_REVERB_PRESET_ROOM               1
#define AL_XBOX_REVERB_PRESET_BATHROOM           2
#define AL_XBOX_REVERB_PRESET_HALL               3
#define AL_XBOX_REVERB_PRESET_CAVE               4
#define AL_XBOX_REVERB_PRESET_ARENA              5
#define AL_XBOX_REVERB_PRESET_SEWER_PIPE         6
#define AL_XBOX_REVERB_PRESET_UNDERWATER         7
#define AL_XBOX_REVERB_PRESET_COUNT              8

AL_API void AL_APIENTRY alXboxReverbPreset(ALint preset);
AL_API void AL_APIENTRY alXboxReverbf(ALenum param, ALfloat value);
AL_API void AL_APIENTRY alXboxGetReverbf(ALenum param, ALfloat *value);
typedef void (AL_APIENTRY *LPALXBOXREVERBPRESET)(ALint preset);
typedef void (AL_APIENTRY *LPALXBOXREVERBF)(ALenum param, ALfloat value);
typedef void (AL_APIENTRY *LPALXBOXGETREVERBF)(ALenum param, ALfloat *value);

/*
 * ============================================================================
 * AL_SOFT_loop_points (same token and semantics as OpenAL Soft)
 * ============================================================================
 *
 * alBufferiv(buffer, AL_LOOP_POINTS_SOFT, {start, end}): sample frames, with
 * 0 <= start < end <= the buffer's frames; AL_INVALID_VALUE otherwise, and
 * AL_INVALID_OPERATION while a source uses the buffer. A looping source plays
 * the buffer from its start (or its offset) and then repeats [start, end): a
 * game's "intro, then loop" sound (Half-Life's cue-point WAVs) is one buffer.
 * alBufferData resets the points to the whole buffer. On the APU the loop is
 * the voice's own (LBO/EBO), so it costs nothing per frame.
 */
#define AL_LOOP_POINTS_SOFT                      0x2015

/*
 * ============================================================================
 * AL_XBOX_source_control (engine control of hardware voices)
 * ============================================================================
 *
 * AL_XBOX_DIRECT_GAINS   alSourcefv(src, ..., {left, right}): linear gains
 *                        (0..1) of the left and right outputs, set by the
 *                        engine; turns direct mode on. In direct mode the
 *                        library's 3D model (pan, distance, cone, Doppler,
 *                        HRTF) is bypassed: the output gains are left/right
 *                        times AL_GAIN times the listener gain, the pitch is
 *                        AL_PITCH. For engines that spatialize themselves.
 * AL_XBOX_DIRECT_MODE    alSourcei/alGetSourcei: AL_TRUE / AL_FALSE (default).
 * AL_XBOX_PRIORITY       alSourcef: the engine's priority (>= 0) for voice
 *                        allocation and stealing, instead of the library's
 *                        gain/distance score; < 0 (default -1) uses the library's.
 * AL_XBOX_VIRTUALIZE     alSourcei: AL_TRUE (default) keeps a source that gets
 *                        no hardware voice (or loses it) playing in virtual
 *                        standby; AL_FALSE stops it instead (AL_STOPPED), so
 *                        the engine knows and can mix it itself.
 * AL_XBOX_HAS_VOICE      alGetSourcei: AL_TRUE while the source holds a hardware voice.
 */
#define AL_XBOX_DIRECT_GAINS                     0x7010
#define AL_XBOX_DIRECT_MODE                      0x7011
#define AL_XBOX_PRIORITY                         0x7012
#define AL_XBOX_VIRTUALIZE                       0x7013
#define AL_XBOX_HAS_VOICE                        0x7014

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

/*
 * AL_XBOX_pool_size: the size in bytes of the physically contiguous APU memory
 * pool the next alcOpenDevice allocates (rounded up to 4 KiB; 0 or less: the
 * default, 2 MiB). The pool holds the voice table, the ITD buffers and the APU
 * copies of buffers voices play directly (a source's AL_BUFFER); buffers that
 * are only queued on streaming sources stay in CPU memory and never use it. An
 * allocation the pool cannot satisfy falls back to its own contiguous block, so
 * a small pool costs speed, not correctness. Call it before alcOpenDevice.
 */
AL_API void AL_APIENTRY alXboxSetPoolSize(ALsizei bytes);
typedef void (AL_APIENTRY *LPALXBOXSETPOOLSIZE)(ALsizei bytes);

/*
 * ============================================================================
 * AL_XBOX_stats (CPU time of the library's audio work)
 * ============================================================================
 *
 * alXboxGetStats(&s) fills the structure below; set s.size = sizeof(s) first
 * (fields past it are left alone). The counters run from alcOpenDevice and
 * never reset: take the difference between two calls (one per game frame).
 * Cycles are the CPU's time-stamp counter, cycle_hz of them per second
 * (733 MHz on a retail console; 0 until the device has run 10 ms).
 *
 *   output_*   the output thread: every 4 ms it moves the APU's mix to the
 *              AC97 codec and scrubs played stream data; output_hook_cycles is
 *              the part spent in the AL_XBOX_output_callback function
 *   update_*   alXboxUpdateVoices / alcProcessContext (the per-frame voice
 *              tick; it includes the output work when the thread is off)
 *   ac97_*     buffers queued on the codec (10.7 ms each), restarts after it
 *              ran dry, and buffers completed with silence because the mix
 *              was late
 */
typedef struct ALXBOXSTATS {
    ALuint size;                         /* sizeof(ALXBOXSTATS), set by the caller */
    ALuint cycle_hz;
    ALuint output_passes;
    ALuint output_max_gap_ms;            /* longest time between two passes */
    unsigned long long output_cycles;
    ALuint output_max_cycles;            /* longest pass */
    unsigned long long output_hook_cycles;
    ALuint update_calls;
    unsigned long long update_cycles;
    ALuint update_max_cycles;
    ALuint ac97_buffers;
    ALuint ac97_underruns;
    ALuint ac97_padded;
    ALuint output_threaded;              /* AL_TRUE while the output thread runs */
    /* output_cycles split (the rest: the codec's registers, the pass itself) */
    unsigned long long output_fifo_cycles;    /* reading the APU's mix */
    unsigned long long output_queue_cycles;   /* copying it to the codec's buffers */
    unsigned long long output_scrub_cycles;   /* silencing played stream data */
    /* update_cycles split (the rest: the reverb check, the call itself) */
    unsigned long long update_stream_cycles;  /* bringing streams up to date */
    unsigned long long update_voice_cycles;   /* the voice manager and the APU service */
    unsigned long long update_service_cycles; /* of it, the APU service (apu_vp_service) */
} ALXBOXSTATS;

AL_API void AL_APIENTRY alXboxGetStats(ALXBOXSTATS *stats);
typedef void (AL_APIENTRY *LPALXBOXGETSTATS)(ALXBOXSTATS *stats);

/*
 * ============================================================================
 * AL_XBOX_output_callback (process the final mix on the CPU)
 * ============================================================================
 *
 * alXboxOutputCallback(fn, user): fn(samples, frames, user) receives every
 * block of the final mix (frames = 512 interleaved 16-bit stereo frames at
 * 48 kHz, 10.7 ms) just before it goes to the AC97 codec, and may change the
 * samples in place (a CPU effect, a recorder, a meter). It runs in the output
 * thread at high priority: keep it well under a millisecond and do not call
 * OpenAL from it. Its time shows in ALXBOXSTATS.output_hook_cycles.
 * fn = NULL removes it. Real console only (nothing is output on the host).
 */
typedef void (AL_APIENTRY *ALXBOXOUTPUTPROC)(ALshort *samples, ALsizei frames, ALvoid *user);
AL_API void AL_APIENTRY alXboxOutputCallback(ALXBOXOUTPUTPROC fn, ALvoid *user);
typedef void (AL_APIENTRY *LPALXBOXOUTPUTCALLBACK)(ALXBOXOUTPUTPROC fn, ALvoid *user);

#if defined(__cplusplus)
}
#endif

#endif /* ALEXT_H */
