; GP frame loop (lib/openal/docs/EFX_PLAN.md 3.3-3.4). Our own code.
;
; The library's half of every GP program: it owns the frame, the output and the
; mailbox, and calls one effect kernel (P:KERNEL, e.g. dsp/gp_kfreeverb.asm).
; Every frame:
;   mailbox  if X:MB_SEQ differs from X:MB_ACK, apply the X:MB_N (address,
;            value) pairs at X:MBOX, then MB_ACK = MB_SEQ: a change of several
;            words lands in one frame
;   kernel off (bit 0 of X:FLAGS clear): the copy program, the FIFO descriptor
;            at X:0 sends mixbins 0/1 to GP output FIFO 0
;   kernel on:
;     read   the kernel's read chain at X:K_RHEAD (0: none), wait for it
;     dry    OUT L/R = bins 0/1
;     kernel jsr KERNEL: it reads its input bins and adds its output to OUT L/R
;     lowpass two one-pole stages per channel on OUT L/R (X:OUTLP; 0 passes)
;     burn   X:BURN cycles of nop (0: none), to measure the headroom
;     write  the chain at X:K_WHEAD (the kernel's writes, ending with the mix
;            descriptor at X:FIFO2; K_WHEAD = FIFO2 with no writes)
;
; Kernel ABI: P:KERNEL..$7FF, X:KX_BASE..$BFF, all of Y, scratch from byte
; $2000 (below it the bootstrap image), at most 60 DMA descriptors per frame
; across its two chains; it may use every register and must return with rts.

CNT     equ     $10             ; frame counter (apu_gp_frames)
FLAGS   equ     $11             ; bit 0: kernel on
BURN    equ     $12             ; cycles of nop per frame (0: none)
SPINS   equ     $13             ; polls of the last end-of-list wait
SPMAX   equ     $14             ; largest SPINS
TMOUT   equ     $15             ; waits that hit the limit
MB_SEQ  equ     $16             ; mailbox: written last by the CPU
MB_ACK  equ     $17             ; mailbox: MB_SEQ once applied
MB_N    equ     $18             ; mailbox: number of pairs
K_RHEAD equ     $19             ; kernel read chain (0: none)
K_WHEAD equ     $1A             ; kernel write chain (ends at FIFO2)
OUTLP   equ     $1B             ; output lowpass coefficient
OUTST   equ     $1C             ; its 4 states: L stage 1, L stage 2, R stage 1, R stage 2
FIFO2   equ     $08             ; mix descriptor: OUT L/R to FIFO 0
OUTL    equ     $20             ; OUT R at $40 (FIFO stride 32)
OUTR    equ     $40
MBOX    equ     $60             ; 64 pairs, to $DF
MB_MAX  equ     64
KX_BASE equ     $E0             ; the kernel's X memory starts here
KERNEL  equ     $100            ; the kernel's entry point and P origin
BIN0    equ     $1400           ; bin 1 follows at $1420
LIMIT   equ     4000            ; polls before a wait gives up

        org     p:0
frame   movep   #1,x:$FFFFC4            ; frame complete
wait    jclr    #1,x:$FFFFC5,wait       ; wait for START_FRAME
        movep   #2,x:$FFFFC5            ; acknowledge it
        movep   #$80,x:$FFFFC5          ; clear end-of-list

; mailbox
        move    x:MB_SEQ,a
        move    x:MB_ACK,x0
        cmp     x0,a
        jeq     nomail
        move    x:MB_N,b
        tst     b
        jeq     mbdone
        move    #MBOX,r0
        do      x:MB_N,mbloop
        move    x:(r0)+,r1              ; address
        move    x:(r0)+,x0              ; value
        move    x0,x:(r1)
mbloop
mbdone  move    a1,x:MB_ACK

nomail  jset    #0,x:FLAGS,kern
        movep   #0,x:$FFFFD4            ; copy: FIFO descriptor at X:0
        movep   #1,x:$FFFFD6
        jmp     count

kern    move    x:K_RHEAD,a
        tst     a
        jeq     noread
        movep   x:K_RHEAD,x:$FFFFD4     ; the kernel's reads
        movep   #1,x:$FFFFD6
        jsr     waiteol
        movep   #$80,x:$FFFFC5

; dry: OUT L/R = bins 0/1 (contiguous, like OUT L/R)
noread  move    #BIN0,r0
        move    #OUTL,r1
        do      #64,dry_end
        move    x:(r0)+,x0
        move    x0,x:(r1)+
dry_end

        jsr     KERNEL

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

        move    x:BURN,a
        tst     a
        jeq     noburn
        rep     x:BURN
        nop
noburn
        movep   x:K_WHEAD,x:$FFFFD4     ; the kernel's writes, then OUT L/R
        movep   #1,x:$FFFFD6
        jsr     waiteol

count   move    x:CNT,b
        add     #1,b
        move    b1,x:CNT
        jmp     frame

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
