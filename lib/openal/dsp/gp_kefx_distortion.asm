; AL_EFFECT_DISTORTION on the GP (lib/openal/docs/SOFT_GAP_PLAN.md
; phase9-07). Our own code, after the algorithm OpenAL Soft's distortion uses;
; runs inside dsp/gp_loop.asm as a library kernel (ABI 2, relocatable).
;
; The slot's two bins summed are the input m (each send arrives half on each),
; as OpenAL Soft distorts one channel and pans it to the front:
;   v = lp(m)                                  lp: biquad lowpass
;   s = -sign(v) |v| / (d + (1 - d) |v|)       the waveshaper
;   OUT L += GAIN bp(s);  OUT R += GAIN bp(s)  bp: biquad bandpass (the EQ)
; (GAIN is in the bandpass's b words, so the bandpass's own peaks above full
; scale are scaled down before anything is limited, as in floating point).
; Every stage's output is limited to full scale (OpenAL Soft's is not).
; OpenAL Soft's waveshaper is three stages of x (1 + k) / (1 + k |x|), the
; middle one negated, k = 2 e / (1 - e); they compose to the one above with
; d = ((1 - e) / (1 + e))^3, computed by the library. It runs them 4x
; oversampled; this kernel runs at 48 kHz (no oversampling: the shaper's
; harmonics above 24 kHz alias, and the filters are designed at 48 kHz).
; Parameters (KPARAM + n), set by the library from the EFX effect:
;   0..4  the lowpass's b0 b1 b2 -a1 -a2, halved; 5..9  the bandpass's,
;   its b0 b1 b2 times GAIN = the EFX gain x the slot's gain; 10  d, 11  1 - d
; No scratch, no DMA, no Y.

K_SCRATCH_BYTES equ 0           ; image header: scratch the kernel needs
K_DESCRIPTORS   equ 0           ; image header: descriptors a frame (both chains)
K_XWORDS        equ $33         ; image header: the X block, ABI words included
K_YWORDS        equ 0

KIN     equ     KX              ; ABI (docs/GP_KERNELS.md)
KPARAM  equ     KX+8
OUTL    equ     $20
OUTR    equ     $40

LPC     equ     KPARAM          ; the lowpass's 5 words
BPC     equ     KPARAM+5        ; the bandpass's
SD      equ     KPARAM+10       ; d
SD1     equ     KPARAM+11       ; 1 - d

LPH     equ     KX+$28          ; lowpass: m[-1] m[-2] v[-1] v[-2]
BPH     equ     KX+$2C          ; bandpass: s[-1] s[-2] y[-1] y[-2]
LPE     equ     KX+$30          ; the lowpass's rounding residual / 2
BPE     equ     KX+$31          ; the bandpass's
VT      equ     KX+$32          ; this sample's v

        org     p:0
kernel  move    x:KIN,r4                ; left bin
        move    x:KIN,a
        move    #32,x0
        add     x0,a
        move    a1,r5                   ; right bin
        move    #OUTL,r2
        move    #OUTR,r3

        do      #32,s_end               ; (the branches are in shape, far from the loop's end)
        move    x:(r4)+,a
        move    x:(r5)+,x0
        add     x0,a                    ; m
        move    a,x0                    ; (limited)
        move    #LPC,r0
        move    #LPH,r1
        move    #LPE,r6
        jsr     bq                      ; v = lp(m)
        move    x:(r1),x1
        move    x0,x:(r1)+
        move    x1,x:(r1)
        jsr     shape                   ; s
        move    #BPC,r0
        move    #BPH,r1
        move    #BPE,r6
        jsr     bq                      ; GAIN bp(s)
        move    x:(r1),x1
        move    x0,x:(r1)+
        move    x1,x:(r1)
        move    x:(r2),a
        add     x0,a
        move    a,x:(r2)+
        move    x:(r3),a
        add     x0,a
        move    a,x:(r3)+
s_end
        rts

; x0 = v -> x0 = -sign(v) u / D, u = |v|, D = d + (1 - d) u (0 < D <= 1,
; u <= D). D is normalised to 0.5 .. 1 (u with it, so u / D stays), then
; 1 / (2 D) by Newton-Raphson from a linear guess (within 1/17, three steps).
; Uses a, b, x0, x1, y0, y1.
shape   move    x0,x:VT
        tfr     x0,a
        abs     a
        move    a,x1                    ; u (|-1| limited)
        move    x:SD1,y0
        move    x:SD,b
        mac     x1,y0,b                 ; D, exact in b
        move    #$80,x0                 ; D < 2^-16: D and u times 2^16
        cmp     x0,b
        jge     n8
        rep     #16
        asl     b
        move    #$8000,y0
        mpy     x1,y0,a
        move    a0,x1
n8      move    #$8000,x0               ; D < 2^-8: times 2^8
        cmp     x0,b
        jge     n4
        rep     #8
        asl     b
        move    #$80,y0
        mpy     x1,y0,a
        move    a0,x1
n4      move    #$80000,x0              ; D < 2^-4: times 2^4
        cmp     x0,b
        jge     n2
        rep     #4
        asl     b
        move    #$8,y0
        mpy     x1,y0,a
        move    a0,x1
n2      move    #$200000,x0             ; D < 2^-2: times 4
        cmp     x0,b
        jge     n1
        asl     b
        asl     b
        move    #$2,y0
        mpy     x1,y0,a
        move    a0,x1
n1      move    #$400000,x0             ; D < 2^-1: times 2
        cmp     x0,b
        jge     nd
        asl     b
        move    #$1,y0
        mpy     x1,y0,a
        move    a0,x1
nd      move    b,x0                    ; D, 0.5 .. 1
        move    #$5A5A5A,a              ; 12/17
        move    #$3C3C3C,y0             ; 8/17
        mac     -x0,y0,a
        asl     a
        move    a,y0                    ; h = 24/17 - 16/17 D ~ 1 / (2 D)
        move    #$400000,a              ; h += 2 h (1/2 - D h), three times
        mac     -x0,y0,a
        asl     a
        move    a,y1
        tfr     y0,a
        mac     y0,y1,a
        move    a,y0
        move    #$400000,a
        mac     -x0,y0,a
        asl     a
        move    a,y1
        tfr     y0,a
        mac     y0,y1,a
        move    a,y0
        move    #$400000,a
        mac     -x0,y0,a
        asl     a
        move    a,y1
        tfr     y0,a
        mac     y0,y1,a
        move    a,y0
        mpy     x1,y0,a
        asl     a                       ; u / D = 2 u h
        move    x:VT,b
        tst     b
        jlt     sh_neg
        neg     a                       ; v >= 0: negative
sh_neg  move    a,x0                    ; (limited)
        rts

; One biquad stage, direct form 1, with first-order error feedback (the
; rounding's residual goes into the next sample: a filter far below fs keeps
; its noise near 24 bits). x0 = its input; r0 -> b0 b1 b2 -a1 -a2 (halved);
; r1 -> x[-1] x[-2], then y[-1] y[-2]; r6 -> the residual / 2. -> x0 = y
; (rounded, limited); x[-1] x[-2] shifted; r1 -> y[-1] (the caller shifts that
; pair). Uses a, b, x1, y0, y1.
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
        move    b0,x:(r6)
        move    a,x0
        rts
