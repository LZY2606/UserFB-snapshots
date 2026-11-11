package com.github.ajalt.mordant.animation.diff

import com.github.ajalt.mordant.animation.FrameDiffer
import com.github.ajalt.mordant.animation.FrameIdentity
import com.github.ajalt.mordant.animation.toFrameSnapshot
import com.github.ajalt.mordant.internal.CSI
import com.github.ajalt.mordant.internal.DEFAULT_STYLE
import com.github.ajalt.mordant.internal.downsample
import com.github.ajalt.mordant.internal.makeTag
import com.github.ajalt.mordant.rendering.AnsiLevel
import com.github.ajalt.mordant.rendering.Line
import com.github.ajalt.mordant.rendering.Lines
import com.github.ajalt.mordant.rendering.Span
import com.github.ajalt.mordant.rendering.TextColors
import io.kotest.assertions.throwables.shouldThrow
import io.kotest.matchers.shouldBe
import io.kotest.matchers.string.shouldContain
import io.kotest.matchers.string.shouldNotContain
import kotlin.test.Test

/**
 * Direct unit tests for [FrameDiffer].
 *
 * The byte-budget threshold is bypassed by handing in an artificially long `fullFrameText`, so
 * these tests pin down the exact control sequences the differ emits. Every diff is also replayed
 * into a [VirtualScreen] and checked against the target frame.
 */
class FrameDifferUnitTest {
    private val identity = FrameIdentity(
        generation = 0,
        width = 40,
        ansiLevel = AnsiLevel.TRUECOLOR,
        hyperlinks = true,
        supportsAnsiCursor = false,
        interactive = true,
        crImpliesLf = false,
    )

    /** A budget large enough that any non-empty diff is accepted. */
    private val unlimitedBudget = "x".repeat(10_000)

    private fun snapshot(vararg lines: String) =
        Lines(lines.map { Line(listOf(Span.word(it))) }).toFrameSnapshot(identity)

    private fun replay(diff: String, previous: String, width: Int = 40): List<String> {
        val screen = VirtualScreen(width)
        screen.replay(previous)
        screen.replay(diff)
        return screen.renderedLines()
    }

    @Test
    fun identicalFramesProduceEmptyDiff() {
        val s = snapshot("hello", "world")
        FrameDiffer.diff(s, snapshot("hello", "world"), unlimitedBudget) shouldBe ""
    }

    @Test
    fun changedCellRewritesOnlyThatRowAndSkipsCommonPrefix() {
        val previous = snapshot("row zero", "row one")
        val current = snapshot("row zero", "row two")
        val diff = FrameDiffer.diff(previous, current, unlimitedBudget)!!
        diff shouldContain "${CSI}4C" // skip the common "row " prefix
        diff shouldNotContain "row zero"
        replay(diff, "row zero\nrow one") shouldBe listOf("row zero", "row two")
    }

    @Test
    fun cjkWidePrefixIsSkippedByCellWidthNotCharacterCount() {
        // "日本語 " occupies 2+2+2+1 = 7 columns before the changed digit.
        val previous = snapshot("日本語 0 終")
        val current = snapshot("日本語 1 終")
        val diff = FrameDiffer.diff(previous, current, unlimitedBudget)!!
        diff shouldContain "${CSI}7C"
        // The wide CJK text is never re-emitted.
        diff shouldNotContain "日"
        replay(diff, "日本語 0 終") shouldBe listOf("日本語 1 終")
    }

    @Test
    fun combiningMarkIsKeptWholeInsideItsCluster() {
        // café with a combining acute (e + U+0301); only the trailing word changes.
        val previous = snapshot("café x")
        val current = snapshot("café y")
        val diff = FrameDiffer.diff(previous, current, unlimitedBudget)!!
        // The é cluster (e + combining mark) is part of the untouched prefix.
        diff shouldNotContain "é"
        replay(diff, "café x") shouldBe listOf("café y")
    }

    @Test
    fun emojiZwjSequenceIsOneAtomicCluster() {
        val previous = snapshot("👩‍💻 status 0")
        val current = snapshot("👩‍💻 status 1")
        val diff = FrameDiffer.diff(previous, current, unlimitedBudget)!!
        // The emoji is two columns and never partially rewritten.
        diff shouldNotContain "👩"
        diff shouldNotContain "‍"
        replay(diff, "👩‍💻 status 0") shouldBe listOf("👩‍💻 status 1")
    }

    @Test
    fun styleOnlyChangeStillProducesADiff() {
        val styled = { color: com.github.ajalt.colormath.Color ->
            Lines(listOf(Line(listOf(Span.word("same", TextColors.color(color))))))
                .toFrameSnapshot(identity)
        }
        val previous = styled(com.github.ajalt.colormath.model.RGB(1, 0, 0))
        val current = styled(com.github.ajalt.colormath.model.RGB(0, 1, 0))
        val diff = FrameDiffer.diff(previous, current, unlimitedBudget)!!
        diff shouldContain "m" // an SGR sequence is present
        val screen = VirtualScreen(40).apply { replay(diff) }
        screen.renderedLines() shouldBe listOf(
            com.github.ajalt.mordant.internal.makeTag(
                DEFAULT_STYLE,
                com.github.ajalt.mordant.internal.downsample(
                    TextColors.color(com.github.ajalt.colormath.model.RGB(0, 1, 0)),
                    AnsiLevel.TRUECOLOR,
                    true,
                )
            ) + "same" + "${CSI}0m"
        )
        screen.currentStyle shouldBe DEFAULT_STYLE
    }

    @Test
    fun pureRowInsertionUsesInsertLineCommand() {
        val previous = snapshot("row0", "row1", "row2")
        val current = snapshot("row0", "inserted", "row1", "row2")
        val diff = FrameDiffer.diff(previous, current, unlimitedBudget)!!
        diff shouldContain "${CSI}1L"
        replay(diff, "row0\nrow1\nrow2") shouldBe
            listOf("row0", "inserted", "row1", "row2")
    }

    @Test
    fun pureRowDeletionUsesDeleteLineCommand() {
        val previous = snapshot("row0", "inserted", "row1", "row2")
        val current = snapshot("row0", "row1", "row2")
        val diff = FrameDiffer.diff(previous, current, unlimitedBudget)!!
        diff shouldContain "${CSI}1M"
        replay(diff, "row0\ninserted\nrow1\nrow2") shouldBe
            listOf("row0", "row1", "row2")
    }

    @Test
    fun shorterRowErasesItsLeftoverTail() {
        val previous = snapshot("abcdefgh")
        val current = snapshot("abc")
        val diff = FrameDiffer.diff(previous, current, unlimitedBudget)!!
        diff shouldContain "${CSI}0K"
        replay(diff, "abcdefgh") shouldBe listOf("abc")
    }

    @Test
    fun diffLargerThanFullFrameIsRejected() {
        val previous = snapshot("ab")
        val current = snapshot("ac")
        // A two-cell full frame is shorter than the cursor positioning any diff needs.
        FrameDiffer.diff(previous, current, "ac") shouldBe null
    }

    @Test
    fun intellijConsoleFallsBackToFullRedraw() {
        val id = identity.copy(supportsAnsiCursor = true)
        val s = Lines(listOf(Line(listOf(Span.word("abc"))))).toFrameSnapshot(id)
        FrameDiffer.diff(s, s, unlimitedBudget) shouldBe ""
        val changed = Lines(listOf(Line(listOf(Span.word("abd"))))).toFrameSnapshot(id)
        FrameDiffer.diff(s, changed, unlimitedBudget) shouldBe null
    }

    @Test
    fun nonInteractiveTerminalFallsBackToFullRedraw() {
        val id = identity.copy(interactive = false)
        val s = Lines(listOf(Line(listOf(Span.word("abc"))))).toFrameSnapshot(id)
        val changed = Lines(listOf(Line(listOf(Span.word("abd"))))).toFrameSnapshot(id)
        FrameDiffer.diff(s, changed, unlimitedBudget) shouldBe null
    }

    @Test
    fun crImpliesLfPlatformFallsBackToFullRedraw() {
        val id = identity.copy(crImpliesLf = true)
        val s = Lines(listOf(Line(listOf(Span.word("abc"))))).toFrameSnapshot(id)
        val changed = Lines(listOf(Line(listOf(Span.word("abd"))))).toFrameSnapshot(id)
        FrameDiffer.diff(s, changed, unlimitedBudget) shouldBe null
    }

    @Test
    fun lineWiderThanTerminalFallsBackToFullRedraw() {
        val narrow = identity.copy(width = 4)
        val s = Lines(listOf(Line(listOf(Span.word("abcdef"))))).toFrameSnapshot(narrow)
        val changed = Lines(listOf(Line(listOf(Span.word("abcdeg"))))).toFrameSnapshot(narrow)
        FrameDiffer.diff(s, changed, unlimitedBudget) shouldBe null
    }

    @Test
    fun mismatchedIdentityIsRejected() {
        val s = snapshot("abc")
        val otherId = identity.copy(generation = 1)
        val changed = Lines(listOf(Line(listOf(Span.word("abd"))))).toFrameSnapshot(otherId)
        shouldThrow<IllegalArgumentException> {
            FrameDiffer.diff(s, changed, unlimitedBudget)
        }
    }
}
