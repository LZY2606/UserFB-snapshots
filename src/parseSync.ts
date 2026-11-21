import { Parser } from './Parser'
import { NodeList, ParseOptions } from '.'

interface SourceLine {
  text: string
  eol: string
}

const splitLines = (input: string): SourceLine[] => {
  const lines: SourceLine[] = []
  let start = 0

  while (start < input.length) {
    const newline = input.indexOf('\n', start)

    if (newline === -1) {
      lines.push({ text: input.slice(start), eol: '' })
      break
    }

    let end = newline
    let eol = '\n'

    if (newline > start && input[newline - 1] === '\r') {
      end = newline - 1
      eol = '\r\n'
    }

    lines.push({ text: input.slice(start, end), eol })
    start = newline + 1
  }

  return lines
}

export const parseSync = (
  input: string,
  options: ParseOptions = {}
): NodeList => {
  const buffer: NodeList = []
  const parser = new Parser({
    push: node => buffer.push(node),
    preserveLayout: options.preserveLayout
  })

  if (options.preserveLayout) {
    for (const line of splitLines(input)) {
      parser.parseBlock(line.text, line.eol)
    }
  } else {
    input
      .replace(/\r\n/g, '\n')
      .split('\n')
      .forEach(line => parser.parseLine(line))
  }

  parser.flush()

  return buffer
}
