/**
 * Copyright (c) LinkedIn Corporation. All rights reserved. Licensed under the BSD-2 Clause license.
 * See LICENSE in the project root for license information.
 */
package com.linkedin.dex.spec

import java.nio.ByteBuffer

class Leb128 private constructor() {
    companion object {
        /**
         * Reads an unsigned integer from byteBuffer.
         *
         * @throws DexException if the sequence is truncated, never terminates, or does not fit in 32 bits
         */
        fun readUnsignedLeb128(byteBuffer: ByteBuffer): Int {
            var result = 0
            var current: Int
            var count = 0

            do {
                if (!byteBuffer.hasRemaining()) {
                    throw DexException("truncated LEB128 sequence at position " + byteBuffer.position())
                }
                current = byteBuffer.get().toInt() and 0xff
                result = result or (current and 0x7f shl count * 7)
                count++
            } while (current and 0x80 == 0x80 && count < 5)

            if (current and 0x80 == 0x80) {
                throw DexException("invalid LEB128 sequence")
            }

            // the fifth byte of a 32 bit value can only contribute 4 bits; anything more is an overflow
            if (count == 5 && current and 0x70 != 0) {
                throw DexException("LEB128 value overflows 32 bits")
            }

            return result
        }
    }
}
