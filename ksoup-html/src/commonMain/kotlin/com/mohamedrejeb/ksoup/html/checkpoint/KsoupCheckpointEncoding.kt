package com.mohamedrejeb.ksoup.html.checkpoint

import com.mohamedrejeb.ksoup.annotation.ExperimentalKsoupApi

/**
 * The character encoding of the byte stream the parser is consuming.
 *
 * Checkpoints bind the encoding because entity decoding, token boundaries and
 * absolute offsets are only meaningful when a resumed process re-decodes the
 * same byte prefix with the same charset. The tokenizer itself works on
 * [String], so the encoding is recorded as part of the checkpoint contract
 * rather than applied internally.
 */
@ExperimentalKsoupApi
public enum class KsoupCheckpointEncoding {
    /** UTF-8, the default for HTML5. */
    UTF_8,

    /** ISO-8859-1 / Latin-1. */
    ISO_8859_1,

    /** Windows-1252, a superset of ISO-8859-1. */
    WINDOWS_1252,

    /** UTF-16, little-endian byte order. */
    UTF_16LE,

    /** UTF-16, big-endian byte order. */
    UTF_16BE,
}
