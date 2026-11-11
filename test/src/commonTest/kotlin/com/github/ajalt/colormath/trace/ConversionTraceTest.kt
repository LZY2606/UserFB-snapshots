package com.github.ajalt.colormath.trace

import com.github.ajalt.colormath.Color
import com.github.ajalt.colormath.ColorSpace
import com.github.ajalt.colormath.convertTo
import com.github.ajalt.colormath.parse
import com.github.ajalt.colormath.model.HSL
import com.github.ajalt.colormath.model.LCHabColorSpaces.LCHab50
import com.github.ajalt.colormath.model.RGB
import com.github.ajalt.colormath.model.RGBColorSpaces.DisplayP3
import com.github.ajalt.colormath.model.RGBColorSpaces.SRGB
import com.github.ajalt.colormath.transform.HueAdjustments
import com.github.ajalt.colormath.transform.interpolate
import io.kotest.matchers.collections.shouldContainExactly
import io.kotest.matchers.doubles.shouldBeNaN
import io.kotest.matchers.floats.plusOrMinus
import io.kotest.matchers.shouldBe
import kotlin.test.Test

/**
 * End-to-end tests for the opt-in [ConversionTrace].
 *
 * Inputs are CSS Color 4 strings. Each test asserts the method used at every step, the numeric
 * values produced, and the per-component state (including undefined hues, exact zeros, and
 * unclamped out-of-range values). On failure the full input and every intermediate step are
 * printed, followed by the first mismatch.
 */
class ConversionTraceTest {
    private val tol = 1e-4f

    // region helpers

    private fun report(trace: ConversionTrace): String = buildString {
        appendLine("TRACE REPORT")
        appendLine("input : ${trace.input}")
        trace.steps.forEachIndexed { i, step ->
            appendLine("step $i: [${step.method}] ${step.detail} (${step.space})")
            step.components.forEach { c ->
                appendLine("    ${c.name} = ${c.value} [${c.state}]")
            }
        }
        appendLine("output: ${trace.output}")
    }

    private fun ConversionTrace.step(method: String): TraceStep =
        steps.first { it.method == method }

    private fun ConversionTrace.steps(method: String): List<TraceStep> =
        steps.filter { it.method == method }

    private fun TraceStep.component(name: String): TraceComponent =
        components.first { it.name == name }

    private fun TraceStep.floatValue(name: String): Float = component(name).value

    private fun assertStepValue(
        trace: ConversionTrace,
        step: TraceStep,
        component: String,
        expected: Float,
    ) {
        val actual = step.floatValue(component)
        if (actual !in (expected - tol)..(expected + tol)) {
            throw AssertionError(
                report(trace) +
                        "FIRST MISMATCH at step [${step.method}] ${step.detail}, " +
                        "component $component: expected $expected (±$tol) but was $actual"
            )
        }
    }

    private fun assertStepState(
        trace: ConversionTrace,
        step: TraceStep,
        component: String,
        expected: TraceComponentState,
    ) {
        val actual = step.component(component).state
        if (actual != expected) {
            throw AssertionError(
                report(trace) +
                        "FIRST MISMATCH at step [${step.method}] ${step.detail}, " +
                        "component $component: expected state $expected but was $actual"
            )
        }
    }

    // endregion

    @Test
    fun cross_white_point_and_cross_gamut_conversion() {
        // CSS Color 4 input: LCH in D50, converted to Display P3 (D65, different gamut).
        val input = Color.parse("lch(52% 58% 250)")
        val traced = ColorTracer.convert(input, DisplayP3)
        val trace = traced.trace

        // The traced result is the exact same calculation as an untraced conversion.
        val untraced = input.convertTo(DisplayP3)
        traced.color shouldBe untraced
        traced.color.toArray().toList() shouldContainExactly untraced.toArray().toList()

        // The recorded methods follow the real conversion pipeline, in order.
        trace.steps.map { it.method }.shouldContainExactly(
            "input",
            "polar-transform",
            "model-transform",
            "chromatic-adaptation",
            "matrix-transform",
            "transfer-function",
            "output",
        )

        val polar = trace.step("polar-transform")
        polar.detail shouldBe "polar-to-rectangular (LCHab to LAB)"
        assertStepValue(trace, polar, "A", -29.755745f)
        assertStepValue(trace, polar, "B", -81.753265f)

        val labToXyz = trace.step("model-transform")
        labToXyz.detail shouldBe "LAB to XYZ"
        assertStepValue(trace, labToXyz, "X", 0.1408928f)
        assertStepValue(trace, labToXyz, "Y", 0.20144327f)
        assertStepValue(trace, labToXyz, "Z", 0.8127241f)

        val adaptation = trace.step("chromatic-adaptation")
        adaptation.detail shouldBe "von Kries chromatic adaptation: D50 -> D65"
        assertStepValue(trace, adaptation, "X", 0.182743f)
        assertStepValue(trace, adaptation, "Y", 0.21948951f)
        assertStepValue(trace, adaptation, "Z", 1.0681274f)
        // Out-of-range values are recorded raw, never clamped for display.
        assertStepState(trace, adaptation, "Z", TraceComponentState.OUT_OF_RANGE)

        val linearRgb = trace.step("matrix-transform")
        linearRgb.detail shouldBe "XYZ to linear RGB matrix"
        assertStepValue(trace, linearRgb, "R", -0.1789062f)
        assertStepValue(trace, linearRgb, "G", 0.26053715f)
        assertStepValue(trace, linearRgb, "B", 1.0119061f)
        assertStepState(trace, linearRgb, "R", TraceComponentState.OUT_OF_RANGE)
        assertStepState(trace, linearRgb, "B", TraceComponentState.OUT_OF_RANGE)

        val encoded = trace.step("transfer-function")
        encoded.detail shouldBe "linear to non-linear RGB OETF"
        assertStepValue(trace, encoded, "R", -2.3114681f)
        assertStepValue(trace, encoded, "B", 1.0052156f)
        assertStepState(trace, encoded, "R", TraceComponentState.OUT_OF_RANGE)

        // The output step holds the final color, still unclamped.
        val output = trace.step("output")
        output.space shouldBe DisplayP3
        assertStepState(trace, output, "R", TraceComponentState.OUT_OF_RANGE)
        output.floatValue("R") shouldBe (untraced.r plusOrMinus tol)
    }

    @Test
    fun hue_boundary_interpolation_records_shorter_path() {
        // Red (hue 0) to blue (hue 240): the shorter path crosses the 0/360 boundary downward.
        val red = Color.parse("hsl(0 100% 50%)")
        val blue = Color.parse("hsl(240 100% 50%)")
        val traced = TracedInterpolator.interpolate(
            HSL, red, blue, 0.5f, premultiplyAlpha = true, hueAdjustment = HueAdjustments.shorter,
        )
        val trace = traced.trace

        trace.steps.map { it.method }.shouldContainExactly(
            "input",
            "input",
            "interpolation-stop",
            "interpolation-stop",
            "hue-adjustment",
            "hue-adjustment",
            "interpolation",
            "output",
        )

        val adjustedHues = trace.steps("hue-adjustment").map { it.floatValue("H") }
        // 0 stays 0; 240 is unwrapped to -120 so the interpolation takes the shorter arc.
        adjustedHues[0] shouldBe (0f plusOrMinus tol)
        adjustedHues[1] shouldBe (-120f plusOrMinus tol)

        val interpolation = trace.step("interpolation")
        assertStepValue(trace, interpolation, "H", -60f)
        assertStepValue(trace, interpolation, "S", 1f)
        assertStepValue(trace, interpolation, "L", 0.5f)
        // Interpolated hues are intentionally not normalized or clamped mid-calculation.
        assertStepState(trace, interpolation, "H", TraceComponentState.OUT_OF_RANGE)

        val output = trace.step("output")
        output.floatValue("H") shouldBe (-60f plusOrMinus tol)

        // The untraced interpolation produces the identical color.
        val untraced = red.convertTo(HSL).interpolate(blue, 0.5f)
        traced.color.toArray().toList() shouldContainExactly untraced.toArray().toList()
    }

    @Test
    fun undefined_hue_is_distinct_from_zero() {
        // "none" is the CSS Color 4 missing keyword, parsed to a NaN (undefined) hue.
        val achromatic = Color.parse("lch(50% 0% none)")
        val trace = ColorTracer.convert(achromatic, SRGB).trace

        val input = trace.step("input")
        assertStepState(trace, input, "H", TraceComponentState.UNDEFINED)
        input.component("H").value.isNaN() shouldBe true
        // The zero chroma is a real zero, not "undefined".
        assertStepState(trace, input, "C", TraceComponentState.ZERO)

        val polar = trace.step("polar-transform")
        // The undefined hue is treated as 0 only for the rectangular conversion, and the trace
        // says so explicitly rather than silently recording a hue of 0.
        polar.detail.contains("undefined hue treated as 0") shouldBe true
        assertStepState(trace, polar, "A", TraceComponentState.ZERO)
        assertStepState(trace, polar, "B", TraceComponentState.ZERO)

        // The only hue component in the whole trace is the input's, and it stays undefined.
        val hueComponents = trace.steps.flatMap { step ->
            step.components.filter { it.name.equals("H", ignoreCase = true) }
        }
        hueComponents.size shouldBe 1
        hueComponents.single().state shouldBe TraceComponentState.UNDEFINED
        hueComponents.single().value.isNaN() shouldBe true

        // Double-check the semantic distinction directly on the state enum.
        (TraceComponentState.UNDEFINED == TraceComponentState.ZERO) shouldBe false
        achromatic.toArray()[2].toDouble().shouldBeNaN()
    }

    @Test
    fun out_of_range_values_are_recorded_without_clamping() {
        val vivid = Color.parse("lch(60% 90% 30)")
        val trace = ColorTracer.convert(vivid, SRGB).trace

        val linearRgb = trace.step("matrix-transform")
        assertStepValue(trace, linearRgb, "R", 1.657282f)
        assertStepValue(trace, linearRgb, "G", -0.12114991f)
        assertStepState(trace, linearRgb, "R", TraceComponentState.OUT_OF_RANGE)
        assertStepState(trace, linearRgb, "G", TraceComponentState.OUT_OF_RANGE)

        // Negative and >1 values survive the transfer function and the output step untouched.
        val output = trace.step("output")
        assertStepValue(trace, output, "R", 1.2471699f)
        assertStepValue(trace, output, "G", -1.5652568f)
        assertStepState(trace, output, "R", TraceComponentState.OUT_OF_RANGE)
        assertStepState(trace, output, "G", TraceComponentState.OUT_OF_RANGE)
        assertStepState(trace, output, "B", TraceComponentState.DEFINED)
    }

    @Test
    fun clamp_records_a_gamut_mapping_step() {
        val vivid = Color.parse("lch(60% 90% 30)").convertTo(SRGB)
        val traced = ColorTracer.clamp(vivid)
        val trace = traced.trace

        val mapping = trace.step("gamut-mapping")
        mapping.detail shouldBe "clamp components to reference ranges"
        mapping.components.forEach { c ->
            val info = SRGB.components.first { it.name == c.name }
            (c.value in info.min..info.max) shouldBe true
        }
        // After mapping, nothing is out of range anymore (values may be exactly zero).
        trace.step("output").components.forEach {
            (it.state == TraceComponentState.OUT_OF_RANGE) shouldBe false
        }
        // And it matches the ordinary clamp() result exactly.
        traced.color shouldBe vivid.clamp()
    }

    @Test
    fun traced_and_untraced_conversions_share_one_implementation() {
        val colors = listOf(
            Color.parse("lch(52% 58% 250)"),
            Color.parse("oklch(0.7 0.15 200 / 0.4)"),
            Color.parse("hsl(200 60% 40% / 0.25)"),
        )
        val spaces: List<ColorSpace<*>> = listOf(SRGB, DisplayP3, LCHab50)
        for (color in colors) {
            for (space in spaces) {
                val traced = ColorTracer.convert(color, space)
                val plain = color.convertTo(space)
                traced.color shouldBe plain
                traced.trace.output shouldBe plain
                traced.trace.input shouldBe color
            }
        }
    }

    @Test
    fun multi_stop_interpolator_records_build_and_interpolation_steps() {
        val handle = TracedInterpolator.interpolator(HSL) {
            stop(Color.parse("hsl(0 100% 50%)"))
            stop(Color.parse("hsl(120 100% 50%)"), 0.5f)
            stop(Color.parse("hsl(240 100% 50%)"))
        }
        val trace = handle.interpolate(0.5f).trace

        // Build-time stop baking and adjustments come before the per-call interpolation.
        trace.steps.map { it.method }.shouldContainExactly(
            "interpolation-stop",
            "interpolation-stop",
            "interpolation-stop",
            "component-adjustment",
            "component-adjustment",
            "component-adjustment",
            "interpolation",
            "output",
        )
        trace.step("interpolation").floatValue("H") shouldBe (120f plusOrMinus tol)
    }

    @Test
    fun alpha_premultiplication_is_recorded() {
        // A partially transparent HSL color premultiplies its rectangular channels before lerping.
        val a = Color.parse("hsl(100 50% 50% / 0.5)")
        val b = Color.parse("hsl(200 50% 50% / 0.5)")
        val trace = TracedInterpolator.interpolate(
            HSL, a, b, 0.5f, premultiplyAlpha = true,
        ).trace

        val stops = trace.steps("interpolation-stop")
        stops.size shouldBe 2
        // The hue (polar) is never premultiplied; S and L are scaled by the 0.5 alpha.
        val stop0 = stops[0]
        stop0.floatValue("H") shouldBe (100f plusOrMinus tol)
        stop0.floatValue("S") shouldBe (0.25f plusOrMinus tol)
        stop0.floatValue("L") shouldBe (0.25f plusOrMinus tol)
    }

    @Test
    fun rgb_conversion_round_trips_through_trace() {
        // Sanity check that a simple in-gamut color never reports out-of-range states.
        val trace = ColorTracer.convert(RGB(0.25f, 0.5f, 0.75f, 1f), SRGB).trace
        trace.steps.forEach { step ->
            step.components.forEach { c ->
                if (c.state == TraceComponentState.OUT_OF_RANGE) {
                    throw AssertionError(report(trace) + "unexpected OUT_OF_RANGE component: $c")
                }
            }
        }
    }
}
