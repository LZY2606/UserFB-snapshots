import {KeyboardHost} from './keyboard'
import {PointerHost} from './pointer'

/**
 * @internal Do not create/alter this by yourself as this type might be subject to changes.
 */
export class System {
  readonly keyboard = new KeyboardHost(this)
  readonly pointer = new PointerHost(this)

  private dragSessionActive = false

  isDragSessionActive() {
    return this.dragSessionActive
  }

  /**
   * A drag session owns the mouse, the drag data and the event sequence.
   * Share the same `System` (e.g. through a derived `setup()`) therefore
   * cannot start a second session concurrently.
   */
  beginDragSession() {
    if (this.dragSessionActive) {
      throw new Error(
        'A drag session is already active on this input system.',
      )
    }
    this.dragSessionActive = true
  }

  endDragSession() {
    this.dragSessionActive = false
  }

  getUIEventModifiers(): EventModifierInit {
    return {
      altKey: this.keyboard.modifiers.Alt,
      ctrlKey: this.keyboard.modifiers.Control,
      metaKey: this.keyboard.modifiers.Meta,
      shiftKey: this.keyboard.modifiers.Shift,
      modifierAltGraph: this.keyboard.modifiers.AltGraph,
      modifierCapsLock: this.keyboard.modifiers.CapsLock,
      modifierFn: this.keyboard.modifiers.Fn,
      modifierFnLock: this.keyboard.modifiers.FnLock,
      modifierNumLock: this.keyboard.modifiers.NumLock,
      modifierScrollLock: this.keyboard.modifiers.ScrollLock,
      modifierSymbol: this.keyboard.modifiers.Symbol,
      modifierSymbolLock: this.keyboard.modifiers.SymbolLock,
    }
  }
}
