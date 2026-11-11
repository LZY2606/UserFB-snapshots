import {type Instance} from '../setup'
import {type pointerKey} from '../system/pointer'
import {
  createDragDataTransfer,
  getWindow,
  isDropEffect,
  type DropEffect,
} from '../utils'

/**
 * Options for `userEvent.dragAndDrop`.
 */
export interface DragAndDropOptions {
  /**
   * Elements the drag passes over before reaching `target`.
   * Consecutive duplicates are visited once (but keep receiving `dragover`).
   */
  via?: Element[]
  /**
   * Initial string data of the drag session, as a map of MIME type to value.
   */
  data?: Record<string, string>
  /**
   * Files made available through `dataTransfer.files` and `dataTransfer.items`.
   */
  files?: File[]
  /**
   * The drop effect selected before each `dragover`.
   * A drop handler may select another effect on the event's `dataTransfer`.
   *
   * @default 'copy'
   */
  dropEffect?: DropEffect
}

type DragEventType =
  | 'dragstart'
  | 'drag'
  | 'dragenter'
  | 'dragleave'
  | 'dragover'
  | 'drop'
  | 'dragend'

/**
 * Simulate a drag-and-drop gesture.
 *
 * The primary mouse button is pressed on `source`, a cancelable `dragstart`
 * event is dispatched, and - unless it has been prevented - the drag moves
 * through `via` and finally reaches `target`. Every step dispatches `drag` on
 * the source, switches `dragleave`/`dragenter` when the target changes and
 * dispatches `dragover` on the current target.
 *
 * A drop only happens when the last `dragover` is prevented and the selected
 * `dropEffect` is allowed by the `effectAllowed` chosen during `dragstart`.
 * The session finishes with a `dragend` on the source carrying the final
 * effect. Releasing the mouse does not synthesize `click`/`dblclick`.
 *
 * All events of one gesture share the same `DataTransfer` and item store.
 * Data is readable and writable during `dragstart`, readable during `drop`
 * and protected during all other drag events.
 *
 * @returns The final effect: `'copy'`, `'move'`, `'link'` or `'none'`.
 */
export async function dragAndDrop(
  this: Instance,
  source: Element,
  target: Element,
  options: DragAndDropOptions = {},
): Promise<DropEffect> {
  const {via, data, files, dropEffect} = validateOptions(options)
  validateElements(this, source, target, via)

  if (this.system.isDragSessionActive()) {
    throw new Error(
      'A drag session is already active on this input system.',
    )
  }

  if (this.system.pointer.isMouseButtonPressed()) {
    throw new Error(
      'Unable to start a drag session while a mouse button is pressed.',
    )
  }

  // Claim the input system before the first event is dispatched so a derived
  // setup that shares this system is rejected during a concurrent session.
  const dataTransfer = createDragDataTransfer(
    getWindow(source),
    files,
    data,
  )
  dataTransfer.dropEffect = dropEffect

  const primaryKey = getPrimaryMouseKey(this)
  const previous = this.system.pointer.getPreviousPosition('mouse')
  const position = {target: source, coords: previous?.coords}
  let finalPosition = position

  this.system.beginDragSession()
  let pressed = false
  let started = false
  let result: DropEffect = 'none'

  try {
    pressed = true
    await this.system.pointer.press(this, primaryKey, position)

    const dispatch = (
      target: Element,
      type: DragEventType,
      init: DragEventInit = {},
    ) => {
      dataTransfer.setMode(
        type === 'dragstart'
          ? 'dragstart'
          : type === 'drop'
            ? 'drop'
            : 'protected',
      )
      try {
        return this.dispatchUIEvent(target, type, {
          ...position.coords,
          dataTransfer,
          ...init,
        })
      } finally {
        dataTransfer.setMode('protected')
      }
    }

    const startedWithPreventedDefault = !dispatch(source, 'dragstart', {
      relatedTarget: null,
    })
    started = !startedWithPreventedDefault

    if (started) {
      const effectAllowed = dataTransfer.effectAllowed
      const steps = [...via, target]
      let currentTarget: Element | undefined
      let lastTarget: Element | undefined
      let overAccepted = false

      for (const step of steps) {
        if (!source.isConnected || !step.isConnected) {
          overAccepted = false
          break
        }

        dispatch(source, 'drag', {
          relatedTarget: currentTarget ?? null,
        })

        if (!source.isConnected || !step.isConnected) {
          overAccepted = false
          break
        }

        if (currentTarget !== step) {
          if (currentTarget) {
            dispatch(currentTarget, 'dragleave', {relatedTarget: step})
          }

          dispatch(step, 'dragenter', {
            relatedTarget: currentTarget ?? source,
          })

          currentTarget = step
        }

        // The handler may select another effect, but every `dragover` starts
        // from the effect requested by the caller.
        dataTransfer.dropEffect = dropEffect

        const notPrevented = dispatch(step, 'dragover', {
          relatedTarget: null,
        })
        lastTarget = step
        overAccepted = !notPrevented

        if (!source.isConnected || !step.isConnected) {
          overAccepted = false
          break
        }

        finalPosition = {target: step, coords: position.coords}
      }

      const selectedEffect = dataTransfer.dropEffect
      if (
        overAccepted &&
        likelyNotNone(selectedEffect) &&
        effectAllowedMatches(effectAllowed, selectedEffect) &&
        lastTarget
      ) {
        result = selectedEffect
        dispatch(lastTarget, 'drop', {relatedTarget: null})
      }

      dataTransfer.dropEffect = result

      // A session that had begun is always finished, even when the source or
      // a target was removed in the meantime.
      dispatch(source, 'dragend', {
        relatedTarget: null,
      })
    }
  } finally {
    try {
      if (pressed) {
        this.system.pointer.setMousePosition(finalPosition)
        await this.system.pointer.releaseWithoutClick(
          this,
          primaryKey,
          finalPosition,
        )
        this.system.pointer.resetClickCount()
      }
    } finally {
      this.system.endDragSession()
    }
  }

  return result
}

function likelyNotNone(
  effect: DataTransfer['dropEffect'],
): effect is Exclude<DropEffect, 'none'> {
  return effect !== 'none' && isDropEffect(effect)
}

function effectAllowedMatches(
  effectAllowed: DataTransfer['effectAllowed'],
  effect: Exclude<DropEffect, 'none'>,
) {
  switch (effectAllowed) {
    case 'none':
      return false
    case 'copy':
      return effect === 'copy'
    case 'move':
      return effect === 'move'
    case 'link':
      return effect === 'link'
    case 'copyLink':
      return effect === 'copy' || effect === 'link'
    case 'copyMove':
      return effect === 'copy' || effect === 'move'
    case 'linkMove':
      return effect === 'link' || effect === 'move'
    case 'all':
    case 'uninitialized':
      return true
    default:
      return false
  }
}

function getPrimaryMouseKey(instance: Instance): pointerKey {
  return (
    instance.config.pointerMap.find(
      key =>
        key.pointerType === 'mouse' &&
        (key.button === 'primary' || key.button === 0),
    ) ?? {name: 'MouseLeft', pointerType: 'mouse', button: 0}
  )
}

function validateOptions(options: DragAndDropOptions) {
  if (
    typeof options !== 'object' ||
    options === null ||
    Array.isArray(options)
  ) {
    throw new TypeError('The drag-and-drop options must be an object.')
  }

  const via = options.via === undefined ? [] : options.via
  if (!Array.isArray(via)) {
    throw new TypeError('The `via` option must be an array of elements.')
  }

  const data = options.data === undefined ? {} : options.data
  if (typeof data !== 'object' || data === null || Array.isArray(data)) {
    throw new TypeError('The `data` option must be an object of strings.')
  }
  for (const value of Object.values(data)) {
    if (typeof value !== 'string') {
      throw new TypeError('The `data` option must be an object of strings.')
    }
  }

  const files = options.files === undefined ? [] : options.files
  if (!Array.isArray(files)) {
    throw new TypeError('The `files` option must be an array of files.')
  }
  for (const file of files) {
    if (
      typeof file !== 'object' ||
      file === null ||
      typeof file.name !== 'string' ||
      typeof file.type !== 'string'
    ) {
      throw new TypeError('The `files` option must be an array of files.')
    }
  }

  const dropEffect =
    options.dropEffect === undefined ? 'copy' : options.dropEffect
  if (!isDropEffect(dropEffect)) {
    throw new TypeError(
      `The \`dropEffect\` option must be one of: none, copy, move, link.`,
    )
  }

  return {via, data, files, dropEffect}
}

function validateElements(
  instance: Instance,
  source: Element,
  target: Element,
  via: Element[],
) {
  if (!isElement(source)) {
    throw new TypeError('The drag source must be an element.')
  }
  if (!isElement(target)) {
    throw new TypeError('The drag target must be an element.')
  }
  if (!via.every(isElement)) {
    throw new TypeError('Every element in `via` must be an element.')
  }

  if (source.getAttribute('draggable')?.toLowerCase() !== 'true') {
    throw new TypeError(
      'The drag source must explicitly have `draggable="true"`.',
    )
  }

  const document = instance.config.document
  for (const element of [source, target, ...via]) {
    if (element.ownerDocument !== document) {
      throw new TypeError(
        'The drag source, target and every element in `via` must belong to the same document.',
      )
    }
    if (!element.isConnected) {
      throw new TypeError(
        'The drag source, target and every element in `via` must be connected to the document.',
      )
    }
  }
}

function isElement(value: unknown): value is Element {
  return (
    typeof value === 'object' &&
    value !== null &&
    (value as Node).nodeType === 1
  )
}
