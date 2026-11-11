package com.github.ajalt.mordant.animation.diff

import com.github.ajalt.mordant.internal.ANSI_RE
import com.github.ajalt.mordant.internal.CSI
import com.github.ajalt.mordant.internal.DEFAULT_STYLE
import com.github.ajalt.mordant.internal.OSC
import com.github.ajalt.mordant.internal.cellWidth
import com.github.ajalt.mordant.internal.codepointSequence
import com.github.ajalt.mordant.internal.codepointToString
import com.github.ajalt.mordant.internal.updateStyle
import com.github.ajalt.mordant.rendering.TextStyle

/**
 * A minimal ANSI terminal emulator that keeps an in-memory grid of (text, style) cells.
 *
 * It understands the subset of escape sequences that Mordant animations emit: carriage return and
 * line feed, relative cursor movement (A/B/C/D), insert/delete line (L/M), erase line and screen
 * (K/J), SGR styling and OSC 8 hyperlinks. Everything else is ignored.
 *
 * The grid grows downward as needed; wide characters occupy two columns and zero-width combining
 * marks are appended to the preceding cell, so two byte streams that render identically compare
 * equal even when their escape sequences differ. This is what lets a frame-diff byte stream be
 * checked against a full-redraw byte stream.
 */
class VirtualScreen(private val width: Int) {
    data class Cell(val text: String, val style: TextStyle)

    private val rows = ArrayList<MutableList<Cell>>()
    private var cursorX = 0
    private var cursorY = 0
    private var activeStyle: TextStyle = DEFAULT_STYLE

    init {
        rows += newRow()
    }

    /** Current cursor column (0-indexed) and row (0-indexed) and the active SGR style. */
    val cursorColumn: Int get() = cursorX
    val cursorRow: Int get() = cursorY
    val currentStyle: TextStyle get() = activeStyle

    /** Replay [stream], applying every ANSI sequence and printable character onto the grid. */
    fun replay(stream: String) {
        var index = 0
        for (match in ANSI_RE.findAll(stream)) {
            if (match.range.first > index) {
                writeText(stream.substring(index, match.range.first))
            }
            handleEscape(match.value)
            index = match.range.last + 1
        }
        if (index < stream.length) writeText(stream.substring(index))
    }

    /**
     * The visible screen as one string per row, with each cell's ANSI styling rendered inline.
     * Trailing blank rows and columns are trimmed so equivalent screens compare equal regardless
     * of how much blank space the terminal scrolled through.
     */
    fun renderedLines(): List<String> {
        val nonEmpty = rows.indexOfLast { row -> row.any { it.text.isNotEmpty() } }
        if (nonEmpty < 0) return emptyList()
        return rows.subList(0, nonEmpty + 1).map { row ->
            val lastContent = row.indexOfLast { it.text.isNotEmpty() }
            buildString {
                for (i in 0..lastContent) {
                    val cell = row[i]
                    if (cell.text.isEmpty()) continue // wide-character continuation cell
                    if (cell.style != DEFAULT_STYLE) append(renderStyle(cell.style))
                    append(cell.text)
                    if (cell.style != DEFAULT_STYLE) append("${CSI}0m")
                }
            }
        }
    }

    private fun renderStyle(style: TextStyle): String {
        // Reuse Mordant's own tag generation so styled cells compare by the same TextStyle rules.
        return com.github.ajalt.mordant.internal.makeTag(DEFAULT_STYLE, style)
    }

    private fun newRow(): MutableList<Cell> =
        ArrayList(List(width) { Cell("", DEFAULT_STYLE) })

    private fun ensureRow(y: Int) {
        while (rows.size <= y) rows += newRow()
    }

    private fun writeText(text: String) {
        for (cp in codepointSequence(text)) {
            when (cp) {
                '\r'.code -> cursorX = 0
                '\n'.code -> {
                    cursorY += 1
                    ensureRow(cursorY)
                }
                '\t'.code -> {
                    val next = ((cursorX / 8) + 1) * 8
                    putCell(" ", DEFAULT_STYLE, 1)
                    while (cursorX < next && cursorX < width) {
                        putCell(" ", DEFAULT_STYLE, 1)
                    }
                }
                else -> writeCodePoint(cp)
            }
        }
    }

    private fun writeCodePoint(cp: Int) {
        val w = cellWidth(cp)
        if (w <= 0) {
            // Combining mark or other zero-width character: attach to the previous cell.
            if (cursorX > 0) {
                val target = rows[cursorY][cursorX - 1]
                rows[cursorY][cursorX - 1] =
                    target.copy(text = target.text + codepointToString(cp))
            }
            return
        }
        putCell(codepointToString(cp), activeStyle, w)
    }

    private fun putCell(text: String, style: TextStyle, width: Int) {
        if (cursorX >= this.width) {
            // Physical line wrap, matching terminal behavior for over-wide output.
            cursorY += 1
            ensureRow(cursorY)
            cursorX = 0
        }
        ensureRow(cursorY)
        rows[cursorY][cursorX] = Cell(text, style)
        if (width == 2 && cursorX + 1 < this.width) {
            rows[cursorY][cursorX + 1] = Cell("", style)
        }
        cursorX += width
    }

    private fun handleEscape(seq: String) {
        when {
            seq.startsWith(CSI) && seq.endsWith("m") -> {
                activeStyle = updateStyle(activeStyle, DEFAULT_STYLE, seq)
            }
            seq.startsWith(OSC) -> {
                activeStyle = updateStyle(activeStyle, DEFAULT_STYLE, seq)
            }
            seq.startsWith(CSI) -> handleCsi(seq)
            else -> { /* DSC/APC and single ESC shifts don't affect the screen */ }
        }
    }

    private fun handleCsi(seq: String) {
        val command = seq.last()
        val params = seq.substring(2, seq.length - 1)
            .split(";")
            .mapNotNull { it.toIntOrNull() }
        val n = params.firstOrNull() ?: 1
        when (command) {
            'A' -> cursorY = (cursorY - n).coerceAtLeast(0)
            'B' -> {
                cursorY += n
                ensureRow(cursorY)
            }
            'C' -> cursorX = (cursorX + n).coerceAtMost(width - 1)
            'D' -> cursorX = (cursorX - n).coerceAtLeast(0)
            'H', 'f' -> {
                val y = (params.getOrElse(0) { 1 } - 1).coerceAtLeast(0)
                val x = (params.getOrElse(1) { 1 } - 1).coerceAtLeast(0)
                ensureRow(y)
                cursorY = y
                cursorX = x
            }
            'L' -> insertLines(n)
            'M' -> deleteLines(n)
            'J' -> eraseScreen(n)
            'K' -> eraseLine(n)
            's' -> Unit // save position: not needed for Mordant output
            'u' -> Unit
        }
    }

    private fun insertLines(count: Int) {
        repeat(count) {
            rows.add(cursorY, newRow())
        }
    }

    private fun deleteLines(count: Int) {
        repeat(count) {
            if (cursorY in rows.indices) rows.removeAt(cursorY)
            rows += newRow()
        }
    }

    private fun eraseLine(mode: Int) {
        val row = rows[cursorY]
        val range = when (mode) {
            1 -> 0..cursorX
            2 -> row.indices
            else -> cursorX..row.lastIndex
        }
        for (i in range) row[i] = Cell("", DEFAULT_STYLE)
    }

    private fun eraseScreen(mode: Int) {
        when (mode) {
            1 -> {
                for (y in 0..cursorY) {
                    val row = rows[y]
                    val end = if (y == cursorY) cursorX else row.lastIndex
                    for (i in 0..end) row[i] = Cell("", DEFAULT_STYLE)
                }
            }
            2 -> rows.indices.forEach { rows[it] = newRow() }
            else -> {
                eraseLine(0)
                for (y in cursorY + 1..rows.lastIndex) rows[y] = newRow()
            }
        }
    }

    companion object {
        /** Convenience: replay [stream] and return the trimmed rendered screen. */
        fun replay(width: Int, stream: String): List<String> {
            return VirtualScreen(width).apply { replay(stream) }.renderedLines()
        }

        /** The escape character as the two-character visible marker used in trace files. */
        const val ESC_MARKER = "␛"

        /** Escape a raw ANSI stream into the human-readable form used in trace files. */
        fun escapeForTrace(stream: String): String =
            stream.replace("\u001B", ESC_MARKER).replace("\r", "␍").replace("\n", "␊")

        /** Inverse of [escapeForTrace]. */
        fun unescapeTrace(stream: String): String =
            stream.replace(ESC_MARKER, "\u001B").replace("␍", "\r").replace("␊", "\n")
    }
}
