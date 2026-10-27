; AL_EFFECT_RING_MODULATOR on the GP (lib/openal/docs/SOFT_GAP_PLAN.md
; phase9-07). Our own code, after the algorithm OpenAL Soft's modulator uses;
; runs inside dsp/gp_loop.asm as a library kernel (ABI 2, relocatable).
;
; Each channel of the slot's pair on its own (u = 2 bin: a builtin slot gets
; half the send on each bin, so a mono source gives u = the send on both):
;   y = hp(u) w(phase)                 hp: a biquad highpass (direct form 1)
;   phase += STEP (24 bits, one cycle = 2^24, before each sample)
;   w: sine sin(2 pi phase / 2^24), sawtooth phase / 2^23 - 1, square
;      -1 then +1 (bit 23 of the phase), or 1 (STEP 0: no modulation)
;   OUT L += y(L);  OUT R += y(R)
; Parameters (KPARAM + n), set by the library from the EFX effect:
;   0  STEP, 1  waveform (0 sine, 1 sawtooth, 2 square, 3 none),
;   2..6  the highpass's b0 b1 b2 -a1 -a2, halved (Q23)
; No scratch, no DMA, no Y.

K_SCRATCH_BYTES equ 0           ; image header: scratch the kernel needs
K_DESCRIPTORS   equ 0           ; image header: descriptors a frame (both chains)
K_XWORDS        equ $53         ; image header: the X block, ABI words included
K_YWORDS        equ 0

KIN     equ     KX              ; ABI (docs/GP_KERNELS.md)
KPARAM  equ     KX+8
OUTL    equ     $20
OUTR    equ     $40

STEP    equ     KPARAM
WAVE    equ     KPARAM+1
HPC     equ     KPARAM+2        ; the highpass's 5 words

; sin(pi r / 2) = 2 r P(r^2), P a cubic (minimax, within 7e-7 of the sine)
SC0     equ     $6487D7
SC1     equ     $D6A9B2
SC2     equ     $051571
SC3     equ     $FFB903

PH      equ     KX+$28          ; the phase (24 bits)
HL      equ     KX+$29          ; left: x[-1] x[-2] y[-1] y[-2]
HR      equ     KX+$2D          ; right
EL      equ     KX+$31          ; left: the rounding's residual / 2
ER      equ     KX+$32          ; right
MOD     equ     KX+$33          ; this frame's 32 modulator samples

        org     p:0
kernel  move    #MOD,r2
        move    x:STEP,y1
        move    x:WAVE,a
        move    #1,x1
        cmp     x1,a
        jeq     saw
        move    #2,x1
        cmp     x1,a
        jeq     square
        move    #3,x1
        cmp     x1,a
        jeq     one

        do      #32,sine_e              ; sine
        move    x:PH,a
        add     y1,a
        move    a1,x0                   ; the phase, wrapped
        move    x0,x:PH
        jsr     sinpi
        move    a,x:(r2)+
sine_e
        jmp     filt

saw     do      #32,saw_e               ; sawtooth: the phase with bit 23 flipped
        move    x:PH,a
        add     y1,a
        move    a1,x0
        move    x0,x:PH
        tfr     x0,a
        move    #$800000,x1
        add     x1,a
        move    a1,x:(r2)+
saw_e
        jmp     filt

square  move    #32,b                   ; square: -1 while bit 23 is clear
        move    #1,y0
sq_l    move    x:PH,a
        add     y1,a
        move    a1,x0
        move    x0,x:PH
        tfr     x0,a
        move    #$7FFFFF,x1
        tst     a
        jlt     sq_put
        move    #$800000,x1
sq_put  move    x1,x:(r2)+
        sub     y0,b
        jne     sq_l
        jmp     filt

one     move    #$7FFFFF,x0             ; no modulation (STEP 0)
        do      #32,one_e
        move    x0,x:(r2)+
one_e

filt    move    x:KIN,r4
        move    #HL,r5
        move    #EL,r7
        move    #OUTL,r3
        jsr     chan
        move    x:KIN,a
        move    #32,x0
        add     x0,a
        move    a1,r4
        move    #HR,r5
        move    #ER,r7
        move    #OUTR,r3
        jsr     chan
        rts

; r4 = an input bin, r5 = its filter state, r7 its residual, r3 = OUT L or R:
; OUT += hp(2 bin) x the modulator
chan    move    #MOD,r2
        do      #32,ch_e
        move    x:(r4)+,a
        asl     a
        move    a,x0                    ; u = 2 bin, limited
        move    #HPC,r0
        move    r5,r1
        move    r7,r6
        jsr     bq
        move    x:(r1),x1               ; y history: y y[-1]
        move    x0,x:(r1)+
        move    x1,x:(r1)
        move    x:(r2)+,y0              ; w
        move    x:(r3),a
        mac     x0,y0,a
        move    a,x:(r3)+
ch_e
        rts

; One biquad stage, direct form 1, with first-order error feedback (the
; rounding's residual goes into the next sample: a highpass or lowpass far
; below fs keeps its noise near 24 bits). x0 = its input; r0 -> b0 b1 b2 -a1
; -a2 (halved); r1 -> x[-1] x[-2], then y[-1] y[-2]; r6 -> the residual / 2.
; -> x0 = y (rounded, limited); x[-1] x[-2] shifted; r1 -> y[-1] (the caller
; shifts that pair); r6 + 1. Uses a, b, x1, y0, y1.
bq      clr     a       x:(r0)+,y0      ; b0
        mac     x0,y0,a x:(r1),x1       ; b0 x;            x1 = x[-1]
        move    x:(r0)+,y0              ; b1
        mac     x1,y0,a x:(r0)+,y0      ; b1 x[-1];        y0 = b2
        move    x0,x:(r1)+              ; x[-1] = x
        move    x:(r1),x0               ; x[-2]
        mac     x0,y0,a x:(r0)+,y0      ; b2 x[-2];        y0 = -a1
        move    x1,x:(r1)+              ; x[-2] = the old x[-1]
        move    x:(r1)+,x1              ; y[-1]
        mac     x1,y0,a x:(r0)+,y0      ; -a1 y[-1];       y0 = -a2
        move    x:(r1)-,x1              ; y[-2]            (r1 -> y[-1])
        mac     x1,y0,a x:(r6),x1       ; -a2 y[-2];       x1 = the residual / 2
        asl     a
        move    #1,y1
        mac     x1,y1,a                 ; + the last residual
        tfr     a,b
        rnd     a
        sub     a,b                     ; this one
        asr     b
        move    b0,x:(r6)+
        move    a,x0
        rts

; x0 = a phase p (-1 .. 1, a whole turn) -> a = sin(pi p). Folded with
; r = 2 |p + 1/2| - 1 (wrapping), then sin(pi r / 2) = 2 r P(r^2).
; Uses a, x0, x1, y0.
sinpi   move    #$400000,x1
        tfr     x0,a
        add     x1,a
        move    a1,x1                   ; p + 1/2, wrapped
        tfr     x1,a
        abs     a
        asl     a
        move    #$800000,x1
        add     x1,a                    ; r
        move    a,x0                    ; (1 limited to 1 - 2^-23)
        mpy     x0,x0,a
        move    a,x1                    ; r^2
        move    #SC3,y0
        move    #SC2,a
        mac     x1,y0,a
        move    a,y0
        move    #SC1,a
        mac     x1,y0,a
        move    a,y0
        move    #SC0,a
        mac     x1,y0,a                 ; P
        move    a,y0
        mpy     x0,y0,a
        asl     a                       ; 2 r P
        rts
