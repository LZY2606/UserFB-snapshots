package com.mohamedrejeb.ksoup.html.checkpoint

import com.mohamedrejeb.ksoup.annotation.ExperimentalKsoupApi

/**
 * Absolute source location of a token or a tree-building node.
 *
 * Offsets are measured over the whole logical input stream (the concatenation
 * of every chunk passed to the parser), independent of chunk boundaries and
 * checkpoint/resume cycles.
 *
 * - [startOffset] / [endOffset] are UTF-16 code unit offsets on the decoded
 *   character stream. [startOffset] is inclusive, [endOffset] is exclusive,
 *   so `length = endOffset - startOffset`.
 * - [startByteOffset] / [endByteOffset] are byte offsets on the original
 *   encoded byte stream when [KsoupCheckpointEncoding.UTF_8] is used (the
 *   default and the only encoding for which byte mapping is tracked). For
 *   other encodings they mirror the character offsets and should not be
 *   interpreted as byte positions.
 */
@ExperimentalKsoupApi
public data class KsoupSpan(
    val startOffset: Int,
    val endOffset: Int,
    val startByteOffset: Long,
    val endByteOffset: Long,
) {
    /** Length in UTF-16 code units. */
    public val length: Int get() = endOffset - startOffset

    public companion object {
        /** A span covering no characters, anchored at [offset]. */
        public fun empty(offset: Int, byteOffset: Long): KsoupSpan =
            KsoupSpan(offset, offset, byteOffset, byteOffset)
    }
}
