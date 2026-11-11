import {type CompositionSessionImpl} from '../composition/session'
import {KeyboardHost} from './keyboard'
import {PointerHost} from './pointer'

/**
 * @internal Do not create/alter this by yourself as this type might be subject to changes.
 */
export class System {
  readonly keyboard = new KeyboardHost(this)
  readonly pointer = new PointerHost(this)

  /** @internal The composition session currently holding this System. */
  compositionSession: CompositionSessionImpl | undefined = undefined

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
