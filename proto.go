package marionette_client

import (
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"strconv"
)

func NewDecoderEncoder(protoVersion int32) (DecoderEncoder, error) {
	if protoVersion == MarionetteProtocolV3 {
		return ProtoV3DecoderEncoder{}, nil
	}

	m := fmt.Sprintf("No DecoderEncoder found for the specified version : %v", protoVersion)

	return nil, errors.New(m)
}

type Decoder interface {
	Decode(buf []byte, r *Response) error
}

type Encoder interface {
	Encode(t Transporter, command string, values any) ([]byte, error)
}

type DecoderEncoder interface {
	Decoder
	Encoder
}

type ProtoV3DecoderEncoder struct{}

func (e ProtoV3DecoderEncoder) Encode(t Transporter, command string, values any) ([]byte, error) {
	message := make([]any, 4)
	message[0] = 0
	message[1] = t.MessageID()
	message[2] = command
	message[3] = values

	b, err := json.Marshal(message)
	if err != nil {
		return nil, err
	}

	if RunningInDebugMode {
		fmt.Println(string(b))
	}

	return []byte(strconv.Itoa(len(b)) + ":" + string(b)), nil

}

func (e ProtoV3DecoderEncoder) Decode(buf []byte, r *Response) error {
	var v []any
	if err := json.Unmarshal(buf, &v); err != nil {
		return err
	}

	//Debug only
	if RunningInDebugMode {
		if len(buf) >= 512 {
			log.Println(string(buf)[0:256] + " - END - " + string(buf)[len(buf)-256:])
		} else {
			log.Println(string(buf))
		}
	}
	//Debug only end

	// A Marionette v3 reply is always a four-element array whose first
	// element is 1 (response type). Anything else is protocol corruption.
	if len(v) != 4 {
		return fmt.Errorf("marionette: invalid response: expected 4 elements, got %d", len(v))
	}

	replyType, ok := v[0].(float64)
	if !ok || replyType != 1 {
		return fmt.Errorf("marionette: invalid response type: expected 1, got %v", v[0])
	}

	// Message IDs are positive int32 integers.
	id, ok := v[1].(float64)
	if !ok || id != float64(int32(id)) || int32(id) <= 0 {
		return fmt.Errorf("marionette: invalid response message ID: %v", v[1])
	}

	r.MessageID = int32(id)
	r.Size = int32(len(buf))

	// error found on response?
	if v[2] != nil {
		errObj, ok := v[2].(map[string]any)
		if !ok {
			return fmt.Errorf("marionette: invalid driver error encoding: %v", v[2])
		}

		re := &DriverError{}
		errorType, ok := errObj["error"].(string)
		if !ok {
			return fmt.Errorf("marionette: driver error is missing a string \"error\" field")
		}
		message, ok := errObj["message"].(string)
		if !ok {
			return fmt.Errorf("marionette: driver error is missing a string \"message\" field")
		}
		re.ErrorType = errorType
		re.Message = message

		switch trace := errObj["stacktrace"].(type) {
		case nil:
		case string:
			theTrace := trace
			re.Stacktrace = &theTrace
		default:
			return fmt.Errorf("marionette: driver error \"stacktrace\" must be a string or null")
		}

		r.DriverError = re
		return re
	}

	// It's a JSON Object
	result, found := v[3].(map[string]any)
	if found {
		resultBytes, err := json.Marshal(result)
		if err != nil {
			return err
		}

		r.Value = string(resultBytes)

		return nil
	}

	// JSON Array
	arrayResult, found := v[3].([]any)
	if found {
		resultBytes, err := json.Marshal(arrayResult)
		if err != nil {
			return err
		}

		r.Value = string(resultBytes)
	}

	// Other scalar result types have no structured encoding; keep the
	// historical behavior of leaving Value empty.
	return nil
}
