; da65 V2.19 - Git b166995d0
; Created:    2026-11-14 12:21:05
; Input file: .task-build/wf_l_acc.bin
; Page:       1


        .setcpu "65816"

        .a8
        .i8
        rep     #$20
        .a16
        lda     #$1234
        sep     #$20
        .a8
        lda     #$56
        rts

