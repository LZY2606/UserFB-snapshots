import {
  commitValueAfterInput,
  getUIValue,
  setUISelection,
  setUIValue,
} from '../document'
import {
  setUISelectionClean,
  setUIValueClean,
} from '../document/UI'
import {type Instance} from '../setup'
import {
  type EditableInputOrTextarea,
  getActiveElement,
  getContentEditable,
  getMaxLength,
  isContentEditable,
  isDisabled,
  isEditable,
  isElementType,
  supportsMaxLength,
} from '../utils'

/**
 * A single composition (IME) session.
 *
 * The session remembers the range that was selected when it was started.
 * Every update replaces the previously accepted candidate in that range.
 */
export interface CompositionSession {
  readonly target: Element

  /**
   * Replace the current candidate with `text`.
   *
   * Returns `false` if a `beforeinput` listener prevented the update or if
   * the input would exceed `maxlength`. In both cases nothing was changed.
   */
  update(text: string): Promise<boolean>

  /**
   * Finish the composition.
   *
   * When `text` is omitted the last accepted candidate is committed. The
   * session is closed on success.
   */
  commit(text?: string): Promise<boolean>

  /**
   * Restore the value/selection that existed before the composition started.
   * The session is closed afterwards.
   */
  cancel(): Promise<void>
}

const supportedInputTypes = ['text', 'search', 'tel', 'url', 'password']

type CompositionKind = 'input' | 'contenteditable'

interface NodePosition {
  node: Node
  offset: number
}

interface InputSnapshot {
  value: string
  startOffset: number
  endOffset: number
  direction: 'forward' | 'backward' | 'none'
}

interface ContentEditableSnapshot {
  start: NodePosition
  end: NodePosition
  anchor: NodePosition
  focus: NodePosition
  textValues: Array<[Text, string]>
}

interface InputState {
  kind: 'input'
  value: string
  selectionStart: number
  selectionEnd: number
}

interface ContentEditableState {
  kind: 'contenteditable'
  nodes: Node[]
  signatures: string[]
  anchorNode: Node | null
  anchorOffset: number
  focusNode: Node | null
  focusOffset: number
}

type EditableState = InputState | ContentEditableState

export class CompositionSessionImpl implements CompositionSession {
  readonly target: Element
  isOpen = true

  private readonly instance: Instance
  private readonly kind: CompositionKind
  private readonly inputSnapshot?: InputSnapshot
  private readonly contentEditableSnapshot?: ContentEditableSnapshot
  private readonly insertedTextNodes: Text[] = []

  private inputRange?: {startOffset: number; endOffset: number}
  private contentEditableRange?: {start: NodePosition; end: NodePosition}
  private currentText = ''
  private expectedState: EditableState
  private inOperation = false

  constructor(instance: Instance, target: Element, kind: CompositionKind) {
    this.instance = instance
    this.target = target
    this.kind = kind

    if (kind === 'input') {
      const element = target as EditableInputOrTextarea
      const startOffset = element.selectionStart
      const endOffset = element.selectionEnd

      /* istanbul ignore if */
      if (startOffset === null || endOffset === null) {
        throw new Error('The target does not expose a valid selection.')
      }

      this.inputSnapshot = {
        value: getUIValue(element),
        startOffset,
        endOffset,
        direction: element.selectionDirection ?? 'none',
      }
      this.inputRange = {startOffset, endOffset}
    } else {
      this.contentEditableSnapshot = captureContentEditableSnapshot(target)
      this.contentEditableRange = {
        start: this.contentEditableSnapshot.start,
        end: this.contentEditableSnapshot.end,
      }
    }

    // The expected state starts out as the untouched state. It is replaced
    // with the state we created after every accepted update.
    this.expectedState = this.captureState()
  }

  async start(): Promise<this> {
    const system = this.instance.system
    if (system.compositionSession) {
      throw new Error(
        'A composition session is already active on this System.',
      )
    }

    system.compositionSession = this

    try {
      const ownerBeforeEvent = this.captureState()

      this.instance.dispatchUIEvent(this.target, 'compositionstart', {
        data: '',
      })

      if (!this.isOwnerIntact(ownerBeforeEvent)) {
        this.loseOwnership()
      }

      this.expectedState = this.captureState()

      return this
    } catch (error) {
      this.close()
      throw error
    }
  }

  async update(text: string): Promise<boolean> {
    this.startOperation()

    try {
      this.assertOwner()

      const candidate = String(text)

      if (this.kind === 'input' && !this.fitsMaxLength(candidate)) {
        return false
      }

      this.instance.dispatchUIEvent(this.target, 'compositionupdate', {
        data: candidate,
      })

      this.assertOwner()

      const unprevented = this.instance.dispatchUIEvent(
        this.target,
        'beforeinput',
        {
          inputType: 'insertCompositionText',
          data: candidate,
          isComposing: true,
        },
      )

      this.assertOwner()

      if (!unprevented) {
        return false
      }

      this.replace(candidate)
      this.currentText = candidate
      this.expectedState = this.captureState()

      this.dispatchInput('insertCompositionText', candidate, true)

      return true
    } finally {
      this.inOperation = false
    }
  }

  async commit(text?: string): Promise<boolean> {
    this.startOperation()

    try {
      this.assertOwner()

      const finalText = text === undefined ? this.currentText : String(text)

      if (this.kind === 'input' && !this.fitsMaxLength(finalText)) {
        return false
      }

      const unprevented = this.instance.dispatchUIEvent(
        this.target,
        'beforeinput',
        {
          inputType: 'insertFromComposition',
          data: finalText,
          isComposing: false,
        },
      )

      this.assertOwner()

      if (!unprevented) {
        return false
      }

      this.replace(finalText)
      this.currentText = finalText
      this.expectedState = this.captureState()

      this.dispatchInput('insertFromComposition', finalText, false)

      this.instance.dispatchUIEvent(this.target, 'compositionend', {
        data: finalText,
      })

      this.close()

      return true
    } finally {
      this.inOperation = false
    }
  }

  async cancel(): Promise<void> {
    this.startOperation()

    try {
      this.assertOwner()

      if (this.kind === 'input') {
        this.restoreInput()
      } else {
        this.restoreContentEditable()
      }

      this.dispatchInput('deleteCompositionText', '', false)

      this.instance.dispatchUIEvent(this.target, 'compositionend', {
        data: '',
      })

      this.close()
    } finally {
      this.inOperation = false
    }
  }

  private startOperation() {
    if (!this.isOpen) {
      throw new Error('The composition session has already been closed.')
    }
    if (this.inOperation) {
      throw new Error(
        'A composition session method cannot be called from its own event listener.',
      )
    }
    this.inOperation = true
  }

  private close() {
    if (!this.isOpen) {
      return
    }

    this.isOpen = false

    if (this.instance.system.compositionSession === this) {
      this.instance.system.compositionSession = undefined
    }

    if (this.kind === 'input') {
      setUIValueClean(this.target as EditableInputOrTextarea)
      setUISelectionClean(this.target as EditableInputOrTextarea)
    }
  }

  private loseOwnership(): never {
    this.close()
    throw new Error(
      'The composition session lost ownership of the target.',
    )
  }

  private assertOwner() {
    if (!this.isOwnerIntact(this.expectedState)) {
      this.loseOwnership()
    }
  }

  private isOwnerIntact(expected: EditableState) {
    if (
      !this.target.isConnected ||
      this.target.ownerDocument !== this.instance.config.document ||
      getActiveElement(this.target.ownerDocument) !== this.target
    ) {
      return false
    }

    const actual = this.captureState()
    if (actual.kind !== expected.kind) {
      return false
    }

    if (actual.kind === 'input' && expected.kind === 'input') {
      return (
        actual.value === expected.value &&
        actual.selectionStart === expected.selectionStart &&
        actual.selectionEnd === expected.selectionEnd
      )
    }

    if (
      actual.kind === 'contenteditable' &&
      expected.kind === 'contenteditable'
    ) {
      return (
        actual.nodes.length === expected.nodes.length &&
        actual.nodes.every((node, i) => node === expected.nodes[i]) &&
        actual.signatures.every(
          (signature, i) => signature === expected.signatures[i],
        ) &&
        actual.anchorNode === expected.anchorNode &&
        actual.anchorOffset === expected.anchorOffset &&
        actual.focusNode === expected.focusNode &&
        actual.focusOffset === expected.focusOffset
      )
    }

    /* istanbul ignore next */
    return false
  }

  private captureState(): EditableState {
    if (this.kind === 'input') {
      const element = this.target as EditableInputOrTextarea
      return {
        kind: 'input',
        value: getUIValue(element),
        selectionStart: element.selectionStart ?? 0,
        selectionEnd: element.selectionEnd ?? 0,
      }
    }

    const selection = this.target.ownerDocument.getSelection()
    const nodes: Node[] = []
    const signatures: string[] = []
    collectStructure(this.target, nodes, signatures)

    return {
      kind: 'contenteditable',
      nodes,
      signatures,
      anchorNode: selection?.anchorNode ?? null,
      anchorOffset: selection?.anchorOffset ?? 0,
      focusNode: selection?.focusNode ?? null,
      focusOffset: selection?.focusOffset ?? 0,
    }
  }

  private fitsMaxLength(text: string) {
    const element = this.target as EditableInputOrTextarea
    if (!supportsMaxLength(element)) {
      return true
    }

    const maxLength = getMaxLength(element)
    if (maxLength === undefined) {
      return true
    }

    const range = this.inputRange as {
      startOffset: number
      endOffset: number
    }
    const valueLength = getUIValue(element).length
    const replacedLength = range.endOffset - range.startOffset
    const lengthAfterInsert = valueLength - replacedLength + text.length

    return lengthAfterInsert <= maxLength
  }

  private replace(text: string) {
    if (this.kind === 'input') {
      this.replaceInput(text)
    } else {
      this.replaceContentEditable(text)
    }
  }

  private replaceInput(text: string) {
    const element = this.target as EditableInputOrTextarea
    const range = this.inputRange as {
      startOffset: number
      endOffset: number
    }
    const value = getUIValue(element)
    const newValue =
      value.slice(0, range.startOffset) + text + value.slice(range.endOffset)

    setUIValue(element, newValue)

    const endOffset = range.startOffset + text.length
    this.inputRange = {startOffset: range.startOffset, endOffset}
    setUISelection(element, {focusOffset: endOffset})
  }

  private replaceContentEditable(text: string) {
    const range = this.contentEditableRange as {
      start: NodePosition
      end: NodePosition
    }

    const newRange = replaceTextInContentEditable(
      this.target,
      range,
      text,
      this.insertedTextNodes,
    )

    this.contentEditableRange = newRange

    setDocumentSelectionCollapsed(this.target, newRange.end)
  }

  private dispatchInput(
    inputType: string,
    data: string,
    isComposing: boolean,
  ) {
    this.instance.dispatchUIEvent(this.target, 'input', {
      inputType,
      data,
      isComposing,
    })

    if (this.kind === 'input') {
      commitValueAfterInput(
        this.target as EditableInputOrTextarea,
        (this.target as EditableInputOrTextarea).selectionEnd ?? 0,
      )
    }
  }

  private restoreInput() {
    const element = this.target as EditableInputOrTextarea
    const snapshot = this.inputSnapshot as InputSnapshot

    setUIValue(element, snapshot.value)
    this.inputRange = {
      startOffset: snapshot.startOffset,
      endOffset: snapshot.endOffset,
    }
    element.setSelectionRange(
      snapshot.startOffset,
      snapshot.endOffset,
      snapshot.direction,
    )
  }

  private restoreContentEditable() {
    const snapshot = this.contentEditableSnapshot as ContentEditableSnapshot

    for (const node of this.insertedTextNodes) {
      node.parentNode?.removeChild(node)
    }
    this.insertedTextNodes.length = 0

    for (const [node, value] of snapshot.textValues) {
      node.nodeValue = value
    }

    this.contentEditableRange = {
      start: snapshot.start,
      end: snapshot.end,
    }

    const selection = this.target.ownerDocument.getSelection()
    selection?.setBaseAndExtent(
      snapshot.anchor.node,
      snapshot.anchor.offset,
      snapshot.focus.node,
      snapshot.focus.offset,
    )
  }
}

/**
 * Validate the target and create a composition session for it.
 */
export async function createCompositionSession(
  instance: Instance,
  target: Element,
): Promise<CompositionSession> {
  const kind = getCompositionKind(instance, target)
  const session = new CompositionSessionImpl(instance, target, kind)

  return session.start()
}

function getCompositionKind(
  instance: Instance,
  target: Element,
): CompositionKind {
  if (!target.isConnected) {
    throw new Error('The composition target is not connected to a document.')
  }

  if (target.ownerDocument !== instance.config.document) {
    throw new Error('The composition target is not in the configured document.')
  }

  if (getActiveElement(target.ownerDocument) !== target) {
    throw new Error('The composition target must be focused.')
  }

  if (isDisabled(target)) {
    throw new Error('The composition target is disabled.')
  }

  if (isElementType(target, 'input')) {
    if (!supportedInputTypes.includes(target.type)) {
      throw new Error(
        `Input type "${target.type}" does not support composition input.`,
      )
    }
    if (!isEditable(target)) {
      throw new Error('The composition target is read-only.')
    }
    assertValidInputSelection(target)
    return 'input'
  }

  if (isElementType(target, 'textarea')) {
    if (!isEditable(target)) {
      throw new Error('The composition target is read-only.')
    }
    assertValidInputSelection(target)
    return 'input'
  }

  if (
    isContentEditable(target) &&
    getContentEditable(target) === target &&
    isEditable(target)
  ) {
    captureContentEditableSnapshot(target)
    return 'contenteditable'
  }

  throw new Error(
    'The composition target must be an editable text input, textarea or contenteditable root.',
  )
}

function assertValidInputSelection(
  element: HTMLInputElement | HTMLTextAreaElement,
) {
  const valueLength = getUIValue(element).length
  const start = element.selectionStart
  const end = element.selectionEnd

  if (
    start === null ||
    end === null ||
    start < 0 ||
    end < start ||
    end > valueLength
  ) {
    throw new Error('The composition target has an invalid selection.')
  }
}

function captureContentEditableSnapshot(
  target: Element,
): ContentEditableSnapshot {
  const selection = target.ownerDocument.getSelection()

  if (!selection || selection.rangeCount === 0) {
    throw new Error('The contenteditable target has an invalid selection.')
  }

  const range = selection.getRangeAt(0)
  const anchorNode = selection.anchorNode
  const focusNode = selection.focusNode

  if (
    !anchorNode ||
    !focusNode ||
    !isEndpointInEditableRoot(anchorNode, target) ||
    !isEndpointInEditableRoot(focusNode, target) ||
    !isEndpointInEditableRoot(range.startContainer, target) ||
    !isEndpointInEditableRoot(range.endContainer, target)
  ) {
    throw new Error(
      'The contenteditable selection must stay within one editing root.',
    )
  }

  assertSelectionDoesNotCrossRoots(range, target)

  const textValues: Array<[Text, string]> = []
  collectTextNodes(target, textValues)

  return {
    start: {node: range.startContainer, offset: range.startOffset},
    end: {node: range.endContainer, offset: range.endOffset},
    anchor: {node: anchorNode, offset: selection.anchorOffset},
    focus: {node: focusNode, offset: selection.focusOffset},
    textValues,
  }
}

function isEndpointInEditableRoot(node: Node, root: Element) {
  if (node === root) {
    return true
  }

  if (!root.contains(node)) {
    return false
  }

  const element = node.nodeType === 1 ? (node as Element) : node.parentElement
  if (!element) {
    return false
  }

  for (let el: Element | null = element; el && el !== root; el = el.parentElement) {
    if (el.getAttribute('contenteditable') === 'false') {
      return false
    }
  }

  return getContentEditable(node) === root
}

function assertSelectionDoesNotCrossRoots(range: Range, root: Element) {
  if (range.collapsed) {
    return
  }

  for (const element of Array.from(root.querySelectorAll('[contenteditable]'))) {
    const value = element.getAttribute('contenteditable')

    if (value === 'false') {
      if (range.intersectsNode(element)) {
        throw new Error(
          'The contenteditable selection must not cross a non-editable region.',
        )
      }
    } else if (
      (value === '' || value === 'true') &&
      getContentEditable(element) === element &&
      range.intersectsNode(element)
    ) {
      throw new Error(
        'The contenteditable selection must stay within one editing root.',
      )
    }
  }
}

function collectTextNodes(
  root: Element,
  result: Array<[Text, string]>,
) {
  for (const child of Array.from(root.childNodes)) {
    if (child.nodeType === 3) {
      result.push([child as Text, (child as Text).nodeValue ?? ''])
    } else {
      collectTextNodes(child as Element, result)
    }
  }
}

function collectTextNodeList(root: Element, result: Text[]) {
  for (const child of Array.from(root.childNodes)) {
    if (child.nodeType === 3) {
      result.push(child as Text)
    } else {
      collectTextNodeList(child as Element, result)
    }
  }
}

function collectStructure(
  root: Element,
  nodes: Node[],
  signatures: string[],
) {
  for (const child of Array.from(root.childNodes)) {
    nodes.push(child)
    if (child.nodeType === 3) {
      signatures.push(`T:${(child as Text).nodeValue ?? ''}`)
    } else if (child.nodeType === 1) {
      const element = child as Element
      signatures.push(
        `E:${element.tagName}:${Array.from(element.attributes)
          .map(attribute => `${attribute.name}=${attribute.value}`)
          .join(';')}`,
      )
      collectStructure(element, nodes, signatures)
    } else {
      signatures.push(`N:${child.nodeType}`)
    }
  }
}

function replaceTextInContentEditable(
  root: Element,
  range: {start: NodePosition; end: NodePosition},
  text: string,
  insertedTextNodes: Text[],
): {start: NodePosition; end: NodePosition} {
  const rangeObject = root.ownerDocument.createRange()
  rangeObject.setStart(range.start.node, range.start.offset)
  rangeObject.setEnd(range.end.node, range.end.offset)

  const textNodes: Text[] = []
  collectTextNodeList(root, textNodes)
  const intersections: Array<{node: Text; start: number; end: number}> = []

  for (const node of textNodes) {
    const length = node.nodeValue?.length ?? 0
    let start = 0
    let end = length

    if (node === range.start.node) {
      start = range.start.offset
    }
    if (node === range.end.node) {
      end = range.end.offset
    }

    if (range.start.node !== node && rangeObject.comparePoint(node, 0) > 0) {
      continue
    }
    if (
      range.end.node !== node &&
      rangeObject.comparePoint(node, length) < 0
    ) {
      continue
    }

    if (start < end) {
      intersections.push({node, start, end})
    }
  }

  for (const {node, start, end} of intersections) {
    node.deleteData(start, end - start)
  }

  const insertion = range.start

  if (text) {
    if (insertion.node.nodeType === 3) {
      const node = insertion.node as Text
      node.insertData(insertion.offset, text)

      return {
        start: {node, offset: insertion.offset},
        end: {node, offset: insertion.offset + text.length},
      }
    }

    const node = root.ownerDocument.createTextNode(text)
    const referenceNode = insertion.node.childNodes[insertion.offset] ?? null
    insertion.node.insertBefore(node, referenceNode)
    insertedTextNodes.push(node)

    return {
      start: {node, offset: 0},
      end: {node, offset: text.length},
    }
  }

  if (insertion.node.nodeType === 3) {
    return {
      start: insertion,
      end: insertion,
    }
  }

  return {
    start: {node: insertion.node, offset: insertion.offset},
    end: {node: insertion.node, offset: insertion.offset},
  }
}

function setDocumentSelectionCollapsed(
  target: Element,
  position: NodePosition,
) {
  target.ownerDocument
    .getSelection()
    ?.setBaseAndExtent(
      position.node,
      position.offset,
      position.node,
      position.offset,
    )
}
