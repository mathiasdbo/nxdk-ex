# Writing a GP kernel (AL_XBOX_gp_program)

A GP kernel is your own DSP56300 code that the APU's GP DSP runs on the effect sends to one effect slot, every
0.67 ms, as an `ALC_EXT_EFX` effect. It costs no CPU time. The library owns the frame, the output and the
parameter delivery; the kernel only processes one 32-sample block per call. Each of the 4 slots runs its own
kernel, so yours runs beside the library's reverb, echo, chorus or on-chip effects (section 10).

**Status:** ABI 1 (one kernel at fixed addresses) ran on a console in `samples/openal_efx` (2026-10-18): the
echo landed exactly on its delay on both ears, a parameter change reached the kernel in one frame, every load
went through the P window without a GP restart, and 5 minutes of switching effects gave 0 underruns. **ABI 2**
(relocatable, one kernel per slot, phase9-04) is host-tested (assembler, simulator, loader), and so are the
library's ABI 2 kernels (echo, chorus, and the on-chip effects of phase9-07); their console run is phase9-08.
The current ABI is 2 (`AL_XBOX_GP_IMAGE_ABI`); ABI 1 images still load, and run only alone.

The example used throughout is `dsp/gp_kecho.asm`, a stereo echo (154 words).

## 1. From the game's side

```c
#include "gp_kecho.h"                 /* generated: gp_kecho_image[], GP_KECHO_IMAGE_WORDS */

ALuint fx, slot;
alGenEffects(1, &fx);
alEffecti(fx, AL_EFFECT_TYPE, AL_EFFECT_XBOX_GP_PROGRAM);
alXboxEffectProgram(fx, gp_kecho_image, GP_KECHO_IMAGE_WORDS, 0);
alEffecti(fx, AL_XBOX_GP_PARAM0 + 0, 4800);      /* delay: 100 ms, a raw integer */
alEffectf(fx, AL_XBOX_GP_PARAM0 + 1, 0.4f);      /* feedback, Q23 */
alEffectf(fx, AL_XBOX_GP_PARAM0 + 2, 0.5f);      /* wet, Q23 */

alGenAuxiliaryEffectSlots(1, &slot);
alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, (ALint)fx);   /* loads and starts it */
alSource3i(src, AL_AUXILIARY_SEND_FILTER, (ALint)slot, 0, AL_FILTER_NULL);
```

- **`alXboxEffectProgram(effect, words, count, flags)`**: `effect` must be of type
  `AL_EFFECT_XBOX_GP_PROGRAM` (`AL_INVALID_OPERATION` otherwise). The library checks the image and keeps its own
  copy, so the game's array can be freed afterwards. `count` 0 clears the program. `flags` must be 0.
  - A bad image gives `AL_INVALID_VALUE`: wrong magic or ABI, code outside 1..0x700 words, a word above 24
    bits, more than 60 descriptors, more scratch than the device has (about 500 KiB), an X block outside
    $28..$B20 words, more than 2048 Y words, or a reloc outside the code.
  - Setting `AL_EFFECT_TYPE` again drops the image and resets the parameters to 0.
- **Parameters** `AL_XBOX_GP_PARAM0 + n`, n < `AL_XBOX_GP_PARAMS` (32): 24-bit words the kernel reads at
  `X:KPARAM + n`.
  - `alEffecti` stores an integer from -0x800000 to 0xFFFFFF; `alGetEffecti` reads it back sign-extended.
  - `alEffectf` stores a fraction from -1 to 1 as Q23. 1.0 saturates to 0x7FFFFF.
  - Out of range: `AL_INVALID_VALUE`, and the old value stays.
- **Changes take effect on attach**, as with every EFX effect: the slot holds a copy. After changing parameters,
  call `alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, fx)` again. With the same image only the changed
  words are sent, all together, at the start of one frame. A different image reloads the kernel (section 7).
- **Deleting the effect** leaves the slot's copy running, as EFX specifies.
- **One kernel per slot, shared budgets.** Every slot (up to 4) runs its own kernel, in slot order, beside the
  others. The GP's memory and DMA are shared: P:$100..$7FF (0x700 words), X:$E0..$BFF ($B20 words), Y (2048
  words), scratch (about 500 KiB) and 60 descriptors a frame, summed over the slots. Attaching an effect that
  does not fit beside the other slots' gives `AL_INVALID_OPERATION` (and nothing changes); so does attaching
  any while `AL_XBOX_REVERB` is on, or an ABI 1 kernel beside anything. For scale: the reverb takes 101 P,
  $8C0 X, 180 KiB, 50 descriptors; the echo 169 P, $C0 X, 128 KiB, 3; the chorus 177 P, $30 X, all of Y; the
  on-chip effects 92 to 247 P and $2B to $53 X each, nothing else (section 10).
- **`AL_EFFECTSLOT_GAIN`** scales the sends into the kernel (the kernel has no word for it), live. For a
  linear kernel such as the echo that is the same as scaling its output; a kernel that is not linear (a
  compressor, a distortion) sees a quieter input, so give it its own output-gain parameter if it needs one.
  The library's distortion, compressor and autowah do that: the slot's gain is one of their words.
- **Load:** `alXboxGetHardwareStatus(AL_XBOX_GP_LOAD, &v)` is the share of the last frame the effects took, in
  percent; `AL_XBOX_GP_LOAD_PEAK` the highest since the previous peak query. 100 means frames are late and the
  dry mix stutters. Console only: 0 elsewhere (section 7).

## 2. The frame

Each frame is 32 samples at 48 kHz. The library's loop (`dsp/gp_loop.asm`) does, in order:

1. wait for the APU's frame start (counting how long it waits: the load meter);
2. apply the mailbox: parameter changes, written into `X:KPARAM + n` before the kernels run;
3. run each running slot's **read chain** (`X:K_RHEAD`, 0 for none) and wait for it: DMA from scratch into X;
4. copy the dry mix (bins 0/1) into OUT L / OUT R;
5. call each running slot's kernel in slot order (`jsr` to the slot's trampoline, a `jmp` to your entry);
6. the output lowpass (only while the reverb runs);
7. run each running slot's **write chain** (`X:K_WHEAD`, 0 for none, ending at end of list `$004000`) and
   wait for it: DMA from X to scratch; then the mix descriptor sends OUT L/R to the output.

Because the reads run before the kernels and the writes after them, data a kernel writes to scratch in frame n
can be read back at the earliest in frame n+1.

## 3. Memory and the ABI words

The library places each kernel where its P, X, Y and scratch fit (first fit, section 7): the image is
assembled at 0 and relocated (section 6). `KX` is the slot's X block, `KY` its Y range, `KSCR` its scratch.

| Where | What | Who sets it |
|---|---|---|
| P (your words, anywhere in $100..$7FF) | your code; entry at its first word, return with `rts` | the library, from the image |
| X:KX+0, KIN | X address of the input's left block (slot k: bins 2+2k, the right block 32 words on) | library |
| X:KX+1, KSCR | byte offset of your scratch | library |
| X:KX+2, KSCRN | the scratch size you declared, in bytes | library |
| X:KX+3, K_RHEAD | first read descriptor, 0 for none | you (the library writes 0) |
| X:KX+4, K_WHEAD | first write descriptor, 0 for none; the chain ends at `$004000` | you (the library writes 0) |
| X:KX+8..KX+$27, KPARAM | the 32 parameters | library, then the mailbox |
| X:KX+$28..KX+K_XWORDS-1 | your own X memory (descriptors, buffers, state) | you |
| X:OUTL ($20) / OUTR ($40) | 32 words each: the dry mix and the earlier slots' output; add yours | loop |
| Y:KY..KY+K_YWORDS-1 | yours; **not initialised** | you |
| scratch from KSCR | your delay lines and tables (system RAM through DMA), zeroed | library |

Before the first frame, the library zeroes your X block and declared scratch, then writes the ABI words.
Anything else you need (descriptors, chain heads, tables, Y) you set up yourself. The usual pattern is a flag
in your X that is 0 on the first call:

```asm
INIT    equ     KX+$28
        org     p:0
kernel  move    x:INIT,a
        tst     a
        jne     ready
        ...                     ; build descriptors, set K_RHEAD / K_WHEAD
        move    #1,x0
        move    x0,x:INIT
ready   ...                     ; the per-frame work
        rts
```

**Rules:**
- Use any register; the loop keeps nothing in them across the call. `jsr` and `do` nest on the DSP's
  hardware stack, and the loop's call takes one level of it.
- Write X only in your block, OUT L/R (32 words each) and your descriptors' targets; Y only in your range.
- Read the parameters every frame (or compare them with a copy). A change arrives between two calls with no
  other notice, and all the words of one change arrive in the same frame.

## 4. Input, output and levels

- **Samples** are 24-bit fractions (Q23) in the low 24 bits of each word.
- **Input:** the EFX sends to your slot. Send n of a source to slot s leaves on voice outputs 4+2n / 5+2n and
  mixes into GP slot k = s - 1's bins, 2+2k / 3+2k (`KIN`). Each voice output carries half the send's gain: a
  mono source feeds both bins at half, so L + R is the send; a stereo source puts its left channel on the even
  bin and its right on the odd one (into an `AL_EFFECT_XBOX_GP_PROGRAM` slot at the whole send gain, so a send
  of 1 is exact). A mono-input effect reads `L + R`; a stereo one reads the pair. When a playing source's send
  moves to another slot, the library mutes it, waits two frames for the VP's volume ramp, then moves the bins.
- **Output:** add to OUT L / OUT R, which already hold the dry mix and the earlier slots' output. The mix
  descriptor sends the top 16 bits of each 24-bit word to the codec. Adding in an accumulator and moving the
  result back to X saturates at full scale, so a loud scene clips: keep headroom in the wet gain.

## 5. DMA and scratch

The GP has 4096 words of X. Anything bigger, such as delay lines, lives in scratch, system RAM the library
maps for the GP, and is moved by the DMA engine. A descriptor is 7 X words:

| Word | Meaning |
|---|---|
| 0 | next descriptor's X address, or `$004000` (end-of-list bit 14) |
| 1 | control (below) |
| 2 | word count |
| 3 | X address of the block |
| 4 | byte offset inside the buffer (advanced by the engine when write-back is on) |
| 5 | byte offset of the buffer in scratch (`KSCR` + your offset) |
| 6 | buffer size in bytes, minus 1 |

Control bits: 0 interleave, 1 direction (1: X to memory), 4 offset write-back, 8:5 buffer (`$E` circular
scratch, `$F` linear scratch), 12:10 format (2: 24-bit), 23:14 channel stride. The echo uses `$0049D0` (read:
memory to X, write-back, circular, 24-bit, stride 1) and `$0049D2` (the same, X to memory). A 24-bit sample
takes 4 bytes of scratch. Memory to X works only for non-interleaved scratch transfers.

- **Budget:** at most 60 descriptors a frame across both chains of every slot (declare yours in
  `K_DESCRIPTORS`); the library's own copy and mix descriptors are extra. The reverb uses 50.
- **Moving a read position.** With write-back on, the engine moves each offset by the transfer size every
  frame. To change a delay, set the read descriptor's offset (word 4) yourself. The echo sets it every frame
  from the write descriptor's offset:
  `read = write + 128 - 4 * DELAY (mod line size)`. The 128 skips the block being written this frame, so a
  new delay applies from the next frame.
- **Scratch layout** is yours: `KSCR` is where it starts (4 KiB aligned), `KSCRN` its size. The echo keeps two
  64 KiB lines, left at `KSCR`, right at `KSCR + $10000`.

## 6. Building the image

The source is assembled at 0 and relocated by the loader. `KX` and `KY` are predefined (the slot's X block
and Y base), and labels move with P. Equates at the top declare what the kernel needs; the assembler writes
them into the image header:

```asm
K_SCRATCH_BYTES equ $20000      ; scratch bytes
K_DESCRIPTORS   equ 4           ; descriptors a frame, both chains
K_XWORDS        equ $E0         ; the X block from KX, the $28 ABI words included
K_YWORDS        equ 0           ; Y words from KY

KIN     equ     KX              ; the ABI words, as equates of your own
K_RHEAD equ     KX+3
RDL     equ     KX+$30          ; your X
        org     p:0
```

```bash
python3 lib/openal/tools/dsp56asm.py lib/openal/dsp/gp_kecho.asm -o lib/openal/dsp/gp_kecho.h --image
```

The header holds `NAME_image[NAME_IMAGE_WORDS]`, the image to pass to `alXboxEffectProgram`, plus every equate
as `NAME_<SYMBOL>` (relocatable ones as offsets from their base). The image layout (ABI 2): `$47504B` ('GPK'),
2, scratch bytes, descriptors, X words, Y words, the reloc count, the relocs, then the code assembled at 0. A
reloc is one word: bits 11:0 the code offset, 13:12 the kind: 0 a P extension word, 1 the 12-bit address field
of a short `jmp` / `jsr` / `jCC`, 2 an X extension word, 3 a Y extension word; the loader adds that base. Any
tool that writes this layout works.

**Expressions:** a relocatable value plus or minus a constant is relocatable, the difference of two of one
space a constant; anything else that involves one (`KX*2`, `KX+KY`, `KX&$FF`) is an error. An operand that
relocates always takes the long form (an extension word, never the 6-bit `aa` address), and the forms with no
room for one refuse it: `jclr` / `jset` / `bset` / `bclr` on `x:KX+n`, `do x:KX`, `rep #KX`. Point a register
at the word instead. `--image` refuses a source without the four equates, one that does not start at
`org p:0`, or a word that does not relocate cleanly. A source without `K_XWORDS` builds an ABI 1 image (code
at `org p:$100`, header magic, 1, scratch, descriptors).

`tools/dsp56asm.py` covers the subset of DSP56300 instructions its header lists (moves, `mac`/`mpy`, `do`/
`rep`, jumps and calls, bit tests, `movep`). Encodings follow Motorola's public DSP56300 Family Manual.

## 7. Loading, switching and the load meter

When a slot gets a new image, the library:
1. switches that slot off and waits two frames (the dry mix and the other slots keep playing);
2. places the kernel: the first range of P ($100..$7FF), X ($E0..$BFF), Y and scratch (4 KiB aligned) that is
   free in each. There is no compaction: a range freed by another slot is reused when something fits in it,
   and an attach that fits the sums but no free range is refused;
3. relocates the code and writes it into P memory through the GP's P window, that range only, and points the
   slot's trampoline (a short `jmp` in the loop) at it, then reads both back. If the read-back differs, it
   restarts the GP from its bootstrap image, which also holds the new code; the restart stops the other slots,
   which the library loads again at its next update. `apu_gp_debug_code_restarts()` counts the restarts;
4. zeroes the slot's X block and scratch, writes the ABI words and the slot table entry, and switches it on.

The kernel starts from silence: delay lines are empty after every load. With nothing running, the meter's
reference (an empty frame's wait) is measured again.

**Load meter.** No cycle counter is known on the GP, so the loop counts how many times it polls for the next
frame start. The library compares that count with an empty frame's: the percentage is the share of the frame
the effects (kernels and their DMA) take. For scale, in the host simulator the reverb runs about 10,800
instructions a frame (34 % of the frame on a console), the echo about 1,060, the chorus about 4,950, and the
three together about 15,600. The on-chip effects run 1,900 (compressor) to 7,200 (autowah) each, and the
reverb with a four-band equalizer, the distortion and the autowah about 27,500: by the reverb's console scale
about 87 % of the frame, about the heaviest set the library's own kernels make (section 10). A kernel that overruns
does not crash, but the output gets late frames: watch `AL_XBOX_GP_LOAD_PEAK`, and `ac97_padded` /
`output_slips` in `AL_XBOX_stats`.

## 8. Testing on the host

`tools/dsp56sim.py` runs the loop and kernels on the PC with the same DMA model, so a kernel can be checked
sample by sample before it reaches a console. `tests/test_gp_programs.py` [5] does it for the echo:

```python
at = {'p': 0x140, 'x': 0x300, 'y': 0}                  # anywhere: the loader's job on a console
gp = new_gp([(ECHO, at)])                               # loop + kernel source, assembled at those bases
load_slot(gp, 0, 0, at, [100, 0, 0x400000])             # what apu_gp.c does: ABI words, trampoline, on
fifo = gp.frame({2: block_left, 3: block_right})        # one frame; returns the output
```

Its helpers `load_slot` (the library's load sequence) and `run_echo` (impulses in, output streams out) can be
copied for a new kernel. Parameter changes are tested by writing the mailbox (`MBOX`, `MB_N`, `MB_SEQ`) as the
library does. [9] checks that every library image relocated by the loader's rules equals its source assembled
at those bases, [10] that three kernels in one loop add exactly what each adds alone. The simulator knows the
instructions the assembler knows; it does not model the GP's timing, only the instruction count
(`gp.frame_steps` for the last frame). Its rounding (`rnd`, `mpyr`, `macr`) is convergent, the rounding mode
the GP starts in (the loop never changes SR).

For a kernel that implements an algorithm, `tests/test_gp_onchip.py` shows the next step: the kernel against a
floating-point model of the algorithm fed the kernel's own words (so the comparison measures the fixed-point
arithmetic), the error in LSB of OUT L / OUT R, and the words taken from the library itself through a small C
helper (`tests/gpsim_onchip_words.c`).

The library's loader is tested with the null backend in `tests/test_apu_gp.c` [7]-[9], and the AL entry points
in `tests/test_efx_program.c` and `tests/test_efx_multi.c`.

## 9. Checklist

- `org p:0`, `rts` at the end, under 0x700 words; addresses as `KX` / `KY` + offsets, never absolute.
- `K_SCRATCH_BYTES`, `K_DESCRIPTORS`, `K_XWORDS` (with the $28 ABI words) and `K_YWORDS` declared and true.
- Descriptors built on the first call; both chains end at `$004000`; K_RHEAD / K_WHEAD 0 if unused.
- Your X from `KX+$28`; Y from `KY`, initialised by you.
- Parameters read every frame; delays that change rewrite their read offsets.
- Headroom in the output; `AL_XBOX_GP_LOAD_PEAK` checked on a console with the worst case running.

## 10. The library's on-chip effects

`AL_EFFECT_RING_MODULATOR`, `AL_EFFECT_DISTORTION`, `AL_EFFECT_EQUALIZER`, `AL_EFFECT_COMPRESSOR` and
`AL_EFFECT_AUTOWAH` (phase9-07) are library kernels, loaded like the echo and the chorus: one image each in
`src/gp_kefx_<name>.h`, assembled from `dsp/gp_kefx_<name>.asm`, with the words `al_efx.c` derives from the
EFX values. They follow OpenAL Soft's algorithms. None uses Y RAM, scratch or DMA, so any three fit beside the
reverb's $8C0 X words, any four together, and each runs beside a chorus (which takes all of Y).

| Kernel | P words | X block | Instructions a frame (simulator) | Against OpenAL Soft's algorithm |
|---|---|---|---|---|
| `gp_kefx_ringmod` | 166 | $53 | 2,600 (no modulation) to 3,500 (sine) | within 0.3 LSB of 16 bits |
| `gp_kefx_distortion` | 169 | $33 | 4,200 to 4,900 (more with a hard edge) | within 0.65 LSB |
| `gp_kefx_equalizer` | 92 | $44 | 600 (every band at gain 1), 2,400 (one band), 6,400 (four) | within 0.04 LSB |
| `gp_kefx_compressor` | 103 | $2B | 1,800 to 1,900 | within 0.05 LSB |
| `gp_kefx_autowah` | 247 | $3C | 7,200 | within 3.3 LSB (OpenAL Soft's own single precision: 3.2) |

The comparison is with OpenAL Soft's algorithm in double precision on the kernel's own words, over tones,
noise and plucks, with the parameters at their defaults and at the ends of their ranges. In one loop the
reverb, a four-band equalizer, the distortion and the autowah take about 27,500 instructions a frame; the ring
modulator, the distortion, the equalizer and the compressor about 15,900.

**What they share:**
- **Input.** The ring modulator and the equalizer filter each bin on its own, as u = 2 bin (a library kernel's
  slot gets half the send on each bin, so a mono source gives u = the send on both sides, the level the echo's
  L + R has). The compressor and the autowah scale or filter each channel the same way, but follow the
  envelope of m = L + R, as OpenAL Soft follows its first channel; the distortion works on m alone and puts
  the result on both sides, as OpenAL Soft pans its distortion to the front.
- **Biquads.** Direct form 1 with the words b0 b1 b2 -a1 -a2 halved (quartered in the equalizer: a +18 dB
  shelf or peak has a b0 above 2), the sum shifted back, rounded (`rnd`), and the rounding's residual added
  into the next sample (first-order error feedback): a 20 Hz highpass stays within 0.05 LSB of 16 bits instead
  of 10. Every stage's output is limited to full scale, also where it feeds back (OpenAL Soft's is not).
- **The slot's gain.** The distortion, the compressor and the autowah apply `AL_EFFECTSLOT_GAIN` at their
  output (a word), as OpenAL Soft does; the ring modulator and the equalizer have their input scaled.

**Where they differ from OpenAL Soft:**
- **Ring modulator.** The phase is a 24-bit accumulator, OpenAL Soft's own resolution. The sine is an odd
  polynomial of degree 7 on the folded phase (within 7e-7); the square's and "no modulation"'s 1 is
  1 - 2^-23. The highpass is OpenAL Soft's (slope 0.75); below 1 Hz it is the identity (as OpenAL Soft's 0 Hz
  is), above 0.49 fs it stays at 0.49 fs.
- **Distortion.** OpenAL Soft's waveshaper is three stages of x (1 + k) / (1 + k |x|), the middle one negated.
  Each stage is a Moebius map, so the three compose into one: -x / (d + (1 - d) |x|) with
  d = ((1 - e) / (1 + e))^3, which the library computes (`tests/test_efx_distortion.c` checks the identity).
  The kernel divides by normalising d + (1 - d) |x| (shifts of 16, 8, 4, 2, 1) and one Newton-Raphson
  reciprocal (a linear guess within 1/17, three steps). It runs at 48 kHz, not 4x oversampled: harmonics above
  24 kHz alias, and the two filters are designed at 48 kHz with the Q OpenAL Soft's oversampled design gives
  them (cutoff and centre at most 0.45 fs, 1/Q at most 20). The gain is in the bandpass's b words, so the
  bandpass's own peaks are scaled down before anything is limited. With an edge near 1 the shaper's slope at
  0 reaches 2^23: one LSB of input at a zero crossing is a full-scale step, in any 24-bit implementation.
- **Equalizer.** Only the bands not at gain 1 run (N stages, packed in order, sharing their histories);
  changing which bands are at gain 1 moves the stages, and their states restart where they land (a click at
  most).
- **Compressor.** 1/env comes from one Newton-Raphson step a sample, from the last sample's (the envelope moves
  less than 0.03 % a sample). The amplitude's clamp at 2 cannot be reached here (the sum is limited to 1).
- **Autowah.** As in OpenAL Soft the peak runs in transposed direct form 2 (with words that move every sample
  the form changes the output: direct form 1 lands up to 1000 LSB of 16 bits away), its states kept at 48 bits
  and its output rounded with error feedback. The envelope is kept at 48 bits too. sin w0 and cos w0 come from
  the half angle (cos w0 = 1 - 2 sin^2(pi f), quartic polynomials within 7e-8), which keeps a1 accurate where
  w0 is small and a1 is near -2; 1/a0 from two Newton-Raphson steps. A peak at 20 Hz with Q 5 integrates
  small differences of its words over thousands of samples, so the output's distance from the double-precision
  model is set by the words' precision: about what OpenAL Soft's own single precision gives.

**Tests.** `tests/test_gp_onchip.py` (ring modulator, distortion, equalizer), `test_gp_onchip_env.py`
(compressor, autowah) and `test_gp_onchip_slots.py` (relocations, four kernels in one loop adding what each
adds alone) in the simulator, on the library's words; `tests/test_efx_<effect>.c` for the effect objects and
their words; `tests/test_efx_multi.c` [4] for the budgets (every set of three beside the reverb and of four;
X and P full give `AL_INVALID_OPERATION`).
