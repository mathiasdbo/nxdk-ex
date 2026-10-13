# OpenAL for nxdk (hardware-accelerated on the MCPX APU)

An OpenAL 1.1 implementation for the **nxdk** Xbox SDK that plays through the original Xbox's Nvidia MCPX Audio
Processing Unit (APU):
- **Voices:** each playing source runs on a hardware voice of the APU's voice processor (VP): resampling, volume,
  HRTF.
- **Mixing and effects:** the APU's GP DSP runs our own program; it forwards the mix and can add a room reverb.
- **Output:** a library thread moves the result to the AC97 codec every 4 ms.

It is a clean-room implementation: no Microsoft firmware, XDK code or state left by other software; hardware
facts come from public sources (xemu, XboxDevWiki) and our own console probes.

**Status:** verified on a retail console:
- streaming, loop points, offsets, 120 voices at once;
- HRTF (64 voices), the GP reverb;
- clean program exit.

The measurements are in [`docs/XEMU_VERIFICATION.md`](docs/XEMU_VERIFICATION.md) section 8.1b. For using it from
a game engine, start with [`docs/ENGINE_INTEGRATION.md`](docs/ENGINE_INTEGRATION.md); the Xash3D port plan is
[`docs/XASH3D_INTEGRATION_PLAN.md`](docs/XASH3D_INTEGRATION_PLAN.md).

## Backends

| Backend | Selected by | What it does |
|---|---|---|
| **Real APU** | compile the library sources into the program with `-DOPENAL_APU_REAL_MMIO` | Drives the APU at BAR0 (`0xFE800000`). On a console: voice records written directly, the GP program, AC97 output through the library's thread (the route measured in 8.1b). On xemu: the same voices through the APU's front-end methods. This is the backend for games |
| **Null (software model)**, the default without the define | nothing | A RAM stand-in for the APU registers. Sources, buffers, spatial math and the voice manager work and are what the host tests exercise; **no sound**, and a one-shot source stays `AL_PLAYING` until stopped (no hardware reports its end) |

`alXboxGetHardwareStatus(AL_XBOX_BACKEND, &v)` returns `AL_XBOX_BACKEND_MMIO` or `AL_XBOX_BACKEND_NULL`;
`alGetString(AL_RENDERER)` names it too.

**Building for the real APU.** Compile each library source into the program with the define, as the samples do:
one wrapper per source in `samples/<name>/mmio/*.c` (`#include "../../../lib/openal/src/<file>.c"`) and
`-DOPENAL_APU_REAL_MMIO` on those objects only (`samples/openal_engine/Makefile`). Copy that pattern; a new
library source needs a new wrapper. The shared `libopenal.lib` built through `lib/Makefile` is the null backend.

## Per-frame update

Call `alXboxUpdateVoices()` once per game frame (`alcProcessContext()` does the same). It:
- ends one-shots whose data has played: they become `AL_STOPPED` up to one frame late;
- refills streaming sources;
- moves sources waiting in virtual standby onto free voices.

The sound itself does not depend on it: the output thread keeps the codec fed every 4 ms, also through loading
screens.

## API

- **ALC:** `alcOpenDevice` (`NULL`, the default name, `"MCPX APU"`, `"DirectSound3D"`, `"DirectSound"`),
  `alcCloseDevice`, `alcCreateContext`, `alcMakeContextCurrent`, `alcDestroyContext`, `alcGetCurrentContext`,
  `alcGetContextsDevice`, `alcGetError`, `alcGetString`, `alcGetIntegerv`, `alcProcessContext` (the voice tick).
  `alcSuspendContext` and capture are stubs.
- **Buffers:** `alGenBuffers`, `alDeleteBuffers`, `alIsBuffer`, `alBufferData` (mono/stereo, 8-bit unsigned or
  16-bit signed PCM, any rate), `alBufferDataStatic` (the same, without the copy), `alGetBufferi`, `alBufferiv`/`alGetBufferiv` for `AL_LOOP_POINTS_SOFT`.
  2048 buffers.
- **Sources:** the OpenAL 1.1 attributes (position, velocity, direction, gain and limits, pitch, looping, relative,
  cones, distances, rolloff, `AL_BUFFER`, state, the three offsets), buffer queueing, play/pause/stop/rewind and
  the vector forms. 256 sources, 120 on hardware voices at once on a console (64 on xemu).
- **Listener:** position, velocity, orientation, gain.
- **Global:** `alDistanceModel` (all models), `alDopplerFactor`, `alSpeedOfSound`, `alGetError`, `alGetString`,
  `alGetInteger*`, `alGetFloat*`, `alEnable`/`alDisable`/`alIsEnabled` (`AL_XBOX_HRTF`, `AL_XBOX_REVERB`).

## Extensions (`include/AL/alext.h`)

`alGetString(AL_EXTENSIONS)` lists them; every entry point is also returned by `alGetProcAddress`.

| Extension | What it adds |
|---|---|
| `AL_EXT_OFFSET` | `AL_SEC_OFFSET` / `AL_SAMPLE_OFFSET` / `AL_BYTE_OFFSET`, read and set (also while playing) |
| `AL_EXT_STATIC_BUFFER` | `alBufferDataStatic`: a buffer plays the application's memory in place, any range of it (3.10) |
| `AL_SOFT_loop_points` | a buffer's loop section: play the intro once, then repeat `[start, end)` in hardware |
| `AL_XBOX_hardware_status` | `alXboxGetHardwareStatus`: voices, free voices, sample memory, backend, AV pack, HRTF / reverb running |
| `AL_XBOX_source_control` | engine control of voices: direct left/right gains, priority, no-standby, has-voice |
| `AL_XBOX_hrtf` | the APU's HRTF stage for mono 3D sources (on by default; `ENGINE_INTEGRATION.md` 3.5) |
| `AL_XBOX_reverb` | a room reverb on the GP DSP, eight presets, a send per source (`ENGINE_INTEGRATION.md` 3.7) |
| `AL_XBOX_stats` | `alXboxGetStats`: CPU cycles of the output thread and of the voice tick, AC97 buffers, underruns (3.8) |
| `AL_XBOX_output_callback` | `alXboxOutputCallback`: a function that sees, and may change, every block of the final mix (3.9) |
| `AL_XBOX_update` | `alXboxUpdateVoices`, the per-frame tick |

## Limits

See the table in `docs/ENGINE_INTEGRATION.md` section 2. In short:
- **Output:** stereo 16-bit 48 kHz only. No 5.1 or Dolby Digital; the 5.1 panning and topology code is model
  state only.
- **Formats:** no ADPCM, no multichannel buffers.
- **Pitch:** playback rate 1/16 to 4 times 48 kHz.
- **Threads:** use the library from one thread. Its own output thread touches only the output path and
  already-played stream data.
- **Old model microcode:** `src/apu_gp_ucode.h` holds the original model's GP microcode, which is not valid
  DSP56300. Only the null backend still writes it, into its RAM stand-in. The console runs `dsp/gp_reverb.asm`,
  assembled with `tools/dsp56asm.py`.

## Tests

The tests are host programs that run the library against mock memory; they check the library's logic, not the
APU (that is what the console samples and `docs/XEMU_VERIFICATION.md` are for).

```bash
python3 lib/openal/tests/run_tests.py       # builds the library and every tests/test_*.c, runs them
make -C lib/openal/tests test               # the same set through make (clang)
sh lib/openal/tests/check_target_syntax.sh  # type-checks the Xbox-only code paths against the nxdk headers
```

Run them with and without `-DOPENAL_APU_REAL_MMIO`. A new `tests/test_*.c` is picked up automatically; one that
links showcase sources needs an `EXTRA_<test>` entry in the Makefile and a branch in `run_tests.py`.

**Console samples:**
- `openal_engine` (engine features), `openal_stream` (streaming), `openal_hrtf`;
- `openal_showcase` (a 3D demo of everything, mode 6 is the reverb);
- the `apu_*` probes behind the hardware findings.

## Layout

- `include/AL/`: public headers (`al.h`, `alc.h`, `alext.h`).
- `src/`:
  - `al_*`: the AL objects;
  - `alc_context.c`: devices and contexts;
  - `apu_vp.c`: voices;
  - `apu_gp.c`, `apu_reverb.c`: the GP program and reverb;
  - `apu_ac97.c`: output;
  - `apu_voice*.c`: the voice manager;
  - `apu_hrtf.c`, `apu_spatial.c`: HRTF table and 3D math.
- `dsp/`: the GP programs in DSP56300 assembly; `tools/dsp56asm.py` assembles them into headers in `src/`.
- `tests/`: host tests and runners.
- `docs/`: hardware findings, engine guide, plans.
