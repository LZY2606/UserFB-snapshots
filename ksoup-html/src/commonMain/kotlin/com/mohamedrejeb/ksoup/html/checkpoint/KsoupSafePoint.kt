package com.mohamedrejeb.ksoup.html.checkpoint

import com.mohamedrejeb.ksoup.annotation.ExperimentalKsoupApi

/**
 * Tokenizer positions at which a checkpoint is allowed.
 *
 * The list is deliberately conservative: a checkpoint is only offered on a
 * token boundary where the tokenizer has no partially consumed construct.
 * In particular, checkpoints are never offered while reading an entity, a
 * comment, a CDATA section, a `<script>`/`<style>`/`<title>`/`<textarea>`
 * raw-text region, or a half-open tag.
 *
 * When foreign content (`svg`/`math`) is open, the full element stack and
 * foreign-context flags are serialized so the resumed parser reconstructs the
 * identical insertion mode.
 */
@ExperimentalKsoupApi
public enum class KsoupSafePoint {
    /** Before any input has been consumed. */
    DOCUMENT_START,

    /** A text-data boundary: all text up to the boundary has been emitted. */
    TEXT_BOUNDARY,

    /** Immediately after an opening tag's `>` was tokenized. */
    AFTER_OPEN_TAG,

    /** Immediately after a closing tag was tokenized. */
    AFTER_CLOSE_TAG,

    /** Immediately after a self-closing tag (`/>`) was tokenized. */
    AFTER_SELF_CLOSING_TAG,

    /** Immediately after a complete comment token. */
    AFTER_COMMENT,

    /** Immediately after a complete CDATA section. */
    AFTER_CDATA,

    /** Immediately after a complete markup declaration. */
    AFTER_DECLARATION,

    /** Immediately after a complete processing instruction. */
    AFTER_PROCESSING_INSTRUCTION,

    /** After the stream was fully consumed and finalized. */
    DOCUMENT_END,
}
