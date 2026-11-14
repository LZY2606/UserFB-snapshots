# Changelog

## Unreleased

### Added

- New `Promise` method `client.dragAndDrop(options)` that performs a native
  drag and drop using drag interception. `options` takes `source`, `capture`
  and `target` points (`{x, y}` in main frame viewport CSS pixels, finite
  numbers, `capture` different from `source`), an optional `via` array of
  intermediate points (default `[]`), an optional positive integer
  `captureTimeoutMs` (default `1000`) and an optional non-empty `sessionId`
  (default: root session). Options are fully validated before any command is
  sent, and only one call per client and session may be in flight at a time.

  The call sends, each after the successful reply of the previous one:
  `Input.setInterceptDrags({enabled: true})`, `mouseMoved` to `source`
  (`buttons: 0`), `mousePressed` at `source` (`button: 'left'`, `buttons: 1`,
  `clickCount: 1`) and `mouseMoved` to `capture` (`buttons: 1`). It then waits
  for both the capture move reply and the first in-scope
  `Input.dragIntercepted` event (matched on the top-level `sessionId`; the
  root session does not collect child session events), storing the
  browser-provided `DragData` as an independent snapshot. The timeout starts
  when the capture move is sent and rejects with an `Error` whose `code` is
  `ERR_CDP_DRAG_CAPTURE_TIMEOUT`. The route `[...via, target]` is walked with
  `dragEnter` on the first point, `dragOver` on every point and `drop` on
  `target`, each carrying an independent copy of the captured data. The
  promise fulfills with a copy of the captured `DragData`.

  Confirmation state tracked by each call: interception enabled, mouse
  pressed, capture move sent and last confirmed mouse position. The normal
  path ends with `mouseReleased` (`button: 'left'`, `buttons: 0`,
  `clickCount: 1`) at the last confirmed position followed by
  `Input.setInterceptDrags({enabled: false})`. After a business failure the
  compensation runs in order — `Input.cancelDragging` if the capture move had
  been sent, `mouseReleased` if the press was confirmed, interception disabled
  if enabling it was confirmed — each item attempted at most once and never
  blocking the following ones. The first business error keeps its identity and
  collects the cleanup errors in its `cleanupErrors` array; with no business
  error the first cleanup error rejects the promise and carries the remaining
  ones in `cleanupErrors`. Closing the underlying connection (including
  `client.close()`) ends the capture wait and skips compensation commands that
  cannot be executed. Every call removes its own timers and listeners and
  holds the session exclusivity until its cleanup is complete.

  `client.send` and the generated protocol API are unchanged.
