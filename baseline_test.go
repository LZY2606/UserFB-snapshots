package main

import (
	"encoding/json"
	"fmt"
	. "github.com/aerokube/ggr/config"
	"net"
	"net/http"
	"net/http/httptest"
	"strconv"
	"strings"
	"testing"
)

func TestBaseline(t *testing.T) {
	const rawID = "driver-1"
	observedCh := make(chan string, 1)
	backend := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		if r.URL.Path == "/wd/hub/session" {
			fmt.Fprint(w, `{"value":{"sessionId":"driver-1","capabilities":{"browserName":"browser","webSocketUrl":"ws://127.0.0.1:9/private/bidi?token=sample-routing-marker"}}}`)
		} else {
			observedCh <- r.URL.Path
			fmt.Fprint(w, `{"value":{"ELEMENT":"e1"}}`)
		}
	}))
	defer backend.Close()
	host, ps, _ := net.SplitHostPort(backend.Listener.Addr().String())
	port, _ := strconv.Atoi(ps)
	h := Host{Name: host, Port: port, Count: 1}
	browsers := Browsers{Browsers: []Browser{
		{Name: "browser", DefaultVersion: "1.0", Versions: []Version{
			{Number: "1.0", Regions: []Region{
				{Name: "local", Hosts: Hosts{h}},
			}},
		}},
	}}
	updateQuota(guestUserName, browsers)
	// Invoke the original public HTTP routing and command proxy handlers.
	router := httptest.NewServer(mux())
	defer router.Close()
	resp, err := http.Post(router.URL+paths.Route, "application/json", strings.NewReader(`{"capabilities":{"alwaysMatch":{"browserName":"browser","webSocketUrl":true}}}`))
	if err != nil {
		t.Fatal(err)
	}
	var doc map[string]interface{}
	if err = json.NewDecoder(resp.Body).Decode(&doc); err != nil {
		t.Fatal(err)
	}
	resp.Body.Close()
	if resp.StatusCode != 200 {
		t.Fatalf("create status=%d body=%v", resp.StatusCode, doc)
	}
	v := doc["value"].(map[string]interface{})
	sid := v["sessionId"].(string)
	if sid != h.Sum()+rawID {
		t.Fatalf("session id=%s", sid)
	}
	req, err := http.NewRequest("POST", router.URL+paths.Proxy+sid+"/element", strings.NewReader(`{"using":"css selector","value":"#form"}`))
	if err != nil {
		t.Fatal(err)
	}
	result, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	result.Body.Close()
	observed := <-observedCh
	if observed != "/wd/hub/session/driver-1/element" || result.StatusCode != 200 {
		t.Fatalf("command=%s status=%d", observed, result.StatusCode)
	}
	endpoint := v["capabilities"].(map[string]interface{})["webSocketUrl"].(string)
	fmt.Println("created_session_has_route_prefix=true")
	fmt.Println("wire_command=" + observed)
	fmt.Printf("bidi_advertisement_rewritten=%v\n", !strings.HasPrefix(endpoint, "ws://127.0.0.1:9/"))
}
