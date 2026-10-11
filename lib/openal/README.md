# OpenAL for nxdk (OpenAL 1.1 subset over an MCPX APU voice-layer model)

An OpenAL 1.1 **subset** for the bare-metal **nxdk** Xbox SDK. The library implements the OpenAL source,
buffer and listener objects, the 3D spatial math (distance models, cones, Doppler, panning, ITD/HRTF
parameters) and a voice manager that multiplexes up to 256 sources over 64 modelled hardware voice slots,
all on top of a **software model** of the Nvidia MCPX Audio Processing Unit (APU) voice layer.

> **Status: the hardware register layer is not yet conformant to xemu or the real MCPX APU.**
> It was checked against xemu's APU model (by reading the xemu source; nothing was run on xemu) and found to
> use a private register map, voice layout and microcode that xemu does not implement; details, evidence and
> a porting plan are in [`docs/XEMU_VERIFICATION.md`](docs/XEMU_VERIFICATION.md). By default the library does
> **not** touch the APU registers and produces **no sound**: nothing mixes samples or feeds the AC97 output,
> and no voice ever completes on its own (see Per-frame update). Driving the real registers is an explicit
> opt-in that is expected to abort xemu (static analysis, not run).

## Backends

| Backend | Selected by | What it does |
|---|---|---|
| **NULL (software model), the default** | nothing to do | The device runs on a zeroed, BAR0-sized RAM stand-in for the APU register file; no real APU register is touched. `alcOpenDevice` still runs its whole bring-up against that stand-in, including uploading the GP microcode of `src/apu_gp_ucode.h` (not valid DSP56300, see Known limitations) into it. Sources, buffers, spatial math and the voice manager work and are what the host tests exercise. No audio is output, and nothing models a voice finishing, so no voice completes by itself |
| **Real MMIO, opt-in** | build the library with `-DOPENAL_APU_REAL_MMIO` (recipe below) | The library writes the registers of `src/apu_hardware.h` at the APU base address (BAR0, `0xFE800000`). That register model is not xemu-conformant: on xemu the device bring-up is expected to disturb or abort the emulator (static analysis, not run), and it has not been verified on a console |

The active backend can be queried with `alXboxGetHardwareStatus(AL_XBOX_BACKEND, &value)` (the value is
`AL_XBOX_BACKEND_NULL` or `AL_XBOX_BACKEND_MMIO`, see `include/AL/alext.h`); `alGetString(AL_RENDERER)` also
names it.

**Opting in.** The define selects code inside the library (`src/alc_context.c`) and gates `samples/apu_tone`, so
everything that uses it has to be rebuilt with it (object files are not rebuilt when only the flags change):

* nxdk application (the library is compiled with the application): `make clean && CFLAGS=-DOPENAL_APU_REAL_MMIO make`
  with `CFLAGS` in the environment (a `CFLAGS=...` argument on the make command line would replace the
  `CFLAGS += -I...` lines of the sample Makefiles), or put `NXDK_CFLAGS += -DOPENAL_APU_REAL_MMIO` in the
  application Makefile before the line that includes `$(NXDK_DIR)/Makefile`.
* Standalone library build (host-side check only): `make -C lib/openal clean && make -C lib/openal
  CFLAGS="-m32 -Wall -Werror -pedantic -std=c99 -DOPENAL_APU_REAL_MMIO"`; passing `CFLAGS` replaces the
  Makefile defaults, so repeat them.

## Per-frame update

Call `alXboxUpdateVoices()` once per frame (`alcProcessContext()` on a valid context performs the same
tick). It reaps one-shot voices whose model ACTIVE bit is clear, so their sources become `AL_STOPPED`, and
promotes sources waiting in virtual standby into free voice slots. Looping sources are never reaped. It does
nothing when no device is open.

On the NULL backend the ACTIVE bit is plain RAM: the library sets it when it starts a voice and clears it when
it stops one, and nothing models a voice finishing (no clock, no hardware behind it). A one-shot source therefore
**stays `AL_PLAYING` until `alSourceStop`**; the tick only reaps a voice when something else clears the bit, as
the host tests do by hand. On xemu the model register would not report completion either (finding C1.A2 in
[`docs/XEMU_VERIFICATION.md`](docs/XEMU_VERIFICATION.md)).

## Supported API subset

* **ALC:** `alcOpenDevice` (device names: `NULL`, the default name, `"MCPX APU"`, `"DirectSound3D"`,
  `"DirectSound"`), `alcCloseDevice`, `alcCreateContext`, `alcMakeContextCurrent`, `alcDestroyContext`,
  `alcGetCurrentContext`, `alcGetContextsDevice`, `alcGetError`, `alcGetString`, `alcGetIntegerv`
  (including `ALC_XBOX_TOPOLOGY`, 0x1014, which is defined only in the private `src/alc_context.h` and not
  exported from `include/AL/`; define it yourself as the samples do). `alcProcessContext` runs the voice tick
  described above. `alcSuspendContext` and the capture functions are stubs.
* **Buffers:** `alGenBuffers`, `alDeleteBuffers`, `alIsBuffer`, `alBufferData` for
  `AL_FORMAT_MONO8/16` and `AL_FORMAT_STEREO8/16` (8-bit data is unsigned, as in OpenAL), `alGetBufferi`
  (`AL_FREQUENCY`, `AL_BITS`, `AL_CHANNELS`, `AL_SIZE`), `alBufferiv`/`alGetBufferiv` for
  `AL_LOOP_POINTS_SOFT`. At most 2048 buffers.
* **Sources:** `alGenSources`, `alDeleteSources`, `alIsSource`, `alSource*`/`alGetSource*` for position,
  velocity, direction, gain, min/max gain, pitch, looping, relative positioning, cone angles and gain,
  reference distance, max distance, rolloff, `AL_BUFFER`, `AL_SOURCE_STATE`, `AL_SEC_OFFSET` / `AL_SAMPLE_OFFSET` /
  `AL_BYTE_OFFSET`, buffer queueing (see the limitations), and the `AL_XBOX_source_control` attributes
  (`include/AL/alext.h`); `alSourcePlay`, `alSourcePause`, `alSourceStop`, `alSourceRewind` and their vector
  forms. At most 256 sources, 120 of them on hardware voices at once on a console (64 on xemu).
* **Engine integration:** see `docs/ENGINE_INTEGRATION.md`.
* **Listener:** position, velocity, orientation, gain.
* **Global state:** `alDistanceModel` (inverse, linear, exponent, clamped variants, none), `alDopplerFactor`,
  `alSpeedOfSound`, `alGetError`, `alGetString` (vendor, version, renderer, extensions), `alGetInteger*`,
  `alGetFloat*`.

## Xbox extensions and extension string

`alGetString(AL_EXTENSIONS)` lists `AL_XBOX_hardware_status` (`alXboxGetHardwareStatus`) and
`AL_XBOX_update` (`alXboxUpdateVoices`); both entry points are also returned by `alGetProcAddress`.

| Query token (`alXboxGetHardwareStatus`) | Meaning |
|---|---|
| `AL_XBOX_HW_VOICE_COUNT` | hardware voice slots managed by the library |
| `AL_XBOX_AV_PACK_TYPE` | AV pack, as an `AL_XBOX_AV_PACK_*` token; the token values equal the raw SMC pack codes xemu reports (see `alext.h`). Read through the kernel's SMBus call on the Xbox target; a standard (composite) pack on the host |
| `AL_XBOX_DOLBY_DIGITAL_ACTIVE` | whether the library configured the 5.1 path; cached software state, **not** a verified encoder |
| `AL_XBOX_VP_BASE_PHYS` | physical address of the library's voice table; cached software state |
| `AL_XBOX_BACKEND` | active backend, see above |

`AL_XBOX_LFE_GAIN` is a per-source parameter (`alSourcef`), not a status query.

The multichannel buffer tokens `AL_FORMAT_QUAD8/16` and `AL_FORMAT_51CHN8/16` are declared in `alext.h` but
`alBufferData` rejects them with `AL_INVALID_ENUM`; do not rely on `AL_EXT_MCFORMATS`.

## Running the tests

The tests are host programs that run the library against mock memory. They verify the library's own logic,
not the behaviour of the APU.

```bash
python3 lib/openal/tests/run_tests.py       # builds the library and every tests/test_*.c, runs them
make -C lib/openal/tests test               # the same set through make (clang), output in lib/openal/tests/build
sh lib/openal/tests/check_target_syntax.sh  # type-checks lib/openal/src as nxdk-cc would (no toolchain needed)
```

`run_tests.py` and the Makefile both discover every `tests/test_*.c` by globbing, so a new test is picked up by
either; `run_tests.py` writes all build output to a temporary directory. A test that links showcase sources
needs an `EXTRA_<test>` entry in the Makefile and a branch in `run_tests.py`. `check_target_syntax.sh` exists
because the host build never compiles the code paths that are guarded for the real Xbox. Strict host compile of
the sources:
`clang -std=c99 -pedantic -Wall -Wextra -Werror -Ilib/openal/include -Ilib/openal/src -fsyntax-only lib/openal/src/*.c`.

`tests/test_benchmark.c` is a timing harness, not a regression test: it times the library's source update path
(the spatial math and the writes of voice contexts into mock memory, plus the harness's own `sinf`/`cosf`) in
host timer ticks (the TSC on x86, `clock()` elsewhere). It asserts no performance threshold and measures no APU
mixing. `samples/openal_benchmark` runs the same kind of measurement on the Xbox and prints the results on
screen. There are no committed measurements.

## Known limitations

* **Not thread-safe.** Use the library from a single thread. On a console the library itself runs one
  internal thread, the AC97 pump (`apu_ac97.c`), which touches only the output path and played stream data.
* **No audio output by default** (see Backends); there is no software mixer.
* **Streaming** (`alSourceQueueBuffers`/`alSourceUnqueueBuffers`, `al_stream.c`):
  * Every buffer in a queue needs the same format and frequency, and a queue holds at most 64 buffers. At most
    32 sources can stream at once.
  * Queued data is copied into a 16384-frame ring at most 8192 frames ahead of the play position, during
    `alXboxUpdateVoices()` and the queue queries. Call it at least every ~170 ms at 48 kHz, or the stream plays
    silence for the gap.
  * Verified on a retail console with `samples/openal_stream` (48/22.05/11.025 kHz, mono/stereo, 8/16-bit,
    starvation and 1.5 s main-thread stalls).
* `AL_LOOPING` can change while a source plays (its voice's loop markers are rewritten in place). Capture is stubbed. Only mono and stereo 8/16-bit PCM is accepted; **no ADPCM**, no multichannel buffers.
* **No clock: nothing completes by itself.** The library never observes real voice progress. A source reaches
  `AL_STOPPED` through `alSourceStop`, or when something clears its voice's model ACTIVE bit (the host tests do);
  a source waiting in virtual standby holds only a saved position and does not advance or finish until it is
  promoted to a voice slot.
* **Spatial parameters are model values.** The Woodworth ITD (0-31 samples; the formula is clamped to 64 but
  never exceeds 31), the Q14 biquad "HRTF", the linear 16.16 pitch step, the linear gains and the 64-voice cap
  are the library's own model. xemu's voice processor uses a 31-tap FIR HRTF with an ITD of up to +/-42 samples,
  a signed 4.12 log2 pitch field, 12-bit attenuations (0 = unity, 0xFFF = mute) and up to 256 concurrent voices.
* **No Dolby Digital / DTS / TOSLink output.** The library writes configuration bits for a 5.1 topology; it
  contains no AC-3 encoder, and xemu models none. Topology detection depends on the AV pack and the EEPROM
  audio flags, whose optical-pack set and flag values are assumptions.
* **The GP DSP microcode in `src/apu_gp_ucode.h` is not valid DSP56300 code.** `alcOpenDevice` still uploads it
  into the model register file (the RAM stand-in by default, the real BAR0 only with
  `-DOPENAL_APU_REAL_MMIO`), and the host tests compare against it.
* **Performance is unmeasured.** No cycle counts or CPU-load figures are published for this library.

## Roadmap

See section 8 of [`docs/XEMU_VERIFICATION.md`](docs/XEMU_VERIFICATION.md): the maintainers have to choose
between a faithful hardware driver (the corrected register model and a 25-step bring-up sequence are in
sections 4 and 7) and an AC97 plus software-mixer backend built on `lib/hal/audio.c`.

## Layout

* `include/AL/` - public headers (`al.h`, `alc.h`, `alext.h`).
* `src/` - the library (`al_*`: AL objects; `alc_context.c`: devices and contexts; `apu_*`: the APU model,
  voice manager, spatial math, memory pool, AV-pack/EEPROM queries).
* `tests/` - host unit tests and the test runners.
* `docs/` - the xemu verification report.
* Exported to nxdk through `lib/Makefile` as `libopenal.lib`.
