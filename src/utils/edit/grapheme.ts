import {hasOwnSelection} from '../focus/selection'
import {getWindow} from '../misc/getWindow'
import {isElementType} from '../misc/isElementType'
import {getContentEditable} from './isContentEditable'

const TEXT_NODE = 3
const ELEMENT_NODE = 1

const segmenterCache = new WeakMap<Window, Intl.Segmenter>()

/**
 * Get an `Intl.Segmenter` in grapheme mode from the window the node belongs to.
 * The segmentation therefore follows the runtime environment of the document.
 */
function getGraphemeSegmenter(node: Node): Intl.Segmenter | undefined {
  let win: Window | undefined
  try {
    win = getWindow(node)
  } catch {
    win = undefined
  }

  // Older TypeScript DOM libs do not declare `Intl` on `Window`.
  const windowIntl = (win as unknown as {Intl?: typeof Intl} | undefined)?.Intl
  const IntlApi = windowIntl ?? globalThis.Intl
  const SegmenterCtor = IntlApi?.Segmenter

  if (!SegmenterCtor) {
    return undefined
  }

  if (win) {
    const cached = segmenterCache.get(win)
    if (cached) {
      return cached
    }
  }

  const segmenter = new SegmenterCtor(undefined, {granularity: 'grapheme'})

  if (win) {
    segmenterCache.set(win, segmenter)
  }

  return segmenter
}

interface GraphemeSpan {
  /** UTF-16 offset of the start of the grapheme */
  start: number
  /** UTF-16 offset of the end of the grapheme */
  end: number
}

/**
 * Whether grapheme segmentation is available in the node's runtime.
 */
export function isGraphemeSegmentationAvailable(node: Node): boolean {
  return getGraphemeSegmenter(node) !== undefined
}

/**
 * Segment a string into extended grapheme cluster spans.
 */
function segmentGraphemes(
  segmenter: Intl.Segmenter,
  text: string,
): GraphemeSpan[] {
  const spans: GraphemeSpan[] = []
  for (const data of segmenter.segment(text)) {
    spans.push({start: data.index, end: data.index + data.segment.length})
  }
  return spans
}

function findGrapheme(
  spans: GraphemeSpan[],
  offset: number,
  direction: -1 | 1,
): GraphemeSpan | undefined {
  if (direction < 0) {
    // An offset on a boundary operates on the grapheme ending there.
    return spans
      .filter(span => span.end <= offset)
      .sort((a, b) => b.end - a.end)[0]
  }

  // An offset on a boundary operates on the grapheme starting there.
  return spans
    .filter(span => span.start >= offset)
    .sort((a, b) => a.start - b.start)[0]
}

/**
 * Determine the UTF-16 offsets of the grapheme removed by `Backspace`/`Delete`.
 *
 * An offset inside a grapheme removes the whole grapheme regardless of direction.
 * Returns `undefined` when there is no adjacent grapheme (start/end of the text),
 * in which case the caller must not modify the value nor fire an `input` event.
 */
export function getGraphemeDeleteSpan(
  node: Node,
  value: string,
  offset: number,
  direction: -1 | 1,
): GraphemeSpan | undefined {
  const segmenter = getGraphemeSegmenter(node)
  if (!segmenter) {
    return undefined
  }

  const spans = segmentGraphemes(segmenter, value)
  const containing = spans.find(
    span => span.start < offset && span.end > offset,
  )

  return containing ?? findGrapheme(spans, offset, direction)
}

/**
 * Move a collapsed caret by one extended grapheme cluster.
 *
 * An offset inside a grapheme moves to its start (left) or end (right).
 * Returns the unchanged offset when no further grapheme boundary exists.
 */
export function moveGraphemeOffset(
  node: Node,
  value: string,
  offset: number,
  direction: -1 | 1,
): number {
  const segmenter = getGraphemeSegmenter(node)
  if (!segmenter) {
    return offset + direction
  }

  const spans = segmentGraphemes(segmenter, value)
  const containing = spans.find(
    span => span.start < offset && span.end > offset,
  )

  if (containing) {
    return direction < 0 ? containing.start : containing.end
  }

  const adjacent = findGrapheme(spans, offset, direction)

  if (!adjacent) {
    return offset
  }

  return direction < 0 ? adjacent.start : adjacent.end
}

/**
 * Grapheme editing only changes ordinary text editing.
 * Number, date and time inputs keep their historical behavior.
 */
export function supportsGraphemeEditing(element: Element): boolean {
  if (isElementType(element, 'textarea')) {
    return true
  }

  if (isElementType(element, 'input')) {
    return [
      'text',
      'search',
      'url',
      'tel',
      'email',
      'password',
    ].includes(element.type)
  }

  return false
}

interface TextPiece {
  node: Text
  /** Local UTF-16 offset of this text node within its run */
  localStart: number
  length: number
}

interface TextRun {
  pieces: TextPiece[]
}

interface CaretPoint {
  node: Node
  offset: number
}

interface TextEdit {
  node: Text
  /** Local UTF-16 offset at which the removed text starts */
  start: number
  /** Local UTF-16 offset at which the removed text ends */
  end: number
}

export interface GraphemeDeletion {
  /** The point at which the grapheme started; the caret goes here */
  start: CaretPoint
  /** Per text node edits. Nodes are edited in place and never removed. */
  edits: TextEdit[]
}

interface ResolvedPoint {
  run: TextRun
  local: number
}

/**
 * Model of the text of a contenteditable element that is aware of
 * extended grapheme clusters, including clusters spanning multiple text nodes
 * and ordinary inline elements.
 */
export interface GraphemeModel {
  /**
   * The caret point one grapheme cluster away in the given direction.
   * Returns `undefined` when no grapheme boundary can be reached inside the
   * same text run. The caller must then keep the caret unmoved.
   */
  cursorPosition(
    node: Node,
    offset: number,
    direction: -1 | 1,
  ): CaretPoint | undefined

  /**
   * The per-text-node edits removing the grapheme for `Backspace`/`Delete`.
   * Returns `undefined` when the point is on a run boundary in the requested
   * direction or the deletion would cross a text run boundary.
   */
  deletion(
    node: Node,
    offset: number,
    direction: -1 | 1,
  ): GraphemeDeletion | undefined
}

/**
 * Elements that terminate a text run. Grapheme clusters never span across
 * these nodes and their subtrees are not traversed during segmentation.
 */
function isRunBoundary(element: Element, host: Element): boolean {
  if (isElementType(element, 'br')) {
    return true
  }

  if (hasOwnSelection(element)) {
    return true
  }

  if (!element.hasAttribute('contenteditable')) {
    return false
  }

  // A nested independent contenteditable root and a non-editable subtree
  // are both boundaries of the host's editing context.
  return element !== host
}

interface ElementPoint {
  run: TextRun | null
  local: number
}

/**
 * Build a grapheme model for a contenteditable host.
 */
export function buildGraphemeModel(host: Element): GraphemeModel | undefined {
  const segmenter = getGraphemeSegmenter(host)
  if (!segmenter) {
    return undefined
  }

  const runs: TextRun[] = []
  const textPoints = new WeakMap<Text, ResolvedPoint>()
  const elementPoints = new WeakMap<Element, ElementPoint[]>()
  let currentRun: TextRun | null = null
  let runLength = 0

  const openRun = () => {
    if (!currentRun) {
      currentRun = {pieces: []}
      runs.push(currentRun)
      runLength = 0
    }
  }

  const closeRun = () => {
    currentRun = null
  }

  const recordPoint = (): ElementPoint => ({
    run: currentRun,
    local: currentRun ? runLength : 0,
  })

  const walk = (node: Node) => {
    if (node.nodeType === TEXT_NODE) {
      openRun()
      const text = (node as Text).nodeValue ?? ''
      const run = currentRun as TextRun
      run.pieces.push({
        node: node as Text,
        localStart: runLength,
        length: text.length,
      })
      textPoints.set(node as Text, {run, local: runLength})
      runLength += text.length
      return
    }

    if (node.nodeType !== ELEMENT_NODE) {
      return
    }

    const element = node as Element

    if (isRunBoundary(element, host)) {
      // The subtree of a boundary node is not part of the host's text runs.
      closeRun()
      return
    }

    const points: ElementPoint[] = [recordPoint()]
    for (let i = 0; i < element.childNodes.length; i++) {
      walk(element.childNodes[i])
      points[i + 1] = recordPoint()
    }
    elementPoints.set(element, points)
  }

  walk(host)

  const runText = (run: TextRun) =>
    run.pieces.map(piece => piece.node.nodeValue ?? '').join('')

  const lengthOf = (run: TextRun) =>
    run.pieces.reduce((length, piece) => length + piece.length, 0)

  const resolvePoint = (
    node: Node,
    offset: number,
  ): ResolvedPoint | undefined => {
    if (node.nodeType === TEXT_NODE) {
      const point = textPoints.get(node as Text)
      if (!point) {
        return undefined
      }
      const piece = point.run.pieces.find(
        candidate => candidate.node === node,
      )
      const max = piece ? piece.length : 0
      return {
        run: point.run,
        local: point.local + Math.max(0, Math.min(max, offset)),
      }
    }

    if (node.nodeType === ELEMENT_NODE) {
      const points = elementPoints.get(node as Element)
      if (!points) {
        return undefined
      }
      const index = Math.max(0, Math.min(points.length - 1, offset))
      const point = points[index]
      if (point.run) {
        return {run: point.run, local: point.local}
      }

      // An opening position before the element's first text node has no run
      // recorded yet. Resolve it through the DOM, but stop at the first run
      // boundary so positions next to `br` or nested editing roots do not
      // silently cross them.
      return firstRunFromChild(node as Element, index)
    }

    return undefined
  }

  const firstRunFromChild = (
    element: Element,
    childIndex: number,
  ): ResolvedPoint | undefined => {
    const stack: Node[] = []
    for (let i = element.childNodes.length - 1; i >= childIndex; i--) {
      stack.push(element.childNodes[i])
    }

    while (stack.length) {
      const current = stack.pop() as Node

      if (current.nodeType === TEXT_NODE) {
        const point = textPoints.get(current as Text)
        if (!point) {
          return undefined
        }
        const piece = point.run.pieces.find(
          candidate => candidate.node === current,
        )
        return {run: point.run, local: piece ? piece.localStart : point.local}
      }

      if (current.nodeType === ELEMENT_NODE) {
        const childElement = current as Element
        if (isRunBoundary(childElement, host)) {
          return undefined
        }
        for (let i = childElement.childNodes.length - 1; i >= 0; i--) {
          stack.push(childElement.childNodes[i])
        }
      }
    }

    return undefined
  }

  const localToPoint = (
    run: TextRun,
    local: number,
    direction: -1 | 1,
  ): CaretPoint | undefined => {
    const {pieces} = run

    for (const piece of pieces) {
      if (
        local >= piece.localStart &&
        local < piece.localStart + piece.length
      ) {
        return {node: piece.node, offset: local - piece.localStart}
      }
    }

    // The position is on a junction between text nodes.
    const index = pieces.findIndex(
      piece => piece.localStart + piece.length === local,
    )

    if (index >= 0) {
      if (direction > 0 && pieces[index + 1]) {
        return {node: pieces[index + 1].node, offset: 0}
      }
      return {node: pieces[index].node, offset: pieces[index].length}
    }

    if (local === 0 && pieces[0]) {
      return {node: pieces[0].node, offset: 0}
    }

    return undefined
  }

  return {
    cursorPosition(node, offset, direction) {
      const point = resolvePoint(node, offset)
      if (!point) {
        return undefined
      }

      const {run, local} = point
      const target = moveGraphemeOffset(host, runText(run), local, direction)

      if (target === local) {
        return undefined
      }

      return localToPoint(run, target, direction)
    },

    deletion(node, offset, direction) {
      const point = resolvePoint(node, offset)
      if (!point) {
        return undefined
      }

      const {run, local} = point
      const length = lengthOf(run)

      if (local === (direction < 0 ? 0 : length)) {
        // At the edge of the run in the requested direction there is no
        // grapheme inside this run to operate on. The boundary must not be crossed.
        return undefined
      }

      const spans = segmentGraphemes(segmenter, runText(run))
      const containing = spans.find(
        span => span.start < local && span.end > local,
      )
      const span = containing ?? findGrapheme(spans, local, direction)

      if (!span) {
        return undefined
      }

      const start = localToPoint(run, span.start, -1)
      if (!start) {
        return undefined
      }

      const edits: TextEdit[] = []
      for (const piece of run.pieces) {
        const startOffset = Math.max(0, span.start - piece.localStart)
        const endOffset = Math.min(
          piece.length,
          span.end - piece.localStart,
        )
        if (endOffset > startOffset) {
          edits.push({node: piece.node, start: startOffset, end: endOffset})
        }
      }

      if (edits.length === 0) {
        return undefined
      }

      return {start, edits}
    },
  }
}

/**
 * Get the nearest contenteditable host of a node.
 */
export function getGraphemeHost(node: Node): Element | null {
  return getContentEditable(node)
}
