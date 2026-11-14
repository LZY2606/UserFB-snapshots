; da65 V2.19 - Git b166995d0
; Created:    2026-11-14 12:21:06
; Input file: .task-build/state.bin
; Page:       1


        .setcpu "65816"

        .a16
        .i8
        rep     #$20
        lda     #$1234
        sep     #$20
        lda     #$6056
