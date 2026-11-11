package com.github.ajalt.mordant.internal

/**
 * Break [text] into extended grapheme clusters, following the rules of
 * [Unicode Standard Annex #29](https://www.unicode.org/reports/tr29/).
 *
 * Mordant never emits control characters other than ANSI escape sequences (which are stripped out
 * before text reaches a [com.github.ajalt.mordant.rendering.Span]), so only the rules needed for
 * printable text are implemented: CR/LF, control/extend/ZWJ/regional indicator/prepend and
 * extended pictographic handling.
 *
 * The returned list covers the whole string; each entry is a substring of exactly one grapheme
 * cluster. This is the atomic unit used by the animation differ: a cluster is never split across a
 * diff boundary, so combining marks, emoji ZWJ sequences and flag sequences are always rewritten
 * whole.
 */
internal fun graphemeClusters(text: String): List<String> {
    if (text.isEmpty()) return emptyList()
    val codePoints = codepointSequence(text).toList()
    if (codePoints.size == 1) return listOf(text)

    val clusters = mutableListOf<String>()
    val builder = StringBuilder()
    builder.append(codepointToString(codePoints[0]))
    var prev = codePoints[0]
    var riCount = if (isRegionalIndicator(prev)) 1 else 0
    var inPictographic = isExtendedPictographic(prev)

    for (i in 1..codePoints.lastIndex) {
        val cp = codePoints[i]
        val broke = shouldBreak(prev, cp, riCount, inPictographic)
        if (broke) {
            clusters += builder.toString()
            builder.setLength(0)
            riCount = 0
        }
        builder.append(codepointToString(cp))
        when {
            isRegionalIndicator(cp) -> riCount += 1
            else -> riCount = 0
        }
        // GB11: an ExtPict Extend* ZWJ sequence stays one cluster. After a break, a fresh cluster
        // can itself start with an extended pictographic; otherwise the run only continues while
        // we see Extend or ZWJ codepoints.
        inPictographic = if (broke) {
            isExtendedPictographic(cp)
        } else {
            inPictographic && (isExtend(cp) || isZWJ(cp))
        }
        prev = cp
    }
    clusters += builder.toString()
    return clusters
}

private fun shouldBreak(prev: Int, next: Int, riCount: Int, inPictographic: Boolean): Boolean {
    // GB3: Do not break between CR and LF
    if (prev == CR && next == LF) return false
    // GB4, GB5: Break after CR, LF, or control
    if (prev == CR || prev == LF || isControl(prev)) return true
    // GB4/GB5 counterpart: break before control
    if (next == CR || next == LF || isControl(next)) return true
    // GB6: Do not break Hangul syllable sequences (L x (L|V|LV|LVT))
    if (isHangulL(prev) && (isHangulL(next) || isHangulV(next) ||
                isHangulLV(next) || isHangulLVT(next))
    ) return false
    // GB7: (LV | V) x (V | T)
    if ((isHangulLV(prev) || isHangulV(prev)) && (isHangulV(next) || isHangulT(next))) return false
    // GB8: (LVT | T) x T
    if ((isHangulLVT(prev) || isHangulT(prev)) && isHangulT(next)) return false
    // GB9, GB9a: x (Extend | ZWJ | SpacingMark); spacing marks are zero width and so are covered
    // by the same width-based check as extend marks below
    if (isExtend(next) || isZWJ(next)) return false
    // GB9b: Prepend x
    if (isPrepend(prev)) return false
    // GB11: \p{Extended_Pictographic} Extend* ZWJ x \p{Extended_Pictographic}
    if (inPictographic && isZWJ(prev) && isExtendedPictographic(next)) return false
    // GB12, GB13: do not break inside pairs of regional indicators
    if (isRegionalIndicator(prev) && isRegionalIndicator(next) && riCount % 2 == 1) return false
    // GB999: otherwise, break
    return true
}

private const val CR = 0x000D
private const val LF = 0x000A
private const val ZWJ = 0x200D

private fun isZWJ(cp: Int): Boolean = cp == ZWJ

private fun isControl(cp: Int): Boolean {
    // The Grapheme_Cluster_Break=Control property, excluding CR and LF which are handled separately.
    return cp in 0..0x0008 || cp in 0x000B..0x000C || cp in 0x000E..0x001F ||
            cp in 0x007F..0x009F ||
            cp == 0x00AD || // soft hyphen
            cp == 0x180E ||
            cp == 0x200B || // zero width space
            cp in 0x200E..0x200F || // LRM, RLM
            cp in 0x2028..0x202E || cp in 0x2060..0x2064 || cp in 0x2066..0x206F ||
            cp == 0xFEFF || // BOM / zero width no-break space
            cp in 0xFFF9..0xFFFB ||
            cp in 0x1D173..0x1D17A
}

private fun isExtend(cp: Int): Boolean {
    if (cp == ZWJ || cp == 0x200C) return true // ZWJ, ZWNJ
    if (isRegionalIndicator(cp)) return false // regional indicators have their own rule
    if (cp in 0xFE00..0xFE0F) return true // variation selectors
    if (cp in 0x1F3FB..0x1F3FF) return true // emoji skin tone modifiers
    if (isControl(cp)) return false
    // Extend and SpacingMark codepoints (combining marks, enclosing marks such as the combining
    // keycap, etc.) all render at zero terminal width. Anything else at width zero that reaches
    // here is a formatting mark that likewise attaches to the base cluster.
    return cellWidth(cp) == 0
}

private fun isPrepend(cp: Int): Boolean {
    // The Prepend GCB codepoints that can occur in normal text.
    return cp in 0x0600..0x0605 || cp == 0x061C || cp == 0x06DD || cp == 0x070F ||
            cp == 0x0890 || cp == 0x0891 || cp == 0x08E2 ||
            cp == 0x110BD || cp == 0x110CD || cp in 0x13430..0x13438 ||
            cp in 0x1BCA0..0x1BCA3 || cp == 0xE0001 ||
            cp in 0xE0020..0xE007F
}

private fun isRegionalIndicator(cp: Int): Boolean = cp in 0x1F1E6..0x1F1FF

private fun isExtendedPictographic(cp: Int): Boolean {
    // Ranges from the emoji-data.txt Extended_Pictographic property list (Unicode 16), flattened
    // to contiguous blocks. This is intentionally conservative: an unknown pictographic codepoint
    // simply breaks before the following ZWJ, costing one extra redraw.
    return cp == 0x00A9 || cp == 0x00AE ||
            cp == 0x203C || cp == 0x2049 || cp == 0x2122 || cp == 0x2139 ||
            cp in 0x2194..0x2199 || cp in 0x21A9..0x21AA ||
            cp in 0x231A..0x231B || cp == 0x2328 || cp == 0x23CF ||
            cp in 0x23E9..0x23F3 || cp in 0x23F8..0x23FA ||
            cp == 0x24C2 ||
            cp in 0x25AA..0x25AB || cp == 0x25B6 || cp == 0x25C0 ||
            cp in 0x25FB..0x25FE ||
            cp in 0x2600..0x2604 || cp == 0x260E || cp == 0x2611 ||
            cp in 0x2614..0x2615 || cp == 0x2618 || cp == 0x261D ||
            cp == 0x2620 || cp in 0x2622..0x2623 || cp == 0x2626 ||
            cp == 0x262A || cp in 0x262E..0x262F || cp in 0x2638..0x263A ||
            cp == 0x2640 || cp == 0x2642 ||
            cp in 0x2648..0x2653 || cp in 0x265F..0x2660 || cp == 0x2663 ||
            cp in 0x2665..0x2666 || cp == 0x2668 || cp == 0x267B ||
            cp in 0x267E..0x267F || cp in 0x2692..0x2697 || cp == 0x2699 ||
            cp in 0x269B..0x269C || cp in 0x26A0..0x26A1 || cp == 0x26A7 ||
            cp in 0x26AA..0x26AB || cp in 0x26B0..0x26B1 || cp in 0x26BD..0x26BE ||
            cp in 0x26C4..0x26C5 || cp == 0x26C8 || cp in 0x26CE..0x26CF ||
            cp == 0x26D1 || cp in 0x26D3..0x26D4 || cp in 0x26E9..0x26EA ||
            cp in 0x26F0..0x26F5 || cp in 0x26F7..0x26FA || cp == 0x26FD ||
            cp == 0x2702 || cp == 0x2705 || cp in 0x2708..0x270D ||
            cp in 0x270F..0x2710 || cp == 0x2712 || cp == 0x2714 ||
            cp == 0x2716 || cp == 0x271D || cp == 0x2721 || cp == 0x2728 ||
            cp in 0x2733..0x2734 || cp == 0x2744 || cp == 0x2747 || cp == 0x274C ||
            cp == 0x274E || cp in 0x2753..0x2755 || cp == 0x2757 ||
            cp in 0x2763..0x2764 || cp in 0x2795..0x2797 || cp == 0x27A1 ||
            cp == 0x27B0 || cp == 0x27BF ||
            cp in 0x2934..0x2935 || cp in 0x2B05..0x2B07 || cp in 0x2B1B..0x2B1C ||
            cp == 0x2B50 || cp == 0x2B55 || cp == 0x3030 ||
            cp == 0x3297 || cp == 0x3299 ||
            cp in 0x1F000..0x1F0FF ||
            cp in 0x1F10D..0x1F10F || cp == 0x1F12F ||
            cp in 0x1F16C..0x1F171 || cp in 0x1F17E..0x1F17F ||
            cp == 0x1F18E || cp in 0x1F191..0x1F19A ||
            cp in 0x1F1AD..0x1F1E5 ||
            cp in 0x1F201..0x1F20F || cp == 0x1F21A || cp == 0x1F22F ||
            cp in 0x1F232..0x1F23A || cp in 0x1F23C..0x1F23F ||
            cp in 0x1F249..0x1F253 || cp in 0x1F25C..0x1F265 ||
            cp in 0x1F300..0x1F5FF ||
            cp in 0x1F600..0x1F64F || cp in 0x1F680..0x1F6FF ||
            cp in 0x1F774..0x1F77F || cp in 0x1F7D5..0x1F7FF ||
            cp in 0x1F80C..0x1F80F || cp in 0x1F848..0x1F84F ||
            cp in 0x1F85A..0x1F85F || cp in 0x1F888..0x1F88F ||
            cp in 0x1F8AE..0x1F8FF ||
            cp in 0x1F90C..0x1F93A || cp in 0x1F93C..0x1F945 ||
            cp in 0x1F947..0x1FAFF || cp in 0x1FC00..0x1FFFD
}

// Hangul syllable block boundaries, see UAX #29 GB6-GB8
private fun isHangulL(cp: Int): Boolean = cp in 0x1100..0x115F || cp in 0xA960..0xA97C
private fun isHangulV(cp: Int): Boolean = cp in 0x1160..0x11A7 || cp in 0xD7B0..0xD7C6
private fun isHangulT(cp: Int): Boolean = cp in 0x11A8..0x11FF || cp in 0xD7CB..0xD7FB
private fun isHangulLV(cp: Int): Boolean {
    if (cp !in 0xAC00..0xD7A3) return false
    return (cp - 0xAC00) % 28 == 0
}
private fun isHangulLVT(cp: Int): Boolean {
    if (cp !in 0xAC00..0xD7A3) return false
    return (cp - 0xAC00) % 28 != 0
}
