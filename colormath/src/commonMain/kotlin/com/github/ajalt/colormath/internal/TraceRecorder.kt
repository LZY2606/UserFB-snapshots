package com.github.ajalt.colormath.internal

import com.github.ajalt.colormath.Color
import com.github.ajalt.colormath.ColorComponentInfo
import com.github.ajalt.colormath.ColorSpace
import com.github.ajalt.colormath.trace.ConversionTrace
import com.github.ajalt.colormath.trace.TraceComponent
import com.github.ajalt.colormath.trace.TraceComponentState
import com.github.ajalt.colormath.trace.TraceStep

/**
 * A sink for the intermediate values of a single color conversion.
 *
 * The recorder is only ever instantiated when tracing is explicitly enabled. The hot conversion
 * paths check [current] (a null check with no allocations) before touching it.
 */
internal class TraceRecorder {
    private val steps = mutableListOf<TraceStep>()

    /** Record an intermediate [color] produced by [method], with an optional [detail] string. */
    fun record(method: String, detail: String, color: Color) {
        steps += TraceStep(method, detail, color.space, componentsOf(color.space, color.toArray()))
    }

    /** Record an intermediate set of [values] in [space], produced by [method]. */
    fun record(method: String, detail: String, space: ColorSpace<*>, values: FloatArray) {
        steps += TraceStep(method, detail, space, componentsOf(space, values))
    }

    fun build(input: Color, output: Color): ConversionTrace {
        return ConversionTrace(input, output, steps.toList())
    }

    /** The steps recorded so far. */
    fun snapshot(): List<TraceStep> = steps.toList()

    /** Append steps recorded by an earlier conversion (for example, interpolator build steps). */
    fun appendSteps(other: List<TraceStep>) {
        steps += other
    }

    companion object {
        /**
         * The recorder for the conversion in progress on this call chain, or null when tracing
         * is disabled.
         *
         * Tracing is a diagnostic feature that is enabled explicitly for a single conversion, so
         * a plain holder is used instead of thread-local storage in order to keep the implementation
         * in common code. Nested traced conversions save and restore the previous value.
         */
        internal var current: TraceRecorder? = null

        inline fun <T> record(block: TraceRecorder.() -> T): T {
            val recorder = TraceRecorder()
            val previous = current
            current = recorder
            try {
                return recorder.block()
            } finally {
                current = previous
            }
        }
    }
}

private fun componentsOf(space: ColorSpace<*>, values: FloatArray): List<TraceComponent> {
    val infos: List<ColorComponentInfo> = space.components
    return buildList(infos.size) {
        for (i in infos.indices) {
            val info = infos[i]
            val value = values[i]
            val state = when {
                value.isNaN() -> TraceComponentState.UNDEFINED
                value == 0f -> TraceComponentState.ZERO
                value !in info.min..info.max -> TraceComponentState.OUT_OF_RANGE
                else -> TraceComponentState.DEFINED
            }
            add(TraceComponent(info.name, value, state))
        }
    }
}
