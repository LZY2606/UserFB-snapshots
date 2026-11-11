/**
 * Copyright (c) LinkedIn Corporation. All rights reserved. Licensed under the BSD-2 Clause license.
 * See LICENSE in the project root for license information.
 */
package com.linkedin.dex.spec

import java.nio.ByteBuffer

data class EncodedAnnotation(
        val typeIdx: Int,
        val size: Int,
        val elements: Array<AnnotationElement>
) {
    companion object {
        fun create(byteBuffer: ByteBuffer): EncodedAnnotation {
            val typeIdx = Leb128.readUnsignedLeb128(byteBuffer)
            val size = Leb128.readUnsignedLeb128(byteBuffer)
            // each element needs at least a name index and a value byte, so 2 bytes minimum
            checkDeclaredSize("encoded annotation elements", size, byteBuffer, 2)
            val elements = Array(size, { AnnotationElement(byteBuffer) })
            return EncodedAnnotation(typeIdx, size, elements)
        }
    }
}
