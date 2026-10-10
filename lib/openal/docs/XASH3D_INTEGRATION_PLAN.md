# Plan: the OpenAL / MCPX APU feature set for Xash3D (Half-Life)

Status: phase A done and verified on a console (2026-10-09); phases B-E planned. Scope: what `lib/openal` must provide so that the Xbox port of Xash3D can play
Half-Life's sound through the real MCPX APU, in what order, and how each step is verified. The engine-side
work happens in the Xash3D port; this document lists only the interfaces it needs from nxdk-ex.

## 1. Where the library stands

Verified on a retail console (see `XEMU_VERIFICATION.md` 8.1b):

| Area | State |
| --- | --- |
| Output path | VP voices → mixbins 0/1 → GP program → FIFO ring → CPU forwards to the AC97. Stereo, 16-bit, 48 kHz |
| Voices | 64 hardware voices (handles 64-127). 256 OpenAL sources, virtualized onto them with priority stealing (`apu_voice_mgr.c`) |
| Formats | `AL_FORMAT_MONO8/16`, `STEREO8/16`, any sample rate (VP pitch is a 4.12 log2 ratio to 48 kHz) |
| Playback control | play, pause, stop, rewind, `AL_LOOPING`, gain, pitch, position-based pan; target updates are ramped by the VP |
| One-shots | loop over a 256-byte silent tail that every buffer carries; `alXboxUpdateVoices()` unlinks them (a voice that reaches its end stops the APU on hardware) |
| 3D | distance and cone model in the library; on a console the pan is folded to stereo volumes (no HRTF: it needs front-end methods, which do not run on hardware) |
| Streaming | buffer queueing on a looping ring voice, and an AC97 pump thread that keeps the output going while the game is not calling the library (phase A, done) |
| Missing | sample/byte/second offsets, loop points, direct gains, any effect (the GP only copies the mix) |

Hardware limits any design must respect:
- At most 64 concurrent voices. A handle above 127 freezes the APU until a reboot (the driver refuses it).
- Sample data must be physically contiguous per buffer and lie in an 8 MiB linear SGE space (2048 pages).
  An entry above 2047 also freezes the APU.
- A voice must never reach its end (silent tail plus driver unlink), and its record must not be rewritten
  while the VP may hold it (fresh handle per start, one-frame quarantine).
- A frozen APU cannot be recovered in software. Every one of these rules is a hard guard in the driver,
  not a convention.
- The AC97 feed is CPU-driven. `alXboxUpdateVoices()` must run at least every ~40 ms, or the output underruns.

## 2. What Half-Life needs

| HL / Xash requirement | Library feature |
| --- | --- |
| Many 8-bit unsigned and 16-bit WAVs at 11 025, 22 050 and 44 100 Hz, loaded per map | static buffers in APU memory, a memory budget (F6) |
| Volume and pitch changes on a playing channel (`SND_CHANGE_VOL`, `SND_CHANGE_PITCH`, pitch 1-255 around 100) | gain and pitch updates (exist); pitch range check (F7) |
| Cue-point loops: play the start once, then loop a section (ambient sounds, machines) | loop points on a buffer (F2) |
| Engine-computed attenuation and stereo pan (`S_SpatializeChannel`) for a bit-faithful mix | direct per-channel gains that bypass the 3D model (F3) |
| CHAN_* priority and stealing, up to 128 engine channels | 64 hardware voices, the rest in the CPU mixer (F5) |
| Music (MP3/OGG), VOX sentences, cinematic audio, voice chat | streamed source (F1) |
| Pause and resume (menu), restart on level change | pause/stop of all sources (exists), buffer release and reuse (F6) |
| Room effects (`room_type`: reverb, echo, underwater) | GP reverb (F8), later |
| Sound continues during loading screens | output pump independent of the frame loop (F4) |

## 3. Feature set (library side)

**F1 - Streaming: buffer queueing.**
- **API:** `alSourceQueueBuffers` / `alSourceUnqueueBuffers`, `AL_BUFFERS_QUEUED` / `AL_BUFFERS_PROCESSED`,
  and `AL_BUFFER = 0` to clear the queue.
- **Design:** one VP voice per streaming source, looping over a ring of N chunks in APU memory. The driver
  copies each queued buffer into the next free ring chunk and counts a buffer as processed when `CBO` leaves
  its chunk.
- **Why a ring:** it keeps a voice permanently looping, so it never reaches an end (no new hardware risk)
  and needs no record rewrite while it plays. The copy costs about 192 KB/s at 48 kHz stereo.
- **Underrun:** when the queue runs dry, the ring is filled with silence and the source reports
  `AL_STOPPED` once the last real chunk has played. The VP's native stream lists (`VPSSLADDR`) are untested
  on hardware and not needed.

**F2 - Loop points.** `AL_SOFT_loop_points` (`AL_LOOP_POINTS_SOFT`, start and end frame per buffer). The VP
does this natively: the voice starts at 0 and wraps from `EBO` (loop end) to `LBO` (loop start). Cue-point
sounds then need neither queueing nor splitting.
- **Stopping a cue loop:** HL lets such a sound play out past the loop end. Implement this by rewriting
  `LBO`/`EBO` to the silent tail. Writing those two fields of a playing record needs a probe first (the same
  write-safety question as the one-shot freeze).

**F3 - Direct channel gains.**
- **API:** a vendor extension `AL_XBOX_direct_gains`, with `alSourcefv(src, AL_DIRECT_GAINS_XBOX,
  {left, right})`. It sets the two output volumes directly and turns the source's 3D model off.
- **Why:** Xash keeps `S_SpatializeChannel` and the mix stays faithful to the reference.
- **Precision:** the VP volume has 12-bit attenuation in 1/64 dB steps. Converting HL's 0-255 volumes
  through it must be checked against the CPU mixer, with a tolerance stated in the test.

**F4 - Output pump thread.**
- **What it does:** forward the GP FIFO to the AC97 from a dedicated thread (`CreateThread`, about every
  5 ms) instead of only from `alXboxUpdateVoices()`.
- **Why:** sound survives loading screens and frame-time spikes.
- **Locking:** the voice records stay single-writer (the game thread). The pump touches only the FIFO and
  AC97 state.
- **Still per frame:** `alXboxUpdateVoices()` keeps the end scan and the voice manager.

**F5 - Voice budget and hybrid mixing.**
- **Voice cap:** `alcGetIntegerv(ALC_MONO_SOURCES)` reports 64.
- **Source-play flag:** a flag on `alSourcePlay` that never virtualizes the source, so the engine owns the
  priority decisions. CHAN_* rules stay in Xash.
- **Overflow:** channels beyond 64, and anything the APU cannot do, go through Xash's CPU mixer into one
  F1 stream source.
- **Routes 1 and 2:** that stream is also all route 1 (output only) needs.

**F6 - Sound memory.**
- **Loading:** `alBufferData` copies into contiguous APU memory today. Add `alBufferDataStatic` (the
  `AL_EXT_STATIC_BUFFER` shape), so the loader decodes straight into an allocation from
  `alXboxAllocSampleMemory()`.
- **Budget:** report APU memory use against a configurable budget, within the 8 MiB SGE space. Freeing a
  buffer frees its SGE pages; the allocator must also defragment or fail cleanly.
- **Measure first:** the size of a map's sound set (the "sound memory survey").

**F7 - Pitch and rate coverage.** A host test plus a hardware probe covering the extreme products:
11 025 Hz at pitch 0.01 (ratio about 0.0023) and 44 100 Hz at pitch 2.55 (about 2.3).
- **Field range:** the signed 4.12 log2 field nominally spans 1/256 to 256, so HL's lowest pitches fall
  below it and need clamping.
- **Probe:** which part of that span the VP actually honours, and whether high ratios cost VP time with
  64 voices playing. Results go into the doc.

**F8 - Room effects on the GP (later, optional).**
- **What:** a DSP56300 program mixing a reverb/echo send (one mixbin pair as the send bus) back into the
  output, with parameters for HL's `room_type` presets.
- **Cost and risk:** largest effort, and needs hardware microcode validation like the copy program.
- **Until then:** room types are a recorded divergence.

**F9 - Offsets.** `AL_SAMPLE_OFFSET` / `AL_SEC_OFFSET` / `AL_BYTE_OFFSET`. The getters read `CBO`. The setters
apply at the next play (record written before linking). These are needed for save/restore and for starting a
sentence mid-way.

## 4. Phases

Every phase ends with host tests green in both builds, a hardware run logged over FTP, and an entry in
`XEMU_VERIFICATION.md` 8.1b.

1. **Phase A - Robust output (F4, F1).**
   - **Build:** the pump thread, queueing on a ring, a new `samples/openal_stream` (tone and music streamed
     in 4 KiB chunks, deliberate starvation and recovery, 10-minute soak).
   - **Done when:** no underrun at steady state, and correct `AL_BUFFERS_PROCESSED` accounting (host test
     with a simulated `CBO`).
   - **Enables:** Xash route 1, the CPU mix through one stream source with a cvar to switch it, an A/B
     against the existing AC97/SDL path.
   - **Result (2026-10-09, retail console, `samples/openal_stream`):** 145 s across all six phases (48 kHz
     mono16, 22.05 kHz stereo16, 11.025 kHz mono8, starvation, 1.5 s main-thread stalls, soak) with 0 AC97
     underruns and 0 padded buffers. The longest gap between pump passes was 5 ms, also during the stalls;
     the static tone played through them and the stream went quiet without repeating. `openal_vp` and the
     showcase run unchanged on the pump thread.
2. **Phase B - Hardware voices for effects (F2, F3, F5, F7).**
   - **Build:** loop points, direct gains, the no-virtualize flag and the pitch range.
   - **Probes:** the playing-record `LBO`/`EBO` write, then a cue-loop WAV from HL itself.
   - **Done when:** a scripted sequence of HL sounds (gunshots, a looping machine with a cue, door, pitch
     sweep) matches the CPU mixer within the stated tolerance, captured from the GP FIFO.
3. **Phase C - Xash full backend.**
   - **Engine side:** each channel maps to a source, with music/VOX/cinematics and overflow in the CPU
     stream.
   - **Library side:** F6 static buffers and the memory budget.
   - **Measure on the console:** frame time with the CPU mixer against the APU, over the same demo.
   - **Done when:** the measured saving is reported, and every divergence is logged and reviewed.
4. **Phase D - Room effects (F8)**, only if phase C's divergence review says room types matter enough.
5. **Phase E (cleanup):** `alGetError` coverage for every new entry point, extension strings in `alGetString`,
   README and API docs.

## 5. Risks and open questions

- **Writes to a playing record:** loop-point changes and stopping a cue loop write `LBO`/`EBO` while the VP
  may hold the record. The one-shot freeze shows such writes can be unsafe; probe first. The fallback is a
  fresh handle restarted at the current `CBO`.
- **Overflow at 64 voices:** HL can request more than 64 channels in busy scenes. The CPU-mixer overflow
  keeps every channel audible, but the frame-time gain shrinks in exactly those scenes.
- **Fidelity:** the 12-bit log attenuation and the VP's interpolation differ from Xash's integer mixer;
  bit-identical output is not possible on hardware voices. The test tolerance and the reviewer's sign-off
  define "same".
- **Memory:** 64 MiB console, 8 MiB SGE space. If a map's sound set does not fit, the engine needs an
  eviction policy aware of playing voices.
- **No recovery from an APU freeze:** every new driver path gets a stress sample like
  `samples/apu_vp_stress` before Xash uses it.
- **xemu:** good for bring-up, not for these limits. xemu has no handle, SGE or end-of-voice freeze and
  ignores `dsp_step`. Every phase needs a console run.
