# Closing the gap with OpenAL Soft: plan

Branch `feat/openal-soft-gap`. Workflow phase 9 (`.agents/workflow/state.json`). This follows the EFX work
(`EFX_PLAN.md`) and adds what OpenAL Soft offers that this hardware can do.

## 1. What cannot be closed

- **Output:** stereo 16-bit on the analog path; no 5.1 or Dolby Digital.
- **HRTF:** the APU's computed model, not OpenAL Soft's measured data sets.
- **Effect count:** the GP's memory caps it (0x700 code words, X memory, about 180 KiB scratch, 60 DMA
  descriptors a frame).
- **Highpass and bandpass on the dry path:** the voice filter has no highpass mode (`EFX_PLAN.md` 2.1).
- **Token values:** the library's own Xbox values (0x7xxx) stay.

## 2. Tasks

| Task | What | API |
|---|---|---|
| phase9-01 | Air absorption and cone outer GAINHF on the voice lowpass, with `AL_DIRECT_FILTER_GAINHF_AUTO`. **Done** | none (EFX tokens) |
| phase9-02 | Stereo sends into a custom kernel at full gain per channel (Xash port request: an exact 0 dB transport). **Done** (#47) | none (behaviour) |
| phase9-03 | **Done** (c53a77f, 1258140). OpenAL Soft extensions done on the CPU: `AL_SOFT_deferred_updates`, `AL_SOFT_source_latency`, `AL_SOFT_direct_channels`, `AL_SOFT_source_spatialize`, `AL_EXT_source_distance_model`, `AL_SOFT_gain_clamp_ex`, `AL_EXT_FLOAT32` (converted at upload), `ALC_SOFT_pause_device`, `ALC_SOFT_HRTF` (switches the APU HRTF) | adds the extensions' own tokens |
| phase9-04 | Several effects at once: relocatable kernel images (`dsp56asm.py` relocations), a GP loop that runs one kernel per active slot, slot k on bins 2+2k / 3+2k, sends routed by `VBIN` with a volume ramp on a move, shared budgets checked at attach. **Done** on the host (image ABI 2; loop 209 words with 4 trampolines and the timed mailbox; reverb, echo and chorus relocated and together in the simulator, each adding exactly what it adds alone, about 15600 instructions a frame; first-fit loader; `GP_KERNELS.md`); console check in phase9-08 | `AL_INVALID_OPERATION` only when the budgets are full |
| phase9-05 | `AL_EFFECT_ECHO` as a library kernel (delay, LR delay, damping, feedback, spread). **Done** on the host (`dsp/gp_kefx_echo.asm`: 169 words, 3 descriptors, 128 KiB scratch, about 1940 instructions a frame in the simulator); console check in phase9-08 | EFX tokens (+ the echo's min / max / default constants in `efx.h`) |
| phase9-06 | `AL_EFFECT_CHORUS` and `AL_EFFECT_FLANGER`. **Done** on the host (`dsp/gp_kefx_chorus.asm`: 171 words, one kernel for both, the line in Y RAM instead of scratch, about 4900 instructions a frame in the simulator, within 4 LSB of a floating-point model); console check in phase9-08 | EFX tokens (+ the chorus's and flanger's min / max / default constants in `efx.h`) |
| phase9-07 | On-chip effects: ring modulator, distortion, equalizer, compressor, autowah. **Done** on the host (five library kernels after OpenAL Soft's algorithms, none with Y, scratch or DMA; `GP_KERNELS.md` 10). In the simulator: ring modulator `gp_kefx_ringmod.asm` 166 words, X $53, about 3500 instructions a frame; distortion `gp_kefx_distortion.asm` 169 / $33, 4200-4900; equalizer `gp_kefx_equalizer.asm` 92 / $44, 600 with every band at gain 1, 2400 with one band, 6400 with four; compressor `gp_kefx_compressor.asm` 103 / $2B, 1900; autowah `gp_kefx_autowah.asm` 247 / $3C, 7200. Against OpenAL Soft's algorithm in double precision, on the library's words: within 0.3, 0.65, 0.04, 0.05 and 3.3 LSB of 16 bits (the autowah's words move every sample; OpenAL Soft's own single precision lands as far from the same model). The reverb and any three of them fit the budgets, any four of them too; the reverb, a four-band equalizer, the distortion and the autowah together about 27500 instructions a frame. Console check in phase9-08 | EFX tokens (+ the five effects' min / max / default constants in `efx.h`) |
| phase9-08 | Sample, docs, console run. **Done: console run 2026-10-27, 45 of 45 checks** (every on-chip effect within its tolerance of the simulator, four slots at once at a GP load peak of 57 %, the budget refusal, 5 minutes of rotation without an underrun; the first run's one failure was the test's own floor, fixed). Sample: `samples/openal_efx` 6i (ring modulator, distortion, equalizer, compressor, autowah against the host simulator, `expected_6i.py`), 6j (four slots at once, the budget refusal), the 7 rotation through every effect and `XReboot()` after it; every log line written through to E:, a `step:` line per section, a 20 s watchdog with an SMC reset. xemu: flow, attaches and refusal pass, the captured mix is silent there (`EFX_PLAN.md` 2.2) | - |

The order puts the cheap, host-testable items first. phase9-04 needs an assembler change before any kernel
can run beside another, so phase9-05..07 are written single-slot first and gain relocation with it.

## 3. Testing

- Host: `tests/run_tests.py` (a test per task); kernels in `tools/dsp56sim.py` against C references.
- Console: one sample run per task that changes what the hardware does; results in `EFX_PLAN.md` 2.1 style.
