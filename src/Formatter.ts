import {
  Cue,
  CueSnapshot,
  FormatOptions,
  Node,
  formatTimestamp
} from '.'

interface BlockOutput {
  text: string
  sep: string
  eol: string
  isNew: boolean
}

export class Formatter {
  private options: FormatOptions
  private isVTT: boolean
  private preserve: boolean
  private hasReceivedHeader: boolean
  private index: number
  private lastKind: 'none' | 'header' | 'cue' | 'metadata'

  // Layout-preserving state: the last node is rendered once the following
  // node is known, because only then can its separator be decided.
  private pendingNode?: Node
  /** The pending node is the first one and still needs a document header. */
  private pendingNeedsHeader: boolean
  private emittedBom: boolean

  constructor(options: FormatOptions) {
    this.options = options
    this.hasReceivedHeader = false
    this.lastKind = 'none'
    this.isVTT = options.format === 'WebVTT'
    this.index = 1
    this.preserve = options.preserveLayout === true && this.isVTT
    this.pendingNeedsHeader = false
    this.emittedBom = false
  }

  public format(node: Node): string {
    if (this.preserve) {
      return this.formatPreservingLayout(node)
    }

    let buffer = ''

    if (node.type === 'header' && this.isVTT) {
      this.hasReceivedHeader = true
      this.lastKind = 'header'
      buffer += `${node.data}\n\n`
    }

    if (node.type === 'metadata' && this.isVTT) {
      if (!this.hasReceivedHeader) {
        this.hasReceivedHeader = true
        buffer += 'WEBVTT\n\n'
      } else if (this.lastKind === 'cue' || this.lastKind === 'metadata') {
        buffer += '\n'
      }

      // Followed by a cue, the newline that cue prepends completes the blank
      // line. A trailing metadata block simply ends with one newline.
      buffer += appendNewline(node.data.text)
      this.lastKind = 'metadata'
    }

    if (node.type === 'cue') {
      if (!this.hasReceivedHeader && this.isVTT) {
        this.hasReceivedHeader = true
        buffer += 'WEBVTT\n\n'
      } else if (this.lastKind === 'metadata' && this.index === 1) {
        buffer += '\n'
      }

      buffer += this.formatCue(node.data, this.index++, this.options)
      this.lastKind = 'cue'
    }

    return buffer
  }

  /**
   * Emits the last block and its trailing separator. Only used when layout
   * preservation is enabled.
   */
  public flush(): string {
    if (!this.preserve || !this.pendingNode) {
      return ''
    }

    const last = this.describe(this.pendingNode, this.pendingNeedsHeader)
    this.pendingNode = undefined
    this.pendingNeedsHeader = false

    // A brand new last block is terminated by a single newline. Everything
    // else carries the separator that followed its original block.
    return last.text + (last.isNew ? last.eol : last.sep)
  }

  private formatCue(cue: Cue, index: number, options: FormatOptions) {
    return [
      `${index > 1 ? '\n' : ''}${index}`,
      `${formatTimestamp(cue.start, options)} --> ${formatTimestamp(
        cue.end,
        options
      )}${
        options.format === 'WebVTT' && cue.settings ? ' ' + cue.settings : ''
      }`,
      cue.text,
      ''
    ].join('\n')
  }

  private formatPreservingLayout(node: Node): string {
    const needsHeader = !this.hasReceivedHeader && node.type !== 'header'

    this.hasReceivedHeader = true
    this.pendingNeedsHeader = needsHeader

    let output = ''

    if (this.pendingNode) {
      const previous = this.describe(this.pendingNode, false)
      output += previous.text + this.separatorFor(previous)
    }

    this.pendingNode = node

    return output
  }

  private describe(node: Node, needsHeader: boolean): BlockOutput {
    const layout = node.layout

    if (!layout || layout.format !== 'WebVTT') {
      return this.describeNew(node, needsHeader)
    }

    const eol = layout.eol || '\n'
    const prefix = this.prefix(layout)

    if (node.type === 'header') {
      const edited = layout.header !== node.data
      const text = edited ? joinLines(node.data, eol) : layout.raw
      return {
        text: prefix + text,
        sep: layout.sep,
        eol,
        isNew: false
      }
    }

    if (node.type === 'metadata') {
      const edited = layout.metadata !== node.data.text
      const text = edited ? joinLines(node.data.text, eol) : layout.raw
      return {
        text: prefix + text,
        sep: layout.sep,
        eol,
        isNew: false
      }
    }

    const edited = !layout.cue || !sameCue(node.data, layout.cue)
    const text = edited ? this.renderCue(node.data, eol) : layout.raw

    return {
      text: prefix + text,
      sep: layout.sep,
      eol,
      isNew: false
    }
  }

  private describeNew(node: Node, needsHeader: boolean): BlockOutput {
    const eol = '\n'
    let text: string

    if (node.type === 'header') {
      text = node.data
    } else if (node.type === 'metadata') {
      // Without a document header a metadata block cannot stand on its own.
      text =
        (needsHeader ? 'WEBVTT' + eol + eol : '') +
        joinLines(node.data.text, eol)
    } else {
      text =
        (needsHeader ? 'WEBVTT' + eol + eol : '') + this.renderCue(node.data, eol)
    }

    return { text, sep: eol + eol, eol, isNew: true }
  }

  private prefix(layout: { leading?: string; bom?: boolean }): string {
    let prefix = ''

    // The BOM belongs to the document header and is emitted exactly once,
    // even if other blocks are moved around or the header is duplicated.
    if (layout.bom && !this.emittedBom) {
      prefix += '﻿'
      this.emittedBom = true
    }

    if (layout.leading) {
      prefix += layout.leading
    }

    return prefix
  }

  private separatorFor(previous: BlockOutput): string {
    // A brand new block always brings its own blank-line separator. Existing
    // blocks carry the separator that followed them in the source document,
    // padded when it does not contain enough blank lines for a valid file.
    if (previous.isNew) {
      return previous.sep
    }

    return ensureBlankLine(previous.sep, previous.eol)
  }

  private renderCue(cue: Cue, eol: string): string {
    const lines: string[] = []
    const identifier =
      cue.identifier === undefined || cue.identifier === null
        ? ''
        : String(cue.identifier)

    if (identifier !== '') {
      lines.push(identifier)
    }

    const settings =
      this.isVTT && cue.settings ? ' ' + cue.settings : ''

    lines.push(
      `${formatTimestamp(cue.start, this.options)} --> ${formatTimestamp(
        cue.end,
        this.options
      )}${settings}`
    )

    if (cue.text !== undefined && cue.text !== null && cue.text !== '') {
      lines.push(...String(cue.text).split(/\r\n|\n/))
    }

    return lines.join(eol)
  }
}

function sameCue(cue: Cue, snapshot: CueSnapshot): boolean {
  return (
    cue.start === snapshot.start &&
    cue.end === snapshot.end &&
    (cue.settings || '') === (snapshot.settings || '') &&
    cue.text === snapshot.text &&
    identifierOf(cue) === identifierOf(snapshot)
  )
}

function identifierOf(cue: { identifier?: string }): string {
  return cue.identifier === undefined || cue.identifier === null
    ? ''
    : String(cue.identifier)
}

function joinLines(text: string, eol: string): string {
  return text.split(/\r\n|\n/).join(eol)
}

function appendNewline(text: string): string {
  return text.endsWith('\n') ? text : text + '\n'
}

function countNewlines(value: string): number {
  const matches = value.match(/\r\n|\n/g)
  return matches ? matches.length : 0
}

function ensureBlankLine(separator: string, eol: string): string {
  const count = countNewlines(separator)

  if (count >= 2) {
    return separator
  }

  if (count === 1) {
    return separator + eol
  }

  return separator + eol + eol
}
