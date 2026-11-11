package com.mohamedrejeb.ksoup.html.parser

import com.mohamedrejeb.ksoup.annotation.ExperimentalKsoupApi
import com.mohamedrejeb.ksoup.html.checkpoint.KsoupBase64
import com.mohamedrejeb.ksoup.html.checkpoint.KsoupCheckpoint
import com.mohamedrejeb.ksoup.html.checkpoint.KsoupCheckpointEncoding
import com.mohamedrejeb.ksoup.html.checkpoint.KsoupCheckpointException
import com.mohamedrejeb.ksoup.html.checkpoint.KsoupDigest
import com.mohamedrejeb.ksoup.html.checkpoint.KsoupNodeEvent
import com.mohamedrejeb.ksoup.html.checkpoint.KsoupSafePoint
import com.mohamedrejeb.ksoup.html.checkpoint.KsoupSafePointHit
import com.mohamedrejeb.ksoup.html.checkpoint.KsoupSpan
import com.mohamedrejeb.ksoup.html.checkpoint.KsoupStreamingSha256
import com.mohamedrejeb.ksoup.html.checkpoint.KsoupTokenEvent
import com.mohamedrejeb.ksoup.html.checkpoint.ParserState
import com.mohamedrejeb.ksoup.html.tokenizer.KsoupTokenizer
import com.mohamedrejeb.ksoup.html.tokenizer.KsoupTokenizerCallbacks

public class KsoupHtmlParser @ExperimentalKsoupApi constructor(
    public val handler: KsoupHtmlHandler = KsoupHtmlHandler.Default,
    public val options: KsoupHtmlOptions = KsoupHtmlOptions.Default,
    public val callbacks: KsoupTokenizerCallbacks = KsoupTokenizerCallbacks.Default,
    /**
     * Enables absolute source spans on [tokenEventLog]/[nodeEventLog] and the
     * checkpoint API. Off by default so existing users pay no overhead.
     */
    @property:ExperimentalKsoupApi
    public val checkpointEnabled: Boolean = false,
    /**
     * Encoding of the original byte stream. Bound into every checkpoint and
     * used for the UTF-8 byte offsets reported in [KsoupSpan].
     */
    @property:ExperimentalKsoupApi
    public val encoding: KsoupCheckpointEncoding = KsoupCheckpointEncoding.UTF_8,
    /**
     * When true, the tokenizer pauses itself at every safe point. Pair with
     * [resume] to walk the stream boundary by boundary (mainly useful for
     * tests and for choosing where to take checkpoints).
     */
    @property:ExperimentalKsoupApi
    public val pauseAtSafePoints: Boolean = false,
) {

    public constructor(
        handler: KsoupHtmlHandler = KsoupHtmlHandler.Default,
        options: KsoupHtmlOptions = KsoupHtmlOptions.Default,
    ): this(
        handler = handler,
        options = options,
        callbacks = KsoupTokenizerCallbacks.Default,
    )

    /** The start index of the last event. */
    private var startIndex: Int = 0
    /** The end index of the last event. */
    private var endIndex: Int = 0
    /**
     * Store the start index of the current open tag,
     * so we can update the start index for attributes.
     */
    private var openTagStart = 0

    private var tagName = ""
    private var attribName = ""
    private var attribValue = ""
    private var attribs: MutableMap<String, String>? = null
    private val stack = mutableListOf<String>()
    private val foreignContext = mutableListOf<Boolean>()

    private val buffers = mutableListOf<String>()
    private var bufferOffset = 0
    /** The index of the last written buffer. Used when resuming after a `pause()`. */
    private var writeIndex = 0
    /** Indicates whether the parser has finished running / `.end` has been called. */
    private var ended = false

    // region Span / checkpoint bookkeeping

    /** Tokenizer events with absolute spans; populated only when [checkpointEnabled]. */
    @ExperimentalKsoupApi
    public val tokenEventLog: MutableList<KsoupTokenEvent> = mutableListOf()

    /** Tree-building events with absolute spans; populated only when [checkpointEnabled]. */
    @ExperimentalKsoupApi
    public val nodeEventLog: MutableList<KsoupNodeEvent> = mutableListOf()

    /** Every safe point reached, in order; populated only when [checkpointEnabled]. */
    @ExperimentalKsoupApi
    public val safePointHits: MutableList<KsoupSafePointHit> = mutableListOf()

    private class ChunkMeta(
        val charStart: Int,
        val byteStart: Long,
        val text: String,
    )

    /** Every chunk ever written, with its absolute char/byte base. Never shifted. */
    private val chunkMeta = mutableListOf<ChunkMeta>()
    private var totalChars = 0
    private var totalBytes = 0L

    private val sha256 = KsoupStreamingSha256()
    /** Up to which absolute char offset [sha256] has been fed. */
    private var digestCharPos = 0

    private var lastSafePoint: KsoupSafePoint? = KsoupSafePoint.DOCUMENT_START
    private var lastSafePointBoundary: Int = 0
    private var attribNameStart = 0
    private var currentTagNameStart = -1
    private var lastAttribEnd = 0
    private var lastTextSpan: KsoupSpan? = null
    private var lastCommentSpan: KsoupSpan? = null
    private var lastPiSpan: KsoupSpan? = null
    /**
     * Chars already received when a checkpoint was captured but not yet fed to
     * the tokenizer after restore. Prepended to the next [write].
     */
    private var pendingSuffix = ""

    private fun record(event: KsoupTokenEvent) {
        if (checkpointEnabled) tokenEventLog.add(event)
    }

    private fun recordNode(event: KsoupNodeEvent) {
        if (checkpointEnabled) nodeEventLog.add(event)
    }

    /**
     * Maps an absolute char range to UTF-8 byte offsets in the original
     * stream. Only called while the covering chunks are still held, so the
     * work per event is proportional to the token's own length.
     */
    private fun spanOf(start: Int, end: Int): KsoupSpan {
        if (!checkpointEnabled || encoding != KsoupCheckpointEncoding.UTF_8) {
            return KsoupSpan(start, end, start.toLong(), end.toLong())
        }
        // Map char positions to byte boundaries. byteOffsetAt(p) only ever
        // reads characters before p, so a half-open end one position past the
        // last registered chunk resolves cleanly to the running byte total.
        return KsoupSpan(start, end, byteOffsetAt(start), byteOffsetAt(end))
    }

    /**
     * Byte offset of char position [position], i.e. bytes of the characters
     * before it. Works whether chunk bookkeeping starts at 0 (fresh parse) or
     * at a checkpoint boundary (restored parse). A streaming code-point
     * counter carries a pending high surrogate across chunk boundaries, so a
     * surrogate pair split by a chunk still contributes exactly four bytes.
     */
    private fun byteOffsetAt(position: Int): Long {
        var bytes = 0L
        var pendingHighSurrogate = false
        var charPos = 0
        outer@ for (meta in this.chunkMeta) {
            for (i in meta.text.indices) {
                if (charPos >= position) break@outer
                val c = meta.text[i].code
                when {
                    c in 0xd800..0xdbff -> {
                        // High surrogate: defer its bytes until the next unit.
                        pendingHighSurrogate = true
                    }
                    c in 0xdc00..0xdfff && pendingHighSurrogate -> {
                        // Completes a pair: the two units together are four bytes.
                        bytes += 4
                        pendingHighSurrogate = false
                    }
                    else -> {
                        if (pendingHighSurrogate) {
                            // Lone high surrogate (malformed): count its 3 replacement bytes.
                            bytes += 3
                            pendingHighSurrogate = false
                        }
                        bytes += KsoupDigest.utf8ByteCount(meta.text[i])
                    }
                }
                charPos++
            }
        }
        if (pendingHighSurrogate && charPos < position) {
            bytes += 3
        }
        return bytes
    }



    // endregion

    private val lowerCaseTagNames get() = options.lowerCaseTags
    private val lowerCaseAttributeNames get() = options.lowerCaseAttributeNames

    // Tokenizer events
    @OptIn(ExperimentalKsoupApi::class)
    private val ksoupTokenizerCallbacks = object : KsoupTokenizerCallbacks {

        override fun onText(start: Int, endIndex: Int) {
            this@KsoupHtmlParser.callbacks.onText(start, endIndex)
            val data = this@KsoupHtmlParser.getSlice(start, endIndex)
            val span = spanOf(start, endIndex)
            lastTextSpan = span
            record(KsoupTokenEvent.Text(data, span))
            this@KsoupHtmlParser.endIndex = endIndex - 1
            this@KsoupHtmlParser.effectiveHandler().onText(data)
            this@KsoupHtmlParser.startIndex = endIndex
        }

        override fun onTextEntity(codepoint: Int, start: Int, endIndex: Int) {
            this@KsoupHtmlParser.callbacks.onTextEntity(codepoint, start, endIndex)
            val span = spanOf(start, endIndex)
            lastTextSpan = span
            record(KsoupTokenEvent.TextEntity(codepoint, span))
            this@KsoupHtmlParser.endIndex = endIndex - 1
            this@KsoupHtmlParser.effectiveHandler().onText(Char(codepoint).toString())
            this@KsoupHtmlParser.startIndex = endIndex
        }

        private fun isVoidElement(name: String): Boolean {
            return !this@KsoupHtmlParser.options.xmlMode && name in voidElements
        }

        override fun onOpenTagName(start: Int, endIndex: Int) {
            this@KsoupHtmlParser.callbacks.onOpenTagName(start, endIndex)
            this@KsoupHtmlParser.endIndex = endIndex

            var name = this@KsoupHtmlParser.getSlice(start, endIndex)

            if (this@KsoupHtmlParser.lowerCaseTagNames) {
                name = name.lowercase()
            }

            record(KsoupTokenEvent.OpenTagName(name, spanOf(start, endIndex)))
            this@KsoupHtmlParser.currentTagNameStart = start
            this.emitOpenTag(name)
        }

        private fun emitOpenTag(name: String) {
            this@KsoupHtmlParser.openTagStart = this@KsoupHtmlParser.startIndex
            this@KsoupHtmlParser.tagName = name

            val impliesClose = openImpliesClose[name]

            if (!this@KsoupHtmlParser.options.xmlMode && impliesClose != null) {
                while (
                    this@KsoupHtmlParser.stack.isNotEmpty() &&
                    impliesClose.contains(this@KsoupHtmlParser.stack.last())
                ) {
                    val element = this@KsoupHtmlParser.stack.removeLast()

                    this@KsoupHtmlParser.effectiveHandler().onCloseTag(element, true)
                }
            }
            if (!this.isVoidElement(name)) {
                this@KsoupHtmlParser.stack.add(name)

                if (name in foreignContextElements) {
                    this@KsoupHtmlParser.foreignContext.add(true)
                } else if (name in htmlIntegrationElements) {
                    this@KsoupHtmlParser.foreignContext.add(false)
                }
            }
            this@KsoupHtmlParser.effectiveHandler().onOpenTagName(name)
            this@KsoupHtmlParser.attribs = mutableMapOf()
        }

        private fun endOpenTag(isImplied: Boolean) {
            this@KsoupHtmlParser.startIndex = this@KsoupHtmlParser.openTagStart

            this@KsoupHtmlParser.attribs?.let {
                this@KsoupHtmlParser.effectiveHandler().onOpenTag(this@KsoupHtmlParser.tagName, it, isImplied)
                this@KsoupHtmlParser.attribs = null
            }
            if (this.isVoidElement(this@KsoupHtmlParser.tagName)) {
                this@KsoupHtmlParser.effectiveHandler().onCloseTag(this@KsoupHtmlParser.tagName, true)
            }

            this@KsoupHtmlParser.tagName = ""
        }

        override fun onOpenTagEnd(endIndex: Int) {
            this@KsoupHtmlParser.callbacks.onOpenTagEnd(endIndex)
            this@KsoupHtmlParser.endIndex = endIndex
            val span = spanOf(endIndex, endIndex + 1)
            record(KsoupTokenEvent.OpenTagEnd(span))
            this.endOpenTag(false)

            // Set the start index for the next node
            this@KsoupHtmlParser.startIndex = endIndex + 1
        }

        override fun onCloseTag(start: Int, endIndex: Int) {
            this@KsoupHtmlParser.callbacks.onCloseTag(start, endIndex)
            this@KsoupHtmlParser.endIndex = endIndex

            var name = this@KsoupHtmlParser.getSlice(start, endIndex)

            if (this@KsoupHtmlParser.lowerCaseTagNames) {
                name = name.lowercase()
            }

            record(KsoupTokenEvent.CloseTag(name, spanOf(start, endIndex)))

            if (
                name in foreignContextElements &&
                name in htmlIntegrationElements
            ) {
                this@KsoupHtmlParser.foreignContext.removeLast()
            }

            if (!this.isVoidElement(name)) {
                val pos = this@KsoupHtmlParser.stack.lastIndexOf(name)

                if (pos != -1) {
                    var count = this@KsoupHtmlParser.stack.size - pos
                    while (count-- > 0) {
                        val element = this@KsoupHtmlParser.stack.removeLast()
                        this@KsoupHtmlParser.effectiveHandler().onCloseTag(element, count != 0)
                    }
                } else if (!this@KsoupHtmlParser.options.xmlMode && name == "p") {
                    // Implicit open before close
                    this.emitOpenTag("p")
                    this.closeCurrentTag(true)
                }
            } else if (!this@KsoupHtmlParser.options.xmlMode && name == "br") {
                // We can't use `emitOpenTag` for implicit open, as `br` would be implicitly closed.
                this@KsoupHtmlParser.effectiveHandler().onOpenTagName("br")
                this@KsoupHtmlParser.effectiveHandler().onOpenTag("br", emptyMap(), true)
                this@KsoupHtmlParser.effectiveHandler().onCloseTag("br", false)
            }

            // Set the start index for the next node
            this@KsoupHtmlParser.startIndex = endIndex + 1
        }

        override fun onSelfClosingTag(endIndex: Int) {
            this@KsoupHtmlParser.callbacks.onSelfClosingTag(endIndex)
            this@KsoupHtmlParser.endIndex = endIndex
            record(KsoupTokenEvent.SelfClosingTag(spanOf(endIndex, endIndex + 1)))
            if (
                this@KsoupHtmlParser.options.xmlMode ||
                this@KsoupHtmlParser.options.recognizeSelfClosing ||
                this@KsoupHtmlParser.foreignContext.lastOrNull() == true
            ) {
                this.closeCurrentTag(false)

                // Set the start index for the next node
                this@KsoupHtmlParser.startIndex = endIndex + 1
            } else {
                // Ignore the fact that this tag is self-closing
                this.onOpenTagEnd(endIndex)
            }
        }

        private fun closeCurrentTag(isOpenImplied: Boolean) {
            val name = this@KsoupHtmlParser.tagName
            this.endOpenTag(isOpenImplied)

            // Self-closing tags will be on the top of the stack
            if (this@KsoupHtmlParser.stack.size > 0 && this@KsoupHtmlParser.stack[this@KsoupHtmlParser.stack.size - 1] == name) {
                // If the opening tag isn't implied, the closing tag has to be implied.
                this@KsoupHtmlParser.effectiveHandler().onCloseTag(name, !isOpenImplied)
                this@KsoupHtmlParser.stack.removeLast()
            }
        }

        override fun onAttribName(start: Int, endIndex: Int) {
            this@KsoupHtmlParser.callbacks.onAttribName(start, endIndex)
            this@KsoupHtmlParser.startIndex = start
            this@KsoupHtmlParser.attribNameStart = start
            val name = this@KsoupHtmlParser.getSlice(start, endIndex)

            record(KsoupTokenEvent.AttribName(name, spanOf(start, endIndex)))

            this@KsoupHtmlParser.attribName = if (this@KsoupHtmlParser.lowerCaseAttributeNames) {
                name.lowercase()
            } else {
                name
            }
        }

        override fun onAttribData(start: Int, endIndex: Int) {
            this@KsoupHtmlParser.callbacks.onAttribData(start, endIndex)
            val data = this@KsoupHtmlParser.getSlice(start, endIndex)
            record(KsoupTokenEvent.AttribData(data, spanOf(start, endIndex)))
            this@KsoupHtmlParser.attribValue += data
        }

        override fun onAttribEntity(codepoint: Int, start: Int, endIndex: Int) {
            this@KsoupHtmlParser.callbacks.onAttribEntity(codepoint, start, endIndex)
            record(KsoupTokenEvent.AttribEntity(codepoint, spanOf(start, endIndex)))
            this@KsoupHtmlParser.attribValue += Char(codepoint)
        }

        override fun onAttribEnd(quote: QuoteType, endIndex: Int) {
            this@KsoupHtmlParser.callbacks.onAttribEnd(quote, endIndex)
            this@KsoupHtmlParser.endIndex = endIndex
            this@KsoupHtmlParser.lastAttribEnd = endIndex
            val span = spanOf(endIndex, endIndex + 1)
            record(KsoupTokenEvent.AttribEnd(quote, span))

            this@KsoupHtmlParser.effectiveHandler().onAttribute(
                name = this@KsoupHtmlParser.attribName,
                value = this@KsoupHtmlParser.attribValue,
                quote = when (quote) {
                    QuoteType.Double -> "\""
                    QuoteType.Single -> "'"
                    else -> null
                }
            )

            this@KsoupHtmlParser.attribs?.let {
                it[this@KsoupHtmlParser.attribName] = this@KsoupHtmlParser.attribValue
            }
            this@KsoupHtmlParser.attribValue = ""
        }

        private fun getInstructionName(value: String): String {
            val index = reNameEnd.find(value)?.range?.start ?: -1

            var name = if (index < 0) {
                value
            } else {
                value.substring(0, index)
            }

            if (this@KsoupHtmlParser.lowerCaseTagNames) {
                name = name.lowercase()
            }

            return name
        }

        override fun onDeclaration(start: Int, endIndex: Int) {
            this@KsoupHtmlParser.callbacks.onDeclaration(start, endIndex)
            this@KsoupHtmlParser.endIndex = endIndex
            val value = this@KsoupHtmlParser.getSlice(start, endIndex)
            val span = spanOf(start, endIndex)
            lastPiSpan = span
            record(KsoupTokenEvent.Declaration(value, span))

            val name = this.getInstructionName(value)
            this@KsoupHtmlParser.effectiveHandler().onProcessingInstruction(name, value)

            // Set the start index for the next node
            this@KsoupHtmlParser.startIndex = endIndex + 1
        }

        override fun onProcessingInstruction(start: Int, endIndex: Int) {
            this@KsoupHtmlParser.callbacks.onProcessingInstruction(start, endIndex)
            this@KsoupHtmlParser.endIndex = endIndex
            val value = this@KsoupHtmlParser.getSlice(start, endIndex)
            val span = spanOf(start, endIndex)
            lastPiSpan = span
            record(KsoupTokenEvent.ProcessingInstruction(value, span))

            val name = this.getInstructionName(value)
            this@KsoupHtmlParser.effectiveHandler().onProcessingInstruction(name, value)

            // Set the start index for the next node
            this@KsoupHtmlParser.startIndex = endIndex + 1
        }

        override fun onComment(start: Int, endIndex: Int, offset: Int) {
            this@KsoupHtmlParser.callbacks.onComment(start, endIndex, offset)
            this@KsoupHtmlParser.endIndex = endIndex
            val content = this@KsoupHtmlParser.getSlice(start, endIndex - offset)
            val span = spanOf(start, endIndex + 1)
            lastCommentSpan = span
            record(KsoupTokenEvent.Comment(content, span))

            this@KsoupHtmlParser.effectiveHandler().onComment(content)
            this@KsoupHtmlParser.effectiveHandler().onCommentEnd()

            // Set the start index for the next node
            this@KsoupHtmlParser.startIndex = endIndex + 1
        }

        override fun onCData(start: Int, endIndex: Int, offset: Int) {
            this@KsoupHtmlParser.callbacks.onCData(start, endIndex, offset)
            this@KsoupHtmlParser.endIndex = endIndex
            val value = this@KsoupHtmlParser.getSlice(start, endIndex - offset)
            val span = spanOf(start, endIndex + 1)
            lastCommentSpan = span
            record(KsoupTokenEvent.CData(value, span))

            if (this@KsoupHtmlParser.options.xmlMode || this@KsoupHtmlParser.options.recognizeCDATA) {
                this@KsoupHtmlParser.effectiveHandler().onCDataStart()
                this@KsoupHtmlParser.effectiveHandler().onText(value)
                this@KsoupHtmlParser.effectiveHandler().onCDataEnd()
            } else {
                this@KsoupHtmlParser.effectiveHandler().onComment("[CDATA[$value]]")
                this@KsoupHtmlParser.effectiveHandler().onCommentEnd()
            }

            // Set the start index for the next node
            this@KsoupHtmlParser.startIndex = endIndex + 1
        }

        override fun onEnd() {
            this@KsoupHtmlParser.callbacks.onEnd()
            // Set the end index for all remaining tags
            this@KsoupHtmlParser.endIndex = this@KsoupHtmlParser.startIndex
            this@KsoupHtmlParser.stack.indices.forEach { i ->
                val index = this@KsoupHtmlParser.stack.lastIndex - i
                this@KsoupHtmlParser.effectiveHandler().onCloseTag(this@KsoupHtmlParser.stack[index], true)
            }
            this@KsoupHtmlParser.effectiveHandler().onEnd()
        }
    }

    private val ksoupTokenizer = KsoupTokenizer(
        options = options,
        callbacks = ksoupTokenizerCallbacks
    ).also { tokenizer ->
        if (checkpointEnabled) {
            tokenizer.safePointListener = { safePoint, boundary ->
                this.lastSafePoint = safePoint
                this.lastSafePointBoundary = boundary
                this.safePointHits.add(KsoupSafePointHit(safePoint, boundary))
                // Document start/end are structural markers, not pause points:
                // pausing at end would deadlock finalization.
                if (this.pauseAtSafePoints &&
                    safePoint != KsoupSafePoint.DOCUMENT_START &&
                    safePoint != KsoupSafePoint.DOCUMENT_END
                ) {
                    tokenizer.pause()
                }
            }
        }
    }

    /**
     * Handler wrapper that mirrors tree construction into [nodeEventLog],
     * including absolute spans. Installed only when checkpoints are enabled.
     */
    @OptIn(ExperimentalKsoupApi::class)
    private val spanHandler: KsoupHtmlHandler? = if (!checkpointEnabled) null else object : KsoupHtmlHandler {
        private val delegate get() = this@KsoupHtmlParser.handler

        override fun onParserInit(ksoupHtmlParser: KsoupHtmlParser) = delegate.onParserInit(ksoupHtmlParser)
        override fun onReset() = delegate.onReset()
        override fun onEnd() = delegate.onEnd()
        override fun onError(error: Exception) = delegate.onError(error)

        override fun onCloseTag(name: String, isImplied: Boolean) {
            val span = this@KsoupHtmlParser.spanOf(
                this@KsoupHtmlParser.startIndex,
                this@KsoupHtmlParser.endIndex + 1
            )
            recordNode(KsoupNodeEvent.CloseElement(name, isImplied, span))
            delegate.onCloseTag(name, isImplied)
        }

        override fun onOpenTagName(name: String) = delegate.onOpenTagName(name)

        override fun onAttribute(name: String, value: String, quote: String?) {
            val span = this@KsoupHtmlParser.spanOf(
                this@KsoupHtmlParser.attribNameStart,
                this@KsoupHtmlParser.lastAttribEnd + 1
            )
            recordNode(KsoupNodeEvent.AttributeNode(name, value, quote, span))
            delegate.onAttribute(name, value, quote)
        }

        override fun onOpenTag(name: String, attributes: Map<String, String>, isImplied: Boolean) {
            val tagStart = maxOf(0, this@KsoupHtmlParser.currentTagNameStart - 1)
            val span = this@KsoupHtmlParser.spanOf(
                tagStart,
                this@KsoupHtmlParser.endIndex + 1
            )
            recordNode(KsoupNodeEvent.OpenElement(name, attributes.toMap(), isImplied, span))
            delegate.onOpenTag(name, attributes, isImplied)
        }

        override fun onText(text: String) {
            val span = this@KsoupHtmlParser.lastTextSpan
                ?: KsoupSpan.empty(this@KsoupHtmlParser.startIndex, this@KsoupHtmlParser.totalBytes)
            recordNode(KsoupNodeEvent.TextNode(text, span))
            delegate.onText(text)
        }

        override fun onComment(comment: String) {
            val span = this@KsoupHtmlParser.lastCommentSpan
                ?: KsoupSpan.empty(this@KsoupHtmlParser.startIndex, this@KsoupHtmlParser.totalBytes)
            recordNode(KsoupNodeEvent.CommentNode(comment, span))
            delegate.onComment(comment)
        }

        override fun onCDataStart() = delegate.onCDataStart()
        override fun onCDataEnd() = delegate.onCDataEnd()
        override fun onCommentEnd() = delegate.onCommentEnd()

        override fun onProcessingInstruction(name: String, data: String) {
            val span = this@KsoupHtmlParser.lastPiSpan
                ?: KsoupSpan.empty(this@KsoupHtmlParser.startIndex, this@KsoupHtmlParser.totalBytes)
            recordNode(KsoupNodeEvent.ProcessingInstructionNode(name, data, span))
            delegate.onProcessingInstruction(name, data)
        }
    }

    /**
     * Resets the parser to a blank state, ready to parse a new HTML document
     */
    @OptIn(ExperimentalKsoupApi::class)
    public fun reset() {
        this.effectiveHandler().onReset()
        this.ksoupTokenizer.reset()
        this.tagName = ""
        this.attribName = ""
        this.attribValue = ""
        this.attribs = null
        this.stack.clear()
        this.foreignContext.clear()
        this.startIndex = 0
        this.endIndex = 0
        this.openTagStart = 0
        this.currentTagNameStart = -1
        this.attribNameStart = 0
        this.lastAttribEnd = 0
        this.lastTextSpan = null
        this.lastCommentSpan = null
        this.lastPiSpan = null
        this.handler.onParserInit(this)
        this.buffers.clear()
        this.bufferOffset = 0
        this.writeIndex = 0
        this.ended = false
        this.chunkMeta.clear()
        this.totalChars = 0
        this.totalBytes = 0
        this.digestCharPos = 0
        this.pendingSuffix = ""
        this.tokenEventLog.clear()
        this.nodeEventLog.clear()
        this.safePointHits.clear()
        this.lastSafePoint = KsoupSafePoint.DOCUMENT_START
        this.lastSafePointBoundary = 0
    }

    private fun effectiveHandler(): KsoupHtmlHandler = this.spanHandler ?: this.handler

    /**
     * Resets the parser, then parses a complete document and
     * pushes it to the handler.
     *
     * @param data Document to parse.
     */
    public fun parseComplete(data: String) {
        this.reset()
        this.end(data)
    }

    private fun getSlice(
        start: Int,
        end: Int
    ): String {
        // Drop fully-consumed buffers that precede the requested range.
        while (this.buffers.isNotEmpty() &&
            start - this.bufferOffset >= this.buffers.first().length
        ) {
            this.shiftBuffer()
        }

        // A token can span several small chunks (this happens naturally after
        // a checkpoint resume), so walk the buffers and stitch the pieces.
        val builder = StringBuilder(end - start)
        var cursor = start
        while (cursor < end) {
            val buffer = this.buffers.first()
            val localStart = cursor - this.bufferOffset
            val localEnd = minOf(end - this.bufferOffset, buffer.length)
            builder.append(buffer.substring(localStart, localEnd))
            cursor = this.bufferOffset + localEnd
            if (cursor < end) {
                this.shiftBuffer()
            }
        }

        return builder.toString()
    }

    private fun shiftBuffer() {
        this.bufferOffset += this.buffers.first().length
        this.writeIndex--
        this.buffers.removeFirst()
    }

    /**
     * Parses a chunk of data and calls the corresponding callbacks.
     *
     * @param chunk Chunk to parse.
     */
    public fun write(chunk: String) {
        if (this.ended) {
            this.effectiveHandler().onError(Exception(".write() after done!"))
            return
        }

        this.feedChunk(chunk)
    }

    /**
     * Feeds one logical chunk, prepending any suffix retained by a restored
     * checkpoint. Joining the suffix with the new chunk is safe because the
     * tokenizer is insensitive to chunk boundaries.
     */
    private fun feedChunk(chunk: String) {
        val effective = if (this.pendingSuffix.isNotEmpty()) {
            val joined = this.pendingSuffix + chunk
            this.pendingSuffix = ""
            joined
        } else {
            chunk
        }

        val byteLength = if (encoding == KsoupCheckpointEncoding.UTF_8) {
            KsoupDigest.encodeUtf8(effective).size.toLong()
        } else {
            effective.length.toLong()
        }
        this.chunkMeta.add(ChunkMeta(this.totalChars, this.totalBytes, effective))
        this.totalChars += effective.length
        this.totalBytes += byteLength

        this.buffers.add(effective)
        if (this.ksoupTokenizer.running) {
            this.ksoupTokenizer.write(effective)
            this.writeIndex++
        }
    }

    /**
     * Parses the end of the buffer and clears the stack, calls onend.
     *
     * @param chunk Optional final chunk to parse.
     */
    public fun end(chunk: String? = null) {
        if (this.ended) {
            this.effectiveHandler().onError(Exception(".end() after done!"))
            return
        }

        chunk?.let { this.feedChunk(it) }
        this.ended = true
        this.ksoupTokenizer.end()
        if (checkpointEnabled) this.finalizeByteSpans()
    }

    /**
     * Recomputes the byte half of every span against the now-complete chunk
     * list. During streaming, byte offsets past the last delivered character
     * cannot be known; doing this once at end-of-stream makes the byte spans
     * independent of chunk size and checkpoint boundaries.
     */
    private fun finalizeByteSpans() {
        for (i in this.tokenEventLog.indices) {
            this.tokenEventLog[i] = withResolvedSpan(this.tokenEventLog[i])
        }
        for (i in this.nodeEventLog.indices) {
            this.nodeEventLog[i] = withResolvedNodeSpan(this.nodeEventLog[i])
        }
    }

    private fun resolve(span: KsoupSpan): KsoupSpan =
        if (encoding != KsoupCheckpointEncoding.UTF_8) span
        else KsoupSpan(span.startOffset, span.endOffset, byteOffsetAt(span.startOffset), byteOffsetAt(span.endOffset))

    private fun withResolvedSpan(event: KsoupTokenEvent): KsoupTokenEvent {
        val span = resolve(event.span)
        return when (event) {
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

    private fun withResolvedNodeSpan(event: KsoupNodeEvent): KsoupNodeEvent {
        val span = resolve(event.span)
        return when (event) {
            is KsoupNodeEvent.OpenElement -> event.copy(span = span)
            is KsoupNodeEvent.CloseElement -> event.copy(span = span)
            is KsoupNodeEvent.TextNode -> event.copy(span = span)
            is KsoupNodeEvent.CommentNode -> event.copy(span = span)
            is KsoupNodeEvent.CDataNode -> event.copy(span = span)
            is KsoupNodeEvent.ProcessingInstructionNode -> event.copy(span = span)
            is KsoupNodeEvent.AttributeNode -> event.copy(span = span)
        }
    }

    /**
     * Pauses parsing. The parser won't emit events until `resume` is called.
     */
    public fun pause() {
        this.ksoupTokenizer.pause()
    }

    /**
     * Resumes parsing after `pause` was called.
     */
    public fun resume() {
        this.ksoupTokenizer.resume()

        while (
            this.ksoupTokenizer.running &&
            this.writeIndex < this.buffers.size
        ) {
            this.ksoupTokenizer.write(this.buffers[this.writeIndex++])
        }

        if (this.ended) this.ksoupTokenizer.end()
    }

    // region Checkpoint capture / restore

    /**
     * Captures a checkpoint at the most recently reached safe point.
     *
     * @param expected Optional safe point type; when given, capture fails if
     *   the latest safe point has a different type.
     * @return the serialized checkpoint, or `null` if no safe point has been
     *   reached yet.
     * @throws KsoupCheckpointException if the tokenizer is mid-construct, the
     *   retained suffix exceeds [MAX_BUFFER_CHARS], or [expected] does not match.
     */
    @ExperimentalKsoupApi
    public fun checkpoint(expected: KsoupSafePoint? = null): KsoupCheckpoint? {
        if (!checkpointEnabled) {
            throw KsoupCheckpointException(
                "Checkpoint support is disabled; construct KsoupHtmlParser with checkpointEnabled = true"
            )
        }
        val safePoint = this.lastSafePoint ?: return null
        val boundary = this.lastSafePointBoundary
        if (expected != null && expected != safePoint) {
            throw KsoupCheckpointException(
                "Requested checkpoint at $expected but latest safe point is $safePoint"
            )
        }
        // DOCUMENT_END is terminal: the stream is fully consumed and no further
        // data can arrive, so a half-open comment/tag left in the tokenizer does
        // not make the checkpoint ambiguous. Every other safe point must be a
        // clean text boundary.
        if (safePoint != KsoupSafePoint.DOCUMENT_END && !this.ksoupTokenizer.isAtSafePoint()) {
            throw KsoupCheckpointException(
                "Tokenizer is not at a safe point (state=${this.ksoupTokenizer.currentStateName}, " +
                    "running=${this.ksoupTokenizer.running}, boundary=$boundary, front=${this.ksoupTokenizer.absoluteIndex})"
            )
        }
        // A checkpoint may only cut at the parse front. This guarantees the
        // resumed tokenizer cannot re-emit events the uninterrupted parse
        // already produced.
        if (boundary != this.ksoupTokenizer.absoluteIndex) {
            throw KsoupCheckpointException(
                "Safe point boundary $boundary is behind the parse front " +
                    "${this.ksoupTokenizer.absoluteIndex}; checkpoint only at the latest boundary"
            )
        }

        val suffix = this.retainedSuffix(boundary)
        if (suffix.length > MAX_BUFFER_CHARS) {
            throw KsoupCheckpointException(
                "Retained suffix of ${suffix.length} chars exceeds buffer limit $MAX_BUFFER_CHARS"
            )
        }

        // Recompute the digest over the whole logical prefix in one shot. This
        // is the only way to match the resuming process, which hashes the single
        // string `input.substring(0, boundary)`; per-chunk streaming would
        // mis-encode surrogate pairs split across chunk boundaries.
        val prefix = this.retainedPrefix(boundary)
        val prefixBytes = KsoupDigest.encodeUtf8(prefix)
        val freshHasher = KsoupStreamingSha256().apply { update(prefixBytes) }
        val hashState = freshHasher.snapshot()
        val prefixDigest = KsoupDigest.toHex(KsoupStreamingSha256().apply { restore(hashState) }.digest())
        this.sha256.restore(hashState)
        this.digestCharPos = boundary

        val tokenizerSnapshot = this.ksoupTokenizer.snapshot(
            boundary,
            allowTerminal = safePoint == KsoupSafePoint.DOCUMENT_END,
        )
        val parserState = ParserState(
            startIndex = this.startIndex,
            endIndex = this.endIndex,
            openTagStart = this.openTagStart,
            tagName = this.tagName,
            attribName = this.attribName,
            attribValue = this.attribValue,
            attribs = this.attribs?.let { LinkedHashMap(it) },
            stack = this.stack.toMutableList(),
            foreignContext = this.foreignContext.toMutableList(),
            ended = this.ended,
            currentTagNameStart = this.currentTagNameStart,
            attribNameStart = this.attribNameStart,
            lastAttribEnd = this.lastAttribEnd,
        )

        return KsoupCheckpoint(
            safePoint = safePoint,
            boundary = boundary,
            consumedBytes = this.byteCountUpTo(boundary),
            encoding = this.encoding,
            options = this.options,
            prefixDigest = prefixDigest,
            hashState = hashState,
            suffix = suffix,
            tokenizerStateName = tokenizerSnapshot.stateName,
            tokenizerBaseStateName = tokenizerSnapshot.baseStateName,
            parserState = parserState,
        )
    }

    private fun retainedSuffix(boundary: Int): String {
        val sb = StringBuilder()
        for (meta in this.chunkMeta) {
            val chunkEnd = meta.charStart + meta.text.length
            if (chunkEnd <= boundary) continue
            val from = maxOf(0, boundary - meta.charStart)
            sb.append(meta.text.substring(from))
        }
        return sb.toString()
    }

    /** Reassembles the full logical prefix of [boundary] chars from chunk bookkeeping. */
    private fun retainedPrefix(boundary: Int): String {
        val sb = StringBuilder(boundary)
        var remaining = boundary
        for (meta in this.chunkMeta) {
            if (remaining <= 0) break
            // Skip any chunk starting beyond the prefix (restored parsers may
            // register chunks at a non-zero absolute base).
            if (meta.charStart >= boundary) break
            val localEnd = minOf(boundary - meta.charStart, meta.text.length)
            if (localEnd > 0) sb.append(meta.text, 0, localEnd)
            remaining = boundary - (meta.charStart + localEnd)
        }
        return sb.toString()
    }

    private fun byteCountUpTo(boundary: Int): Long =
        if (encoding != KsoupCheckpointEncoding.UTF_8) boundary.toLong()
        else byteOffsetAt(boundary)

    /**
     * Restores parser and tokenizer state from [checkpoint] in this (fresh)
     * parser instance.
     *
     * @param consumedPrefix the exact input that had been fed when the
     *   checkpoint was captured. Its SHA-256 must match the bound digest.
     * @throws KsoupCheckpointException on any mismatch of options, encoding,
     *   digest or boundary, or when the document had already ended.
     */
    @ExperimentalKsoupApi
    public fun restoreCheckpoint(checkpoint: KsoupCheckpoint, consumedPrefix: String) {
        if (!checkpointEnabled) {
            throw KsoupCheckpointException(
                "Checkpoint support is disabled; construct KsoupHtmlParser with checkpointEnabled = true"
            )
        }
        if (this.options != checkpoint.options) {
            throw KsoupCheckpointException(
                "Parser options do not match the checkpoint (parser=${this.options}, checkpoint=${checkpoint.options})"
            )
        }
        if (this.encoding != checkpoint.encoding) {
            throw KsoupCheckpointException(
                "Encoding does not match the checkpoint (parser=${this.encoding}, checkpoint=${checkpoint.encoding})"
            )
        }
        if (consumedPrefix.length != checkpoint.boundary) {
            throw KsoupCheckpointException(
                "Consumed prefix length ${consumedPrefix.length} does not match checkpoint boundary ${checkpoint.boundary}"
            )
        }
        val actualDigest = KsoupDigest.sha256Hex(KsoupDigest.encodeUtf8(consumedPrefix))
        if (!constantTimeEquals(actualDigest, checkpoint.prefixDigest)) {
            throw KsoupCheckpointException(
                "Prefix digest mismatch: the consumed input does not match the checkpoint"
            )
        }

        val p = checkpoint.parserState
        this.startIndex = p.startIndex
        this.endIndex = p.endIndex
        this.openTagStart = p.openTagStart
        this.tagName = p.tagName
        this.attribName = p.attribName
        this.attribValue = p.attribValue
        this.attribs = p.attribs
        this.stack.clear()
        this.stack.addAll(p.stack)
        this.foreignContext.clear()
        this.foreignContext.addAll(p.foreignContext)
        this.ended = p.ended
        this.currentTagNameStart = p.currentTagNameStart
        this.attribNameStart = p.attribNameStart
        this.lastAttribEnd = p.lastAttribEnd

        // The retained suffix was already delivered before the checkpoint but
        // never tokenized; replay it through the restored tokenizer, which
        // continues indexing at the checkpoint boundary.
        this.buffers.clear()
        // Absolute slice indices continue at the boundary, so the buffer base
        // must be shifted to match (getSlice subtracts bufferOffset).
        this.bufferOffset = checkpoint.boundary
        this.writeIndex = 0
        this.pendingSuffix = ""

        // No chunks are registered yet: the replay starts exactly at the
        // boundary, with the consumed prefix byte count as its byte base.
        this.chunkMeta.clear()
        this.totalChars = checkpoint.boundary
        this.totalBytes = checkpoint.consumedBytes
        this.sha256.restore(checkpoint.hashState)
        this.digestCharPos = checkpoint.boundary

        val tokenizerSnapshot = KsoupTokenizer.Snapshot(
            stateName = checkpoint.tokenizerStateName,
            baseStateName = checkpoint.tokenizerBaseStateName,
            boundary = checkpoint.boundary,
        )
        this.ksoupTokenizer.restore(tokenizerSnapshot)

        this.lastSafePoint = checkpoint.safePoint
        this.lastSafePointBoundary = checkpoint.boundary
        this.effectiveHandler().onParserInit(this)

        if (checkpoint.suffix.isNotEmpty()) {
            this.feedChunk(checkpoint.suffix)
            // When the suffix was consumed back to a text boundary, no
            // in-progress token references it. Drop the exhausted buffer and
            // rebase the slice offset so getSlice()'s first-buffer invariant
            // holds for the next write. A suffix ending mid-tag is retained.
            if (this.ksoupTokenizer.isAtSafePoint() &&
                this.ksoupTokenizer.absoluteIndex >= checkpoint.nextFeedOffset
            ) {
                this.buffers.clear()
                this.bufferOffset = checkpoint.nextFeedOffset
                this.writeIndex = 0
            }
        }
    }

    private fun constantTimeEquals(a: String, b: String): Boolean {
        if (a.length != b.length) return false
        var diff = 0
        for (i in a.indices) diff = diff or (a[i].code xor b[i].code)
        return diff == 0
    }

    // endregion

    /** Whether the tokenizer is currently running (false while paused at a safe point). */
    public val isRunning: Boolean get() = this.ksoupTokenizer.running

    /** Whether the tokenizer is parked on a capturable text-boundary safe point. */
    @ExperimentalKsoupApi
    public fun atSafePoint(): Boolean = this.ksoupTokenizer.isAtSafePoint()

    /** Whether [end] has been called. */
    public val isEnded: Boolean get() = this.ended

    /** The most recently reached safe point, or `null` before the first one. */
    @ExperimentalKsoupApi
    public val latestSafePoint: KsoupSafePoint? get() = this.lastSafePoint

    /** Absolute char boundary of the most recently reached safe point. */
    @ExperimentalKsoupApi
    public val latestSafePointBoundary: Int get() = this.lastSafePointBoundary

    public enum class QuoteType {
        NoValue,
        Unquoted,
        Single,
        Double,
    }

    public companion object {
        /**
         * Maximum number of not-yet-tokenized characters a checkpoint may
         * retain. Above this, [checkpoint] fails rather than serializing an
         * unbounded buffer. Choose a smaller chunk size if you hit the limit.
         */
        @ExperimentalKsoupApi
        public const val MAX_BUFFER_CHARS: Int = 1 shl 20 // 1,048,576 chars (~1 MiB of UTF-16)

        private val formTags = setOf(
            "input",
            "option",
            "optgroup",
            "select",
            "button",
            "datalist",
            "textarea",
        )
        private val pTag = setOf("p")
        private val tableSectionTags = setOf("thead", "tbody")
        private val ddtTags = setOf("dt", "dd")
        private val rtpTags = setOf("rt", "rp")

        private val openImpliesClose = mapOf(
            "tr" to setOf("tr", "th", "td"),
            "th" to setOf("th"),
            "td" to setOf("thead", "th", "td"),
            "body" to setOf("head", "link", "script"),
            "li" to setOf("li"),
            "p" to pTag,
            "h1" to pTag,
            "h2" to pTag,
            "h3" to pTag,
            "h4" to pTag,
            "h5" to pTag,
            "h6" to pTag,
            "select" to formTags,
            "input" to formTags,
            "output" to formTags,
            "button" to formTags,
            "datalist" to formTags,
            "textarea" to formTags,
            "option" to setOf("option"),
            "optgroup" to setOf("optgroup", "option"),
            "dd" to ddtTags,
            "dt" to ddtTags,
            "address" to pTag,
            "article" to pTag,
            "aside" to pTag,
            "blockquote" to pTag,
            "details" to pTag,
            "div" to pTag,
            "dl" to pTag,
            "fieldset" to pTag,
            "figcaption" to pTag,
            "figure" to pTag,
            "footer" to pTag,
            "form" to pTag,
            "header" to pTag,
            "hr" to pTag,
            "main" to pTag,
            "menu" to pTag,
            "nav" to pTag,
            "ol" to pTag,
            "pre" to pTag,
            "section" to pTag,
            "table" to pTag,
            "ul" to pTag,
            "rt" to rtpTags,
            "rp" to rtpTags,
            "tbody" to tableSectionTags,
            "tfoot" to tableSectionTags,
        )

        private val voidElements = setOf(
            "area",
            "base",
            "basefont",
            "br",
            "col",
            "command",
            "embed",
            "frame",
            "hr",
            "img",
            "input",
            "isindex",
            "keygen",
            "link",
            "meta",
            "param",
            "source",
            "track",
            "wbr",
        )

        private val foreignContextElements = setOf(
            "math",
            "svg",
        )

        private val htmlIntegrationElements = setOf(
            "mi",
            "mo",
            "mn",
            "ms",
            "mtext",
            "annotation-xml",
            "foreignobject",
            "desc",
            "title",
        )

        private val reNameEnd = Regex("\\s|/")
    }

}
