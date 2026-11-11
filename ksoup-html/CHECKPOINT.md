# Checkpoint / Resume

Ksoup can serialize the state of a partially completed parse to a plain,
versioned string and resume it in another process, producing exactly the same
token stream, element tree and source offsets as an uninterrupted parse.

The API is experimental and opt-in:

```kotlin
val parser = KsoupHtmlParser(
    checkpointEnabled = true,
    encoding = KsoupCheckpointEncoding.UTF_8, // default
)
parser.write(chunk1)

// At a safe point, capture and persist a self-contained checkpoint.
val checkpoint = parser.checkpoint() ?: return
val serialized: String = KsoupCheckpoint.encode(checkpoint)

// ... later, possibly in a new process ...
val resumed = KsoupHtmlParser(checkpointEnabled = true)
val decoded = KsoupCheckpoint.decode(serialized)
resumed.restoreCheckpoint(decoded, consumedPrefix) // exact bytes/chars already fed
resumed.write(chunk2)
resumed.end()
```

## Safe points

A checkpoint is only valid on a **token boundary** where no construct is half
consumed. The tokenizer exposes these safe points:

| Safe point | Meaning |
|---|---|
| `DOCUMENT_START` | Before any input is consumed (structural, never pauses) |
| `TEXT_BOUNDARY` | All character data up to the boundary has been emitted |
| `AFTER_OPEN_TAG` | Just after an opening tag's `>` |
| `AFTER_CLOSE_TAG` | Just after a closing tag |
| `AFTER_SELF_CLOSING_TAG` | Just after a `/>` self-closing tag |
| `AFTER_COMMENT` | Just after a complete comment |
| `AFTER_CDATA` | Just after a complete CDATA section |
| `AFTER_DECLARATION` | Just after a markup declaration |
| `AFTER_PROCESSING_INSTRUCTION` | Just after a processing instruction |
| `DOCUMENT_END` | After the whole stream is finalized (structural, never pauses) |

The rule is deliberately conservative. Checkpoints are **never** offered while:

- an entity (`&amp;`, `&#8211;`, ...) is being read,
- a comment or CDATA section is open,
- a `<script>`/`<style>`/`<title>`/`<textarea>` raw-text region is open,
- a tag is half open (inside a name, attribute value, ...).

This is what makes the "宁可少给" guarantee hold: a resumed parse cannot
re-interpret a partially consumed construct. To walk the stream one safe point
at a time, construct the parser with `pauseAtSafePoints = true` and call
`resume()` between pauses. `DOCUMENT_START` and `DOCUMENT_END` are recorded in
`safePointHits` but never pause finalization.

### Foreign content

When `svg` or `math` content is open, the checkpoint serializes the full
element stack and the per-element foreign-context flags, so the resumed parser
reconstructs the identical insertion mode (including self-closing tag handling
inside SVG and the HTML integration points such as `foreignObject`,
`annotation-xml`, `title`, `desc`).

## Absolute spans

With `checkpointEnabled = true`, every token is available in
`parser.tokenEventLog` as a `KsoupTokenEvent` and every tree-building result in
`parser.nodeEventLog` as a `KsoupNodeEvent`, each carrying a `KsoupSpan`:

- `startOffset`/`endOffset`: absolute UTF-16 code-unit offsets over the whole
  logical input (end exclusive), independent of chunk and checkpoint
  boundaries;
- `startByteOffset`/`endByteOffset`: absolute UTF-8 byte offsets for the
  default UTF-8 encoding. For other encodings they mirror the char offsets.

Byte spans are finalized at `end()` against the complete chunk list, so they do
not depend on how the input was split. Surrogate pairs split by a chunk
boundary are counted as a single four-byte code point.

## Checkpoint schema and compatibility

The serialized form is line-oriented ASCII beginning with the magic header
`KSOUP-CP-<version>`. Current version:

```
KsoupCheckpoint.FORMAT_VERSION == 1
```

String/binary fields are Base64; numbers and booleans are plain text. Fields:

| Key | Contents |
|---|---|
| `safePoint` | safe point type |
| `boundary` | absolute char offset of the first unconsumed char |
| `consumedBytes` | UTF-8 byte count of the consumed prefix |
| `encoding` | bound `KsoupCheckpointEncoding` |
| `options` | the six `KsoupHtmlOptions` flags, in declaration order |
| `prefixDigest` | hex SHA-256 of the consumed prefix |
| `hashState` | Base64 snapshot of the streaming SHA-256 internal state |
| `suffix` | Base64 chars delivered but not yet tokenized |
| `tokenizerState`, `tokenizerBaseState` | tokenizer state names |
| `parser.*` | indices, tag/attribute scratch fields, element stack, foreign context |

Callbacks and object references are never serialized — only plain data.

A resumed parser rejects a checkpoint with `KsoupCheckpointException` when:

- the magic/version is unknown,
- the parser options or encoding differ,
- the consumed-prefix SHA-256 does not match,
- the boundary does not line up,
- the retained suffix would exceed the buffer limit.

### Buffer limit

`KsoupHtmlParser.MAX_BUFFER_CHARS` (1 MiB of UTF-16 by default) caps the
not-yet-tokenized suffix a checkpoint may retain. Above it, `checkpoint()`
fails rather than serializing an unbounded buffer; feed smaller chunks.

### Example: upgrade that breaks compatibility

A checkpoint written by a hypothetical future format cannot be read by this
build — there is no silent migration:

```kotlin
val future = KsoupCheckpoint.encode(checkpoint)
    .replaceFirst("KSOUP-CP-1", "KSOUP-CP-2")
KsoupCheckpoint.decode(future)
// throws KsoupCheckpointException:
// "Incompatible checkpoint version 2; this build supports only 1"
```

## Design notes

- The tokenizer keeps the absolute offset space continuous across resumes by
  restoring `offset`/`index`/`sectionStart` at the checkpoint boundary, so
  emitted indices are identical to a single-pass parse.
- Prefix digests are recomputed over the reassembled whole logical prefix
  (not incrementally per chunk) so a surrogate pair split across chunks hashes
  identically on both sides.
- SHA-256, UTF-8 and Base64 are implemented in pure Kotlin under
  `com.mohamedrejeb.ksoup.html.checkpoint`, with no platform-specific APIs, so
  the feature works on every Kotlin Multiplatform target.
