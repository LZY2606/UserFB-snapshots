import {type Instance} from '../setup'

/**
 * Editing APIs owned by an instance must not run while a composition
 * session holds the shared System.
 */
export function assertNotComposing(instance: Instance) {
  if (instance.system.compositionSession) {
    throw new Error(
      'Cannot perform editing actions while a composition session is active.',
    )
  }
}
