package baseline

import (
	"context"
	"github.com/tetratelabs/wazero"
	"testing"
)

func TestOriginStart(t *testing.T) {
	ctx := context.Background()
	r := wazero.NewRuntimeWithConfig(ctx, wazero.NewRuntimeConfigInterpreter())
	defer r.Close(ctx)
	wasm := []byte{0, 97, 115, 109, 1, 0, 0, 0, 1, 6, 1, 96, 1, 127, 1, 127, 3, 2, 1, 0, 5, 3, 1, 0, 1, 7, 7, 1, 3, 114, 117, 110, 0, 0, 10, 12, 1, 10, 0, 32, 0, 40, 2, 0, 65, 1, 106, 11}
	m, err := r.Instantiate(ctx, wasm)
	if err != nil {
		t.Fatal(err)
	}
	f := m.ExportedFunction("run")
	if !m.Memory().WriteUint32Le(0, 41) {
		t.Fatal("write")
	}
	v, err := f.Call(ctx, 0)
	if err != nil || len(v) != 1 || v[0] != 42 {
		t.Fatalf("load=%v err=%v", v, err)
	}
	if _, err = f.Call(ctx, 65535); err == nil {
		t.Fatal("expected bounds trap")
	}
	if !m.Memory().WriteUint32Le(0, 6) {
		t.Fatal("rewrite")
	}
	v, err = f.Call(ctx, 0)
	if err != nil || len(v) != 1 || v[0] != 7 {
		t.Fatalf("retry=%v err=%v", v, err)
	}
	t.Log("WAZERO_BASELINE_OK load=42 bounds=trap retry=7")
}
