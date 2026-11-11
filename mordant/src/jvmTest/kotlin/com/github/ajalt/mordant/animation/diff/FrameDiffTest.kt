package com.github.ajalt.mordant.animation.diff

import com.github.ajalt.mordant.animation.Animation
import com.github.ajalt.mordant.internal.CSI
import com.github.ajalt.mordant.internal.DEFAULT_STYLE
import com.github.ajalt.mordant.rendering.AnsiLevel
import com.github.ajalt.mordant.rendering.TextColors
import com.github.ajalt.mordant.rendering.TextStyles
import com.github.ajalt.mordant.rendering.Widget
import com.github.ajalt.mordant.terminal.Terminal
import com.github.ajalt.mordant.widgets.Text
import io.kotest.matchers.comparables.shouldBeLessThan
import io.kotest.matchers.shouldBe
import io.kotest.matchers.string.shouldContain
import io.kotest.matchers.string.shouldNotContain
import kotlin.test.Test

/**
 * End-to-end checks for frame snapshotting and diffing.
 *
 * Every scenario runs twice against identical in-memory terminals: once with a normal (diffing)
 * animation and once with diffing forced off. The full byte streams are replayed into
 * [VirtualScreen]s, and the resulting visible screens, cursor positions and styles must match.
 */
class FrameDiffTest {
    private val width = 24

    private fun newHarness(
        ansiLevel: AnsiLevel = AnsiLevel.TRUECOLOR,
        interactive: Boolean = true,
        supportsAnsiCursor: Boolean = false,
        initialWidth: Int = width,
    ): Harness {
        val diffRec = ResizableTerminalRecorder(initialWidth, ansiLevel = ansiLevel,
            outputInteractive = interactive, supportsAnsiCursor = supportsAnsiCursor)
        val fullRec = ResizableTerminalRecorder(initialWidth, ansiLevel = ansiLevel,
            outputInteractive = interactive, supportsAnsiCursor = supportsAnsiCursor)
        return Harness(
            diffTerminal = Terminal(terminalInterface = diffRec),
            fullTerminal = Terminal(terminalInterface = fullRec),
            diffRec = diffRec,
            fullRec = fullRec,
        )
    }

    private class Harness(
        val diffTerminal: Terminal,
        val fullTerminal: Terminal,
        val diffRec: ResizableTerminalRecorder,
        val fullRec: ResizableTerminalRecorder,
    ) {
        lateinit var diffAnimation: Animation<Int>
        lateinit var fullAnimation: Animation<Int>

        fun runScenario(scenario: Scenario) {
            diffAnimation = TestAnimations.diffing(diffTerminal, scenario::frame)
            fullAnimation = TestAnimations.fullRedraw(fullTerminal, scenario::frame)
            for (event in scenario.events) {
                when (event) {
                    is Event.Update -> {
                        diffAnimation.update(event.frame)
                        fullAnimation.update(event.frame)
                    }
                    is Event.Log -> {
                        diffTerminal.println(event.text)
                        fullTerminal.println(event.text)
                    }
                    is Event.Resize -> {
                        diffRec.resize(event.width, event.height)
                        fullRec.resize(event.width, event.height)
                        diffTerminal.updateSize()
                        fullTerminal.updateSize()
                    }
                    is Event.CapabilityChange -> {
                        diffTerminal.terminalInfo.ansiLevel = event.level
                        fullTerminal.terminalInfo.ansiLevel = event.level
                    }
                }
            }
        }

        /**
         * Replay both accumulated streams and assert that the screens end up identical.
         *
         * [screenWidth] must be the narrowest terminal width used in the scenario so that physical
         * line wrapping is emulated correctly.
         */
        fun assertScreensMatch(screenWidth: Int = width) {
            val diffScreen = VirtualScreen(screenWidth).apply { replay(diffRec.output()) }
            val fullScreen = VirtualScreen(screenWidth).apply { replay(fullRec.output()) }
            diffScreen.renderedLines() shouldBe fullScreen.renderedLines()
            diffScreen.cursorRow shouldBe fullScreen.cursorRow
            diffScreen.cursorColumn shouldBe fullScreen.cursorColumn
            diffScreen.currentStyle shouldBe fullScreen.currentStyle
            diffScreen.currentStyle shouldBe DEFAULT_STYLE
        }
    }

    private interface Scenario {
        val events: List<Event>
        fun frame(index: Int): Widget
    }

    private sealed interface Event {
        data class Update(val frame: Int) : Event
        data class Log(val text: String) : Event
        data class Resize(val width: Int, val height: Int = 24) : Event
        data class CapabilityChange(val level: AnsiLevel) : Event
    }

    private fun scenario(
        frames: (Int) -> Widget,
        vararg events: Event,
    ): Scenario = object : Scenario {
        override val events = events.toList()
        override fun frame(index: Int) = frames(index)
    }

    // --- The scenarios mandated by the task ----------------------------------------------

    @Test
    fun cjkWideCellsAreNeverSplit() {
        val h = newHarness(initialWidth = 40)
        h.runScenario(
            scenario(
                { i -> Text("日本語テスト status number $i 終わり") },
                Event.Update(0), Event.Update(1), Event.Update(2),
            )
        )
        // The exact cell-skipping sequence is pinned in FrameDifferUnitTest; here we verify that
        // whatever bytes the differ chooses reproduce the full-redraw screen.
        h.assertScreensMatch(screenWidth = 40)
    }

    @Test
    fun combiningMarksStayAttachedToTheirBase() {
        val h = newHarness(initialWidth = 40)
        // é is e + U+0301 combining acute; the long common prefix stays untouched while the
        // trailing number changes.
        h.runScenario(
            scenario(
                { i -> Text("the café table reservation number $i") },
                Event.Update(0), Event.Update(1),
            )
        )
        h.assertScreensMatch(screenWidth = 40)
    }

    @Test
    fun emojiZwjSequencesAreAtomic() {
        val h = newHarness(initialWidth = 40)
        h.runScenario(
            scenario(
                { i -> Text("👩‍💻 build pipeline status update $i") },
                Event.Update(0), Event.Update(1), Event.Update(2),
            )
        )
        h.assertScreensMatch(screenWidth = 40)
    }

    @Test
    fun rowsAreInsertedAndDeletedWithScrollCommands() {
        val h = newHarness(initialWidth = 40)
        val frames: (Int) -> Widget = { i ->
            Text(
                when (i) {
                    0 -> listOf(
                        "the first row stays constant",
                        "the second row stays constant",
                        "the third row stays constant",
                    )
                    1 -> listOf(
                        "the first row stays constant",
                        "a brand new inserted middle row",
                        "the second row stays constant",
                        "the third row stays constant",
                    )
                    else -> listOf(
                        "the first row stays constant",
                        "the second row stays constant",
                        "the third row stays constant",
                    )
                }.joinToString("\n")
            )
        }
        h.runScenario(scenario(frames, Event.Update(0), Event.Update(1), Event.Update(2)))
        h.assertScreensMatch(screenWidth = 40)
        // The insert/delete control sequences themselves are pinned in FrameDifferUnitTest.
    }

    @Test
    fun narrowingTheTerminalInvalidatesTheSnapshotAndRedrawsFully() {
        val h = newHarness(initialWidth = 30)
        h.runScenario(
            scenario(
                { i -> Text("frame number $i with some text") },
                Event.Update(0),
                Event.Resize(width = 12),
                Event.Update(1),
                Event.Update(2),
            )
        )
        h.assertScreensMatch(screenWidth = 12)
    }

    @Test
    fun interleavedLogLineInvalidatesTheSnapshot() {
        val h = newHarness(initialWidth = 40)
        h.runScenario(
            scenario(
                { i -> Text("background job progress indicator $i") },
                Event.Update(0),
                Event.Log("a log line"),
                Event.Update(1),
                Event.Update(2),
            )
        )
        h.assertScreensMatch(screenWidth = 40)
        h.diffRec.output() shouldContain "a log line"
    }

    // --- Additional correctness cases -----------------------------------------------------

    @Test
    fun styleOnlyChangeIsDetectedWhenTextIsIdentical() {
        // ANSI16 so the downsampled codes are the compact 31m/32m rather than 24-bit sequences.
        val h = newHarness(ansiLevel = AnsiLevel.ANSI16, initialWidth = 40)
        h.runScenario(
            scenario(
                { i ->
                    Text(
                        if (i == 0) TextColors.red("the same long status message")
                        else TextColors.green("the same long status message")
                    )
                },
                Event.Update(0), Event.Update(1),
            )
        )
        h.assertScreensMatch(screenWidth = 40)
        // Frame 0 carries red; frame 1 (full redraw or diff) carries green.
        h.diffRec.output() shouldContain "${CSI}31m"
        h.diffRec.output() shouldContain "${CSI}32m"
    }

    @Test
    fun identicalFrameWritesNothing() {
        val h = newHarness()
        h.runScenario(
            scenario({ Text("stable") }, Event.Update(0), Event.Update(1), Event.Update(2))
        )
        h.assertScreensMatch()
        // The first update draws; the two identical updates emit no bytes at all.
        h.diffRec.output() shouldBe "stable"
    }

    @Test
    fun diffThatIsNotSmallerThanFullRedrawFallsBack() {
        val h = newHarness()
        // Two-character frames: positioning overhead makes any diff larger than the 2-byte frame.
        h.runScenario(scenario({ Text("v$it") }, Event.Update(0), Event.Update(1), Event.Update(2)))
        h.assertScreensMatch()
        h.diffRec.output() shouldBe h.fullRec.output()
        h.diffRec.output() shouldNotContain "${CSI}1A"
    }

    @Test
    fun largerFrameProducesASmallerDiff() {
        // Wide enough that the 36-column line fits on one row and can be diffed in place.
        val h = newHarness(initialWidth = 40)
        h.runScenario(
            scenario(
                { i -> Text("the quick brown fox jumps over $i") },
                Event.Update(0), Event.Update(1),
            )
        )
        h.assertScreensMatch(screenWidth = 40)
        val firstFrame = "the quick brown fox jumps over 0"
        val secondFrame = "the quick brown fox jumps over 1"
        // Everything after the first frame is the incremental update.
        val diffOnly = h.diffRec.output().substringAfter(firstFrame)
        diffOnly shouldContain CSI
        diffOnly.length shouldBeLessThan secondFrame.length
    }

    @Test
    fun capabilityChangeForcesFullRedraw() {
        val h = newHarness(initialWidth = 40)
        h.runScenario(
            scenario(
                { i -> Text(TextColors.rgb(1, 2, 3)("the colored status frame $i")) },
                Event.Update(0),
                Event.CapabilityChange(AnsiLevel.ANSI16),
                Event.Update(1),
            )
        )
        h.assertScreensMatch(screenWidth = 40)
    }

    @Test
    fun intellijStyleConsoleWithoutCursorMovesRedrawsFully() {
        val h = newHarness(supportsAnsiCursor = true)
        h.runScenario(
            scenario({ Text("line $it") }, Event.Update(0), Event.Update(1))
        )
        h.assertScreensMatch()
        h.diffRec.output() shouldBe h.fullRec.output()
    }

    @Test
    fun nonInteractiveOutputIsUnchanged() {
        val h = newHarness(interactive = false, ansiLevel = AnsiLevel.NONE)
        h.runScenario(
            scenario({ Text("frame $it") }, Event.Update(0), Event.Update(1))
        )
        // Animations render nothing on non-interactive terminals, exactly as before diffing.
        h.diffRec.output() shouldBe ""
        h.fullRec.output() shouldBe ""
    }

    @Test
    fun renderExceptionResetsStateAndNextFrameRedrawsFully() {
        val h = newHarness()
        val animation = TestAnimations.diffing(h.diffTerminal) { i ->
            if (i == 1) throw RuntimeException("boom") else Text("good frame $i")
        }
        animation.update(0)
        h.diffRec.clearOutput()

        val thrown = runCatching { animation.update(1) }.exceptionOrNull()
        thrown?.message shouldBe "boom"
        // Style is reset even though the frame on screen was never touched.
        h.diffRec.output() shouldBe "${CSI}0m"

        h.diffRec.clearOutput()
        animation.update(2)
        // Snapshot was invalidated, so the recovery frame is a full redraw containing all its text.
        h.diffRec.output() shouldContain "good frame 2"
        VirtualScreen(width).apply { replay(h.diffRec.output()) }.renderedLines() shouldBe
            listOf("good frame 2")
    }

    @Test
    fun boldAndDimStyleTransitionsRoundTrip() {
        val h = newHarness(initialWidth = 40)
        h.runScenario(
            scenario(
                { i ->
                    Text(
                        if (i == 0) TextStyles.bold("a long styled prefix") + " plain suffix"
                        else TextStyles.underline("a long styled prefix") + " plain suffix"
                    )
                },
                Event.Update(0), Event.Update(1),
            )
        )
        h.assertScreensMatch(screenWidth = 40)
    }
}
