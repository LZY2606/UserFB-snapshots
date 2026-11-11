package com.github.ajalt.mordant.animation

import com.github.ajalt.mordant.internal.CR_IMPLIES_LF
import com.github.ajalt.mordant.internal.CSI
import com.github.ajalt.mordant.internal.MppAtomicRef
import com.github.ajalt.mordant.internal.update
import com.github.ajalt.mordant.rendering.*
import com.github.ajalt.mordant.terminal.PrintRequest
import com.github.ajalt.mordant.terminal.Terminal
import com.github.ajalt.mordant.terminal.TerminalInterceptor
import com.github.ajalt.mordant.widgets.EmptyWidget
import com.github.ajalt.mordant.widgets.Text
import com.github.ajalt.mordant.widgets.progress.progressBarLayout

/**
 * An Animation renders a widget to the screen each time [update] is called, clearing the render
 * from the previous call.
 *
 * On ordinary interactive terminals, each frame is compared to the previous one as *rendered
 * screen state* — grapheme clusters with their downsampled styles, after measure/render and
 * column-width wrapping — and only the cells, rows and styles that changed are written. If an
 * incremental update cannot be done safely (after a resize, after output interleaved from another
 * source, after the terminal capabilities change, while another animation is on screen, or when
 * the diff would not be smaller than a full redraw), the whole frame is redrawn instead.
 *
 * If you [print][Terminal.print] anything in between calls to [update], the rendered widget will
 * be cleared before the print, then rendered again below it. Interleaved output invalidates the
 * frame snapshot, so the next [update] always does a full redraw.
 *
 * Note that [Animation] doesn't render the widget automatically: you need to call [update] yourself
 * when your data changes. If your terminal is not [interactive][TerminalInfo], the animation will
 * not render anything, no diffing is performed, and the output behaves like a sequence of plain
 * prints.
 *
 * You can create instances of Animations with [animation], [textAnimation], animate a
 * [progressBarLayout], or by creating a subclass.
 *
 * Note that although this class's state is thread safe, calling [update] concurrently will likely
 * cause garbled output, so usage of this class should be serialized.
 */
abstract class Animation<T>(
    val terminal: Terminal,
) : StoppableAnimation {
    /**
     * Test-only constructor that allows frame diffing to be disabled so that full-redraw behavior
     * can be compared against incremental behavior. Not part of the public API.
     */
    internal constructor(terminal: Terminal, frameDiffingEnabled: Boolean) : this(terminal) {
        this.frameDiffingEnabled = frameDiffingEnabled
    }

    private var frameDiffingEnabled: Boolean = true

    /**
     * Test seam that disables frame diffing globally for animations whose own
     * [frameDiffingEnabled] flag is on. Production code never touches it. It lets legacy tests
     * that assert exact full-redraw byte streams keep their expectations while the new virtual
     * screen tests cover the default diffing behavior. Not part of the public API.
     */
    internal object TestHooks {
        var forceFullRedraw: Boolean = false
    }

    private data class State(
        /** The length of each line of the last rendered widget */
        val size: List<Int>? = null,
        /** The length of each line of the previously rendered widget */
        val lastSize: List<Int>? = null,
        val lastTerminalSize: Size? = null,
        /** The ANSI bytes of the frame currently on screen */
        val text: String? = null,
        /** The rendered screen snapshot of the frame currently on screen, or null if invalidated */
        val snapshot: FrameSnapshot? = null,
        /** Bumped whenever the snapshot can no longer be trusted (interleaved output, stop, ...) */
        val generation: Int = 0,
        /** True while [update] is writing a diff directly, so the interceptor passes it through */
        val writingDiff: Boolean = false,
        val interceptorInstalled: Boolean = false,
        val firstDraw: Boolean = true,
    )

    private val state = MppAtomicRef(State())

    private val interceptor: TerminalInterceptor = TerminalInterceptor { req ->
        // A diff written directly by update() passes through the chain untouched.
        if (state.value.writingDiff) return@TerminalInterceptor req

        val terminalSize = Size(terminal.size.width, terminal.size.height)
        // Our own full-redraw tick prints an empty widget; anything else is external output or a
        // sibling animation that changes the screen behind our back.
        val ownTick = req.text.isEmpty() && !req.trailingLinebreak
        val (st, _) = state.update {
            copy(
                firstDraw = false,
                lastSize = size,
                lastTerminalSize = terminalSize,
                // External output invalidates the snapshot and advances the generation. Our own
                // tick leaves the frame on screen untouched, so the snapshot stays valid.
                snapshot = if (ownTick) snapshot else null,
                generation = if (ownTick) generation else generation + 1,
            )
        }
        val animationText = st.text ?: return@TerminalInterceptor req
        val reqText = if (req.trailingLinebreak) req.text + "\n" else req.text
        val newText = buildString {
            // move the cursor to the start of the widget, then append the request (which might
            // start with moves if it's an animation), then our text
            getCursorMoves(
                firstDraw = st.firstDraw,
                clearScreen = reqText.isNotEmpty(),
                lastSize = st.lastSize,
                size = st.size,
                terminalSize = terminalSize,
                lastTerminalSize = st.lastTerminalSize,
                extraUp = if (reqText.startsWith("\r")) 1 else 0, // it's another animation
            )?.let { append(it) }
            when {
                reqText.endsWith("\n") -> append(reqText)
                reqText.isNotEmpty() -> appendLine(reqText)
            }
            append(animationText)
        }

        PrintRequest(
            text = newText,
            trailingLinebreak = false,
            stderr = req.stderr
        )
    }

    protected abstract fun renderData(data: T): Widget

    /**
     * Stop this animation and remove it from the screen.
     *
     * Future calls to [update] will cause the animation to resume.
     */
    final override fun clear() {
        val (old, _) = doStop(clearSize = true, newline = false)
        getCursorMoves(
            firstDraw = false,
            clearScreen = true,
            lastSize = old.size,
            size = null,
            terminalSize = Size(terminal.size.width, terminal.size.height),
            lastTerminalSize = old.lastTerminalSize,
            // if we previously stopped, we need to move up past the final newline we added
            extraUp = if (old.firstDraw && old.size != null) 1 else 0,
        )?.let { terminal.rawPrint(it) }
    }

    /**
     * Stop this animation without removing it from the screen.
     *
     * Anything printed to the terminal after this call will be printed below this last frame of
     * this animation.
     *
     * Future calls to [update] will cause the animation to start again.
     */
    final override fun stop() {
        doStop(clearSize = false, newline = true)
    }

    private fun doStop(clearSize: Boolean, newline: Boolean): Pair<State, State> {
        val (old, new) = state.update {
            copy(
                interceptorInstalled = false,
                firstDraw = true,
                text = null,
                snapshot = null,
                writingDiff = false,
                generation = generation + 1,
                size = if (clearSize) null else size,
            )
        }
        if (old.interceptorInstalled) {
            terminal.removeInterceptor(interceptor)
            // If another animation is still active, it will draw below this frame on its own
            if (newline && !terminal.hasInterceptors()) terminal.println()
        }
        return old to new
    }

    /**
     * Update the data for this animation.
     *
     * The new data is rendered and compared against the frame currently on screen. When the
     * terminal supports the needed cursor sequences and the previous snapshot is still valid, only
     * the changed cells and rows are written; otherwise the whole frame is redrawn.
     */
    fun update(data: T) {
        if (terminal.terminalInterface.shouldAutoUpdateSize()) terminal.updateSize()

        val rendered: Lines
        val fullText: String
        try {
            rendered = renderData(data).render(terminal)
            fullText = terminal.render(rendered)
        } catch (e: Throwable) {
            // Nothing is written until the diff is fully computed, so the frame on screen is
            // untouched. Invalidate the snapshot, reset any style the terminal might hold, and
            // rethrow so the caller (including a coroutine cancellation) can handle the failure.
            invalidateAfterFailure()
            throw e
        }

        val terminalSize = Size(terminal.size.width, terminal.size.height)
        val info = terminal.terminalInfo
        val old = state.value
        val identity = FrameIdentity(
            generation = old.generation,
            width = terminalSize.width,
            ansiLevel = info.ansiLevel,
            hyperlinks = info.ansiHyperLinks,
            supportsAnsiCursor = info.supportsAnsiCursor,
            interactive = info.outputInteractive,
            crImpliesLf = CR_IMPLIES_LF,
        )
        val newSnapshot = rendered.toFrameSnapshot(identity)
        val resized = old.lastTerminalSize != null &&
                (terminalSize.width != old.lastTerminalSize.width ||
                        terminalSize.height != old.lastTerminalSize.height)
        // A sibling animation shares the interceptor chain and relies on full-frame reflow.
        val siblingActive = old.interceptorInstalled && terminal.interceptorCount() > 1

        val diff = if (frameDiffingEnabled && !TestHooks.forceFullRedraw &&
            old.snapshot != null && !old.firstDraw &&
            !resized && !siblingActive && old.snapshot.identity == identity
        ) {
            FrameDiffer.diff(old.snapshot, newSnapshot, fullText)
        } else null

        state.update {
            copy(
                size = rendered.lines.map { it.lineWidth },
                lastSize = size,
                lastTerminalSize = terminalSize,
                interceptorInstalled = true,
                firstDraw = false,
                text = fullText,
                snapshot = newSnapshot,
                writingDiff = false,
            )
        }

        if (!old.interceptorInstalled && info.outputInteractive) {
            terminal.addInterceptor(interceptor)
        }
        try {
            when {
                diff == null -> {
                    // Full redraw: print an empty widget to trigger our interceptor, which prepends
                    // the cursor moves and appends the full frame text.
                    terminal.print(EmptyWidget)
                }

                diff.isEmpty() -> {
                    // The visible frame did not change; nothing to write.
                }

                else -> {
                    // Incremental update. Flag the write so the interceptor passes it through, then
                    // restore the flag in finally so an exception can't leave it stuck.
                    state.update { copy(writingDiff = true) }
                    terminal.rawPrint(diff)
                }
            }
        } catch (e: Throwable) {
            // The write may have reached the terminal partially, so the snapshot can no longer be
            // trusted. Invalidate it and reset the style, then rethrow.
            invalidateAfterFailure()
            throw e
        } finally {
            state.update { copy(writingDiff = false) }
        }
    }

    /**
     * Mark the on-screen snapshot invalid so the next [update] redraws fully, and emit an SGR reset
     * directly (bypassing the interceptor) so the terminal is not left holding a partial style.
     */
    private fun invalidateAfterFailure() {
        state.update {
            copy(snapshot = null, writingDiff = false, generation = generation + 1)
        }
        if (terminal.terminalInfo.outputInteractive) {
            state.update { copy(writingDiff = true) }
            try {
                runCatching { terminal.rawPrint("${CSI}0m") }
            } finally {
                state.update { copy(writingDiff = false) }
            }
        }
    }

    private fun getCursorMoves(
        firstDraw: Boolean,
        clearScreen: Boolean,
        lastSize: List<Int>?,
        size: List<Int>?,
        terminalSize: Size,
        lastTerminalSize: Size?,
        extraUp: Int = 0,
    ): String? {
        if (firstDraw || lastSize == null) return null
        return terminal.cursor.getMoves {
            startOfLine()
            if (CR_IMPLIES_LF) up(1)

            if (terminal.terminalInfo.supportsAnsiCursor) {
                // IntelliJ doesn't support cursor moves, so this is all we can do
                return@getMoves
            }

            val lastWidth = lastSize.maxOrNull() ?: 0
            val lastHeight = lastSize.size
            val terminalShrank = lastTerminalSize != null
                    && terminalSize.width < lastTerminalSize.width
                    && terminalSize.width < lastWidth
            val widgetShrank = size != null && (
                    size.size < lastHeight
                            || size.zip(lastSize).any { (a, b) -> a < b }
                    )
            val up = if (terminalShrank) {
                // The terminal shrank and caused the text to wrap, we need to move back to the
                // start of the text
                lastHeight * (lastWidth.toDouble() / terminalSize.width).toInt()
            } else {
                (lastHeight - 1).coerceAtLeast(0)
            }

            up(up + extraUp)

            // To avoid flickering don't clear the screen if the render will completely cover
            // the last frame
            if (terminalShrank || widgetShrank || clearScreen) clearScreenAfterCursor()
        }
    }
}

/**
 * Create an [Animation] that uses the [draw] function to render objects of type [T].
 *
 * @see Animation
 */
inline fun <T> Terminal.animation(
    crossinline draw: (T) -> Widget,
): Animation<T> {
    return object : Animation<T>(this) {
        override fun renderData(data: T): Widget = draw(data)
    }
}

/**
 * Create an [Animation] that wraps the result of the [draw] function into a [Text] widget and
 * renders it.
 */
inline fun <T> Terminal.textAnimation(
    whitespace: Whitespace = Whitespace.PRE,
    align: TextAlign = TextAlign.NONE,
    overflowWrap: OverflowWrap = OverflowWrap.NORMAL,
    width: Int? = null,
    tabWidth: Int? = null,
    crossinline draw: (T) -> String,
): Animation<T> {
    return object : Animation<T>(this) {
        override fun renderData(data: T): Widget {
            return Text(draw(data), whitespace, align, overflowWrap, width, tabWidth)
        }
    }
}
