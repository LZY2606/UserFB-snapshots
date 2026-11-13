package expressions

import (
	"context"

	"github.com/MontFerret/ferret/pkg/runtime/core"
	"github.com/MontFerret/ferret/pkg/runtime/values"
)

// StreamFunc receives values as they are produced by an expression.
// Returning an error stops the execution.
//
// It is an alias of the underlying function type so that implementations
// satisfy interfaces declared with the unnamed function signature.
type StreamFunc = func(value core.Value) error

// ExecStream evaluates the body in the same order as Exec, but hands the
// result of the final expression over incrementally instead of materializing
// the whole result first.
func (b *BodyExpression) ExecStream(ctx context.Context, scope *core.Scope, emit StreamFunc) error {
	select {
	case <-ctx.Done():
		return core.ErrTerminated
	default:
	}

	for _, exp := range b.statements {
		if _, err := exp.Exec(ctx, scope); err != nil {
			return err
		}
	}

	if b.expression == nil {
		return nil
	}

	if streamable, ok := b.expression.(interface {
		ExecStream(ctx context.Context, scope *core.Scope, emit StreamFunc) error
	}); ok {
		return streamable.ExecStream(ctx, scope, emit)
	}

	out, err := b.expression.Exec(ctx, scope)

	if err != nil {
		return err
	}

	return emit(out)
}

// ExecStream evaluates a top-level RETURN expression once and delivers the
// whole value. A nested FOR expression is a value in this position and must
// not be streamed item by item.
func (e *ReturnExpression) ExecStream(ctx context.Context, scope *core.Scope, emit StreamFunc) error {
	out, err := e.Exec(ctx, scope)

	if err != nil {
		return err
	}

	return emit(out)
}

// ExecStream evaluates a FOR expression without collecting its result.
//
// It follows the same expansion rules as Exec:
//   - clauses wrap the data source and are pulled one scope at a time;
//   - a nested FOR expression (a loop chain) is streamed recursively and its
//     values are pushed through the outer result rules (spread, distinct);
//   - RETURN NONE keeps executing the loop body but delivers nothing.
func (e *ForExpression) ExecStream(ctx context.Context, scope *core.Scope, emit StreamFunc) error {
	select {
	case <-ctx.Done():
		return core.ErrTerminated
	default:
	}

	iterator, err := e.dataSource.Iterate(ctx, scope)

	if err != nil {
		return err
	}

	result := newForStream(ctx, e.distinct, e.spread, e.passThrough, emit)

	for {
		select {
		case <-ctx.Done():
			return core.ErrTerminated
		default:
		}

		nextScope, err := iterator.Next(ctx, scope)

		if err != nil {
			if core.IsNoMoreData(err) {
				break
			}

			return core.SourceError(e.src, err)
		}

		// A loop chain is expanded lazily: values of the inner loop are
		// handed to the outer result rules as soon as they are produced.
		if nested, ok := e.predicate.(*ForExpression); ok && e.spread {
			if err := nested.ExecStream(ctx, nextScope, result.push); err != nil {
				return err
			}

			continue
		}

		out, err := e.predicate.Exec(ctx, nextScope)

		if err != nil {
			return err
		}

		if err := result.push(out); err != nil {
			return err
		}
	}

	return nil
}

// forStream applies the same distinct/spread/pass-through rules as ForResult,
// but emits values instead of storing them.
type forStream struct {
	ctx         context.Context
	distinct    bool
	spread      bool
	passThrough bool
	hashes      map[uint64]bool
	emit        StreamFunc
}

func newForStream(ctx context.Context, distinct, spread, passThrough bool, emit StreamFunc) *forStream {
	stream := &forStream{
		ctx:         ctx,
		distinct:    distinct,
		spread:      spread,
		passThrough: passThrough,
		emit:        emit,
	}

	if distinct {
		stream.hashes = make(map[uint64]bool)
	}

	return stream
}

func (s *forStream) push(value core.Value) error {
	select {
	case <-s.ctx.Done():
		return core.ErrTerminated
	default:
	}

	if s.passThrough {
		return nil
	}

	if s.distinct {
		hash := value.Hash()

		if s.hashes[hash] {
			return nil
		}

		s.hashes[hash] = true
	}

	if !s.spread {
		return s.emit(value)
	}

	elements, ok := value.(*values.Array)

	if !ok {
		return s.emit(value)
	}

	var emitErr error

	elements.ForEach(func(item core.Value, _ int) bool {
		emitErr = s.push(item)

		return emitErr == nil
	})

	return emitErr
}
