package secure

import (
	"context"
	"crypto/rand"
	"encoding/base64"
	"errors"
	"io"
	"net/http"
)

type key int

const cspNonceKey key = iota

// ErrInvalidNonce is returned when a custom NonceSource generates a nonce that
// is empty or contains characters outside the allowed set
// (ASCII letters, digits and '+', '/', '_', '-', '=').
// Use errors.Is to test for it.
var ErrInvalidNonce = errors.New("secure: invalid CSP nonce")

// CSPNonce returns the nonce value associated with the present request.
// If no nonce has been generated it returns an empty string.
//
// Within a chain of Secure middlewares, every layer inherits the nonce of the
// first layer that needed one, so all expanded `$NONCE` placeholders and the
// value read here are identical for the duration of a request.
func CSPNonce(c context.Context) string {
	if val, ok := c.Value(cspNonceKey).(string); ok {
		return val
	}

	return ""
}

// WithCSPNonce returns a context derived from ctx containing the given nonce as a value.
//
// This is intended for testing or more advanced use-cases;
// For ordinary HTTP handlers, clients can rely on this package's middleware to populate the CSP nonce in the context.
//
// A non-empty value set here always takes precedence over the built-in
// generator and the Options.NonceSource function, and it is inherited as-is by
// every subsequent Secure middleware without validation: callers are trusted
// to supply a safe value. An empty string is treated as "not provided", so the
// generator still runs when a policy contains `$NONCE`.
func WithCSPNonce(ctx context.Context, nonce string) context.Context {
	return context.WithValue(ctx, cspNonceKey, nonce)
}

func withCSPNonce(r *http.Request, nonce string) *http.Request {
	return r.WithContext(WithCSPNonce(r.Context(), nonce))
}

func cspRandNonce() string {
	var buf [cspNonceSize]byte

	_, err := io.ReadFull(rand.Reader, buf[:])
	if err != nil {
		panic("CSP Nonce rand.Reader failed" + err.Error())
	}

	return base64.RawStdEncoding.EncodeToString(buf[:])
}

// isValidNonce reports whether nonce is non-empty and only contains characters
// from the allowed set: ASCII letters, digits and '+', '/', '_', '-', '='.
func isValidNonce(nonce string) bool {
	if nonce == "" {
		return false
	}

	for _, c := range nonce {
		switch {
		case c >= 'a' && c <= 'z', c >= 'A' && c <= 'Z', c >= '0' && c <= '9':
		case c == '+' || c == '/' || c == '_' || c == '-' || c == '=':
		default:
			return false
		}
	}

	return true
}
