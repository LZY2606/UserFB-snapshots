package main

import (
	"context"
	"encoding/json"
	"fmt"
	ferret "github.com/MontFerret/ferret"
	httpdriver "github.com/MontFerret/ferret/pkg/drivers/http"
	"github.com/MontFerret/ferret/pkg/runtime"
	"net/http"
	"net/http/httptest"
	"sync/atomic"
)

func main() {
	var hits atomic.Int32
	s := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		hits.Add(1)
		fmt.Fprintf(w, "<html><body><h1>%s</h1></body></html>", r.URL.Path[1:])
	}))
	defer s.Close()
	f := ferret.New()
	if err := f.Drivers().Register(httpdriver.NewDriver()); err != nil {
		panic(err)
	}
	p, err := f.Compile(`FOR id IN [1,2,3] LET page = DOCUMENT(CONCAT(@base, "/", TO_STRING(id))) RETURN INNER_TEXT(page, "h1")`)
	if err != nil {
		panic(err)
	}
	out, err := f.Run(context.Background(), p, runtime.WithParam("base", s.URL))
	if err != nil {
		panic(err)
	}
	var got []string
	if err = json.Unmarshal(out, &got); err != nil {
		panic(err)
	}
	if len(got) != 3 || got[0] != "1" || got[1] != "2" || got[2] != "3" || hits.Load() != 3 {
		panic(fmt.Sprintf("unexpected %s hits=%d", out, hits.Load()))
	}
	fmt.Printf("RUN_RESULT=%s REQUESTS_AT_RUN_RETURN=%d\n", out, hits.Load())
	fmt.Println("BASELINE_OK real FQL DOCUMENT HTTP extraction; Stream is not implemented here")
}
