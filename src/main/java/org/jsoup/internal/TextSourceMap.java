package org.jsoup.internal;

import java.util.Arrays;

/**
 An immutable map from each UTF-16 unit of a text node's content back to its position in the original input source.
 Positions are half-open, zero-based UTF-16 offsets. A position of {@code -1} indicates no source (for example, a
 synthetically created node, or text set via {@code TextNode.text(String)}).
 <p>The map remembers the text it was built for, so it can be ignored if the node's content is later changed by another
 route (such as {@code attr("#text", ...)}).</p>
 <p>This is an internal class. Do not rely on it as part of the public API.</p>

 @see org.jsoup.nodes.TextSource
 */
public final class TextSourceMap {
    private final String text;
    private final int[] starts;
    private final int[] ends;

    private TextSourceMap(String text, int[] starts, int[] ends) {
        this.text = text;
        this.starts = starts;
        this.ends = ends;
    }

    /** Get the text this map was built for. */
    public String text() {
        return text;
    }

    /** Get the number of UTF-16 units that this map covers. */
    public int length() {
        return starts.length;
    }

    /** Get the source start position of the given UTF-16 unit, or -1 if untracked. */
    public int sourceStart(int unit) {
        return starts[unit];
    }

    /** Get the source end position of the given UTF-16 unit, or -1 if untracked. */
    public int sourceEnd(int unit) {
        return ends[unit];
    }

    /**
     Create a map covering a sub-range of this map, for use when a text node is split.
     @param from start UTF-16 index, inclusive
     @param to end UTF-16 index, exclusive
     @return the sliced map
     */
    public TextSourceMap slice(int from, int to) {
        return new TextSourceMap(text.substring(from, to),
            Arrays.copyOfRange(starts, from, to),
            Arrays.copyOfRange(ends, from, to));
    }

    /** A growable builder, used by the Tokeniser while tracking positions. */
    public static final class Builder {
        private int[] starts = new int[64];
        private int[] ends = new int[64];
        private int size;

        private void ensure(int required) {
            if (required <= starts.length) return;
            int cap = Math.max(required, starts.length * 2);
            starts = Arrays.copyOf(starts, cap);
            ends = Arrays.copyOf(ends, cap);
        }

        /** Add a source range for the next UTF-16 unit. */
        public void add(int sourceStart, int sourceEnd) {
            ensure(size + 1);
            starts[size] = sourceStart;
            ends[size] = sourceEnd;
            size++;
        }

        /** Clear the builder, retaining its buffers. */
        public void reset() {
            size = 0;
        }

        /** Build an immutable map of the currently added units, or null if they do not match the text length. */
        public TextSourceMap buildOrNull(String text) {
            if (text.length() != size) return null;
            return new TextSourceMap(text, Arrays.copyOf(starts, size), Arrays.copyOf(ends, size));
        }
    }
}
