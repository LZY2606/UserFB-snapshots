# Expose the source of a page call

Status: Decided.

A collector receives records from an async callback on a page. One page can
hold two iframes of the same site, and both call the same exposed func. The
func of `ExposeFunc` cannot tell which document made the call. A navigation
can also replace the document while a call still waits for the backend.

## Why

The page sends the arguments of a binding call as JSON. A value that names
the frame or the URL in that JSON is not safe, because the page script
controls it. The browser alone knows the execution context and the frame of
the call. The target already keeps both in its frame records from the
events `Runtime.executionContextCreated` and `Page.frameNavigated`.

A call needs a context that ends with its document. The context of
`ExposeFunc` lives until the target ends, so a navigation does not end it.
That is correct for `ExposeFunc`, and the new func must not change it.

## What was done

- `ExposeSourceFunc[A, R any](name string, fn func(context.Context, CallSource, A) (R, error)) Action[Void]`
  is new. It registers the same browser binding and the same wrapper script
  as `ExposeFunc`, so the rules for arguments, slices, `Void`, errors and
  panics stay the same. The two funcs share one set of names on a target.
- `CallSource` has three exported fields: `FrameID` of type `cdp.FrameID`,
  `ExecutionContextID` of type `runtime.ExecutionContextID`, and `URL` of
  type string. The target fills them from the default execution world and
  the frame records. The target maps only contexts whose aux data marks
  `isDefault` as true, so an isolated world or a worker cannot take the
  frame of a document.
- The URL comes from the frame record at the time of the call. It holds the
  final address after an HTTP redirect. A later change of the hash part
  updates the frame, but it does not rewrite a source that the target
  already delivered.
- Each call gets a context from a tracker on the target. The context keeps
  the values of the registration context, and it gets no deadline from the
  action that registered the func. It ends when fn returns, when the browser
  destroys the execution context, when the browser removes the frame, or
  when the target ends. A hash change does not end it, and a call in another
  document keeps running.
- The target tracks the unique id of the execution context of each call.
  Before it sends a result, it checks that the frame still owns the same
  context. A reused context id fails the check, so a late result cannot
  reach a new document with the same promise number.
- A call from a context that the target does not know never reaches fn. The
  target never treats it as a call of the top frame. Its promise stays
  unsettled when the context is gone.
- fn runs in a new goroutine for each call, and the target keeps reading its
  events while fn runs. So fn can run a normal chromedp query without a
  deadlock.

## What was not done

- Support for an out-of-process iframe and a worker. Each one runs in
  another target, and the frame records of one target do not cover them.
- A way to cancel an exposed func before the target ends. Both
  `ExposeFunc` and `ExposeSourceFunc` stay until the target ends, as before.
