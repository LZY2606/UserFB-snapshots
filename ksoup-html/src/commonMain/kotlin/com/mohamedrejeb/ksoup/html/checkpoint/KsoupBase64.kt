package com.mohamedrejeb.ksoup.html.checkpoint

/**
 * Minimal RFC 4648 Base64 codec (standard alphabet, padding required).
 *
 * Checkpoint serialization stores all binary fields (hash state, buffers)
 * through this codec so the whole checkpoint is plain ASCII and can be
 * persisted or moved between processes without platform dependencies.
 */
internal object KsoupBase64 {

    private val ALPHABET =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/".toCharArray()
    private val DECODE = IntArray(128) { -1 }

    init {
        ALPHABET.forEachIndexed { index, char -> DECODE[char.code] = index }
    }

    fun encode(bytes: ByteArray): String {
        if (bytes.isEmpty()) return ""
        val sb = StringBuilder((bytes.size + 2) / 3 * 4)
        var i = 0
        while (i + 2 < bytes.size) {
            val b0 = bytes[i].toInt() and 0xff
            val b1 = bytes[i + 1].toInt() and 0xff
            val b2 = bytes[i + 2].toInt() and 0xff
            sb.append(ALPHABET[b0 ushr 2])
            sb.append(ALPHABET[((b0 and 0x03) shl 4) or (b1 ushr 4)])
            sb.append(ALPHABET[((b1 and 0x0f) shl 2) or (b2 ushr 6)])
            sb.append(ALPHABET[b2 and 0x3f])
            i += 3
        }
        when (bytes.size - i) {
            1 -> {
                val b0 = bytes[i].toInt() and 0xff
                sb.append(ALPHABET[b0 ushr 2])
                sb.append(ALPHABET[(b0 and 0x03) shl 4])
                sb.append("==")
            }
            2 -> {
                val b0 = bytes[i].toInt() and 0xff
                val b1 = bytes[i + 1].toInt() and 0xff
                sb.append(ALPHABET[b0 ushr 2])
                sb.append(ALPHABET[((b0 and 0x03) shl 4) or (b1 ushr 4)])
                sb.append(ALPHABET[(b1 and 0x0f) shl 2])
                sb.append('=')
            }
        }
        return sb.toString()
    }

    fun decode(value: String): ByteArray {
        if (value.isEmpty()) return ByteArray(0)
        val padding = when {
            value.endsWith("==") -> 2
            value.endsWith("=") -> 1
            else -> 0
        }
        val out = ByteArray(value.length / 4 * 3 - padding)
        var outIndex = 0
        var i = 0
        while (i < value.length) {
            val c0 = decodeChar(value, i)
            val c1 = decodeChar(value, i + 1)
            val c2 = if (i + 2 < value.length && value[i + 2] != '=') decodeChar(value, i + 2) else 0
            val c3 = if (i + 3 < value.length && value[i + 3] != '=') decodeChar(value, i + 3) else 0

            val triple = (c0 shl 18) or (c1 shl 12) or (c2 shl 6) or c3
            if (outIndex < out.size) out[outIndex++] = (triple ushr 16).toByte()
            if (outIndex < out.size) out[outIndex++] = (triple ushr 8).toByte()
            if (outIndex < out.size) out[outIndex++] = triple.toByte()
            i += 4
        }
        return out
    }

    private fun decodeChar(value: String, index: Int): Int {
        val c = value[index].code
        require(c < DECODE.size && DECODE[c] >= 0) { "Invalid Base64 character at $index" }
        return DECODE[c]
    }
}
