package regress_test

import (
	h "github.com/gregjones/httpcache"
	"io"
	"net/http"
	"net/http/httptest"
	"sync/atomic"
	"testing"
)

func TestVaryBaseline(t *testing.T) {
	var calls atomic.Int32
	s := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		calls.Add(1)
		w.Header().Set("Vary", "Accept-Language")
		w.Header().Set("Cache-Control", "max-age=3600")
		io.WriteString(w, r.Header.Get("Accept-Language"))
	}))
	defer s.Close()
	tr := h.NewMemoryCacheTransport()
	tr.Transport = s.Client().Transport
	client := tr.Client()
	for _, lang := range []string{"en", "en", "fr", "en"} {
		req, _ := http.NewRequest("GET", s.URL, nil)
		req.Header.Set("Accept-Language", lang)
		res, err := client.Do(req)
		if err != nil {
			t.Fatal(err)
		}
		b, err := io.ReadAll(res.Body)
		res.Body.Close()
		if err != nil || string(b) != lang {
			t.Fatalf("%q %v", b, err)
		}
	}
	if calls.Load() != 3 {
		t.Fatalf("origin calls=%d", calls.Load())
	}
	t.Log("VARY_BASELINE_OK bodies=en,en,fr,en origin_calls=3 single_slot_replacement")
}
