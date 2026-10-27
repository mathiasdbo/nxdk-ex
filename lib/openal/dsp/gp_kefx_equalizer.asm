; AL_EFFECT_EQUALIZER on the GP (lib/openal/docs/SOFT_GAP_PLAN.md
; phase9-07). Our own code, after the algorithm OpenAL Soft's equalizer uses;
; runs inside dsp/gp_loop.asm as a library kernel (ABI 2, relocatable).
;
; Each channel of the slot's pair on its own (u = 2 bin: a builtin slot gets
; half the send on each bin, so a mono source gives u = the send on both):
;   OUT L += eq(u L);  OUT R += eq(u R)
; eq: OpenAL Soft's four biquads in a row (low shelf, mid 1 and mid 2
; peaking, high shelf). A band at gain 1 is the identity there, so the library
; sends only the others, in order: N (0..4) stages. Each is direct form 1 with
; its words quartered (a shelf or peak at +18 dB has a b0 above 2), the sum
; times 4, rounded with first-order error feedback, limited to full scale.
; The stages share histories (a stage's output history is the next one's
; input history). Changing which bands are at gain 1 moves the stages, and
; their states restart where they land (a click at most).
; Parameters (KPARAM + n), set by the library from the EFX effect:
;   0  N, 1..5N  each stage's b0 b1 b2 -a1 -a2, quartered (Q23)
; No scratch, no DMA, no Y.

K_SCRATCH_BYTES equ 0           ; image header: scratch the kernel needs
K_DESCRIPTORS   equ 0           ; image header: descriptors a frame (both chains)
K_XWORDS        equ $44         ; image header: the X block, ABI words included
K_YWORDS        equ 0

KIN     equ     KX              ; ABI (docs/GP_KERNELS.md)
KPARAM  equ     KX+8
OUTL    equ     $20
OUTR    equ     $40

NST     equ     KPARAM          ; stages
EQC     equ     KPARAM+1        ; their words, 5 each

HL      equ     KX+$28          ; left: the input's history, then each stage's output's (2 words each)
HR      equ     KX+$32          ; right
EL      equ     KX+$3C          ; left: each stage's rounding residual / 2
ER      equ     KX+$40          ; right

        org     p:0
kernel  move    x:NST,a
        move    a1,n7                   ; N, the stage loop's count
        move    x:KIN,r4
        move    #HL,r5
        move    #EL,r2
        move    #OUTL,r3
        tst     a
        jeq     flat
        jsr     chan
        move    x:KIN,a
        move    #32,x0
        add     x0,a
        move    a1,r4
        move    #HR,r5
        move    #ER,r2
        move    #OUTR,r3
        jsr     chan
        rts

flat    move    x:KIN,a                 ; no stage: OUT += u, both channels
        move    #32,x0
        add     x0,a
        move    a1,r5
        move    #OUTR,r2
        do      #32,fl_e
        move    x:(r4)+,a
        asl     a
        move    a,x0
        move    x:(r3),a
        add     x0,a
        move    a,x:(r3)+
        move    x:(r5)+,a
        asl     a
        move    a,x0
        move    x:(r2),a
        add     x0,a
        move    a,x:(r2)+
fl_e
        rts

; r4 = an input bin, r5 = its histories, r2 = its residuals, r3 = OUT L or R:
; OUT += the N stages of 2 bin
chan    move    #1,y1
        do      #32,ch_e
        move    x:(r4)+,a
        asl     a
        move    a,x0                    ; u = 2 bin, limited
        move    #EQC,r0
        move    r5,r1
        move    r2,r6
        do      n7,st_e                 ; a stage: x0 in, x0 out
        clr     a       x:(r0)+,y0      ; b0
        mac     x0,y0,a x:(r1),x1       ; b0 x;            x1 = x[-1]
        move    x:(r0)+,y0              ; b1
        mac     x1,y0,a x:(r0)+,y0      ; b1 x[-1];        y0 = b2
        move    x0,x:(r1)+              ; x[-1] = x
        move    x:(r1),x0               ; x[-2]
        mac     x0,y0,a x:(r0)+,y0      ; b2 x[-2];        y0 = -a1
        move    x1,x:(r1)+              ; x[-2] = the old x[-1]  (r1 -> this stage's y[-1])
        move    x:(r1)+,x1              ; y[-1]
        mac     x1,y0,a x:(r0)+,y0      ; -a1 y[-1];       y0 = -a2
        move    x:(r1)-,x1              ; y[-2]            (r1 -> y[-1]: the next stage's x[-1])
        mac     x1,y0,a x:(r6),x1       ; -a2 y[-2];       x1 = the residual / 2
        asl     a
        asl     a                       ; the words were quartered
        mac     x1,y1,a                 ; + the last residual (y1 = 2^-23)
        tfr     a,b
        rnd     a
        sub     a,b                     ; this one
        asr     b
        move    b0,x:(r6)+
        move    a,x0                    ; y, limited
st_e
        move    x:(r1),x1               ; the last stage's output history: y y[-1]
        move    x0,x:(r1)+
        move    x1,x:(r1)
        move    x:(r3),a
        add     x0,a
        move    a,x:(r3)+
ch_e
        rts
