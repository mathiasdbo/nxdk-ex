; AL_EFFECT_CHORUS and AL_EFFECT_FLANGER on the GP (lib/openal/docs/SOFT_GAP_PLAN.md
; phase9-06). Our own code, from the EFX chorus and flanger parameter
; descriptions; runs inside dsp/gp_loop.asm as a library kernel, loaded like a
; custom kernel. The two effects differ only in their parameter ranges.
;
; The slot's two bins summed are the input m (each send arrives half on each).
; One line in Y RAM, written and read every sample, two modulated taps:
;   line[t] = m
;   dL = D + DD w(p),  dR = D + DD w(p + PHASE)   (w: sine or triangle, -1..1)
;   tL = line[t - dL], tR = line[t - dR]           (linear interpolation)
;   OUT L += tL;  OUT R += tR
;   line[t] += FEEDBACK (tL + tR) / 2
; Parameters (KPARAM + n), set by the library from the EFX effect:
;   0  D / 2048, 1  DD / 2048 (delays in samples, D + DD <= 1536)
;   2  feedback, 3  LFO step a sample (one cycle = 2.0), 4  PHASE / 180,
;   5  waveform (0 sine, 1 triangle), all Q23
; The line takes all of Y RAM (2048 words); no scratch, no DMA.

K_SCRATCH_BYTES equ 0           ; image header: scratch the kernel needs
K_DESCRIPTORS   equ 0           ; image header: descriptors a frame (both chains)

KIN     equ     $E0             ; ABI (gp_loop.asm)
KPARAM  equ     $E8
OUTL    equ     $20
OUTR    equ     $40

LINE    equ     2048            ; samples in the line (Y RAM)
SIN_K   equ     $1CCCCD         ; 0.225: the parabola's refinement

INIT    equ     $108            ; 1 once the line is cleared
WPTR    equ     $109            ; the line's write index
PH      equ     $10A            ; the LFO phase, -1 .. 1 (wraps)
MIN     equ     $10B            ; this sample's input
TV      equ     $10C            ; tap: its value
TL      equ     $10D            ; the left tap's value
SCNT    equ     $10E            ; samples left in the frame

        org     p:$100
kernel  move    x:INIT,a
        tst     a
        jne     ready

; first call: the line starts from silence (Y RAM keeps what was there)
        clr     a
        move    #0,r0
        do      #LINE,clr_end
        move    a,y:(r0)+
clr_end
        move    #1,x0
        move    x0,x:INIT

ready   move    x:KIN,r4                ; left bin
        move    x:KIN,a
        add     #32,a
        move    a1,r5                   ; right bin
        move    #OUTL,r2
        move    #OUTR,r3
        move    #32,x0
        move    x0,x:SCNT

sloop   move    x:(r4)+,a
        move    x:(r5)+,x0
        add     x0,a                    ; m
        move    a,x:MIN
        move    x:WPTR,r6
        move    a,y:(r6)                ; line[t] = m (a delay under one sample reads it)

        move    x:PH,a                  ; the LFO: phase += step, wrapping at 1
        move    x:KPARAM+3,x0
        add     x0,a
        move    a1,x:PH

        move    x:PH,x1                 ; left tap
        jsr     tap
        move    x:TV,a
        move    a,x:TL
        move    x:(r2),b
        add     a,b
        move    b,x:(r2)+

        move    x:PH,a                  ; right tap: phase + PHASE
        move    x:KPARAM+4,x0
        add     x0,a
        move    a1,x1
        jsr     tap
        move    x:TV,x0
        move    x:(r3),b
        add     x0,b
        move    b,x:(r3)+

        move    x:TL,a                  ; line[t] += FEEDBACK (tL + tR) / 2
        add     x0,a
        asr     a
        move    a,x0
        move    x:MIN,a
        move    x:KPARAM+2,y0
        mac     x0,y0,a
        move    a,y:(r6)

        move    x:WPTR,a                ; t + 1 (mod LINE)
        move    #1,x0
        add     x0,a
        move    #LINE,x0
        cmp     x0,a
        jlt     w_ok
        clr     a
w_ok    move    a1,x:WPTR

        move    x:SCNT,a
        move    #1,x0
        sub     x0,a
        move    a1,x:SCNT
        jne     sloop
        rts

; x1 = an LFO phase -> x:TV = line[t - (D + DD w)], interpolated
; (uses a, b, x0, x1, y0, y1, r0, r1)
tap     move    x:KPARAM+5,a
        tst     a
        jne     tri
        tfr     x1,a                    ; sine: y = 4 p (1 - |p|), refined
        abs     a
        move    a,x0
        move    #$7FFFFF,b
        sub     x0,b                    ; 1 - |p|
        move    b,y0
        mpy     x1,y0,a                 ; p (1 - |p|), up to 0.25
        asl     a
        asl     a
        move    a,x0                    ; y
        tfr     x0,b
        abs     b
        move    b,y0
        mpy     x0,y0,a                 ; y |y|
        sub     x0,a                    ; y |y| - y
        move    a,y0
        move    #SIN_K,y1
        tfr     x0,a
        mac     y0,y1,a                 ; y + 0.225 (y |y| - y)
        move    a,x0
        jmp     delay
tri     tfr     x1,a                    ; triangle: 1 - 2 |p|
        abs     a
        asl     a
        move    #$7FFFFF,b
        sub     a,b
        move    b,x0

delay   move    x:KPARAM,a              ; d = D + DD w, never below 0
        move    x:KPARAM+1,y0
        mac     x0,y0,a
        jge     d_pos
        clr     a
d_pos   move    a,x0                    ; d / 2048
        move    #$000800,y0
        mpy     x0,y0,a                 ; A1 = whole samples, A0 = the fraction
        move    a0,b
        lsr     b
        move    b1,y1                   ; the fraction, 0 .. 1
        move    a1,x0
        move    x:WPTR,a                ; s0 = line[t - i]
        sub     x0,a
        jge     a_ok
        move    #LINE,x0
        add     x0,a
a_ok    move    a1,r0
        move    #1,x0                   ; s1 = line[t - i - 1]
        sub     x0,a
        jge     b_ok
        move    #LINE,x0
        add     x0,a
b_ok    move    a1,r1
        move    y:(r0),x0
        move    y:(r1),a
        sub     x0,a                    ; s1 - s0
        move    a,x1
        tfr     x0,a
        mac     x1,y1,a                 ; s0 + f (s1 - s0)
        move    a,x:TV
        rts
