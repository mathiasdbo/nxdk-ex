# openal_vp: OpenAL on the MCPX APU Voice Processor (route A)

Plays sources through the OpenAL API with the library built for the real APU (`-DOPENAL_APU_REAL_MMIO`).
`alcOpenDevice()` then runs the corrected Voice Processor bring-up in `lib/openal/src/apu_vp.c`, and every
source that gets a voice slot plays on a hardware VP voice (slot *n* is VP handle 64 + *n*). See
`lib/openal/docs/XEMU_VERIFICATION.md` (route A, section 8.2) for the hardware model.

The library is compiled into this sample through `mmio/*.c` (each includes one library source), so only those
objects get the define and the shared `lib/libopenal.lib` keeps the default null backend.

Phases (repeating):

1. a 440 Hz loop at 2 m;
2. a distance sweep 1 m to 30 m (effective gain becomes the voice's output attenuation);
3. a pitch sweep 0.5x to 2x (`AL_PITCH` becomes the 4.12 log2 `TAR_PITCH` field);
4. an orbit around the listener (pan gains become the six output attenuations);
5. a chord on three voices;
6. three one-shot pings, each of which must end on its own and be reported `AL_STOPPED`.

Where the front end runs methods (xemu), mono sources play on HRTF voices (handles 0-63) with the HRTF entry of
their direction latched from the spherical-head table in `lib/openal/src/apu_hrtf.c`; stereo sources use plain
voices (64 and up). On a real console every source is a plain voice panned by its output volumes.

The screen shows the backend, each source's state, VP handle, `CBO` and `ACTIVE_VOICE`. For HRTF voices it also shows the latched entry
(read back from the voice record) and its azimuth/elevation; it follows the source in the orbit phase.

## What it shows on xemu

Verified with "Real-time DSP processing" off (the default): the backend is MMIO, three voices run at once, the
pings end and are reaped, and xemu does not abort. In that mode xemu taps the VP output and hears each voice at
its loudest output, ignoring the mixbins, so distance and pitch are audible but left/right panning is not.

## On a real console

The front-end method queue stalls after one method on hardware, so the library never uses methods for voices: it
writes each voice record and the list heads itself, and the VP plays them (`apu_probe` round 16). The GP program
in `apu_gp.c` copies mixbins 0/1 into a FIFO ring every frame, and `alXboxUpdateVoices()` forwards that ring to the
AC97 (`apu_ac97.c`; round 17 checked this path bit-exact with a 440 Hz tone). The 5.1 pan is folded into
those two bins. The HRTF table can only be loaded with methods, so HRTF is off there. The status line shows
whether HRTF is on, the GP frame count and the AC97 buffer/underrun counts.

## Building

```bash
cd samples/openal_vp
make
```

Applications using this backend must call `alXboxUpdateVoices()` regularly (every frame): it unlinks voices that
ended, releases xemu's idle-voice trap (without which xemu halts every voice) and, on a real console, keeps the
AC97 fed from the GP FIFO (about 43 ms is queued ahead, so a longer stall is heard as a gap).
