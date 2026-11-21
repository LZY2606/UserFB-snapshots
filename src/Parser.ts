import stripBom from 'strip-bom'
import {
  MetadataKind,
  Node,
  NodeLayout,
  RE_TIMESTAMP,
  parseTimestamps
} from '.'

export type Pusher = (node: Node) => void

export interface ParseState {
  expect: 'header' | 'id' | 'timestamp' | 'text' | 'vtt_comment'
  row: number
  hasContentStarted: boolean
  isWebVTT: boolean
  node: Partial<Node>
  buffer: string[]
}

type BlockType = 'header' | 'cue' | 'metadata'

const VTT_METADATA_LINE = /^(NOTE|STYLE|REGION)(?:$|(?=[\s]))/
const VTT_HEADER_LINE = /^WEBVTT(?:$|(?=[\s]))/

interface LayoutLine {
  text: string
  eol: string
}

interface PendingBlock {
  type: BlockType
  /** Raw content lines, without the separator that follows the block. */
  lines: LayoutLine[]
  identifier?: string
  leading?: string
  awaitingTimestamp?: boolean
}

interface LayoutState {
  mode: 'undecided' | 'vtt' | 'srt'
  buffered: LayoutLine[]
  leading: LayoutLine[]
  bom: boolean
  pending?: PendingBlock
  lastPushed?: Node
  /** Blank lines read after the last block, before the next one starts. */
  tail: string
  /** BOM seen before the header: attached once to the header block. */
  bomPending: boolean
  hasEmitted: boolean
  row: number
}

/**
 * Parser for SRT and WebVTT contents.
 *
 * `parseLine` keeps the historical line-by-line behaviour. `parseBlock` is
 * used by the `preserveLayout` entry points: it splits the document into
 * blocks so that a block can be delivered as soon as the separator that
 * follows it has been read.
 */
export class Parser {
  private push: Pusher
  private state: ParseState
  private layout?: LayoutState

  constructor({
    push,
    preserveLayout = false
  }: {
    push: Pusher
    preserveLayout?: boolean
  }) {
    this.push = push
    this.state = {
      expect: 'header',
      row: 0,
      hasContentStarted: false,
      isWebVTT: false,
      node: {},
      buffer: []
    }

    if (preserveLayout) {
      this.layout = {
        mode: 'undecided',
        buffered: [],
        leading: [],
        bom: false,
        tail: '',
        bomPending: false,
        hasEmitted: false,
        row: 0
      }
    }
  }

  private isIndex(line: string): boolean {
    return /^\d+$/.test(line.trim())
  }

  private isTimestamp(line: string): boolean {
    return RE_TIMESTAMP.test(line)
  }

  private isVttComment(line: string): boolean {
    return /^NOTE/.test(line)
  }

  private isMetadataLine(line: string): boolean {
    return VTT_METADATA_LINE.test(line)
  }

  private getError(expected: string, index: number, row: string): Error {
    return new Error(
      `expected ${expected} at row ${index + 1}, but received: "${row}"`
    )
  }

  public parseLine(line: string): void {
    const contents = this.state.row === 0 ? stripBom(line) : line

    if (!this.state.hasContentStarted) {
      if (contents.trim()) {
        this.state.hasContentStarted = true
      } else {
        return
      }
    }

    const parse = {
      header: this.parseHeader,
      id: this.parseId,
      timestamp: this.parseTimestamp,
      text: this.parseText,
      vtt_comment: this.parseVttComment
    }[this.state.expect]

    parse.call(this, contents)

    this.state.row++
  }

  /**
   * Block-level parsing used for layout preservation.
   *
   * `line` must not contain newline characters. `eol` is the newline sequence
   * that terminated it (`\r\n`, `\n`, or `''` for a last line without one).
   */
  public parseBlock(line: string, eol = ''): void {
    const layout = this.layout

    if (!layout || layout.mode === 'srt') {
      this.parseLine(line)
      return
    }

    if (layout.mode === 'undecided') {
      layout.buffered.push({ text: line, eol })

      if (!line.trim()) {
        return
      }

      const buffered = layout.buffered
      layout.buffered = []

      // Skip blank lines while looking for the first meaningful line. They are
      // kept as the layout prefix of the first block.
      const firstIndex = buffered.findIndex(item => Boolean(item.text.trim()))
      const first = stripBom(buffered[firstIndex].text)

      if (!VTT_HEADER_LINE.test(first)) {
        layout.mode = 'srt'
        buffered.forEach(item => this.parseLine(item.text))
        return
      }

      layout.mode = 'vtt'
      layout.bom = buffered[firstIndex].text.charCodeAt(0) === 0xfeff
      layout.bomPending = layout.bom
      layout.leading = buffered.slice(0, firstIndex)

      buffered.slice(firstIndex).forEach((item, index) => {
        const isLast = index === buffered.length - firstIndex - 1
        this.handleVttLine(isLast ? first : item.text, item.eol)
      })
      return
    }

    this.handleVttLine(line, eol)
  }

  public flush(): void {
    const layout = this.layout

    if (!layout) {
      if (this.state.buffer.length > 0) {
        this.pushNode()
      }
      return
    }

    if (layout.mode !== 'vtt') {
      if (this.state.buffer.length > 0) {
        this.pushNode()
      }
      return
    }

    const pending = layout.pending

    if (pending) {
      if (pending.awaitingTimestamp) {
        throw this.getError('timestamp', layout.row, '')
      }

      this.finalizeBlock(false, '')
    }

    if (layout.tail && layout.lastPushed && layout.lastPushed.layout) {
      // Trailing blank lines at the end of the document belong to the
      // separator of the last block.
      layout.lastPushed.layout.sep += layout.tail
      layout.tail = ''
    }
  }

  private handleVttLine(line: string, eol: string): void {
    const layout = this.layout!
    layout.row++

    const pending = layout.pending

    if (pending) {
      if (pending.awaitingTimestamp) {
        if (!this.isTimestamp(line)) {
          throw this.getError('timestamp', layout.row - 1, line)
        }
        pending.lines.push({ text: line, eol })
        pending.awaitingTimestamp = false
        return
      }

      if (pending.type === 'header' || pending.type === 'metadata') {
        if (line === '') {
          this.finalizeBlock(true, eol)
        } else {
          pending.lines.push({ text: line, eol })
        }
        return
      }

      // cue
      if (line === '') {
        this.finalizeBlock(true, eol)
        return
      }

      if (this.isTimestamp(line)) {
        this.finalizeBlock(false, '')
        this.startCue(line, eol)
        return
      }

      if (this.isMetadataLine(line)) {
        this.finalizeBlock(false, '')
        this.startMetadata(line, eol)
        return
      }

      // A whitespace-only line is still part of the cue payload, as it was in
      // the historical parser. The block is only closed by a real blank line
      // or by the timestamp of the next cue.
      pending.lines.push({ text: line, eol })
      return
    }

    if (line === '') {
      this.handleBlankLine(eol)
      return
    }

    if (!line.trim()) {
      this.handleWhitespaceLine(line, eol)
      return
    }

    const leading = this.takeLeading()

    if (!layout.hasEmitted && VTT_HEADER_LINE.test(line)) {
      layout.pending = {
        type: 'header',
        lines: [{ text: line, eol }],
        leading
      }
      return
    }

    if (this.isMetadataLine(line)) {
      this.startMetadata(line, eol, leading)
      return
    }

    if (this.isTimestamp(line)) {
      layout.pending = {
        type: 'cue',
        lines: [{ text: line, eol }],
        leading
      }
      return
    }

    // A line that is neither a timestamp nor a metadata block starts a cue
    // identifier. The timestamp has to come on the next line.
    layout.pending = {
      type: 'cue',
      lines: [{ text: line, eol }],
      identifier: line,
      awaitingTimestamp: true,
      leading
    }
  }

  private handleWhitespaceLine(line: string, eol: string): void {
    const layout = this.layout!

    if (layout.hasEmitted) {
      // Whitespace that follows a separator is not a block of its own. It is
      // handed to the next block as leading whitespace (or to the last block
      // as trailing bytes at the end of the document).
      layout.tail += line + eol
      return
    }

    layout.leading.push({ text: line, eol })
  }

  private handleBlankLine(eol: string): void {
    const layout = this.layout!
    const pending = layout.pending

    if (pending) {
      if (pending.awaitingTimestamp) {
        throw this.getError('timestamp', layout.row - 1, '')
      }

      this.finalizeBlock(true, eol)
      return
    }

    if (!layout.hasEmitted) {
      layout.leading.push({ text: '', eol })
      return
    }

    layout.tail += eol
  }

  private takeLeading(): string | undefined {
    const layout = this.layout!

    if (layout.leading.length === 0 && !layout.tail) {
      return undefined
    }

    const leading =
      layout.leading.map(item => item.text + item.eol).join('') + layout.tail
    layout.leading = []
    layout.tail = ''

    return leading
  }

  private startCue(
    line: string,
    eol: string,
    leading?: string
  ): void {
    this.layout!.pending = {
      type: 'cue',
      lines: [{ text: line, eol }],
      leading
    }
  }

  private startMetadata(
    line: string,
    eol: string,
    leading?: string
  ): void {
    this.layout!.pending = {
      type: 'metadata',
      lines: [{ text: line, eol }],
      leading
    }
  }

  private finalizeBlock(withBlank: boolean, blankEol: string): void {
    const layout = this.layout!
    const pending = layout.pending!
    const lines = pending.lines
    const last = lines[lines.length - 1]
    const raw = serialize(lines)
    const eol = blockEol(lines)
    const separator = withBlank
      ? (last ? last.eol : '') + blankEol
      : last
      ? last.eol
      : ''

    if (pending.type === 'header') {
      const data = lines
        .map(item => item.text)
        .join('\n')
        .trim()

      const layoutInfo: NodeLayout = {
        format: 'WebVTT',
        raw,
        sep: separator,
        eol,
        header: data
      }

      if (layout.bomPending) {
        layoutInfo.bom = true
        layout.bomPending = false
      }

      if (pending.leading !== undefined) {
        layoutInfo.leading = pending.leading
      }

      this.emitNode({ type: 'header', data, layout: layoutInfo })
    } else if (pending.type === 'metadata') {
      const text = lines.map(item => item.text).join('\n')
      const kind = metadataKind(lines[0].text)

      const layoutInfo: NodeLayout = {
        format: 'WebVTT',
        raw,
        sep: separator,
        eol,
        kind,
        metadata: text
      }

      if (pending.leading !== undefined) {
        layoutInfo.leading = pending.leading
      }

      this.emitNode({
        type: 'metadata',
        data: { kind, text },
        layout: layoutInfo
      })
    } else {
      const timestampIndex = pending.identifier === undefined ? 0 : 1
      const timestamp = lines[timestampIndex]

      if (!timestamp) {
        throw this.getError('timestamp', layout.row - 1, '')
      }

      const timestamps = parseTimestamps(timestamp.text)
      const text = lines
        .slice(timestampIndex + 1)
        .map(item => item.text)
        .join('\n')

      const data = { ...timestamps, text }

      if (pending.identifier !== undefined) {
        ;(data as { identifier?: string }).identifier = pending.identifier
      }

      const layoutInfo: NodeLayout = {
        format: 'WebVTT',
        raw,
        sep: separator,
        eol,
        cue: {
          ...timestamps,
          text,
          ...(pending.identifier === undefined
            ? {}
            : { identifier: pending.identifier })
        }
      }

      if (pending.leading !== undefined) {
        layoutInfo.leading = pending.leading
      }

      this.emitNode({ type: 'cue', data, layout: layoutInfo })
    }

    layout.hasEmitted = true
    layout.pending = undefined
  }

  private emitNode(node: Node): void {
    // Track the delivered node so trailing blank lines at the end of the
    // document can be folded into its separator by `flush`.
    this.layout!.lastPushed = node
    this.push(node)
  }

  private parseHeader(line: string) {
    if (!this.state.isWebVTT) {
      this.state.isWebVTT = /^WEBVTT/.test(line)

      if (this.state.isWebVTT) {
        this.state.node.type = 'header'
      } else {
        this.parseId(line)
        return
      }
    }

    this.state.buffer.push(line)

    if (!line) {
      this.state.expect = 'id'
      return
    }
  }

  private parseId(line: string) {
    this.state.expect = 'timestamp'

    if (this.state.node.type === 'header') {
      this.pushNode()
    }

    if (this.isIndex(line)) return

    if (this.state.isWebVTT && this.isVttComment(line)) {
      this.state.expect = 'vtt_comment'
      return
    }

    this.parseTimestamp(line)
  }

  private parseVttComment(line: string) {
    this.state.expect = 'vtt_comment'

    if (line.trim() === '') {
      this.state.expect = 'id'
    }
  }

  private parseTimestamp(line: string) {
    if (!this.isTimestamp(line)) {
      throw this.getError('timestamp', this.state.row, line)
    }

    this.state.node = {
      type: 'cue',
      data: {
        ...parseTimestamps(line),
        text: ''
      }
    }

    this.state.expect = 'text'
  }

  private parseText(line: string) {
    if (this.state.buffer.length === 0) {
      this.state.buffer.push(line)
      return
    }

    if (this.isTimestamp(line)) {
      const lastIndex = this.state.buffer.length - 1

      if (this.isIndex(this.state.buffer[lastIndex])) {
        this.state.buffer.pop()
      }

      this.pushNode()
      this.parseTimestamp(line)
      return
    }

    if (this.isVttComment(line)) {
      this.pushNode()
      this.parseVttComment(line)
      return
    }

    this.state.buffer.push(line)
  }

  private pushNode(): void {
    if (this.state.node.type === 'cue') {
      while (true) {
        const lastItem = this.state.buffer[this.state.buffer.length - 1]
        if (['', '\n'].includes(lastItem)) {
          this.state.buffer.pop()
        } else {
          break
        }
      }

      while (true) {
        const firstItem = this.state.buffer[0]
        if (['', '\n'].includes(firstItem)) {
          this.state.buffer.shift()
        } else {
          break
        }
      }

      this.state.node.data!.text = this.state.buffer.join('\n')
    }

    if (this.state.node.type === 'header') {
      this.state.node.data = this.state.buffer.join('\n').trim()
    }

    this.push(this.state.node as Node)

    this.state.node = {}
    this.state.buffer = []
  }
}

function metadataKind(line: string): MetadataKind {
  const match = VTT_METADATA_LINE.exec(line)
  return (match && match[1] ? match[1] : 'NOTE') as MetadataKind
}

function serialize(lines: LayoutLine[]): string {
  return lines
    .map((line, index) => (index < lines.length - 1 ? line.text + line.eol : line.text))
    .join('')
}

function blockEol(lines: LayoutLine[]): string {
  const last = lines[lines.length - 1]

  if (last && last.eol) {
    return last.eol
  }

  const found = lines.find(line => line.eol)
  return found ? found.eol : '\n'
}
