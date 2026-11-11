package com.mohamedrejeb.ksoup.html.checkpoint

import com.mohamedrejeb.ksoup.annotation.ExperimentalKsoupApi
import com.mohamedrejeb.ksoup.html.parser.KsoupHtmlParser

/**
 * A single tokenizer event with its absolute [span] in the source stream.
 *
 * Every token the tokenizer can emit is represented here. The variants mirror
 * [com.mohamedrejeb.ksoup.html.tokenizer.KsoupTokenizerCallbacks] one-to-one,
 * but carry a [KsoupSpan] instead of raw relative indices, so a stream of
 * events is identical regardless of where chunk or checkpoint boundaries fall.
 */
@ExperimentalKsoupApi
public sealed class KsoupTokenEvent {

    /** Absolute location of the token in the source stream. */
    public abstract val span: KsoupSpan

    /** A run of character data. */
    public data class Text(
        val text: String,
        override val span: KsoupSpan,
    ) : KsoupTokenEvent()

    /** A decoded entity appearing in text, carrying the decoded code point. */
    public data class TextEntity(
        val codePoint: Int,
        override val span: KsoupSpan,
    ) : KsoupTokenEvent()

    /** The name part of an opening tag, e.g. `div` in `<div>`. */
    public data class OpenTagName(
        val name: String,
        override val span: KsoupSpan,
    ) : KsoupTokenEvent()

    /** End of an opening tag (the `>` position), attribute collection is complete. */
    public data class OpenTagEnd(
        override val span: KsoupSpan,
    ) : KsoupTokenEvent()

    /** A closing tag name. */
    public data class CloseTag(
        val name: String,
        override val span: KsoupSpan,
    ) : KsoupTokenEvent()

    /** A self-closing tag (`/>`). */
    public data class SelfClosingTag(
        override val span: KsoupSpan,
    ) : KsoupTokenEvent()

    /** An attribute name. */
    public data class AttribName(
        val name: String,
        override val span: KsoupSpan,
    ) : KsoupTokenEvent()

    /** A run of (still possibly entity-containing) attribute value data. */
    public data class AttribData(
        val data: String,
        override val span: KsoupSpan,
    ) : KsoupTokenEvent()

    /** A decoded entity appearing inside an attribute value. */
    public data class AttribEntity(
        val codePoint: Int,
        override val span: KsoupSpan,
    ) : KsoupTokenEvent()

    /** End of an attribute, recording the quote style. */
    public data class AttribEnd(
        val quote: KsoupHtmlParser.QuoteType,
        override val span: KsoupSpan,
    ) : KsoupTokenEvent()

    /** A markup declaration like `<!DOCTYPE html>`. */
    public data class Declaration(
        val value: String,
        override val span: KsoupSpan,
    ) : KsoupTokenEvent()

    /** A processing instruction like `<?xml ...?>`. */
    public data class ProcessingInstruction(
        val value: String,
        override val span: KsoupSpan,
    ) : KsoupTokenEvent()

    /** A comment. */
    public data class Comment(
        val comment: String,
        override val span: KsoupSpan,
    ) : KsoupTokenEvent()

    /** A CDATA section. */
    public data class CData(
        val data: String,
        override val span: KsoupSpan,
    ) : KsoupTokenEvent()
}
