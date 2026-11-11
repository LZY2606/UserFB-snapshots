import test from 'node:test'
import assert from 'node:assert/strict'
import {JSDOM} from 'jsdom'
import userEvent, {type CompositionSession} from '../src/index'

const compositionEventTypes = [
  'compositionstart',
  'compositionupdate',
  'compositionend',
  'beforeinput',
  'input',
] as const

interface EventRecord {
  type: string
  data: string | null
  inputType: string
  isComposing: boolean
  value: string | null
  textContent: string | null
  selection: string
}

function describeNode(node: Node | null) {
  if (!node) {
    return 'null'
  }
  if (node.nodeType === 3) {
    return `T(${node.nodeValue})`
  }
  if (node.nodeType === 1) {
    return `E(${(node as Element).tagName})`
  }
  return `N(${node.nodeType})`
}

function getSelectionDescriptor(element: Element, document: Document) {
  if (
    element instanceof document.defaultView!.HTMLInputElement ||
    element instanceof document.defaultView!.HTMLTextAreaElement
  ) {
    return `${element.selectionStart}:${element.selectionEnd}`
  }

  const selection = document.getSelection()

  return selection
    ? `${describeNode(selection.anchorNode)}@${selection.anchorOffset}-${describeNode(
        selection.focusNode,
      )}@${selection.focusOffset}`
    : 'none'
}

function recordEvents(elements: Element[], document: Document) {
  const records: EventRecord[] = []

  for (const element of elements) {
    for (const type of compositionEventTypes) {
      element.addEventListener(type, event => {
        const inputEvent = event as InputEvent & CompositionEvent
        records.push({
          type: event.type,
          data: typeof inputEvent.data === 'string' ? inputEvent.data : null,
          inputType:
            typeof inputEvent.inputType === 'string'
              ? inputEvent.inputType
              : '',
          isComposing: Boolean(inputEvent.isComposing),
          value:
            element instanceof document.defaultView!.HTMLInputElement ||
            element instanceof document.defaultView!.HTMLTextAreaElement
              ? element.value
              : null,
          textContent: element.textContent,
          selection: getSelectionDescriptor(element, document),
        })
      })
    }
  }

  return records
}

function eventTypes(records: EventRecord[]) {
  return records.map(record => record.type)
}

function assertSequence(records: EventRecord[], expected: string[]) {
  assert.deepEqual(eventTypes(records), expected)
}

function focusElement(element: Element) {
  ;(element as HTMLElement).focus()
  assert.equal(element.ownerDocument.activeElement, element)
}

function appendInput(
  document: Document,
  value = '',
  attributes: Record<string, string> = {},
) {
  const input = document.createElement('input')
  input.value = value
  for (const [name, attributeValue] of Object.entries(attributes)) {
    input.setAttribute(name, attributeValue)
  }
  document.body.append(input)
  return input
}

function appendTextarea(document: Document, value = '') {
  const textarea = document.createElement('textarea')
  textarea.value = value
  document.body.append(textarea)
  return textarea
}

function createEnvironment(html: string) {
  const dom = new JSDOM(`<!doctype html><body>${html}</body>`)
  const document = dom.window.document
  const user = userEvent.setup({document, delay: null})

  return {dom, document, user}
}

async function expectClosed(session: CompositionSession) {
  await assert.rejects(session.update('x'))
  await assert.rejects(session.commit('x'))
  await assert.rejects(session.cancel())
}

test('Composition editing contract', async () => {
  // ---------------------------------------------------------------------------
  // Range, target and replacement contract
  // ---------------------------------------------------------------------------
  {
    const {dom, document, user} = createEnvironment('')
    const input = appendInput(document, 'abcd')
    focusElement(input)
    input.setSelectionRange(1, 3)
    const records = recordEvents([input], document)

    const session = await user.compositionStart(input)

    assertSequence(records, ['compositionstart'])
    assert.equal(records[0]!.data, '')
    assert.equal(input.selectionStart, 1)
    assert.equal(input.selectionEnd, 3)

    assert.equal(await session.update('XY'), true)
    assert.equal(input.value, 'aXYd')
    assert.equal(input.selectionStart, 3)
    assert.equal(input.selectionEnd, 3)

    await session.cancel()
    assert.equal(input.value, 'abcd')
    assert.equal(input.selectionStart, 1)
    assert.equal(input.selectionEnd, 3)

    assertSequence(records, [
      'compositionstart',
      'compositionupdate',
      'beforeinput',
      'input',
      'input',
      'compositionend',
    ])

    dom.window.close()
  }

  {
    // Every update replaces the previously accepted candidate, no matter which
    // candidate length was rejected in between.
    const {dom, document, user} = createEnvironment('')
    const input = appendInput(document, 'ab')
    focusElement(input)
    input.setSelectionRange(1, 1)
    const records = recordEvents([input], document)

    const session = await user.compositionStart(input)

    assert.equal(await session.update('zhong'), true)
    assert.equal(await session.update('中'), true)
    assert.equal(await session.update('中文'), true)
    assert.equal(input.value, 'a中文b')
    assert.equal(input.selectionStart, 3)
    assert.equal(records.at(-1)!.data, '中文')

    assert.equal(await session.commit(), true)
    assert.equal(input.value, 'a中文b')

    dom.window.close()
  }

  {
    const {dom, document, user} = createEnvironment('')
    const textarea = appendTextarea(document, 'hello')
    focusElement(textarea)
    textarea.setSelectionRange(1, 4)
    const records = recordEvents([textarea], document)

    const session = await user.compositionStart(textarea)

    assert.equal(await session.update('é'), true)
    assert.equal(textarea.value, 'héo')
    assert.equal(await session.commit('😀'), true)
    assert.match(textarea.value, /^h😀o$/)
    assertSequence(records, [
      'compositionstart',
      'compositionupdate',
      'beforeinput',
      'input',
      'beforeinput',
      'input',
      'compositionend',
    ])
    assert.equal(records[1]!.inputType, '')
    assert.equal(records[2]!.inputType, 'insertCompositionText')
    assert.equal(records[2]!.data, 'é')
    assert.equal(records[2]!.isComposing, true)
    assert.equal(records[4]!.inputType, 'insertFromComposition')
    assert.equal(records[4]!.isComposing, false)
    assert.equal(records[6]!.data, '😀')

    dom.window.close()
  }

  {
    // Cross-node rich text replacement and cancel restores the original markup
    // without replacing node identities.
    const {dom, document, user} = createEnvironment(
      '<div contenteditable="true"><b>a</b><i>b</i></div>',
    )
    const ce = document.querySelector('div') as HTMLElement
    const bold = ce.firstChild as HTMLElement
    const italic = ce.lastChild as HTMLElement
    const boldText = bold.firstChild as Text
    const italicText = italic.firstChild as Text

    focusElement(ce)
    const range = document.createRange()
    range.setStart(boldText, 1)
    range.setEnd(italicText, 1)
    document.getSelection()!.removeAllRanges()
    document.getSelection()!.addRange(range)

    const records = recordEvents([ce], document)
    const session = await user.compositionStart(ce)

    assert.equal(await session.update('X'), true)
    assert.equal(ce.textContent, 'aX')
    assert.equal(bold.firstChild, boldText)
    assert.equal(italic.firstChild, italicText)
    assert.equal(bold, ce.firstChild)
    assert.equal(italic, ce.lastChild)

    assert.equal(await session.update('中'), true)
    assert.equal(ce.textContent, 'a中')
    assert.equal(bold.firstChild, boldText)
    assert.equal(italic.firstChild, italicText)

    await session.cancel()
    assert.equal(ce.textContent, 'ab')
    assert.equal(ce.innerHTML, '<b>a</b><i>b</i>')
    assert.equal(ce.firstChild, bold)
    assert.equal(ce.lastChild, italic)
    assert.equal(bold.firstChild, boldText)
    assert.equal(italic.firstChild, italicText)
    assert.equal(italicText.nodeValue, 'b')
    assert.equal(document.getSelection()!.anchorNode, boldText)
    assert.equal(document.getSelection()!.anchorOffset, 1)
    assert.equal(document.getSelection()!.focusNode, italicText)
    assert.equal(document.getSelection()!.focusOffset, 1)

    // The cancelled session released its hold and the range can be restarted.
    const restarted = await user.compositionStart(ce)
    assert.equal(await restarted.update('Z'), true)
    await restarted.cancel()

    dom.window.close()
  }

  {
    // Invalid targets and invalid selections are rejected before any event.
    const {dom, document, user} = createEnvironment(`
      <input id="unfocused" value="abc">
      <input id="readonly" value="abc" readonly>
      <input id="disabled" value="abc" disabled>
      <input id="number" type="number" value="1">
      <div id="ce-invalid" contenteditable="true">root</div>
      <div id="ce-false" contenteditable="true">a<span contenteditable="false">b</span>c</div>
      <div id="outside">outside</div>
    `)

    const invalidInputs = [
      document.querySelector('#readonly') as HTMLInputElement,
      document.querySelector('#disabled') as HTMLInputElement,
      document.querySelector('#number') as HTMLInputElement,
    ]

    const unfocused = document.querySelector('#unfocused') as HTMLInputElement
    const records = recordEvents(
      [...invalidInputs, unfocused],
      document,
    )

    await assert.rejects(user.compositionStart(unfocused))
    assert.equal(records.length, 0)

    for (const element of invalidInputs) {
      ;(element as HTMLElement).focus()
      await assert.rejects(user.compositionStart(element))
      assert.equal(records.length, 0)
    }

    const invalidCe = document.querySelector('#ce-invalid') as HTMLElement
    focusElement(invalidCe)
    const outsideText = (document.querySelector('#outside') as HTMLElement)
      .firstChild as Text
    const outsideRange = document.createRange()
    outsideRange.setStart(outsideText, 1)
    outsideRange.setEnd(outsideText, 3)
    document.getSelection()!.removeAllRanges()
    document.getSelection()!.addRange(outsideRange)

    const ceRecords = recordEvents([invalidCe], document)
    await assert.rejects(user.compositionStart(invalidCe))
    assert.equal(ceRecords.length, 0)

    const crossCe = document.querySelector('#ce-false') as HTMLElement
    focusElement(crossCe)
    const beforeFalse = crossCe.firstChild as Text
    const afterFalse = crossCe.lastChild as Text
    const crossingRange = document.createRange()
    crossingRange.setStart(beforeFalse, 1)
    crossingRange.setEnd(afterFalse, 0)
    document.getSelection()!.removeAllRanges()
    document.getSelection()!.addRange(crossingRange)

    await assert.rejects(user.compositionStart(crossCe))
    assert.equal(ceRecords.length, 0)

    dom.window.close()
  }

  console.log('IME_RANGE_OK')

  // ---------------------------------------------------------------------------
  // Event replay contract
  // ---------------------------------------------------------------------------
  {
    const {dom, document, user} = createEnvironment('')
    const input = appendInput(document, 'ab')
    focusElement(input)
    input.setSelectionRange(1, 1)
    const records = recordEvents([input], document)

    const session = await user.compositionStart(input)
    assert.equal(await session.update('zhong'), true)

    assertSequence(records, [
      'compositionstart',
      'compositionupdate',
      'beforeinput',
      'input',
    ])
    assert.equal(records[0]!.data, '')
    for (const index of [1, 2, 3]) {
      assert.equal(records[index]!.data, 'zhong')
    }
    assert.equal(records[2]!.inputType, 'insertCompositionText')
    assert.equal(records[2]!.isComposing, true)
    assert.equal(records[3]!.inputType, 'insertCompositionText')
    assert.equal(records[3]!.isComposing, true)
    assert.equal(input.value, 'azhongb')
    assert.equal(input.selectionStart, 6)
    assert.equal(input.selectionEnd, 6)

    // A listener that only prevents `beforeinput` rejects the update without
    // changing anything and without dispatching `input`.
    const prevent = (event: Event) => event.preventDefault()
    input.addEventListener('beforeinput', prevent)
    records.length = 0

    assert.equal(await session.update('LONG-CANDIDATE'), false)
    assert.equal(input.value, 'azhongb')
    assert.equal(input.selectionStart, 6)
    assertSequence(records, ['compositionupdate', 'beforeinput'])
    assert.equal(records[1]!.inputType, 'insertCompositionText')

    input.removeEventListener('beforeinput', prevent)
    records.length = 0

    // The rejected candidate did not shift the remembered replacement range.
    assert.equal(await session.update('Q'), true)
    assert.equal(input.value, 'aQb')
    assert.equal(input.selectionStart, 2)

    records.length = 0
    assert.equal(await session.commit(), true)
    assertSequence(records, ['beforeinput', 'input', 'compositionend'])
    assert.equal(records[0]!.inputType, 'insertFromComposition')
    assert.equal(records[0]!.data, 'Q')
    assert.equal(records[0]!.isComposing, false)
    assert.equal(records[1]!.inputType, 'insertFromComposition')
    assert.equal(records[1]!.isComposing, false)
    assert.equal(records[2]!.data, 'Q')
    assert.equal(input.value, 'aQb')
    assert.equal(input.selectionStart, 2)
    assert.equal(input.selectionEnd, 2)

    const closedRecords = recordEvents([input], document)
    await expectClosed(session)
    assert.equal(closedRecords.length, 0)

    dom.window.close()
  }

  {
    // A prevented commit leaves the session usable.
    const {dom, document, user} = createEnvironment('')
    const input = appendInput(document, 'xyz')
    focusElement(input)
    input.setSelectionRange(1, 2)
    const records = recordEvents([input], document)

    const session = await user.compositionStart(input)
    assert.equal(await session.update('中'), true)
    assert.equal(await session.commit('文'), true)
    assert.equal(input.value, 'x文z')

    dom.window.close()
  }

  {
    const {dom, document, user} = createEnvironment('')
    const input = appendInput(document, 'xyz')
    focusElement(input)
    input.setSelectionRange(1, 2)
    const records = recordEvents([input], document)
    const session = await user.compositionStart(input)
    assert.equal(await session.update('中'), true)

    const prevent = (event: Event) => event.preventDefault()
    input.addEventListener('beforeinput', prevent)
    records.length = 0

    assert.equal(await session.commit('文'), false)
    assert.equal(input.value, 'x中z')
    assertSequence(records, ['beforeinput'])
    assert.equal(records[0]!.inputType, 'insertFromComposition')

    input.removeEventListener('beforeinput', prevent)
    records.length = 0

    assert.equal(await session.commit('文'), true)
    assert.equal(input.value, 'x文z')
    assertSequence(records, ['beforeinput', 'input', 'compositionend'])

    dom.window.close()
  }

  {
    // Committing the same text still dispatches all events but never appends a
    // second copy.
    const {dom, document, user} = createEnvironment('')
    const input = appendInput(document, 'ab')
    focusElement(input)
    input.setSelectionRange(1, 1)
    const records = recordEvents([input], document)
    const session = await user.compositionStart(input)
    assert.equal(await session.update('S'), true)

    records.length = 0
    assert.equal(await session.commit('S'), true)
    assert.equal(input.value, 'aSb')
    assertSequence(records, ['beforeinput', 'input', 'compositionend'])

    dom.window.close()
  }

  {
    // Empty candidates and multi-code-point candidates.
    const {dom, document, user} = createEnvironment('')
    const input = appendInput(document, 'abc')
    focusElement(input)
    input.setSelectionRange(0, 2)
    const records = recordEvents([input], document)
    const session = await user.compositionStart(input)

    assert.equal(await session.update(''), true)
    assert.equal(input.value, 'c')
    assert.equal(input.selectionStart, 0)
    assert.equal(records[1]!.data, '')
    assert.equal(records[2]!.data, '')
    assert.equal(records[3]!.data, '')

    assert.equal(await session.update('😀😀'), true)
    assert.equal(input.value, '😀😀c')
    assert.equal(input.selectionStart, 4)
    assert.equal(records.at(-1)!.data, '😀😀')

    assert.equal(await session.commit(''), true)
    assert.equal(input.value, 'c')
    assert.equal(input.selectionStart, 0)

    dom.window.close()
  }

  {
    // maxlength is checked against the whole replacement result before any
    // update event and never truncates the candidate.
    const {dom, document, user} = createEnvironment('')
    const input = appendInput(document, 'ab', {maxlength: '4'})
    focusElement(input)
    input.setSelectionRange(0, 0)
    const records = recordEvents([input], document)
    const session = await user.compositionStart(input)

    assert.equal(await session.update('xy'), true)
    assert.equal(input.value, 'xyab')

    records.length = 0
    assert.equal(await session.update('中文字'), false)
    assert.equal(records.length, 0)
    assert.equal(input.value, 'xyab')

    assert.equal(await session.update('z'), true)
    assert.equal(input.value, 'zab')
    assert.equal(input.selectionStart, 1)
    assert.match(records[0]!.data as string, /^z$/)

    dom.window.close()
  }

  {
    const {dom, document, user} = createEnvironment('')
    const textarea = appendTextarea(document, 'xy')
    focusElement(textarea)
    textarea.setSelectionRange(0, 1)
    const records = recordEvents([textarea], document)
    const session = await user.compositionStart(textarea)

    assert.equal(await session.update('字'), true)
    assert.equal(textarea.value, '字y')
    assertSequence(records, [
      'compositionstart',
      'compositionupdate',
      'beforeinput',
      'input',
    ])
    assert.equal(records[2]!.inputType, 'insertCompositionText')
    assert.equal(await session.commit('😀'), true)
    assert.equal(textarea.value, '😀y')

    dom.window.close()
  }

  console.log('IME_EVENTS_OK')

  // ---------------------------------------------------------------------------
  // Session, ownership and concurrency contract
  // ---------------------------------------------------------------------------
  {
    // cancel restores value and selection, dispatches no beforeinput and lets
    // a new session start afterwards.
    const {dom, document, user} = createEnvironment('')
    const input = appendInput(document, 'abc')
    focusElement(input)
    input.setSelectionRange(1, 2)
    const records = recordEvents([input], document)
    const session = await user.compositionStart(input)

    assert.equal(await session.update('Z'), true)
    assert.equal(input.value, 'aZc')

    records.length = 0
    await session.cancel()
    assert.equal(input.value, 'abc')
    assert.equal(input.selectionStart, 1)
    assert.equal(input.selectionEnd, 2)
    assertSequence(records, ['input', 'compositionend'])
    assert.equal(records[0]!.inputType, 'deleteCompositionText')
    assert.equal(records[0]!.isComposing, false)
    assert.equal(records[1]!.data, '')
    assert.ok(!eventTypes(records).includes('beforeinput'))

    const restarted = await user.compositionStart(input)
    assert.equal(await restarted.update('Y'), true)
    await restarted.cancel()

    dom.window.close()
  }

  {
    // Ownership lost in compositionupdate: reject before writing and release.
    const {dom, document, user} = createEnvironment('')
    const input = appendInput(document, 'abc')
    focusElement(input)
    input.setSelectionRange(1, 1)
    const records = recordEvents([input], document)
    const session = await user.compositionStart(input)

    const mutate = () => {
      input.value = 'app'
      input.removeEventListener('compositionupdate', mutate)
    }
    input.addEventListener('compositionupdate', mutate)
    records.length = 0

    await assert.rejects(session.update('x'), /ownership/)
    assert.equal(input.value, 'app')
    assertSequence(records, ['compositionupdate'])

    await assert.rejects(session.update('x'), /closed/)

    // The hold was released, so a fresh session can start.
    input.setSelectionRange(0, 0)
    const restarted = await user.compositionStart(input)
    await restarted.cancel()

    dom.window.close()
  }

  {
    // Ownership lost in beforeinput: the same operation must not overwrite the
    // application's change and no input event may follow.
    const {dom, document, user} = createEnvironment('')
    const input = appendInput(document, 'abc')
    focusElement(input)
    input.setSelectionRange(1, 1)
    const records = recordEvents([input], document)
    const session = await user.compositionStart(input)

    const mutate = () => {
      input.value = 'app2'
      input.removeEventListener('beforeinput', mutate)
    }
    input.addEventListener('beforeinput', mutate)
    records.length = 0

    await assert.rejects(session.update('x'), /ownership/)
    assert.equal(input.value, 'app2')
    assertSequence(records, ['compositionupdate', 'beforeinput'])

    dom.window.close()
  }

  {
    // Ownership lost in a rich text tree mutation.
    const {dom, document, user} = createEnvironment(
      '<div contenteditable="true"><b>a</b><i>b</i></div>',
    )
    const ce = document.querySelector('div') as HTMLElement
    const originalBold = ce.querySelector('b')
    focusElement(ce)
    const range = document.createRange()
    range.setStart(ce.firstChild!.firstChild as Text, 0)
    range.setEnd(ce.firstChild!.firstChild as Text, 1)
    document.getSelection()!.removeAllRanges()
    document.getSelection()!.addRange(range)

    const records = recordEvents([ce], document)
    const session = await user.compositionStart(ce)

    const mutate = () => {
      ce.innerHTML = '<b>z</b>'
      ce.removeEventListener('compositionupdate', mutate)
    }
    ce.addEventListener('compositionupdate', mutate)
    records.length = 0

    await assert.rejects(session.update('x'), /ownership/)
    assert.equal(ce.innerHTML, '<b>z</b>')
    assert.ok(!originalBold!.isConnected)
    assertSequence(records, ['compositionupdate'])

    // The session released the System even though the DOM was replaced.
    const newBoldText = ce.querySelector('b')!.firstChild as Text
    const restartRange = document.createRange()
    restartRange.setStart(newBoldText, 0)
    restartRange.setEnd(newBoldText, 1)
    document.getSelection()!.removeAllRanges()
    document.getSelection()!.addRange(restartRange)
    const restarted = await user.compositionStart(ce)
    await restarted.cancel()

    dom.window.close()
  }

  {
    // Removing the target is treated as lost ownership.
    const {dom, document, user} = createEnvironment('')
    const input = appendInput(document, 'abc')
    focusElement(input)
    input.setSelectionRange(1, 1)
    const records = recordEvents([input], document)
    const session = await user.compositionStart(input)

    const remove = () => {
      input.remove()
      input.removeEventListener('compositionupdate', remove)
    }
    input.addEventListener('compositionupdate', remove)
    records.length = 0

    await assert.rejects(session.update('x'), /ownership/)
    assert.equal(input.isConnected, false)
    assertSequence(records, ['compositionupdate'])

    dom.window.close()
  }

  {
    // Session methods cannot be re-entered from their own event listener.
    const {dom, document, user} = createEnvironment('')
    const input = appendInput(document, 'ab')
    focusElement(input)
    input.setSelectionRange(1, 1)
    const records = recordEvents([input], document)
    const session = await user.compositionStart(input)

    let innerError: Error | undefined
    let reentered = false
    const onUpdate = () => {
      if (reentered) {
        return
      }
      reentered = true
      session.update('inner').catch(error => {
        innerError = error as Error
      })
    }
    input.addEventListener('compositionupdate', onUpdate)

    assert.equal(await session.update('outer'), true)
    assert.equal(input.value, 'aouterb')
    assert.match(String(innerError?.message), /own event listener/)
    assertSequence(records, [
      'compositionstart',
      'compositionupdate',
      'beforeinput',
      'input',
    ])

    await session.commit()

    dom.window.close()
  }

  {
    // Derived setups share the System and cannot start a second session
    // before dispatching any event.
    const {dom, document, user} = createEnvironment('')
    const input = appendInput(document, 'abc')
    focusElement(input)
    input.setSelectionRange(1, 1)
    const derived = user.setup()
    const records = recordEvents([input], document)

    const session = await user.compositionStart(input)
    const countAfterStart = records.length

    await assert.rejects(derived.compositionStart(input), /already active/)
    assert.equal(records.length, countAfterStart)
    await assert.rejects(user.compositionStart(input), /already active/)
    assert.equal(records.length, countAfterStart)

    await session.cancel()

    dom.window.close()
  }

  {
    // Editing APIs are blocked while the same System holds a session, but copy
    // continues to work.
    const {dom, document, user} = createEnvironment('')
    const input = appendInput(document, 'abc')
    focusElement(input)
    input.setSelectionRange(1, 1)
    const records = recordEvents([input], document)
    const session = await user.compositionStart(input)

    await assert.rejects(user.keyboard('x'), /composition session/)
    await assert.rejects(user.type(input, 'x'), /composition session/)
    await assert.rejects(user.paste('x'), /composition session/)
    await assert.rejects(user.cut(), /composition session/)
    await assert.rejects(user.clear(input), /composition session/)

    await user.copy()

    assert.equal(input.value, 'abc')
    assertSequence(records, ['compositionstart'])

    await session.cancel()

    dom.window.close()
  }

  {
    // Independent setups are unaffected by another System's session.
    const {dom, document, user} = createEnvironment('')
    const first = appendInput(document, 'abc')
    const second = appendInput(document, 'xyz')
    const other = userEvent.setup({document, delay: null})

    focusElement(first)
    const firstSession = await user.compositionStart(first)

    focusElement(second)
    second.setSelectionRange(0, 3)
    const secondSession = await other.compositionStart(second)
    assert.equal(await secondSession.update('OK'), true)
    assert.equal(await secondSession.commit(), true)
    assert.equal(second.value, 'OK')

    // Moving focus away means the first session has lost ownership.
    await assert.rejects(firstSession.update('x'), /ownership/)

    dom.window.close()
  }

  {
    // Closing a session does not release keys held by the keyboard device.
    const {dom, document, user} = createEnvironment('')
    const input = appendInput(document, 'ab')
    focusElement(input)
    input.setSelectionRange(1, 1)
    const keyups: string[] = []
    input.addEventListener('keyup', event => keyups.push(event.key))

    await user.keyboard('{Shift>}')
    const session = await user.compositionStart(input)
    assert.equal(await session.update('中'), true)
    assert.equal(await session.commit(), true)
    assert.deepEqual(keyups, [])

    await user.keyboard('{/Shift}')
    assert.deepEqual(keyups, ['Shift'])

    dom.window.close()
  }

  {
    // Closed handles reject without dispatching more events, and ordinary
    // type/clear afterwards do not see a stale candidate or UI cache.
    const {dom, document, user} = createEnvironment('')
    const input = appendInput(document, 'abc')
    focusElement(input)
    input.setSelectionRange(1, 1)
    const records = recordEvents([input], document)
    const session = await user.compositionStart(input)
    assert.equal(await session.update('中'), true)
    assert.equal(await session.commit(), true)

    records.length = 0
    await expectClosed(session)
    assert.equal(records.length, 0)

    await user.clear(input)
    assert.equal(input.value, '')
    await user.type(input, 'ok')
    assert.equal(input.value, 'ok')

    const cancelledInput = appendInput(document, 'abcd')
    focusElement(cancelledInput)
    cancelledInput.setSelectionRange(1, 3)
    const cancelledSession = await user.compositionStart(cancelledInput)
    assert.equal(await cancelledSession.update('Z'), true)
    await cancelledSession.cancel()
    assert.equal(cancelledInput.value, 'abcd')
    await user.clear(cancelledInput)
    assert.equal(cancelledInput.value, '')

    dom.window.close()
  }

  {
    // Derived setups and the direct API can create sessions as well.
    const {dom, document, user} = createEnvironment('')
    const input = appendInput(document, 'ab')
    const derivedInput = appendInput(document, 'cd')
    focusElement(input)
    input.setSelectionRange(1, 1)

    const derived = user.setup()
    const derivedSession = await derived.compositionStart(input)
    assert.equal(await derivedSession.update('D'), true)
    assert.equal(await derivedSession.commit(), true)
    assert.equal(input.value, 'aDb')

    focusElement(derivedInput)
    derivedInput.setSelectionRange(1, 1)
    const directSession = await userEvent.compositionStart(derivedInput)
    assert.equal(await directSession.update('C'), true)
    assert.equal(await directSession.commit(), true)
    assert.equal(derivedInput.value, 'cCd')

    dom.window.close()
  }

  console.log('IME_SESSION_OK')
})
