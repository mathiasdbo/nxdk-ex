; GP frame loop (lib/openal/docs/EFX_PLAN.md 3.3-3.4, GP_KERNELS.md). Our own code.
;
; The library's half of every GP program: it owns the frame, the output and the
; mailbox, and calls up to four effect kernels, one per effect slot (relocatable
; images, ABI 2), or one legacy kernel at P:$100 (ABI 1). Every frame:
;   idle     polls of START_FRAME while waiting for it into X:IDLE, the
;            smallest since the CPU last reset it into X:IDLMIN (the CPU's
;            measure of the GP's load: no cycle counter is known on the GP)
;   mailbox  if X:MB_SEQ differs from X:MB_ACK, apply the X:MB_N (address,
;            value) pairs at X:MBOX, then MB_ACK = MB_SEQ: a change of several
;            words lands in one frame
;   no kernel (X:FLAGS 0): the copy program, the FIFO descriptor at X:0 sends
;            mixbins 0/1 to GP output FIFO 0
;   slots (FLAGS bit k: slot k on; X:SLOTX + k: its X block, KX):
;     read   each on slot's read chain (X:KX+3, 0: none), waiting for each
;     dry    OUT L/R = bins 0/1
;     kernel jsr TRAMPk for each on slot, in slot order: the trampoline is a
;            jmp to the kernel's entry (the CPU rewrites it while the slot is
;            off); each kernel reads its input bins and adds to OUT L/R
;     lowpass two one-pole stages per channel on OUT L/R (X:OUTLP), while
;            FLAGS bit 5 is set (the reverb's slot runs)
;     burn   X:BURN cycles of nop (0: none), to measure the headroom
;     write  each on slot's write chain (X:KX+4, 0: none; it ends at end of
;            list), waiting for each, then the mix descriptor at X:FIFO2
;   legacy (FLAGS bit 4, alone): read chain X:K_RHEAD, dry, jsr KERNEL,
;            lowpass, burn, write chain X:K_WHEAD ending at FIFO2 (ABI 1)
;
; Kernel ABI 2 (relocatable): its P, X block and Y range wherever the library
; placed them, scratch from KSCR, at most 60 DMA descriptors per frame across
; all kernels; it may use every register and must return with rts. Before its
; first frame the library zeroes its X block and scratch and writes KIN (X
; address of the slot's input pair, the right bin 32 words on), KSCR (byte
; offset of its scratch), KSCRN (its size), KPARAM0..31 (later changes arrive
; through the mailbox, all of one change in one frame), K_RHEAD = K_WHEAD = 0.
; Y is not initialised. ABI 1 is the same at fixed addresses (KX = $E0, all of
; P:$100..$7FF, X:$E0..$BFF and Y) with the chain heads at X:$19 / X:$1A.

CNT     equ     $10             ; frame counter (apu_gp_frames)
FLAGS   equ     $11             ; bits 0-3: slot k on (ABI 2); 4: the legacy kernel on (ABI 1); 5: output lowpass on
BURN    equ     $12             ; cycles of nop per frame (0: none)
SPINS   equ     $13             ; polls of the last end-of-list wait
SPMAX   equ     $14             ; largest SPINS
TMOUT   equ     $15             ; waits that hit the limit
MB_SEQ  equ     $16             ; mailbox: written last by the CPU
MB_ACK  equ     $17             ; mailbox: MB_SEQ once applied
MB_N    equ     $18             ; mailbox: number of pairs
K_RHEAD equ     $19             ; legacy kernel's read chain (0: none)
K_WHEAD equ     $1A             ; legacy kernel's write chain (ends at FIFO2)
OUTLP   equ     $1B             ; output lowpass coefficient
OUTST   equ     $1C             ; its 4 states: L stage 1, L stage 2, R stage 1, R stage 2
IDLE    equ     $07             ; START_FRAME polls before this frame (0: the frame was late)
FIFO2   equ     $08             ; mix descriptor: OUT L/R to FIFO 0
IDLMIN  equ     $0F             ; smallest IDLE since the CPU wrote $7FFFFF
OUTL    equ     $20             ; OUT R at $40 (FIFO stride 32)
OUTR    equ     $40
MBOX    equ     $60             ; 56 pairs, to $CF
MB_MAX  equ     56
SLOTX   equ     $D0             ; slot table: X block of slot k at SLOTX + k
SLOTS   equ     4
CHAIN   equ     $D4             ; the chain being started
KX_BASE equ     $E0             ; the kernels' X memory starts here
K_RH    equ     3               ; ABI 2: read chain head, at KX + 3
K_WH    equ     4               ; ABI 2: write chain head, at KX + 4
KIN     equ     $E0             ; ABI 1: input pair address
KSCR    equ     $E1             ; ABI 1: scratch byte offset
KSCRN   equ     $E2             ; ABI 1: scratch bytes
KPARAM  equ     $E8             ; ABI 1: 32 parameters, to $107
KOWN    equ     $108            ; ABI 1: the kernel's own X from here
KERNEL  equ     $100            ; ABI 1: entry point and P origin; ABI 2 kernels from here on
BIN0    equ     $1400           ; bin 1 follows at $1420
LIMIT   equ     4000            ; polls before a wait gives up

        org     p:0
frame   movep   #1,x:$FFFFC4            ; frame complete
        clr     b
wait    jset    #1,x:$FFFFC5,start      ; wait for START_FRAME, counting polls
        add     #1,b
        jmp     wait
start   movep   #2,x:$FFFFC5            ; acknowledge it
        movep   #$80,x:$FFFFC5          ; clear end-of-list
        move    b1,x:IDLE
        move    x:IDLMIN,x0
        cmp     x0,b
        jge     idle_ok
        move    b1,x:IDLMIN
idle_ok

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

nomail  move    x:FLAGS,a
        tst     a
        jne     kern
        movep   #0,x:$FFFFD4            ; copy: FIFO descriptor at X:0
        movep   #1,x:$FFFFD6
        jmp     count

kern    jset    #4,x:FLAGS,legacy

; the slots' read chains
        jclr    #0,x:FLAGS,rd1
        move    x:SLOTX,a
        jsr     rchain
rd1     jclr    #1,x:FLAGS,rd2
        move    x:SLOTX+1,a
        jsr     rchain
rd2     jclr    #2,x:FLAGS,rd3
        move    x:SLOTX+2,a
        jsr     rchain
rd3     jclr    #3,x:FLAGS,rd4
        move    x:SLOTX+3,a
        jsr     rchain
rd4     jsr     dry

; the kernels, in slot order
        jclr    #0,x:FLAGS,kc1
        jsr     TRAMP0
kc1     jclr    #1,x:FLAGS,kc2
        jsr     TRAMP1
kc2     jclr    #2,x:FLAGS,kc3
        jsr     TRAMP2
kc3     jclr    #3,x:FLAGS,kc4
        jsr     TRAMP3
kc4     jsr     post

; the slots' write chains, then OUT L/R
        jclr    #0,x:FLAGS,wr1
        move    x:SLOTX,a
        jsr     wchain
wr1     jclr    #1,x:FLAGS,wr2
        move    x:SLOTX+1,a
        jsr     wchain
wr2     jclr    #2,x:FLAGS,wr3
        move    x:SLOTX+2,a
        jsr     wchain
wr3     jclr    #3,x:FLAGS,wr4
        move    x:SLOTX+3,a
        jsr     wchain
wr4     movep   #FIFO2,x:$FFFFD4
        movep   #1,x:$FFFFD6
        jsr     waiteol
        jmp     count

; ABI 1: one kernel at P:$100, its chains at K_RHEAD / K_WHEAD
legacy  move    #K_RHEAD,r0
        jsr     runch
        jsr     dry
        jsr     KERNEL
        jsr     post
        movep   x:K_WHEAD,x:$FFFFD4     ; the kernel's writes, then OUT L/R
        movep   #1,x:$FFFFD6
        jsr     waiteol

count   move    x:CNT,b
        add     #1,b
        move    b1,x:CNT
        jmp     frame

; Trampolines: P:TRAMPk is "jmp entry" of slot k's kernel (the CPU writes the
; word, a short jmp, while the slot is off); knone until then.
TRAMP0  jmp     knone
TRAMP1  jmp     knone
TRAMP2  jmp     knone
TRAMP3  jmp     knone
knone   rts

; a = a slot's X block: its read (rchain) or write (wchain) chain, if any
rchain  add     #K_RH,a
        move    a1,r0
        jmp     runch
wchain  add     #K_WH,a
        move    a1,r0

; Run the chain whose head is at X:(r0), if not 0, and wait for it (uses a, b, x0)
runch   move    x:(r0),a
        tst     a
        jeq     ch_none
        move    a1,x:CHAIN
        movep   x:CHAIN,x:$FFFFD4
        movep   #1,x:$FFFFD6
        jsr     waiteol
        movep   #$80,x:$FFFFC5
ch_none rts

; dry: OUT L/R = bins 0/1 (contiguous, like OUT L/R)
dry     move    #BIN0,r0
        move    #OUTL,r1
        do      #64,dry_end
        move    x:(r0)+,x0
        move    x0,x:(r1)+
dry_end
        rts

; After the kernels: the output lowpass (OUT L twice, then OUT R twice; r2
; walks the blocks, r5 the states) while FLAGS bit 5 is set, then the burn
post    jclr    #5,x:FLAGS,nolp
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
nolp    move    x:BURN,a
        tst     a
        jeq     noburn
        rep     x:BURN
        nop
noburn  rts

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
