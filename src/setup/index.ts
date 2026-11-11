import {setupMain} from './setup'
import * as directApi from './directApi'

export type {Instance} from './setup'
export type {CompositionSession} from '../composition'

export const userEvent = {
  ...directApi,
  setup: setupMain,
} as const
