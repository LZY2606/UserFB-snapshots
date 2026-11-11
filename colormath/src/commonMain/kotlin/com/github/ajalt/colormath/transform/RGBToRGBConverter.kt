package com.github.ajalt.colormath.transform

import com.github.ajalt.colormath.internal.Matrix
import com.github.ajalt.colormath.internal.TraceRecorder
import com.github.ajalt.colormath.internal.dot
import com.github.ajalt.colormath.model.RGB
import com.github.ajalt.colormath.model.RGBColorSpace
import com.github.ajalt.colormath.model.XYZColorSpace
import com.github.ajalt.colormath.model.chromaticAdaptationMatrix


/**
 * A converter that transforms [RGB] colors from one [RGBColorSpace] to another.
 *
 * You can also convert a color directly with [RGB.convertTo], but using a converter is more efficient
 * if you need to convert more than one color at a time.
 */
interface RGBToRGBConverter {
    fun convert(rgb: RGB): RGB
}

/**
 * Create an [RGBToRGBConverter] that transforms [RGB] colors from this [RGBColorSpace] to the [destination] space.
 */
fun RGBColorSpace.converterTo(destination: RGBColorSpace): RGBToRGBConverter {
    return RGBToRGBConverterImpl(this, destination, rgbToRgbMatrix(this, destination))
}

private class RGBToRGBConverterImpl(
    private val src: RGBColorSpace,
    private val dst: RGBColorSpace,
    private val transform: Matrix,
) : RGBToRGBConverter {
    override fun convert(rgb: RGB): RGB {
        require(rgb.space == src) { "invalid rgb space: ${rgb.space}, expected $src" }
        val fsrc = src.transferFunctions
        val fdst = dst.transferFunctions
        val lr = fsrc.eotf(rgb.r)
        val lg = fsrc.eotf(rgb.g)
        val lb = fsrc.eotf(rgb.b)
        TraceRecorder.current?.let {
            it.record(
                "transfer-function", "source RGB EOTF to linear-light",
                src, floatArrayOf(lr, lg, lb, rgb.alpha),
            )
        }
        return transform.dot(lr, lg, lb) { rr, gg, bb ->
            TraceRecorder.current?.let { recorder ->
                val detail = if (src.whitePoint == dst.whitePoint) {
                    "RGB to RGB matrix (same white point)"
                } else {
                    "RGB to RGB matrix (with chromatic adaptation ${src.whitePoint} -> ${dst.whitePoint})"
                }
                recorder.record(
                    "matrix-transform", detail, dst,
                    floatArrayOf(rr, gg, bb, rgb.alpha),
                )
            }
            dst(fdst.oetf(rr), fdst.oetf(gg), fdst.oetf(bb)).also {
                TraceRecorder.current?.record(
                    "transfer-function", "destination linear RGB OETF", it,
                )
            }
        }
    }
}

internal fun rgbToRgbMatrix(src: RGBColorSpace, dst: RGBColorSpace): Matrix {
    return if (src.whitePoint == dst.whitePoint) {
        Matrix(dst.matrixFromXyz).dot(Matrix(src.matrixToXyz))
    } else {
        val adaptation =
            XYZColorSpace(dst.whitePoint).chromaticAdaptationMatrix(src.whitePoint.chromaticity)
        Matrix(dst.matrixFromXyz).dot(adaptation).dot(Matrix(src.matrixToXyz))
    }
}
