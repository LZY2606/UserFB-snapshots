import { NodeList, FormatOptions } from '.'
import { Formatter } from './Formatter'

export const stringifySync = (
  list: NodeList,
  options: FormatOptions = { format: 'SRT' }
): string => {
  const formatter = new Formatter(options)

  const output = list.reduce((buffer, node) => {
    return buffer + formatter.format(node)
  }, '')

  return output + formatter.flush()
}
