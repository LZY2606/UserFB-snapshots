package com.mohamedrejeb.ksoup.html

import com.mohamedrejeb.ksoup.html.checkpoint.KsoupNodeEvent
import com.mohamedrejeb.ksoup.html.checkpoint.KsoupSpan
import com.mohamedrejeb.ksoup.html.checkpoint.KsoupTokenEvent

/**
 * Event normalizers for checkpoint tests.
 *
 * Text and attribute-data events are flushed per write, so their granularity
 * depends on chunk boundaries. A correct resume may split them differently;
 * merging adjacent runs yields a chunk-independent sequence while preserving
 * the absolute span of the merged region.
 */
internal object CheckpointNormalizers {

    fun normalizeTokens(events: List<KsoupTokenEvent>): List<KsoupTokenEvent> {
        val out = mutableListOf<KsoupTokenEvent>()
        for (event in events) {
            val last = out.lastOrNull()
            when {
                event is KsoupTokenEvent.Text && last is KsoupTokenEvent.Text ->
                    out[out.lastIndex] = KsoupTokenEvent.Text(last.text + event.text, merge(last.span, event.span))
                event is KsoupTokenEvent.AttribData && last is KsoupTokenEvent.AttribData ->
                    out[out.lastIndex] = KsoupTokenEvent.AttribData(last.data + event.data, merge(last.span, event.span))
                else -> out.add(event)
            }
        }
        return out
    }

    fun normalizeNodes(events: List<KsoupNodeEvent>): List<KsoupNodeEvent> {
        val out = mutableListOf<KsoupNodeEvent>()
        for (event in events) {
            val last = out.lastOrNull()
            when {
                event is KsoupNodeEvent.TextNode && last is KsoupNodeEvent.TextNode ->
                    out[out.lastIndex] = KsoupNodeEvent.TextNode(last.text + event.text, merge(last.span, event.span))
                else -> out.add(event)
            }
        }
        return out
    }

    private fun merge(a: KsoupSpan, b: KsoupSpan): KsoupSpan =
        KsoupSpan(a.startOffset, b.endOffset, a.startByteOffset, b.endByteOffset)

    /**
     * Ground-truth char -> UTF-8 byte map over the complete input. Byte spans
     * recorded mid-stream can only cover delivered characters, so comparisons
     * of stitched (paused + resumed) logs re-derive byte offsets here instead
     * of trusting the provisional values.
     */
    /**
     * Ground-truth char -> UTF-8 byte map over the complete input.
     *
     * A supplementary code point occupies two UTF-16 units but four UTF-8
     * bytes; both unit positions map to the code point's starting byte, and
     * the byte advance is applied after the low surrogate. Span endpoints are
     * always on code-point boundaries, so this matches the parser's model.
     */
    fun byteOffsetTable(input: String): LongArray {
        val table = LongArray(input.length + 1)
        var bytes = 0L
        var i = 0
        while (i < input.length) {
            table[i] = bytes
            val high = input[i].code
            if (high in 0xd800..0xdbff && i + 1 < input.length &&
                input[i + 1].code in 0xdc00..0xdfff
            ) {
                table[i + 1] = bytes // low surrogate starts at the same byte
                bytes += 4
                i += 2
            } else {
                bytes += utf8ByteCount(input[i]).let { if (it == 0) 3 else it }
                i++
            }
        }
        table[input.length] = bytes
        return table
    }

    private fun utf8ByteCount(c: Char): Int {
        val code = c.code
        return when {
            code < 0x80 -> 1
            code < 0x800 -> 2
            code in 0xd800..0xdbff -> 4
            code in 0xdc00..0xdfff -> 0
            else -> 3
        }
    }

    private fun KsoupSpan.ground(input: String, table: LongArray): KsoupSpan =
        KsoupSpan(
            startOffset,
            endOffset,
            table[startOffset.coerceAtMost(input.length)],
            table[endOffset.coerceAtMost(input.length)],
        )

    fun groundTokens(events: List<KsoupTokenEvent>, input: String): List<KsoupTokenEvent> =
        groundTokens(events, input, byteOffsetTable(input))

    fun groundTokens(events: List<KsoupTokenEvent>, input: String, table: LongArray): List<KsoupTokenEvent> {
        return events.map { event ->
            val span = event.span.ground(input, table)
            when (event) {
                is KsoupTokenEvent.Text -> event.copy(span = span)
                is KsoupTokenEvent.TextEntity -> event.copy(span = span)
                is KsoupTokenEvent.OpenTagName -> event.copy(span = span)
                is KsoupTokenEvent.OpenTagEnd -> event.copy(span = span)
                is KsoupTokenEvent.CloseTag -> event.copy(span = span)
                is KsoupTokenEvent.SelfClosingTag -> event.copy(span = span)
                is KsoupTokenEvent.AttribName -> event.copy(span = span)
                is KsoupTokenEvent.AttribData -> event.copy(span = span)
                is KsoupTokenEvent.AttribEntity -> event.copy(span = span)
                is KsoupTokenEvent.AttribEnd -> event.copy(span = span)
                is KsoupTokenEvent.Declaration -> event.copy(span = span)
                is KsoupTokenEvent.ProcessingInstruction -> event.copy(span = span)
                is KsoupTokenEvent.Comment -> event.copy(span = span)
                is KsoupTokenEvent.CData -> event.copy(span = span)
            }
        }
    }

    fun groundNodes(events: List<KsoupNodeEvent>, input: String): List<KsoupNodeEvent> =
        groundNodes(events, input, byteOffsetTable(input))

    fun groundNodes(events: List<KsoupNodeEvent>, input: String, table: LongArray): List<KsoupNodeEvent> {
        return events.map { event ->
            val span = event.span.ground(input, table)
            when (event) {
                is KsoupNodeEvent.OpenElement -> event.copy(span = span)
                is KsoupNodeEvent.CloseElement -> event.copy(span = span)
                is KsoupNodeEvent.TextNode -> event.copy(span = span)
                is KsoupNodeEvent.CommentNode -> event.copy(span = span)
                is KsoupNodeEvent.CDataNode -> event.copy(span = span)
                is KsoupNodeEvent.ProcessingInstructionNode -> event.copy(span = span)
                is KsoupNodeEvent.AttributeNode -> event.copy(span = span)
            }
        }
    }
}
