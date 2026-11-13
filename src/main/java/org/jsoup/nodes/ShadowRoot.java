package org.jsoup.nodes;

import org.jsoup.parser.Tag;
import org.jspecify.annotations.Nullable;

/**
 A ShadowRoot represents the shadow tree attached to a host {@link Element}, created when the HTML parser consumes a
 declarative shadow root (a {@code <template shadowrootmode="open">} that is a direct child of a valid host). See
 {@link org.jsoup.parser.Parser#setDeclarativeShadowRoots(boolean)} to enable.
 <p>The root is not part of the host's {@code childNodes} / {@code children} and holds no sibling position, so
 traversals, {@code select}, and {@code text} on the host (or its ancestors) do not cross into the shadow tree. The
 root itself supports the usual {@link Element} read APIs ({@code select}, {@code text}, {@code childNodes}, ...);
 queries made within the root do not see the host's ancestors. The root's {@link #baseUri()} and
 {@link #ownerDocument()} follow its {@link #host()}.</p>
 <p>Slot assignment, CSS layout, and script execution are out of scope; this is a tree-level model only.</p>
 @see Element#shadowRoot()
 @see Element#outerHtmlWithShadowRoots()
 @since 1.24.2
 */
public class ShadowRoot extends Element {
    @Nullable Element host; // the element this root is attached to; a back-reference, not a parent link

    ShadowRoot(Tag tag) {
        super(tag, null); // baseUri is inherited from the host
    }

    /**
     Get the host element that this shadow root is attached to.
     @return the host element
     */
    public @Nullable Element host() {
        return host;
    }

    @Override
    public String nodeName() {
        return "#shadowroot";
    }
}
