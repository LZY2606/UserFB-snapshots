import org.jsoup.nodes.Element;
import org.jsoup.nodes.TextNode;
import org.jsoup.nodes.TextSource;
import org.jsoup.parser.Parser;
import task_support.Checks;

import java.io.IOException;
import java.io.Reader;
import java.io.StringReader;

/**
 Contract regression tests for {@code Element.textWithSource()} and {@code TextSource}.
 <p>Covers:</p>
 <ul>
 <li>ENTITIES: character references, including references that expand to two code points, and repeated text that cannot
 be disambiguated by searching.</li>
 <li>LAYOUT: whitespace collapsed across tags, and layout-generated spaces at block and {@code br} boundaries.</li>
 <li>READER: a streaming Reader that cannot reset, with reads that straddle buffer boundaries, and the source
 positions of text that table error recovery re-parents.</li>
 <li>DOM: cloning, moving, splitting, and rewriting nodes; untracked and programmatically created text; snapshots; and
 parser reuse.</li>
 </ul>
 */
public final class TextSourceRegression {

    public static void run() {
        Checks.group("ENTITIES", TextSourceRegression::entities);
        Checks.group("LAYOUT", TextSourceRegression::layout);
        Checks.group("READER", TextSourceRegression::reader);
        Checks.group("DOM", TextSourceRegression::dom);
    }

    // ------------------------------------------------------------------ helpers

    private static Parser trackingParser() {
        return Parser.htmlParser().setTrackPosition(true);
    }

    private static Element parse(String html) {
        return trackingParser().parseInput(html, "").body();
    }

    private static Element parseTrackedReader(Reader reader) {
        return trackingParser().parseInput(reader, "").body();
    }

    /** Assert the structural contract: text equality, one span per code point, contiguous half-open output ranges. */
    private static void assertWellFormed(Element el, TextSource ts) {
        Checks.check(ts.text().equals(el.text()),
            "TextSource text must equal Element.text() at the same moment: [" + ts.text() + "] vs [" + el.text() + "]");

        int expectedStart = 0;
        int codePoints = 0;
        for (TextSource.Span span : ts.spans()) {
            Checks.check(span.outputStart() == expectedStart,
                "span output ranges must be contiguous; expected " + expectedStart + " but was " + span.outputStart());
            int codePoint = ts.text().codePointAt(span.outputStart());
            Checks.check(span.outputEnd() - span.outputStart() == Character.charCount(codePoint),
                "each span must cover exactly one code point: " + span);
            if (span.sourceStart() >= 0)
                Checks.check(span.sourceEnd() >= span.sourceStart(),
                    "a tracked source range must not be reversed: " + span);
            else
                Checks.check(span.sourceEnd() == -1, "an untracked source must be -1,-1: " + span);
            expectedStart = span.outputEnd();
            codePoints++;
        }
        Checks.check(expectedStart == ts.text().length(),
            "spans must cover the whole text; covered to " + expectedStart + " of " + ts.text().length());
        Checks.check(codePoints == ts.text().codePointCount(0, ts.text().length()),
            "there must be one span per code point; got " + codePoints);
    }

    /** The span covering the output code point at the given offset. */
    private static TextSource.Span spanAt(TextSource ts, int outputStart) {
        for (TextSource.Span span : ts.spans())
            if (span.outputStart() <= outputStart && outputStart < span.outputEnd())
                return span;
        throw new AssertionError("no span covers output offset " + outputStart);
    }

    private static String sourceText(String html, TextSource.Span span) {
        if (span.sourceStart() < 0) return null;
        Checks.check(span.sourceStart() >= 0 && span.sourceEnd() <= html.length(),
            "source range must be inside the original input: " + span);
        return html.substring(span.sourceStart(), span.sourceEnd());
    }

    private static void checkSource(char expectedOutput, TextSource ts, String html, int outputStart, String expectedSource) {
        TextSource.Span span = spanAt(ts, outputStart);
        Checks.check(ts.text().charAt(outputStart) == expectedOutput,
            "unexpected output at " + outputStart + ": [" + ts.text().charAt(outputStart) + "]");
        Checks.check(expectedSource.equals(sourceText(html, span)),
            "source for output '" + expectedOutput + "' at " + outputStart + " should be [" + expectedSource
                + "] but was [" + sourceText(html, span) + "] (" + span + ")");
    }

    private static String repeat(String s, int n) {
        StringBuilder sb = new StringBuilder(s.length() * n);
        for (int i = 0; i < n; i++) sb.append(s);
        return sb.toString();
    }

    // ---------------------------------------------------------------- ENTITIES

    private static final String EntitiesHtml =
        "<p>A &amp; B<b> C</b><br>D &NotEqualTilde; &#x1F642;</p>";

    private static void entities() {
        Element p = parse(EntitiesHtml).selectFirst("p");
        Checks.check(p != null, "p element should parse");
        TextSource ts = p.textWithSource();
        assertWellFormed(p, ts);
        Checks.check(ts.text().equals("A & B C D ≂̸ 🙂"),
            "decoded and normalised text: [" + ts.text() + "]");

        // '&' from &amp; maps to the whole reference (including the leading &)
        checkSource('&', ts, EntitiesHtml, ts.text().indexOf('&'), "&amp;");

        // A numeric reference expands to a supplementary code point; its span covers one code point (two UTF-16 units)
        int smile = ts.text().indexOf('\uD83D');
        TextSource.Span smileSpan = spanAt(ts, smile);
        Checks.check(smileSpan.outputEnd() - smileSpan.outputStart() == 2,
            "a surrogate pair is one code point: " + smileSpan);
        Checks.check("&#x1F642;".equals(sourceText(EntitiesHtml, smileSpan)),
            "numeric reference source: " + smileSpan);

        // &NotEqualTilde; expands to two code points; both point back to the same single reference
        int tildeFirst = ts.text().indexOf('≂');
        TextSource.Span first = spanAt(ts, tildeFirst);
        TextSource.Span second = spanAt(ts, tildeFirst + 1);
        Checks.check(first.sourceStart() == second.sourceStart() && first.sourceEnd() == second.sourceEnd(),
            "both code points of an expansion must share one source range");
        Checks.check("&NotEqualTilde;".equals(sourceText(EntitiesHtml, first)),
            "both code points of an expansion map to the whole reference: " + first);

        // Repeated text: "Dup Dup Dup" must not be resolved by searching
        String dupHtml = "<p>Dup Dup Dup</p>";
        Element dupEl = parse(dupHtml).selectFirst("p");
        TextSource dup = dupEl.textWithSource();
        assertWellFormed(dupEl, dup);
        Checks.check(dup.text().equals("Dup Dup Dup"), "repeated text: [" + dup.text() + "]");
        int firstDup = dupHtml.indexOf("Dup");
        int secondDup = dupHtml.indexOf("Dup", firstDup + 1);
        int thirdDup = dupHtml.indexOf("Dup", secondDup + 1);
        checkSource('D', dup, dupHtml, 0, "D");
        Checks.check(spanAt(dup, 0).sourceStart() == firstDup,
            "first occurrence must point at the first source occurrence");
        Checks.check(spanAt(dup, 4).sourceStart() == secondDup,
            "second occurrence must point at the second source occurrence, not be found by search");
        Checks.check(spanAt(dup, 8).sourceStart() == thirdDup,
            "third occurrence must point at the third source occurrence");

        // The same company name in separate nodes must each resolve to its own source position
        String companyHtml = "<p><b>Acme</b> said <i>Acme</i> would buy Acme.</p>";
        Element companyEl = parse(companyHtml).selectFirst("p");
        TextSource company = companyEl.textWithSource();
        assertWellFormed(companyEl, company);
        Checks.check(company.text().equals("Acme said Acme would buy Acme."), "company text: [" + company.text() + "]");
        int occurrence1 = companyHtml.indexOf("Acme");
        int occurrence2 = companyHtml.indexOf("Acme", occurrence1 + 1);
        int occurrence3 = companyHtml.indexOf("Acme", occurrence2 + 1);
        Checks.check(spanAt(company, 0).sourceStart() == occurrence1, "bold Acme maps to the first source position");
        Checks.check(spanAt(company, 10).sourceStart() == occurrence2, "italic Acme maps to the second source position");
        Checks.check(spanAt(company, company.text().lastIndexOf("Acme")).sourceStart() == occurrence3,
            "plain Acme maps to the third source position");

        // Two adjacent identical references each map to their own reference, not a shared one
        String pairHtml = "<p>&NotEqualTilde;&NotEqualTilde;</p>";
        Element pairEl = parse(pairHtml).selectFirst("p");
        TextSource pair = pairEl.textWithSource();
        assertWellFormed(pairEl, pair);
        TextSource.Span pairFirst = spanAt(pair, 0);
        TextSource.Span pairSecond = spanAt(pair, 2);
        Checks.check(pairFirst.sourceStart() != pairSecond.sourceStart(),
            "adjacent references must not share a source range");
        Checks.check("&NotEqualTilde;".equals(sourceText(pairHtml, pairFirst))
            && "&NotEqualTilde;".equals(sourceText(pairHtml, pairSecond)),
            "each adjacent reference maps to its own occurrence");

        // Invisible characters are ignored by text(), so they get no span; punctuation stays
        String invisibleHtml = "<p>a&#8203;b&#173;c</p>";
        Element invisibleEl = parse(invisibleHtml).selectFirst("p");
        TextSource invisible = invisibleEl.textWithSource();
        assertWellFormed(invisibleEl, invisible);
        Checks.check(invisible.text().equals("abc"), "invisible characters are dropped: [" + invisible.text() + "]");
        Checks.check(invisible.spans().size() == 3, "no spans for dropped invisible characters");
        checkSource('b', invisible, invisibleHtml, 1, "b");

        // Missing semicolon still maps to the consumed reference text
        String semiHtml = "<p>&amp b</p>";
        Element semiEl = parse(semiHtml).selectFirst("p");
        TextSource semi = semiEl.textWithSource();
        assertWellFormed(semiEl, semi);
        checkSource('&', semi, semiHtml, 0, "&amp");
        checkSource('b', semi, semiHtml, 2, "b");
    }

    // ------------------------------------------------------------------ LAYOUT

    private static void layout() {
        // Collapsed whitespace across inline tags keeps the source of its first contributing character
        String inlineHtml = "<div>Hello   <b>there</b>   now! </div>";
        Element inlineEl = parse(inlineHtml).selectFirst("div");
        TextSource ts = inlineEl.textWithSource();
        assertWellFormed(inlineEl, ts);
        Checks.check(ts.text().equals("Hello there now!"), "collapsed text: [" + ts.text() + "]");
        int firstSpace = ts.text().indexOf(' ');
        TextSource.Span space = spanAt(ts, firstSpace);
        Checks.check(space.sourceStart() >= 0, "collapsed whitespace inside text is tracked");
        Checks.check(inlineHtml.substring(space.sourceStart(), space.sourceEnd()).trim().isEmpty(),
            "the retained space must point at a whitespace character in the source: " + space);
        Checks.check(space.sourceStart() == inlineHtml.indexOf("Hello") + "Hello".length(),
            "the retained space uses the first contributing whitespace character");
        checkSource('n', ts, inlineHtml, 12, "n");
        checkSource('!', ts, inlineHtml, 15, "!");

        // Leading and trailing whitespace is trimmed exactly as text(); trimmed characters get no span
        String trimHtml = "<p>  lead <i>mid</i> trail  </p>";
        Element trimEl = parse(trimHtml).selectFirst("p");
        TextSource trim = trimEl.textWithSource();
        assertWellFormed(trimEl, trim);
        Checks.check(trim.text().equals("lead mid trail"), "trimmed text: [" + trim.text() + "]");
        checkSource('l', trim, trimHtml, 0, "l");
        checkSource('l', trim, trimHtml, 13, "l");
        Checks.check(spanAt(trim, 0).sourceStart() == trimHtml.indexOf("lead"),
            "leading trim must not include the leading spaces in the source");

        // Block and br boundaries generate layout spaces with no source
        String blockHtml = "<div>One</div><div>Two</div>";
        Element blockEl = parse(blockHtml);
        TextSource block = blockEl.textWithSource();
        assertWellFormed(blockEl, block);
        Checks.check(block.text().equals("One Two"), "block join text: [" + block.text() + "]");
        TextSource.Span blockSpace = spanAt(block, 3);
        Checks.check(blockSpace.sourceStart() == -1 && blockSpace.sourceEnd() == -1,
            "a layout-generated block space must be untracked: " + blockSpace);

        String brHtml = "<div>a<br>b</div>";
        Element brEl = parse(brHtml).selectFirst("div");
        TextSource br = brEl.textWithSource();
        assertWellFormed(brEl, br);
        Checks.check(br.text().equals("a b"), "br join text: [" + br.text() + "]");
        TextSource.Span brSpace = spanAt(br, 1);
        Checks.check(brSpace.sourceStart() == -1 && brSpace.sourceEnd() == -1,
            "a layout-generated br space must be untracked: " + brSpace);
        checkSource('b', br, brHtml, 2, "b");

        // Nested blocks and text: each separator is layout, and real text keeps its source
        String nestedHtml = "<div>a<p>b</p>c</div>";
        Element nestedEl = parse(nestedHtml).selectFirst("div");
        TextSource nested = nestedEl.textWithSource();
        assertWellFormed(nestedEl, nested);
        Checks.check(nested.text().equals("a b c"), "nested block text: [" + nested.text() + "]");
        Checks.check(spanAt(nested, 1).sourceStart() == -1 && spanAt(nested, 3).sourceStart() == -1,
            "both block separators must be untracked");
        checkSource('a', nested, nestedHtml, 0, "a");
        checkSource('b', nested, nestedHtml, 2, "b");
        checkSource('c', nested, nestedHtml, 4, "c");

        // pre preserves whitespace exactly as text(); preserved spaces are real source characters
        String preHtml = "<pre>  a\n  b  </pre>";
        Element preEl = parse(preHtml).selectFirst("pre");
        TextSource pre = preEl.textWithSource();
        assertWellFormed(preEl, pre);
        Checks.check(pre.text().equals("a\n  b"), "pre text must match text(): [" + pre.text() + "]");
        checkSource('\n', pre, preHtml, 1, "\n");
        TextSource.Span preSpace = spanAt(pre, 2);
        Checks.check(preSpace.sourceStart() >= 0, "a preserved pre space is tracked");

        // Collapsing is per text node; a retained space after an inline element regenerates if needed
        String inlineSpaceHtml = "<div>a<span>  </span>b</div>";
        Element inlineSpaceEl = parse(inlineSpaceHtml).selectFirst("div");
        TextSource inlineSpace = inlineSpaceEl.textWithSource();
        assertWellFormed(inlineSpaceEl, inlineSpace);
        Checks.check(inlineSpace.text().equals("a b"), "inline whitespace-only text normalises: [" + inlineSpace.text() + "]");
        Checks.check(spanAt(inlineSpace, 1).sourceStart() >= 0,
            "the space from the inner text node keeps its source");
    }

    // ------------------------------------------------------------------ READER

    /** A Reader that dribbles characters and cannot be reset or re-read. */
    private static final class DribbleReader extends Reader {
        private final String content;
        private int pos;
        private final int chunk;

        DribbleReader(String content, int chunk) {
            this.content = content;
            this.chunk = chunk;
        }

        @Override public int read(char[] cbuf, int off, int len) {
            if (pos >= content.length()) return -1;
            int n = Math.min(Math.min(len, chunk), content.length() - pos);
            content.getChars(pos, pos + n, cbuf, off);
            pos += n;
            return n;
        }

        @Override public void close() {
        }

        @Override public boolean markSupported() {
            return false;
        }

        @Override public void reset() throws IOException {
            throw new IOException("reset is not supported");
        }
    }

    private static void reader() {
        // The entity and the duplicate text sit beyond several small read boundaries
        String html = "<p>" + repeat("filler ", 400) + "Alpha &amp; Beta Alpha</p>";
        Element p = parseTrackedReader(new DribbleReader(html, 7));
        TextSource ts;
        p = p.selectFirst("p");
        Checks.check(p != null, "p element should parse from a Reader");
        ts = p.textWithSource();
        assertWellFormed(p, ts);

        int secondAlpha = ts.text().lastIndexOf("Alpha");
        Checks.check(secondAlpha > 0, "the second Alpha should be present");
        TextSource.Span lastAlpha = spanAt(ts, secondAlpha);
        int expectedAlpha = html.lastIndexOf("Alpha");
        Checks.check(lastAlpha.sourceStart() == expectedAlpha,
            "the final Alpha must point at the last source occurrence, even across read boundaries");

        TextSource.Span amp = spanAt(ts, ts.text().indexOf('&'));
        Checks.check("&amp;".equals(sourceText(html, amp)),
            "the entity must map to its complete reference across a read boundary: " + amp);

        // A multi-code-point reference split across tiny reads still maps both code points to the whole reference
        String splitRefHtml = "<p>" + repeat("z ", 900) + "&NotEqualTilde; tail</p>";
        Element splitRefEl = parseTrackedReader(new DribbleReader(splitRefHtml, 2)).selectFirst("p");
        TextSource splitRef = splitRefEl.textWithSource();
        assertWellFormed(splitRefEl, splitRef);
        int tilde = splitRef.text().indexOf('≂');
        TextSource.Span tildeFirst = spanAt(splitRef, tilde);
        TextSource.Span tildeSecond = spanAt(splitRef, tilde + 1);
        Checks.check(tildeFirst.sourceStart() == splitRefHtml.indexOf("&NotEqualTilde;"),
            "the first code point of the split reference maps to the reference");
        Checks.check(tildeFirst.sourceStart() == tildeSecond.sourceStart()
            && tildeFirst.sourceEnd() == tildeSecond.sourceEnd(),
            "both code points of the split reference map to the same complete reference");

        // Table error recovery fosters content out of the table. The fostered node appears before the table in the DOM
        // (and so before its text in the output), although it comes later in the source. Its span must still point to
        // the later source position rather than being inferred from output order.
        String tableHtml = "<div><table><tr><td>a</td></tr><i>late</i></table></div>";
        Element div = parseTrackedReader(new StringReader(tableHtml)).selectFirst("div");
        TextSource table = div.textWithSource();
        assertWellFormed(div, table);

        int lateAt = table.text().indexOf("late");
        int cellAt = table.text().indexOf(" a") + 1; // the cell's 'a', after the "late" text
        Checks.check(lateAt >= 0 && cellAt > 0, "fostered and cell text should both be present: [" + table.text() + "]");
        Checks.check(lateAt < cellAt, "re-parented 'late' is expected before 'a' in the DOM text: [" + table.text() + "]");
        TextSource.Span lateSpan = spanAt(table, lateAt);
        Checks.check(lateSpan.sourceStart() == tableHtml.indexOf("late"),
            "fostered text must map to its original (later) source position, not to the output position");
        TextSource.Span cellSpan = spanAt(table, cellAt);
        Checks.check(cellSpan.sourceStart() == tableHtml.indexOf("<td>a") + "<td>".length(),
            "cell text must map to its original source position");

        // Stray text directly in the table body is also fostered
        String strayHtml = "<div><table>stray<tr><td>Cell</td></tr></table></div>";
        Element strayDiv = parseTrackedReader(new StringReader(strayHtml)).selectFirst("div");
        TextSource stray = strayDiv.textWithSource();
        assertWellFormed(strayDiv, stray);
        Checks.check(spanAt(stray, stray.text().indexOf("stray")).sourceStart() == strayHtml.indexOf("stray"),
            "fostered stray text keeps its source position");
        Checks.check(spanAt(stray, stray.text().indexOf("Cell")).sourceStart() == strayHtml.indexOf("Cell"),
            "cell text in a fostered table keeps its source position");

        // Text in a td that is still parsed within the table keeps its source
        String cellHtml = "<table><tr><td>Left</td><td>Right</td></tr></table>";
        Element tableEl = parseTrackedReader(new StringReader(cellHtml)).selectFirst("table");
        TextSource cells = tableEl.textWithSource();
        assertWellFormed(tableEl, cells);
        checkSource('L', cells, cellHtml, cells.text().indexOf("Left"), "L");
        checkSource('R', cells, cellHtml, cells.text().indexOf("Right"), "R");
        Checks.check(spanAt(cells, cells.text().indexOf("Left")).sourceStart() == cellHtml.indexOf("Left"),
            "left cell source position");
        Checks.check(spanAt(cells, cells.text().indexOf("Right")).sourceStart() == cellHtml.indexOf("Right"),
            "right cell source position");
    }

    // --------------------------------------------------------------------- DOM

    private static void dom() {
        String html = "<div><p>Alpha</p><p>Beta Gamma</p></div>";
        Element div = parse(html).selectFirst("div");

        // A snapshot is unaffected by later DOM changes
        TextSource snapshot = div.textWithSource();
        assertWellFormed(div, snapshot);
        Checks.check(snapshot.text().equals("Alpha Beta Gamma"), "snapshot text: [" + snapshot.text() + "]");
        String before = snapshot.text();
        int spansBefore = snapshot.spans().size();
        div.text("Replaced");
        Checks.check(snapshot.text().equals(before) && snapshot.spans().size() == spansBefore,
            "a TextSource snapshot must not change when the DOM changes");
        Checks.check(div.textWithSource().text().equals("Replaced"), "the new text is reflected in a new TextSource");

        // text(String) rewrites the text; sources become -1,-1
        Element rewritten = parse(html).selectFirst("p");
        rewritten.text("Rewritten");
        TextSource rewrittenSource = rewritten.textWithSource();
        assertWellFormed(rewritten, rewrittenSource);
        Checks.check(rewrittenSource.text().equals("Rewritten"), "rewritten text: [" + rewrittenSource.text() + "]");
        for (TextSource.Span span : rewrittenSource.spans())
            Checks.check(span.sourceStart() == -1 && span.sourceEnd() == -1, "rewritten text must be untracked");

        // A programmatically created TextNode is untracked but still outputs text normally
        Element made = new Element("p").text("Made Up");
        TextSource madeSource = made.textWithSource();
        assertWellFormed(made, madeSource);
        Checks.check(madeSource.text().equals("Made Up"), "programmatic text: [" + madeSource.text() + "]");
        Checks.check(madeSource.spans().size() == madeSource.text().length(),
            "one span per code point for programmatic text");
        for (TextSource.Span span : madeSource.spans())
            Checks.check(span.sourceStart() == -1, "programmatic text must be untracked");

        // Without position tracking, text is normal but sources are -1,-1
        Element untracked = Parser.htmlParser().parseInput("<p>Not Tracked</p>", "").selectFirst("p");
        TextSource untrackedSource = untracked.textWithSource();
        assertWellFormed(untracked, untrackedSource);
        Checks.check(untrackedSource.text().equals("Not Tracked"), "untracked parse text: [" + untrackedSource.text() + "]");
        for (TextSource.Span span : untrackedSource.spans())
            Checks.check(span.sourceStart() == -1 && span.sourceEnd() == -1, "untracked parse must map to -1,-1");

        // Clones retain their source positions
        Element original = parse(html).selectFirst("div");
        Element cloned = original.clone();
        TextSource clonedSource = cloned.textWithSource();
        assertWellFormed(cloned, clonedSource);
        Checks.check(clonedSource.text().equals("Alpha Beta Gamma"), "clone text: [" + clonedSource.text() + "]");
        int betaAt = clonedSource.text().indexOf("Beta");
        Checks.check(spanAt(clonedSource, betaAt).sourceStart() == html.indexOf("Beta"),
            "a clone must retain its original source positions");

        // Moving a node preserves its source
        Element moveDiv = parse(html).selectFirst("div");
        Element moved = moveDiv.selectFirst("p:nth-child(2)");
        moved.remove();
        moveDiv.appendChild(moved);
        TextSource movedSource = moved.textWithSource();
        assertWellFormed(moved, movedSource);
        Checks.check(movedSource.text().equals("Beta Gamma"), "moved text: [" + movedSource.text() + "]");
        int gammaAt = movedSource.text().indexOf("Gamma");
        Checks.check(spanAt(movedSource, gammaAt).sourceStart() == html.indexOf("Gamma"),
            "a moved node must keep the source positions it was parsed from");

        // splitText at a code point boundary: each side keeps its own source
        Element splitEl = parse("<p>Alpha</p>").selectFirst("p");
        TextNode textNode = (TextNode) splitEl.childNode(0);
        TextNode tail = textNode.splitText(2);
        Checks.check(textNode.getWholeText().equals("Al") && tail.getWholeText().equals("pha"),
            "splitText halves");
        TextSource splitSource = splitEl.textWithSource();
        assertWellFormed(splitEl, splitSource);
        Checks.check(spanAt(splitSource, 0).sourceStart() == "<p>Alpha</p>".indexOf('A'),
            "split head keeps the first source position");
        Checks.check(spanAt(splitSource, 2).sourceStart() == "<p>Alpha</p>".indexOf("Alpha") + 2,
            "split tail keeps positions from the tail of the original");

        // A split that would separate a surrogate pair cannot keep meaningful sources; the halves still output text,
        // but are untracked rather than reporting a misleading mapping
        Element surrogateEl = parse("<p>A🙂B</p>").selectFirst("p");
        TextNode surrogateNode = (TextNode) surrogateEl.childNode(0);
        surrogateNode.splitText(2); // between the high and low surrogate
        TextSource surrogateSource = surrogateEl.textWithSource();
        assertWellFormed(surrogateEl, surrogateSource);
        Checks.check(surrogateSource.text().equals("A🙂B"), "surrogate split preserves text");
        for (TextSource.Span span : surrogateSource.spans())
            Checks.check(span.sourceStart() == -1, "a split inside a surrogate pair must not report sources");

        // Text changed by another route (attr) invalidates the source map even if the length is unchanged
        Element attrEl = parse("<p>Original</p>").selectFirst("p");
        TextNode attrNode = (TextNode) attrEl.childNode(0);
        attrNode.attr("#text", "Modified"); // same length as "Original"
        TextSource attrSource = attrEl.textWithSource();
        assertWellFormed(attrEl, attrSource);
        Checks.check(attrSource.text().equals("Modified"), "attr-modified text: [" + attrSource.text() + "]");
        for (TextSource.Span span : attrSource.spans())
            Checks.check(span.sourceStart() == -1, "text changed outside text(String) must not keep stale sources");

        // A parser reused for different pages must not carry positions from the previous page
        Parser parser = trackingParser();
        String firstHtml = "<p>First &amp; One</p>";
        String secondHtml = "<p>Second &amp; Two</p>";
        Element first = parser.parseInput(firstHtml, "").selectFirst("p");
        Element second = parser.parseInput(secondHtml, "").selectFirst("p");
        TextSource firstSource = first.textWithSource();
        TextSource secondSource = second.textWithSource();
        assertWellFormed(first, firstSource);
        assertWellFormed(second, secondSource);
        Checks.check(spanAt(firstSource, 0).sourceStart() == firstHtml.indexOf("First"),
            "first page source position");
        Checks.check(spanAt(secondSource, 0).sourceStart() == secondHtml.indexOf("Second"),
            "a reused parser must report the second page's positions");
        Checks.check(!secondSource.text().equals(firstSource.text()),
            "reused parser pages must not share text");
    }
}
