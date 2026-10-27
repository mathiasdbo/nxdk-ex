; AL_EFFECT_AUTOWAH on the GP (lib/openal/docs/SOFT_GAP_PLAN.md phase9-07).
; Our own code, after the algorithm OpenAL Soft's autowah uses; runs inside
; dsp/gp_loop.asm as a library kernel (ABI 2, relocatable).
;
; An envelope follows the slot's input m = L + R (OpenAL Soft follows its
; first channel) and moves a resonant peak; each channel (u = 2 bin) goes
; through it:
;   s = PEAK |m|;  env = s + rate (env - s)   rate: attack if s > env, else release
;   (the envelope is kept at 48 bits and rounded for w0: the peak's output
;   integrates small changes of w0 over thousands of samples)
;   w0 = 2 pi min(bandwidth env + fmin, 0.46)  (20 .. 2500 Hz)
;   a peaking biquad at w0, Q 5, gain RES, its words recomputed every sample:
;     alpha = sin w0 / 10;  a0 = 1 + alpha / RES
;     b0 = (1 + alpha RES) / a0, b1 = a1 = -2 cos w0 / a0, b2 = (1 - alpha RES) / a0,
;     a2 = (1 - alpha / RES) / a0
;   OUT L += GAIN peak(u L);  OUT R += GAIN peak(u R)
; As in OpenAL Soft the filter runs in transposed direct form 2 (with words
; that move every sample, the form changes the output); its two states are
; kept at 48 bits (z / 8: a high and a low word). sin w0 and cos w0 come from
; the half angle (cos w0 = 1 - 2 sin^2(pi f) stays accurate where w0 is small
; and a1 is near -2), 1 / a0 from two Newton-Raphson steps. The output y is rounded and limited to full scale
; before it goes back into the states (OpenAL Soft's is not limited).
; Parameters (KPARAM + n), set by the library from the EFX effect (Q23):
;   0  1 - the attack rate, 1  1 - the release rate, 2  PEAK / 16, 3  16 bandwidth,
;   4  fmin (both / 48 kHz), 5  RES / 4, 6  1 / RES, 7  GAIN = the slot's gain
; No scratch, no DMA, no Y.

K_SCRATCH_BYTES equ 0           ; image header: scratch the kernel needs
K_DESCRIPTORS   equ 0           ; image header: descriptors a frame (both chains)
K_XWORDS        equ $3C         ; image header: the X block, ABI words included
K_YWORDS        equ 0

KIN     equ     KX              ; ABI (docs/GP_KERNELS.md)
KPARAM  equ     KX+8
OUTL    equ     $20
OUTR    equ     $40

KATT    equ     KPARAM          ; 1 - the attack rate
KREL    equ     KPARAM+1        ; 1 - the release rate
PKG     equ     KPARAM+2
BW16    equ     KPARAM+3
FMIN    equ     KPARAM+4
RES4    equ     KPARAM+5
IRES    equ     KPARAM+6
GAIN    equ     KPARAM+7

FMAX    equ     $3AE148         ; 0.46
QR      equ     $19999A         ; 1 / 5: alpha = sin w0 / (2 Q) = sin(pi f) cos(pi f) / 5, Q = 5
; sin(pi f) = 2 r P(r^2), cos(pi f) = 2 C(r^2), r = 2 f <= 0.92: quartics
; fitted on that range (P within 7e-8 relative, C within 4e-8)
SP0     equ     $6487ED
SP1     equ     $D6A887
SP2     equ     $0519A8
SP3     equ     $FFB363
SP4     equ     $000281
CP0     equ     $400000
CP1     equ     $B10B12
CP2     equ     $103BEF
CP3     equ     $FEAACC
CP4     equ     $000E36

ENV     equ     KX+$28          ; the envelope / 16 (high, low)
CO      equ     KX+$2A          ; this sample's b0 b1 -a1 b2 -a2, halved
ZL      equ     KX+$2F          ; left: z1 / 8, z2 / 8 (high, low each)
ZR      equ     KX+$33          ; right
ALPHA   equ     KX+$37
COSW    equ     KX+$38
TT      equ     KX+$39          ; alpha / RES
EL      equ     KX+$3A          ; left: y's rounding residual / 2
ER      equ     KX+$3B          ; right

        org     p:0
kernel  move    x:KIN,r4                ; left bin
        move    x:KIN,a
        move    #32,x0
        add     x0,a
        move    a1,r5                   ; right bin
        move    #OUTL,r2
        move    #OUTR,r3
        do      #32,s_end               ; (the branches are in coef, far from the loop's end)
        jsr     coef
        move    x:(r4)+,a
        asl     a
        move    a,x0                    ; u = 2 bin, limited
        move    #ZL,r1
        move    #ZL+2,r6
        move    #EL,r7
        jsr     tdf
        move    x:GAIN,y0
        move    x:(r2),a
        mac     x1,y0,a
        move    a,x:(r2)+
        move    x:(r5)+,a
        asl     a
        move    a,x0
        move    #ZR,r1
        move    #ZR+2,r6
        move    #ER,r7
        jsr     tdf
        move    x:GAIN,y0
        move    x:(r3),a
        mac     x1,y0,a
        move    a,x:(r3)+
s_end
        rts

; The envelope and this sample's words (CO) from bins (r4) and (r5).
; Uses a, b, x0, x1, y0, y1, r0.
coef    move    x:(r4),a
        move    x:(r5),x0
        add     x0,a
        abs     a
        move    a,x0                    ; |m|, limited
        move    x:PKG,y0
        mpyr    x0,y0,a
        move    a,x1                    ; s / 16
        move    x:ENV,b
        move    x:ENV+1,b0              ; env / 16, 48 bits
        move    x:KREL,y0
        cmp     x1,b
        jge     c_rel                   ; s <= env: release
        move    x:KATT,y0
c_rel   tfr     b,a
        sub     x1,a
        rnd     a
        move    a,x0                    ; env - s
        mac     -x0,y0,b                ; env - (1 - rate) (env - s)
        move    b1,x:ENV
        move    b0,x:ENV+1
        rnd     b
        move    b,x0                    ; env / 16, rounded
        move    x:BW16,y0
        move    x:FMIN,a
        mac     x0,y0,a                 ; f = bandwidth env + fmin
        move    #FMAX,x0
        cmp     x0,a
        jle     c_f
        tfr     x0,a
c_f     asl     a
        rnd     a
        move    a,x0                    ; r = 2 f: w0 = pi r
        mpyr    x0,x0,a
        move    a,x1                    ; r^2
        move    #SP4,y0                 ; sin(pi f) = 2 r P(r^2)
        move    #SP3,a
        macr    x1,y0,a
        move    a,y0
        move    #SP2,a
        macr    x1,y0,a
        move    a,y0
        move    #SP1,a
        macr    x1,y0,a
        move    a,y0
        move    #SP0,a
        macr    x1,y0,a
        move    a,y0
        mpyr    x0,y0,a
        asl     a
        move    a,x0                    ; sin(pi f)
        move    #CP4,y0                 ; cos(pi f) = 2 C(r^2)
        move    #CP3,a
        macr    x1,y0,a
        move    a,y0
        move    #CP2,a
        macr    x1,y0,a
        move    a,y0
        move    #CP1,a
        macr    x1,y0,a
        move    a,y0
        move    #CP0,a
        macr    x1,y0,a
        asl     a
        move    a,y0                    ; cos(pi f)
        mpyr    x0,y0,a                 ; sin w0 / 2
        move    a,y0
        move    #QR,y1
        mpyr    y0,y1,a                 ; alpha = sin w0 / 10
        move    a,x1
        move    x1,x:ALPHA
        mpy     x0,x0,a                 ; cos w0 = 1 - 2 sin(pi f)^2
        asl     a
        neg     a
        move    #$400000,y0
        add     y0,a
        add     y0,a
        rnd     a
        move    a,x:COSW
        move    x1,x0                   ; alpha
        move    x:IRES,y0
        mpyr    x0,y0,a                 ; t = alpha / RES
        move    a,x1
        move    x1,x:TT
        move    #$7FFFFF,a              ; 1 / a0 = 1 / (1 + t): from 1 - t,
        sub     x1,a                    ; g -= g ((1 + t) g - 1), twice
        move    a,y0
        move    #$800000,y1             ; -1
        tfr     y0,a
        mac     x1,y0,a
        add     y1,a
        move    a,x0
        tfr     y0,a
        mac     -x0,y0,a
        move    a,y0
        tfr     y0,a
        mac     x1,y0,a
        add     y1,a
        move    a,x0
        tfr     y0,a
        mac     -x0,y0,a
        move    a,y0                    ; 1 / a0
        move    x:ALPHA,x0
        move    x:RES4,y1
        mpy     x0,y1,a
        asl     a
        rnd     a
        move    a,x0                    ; alpha RES / 2
        move    #CO,r0
        move    #$400000,b              ; 1/2
        tfr     b,a
        add     x0,a
        move    a,x1
        mpyr    x1,y0,a                 ; b0 / 2 = (1/2 + alpha RES / 2) / a0
        move    a,x:(r0)+
        move    x:COSW,x1
        mpyr    -x1,y0,a                ; b1 / 2 = -cos w0 / a0
        move    a,x:(r0)+
        neg     a                       ; -a1 / 2
        move    a,x:(r0)+
        tfr     b,a
        sub     x0,a
        move    a,x1
        mpyr    x1,y0,a                 ; b2 / 2 = (1/2 - alpha RES / 2) / a0
        move    a,x:(r0)+
        move    x:TT,a
        asr     a
        neg     a
        add     b,a
        move    a,x1
        mpyr    -x1,y0,a                ; -a2 / 2 = -(1/2 - t / 2) / a0
        move    a,x:(r0)+
        rts

; The peak on one channel, transposed direct form 2. x0 = u; r1 -> z1 / 8,
; r6 -> z2 / 8 (high, low); r7 -> y's last rounding residual / 2 (first-order
; error feedback, as in the other kernels' biquads). -> x1 = y (rounded,
; limited); the states moved. Uses a, b, x1, y0, y1, r0.
tdf     move    #CO,r0
        move    x:(r1)+,a
        move    x:(r1)-,a0              ; z1 / 8
        asl     a       x:(r0)+,y0      ; b0 / 2
        asl     a                       ; z1 / 2
        mac     x0,y0,a x:(r7),x1       ; y / 2;  x1 = the residual / 2
        asl     a
        move    #1,y1
        mac     x1,y1,a                 ; + the last residual
        tfr     a,b
        rnd     a
        sub     a,b
        asr     b
        move    b0,x:(r7)
        move    a,x1                    ; y
        move    x:(r6)+,a
        move    x:(r6)-,a0              ; z2 / 8
        asl     a       x:(r0)+,y0      ; b1 / 2
        asl     a                       ; z2 / 2
        mac     x0,y0,a x:(r0)+,y0      ; + b1 u / 2;          y0 = -a1 / 2
        mac     x1,y0,a x:(r0)+,y0      ; - a1 y / 2 = z1 / 2;  y0 = b2 / 2
        asr     a
        asr     a
        move    a1,x:(r1)+
        move    a0,x:(r1)-              ; z1 / 8
        mpy     x0,y0,a x:(r0)+,y0      ; b2 u / 2;            y0 = -a2 / 2
        mac     x1,y0,a                 ; z2 / 2
        asr     a
        asr     a
        move    a1,x:(r6)+
        move    a0,x:(r6)-              ; z2 / 8
        rts
