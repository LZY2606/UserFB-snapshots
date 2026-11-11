/**
 * Copyright (c) LinkedIn Corporation. All rights reserved. Licensed under the BSD-2 Clause license.
 * See LICENSE in the project root for license information.
 */
package com.linkedin.dex.spec

import java.nio.ByteBuffer

class DexException(override val message: String?) : RuntimeException(message)

/**
 * Verifies that a count declared by a dex structure can possibly fit in the bytes remaining in the buffer,
 * given that each element of the structure occupies at least [minElementSize] bytes. This guards against
 * corrupt files that declare huge element counts, which would otherwise cause oversized allocations or
 * out-of-bounds reads.
 *
 * @throws DexException if the declared size cannot fit in the remaining bytes
 */
internal fun checkDeclaredSize(what: String, size: Int, byteBuffer: ByteBuffer, minElementSize: Int) {
    if (size < 0 || size.toLong() * minElementSize > byteBuffer.remaining()) {
        throw DexException("invalid declared " + what + " size: " + size + " (" + byteBuffer.remaining() + " bytes remaining)")
    }
}
