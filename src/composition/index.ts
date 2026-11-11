import {type Instance} from '../setup'
import {createCompositionSession, type CompositionSession} from './session'

export type {CompositionSession} from './session'

/**
 * Start an explicit composition (IME) session on the given target.
 */
export async function compositionStart(
  this: Instance,
  target: Element,
): Promise<CompositionSession> {
  return createCompositionSession(this, target)
}
