; GP probe program (samples/apu_gp_probe, reverb plan phase 0).
; The copy program's frame loop plus optional measurements, switched by the
; flag word at X:$11 that the CPU writes through the X window:
;   bit 0  after starting the DMA chain, wait for its end-of-list flag
;          ($FFFFC5 bit 7), at most X:$17 polls; log the poll counts
;   bit 1  burn X:$12 cycles per frame (rep of nop)
;   bit 2  read X:(X:$13) and Y:(X:$13) with the core into X:$14 / X:$1A
; Every frame it stamps the frame counter into X:$300 (the DMA test block).

FLAGS   equ     $11
BURN    equ     $12
RADDR   equ     $13
RX      equ     $14
SPINS   equ     $15             ; polls until end-of-list, last frame
SPMAX   equ     $16             ; largest SPINS seen
SPLIM   equ     $17             ; poll limit
TMOUT   equ     $18             ; frames that hit the limit
RAWC5   equ     $19             ; $FFFFC5 before the clear, last frame
RY      equ     $1A
DMACTL  equ     $1B             ; DMA CONTROL after the wait, last frame
STAMP   equ     $300

        org     p:0
frame   movep   #1,x:$FFFFC4            ; frame complete
wait    jclr    #1,x:$FFFFC5,wait       ; wait for START_FRAME
        movep   #2,x:$FFFFC5            ; acknowledge it
        move    a0,x:STAMP
        movep   x:$FFFFC5,x:RAWC5
        movep   #$80,x:$FFFFC5          ; clear end-of-list
        movep   #0,x:$FFFFD4            ; chain head X:0
        movep   #1,x:$FFFFD6            ; start
        jclr    #0,x:FLAGS,nowait
        clr     b
        move    x:SPLIM,x0
spin    jset    #7,x:$FFFFC5,done
        add     #1,b
        cmp     x0,b
        jlt     spin
        move    b1,x:SPINS              ; limit reached
        move    x:TMOUT,b
        add     #1,b
        move    b1,x:TMOUT
        jmp     after
done    move    b1,x:SPINS
        move    x:SPMAX,x0
        cmp     x0,b
        jle     after
        move    b1,x:SPMAX
after   movep   x:$FFFFD6,x:DMACTL
nowait  jclr    #1,x:FLAGS,noburn
        rep     x:BURN
        nop
noburn  jclr    #2,x:FLAGS,noread
        move    x:RADDR,r0
        nop
        move    x:(r0),b
        move    b1,x:RX
        move    y:(r0),b
        move    b1,x:RY
noread  inc     a
        move    a0,x:$10                ; frame counter
        jmp     frame
