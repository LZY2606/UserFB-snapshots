package com.github.ajalt.colormath.trace

import com.github.ajalt.colormath.Color
import com.github.ajalt.colormath.ColorSpace
import com.github.ajalt.colormath.internal.TraceRecorder

/**
 * Run color conversions with an optional [ConversionTrace].
 *
 * Tracing is opt-in: the functions on this object are the only way to enable it. When tracing is
 * not enabled, conversions behave exactly as the untraced ones, with no extra allocations on the
 * hot path.
 */
object ColorTracer {
    /**
     * Convert [color] to [space], returning both the result and a [ConversionTrace] of every
     * intermediate calculation.
     *
     * The returned [TracedConversion.color] is computed by the exact same conversion code as
     * [Color.convertTo]; tracing only observes the intermediate values, it does not re-parse the
     * input or run a parallel implementation.
     */
    fun <T : Color> convert(color: Color, space: ColorSpace<T>): TracedConversion<T> {
        lateinit var output: T
        lateinit var trace: ConversionTrace
        TraceRecorder.record {
            record("input", "", color)
            output = space.convert(color)
            record("output", "", output)
            trace = build(color, output)
        }
        return TracedConversion(output, trace)
    }

    /**
     * The result of a traced conversion: the [color] that an untraced [Color.convertTo] would
     * return, plus the [trace] of how it was calculated.
     */
    data class TracedConversion<T : Color>(
        val color: T,
        val trace: ConversionTrace,
    )

    /**
     * Clamp [color] to its space's reference range, returning the result together with a trace.
     *
     * When no component is out of range, the color is returned unchanged and the trace contains
     * only the input and output steps. Otherwise, a `gamut-mapping` step records the clamped
     * result.
     */
    fun <T : Color> clamp(color: T): TracedConversion<T> {
        lateinit var output: Color
        lateinit var trace: ConversionTrace
        TraceRecorder.record {
            record("input", "", color)
            output = color.clamp()
            record("output", "", output)
            trace = build(color, output)
        }
        @Suppress("UNCHECKED_CAST")
        return TracedConversion(output as T, trace)
    }
}
