package com.github.ajalt.mordant.animation

import com.github.ajalt.mordant.internal.CSI
import com.github.ajalt.mordant.internal.DEFAULT_STYLE
import com.github.ajalt.mordant.internal.makeTag
import com.github.ajalt.mordant.internal.utf8ByteLength
import com.github.ajalt.mordant.rendering.TextStyle

/**
 * Compute the byte-level difference between two animation frames.
 *
 * The input is two [FrameSnapshot]s of the *visible* screen state (grapheme clusters plus their
 * downsampled styles), not widget objects. The output is a string of ANSI control sequences and
 * text that, written with the cursor at the top-left corner of the previous frame, transforms the
 * screen into the current frame, and leaves the cursor at the end of the last frame row — exactly
 * where a full redraw would leave it.
 *
 * Returns `null` when a safe incremental update cannot be produced. The caller must then fall back
 * to a full frame redraw.
 */
internal object FrameDiffer {

    /**
     * Build the diff from [previous] to [current].
     *
     * @param fullFrameText The bytes that a full redraw of [current] would write. It is used only
     *   for the byte-budget threshold: if the diff is not strictly smaller, a full redraw is used
     *   instead.
     * @return The diff text, an empty string if the frames are identical, or `null` to force a
     *   full redraw.
     */
    fun diff(previous: FrameSnapshot, current: FrameSnapshot, fullFrameText: String): String? {
        require(previous.identity == current.identity) {
            "Frame snapshots with different identities cannot be diffed"
        }
        val identity = current.identity

        val oldLines = previous.lines
        val newLines = current.lines

        // Identical frames need no bytes on any terminal, so this short-circuits before the
        // capability gates below.
        if (oldLines == newLines) return ""

        // A full ANSI cursor vocabulary (relative moves, erase line, insert/delete line) is only
        // assumed on ordinary interactive terminals. IntelliJ's console (supportsAnsiCursor=true)
        // handles just a carriage return, and on wasm a CR also moves down a row; both fall back to
        // a full redraw. Non-interactive streams never diff.
        if (!identity.interactive || identity.supportsAnsiCursor || identity.crImpliesLf) {
            return null
        }

        // If any line is wider than the terminal, it physically wraps, so our row and column
        // arithmetic no longer maps onto the screen.
        if (oldLines.any { it.width > identity.width } ||
            newLines.any { it.width > identity.width }
        ) return null

        val prefix = commonPrefixLength(oldLines, newLines)
        val suffix = commonSuffixLength(oldLines, newLines, prefix)
        val oldMiddleEnd = oldLines.size - suffix
        val newMiddleEnd = newLines.size - suffix

        val diff = when {
            oldLines.size == newLines.size ->
                sameHeightDiff(oldLines, newLines, prefix, newMiddleEnd)

            // Pure insertion: no old rows in the middle, whole new rows are added in one block.
            prefix == oldMiddleEnd ->
                insertDiff(newLines, prefix, newMiddleEnd)

            // Pure deletion: no new rows in the middle, whole old rows are removed in one block.
            prefix == newMiddleEnd ->
                deleteDiff(newLines, prefix, oldMiddleEnd - prefix)

            // A mixed restructure (both deleted and inserted rows, plus moved content). Repainting
            // the whole frame is simpler, and the byte budget would usually reject the diff anyway.
            else -> null
        } ?: return null

        // The cursor starts at the end of the previous frame: move it back to the frame's top-left
        // cell before applying the row diffs. crImpliesLf platforms never reach here.
        val positioned = buildString {
            append("\r")
            val up = oldLines.size - 1
            if (up > 0) append(csi("${up}A"))
            append(diff)
        }

        // Never write more UTF-8 bytes than a full redraw would; an empty diff means a no-op frame.
        return positioned.takeIf {
            it.isNotEmpty() && utf8ByteLength(it) < utf8ByteLength(fullFrameText)
        }
    }

    /**
     * Diff frames of equal height row by row. Only the changed rows are touched; the cursor is
     * moved between them with relative movement commands.
     *
     * The cursor is assumed to start at column 0 of the first frame row. Every visited row begins
     * with a carriage return so vertical moves always happen from column 0.
     */
    private fun sameHeightDiff(
        oldLines: List<SnapshotLine>,
        newLines: List<SnapshotLine>,
        firstChanged: Int,
        middleEnd: Int,
    ): String {
        val builder = StringBuilder()
        var row = 0
        for (r in firstChanged until middleEnd) {
            builder.append("\r")
            // Vertical moves preserve the column, so the preceding carriage return puts us at
            // column 0 of the target row. No line feeds are emitted, so wrapping can't shift us.
            if (r > row) builder.append(csi("${r - row}B"))
            row = r
            appendRowDiff(builder, oldLines[r], newLines[r])
        }
        appendMoveToFrameEnd(builder, row, newLines)
        return builder.toString()
    }

    /** Insert blank lines at row [row], scroll the suffix down, then write the new rows. */
    private fun insertDiff(
        newLines: List<SnapshotLine>,
        row: Int,
        middleEnd: Int,
    ): String {
        val count = middleEnd - row
        return buildString {
            append("\r")
            if (row > 0) append(csi("${row}B"))
            // CSI n L: insert n blank lines at the cursor; rows below scroll down.
            append(csi("${count}L"))
            var activeStyle: TextStyle = DEFAULT_STYLE
            for (r in row until middleEnd) {
                if (r > row) append("\r\n")
                activeStyle = appendRow(this, newLines[r], activeStyle)
            }
            appendMoveToFrameEnd(this, middleEnd - 1, newLines)
        }
    }

    /** Delete [count] lines starting at row [row]; the common suffix scrolls up on its own. */
    private fun deleteDiff(newLines: List<SnapshotLine>, row: Int, count: Int): String {
        return buildString {
            append("\r")
            if (row > 0) append(csi("${row}B"))
            // CSI n M: delete n lines at the cursor; rows below scroll up and blank lines appear
            // at the bottom of the screen.
            append(csi("${count}M"))
            appendMoveToFrameEnd(this, row, newLines)
        }
    }

    /**
     * Move the cursor from [currentRow] to the end of the last frame row, matching the position a
     * full redraw (which prints no trailing line break) would leave behind.
     */
    private fun appendMoveToFrameEnd(
        builder: StringBuilder,
        currentRow: Int,
        newLines: List<SnapshotLine>,
    ) {
        if (newLines.isEmpty()) return
        val lastRow = newLines.lastIndex
        // Carriage return first so the vertical move happens from column 0 and the final right
        // move lands on the exact end column regardless of where we started. The move can go up
        // (after deleting trailing rows) as well as down.
        builder.append("\r")
        when {
            lastRow > currentRow -> builder.append(csi("${lastRow - currentRow}B"))
            lastRow < currentRow -> builder.append(csi("${currentRow - lastRow}A"))
        }
        val endWidth = newLines[lastRow].width
        if (endWidth > 0) builder.append(csi("${endWidth}C"))
    }

    /**
     * Write the diff for a single row. The cursor is assumed to already be at column 0 of the
     * row.
     *
     * The unchanged cell prefix is skipped with a right move; cells between the common prefix and
     * suffix are rewritten; the common suffix is skipped with another right move when it did not
     * shift, or rewritten whole when the middle changed width (and therefore shifted it). A shorter
     * new row erases its leftover tail. Grapheme clusters are always rewritten whole because
     * [Cell]s are cluster-sized.
     */
    private fun appendRowDiff(
        builder: StringBuilder,
        oldLine: SnapshotLine,
        newLine: SnapshotLine,
    ) {
        val oldCells = oldLine.cells
        val newCells = newLine.cells
        val prefix = commonCellPrefix(oldCells, newCells)
        val suffix = commonCellSuffix(oldCells, newCells, prefix)

        val oldMiddleWidth = widthOf(oldCells, prefix, oldCells.size - suffix)
        val newMiddleWidth = widthOf(newCells, prefix, newCells.size - suffix)
        val suffixWidth = widthOf(newCells, newCells.size - suffix, newCells.size)
        val prefixWidth = widthOf(newCells, 0, prefix)

        if (prefixWidth > 0) builder.append(csi("${prefixWidth}C"))

        // Skipping the prefix with a cursor move leaves the terminal holding the prefix's last
        // cell style, so SGR transitions must start from there rather than from the default.
        var activeStyle: TextStyle =
            if (prefix > 0) newCells[prefix - 1].style else DEFAULT_STYLE
        for (i in prefix until newCells.size - suffix) {
            val cell = newCells[i]
            builder.append(makeTag(activeStyle, cell.style))
            activeStyle = cell.style
            builder.append(cell.text)
        }

        if (oldMiddleWidth == newMiddleWidth) {
            // The suffix did not shift; leave its bytes and styling on screen and jump over it.
            if (suffixWidth > 0) builder.append(csi("${suffixWidth}C"))
        } else {
            // The suffix shifted horizontally, so it has to be rewritten whole.
            for (i in newCells.size - suffix until newCells.size) {
                val cell = newCells[i]
                builder.append(makeTag(activeStyle, cell.style))
                activeStyle = cell.style
                builder.append(cell.text)
            }
        }
        builder.append(makeTag(activeStyle, DEFAULT_STYLE))

        if (newLine.width < oldLine.width) {
            // Erase any cells of the old row that extend past the new row's end.
            builder.append(csi("0K"))
        }
    }

    private fun appendRow(
        builder: StringBuilder,
        line: SnapshotLine,
        initialStyle: TextStyle,
    ): TextStyle {
        builder.append("\r")
        var activeStyle = initialStyle
        for (cell in line.cells) {
            builder.append(makeTag(activeStyle, cell.style))
            activeStyle = cell.style
            builder.append(cell.text)
        }
        builder.append(makeTag(activeStyle, DEFAULT_STYLE))
        return DEFAULT_STYLE
    }

    private fun widthOf(cells: List<Cell>, from: Int, until: Int): Int {
        var width = 0
        for (i in from until until) width += cells[i].width
        return width
    }

    private fun commonCellPrefix(a: List<Cell>, b: List<Cell>): Int {
        val max = minOf(a.size, b.size)
        var i = 0
        while (i < max && a[i] == b[i]) i++
        return i
    }

    private fun commonCellSuffix(a: List<Cell>, b: List<Cell>, prefix: Int): Int {
        var i = 0
        val max = minOf(a.size, b.size) - prefix
        while (i < max && a[a.size - 1 - i] == b[b.size - 1 - i]) i++
        return i
    }

    private fun commonPrefixLength(a: List<SnapshotLine>, b: List<SnapshotLine>): Int {
        val max = minOf(a.size, b.size)
        var i = 0
        while (i < max && a[i].cells == b[i].cells) i++
        return i
    }

    private fun commonSuffixLength(
        a: List<SnapshotLine>,
        b: List<SnapshotLine>,
        prefix: Int,
    ): Int {
        var i = 0
        val max = minOf(a.size, b.size) - prefix
        while (i < max && a[a.size - 1 - i].cells == b[b.size - 1 - i]) i++
        return i
    }

    private fun csi(command: String): String = CSI + command
}
