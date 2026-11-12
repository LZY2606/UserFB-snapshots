/*
Package secure is an HTTP middleware for Go that facilitates some quick security wins.

	package main

	import (
	    "net/http"

	    "github.com/unrolled/secure"
	)

	var myHandler = http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
	    w.Write([]byte("hello world"))
	})

	func main() {
	    secureMiddleware := secure.New(secure.Options{
	        AllowedHosts: []string{"www.example.com", "sub.example.com"},
	        SSLRedirect:  true,
	    })

	    app := secureMiddleware.Handler(myHandler)
	    http.ListenAndServe("127.0.0.1:3000", app)
	}

A Content-Security-Policy value may contain the $NONCE placeholder. It is
expanded once per request and the same nonce is shared by every Secure
middleware in the chain and by CSPNonce. The Options.NonceSource function lets
applications plug in their own nonce generator; see WithCSPNonce and
ErrInvalidNonce for the related contracts.
*/
package secure
