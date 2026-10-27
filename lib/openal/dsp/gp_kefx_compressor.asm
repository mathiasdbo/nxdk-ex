; AL_EFFECT_COMPRESSOR on the GP (lib/openal/docs/SOFT_GAP_PLAN.md
; phase9-07). Our own code, after the algorithm OpenAL Soft's compressor uses;
; runs inside dsp/gp_loop.asm as a library kernel (ABI 2, relocatable).
;
; An envelope follows the slot's input m = L + R (OpenAL Soft follows its
; first channel), and each channel (u = 2 bin) is scaled by its reciprocal:
;   amp = clamp(|m|, 1/2, 1), or 1 while switched off
;   amp > env: env = min(env A, amp);  amp < env: env = max(env R, amp)
;   OUT L += GAIN u L / env;  OUT R += GAIN u R / env
; A and R take the envelope from 1/2 to 2 in 0.1 s and back in 0.2 s (its
; fixed times); the envelope starts at 1. OpenAL Soft clamps amp to 2 at the
; top: here no input reaches 1. 1 / env comes from one Newton-Raphson step a
; sample from the last one (env moves by under 0.03 % a sample).
; Parameters (KPARAM + n), set by the library from the EFX effect:
;   0  on (0 / 1), 1  A - 1, 2  R, 3  GAIN = the slot's gain (Q23)
; No scratch, no DMA, no Y.

K_SCRATCH_BYTES equ 0           ; image header: scratch the kernel needs
K_DESCRIPTORS   equ 0           ; image header: descriptors a frame (both chains)
K_XWORDS        equ $2B         ; image header: the X block, ABI words included
K_YWORDS        equ 0

KIN     equ     KX              ; ABI (docs/GP_KERNELS.md)
KPARAM  equ     KX+8
OUTL    equ     $20
OUTR    equ     $40

ON      equ     KPARAM
ATT     equ     KPARAM+1        ; A - 1
REL     equ     KPARAM+2        ; R
GAIN    equ     KPARAM+3

INIT    equ     KX+$28          ; 1 once ENV / HG hold their start
ENV     equ     KX+$29          ; the envelope, 1/2 .. 1
HG      equ     KX+$2A          ; 1 / (2 env)

        org     p:0
kernel  move    x:INIT,a
        tst     a
        jne     ready
        move    #$7FFFFF,x0             ; the envelope starts at 1
        move    x0,x:ENV
        move    #$400000,x0
        move    x0,x:HG
        move    #1,x0
        move    x0,x:INIT

ready   move    x:KIN,r4                ; left bin
        move    x:KIN,a
        move    #32,x0
        add     x0,a
        move    a1,r5                   ; right bin
        move    #OUTL,r2
        move    #OUTR,r3
        do      #32,s_end               ; (the branches are in follow, far from the loop's end)
        jsr     follow                  ; y0 = 1 / (2 env)
        move    x:GAIN,y1
        move    x:(r4)+,x0
        mpy     x0,y0,a                 ; bin / (2 env)
        move    a,x0
        mpy     x0,y1,a
        asl     a
        asl     a                       ; GAIN u / env
        move    x:(r2),b
        add     a,b
        move    b,x:(r2)+
        move    x:(r5)+,x0
        mpy     x0,y0,a
        move    a,x0
        mpy     x0,y1,a
        asl     a
        asl     a
        move    x:(r3),b
        add     a,b
        move    b,x:(r3)+
s_end
        rts

; This sample's envelope from bins (r4) and (r5) -> y0 = 1 / (2 env), kept in
; HG. Uses a, b, x0, x1, y0, y1.
follow  move    x:(r4),a
        move    x:(r5),x0
        add     x0,a
        abs     a                       ; |m|
        move    #$400000,x0
        cmp     x0,a
        jge     e_min
        tfr     x0,a                    ; at least 1/2
e_min   move    x:ON,b
        tst     b
        jne     e_on
        move    #$7FFFFF,a              ; off: the amplitude is 1
e_on    move    a,x1                    ; amp (limited to 1)
        move    x:ENV,a
        move    a,y0                    ; env
        cmp     x1,a
        jeq     e_set
        jgt     e_rel
        move    x:ATT,x0                ; attack: env A = env + env (A - 1), at most amp
        macr    x0,y0,a
        cmp     x1,a
        jle     e_set
        tfr     x1,a
        jmp     e_set
e_rel   move    x:REL,x0                ; release: env R, at least amp
        mpyr    x0,y0,a
        cmp     x1,a
        jge     e_set
        tfr     x1,a
e_set   move    a,x0                    ; env
        move    x0,x:ENV
        move    x:HG,y0                 ; h += 2 h (1/2 - env h)
        move    #$400000,a
        mac     -x0,y0,a
        asl     a
        move    a,y1
        tfr     y0,a
        mac     y0,y1,a
        move    a,y0
        move    y0,x:HG
        rts
