package com.mohamedrejeb.ksoup.html.checkpoint

import com.mohamedrejeb.ksoup.annotation.ExperimentalKsoupApi

/**
 * A tree-building event emitted by the parser, annotated with the absolute
 * [span] of the node in the original stream.
 *
 * Unlike the lower level [KsoupTokenEvent], these events follow the parser's
 * tree construction rules: implied open/close tags are reported as such,
 * void elements emit a close event, and foreign (`svg`/`math`) context is
 * already reflected in the element stack carried by checkpoints.
 */
@ExperimentalKsoupApi
public sealed class KsoupNodeEvent {

    public abstract val span: KsoupSpan

    /** An opening element node, including its full tag span and resolved attributes. */
    public data class OpenElement(
        val name: String,
        val attributes: Map<String, String>,
        val isImplied: Boolean,
        override val span: KsoupSpan,
    ) : KsoupNodeEvent()

    /** A closing element node. [isImplied] marks parser-generated closes. */
    public data class CloseElement(
        val name: String,
        val isImplied: Boolean,
        override val span: KsoupSpan,
    ) : KsoupNodeEvent()

    /** A text node. Entity-decoded text and raw text share this variant. */
    public data class TextNode(
        val text: String,
        override val span: KsoupSpan,
    ) : KsoupNodeEvent()

    /** A comment node. */
    public data class CommentNode(
        val comment: String,
        override val span: KsoupSpan,
    ) : KsoupNodeEvent()

    /** A CDATA node. */
    public data class CDataNode(
        val data: String,
        override val span: KsoupSpan,
    ) : KsoupNodeEvent()

    /** A processing instruction / markup declaration node. */
    public data class ProcessingInstructionNode(
        val name: String,
        val data: String,
        override val span: KsoupSpan,
    ) : KsoupNodeEvent()

    /** A single attribute attached to the element currently being opened. */
    public data class AttributeNode(
        val name: String,
        val value: String,
        val quote: String?,
        override val span: KsoupSpan,
    ) : KsoupNodeEvent()
}
