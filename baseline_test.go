package xmlquery

import (
	"io"
	"net/http"
	"net/http/httptest"
	"reflect"
	"strings"
	"testing"
)

func TestRecordBaseline(t *testing.T) {
	const body = `<root xmlns:p="urn:records"><p:item id="outer"><p:item id="inner">IN</p:item><v>TAIL</v></p:item><p:item id="last">LAST</p:item></root>`
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/xml")
		io.WriteString(w, body)
	}))
	defer srv.Close()
	doc, err := LoadURL(srv.URL)
	if err != nil {
		t.Fatal(err)
	}
	all := Find(doc, "//*[local-name()='item']")
	if len(all) != 3 {
		t.Fatal(len(all))
	}
	sp, err := CreateStreamParser(strings.NewReader(body), "//*[local-name()='item']")
	if err != nil {
		t.Fatal(err)
	}
	var ids []string
	for {
		n, e := sp.Read()
		if e == io.EOF {
			break
		}
		if e != nil {
			t.Fatal(e)
		}
		ids = append(ids, n.SelectAttr("id"))
	}
	if !reflect.DeepEqual(ids, []string{"outer", "last"}) {
		t.Fatal(ids)
	}
	t.Log("XML_BASELINE_OK full=3 streamed=2")
}
