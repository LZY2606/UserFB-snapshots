package graphql_test

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strings"

	graphql "github.com/machinebox/graphql"
)

// exampleTransport answers the two catalogue requests used by ExampleCollection
// without opening any network sockets.
type exampleTransport struct {
	calls int
}

func (t *exampleTransport) RoundTrip(r *http.Request) (*http.Response, error) {
	defer r.Body.Close()
	t.calls++
	body := `{"data":{"left":{"t":"Product","k":"7","name":"Paper","seller":{"__typename":"Seller","id":"s","name":"Old"}}}}`
	if t.calls == 2 {
		body = `{"data":{"right":{"__typename":"Product","id":"7","price":19,"seller":{"__typename":"Seller","id":"s","name":"New"}}}}`
	}
	return &http.Response{
		StatusCode: http.StatusOK,
		Header:     make(http.Header),
		Body:       io.NopCloser(strings.NewReader(body)),
		Request:    r,
	}, nil
}

// ExampleCollection shows how repeated GraphQL responses for the same entity
// are merged into one in-memory Collection and read back through a query with
// a different shape.
func ExampleCollection() {
	client := graphql.NewClient(
		"https://shop.invalid/graphql",
		graphql.WithHTTPClient(&http.Client{Transport: &exampleTransport{}}),
	)
	collection := graphql.NewCollection()
	ctx := context.Background()

	first := graphql.NewRequest(`
		query ($id: ID!) {
			left: product(id: $id) {
				t: __typename
				k: id
				name
				seller { __typename id name }
			}
		}
	`)
	first.Var("id", "7")
	if err := client.Collect(ctx, first, collection); err != nil {
		fmt.Println("first collect:", err)
		return
	}

	// The same Product arrives under another alias with a different field set;
	// Collect still performs a real POST and merges the new fields.
	second := graphql.NewRequest(`{
		right: product(id: "7") { __typename id price seller { __typename id name } }
	}`)
	if err := client.Collect(ctx, second, collection); err != nil {
		fmt.Println("second collect:", err)
		return
	}

	// Read performs no network access and may use fresh aliases, arguments,
	// fragments and directives.
	read := graphql.NewRequest(`
		query ($id: ID!, $withSeller: Boolean!) {
			item: product(id: $id) {
				__typename
				...ProductFields
				... on Product { price }
				seller @include(if: $withSeller) { __typename ...SellerFields }
			}
		}
		fragment ProductFields on Product { k: id name }
		fragment SellerFields on Seller { id name }
	`)
	read.Var("id", "7")
	read.Var("withSeller", true)

	out, err := collection.Read(read)
	if err != nil {
		fmt.Println("read:", err)
		return
	}
	item := out["item"].(map[string]interface{})
	seller := item["seller"].(map[string]interface{})
	fmt.Println(item["name"], item["price"], seller["name"])

	// A field that was never collected is reported through ErrIncomplete,
	// never silently as null.
	_, err = collection.Read(graphql.NewRequest(`{ product(id: "7") { __typename id weight } }`))
	fmt.Println(errors.Is(err, graphql.ErrIncomplete), strings.Contains(err.Error(), "missing"))
	// Output:
	// Paper 19 New
	// true true
}
