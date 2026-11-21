'use strict'

const { Readable } = require('node:stream')
const { checked } = require('../task_support/check.cjs')
const {
  parse,
  parseSync,
  stringify,
  stringifySync
} = require('../.task-build/index.js')

const VTT = { format: 'WebVTT', preserveLayout: true }
const LAYOUT = { preserveLayout: true }

const stringifyLayout = nodes => stringifySync(nodes, VTT)
const clone = value => JSON.parse(JSON.stringify(value))
const cues = nodes => nodes.filter(node => node.type === 'cue')

const streamNodes = (chunks, options) =>
  new Promise((resolve, reject) => {
    const source = new Readable({ read() {} })
    const parsed = source.pipe(parse(options))
    const output = []

    parsed
      .on('data', node => output.push(node))
      .on('error', reject)
      .on('end', () => resolve(output))

    chunks.forEach(chunk => source.push(chunk))
    source.push(null)
  })

const streamString = (nodes, options) =>
  new Promise((resolve, reject) => {
    const source = new Readable({ objectMode: true, read() {} })
    let output = ''

    source
      .pipe(stringify(options))
      .on('data', chunk => {
        output += chunk
      })
      .on('error', reject)
      .on('end', () => resolve(output))

    nodes.forEach(node => source.push(node))
    source.push(null)
  })

const DOC =
  '﻿WEBVTT - 布局测试\r\n' +
  '\r\n' +
  'STYLE\r\n' +
  '::cue { color: lime }\r\n' +
  '\r\n' +
  'REGION\r\n' +
  'id:main\r\n' +
  '\r\n' +
  'NOTE 注释\r\n' +
  '第二行\r\n' +
  '\r\n' +
  '007\r\n' +
  '00:00:01.000 --> 00:00:02.500 align:start\r\n' +
  '中文🙂\r\n' +
  '\r\n' +
  '00:03.000 --> 00:04.000\r\n' +
  'Second\n' +
  '\r\n' +
  'third\r\n' +
  '00:05.000 --> 00:06.000 line:90%\r\n' +
  'Third line'

checked('untouched', async ({ check }) => {
  const nodes = parseSync(DOC, LAYOUT)

  check(stringifyLayout(nodes), DOC)

  check(
    nodes.map(node => node.type),
    ['header', 'metadata', 'metadata', 'metadata', 'cue', 'cue', 'cue']
  )

  const metadata = nodes.filter(node => node.type === 'metadata')
  check(
    metadata.map(node => node.data.kind),
    ['STYLE', 'REGION', 'NOTE']
  )
  check(metadata[0].data.text, 'STYLE\n::cue { color: lime }')
  check(metadata[2].data.text, 'NOTE 注释\n第二行')

  const [first, second, third] = cues(nodes)
  check(first.data.identifier, '007')
  check(first.data.settings, 'align:start')
  check(first.data.text, '中文🙂')
  check(second.data.text, 'Second')
  check(third.data.identifier, 'third')
  check(third.data.text, 'Third line')

  // A BOM, CRLF line endings, mixed endings, a missing final newline and the
  // original number of blank lines must survive an untouched round-trip.
  check(stringifyLayout(clone(nodes)), DOC)
  check(stringifyLayout(clone(nodes)).charCodeAt(0), 0xfeff)

  const noFinalNewline = 'WEBVTT\r\n\r\n00:01.000 --> 00:02.000\r\nA'
  check(
    stringifyLayout(parseSync(noFinalNewline, LAYOUT)),
    noFinalNewline
  )

  const mixed = 'WEBVTT\n\n00:01.000 --> 00:02.000\r\nA\r\n\r\n00:03.000 --> 00:04.000\nB'
  check(stringifyLayout(parseSync(mixed, LAYOUT)), mixed)

  const blankRuns = 'WEBVTT\n\n\n\n00:01.000 --> 00:02.000\nA\n\n\n\n'
  check(stringifyLayout(parseSync(blankRuns, LAYOUT)), blankRuns)

  // SRT content keeps the old shape and is not given layout metadata.
  const srt = '1\n00:00:01,000 --> 00:00:02,500\n字幕🙂\n'
  const srtNodes = parseSync(srt, LAYOUT)
  check(JSON.stringify(srtNodes), JSON.stringify(parseSync(srt)))
  check(stringifySync(srtNodes, { format: 'SRT', preserveLayout: true }), srt)
})

checked('live-edits', async ({ check }) => {
  const source =
    '﻿WEBVTT\r\n' +
    '\r\n' +
    'NOTE 原文\r\n' +
    '\r\n' +
    '007\r\n' +
    '00:00:01.000 --> 00:00:02.500 align:start\r\n' +
    '第一条\r\n' +
    '\r\n' +
    '00:03.000 --> 00:04.000\r\n' +
    '第二条\r\n' +
    '\r\n' +
    'third\r\n' +
    '00:05.000 --> 00:06.000\r\n' +
    '第三条'

  const nodes = parseSync(source, LAYOUT)
  const copy = clone(nodes)

  // Editing a JSON copy must not touch the original nodes.
  copy[3].data.text = '第二条（改）'
  copy[3].data.start = 3500
  copy[3].data.end = 4500
  copy[3].data.settings = 'line:0'
  copy[3].data.identifier = '42'
  check(nodes[3].data.text, '第二条')
  check(nodes[3].data.start, 3000)
  check(nodes[3].data.identifier, undefined)

  const expected =
    '﻿WEBVTT\r\n' +
    '\r\n' +
    'NOTE 原文\r\n' +
    '\r\n' +
    '007\r\n' +
    '00:00:01.000 --> 00:00:02.500 align:start\r\n' +
    '第一条\r\n' +
    '\r\n' +
    '42\r\n' +
    '00:00:03.500 --> 00:00:04.500 line:0\r\n' +
    '第二条（改）\r\n' +
    '\r\n' +
    'third\r\n' +
    '00:05.000 --> 00:06.000\r\n' +
    '第三条'

  check(stringifyLayout(copy), expected)

  // Editing the header keeps the BOM that belongs to it.
  const headerEdit = clone(nodes)
  headerEdit[0].data = 'WEBVTT - 新头部'
  check(
    stringifyLayout(headerEdit),
    source.replace('WEBVTT', 'WEBVTT - 新头部')
  )

  // Editing a metadata block regenerates only that block.
  const metadataEdit = clone(nodes)
  metadataEdit[1].data.text = 'NOTE 修改后的注释'
  check(
    stringifyLayout(metadataEdit),
    source.replace('NOTE 原文', 'NOTE 修改后的注释')
  )

  // Empty cue bodies, numeric and non-numeric identifiers survive verbatim.
  const edge =
    'WEBVTT\n' +
    '\n' +
    '42\n' +
    '00:00:01.000 --> 00:00:02.000\n' +
    '\n' +
    'not-a-number\n' +
    '00:00:03.000 --> 00:00:04.000\n' +
    'B\n'
  const edgeNodes = parseSync(edge, LAYOUT)
  check(edgeNodes[1].data.identifier, '42')
  check(edgeNodes[1].data.text, '')
  check(edgeNodes[2].data.identifier, 'not-a-number')
  check(stringifyLayout(edgeNodes), edge)

  // A timeline arrow inside a NOTE stays inside the metadata block.
  const note =
    'WEBVTT\n' +
    '\n' +
    'NOTE 这里有 00:01.000 --> 00:02.000\n' +
    '仍然是注释\n' +
    '\n' +
    '00:03.000 --> 00:04.000\n' +
    'Hi\n'
  const noteNodes = parseSync(note, LAYOUT)
  check(noteNodes.map(node => node.type), [
    'header',
    'metadata',
    'cue'
  ])
  check(noteNodes[1].data.kind, 'NOTE')
  check(noteNodes[1].data.text, 'NOTE 这里有 00:01.000 --> 00:02.000\n仍然是注释')
  check(stringifyLayout(noteNodes), note)

  // Metadata blocks are ignored by SRT output and written for WebVTT.
  check(
    stringifySync(noteNodes, { format: 'SRT', preserveLayout: true }),
    '1\n00:00:03,000 --> 00:00:04,000\nHi\n'
  )
  check(
    stringifySync(noteNodes, { format: 'WebVTT' }),
    'WEBVTT\n\nNOTE 这里有 00:01.000 --> 00:02.000\n仍然是注释\n\n' +
      '1\n00:00:03.000 --> 00:00:04.000\nHi\n'
  )
})

checked('reorder', async ({ check }) => {
  const source =
    'WEBVTT\n' +
    '\n' +
    '00:01.000 --> 00:02.000\n' +
    'A\n' +
    '\n' +
    '00:03.000 --> 00:04.000\n' +
    'B\n' +
    '\n' +
    '00:05.000 --> 00:06.000\n' +
    'C\n'
  const nodes = parseSync(source, LAYOUT)

  // Move the last block to the middle. The separator of each original block
  // travels with it.
  const moved = clone(nodes)
  moved.splice(1, 0, moved.pop())
  check(
    stringifyLayout(moved),
    'WEBVTT\n' +
      '\n' +
      '00:05.000 --> 00:06.000\n' +
      'C\n' +
      '\n' +
      '00:01.000 --> 00:02.000\n' +
      'A\n' +
      '\n' +
      '00:03.000 --> 00:04.000\n' +
      'B\n' +
      '\n'
  )

  // Delete a block in the middle.
  const removed = clone(nodes)
  removed.splice(2, 1)
  check(
    stringifyLayout(removed),
    'WEBVTT\n\n00:01.000 --> 00:02.000\nA\n\n00:05.000 --> 00:06.000\nC\n'
  )

  // Insert a new cue: it uses LF and one blank line as its separator.
  const inserted = clone(nodes)
  inserted.splice(2, 0, {
    type: 'cue',
    data: { start: 2500, end: 2900, text: '新块' }
  })
  check(
    stringifyLayout(inserted),
    'WEBVTT\n' +
      '\n' +
      '00:01.000 --> 00:02.000\n' +
      'A\n' +
      '\n' +
      '00:00:02.500 --> 00:00:02.900\n' +
      '新块\n' +
      '\n' +
      '00:03.000 --> 00:04.000\n' +
      'B\n' +
      '\n' +
      '00:05.000 --> 00:06.000\n' +
      'C\n'
  )

  // A new last block is terminated by a single newline.
  const appended = clone(nodes)
  appended.push({ type: 'cue', data: { start: 7000, end: 8000, text: 'D' } })
  check(
    stringifyLayout(appended),
    source + '\n00:00:07.000 --> 00:00:08.000\nD\n'
  )

  // Copies are independent: editing one must not change the other.
  const duplicated = clone(nodes)
  duplicated.splice(2, 0, clone(duplicated[1]))
  duplicated[2].data.text = 'A copy'
  check(
    stringifyLayout(duplicated),
    'WEBVTT\n' +
      '\n' +
      '00:01.000 --> 00:02.000\n' +
      'A\n' +
      '\n' +
      '00:00:01.000 --> 00:00:02.000\n' +
      'A copy\n' +
      '\n' +
      '00:03.000 --> 00:04.000\n' +
      'B\n' +
      '\n' +
      '00:05.000 --> 00:06.000\n' +
      'C\n'
  )
  check(duplicated[3].data.text, 'B')

  // A block whose original separator has no blank line is padded to exactly
  // one blank line so that neighbouring blocks do not stick together.
  const glued =
    'WEBVTT\n' + '\n' + '00:01.000 --> 00:02.000\n' + 'A\n' +
    '00:03.000 --> 00:04.000\n' + 'B\n'
  const glueNodes = parseSync(glued, LAYOUT)
  check(glueNodes.length, 3)
  check(
    stringifyLayout(glueNodes),
    'WEBVTT\n\n00:01.000 --> 00:02.000\nA\n\n00:03.000 --> 00:04.000\nB\n'
  )

  // The BOM is emitted once by the document header, even when the header is
  // duplicated or other blocks are moved around.
  const bom = '﻿WEBVTT\n\n00:01.000 --> 00:02.000\nA\n'
  const bomNodes = parseSync(bom, LAYOUT)
  const duplicatedHeader = clone(bomNodes)
  duplicatedHeader.splice(1, 0, clone(duplicatedHeader[0]))
  duplicatedHeader[1].data = 'WEBVTT copy'
  const bomOutput = stringifyLayout(duplicatedHeader)
  check(bomOutput.split('﻿').length - 1, 1)
  check(bomOutput, '﻿WEBVTT\n\nWEBVTT copy\n\n00:01.000 --> 00:02.000\nA\n')

  // Moving the header block keeps the BOM attached to it, exactly once.
  const movedCue = clone(bomNodes)
  movedCue.push(movedCue.shift())
  const movedOutput = stringifyLayout(movedCue)
  check(movedOutput.split('\ufeff').length - 1, 1)
  check(
    movedOutput,
    '00:01.000 --> 00:02.000\nA\n\n\ufeffWEBVTT\n\n'
  )
})

checked('streaming', async ({ check }) => {
  const source =
    '﻿WEBVTT\r\n' +
    '\r\n' +
    'NOTE 流式\r\n' +
    '\r\n' +
    '007\r\n' +
    '00:00:01.000 --> 00:00:02.500\r\n' +
    '中文🙂\r\n' +
    '\r\n' +
    '00:03.000 --> 00:04.000\r\n' +
    '最后一块'

  const bytes = Buffer.from(source)
  const emojiAt = bytes.indexOf(Buffer.from('🙂'))
  const crlfAt = bytes.indexOf(Buffer.from('\r\n\r\n'))
  const cuts = [
    0,
    2,
    crlfAt + 1,
    crlfAt + 2,
    emojiAt + 1,
    emojiAt + 2,
    emojiAt + 3,
    bytes.length - 3,
    bytes.length
  ].filter((value, index, all) => all.indexOf(value) === index)
  const chunks = []
  for (let index = 1; index < cuts.length; index++) {
    if (cuts[index] > cuts[index - 1]) {
      chunks.push(bytes.slice(cuts[index - 1], cuts[index]))
    }
  }

  const syncNodes = parseSync(source, LAYOUT)
  const streamedNodes = await streamNodes(chunks, LAYOUT)

  check(JSON.stringify(streamedNodes), JSON.stringify(syncNodes))
  check(
    await streamString(streamedNodes, VTT),
    stringifyLayout(syncNodes)
  )
  check(await streamString(streamedNodes, VTT), source)

  // A block is delivered as soon as the separator that follows it has been
  // read, without waiting for the end of the stream.
  const early = []
  const earlySource = new Readable({ read() {} })
  const earlyParser = earlySource.pipe(parse(LAYOUT))
  earlyParser.on('data', node => early.push(node))

  earlySource.push(
    Buffer.from('WEBVTT\n\n00:01.000 --> 00:02.000\nA\n\n')
  )
  await new Promise(resolve => setTimeout(resolve, 30))
  check(early.map(node => node.type), ['header', 'cue'])
  check(early[1].data.text, 'A')

  earlySource.push(Buffer.from('00:03.000 --> 00:04.000\nB'))
  earlySource.push(null)
  await new Promise((resolve, reject) => {
    earlyParser.on('error', reject)
    earlyParser.on('end', resolve)
  })
  check(early.length, 3)

  // The last block has no trailing newline: flush delivers it exactly once.
  const lastOnly = await streamNodes(
    [Buffer.from('WEBVTT\n\n00:01.000 --> 00:02.000\nA')],
    LAYOUT
  )
  check(lastOnly.length, 2)
  check(cues(lastOnly).map(cue => cue.data.text), ['A'])

  // Legacy streaming still produces the same nodes as the sync entry point.
  const legacySource = '1\n00:00:01,000 --> 00:00:02,500\n字幕🙂\n'
  check(
    JSON.stringify(
      await streamNodes([Buffer.from(legacySource)], undefined)
    ),
    JSON.stringify(parseSync(legacySource))
  )
})

checked('legacy', async ({ check }) => {
  const srt =
    '1\n00:00:01,000 --> 00:00:02,500\n字幕🙂\n\n' +
    '2\n00:00:03,000 --> 00:00:04,000\nSecond\n'

  const nodes = parseSync(srt)
  check(JSON.stringify(nodes), JSON.stringify([
    { type: 'cue', data: { start: 1000, end: 2500, text: '字幕🙂' } },
    { type: 'cue', data: { start: 3000, end: 4000, text: 'Second' } }
  ]))
  check(stringifySync(nodes, { format: 'SRT' }), srt)

  // Default WebVTT output keeps sequential numbering and ignores cue
  // identifiers.
  const vtt =
    'WEBVTT\n\n' +
    '01\n' +
    '00:00:01.000 --> 00:00:02.000 align:start\n' +
    'A\n' +
    '\n' +
    '00:00:03.000 --> 00:00:04.000\n' +
    'B\n'

  const legacyNodes = [
    { type: 'header', data: 'WEBVTT' },
    {
      type: 'cue',
      data: {
        start: 1000,
        end: 2000,
        text: 'A',
        settings: 'align:start',
        identifier: '01'
      }
    },
    {
      type: 'metadata',
      data: { kind: 'NOTE', text: 'NOTE ignored by renumbering' }
    },
    {
      type: 'cue',
      data: {
        start: 3000,
        end: 4000,
        text: 'B',
        identifier: 'second'
      }
    }
  ]

  check(
    stringifySync(legacyNodes, { format: 'WebVTT' }),
    'WEBVTT\n\n' +
      '1\n00:00:01.000 --> 00:00:02.000 align:start\nA\n\n' +
      'NOTE ignored by renumbering\n\n' +
      '2\n00:00:03.000 --> 00:00:04.000\nB\n'
  )
  check(
    stringifySync(legacyNodes, { format: 'SRT' }),
    '1\n00:00:01,000 --> 00:00:02,000\nA\n\n' +
      '2\n00:00:03,000 --> 00:00:04,000\nB\n'
  )

  // Old call sites without options are untouched.
  check(parseSync(vtt).length, 3)
  check(
    stringifySync(parseSync(vtt), { format: 'WebVTT' }),
    'WEBVTT\n\n1\n00:00:01.000 --> 00:00:02.000 align:start\nA\n\n' +
      '2\n00:00:03.000 --> 00:00:04.000\nB\n'
  )

  // SRT output of a layout-parsed document still uses the legacy numbering
  // and ignores metadata blocks.
  const layoutNodes = parseSync(
    'WEBVTT\r\n\r\nNOTE 注释\r\n\r\n007\r\n00:00:01.000 --> 00:00:02.000\r\nA\r\n',
    LAYOUT
  )
  check(
    stringifySync(layoutNodes, { format: 'SRT', preserveLayout: true }),
    '1\n00:00:01,000 --> 00:00:02,000\nA\n'
  )
})
