# graphql [![GoDoc](https://godoc.org/github.com/machinebox/graphql?status.png)](http://godoc.org/github.com/machinebox/graphql) [![Build Status](https://travis-ci.org/machinebox/graphql.svg?branch=master)](https://travis-ci.org/machinebox/graphql) [![Go Report Card](https://goreportcard.com/badge/github.com/machinebox/graphql)](https://goreportcard.com/report/github.com/machinebox/graphql)

Low-level GraphQL client for Go.

* Simple, familiar API
* Respects `context.Context` timeouts and cancellation
* Build and execute any kind of GraphQL request
* Use strong Go types for response data
* Use variables and upload files
* Simple error handling

## Installation
Make sure you have a working Go environment. To install graphql, simply run:

```
$ go get github.com/machinebox/graphql
```

## Usage

```go
import "context"

// create a client (safe to share across requests)
client := graphql.NewClient("https://machinebox.io/graphql")

// make a request
req := graphql.NewRequest(`
    query ($key: String!) {
        items (id:$key) {
            field1
            field2
            field3
        }
    }
`)

// set any variables
req.Var("key", "value")

// set header fields
req.Header.Set("Cache-Control", "no-cache")

// define a Context for the request
ctx := context.Background()

// run it and capture the response
var respData ResponseStruct
if err := client.Run(ctx, req, &respData); err != nil {
    log.Fatal(err)
}
```

### File support via multipart form data

By default, the package will send a JSON body. To enable the sending of files, you can opt to
use multipart form data instead using the `UseMultipartForm` option when you create your `Client`:

```
client := graphql.NewClient("https://machinebox.io/graphql", graphql.UseMultipartForm())
```

## Entity collections

When the same product keeps coming back under different aliases, fragments and list
positions, `Run` decodes every response into an independent object and later detail
fetches cannot reach the record stored by an earlier catalogue fetch. A `Collection`
solves this: it deduplicates the entities found in responses, merges fields across
requests, and projects the stored entities back into the shape of any compatible query.

Create one collection per data source and keep it in memory; collections are safe for
concurrent `Collect` and `Read` calls.

```go
collection := graphql.NewCollection()

// Collect performs a real HTTP POST every time; local data never replaces the
// network request. Only a single JSON query operation (shorthand or named, with
// variables and named fragments) is accepted — mutations, subscriptions,
// multi-operation documents and file uploads are rejected.
req := graphql.NewRequest(`
    query ($id: ID!) {
        left: product(id: $id) { t: __typename k: id name seller { __typename id name } }
    }
`)
req.Var("id", "7")
if err := client.Collect(ctx, req, collection); err != nil {
    log.Fatal(err)
}
```

Entities are identified by the ordered `(__typename, id)` pair, so response aliases
never affect identity. A field's identity is its original field name plus its bound
arguments; argument and input-object key order is irrelevant while list order and
integer values are normalized. Objects without a non-empty string `__typename` and
`id` stay independent embedded values that are replaced with their owning field.
Explicit `null` overwrites a previous scalar, and list fields are replaced wholesale
while preserving order, duplicates and null slots. If one response gives the same
entity field two different values, the whole response is rejected and the previous
collection is left untouched.

`Read` never touches the network. It accepts fresh aliases, equivalent arguments,
named and inline fragments and `@skip`/`@include` directives, and returns an
independent result — mutating it cannot change the collection. A field that has never
been collected yields a nil result and an error wrapping `graphql.ErrIncomplete`,
rather than being mistaken for an explicit null.

```go
out, err := collection.Read(graphql.NewRequest(`
    query ($id: ID!) {
        item: product(id: $id) {
            __typename
            ...ProductFields
            seller { __typename id name }
        }
    }
    fragment ProductFields on Product { k: id name price }
`))
if errors.Is(err, graphql.ErrIncomplete) {
    // some selected field has not been collected yet
}
```

For more information, [read the godoc package documentation](http://godoc.org/github.com/machinebox/graphql) or the [blog post](https://blog.machinebox.io/a-graphql-client-library-for-go-5bffd0455878).

## Thanks

Thanks to [Chris Broadfoot](https://github.com/broady) for design help.
