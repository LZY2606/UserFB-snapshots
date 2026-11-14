; da65 V2.19 - Git b166995d0
; Created:    2026-11-14 12:21:05
; Input file: .task-build/wf_l_emu.bin
; Page:       1


        .setcpu "65816"

        .a8
        .i8
        sec
        xce
        lda     #$42
        ldx     #$17
        rts

