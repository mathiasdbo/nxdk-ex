# Using the OpenAL / MCPX APU library from a game engine

This guide is for porting an engine's sound system (Xash3D, Quake-family engines, or any homebrew engine) to the
original Xbox on this library. The library is OpenAL 1.1 plus a few extensions. On a real console it plays
through the MCPX APU: sources run on hardware voices, the APU mixes them, and the library forwards the mix to
the AC97 codec. Console verification of each feature is recorded in `XEMU_VERIFICATION.md` 8.1b. Every call, with its values
and errors, is listed in [`API_REFERENCE.md`](API_REFERENCE.md); this guide shows how to combine them.

## 1. Build and start-up

- **Real-hardware build.** Compile the library sources into your program with `-DOPENAL_APU_REAL_MMIO`. The
  samples do this with one wrapper file per library source (`samples/openal_engine/mmio/*.c` and its
  `Makefile`); copy that pattern. Without the define the library runs its null backend, which makes no sound.
- **Device.** `alcOpenDevice(NULL)`, `alcCreateContext`, `alcMakeContextCurrent`. The device brings the APU
  up and starts the AC97 output. `alXboxGetHardwareStatus(AL_XBOX_BACKEND)` returns `AL_XBOX_BACKEND_MMIO` on
  the real APU.
- **Once per frame:** call `alXboxUpdateVoices()` (or `alcProcessContext`). It does three things:
  - ends one-shots that have finished;
  - refills streaming sources;
  - gives free voices to waiting sources.

  The sound output does not depend on it: a library thread feeds the codec every 4 ms, including during
  loading screens.
- **Threads.** Call the library from one thread. Its own pump thread touches only the output and the
  already-played part of stream rings.

## 2. Limits

| Resource | Limit | Notes |
| --- | --- | --- |
| Hardware voices | 120 (64 on xemu) | handles 0-127; the APU can stop for good on a higher handle, and the driver refuses one |
| Sources | 256 | the rest wait in virtual standby, or stop (`AL_XBOX_VIRTUALIZE`) |
| Buffers | 2048 (`-DAL_MAX_BUFFERS=<n>` to build fewer) | copied (`alBufferData`) or played in place from the engine's memory (`alBufferDataStatic`, 3.10) |
| Formats | mono/stereo, 8-bit unsigned or 16-bit signed PCM, Xbox ADPCM | any sample rate; ADPCM decoded by the APU (3.11); no multichannel |
| Pitch | playback rate 1/256 to 3.8 times 48 kHz | `AL_PITCH` times the buffer's rate is clamped to it (a console plays exactly down to 1/256 and tops out at 3.82) |
| Sample space | 2048 pages of 4 KiB (8 MiB) | counts buffers of playing voices only; reclaimed automatically (`AL_XBOX_SAMPLE_PAGES_USED`) |
| Streaming sources | 32, each with up to 64 queued buffers | ring of 16384 frames, data written up to 8192 frames ahead |
| Output | stereo, 16-bit, 48 kHz | no 5.1 or Dolby Digital |
| HRTF voices | 64 at once | mono 3D sources beyond that are panned by volume (`AL_XBOX_HRTF`) |
| Reverb | one room for the whole mix | on the GP DSP, no CPU time; each source chooses how much it sends (`AL_XBOX_reverb`, 3.7) |
| Effects (EFX) | 4 slots, one effect running; 2 sends per source; lowpass filters only | reverb / EAX reverb on the GP (3.12), or the game's own GP kernel: 0x700 code words, 60 DMA descriptors a frame, ~180 KiB scratch (3.13) |

Buffer memory itself comes from contiguous RAM (`apu_mem.c`), so the console's 64 MiB is the real ceiling.
`alBufferData` keeps a buffer's data in ordinary cached RAM; the APU copy (contiguous, write-combined) is made
when a source takes the buffer as its `AL_BUFFER`, so buffers that are only queued on streaming sources never
use APU memory. The contiguous pool that copy and the voice table come from is 2 MiB unless
`alXboxSetPoolSize(bytes)` is called before `alcOpenDevice` (`AL_XBOX_pool_size`); an allocation the pool
cannot hold gets its own block, so a small pool only costs speed.

## 3. Recipes

### 3.1 Play the engine's own software mix (output only)

Use this when the engine keeps mixing on the CPU and only needs an output device, the way it would use SDL
audio or a sound card driver. Create one streaming source and keep 4-8 buffers of 10-40 ms queued:

```c
alGenSources(1, &src);
const ALfloat unity[2] = { 1.0f, 1.0f };
alSourcefv(src, AL_XBOX_DIRECT_GAINS, unity);        /* the mix is panned already: no 3D model */
for (i = 0; i < NBUF; i++) { mix(chunk); alBufferData(buf[i], AL_FORMAT_STEREO16, chunk, bytes, rate);
                             alSourceQueueBuffers(src, 1, &buf[i]); }
alSourcePlay(src);
/* every frame */
alGetSourcei(src, AL_BUFFERS_PROCESSED, &n);
while (n--) { alSourceUnqueueBuffers(src, 1, &b); mix(chunk); alBufferData(b, ...); alSourceQueueBuffers(src, 1, &b); }
alGetSourcei(src, AL_SOURCE_STATE, &st);
if (st != AL_PLAYING) alSourcePlay(src);               /* it ran dry: restart */
alXboxUpdateVoices();
```

Mix at the engine's own rate (11/22/44.1 kHz). The APU resamples to 48 kHz for free.

Use direct mode, not a relative source at the origin: that one still goes through the 5.1 pan, which spreads a
source at the listener evenly over four speakers and folds them to stereo 3 dB down. The queued buffers stay
in CPU memory (section 2), and the stream copies them into its ring from there, not from write-combined
memory, which the CPU reads uncached; refilling the same buffers allocates nothing. If no source plays a buffer
directly, `alXboxSetPoolSize(256 * 1024)` before opening the device returns most of the default 2 MiB pool.

### 3.2 Engine-spatialized effects on hardware voices

Use this when the engine computes each channel's volume and pan itself (Half-Life's `S_SpatializeChannel`) and
the mix must stay faithful to it:

- **Direct gains.** `alSourcefv(src, AL_XBOX_DIRECT_GAINS, (ALfloat[2]){left, right})` with gains in 0..1.
  - The library's distance, cone, Doppler and pan are bypassed.
  - The output gain is left/right times `AL_GAIN` times the listener gain.
  - Update it whenever the engine re-spatializes; the APU ramps volume changes.
- **Pitch.** Set `AL_PITCH` from the engine's pitch (Half-Life: `pitch / 100`). The playback rate may go from
  1/256 to 3.8 times 48 kHz, so an 11 kHz sound plays down to `AL_PITCH` 0.017.
- **Priority.** `alSourcef(src, AL_XBOX_PRIORITY, p)` lets the engine rank its channels. A higher value takes a
  voice from a lower one when all 64 are busy.
- **No standby.** `alSourcei(src, AL_XBOX_VIRTUALIZE, AL_FALSE)` makes a source that gets no voice, or loses
  one, become `AL_STOPPED` instead of waiting. `AL_XBOX_HAS_VOICE` says whether it is on hardware right now.
  Mix the rest on the CPU and send that through 3.1.

### 3.3 Sounds with a loop section (cue points)

For a WAV that plays an intro once and then loops a section, put its loop points on the buffer:

```c
alBufferData(buf, fmt, data, size, rate);
ALint lp[2] = { loop_start_frame, loop_end_frame };    /* end = frames for "to the end" */
alBufferiv(buf, AL_LOOP_POINTS_SOFT, lp);
alSourcei(src, AL_LOOPING, AL_TRUE);
```

The voice plays from the start, then repeats `[start, end)` in hardware. Stop the sound with `alSourceStop`, or
set `AL_LOOPING` to `AL_FALSE` while it plays to let it play on to the end of the data and stop there.
Setting it to `AL_TRUE` on a playing one-shot makes it loop (if it is already past the loop end, it first plays
to the end of the data).

### 3.4 Positions and seeking

`AL_SAMPLE_OFFSET`, `AL_BYTE_OFFSET` and `AL_SEC_OFFSET` work on static and streaming sources.
- **Setting while stopped:** the offset applies at the next `alSourcePlay`.
- **Setting while playing or paused:** the source jumps at once.
- **Streaming sources:** the offset counts from the start of the queue, and buffers before it count as
  processed.
- **Preemption:** a source that loses its voice to a higher priority resumes where it was when it gets one back.

### 3.5 3D sources and HRTF

A mono source positioned with `AL_POSITION` plays through the APU's HRTF stage: a two-ear filter for its
direction, computed by the library from a spherical-head model.
- **Limit:** up to 64 sources at once use it; any more are panned by volume.
- **Switching it off:** HRTF suits headphones. For TV speakers, or for an engine that pans itself (direct gains,
  3.2), call `alDisable(AL_XBOX_HRTF)`. The setting applies to each source at its next `alSourcePlay`.
- **Checking it:** `alXboxGetHardwareStatus(AL_XBOX_HRTF_ACTIVE)` reports whether HRTF is in use.
- **Not affected:** stereo sources and sources in direct mode never use HRTF.

### 3.6 Music

Decode MP3/OGG on the CPU into a streaming source (3.1), or into the engine's software mix.

### 3.7 Room reverb (AL_XBOX_reverb)

The APU's GP DSP runs a reverb on the whole mix: 8 damped feedback combs and 4 allpasses per ear (the
public-domain Freeverb tuning) on 25 delay lines in system RAM. It costs no CPU time. Each source decides how
much of its sound goes into the reverb; the dry sound is not affected, except by the output lowpass below.

```c
alXboxReverbPreset(AL_XBOX_REVERB_PRESET_HALL);        /* pick the room */
alEnable(AL_XBOX_REVERB);                              /* off by default */
alSourcef(src, AL_XBOX_REVERB_SEND, 0.5f);             /* per source, 0..1, default 0 */
```

- **The send** follows the source's gain (`AL_GAIN`, distance attenuation, direct gains) and is taken before
  panning and HRTF, so a far source sounds farther: its dry sound drops while the room stays. Stereo sources
  send both channels mixed. It works in direct mode (3.2) too.
- **Presets** (`AL_XBOX_REVERB_PRESET_*`, our own settings): `GENERIC`, `ROOM`, `BATHROOM`, `HALL`, `CAVE`,
  `ARENA`, `SEWER_PIPE`, `UNDERWATER`.
- **Parameters** (`alXboxReverbf` / `alXboxGetReverbf`):

  | Parameter | Range | Effect |
  | --- | --- | --- |
  | `AL_XBOX_REVERB_DECAY_TIME` | 0.1 - 20 s | time for the tail to fall by 60 dB (measured on a console to 0.2 dB) |
  | `AL_XBOX_REVERB_HF_RATIO` | 0.1 - 1 | how much faster the highs die (1: not at all) |
  | `AL_XBOX_REVERB_GAIN` | 0 - 1 | reverb level in the output |
  | `AL_XBOX_REVERB_PRE_DELAY` | 0 - 0.1 s | gap before the reverb starts (at least 0.67 ms) |
  | `AL_XBOX_REVERB_DIFFUSION` | 0 - 1 | echo density of the tail |
  | `AL_XBOX_REVERB_INPUT_GAIN` | 0 - 1 | headroom before the combs (lower it if loud scenes clip) |
  | `AL_XBOX_REVERB_INPUT_LOWPASS` | 0 - 0.95 | darkens what enters the reverb |
  | `AL_XBOX_REVERB_OUTPUT_LOWPASS` | 0 - 0.98 | muffles the whole mix, dry sound included (underwater) |

- **Changing rooms:** a preset with the same pre-delay changes the gains at once, with the tail running on; a
  different pre-delay restarts the reverb (its tail is cut). Changing rooms a few times a second is fine.
- **Where it runs:** on a console, `alXboxGetHardwareStatus(AL_XBOX_REVERB_ACTIVE)` reports 1 while it runs.
  On xemu's front-end route it stays off (bin 2 is a surround bin there). The settings are kept while no
  device is open and applied when one is.
- **Half-Life** (`room_type` 0-28 from `env_sound`, in the SDK's groups: generic, metallic, tunnel, chamber,
  bright, water, concrete, outside, cavern, weirdo, three sizes each): one possible mapping, to tune by ear in
  the port. 0 (normal): reverb off. 1 (generic): `GENERIC`. 2-4 (metallic) and 17-19 (concrete): `ROOM` /
  `HALL` / `ARENA` by size. 5-7 (tunnel): `SEWER_PIPE`. 8-10 (chamber): `HALL`. 11-13 (bright):
  `BATHROOM`. 14-16 (water, used while the player is under water): `UNDERWATER`. 20-22 (outside): reverb off
  or `GENERIC` with a low gain. 23-25 (cavern): `CAVE`. 26-28 (weirdo): `CAVE` with a long pre-delay. Send
  about 0.3-0.6 for world sounds, 0 for HUD sounds and music.
- **The same reverb through EFX:** `AL_EFFECT_REVERB` on an auxiliary slot (3.12). Use one or the other: both
  at once give `AL_INVALID_OPERATION`.

### 3.8 Measuring the library's CPU time (AL_XBOX_stats)

With OpenAL the audio CPU time is in two places: the library's **output thread** (every 4 ms, top priority; it
keeps the AC97 fed with the APU's mix and scrubs played stream data) and the **voice tick** (`alXboxUpdateVoices`, on
the game thread). Both are counted in CPU cycles:

```c
ALXBOXSTATS s = { sizeof(s) };
alXboxGetStats(&s);               /* once per frame; keep the previous one */
double out_ms = (s.output_cycles - prev.output_cycles) * 1000.0 / s.cycle_hz;
double tick_ms = (s.update_cycles - prev.update_cycles) * 1000.0 / s.cycle_hz;
```

- **Counters:** they never reset; take differences. `cycle_hz` is measured (733 MHz on a retail console), 0 for
  the first 10 ms.
- **Other fields:**
  - `output_passes`, `output_max_gap_ms`, `output_max_cycles`: how regularly the thread ran and its longest pass;
  - `output_hook_cycles`: the share of 3.9's callback;
  - `update_calls`, `update_max_cycles`: the voice tick;
  - `ac97_buffers`, `ac97_underruns`, `ac97_padded`: output health (copy mode: 10.7 ms per buffer, padded means
    the APU's mix was late; zero-copy: 2.67 ms per buffer-list entry, an underrun means the thread was away for
    the whole ~85 ms queue);
  - `output_zero_copy`: the codec reads the APU's mix directly (the default; see below);
  - `output_slips`, `output_lead_min` / `_max` / `_last`: zero-copy only. The lead is how many frames the APU's mix
    is ahead of the codec (about 1024, half the 8 KiB ring, ~21 ms of latency); a slip plays a 2.67 ms piece twice
    or skips one to keep it in the middle half of the ring. On a console the two clocks are locked, so slips should
    stay at 0.
- **Zero-copy output:** the APU writes its mix in the codec's format, so by default the codec reads it in place
  and the output thread only refills the codec's buffer list (no samples go through the CPU). An output callback
  (3.9) needs the samples, so while one is installed the output runs in copy mode. `APU_VP_DBG_AC97_COPY`
  (`apu_vp_debug_set_flags`, before the context is created) forces the copy mode with its larger ring.
- **Without the output thread:** the output work runs inside the tick, so `update_cycles` includes it.
- **Older structures:** only `size` bytes are written, so a program built against an older `ALXBOXSTATS` keeps
  working.

### 3.9 Processing the final mix on the CPU (AL_XBOX_output_callback)

```c
static void AL_APIENTRY my_fx(ALshort *samples, ALsizei frames, ALvoid *user) {
    /* frames = 512 interleaved 16-bit stereo frames at 48 kHz; change them in place */
}
alXboxOutputCallback(my_fx, my_state);   /* NULL removes it */
```

- **When it runs:** for every block of the final mix (after the APU's voices, HRTF and GP reverb), just before
  it goes to the codec. The samples are in cached memory, so reading them is cheap.
- **It costs the zero-copy output:** installing a callback switches the output to copy mode (a short gap while
  the codec restarts) and the output thread copies every block again; removing it switches back (3.8).
- **Gaps:** the APU's ring is the 8 KiB zero-copy one, so in copy mode the output tolerates ~37 ms between two
  passes. The output thread runs every 4-6 ms; without it (the output in the tick), keep above ~30 fps while a
  callback is installed. `APU_VP_DBG_AC97_COPY` gives copy mode its 64 KiB ring (~300 ms) but turns zero-copy off.
- **Constraints:** it runs in the output thread at high priority. Keep it well under a millisecond per block
  (it shows in `output_hook_cycles`), and do not call OpenAL from it.
- **Uses:** a CPU effect on the whole mix, a recorder, a level meter.
- **Effects that don't need it:** the GP reverb (3.7, 3.12) and custom GP kernels (3.13) add effects at no CPU
  cost.

### 3.10 Sounds from the engine's own memory (AL_EXT_STATIC_BUFFER)

`alBufferData` copies the sound into the library, so an engine that keeps its sound pool holds every sound twice.
`alBufferDataStatic` (the same entry point as Apple's and OpenAL Soft's) plays the engine's memory in place:

```c
alBufferDataStatic(buf, AL_FORMAT_MONO8, sfx->data, sfx->size, sfx->rate);   /* no copy */
```

- **The memory:** ordinary RAM (malloc, a sound pool, a loaded file). No alignment and no physical
  contiguity are needed; the APU maps it page by page.
- **Formats:** every format `alBufferData` takes. That includes 8-bit unsigned PCM (Half-Life's effects; the
  silent tail is written as 0x80) and Xbox ADPCM (3.11).
- **Keep it valid:** it must stay allocated, unmoved and unchanged while the buffer has this data. Delete the
  buffer, or give it other data, before freeing or reusing the memory; a buffer a source uses can't be changed
  (`AL_INVALID_OPERATION`), as in OpenAL. After writing new bytes into the same memory, call
  `alBufferDataStatic` again.
- **Cost:** the library copies only the sound's last partial 4 KiB page (a silent tail must follow the data)
  and writes the CPU cache back to RAM. On a console a 96 KB sound in 24 physically scattered pages cost the
  library 4 KB.
- **Budget queries:** `alXboxGetHardwareStatus(AL_XBOX_BUFFER_BYTES)` is the library's own buffer memory,
  `AL_XBOX_STATIC_BUFFER_BYTES` the engine memory static buffers point at.

**Part of a sound, and sentences.** A static buffer can start and end at any byte, so a range of a sound
needs no copy either. Half-Life's sentence words with start/end percentages are buffers into the word's data.
To play words back to back, queue them on one source; queued buffers play without a gap (on a console five
queued pieces of one tone came out as one continuous tone):

```c
for (w = 0; w < n; w++) {
    const uint8_t *d = word[w].data + word[w].start;
    alBufferDataStatic(piece[w], fmt, d, word[w].end - word[w].start, word[w].rate);
}
alSourceQueueBuffers(src, n, piece);   /* one format and rate per queue */
alSourcePlay(src);
```

When a sound has ended, `alGetSourcei(src, AL_SOURCE_STATE)` says so at once: it reads the voice's position (a
250 ms one-shot reported `AL_STOPPED` after 252 ms on a console). The only delay is how often the engine asks.

### 3.11 Xbox ADPCM (AL_XBOX_adpcm)

The APU decodes Xbox ADPCM itself, so ADPCM music or speech costs no CPU to decode:

```c
alBufferDataStatic(buf, AL_FORMAT_STEREO_XBOX_ADPCM, xwav_data, xwav_bytes, 44100);   /* or alBufferData */
```

- **Layout:** the one of `.xwav` files (`WAVE_FORMAT_XBOX_ADPCM`, 0x0069).
  - 36-byte IMA ADPCM blocks per channel, 64 samples each.
  - The block header holds the first sample and the step index.
  - The 64th 4-bit code is not played; the next header carries that sample.
  - Stereo puts both headers first, then 4-byte chunks alternating left and right.
  - The size must be whole blocks (`AL_INVALID_VALUE` otherwise).
- **Positions:** frames (64 per block) for `AL_SAMPLE_OFFSET`, `AL_SEC_OFFSET` and loop points.
  `AL_BYTE_OFFSET` reads and sets a block's start. `AL_BITS` reports 4.
- **Streaming:** queue ADPCM chunks on a source as with PCM (3.1); every buffer in a queue has the same format and
  rate, each a whole number of blocks. Seeks and restarts land on a block start. This plays songs of any length
  from disk with no CPU decoding. A whole song can also be one buffer (static: no memory beyond the file), up to
  the 8 MiB the APU maps at once, about 3 minutes of 44.1 kHz stereo.
- **Measured on a console:** 480 Hz and 960 Hz tones played at the exact rate, mono, stereo, copied and static,
  and a one-shot reported `AL_STOPPED` 2 ms after its last sample. Streamed in 256 ms chunks, four queued and
  refilled for 3 s, the tone came out at 480 Hz without a gap.

### 3.12 Effects through EFX (ALC_EXT_EFX)

Engines and ports that already use the OpenAL Effects Extension (`AL/efx.h`) run unchanged. The effect runs on
the GP (no CPU time) and the sends are voice outputs mixed by the APU:

```c
ALuint fx, slot;
alGenEffects(1, &fx);
alEffecti(fx, AL_EFFECT_TYPE, AL_EFFECT_REVERB);       /* or AL_EFFECT_EAXREVERB */
alEffectf(fx, AL_REVERB_DECAY_TIME, 2.5f);
alGenAuxiliaryEffectSlots(1, &slot);
alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, (ALint)fx);
alSource3i(src, AL_AUXILIARY_SEND_FILTER, (ALint)slot, 0, AL_FILTER_NULL);   /* send 0 to the slot */
```

- **What there is:**
  - `ALC_EXT_EFX` 1.0 in the device's extension string. Every EFX function is available through
    `alGetProcAddress`.
  - Effect types: `AL_EFFECT_NULL`, `AL_EFFECT_REVERB`, `AL_EFFECT_EAXREVERB` and `AL_EFFECT_XBOX_GP_PROGRAM`
    (3.13). Other types give `AL_INVALID_VALUE`, as the extension allows.
  - Filters: `AL_FILTER_NULL` and `AL_FILTER_LOWPASS` (below). Highpass and bandpass give `AL_INVALID_VALUE`.
  - Up to 4 slots and 256 effects and filters. `ALC_MAX_AUXILIARY_SENDS` is 2 (the default; larger requests are
    clamped).
- **One effect per device.** The GP runs one program. While one slot holds an effect, attaching an effect to
  another slot gives `AL_INVALID_OPERATION`. `AL_XBOX_REVERB` (3.7) counts as that effect too. Empty slots are
  legal and silent.
- **Sends:**
  - A send's level is the source's gain with distance attenuation by `AL_ROOM_ROLLOFF_FACTOR`, plus the
    reverb's own room rolloff; the cone too when `AL_AUXILIARY_SEND_FILTER_GAIN_AUTO` is on.
  - Volume changes apply live, while the sound plays.
  - Sends to a slot that does not hold the active effect are silent.
- **Copy semantics, as in the spec:** a slot takes a copy of the effect when it is attached. After changing
  the effect, attach it again. A source keeps its slot; deleting a slot a source sends to gives
  `AL_INVALID_OPERATION`.
- **The reverb is an approximation.** It is the Freeverb of 3.7 driven by the EFX parameters below. Every
  parameter is stored and read back; only these change the sound:

  | EFX parameter | What it drives |
  | --- | --- |
  | `DECAY_TIME` | decay time |
  | `DECAY_HFRATIO` | HF ratio, clamped to 1 (no boost) |
  | `GAIN` x `LATE_REVERB_GAIN` x `AL_EFFECTSLOT_GAIN` | reverb level (clamped to 1) |
  | `REFLECTIONS_DELAY` + `LATE_REVERB_DELAY` | pre-delay, clamped to 0.1 s |
  | `DIFFUSION` | diffusion |
  | `GAINHF` | input lowpass |

  `REFLECTIONS_GAIN`, `DENSITY`, `AIR_ABSORPTION_GAINHF`, `DECAY_HFLIMIT` and the EAX-only parameters (LF
  gain, echo, modulation, pans, references) are stored but not used. Presets such as Creative's
  `EFX_REVERB_PRESET_*` are the application's own (the library ships none).
- **Changing rooms** behaves as in 3.7: gains change live, a new pre-delay restarts the reverb.
- **Lowpass filters** (`AL_FILTER_LOWPASS`, for occlusion and muffling):

  ```c
  alFilteri(f, AL_FILTER_TYPE, AL_FILTER_LOWPASS);
  alFilterf(f, AL_LOWPASS_GAIN, 0.8f);      /* the whole sound */
  alFilterf(f, AL_LOWPASS_GAINHF, 0.2f);    /* the gain at 5 kHz */
  alSourcei(src, AL_DIRECT_FILTER, (ALint)f);
  ```

  - `GAINHF` runs on the voice's own hardware filter (a 2-pole lowpass), fitted so that the level at 5 kHz
    is `GAINHF`, measured on a console. It costs no CPU time, works on plain, HRTF and stereo voices,
    and can change while the sound plays without a click. The sound below the cutoff is unchanged, above it
    falls by about 12 dB an octave: deeper than OpenAL Soft's shelf, so a low `GAINHF` muffles more.
  - `GAIN` scales the dry sound (direct filter) or that send (send filter).
  - The hardware filter is one per voice, so it also shapes the sends. A send filter's `GAINHF` is not
    applied (its `GAIN` is).
- **Where it runs:** console only, like 3.7. `alXboxGetHardwareStatus(AL_XBOX_REVERB_ACTIVE)` reports 1 while
  an EFX reverb runs.

### 3.13 Your own effect on the GP (AL_XBOX_gp_program)

`AL_EFFECT_XBOX_GP_PROGRAM` is an EFX effect type that runs the game's own DSP56300 kernel instead of the
reverb, for example Half-Life's room effect or an echo:

```c
alEffecti(fx, AL_EFFECT_TYPE, AL_EFFECT_XBOX_GP_PROGRAM);
alXboxEffectProgram(fx, gp_kecho_image, GP_KECHO_IMAGE_WORDS, 0);   /* image from dsp56asm.py --image */
alEffecti(fx, AL_XBOX_GP_PARAM0 + 0, 4800);                          /* parameters: 24-bit words */
alEffectf(fx, AL_XBOX_GP_PARAM0 + 2, 0.5f);                          /* or fractions, as Q23 */
alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, (ALint)fx);       /* loads and starts it */
```

- **Parameters:** a change reaches the kernel when the effect is attached again, all the changed words in the
  same 0.67 ms frame.
- **Changes timed to the mix:** an engine that mixes ahead into a stream (Half-Life's room sound, ~0.12 s
  ahead) queues each change at the stream position it was mixed for:
  `alXboxEffectSlotAtSample(slot, fx, room_stream, painted_frame)`. The change lands in the GP frame that
  mixes that frame, not when it was sent. Start the dry and the room streams together with `alSourcePlayv`
  so their positions match. Details in API_REFERENCE 6.4.
- **A new image** reloads the kernel, starting from silence. A brief gap in the effect, not in the dry sound.
- **`AL_EFFECTSLOT_GAIN`** scales the sends into the kernel, live (for a linear kernel, the same as its output).
- **Input levels:** a stereo source sends its left and right each at the whole send gain, so at send 1 the
  kernel reads the samples unscaled (0 dB exactly). A mono source puts half the send on each bin.
- **Cost:** `alXboxGetHardwareStatus(AL_XBOX_GP_LOAD / AL_XBOX_GP_LOAD_PEAK)` is how much of the GP's frame
  the effect takes, in percent.
- **Example:** `dsp/gp_kecho.asm` is a stereo echo with delay (32-16384 samples), feedback and wet parameters.
- **Writing a kernel:** the ABI, the memory map, DMA, building the image and testing it in the host simulator
  are in `docs/GP_KERNELS.md`.
- **Status:** checked on a console by `samples/openal_efx`: the echo lands exactly on its delay, parameters
  change in one frame, the code loads without a GP restart. The reverb takes 34 % of the GP's frame, the echo 4 %.

## 4. Things to know

- **A one-shot ends a little late.** On a console a voice must never reach the end of its data: that stops the
  whole APU. So every buffer carries 256 bytes of silence, a one-shot loops over it, and the library takes the
  voice off at the next `alXboxUpdateVoices()`. A state query reports `AL_STOPPED` as soon as the data has
  played (it reads the voice's position); the voice itself is released at the next tick.
- **Stopping and restarting is cheap.** Each start takes a fresh hardware voice. Restarting a sound every
  frame is fine.
- **Deleting a buffer that is attached or queued** fails with `AL_INVALID_OPERATION`, as in OpenAL.
- **Loading per level is safe.** Deleting buffers and loading new ones never exhausts the sample space: mappings
  of buffers no voice plays are reclaimed.
- **Late streams.** A streaming source the application stops refilling plays out what it has (up to 8192
  frames), then silence. It does not repeat old data.
- **xemu** runs all of this, but has none of the console's freeze conditions. Test on hardware.

## 5. Checking it on a console

`samples/openal_engine` exercises loop points, direct gains, offsets, the pitch range, the voice budget and
buffer churn, and logs to `E:\openal_engine_N.txt` (a new file each run). `samples/openal_stream` covers streaming and the output
thread. Mode 6 of `samples/openal_showcase` plays dry recordings through every reverb preset;
`samples/apu_gp_reverb` measures the reverb's decay on the console. `samples/openal_efx` runs EFX the way a
port does (entry points from `alGetProcAddress`): an EFX reverb, the echo kernel through `alXboxEffectProgram`
with its delay measured from the final mix, live parameters, two sends into one slot, then 5 minutes of
listening with the effects changing, and logs to `E:\openal_efx.txt`.
