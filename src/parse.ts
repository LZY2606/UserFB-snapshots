import multipipe from 'multipipe'
import split2 from 'split2'
import { StringDecoder } from 'string_decoder'
import { createDuplex } from './utils'
import { Parser } from './Parser'
import { Node, ParseOptions } from '.'

export const parse = (options: ParseOptions = {}) => {
  let emit: (node: Node) => void

  const parser = new Parser({
    push: node => emit(node),
    preserveLayout: options.preserveLayout
  })

  let writeLine: (chunk: Buffer | string, encoding?: string) => void
  let flushLines: () => void

  if (options.preserveLayout) {
    // The layout-aware parser consumes raw chunks: line splitting and UTF-8
    // decoding happen incrementally so a block is delivered as soon as the
    // blank line that closes it has been read.
    const decoder = new StringDecoder('utf8')
    let pending = ''

    const emitLines = () => {
      while (true) {
        const newline = pending.indexOf('\n')

        if (newline === -1) {
          return
        }

        const segment = pending.slice(0, newline)
        pending = pending.slice(newline + 1)

        if (segment.endsWith('\r')) {
          parser.parseBlock(segment.slice(0, -1), '\r\n')
        } else {
          parser.parseBlock(segment, '\n')
        }
      }
    }

    writeLine = chunk => {
      pending += typeof chunk === 'string' ? chunk : decoder.write(chunk)
      emitLines()
    }

    flushLines = () => {
      pending += decoder.end()

      if (pending !== '') {
        const line = pending
        pending = ''
        parser.parseBlock(line, '')
      }
    }
  } else {
    writeLine = chunk => {
      parser.parseLine(typeof chunk === 'string' ? chunk : chunk.toString())
    }
    flushLines = () => undefined
  }

  const stream = createDuplex({
    write(chunk, _encoding, next) {
      try {
        writeLine(chunk)
      } catch (err) {
        return next(err)
      }

      next()
    }
  })

  emit = node => stream.push(node)

  let hasFinished = false

  const finish = () => {
    if (hasFinished) return
    hasFinished = true

    try {
      flushLines()
      parser.flush()
    } catch (err) {
      stream.emit('error', err)
      return
    }

    stream.push(null)
  }

  if (options.preserveLayout) {
    stream.on('finish', finish)
    return stream
  }

  const splitStream = split2()

  splitStream.on('finish', finish)

  return multipipe(splitStream, stream, {
    objectMode: true
  })
}
