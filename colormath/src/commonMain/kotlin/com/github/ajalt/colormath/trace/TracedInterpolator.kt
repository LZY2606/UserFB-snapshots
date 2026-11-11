package com.github.ajalt.colormath.trace

import com.github.ajalt.colormath.Color
import com.github.ajalt.colormath.ColorSpace
import com.github.ajalt.colormath.internal.TraceRecorder
import com.github.ajalt.colormath.transform.ComponentAdjustment
import com.github.ajalt.colormath.transform.HueAdjustments
import com.github.ajalt.colormath.transform.Interpolator
import com.github.ajalt.colormath.transform.InterpolatorBuilder
import com.github.ajalt.colormath.transform.interpolate
import com.github.ajalt.colormath.transform.interpolator

/**
 * Interpolation entry points that return a [ConversionTrace] alongside each interpolated color.
 *
 * As with [ColorTracer.convert], tracing is opt-in and shares the exact same calculation code as
 * the untraced interpolation functions.
 */
object TracedInterpolator {
    /**
     * Interpolate between [stop1] and [stop2] in [space], returning the result at [t] together
     * with a trace of every intermediate calculation, including stop conversions, hue adjustments,
     * alpha premultiplication, and the interpolation itself.
     */
    fun <T : Color> interpolate(
        space: ColorSpace<T>,
        stop1: Color,
        stop2: Color,
        t: Number,
        premultiplyAlpha: Boolean = true,
        hueAdjustment: ComponentAdjustment = HueAdjustments.shorter,
    ): ColorTracer.TracedConversion<T> {
        lateinit var output: T
        lateinit var trace: ConversionTrace
        TraceRecorder.record {
            record("input", "", stop1)
            record("input", "", stop2)
            output = space.convert(stop1)
                .interpolate(stop2, t, premultiplyAlpha, hueAdjustment)
            record("output", "", output)
            trace = build(stop1, output)
        }
        return ColorTracer.TracedConversion(output, trace)
    }

    /**
     * Build a traced multi-stop interpolator in [space].
     *
     * The stop conversions and component adjustments performed while building are recorded once,
     * and every [TracedInterpolatorHandle.interpolate] call returns a trace that includes those
     * build steps followed by the per-call interpolation steps.
     */
    fun <T : Color> interpolator(
        space: ColorSpace<T>,
        builder: InterpolatorBuilder.() -> Unit,
    ): TracedInterpolatorHandle<T> {
        lateinit var interpolator: Interpolator<T>
        lateinit var buildSteps: List<TraceStep>
        TraceRecorder.record {
            interpolator = space.interpolator(builder)
            buildSteps = snapshot()
        }
        return TracedInterpolatorHandle(interpolator, buildSteps)
    }
}

/**
 * A multi-stop interpolator whose [interpolate] calls return a [ConversionTrace].
 */
class TracedInterpolatorHandle<T : Color> internal constructor(
    private val interpolator: Interpolator<T>,
    private val buildSteps: List<TraceStep>,
) {
    /** Interpolate a color at [t], returning it with a trace of the full calculation. */
    fun interpolate(t: Number): ColorTracer.TracedConversion<T> {
        val firstStep = buildSteps.first()
        val input = firstStep.space.create(
            FloatArray(firstStep.components.size) { firstStep.components[it].value },
        )
        lateinit var output: T
        lateinit var trace: ConversionTrace
        TraceRecorder.record {
            appendSteps(buildSteps)
            output = interpolator.interpolate(t)
            record("output", "", output)
            trace = build(input, output)
        }
        return ColorTracer.TracedConversion(output, trace)
    }
}
