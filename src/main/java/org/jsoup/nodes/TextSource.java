package org.jsoup.nodes;

import org.jsoup.internal.StringUtil;
import org.jsoup.internal.TextSourceMap;
import org.jsoup.select.NodeTraversor;
import org.jsoup.select.NodeVisitor;
import org.jspecify.annotations.Nullable;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.Objects;

/**
 The decoded, whitespace-normalized text of an {@link Element}, together with a mapping from each output character
 back to the position in the original input source it came from.
 <p>
 Obtain a {@code TextSource} with {@link Element#textWithSource()}. The {@link #text()} of a {@code TextSource} is
 always exactly equal to the {@link Element#text()} of the element at the moment the {@code TextSource} was created.
 The returned object is a snapshot: subsequent changes to the DOM do not affect it.
 <p>
 Source positions are only tracked when the {@link org.jsoup.parser.Parser} was configured with
 {@link org.jsoup.parser.Parser#setTrackPosition(boolean)}. Text that was not tracked, that was created
 programmatically, or that was set with {@link TextNode#text(String)} (or {@link Element#text(String)}) has a source
 of {@code -1, -1}.
 <p>
 Each {@link Span} covers one Unicode code point of the output (a surrogate pair is one span), and the spans appear in
 output order. Output ranges are half-open UTF-16 offsets, and together they cover all of {@link #text()}. Source
 ranges are also half-open UTF-16 offsets into the original input. A character reference such as {@code &amp;} maps to
 its whole reference; a reference that expands to two code points (such as {@code &NotEqualTilde;}) maps both code
 points to the same reference. Retained whitespace maps to the first source character that contributed it. Spaces
 generated for layout (between block elements, and at {@code <br>}) map to {@code -1, -1}.
 <p>
 For example, to find where the company name in an article's text came from:
 <pre>{@code
 String html = "<p>Acme &amp; Co <b>Ltd</b></p>";
 Element p = Parser.htmlParser().setTrackPosition(true).parseInput(html, "").selectFirst("p");
 TextSource ts = p.textWithSource();

 int at = ts.text().indexOf("Ltd");
 TextSource.Span first = ts.spans().get(at); // spans are one per code point, in output order
 TextSource.Span last = ts.spans().get(at + "Ltd".length() - 1);
 if (first.sourceStart() >= 0 && last.sourceEnd() >= 0) {
     String inSource = html.substring(first.sourceStart(), last.sourceEnd()); // "Ltd"
     System.out.printf("%s came from [%s] at %d%n", "Ltd", inSource, first.sourceStart());
 }
 }</pre>

 @since 1.18.3
 @see Element#textWithSource()
 */
public class TextSource {
    private final String text;
    private final List<Span> spans;

    TextSource(String text, List<Span> spans) {
        this.text = text;
        this.spans = Collections.unmodifiableList(spans);
    }

    /**
     Get the normalized text. This is always equal to {@link Element#text()} of the element this was created from.
     @return the text
     */
    public String text() {
        return text;
    }

    /**
     Get the mapping for each Unicode code point of the {@link #text()}, in output order.
     <p>The output ranges of the spans are half-open, counted in Java UTF-16 units, and contiguously cover the whole
     of {@link #text()}. A source range of {@code -1, -1} indicates that the output was generated for layout (for
     example, the space inserted between block elements), or that its source position was not tracked.</p>
     @return an unmodifiable list of spans, one per Unicode code point
     */
    public List<Span> spans() {
        return spans;
    }

    /**
     A mapping for a single Unicode code point of the output text, to a half-open position range in the original input
     source. Positions are counted in Java UTF-16 units. A source range of {@code -1, -1} indicates no tracked source.
     */
    public static final class Span {
        private final int outputStart, outputEnd, sourceStart, sourceEnd;

        Span(int outputStart, int outputEnd, int sourceStart, int sourceEnd) {
            this.outputStart = outputStart;
            this.outputEnd = outputEnd;
            this.sourceStart = sourceStart;
            this.sourceEnd = sourceEnd;
        }

        /**
         Get the start of this code point in the output text, as a UTF-16 offset (inclusive).
         @return the output start offset
         */
        public int outputStart() {
            return outputStart;
        }

        /**
         Get the end of this code point in the output text, as a UTF-16 offset (exclusive).
         @return the output end offset
         */
        public int outputEnd() {
            return outputEnd;
        }

        /**
         Get the start of the source range in the original input, as a UTF-16 offset (inclusive), or {@code -1} if
         untracked.
         @return the source start offset
         */
        public int sourceStart() {
            return sourceStart;
        }

        /**
         Get the end of the source range in the original input, as a UTF-16 offset (exclusive), or {@code -1} if
         untracked.
         @return the source end offset
         */
        public int sourceEnd() {
            return sourceEnd;
        }

        @Override public boolean equals(@Nullable Object o) {
            if (this == o) return true;
            if (o == null || getClass() != o.getClass()) return false;
            Span span = (Span) o;
            return outputStart == span.outputStart && outputEnd == span.outputEnd
                && sourceStart == span.sourceStart && sourceEnd == span.sourceEnd;
        }

        @Override public int hashCode() {
            return Objects.hash(outputStart, outputEnd, sourceStart, sourceEnd);
        }

        @Override public String toString() {
            return "Span[" + outputStart + "-" + outputEnd + " <- " + sourceStart + "-" + sourceEnd + "]";
        }
    }

    /**
     Create the TextSource for an element, using the exact same traversal and whitespace normalization as
     {@link Element#text()}.
     */
    static TextSource of(Element element) {
        Accum accumulator = new Accum();
        NodeTraversor.traverse(accumulator, element);
        return accumulator.build();
    }

    private static final class Accum implements NodeVisitor {
        private final StringBuilder text = new StringBuilder();
        private int[] sourceStarts = new int[32];
        private int[] sourceEnds = new int[32];
        private int count; // number of code points appended

        @Override public void head(Node node, int depth) {
            if (node instanceof TextNode) {
                appendNormalisedText((TextNode) node);
            } else if (node instanceof Element) {
                Element element = (Element) node;
                if (text.length() > 0 &&
                    (element.isBlock() || element.nameIs("br")) &&
                    !TextNode.lastCharIsWhitespace(text))
                    appendLayoutSpace();
            }
        }

        @Override public void tail(Node node, int depth) {
            if (node instanceof Element) {
                Element element = (Element) node;
                Node next = node.nextSibling();
                if (element.isBlock() &&
                    (next instanceof TextNode || (next instanceof Element && !((Element) next).tag().formatAsBlock())) &&
                    !TextNode.lastCharIsWhitespace(text))
                    appendLayoutSpace();
            }
        }

        private void appendNormalisedText(TextNode textNode) {
            String whole = textNode.getWholeText();
            TextSourceMap map = textNode.sourceMap(); // null if untracked or its text no longer matches

            if (Element.preserveWhitespace(textNode.parentNode) || textNode instanceof CDataNode) {
                // raw, non-normalized whitespace
                int i = 0;
                while (i < whole.length()) {
                    int codePoint = whole.codePointAt(i);
                    int codePointLength = Character.charCount(codePoint);
                    appendCodePoint(codePoint, map, i, codePointLength);
                    i += codePointLength;
                }
                return;
            }

            boolean lastWasWhite = false;
            boolean reachedNonWhite = false;
            boolean stripLeading = TextNode.lastCharIsWhitespace(text);

            int i = 0;
            while (i < whole.length()) {
                int codePoint = whole.codePointAt(i);
                int codePointLength = Character.charCount(codePoint);
                if (StringUtil.isActuallyWhitespace(codePoint)) {
                    if ((stripLeading && !reachedNonWhite) || lastWasWhite) {
                        i += codePointLength;
                        continue;
                    }
                    // The retained space gets the source of the first whitespace character that contributed it.
                    if (map != null)
                        append(' ', map.sourceStart(i), map.sourceEnd(i + codePointLength - 1));
                    else
                        append(' ', -1, -1);
                    lastWasWhite = true;
                } else if (!StringUtil.isInvisibleChar(codePoint)) {
                    appendCodePoint(codePoint, map, i, codePointLength);
                    lastWasWhite = false;
                    reachedNonWhite = true;
                }
                i += codePointLength;
            }
        }

        private void appendCodePoint(int codePoint, @Nullable TextSourceMap map, int unit, int unitLength) {
            if (map != null)
                append(codePoint, map.sourceStart(unit), map.sourceEnd(unit + unitLength - 1));
            else
                append(codePoint, -1, -1);
        }

        private void appendLayoutSpace() {
            append(' ', -1, -1);
        }

        private void append(int codePoint, int sourceStart, int sourceEnd) {
            text.appendCodePoint(codePoint);
            if (count == sourceStarts.length) {
                int cap = count * 2;
                sourceStarts = Arrays.copyOf(sourceStarts, cap);
                sourceEnds = Arrays.copyOf(sourceEnds, cap);
            }
            sourceStarts[count] = sourceStart;
            sourceEnds[count] = sourceEnd;
            count++;
        }

        private TextSource build() {
            String full = text.toString();
            int trimStart = 0;
            int trimEnd = full.length();
            while (trimStart < trimEnd && full.charAt(trimStart) <= ' ') trimStart++;
            while (trimEnd > trimStart && full.charAt(trimEnd - 1) <= ' ') trimEnd--;

            List<Span> spans = new ArrayList<>(count);
            int codePointIndex = 0;
            int outputOffset = 0;
            int i = 0;
            while (i < full.length()) {
                int codePoint = full.codePointAt(i);
                int codePointLength = Character.charCount(codePoint);
                if (i >= trimStart && i + codePointLength <= trimEnd) {
                    int outputStart = outputOffset - trimStart;
                    spans.add(new Span(outputStart, outputStart + codePointLength,
                        sourceStarts[codePointIndex], sourceEnds[codePointIndex]));
                }
                i += codePointLength;
                outputOffset += codePointLength;
                codePointIndex++;
            }

            String trimmed = full.substring(trimStart, trimEnd);
            return new TextSource(trimmed, spans);
        }
    }
}
