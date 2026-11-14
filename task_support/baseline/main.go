package main

import (
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"

	"github.com/gorilla/websocket"
	"github.com/tebeka/selenium"
)

func main() {
	var address string
	var requested bool
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		if r.URL.Path == "/bidi" {
			upgrader := websocket.Upgrader{}
			conn, err := upgrader.Upgrade(w, r, nil)
			if err != nil {
				return
			}
			defer conn.Close()
			var request struct {
				ID     int    `json:"id"`
				Method string `json:"method"`
			}
			if err := conn.ReadJSON(&request); err != nil {
				return
			}
			if request.Method != "session.status" {
				panic("unexpected wire fixture command")
			}
			must(conn.WriteJSON(map[string]any{"id": request.ID, "type": "success", "result": map[string]any{"ready": true, "message": "synthetic peer"}}))
			return
		}
		enc := json.NewEncoder(w)
		switch {
		case r.Method == "POST" && r.URL.Path == "/session":
			var request map[string]any
			must(json.NewDecoder(r.Body).Decode(&request))
			caps := request["capabilities"].(map[string]any)["alwaysMatch"].(map[string]any)
			requested, _ = caps["webSocketUrl"].(bool)
			fmt.Printf("CREATE alwaysMatch.webSocketUrl=%v\n", requested)
			must(enc.Encode(map[string]any{"value": map[string]any{"sessionId": "session-1", "capabilities": map[string]any{"browserName": "firefox", "browserVersion": "120.0", "webSocketUrl": address + "/bidi"}}}))
		case r.Method == "POST" && r.URL.Path == "/session/session-1/execute/sync":
			must(enc.Encode(map[string]any{"value": 42}))
		case r.Method == "DELETE" && r.URL.Path == "/session/session-1":
			must(enc.Encode(map[string]any{"value": nil}))
		default:
			w.WriteHeader(http.StatusNotFound)
			must(enc.Encode(map[string]any{"value": map[string]any{"error": "unknown command", "message": "fixture accepts no capability re-fetch"}}))
		}
	}))
	defer server.Close()
	address = "ws" + strings.TrimPrefix(server.URL, "http")
	caps := selenium.Capabilities{"browserName": "firefox", "webSocketUrl": true}
	wd, err := selenium.NewRemote(caps, server.URL)
	must(err)
	result, err := wd.ExecuteScript("return 6*7", nil)
	must(err)
	if result != float64(42) {
		panic(fmt.Sprintf("unexpected script result %v", result))
	}
	fmt.Printf("HTTP session=%s ExecuteScript=%v requested_capability=%v\n", wd.SessionID(), result, caps["webSocketUrl"])
	conn, _, err := websocket.DefaultDialer.Dial(address+"/bidi", nil)
	must(err)
	must(conn.WriteJSON(map[string]any{"id": 1, "method": "session.status", "params": map[string]any{}}))
	var response struct {
		ID     int    `json:"id"`
		Type   string `json:"type"`
		Result struct {
			Ready bool `json:"ready"`
		} `json:"result"`
	}
	must(conn.ReadJSON(&response))
	must(conn.Close())
	if response.ID != 1 || response.Type != "success" || !response.Result.Ready {
		panic("WebSocket fixture did not reply")
	}
	fmt.Println("WIRE fixture session.status ready=true; no production BiDi client exists")
	must(wd.Quit())
	fmt.Println("BASELINE production HTTP create/script/quit OK; W3C webSocketUrl request missing")
}

func must(err error) {
	if err != nil {
		panic(err)
	}
}
