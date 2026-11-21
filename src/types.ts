export interface Timestamp {
  start: number
  end: number
  settings?: string
}

export interface Cue extends Timestamp {
  text: string
  identifier?: string
}

export type Format = 'SRT' | 'WebVTT'

export interface FormatOptions {
  format: Format
  /**
   * When enabled, WebVTT output keeps the original file layout (BOM, line
   * endings, blank lines, metadata blocks, cue identifiers and settings) for
   * every node that was not edited.
   */
  preserveLayout?: boolean
}

export interface ParseOptions {
  /**
   * Records the layout information needed by the formatter to round-trip a
   * WebVTT file. Node shapes are left untouched, except for the optional
   * `data.identifier` of cues.
   */
  preserveLayout?: boolean
}

export type MetadataKind = 'NOTE' | 'STYLE' | 'REGION'

export interface Metadata {
  kind: MetadataKind
  /**
   * The whole block, including its first line, without the trailing blank
   * separator line.
   */
  text: string
}

export interface CueSnapshot {
  start: number
  end: number
  settings?: string
  text: string
  identifier?: string
}

/**
 * Layout information captured by the parser and consumed by the formatter.
 *
 * Everything here is made of plain JSON-serializable values, so it travels
 * with a node through JSON.stringify/JSON.parse. It is not a copy of the whole
 * source file: each block only carries its own bytes.
 */
export interface NodeLayout {
  /** Format of the document this block came from. */
  format: Format
  /** Original bytes of the block, excluding BOM and the trailing separator. */
  raw: string
  /** Original bytes following the block, up to the next block. */
  sep: string
  /** Newline sequence that terminated the lines of this block. */
  eol: string
  /** Blank lines that preceded this block (only used by the first block). */
  leading?: string
  /** Present on the document header when the file had a BOM. */
  bom?: boolean
  /** Snapshot of header.data. */
  header?: string
  /** Snapshot of metadata.kind and metadata.text. */
  kind?: MetadataKind
  metadata?: string
  /** Snapshot of the editable cue fields. */
  cue?: CueSnapshot
}

export interface NodeHeader {
  type: 'header'
  data: string
  layout?: NodeLayout
}

export interface NodeCue {
  type: 'cue'
  data: Cue
  layout?: NodeLayout
}

export interface NodeMetadata {
  type: 'metadata'
  data: Metadata
  layout?: NodeLayout
}

export type Node = NodeHeader | NodeCue | NodeMetadata

export type NodeList = Node[]
