/* eslint-disable @typescript-eslint/no-unused-vars */
import assert from 'node:assert/strict'
import test from 'node:test'
import {JSDOM} from 'jsdom'
import {userEvent} from '../src/index'

const DRAG_EVENT_TYPES = [
  'dragstart',
  'drag',
  'dragenter',
  'dragleave',
  'dragover',
  'drop',
  'dragend',
] as const

const DRAG_EXTRA_EVENT_TYPES = [
  'pointerdown',
  'mousedown',
  'pointerup',
  'mouseup',
  'click',
  'dblclick',
] as const

function createPage(html: string) {
  const dom = new JSDOM(`<!doctype html><html><body>${html}</body></html>`, {
    pretendToBeVisual: true,
  })

  return {
    dom,
    document: dom.window.document,
    window: dom.window,
    user: userEvent.setup({document: dom.window.document}),
  }
}

function idOf(node: EventTarget | null) {
  const element = node as Element | null
  return element?.id || element?.tagName?.toLowerCase() || null
}

function recordEvents(
  document: Document,
  types: readonly string[] = [...DRAG_EVENT_TYPES, ...DRAG_EXTRA_EVENT_TYPES],
) {
  const records: Array<{
    type: string
    target: string | null
    relatedTarget: string | null
    dataTransfer: DataTransfer
    shiftKey: boolean
    ctrlKey: boolean
    altKey: boolean
    metaKey: boolean
    cancelable: boolean
    defaultPrevented: boolean
  }> = []

  const listeners = types.map(type => {
    const listener = (event: Event) => {
      const dragEvent = event as DragEvent & MouseEvent
      records.push({
        type,
        target: idOf(event.target),
        relatedTarget: idOf(dragEvent.relatedTarget),
        dataTransfer: dragEvent.dataTransfer,
        shiftKey: Boolean(dragEvent.shiftKey),
        ctrlKey: Boolean(dragEvent.ctrlKey),
        altKey: Boolean(dragEvent.altKey),
        metaKey: Boolean(dragEvent.metaKey),
        cancelable: event.cancelable,
        defaultPrevented: event.defaultPrevented,
      })
    }
    document.addEventListener(type, listener)
    return [type, listener] as const
  })

  return {
    records,
    dragRecords() {
      return records.filter(record => record.dataTransfer)
    },
    stop() {
      for (const [type, listener] of listeners) {
        document.removeEventListener(type, listener)
      }
    },
  }
}

function flushAsync() {
  return new Promise<void>(resolve => setTimeout(resolve, 0))
}

/**
 * Notice that `assert` failures thrown inside event handlers propagate out of
 * the dispatched event and reject the drag-and-drop promise.
 */
test('drag session contract', async () => {
  // #region DRAG_ROUTE_OK
  {
    const {dom, document, user} = createPage(`
      <div id="source" draggable="true">record</div>
      <div id="a">via a</div>
      <div id="b">via b</div>
      <div id="target">collection area</div>
    `)

    try {
      const source = document.querySelector('#source')!
      const a = document.querySelector('#a')!
      const b = document.querySelector('#b')!
      const target = document.querySelector('#target')!
      const rec = recordEvents(document)

      target.addEventListener('dragover', event => event.preventDefault())

      const result = await user.dragAndDrop(source, target, {
        via: [a, a, b],
        data: {text: 'record-1'},
        dropEffect: 'copy',
      })

      assert.equal(result, 'copy')

      const dragEvents = rec.dragRecords()
      assert.deepEqual(
        dragEvents.map(event => event.type),
        [
          'dragstart',
          'drag',
          'dragenter',
          'dragover',
          'drag',
          'dragover',
          'drag',
          'dragleave',
          'dragenter',
          'dragover',
          'drag',
          'dragleave',
          'dragenter',
          'dragover',
          'drop',
          'dragend',
        ],
      )
      assert.deepEqual(
        dragEvents.map(event => event.target),
        ['source', 'source', 'a', 'a', 'source', 'a', 'source', 'a', 'b', 'b', 'source', 'b', 'target', 'target', 'target', 'source'],
      )

      // "a" is visited twice in a row: it is entered once and left only when
      // the route actually switches target, but still gets one `dragover`
      // per step.
      assert.equal(
        dragEvents.filter(event => event.type === 'dragenter' && event.target === 'a').length,
        1,
      )
      assert.equal(
        dragEvents.filter(event => event.type === 'dragleave' && event.target === 'a').length,
        1,
      )
      assert.equal(
        dragEvents.filter(event => event.type === 'dragover' && event.target === 'a').length,
        2,
      )
      assert.equal(
        dragEvents.filter(event => event.type === 'drag' && event.target === 'source').length,
        4,
      )

      // Angular route switches carry the previous/next target as
      // `relatedTarget`; the source is the related target of the first
      // entered element.
      const enterA = dragEvents.find(event => event.type === 'dragenter' && event.target === 'a')!
      const enterB = dragEvents.find(event => event.type === 'dragenter' && event.target === 'b')!
      const enterTarget = dragEvents.find(event => event.type === 'dragenter' && event.target === 'target')!
      const leaveA = dragEvents.find(event => event.type === 'dragleave' && event.target === 'a')!
      const leaveB = dragEvents.find(event => event.type === 'dragleave' && event.target === 'b')!

      assert.equal(enterA.relatedTarget, 'source')
      assert.equal(enterB.relatedTarget, 'a')
      assert.equal(enterTarget.relatedTarget, 'b')
      assert.equal(leaveA.relatedTarget, 'b')
      assert.equal(leaveB.relatedTarget, 'target')

      // Every event of the gesture shares the same DataTransfer instance.
      const dragStart = dragEvents.find(event => event.type === 'dragstart')!
      assert.ok(dragStart.dataTransfer)
      assert.ok(dragEvents.every(event => event.dataTransfer === dragStart.dataTransfer))
      assert.equal(dragStart.cancelable, true)

      // The events bubble from the actual elements to the document recorder.
      assert.equal(dragStart.target, 'source')
      assert.equal(dragEvents.at(-1)!.target, 'source')

      // Pointer cleanup: the mouse is pressed on the source and released on
      // the final target, without synthesizing a click on a common ancestor.
      const pointerEvents = rec.records.filter(record => !record.dataTransfer)
      assert.deepEqual(
        pointerEvents.map(event => [event.type, event.target]),
        [
          ['pointerdown', 'source'],
          ['mousedown', 'source'],
          ['pointerup', 'target'],
          ['mouseup', 'target'],
        ],
      )
      assert.equal(pointerEvents.filter(event => event.type === 'click').length, 0)
      assert.equal(pointerEvents.filter(event => event.type === 'dblclick').length, 0)

      rec.stop()
      console.log('DRAG_ROUTE_OK')
    } finally {
      dom.window.close()
    }
  }
  // #endregion DRAG_ROUTE_OK

  // #region DRAG_STORE_OK
  {
    const {dom, document, window, user} = createPage(`
      <div id="source" draggable="true">record</div>
      <div id="via">via</div>
      <div id="target">collection area</div>
    `)

    try {
      const source = document.querySelector('#source')!
      const via = document.querySelector('#via')!
      const target = document.querySelector('#target')!

      const f0 = new window.File(['zero'], 'zero.txt', {type: 'text/plain'})
      const f1 = new window.File(['one'], 'one.txt', {type: 'text/plain'})
      const f2 = new window.File(['two'], 'two.txt', {type: 'text/plain'})

      let cachedStartDataTransfer: DataTransfer | undefined
      let cachedProtectedItem: DataTransferItem | undefined
      let cachedDropDataTransfer: DataTransfer | undefined
      let cachedDropItem: DataTransferItem | undefined
      let dropText: string | undefined
      let resolveDropText: (() => void) | undefined
      let rejectedDropTextHeap: string | undefined
      const dropTextPromise = new Promise<void>(resolve => (resolveDropText = resolve))
      let protectedGetAsStringCalled = false
      let protectedEndGetAsStringCalled = false
      let revokedGetAsStringCalled = false

      const seen: any = {}

      target.addEventListener('dragover', event => event.preventDefault())

      source.addEventListener('dragstart', event => {
        const dataTransfer = event.dataTransfer!
        cachedStartDataTransfer = dataTransfer
        const items = Array.from(dataTransfer.items)

        // Initial string map and files share one ordered item store.
        seen.start = {
          types: [...dataTransfer.types],
          plain: dataTransfer.getData('text/plain'),
          html: dataTransfer.getData('text/html'),
          filesLength: dataTransfer.files.length,
          files: [dataTransfer.files[0], dataTransfer.files[1]],
          fileItems: items.filter(item => item.kind === 'file').length,
          itemCount: items.length,
        }

        // The writable phase replaces string MIME types instead of
        // duplicating them and never touches file items that share a MIME
        // type.
        dataTransfer.setData('text/plain', 'changed')
        const replacedItem = dataTransfer.items.add('upper', 'TEXT/PLAIN')
        seen.afterReplace = {
          plain: dataTransfer.getData('text/plain'),
          types: [...dataTransfer.types],
          plainTypes: dataTransfer.types.filter(type => type === 'text/plain').length,
          addedKind: replacedItem.kind,
          addedType: replacedItem.type,
        }

        dataTransfer.items.add('json', 'application/json')
        seen.afterAddJson = {types: [...dataTransfer.types]}

        // `items.remove` of a file is reflected by `files` and `types`.
        dataTransfer.items.remove(0)
        seen.afterRemoveFile = {
          filesLength: dataTransfer.files.length,
          files: [dataTransfer.files[0]],
          filesTypeCount: dataTransfer.types.filter(type => type === 'Files').length,
        }

        dataTransfer.items.add(f2)
        seen.afterAddFile = {
          filesLength: dataTransfer.files.length,
          files: [dataTransfer.files[0], dataTransfer.files[1]],
        }

        dataTransfer.clearData('text/html')
        const finalItems = Array.from(dataTransfer.items)
        seen.afterClear = {
          types: [...dataTransfer.types],
          fileItems: finalItems.filter(item => item.kind === 'file').length,
          itemCount: finalItems.length,
        }
      })

      via.addEventListener('dragover', event => {
        const dataTransfer = event.dataTransfer!
        const items = Array.from(dataTransfer.items)

        // Metadata is visible in the protected phase.
        const stringItem = items.find(
          item => item.kind === 'string' && item.type === 'text/plain',
        )!
        const fileItem = items.find(item => item.kind === 'file')!
        cachedProtectedItem = stringItem

        const typesBefore = [...dataTransfer.types]
        const filesLengthBefore = dataTransfer.files.length

        // Reads are protected and writes are ignored.
        stringItem.getAsString(() => {
          protectedGetAsStringCalled = true
        })
        dataTransfer.setData('text/plain', 'protected-write')
        dataTransfer.items.add('protected', 'application/x-protected')
        dataTransfer.items.remove(0)
        dataTransfer.items.clear()

        const itemsAfter = Array.from(dataTransfer.items)
        seen.protected = {
          plain: dataTransfer.getData('text/plain'),
          json: dataTransfer.getData('application/json'),
          filesLengthBefore,
          filesLengthAfter: dataTransfer.files.length,
          typesBefore,
          typesAfter: [...dataTransfer.types],
          stringKind: stringItem.kind,
          stringType: stringItem.type,
          stringFile: stringItem.getAsFile(),
          fileKind: fileItem.kind,
          fileType: fileItem.type,
          fileFile: fileItem.getAsFile(),
          afterWritePlain: dataTransfer.getData('text/plain'),
          afterWriteFileItems: itemsAfter.filter(item => item.kind === 'file').length,
          afterWriteItemCount: itemsAfter.length,
        }
      })

      target.addEventListener('drop', event => {
        const dataTransfer = event.dataTransfer!
        cachedDropDataTransfer = dataTransfer
        const items = Array.from(dataTransfer.items)

        // The drop phase can read the data...
        const beforeWrite = {
          plain: dataTransfer.getData('text/plain'),
          json: dataTransfer.getData('application/json'),
          filesLength: dataTransfer.files.length,
          types: [...dataTransfer.types],
        }

        // ... but cannot modify it.
        dataTransfer.setData('text/plain', 'drop-write')
        dataTransfer.items.add('drop', 'application/x-drop')
        dataTransfer.items.remove(0)
        dataTransfer.items.clear()

        const itemsAfter = Array.from(dataTransfer.items)
        seen.drop = {
          plain: beforeWrite.plain,
          json: beforeWrite.json,
          filesLength: beforeWrite.filesLength,
          types: beforeWrite.types,
          afterWritePlain: dataTransfer.getData('text/plain'),
          afterWriteTypes: [...dataTransfer.types],
          afterWriteFileItems: itemsAfter.filter(item => item.kind === 'file').length,
          afterWriteItemCount: itemsAfter.length,
        }

        // The asynchronous read captures the value at call time.
        const item = items.find(
          item => item.kind === 'string' && item.type === 'text/plain',
        )!
        cachedDropItem = item
        item.getAsString(text => {
          dropText = text
          resolveDropText?.()
        })
      })

      source.addEventListener('dragend', event => {
        const dataTransfer = event.dataTransfer!
        const item = Array.from(dataTransfer.items).find(
          item => item.kind === 'string',
        )!

        // Protected phase: no data and no callback.
        item.getAsString(() => {
          protectedEndGetAsStringCalled = true
        })

        seen.end = {
          plain: dataTransfer.getData('text/plain'),
          filesLength: dataTransfer.files.length,
          types: [...dataTransfer.types],
        }
      })

      const result = await user.dragAndDrop(source, target, {
        via: [via],
        files: [f0, f1],
        data: {'text/plain': 'initial-plain', 'text/html': '<b>initial</b>'},
        dropEffect: 'copy',
      })
      assert.equal(result, 'copy')

      // A missing drop callback would otherwise hang the test.
      let dropTextTimeout: ReturnType<typeof setTimeout> | undefined
      await Promise.race([
        dropTextPromise,
        new Promise<never>((_, reject) => {
          dropTextTimeout = setTimeout(
            () => reject(new Error('drop getAsString did not resolve')),
            2000,
          )
        }),
      ])
      if (dropTextTimeout !== undefined) {
        clearTimeout(dropTextTimeout)
      }
      assert.equal(dropText, 'upper')

      // Initial data and files.
      assert.deepEqual(seen.start.types, ['Files', 'text/plain', 'text/html'])
      assert.equal(seen.start.plain, 'initial-plain')
      assert.equal(seen.start.html, '<b>initial</b>')
      assert.equal(seen.start.filesLength, 2)
      assert.equal(seen.start.files[0], f0)
      assert.equal(seen.start.files[1], f1)
      assert.equal(seen.start.fileItems, 2)
      assert.equal(seen.start.itemCount, 4)

      // Writable dragstart phase.
      assert.equal(seen.afterReplace.plain, 'upper')
      assert.deepEqual(seen.afterReplace.types, [
        'Files',
        'text/plain',
        'text/html',
      ])
      assert.equal(seen.afterReplace.plainTypes, 1)
      assert.equal(seen.afterReplace.addedKind, 'string')
      assert.equal(seen.afterReplace.addedType, 'text/plain')
      assert.deepEqual(seen.afterAddJson.types, [
        'Files',
        'text/plain',
        'text/html',
        'application/json',
      ])
      assert.equal(seen.afterRemoveFile.filesLength, 1)
      assert.equal(seen.afterRemoveFile.files[0], f1)
      assert.equal(seen.afterRemoveFile.filesTypeCount, 1)
      assert.equal(seen.afterAddFile.filesLength, 2)
      assert.equal(seen.afterAddFile.files[0], f1)
      assert.equal(seen.afterAddFile.files[1], f2)
      assert.deepEqual(seen.afterClear.types, [
        'Files',
        'text/plain',
        'application/json',
      ])
      assert.equal(seen.afterClear.fileItems, 2)
      assert.equal(seen.afterClear.itemCount, 4)

      // Protected drag/enter/over/leave phase.
      assert.equal(seen.protected.plain, '')
      assert.equal(seen.protected.json, '')
      assert.equal(seen.protected.filesLengthBefore, 0)
      assert.equal(seen.protected.filesLengthAfter, 0)
      assert.deepEqual(seen.protected.typesBefore, [
        'Files',
        'text/plain',
        'application/json',
      ])
      assert.deepEqual(seen.protected.typesAfter, seen.protected.typesBefore)
      assert.equal(seen.protected.stringKind, 'string')
      assert.equal(seen.protected.stringType, 'text/plain')
      assert.equal(seen.protected.stringFile, null)
      assert.equal(seen.protected.fileKind, 'file')
      assert.equal(seen.protected.fileType, 'text/plain')
      assert.equal(seen.protected.fileFile, null)
      assert.equal(seen.protected.afterWritePlain, '')
      assert.equal(seen.protected.afterWriteFileItems, 2)
      assert.equal(seen.protected.afterWriteItemCount, 4)

      // Readable but not writable drop phase.
      assert.equal(seen.drop.plain, 'upper')
      assert.equal(seen.drop.json, 'json')
      assert.equal(seen.drop.filesLength, 2)
      assert.deepEqual(seen.drop.types, [
        'Files',
        'text/plain',
        'application/json',
      ])
      assert.equal(seen.drop.afterWritePlain, 'upper')
      assert.deepEqual(seen.drop.afterWriteTypes, seen.drop.types)
      assert.equal(seen.drop.afterWriteFileItems, 2)
      assert.equal(seen.drop.afterWriteItemCount, 4)

      // Protected dragend phase.
      assert.equal(seen.end.plain, '')
      assert.equal(seen.end.filesLength, 0)
      assert.deepEqual(seen.end.types, [
        'Files',
        'text/plain',
        'application/json',
      ])

      // A protected callback is never invoked, even after a microtask flush.
      await flushAsync()
      assert.equal(protectedGetAsStringCalled, false)
      assert.equal(protectedEndGetAsStringCalled, false)

      // Permissions are revoked as soon as the event dispatch returns, even
      // through cached DataTransfer and item handles.
      assert.ok(cachedStartDataTransfer)
      assert.equal(cachedStartDataTransfer!.getData('text/plain'), '')
      assert.equal(cachedStartDataTransfer!.files.length, 0)

      assert.ok(cachedProtectedItem)
      assert.equal(cachedProtectedItem!.getAsFile(), null)

      assert.ok(cachedDropDataTransfer)
      assert.equal(cachedDropDataTransfer!.getData('text/plain'), '')
      assert.equal(cachedDropDataTransfer!.files.length, 0)
      assert.deepEqual(cachedDropDataTransfer!.types, [
        'Files',
        'text/plain',
        'application/json',
      ])
      cachedDropDataTransfer!.setData('text/plain', 'after-dispatch')
      assert.equal(cachedDropDataTransfer!.getData('text/plain'), '')

      assert.ok(cachedDropItem)
      cachedDropItem!.getAsString(text => {
        rejectedDropTextHeap = text
        revokedGetAsStringCalled = true
      })
      await flushAsync()
      assert.equal(revokedGetAsStringCalled, false)
      assert.equal(rejectedDropTextHeap, undefined)

      console.log('DRAG_STORE_OK')
    } finally {
      dom.window.close()
    }
  }
  // #endregion DRAG_STORE_OK

  // #region DRAG_EFFECT_OK
  {
    interface Scenario {
      effectAllowed?: string
      optionDropEffect?: 'copy' | 'move' | 'link' | 'none'
      lastOverEffect?: 'copy' | 'move' | 'link' | 'none'
      preventLastOver?: boolean
      preventEarlierOver?: boolean
      modifyEarlierOver?: boolean
    }

    async function runScenario(scenario: Scenario) {
      const {dom, document, user} = createPage(`
        <div id="source" draggable="true">record</div>
        <div id="via">via</div>
        <div id="target">collection area</div>
      `)

      try {
        const source = document.querySelector('#source')!
        const via = document.querySelector('#via')!
        const target = document.querySelector('#target')!

        let effectAllowedAtDragStart: string | undefined
        let dropEffectBeforeLastOver: string | undefined
        let dropCount = 0
        let dragEndEffect: string | undefined

        source.addEventListener('dragstart', event => {
          const dataTransfer = event.dataTransfer!
          if (scenario.effectAllowed !== undefined) {
            dataTransfer.effectAllowed = scenario.effectAllowed as DataTransfer['effectAllowed']
          }
          effectAllowedAtDragStart = dataTransfer.effectAllowed
        })

        via.addEventListener('dragover', event => {
          if (scenario.preventEarlierOver) {
            event.preventDefault()
          }
          if (scenario.modifyEarlierOver) {
            event.dataTransfer!.dropEffect = 'move'
          }
        })

        target.addEventListener('dragover', event => {
          const dataTransfer = event.dataTransfer!
          dropEffectBeforeLastOver = dataTransfer.dropEffect
          if (scenario.preventLastOver !== false) {
            event.preventDefault()
          }
          if (scenario.lastOverEffect) {
            dataTransfer.dropEffect = scenario.lastOverEffect
          }
        })

        target.addEventListener('drop', () => dropCount++)
        source.addEventListener('dragend', event => {
          dragEndEffect = event.dataTransfer!.dropEffect
        })

        const result = await user.dragAndDrop(source, target, {
          via: [via],
          data: {text: 'effect'},
          dropEffect: scenario.optionDropEffect ?? 'copy',
        })

        return {
          result,
          dropCount,
          dragEndEffect,
          effectAllowedAtDragStart,
          dropEffectBeforeLastOver,
        }
      } finally {
        dom.window.close()
      }
    }

    let scenario = await runScenario({})
    assert.equal(scenario.effectAllowedAtDragStart, 'all')
    assert.equal(scenario.dropEffectBeforeLastOver, 'copy')
    assert.equal(scenario.result, 'copy')
    assert.equal(scenario.dropCount, 1)
    assert.equal(scenario.dragEndEffect, 'copy')

    scenario = await runScenario({
      effectAllowed: 'link',
      optionDropEffect: 'link',
      lastOverEffect: 'link',
    })
    assert.equal(scenario.result, 'link')
    assert.equal(scenario.dropCount, 1)
    assert.equal(scenario.dragEndEffect, 'link')

    scenario = await runScenario({
      effectAllowed: 'copyMove',
      optionDropEffect: 'move',
      lastOverEffect: 'move',
    })
    assert.equal(scenario.result, 'move')
    assert.equal(scenario.dropCount, 1)

    scenario = await runScenario({
      effectAllowed: 'linkMove',
      optionDropEffect: 'link',
      lastOverEffect: 'link',
    })
    assert.equal(scenario.result, 'link')
    assert.equal(scenario.dropCount, 1)

    // Incompatible effect: `copy` is not allowed by `effectAllowed`.
    scenario = await runScenario({
      effectAllowed: 'copy',
      optionDropEffect: 'move',
      lastOverEffect: 'move',
    })
    assert.equal(scenario.result, 'none')
    assert.equal(scenario.dropCount, 0)
    assert.equal(scenario.dragEndEffect, 'none')

    // `none` is never a drop.
    scenario = await runScenario({
      effectAllowed: 'none',
      optionDropEffect: 'copy',
      lastOverEffect: 'copy',
    })
    assert.equal(scenario.result, 'none')
    assert.equal(scenario.dropCount, 0)

    // Only the last `dragover` decides. An earlier accepted over does not
    // survive when the final one is not accepted.
    scenario = await runScenario({
      preventEarlierOver: true,
      preventLastOver: false,
    })
    assert.equal(scenario.result, 'none')
    assert.equal(scenario.dropCount, 0)

    // Earlier over modifications do not leak into the next over.
    scenario = await runScenario({
      modifyEarlierOver: true,
      lastOverEffect: 'copy',
    })
    assert.equal(scenario.dropEffectBeforeLastOver, 'copy')
    assert.equal(scenario.result, 'copy')

    // Invalid effect assignments are ignored and do not break the session.
    scenario = await runScenario({effectAllowed: 'not-an-effect'})
    assert.equal(scenario.effectAllowedAtDragStart, 'all')
    assert.equal(scenario.result, 'copy')

    console.log('DRAG_EFFECT_OK')
  }
  // #endregion DRAG_EFFECT_OK

  // #region DRAG_STATE_OK
  {
    // Rejected `dragstart`: no further drag events, mouse is released, click
    // counting is clean and a later drag works.
    {
      const {dom, document, user} = createPage(`
        <div id="source" draggable="true">record</div>
        <div id="target">collection area</div>
        <button id="button">confirm</button>
      `)

      try {
        const source = document.querySelector('#source')!
        const target = document.querySelector('#target')!
        const button = document.querySelector('#button') as HTMLButtonElement
        const rec = recordEvents(document)
        let preventDragStart = true

        source.addEventListener('dragstart', event => {
          if (preventDragStart) {
            event.preventDefault()
          }
        })

        let clicked = 0
        let dblclicked = 0
        button.addEventListener('click', () => clicked++)
        button.addEventListener('dblclick', () => dblclicked++)

        const result = await user.dragAndDrop(source, target)
        assert.equal(result, 'none')
        assert.deepEqual(rec.dragRecords().map(event => event.type), ['dragstart'])

        await user.click(button)
        assert.equal(clicked, 1)
        assert.equal(dblclicked, 0)

        // After the rejected start a new drag session is possible.
        preventDragStart = false
        target.addEventListener('dragover', event => event.preventDefault())
        const result2 = await user.dragAndDrop(source, target)
        assert.equal(result2, 'copy')

        rec.stop()
      } finally {
        dom.window.close()
      }
    }

    // A pressed mouse button rejects the gesture before any drag event.
    {
      const {dom, document, user} = createPage(`
        <div id="source" draggable="true">record</div>
        <div id="target">collection area</div>
      `)

      try {
        const source = document.querySelector('#source')!
        const target = document.querySelector('#target')!
        const rec = recordEvents(document)

        await user.pointer({keys: '[MouseLeft>]', target: source})
        await assert.rejects(
          user.dragAndDrop(source, target),
          /mouse button is pressed/,
        )
        assert.equal(rec.dragRecords().length, 0)

        await user.pointer('[/MouseLeft]')

        // The pointer state is clean afterwards.
        target.addEventListener('dragover', event => event.preventDefault())
        const result = await user.dragAndDrop(source, target)
        assert.equal(result, 'copy')

        rec.stop()
      } finally {
        dom.window.close()
      }
    }

    // Cleanup after a completed gesture: no stray click/dblclick, normal
    // click and type still work, the DOM is not moved, submitted or deleted.
    {
      const {dom, document, user} = createPage(`
        <div id="source" draggable="true">record</div>
        <form id="form"><div id="target">collection area</div></form>
        <button id="button">confirm</button>
        <input id="input" />
      `)

      try {
        const source = document.querySelector('#source')!
        const target = document.querySelector('#target')!
        const button = document.querySelector('#button') as HTMLButtonElement
        const input = document.querySelector('#input') as HTMLInputElement

        let clicks = 0
        let dblclicks = 0
        let submits = 0
        document.addEventListener('click', () => clicks++)
        document.addEventListener('dblclick', () => dblclicks++)
        document.querySelector('#form')!.addEventListener('submit', event => {
          submits++
          event.preventDefault()
        })

        target.addEventListener('dragover', event => event.preventDefault())

        const result = await user.dragAndDrop(source, target)
        assert.equal(result, 'copy')
        assert.equal(clicks, 0)
        assert.equal(dblclicks, 0)
        assert.equal(submits, 0)
        assert.equal(source.isConnected, true)
        assert.equal(source.parentElement, document.body)
        assert.equal(target.isConnected, true)

        // The next normal click is a fresh single click.
        let buttonClicks = 0
        let buttonDblclicks = 0
        let lastDetail: number | undefined
        button.addEventListener('click', event => {
          buttonClicks++
          lastDetail = event.detail
        })
        button.addEventListener('dblclick', () => buttonDblclicks++)
        await user.click(button)
        assert.equal(buttonClicks, 1)
        assert.equal(buttonDblclicks, 0)
        assert.equal(lastDetail, 1)

        await user.type(input, 'abc')
        assert.equal(input.value, 'abc')
      } finally {
        dom.window.close()
      }
    }

    // Keyboard state is the caller's: pressed keys are used for modifiers and
    // are not released by the gesture.
    {
      const {dom, document, user} = createPage(`
        <div id="source" draggable="true">record</div>
        <div id="target">collection area</div>
        <button id="button">confirm</button>
      `)

      try {
        const source = document.querySelector('#source')!
        const target = document.querySelector('#target')!
        const button = document.querySelector('#button') as HTMLButtonElement

        let dragStartShift = false
        let clickShift = false
        source.addEventListener('dragstart', event => {
          dragStartShift = event.shiftKey
        })
        button.addEventListener('click', event => {
          clickShift = event.shiftKey
        })
        target.addEventListener('dragover', event => event.preventDefault())

        await user.keyboard('[ShiftLeft>]')
        const result = await user.dragAndDrop(source, target)
        assert.equal(result, 'copy')
        assert.equal(dragStartShift, true)

        await user.click(button)
        assert.equal(clickShift, true)

        await user.keyboard('[/ShiftLeft]')
        await flushAsync()
      } finally {
        dom.window.close()
      }
    }

    // A removed target ends the started session with `none` and still
    // finishes it, so the next gesture can run.
    {
      const {dom, document, user} = createPage(`
        <div id="source" draggable="true">record</div>
        <div id="via">via</div>
        <div id="target">collection area</div>
      `)

      try {
        const source = document.querySelector('#source')!
        const via = document.querySelector('#via')!
        let target: Element | null = document.querySelector('#target')!
        const rec = recordEvents(document)
        let removed = false

        via.addEventListener('dragover', event => {
          event.preventDefault()
          if (!removed) {
            removed = true
            target!.remove()
          }
        })

        const result = await user.dragAndDrop(source, target!, {via: [via]})
        assert.equal(result, 'none')
        assert.equal(rec.dragRecords().filter(event => event.type === 'drop').length, 0)
        assert.equal(rec.dragRecords().filter(event => event.type === 'dragend').length, 1)

        // Recreate a target and drag again.
        target = document.createElement('div')
        target.id = 'target'
        document.body.append(target)
        target.addEventListener('dragover', event => event.preventDefault())

        const result2 = await user.dragAndDrop(source, target, {via: [via]})
        assert.equal(result2, 'copy')

        rec.stop()
      } finally {
        dom.window.close()
      }
    }

    // A removed source ends with `none` and does not leave the system busy.
    {
      const {dom, document, user} = createPage(`
        <div id="source" draggable="true">record</div>
        <div id="target">collection area</div>
        <div id="source2" draggable="true">record 2</div>
        <div id="target2">collection area 2</div>
      `)

      try {
        const source = document.querySelector('#source')!
        const target = document.querySelector('#target')!
        const source2 = document.querySelector('#source2')!
        const target2 = document.querySelector('#target2')!
        const rec = recordEvents(document)
        let removed = false

        source.addEventListener('dragstart', () => {
          if (!removed) {
            removed = true
            source.remove()
          }
        })
        target.addEventListener('dragover', event => event.preventDefault())
        target2.addEventListener('dragover', event => event.preventDefault())

        const result = await user.dragAndDrop(source, target)
        assert.equal(result, 'none')
        assert.equal(rec.dragRecords().filter(event => event.type === 'drop').length, 0)

        const result2 = await user.dragAndDrop(source2, target2)
        assert.equal(result2, 'copy')

        rec.stop()
      } finally {
        dom.window.close()
      }
    }

    // A derived setup shares the input system and cannot start a concurrent
    // drag session. The rejection happens before any of its events.
    {
      const {dom, document, user} = createPage(`
        <div id="source" draggable="true">record</div>
        <div id="target">collection area</div>
        <div id="target2">other area</div>
      `)

      try {
        const source = document.querySelector('#source')!
        const target = document.querySelector('#target')!
        const target2 = document.querySelector('#target2')!
        const derived = user.setup()
        const rec = recordEvents(document)

        target.addEventListener('dragover', event => event.preventDefault())
        target2.addEventListener('dragover', event => event.preventDefault())

        const first = user.dragAndDrop(source, target, {
          data: {text: 'first'},
        })

        await assert.rejects(
          derived.dragAndDrop(source, target2, {data: {text: 'second'}}),
          /already active/,
        )

        // The rejected session did not dispatch any event on its target.
        assert.equal(
          rec.records.filter(
            record => record.dataTransfer && record.target === 'target2',
          ).length,
          0,
        )

        const firstResult = await first
        assert.equal(firstResult, 'copy')

        // The guard has been released for the same system.
        const secondResult = await derived.dragAndDrop(source, target2)
        assert.equal(secondResult, 'copy')

        rec.stop()
      } finally {
        dom.window.close()
      }
    }

    // Independent setups keep their systems and drag data separate.
    {
      const one = createPage(`
        <div id="source" draggable="true">record</div>
        <div id="target">collection area</div>
      `)
      const two = createPage(`
        <div id="source" draggable="true">record</div>
        <div id="target">collection area</div>
      `)

      try {
        const source1 = one.document.querySelector('#source')!
        const target1 = one.document.querySelector('#target')!
        const source2 = two.document.querySelector('#source')!
        const target2 = two.document.querySelector('#target')!

        let dataTransfer1: DataTransfer | undefined
        let dataTransfer2: DataTransfer | undefined
        let droppedText1: string | undefined
        let droppedText2: string | undefined

        source1.addEventListener('dragstart', event => {
          dataTransfer1 = event.dataTransfer!
        })
        source2.addEventListener('dragstart', event => {
          dataTransfer2 = event.dataTransfer!
        })

        target1.addEventListener('dragover', event => event.preventDefault())
        target2.addEventListener('dragover', event => event.preventDefault())
        target1.addEventListener('drop', event => {
          droppedText1 = event.dataTransfer!.getData('text/plain')
        })
        target2.addEventListener('drop', event => {
          droppedText2 = event.dataTransfer!.getData('text/plain')
        })

        const [result1, result2] = await Promise.all([
          one.user.dragAndDrop(source1, target1, {data: {text: 'one'}}),
          two.user.dragAndDrop(source2, target2, {data: {text: 'two'}}),
        ])

        assert.equal(result1, 'copy')
        assert.equal(result2, 'copy')
        assert.notEqual(dataTransfer1, dataTransfer2)
        assert.equal(droppedText1, 'one')
        assert.equal(droppedText2, 'two')
      } finally {
        one.dom.window.close()
        two.dom.window.close()
      }
    }

    // The direct API (without `setup`) can start a gesture, too.
    {
      const {dom, document} = createPage(`
        <div id="source" draggable="true">record</div>
        <div id="target">collection area</div>
      `)

      try {
        const source = document.querySelector('#source')!
        const target = document.querySelector('#target')!
        target.addEventListener('dragover', event => event.preventDefault())

        const result = await userEvent.dragAndDrop(source, target, {
          document,
          data: {text: 'direct'},
          dropEffect: 'move',
        })
        assert.equal(result, 'move')
      } finally {
        dom.window.close()
      }
    }

    console.log('DRAG_STATE_OK')
  }
  // #endregion DRAG_STATE_OK
})
