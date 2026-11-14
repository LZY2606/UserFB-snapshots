package org.jsoup.examples;

import org.jsoup.nodes.Element;
import org.jsoup.nodes.TextSource;
import org.jsoup.parser.Parser;

/**
 * Demonstrates {@link Element#textWithSource()}: mapping text extracted with {@code Element.text()} back to the
 * position it came from in the original HTML.
 * <p>
 * A common scraping task is to find a value in an element's normalized text (which has had entities decoded and
 * whitespace collapsed), and then mark that value in the original source. Searching the raw HTML for the value is not
 * reliable when the same text occurs more than once, or when the source used a character reference. {@code
 * textWithSource()} gives the exact source range for each output character.
 * </p>
 * <p>To invoke from the command line, assuming you have downloaded the jsoup jar to your current directory:</p>
 * <pre>{@code java -cp jsoup.jar org.jsoup.examples.TextSourceExample}</pre>
 */
public class TextSourceExample {
    public static void main(String... args) {
        String html = "<article>" +
            "<p>Our partner <b>Acme &amp; Co</b> announced the <i>Acme</i> deal.</p>" +
            "<p>The company name appears again: Acme &amp; Co.</p>" +
            "</article>";

        Element article = Parser.htmlParser()
            .setTrackPosition(true) // required for source positions
            .parseInput(html, "")
            .selectFirst("article");

        TextSource textSource = article.textWithSource();
        System.out.println("Text: " + textSource.text());

        // Find each occurrence of the company name. The same text appears twice, so a plain indexOf against the
        // original HTML could pick the wrong one; the spans tell us exactly where each occurrence came from.
        String company = "Acme & Co";
        int from = 0;
        while (true) {
            int at = textSource.text().indexOf(company, from);
            if (at < 0) break;
            from = at + company.length();

            // One span per code point, in output order, so the span for the first character starts here:
            TextSource.Span first = textSource.spans().get(at);
            TextSource.Span last = textSource.spans().get(at + company.length() - 1);

            // The source range of a contiguous match runs from the start of the first character's source to the end of
            // the last character's source. (A match can straddle tags, in which case those ranges are not contiguous.)
            String source = first.sourceStart() >= 0 && last.sourceEnd() >= 0
                ? html.substring(first.sourceStart(), last.sourceEnd())
                : "(no source)";
            System.out.printf("Found [%s] at text offset %d; came from [%s] at %d%n",
                company, at, source, first.sourceStart());
        }

        // Layout-generated spaces (such as those between block elements) have no source position:
        for (TextSource.Span span : textSource.spans()) {
            char c = textSource.text().charAt(span.outputStart());
            if (c == ' ' && span.sourceStart() < 0)
                System.out.println("Space at text offset " + span.outputStart() + " was generated for layout");
        }
    }
}
