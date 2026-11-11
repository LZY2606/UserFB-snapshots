package com.mohamedrejeb.ksoup.html.checkpoint

/**
 * Pure-Kotlin SHA-256 and UTF-8 helpers, so checkpoints carry a prefix digest
 * on every Kotlin Multiplatform target without expect/actual or platform APIs.
 *
 * [KsoupStreamingSha256] additionally exposes snapshot/restore of its internal
 * hash state, which checkpoints rely on to keep digesting after a resume
 * without re-feeding the already consumed prefix.
 */
internal object KsoupDigest {

    fun sha256Hex(input: ByteArray): String = toHex(KsoupStreamingSha256().apply { update(input) }.digest())

    fun toHex(bytes: ByteArray): String {
        val sb = StringBuilder(bytes.size * 2)
        for (b in bytes) {
            val v = b.toInt() and 0xff
            sb.append(HEX[v ushr 4])
            sb.append(HEX[v and 0x0f])
        }
        return sb.toString()
    }

    fun encodeUtf8(value: String): ByteArray {
        val out = ArrayList<Byte>(value.length)
        var i = 0
        while (i < value.length) {
            val cp = codePointAt(value, i)
            when {
                cp <= 0x7f -> out.add(cp.toByte())
                cp <= 0x7ff -> {
                    out.add((0xc0 or (cp ushr 6)).toByte())
                    out.add((0x80 or (cp and 0x3f)).toByte())
                }
                cp <= 0xffff -> {
                    out.add((0xe0 or (cp ushr 12)).toByte())
                    out.add((0x80 or ((cp ushr 6) and 0x3f)).toByte())
                    out.add((0x80 or (cp and 0x3f)).toByte())
                }
                else -> {
                    out.add((0xf0 or (cp ushr 18)).toByte())
                    out.add((0x80 or ((cp ushr 12) and 0x3f)).toByte())
                    out.add((0x80 or ((cp ushr 6) and 0x3f)).toByte())
                    out.add((0x80 or (cp and 0x3f)).toByte())
                }
            }
            i += Character.charCount(cp)
        }
        return out.toByteArray()
    }

    /** Number of UTF-8 bytes a single UTF-16 code unit contributes to the stream. */
    fun utf8ByteCount(codeUnit: Char): Int {
        val c = codeUnit.code
        return when {
            c < 0x80 -> 1
            c < 0x800 -> 2
            c in 0xd800..0xdbff -> 4 // high surrogate; the following low surrogate contributes 0
            c in 0xdc00..0xdfff -> 0
            else -> 3
        }
    }

    private fun codePointAt(value: String, index: Int): Int {
        val high = value[index].code
        if (high in 0xd800..0xdbff && index + 1 < value.length) {
            val low = value[index + 1].code
            if (low in 0xdc00..0xdfff) {
                return 0x10000 + ((high - 0xd800) shl 10) + (low - 0xdc00)
            }
        }
        return high
    }

    private val HEX = "0123456789abcdef".toCharArray()
}

/**
 * Streaming SHA-256 with serializable internal state.
 */
internal class KsoupStreamingSha256 {

    private val h = intArrayOf(
        0x6a09e667.toInt(), -0x4498517b, 0x3c6ef372.toInt(), -0x5ab00ac6,
        0x510e527f.toInt(), -0x64fa9774, 0x1f83d9ab.toInt(), 0x5be0cd19.toInt(),
    )
    private val buffer = ByteArray(64)
    private var buffered = 0
    private var totalBytes = 0L

    fun update(bytes: ByteArray) {
        totalBytes += bytes.size
        var offset = 0
        if (buffered > 0) {
            val take = minOf(64 - buffered, bytes.size)
            bytes.copyInto(buffer, buffered, 0, take)
            buffered += take
            offset = take
            if (buffered == 64) {
                processBlock(buffer, 0)
                buffered = 0
            }
        }
        while (offset + 64 <= bytes.size) {
            processBlock(bytes, offset)
            offset += 64
        }
        if (offset < bytes.size) {
            bytes.copyInto(buffer, 0, offset, bytes.size)
            buffered = bytes.size - offset
        }
    }

    fun digest(): ByteArray {
        val bitLength = totalBytes * 8L
        buffer[buffered] = 0x80.toByte()
        for (i in buffered + 1 until 64) buffer[i] = 0
        if (buffered >= 56) {
            processBlock(buffer, 0)
            for (i in 0 until 56) buffer[i] = 0
        }
        for (i in 0 until 8) {
            buffer[63 - i] = (bitLength ushr (i * 8)).toByte()
        }
        processBlock(buffer, 0)

        val out = ByteArray(32)
        for (i in 0 until 8) {
            out[i * 4] = (h[i] ushr 24).toByte()
            out[i * 4 + 1] = (h[i] ushr 16).toByte()
            out[i * 4 + 2] = (h[i] ushr 8).toByte()
            out[i * 4 + 3] = h[i].toByte()
        }
        return out
    }

    fun snapshot(): State = State(h.copyOf(), buffer.copyOf(), buffered, totalBytes)

    fun restore(state: State) {
        state.h.copyInto(h)
        state.buffer.copyInto(buffer)
        buffered = state.buffered
        totalBytes = state.totalBytes
    }

    data class State(
        val h: IntArray,
        val buffer: ByteArray,
        val buffered: Int,
        val totalBytes: Long,
    ) {
        override fun equals(other: Any?): Boolean {
            if (this === other) return true
            if (other !is State) return false
            return h.contentEquals(other.h) && buffer.contentEquals(other.buffer) &&
                buffered == other.buffered && totalBytes == other.totalBytes
        }

        override fun hashCode(): Int {
            var result = h.contentHashCode()
            result = 31 * result + buffer.contentHashCode()
            result = 31 * result + buffered
            result = 31 * result + totalBytes.hashCode()
            return result
        }
    }

    private fun processBlock(data: ByteArray, base: Int) {
        val w = IntArray(64)
        for (i in 0 until 16) {
            val j = base + i * 4
            w[i] = ((data[j].toInt() and 0xff) shl 24) or
                ((data[j + 1].toInt() and 0xff) shl 16) or
                ((data[j + 2].toInt() and 0xff) shl 8) or
                (data[j + 3].toInt() and 0xff)
        }
        for (i in 16 until 64) {
            val s0 = rotr(w[i - 15], 7) xor rotr(w[i - 15], 18) xor (w[i - 15] ushr 3)
            val s1 = rotr(w[i - 2], 17) xor rotr(w[i - 2], 19) xor (w[i - 2] ushr 10)
            w[i] = w[i - 16] + s0 + w[i - 7] + s1
        }

        var a = h[0]; var b = h[1]; var c = h[2]; var d = h[3]
        var e = h[4]; var f = h[5]; var g = h[6]; var hh = h[7]

        for (i in 0 until 64) {
            val s1 = rotr(e, 6) xor rotr(e, 11) xor rotr(e, 25)
            val ch = (e and f) xor (e.inv() and g)
            val temp1 = hh + s1 + ch + K[i] + w[i]
            val s0 = rotr(a, 2) xor rotr(a, 13) xor rotr(a, 22)
            val maj = (a and b) xor (a and c) xor (b and c)
            val temp2 = s0 + maj
            hh = g; g = f; f = e; e = d + temp1
            d = c; c = b; b = a; a = temp1 + temp2
        }

        h[0] += a; h[1] += b; h[2] += c; h[3] += d
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh
    }

    private fun rotr(x: Int, n: Int): Int = (x ushr n) or (x shl (32 - n))

    private companion object {
        val K = intArrayOf(
            0x428a2f98.toInt(), 0x71374491.toInt(), 0xb5c0fbcf.toInt(), 0xe9b5dba5.toInt(),
            0x3956c25b.toInt(), 0x59f111f1.toInt(), 0x923f82a4.toInt(), 0xab1c5ed5.toInt(),
            0xd807aa98.toInt(), 0x12835b01.toInt(), 0x243185be.toInt(), 0x550c7dc3.toInt(),
            0x72be5d74.toInt(), 0x80deb1fe.toInt(), 0x9bdc06a7.toInt(), 0xc19bf174.toInt(),
            0xe49b69c1.toInt(), 0xefbe4786.toInt(), 0x0fc19dc6.toInt(), 0x240ca1cc.toInt(),
            0x2de92c6f.toInt(), 0x4a7484aa.toInt(), 0x5cb0a9dc.toInt(), 0x76f988da.toInt(),
            0x983e5152.toInt(), 0xa831c66d.toInt(), 0xb00327c8.toInt(), 0xbf597fc7.toInt(),
            0xc6e00bf3.toInt(), 0xd5a79147.toInt(), 0x06ca6351.toInt(), 0x14292967.toInt(),
            0x27b70a85.toInt(), 0x2e1b2138.toInt(), 0x4d2c6dfc.toInt(), 0x53380d13.toInt(),
            0x650a7354.toInt(), 0x766a0abb.toInt(), 0x81c2c92e.toInt(), 0x92722c85.toInt(),
            0xa2bfe8a1.toInt(), 0xa81a664b.toInt(), 0xc24b8b70.toInt(), 0xc76c51a3.toInt(),
            0xd192e819.toInt(), 0xd6990624.toInt(), 0xf40e3585.toInt(), 0x106aa070.toInt(),
            0x19a4c116.toInt(), 0x1e376c08.toInt(), 0x2748774c.toInt(), 0x34b0bcb5.toInt(),
            0x391c0cb3.toInt(), 0x4ed8aa4a.toInt(), 0x5b9cca4f.toInt(), 0x682e6ff3.toInt(),
            0x748f82ee.toInt(), 0x78a5636f.toInt(), 0x84c87814.toInt(), 0x8cc70208.toInt(),
            0x90befffa.toInt(), 0xa4506ceb.toInt(), 0xbef9a3f7.toInt(), 0xc67178f2.toInt(),
        )
    }
}
