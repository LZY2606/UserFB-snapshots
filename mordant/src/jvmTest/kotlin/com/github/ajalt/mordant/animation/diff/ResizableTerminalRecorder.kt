package com.github.ajalt.mordant.animation.diff

import com.github.ajalt.mordant.input.InputEvent
import com.github.ajalt.mordant.input.MouseTracking
import com.github.ajalt.mordant.rendering.AnsiLevel
import com.github.ajalt.mordant.rendering.Size
import com.github.ajalt.mordant.terminal.PrintRequest
import com.github.ajalt.mordant.terminal.TerminalInfo
import com.github.ajalt.mordant.terminal.TerminalInterface
import kotlin.time.TimeMark

/**
 * A [TerminalInterface] for tests that records output like
 * [com.github.ajalt.mordant.terminal.TerminalRecorder], but whose size can change at runtime to
 * simulate a terminal resize.
 */
class ResizableTerminalRecorder(
    var width: Int = 40,
    var height: Int = 24,
    private val ansiLevel: AnsiLevel = AnsiLevel.TRUECOLOR,
    private val outputInteractive: Boolean = true,
    private val supportsAnsiCursor: Boolean = false,
) : TerminalInterface {
    private val output = StringBuilder()

    fun output(): String = output.toString()
    fun clearOutput() = output.setLength(0)

    fun resize(width: Int, height: Int = this.height) {
        this.width = width
        this.height = height
    }

    override fun completePrintRequest(request: PrintRequest) {
        output.append(request.text)
        if (request.trailingLinebreak) output.append("\n")
    }

    override fun readLineOrNull(hideInput: Boolean): String? = null

    override fun getTerminalSize(): Size = Size(width, height)

    override fun info(
        ansiLevel: AnsiLevel?,
        hyperlinks: Boolean?,
        outputInteractive: Boolean?,
        inputInteractive: Boolean?,
    ): TerminalInfo = TerminalInfo(
        ansiLevel = ansiLevel ?: this.ansiLevel,
        ansiHyperLinks = hyperlinks ?: (this.ansiLevel != AnsiLevel.NONE),
        outputInteractive = outputInteractive ?: this.outputInteractive,
        inputInteractive = inputInteractive ?: this.outputInteractive,
        supportsAnsiCursor = supportsAnsiCursor,
    )

    override fun readInputEvent(timeout: TimeMark, mouseTracking: MouseTracking): InputEvent? = null

    override fun enterRawMode(mouseTracking: MouseTracking): AutoCloseable =
        AutoCloseable { }
}
