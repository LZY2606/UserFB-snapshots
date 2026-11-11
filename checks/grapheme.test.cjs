'use strict'

const {test} = require('node:test')
const assert = require('node:assert/strict')
const {JSDOM} = require('../.build/node_modules/jsdom')
const userEvent = require('../.build/user-event.cjs').default

const GRIN = String.fromCodePoint(0x1f600) // 😀 surrogate pair
const ACUTE = String.fromCharCode(0x0301) // combining acute accent
const ZWJ = String.fromCharCode(0x200d)
const FAMILY = [0x1f468, 0x200d, 0x1f469, 0x200d, 0x1f467]
  .map(cp => String.fromCodePoint(cp))
  .join('') // 👨‍👩‍👧
const FLAG = [0x1f1fa, 0x1f1f8]
  .map(cp => String.fromCodePoint(cp))
  .join('') // 🇺🇸

function createDom(html = '') {
  const dom = new JSDOM(html)
  return dom
}

function setupUser(dom, options = {}) {
  return userEvent.setup({document: dom.window.document, delay: null, ...options})
}

test('input: grapheme Backspace removes a whole surrogate pair', async () => {
  const dom = createDom(`<input value="A${GRIN}B">`)
  const {document} = dom.window
  const input = document.querySelector('input')
  const user = setupUser(dom, {textUnit: 'grapheme'})

  input.focus()
  input.setSelectionRange(3, 3)
  await user.keyboard('{Backspace}')

  assert.equal(input.value, 'AB')
  // Public selection offsets remain UTF-16 offsets.
  assert.equal(input.selectionStart, 1)
  assert.equal(input.selectionEnd, 1)
  dom.window.close()
})

test('input: grapheme ArrowLeft/ArrowRight jump across the surrogate pair', async () => {
  const dom = createDom(`<input value="A${GRIN}B">`)
  const {document} = dom.window
  const input = document.querySelector('input')
  const user = setupUser(dom, {textUnit: 'grapheme'})

  input.focus()
  input.setSelectionRange(4, 4)
  await user.keyboard('{ArrowLeft}')
  assert.equal(input.selectionStart, 3)
  await user.keyboard('{ArrowLeft}')
  assert.equal(input.selectionStart, 1)
  await user.keyboard('{ArrowRight}')
  assert.equal(input.selectionStart, 3)
  await user.keyboard('{ArrowRight}')
  assert.equal(input.selectionStart, 4)
  dom.window.close()
})

test('input: caret inside a surrogate snaps to the grapheme edges', async () => {
  const dom = createDom(`<input value="A${GRIN}B">`)
  const {document} = dom.window
  const input = document.querySelector('input')
  const user = setupUser(dom, {textUnit: 'grapheme'})

  input.focus()
  input.setSelectionRange(2, 2) // between the two UTF-16 units of the emoji
  await user.keyboard('{ArrowLeft}')
  assert.equal(input.selectionStart, 1)

  input.setSelectionRange(2, 2)
  await user.keyboard('{ArrowRight}')
  assert.equal(input.selectionStart, 3)

  input.setSelectionRange(2, 2)
  await user.keyboard('{Backspace}')
  assert.equal(input.value, 'AB')
  assert.equal(input.selectionStart, 1)

  input.value = `A${GRIN}B`
  input.setSelectionRange(2, 2)
  await user.keyboard('{Delete}')
  assert.equal(input.value, 'AB')
  assert.equal(input.selectionStart, 1)
  dom.window.close()
})

test('input: deletion at the start/end changes nothing and fires no input', async () => {
  const dom = createDom('<input value="AB">')
  const {document} = dom.window
  const input = document.querySelector('input')
  const user = setupUser(dom, {textUnit: 'grapheme'})
  let inputEvents = 0
  input.addEventListener('input', () => inputEvents++)

  input.focus()
  input.setSelectionRange(0, 0)
  await user.keyboard('{Backspace}')
  input.setSelectionRange(2, 2)
  await user.keyboard('{Delete}')

  assert.equal(input.value, 'AB')
  assert.equal(inputEvents, 0)
  dom.window.close()
})

test('textarea: combining accent and base letter form one grapheme', async () => {
  const dom = createDom('<textarea></textarea>')
  const {document} = dom.window
  const textarea = document.querySelector('textarea')
  textarea.value = 'e' + ACUTE + 'x'
  const user = setupUser(dom, {textUnit: 'grapheme'})

  textarea.focus()
  textarea.setSelectionRange(3, 3)
  await user.keyboard('{Backspace}')
  assert.equal(textarea.value, 'e' + ACUTE)
  assert.equal(textarea.selectionStart, 2)

  textarea.value = 'e' + ACUTE + 'x'
  textarea.setSelectionRange(3, 3)
  await user.keyboard('{ArrowLeft}')
  assert.equal(textarea.selectionStart, 2)
  await user.keyboard('{ArrowLeft}')
  assert.equal(textarea.selectionStart, 0)

  textarea.value = 'e' + ACUTE + 'x'
  textarea.setSelectionRange(1, 1) // between base and combining mark
  await user.keyboard('{Delete}')
  assert.equal(textarea.value, 'x')
  assert.equal(textarea.selectionStart, 0)
  dom.window.close()
})

test('input: a flag sequence is a single grapheme', async () => {
  const dom = createDom('<input>')
  const {document} = dom.window
  const input = document.querySelector('input')
  const user = setupUser(dom, {textUnit: 'grapheme'})

  input.focus()
  input.value = FLAG + 'z'
  input.setSelectionRange(5, 5)
  await user.keyboard('{Backspace}')
  assert.equal(input.value, FLAG)
  assert.equal(input.selectionStart, 4)

  await user.keyboard('{Backspace}')
  assert.equal(input.value, '')
  assert.equal(input.selectionStart, 0)
  dom.window.close()
})

test('input: a ZWJ family emoji is a single grapheme', async () => {
  const dom = createDom('<input>')
  const {document} = dom.window
  const input = document.querySelector('input')
  const user = setupUser(dom, {textUnit: 'grapheme'})

  input.focus()
  input.value = FAMILY + 'q'
  assert.equal(FAMILY.length, 8)
  input.setSelectionRange(9, 9)
  await user.keyboard('{Backspace}')
  assert.equal(input.value, FAMILY)
  assert.equal(input.selectionStart, 8)

  await user.keyboard('{Backspace}')
  assert.equal(input.value, '')
  assert.equal(input.selectionStart, 0)
  dom.window.close()
})

test('contenteditable: grapheme spanning span and em is deleted as one', async () => {
  const dom = createDom('<div contenteditable="true"></div>')
  const {document} = dom.window
  const host = document.querySelector('[contenteditable]')
  const span = document.createElement('span')
  span.textContent = 'e'
  const em = document.createElement('em')
  em.textContent = ACUTE
  const tail = document.createTextNode('x')
  host.append(span, em, tail)

  // A listener on an untouched element must survive the edit.
  let clicks = 0
  span.addEventListener('click', () => clicks++)

  const spanText = span.firstChild
  const emText = em.firstChild
  const user = setupUser(dom, {textUnit: 'grapheme'})
  host.focus()
  const selection = document.getSelection()
  selection.setBaseAndExtent(emText, 1, emText, 1)

  await user.keyboard('{Backspace}')

  assert.equal(host.textContent, 'x')
  // Nodes are edited in place: identity and listeners are preserved.
  assert.equal(span.firstChild, spanText)
  assert.equal(em.firstChild, emText)
  assert.equal(spanText.isConnected, true)
  assert.equal(emText.isConnected, true)
  assert.equal(spanText.nodeValue, '')
  assert.equal(emText.nodeValue, '')
  span.dispatchEvent(new dom.window.Event('click', {bubbles: true}))
  assert.equal(clicks, 1)
  // The caret returns to the original start of the grapheme.
  assert.equal(selection.focusNode, spanText)
  assert.equal(selection.focusOffset, 0)
  dom.window.close()
})

test('contenteditable: ZWJ sequence split across spans is one grapheme', async () => {
  const dom = createDom('<div contenteditable="true"></div>')
  const {document} = dom.window
  const host = document.querySelector('[contenteditable]')
  const parts = [
    String.fromCodePoint(0x1f468) + ZWJ,
    String.fromCodePoint(0x1f469) + ZWJ,
    String.fromCodePoint(0x1f467),
  ]
  const spans = parts.map(text => {
    const span = document.createElement('span')
    span.textContent = text
    host.append(span)
    return span
  })
  host.append(document.createTextNode('z'))
  const textNodes = spans.map(span => span.firstChild)

  const user = setupUser(dom, {textUnit: 'grapheme'})
  host.focus()
  const selection = document.getSelection()
  selection.setBaseAndExtent(textNodes[2], 2, textNodes[2], 2)

  await user.keyboard('{Backspace}')

  assert.equal(host.textContent, 'z')
  // Every text node keeps its identity; only their data is emptied.
  spans.forEach((span, index) => {
    assert.equal(span.firstChild, textNodes[index])
    assert.equal(span.firstChild.nodeValue, '')
  })
  dom.window.close()
})

test('contenteditable: selection on an element child offset is mapped correctly', async () => {
  const dom = createDom('<div contenteditable="true"></div>')
  const {document} = dom.window
  const host = document.querySelector('[contenteditable]')
  host.append(
    document.createTextNode('A'),
    Object.assign(document.createElement('span'), {textContent: GRIN}),
    document.createTextNode('B'),
  )
  const firstText = host.firstChild
  const user = setupUser(dom, {textUnit: 'grapheme'})
  host.focus()
  const selection = document.getSelection()

  // Element offset 1 is the position between text "A" and the span.
  selection.setBaseAndExtent(host, 1, host, 1)
  await user.keyboard('{ArrowLeft}')
  assert.equal(selection.focusNode, firstText)
  assert.equal(selection.focusOffset, 0)

  // Backspace at an element offset removes the preceding whole grapheme.
  selection.setBaseAndExtent(host, 1, host, 1)
  await user.keyboard('{Backspace}')
  assert.equal(host.textContent, `${GRIN}B`)
  assert.equal(selection.focusNode, firstText)
  assert.equal(selection.focusOffset, 0)
  dom.window.close()
})

test('contenteditable: br, non-editable and nested roots are grapheme boundaries', async () => {
  const dom = createDom(
    '<div contenteditable="true">ab<br>cd' +
      '<span contenteditable="false">btn</span>ef' +
      '<div contenteditable="true">inner</div>gh</div>',
  )
  const {document} = dom.window
  const host = document.querySelector('[contenteditable]')
  const user = setupUser(dom, {textUnit: 'grapheme'})
  host.focus()
  const selection = document.getSelection()
  let inputEvents = 0
  host.addEventListener('input', () => inputEvents++)

  // Caret at the start of the text following the br: it must not cross it.
  const afterBr = host.childNodes[2]
  selection.setBaseAndExtent(afterBr, 0, afterBr, 0)
  await user.keyboard('{ArrowLeft}')
  assert.equal(selection.focusNode, afterBr)
  assert.equal(selection.focusOffset, 0)
  await user.keyboard('{Backspace}')
  assert.equal(inputEvents, 0)

  // Caret right before a contenteditable=false subtree.
  const beforeFalse = host.querySelector('span[contenteditable="false"]')
    .previousSibling
  selection.setBaseAndExtent(
    beforeFalse,
    beforeFalse.length,
    beforeFalse,
    beforeFalse.length,
  )
  await user.keyboard('{Delete}')
  assert.equal(inputEvents, 0)

  // Caret right before a nested independent contenteditable root.
  const nested = host.querySelector('div[contenteditable="true"]')
  const beforeNested = nested.previousSibling
  selection.setBaseAndExtent(
    beforeNested,
    beforeNested.length,
    beforeNested,
    beforeNested.length,
  )
  await user.keyboard('{Delete}')
  assert.equal(inputEvents, 0)
  assert.equal(host.textContent, 'abcdbtnefinnergh')
  dom.window.close()
})

test('contenteditable: ordinary inline tags and empty text nodes join clusters', async () => {
  const dom = createDom('<div contenteditable="true"></div>')
  const {document} = dom.window
  const host = document.querySelector('[contenteditable]')
  const span = document.createElement('span')
  span.textContent = 'e'
  const em = document.createElement('em')
  em.textContent = ACUTE
  host.append(span, document.createTextNode(''), em, document.createTextNode('x'))
  const spanText = span.firstChild
  const emText = em.firstChild

  const user = setupUser(dom, {textUnit: 'grapheme'})
  host.focus()
  const selection = document.getSelection()
  selection.setBaseAndExtent(emText, 1, emText, 1)
  await user.keyboard('{Backspace}')

  assert.equal(host.textContent, 'x')
  assert.equal(spanText.nodeValue, '')
  assert.equal(emText.nodeValue, '')
  dom.window.close()
})

test('contenteditable: caret inside a surrogate snaps to the grapheme edges', async () => {
  const dom = createDom('<div contenteditable="true"></div>')
  const {document} = dom.window
  const host = document.querySelector('[contenteditable]')
  host.textContent = 'A' + GRIN + 'B'
  const textNode = host.firstChild
  const user = setupUser(dom, {textUnit: 'grapheme'})
  host.focus()
  const selection = document.getSelection()

  selection.setBaseAndExtent(textNode, 2, textNode, 2)
  await user.keyboard('{ArrowLeft}')
  assert.equal(selection.focusNode, textNode)
  assert.equal(selection.focusOffset, 1)

  selection.setBaseAndExtent(textNode, 2, textNode, 2)
  await user.keyboard('{ArrowRight}')
  assert.equal(selection.focusNode, textNode)
  assert.equal(selection.focusOffset, 3)

  selection.setBaseAndExtent(textNode, 2, textNode, 2)
  await user.keyboard('{Backspace}')
  assert.equal(host.textContent, 'AB')
  assert.equal(selection.focusNode, textNode)
  assert.equal(selection.focusOffset, 1)
  dom.window.close()
})

test('contenteditable: caret in the middle node of a cross-node grapheme deletes all of it', async () => {
  const dom = createDom('<div contenteditable="true"></div>')
  const {document} = dom.window
  const host = document.querySelector('[contenteditable]')
  const first = document.createElement('span')
  first.textContent = 'e'
  const second = document.createElement('em')
  second.textContent = ACUTE
  const tail = document.createTextNode('x')
  host.append(first, second, tail)
  const firstText = first.firstChild
  const secondText = second.firstChild

  const user = setupUser(dom, {textUnit: 'grapheme'})
  host.focus()
  const selection = document.getSelection()

  // ArrowRight from the start skips the whole joined grapheme and lands at
  // the start of the following text node.
  selection.setBaseAndExtent(host, 0, host, 0)
  await user.keyboard('{ArrowRight}')
  assert.equal(selection.focusNode, tail)
  assert.equal(selection.focusOffset, 0)

  // Backspace from the end node of the joined grapheme removes both nodes'
  // data and returns the caret to the grapheme's original start.
  selection.setBaseAndExtent(secondText, 1, secondText, 1)
  await user.keyboard('{Backspace}')
  assert.equal(host.textContent, 'x')
  assert.equal(firstText.nodeValue, '')
  assert.equal(secondText.nodeValue, '')
  assert.equal(selection.focusNode, firstText)
  assert.equal(selection.focusOffset, 0)
  dom.window.close()
})

test('prevented keydown keeps content and selection untouched', async () => {
  const dom = createDom(`<input value="A${GRIN}B">`)
  const {document} = dom.window
  const input = document.querySelector('input')
  const user = setupUser(dom, {textUnit: 'grapheme'})
  const events = []
  input.addEventListener('keydown', event => {
    events.push('keydown')
    event.preventDefault()
  })
  input.addEventListener('beforeinput', () => events.push('beforeinput'))
  input.addEventListener('input', () => events.push('input'))

  input.focus()
  input.setSelectionRange(3, 3)
  await user.keyboard('{Backspace}')

  assert.equal(input.value, `A${GRIN}B`)
  assert.equal(input.selectionStart, 3)
  assert.deepEqual(events, ['keydown'])
  dom.window.close()
})

test('prevented beforeinput keeps content and selection untouched', async () => {
  const dom = createDom(`<input value="A${GRIN}B">`)
  const {document} = dom.window
  const input = document.querySelector('input')
  const user = setupUser(dom, {textUnit: 'grapheme'})
  input.addEventListener('beforeinput', event => event.preventDefault())

  input.focus()
  input.setSelectionRange(3, 3)
  await user.keyboard('{Backspace}')

  assert.equal(input.value, `A${GRIN}B`)
  assert.equal(input.selectionStart, 3)
  dom.window.close()
})

test('a successful grapheme deletion keeps the standard event chain and a single input', async () => {
  const dom = createDom(`<input value="A${GRIN}B">`)
  const {document} = dom.window
  const input = document.querySelector('input')
  const user = setupUser(dom, {textUnit: 'grapheme'})
  const events = []
  for (const type of ['keydown', 'beforeinput', 'input', 'keyup']) {
    input.addEventListener(type, event =>
      events.push(event.inputType ? `${type}:${event.inputType}` : type),
    )
  }

  input.focus()
  input.setSelectionRange(3, 3)
  await user.keyboard('{Backspace}')

  assert.equal(input.value, 'AB')
  assert.deepEqual(events, [
    'keydown',
    'beforeinput:deleteContentBackward',
    'input:deleteContentBackward',
    'keyup',
  ])
  assert.equal(events.filter(type => type.startsWith('input:')).length, 1)
  dom.window.close()
})

test('a non-collapsed selection is deleted exactly, never widened to a grapheme', async () => {
  const dom = createDom(`<input value="A${GRIN}B">`)
  const {document} = dom.window
  const input = document.querySelector('input')
  const user = setupUser(dom, {textUnit: 'grapheme'})

  input.focus()
  input.setSelectionRange(1, 2) // only the high surrogate of the emoji
  await user.keyboard('{Backspace}')
  assert.equal(input.value, 'A\uDE00B')

  input.value = `A${GRIN}B`
  input.setSelectionRange(1, 3)
  await user.keyboard('{ArrowLeft}')
  assert.equal(input.selectionStart, 1)
  assert.equal(input.selectionEnd, 1)
  dom.window.close()
})

test('default codeUnit mode keeps the historical behavior (regression)', async () => {
  const dom = createDom(`<input value="A${GRIN}B">`)
  const {document} = dom.window
  const input = document.querySelector('input')
  const user = setupUser(dom)
  const events = []
  input.addEventListener('beforeinput', event =>
    events.push(event.inputType),
  )
  input.addEventListener('input', event => events.push(event.inputType))

  input.focus()
  input.setSelectionRange(3, 3)
  await user.keyboard('{Backspace}')

  assert.equal(input.value, 'A\uD83dB')
  assert.equal(input.selectionStart, 2)
  assert.deepEqual(events, [
    'deleteContentBackward',
    'deleteContentBackward',
  ])

  input.setSelectionRange(2, 2)
  await user.keyboard('{ArrowLeft}')
  assert.equal(input.selectionStart, 1)
  dom.window.close()
})

test('textUnit is honored by derived setups and the direct keyboard API', async () => {
  const dom = createDom(`<input value="A${GRIN}B">`)
  const {document} = dom.window
  const input = document.querySelector('input')

  const root = userEvent.setup({
    document,
    delay: null,
    textUnit: 'grapheme',
  })
  const derived = root.setup()
  input.focus()
  input.setSelectionRange(3, 3)
  await derived.keyboard('{Backspace}')
  assert.equal(input.value, 'AB')

  // A derived setup can switch back to the historical code-unit behavior.
  input.value = `A${GRIN}B`
  const codeUnit = root.setup({textUnit: 'codeUnit'})
  input.setSelectionRange(3, 3)
  await codeUnit.keyboard('{Backspace}')
  assert.equal(input.value, 'A\uD83dB')
  assert.equal(input.selectionStart, 2)

  const dom2 = createDom(`<input value="A${GRIN}B">`)
  const input2 = dom2.window.document.querySelector('input')
  input2.focus()
  input2.setSelectionRange(3, 3)
  await userEvent.keyboard('{Backspace}', {
    document: dom2.window.document,
    delay: null,
    textUnit: 'grapheme',
  })
  assert.equal(input2.value, 'AB')
  dom.window.close()
  dom2.window.close()
})

test('number inputs and paste keep their established behavior', async () => {
  const dom = createDom('<input type="number" value="123">')
  const {document} = dom.window
  const input = document.querySelector('input')
  const user = setupUser(dom, {textUnit: 'grapheme'})
  await user.click(input)
  await user.keyboard('{Backspace}')
  assert.equal(input.value, '12')

  const dom2 = createDom('<input value="AB">')
  const input2 = dom2.window.document.querySelector('input')
  const user2 = userEvent.setup({
    document: dom2.window.document,
    delay: null,
    textUnit: 'grapheme',
  })
  input2.focus()
  input2.setSelectionRange(1, 1)
  await user2.paste(GRIN)
  assert.equal(input2.value, `A${GRIN}B`)
  dom.window.close()
  dom2.window.close()
})

test('clear is unaffected by the grapheme option', async () => {
  const dom = createDom(`<input value="A${GRIN}B">`)
  const {document} = dom.window
  const input = document.querySelector('input')
  const user = setupUser(dom, {textUnit: 'grapheme'})
  await user.clear(input)
  assert.equal(input.value, '')
  assert.equal(input.selectionStart, 0)
  dom.window.close()
})
