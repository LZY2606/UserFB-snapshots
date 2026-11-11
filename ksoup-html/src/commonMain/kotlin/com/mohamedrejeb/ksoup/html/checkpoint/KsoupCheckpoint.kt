package com.mohamedrejeb.ksoup.html.checkpoint

import com.mohamedrejeb.ksoup.annotation.ExperimentalKsoupApi
import com.mohamedrejeb.ksoup.html.parser.KsoupHtmlOptions

/**
 * A serializable, versioned snapshot of a partially completed parse.
 *
 * Instances are produced by
 * [com.mohamedrejeb.ksoup.html.parser.KsoupHtmlParser.checkpoint] and consumed
 * by [com.mohamedrejeb.ksoup.html.parser.KsoupHtmlParser.resumeFromCheckpoint],
 * potentially in another process.
 *
 * # Schema
 *
 * The wire format produced by [encode] / parsed by [decode] is line oriented
 * ASCII and starts with the magic header `KSOUP-CP-<version>`:
 *
 * ```
 * KSOUP-CP-1
 * key=value
 * ...
 * ```
 *
 * String and binary values are Base64 encoded (see [KsoupBase64]); numeric
 * and boolean values are plain text. The fields are:
 *
 * | key | meaning |
 * |---|---|
 * | `safePoint` | [KsoupSafePoint] at which the snapshot was taken |
 * | `boundary` | absolute char offset of the first unconsumed character |
 * | `consumedBytes` | UTF-8 byte count of the consumed prefix |
 * | `encoding` | [KsoupCheckpointEncoding] name |
 * | `options` | the six [KsoupHtmlOptions] flags, in declaration order |
 * | `prefixDigest` | hex SHA-256 of the UTF-8 bytes of the consumed prefix |
 * | `hashState` | Base64 snapshot of the streaming SHA-256 internal state |
 * | `suffix` | Base64 chars already received but not yet tokenized at the boundary |
 * | `tokenizerState`, `tokenizerBaseState` | tokenizer state names |
 * | `parser.*` | parser indices, open tag/attribute scratch fields, element stack and foreign context |
 *
 * # Compatibility
 *
 * [FORMAT_VERSION] is the only version this build can read or write. A
 * checkpoint with a different magic/version is rejected with
 * [KsoupCheckpointException]; there is no silent migration. Callbacks and
 * object references are never part of the schema — only plain data.
 */
@ExperimentalKsoupApi
public class KsoupCheckpoint internal constructor(
    public val safePoint: KsoupSafePoint,
    public val boundary: Int,
    public val consumedBytes: Long,
    public val encoding: KsoupCheckpointEncoding,
    public val options: KsoupHtmlOptions,
    public val prefixDigest: String,
    internal val hashState: KsoupStreamingSha256.State,
    /**
     * Characters already delivered to the original parser but not yet
     * tokenized when the checkpoint was taken. They are replayed internally
     * on restore, so the next write must begin at [nextFeedOffset].
     */
    public val suffix: String,
    internal val tokenizerStateName: String,
    internal val tokenizerBaseStateName: String,
    internal val parserState: ParserState,
) {

    /**
     * Absolute char offset at which the caller must resume feeding the stream
     * after restoring this checkpoint. Equals [boundary] + [suffix].length.
     */
    public val nextFeedOffset: Int get() = boundary + suffix.length

    public companion object {
        /** Wire-format version. Bumped on any incompatible schema change. */
        public const val FORMAT_VERSION: Int = 1

        internal const val MAGIC: String = "KSOUP-CP-"

        public fun encode(checkpoint: KsoupCheckpoint): String {
            val sb = StringBuilder()
            sb.append(MAGIC).append(FORMAT_VERSION).append('\n')
            val p = checkpoint.parserState
            sb.append("safePoint=").append(checkpoint.safePoint.name).append('\n')
            sb.append("boundary=").append(checkpoint.boundary).append('\n')
            sb.append("consumedBytes=").append(checkpoint.consumedBytes).append('\n')
            sb.append("encoding=").append(checkpoint.encoding.name).append('\n')
            sb.append("options=")
                .append(checkpoint.options.xmlMode).append(',')
                .append(checkpoint.options.decodeEntities).append(',')
                .append(checkpoint.options.lowerCaseTags).append(',')
                .append(checkpoint.options.lowerCaseAttributeNames).append(',')
                .append(checkpoint.options.recognizeCDATA).append(',')
                .append(checkpoint.options.recognizeSelfClosing).append('\n')
            sb.append("prefixDigest=").append(checkpoint.prefixDigest).append('\n')
            sb.append("hashState=").append(encodeHashState(checkpoint.hashState)).append('\n')
            sb.append("suffix=").append(KsoupBase64.encode(KsoupDigest.encodeUtf8(checkpoint.suffix))).append('\n')
            sb.append("tokenizerState=").append(checkpoint.tokenizerStateName).append('\n')
            sb.append("tokenizerBaseState=").append(checkpoint.tokenizerBaseStateName).append('\n')
            sb.append("parser.startIndex=").append(p.startIndex).append('\n')
            sb.append("parser.endIndex=").append(p.endIndex).append('\n')
            sb.append("parser.openTagStart=").append(p.openTagStart).append('\n')
            sb.append("parser.tagName=").append(b64(p.tagName)).append('\n')
            sb.append("parser.attribName=").append(b64(p.attribName)).append('\n')
            sb.append("parser.attribValue=").append(b64(p.attribValue)).append('\n')
            sb.append("parser.hasAttribs=").append(p.attribs != null).append('\n')
            sb.append("parser.attribs=").append(encodeAttribs(p.attribs)).append('\n')
            sb.append("parser.stack=").append(encodeStringList(p.stack)).append('\n')
            sb.append("parser.foreignContext=").append(encodeBoolList(p.foreignContext)).append('\n')
            sb.append("parser.ended=").append(p.ended).append('\n')
            sb.append("parser.currentTagNameStart=").append(p.currentTagNameStart).append('\n')
            sb.append("parser.attribNameStart=").append(p.attribNameStart).append('\n')
            sb.append("parser.lastAttribEnd=").append(p.lastAttribEnd).append('\n')
            return sb.toString()
        }

        public fun decode(text: String): KsoupCheckpoint {
            val lines = text.split('\n').filter { it.isNotEmpty() }
            require(lines.isNotEmpty()) { "Empty checkpoint" }
            val header = lines.first()
            if (!header.startsWith(MAGIC)) {
                throw KsoupCheckpointException("Not a Ksoup checkpoint (bad magic): $header")
            }
            val version = header.removePrefix(MAGIC).toIntOrNull()
                ?: throw KsoupCheckpointException("Malformed checkpoint header: $header")
            if (version != FORMAT_VERSION) {
                throw KsoupCheckpointException(
                    "Incompatible checkpoint version $version; this build supports only $FORMAT_VERSION"
                )
            }

            val map = HashMap<String, String>()
            for (line in lines.drop(1)) {
                val eq = line.indexOf('=')
                if (eq <= 0) throw KsoupCheckpointException("Malformed checkpoint line: $line")
                map[line.substring(0, eq)] = line.substring(eq + 1)
            }

            fun required(key: String) = map[key] ?: throw KsoupCheckpointException("Missing field: $key")

            val optionParts = required("options").split(',').map { it.toBooleanStrict() }
            if (optionParts.size != 6) {
                throw KsoupCheckpointException("Corrupt options field")
            }
            val options = KsoupHtmlOptions(
                xmlMode = optionParts[0],
                decodeEntities = optionParts[1],
                lowerCaseTags = optionParts[2],
                lowerCaseAttributeNames = optionParts[3],
                recognizeCDATA = optionParts[4],
                recognizeSelfClosing = optionParts[5],
            )

            val hasAttribs = required("parser.hasAttribs").toBooleanStrict()
            val parserState = ParserState(
                startIndex = required("parser.startIndex").toInt(),
                endIndex = required("parser.endIndex").toInt(),
                openTagStart = required("parser.openTagStart").toInt(),
                tagName = unb64(required("parser.tagName")),
                attribName = unb64(required("parser.attribName")),
                attribValue = unb64(required("parser.attribValue")),
                attribs = if (hasAttribs) decodeAttribs(required("parser.attribs")) else null,
                stack = decodeStringList(required("parser.stack")),
                foreignContext = decodeBoolList(required("parser.foreignContext")),
                ended = required("parser.ended").toBooleanStrict(),
                currentTagNameStart = required("parser.currentTagNameStart").toInt(),
                attribNameStart = required("parser.attribNameStart").toInt(),
                lastAttribEnd = required("parser.lastAttribEnd").toInt(),
            )

            return KsoupCheckpoint(
                safePoint = runCatching { KsoupSafePoint.valueOf(required("safePoint")) }
                    .getOrElse { throw KsoupCheckpointException("Unknown safe point: ${map["safePoint"]}") },
                boundary = required("boundary").toInt(),
                consumedBytes = required("consumedBytes").toLong(),
                encoding = runCatching { KsoupCheckpointEncoding.valueOf(required("encoding")) }
                    .getOrElse { throw KsoupCheckpointException("Unknown encoding: ${map["encoding"]}") },
                options = options,
                prefixDigest = required("prefixDigest"),
                hashState = decodeHashState(required("hashState")),
                suffix = decodeUtf8(required("suffix")),
                tokenizerStateName = required("tokenizerState"),
                tokenizerBaseStateName = required("tokenizerBaseState"),
                parserState = parserState,
            )
        }

        private fun b64(value: String): String = KsoupBase64.encode(KsoupDigest.encodeUtf8(value))
        private fun unb64(value: String): String = decodeUtf8(value)

        private fun decodeUtf8(value: String): String {
            val bytes = KsoupBase64.decode(value)
            if (bytes.isEmpty()) return ""
            val sb = StringBuilder()
            var i = 0
            while (i < bytes.size) {
                val b0 = bytes[i].toInt() and 0xff
                val cp: Int
                val len: Int
                when {
                    b0 < 0x80 -> { cp = b0; len = 1 }
                    b0 ushr 5 == 0b110 -> {
                        cp = ((b0 and 0x1f) shl 6) or (bytes[i + 1].toInt() and 0x3f); len = 2
                    }
                    b0 ushr 4 == 0b1110 -> {
                        cp = ((b0 and 0x0f) shl 12) or
                            ((bytes[i + 1].toInt() and 0x3f) shl 6) or
                            (bytes[i + 2].toInt() and 0x3f); len = 3
                    }
                    else -> {
                        cp = ((b0 and 0x07) shl 18) or
                            ((bytes[i + 1].toInt() and 0x3f) shl 12) or
                            ((bytes[i + 2].toInt() and 0x3f) shl 6) or
                            (bytes[i + 3].toInt() and 0x3f); len = 4
                    }
                }
                sb.appendCompat(cp)
                i += len
            }
            return sb.toString()
        }

        private fun StringBuilder.appendCompat(codePoint: Int) {
            if (codePoint <= 0xffff) {
                append(codePoint.toChar())
            } else {
                val adjusted = codePoint - 0x10000
                append((0xd800 + (adjusted ushr 10)).toChar())
                append((0xdc00 + (adjusted and 0x3ff)).toChar())
            }
        }

        private fun encodeStringList(list: List<String>): String =
            list.joinToString(separator = ";") { b64(it) }

        private fun decodeStringList(value: String): MutableList<String> =
            if (value.isEmpty()) mutableListOf() else value.split(';').map { unb64(it) }.toMutableList()

        private fun encodeBoolList(list: List<Boolean>): String =
            list.joinToString(separator = ",") { it.toString() }

        private fun decodeBoolList(value: String): MutableList<Boolean> =
            if (value.isEmpty()) mutableListOf() else value.split(',').map { it.toBooleanStrict() }.toMutableList()

        private fun encodeAttribs(attribs: Map<String, String>?): String {
            if (attribs == null) return ""
            return attribs.entries.joinToString(separator = ";") { "${b64(it.key)}:${b64(it.value)}" }
        }

        private fun decodeAttribs(value: String): MutableMap<String, String> {
            val map = LinkedHashMap<String, String>()
            if (value.isEmpty()) return map
            value.split(';').forEach { entry ->
                val colon = entry.indexOf(':')
                map[unb64(entry.substring(0, colon))] = unb64(entry.substring(colon + 1))
            }
            return map
        }

        private fun encodeHashState(state: KsoupStreamingSha256.State): String {
            val out = KsoupBinaryWriter(8 * 4 + 64 + 4 + 8)
            state.h.forEach { out.writeIntBE(it) }
            out.writeBytes(state.buffer)
            out.writeIntBE(state.buffered)
            out.writeLongBE(state.totalBytes)
            return KsoupBase64.encode(out.toByteArray())
        }

        private fun decodeHashState(value: String): KsoupStreamingSha256.State {
            val bytes = KsoupBase64.decode(value)
            if (bytes.size != 32 + 64 + 4 + 8) {
                throw KsoupCheckpointException("Corrupt hash state (length=${bytes.size})")
            }
            val h = IntArray(8) { readIntBE(bytes, it * 4) }
            val buffer = bytes.copyOfRange(32, 96)
            val buffered = readIntBE(bytes, 96)
            val totalBytes = readLongBE(bytes, 100)
            return KsoupStreamingSha256.State(h, buffer, buffered, totalBytes)
        }

        private fun readIntBE(bytes: ByteArray, offset: Int): Int =
            ((bytes[offset].toInt() and 0xff) shl 24) or
                ((bytes[offset + 1].toInt() and 0xff) shl 16) or
                ((bytes[offset + 2].toInt() and 0xff) shl 8) or
                (bytes[offset + 3].toInt() and 0xff)

        private fun readLongBE(bytes: ByteArray, offset: Int): Long {
            var result = 0L
            for (i in 0 until 8) {
                result = (result shl 8) or (bytes[offset + i].toLong() and 0xff)
            }
            return result
        }
    }
}

/** Pure-Kotlin big-endian byte writer (no JVM-only stream types). */
internal class KsoupBinaryWriter(initialCapacity: Int = 32) {
    private var data = ByteArray(initialCapacity)
    private var count = 0

    fun write(byte: Int) {
        ensureCapacity(count + 1)
        data[count++] = byte.toByte()
    }

    fun writeBytes(bytes: ByteArray) {
        ensureCapacity(count + bytes.size)
        bytes.copyInto(data, count)
        count += bytes.size
    }

    fun writeIntBE(value: Int) {
        write(value ushr 24); write(value ushr 16); write(value ushr 8); write(value)
    }

    fun writeLongBE(value: Long) {
        for (shift in 56 downTo 0 step 8) write((value ushr shift).toInt())
    }

    fun toByteArray(): ByteArray = data.copyOfRange(0, count)

    private fun ensureCapacity(required: Int) {
        if (required <= data.size) return
        var newSize = data.size * 2
        if (newSize < required) newSize = required
        data = data.copyOf(newSize)
    }
}

/**
 * Serializable parser-side state. Mirrors the mutable scratch fields and
 * element stack of [com.mohamedrejeb.ksoup.html.parser.KsoupHtmlParser].
 */
internal class ParserState(
    val startIndex: Int,
    val endIndex: Int,
    val openTagStart: Int,
    val tagName: String,
    val attribName: String,
    val attribValue: String,
    val attribs: MutableMap<String, String>?,
    val stack: MutableList<String>,
    val foreignContext: MutableList<Boolean>,
    val ended: Boolean,
    // Absolute source-position anchors used to build node spans. These must be
    // carried over so implied elements emitted after a resume keep correct spans.
    val currentTagNameStart: Int,
    val attribNameStart: Int,
    val lastAttribEnd: Int,
)
