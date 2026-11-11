import {createFileList} from './FileList'

/**
 * The access level of a `DataTransfer` during a drag session.
 *
 * - `dragstart`: the data is readable and writable.
 * - `drop`: the data is readable but protected from modifications.
 * - `protected`: metadata (`kind`/`type`) is visible, but neither the data nor
 *   the files can be read and the data can not be modified.
 */
export type DragDataTransferMode = 'dragstart' | 'drop' | 'protected'

export type DropEffect = 'none' | 'copy' | 'move' | 'link'

const dropEffects: readonly DropEffect[] = ['none', 'copy', 'move', 'link']

const effectAllows: readonly DataTransfer['effectAllowed'][] = [
  'none',
  'copy',
  'copyLink',
  'copyMove',
  'link',
  'linkMove',
  'move',
  'all',
  'uninitialized',
]

export function isDropEffect(value: unknown): value is DropEffect {
  return dropEffects.includes(value as DropEffect)
}

export function isEffectAllowed(
  value: unknown,
): value is DataTransfer['effectAllowed'] {
  return effectAllows.includes(value as DataTransfer['effectAllowed'])
}

function toAsciiLowercase(value: string) {
  return value.replace(/[A-Z]/g, char => char.toLowerCase())
}

// Mirrors the clipboard implementation: `text` and `url` are shorthands for
// their MIME types in `setData`/`clearData`, but not in `items.add`.
function normalizeFormat(format: string) {
  const type = toAsciiLowercase(format)
  return type === 'text'
    ? 'text/plain'
    : type === 'url'
      ? 'text/uri-list'
      : type
}

function getTypeMatcher(type: string, exact: boolean) {
  const [group, sub] = type.split('/')
  const isGroup = !sub || sub === '*'
  return (item: DataTransferItem) => {
    // Only string items participate in `setData`/`getData`/`clearData`.
    // Files are tracked separately through `items` and `files`.
    return (
      item.kind === 'string' &&
      (exact
        ? item.type === (isGroup ? group : type)
        : isGroup
          ? item.type.startsWith(`${group}/`)
          : item.type === group)
    )
  }
}

class DragDataTransferItem implements DataTransferItem {
  readonly kind: DataTransferItem['kind']
  readonly type: string

  constructor(
    private readonly store: DragDataTransfer,
    kind: DataTransferItem['kind'],
    type: string,
    readonly data?: string,
    readonly file?: File,
  ) {
    this.kind = kind
    this.type = type
  }

  getAsFile() {
    if (this.kind !== 'file') {
      return null
    }
    return this.store.isReadable() ? this.file ?? null : null
  }

  getAsString(callback: (data: string) => unknown) {
    if (this.kind !== 'string') {
      return
    }

    // The value is captured at call time. The callback itself is delivered
    // asynchronously - even if the access level changes in the meantime.
    const data = this.store.readString(this)
    if (data === undefined) {
      return
    }

    Promise.resolve().then(() => callback(data))
  }

  /* istanbul ignore next */
  webkitGetAsEntry(): never {
    throw new Error('not implemented')
  }
}

class DragDataTransferItemList
  extends Array<DataTransferItem>
  implements DataTransferItemList {
  constructor(private readonly store: DragDataTransfer) {
    super()
  }

  add(data: string, type: string): DataTransferItem
  add(file: File): DataTransferItem
  add(...args: never[]) {
    const isFile = typeof args[0] !== 'string'
    const item = isFile
      ? new DragDataTransferItem(
          this.store,
          'file',
          (args[0] as File).type,
          undefined,
          args[0] as File,
        )
      : new DragDataTransferItem(
          this.store,
          'string',
          toAsciiLowercase(args[1] as string),
          args[0] as string,
        )

    if (!this.store.isWritable()) {
      return item
    }

    if (isFile) {
      this.push(item)
    } else {
      // String MIME types are unique. Replacing the existing item keeps the
      // order of the item store intact.
      const index = this.findIndex(getTypeMatcher(item.type, true))
      if (index >= 0) {
        this.splice(index, 1, item)
      } else {
        this.push(item)
      }
    }

    return item
  }

  item(index: number) {
    return this[index] ?? null
  }

  remove(index: number) {
    if (!this.store.isWritable()) {
      return
    }
    this.splice(index, 1)
  }

  clear() {
    if (!this.store.isWritable()) {
      return
    }
    this.splice(0, this.length)
  }
}

/**
 * A `DataTransfer` implementation for a drag session.
 *
 * One instance is shared by every event of a gesture. The access level is
 * changed on the same object instead of swapping between different copies.
 */
export class DragDataTransfer implements DataTransfer {
  readonly items: DragDataTransferItemList

  private mode: DragDataTransferMode = 'protected'
  private effectAllowedValue: DataTransfer['effectAllowed'] = 'all'
  private dropEffectValue: DataTransfer['dropEffect'] = 'none'

  constructor(
    private readonly window: Window & typeof globalThis,
    files: File[] = [],
    data: Record<string, string> = {},
  ) {
    this.items = new DragDataTransferItemList(this)

    // The initial values are part of the drag data and are therefore applied
    // in the writable phase before the first event is dispatched.
    this.mode = 'dragstart'
    for (const file of files) {
      this.items.push(new DragDataTransferItem(this, 'file', file.type, undefined, file))
    }
    for (const [format, value] of Object.entries(data)) {
      this.setData(format, value)
    }
    this.mode = 'protected'
  }

  setMode(mode: DragDataTransferMode) {
    this.mode = mode
  }

  isReadable() {
    return this.mode === 'dragstart' || this.mode === 'drop'
  }

  isWritable() {
    return this.mode === 'dragstart'
  }

  readString(item: DragDataTransferItem) {
    return this.isReadable() ? item.data : undefined
  }

  getData(format: string) {
    if (!this.isReadable()) {
      return ''
    }

    const type = normalizeFormat(format)
    const match =
      this.items.find(getTypeMatcher(type, true)) ??
      this.items.find(getTypeMatcher(type, false))

    return match instanceof DragDataTransferItem ? match.data ?? '' : ''
  }

  setData(format: string, data: string) {
    if (!this.isWritable()) {
      return
    }

    const type = normalizeFormat(format)
    const item = new DragDataTransferItem(this, 'string', type, String(data))
    const index = this.items.findIndex(getTypeMatcher(type, true))

    if (index >= 0) {
      this.items.splice(index, 1, item)
    } else {
      this.items.push(item)
    }
  }

  clearData(format?: string) {
    if (!this.isWritable()) {
      return
    }

    if (format) {
      const type = normalizeFormat(format)
      const index = this.items.findIndex(getTypeMatcher(type, true))

      if (index >= 0) {
        this.items.splice(index, 1)
      }
    } else {
      this.items.splice(0, this.items.length)
    }
  }

  get dropEffect() {
    return this.dropEffectValue
  }

  set dropEffect(value: DataTransfer['dropEffect']) {
    if (isDropEffect(value)) {
      this.dropEffectValue = value
    }
  }

  get effectAllowed() {
    return this.effectAllowedValue
  }

  set effectAllowed(value: DataTransfer['effectAllowed']) {
    if (isEffectAllowed(value)) {
      this.effectAllowedValue = value
    }
  }

  get files() {
    const files = this.isReadable()
      ? this.items
          .filter(
            (item): item is DragDataTransferItem =>
              item instanceof DragDataTransferItem && item.kind === 'file',
          )
          .map(item => item.getAsFile() as File)
      : []

    return createFileList(this.window, files)
  }

  get types() {
    const types: string[] = []

    if (this.items.some(item => item.kind === 'file')) {
      types.push('Files')
    }

    for (const item of this.items) {
      if (item.kind === 'string' && !types.includes(item.type)) {
        types.push(item.type)
      }
    }

    Object.freeze(types)

    return types
  }

  /* istanbul ignore next */
  setDragImage() {}
}

export function createDragDataTransfer(
  window: Window & typeof globalThis,
  files: File[] = [],
  data: Record<string, string> = {},
) {
  return new DragDataTransfer(window, files, data)
}
