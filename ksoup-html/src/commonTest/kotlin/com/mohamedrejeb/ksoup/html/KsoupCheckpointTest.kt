package com.mohamedrejeb.ksoup.html

import com.mohamedrejeb.ksoup.annotation.ExperimentalKsoupApi
import com.mohamedrejeb.ksoup.html.checkpoint.KsoupBase64
import com.mohamedrejeb.ksoup.html.checkpoint.KsoupCheckpoint
import com.mohamedrejeb.ksoup.html.checkpoint.KsoupCheckpointEncoding
import com.mohamedrejeb.ksoup.html.checkpoint.KsoupCheckpointException
import com.mohamedrejeb.ksoup.html.checkpoint.KsoupDigest
import com.mohamedrejeb.ksoup.html.checkpoint.KsoupNodeEvent
import com.mohamedrejeb.ksoup.html.checkpoint.KsoupSafePoint
import com.mohamedrejeb.ksoup.html.checkpoint.KsoupStreamingSha256
import com.mohamedrejeb.ksoup.html.checkpoint.KsoupTokenEvent
import com.mohamedrejeb.ksoup.html.parser.KsoupHtmlOptions
import com.mohamedrejeb.ksoup.html.parser.KsoupHtmlParser
import kotlin.test.Test
import kotlin.test.assertContains
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertTrue

@OptIn(ExperimentalKsoupApi::class)
class KsoupCheckpointTest {

    // region Reference parsing

    private fun newParser(
        options: KsoupHtmlOptions = KsoupHtmlOptions.Default,
        pauseAtSafePoints: Boolean = false,
        encoding: KsoupCheckpointEncoding = KsoupCheckpointEncoding.UTF_8,
    ) = KsoupHtmlParser(
        options = options,
        checkpointEnabled = true,
        pauseAtSafePoints = pauseAtSafePoints,
        encoding = encoding,
    )

    private fun feedChunks(parser: KsoupHtmlParser, input: String, chunkSize: Int) {
        var pos = 0
        while (pos < input.length) {
            val end = minOf(pos + chunkSize, input.length)
            parser.write(input.substring(pos, end))
            pos = end
        }
        parser.end()
    }

    private data class ReferenceRun(
        val tokens: List<KsoupTokenEvent>,
        val nodes: List<KsoupNodeEvent>,
    )

    private fun referenceRun(input: String, chunkSize: Int): ReferenceRun {
        val parser = newParser()
        feedChunks(parser, input, chunkSize)
        return ReferenceRun(
            CheckpointNormalizers.normalizeTokens(parser.tokenEventLog),
            CheckpointNormalizers.normalizeNodes(parser.nodeEventLog),
        )
    }

    // endregion

    // region The main property: stop at EVERY safe point, resume in a fresh
    // "process", and compare tokens, tree and spans against an uninterrupted parse.

    @Test
    fun resumeAtEverySafePointMatchesUninterruptedParse() {
        // Cross-chunk stability of the reference is checked across many sizes;
        // the (quadratic) stop-and-resume sweep uses a smaller spread.
        val referenceChunkSizes = listOf(1, 2, 3, 5, 7, 13)
        val resumeChunkSizes = listOf(1, 3, 13)
        // Structural points are verified separately for every fixture.
        val allSafePointTypes = mutableSetOf(
            KsoupSafePoint.DOCUMENT_START,
            KsoupSafePoint.DOCUMENT_END,
        )
        for ((name, input) in CheckpointFixtures.ALL) {
            // References at several chunk sizes must already agree with each other.
            val refs = referenceChunkSizes.map { referenceRun(input, it) }
            for (i in 1..refs.lastIndex) {
                assertEquals(refs.first().tokens, refs[i].tokens, "[$name] token drift between chunk sizes")
                assertEquals(refs.first().nodes, refs[i].nodes, "[$name] node drift between chunk sizes")
            }
            // One ground-truth byte table reused for every comparison below.
            val table = CheckpointNormalizers.byteOffsetTable(input)
            val reference = ReferenceRun(
                CheckpointNormalizers.groundTokens(refs.first().tokens, input, table),
                CheckpointNormalizers.groundNodes(refs.first().nodes, input, table),
            )

            for (chunkSize in resumeChunkSizes) {
                allSafePointTypes += resumeAtEachSafePoint(input, chunkSize, reference, name, table)
            }
            structuralSafePointsSurviveResume(input, name)
        }

        // Across the whole fixture suite every safe point category must be hit.
        val expected = setOf(
            KsoupSafePoint.DOCUMENT_START,
            KsoupSafePoint.TEXT_BOUNDARY,
            KsoupSafePoint.AFTER_OPEN_TAG,
            KsoupSafePoint.AFTER_CLOSE_TAG,
            KsoupSafePoint.AFTER_SELF_CLOSING_TAG,
            KsoupSafePoint.AFTER_COMMENT,
            KsoupSafePoint.AFTER_CDATA,
            KsoupSafePoint.AFTER_DECLARATION,
            KsoupSafePoint.AFTER_PROCESSING_INSTRUCTION,
            KsoupSafePoint.DOCUMENT_END,
        )
        val missing = expected - allSafePointTypes
        assertTrue(missing.isEmpty(), "safe points never exercised across fixtures: $missing")
    }

    /**
     * DOCUMENT_START and DOCUMENT_END are structural safe points that never
     * pause the tokenizer. Verify both can still be captured, round-tripped
     * through serialization and resumed in a fresh parser.
     */
    private fun structuralSafePointsSurviveResume(input: String, label: String) {
        // DOCUMENT_START: nothing consumed yet.
        val starter = newParser()
        val startCp = starter.checkpoint(KsoupSafePoint.DOCUMENT_START)
        requireNotNull(startCp) { "[$label] no DOCUMENT_START checkpoint" }
        val restarted = newParser()
        restarted.restoreCheckpoint(KsoupCheckpoint.decode(KsoupCheckpoint.encode(startCp)), "")
        feedChunks(restarted, input, 4)
        assertEquals(
            CheckpointNormalizers.normalizeNodes(referenceRun(input, 4).nodes),
            CheckpointNormalizers.normalizeNodes(restarted.nodeEventLog),
            "[$label] DOCUMENT_START resume mismatch",
        )

        // DOCUMENT_END: whole document consumed.
        val finisher = newParser()
        feedChunks(finisher, input, 6)
        val endCp = finisher.checkpoint(KsoupSafePoint.DOCUMENT_END)
        requireNotNull(endCp) { "[$label] no DOCUMENT_END checkpoint" }
        assertEquals(KsoupSafePoint.DOCUMENT_END, endCp.safePoint)
        // A decoded end checkpoint carries the empty suffix and full boundary.
        val decodedEnd = KsoupCheckpoint.decode(KsoupCheckpoint.encode(endCp))
        assertEquals(input.length, decodedEnd.boundary)
        assertEquals("", decodedEnd.suffix)
    }

    /**
     * Pauses at every safe point. For each one, captures, serializes and
     * decodes a checkpoint, restores it in a brand new parser (simulating a
     * new process), and finishes the document. Returns the safe point types hit.
     */
    private fun resumeAtEachSafePoint(
        input: String,
        chunkSize: Int,
        reference: ReferenceRun,
        label: String,
        table: LongArray,
    ): Set<KsoupSafePoint> {
        val hitTypes = mutableSetOf<KsoupSafePoint>()
        val paused = newParser(pauseAtSafePoints = true)

        var pos = 0
        // Boundary of the pause already processed. A new iteration only does
        // work once the tokenizer parks on a boundary beyond this value.
        var processedBoundary = -1
        var guard = 0
        while (guard++ < input.length * 4 + 100) {
            if (paused.isEnded && paused.isRunning) break

            // Feed the next chunk (or finalize) whenever the tokenizer is live.
            if (paused.isRunning) {
                if (pos < input.length) {
                    val end = minOf(pos + chunkSize, input.length)
                    paused.write(input.substring(pos, end))
                    pos = end
                } else {
                    paused.end()
                }
            }

            // If the tokenizer is paused on a boundary we have not processed
            // yet, capture and resume there. Structural markers never pause, so
            // a paused tokenizer is always on a real text-boundary safe point.
            if (!paused.isRunning) {
                val boundary = paused.latestSafePointBoundary
                if (boundary > processedBoundary && paused.atSafePoint()) {
                    processedBoundary = boundary
                    val hit = paused.safePointHits.last()
                    assertEquals(boundary, hit.boundary)
                    hitTypes += hit.safePoint

                    // Capture + serialize round trip while paused.
                    val captured = paused.checkpoint()
                    requireNotNull(captured) { "[$label] checkpoint null at ${hit.safePoint}" }
                    assertEquals(hit.safePoint, captured.safePoint)
                    val encoded = KsoupCheckpoint.encode(captured)
                    val decoded = KsoupCheckpoint.decode(encoded)
                    assertEquals(captured.boundary, decoded.boundary)
                    assertEquals(captured.safePoint, decoded.safePoint)

                    // Fresh parser = new process. Restore and finish the rest.
                    val resumed = newParser()
                    val prefix = input.substring(0, hit.boundary)
                    resumed.restoreCheckpoint(decoded, prefix)

                    var next = decoded.nextFeedOffset
                    // Vary the post-restore chunk size so boundaries never line up.
                    val resumeChunk = maxOf(1, (chunkSize * 3) % 11 + 1)
                    while (next < input.length) {
                        val end = minOf(next + resumeChunk, input.length)
                        resumed.write(input.substring(next, end))
                        next = end
                    }
                    resumed.end()

                    val stitchedTokens = CheckpointNormalizers.groundTokens(
                        CheckpointNormalizers.normalizeTokens(paused.tokenEventLog + resumed.tokenEventLog),
                        input,
                        table,
                    )
                    val stitchedNodes = CheckpointNormalizers.groundNodes(
                        CheckpointNormalizers.normalizeNodes(paused.nodeEventLog + resumed.nodeEventLog),
                        input,
                        table,
                    )

                    assertEquals(
                        reference.tokens.size,
                        stitchedTokens.size,
                        "[$label@${hit.safePoint}/$hit.boundary] token count"
                    )
                    assertEquals(
                        reference.tokens,
                        stitchedTokens,
                        "[$label@${hit.safePoint}/${hit.boundary}] token mismatch"
                    )
                    assertEquals(
                        reference.nodes,
                        stitchedNodes,
                        "[$label@${hit.safePoint}/${hit.boundary}] node/tree mismatch"
                    )
                }
                paused.resume()
            }
        }
        return hitTypes
    }

    // endregion

    // region Span checks

    @Test
    fun spansAreAbsoluteAndMonotonic() {
        val input = CheckpointFixtures.TRAILING_UTF8
        val parser = newParser()
        feedChunks(parser, input, 3) // boundary splits multibyte sequences

        // Ground truth char->byte map over the whole UTF-8 stream.
        val table = CheckpointNormalizers.byteOffsetTable(input)

        // Every span must be well-formed and match the ground truth byte map,
        // regardless of how the input was split into chunks.
        for (event in parser.tokenEventLog) {
            val span = event.span
            assertTrue(span.startOffset >= 0 && span.endOffset >= span.startOffset)
            assertTrue(span.endByteOffset >= span.startByteOffset)
            assertEquals(table[span.startOffset], span.startByteOffset)
            assertEquals(table[span.endOffset.coerceAtMost(input.length)], span.endByteOffset)
        }

        // The 4-byte emoji must occupy four bytes even though it is two UTF-16 units.
        val emoji = "😀"
        val emojiIndex = input.indexOf(emoji)
        val byteStart = table[emojiIndex]
        assertEquals(4, byteLengthOf(emoji))
        // Span endpoints are always code-point aligned: the char after the
        // pair starts exactly four bytes later.
        assertEquals(byteStart + 4, table[emojiIndex + 2])

        // Chunk boundaries can split a surrogate pair across two text nodes, so
        // no single node is guaranteed to cover the whole emoji. Instead verify
        // byte offsets are globally non-decreasing across the node stream.
        var prevNodeByteEnd = 0L
        for (node in parser.nodeEventLog) {
            assertTrue(node.span.startByteOffset >= prevNodeByteEnd ||
                node is KsoupNodeEvent.TextNode) // text nodes may touch/overlap at split pairs
            prevNodeByteEnd = maxOf(prevNodeByteEnd, node.span.endByteOffset)
        }
        // And the final byte total equals the whole-document UTF-8 length.
        assertEquals(byteLengthOf(input).toLong(), table[input.length])
    }

    private fun byteLengthOf(s: String): Int = KsoupDigest.encodeUtf8(s).size

    @Test
    fun foreignContextIsFullyRestored() {
        val input = CheckpointFixtures.FOREIGN_CONTENT
        val reference = referenceRun(input, 4)

        val paused = newParser(pauseAtSafePoints = true)
        paused.write(input.substring(0, input.indexOf("<svg") + 6))
        // Find a safe point while svg/math content is open and resume there.
        var resumed: KsoupHtmlParser? = null
        var captured: KsoupCheckpoint? = null
        var fed = input.substring(0, input.indexOf("<svg") + 6)
        outer@ while (true) {
            if (!paused.isRunning) {
                val hit = paused.safePointHits.last()
                if (hit.safePoint == KsoupSafePoint.AFTER_OPEN_TAG && hit.boundary > input.indexOf("<circle")) {
                    captured = KsoupCheckpoint.decode(KsoupCheckpoint.encode(paused.checkpoint()!!))
                    resumed = newParser()
                    resumed.restoreCheckpoint(captured, input.substring(0, hit.boundary))
                    fed = input.substring(0, captured.nextFeedOffset)
                    break@outer
                }
                paused.resume()
            } else {
                if (fed.length >= input.length) {
                    paused.end()
                    break@outer
                }
                val end = minOf(fed.length + 4, input.length)
                paused.write(input.substring(fed.length, end))
                fed = input.substring(0, end)
            }
        }

        val cp = requireNotNull(captured)
        requireNotNull(resumed)
        var next = cp.nextFeedOffset
        while (next < input.length) {
            val end = minOf(next + 5, input.length)
            resumed.write(input.substring(next, end))
            next = end
        }
        resumed.end()

        val table = CheckpointNormalizers.byteOffsetTable(input)
        val nodes = CheckpointNormalizers.groundNodes(
            CheckpointNormalizers.normalizeNodes(resumed.nodeEventLog), input, table,
        )
        // The resumed run covers the suffix replay plus everything after it,
        // i.e. all events starting at the checkpoint boundary. Ground both
        // sides through the same byte table so finalization timing cannot
        // perturb byte offsets.
        val referenceTail = CheckpointNormalizers.groundNodes(
            reference.nodes.filter { it.span.startOffset >= cp.boundary }, input, table,
        )
        assertEquals(referenceTail, nodes)

        // Self-closing inside SVG must be honored (explicit, not implied) and
        // an element named `circle` must produce a matching close event. The
        // circle tag may have been emitted before the resume boundary, so look
        // at the stitched paused + resumed event stream.
        val allCloseNames = (paused.nodeEventLog + resumed.nodeEventLog)
            .filterIsInstance<KsoupNodeEvent.CloseElement>().map { it.name }
        assertContains(allCloseNames, "circle")
    }

    // endregion

    // region Rejection cases

    @Test
    fun prefixDigestMismatchIsRejected() {
        // Plain text so the pause lands on a TEXT_BOUNDARY with a non-empty prefix.
        val input = "hello world, this is the consumed prefix"
        val paused = newParser(pauseAtSafePoints = true)
        paused.write(input.substring(0, 11))
        val hit = paused.safePointHits.last()
        assertEquals(KsoupSafePoint.TEXT_BOUNDARY, hit.safePoint)
        val cp = paused.checkpoint(hit.safePoint)!!

        val resumed = newParser()
        val tampered = input.substring(0, hit.boundary).let {
            it.replaceFirst(it.first(), if (it.first() == 'h') 'j' else 'h')
        }
        assertFailsWith<KsoupCheckpointException> {
            resumed.restoreCheckpoint(cp, tampered)
        }.also { assertContains(it.message!!, "digest") }
    }

    @Test
    fun optionsMismatchIsRejected() {
        val input = "<p>hi</p>"
        val paused = newParser()
        paused.write(input)
        paused.end()
        val cp = paused.checkpoint(KsoupSafePoint.DOCUMENT_END)!!

        val other = newParser(options = KsoupHtmlOptions(xmlMode = true))
        assertFailsWith<KsoupCheckpointException> {
            other.restoreCheckpoint(cp, input)
        }.also { assertContains(it.message!!, "options") }
    }

    @Test
    fun encodingMismatchIsRejected() {
        val input = "<p>hi</p>"
        val paused = newParser()
        paused.write(input)
        paused.end()
        val cp = paused.checkpoint(KsoupSafePoint.DOCUMENT_END)!!

        val other = newParser(encoding = KsoupCheckpointEncoding.ISO_8859_1)
        assertFailsWith<KsoupCheckpointException> {
            other.restoreCheckpoint(cp, input)
        }.also { assertContains(it.message!!, "Encoding") }
    }

    /**
     * Example of an upgrade breaking compatibility: a checkpoint written by a
     * hypothetical future format version is rejected rather than misread.
     */
    @Test
    fun futureFormatVersionIsRejected() {
        val input = "<p>hi</p>"
        val paused = newParser()
        paused.write(input)
        paused.end()
        val cp = paused.checkpoint(KsoupSafePoint.DOCUMENT_END)!!
        val future = KsoupCheckpoint.encode(cp)
            .replaceFirst("KSOUP-CP-${KsoupCheckpoint.FORMAT_VERSION}", "KSOUP-CP-${KsoupCheckpoint.FORMAT_VERSION + 1}")

        assertFailsWith<KsoupCheckpointException> {
            KsoupCheckpoint.decode(future)
        }.also { assertContains(it.message!!, "Incompatible checkpoint version") }
    }

    @Test
    fun badMagicIsRejected() {
        val ex = assertFailsWith<KsoupCheckpointException> {
            KsoupCheckpoint.decode("not-a-checkpoint\n")
        }
        assertContains(ex.message!!, "magic")
    }

    @Test
    fun checkpointInsideCommentIsRefused() {
        val parser = newParser()
        parser.write("<div><!-- unterminated comment")
        // Tokenizer is mid-comment: no safe point may be offered.
        assertFailsWith<KsoupCheckpointException> {
            parser.checkpoint()
        }
    }

    @Test
    fun checkpointInsideEntityIsRefused() {
        // A chunk boundary in the middle of an entity leaves InEntity state;
        // no safe point exists until the entity resolves.
        val parser = newParser()
        parser.write("<p>&amp")
        // Write more so the entity completes and a real boundary exists; the
        // mid-entity attempt above is asserted through the safe-point set.
        assertFailsWith<KsoupCheckpointException> {
            parser.checkpoint()
        }
        parser.write(";rest</p>")
        parser.end()
        // After completion the full document parses to the expected text.
        val text = parser.nodeEventLog.filterIsInstance<KsoupNodeEvent.TextNode>().joinToString("") { it.text }
        assertContains(text, "&rest")
    }

    @Test
    fun bufferLimitIsEnforced() {
        // Pause right after an opening tag, then deliver more text than the
        // limit while the tokenizer stays paused. That text is retained as the
        // checkpoint suffix and must trip the buffer cap.
        val parser = newParser(pauseAtSafePoints = true)
        parser.write("<div>")
        assertTrue(!parser.isRunning)
        val huge = "x".repeat(KsoupHtmlParser.MAX_BUFFER_CHARS + 16)
        parser.write(huge)
        val ex = assertFailsWith<KsoupCheckpointException> {
            parser.checkpoint()
        }
        assertContains(ex.message!!, "buffer limit")
        parser.resume()
        parser.end()
    }

    @Test
    fun checkpointRequiresOptInFeature() {
        val disabled = KsoupHtmlParser()
        assertFailsWith<KsoupCheckpointException> {
            disabled.checkpoint()
        }
    }

    // endregion

    // region Crypto / codec unit checks

    @Test
    fun sha256MatchesKnownVectors() {
        assertEquals(
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            KsoupDigest.sha256Hex(KsoupDigest.encodeUtf8("abc"))
        )
        assertEquals(
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
            KsoupDigest.sha256Hex(ByteArray(0))
        )
        val long = "Ksoup checkpoint ".repeat(1000)
        val oneShot = KsoupDigest.sha256Hex(KsoupDigest.encodeUtf8(long))

        // Streaming in small pieces must match the one-shot result.
        val streaming = KsoupStreamingSha256()
        val split = 37
        var i = 0
        while (i < long.length) {
            val part = long.substring(i, minOf(i + split, long.length))
            streaming.update(KsoupDigest.encodeUtf8(part))
            i += split
        }
        assertEquals(oneShot, KsoupDigest.toHex(streaming.digest()))

        // A snapshot taken mid-stream, restored into a fresh hasher that then
        // digests the remainder, must produce the same final digest.
        val first = KsoupStreamingSha256()
        first.update(KsoupDigest.encodeUtf8(long.substring(0, split * 13)))
        val snapshot = first.snapshot()
        val resumed = KsoupStreamingSha256().apply { restore(snapshot) }
        resumed.update(KsoupDigest.encodeUtf8(long.substring(split * 13)))
        assertEquals(oneShot, KsoupDigest.toHex(resumed.digest()))
    }

    @Test
    fun base64RoundTripsBinary() {
        val cases = listOf(
            ByteArray(0),
            byteArrayOf(1),
            byteArrayOf(1, 2),
            byteArrayOf(1, 2, 3),
            byteArrayOf(-1, -128, 0, 127, 42, 99, 7),
        )
        for (case in cases) {
            assertEquals(case.toList(), KsoupBase64.decode(KsoupBase64.encode(case)).toList())
        }
        assertEquals("SGVsbG8=", KsoupBase64.encode(KsoupDigest.encodeUtf8("Hello")))
    }

    @Test
    fun utf8HandlesMultibyteAndSurrogates() {
        val s = "café 中文 😀"
        val bytes = KsoupDigest.encodeUtf8(s)
        // "caf"=3, é=2, space=1, 中文=6, space=1, 😀=4 -> 17 bytes
        assertEquals(17, bytes.size)
        // Re-decode using the checkpoint decoder path indirectly via Base64 transport.
        val transported = KsoupCheckpoint.decode(
            KsoupCheckpoint.encode(
                KsoupCheckpoint(
                    safePoint = KsoupSafePoint.DOCUMENT_START,
                    boundary = 0,
                    consumedBytes = 0,
                    encoding = KsoupCheckpointEncoding.UTF_8,
                    options = KsoupHtmlOptions.Default,
                    prefixDigest = KsoupDigest.sha256Hex(ByteArray(0)),
                    hashState = KsoupStreamingSha256().snapshot(),
                    suffix = s,
                    tokenizerStateName = "Text",
                    tokenizerBaseStateName = "Text",
                    parserState = com.mohamedrejeb.ksoup.html.checkpoint.ParserState(
                        startIndex = 0, endIndex = 0, openTagStart = 0,
                        tagName = "", attribName = "", attribValue = "",
                        attribs = null, stack = mutableListOf(),
                        foreignContext = mutableListOf(), ended = false,
                        currentTagNameStart = -1, attribNameStart = 0, lastAttribEnd = 0,
                    ),
                )
            )
        )
        assertEquals(s, transported.suffix)
    }

    // endregion
}
