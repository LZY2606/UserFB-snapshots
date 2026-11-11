/**
 * Copyright (c) LinkedIn Corporation. All rights reserved. Licensed under the BSD-2 Clause license.
 * See LICENSE in the project root for license information.
 */
package com.linkedin.dex.spec

import com.linkedin.dex.parser.ParseUtils
import java.nio.ByteBuffer

data class AnnotationsDirectoryItem(
        val classAnnotationsOff: Int,
        val fieldsSize: Int,
        val annotatedMethodsSize: Int,
        val annotatedParametersSize: Int,
        val fieldAnnotations: Array<FieldAnnotation>,
        val methodAnnotations: Array<MethodAnnotation>,
        val parameterAnnotations: Array<ParameterAnnotation>
) {
    companion object {
        fun create(byteBuffer: ByteBuffer, offset: Int): AnnotationsDirectoryItem {
            ParseUtils.checkedPosition(byteBuffer, offset, "annotations directory")
            if (byteBuffer.remaining() < 16) {
                throw DexException("truncated annotations directory at offset " + offset)
            }

            val classAnnotationsOff = byteBuffer.int
            val fieldsSize = byteBuffer.int
            val annotatedMethodsSize = byteBuffer.int
            val annotatedParametersSize = byteBuffer.int
            // each field, method and parameter annotation entry is 8 bytes
            val declaredEntriesBytes = (fieldsSize.toLong() + annotatedMethodsSize + annotatedParametersSize) * 8
            if (fieldsSize < 0 || annotatedMethodsSize < 0 || annotatedParametersSize < 0 ||
                    declaredEntriesBytes > byteBuffer.remaining()) {
                throw DexException("invalid declared annotations directory sizes: fields=" + fieldsSize +
                        ", methods=" + annotatedMethodsSize + ", parameters=" + annotatedParametersSize +
                        " (" + byteBuffer.remaining() + " bytes remaining)")
            }
            val fieldAnnotations = Array(fieldsSize, { FieldAnnotation(byteBuffer) })
            val methodAnnotations = Array(annotatedMethodsSize, { MethodAnnotation(byteBuffer) })
            val parameterAnnotations = Array(annotatedParametersSize, { ParameterAnnotation(byteBuffer) })
            return AnnotationsDirectoryItem(
                    classAnnotationsOff,
                    fieldsSize,
                    annotatedMethodsSize,
                    annotatedParametersSize,
                    fieldAnnotations,
                    methodAnnotations,
                    parameterAnnotations
            )
        }
    }
}
