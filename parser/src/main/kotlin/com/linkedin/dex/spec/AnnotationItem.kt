/**
 * Copyright (c) LinkedIn Corporation. All rights reserved. Licensed under the BSD-2 Clause license.
 * See LICENSE in the project root for license information.
 */
package com.linkedin.dex.spec

import com.linkedin.dex.parser.ParseUtils
import java.nio.ByteBuffer

data class AnnotationItem(
        val visibility: Byte,
        val encodedAnnotation: EncodedAnnotation
) {
    companion object {
        fun create(byteBuffer: ByteBuffer, offset: Int): AnnotationItem {
            ParseUtils.checkedPosition(byteBuffer, offset, "annotation")
            if (!byteBuffer.hasRemaining()) {
                throw DexException("truncated annotation at offset " + offset)
            }

            return AnnotationItem(
                    visibility = byteBuffer.get(),
                    encodedAnnotation = EncodedAnnotation.create(byteBuffer)
            )
        }
    }
}