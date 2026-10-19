# OpenAL for nxdk: API reference

Use this page to look up a call. It lists every function and token the library provides, with values,
defaults, errors and limits. For how to put the calls together in a game,
[`ENGINE_INTEGRATION.md`](ENGINE_INTEGRATION.md) has worked recipes; section numbers like "3.12" below point
there.

The library is OpenAL 1.1, the Effects Extension (`ALC_EXT_EFX` 1.0) and a set of Xbox extensions. Code written
for OpenAL Soft or Creative's OpenAL builds unchanged as long as it stays inside what is listed here.

## 1. Headers and linking

```c
#include <AL/al.h>      /* OpenAL 1.1 */
#include <AL/alc.h>     /* devices and contexts */
#include <AL/alext.h>   /* the Xbox extensions, AL_EXT_STATIC_BUFFER, AL_SOFT_loop_points */
#include <AL/efx.h>     /* ALC_EXT_EFX 1.0 */
```

- **Prototypes:**
  - The functions in `al.h`, `alc.h` and `alext.h` are declared directly.
  - The EFX functions in `efx.h` are declared only if `AL_ALEXT_PROTOTYPES` is defined before the include.
  - Without it, fetch them with `alGetProcAddress`, as portable code does. Every entry point, Xbox ones
    included, is returned by `alGetProcAddress`.
- **Getting sound:** compile the library sources into your program with `-DOPENAL_APU_REAL_MMIO`. One wrapper
  file per source is enough; see the README's *Building for the real APU* and `samples/openal_engine`.
- **Null backend:** `libopenal.lib`, built by nxdk's `lib/Makefile`. It produces no sound and is what the host
  tests use.
- **Threads:** call the library from one thread. Its own output thread never runs your code, except the
  output callback (8.4).

## 2. Errors

As in OpenAL 1.1: `alGetError()` returns the first error since the last call and clears it, and
`alcGetError(device)` does the same for ALC calls. A call that fails changes nothing. The codes:

| Error | Raised for |
|---|---|
| `AL_INVALID_NAME` | a buffer, source, effect, filter or slot that does not exist |
| `AL_INVALID_ENUM` | a parameter the object does not have |
| `AL_INVALID_VALUE` | a value out of range, a NULL pointer, an unsupported effect or filter type, a bad GP image |
| `AL_INVALID_OPERATION` | no current context; deleting something in use; a second effect while one runs |
| `AL_OUT_OF_MEMORY` | no free objects, or memory for buffer data |

## 3. Devices and contexts (ALC)

| Call | Notes |
|---|---|
| `alcOpenDevice(name)` | `NULL`, the default name, `"MCPX APU"`, `"DirectSound3D"` or `"DirectSound"`; other names fail. The first open brings the APU up and starts the AC97 output; further opens share it |
| `alcCloseDevice(dev)` | the last close stops the voices, the GP and the output |
| `alcCreateContext(dev, attrs)` | `ALC_MAX_AUXILIARY_SENDS` requests the sends per source (0..2, larger is clamped; default 2). `ALC_FREQUENCY`, `ALC_REFRESH`, `ALC_SYNC`, `ALC_MONO_SOURCES` and `ALC_STEREO_SOURCES` are stored and do not change the output (always 48 kHz) |
| `alcMakeContextCurrent`, `alcGetCurrentContext`, `alcGetContextsDevice`, `alcDestroyContext` | standard |
| `alcProcessContext(ctx)` | the per-frame tick, the same as `alXboxUpdateVoices` (3.1 of this page) |
| `alcSuspendContext` | does nothing |
| `alcGetString`, `alcGetIntegerv`, `alcGetError`, `alcIsExtensionPresent`, `alcGetProcAddress`, `alcGetEnumValue` | `alcGetIntegerv` answers the 1.1 queries plus `ALC_EFX_MAJOR_VERSION` (1), `ALC_EFX_MINOR_VERSION` (0) and `ALC_MAX_AUXILIARY_SENDS` |
| capture (`alcCaptureOpenDevice` ...) | stubs: the device cannot be opened |

### 3.1 The frame tick

```c
void alXboxUpdateVoices(void);   /* AL_XBOX_update; alcProcessContext does the same */
```

Call it once per game frame. It:
- marks one-shots that have finished as `AL_STOPPED`, at most one frame late;
- refills streaming sources;
- gives free hardware voices to sources waiting in virtual standby.

Sound keeps playing without it, because the output thread runs every 4 ms. Calling it more often than every
12 ms gains nothing.

### 3.2 Pool size

```c
void alXboxSetPoolSize(ALsizei bytes);   /* AL_XBOX_pool_size, before alcOpenDevice */
```

Sets the size of the contiguous APU memory pool the next `alcOpenDevice` allocates. It is rounded up to 4 KiB,
and 0 or less means the default of 2 MiB. The pool holds the voice table, the ITD buffers, and the APU copies
of buffers attached with `AL_BUFFER`. A request that does not fit gets its own block, so a small pool only
costs speed.

## 4. Buffers

| Call | Notes |
|---|---|
| `alGenBuffers` / `alDeleteBuffers` / `alIsBuffer` | 2048 buffers (`-DAL_MAX_BUFFERS=<n>` builds fewer). Deleting a buffer that is attached or queued: `AL_INVALID_OPERATION` |
| `alBufferData(b, format, data, size, freq)` | copies the data. Any rate. Formats in the table below |
| `alBufferDataStatic(b, format, data, size, freq)` | `AL_EXT_STATIC_BUFFER`: plays your memory in place (ENGINE_INTEGRATION 3.10) |
| `alBufferiv(b, AL_LOOP_POINTS_SOFT, {start, end})` | `AL_SOFT_loop_points`, in sample frames, 0 ≤ start < end ≤ frames. A looping source plays from the start, then repeats `[start, end)` in hardware (3.3). `alBufferData` resets them |
| `alGetBufferi` / `alGetBufferiv` | `AL_FREQUENCY`, `AL_BITS`, `AL_CHANNELS`, `AL_SIZE`, `AL_LOOP_POINTS_SOFT` |

**Formats:**

| Format | Data |
|---|---|
| `AL_FORMAT_MONO8`, `AL_FORMAT_STEREO8` | 8-bit unsigned PCM |
| `AL_FORMAT_MONO16`, `AL_FORMAT_STEREO16` | 16-bit signed PCM |
| `AL_FORMAT_MONO_XBOX_ADPCM`, `AL_FORMAT_STEREO_XBOX_ADPCM` | `AL_XBOX_adpcm`: Xbox IMA ADPCM, 36-byte blocks of 64 frames per channel, the size a whole number of blocks; decoded by the APU (3.11). `AL_BITS` reports 4 |
| `AL_FORMAT_QUAD*`, `AL_FORMAT_51CHN*` | reserved, `AL_INVALID_ENUM` |

**Static buffers:**
- `data` can be any ordinary RAM, at any byte offset, including the middle of a larger sound.
- It must stay allocated and unchanged until the buffer is deleted or given other data. After changing the
  bytes, call `alBufferDataStatic` again.
- The library copies only the last partial 4 KiB page.

## 5. Sources

`alGenSources` / `alDeleteSources` / `alIsSource`. There are 256 sources; 120 can hold hardware voices at once
on a console (64 on xemu). The others wait in virtual standby, or stop if `AL_XBOX_VIRTUALIZE` is off.

**OpenAL 1.1 attributes**, set with `alSourcef/3f/fv/i/3i/iv` and read with the `alGetSource*` forms:
- `AL_POSITION`, `AL_VELOCITY`, `AL_DIRECTION`, `AL_SOURCE_RELATIVE`;
- `AL_GAIN`, `AL_MIN_GAIN`, `AL_MAX_GAIN`, `AL_PITCH`, `AL_LOOPING`;
- `AL_CONE_INNER_ANGLE`, `AL_CONE_OUTER_ANGLE`, `AL_CONE_OUTER_GAIN`;
- `AL_REFERENCE_DISTANCE`, `AL_MAX_DISTANCE`, `AL_ROLLOFF_FACTOR`;
- `AL_BUFFER`, `AL_SOURCE_STATE`, `AL_SOURCE_TYPE`, `AL_BUFFERS_QUEUED`, `AL_BUFFERS_PROCESSED`;
- the offsets `AL_SEC_OFFSET`, `AL_SAMPLE_OFFSET`, `AL_BYTE_OFFSET` (`AL_EXT_OFFSET`), readable and settable
  while playing.

**Calls:**
- **Playback:** `alSourcePlay`, `alSourcePause`, `alSourceStop`, `alSourceRewind` and their `v` forms.
  `alSourcePlayv` starts all its sources on the same 0.67 ms APU frame (their voices join the VP's list with
  one register write). `alSourcePausev` / `alSourceStopv` act within microseconds of each other: on the same
  frame unless one falls on a frame boundary.
- **Streaming:** `alSourceQueueBuffers` / `alSourceUnqueueBuffers`. Up to 32 streaming sources with 64 queued
  buffers each.

**Pitch:** the playback rate (`AL_PITCH` × the buffer's rate) is clamped to 1/256 .. 3.8 × 48 kHz.

### 5.1 Xbox source attributes (`AL_XBOX_source_control`, `AL_XBOX_reverb`)

| Attribute | Call | Values | Meaning |
|---|---|---|---|
| `AL_XBOX_DIRECT_GAINS` | `alSourcefv` | `{left, right}`, 0..1 | turns direct mode on: your own left/right gains × `AL_GAIN` × the listener gain; no 3D model, no HRTF (3.1, 3.2) |
| `AL_XBOX_DIRECT_MODE` | `alSourcei` / get | `AL_TRUE` / `AL_FALSE` (default) | direct mode on or off |
| `AL_XBOX_PRIORITY` | `alSourcef` | ≥ 0, or −1 (default) | your priority for voice allocation and stealing; −1 uses the library's score from gain and distance |
| `AL_XBOX_VIRTUALIZE` | `alSourcei` | `AL_TRUE` (default) / `AL_FALSE` | without a voice: wait in standby, or stop (`AL_STOPPED`) so you can mix it yourself |
| `AL_XBOX_HAS_VOICE` | `alGetSourcei` | `AL_TRUE` / `AL_FALSE` | whether the source holds a hardware voice now |
| `AL_XBOX_SEND_PANNED` | `alSourcei` / get | `AL_TRUE` / `AL_FALSE` (default) | in direct mode, the sends into a custom GP kernel follow the direct gains: left input = send × left, right input = send × right (mono: the sample on both; stereo: each channel on its own); the direct path's gain and filter do not touch them, so a source muted by a lowpass at gain 0 is heard only through the kernel (ENGINE_INTEGRATION 3.13) |
| `AL_XBOX_REVERB_SEND` | `alSourcef` | 0..1, default 0 | how much goes to the `AL_XBOX_reverb` room (7.1) |

### 5.2 EFX source attributes (`AL/efx.h`)

| Attribute | Call | Effect here |
|---|---|---|
| `AL_DIRECT_FILTER` | `alSourcei(src, ..., filter)` | takes a copy of the filter; 0 or `AL_FILTER_NULL` removes it (6.2) |
| `AL_AUXILIARY_SEND_FILTER` | `alSource3i(src, ..., slot, send, filter)` | send `send` (0..`ALC_MAX_AUXILIARY_SENDS`−1) to `slot` (0: none) through a copy of `filter` |
| `AL_ROOM_ROLLOFF_FACTOR` | `alSourcef`, 0..10, default 0 | distance attenuation of the sends |
| `AL_AUXILIARY_SEND_FILTER_GAIN_AUTO` | `alSourcei`, default `AL_TRUE` | the sends follow distance and cone |
| `AL_AIR_ABSORPTION_FACTOR`, `AL_CONE_OUTER_GAINHF`, `AL_DIRECT_FILTER_GAINHF_AUTO`, `AL_AUXILIARY_SEND_FILTER_GAINHF_AUTO` | standard ranges | stored and read back, not applied to the sound |

## 6. Effects (`ALC_EXT_EFX` 1.0)

Recipes: ENGINE_INTEGRATION 3.12 (EFX) and 3.13 (your own GP kernel). Every EFX function exists:
`alGen/Delete/IsEffects`, `alEffect{i,iv,f,fv}` and the gets, the same for `Filter` and `AuxiliaryEffectSlot`.

### 6.1 Effects

`alEffecti(fx, AL_EFFECT_TYPE, type)` with:

| Type | Runs as |
|---|---|
| `AL_EFFECT_NULL` | nothing (the default) |
| `AL_EFFECT_REVERB`, `AL_EFFECT_EAXREVERB` | the GP reverb. Every parameter is stored with the extension's ranges and defaults; which ones change the sound is in the table in 3.12 |
| `AL_EFFECT_XBOX_GP_PROGRAM` | your DSP56300 kernel (6.4) |
| any other | `AL_INVALID_VALUE` |

Up to 256 effects. Changing the type resets the parameters to that type's defaults.

### 6.2 Filters

| Type | Parameters |
|---|---|
| `AL_FILTER_NULL` | none |
| `AL_FILTER_LOWPASS` | `AL_LOWPASS_GAIN` 0..1 (default 1): scales the sound or the send. `AL_LOWPASS_GAINHF` 0..1 (default 1): the level at 5 kHz, on the voice's hardware lowpass |
| `AL_FILTER_HIGHPASS`, `AL_FILTER_BANDPASS` | `AL_INVALID_VALUE` (the hardware has no highpass) |

Up to 256 filters. Sources hold copies, so attach the filter again after changing it.

**How the lowpass behaves:**
- It can change while the sound plays, without a click.
- It acts the same on plain, HRTF and stereo voices.
- Above the cutoff the sound falls about 12 dB per octave.
- The hardware filter is one per voice: a send filter's `GAINHF` is not applied, only its `GAIN`.
- Console check: the 5 kHz level is within 0.2 dB of 20·log10(`GAINHF`) for 0.9 .. 0.03.

### 6.3 Auxiliary effect slots

| Parameter | Values | Meaning |
|---|---|---|
| `AL_EFFECTSLOT_EFFECT` | an effect, or 0 | takes a copy of the effect and starts it on the GP. Attach again after changing the effect |
| `AL_EFFECTSLOT_GAIN` | 0..1, default 1 | the effect's level (reverb: its output; GP program: the sends into it) |
| `AL_EFFECTSLOT_AUXILIARY_SEND_AUTO` | `AL_TRUE` (default) / `AL_FALSE` | the reverb's own room rolloff added to the sends |

**Limits:**
- There are 4 slots, and the GP runs one effect at a time. Attaching an effect to a second slot while one runs
  gives `AL_INVALID_OPERATION`. `AL_XBOX_REVERB` counts as that effect too.
- Empty slots are legal and silent.
- Deleting a slot a source sends to gives `AL_INVALID_OPERATION`.

### 6.4 Your own GP kernel (`AL_XBOX_gp_program`)

```c
void alXboxEffectProgram(ALuint effect, const ALuint *words, ALsizei count, ALuint flags);
```

**Loading:**
- `words` is an image from `tools/dsp56asm.py --image`. Its header holds `AL_XBOX_GP_IMAGE_MAGIC`, the ABI
  `AL_XBOX_GP_IMAGE_ABI` (1), the scratch bytes, and the DMA descriptors (at most 60). Up to 0x700 code words
  follow.
- `flags` must be 0. A bad header, size or scratch request (about 180 KiB at most) gives `AL_INVALID_VALUE`.
- The library keeps a copy of the image.

**Parameters:**
- 32 words at `AL_XBOX_GP_PARAM0 + n`.
- `alEffecti` takes an integer from −0x800000 to 0xFFFFFF. `alEffectf` takes a fraction from −1 to 1, stored as
  Q23.
- After attaching the effect again, all changed words reach the kernel in the same 0.67 ms frame.

**Running:**
- The kernel's input is the slot's sends, on a bin pair (left, right). Its output is added to the dry mix.
- **Send levels on the pair:** a stereo source sends each channel at the whole send gain, so a send of 1 is
  exactly 0 dB (the samples arrive unscaled). A mono source sends half the gain on each bin, so their sum is
  the send.
- Code loads while the GP runs, without a restart.

**Timed to a stream:**

```c
void alXboxEffectSlotAtSample(ALuint slot, ALuint effect, ALuint source, ALuint sample);
```

- Attaches `effect` to `slot` (a copy, taken now) at the GP frame that mixes frame `sample` of the streaming
  source `source`. `sample` is the source's stream position: frames played since its queue started, 32 bits,
  wrapping.
- The effect must run the kernel the slot already runs (the same image), and the slot must be the active one.
  Otherwise, a source that is not streaming, or 16 changes already queued: `AL_INVALID_OPERATION`. A name that
  is no slot, effect or source: `AL_INVALID_NAME`.
- Changes apply in order, to the frame (0.67 ms), never early. A sample already played: the next frame.
- While the source is paused or not started, changes wait. A stopped source's apply at once, as does every
  change without a GP (the host).
- The GP holds one change at a time; the next goes out at the first `alXboxUpdateVoices()` after it. Changes
  closer together than that are late by up to one update.
- `AL_EFFECTSLOT_EFFECT`, or deleting the slot, drops the changes still queued for it.

How to write a kernel: [`GP_KERNELS.md`](GP_KERNELS.md).

### 6.5 Listener

`AL_METERS_PER_UNIT` (`alListenerf`, > 0, default 1) is stored and read back.

## 7. Xbox effects outside EFX

### 7.1 Room reverb (`AL_XBOX_reverb`, ENGINE_INTEGRATION 3.7)

```c
alEnable(AL_XBOX_REVERB);                          /* off by default */
alXboxReverbPreset(AL_XBOX_REVERB_PRESET_HALL);    /* GENERIC, ROOM, BATHROOM, HALL, CAVE, ARENA, SEWER_PIPE, UNDERWATER */
alXboxReverbf(AL_XBOX_REVERB_DECAY_TIME, 2.0f);
alSourcef(src, AL_XBOX_REVERB_SEND, 0.3f);
```

| Parameter | Range | Meaning |
|---|---|---|
| `AL_XBOX_REVERB_DECAY_TIME` | 0.1 .. 20 s | time to fall 60 dB |
| `AL_XBOX_REVERB_HF_RATIO` | 0.1 .. 1 | high-frequency decay |
| `AL_XBOX_REVERB_GAIN` | 0 .. 1 | reverb level |
| `AL_XBOX_REVERB_PRE_DELAY` | 0 .. 0.1 s | a change restarts the reverb |
| `AL_XBOX_REVERB_DIFFUSION` | 0 .. 1 | echo density |
| `AL_XBOX_REVERB_INPUT_GAIN` | 0 .. 1 | input headroom |
| `AL_XBOX_REVERB_INPUT_LOWPASS` | 0 .. 0.95 | muffles what enters the reverb |
| `AL_XBOX_REVERB_OUTPUT_LOWPASS` | 0 .. 0.98 | muffles the whole mix, dry sound included |

`alXboxGetReverbf` reads a value back. The settings persist while no device is open.

### 7.2 HRTF (`AL_XBOX_hrtf`, 3.5)

`alEnable/alDisable/alIsEnabled(AL_XBOX_HRTF)`, on by default:
- Mono 3D sources play through the APU's HRTF stage, up to 64 at once; beyond that they are panned by volume.
- Direct-mode and stereo sources never use it.
- A change applies at each source's next play.

## 8. Status and measurement

### 8.1 Hardware status (`AL_XBOX_hardware_status`)

```c
ALint v;
alXboxGetHardwareStatus(AL_XBOX_FREE_VOICES, &v);
```

| Query | Value |
|---|---|
| `AL_XBOX_BACKEND` | `AL_XBOX_BACKEND_MMIO` (the real APU) or `AL_XBOX_BACKEND_NULL` |
| `AL_XBOX_HW_VOICE_COUNT`, `AL_XBOX_FREE_VOICES` | hardware voices, and those not held by a source |
| `AL_XBOX_SAMPLE_PAGES_USED`, `AL_XBOX_SAMPLE_PAGES_TOTAL` | 4 KiB pages of the APU's sample space (2048 in all) |
| `AL_XBOX_BUFFER_BYTES`, `AL_XBOX_STATIC_BUFFER_BYTES` | library memory for buffer data; your memory that static buffers play |
| `AL_XBOX_HRTF_ACTIVE`, `AL_XBOX_REVERB_ACTIVE` | the HRTF stage, or a reverb (Xbox or EFX), is running |
| `AL_XBOX_GP_LOAD`, `AL_XBOX_GP_LOAD_PEAK` | percent of the GP's frame the effect takes, last frame / the peak since the last peak query (100: late frames) |
| `AL_XBOX_AV_PACK_TYPE` | `AL_XBOX_AV_PACK_*` |
| `AL_XBOX_DOLBY_DIGITAL_ACTIVE`, `AL_XBOX_VP_BASE_PHYS`, `AL_XBOX_LFE_GAIN` | cached state, 0 without a device |

An unknown query gives `AL_INVALID_ENUM`, and a NULL pointer gives `AL_INVALID_VALUE`.

### 8.2 CPU time (`AL_XBOX_stats`, 3.8)

```c
ALXBOXSTATS s = { sizeof(s) };
alXboxGetStats(&s);
```

- **Fields:** cycles and call counts for the output thread and the voice tick (with their splits), AC97 buffers,
  underruns, padded buffers, and the zero-copy lead and slips.
- **Reading them:** the counters never reset, so take the difference between two calls. `cycle_hz` converts
  cycles to time.

### 8.3 Strings

- `alGetString(AL_EXTENSIONS)` lists the extensions.
- `alGetString(AL_RENDERER)` names the backend.

### 8.4 Final-mix callback (`AL_XBOX_output_callback`, 3.9)

```c
void cb(ALshort *samples, ALsizei frames, ALvoid *user);   /* 512 stereo 16-bit frames, 48 kHz */
alXboxOutputCallback(cb, user);                             /* NULL removes it */
```

- It sees, and may change in place, every block of the final mix just before it reaches the codec.
- It runs in the output thread at high priority: keep it well under 1 ms, and do not call OpenAL from it.
- Real console only.

## 9. Global state

- **Supported:** `alDistanceModel` (every 1.1 model), `alDopplerFactor`, `alSpeedOfSound`, the `alGet{Boolean,
  Integer,Float,Double}[v]` queries, `alEnable` / `alDisable` / `alIsEnabled` for `AL_XBOX_HRTF` and
  `AL_XBOX_REVERB`, `alIsExtensionPresent`, `alGetProcAddress`, `alGetEnumValue`.
- **Listener:** `AL_POSITION`, `AL_VELOCITY`, `AL_ORIENTATION`, `AL_GAIN`, and `AL_METERS_PER_UNIT` (6.5).

## 10. Not supported

- Output other than stereo 16-bit 48 kHz: no 5.1 or Dolby Digital.
- Multichannel buffers.
- Capture.
- EFX effect types other than reverb, EAX reverb and the GP program.
- Highpass and bandpass filters.
- More than one running effect, or more than 2 sends per source.
