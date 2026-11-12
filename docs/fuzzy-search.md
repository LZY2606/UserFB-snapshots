# Fuzzy search: how the distance is computed

`marisa::Trie::fuzzy_search(query, max_edits, stats)` returns every key in
the dictionary whose distance to `query` is at most `max_edits` (0, 1 or 2).

## Distance definition

The distance is the **Levenshtein distance over Unicode scalar values**
(codepoints) between the complete query and the complete key:

* Both strings are decoded from UTF-8 into sequences of codepoints before
  anything is measured. One multi-byte character is one position, so
  replacing `猫` (3 bytes) with `🐱` (4 bytes) costs 1, not 3 or 4.
* Each insertion, deletion or substitution of one codepoint costs 1.
  Adjacent transpositions are **not** a special case (they cost 2).
* No normalization, case folding or compatibility mapping is applied:
  `é` (U+00E9) and `e` + U+0301 are different strings, and `A` != `a`.
* An embedded `'\0'` byte is the ordinary codepoint U+0000; it does not
  terminate anything.
* The query must be strictly valid UTF-8 (no overlong forms, no surrogates,
  nothing above U+10FFFF), otherwise `std::invalid_argument` is thrown.
  Dictionary keys that are not valid UTF-8 — marisa dictionaries may hold
  arbitrary binary keys — simply never appear in the result; every other key
  is searched normally.

Results are sorted by distance and then by the unsigned byte order of the
original UTF-8 keys, and each match carries the same key `id` that
`lookup()` would return.

## How the search uses the trie

The search walks the compressed trie depth-first instead of enumerating the
keys. It maintains one Levenshtein dynamic-programming row per path: after
consuming a dictionary prefix *p*, `row[i]` is the distance between *p* and
the first *i* codepoints of the query. When a codepoint `c` is decoded, the
row is advanced with the standard recurrence

```
next[0]   = row[0] + 1
next[i]   = min(row[i] + 1,            // deletion from the dictionary side
                next[i-1] + 1,         // insertion relative to the query
                row[i-1] + (query[i-1] == c ? 0 : 1))
```

Two properties make the walk efficient and correct:

* **Pruning.** The minimum of a row is a lower bound on the distance
  between the query and *any* extension of the current prefix. Once that
  minimum exceeds `max_edits`, the whole subtree is skipped — long shared
  prefixes that are already too far away are never scanned.
* **Split-tolerant decoding.** A UTF-8 character may be cut by a node
  boundary, a compressed link or a tail segment. The decoder state travels
  with the search path and resumes across segment boundaries, so such a
  character is still decoded exactly once. A branch whose bytes are not
  valid UTF-8 is abandoned without affecting the decoder state of its
  siblings.

`FuzzyStats::visited_nodes` counts the top-level trie nodes the search
actually expanded, and `FuzzyStats::decoded_codepoints` counts the
dictionary codepoints that were fully decoded to advance the distance
state. Both are reset to zero at the start of every call. The search keeps
all of its temporary state on the stack, so concurrent calls on the same
`Trie` object (including read-only dictionaries obtained via `load()` or
`mmap()`) do not interfere with each other.
