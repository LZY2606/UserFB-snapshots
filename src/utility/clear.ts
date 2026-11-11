import {assertNotComposing} from '../composition/guard'
import {focusElement, input, isAllSelected, selectAll} from '../event'
import {type Instance} from '../setup'
import {isDisabled, isEditable} from '../utils'

export async function clear(this: Instance, element: Element) {
  assertNotComposing(this)

  if (!isEditable(element) || isDisabled(element)) {
    throw new Error('clear()` is only supported on editable elements.')
  }

  focusElement(element)

  if (element.ownerDocument.activeElement !== element) {
    throw new Error('The element to be cleared could not be focused.')
  }

  selectAll(element)

  if (!isAllSelected(element)) {
    throw new Error('The element content to be cleared could not be selected.')
  }

  input(this, element, '', 'deleteContentBackward')
}
