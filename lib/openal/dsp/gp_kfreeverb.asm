; GP kernel: the reverb (lib/openal/docs/GP_REVERB_PLAN.md, EFX_PLAN.md 3.3).
; Our own code; runs inside dsp/gp_loop.asm as a relocatable kernel (ABI 2,
; docs/GP_KERNELS.md), in any effect slot.
;
; The loop has read one 32-word block of each of the 25 delay lines (system
; RAM, circular scratch buffers) into its IN block (chain at RCHAIN) and put
; the dry bins in OUT L/R. Then:
;   pre:     PRE_OUT = lowpass((bin L + bin R) * INGAIN)  -> pre-delay line
;            (the input pair at X:KIN: the slot's two bins)
;   combs:   y = CIN; ACC += y; lp = y + damp (lp - y); COUT = PRE_IN + g lp
;            (8 per ear, table CTAB: CIN, COUT, ACC, g, damp, lp)
;   allpass: v = AIN; w = s + apg v; AOUT = w; s = v - apg w
;            (4 per ear in series on ACC L / ACC R, table ATAB: AIN, AOUT, ACC)
;   out:     OUT L/R += WET * ACC L/R
; and the loop writes each line's OUT block (chain at WCHAIN, ending at end of
; list). The CPU writes the descriptors, tables and coefficients for the
; slot's X block and scratch (apu_reverb.c). Each line is at least 32 samples
; long, so a frame never reads what it writes.

K_SCRATCH_BYTES equ $2D000      ; image header: the 25 lines (apu_reverb_scratch_bytes())
K_DESCRIPTORS   equ 50          ; image header: one read and one write per line
K_XWORDS        equ $8C0        ; image header: the X block, ABI words included
K_YWORDS        equ 0

KIN     equ     KX              ; ABI: X address of the input pair's left bin (the right one follows)
INGAIN  equ     KX+$28          ; Q23 coefficients and states
INLP    equ     KX+$29
INST    equ     KX+$2A
WET     equ     KX+$2B
APG     equ     KX+$2C
CTAB    equ     KX+$30          ; 16 x 6 words
ATAB    equ     KX+$90          ; 8 x 3 words
ACCL    equ     KX+$B0          ; ACC R at ACC L + 32
ACCR    equ     KX+$D0
RCHAIN  equ     KX+$F0          ; line k: read descriptor at RCHAIN + 16k, write at WCHAIN + 16k
WCHAIN  equ     KX+$F8
PRE_IN  equ     KX+$280         ; line k: IN block PRE_IN + 64k, OUT block PRE_OUT + 64k
PRE_OUT equ     KX+$2A0
OUTL    equ     $20             ; the loop's OUT L/R (gp_loop.asm)

        org     p:0
kernel
; input: (L + R) * gain, lowpass, into the pre-delay line
        move    x:KIN,r0
        move    x:KIN,a
        add     #32,a
        move    a1,r4                   ; the right bin
        move    #PRE_OUT,r1
        move    x:INGAIN,y0
        move    x:INLP,y1
        move    x:INST,b
        do      #32,pre_end
        move    x:(r0)+,a
        move    x:(r4)+,x0
        add     x0,a                    ; L + R
        move    a,x0                    ; (limited)
        mpy     x0,y0,a                 ; s = in * gain
        move    a,x1
        tfr     b,a
        sub     x1,a                    ; lp - s
        move    a,x0
        tfr     x1,b
        mac     x0,y1,b                 ; lp = s + p (lp - s)
        move    b,x:(r1)+
pre_end
        move    b,x:INST

; the comb sums start from zero
        clr     a
        move    #ACCL,r3
        rep     #64
        move    a,x:(r3)+

        move    #CTAB,r5
        do      #16,combs_end
        jsr     comb
        nop
combs_end

        move    #ATAB,r5
        move    x:APG,y0
        do      #8,aps_end
        jsr     allpass
        nop
aps_end

; out += wet * reverb, both channels (ACC L/R and OUT L/R are contiguous)
        move    #OUTL,r0
        move    #ACCL,r1
        move    x:WET,y0
        do      #64,out_end
        move    x:(r1)+,x0
        move    x:(r0),a
        mac     x0,y0,a
        move    a,x:(r0)+
out_end
        rts

; One comb; r5 points at its table entry and is left at the next one.
comb    move    x:(r5)+,r0              ; CIN
        move    x:(r5)+,r1              ; COUT
        move    x:(r5)+,r3              ; ACC
        move    x:(r5)+,y0              ; g
        move    x:(r5)+,y1              ; damp
        move    x:(r5),b                ; lp
        move    #PRE_IN,r2
        do      #32,comb_end
        move    x:(r0)+,x0              ; y = line output
        move    x:(r3),a
        add     x0,a
        move    a,x:(r3)+               ; ACC += y
        tfr     b,a
        sub     x0,a                    ; lp - y
        move    a,x1
        tfr     x0,b
        mac     x1,y1,b                 ; lp = y + damp (lp - y)
        move    x:(r2)+,a               ; pre-delayed input
        move    b,x1
        mac     x1,y0,a                 ; input + g lp
        move    a,x:(r1)+               ; COUT
comb_end
        move    b,x:(r5)+
        rts

; One allpass, in place on its ACC block; y0 = apg.
allpass move    x:(r5)+,r0              ; AIN
        move    x:(r5)+,r1              ; AOUT
        move    x:(r5)+,r3              ; ACC
        do      #32,ap_end
        move    x:(r0)+,x0              ; v = w[n - L]
        move    x:(r3),a
        mac     x0,y0,a                 ; w = s + g v
        move    a,x1
        move    a,x:(r1)+
        tfr     x0,a
        mac     -x1,y0,a                ; s = v - g w
        move    a,x:(r3)+
ap_end
        rts
