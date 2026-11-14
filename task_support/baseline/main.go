package main

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strings"

	graphql "github.com/machinebox/graphql"
)

type transport func(*http.Request) (*http.Response, error)

func (f transport) RoundTrip(r *http.Request) (*http.Response, error) { return f(r) }

type countedBody struct {
	io.Reader
	closed *int
}

func (b *countedBody) Close() error { *b.closed++; return nil }

type badReader struct{ err error }

func (b badReader) Read([]byte) (int, error) { return 0, b.err }

func main() {
	calls, closed := 0, 0
	wire := []string{
		`{"data":{"left":{"__typename":"Product","id":"7","name":"Paper","seller":{"__typename":"Seller","id":"s","name":"Old"}}}}`,
		`{"data":{"right":{"__typename":"Product","id":"7","price":19,"seller":{"__typename":"Seller","id":"s","name":"New"}}}}`,
		`{"errors":[{"message":"denied"}]}`,
	}
	client := graphql.NewClient("https://collector.invalid/graphql", graphql.WithHTTPClient(&http.Client{Transport: transport(func(r *http.Request) (*http.Response, error) {
		defer r.Body.Close()
		raw, err := io.ReadAll(r.Body)
		if err != nil {
			return nil, err
		}
		var body struct {
			Query     string                 `json:"query"`
			Variables map[string]interface{} `json:"variables"`
		}
		if err = json.Unmarshal(raw, &body); err != nil {
			return nil, err
		}
		if r.Method != "POST" || r.Header.Get("X-Collection") != "baseline" || body.Variables["id"] != "7" || !strings.Contains(body.Query, "product(id:$id)") {
			panic("request construction changed")
		}
		if calls >= len(wire) {
			panic("unexpected request")
		}
		data := wire[calls]
		calls++
		return &http.Response{StatusCode: 200, Header: make(http.Header), Body: &countedBody{Reader: strings.NewReader(data), closed: &closed}, Request: r}, nil
	})}))
	request := func(alias, fields string) *graphql.Request {
		req := graphql.NewRequest("query($id:ID!){" + alias + ":product(id:$id){__typename id " + fields + " seller{__typename id name}}}")
		req.Var("id", "7")
		req.Header.Set("X-Collection", "baseline")
		return req
	}
	var first, second json.RawMessage
	if err := client.Run(context.Background(), request("left", "name"), &first); err != nil {
		panic(err)
	}
	if err := client.Run(context.Background(), request("right", "price"), &second); err != nil {
		panic(err)
	}
	var a, b map[string]interface{}
	if err := json.Unmarshal(first, &a); err != nil {
		panic(err)
	}
	if err := json.Unmarshal(second, &b); err != nil {
		panic(err)
	}
	left := a["left"].(map[string]interface{})
	right := b["right"].(map[string]interface{})
	if left["id"] != right["id"] || left["name"] != "Paper" || right["price"] != float64(19) || left["seller"].(map[string]interface{})["name"] != "Old" || right["seller"].(map[string]interface{})["name"] != "New" {
		panic("normal extraction mismatch")
	}
	if _, ok := left["price"]; ok {
		panic("old response unexpectedly changed")
	}
	fmt.Println("BASELINE GraphQL requests=2 same-entity independent-results=2 old-seller=Old new-seller=New")
	if err := client.Run(context.Background(), request("left", "name"), nil); err == nil || err.Error() != "graphql: denied" {
		panic("business error changed")
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if err := client.Run(ctx, request("left", "name"), nil); !errors.Is(err, context.Canceled) || calls != 3 || closed != 3 {
		panic("cancellation or body close mismatch")
	}
	sentinel := errors.New("body read sentinel")
	failing := graphql.NewClient("https://collector.invalid/graphql", graphql.WithHTTPClient(&http.Client{Transport: transport(func(r *http.Request) (*http.Response, error) {
		return &http.Response{StatusCode: 200, Body: &countedBody{Reader: badReader{sentinel}, closed: &closed}, Request: r}, nil
	})}))
	if err := failing.Run(context.Background(), request("left", "name"), nil); !errors.Is(err, sentinel) || closed != 4 {
		panic("read error cause lost")
	}
	fmt.Println("CONTROL GraphQL business-error=1 pre-cancel-no-request=1 read-error-cause=1 bodies-closed=4")
}
