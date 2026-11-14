; da65 V2.19 - Git b166995d0
; Created:    2026-11-14 12:21:05
; Input file: .task-build/wf_l_idx.bin
; Page:       1


        .setcpu "65816"

        .a8
        .i8
        rep     #$10
        .i16
        ldx     #$1234
        sep     #$10
        .i8
        ldx     #$34
        rts

