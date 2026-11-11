package com.github.ajalt.colormath.trace

import com.github.ajalt.colormath.Color
import com.github.ajalt.colormath.ColorSpace

/**
 * The state of a single color component recorded in a [TraceStep].
 */
enum class TraceComponentState {
    /** The component has an ordinary numeric value. */
    DEFINED,

    /**
     * The component is not determined.
     *
     * This is used for the hue of an achromatic color, whose hue is [NaN][Float.NaN] rather
     * than `0`. It is distinct from [ZERO], which marks a value that is numerically zero.
     */
    UNDEFINED,

    /** The component's value is numerically equal to zero. */
    ZERO,

    /** The component's value lies outside its space's reference range. The unclamped value is recorded as-is. */
    OUT_OF_RANGE,
}

/**
 * A single color component recorded in a [TraceStep].
 *
 * @property name The name of the component, matching [com.github.ajalt.colormath.ColorComponentInfo.name].
 * @property value The unclamped component value. It is [NaN][Float.NaN] when [state] is
 *   [TraceComponentState.UNDEFINED].
 * @property state Whether the component is defined, undefined, zero, or out of range.
 */
data class TraceComponent(
    val name: String,
    val value: Float,
    val state: TraceComponentState,
)

/**
 * One recorded stage of a traced color conversion, in the order that the calculation was performed.
 *
 * @property method What calculation was performed in this stage, e.g. `matrix` or `chromatic-adaptation`.
 * @property detail A human-readable description of how the calculation was performed, such as the
 *   name of an adaptation matrix or an interpolation adjustment.
 * @property space The color space that the [components] are expressed in.
 * @property components The unclamped component values produced by this stage, in the same order
 *   as [ColorSpace.components].
 */
data class TraceStep(
    val method: String,
    val detail: String,
    val space: ColorSpace<*>,
    val components: List<TraceComponent>,
) {
    /** The names of the components, in the same order as [components]. */
    val componentNames: List<String> get() = components.map { it.name }

    /** The numeric component values, in the same order as [components]. */
    val values: List<Float> get() = components.map { it.value }
}

/**
 * The full record of a traced color conversion.
 *
 * @property input The parsed input color, before any conversion was performed.
 * @property output The final converted color, identical to the result of an untraced conversion.
 * @property steps Every intermediate calculation, in execution order. The first step records the
 *   input color in its original space, and the last step records the [output].
 */
data class ConversionTrace(
    val input: Color,
    val output: Color,
    val steps: List<TraceStep>,
) {
    /**
     * The names of the color spaces visited by this conversion, in execution order.
     */
    val spaces: List<ColorSpace<*>> get() = steps.map { it.space }
}
