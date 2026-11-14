package fetch

import "encoding/json"

// MarshalJSON encodes FulfillRequestArgs while distinguishing a nil Body
// from a non-nil, empty one.
//
// A nil Body keeps the generated behavior and omits the "body" field, so
// that the browser's original response body is used. A non-nil slice is
// always emitted (as a base64 string), including an empty slice which
// serializes as "body": "".
func (a FulfillRequestArgs) MarshalJSON() ([]byte, error) {
	// Alias strips methods so json.Marshal does not recurse back
	// into this MarshalJSON.
	type alias FulfillRequestArgs
	wire := struct {
		alias
		Body *[]byte `json:"body,omitempty"`
	}{
		alias: alias(a),
	}
	if a.Body != nil {
		// The shallower Body field shadows the embedded one, so a
		// non-nil pointer is always emitted; a nil pointer omits
		// the field just like the generated struct.
		body := a.Body
		wire.Body = &body
	}
	return json.Marshal(wire)
}
