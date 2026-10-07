# nxdk OpenAL Showcase Tech Demo (`openal_showcase.xbe`)
## Hardware-Accelerated OpenAL 1.1 MCPX APU Demonstration

This interactive application demonstrates and validates all hardware capabilities of the **Nvidia MCPX Audio Processing Unit (APU)** running under the bare-metal **nxdk** SDK for the original Xbox.

All audio waveforms are standardized **16-bit 48 kHz PCM** (CC0 Public Domain Dedication), fully pre-converted and embedded into the executable so the demo runs standalone with zero filesystem dependencies.

---

## 1. Demonstration Modes

### Mode 1: 3D Positional Orbit & Headphone ITD/HRTF
- **Audio Asset:** `orbit_mono_48k.wav` (Mono 48kHz rich harmonic timbre).
- **APU Feature:** Voice Processor 3D spatial positioning, Woodworth Interaural Time Delay (ITD, $0\text{–}64$ samples circular buffer delay), and Q14 2nd-order Butterworth elevation/pinna biquad filtering.
- **Controls:**
  - **Left Thumbstick:** Up/Down adjusts orbit radius ($1.5\text{m} \dots 15\text{m}$); Left/Right adjusts angular orbit velocity.
  - **Right Thumbstick:** Rotates listener head orientation (Yaw and Pitch). Hear front/back pinna attenuation and ear-to-ear ITD phase delay on headphones!

### Mode 2: High-Speed Fly-by Doppler Pitch Shifter
- **Audio Asset:** `siren_doppler_48k.wav` (Mono 48kHz constant dual-tone horn/siren).
- **APU Feature:** Real-time APU polyphase pitch resampler (16.16 fixed-point format).
- **Controls:**
  - **Right Trigger (RT) or Button A:** Launches a high-speed projectile ($42\text{ m/s} = 151\text{ km/h}$) from $x = -35\text{m}$ passing inches from the listener's head at $z = -1.2\text{m}$.
  - The APU dynamically modulates playback pitch ($f > f_0$ approaching, sharp glissando pitch drop as it passes, $f < f_0$ receding).

### Mode 3: Multichannel 5.1 Surround & LFE Discrete Channel Test
- **Audio Assets:** `orbit_mono_48k.wav`, `voice_center_48k.wav` (Center channel radio voice), `explosion_lfe_48k.wav` (Deep sub-bass impact).
- **APU Feature:** Pairwise equal-power 5.1 multichannel panning, Output Processor (EP) FIFO routing, discrete subwoofer LFE routing (`AL_XBOX_LFE_GAIN`), and hardware Dolby Digital DSE real-time AC-3 bitstream encoding over optical TOSLink.
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
- **APU Feature:** APU Voice Virtualization Manager multiplexing 128 virtual sources over the 64 physical hardware slots, dynamic distance/gain priority stealing, and click-free preemption (instant gain zeroing before halting).
- **Controls:**
  - **Hold Button A:** Spawns streams of $100+$ spatialized laser sources. Watch the on-screen hardware counter hit $64/64$ active channels while CPU mixing load remains at $0.0\%$, sustaining a rock-solid 50/60 FPS.

### Mode 5: Concurrent 2D Direct Stereo Music & 3D Spatial Audio
- **Audio Asset:** `bgm_stereo_48k.wav` (Stereo 48kHz 2-channel ambient synthwave loop).
- **APU Feature:** Simultaneous direct 2D stereo DMA playback (MixBins 0 & 1) running concurrently alongside active 3D positional emitters, validating that APU DMA bus bandwidth easily handles combined streams.
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
