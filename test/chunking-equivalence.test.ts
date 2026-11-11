import {expect, test} from 'vitest'

import {ParseError} from '../src/errors.ts'
import {createParser} from '../src/parse.ts'
import {EventSourceParserStream} from '../src/stream.ts'
import type {ParserCallbacks} from '../src/types.ts'

/**
 * Exhaustive chunking equivalence between the two parser entry points.
 *
 * The same SSE stream, no matter how it is sliced into chunks, must produce the
 * exact same callback trace from `createParser()` (direct) and from
 * `EventSourceParserStream` (TransformStream). Events, comments, retries, ids and
 * errors all count, and so does their order.
 *
 * One ordering subtlety: a TransformStream consumer cannot observe an enqueued
 * event before callbacks that fire later in the same `transform()` call — reads
 * resolve in microtasks, after the synchronous transform finishes. The stream
 * entry therefore observably produces, per fed chunk, all non-event callbacks
 * first and then all events. The direct harness below canonicalizes to the same
 * observable order (events of one `feed()` call are appended after that call's
 * other callbacks), which is the strongest equivalence the stream API can express.
 * Ordering across chunk boundaries is compared exactly.
 */

type TraceEntry =
  | {type: 'event'; id: string | undefined; event: string | undefined; data: string}
  | {type: 'comment'; comment: string}
  | {type: 'retry'; retry: number}
  | {type: 'id'; id: string}
  | {type: 'error'; errorType: string; message: string}

type ParserFactory = (callbacks: ParserCallbacks) => {feed(chunk: string): void}

const encoder = new TextEncoder()

/**
 * Inputs are kept short on purpose: every input is fed under every possible
 * chunking (see `cutPoints`), so cost grows quadratically with byte length.
 */
const inputs: Array<[name: string, input: string]> = [
  ['empty input', ''],
  ['single line feed', '\n'],
  ['single carriage return', '\r'],
  ['single CRLF', '\r\n'],
  ['leading BOM', '\uFEFFdata: bom\n\n'],
  ['raw byte-valued BOM', '\xEF\xBB\xBFdata: raw bom\n\n'],
  ['double BOM', '\uFEFF\uFEFFdata: bom\n\n'],
  ['mixed CR/LF/CRLF terminators', 'data: a\r\ndata: b\ndata: c\r\rdata: d\r\n\r\n'],
  ['CRLF-terminated events', 'data: a\r\ndata: b\r\n\r\n'],
  ['trailing CR at end of input', 'data: a\n\nretry: 5\r'],
  ['empty data field', 'data:\n\n'],
  ['data field with space-only value', 'data: \n\n'],
  ['field without colon', 'data\n\n'],
  ['multi-line data', 'data: a\ndata: b\ndata: c\n\n'],
  ['id containing NUL is ignored', 'id: a\0b\ndata: x\n\n'],
  ['id with NUL keeps previous id', 'id: ok\nid: a\0b\ndata: x\n\n'],
  ['id is reset between events', 'id: 1\ndata: a\n\ndata: b\n\n'],
  ['retry value overflow', 'retry: 99999999999999999999999\ndata: x\n\n'],
  ['invalid retry value', 'retry: 12a\ndata: x\n\n'],
  ['empty retry value', 'retry:\ndata: x\n\n'],
  ['unknown field', 'foo: bar\ndata: x\n\n'],
  ['comment lines', ': hello\n:world\n:\ndata: x\n\n'],
  ['custom event type', 'event: custom\ndata: x\n\n'],
  ['empty event field resets type', 'event: custom\nevent:\ndata: x\n\n'],
  ['multibyte characters', 'data: héllo ✔ ünïcodé\n\n'],
  ['no trailing blank line', 'id: 1\ndata: never'],
  ['no trailing newline at all', 'data: never'],
  ['retry persists across events', 'retry: 100\ndata: a\n\ndata: b\n\n'],
]

/**
 * Inputs up to this many bytes are checked under every conceivable chunking
 * (2^(n-1) ways). Longer inputs are checked under every 1-, 2- and 3-chunk
 * split plus the byte-at-a-time split.
 */
const EXHAUSTIVE_BYTE_LIMIT = 12

/** Yields every sorted array of byte cut points for the given input length. */
function* cutPoints(length: number): Generator<number[]> {
  const exhaustive = length <= EXHAUSTIVE_BYTE_LIMIT
  const maxParts = exhaustive ? Math.max(length, 1) : 3
  for (let parts = 1; parts <= maxParts; parts++) {
    yield* combinations(length - 1, parts - 1)
  }
  if (!exhaustive && length > 1) {
    // Byte-at-a-time: every byte its own chunk.
    yield Array.from({length: length - 1}, (_, index) => index + 1)
  }
}

/** Yields all ways to choose `k` cut positions from `1..n`, in sorted order. */
function* combinations(n: number, k: number): Generator<number[]> {
  if (k === 0) {
    yield []
    return
  }
  if (k > n) {
    return
  }
  const combo: number[] = []
  for (let i = 0; i < k; i++) {
    combo.push(i + 1)
  }
  for (;;) {
    yield [...combo]
    let i = k - 1
    while (i >= 0 && combo[i] === n - k + i + 1) {
      i--
    }
    if (i < 0) {
      return
    }
    combo[i] = (combo[i] ?? 0) + 1
    for (let j = i + 1; j < k; j++) {
      combo[j] = (combo[j - 1] ?? 0) + 1
    }
  }
}

/**
 * Slices the input bytes at the given cut points and decodes each chunk with a
 * streaming UTF-8 decoder. Decoding is pinned to `TextDecoder` in streaming
 * mode so the test does not depend on host stream timing (e.g. how
 * `TextDecoderStream` happens to group bytes).
 */
function toChunks(bytes: Uint8Array, cuts: number[]): string[] {
  const decoder = new TextDecoder('utf-8')
  const chunks: string[] = []
  let offset = 0
  for (const cut of cuts) {
    chunks.push(decoder.decode(bytes.subarray(offset, cut), {stream: true}))
    offset = cut
  }
  chunks.push(decoder.decode(bytes.subarray(offset), {stream: true}))
  chunks.push(decoder.decode())
  return chunks
}

function runDirect(chunks: string[], factory: ParserFactory = createParser): TraceEntry[] {
  const trace: TraceEntry[] = []
  // Events are buffered per `feed()` and appended after that feed's other
  // callbacks, matching the observable order of the stream entry point (see
  // the file header comment).
  let pendingEvents: TraceEntry[] = []
  const parser = factory({
    onEvent: (event) => {
      pendingEvents.push({type: 'event', id: event.id, event: event.event, data: event.data})
    },
    onComment: (comment) => trace.push({type: 'comment', comment}),
    onRetry: (retry) => trace.push({type: 'retry', retry}),
    onId: (id) => trace.push({type: 'id', id}),
    onError: (error) => trace.push({type: 'error', errorType: error.type, message: error.message}),
  })
  for (const chunk of chunks) {
    parser.feed(chunk)
    trace.push(...pendingEvents)
    pendingEvents = []
  }
  return trace
}

async function runStream(chunks: string[]): Promise<TraceEntry[]> {
  const trace: TraceEntry[] = []
  const stream = new EventSourceParserStream({
    onComment: (comment) => trace.push({type: 'comment', comment}),
    onRetry: (retry) => trace.push({type: 'retry', retry}),
    onId: (id) => trace.push({type: 'id', id}),
    onError: (error) =>
      trace.push({
        type: 'error',
        errorType: error instanceof ParseError ? error.type : error.name,
        message: error.message,
      }),
  })
  const reader = stream.readable.getReader()
  const writer = stream.writable.getWriter()
  const pump = (async () => {
    for (;;) {
      const {done, value} = await reader.read()
      if (done) {
        return
      }
      trace.push({type: 'event', id: value.id, event: value.event, data: value.data})
    }
  })()
  for (const chunk of chunks) {
    await writer.write(chunk)
  }
  await writer.close()
  await pump
  return trace
}

function entriesEqual(a: TraceEntry, b: TraceEntry): boolean {
  if (a.type !== b.type) {
    return false
  }
  if (a.type === 'event' && b.type === 'event') {
    return a.id === b.id && a.event === b.event && a.data === b.data
  }
  if (a.type === 'comment' && b.type === 'comment') {
    return a.comment === b.comment
  }
  if (a.type === 'retry' && b.type === 'retry') {
    return a.retry === b.retry
  }
  if (a.type === 'id' && b.type === 'id') {
    return a.id === b.id
  }
  if (a.type === 'error' && b.type === 'error') {
    return a.errorType === b.errorType && a.message === b.message
  }
  return false
}

function firstDivergence(a: TraceEntry[], b: TraceEntry[]): number {
  const length = Math.max(a.length, b.length)
  for (let i = 0; i < length; i++) {
    const left = a[i]
    const right = b[i]
    if (left === undefined || right === undefined) {
      if (left !== right) {
        return i
      }
    } else if (!entriesEqual(left, right)) {
      return i
    }
  }
  return -1
}

/** Escapes control characters so failing inputs are printable in test output. */
function escape(input: string): string {
  let out = ''
  for (const char of input) {
    const code = char.codePointAt(0) ?? 0
    if (char === '\n') {
      out += '\\n'
    } else if (char === '\r') {
      out += '\\r'
    } else if (char === '\t') {
      out += '\\t'
    } else if (char === '\0') {
      out += '\\0'
    } else if (char === '\\') {
      out += '\\\\'
    } else if (char === '"') {
      out += '\\"'
    } else if (code < 0x20 || code === 0x7f || code === 0xfeff) {
      out += `\\u{${code.toString(16)}}`
    } else {
      out += char
    }
  }
  return out
}

function formatValue(value: string | undefined): string {
  return value === undefined ? 'undefined' : `"${escape(value)}"`
}

function formatEntry(entry: TraceEntry): string {
  switch (entry.type) {
    case 'event':
      return `event(id=${formatValue(entry.id)}, event=${formatValue(entry.event)}, data="${escape(entry.data)}")`
    case 'comment':
      return `comment("${escape(entry.comment)}")`
    case 'retry':
      return `retry(${entry.retry})`
    case 'id':
      return `id("${escape(entry.id)}")`
    case 'error':
      return `error(${entry.errorType}: "${escape(entry.message)}")`
  }
}

function formatTrace(label: string, trace: TraceEntry[]): string[] {
  return [
    `${label} (${trace.length} entries):`,
    ...trace.map((entry, index) => `  [${index}] ${formatEntry(entry)}`),
  ]
}

function buildMismatchReport(
  input: string,
  chunks: string[],
  direct: TraceEntry[],
  stream: TraceEntry[],
): string {
  const divergence = firstDivergence(direct, stream)
  const directEntry = direct[divergence]
  const streamEntry = stream[divergence]
  return [
    'chunking equivalence mismatch between createParser() and EventSourceParserStream',
    '',
    `input:  "${escape(input)}"`,
    `chunks: [${chunks.map((chunk) => `"${escape(chunk)}"`).join(', ')}]`,
    '',
    `first divergence at trace index ${divergence}:`,
    `  direct: ${directEntry ? formatEntry(directEntry) : '<no entry>'}`,
    `  stream: ${streamEntry ? formatEntry(streamEntry) : '<no entry>'}`,
    '',
    ...formatTrace('direct trace', direct),
    '',
    ...formatTrace('stream trace', stream),
  ].join('\n')
}

async function assertChunkingEquivalence(input: string): Promise<void> {
  const bytes = encoder.encode(input)
  for (const cuts of cutPoints(bytes.length)) {
    const chunks = toChunks(bytes, cuts)
    const direct = runDirect(chunks)
    const stream = await runStream(chunks)
    if (firstDivergence(direct, stream) !== -1) {
      throw new Error(buildMismatchReport(input, chunks, direct, stream))
    }
  }
}

for (const [name, input] of inputs) {
  test(`equivalence under all chunkings: ${name}`, {timeout: 60_000}, async () => {
    await assertChunkingEquivalence(input)
  })
}

test('id and retry callbacks stay consistent across subsequent events', async () => {
  const input = 'retry: 100\nid: 1\ndata: a\n\ndata: b\n\nretry: 200\nid: 2\ndata: c\n\n'
  const direct = runDirect([input])
  const stream = await runStream([input])
  const expected: TraceEntry[] = [
    {type: 'retry', retry: 100},
    {type: 'id', id: '1'},
    {type: 'retry', retry: 200},
    {type: 'id', id: '2'},
    {type: 'event', id: '1', event: undefined, data: 'a'},
    {type: 'event', id: undefined, event: undefined, data: 'b'},
    {type: 'event', id: '2', event: undefined, data: 'c'},
  ]
  expect(direct).toEqual(expected)
  expect(stream).toEqual(expected)
})

/**
 * A deliberately buggy parser variant: it treats a `\r` at the very end of a
 * chunk as a complete line terminator but keeps no state about it, so a `\n`
 * arriving at the start of the next chunk (the second half of the split `\r\n`)
 * is parsed as a brand new blank line — dispatching the buffered event early
 * and splitting one event into two.
 */
function createCrLfSplitBugParser(callbacks: ParserCallbacks): {feed(chunk: string): void} {
  const {onComment, onError, onEvent, onId, onRetry} = callbacks
  let buffer = ''
  let bomChecked = false
  let id: string | undefined
  let data = ''
  let dataLines = 0
  let eventType: string | undefined

  function dispatch() {
    if (id !== undefined) {
      onId?.(id)
    }
    if (dataLines > 0) {
      onEvent?.({id, event: eventType, data})
    }
    id = undefined
    data = ''
    dataLines = 0
    eventType = undefined
  }

  function processField(field: string, value: string, line: string) {
    switch (field) {
      case 'event':
        eventType = value || undefined
        break
      case 'data':
        data = dataLines === 0 ? value : `${data}\n${value}`
        dataLines++
        break
      case 'id':
        if (!value.includes('\0')) {
          id = value
        }
        break
      case 'retry':
        if (/^\d+$/.test(value)) {
          onRetry?.(parseInt(value, 10))
        } else {
          onError?.(
            new ParseError(`Invalid \`retry\` value: "${value}"`, {
              type: 'invalid-retry',
              value,
              line,
            }),
          )
        }
        break
      default:
        onError?.(
          new ParseError(
            `Unknown field "${field.length > 20 ? `${field.slice(0, 20)}…` : field}"`,
            {type: 'unknown-field', field, value, line},
          ),
        )
    }
  }

  function processLine(line: string) {
    if (line.startsWith(':')) {
      onComment?.(line.slice(line.charCodeAt(1) === 32 ? 2 : 1))
      return
    }
    const separatorIndex = line.indexOf(':')
    if (separatorIndex === -1) {
      processField(line, '', line)
      return
    }
    const field = line.slice(0, separatorIndex)
    const value = line.slice(separatorIndex + (line.charCodeAt(separatorIndex + 1) === 32 ? 2 : 1))
    processField(field, value, line)
  }

  return {
    feed(chunk: string) {
      if (!bomChecked) {
        bomChecked = true
        chunk = chunk.replace(/^\uFEFF/, '')
      }
      buffer += chunk
      for (;;) {
        const crIndex = buffer.indexOf('\r')
        const lfIndex = buffer.indexOf('\n')
        const lineEnd =
          crIndex === -1 ? lfIndex : lfIndex === -1 ? crIndex : Math.min(crIndex, lfIndex)
        if (lineEnd === -1) {
          return
        }
        const line = buffer.slice(0, lineEnd)
        // BUG: a `\r` at the very end of the buffer is consumed immediately and
        // forgotten, so a `\n` arriving in the next chunk becomes a blank line.
        const terminatorLength =
          buffer.charCodeAt(lineEnd) === 13 && buffer.charCodeAt(lineEnd + 1) === 10 ? 2 : 1
        buffer = buffer.slice(lineEnd + terminatorLength)
        if (line === '') {
          dispatch()
        } else {
          processLine(line)
        }
      }
    },
  }
}

test('the harness catches a parser that double-dispatches when CR and LF land in different chunks', async () => {
  const input = 'data: a\r\ndata: b\n\n'
  const bytes = encoder.encode(input)

  // Fed as a single chunk, the buggy parser agrees with the real one — the bug
  // only exists at a chunk boundary, which is exactly what this suite exercises.
  expect(firstDivergence(runDirect([input], createCrLfSplitBugParser), runDirect([input]))).toBe(-1)

  // Run the same two-entry-point comparison the equivalence tests use, but with
  // the buggy parser on the direct side: it must be caught.
  const mismatches: string[][] = []
  for (const cuts of cutPoints(bytes.length)) {
    const chunks = toChunks(bytes, cuts)
    const buggy = runDirect(chunks, createCrLfSplitBugParser)
    const reference = await runStream(chunks)
    if (firstDivergence(buggy, reference) !== -1) {
      mismatches.push(chunks)
    }
  }
  expect(mismatches.length).toBeGreaterThan(0)

  // The canonical failure: split exactly between the `\r` and the `\n`. The
  // buggy parser dispatches two events (`a`, then `b`); the real one dispatches
  // a single multi-line event (`a\nb`).
  const split = toChunks(bytes, [8])
  expect(split).toEqual(['data: a\r', '\ndata: b\n\n', ''])
  const buggy = runDirect(split, createCrLfSplitBugParser)
  const reference = await runStream(split)
  expect(buggy.filter((entry) => entry.type === 'event')).toEqual([
    {type: 'event', id: undefined, event: undefined, data: 'a'},
    {type: 'event', id: undefined, event: undefined, data: 'b'},
  ])
  expect(reference.filter((entry) => entry.type === 'event')).toEqual([
    {type: 'event', id: undefined, event: undefined, data: 'a\nb'},
  ])
  expect(firstDivergence(buggy, reference)).not.toBe(-1)
})
