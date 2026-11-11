package com.github.ajalt.mordant.animation

import com.github.ajalt.mordant.internal.downsample
import com.github.ajalt.mordant.internal.graphemeClusters
import com.github.ajalt.mordant.internal.stringCellWidth
import com.github.ajalt.mordant.rendering.AnsiLevel
import com.github.ajalt.mordant.rendering.Line
import com.github.ajalt.mordant.rendering.Lines
import com.github.ajalt.mordant.rendering.Span
import com.github.ajalt.mordant.rendering.TextStyle

/**
 * The identity of the environment a frame was rendered in.
 *
 * A [FrameSnapshot] is only reusable while its identity matches the current terminal. Any change
 * to one of these fields can change the bytes that end up on screen even when the widget itself is
 * unchanged, so a mismatch invalidates the snapshot and forces a full redraw.
 *
 * @param generation Bumped whenever the screen may have changed outside this animation's control
 *   (interleaved output, stop/clear, an exception during rendering). Two frames can only be diffed
 *   when they share a generation.
 * @param width The terminal column count used to measure and render the frame (the "column width
 *   policy" in effect for the frame).
 * @param ansiLevel The color level the frame's styles were downsampled to.
 * @param hyperlinks Whether OSC 8 hyperlink sequences are emitted.
 * @param supportsAnsiCursor Whether cursor movement, erase and scroll sequences work.
 * @param interactive Whether the output stream is interactive; non-interactive output never diffs.
 * @param crImpliesLf Whether a carriage return also moves the cursor down a row. On platforms with
 *   that behavior, relative row positioning cannot be computed safely, so the snapshot never diffs.
 */
internal data class FrameIdentity(
    val generation: Int,
    val width: Int,
    val ansiLevel: AnsiLevel,
    val hyperlinks: Boolean,
    val supportsAnsiCursor: Boolean,
    val interactive: Boolean,
    val crImpliesLf: Boolean,
)

/**
 * A single screen cell: one grapheme cluster and the downsampled [TextStyle] it is drawn with.
 *
 * Cells are the atoms compared by the differ. Because [text] is always a complete grapheme
 * cluster, the differ can never split a combining mark, emoji ZWJ sequence or flag sequence, and
 * because [style] is compared too, a color-only change is detected even when the visible text is
 * identical.
 */
internal data class Cell(val text: String, val style: TextStyle) {
    /** Width in terminal columns; cached since [stringCellWidth] is not trivial. */
    val width: Int = stringCellWidth(text)
}

internal data class SnapshotLine(val cells: List<Cell>) {
    val width: Int = cells.sumOf { it.width }
}

/**
 * An immutable picture of exactly what is visible on screen after a frame is written: the rendered
 * lines broken into [Cell]s, together with the [FrameIdentity] they were rendered under.
 *
 * The snapshot describes visible screen state rather than widget state: it is built from the
 * rendered [Lines] after measure/render, style downsampling and column-width wrapping have all run.
 */
internal class FrameSnapshot(
    val identity: FrameIdentity,
    val lines: List<SnapshotLine>,
) {
    val height: Int get() = lines.size
}

/**
 * Flatten rendered [Lines] into a [FrameSnapshot].
 *
 * Each [Span]'s text is split into grapheme clusters so that the resulting cells can never cut
 * across a cluster boundary, and its style is downsampled with the same logic used to render the
 * ANSI output, so cell equality matches on-screen equality.
 */
internal fun Lines.toFrameSnapshot(identity: FrameIdentity): FrameSnapshot {
    val snapshotLines = ArrayList<SnapshotLine>(lines.size)
    for (line: Line in lines) {
        val cells = ArrayList<Cell>()
        for (span: Span in line) {
            val style = downsample(span.style, identity.ansiLevel, identity.hyperlinks)
            for (cluster in graphemeClusters(span.text)) {
                cells += Cell(cluster, style)
            }
        }
        snapshotLines += SnapshotLine(cells)
    }
    return FrameSnapshot(identity, snapshotLines)
}
