package com.github.ajalt.mordant.animation.diff

import java.io.File
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue

/**
 * Replays the checked-in virtual terminal trace fixture
 * `src/jvmTest/resources/animation/frame_diff_trace.txt` and asserts that the recorded byte
 * streams reproduce the recorded screens.
 *
 * The fixture is a hand-verified, directly replayable trace of the sequences [FrameDiffer] emits:
 * a full first frame, an in-place cell edit, an unchanged (empty) frame, a row insertion via
 * `CSI 1 L`, and a row deletion via `CSI 1 M`. Keeping it as a static file means it can be replayed
 * by any ANSI terminal implementation without running Mordant. ESC is written as `␛`, CR as `␍`
 * and LF as `␊` so the file stays human-readable.
 */
class FrameDiffTraceTest {
    private val width = 40
    private val resourcePath = "src/jvmTest/resources/animation/frame_diff_trace.txt"
    private val fallbackPath =
        "../../../../mordant/src/jvmTest/resources/animation/frame_diff_trace.txt"

    @Test
    fun recordedTraceReplaysToRecordedScreens() {
        val file = sequenceOf(resourcePath, fallbackPath)
            .map { File(it) }
            .firstOrNull { it.exists() }
        requireNotNull(file) { "Trace fixture not found at $resourcePath" }

        // Parse in order, pairing each TRACE with the SCREEN rows that follow it.
        data class Step(val trace: String, val expectedScreen: List<String>)
        val steps = mutableListOf<Step>()
        var pendingTrace: String? = null
        var pendingScreen = mutableListOf<String>()
        for (line in file.readLines()) {
            when {
                line.startsWith("TRACE ") -> {
                    if (pendingTrace != null) steps += Step(pendingTrace, pendingScreen)
                    pendingTrace = VirtualScreen.unescapeTrace(line.removePrefix("TRACE "))
                    pendingScreen = mutableListOf()
                }
                line.startsWith("SCREEN ") -> pendingScreen += line.removePrefix("SCREEN ")
            }
        }
        if (pendingTrace != null) steps += Step(pendingTrace, pendingScreen)

        // Replay every trace onto one virtual screen and compare after each write.
        val screen = VirtualScreen(width)
        val actualScreens = steps.map { step ->
            screen.replay(step.trace)
            screen.renderedLines()
        }

        assertTrue(steps.isNotEmpty(), "Fixture contained no TRACE records")
        assertEquals(steps.map { it.expectedScreen }, actualScreens)
    }
}
