# Using the OpenAL / MCPX APU library from a game engine

This guide is for porting an engine's sound system (Xash3D, Quake-family engines, or any homebrew engine) to the
original Xbox on this library. The library is OpenAL 1.1 plus a few extensions. On a real console it plays
through the MCPX APU: sources run on hardware voices, the APU mixes them, and the library forwards the mix to
the AC97 codec. Console verification of each feature is recorded in `XEMU_VERIFICATION.md` 8.1b.

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
| Buffers | 2048 | |
| Formats | mono/stereo, 8-bit unsigned or 16-bit signed PCM | any sample rate; no ADPCM, no multichannel |
| Pitch | playback rate 1/16 to 4 times 48 kHz | `AL_PITCH` times the buffer's rate is clamped to it |
| Sample space | 2048 pages of 4 KiB (8 MiB) | counts buffers of playing voices only; reclaimed automatically (`AL_XBOX_SAMPLE_PAGES_USED`) |
| Streaming sources | 32, each with up to 64 queued buffers | ring of 16384 frames, data written up to 8192 frames ahead |
| Output | stereo, 16-bit, 48 kHz | no 5.1 or Dolby Digital; HRTF only on xemu |

Buffer memory itself comes from contiguous RAM (`apu_mem.c`), so the console's 64 MiB is the real ceiling.

## 3. Recipes

### 3.1 Play the engine's own software mix (output only)

Use this when the engine keeps mixing on the CPU and only needs an output device, the way it would use SDL
audio or a sound card driver. Create one streaming source and keep 4-8 buffers of 10-40 ms queued:

```c
alGenSources(1, &src);
alSourcei(src, AL_SOURCE_RELATIVE, AL_TRUE);          /* no 3D model on the mix */
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

### 3.2 Engine-spatialized effects on hardware voices

Use this when the engine computes each channel's volume and pan itself (Half-Life's `S_SpatializeChannel`) and
the mix must stay faithful to it:

- **Direct gains.** `alSourcefv(src, AL_XBOX_DIRECT_GAINS, (ALfloat[2]){left, right})` with gains in 0..1.
  - The library's distance, cone, Doppler and pan are bypassed.
  - The output gain is left/right times `AL_GAIN` times the listener gain.
  - Update it whenever the engine re-spatializes; the APU ramps volume changes.
- **Pitch.** Set `AL_PITCH` from the engine's pitch (Half-Life: `pitch / 100`).
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

### 3.5 Music

Decode MP3/OGG on the CPU into a streaming source (3.1), or into the engine's software mix.

## 4. Things to know

- **A one-shot ends a little late.** On a console a voice must never reach the end of its data: that stops the
  whole APU. So every buffer carries 256 bytes of silence, a one-shot loops over it, and the library ends it at
  the next `alXboxUpdateVoices()`. A source reports `AL_STOPPED` up to one frame after its data ends.
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
thread.
