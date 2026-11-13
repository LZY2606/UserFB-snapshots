package ferret

import (
	"context"
	"encoding/json"

	"github.com/MontFerret/ferret/pkg/runtime"
	"github.com/MontFerret/ferret/pkg/runtime/core"
)

// Stream executes the program and hands every logical result to emit as a
// complete JSON value, without materializing the whole result set first.
//
// Like Run, it injects the instance's driver container into the execution
// context. Neither a nil program nor a nil emit starts the query.
func (i *Instance) Stream(ctx context.Context, program *runtime.Program, emit func(json.RawMessage) error, opts ...runtime.Option) error {
	if program == nil {
		return core.Error(core.ErrInvalidArgument, "program")
	}

	if emit == nil {
		return core.Error(core.ErrInvalidArgument, "emit")
	}

	ctx = i.drivers.WithContext(ctx)

	return program.Stream(ctx, emit, opts...)
}
