# Reverb on the GP DSP (F8): implementation plan

Branch `feat/gp-reverb`. Status: phases 0 and 2 done on a console (`samples/apu_gp_probe` rounds 1-2), the
assembler of phase 1 done; the reverb program (phase 3) next. Facts marked *measured* come from our probes
(`XEMU_VERIFICATION.md` 8.1b).

**Phase 0/2 results, in short:** on-chip RAM is X 3072 + Y 2048 words (as in xemu), so the delay lines go to
system RAM over DMA as planned; circular scratch DMA works both ways (blocks need `dsp_step` = 1); a chained
write/read pair delays by exactly the planned amount; the end-of-list flag works as the wait; 64000 cycles and
64 descriptors per frame still run at full rate; output 4 of plain and HRTF voices reaches bin 2; an echo on the
GP came out sample-exact, also for delays that are not a multiple of 32.

## 1. Goal

A room-effect stage running on the APU's Global Processor (GP, a DSP56300) in a program written for this
library: one mono send bus (a mixbin), a stereo reverb computed on the DSP, mixed into the stereo output the
CPU already forwards to the AC97. Runtime CPU cost: parameter writes only. Parameters change live, so an engine
can follow Half-Life's `room_type` (reverb, echo, underwater).

**Clean-room.** The algorithm is our own (a comb/allpass reverb in the Schroeder/Moorer family). DSP56300
instruction words are encoded from Motorola's public *DSP56300 Family Manual* (opcode appendix), cross-checked
with xemu's public interpreter tables. Hardware behaviour comes from our own probes. No DirectSound microcode,
no dumps, no state left by other software.

**Not in scope.** Full `ALC_EXT_EFX`, the EP DSP, 5.1 output, modulated (chorus-like) delays.

## 2. What we start from

- **GP frame (measured).** 32 samples at 48 kHz (667 us). Our 13-word loop: frame complete -> wait for
  START_FRAME -> acknowledge -> start the DMA chain at X:0 -> count at X:$10. Mixbin n is 32 words of 24-bit
  samples at `X:$1400 + 32n`. The output descriptor copies bins 0/1 interleaved, 16-bit, into GP output FIFO 0
  (channel stride `dsp_step` 32, measured); `apu_ac97.c` forwards that ring to the codec every 4 ms. Its
  `padded` counter says when the GP falls behind real time (it caught the 120-voice HRTF overload).
- **Voice outputs (measured).** Each voice record has 8 outputs, each with a mixbin and a 12-bit volume. On a
  console, stereo-panned voices use outputs 0/1 -> bins 0/1; HRTF voices use outputs 0/2 -> submix slots 0/2
  -> bins 0/1 (register 0x20AC), outputs 1/3 feed the unused slots. Outputs 4-7 are free on both kinds.
  Whether output 4 of an HRTF voice (3D list) reaches a plain bin is not measured.
- **DMA engine (xemu model; the FIFO-out path and the stride are measured).** A descriptor is 7 words
  (next | end-of-list bit 14, control, count, DSP address, scratch offset, scratch base, size-1), chained
  from NEXT_BLOCK (`X:$FFFFD4`), started by CONTROL (`X:$FFFFD6`) = 1. Control bits: 0 interleave,
  1 direction (DSP -> memory), 4 offset write-back, 8:5 buffer (0-3 output FIFOs, 0xE circular scratch,
  0xF linear scratch), 12:10 format (1 = 16-bit, 2/6 = 24-bit), 23:14 channel stride. Memory -> DSP exists
  only for non-interleaved scratch transfers. "Scratch" is our own SGE-mapped system RAM (`GPSADDR`,
  `GPSMAXSGE`; today 0x2000 bytes holding the bootstrap image). The end-of-list flag is bit 7 of
  `X:$FFFFC5`, cleared by writing it. Never polled on silicon yet.
- **On-chip RAM (xemu model).** X 4096 words plus the 1024-word mixbin buffer, Y 2048, P 4096; the
  bootstrap loads P:0..$7FF. Hardware sizes are not measured.
- **Live access (measured).** The CPU writes X memory through the window while the core runs (the descriptor
  is written after the release) and reads the frame counter the same way. So parameters can live in X memory.
- **Instructions (measured).** `movep #imm`, `jclr`, `inc a`, `move a0,x:aa`, `jmp`. xemu's interpreter
  implements 106 of the 187 templates and asserts on the rest, so a program that runs on xemu decodes
  everywhere we need; the reverse is not guaranteed (xemu may lack an instruction the silicon has).
- **Unknown before phase 0 (now measured, see the status above).** The GP clock and so the instruction budget
  per frame; DMA throughput and latency; whether memory -> DSP scratch DMA works on silicon as modelled.

## 3. Design

### 3.1 Signal path

```
voice output 4 -> bin 2            mono send, pre-pan and pre-HRTF, volume = master x send level
GP, every frame:
    send[32] = X:$1440 (bin 2)
    dryL/R   = bins 0/1
    wetL/R   = reverb(send)
    outL/R   = dryL/R + wetL/R      -> staging area X:$20.. (2 x 32 words)
    FIFO 0 descriptor copies the staging area (today it copies the bins)
```

The send is mono: a reverb's input is the sound, not its position. Stereo comes out of the reverb (different
line lengths per ear). The per-source send level is `AL_XBOX_REVERB_SEND` (0..1, default 0), scaled by the
source's own gain; an engine can also scale it by distance.

### 3.2 Algorithm (fixed point, 24-bit)

Order: pre-delay (0..100 ms) -> input one-pole lowpass (room HF damping) -> 4 parallel feedback combs per ear
(8 in all), each with a one-pole lowpass inside the feedback -> 2 (up to 4) series allpasses per ear ->
wet gain -> add to dry.

- Comb lengths near 1116/1188/1277/1356 samples at 44.1 kHz, rescaled to 48 kHz, right ear +23 samples;
  allpass lengths 225..556 rescaled. Mutually prime, as usual. Our own constants; adjust by ear on the console.
- Feedback `g = 10^(-3 L / (T60 fs))` from the decay time; feedback damping from the HF ratio; allpass
  `g = 0.5` (diffusion). Wet gain in 0..1. All computed on the CPU in float, written as Q23 words.
- Integer delays only, no interpolation, no modulation.
- Samples are 24-bit fractions, coefficients Q23, accumulators 56-bit, `macr` rounding, saturation mode
  enabled at init (the SR bit to set is checked on xemu first).
- Memory: about 2 x (4 x 1500 + 2 x 500) ~ 14000 words, more than the on-chip RAM the model shows. So the
  delay lines live in system RAM as circular scratch buffers, one per line, moved by DMA (3.3). If phase 0
  shows the silicon has enough on-chip RAM, the lines go there and the DMA chain shrinks to the FIFO
  descriptor; the program structure stays the same.

### 3.3 Delay lines over DMA

A line of length `L >= 32` is a circular scratch buffer of `S >= L + 32` words (size a power of two). Two
descriptors per line and frame, both with offset write-back so the engine advances them:

- **read**: 32 words from offset `w - L` (mod S), memory -> DSP, into the line's input block in X memory;
- **write**: 32 words from the line's output block, DSP -> memory, at offset `w`.

The driver sets the initial offsets so `w - r = L`; after that both advance 32 per frame on their own. The
comb reads before it writes (`y[n] = buf[n-L]`, `buf[n] = x[n] + g lp(y[n])`), so with `L >= 32` the frame's
reads never depend on the frame's writes.

Frame schedule:

```
complete; wait start; ack
NEXT_BLOCK = chain R (all read descriptors)           ; CONTROL = start
poll $FFFFC5 bit 7 (end of list), clear it            ; count the spins into X:$11 (debug)
compute 32 samples                                    ; do #32 outer loop, r-registers over the line blocks
NEXT_BLOCK = chain W (write descriptors + FIFO out)   ; CONTROL = start
counter; jmp
```

Chain W is not awaited; it has a whole frame to finish before chain R of the next frame reads behind it. Full
topology: 12 lines per ear x 2 descriptors = 48 plus the FIFO descriptor. If phase 0 says that is too many
per frame, the "lite" topology is a mono core (6 combs + 2 allpasses) made stereo by one extra short allpass
per ear: 10 lines, 20 descriptors.

### 3.4 Memory map (GP X memory, proposal)

| Range | Use |
|---|---|
| X:$0..$F | FIFO-out descriptor (as today) |
| X:$10..$1F | debug words: frame counter, last end-of-list spin count, max spin count, overrun flag |
| X:$20..$5F | staging area: out L (32), out R (32) |
| X:$80..$BF | parameters, written by the CPU: enable, wet L/R, comb g[8], damp, allpass g, input LP, pre-delay in frames |
| X:$100..$1FF | descriptor chains R and W (49 x 7 words fits in 0x157) |
| X:$200..$7FF | line blocks: input and output 32-word blocks per line, filter states |

Parameters that change the delay (pre-delay) rewrite descriptor offsets, which races the engine; they are
applied with the wet gain at zero for a frame. Everything else is a single word write.

### 3.5 Program size and budget

Per sample: 8 combs x ~5 instructions + 4-8 allpasses x ~4 + mixing ~6: roughly 80 instructions, 2600 per
frame plus DMA set-up. A DSP56300 at tens of MHz has 20000+ cycles in 667 us, so arithmetic is not the
limit; DMA cost is what phase 0 measures. The program is a few hundred words, well inside P:0..$7FF.

### 3.6 Tooling

- **Assembler** `lib/openal/tools/dsp56asm.py`: a small two-pass assembler for the subset we use (`nop`,
  `jmp`, `jclr`/`jset`, `movep #imm,x:pp`, `move` immediate/register/`x:(rn)+`/`x:aa`/`y:(rn)+`, `do #n`
  and `do x:aa`, `rep`, `mpy`/`mac`/`macr`, `add`/`sub`/`tfr`/`clr`/`asl`/`asr`, `bset`/`bclr` on SR,
  `ori #,mr`). Encodings from the Motorola manual's opcode appendix. It emits a C header with the word array
  and the symbol addresses (parameter block, debug words). Self-test: the current 13-word program must
  reassemble bit-exact. Source lives in `lib/openal/dsp/gp_reverb.asm`.
- **Host reference** `apu_reverb_ref.c`: the same algorithm in C with the DSP's arithmetic (24-bit samples,
  56-bit accumulation, `macr` rounding, saturation). Host tests check the parameter-to-coefficient maths and
  produce the expected output for console captures.
- **xemu** with `audio.use_dsp` on: a program that asserts has an undecodable word, so xemu is the first check
  of every new instruction class and of the circular-scratch DMA logic (which xemu implements). Not a timing
  check.
- **Console**: `samples/apu_gp_probe` (phase 0), `samples/openal_reverb` (phase 4), logs on `E:\` as usual;
  captures of the GP FIFO to a file for comparison with the host reference.

### 3.7 API (`AL_XBOX_reverb`)

- `alEnable(AL_XBOX_REVERB)` / `alDisable` / `alIsEnabled`: loads the reverb program (otherwise the plain copy
  program stays).
- `alXboxReverbf(param, value)` / `alXboxGetReverbf`: `AL_XBOX_REVERB_DECAY_TIME` (0.1..20 s),
  `AL_XBOX_REVERB_DECAY_HF_RATIO` (0.1..1), `AL_XBOX_REVERB_GAIN` (wet, 0..1), `AL_XBOX_REVERB_PRE_DELAY`
  (0..0.1 s), `AL_XBOX_REVERB_DIFFUSION` (0..1).
- `alXboxReverbPreset(n)`: ~10 presets with our own numbers (generic, small room, bathroom, hall, cave,
  arena, tunnel, sewer pipe, underwater, dark/underground) for Half-Life's room types; the Xash mapping table
  is a doc item.
- Per source: `alSourcef(src, AL_XBOX_REVERB_SEND, 0..1)`, works for panned, HRTF and direct-mode sources.
- Status: `alXboxGetHardwareStatus(AL_XBOX_REVERB_ACTIVE)`.

Underwater muffling of the dry path is a separate, cheaper feature (the measured per-voice filter, `FMODE`),
noted here, not part of this plan.

## 4. Phases

### Phase 0: probes (`samples/apu_gp_probe`, 1-2 console rounds)

Batched, each step logged, every result into `XEMU_VERIFICATION.md` 8.1b:

1. **RAM sizes.** Write patterns through the X/Y/P windows at increasing addresses; a program copies `X:n`
   and `Y:n` to `X:$10` so the core's own view is measured, not the window's aliasing.
2. **Circular scratch DMA.** A descriptor writes 32 counter words to a circular scratch buffer (4096 words,
   write-back on), another reads 32 words back from `offset - 64` into `X:$300`; check through the window.
   Measures: memory -> DSP works, the offsets advance, the end-of-list bit sets and clears.
3. **Formats.** 24-bit round trip (formats 2 and 6) is exact; 16-bit truncation as modelled.
4. **Instruction budget.** The program runs `do x:$12` NOPs per frame; the CPU raises the count until
   `padded` rises. Then the same with chains of k dummy descriptors (k up to 64): instructions and descriptors
   per frame available.
5. **Send routing.** A panned voice and an HRTF voice with output 4 -> bin 2 at full volume; the GP copies
   bin 2 instead of bins 0/1 for one step. Is the send heard, and are bins 0/1 unchanged?
6. **End-of-list wait.** The loop waits for bit 7 after the chain; log the spin counts (phase 1's schedule
   depends on it).

Exit: numbers for RAM, descriptors per frame, instructions per frame, and the send route; a go/no-go on
DMA-backed lines versus on-chip lines versus the CPU fallback (5).

### Phase 1: toolchain (host only)

The assembler with its self-test, the generated header replacing the hand-written word list in `apu_gp.c`,
the C reference model and its tests (impulse through one comb, decay time from `g`, saturation).

### Phase 2: an echo on hardware (1-2 rounds)

Program: copy + one comb on bin 2 (250 ms, feedback 0.5) added to bins 0/1. Validates the whole mechanism:
reads before writes, the wait, the arithmetic, the staging area and the changed FIFO descriptor. The user hears
a click echo; a capture is compared with the reference. The frame budget is measured with the real schedule.

### Phase 3: the reverb program (2-3 rounds)

Full (or lite) topology, parameter block, presets. xemu first (decode and logic), then the console: an
impulse capture gives T60 against the setting, `padded` must stay 0 with 64 HRTF voices playing, and a
listening pass over the presets.

### Phase 4: library integration (1-2 rounds)

`apu_gp.c` loads either program; `apu_reverb.c` (parameters -> Q23 words, X-window writes, pre-delay
switch); the voice send on output 4 (`apu_voice.c` `vp_params_from_context`, all three routing branches);
the AL entry points and `alext.h` tokens; the extension string; `openal_reverb` sample; `test_apu_reverb.c`
and the `test_lib_build.c` extension check; `ENGINE_INTEGRATION.md` 3.7 and the limits table; README.

### Phase 5: Xash mapping

A table from HL `room_type` values to presets and send levels, in `XASH3D_INTEGRATION_PLAN.md` F8, for the
engine port to apply.

## 5. Risks and fallbacks

| Risk | Fallback |
|---|---|
| Memory -> DSP scratch DMA does not work on silicon, or 48 descriptors per frame do not fit | lite topology (20 descriptors); on-chip lines if phase 0 finds the RAM; otherwise a CPU reverb in the AC97 pump on the same send bus (the GP copies bin 2 to a second FIFO) |
| The end-of-list wait never completes on hardware (flag semantics differ) | poll the read block for a frame tag written by the DSP after the data; last resort, consume the previous frame's reads (one frame of extra latency) |
| Output 4 of HRTF voices does not reach a plain bin | HRTF voices get no send (they are the panned minority) or use output 5-7; measured in phase 0 |
| A hand-encoded word is wrong | every new instruction class runs on xemu before the console; the console probe keeps the debug words at X:$10..$1F readable through the window |
| Overflow in the wet sum | saturation mode plus headroom in the wet gain; no limiter planned |
| Pre-delay changes race the DMA write-back | applied with the wet gain at zero for a frame |

## 6. Estimate and done criteria

About 6-8 console rounds across phases 0, 2, 3 and 4; phase 1 is host work between rounds.

Done when: the presets are audible on a console with no CPU cost beyond parameter writes, `padded` stays 0
with 64 HRTF voices and the reverb on, the host tests cover the reference model and the API, the probe
numbers are in `XEMU_VERIFICATION.md`, and `ENGINE_INTEGRATION.md` documents the extension.
