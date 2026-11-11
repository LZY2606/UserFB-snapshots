import {
  clearInitialValue,
  commitValueAfterInput,
  getUIValue,
  setUIValue,
  UISelectionRange,
} from '../document'
import {type Instance} from '../setup'
import {
  buildGraphemeModel,
  buildTimeValue,
  EditableInputOrTextarea,
  getGraphemeDeleteSpan,
  getMaxLength,
  getNextCursorPosition,
  isElementType,
  isGraphemeSegmentationAvailable,
  isValidDateOrTimeValue,
  supportsGraphemeEditing,
  supportsMaxLength,
} from '../utils'
import {getInputRange, setSelection} from './selection'

type DateOrTimeInput = HTMLInputElement & {type: 'date' | 'time'}

function isDateOrTime(element: Element): element is DateOrTimeInput {
  return (
    isElementType(element, 'input') && ['date', 'time'].includes(element.type)
  )
}

export function input(
  instance: Instance,
  element: Element,
  data: string,
  inputType: string = 'insertText',
) {
  const inputRange = getInputRange(element)

  /* istanbul ignore if */
  if (!inputRange) {
    return
  }

  // There is no `beforeinput` event on `date` and `time` input
  if (!isDateOrTime(element)) {
    const unprevented = instance.dispatchUIEvent(element, 'beforeinput', {
      inputType,
      data,
    })

    if (!unprevented) {
      return
    }
  }

  if ('startContainer' in inputRange) {
    editContenteditable(instance, element, inputRange, data, inputType)
  } else {
    editInputElement(
      instance,
      element as EditableInputOrTextarea,
      inputRange,
      data,
      inputType,
    )
  }
}

function editContenteditable(
  instance: Instance,
  element: Element,
  inputRange: Range,
  data: string,
  inputType: string,
) {
  let del: boolean = false

  if (!inputRange.collapsed) {
    del = true
    inputRange.deleteContents()
  } else if (
    ['deleteContentBackward', 'deleteContentForward'].includes(inputType)
  ) {
    const direction =
      inputType === 'deleteContentBackward' ? (-1 as const) : (1 as const)

    const model =
      instance.config.textUnit === 'grapheme'
        ? buildGraphemeModel(element)
        : undefined

    if (model) {
      const deletion = model.deletion(
        inputRange.startContainer,
        inputRange.startOffset,
        direction,
      )

      if (deletion) {
        del = true
        // Edit the text nodes in place. Nodes are never removed or replaced,
        // so their identity and listeners survive, including empty nodes.
        for (const edit of deletion.edits) {
          edit.node.deleteData(edit.start, edit.end - edit.start)
        }
        setSelection({
          focusNode: deletion.start.node,
          focusOffset: deletion.start.offset,
        })
      }
      // A missing deletion means the caret sits on a run boundary (e.g. a
      // `br` or nested editing root). The boundary is never crossed, so the
      // content and selection stay unchanged and no `input` event fires.
    } else {
      del = deleteAtCursorPosition(inputRange, direction, inputType)
    }
  }

  if (data) {
    if (inputRange.endContainer.nodeType === 3) {
      const offset = inputRange.endOffset
      ;(inputRange.endContainer as Text).insertData(offset, data)
      inputRange.setStart(inputRange.endContainer, offset + data.length)
      inputRange.setEnd(inputRange.endContainer, offset + data.length)
    } else {
      const text = element.ownerDocument.createTextNode(data)
      inputRange.insertNode(text)
      inputRange.setStart(text, data.length)
      inputRange.setEnd(text, data.length)
    }
  }

  if (del || data) {
    instance.dispatchUIEvent(element, 'input', {inputType})
  }
}

/**
 * The historical single code-unit deletion for a collapsed contenteditable
 * caret. Used in the default `codeUnit` mode and as a fallback when the
 * runtime does not provide `Intl.Segmenter`.
 */
function deleteAtCursorPosition(
  inputRange: Range,
  direction: -1 | 1,
  inputType: string,
): boolean {
  const nextPosition = getNextCursorPosition(
    inputRange.startContainer,
    inputRange.startOffset,
    direction,
    inputType,
  )
  if (!nextPosition) {
    return false
  }

  const delRange = inputRange.cloneRange()
  if (delRange.comparePoint(nextPosition.node, nextPosition.offset) < 0) {
    delRange.setStart(nextPosition.node, nextPosition.offset)
  } else {
    delRange.setEnd(nextPosition.node, nextPosition.offset)
  }
  delRange.deleteContents()
  return true
}

function editInputElement(
  instance: Instance,
  element: EditableInputOrTextarea,
  inputRange: UISelectionRange,
  data: string,
  inputType: string,
) {
  let dataToInsert = data
  if (supportsMaxLength(element)) {
    const maxLength = getMaxLength(element)
    if (maxLength !== undefined && data.length > 0) {
      const selectionLength = inputRange.endOffset - inputRange.startOffset
      const spaceUntilMaxLength =
        maxLength - element.value.length + selectionLength
      if (spaceUntilMaxLength > 0) {
        dataToInsert = data.substring(0, spaceUntilMaxLength)
      } else {
        return
      }
    }
  }

  let effectiveRange: UISelectionRange = inputRange

  if (
    instance.config.textUnit === 'grapheme' &&
    supportsGraphemeEditing(element) &&
    isGraphemeSegmentationAvailable(element) &&
    inputRange.startOffset === inputRange.endOffset &&
    (inputType === 'deleteContentBackward' ||
      inputType === 'deleteContentForward')
  ) {
    const span = getGraphemeDeleteSpan(
      element,
      getUIValue(element),
      inputRange.startOffset,
      inputType === 'deleteContentBackward' ? -1 : 1,
    )

    if (!span) {
      // At the start/end there is no adjacent grapheme: don't change
      // anything and don't fire an `input` event.
      return
    }

    // Widen the collapsed caret to the whole grapheme cluster.
    effectiveRange = {
      ...inputRange,
      startOffset: span.start,
      endOffset: span.end,
    }
  }

  const {newValue, newOffset, oldValue} = calculateNewValue(
    dataToInsert,
    element,
    effectiveRange,
    inputType,
  )

  if (
    newValue === oldValue &&
    newOffset === effectiveRange.startOffset &&
    newOffset === effectiveRange.endOffset
  ) {
    return
  }

  if (
    isElementType(element, 'input', {type: 'number'}) &&
    !isValidNumberInput(newValue)
  ) {
    return
  }

  setUIValue(element, newValue)
  setSelection({
    focusNode: element,
    anchorOffset: newOffset,
    focusOffset: newOffset,
  })

  if (isDateOrTime(element)) {
    if (isValidDateOrTimeValue(element, newValue)) {
      commitInput(instance, element, newOffset, {})
      instance.dispatchUIEvent(element, 'change')
      clearInitialValue(element)
    }
  } else {
    commitInput(instance, element, newOffset, {
      data,
      inputType,
    })
  }
}

function calculateNewValue(
  inputData: string,
  node: EditableInputOrTextarea,
  {
    startOffset,
    endOffset,
  }: {
    startOffset: number
    endOffset: number
  },
  inputType?: string,
) {
  const value = getUIValue(node)

  const prologEnd = Math.max(
    0,
    startOffset === endOffset && inputType === 'deleteContentBackward'
      ? startOffset - 1
      : startOffset,
  )
  const prolog = value.substring(0, prologEnd)
  const epilogStart = Math.min(
    value.length,
    startOffset === endOffset && inputType === 'deleteContentForward'
      ? startOffset + 1
      : endOffset,
  )
  const epilog = value.substring(epilogStart, value.length)

  let newValue = `${prolog}${inputData}${epilog}`
  let newOffset = prologEnd + inputData.length

  if (isElementType(node, 'input', {type: 'time'} as const)) {
    const builtValue = buildTimeValue(newValue)
    if (builtValue !== '' && isValidDateOrTimeValue(node, builtValue)) {
      newValue = builtValue
      newOffset = builtValue.length
    }
  }

  return {
    oldValue: value,
    newValue,
    newOffset,
  }
}

function commitInput(
  instance: Instance,
  element: EditableInputOrTextarea,
  newOffset: number,
  inputInit: InputEventInit,
) {
  instance.dispatchUIEvent(element, 'input', inputInit)

  commitValueAfterInput(element, newOffset)
}

function isValidNumberInput(value: string) {
  // the browser allows some invalid input but not others
  // it allows up to two '-' at any place before any 'e' or one directly following 'e'
  // it allows one '.' at any place before e
  const valueParts = value.split('e', 2)
  return !(
    /[^\d.\-e]/.test(value) ||
    Number(value.match(/-/g)?.length) > 2 ||
    Number(value.match(/\./g)?.length) > 1 ||
    (valueParts[1] && !/^-?\d*$/.test(valueParts[1]))
  )
}
