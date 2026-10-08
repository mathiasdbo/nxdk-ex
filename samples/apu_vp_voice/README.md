# apu_vp_voice: MCPX APU Voice Processor bring-up (route A, stage 1)

Drives the MCPX APU's Voice Processor directly, following the bring-up sequence in
`lib/openal/docs/XEMU_VERIFICATION.md` (section 7.2, steps 1-13 and 17-24) and the corrected register model in
`lib/openal/src/mcpx_apu_regs.h`. It is the first step of route A in that document's roadmap (section 8.2):
one PCM16 mono buffer voice, no DSP code.

It plays, in a loop:

1. a 1 s 440 Hz one-shot at 48 kHz (pitch 0);
2. the same buffer a fifth higher (pitch `round(4096 * 7/12)` = 2389, 4.12 log2);
3. the buffer looped an octave lower (pitch -4096), switched off with `VOICE_OFF` after 3 s.

For each, the screen shows whether the VP advanced `CBO`, cleared `ACTIVE_VOICE`, wrote the voice's notifier
byte, and whether the idle-voice trap was serviced and the voice unlinked from the 2D list (PASS/FAIL).

## Where it is audible

* **xemu** with "Real-time DSP processing" off (the default): xemu taps the VP output directly, so the tones
  are heard. Verified: all three steps pass and xemu does not abort.
* **Real console:** the VP only writes mixbins; turning them into output needs GP/EP DSP firmware, which is a
  later stage. Expect silence, but the PASS/FAIL lines still show whether the VP ran on real silicon.

## Building

```bash
cd samples/apu_vp_voice
make
```
