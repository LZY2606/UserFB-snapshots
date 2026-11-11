package chromedp

import (
	"context"
	"errors"
	"fmt"
	"reflect"
	"strconv"
	"strings"
	"time"

	"github.com/chromedp/cdproto/cdp"
	jsonv2 "github.com/chromedp/cdproto/cdp/jsonv2"
	"github.com/chromedp/cdproto/page"
	"github.com/chromedp/cdproto/runtime"
)

// exposePrefix is the start of the name of each binding that ExposeFunc and
// ExposeSourceFunc add. The name of an exposed func cannot start with it.
const exposePrefix = "__chromedp_"

// ExposeFunc is an action that makes the Go func fn available to the page as
// window.<name>. A page script calls it like an async function. The call
// returns a promise that the browser settles with the result of fn, or rejects
// with an Error when fn returns an error. It is like exposeFunction in
// Puppeteer.
//
// The function is there in the current document and in every document that the
// target loads later, for the main frame and for every iframe of the same
// site, so it survives a navigation. An iframe of another site runs in another
// process and is another target, so the function is not there. It stays until
// the target ends. The listener for the calls
// stops when the context of the target ends. A second call of ExposeFunc or
// ExposeSourceFunc with the same name on the same target returns an error. So
// does a name that is empty, that is not valid UTF-8, or that starts with
// "__chromedp_", the prefix of the private names of the package.
//
// A script passes its arguments as JSON, so a value such as a function or a
// DOM node does not work. How the arguments reach fn depends on the type A:
//
//   - When A is a slice type, such as []any or []float64, fn gets all the
//     arguments of the call in the slice. Use it for a call with several
//     arguments, or with none.
//   - For every other A, the call must have one argument, and the action
//     decodes it into A. Use a struct for an object, or a basic type. A call
//     with no argument gives the zero value of A, and a call with more than one
//     argument is an error that rejects the promise.
//
// The action encodes the result of fn as JSON for the page. When R is [Void],
// the promise resolves with undefined.
//
// The action runs fn in a new goroutine for each call, so fn can run several
// times at the same time and must be safe for that. The ctx of fn has the
// values of the context that ExposeFunc received, and it ends when the target
// ends. If fn panics, the promise rejects with an Error whose message is "panic:"
// and the panic value, for example "panic: boom". The message of an error that
// fn returns replaces each byte that is not valid UTF-8 with U+FFFD.
//
// For a func that also needs the document of the call, use [ExposeSourceFunc].
//
// For example, a page that calls window.square(7):
//
//	err := chromedp.Do(ctx,
//		chromedp.ExposeFunc("square", func(ctx context.Context, n float64) (float64, error) {
//			return n * n, nil
//		}),
//		chromedp.Navigate(url),
//	)
func ExposeFunc[A, R any](name string, fn func(ctx context.Context, arg A) (R, error)) Action[Void] {
	return Func(func(ctx context.Context, t *Target) (err error) {
		b, err := addExposedBinding(ctx, t, name, true)
		if err != nil {
			return err
		}
		go func() {
			// The channel closes when the target ends.
			defer b.unsubscribe()
			defer b.stop()
			for raw := range b.calls {
				var ev runtime.EventBindingCalled
				if err := jsonv2.Unmarshal(raw, &ev, DefaultUnmarshalOptions); err != nil || ev.Name != b.binding {
					continue
				}
				go exposedCall(b.lctx, t, b.binding, &ev, fn)
			}
		}()
		return nil
	})
}

// ExposeSourceFunc is like [ExposeFunc], but fn also gets a [CallSource] that
// names the document of the call. The target builds the source from its frame
// records and the default execution world of the browser. A value that the
// page script sends cannot change it, so a page cannot claim the URL or the
// frame of another document.
//
// The URL of the source is the address of the document when the browser
// created it, including the final address after an HTTP redirect. A later
// change of the hash part does not change a source that the target already
// delivered.
//
// The main frame and an iframe of the same site work. An iframe of another
// site runs in another process and is another target, so it does not work. A
// worker does not work either. A call from an execution context that the
// target does not know never reaches fn. The target never treats it as a call
// of the top frame. When that context is gone, its promise stays unsettled.
//
// Each call gets its own context. The context keeps the values of the context
// that registered the func, and it has no deadline of that registration. It
// ends when one of these happens:
//
//   - fn returns.
//   - The browser destroys the execution context of the document, for example
//     on a navigation to another document.
//   - The browser removes the frame from the target.
//   - The target closes.
//
// A change of the hash part of the address does not end the context, and a
// call in another document keeps running. When the document of a call is gone,
// a late result does not reach the new document, even when the new document
// reuses the number of the old promise.
//
// fn runs in a new goroutine for each call, and the target keeps reading its
// events while fn runs. So fn can run a normal chromedp query without a
// deadlock. The order of the results does not have to match the order of the
// calls. Each promise still gets the result of its own call.
//
// The rules for the arguments, the result, [Void], an error and a panic are
// the same as for [ExposeFunc]. The two funcs share one set of names: a name
// that one of them holds cannot be registered again until the registration
// fails or the target ends.
func ExposeSourceFunc[A, R any](name string, fn func(ctx context.Context, source CallSource, arg A) (R, error)) Action[Void] {
	return Func(func(ctx context.Context, t *Target) (err error) {
		// Do not subscribe to the binding events. The loop that reads the
		// messages dispatches them through sourceBindings, after it
		// handled the event that made the execution context.
		b, err := addExposedBinding(ctx, t, name, false)
		if err != nil {
			return err
		}
		tracker := t.sourceTrackerFor(ctx)
		t.addSourceBinding(b.binding, func(ev *runtime.EventBindingCalled) {
			go exposedSourceCall(t, tracker, b.binding, ev, fn)
		})
		return nil
	})
}

// exposedBinding holds the resources that one registration of ExposeFunc or
// ExposeSourceFunc uses.
type exposedBinding struct {
	// binding is the private name that the page script calls.
	binding string
	// lctx lives until the target ends. It has no deadline of the action.
	// It is nil when the binding dispatches through sourceBindings.
	lctx context.Context
	// calls receives the BindingCalled events for the binding. It is nil
	// when the binding dispatches through sourceBindings.
	calls <-chan jsonv2.Value
	// stop ends lctx. It is nil when the binding dispatches through
	// sourceBindings.
	stop context.CancelFunc
	// unsubscribe stops the event subscription. It is nil when the
	// binding dispatches through sourceBindings.
	unsubscribe func()
}

// addExposedBinding claims name on t, installs the browser binding and the
// wrapper script, and, when subscribe is true, subscribes to the calls of the
// binding. ExposeFunc subscribes. ExposeSourceFunc passes false, because the
// loop that reads the messages dispatches its calls through sourceBindings.
// The two funcs share the name handling and the special characters. On error
// it takes every resource back, so a retry with the same name starts clean.
func addExposedBinding(ctx context.Context, t *Target, name string, subscribe bool) (b *exposedBinding, err error) {
	if name == "" {
		return nil, errors.New("exposing a func: the name is empty")
	}
	if strings.HasPrefix(name, exposePrefix) {
		return nil, fmt.Errorf("exposing func %q: the name starts with %q", name, exposePrefix)
	}
	jsName, err := jsonv2.Marshal(name)
	if err != nil {
		return nil, fmt.Errorf("exposing func %q: the name is not valid: %w", name, err)
	}
	t.exposedMu.Lock()
	if _, ok := t.exposed[name]; ok {
		t.exposedMu.Unlock()
		return nil, fmt.Errorf("exposing func %q: the name is in use already", name)
	}
	if t.exposed == nil {
		t.exposed = make(map[string]struct{})
	}
	t.exposed[name] = struct{}{}
	t.exposedSeq++
	// The binding has a number and not the name, so that no two names make
	// the same binding or the same reply func.
	binding := exposePrefix + strconv.Itoa(t.exposedSeq)
	t.exposedMu.Unlock()
	defer func() {
		if err != nil {
			t.exposedMu.Lock()
			delete(t.exposed, name)
			t.exposedMu.Unlock()
		}
	}()

	// The page script calls a private binding. The wrapper script defines
	// window[name] on top of it.
	jsBinding, err := jsonv2.Marshal(binding)
	if err != nil {
		return nil, fmt.Errorf("exposing func %q: %w", name, err)
	}
	script := "(" + exposeFuncJS + ")(" + string(jsName) + "," + string(jsBinding) + ")"

	// Subscribe first, so that no call is lost. ExposeSourceFunc gets its
	// calls from the message loop instead, to keep the order with the
	// events that create and destroy execution contexts.
	var (
		lctx        context.Context
		calls       <-chan jsonv2.Value
		stop        context.CancelFunc
		unsubscribe func()
	)
	if subscribe {
		lctx, stop = context.WithCancel(context.WithoutCancel(ctx))
		calls, unsubscribe = t.Subscribe(runtime.BindingCalled.Method)
	}
	var undo []func(context.Context)
	defer func() {
		if err != nil {
			if unsubscribe != nil {
				unsubscribe()
				stop()
			}
			// Take back the binding and the script, so that a retry starts
			// clean. The context of the call can be the reason for the
			// failure, so the cleanup has its own context.
			cctx, cancel := context.WithTimeout(context.WithoutCancel(ctx), 5*time.Second)
			defer cancel()
			for i := len(undo) - 1; i >= 0; i-- {
				undo[i](cctx)
			}
		}
	}()

	if _, err = cdp.Call(ctx, t, runtime.AddBinding, runtime.AddBindingParams{Name: binding}); err != nil {
		return nil, fmt.Errorf("exposing func %q: %w", name, err)
	}
	undo = append(undo, func(ctx context.Context) {
		_, _ = cdp.Call(ctx, t, runtime.RemoveBinding, runtime.RemoveBindingParams{Name: binding})
	})
	if !t.isWorker {
		// Run the script in each new document. Where the browser knows
		// the flag, it runs the script in the open documents, such as an
		// iframe that is there already.
		added, err := cdp.Call(ctx, t, page.AddScriptToEvaluateOnNewDocument, page.AddScriptToEvaluateOnNewDocumentParams{
			Source:         script,
			RunImmediately: ptr(true),
		})
		if err != nil {
			return nil, fmt.Errorf("exposing func %q: %w", name, err)
		}
		undo = append(undo, func(ctx context.Context) {
			_, _ = cdp.Call(ctx, t, page.RemoveScriptToEvaluateOnNewDocument, page.RemoveScriptToEvaluateOnNewDocumentParams{Identifier: added.Identifier})
		})
	}
	// The main document is open already. The script does nothing the second
	// time that it runs in a document.
	res, err := cdp.Call(ctx, t, runtime.Evaluate, runtime.EvaluateParams{Expression: script})
	if err != nil {
		return nil, fmt.Errorf("exposing func %q: %w", name, err)
	}
	if res.ExceptionDetails != nil {
		return nil, fmt.Errorf("exposing func %q: %w", name, &ExceptionError{res.ExceptionDetails})
	}

	return &exposedBinding{
		binding:     binding,
		lctx:        lctx,
		calls:       calls,
		stop:        stop,
		unsubscribe: unsubscribe,
	}, nil
}

// exposedCall runs one call of an ExposeFunc, and sends the result back to
// the context of the page that made the call.
func exposedCall[A, R any](ctx context.Context, t *Target, binding string, ev *runtime.EventBindingCalled, fn func(context.Context, A) (R, error)) {
	var call struct {
		ID   int64          `json:"id"`
		Args []jsonv2.Value `json:"args"`
	}
	if err := jsonv2.Unmarshal([]byte(ev.Payload), &call, DefaultUnmarshalOptions); err != nil {
		// Without the id the promise cannot settle, and the payload did not come
		// from the wrapper.
		return
	}

	res, err := func() (res R, err error) {
		defer func() {
			if r := recover(); r != nil {
				err = fmt.Errorf("panic: %v", r)
			}
		}()
		arg, err := exposedArg[A](call.Args)
		if err != nil {
			return res, err
		}
		return fn(ctx, arg)
	}()

	ok, value := exposedResult(res, err)
	// The page can be gone, for example after a navigation. Then nobody waits
	// for the result, and the error does not matter.
	sendExposedReply(ctx, t, binding, ev.ExecutionContextID, call.ID, ok, value)
}

// exposedSourceCall runs one call of an ExposeSourceFunc. It resolves the
// source from the records of the target, gives the call its own context, and
// drops the result when the document of the call is gone.
func exposedSourceCall[A, R any](t *Target, tracker *sourceTracker, binding string, ev *runtime.EventBindingCalled, fn func(context.Context, CallSource, A) (R, error)) {
	var call struct {
		ID   int64          `json:"id"`
		Args []jsonv2.Value `json:"args"`
	}
	if err := jsonv2.Unmarshal([]byte(ev.Payload), &call, DefaultUnmarshalOptions); err != nil {
		// Without the id the promise cannot settle, and the payload did not come
		// from the wrapper.
		return
	}

	source, uniqueID, known := t.callSource(ev)
	if !known {
		// Do not run fn for a context that the target does not know, so the
		// target never treats a worker or a stray context as the top frame.
		// The reply needs a live context, so a truly unknown context keeps
		// its promise unsettled. The attempt is harmless when it is gone.
		sendExposedReply(context.WithoutCancel(tracker.root), t, binding, ev.ExecutionContextID, call.ID, false,
			exposedErrorValue(errors.New("the call came from an execution context that the target does not know")))
		return
	}

	callCtx, handle := tracker.start(source.FrameID)
	defer tracker.end(handle)

	res, err := func() (res R, err error) {
		defer func() {
			if r := recover(); r != nil {
				err = fmt.Errorf("panic: %v", r)
			}
		}()
		arg, err := exposedArg[A](call.Args)
		if err != nil {
			return res, err
		}
		return fn(callCtx, source, arg)
	}()

	// The document of the call is gone when its execution context changed.
	// Drop the result, so it cannot reach a new document with a reused
	// context id or a reused promise number.
	if !t.contextLives(source.FrameID, uniqueID) {
		return
	}
	ok, value := exposedResult(res, err)
	sendExposedReply(context.WithoutCancel(callCtx), t, binding, ev.ExecutionContextID, call.ID, ok, value)
}

// exposedResult encodes the outcome of an exposed func for the reply. ok is
// false for an error. When R is Void, a successful call resolves with
// undefined.
func exposedResult[R any](res R, err error) (ok bool, value string) {
	if err != nil {
		return false, exposedErrorValue(err)
	}
	if reflect.TypeFor[R]() == reflect.TypeFor[Void]() {
		return true, "undefined"
	}
	b, merr := jsonv2.Marshal(res, DefaultMarshalOptions)
	if merr != nil {
		return false, exposedErrorValue(fmt.Errorf("encoding the result: %w", merr))
	}
	return true, string(b)
}

// exposedErrorValue encodes the message of an error as a JSON string for the
// reply. It replaces each byte that is not valid UTF-8 with U+FFFD.
func exposedErrorValue(err error) string {
	b, merr := jsonv2.Marshal(strings.ToValidUTF8(err.Error(), "�"), DefaultMarshalOptions)
	if merr != nil {
		return `"the error message is not valid"`
	}
	return string(b)
}

// sendExposedReply settles the promise of a call in the execution context
// that made the call. A missing page or a destroyed context makes the
// evaluation fail, and the caller ignores the error.
func sendExposedReply(ctx context.Context, t *Target, binding string, contextID runtime.ExecutionContextID, id int64, ok bool, value string) {
	reply, err := jsonv2.Marshal(binding + "_reply")
	if err != nil {
		return
	}
	okValue := "true"
	if !ok {
		okValue = "false"
	}
	expr := "window[" + string(reply) + "](" + strconv.FormatInt(id, 10) + "," + okValue + "," + value + ")"
	_, _ = cdp.Call(ctx, t, runtime.Evaluate, runtime.EvaluateParams{
		Expression: expr,
		ContextID:  contextID,
		Silent:     ptr(true),
	})
}

// exposedArg decodes the arguments of a call into the type A.
func exposedArg[A any](args []jsonv2.Value) (A, error) {
	var arg A
	if reflect.TypeFor[A]().Kind() == reflect.Slice {
		b, err := jsonv2.Marshal(args)
		if err != nil {
			return arg, err
		}
		if err := jsonv2.Unmarshal(b, &arg, DefaultUnmarshalOptions); err != nil {
			return arg, fmt.Errorf("decoding the arguments: %w", err)
		}
		return arg, nil
	}
	switch len(args) {
	case 0:
		return arg, nil
	case 1:
		if err := jsonv2.Unmarshal(args[0], &arg, DefaultUnmarshalOptions); err != nil {
			return arg, fmt.Errorf("decoding the argument: %w", err)
		}
		return arg, nil
	}
	return arg, fmt.Errorf("expected one argument, got %d", len(args))
}
