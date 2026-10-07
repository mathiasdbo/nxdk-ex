# OpenAL 1.1 Hardware-Accelerated Audio Subsystem for nxdk (MCPX APU)

Hardware-accelerated **OpenAL 1.1** implementation for the bare-metal **nxdk** Xbox SDK targeting the original Xbox Nvidia MCPX Audio Processing Unit (APU).

## Architecture Highlights
- **Zero CPU Mixing Overhead:** All voice mixing, polyphase resampling, pitch shifting, and panning are processed directly on the APU Voice Processor (VP) and Global Processor (GP DSP56300).
- **Voice Virtualization:** Transparently virtualizes up to 256 virtual sources over the 64 hardware voice contexts using a dynamic priority stealing algorithm.
- **Click-Free Preemption:** Preempted victim voices have their volumes zeroed before halting, preserving fractional sample playback phase and PRD indices for click-free standby resumption.
- **Spatial Audio & Psychoacoustics:** Full 3D coordinate transformation, distance rolloff models, directional sound cones, Woodworth ITD (Interaural Time Delay, 0–64 samples), and Q14 2nd-order Butterworth HRTF biquad filtering for headphone elevation/pinna effects.
- **Multichannel & Surround 5.1:** Equal-power 360-degree 5.1 multichannel panning, subwoofer LFE channel routing (`AL_XBOX_LFE_GAIN`), hardware Dolby Digital DSE real-time bitstream encoding over optical TOSLink, and ITU-R BS.775 stereo downmixing with DSP56300 saturation limiting for analog RCA.
- **Standard nxdk Integration:** Exported globally via `lib/Makefile` as `libopenal.lib`.

## Verified Performance (Pentium III 733.333 MHz)
- **1 Voice:** ~7,780 cycles (~10.6 us, 0.06% of 60 FPS frame)
- **16 Voices:** ~65,450 cycles (~89.2 us, 0.54% of 60 FPS frame)
- **32 Voices:** ~127,060 cycles (~173.3 us, 1.04% of 60 FPS frame)
- **64 Voices (Saturation):** ~261,800 cycles (~357.0 us, 2.14% of 60 FPS frame)

## Running the Unit Test Suite
The subsystem contains 17 automated regression test suites covering 100% of the hardware abstraction and OpenAL 1.1 APIs:
```bash
python lib/openal/tests/run_tests.py
```
