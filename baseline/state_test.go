package baseline

import (
	"bytes"
	"context"
	"github.com/tetratelabs/wazero"
	"testing"
)

func u32(v uint32) (b []byte) {
	for {
		x := byte(v & 127)
		v >>= 7
		if v != 0 {
			x |= 128
		}
		b = append(b, x)
		if v == 0 {
			return
		}
	}
}
func sec(id byte, b []byte) []byte { return append(append([]byte{id}, u32(uint32(len(b)))...), b...) }
func body(b []byte) []byte         { return append(u32(uint32(len(b))), b...) }

// This constructs only existing numerical execution. No origin API or expected origin masks.
func startModule() []byte {
	b := []byte{0, 97, 115, 109, 1, 0, 0, 0}
	b = append(b, sec(1, []byte{2, 96, 1, 127, 1, 127, 96, 0, 1, 127})...)
	b = append(b, sec(3, []byte{4, 0, 0, 1, 1})...)
	b = append(b, sec(5, []byte{1, 1, 1, 2})...)
	b = append(b, sec(6, []byte{1, 127, 1, 65, 0, 11})...)
	exports := []byte{3, 3, 'r', 'u', 'n', 0, 1, 6, 'm', 'u', 't', 'a', 't', 'e', 0, 2, 4, 't', 'r', 'a', 'p', 0, 3}
	b = append(b, sec(7, exports)...)
	code := []byte{4}
	code = append(code, body([]byte{0, 32, 0, 40, 2, 0, 65, 1, 106, 11})...)
	code = append(code, body([]byte{1, 1, 127, 32, 0, 16, 0, 34, 1, 36, 0, 32, 1, 26, 35, 0, 11})...)
	code = append(code, body([]byte{0, 65, 1, 65, 0, 65, 4, 252, 10, 0, 0, 65, 8, 65, 42, 65, 2, 252, 11, 0, 65, 1, 64, 0, 11})...)
	code = append(code, body([]byte{0, 65, 12, 65, 9, 58, 0, 0, 65, 255, 255, 7, 40, 2, 0, 11})...)
	return append(b, sec(10, code)...)
}
func TestOriginStartState(t *testing.T) {
	ctx := context.Background()
	r := wazero.NewRuntimeWithConfig(ctx, wazero.NewRuntimeConfigInterpreter())
	defer r.Close(ctx)
	c, e := r.CompileModule(ctx, startModule())
	if e != nil {
		t.Fatal(e)
	}
	a, e := r.InstantiateModule(ctx, c, wazero.NewModuleConfig().WithName("a"))
	if e != nil {
		t.Fatal(e)
	}
	z, e := r.InstantiateModule(ctx, c, wazero.NewModuleConfig().WithName("z"))
	if e != nil {
		t.Fatal(e)
	}
	if !a.Memory().WriteUint32Le(0, 41) {
		t.Fatal("write")
	}
	v, e := a.ExportedFunction("run").Call(ctx, 0)
	if e != nil || len(v) != 1 || v[0] != 42 {
		t.Fatalf("call=%v %v", v, e)
	}
	if !a.Memory().Write(0, []byte{1, 2, 3, 4, 5}) {
		t.Fatal("write")
	}
	v, e = a.ExportedFunction("mutate").Call(ctx)
	if e != nil || len(v) != 1 || v[0] != 1 {
		t.Fatalf("grow=%v %v", v, e)
	}
	got, ok := a.Memory().Read(0, 5)
	if !ok || !bytes.Equal(got, []byte{1, 1, 2, 3, 4}) {
		t.Fatalf("overlap=%v", got)
	}
	got, ok = a.Memory().Read(8, 2)
	if !ok || !bytes.Equal(got, []byte{42, 42}) {
		t.Fatalf("fill=%v", got)
	}
	if a.Memory().Size() != 131072 || z.Memory().Size() != 65536 {
		t.Fatal("instance/grow isolation")
	}
	v, e = a.ExportedFunction("trap").Call(ctx)
	if e == nil {
		t.Fatal("expected trap", v)
	}
	n, ok := a.Memory().ReadByte(12)
	if !ok || n != 9 {
		t.Fatal("completed prefix write lost")
	}
	a.Memory().WriteUint32Le(0, 6)
	v, e = a.ExportedFunction("run").Call(ctx, 0)
	if e != nil || v[0] != 7 {
		t.Fatalf("retry=%v %v", v, e)
	}
	n32, _ := z.Memory().ReadUint32Le(0)
	if n32 != 0 {
		t.Fatal("other instance changed")
	}
	t.Log("WAZERO_STATE_OK call=42 overlap=11234 fill=42 grow=2 trap-prefix=9 retry=7 isolated=1")
}
