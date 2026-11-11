/**
 * Copyright (c) LinkedIn Corporation. All rights reserved. Licensed under the BSD-2 Clause license.
 * See LICENSE in the project root for license information.
 */
package com.linkedin.dex.spec

import com.linkedin.dex.parser.ParseUtils
import java.nio.ByteBuffer

data class AnnotationSetItem(
        val size: Int,
        val entries: Array<AnnotationOffItem>
) {
    companion object {
        fun create(byteBuffer: ByteBuffer, offset: Int): AnnotationSetItem {
            ParseUtils.checkedPosition(byteBuffer, offset, "annotation set")
            if (byteBuffer.remaining() < 4) {
                throw DexException("truncated annotation set at offset " + offset)
            }

            val size = byteBuffer.int
            // each entry is a 4 byte annotation offset
            checkDeclaredSize("annotation set", size, byteBuffer, 4)
            val entries = Array(size, { AnnotationOffItem(byteBuffer) })
            return AnnotationSetItem(size, entries)
        }
    }
}
