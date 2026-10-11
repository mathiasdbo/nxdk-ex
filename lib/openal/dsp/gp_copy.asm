; GP copy program (apu_gp.h): forwards mixbins 0/1 to GP output FIFO 0.
; Assembled form of the hand-encoded apu_gp_program[]; tests check they match.

        org     p:0
frame   movep   #1,x:$FFFFC4            ; frame complete
wait    jclr    #1,x:$FFFFC5,wait       ; wait for START_FRAME
        movep   #2,x:$FFFFC5            ; acknowledge it
        movep   #0,x:$FFFFD4            ; DMA NEXT_BLOCK = descriptor at X:0
        movep   #1,x:$FFFFD6            ; DMA CONTROL = start
        inc     a
        move    a0,x:$10                ; frame counter
        jmp     frame
