; GP program with a reverb (lib/openal/docs/GP_REVERB_PLAN.md). Our own code.
;
; With bit 0 of X:FLAGS clear it is the copy program: the FIFO descriptor at
; X:0 sends mixbins 0/1 to GP output FIFO 0. With it set, every frame:
;   chain R (X:RCHAIN) reads one 32-word block of each of the 25 delay lines
;   (system RAM, circular scratch buffers) into its IN block, then
;     pre:     PRE_OUT = lowpass(bin 2 * INGAIN)          -> pre-delay line
;     combs:   y = CIN; ACC += y; lp = y + damp (lp - y); COUT = PRE_IN + g lp
;              (8 per ear, table CTAB: CIN, COUT, ACC, g, damp, lp)
;     allpass: v = AIN; w = s + apg v; AOUT = w; s = v - apg w
;              (4 per ear in series on ACC L / ACC R, table ATAB: AIN, AOUT, ACC)
;     out:     OUT L/R = bins 0/1 + WET * ACC L/R, then two one-pole lowpasses
;              per channel (OUTLP; 0 passes the mix unchanged: underwater)
;   chain W (X:WCHAIN) writes each line's OUT block and ends with the FIFO
;   descriptor at X:8, which sends OUT L/R.
; The CPU writes the descriptors, tables and coefficients (apu_reverb.c).
; Each line is at least 32 samples long, so a frame never reads what it writes.

CNT     equ     $10             ; frame counter (apu_gp_frames)
FLAGS   equ     $11             ; bit 0: reverb on
SPINS   equ     $13             ; polls of the last end-of-list wait
SPMAX   equ     $14             ; largest SPINS
TMOUT   equ     $15             ; waits that hit the limit
INGAIN  equ     $20             ; Q23 coefficients and states
INLP    equ     $21
INST    equ     $22
WET     equ     $23
APG     equ     $24
OUTLP   equ     $25             ; output lowpass coefficient
OUTST   equ     $2C             ; its 4 states: L stage 1, L stage 2, R stage 1, R stage 2
CTAB    equ     $30             ; 16 x 6 words
ATAB    equ     $90             ; 8 x 3 words
FIFO2   equ     $08             ; FIFO descriptor that sends OUT L/R
RCHAIN  equ     $130            ; line k: read descriptor at $130 + 16k, write at $138 + 16k
WCHAIN  equ     $138
OUTL    equ     $A8             ; OUT R at $C8 (FIFO stride 32)
OUTR    equ     $C8
ACCL    equ     $E8             ; ACC R at $108
PRE_IN  equ     $300            ; line k: IN block $300 + 64k, OUT block $320 + 64k
PRE_OUT equ     $320
BIN0    equ     $1400           ; bin 1 follows at $1420
BIN2    equ     $1440
LIMIT   equ     4000            ; polls before a wait gives up

        org     p:0
frame   movep   #1,x:$FFFFC4            ; frame complete
wait    jclr    #1,x:$FFFFC5,wait       ; wait for START_FRAME
        movep   #2,x:$FFFFC5            ; acknowledge it
        movep   #$80,x:$FFFFC5          ; clear end-of-list
        jset    #0,x:FLAGS,reverb
        movep   #0,x:$FFFFD4            ; copy: FIFO descriptor at X:0
        movep   #1,x:$FFFFD6
        jmp     count

reverb  movep   #RCHAIN,x:$FFFFD4       ; read every line
        movep   #1,x:$FFFFD6
        jsr     waiteol

; input: scale, lowpass, into the pre-delay line
        move    #BIN2,r0
        move    #PRE_OUT,r1
        move    x:INGAIN,y0
        move    x:INLP,y1
        move    x:INST,b
        do      #32,pre_end
        move    x:(r0)+,x0
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

; out = dry + wet * reverb, both channels (bins 0/1, ACC L/R and OUT L/R are contiguous)
        move    #BIN0,r0
        move    #ACCL,r1
        move    #OUTL,r2
        move    x:WET,y0
        do      #64,out_end
        move    x:(r1)+,x0
        move    x:(r0)+,a
        mac     x0,y0,a
        move    a,x:(r2)+
out_end

; output lowpass: OUT L twice, then OUT R twice (r2 walks the blocks, r5 the states)
        move    x:OUTLP,y1
        move    #OUTST,r5
        move    #OUTL,r2
        jsr     lowpass
        move    #OUTL,r2
        jsr     lowpass
        move    #OUTR,r2
        jsr     lowpass
        move    #OUTR,r2
        jsr     lowpass

        movep   #$80,x:$FFFFC5
        movep   #WCHAIN,x:$FFFFD4       ; write every line, then send OUT L/R
        movep   #1,x:$FFFFD6
        jsr     waiteol

count   move    x:CNT,b
        add     #1,b
        move    b1,x:CNT
        jmp     frame

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

; One-pole lowpass in place on 32 words at r2, state at x:(r5), r5 advanced; y1 = coefficient
lowpass move    x:(r5),b
        do      #32,lp_end
        move    x:(r2),x1               ; s
        tfr     b,a
        sub     x1,a                    ; lp - s
        move    a,x0
        tfr     x1,b
        mac     x0,y1,b                 ; lp = s + p (lp - s)
        move    b,x:(r2)+
lp_end
        move    b,x:(r5)+
        rts

; Wait for the chain's end-of-list flag (uses b, x0).
waiteol clr     b
        move    #LIMIT,x0
spin    jset    #7,x:$FFFFC5,done
        add     #1,b
        cmp     x0,b
        jlt     spin
        move    x:TMOUT,b
        add     #1,b
        move    b1,x:TMOUT
        rts
done    move    b1,x:SPINS
        move    x:SPMAX,x0
        cmp     x0,b
        jle     wdone
        move    b1,x:SPMAX
wdone   rts
