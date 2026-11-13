package runtime

import (
	"context"
	"encoding/json"
	"fmt"

	pkgerrors "github.com/pkg/errors"

	"github.com/MontFerret/ferret/pkg/runtime/core"
	"github.com/MontFerret/ferret/pkg/runtime/logging"
)

// ErrStreamCallback is returned when an emit callback fails or panics.
// The original failure is available via errors.Is / errors.As.
var ErrStreamCallback = pkgerrors.New("stream callback failed")

type streamCallbackError struct {
	cause error
}

func (e *streamCallbackError) Error() string {
	return fmt.Sprintf("%s: %s", ErrStreamCallback.Error(), e.cause.Error())
}

func (e *streamCallbackError) Unwrap() error {
	return e.cause
}

// Is keeps ErrStreamCallback reachable through errors.Is while Unwrap keeps
// the original callback failure reachable as well.
func (e *streamCallbackError) Is(target error) bool {
	return target == ErrStreamCallback
}

// streamExpression is implemented by the compiled top level expressions that
// can push their results one by one instead of collecting them.
type streamExpression interface {
	ExecStream(ctx context.Context, scope *core.Scope, emit func(core.Value) error) error
}

// Stream executes the program and delivers each logical result to emit as a
// complete JSON value, in the same order Run would produce them.
//
// A top level FOR expression streams its items: collecting the emitted values
// into an array yields exactly the JSON output of Run. A top level RETURN
// expression delivers its single value as a whole. Values produced inside
// subqueries, LET bindings, objects and arrays stay regular value computations
// and are never delivered separately.
//
// emit must not be nil and is called synchronously: the evaluation does not
// advance until emit returns. An error returned by emit stops the execution
// and is returned unwrapped, so errors.Is keeps matching the original error.
// Already delivered values are not rolled back.
func (p *Program) Stream(ctx context.Context, emit func(json.RawMessage) error, setters ...Option) (err error) {
	if emit == nil {
		return core.Error(core.ErrInvalidArgument, "emit")
	}

	opts := NewOptions(setters)

	if err = p.validateParams(opts); err != nil {
		return err
	}

	ctx = opts.WithContext(ctx)
	logger := logging.FromContext(ctx)

	defer func() {
		if r := recover(); r != nil {
			switch x := r.(type) {
			case string:
				err = pkgerrors.New(x)
			case error:
				err = pkgerrors.WithStack(x)
			default:
				err = pkgerrors.New("unknown panic")
			}

			logger.Error().
				Timestamp().
				Err(err).
				Str("stack", fmt.Sprintf("%+v", err)).
				Msg("panic")
		}
	}()

	scope, closeFn := core.NewRootScope()

	defer func() {
		if closeErr := closeFn(); closeErr != nil {
			logger.Error().
				Timestamp().
				Err(closeErr).
				Msg("closing root scope")
		}
	}()

	stream := func(value core.Value) error {
		raw, marshalErr := value.MarshalJSON()

		if marshalErr != nil {
			return marshalErr
		}

		return safeEmit(emit, json.RawMessage(raw))
	}

	if streamable, ok := p.body.(streamExpression); ok {
		return streamable.ExecStream(ctx, scope, stream)
	}

	// Fallback for program bodies that do not support streaming: evaluate
	// once and deliver the single value, preserving Run semantics.
	out, err := p.body.Exec(ctx, scope)

	if err != nil {
		return err
	}

	return stream(out)
}

// safeEmit isolates panics raised by the receiver so they never escape
// Stream. The wrapped cause stays reachable through errors.Is.
func safeEmit(emit func(json.RawMessage) error, raw json.RawMessage) (err error) {
	defer func() {
		if r := recover(); r != nil {
			switch x := r.(type) {
			case string:
				err = &streamCallbackError{cause: pkgerrors.New(x)}
			case error:
				err = &streamCallbackError{cause: x}
			default:
				err = &streamCallbackError{cause: pkgerrors.New("unknown panic")}
			}
		}
	}()

	return emit(raw)
}
