import { Transform } from 'stream'
import { FormatOptions, Node } from '.'
import { Formatter } from './Formatter'

export const stringify = (options: FormatOptions) => {
  const formatter = new Formatter(options)

  return new Transform({
    objectMode: true,
    autoDestroy: false,
    transform(chunk: Node, _encoding, callback) {
      try {
        callback(null, formatter.format(chunk))
      } catch (error) {
        callback(error)
      }
    },
    flush(callback) {
      try {
        const tail = formatter.flush()

        if (tail) {
          this.push(tail)
        }

        callback()
      } catch (error) {
        callback(error)
      }
    }
  })
}
