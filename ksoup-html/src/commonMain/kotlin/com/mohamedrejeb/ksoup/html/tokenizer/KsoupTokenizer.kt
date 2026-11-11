package com.mohamedrejeb.ksoup.html.tokenizer

import com.mohamedrejeb.ksoup.entities.KsoupEntities
import com.mohamedrejeb.ksoup.html.checkpoint.KsoupCheckpointException
import com.mohamedrejeb.ksoup.html.checkpoint.KsoupSafePoint
import com.mohamedrejeb.ksoup.html.parser.KsoupHtmlOptions
import com.mohamedrejeb.ksoup.html.parser.KsoupHtmlParser

/**
 * KsoupTokenizer is an HTML Tokenizer which is able to receive HTML string,
 * breaks it up into individual tokens, and return those tokens with the [KsoupTokenizerCallbacks]
 *
 * @param options KsoupHtmlOptions
 *
 */
internal class KsoupTokenizer(
    options: KsoupHtmlOptions,
    private val callbacks: KsoupTokenizerCallbacks
) {
    private val xmlMode = options.xmlMode
    private val decodeEntities = options.decodeEntities

    /** The current state the tokenizer is in. */
    private var state = State.Text
    /** The read buffer. */
    private var buffer = ""
    /** The beginning of the section that is currently being read. */
    private var sectionStart = 0
    /** The index within the buffer that we are currently looking at. */
    private var index = 0
    /** The start of the last entity. */
    private var entityStart = 0
    /**
     * Some behavior, e.g., When decoding entities, is done while we are in another state.
     * This keeps track of the other state type.
     */
    private var baseState = State.Text
    /** For special parsing behavior inside script and style tags. */
    private var isSpecial = false
    /** Indicates whether the tokenizer has been paused. */
    public var running: Boolean = true
    /** The offset of the current buffer. */
    private var offset = 0

    /**
     * Invoked whenever the tokenizer reaches a [KsoupSafePoint]. Opt-in:
     * `null` (the default) keeps the tokenizer allocation-free.
     *
     * The callback receives the safe point and the absolute character offset
     * just past the last consumed character.
     */
    internal var safePointListener: ((KsoupSafePoint, Int) -> Unit)? = null

    private var lastSafePointOffset = -1
    private var documentStartEmitted = false
    /**
     * Boundary recorded by the most recent safe point. When a listener pauses
     * the tokenizer, the parse loop jumps [index] here before returning, so no
     * character past the safe point gets consumed ahead of the pause.
     */
    private var pauseBoundary = -1
    private var pausedAtBoundary = false
    private var finished = false

    private fun emitSafePoint(safePoint: KsoupSafePoint, absoluteIndex: Int = this.index) {
        this.pauseBoundary = absoluteIndex
        // Document start/end are structural markers and must always be
        // observable even when they share an offset with the previous token
        // boundary (e.g. DOCUMENT_END right after the final closing tag).
        val alwaysEmit = safePoint == KsoupSafePoint.DOCUMENT_START ||
            safePoint == KsoupSafePoint.DOCUMENT_END
        if (alwaysEmit || absoluteIndex != this.lastSafePointOffset) {
            this.lastSafePointOffset = absoluteIndex
            this.safePointListener?.invoke(safePoint, absoluteIndex)
            // A listener may pause the tokenizer. When it does, pin the parse
            // front to this boundary so no further character is consumed until
            // resume() re-enters the loop.
            this.pausedAtBoundary = !this.running
        }
    }

    /**
     * A checkpoint is only valid on a token boundary in the plain [State.Text]
     * state. This guarantees no entity, comment, CDATA, raw-text element or
     * half-open tag is in progress.
     */
    internal fun isAtSafePoint(): Boolean = this.state == State.Text

    /** Name of the current tokenizer state, used in checkpoint error messages. */
    internal val currentStateName: String get() = this.state.name

    /** Absolute char offset the parse loop has reached ([index] is already absolute). */
    internal val absoluteIndex: Int get() = this.index

    @OptIn(ExperimentalUnsignedTypes::class)
    fun reset() {
        this.state = State.Text
        this.buffer = ""
        this.sectionStart = 0
        this.index = 0
        this.baseState = State.Text
        this.currentSequence = null
        this.running = true
        this.offset = 0
        this.isSpecial = false
        this.sequenceIndex = 0
        this.entityStart = 0
        this.lastSafePointOffset = -1
        this.documentStartEmitted = false
        this.pauseBoundary = -1
        this.pausedAtBoundary = false
        this.finished = false
    }

    fun write(chunk: String) {
        if (!this.documentStartEmitted) {
            this.documentStartEmitted = true
            this.emitSafePoint(KsoupSafePoint.DOCUMENT_START, this.index)
        }

        // Retain every character still referenced by an in-flight construct
        // (an entity, a tag, a comment, ...). At a text boundary nothing is in
        // flight and the consumed prefix can be dropped. This is what allows
        // entities and tags to span chunk boundaries: absolute indices stay
        // valid because only the buffer base ([offset]) moves.
        val retainFrom = this.retainFromAbsolute()
        if (retainFrom > this.offset) {
            this.buffer = this.buffer.substring(retainFrom - this.offset)
            this.offset = retainFrom
        }
        this.buffer += chunk
        this.parse()
        // Only report a text boundary when the whole chunk was actually
        // consumed; when paused at an earlier safe point the parse front is
        // somewhere inside the buffer and no new boundary exists yet.
        if (this.state == State.Text && this.index >= this.offset + this.buffer.length) {
            this.emitSafePoint(KsoupSafePoint.TEXT_BOUNDARY, this.index)
        }
    }

    /**
     * Lowest absolute index that must remain in [buffer] for the current
     * construct to keep working. On a plain text boundary that is simply the
     * parse front; inside a token it can be the tag/entity start.
     */
    private fun retainFromAbsolute(): Int {
        if (this.state == State.Text) return this.index
        var lowest = minOf(this.index, this.sectionStart.coerceAtLeast(this.offset))
        if (this.state == State.InEntity) {
            lowest = minOf(lowest, this.entityStart)
        }
        return lowest.coerceAtLeast(this.offset)
    }

    fun end() {
        // Guard against double finalization: a resume() after an end()-induced
        // pause must not run finish() (and re-emit onEnd) a second time.
        if (this.running && !this.finished) {
            this.finished = true
            this.finish()
        }
    }

    fun pause() {
        this.running = false
        this.pausedAtBoundary = false
    }

    fun resume() {
        this.running = true
        this.pausedAtBoundary = false
        if (this.index < this.buffer.length + this.offset) {
            this.parse()
        }
    }

    private fun stateText(c: Int) {
        if (
            c == CharCodes.Lt.code ||
            (!this.decodeEntities && this.fastForwardTo(CharCodes.Lt.code))
        ) {
            if (this.index > this.sectionStart) {
                this.callbacks.onText(this.sectionStart, this.index)
            }
            this.state = State.BeforeTagName
            this.sectionStart = this.index
        } else if (this.decodeEntities && c == CharCodes.Amp.code) {
            this.startEntity()
        }
    }

    @OptIn(ExperimentalUnsignedTypes::class)
    private var currentSequence: UByteArray? = null
    private var sequenceIndex = 0
    @OptIn(ExperimentalUnsignedTypes::class)
    private fun stateSpecialStartSequence(c: Int) {
        val currentSequence = this.currentSequence ?: return

        val isEnd = this.sequenceIndex == currentSequence.size
        val isMatch = if (isEnd) {
            // If we are at the end of the sequence, make sure the tag name has ended
            isEndOfTagSection(c)
        } else {
            // Otherwise, do a case-insensitive comparison
            (c or 0x20) == currentSequence[this.sequenceIndex].toInt()
        }

        if (!isMatch) {
            this.isSpecial = false
        } else if (!isEnd) {
            this.sequenceIndex++
            return
        }

        this.sequenceIndex = 0
        this.state = State.InTagName

        this.stateInTagName(c)
    }

    /** Look for an end tag. For <title> tags, also decode entities. */
    @OptIn(ExperimentalUnsignedTypes::class)
    private fun stateInSpecialTag(c: Int) {
        val currentSequence = this.currentSequence ?: return

        if (this.sequenceIndex == currentSequence.size) {
            if (c == CharCodes.Gt.code || isWhitespace(c)) {
                val endOfText = this.index - currentSequence.size

                if (this.sectionStart < endOfText) {
                    // Spoof the index so that reported locations match up.
                    val actualIndex = this.index
                    this.index = endOfText
                    this.callbacks.onText(this.sectionStart, endOfText)
                    this.index = actualIndex
                }

                this.isSpecial = false
                this.sectionStart = endOfText + 2 // Skip over the `</`
                this.stateInClosingTagName(c)
                return // We are done skip the rest of the function.
            }

            this.sequenceIndex = 0
        }

        if ((c or 0x20) == currentSequence[this.sequenceIndex].toInt()) {
            this.sequenceIndex += 1
        } else if (this.sequenceIndex == 0) {
            if (currentSequence == Sequences.TitleEnd) {
                // We have to parse entities in <title> tags.
                if (this.decodeEntities && c == CharCodes.Amp.code) {
                    this.startEntity()
                }
            } else if (this.fastForwardTo(CharCodes.Lt.code)) {
                // Outside <title> tags, we can fast-forward.
                this.sequenceIndex = 1
            }
        } else {
            // If we see a `<`, set the sequence index to 1 useful for eg. `<</script>`.
            this.sequenceIndex = if (c == CharCodes.Lt.code) 1 else 0
        }
    }

    @OptIn(ExperimentalUnsignedTypes::class)
    private fun stateCDATASequence(c: Int) {
        if (c == Sequences.Cdata[sequenceIndex].toInt()) {
            if (++sequenceIndex == Sequences.Cdata.size) {
                this.state = State.InCommentLike
                this.currentSequence = Sequences.CdataEnd
                this.sequenceIndex = 0
                this.sectionStart = index + 1
            }
        } else {
            this.sequenceIndex = 0
            this.state = State.InDeclaration
            this.stateInDeclaration(c) // Re-Consume the character
        }
    }

    /**
     * When we wait for one specific character, we can speed things up
     * by skipping through the buffer until we find it.
     *
     * @returns Whether the character was found.
     */
    private fun fastForwardTo(c: Int): Boolean {
        while (this.index < this.buffer.length + this.offset) {
            if (this.buffer[this.index - this.offset].code == c) {
                return true
            }
            index++
        }

        /*
         * We increment the index at the end of the `parse` loop,
         * so set it to `buffer.length - 1` here.
         *
         * TODO: Refactor `parse` to increment index before calling states.
         */
        this.index = this.buffer.length + this.offset - 1

        return false
    }

    /**
     * Comments and CDATA end with `-->` and `]]>`.
     *
     * Their common qualities are:
     * - Their end sequences have a distinct character they start with.
     * - That character is then repeated, so we have to check multiple repeats.
     * - All characters but the start character of the sequence can be skipped.
     */
    @OptIn(ExperimentalUnsignedTypes::class)
    private fun stateInCommentLike(c: Int) {
        val currentSequence = this.currentSequence ?: return

        if (c == currentSequence[sequenceIndex].toInt()) {
            if (++sequenceIndex == currentSequence.size) {
                if (currentSequence == Sequences.CdataEnd) {
                    callbacks.onCData(sectionStart, index, 2)
                    emitSafePoint(KsoupSafePoint.AFTER_CDATA, index + 1)
                } else {
                    callbacks.onComment(sectionStart, index, 2)
                    emitSafePoint(KsoupSafePoint.AFTER_COMMENT, index + 1)
                }

                sequenceIndex = 0
                sectionStart = index + 1
                state = State.Text
            }
        } else if (sequenceIndex == 0) {
            // Fast-forward to the first character of the sequence
            if (fastForwardTo(currentSequence[0].toInt())) {
                sequenceIndex = 1
            }
        } else if (c != currentSequence[sequenceIndex - 1].toInt()) {
            // Allow long sequences, e.g., --->, ]]>
            sequenceIndex = 0
        }
    }

    /**
     * HTML only allows ASCII alpha characters (a-z and A-Z) at the beginning of a tag name.
     *
     * XML allows a lot more characters here (@see https://www.w3.org/TR/REC-xml/#NT-NameStartChar).
     * We allow anything that wouldn't end the tag.
     */
    private fun isTagStartChar(c: Int): Boolean {
        return if (xmlMode) !isEndOfTagSection(c) else isASCIIAlpha(c)
    }

    /**
     * Check if `c` is a valid character of an HTML Entity.
     */
    private fun isInEntityChar(c: Int): Boolean {
        return isASCIIAlpha(c) || isDigit(c) || c == CharCodes.Semi.code
    }

    @OptIn(ExperimentalUnsignedTypes::class)
    private fun startSpecial(sequence: UByteArray, offset: Int) {
        this.isSpecial = true
        this.currentSequence = sequence
        this.sequenceIndex = offset
        this.state = State.SpecialStartSequence
    }

    @OptIn(ExperimentalUnsignedTypes::class)
    private fun stateBeforeTagName(c: Int) {
        if (c == CharCodes.ExclamationMark.code) {
            state = State.BeforeDeclaration
            sectionStart = index + 1
        } else if (c == CharCodes.QuestionMark.code) {
            state = State.InProcessingInstruction
            sectionStart = index + 1
        } else if (isTagStartChar(c)) {
            // Lowercase the character
            val lower = c or 0x20
            sectionStart = index

            if (xmlMode) {
                state = State.InTagName
            } else if (lower == Sequences.ScriptEnd[2].toInt()) {
                this.state = State.BeforeSpecialS
            } else if (lower == Sequences.TitleEnd[2].toInt()) {
                this.state = State.BeforeSpecialT
            } else {
                this.state = State.InTagName
            }
        } else if (c == CharCodes.Slash.code) {
            state = State.BeforeClosingTagName
        } else {
            state = State.Text
            stateText(c)
        }
    }

    private fun stateInTagName(c: Int) {
        if (isEndOfTagSection(c)) {
            callbacks.onOpenTagName(sectionStart, index)
            sectionStart = -1
            state = State.BeforeAttributeName
            stateBeforeAttributeName(c)
        }
    }

    private fun stateBeforeClosingTagName(c: Int) {
        if (isWhitespace(c)) {
            // Ignore
        } else if (c == CharCodes.Gt.code) {
            state = State.Text
        } else {
            state = if (isTagStartChar(c)) {
                State.InClosingTagName
            } else {
                State.InSpecialComment
            }
            sectionStart = index
        }
    }

    private fun stateInClosingTagName(c: Int) {
        if (c == CharCodes.Gt.code || isWhitespace(c)) {
            callbacks.onCloseTag(sectionStart, index)
            sectionStart = -1
            state = State.AfterClosingTagName
            this.stateAfterClosingTagName(c)
        }
    }

    private fun stateAfterClosingTagName(c: Int) {
        // Skip everything until ">"
        if (c == CharCodes.Gt.code || this.fastForwardTo(CharCodes.Gt.code)) {
            this.state = State.Text
            this.sectionStart = this.index + 1
            this.emitSafePoint(KsoupSafePoint.AFTER_CLOSE_TAG, this.index + 1)
        }
    }

    private fun stateBeforeAttributeName(c: Int) {
        if (c == CharCodes.Gt.code) {
            this.callbacks.onOpenTagEnd(this.index)
            if (this.isSpecial) {
                // Raw-text elements (script/style/title/textarea): no safe point
                // until the matching end tag has been fully consumed.
                this.state = State.InSpecialTag
                this.sequenceIndex = 0
            } else {
                this.state = State.Text
                this.emitSafePoint(KsoupSafePoint.AFTER_OPEN_TAG, this.index + 1)
            }
            this.sectionStart = this.index + 1
        } else if (c == CharCodes.Slash.code) {
            this.state = State.InSelfClosingTag
        } else if (!isWhitespace(c)) {
            this.state = State.InAttributeName
            this.sectionStart = this.index
        }
    }

    private fun stateInSelfClosingTag(c: Int) {
        if (c == CharCodes.Gt.code) {
            this.callbacks.onSelfClosingTag(this.index)
            this.state = State.Text
            this.sectionStart = this.index + 1
            this.isSpecial = false // Reset special state, in case of self-closing special tags
            this.emitSafePoint(KsoupSafePoint.AFTER_SELF_CLOSING_TAG, this.index + 1)
        } else if (!isWhitespace(c)) {
            this.state = State.BeforeAttributeName
            this.stateBeforeAttributeName(c)
        }
    }

    private fun stateInAttributeName(c: Int) {
        if (c == CharCodes.Eq.code || isEndOfTagSection(c)) {
            this.callbacks.onAttribName(this.sectionStart, this.index)
            this.sectionStart = this.index
            this.state = State.AfterAttributeName
            this.stateAfterAttributeName(c)
        }
    }

    private fun stateAfterAttributeName(c: Int) {
        if (c == CharCodes.Eq.code) {
            this.state = State.BeforeAttributeValue
        } else if (c == CharCodes.Slash.code || c == CharCodes.Gt.code) {
            this.callbacks.onAttribEnd(KsoupHtmlParser.QuoteType.NoValue, this.sectionStart)
            this.sectionStart = -1
            this.state = State.BeforeAttributeName
            this.stateBeforeAttributeName(c)
        } else if (!isWhitespace(c)) {
            this.callbacks.onAttribEnd(KsoupHtmlParser.QuoteType.NoValue, this.sectionStart)
            this.state = State.InAttributeName
            this.sectionStart = this.index
        }
    }

    private fun stateBeforeAttributeValue(c: Int) {
        if (c == CharCodes.DoubleQuote.code) {
            this.state = State.InAttributeValueDq
            this.sectionStart = this.index + 1
        } else if (c == CharCodes.SingleQuote.code) {
            this.state = State.InAttributeValueSq
            this.sectionStart = this.index + 1
        } else if (!isWhitespace(c)) {
            this.sectionStart = this.index
            this.state = State.InAttributeValueNq
            this.stateInAttributeValueNoQuotes(c) // Re-Consume token
        }
    }

    private fun handleInAttributeValue(c: Int, quote: Int) {
        if (
            c == quote ||
            (!this.decodeEntities && this.fastForwardTo(quote))
        ) {
            this.callbacks.onAttribData(this.sectionStart, this.index)
            this.sectionStart = -1
            this.callbacks.onAttribEnd(
                if (quote == CharCodes.DoubleQuote.code)
                    KsoupHtmlParser.QuoteType.Double
                else
                    KsoupHtmlParser.QuoteType.Single,
                this.index + 1
            )
            this.state = State.BeforeAttributeName
        } else if (this.decodeEntities && c == CharCodes.Amp.code) {
            this.startEntity()
        }
    }
    private fun stateInAttributeValueDoubleQuotes(c: Int) {
        this.handleInAttributeValue(c, CharCodes.DoubleQuote.code)
    }
    private fun stateInAttributeValueSingleQuotes(c: Int) {
        this.handleInAttributeValue(c, CharCodes.SingleQuote.code)
    }
    private fun stateInAttributeValueNoQuotes(c: Int) {
        if (isWhitespace(c) || c == CharCodes.Gt.code) {
            this.callbacks.onAttribData(this.sectionStart, this.index)
            this.sectionStart = -1
            this.callbacks.onAttribEnd(KsoupHtmlParser.QuoteType.Unquoted, this.index)
            this.state = State.BeforeAttributeName
            this.stateBeforeAttributeName(c)
        } else if (this.decodeEntities && c == CharCodes.Amp.code) {
            this.startEntity()
        }
    }

    private fun stateBeforeDeclaration(c: Int) {
        if (c == CharCodes.OpeningSquareBracket.code) {
            this.state = State.CDATASequence
            this.sequenceIndex = 0
        } else {
            this.state = if (c == CharCodes.Dash.code)
                State.BeforeComment
            else
                State.InDeclaration
        }
    }

    private fun stateInDeclaration(c: Int) {
        if (c == CharCodes.Gt.code || this.fastForwardTo(CharCodes.Gt.code)) {
            this.callbacks.onDeclaration(this.sectionStart, this.index)
            this.state = State.Text
            this.sectionStart = this.index + 1
            this.emitSafePoint(KsoupSafePoint.AFTER_DECLARATION, this.index + 1)
        }
    }

    private fun stateInProcessingInstruction(c: Int) {
        if (c == CharCodes.Gt.code || this.fastForwardTo(CharCodes.Gt.code)) {
            this.callbacks.onProcessingInstruction(this.sectionStart, this.index)
            this.state = State.Text
            this.sectionStart = this.index + 1
            this.emitSafePoint(KsoupSafePoint.AFTER_PROCESSING_INSTRUCTION, this.index + 1)
        }
    }

    @OptIn(ExperimentalUnsignedTypes::class)
    private fun stateBeforeComment(c: Int) {
        if (c == CharCodes.Dash.code) {
            this.state = State.InCommentLike
            this.currentSequence = Sequences.CommentEnd
            // Allow short comments (eg. <!-->)
            this.sequenceIndex = 2
            this.sectionStart = this.index + 1
        } else {
            this.state = State.InDeclaration
        }
    }

    @OptIn(ExperimentalUnsignedTypes::class)
    private fun stateInSpecialComment(c: Int) {
        currentSequence?.let { currentSequence ->
            if (c == CharCodes.Gt.code) {
                if (sequenceIndex == currentSequence.size - 1) {
                    callbacks.onComment(sectionStart, index - currentSequence.size + 1, 3)
                    sectionStart = -1
                    state = State.Text
                    emitSafePoint(KsoupSafePoint.AFTER_COMMENT, index + 1)
                }
            } else if (c != currentSequence[sequenceIndex].toInt()) {
                state = State.InTagName
                stateInTagName(c) // Re-Consume the character
            }
        }
    }

    @OptIn(ExperimentalUnsignedTypes::class)
    private fun stateBeforeSpecialS(c: Int) {
        when (c or 0x20) {
            Sequences.ScriptEnd[3].toInt() -> {
                this.startSpecial(Sequences.ScriptEnd, 4)
            }
            Sequences.StyleEnd[3].toInt() -> {
                this.startSpecial(Sequences.StyleEnd, 4)
            }
            else -> {
                this.state = State.InTagName
                this.stateInTagName(c) // Consume the token again
            }
        }
    }

    @OptIn(ExperimentalUnsignedTypes::class)
    private fun stateBeforeSpecialT(c: Int) {
        when (c or 0x20) {
            Sequences.TitleEnd[3].toInt() -> {
                this.startSpecial(Sequences.TitleEnd, 4)
            }
            Sequences.TextareaEnd[3].toInt() -> {
                this.startSpecial(Sequences.TextareaEnd, 4)
            }
            else -> {
                this.state = State.InTagName
                this.stateInTagName(c) // Consume the token again
            }
        }
    }

    private fun startEntity() {
        this.baseState = this.state
        this.state = State.InEntity
        this.entityStart = this.index
    }

    private fun stateInEntity(c: Int) {
        if (c == CharCodes.Semi.code) {
            val decoded = KsoupEntities.decodeHtml(
                this.buffer.substring(this.entityStart - this.offset, this.index - this.offset + 1)
            )

            this.state = this.baseState
            if (decoded.isEmpty()) {
                this.index = this.entityStart
            } else {
                emitCodePoint(decoded.first().code, this.index + 1 - this.entityStart)
            }
        }

        if (
            this.index + 1 - this.entityStart > LONGEST_HTML_ENTITY_LENGTH ||
            !isInEntityChar(c)
        ) {
            this.state = this.baseState
            this.index = this.entityStart
        }
    }

    /**
     * Remove data that has already been consumed from the buffer.
     */
    private fun cleanup() {
        // If we are inside text or attributes, emit what we already have.
        if (this.running && this.sectionStart != this.index) {
            if (
                this.state == State.Text ||
                (this.state == State.InSpecialTag && this.sequenceIndex == 0)
            ) {
                this.callbacks.onText(this.sectionStart, this.index)
                this.sectionStart = this.index
            } else if (
                this.state == State.InAttributeValueDq ||
                this.state == State.InAttributeValueSq ||
                this.state == State.InAttributeValueNq
            ) {
                this.callbacks.onAttribData(this.sectionStart, this.index)
                this.sectionStart = this.index
            }
        }
    }

    private fun shouldContinue(): Boolean {
        if (!this.running) {
            if (this.pausedAtBoundary && this.pauseBoundary in 0..this.index) {
                // Re-anchor the parse front at the safe point boundary; the
                // loop will resume exactly there after resume().
                this.index = this.pauseBoundary
            }
            return false
        }
        return this.index < this.buffer.length + this.offset
    }

    /**
     * Iterates through the buffer, calling the function corresponding to the current state.
     *
     * States that are more likely to be hit are higher up, as a performance improvement.
     */
    private fun parse() {
        while (this.shouldContinue()) {
            val c = this.buffer[this.index - this.offset].code
            when (this.state) {
                State.Text ->
                    this.stateText(c)
                State.SpecialStartSequence ->
                    this.stateSpecialStartSequence(c)
                State.InSpecialTag ->
                    this.stateInSpecialTag(c)
                State.CDATASequence ->
                    this.stateCDATASequence(c)
                State.InAttributeValueDq ->
                    this.stateInAttributeValueDoubleQuotes(c)
                State.InAttributeName ->
                    this.stateInAttributeName(c)
                State.InCommentLike ->
                    this.stateInCommentLike(c)
                State.InSpecialComment ->
                    this.stateInSpecialComment(c)
                State.BeforeAttributeName ->
                    this.stateBeforeAttributeName(c)
                State.InTagName ->
                    this.stateInTagName(c)
                State.InClosingTagName ->
                    this.stateInClosingTagName(c)
                State.BeforeTagName ->
                    this.stateBeforeTagName(c)
                State.AfterAttributeName ->
                    this.stateAfterAttributeName(c)
                State.InAttributeValueSq ->
                    this.stateInAttributeValueSingleQuotes(c)
                State.BeforeAttributeValue ->
                    this.stateBeforeAttributeValue(c)
                State.BeforeClosingTagName ->
                    this.stateBeforeClosingTagName(c)
                State.AfterClosingTagName ->
                    this.stateAfterClosingTagName(c)
                State.BeforeSpecialS ->
                    this.stateBeforeSpecialS(c)
                State.BeforeSpecialT ->
                    this.stateBeforeSpecialT(c)
                State.InAttributeValueNq ->
                    this.stateInAttributeValueNoQuotes(c)
                State.InSelfClosingTag ->
                    this.stateInSelfClosingTag(c)
                State.InDeclaration ->
                    this.stateInDeclaration(c)
                State.BeforeDeclaration ->
                    this.stateBeforeDeclaration(c)
                State.BeforeComment ->
                    this.stateBeforeComment(c)
                State.InProcessingInstruction ->
                    this.stateInProcessingInstruction(c)
                State.InEntity ->
                    this.stateInEntity(c)
            }
            this.index++
        }
        this.cleanup()
    }

    private fun finish() {
        if (this.state == State.InEntity) {
            // Todo remove entityDecoder
//            this.entityDecoder.end()
            this.state = this.baseState
        }

        this.handleTrailingData()

        this.callbacks.onEnd()
        this.emitSafePoint(KsoupSafePoint.DOCUMENT_END, this.index)
    }

    /** Handle any trailing data. */
    @OptIn(ExperimentalUnsignedTypes::class)
    private fun handleTrailingData() {
        val endIndex = this.buffer.length + this.offset

        // If there is no remaining data, we are done.
        if (this.sectionStart >= endIndex) {
            return
        }

        if (this.state == State.InCommentLike) {
            if (this.currentSequence == Sequences.CdataEnd) {
                this.callbacks.onCData(this.sectionStart, endIndex, 0)
            } else {
                this.callbacks.onComment(this.sectionStart, endIndex, 0)
            }
        } else if (
            this.state == State.InTagName ||
            this.state == State.BeforeAttributeName ||
            this.state == State.BeforeAttributeValue ||
            this.state == State.AfterAttributeName ||
            this.state == State.InAttributeName ||
            this.state == State.InAttributeValueSq ||
            this.state == State.InAttributeValueDq ||
            this.state == State.InAttributeValueNq ||
            this.state == State.InClosingTagName ||
            this.state == State.InSelfClosingTag
        ) {
            /*
             * If we are currently in an opening or closing tag, us not calling the
             * respective callback signals that the tag should be ignored.
             */
        } else {
            this.callbacks.onText(this.sectionStart, endIndex)
        }
    }

    private fun emitCodePoint(cp: Int, consumed: Int) {
        if (
            this.baseState != State.Text &&
            this.baseState != State.InSpecialTag
        ) {
            if (this.sectionStart < this.entityStart) {
                this.callbacks.onAttribData(this.sectionStart, this.entityStart)
            }
            this.sectionStart = this.entityStart + consumed
            this.index = this.sectionStart - 1

            this.callbacks.onAttribEntity(cp, this.entityStart, this.sectionStart)
        } else {
            if (this.sectionStart < this.entityStart) {
                this.callbacks.onText(this.sectionStart, this.entityStart)
            }
            this.sectionStart = this.entityStart + consumed
            this.index = this.sectionStart - 1

            this.callbacks.onTextEntity(cp, this.entityStart, this.sectionStart)
        }
    }

    // region Checkpoint snapshot / restore

    /**
     * Plain, serializable view of everything the tokenizer needs to continue
     * after a process boundary. Only captured on a [isAtSafePoint], so the
     * state is always a plain text boundary: no partial entity, tag, comment,
     * CDATA or raw-text element is in progress.
     *
     * [boundary] is the absolute char offset of the first unconsumed
     * character. On restore, internal indices continue at [boundary], so the
     * resumed tokenizer emits the same absolute offsets as an uninterrupted
     * parse, even though it is fed only the suffix.
     */
    internal class Snapshot(
        val stateName: String,
        val baseStateName: String,
        val boundary: Int,
    )

    @OptIn(ExperimentalUnsignedTypes::class)
    internal fun snapshot(boundary: Int, allowTerminal: Boolean = false): Snapshot {
        if (!allowTerminal && !this.isAtSafePoint()) {
            throw KsoupCheckpointException(
                "No safe tokenizer state for a checkpoint (current state: ${this.state}). " +
                    "Checkpoints are only allowed on plain text boundaries."
            )
        }
        return Snapshot(
            stateName = this.state.name,
            baseStateName = this.baseState.name,
            boundary = boundary,
        )
    }

    @OptIn(ExperimentalUnsignedTypes::class)
    internal fun restore(snapshot: Snapshot) {
        val state = State.valueOf(snapshot.stateName)
        if (state != State.Text) {
            throw KsoupCheckpointException(
                "Refusing to restore a tokenizer in non-text state ${snapshot.stateName}; " +
                    "only text-boundary checkpoints are supported"
            )
        }
        this.state = state
        this.baseState = State.valueOf(snapshot.baseStateName)
        // Continue the absolute index space at the checkpoint boundary. The
        // first suffix write keeps this offset, so buffer[index - offset]
        // resolves to the first suffix character.
        this.offset = snapshot.boundary
        this.index = snapshot.boundary
        this.sectionStart = snapshot.boundary
        this.buffer = ""
        this.entityStart = snapshot.boundary
        this.isSpecial = false
        this.sequenceIndex = 0
        this.currentSequence = null
        this.running = true
        this.lastSafePointOffset = snapshot.boundary
        this.documentStartEmitted = true
        this.pausedAtBoundary = false
        this.pauseBoundary = snapshot.boundary
        this.finished = false
    }

    // endregion

    internal enum class CharCodes(val code: Int) {
        Tab(0x9), // "\t"
        NewLine(0xa), // "\n"
        FormFeed(0xc), // "\f"
        CarriageReturn(0xd), // "\r"
        Space(0x20), // " "
        ExclamationMark(0x21), // "!"
        Number(0x23), // "#"
        Amp(0x26), // "&"
        SingleQuote(0x27), // "'"
        DoubleQuote(0x22), // '"'
        Dash(0x2d), // "-"
        Slash(0x2f), // "/"
        Zero(0x30), // "0"
        Nine(0x39), // "9"
        Semi(0x3b), // ";"
        Lt(0x3c), // "<"
        Eq(0x3d), // "="
        Gt(0x3e), // ">"
        QuestionMark(0x3f), // "?"
        UpperA(0x41), // "A"
        LowerA(0x61), // "a"
        UpperF(0x46), // "F"
        LowerF(0x66), // "f"
        UpperZ(0x5a), // "Z"
        LowerZ(0x7a), // "z"
        LowerX(0x78), // "x"
        OpeningSquareBracket(0x5b), // "["
    }

    /** All the states the tokenizer can be in. */
    internal enum class State {
        Text,
        BeforeTagName, // After <
        InTagName,
        InSelfClosingTag,
        BeforeClosingTagName,
        InClosingTagName,
        AfterClosingTagName,

        // Attributes
        BeforeAttributeName,
        InAttributeName,
        AfterAttributeName,
        BeforeAttributeValue,
        InAttributeValueDq, // "
        InAttributeValueSq, // '
        InAttributeValueNq,

        // Declarations
        BeforeDeclaration, // !
        InDeclaration,

        // Processing instructions
        InProcessingInstruction, // ?

        // Comments & CDATA
        BeforeComment,
        CDATASequence,
        InSpecialComment,
        InCommentLike,

        // Special tags
        BeforeSpecialS, // Decide if we deal with `<script` or `<style`
        BeforeSpecialT, // Decide if we deal with `<title` or `<textarea`
        SpecialStartSequence,
        InSpecialTag,

        InEntity,
    }

    @OptIn(ExperimentalUnsignedTypes::class)
    private object Sequences {
        val Cdata = ubyteArrayOf(67u, 68u, 65u, 84u, 65u, 91u) // CDATA[

        val CdataEnd = ubyteArrayOf(93u, 93u, 62u) // ]]>

        val CommentEnd = ubyteArrayOf(45u, 45u, 62u) // `-->`

        val ScriptEnd = ubyteArrayOf(60u, 47u, 115u, 99u, 114u, 105u, 112u, 116u) // `</script`

        val StyleEnd = ubyteArrayOf(60u, 47u, 115u, 116u, 121u, 108u, 101u) // `</style`

        val TitleEnd = ubyteArrayOf(60u, 47u, 116u, 105u, 116u, 108u, 101u) // `</title`

        val TextareaEnd = ubyteArrayOf(60u, 47u, 116u, 101u, 120u, 116u, 97u, 114u, 101u, 97u) // `</textarea`
    }

    private companion object {
        const val LONGEST_HTML_ENTITY_LENGTH = "&CounterClockwiseContourIntegral;".length

        /**
         * Returns true if the given code point is a whitespace character.
         * [See this link](https://html.spec.whatwg.org/multipage/syntax.html#whitespace) for reference.
         *
         * @param c The code point to check.
         * @return True if the code point is a whitespace character.
         */
        fun isWhitespace(c: Int): Boolean {
            return (
                c == CharCodes.Space.code ||
                c == CharCodes.NewLine.code ||
                c == CharCodes.Tab.code ||
                c == CharCodes.FormFeed.code ||
                c == CharCodes.CarriageReturn.code
            )
        }

        fun isEndOfTagSection(c: Int): Boolean {
            return c == CharCodes.Slash.code || c == CharCodes.Gt.code || isWhitespace(c)
        }

        /**
         * Returns true if the given code point is a valid character for a tag name.
         * [See this link](https://html.spec.whatwg.org/multipage/syntax.html#tag-name-state) for reference.
         *
         * @param c The code point to check.
         * @return True if the code point is a valid character for a tag name.
         */
        fun isASCIIAlpha(c: Int): Boolean {
            return (
                (c >= CharCodes.LowerA.code && c <= CharCodes.LowerZ.code) ||
                (c >= CharCodes.UpperA.code && c <= CharCodes.UpperZ.code)
            )
        }

        /**
         * Returns true if the given code point is a valid character for a tag name.
         * [See this link](https://html.spec.whatwg.org/multipage/syntax.html#tag-name-state) for reference.
         *
         * @param c The code point to check.
         * @return True if the code point is a valid digit.
         */
        fun isDigit(c: Int): Boolean {
            return c >= CharCodes.Zero.code && c <= CharCodes.Nine.code
        }
    }

}
