package com.github.ajalt.mordant.animation.diff

import com.github.ajalt.mordant.animation.Animation
import com.github.ajalt.mordant.rendering.Widget
import com.github.ajalt.mordant.terminal.Terminal

/** Test factories that select between incremental diffing and forced full redraw. */
object TestAnimations {
    /** An animation with frame diffing enabled, matching the default production behavior. */
    fun diffing(terminal: Terminal, draw: (Int) -> Widget): Animation<Int> =
        object : Animation<Int>(terminal) {
            override fun renderData(data: Int): Widget = draw(data)
        }

    /** An animation with frame diffing disabled, so every frame is a full redraw. */
    fun fullRedraw(terminal: Terminal, draw: (Int) -> Widget): Animation<Int> =
        object : Animation<Int>(terminal, frameDiffingEnabled = false) {
            override fun renderData(data: Int): Widget = draw(data)
        }
}
