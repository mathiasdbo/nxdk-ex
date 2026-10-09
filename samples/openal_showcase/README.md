# nxdk OpenAL Showcase Tech Demo (`openal_showcase.xbe`)
## OpenAL Demonstration over the MCPX APU Voice-Layer Model

This interactive application exercises the nxdk OpenAL library (an OpenAL 1.1 subset over a software model of an **Nvidia MCPX Audio Processing Unit (APU)** voice layer, see `lib/openal/README.md`) under the bare-metal **nxdk** SDK for the original Xbox.

**Status:** the default build plays the sources on the real MCPX APU Voice Processor (section 4). This path is verified with `samples/openal_vp` on a retail console and on xemu (see `lib/openal/docs/XEMU_VERIFICATION.md`). The per-mode descriptions below come from the library's spatial model:
- **Applied on a console:** the 5.1 pan, distance, cone and Doppler-scaled pitch (the 5.1 pan folded to stereo).
- **Not applied on a console:** the ITD and elevation biquad. HRTF is xemu-only for now.

All audio is **16-bit 48 kHz PCM**, pre-converted and embedded into the executable so the demo runs standalone with no filesystem dependencies. The sound effects and music are CC0 recordings from Freesound (helicopter, siren, explosion, rifle shot, breaking glass, music; sources, licenses and edits in `assets/sfx/sounds.txt`, converted by `assets/sfx/make_sfx_assets.py` (Python 3 + ffmpeg) into `sound_assets_sfx.h`); the laser chirp and the center-channel voice are synthesized (CC0, `generate_assets.py`).

---

## 1. Demonstration Modes

### Mode 1: 3D Positional Orbit & Headphone ITD/HRTF
- **Audio Asset:** `helicopter_loop_48k.wav` (UH-60 Black Hawk hover loop, mono).
- **Library Feature:** 3D spatial positioning with a Woodworth Interaural Time Delay (ITD, $0\text{–}31$ samples: the formula is clamped to 64 but never exceeds 31) and a Q14 2nd-order Butterworth elevation/pinna biquad, computed on the CPU and stored in the library's voice-context model. These are model values: xemu's Voice Processor implements HRTF as a 31-tap FIR plus an ITD of up to $\pm 42$ samples loaded through front-end methods, so this model is not validated against hardware.
- **Controls:**
  - **Left Thumbstick:** Up/Down adjusts orbit radius ($1.5\text{m} \dots 15\text{m}$); Left/Right adjusts angular orbit velocity.
  - **Right Thumbstick:** Rotates listener head orientation (Yaw and Pitch). The library computes the front/back filter and the ear-to-ear ITD for the new orientation and the demo shows the source position and azimuth on screen; the software mixer (section 4) makes the level difference, the ear delay and the filter audible on stereo output.

### Mode 2: High-Speed Fly-by Doppler Pitch Shifter
- **Audio Asset:** `siren_loop_48k.wav` (siren, mono loop).
- **Library Feature:** Doppler pitch shift computed in software and expressed as the library's linear 16.16 pitch step (a model value: the hardware pitch field is a signed 4.12 log2 value).
- **Controls:**
  - **Right Trigger (RT) or Button A:** Launches a high-speed projectile ($42\text{ m/s} = 151\text{ km/h}$) from $x = -35\text{m}$ passing inches from the listener's head at $z = -1.2\text{m}$.
  - The library computes a Doppler pitch multiplier ($f > f_0$ approaching, sharp drop as it passes, $f < f_0$ receding) and shows it on screen; the software mixer (section 4) applies it, so the glissando is audible.

### Mode 3: Multichannel 5.1 Surround & LFE Discrete Channel Test
- **Audio Assets:** `helicopter_loop_48k.wav` (satellite speakers), `voice_center_48k.wav` (synthesized center-channel voice), `explosion_48k.wav` (explosion on the subwoofer channel).
- **Library Feature:** Pairwise equal-power 5.1 multichannel panning onto MixBins 0..5 (this library's convention, not verified against hardware) and discrete subwoofer LFE routing (`AL_XBOX_LFE_GAIN`). The library also writes EP/Dolby configuration to model registers; no AC-3 encoder or Dolby Digital output is implemented, and xemu models neither, so this cannot be validated there.
- **Controls:**
  - **D-Pad Up / Down or Button A:** Step through each discrete speaker channel:
    - `[0]` Front Left (MixBin 0)
    - `[1]` Center (MixBin 4) — Voice snippet anchored strictly to Center speaker.
    - `[2]` Front Right (MixBin 1)
    - `[3]` Surround Right (MixBin 3)
    - `[4]` Surround Left (MixBin 2)
    - `[5]` Subwoofer LFE (MixBin 5) — Deep punch routed exclusively to the subwoofer.

### Mode 4: 64-Voice Polyphony & Priority Stealing Stress Test
- **Audio Assets:** `laser_stress_48k.wav` (synthesized 100 ms laser chirp), with `rifle_shot_48k.wav` and `glass_break_48k.wav` in every sixth source each; the longer one-shots fill the 64 voice slots faster and make voice stealing audible.
- **Library Feature:** Voice virtualization manager multiplexing 128 virtual sources over the library's 64 voice slots with distance/gain priority stealing. A preempted voice has its gains zeroed and is halted in the same call, i.e. an abrupt cut, not a fade (xemu applies volume changes as steps, with no ramp; hardware ramping is unverified).
- **Controls:**
  - **Hold Button A:** Spawns streams of $100+$ spatialized laser sources. Watch the on-screen hardware counter reach $64/64$ active slots. The demo makes no CPU-load or frame-rate claim; `samples/openal_benchmark` measures the CPU cost of the software spatial math.

### Mode 5: Concurrent 2D Direct Stereo Music & 3D Spatial Audio
- **Audio Asset:** `music_loop_48k.wav` ("Space Dance 2" by szegvari, a 4-bar 120 BPM stereo loop).
- **Library Feature:** A 2D stereo source (MixBins 0 & 1 in the library's model) playing concurrently with active 3D positional emitters.
- **Controls:**
  - **Button Y or Button A:** Toggle 2D stereo background music play/pause.

---

## 2. Controller Button Map

| Button | Action |
| :--- | :--- |
| **White / D-Pad Right** | Next Demonstration Mode |
| **Black / D-Pad Left** | Previous Demonstration Mode |
| **Left Thumbstick** | Mode 1: Adjust Orbit Distance & Speed (walk mode: walk / strafe instead) |
| **Button X** | All modes: Walk mode on/off. While on, the left stick walks forward/back and strafes (3 m/s, in the direction the head faces); the listener velocity is passed to OpenAL, so walking towards or away from a source shifts its pitch |
| **Button B** | All modes: Reset the listener to the centre, facing forward |
| **Right Thumbstick** | All modes: Turn the Listener's Head (Yaw & Pitch) |
| **Right Trigger (RT) / A** | Mode 2: Launch Doppler Projectile |
| **D-Pad Up / Down / A** | Mode 3: Cycle Discrete 5.1 Channels |
| **Hold Button A** | Mode 4: Spawn Polyphony Stress Emitters |
| **Button Y / A** | Mode 5: Toggle 2D Stereo Music Play/Pause |
| **Back + Start** | Exit Demo |

*Note: If no controller is connected (e.g., in automated testing), the demo automatically runs in **Autonomous Tour Mode**, cycling modes every 6 seconds.*

The same map is shown on screen: the **CONTROLS** legend at the bottom lists the current mode's inputs on the left and the global ones (next/previous mode, exit) on the right, each with a drawing of the button (A/B/X/Y in their colours, White/Black, D-Pad with the active arms lit, thumbsticks, triggers, Back/Start). Controls that must be held are tagged **HOLD**. The legend comes from `showcase_app_get_legend()` in `showcase_modes.c`, next to the input handling it describes.

---

## 3. NV2A 3D View

Each frame is drawn on the NV2A through pbkit (`showcase_scene.c`), so the spatial values the library computes can be seen while they change:

- **Scene:** a floor grid with the listener at the origin, a chase camera behind the listener's head (what is in front of the listener is in front of the camera, so screen left/right match the left/right ears), the head with its left (blue) and right (red) ears and a nose that follows yaw and pitch.
- **Sources:** each playing source is a diamond with a line down to the floor, rings that ripple outward while it plays, and a line to the listener that fades with the source's gain. Mode-specific extras: the orbit path (Mode 1); the flight path and launcher, with the projectile tinted blue while approaching and red while receding (Mode 2); the six speaker cabinets with the active one lit (Mode 3); one cube per stress source, green while it holds a hardware voice slot and orange while it waits virtualized (Mode 4).
- **Distance rings:** the focus source's `AL_REFERENCE_DISTANCE` (green) and `AL_MAX_DISTANCE` (orange) on the floor.
- **Radar:** top-down, listener-relative (facing up), so a source's azimuth reads directly.
- **Spatial model panel:** for the focus source, the values from `al_source_calc_spatial()`: distance, azimuth/elevation, effective gain, Doppler pitch multiplier, the left/right ITD delays in samples, and the six 5.1 MixBin gains. Mode 4 shows the 64 voice slots instead.
- **Performance line** (status panel): frames per second, CPU time to build the 3D frame and to mix the audio (both smoothed), sources mixed, and AC97 underruns (chunks played as silence because the mix fell behind; shown in orange when non-zero). The main loop runs once per video frame (paced by the vertical blank) and moves everything by the measured frame time, so speeds are the same at 50 and 60 Hz.
These are the library's model values: what the library would send to the APU. What you hear is the software mix of those values (section 4), not APU output.

The scene is built on the CPU into one vertex buffer of screen-space positions and colours and drawn back to front (painter's algorithm, back faces skipped), with the same colour-passthrough Cg shaders as `samples/triangle`. Text uses the public-domain 8x8 font that ships with nxdk (`lib/hal/font_terminal.h`). The frame build is plain C and is covered on the host by `lib/openal/tests/test_showcase_scene.c`.

---

## 4. Sound: the MCPX APU, or a Software Mix to AC97

The status panel says which output is in use.

**APU (default build, `SHOWCASE_BACKEND=apu`).** The OpenAL library is compiled into the sample with `-DOPENAL_APU_REAL_MMIO` (one wrapper per library source in `apu/`, as in `samples/openal_vp`). `alcOpenDevice()` then brings up the MCPX APU, and every source that holds a voice slot plays on a hardware Voice Processor voice. The library writes the voice records itself; see `lib/openal/src/apu_vp.h`.

- **On a console:** the GP DSP copies the mix to a FIFO, and `alXboxUpdateVoices()` forwards it to the AC97. HRTF needs front-end methods, which do not run on hardware, so 3D sources are panned by volume and the 5.1 pan is folded to stereo.
- **On xemu:** sources use the HRTF stage. With "Real-time DSP processing" off, xemu plays the VP output itself.
- **Status panel:** the "mix" time is the library's frame tick, and "src" counts active VP voices.

**CPU mixer (`make SHOWCASE_BACKEND=cpu`, or if the APU does not come up).** The library runs its null backend, which computes the spatial model but drives no audio hardware. The showcase renders the sources itself on the CPU and plays them through nxdk's `XAudio` AC97 output (`showcase_audio.c`). It uses the library's own values, so what you hear is what the screen shows:

- **Which sources:** every playing source that holds one of the 64 voice slots, loudest 32 first. A source waiting virtualized in the voice manager is silent, as it would be on the hardware, so voice stealing in Mode 4 is audible.
- **Per source:** resampling by the source pitch and the library's Doppler factor; the library's Q14 elevation/pinna biquad; each ear delayed by the library's Woodworth ITD taps; the library's 5.1 pan gains folded to stereo (FL+SL, FR+SR, centre and LFE to both) with 35 % crossfeed so the far ear still hears a hard-panned source; the effective gain (distance, cone, listener). Gains ramp across each chunk, so movement does not click.
- **2D stereo buffers** (Mode 5 music) play straight to left and right.
- **One-shots:** when a non-looping buffer runs out, the mixer clears the voice's ACTIVE bit in the null backend's register stand-in, as the voice processor would, so the library reports `AL_STOPPED` and frees the slot.
- **CPU instructions:** resampling, the biquad and the ITD delay line are scalar (each sample depends on the previous one). Summing the sources into the 32-bit stereo bus and the saturating conversion to 16-bit run as MMX (`pmullw`/`pmulhw`/`paddd`/`packssdw`) on the Pentium III, written as inline assembly because clang's MMX intrinsics now require SSE2. A scalar version gives bit-identical output (`-DSHOWCASE_AUDIO_SCALAR`), which `lib/openal/tests/test_showcase_audio.c` checks.
- **Timing:** every main-loop frame reads the codec's current and last-valid descriptor indices and mixes 512-frame chunks straight into the free DMA buffers until 8 chunks (about 85 ms) are queued, restarting the DMA if it ever halted (counted as an underrun). No XAudio callback is used: nxdk's driver stops scheduling it after the output drains once, which silenced the sound after about a second.

Output is stereo, 16-bit, 48 kHz with either backend. 5.1 and Dolby Digital are not produced.

---

## 5. Building & Deployment

### Building with nxdk:
```bash
cd samples/openal_showcase
make
```
This generates:
- `nxdk sample - openal_showcase.xbe`
- `nxdk sample - openal_showcase.iso`

### Running on xemu Emulator:
```bash
xemu -dvd_path "nxdk sample - openal_showcase.iso"
```
Or FTP `openal_showcase.xbe` to your Xbox console dashboard (`E:\Games\OpenAL Showcase\default.xbe`).

**Note:** the default build plays through the APU on both xemu and a console (section 4). Build with `make SHOWCASE_BACKEND=cpu` for the software mixer only.
