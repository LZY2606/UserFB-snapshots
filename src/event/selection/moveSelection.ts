import {getUISelection, getUIValue} from '../../document'
import {type TextUnit} from '../../options'
import {
  buildGraphemeModel,
  getGraphemeHost,
  getNextCursorPosition,
  hasOwnSelection,
  moveGraphemeOffset,
  supportsGraphemeEditing,
} from '../../utils'
import {setSelection} from './setSelection'

/**
 * Move the selection
 */
export function moveSelection(
  node: Element,
  direction: -1 | 1,
  textUnit: TextUnit = 'codeUnit',
) {
  // TODO: implement shift

  if (hasOwnSelection(node)) {
    const selection = getUISelection(node)

    if (selection.startOffset !== selection.endOffset) {
      // A non-collapsed selection collapses to the respective end.
      setSelection({
        focusNode: node,
        focusOffset:
          direction < 0 ? selection.startOffset : selection.endOffset,
      })
      return
    }

    if (textUnit === 'grapheme' && supportsGraphemeEditing(node)) {
      const nextOffset = moveGraphemeOffset(
        node,
        getUIValue(node as HTMLInputElement | HTMLTextAreaElement),
        selection.focusOffset,
        direction,
      )

      if (nextOffset !== selection.focusOffset) {
        setSelection({focusNode: node, focusOffset: nextOffset})
      }
      return
    }

    setSelection({
      focusNode: node,
      focusOffset: selection.focusOffset + direction,
    })
  } else {
    const selection = node.ownerDocument.getSelection()

    if (!selection?.focusNode) {
      return
    }

    if (!selection.isCollapsed) {
      selection[direction < 0 ? 'collapseToStart' : 'collapseToEnd']()
      return
    }

    if (textUnit === 'grapheme') {
      const host = getGraphemeHost(selection.focusNode)
      const model = host ? buildGraphemeModel(host) : undefined

      if (model) {
        const point = model.cursorPosition(
          selection.focusNode,
          selection.focusOffset,
          direction,
        )

        // A missing point means the caret is on the edge of a text run, e.g.
        // next to a `br` or a nested editing boundary. Those boundaries are
        // never crossed in grapheme mode, so the caret stays put.
        if (point) {
          setSelection({focusNode: point.node, focusOffset: point.offset})
        }
        return
      }
    }

    const nextPosition = getNextCursorPosition(
      selection.focusNode,
      selection.focusOffset,
      direction,
    )
    if (nextPosition) {
      setSelection({
        focusNode: nextPosition.node,
        focusOffset: nextPosition.offset,
      })
    }
  }
}
