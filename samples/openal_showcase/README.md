# nxdk OpenAL Showcase Tech Demo (`openal_showcase.xbe`)
## OpenAL Demonstration over the MCPX APU Voice-Layer Model

This interactive application exercises the nxdk OpenAL library (an OpenAL 1.1 subset over a software model of an **Nvidia MCPX Audio Processing Unit (APU)** voice layer, see `lib/openal/README.md`) under the bare-metal **nxdk** SDK for the original Xbox.

**Status:** the library's hardware register layer is not yet conformant to xemu or the real MCPX APU (see `lib/openal/docs/XEMU_VERIFICATION.md`). By default the library runs on its software model and does not drive the APU, so this demo does not exercise real APU voice processing, and none of the hardware behaviour described below is verified against xemu or silicon.

All audio waveforms are standardized **16-bit 48 kHz PCM** (CC0 Public Domain Dedication), fully pre-converted and embedded into the executable so the demo runs standalone with zero filesystem dependencies.

---

## 1. Demonstration Modes

### Mode 1: 3D Positional Orbit & Headphone ITD/HRTF
- **Audio Asset:** `orbit_mono_48k.wav` (Mono 48kHz rich harmonic timbre).
- **Library Feature:** 3D spatial positioning with a Woodworth Interaural Time Delay (ITD, $0\text{–}31$ samples: the formula is clamped to 64 but never exceeds 31) and a Q14 2nd-order Butterworth elevation/pinna biquad, computed on the CPU and stored in the library's voice-context model. These are model values: xemu's Voice Processor implements HRTF as a 31-tap FIR plus an ITD of up to $\pm 42$ samples loaded through front-end methods, so this model is not validated against hardware.
- **Controls:**
  - **Left Thumbstick:** Up/Down adjusts orbit radius ($1.5\text{m} \dots 15\text{m}$); Left/Right adjusts angular orbit velocity.
  - **Right Thumbstick:** Rotates listener head orientation (Yaw and Pitch). The library computes the front/back filter and the ear-to-ear ITD for the new orientation and the demo shows the source position and azimuth on screen; the default backend outputs no sound, so none of it is audible.

### Mode 2: High-Speed Fly-by Doppler Pitch Shifter
- **Audio Asset:** `siren_doppler_48k.wav` (Mono 48kHz constant dual-tone horn/siren).
- **Library Feature:** Doppler pitch shift computed in software and expressed as the library's linear 16.16 pitch step (a model value: the hardware pitch field is a signed 4.12 log2 value).
- **Controls:**
  - **Right Trigger (RT) or Button A:** Launches a high-speed projectile ($42\text{ m/s} = 151\text{ km/h}$) from $x = -35\text{m}$ passing inches from the listener's head at $z = -1.2\text{m}$.
  - The library computes a Doppler pitch multiplier ($f > f_0$ approaching, sharp drop as it passes, $f < f_0$ receding) and shows it on screen; the default backend outputs no sound, so the glissando is not audible.

### Mode 3: Multichannel 5.1 Surround & LFE Discrete Channel Test
- **Audio Assets:** `orbit_mono_48k.wav`, `voice_center_48k.wav` (Center channel radio voice), `explosion_lfe_48k.wav` (Deep sub-bass impact).
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
- **Audio Asset:** `laser_stress_48k.wav` (Mono 48kHz transient dry laser chirp, 100ms).
- **Library Feature:** Voice virtualization manager multiplexing 128 virtual sources over the library's 64 voice slots with distance/gain priority stealing. A preempted voice has its gains zeroed and is halted in the same call, i.e. an abrupt cut, not a fade (xemu applies volume changes as steps, with no ramp; hardware ramping is unverified).
- **Controls:**
  - **Hold Button A:** Spawns streams of $100+$ spatialized laser sources. Watch the on-screen hardware counter reach $64/64$ active slots. The demo makes no CPU-load or frame-rate claim; `samples/openal_benchmark` measures the CPU cost of the software spatial math.

### Mode 5: Concurrent 2D Direct Stereo Music & 3D Spatial Audio
- **Audio Asset:** `bgm_stereo_48k.wav` (Stereo 48kHz 2-channel ambient synthwave loop).
- **Library Feature:** A 2D stereo source (MixBins 0 & 1 in the library's model) playing concurrently with active 3D positional emitters.
- **Controls:**
  - **Button Y or Button A:** Toggle 2D stereo background music play/pause.

---

## 2. Controller Button Map

| Button | Action |
| :--- | :--- |
| **White / D-Pad Right** | Next Demonstration Mode |
| **Black / D-Pad Left** | Previous Demonstration Mode |
| **Left Thumbstick** | Mode 1: Adjust Orbit Distance & Speed |
| **Right Thumbstick** | Mode 1: Rotate Listener Camera (Yaw & Pitch) |
| **Right Trigger (RT) / A** | Mode 2: Launch Doppler Projectile |
| **D-Pad Up / Down / A** | Mode 3: Cycle Discrete 5.1 Channels |
| **Hold Button A** | Mode 4: Spawn Polyphony Stress Emitters |
| **Button Y / A** | Mode 5: Toggle 2D Stereo Music Play/Pause |
| **Back + Start** | Exit Demo |

*Note: If no controller is connected (e.g., in automated testing), the demo automatically runs in **Autonomous Tour Mode**, cycling modes every 6 seconds.*

---

## 3. Building & Deployment

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

**Note:** the library's default (null) backend does not drive the APU, so the demo produces no sound under xemu or on a console. Do not build the library with `-DOPENAL_APU_REAL_MMIO` for xemu: its register layer is not xemu-conformant and device bring-up is expected to abort the emulator (static analysis, not run; see `lib/openal/docs/XEMU_VERIFICATION.md`).
