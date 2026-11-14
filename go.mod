module github.com/machinebox/graphql

go 1.23

require (
 github.com/pkg/errors v0.9.1
 github.com/matryer/is v1.4.1
 github.com/gorilla/websocket v1.5.3
)

replace github.com/pkg/errors => ./deps/pkg/errors

replace github.com/matryer/is => ./deps/matryer/is

replace github.com/gorilla/websocket => ./deps/gorilla/websocket
