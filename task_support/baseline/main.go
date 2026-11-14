package main

import (
	"bytes"
	"context"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"time"

	"github.com/mafredri/cdp"
	"github.com/mafredri/cdp/protocol/fetch"
	cdpio "github.com/mafredri/cdp/protocol/io"
	"github.com/mafredri/cdp/rpcc"
)

type request struct {
	ID     int             `json:"id"`
	Method string          `json:"method"`
	Params json.RawMessage `json:"params"`
}

func serve(peer net.Conn) error {
	dec, enc := json.NewDecoder(peer), json.NewEncoder(peer)
	reads, closed, fulfilled := 0, false, false
	for {
		var req request
		if err := dec.Decode(&req); err != nil {
			if err == io.EOF && reads == 2 && closed && fulfilled {
				return nil
			}
			return fmt.Errorf("peer ended: reads=%d close=%t fulfill=%t: %w", reads, closed, fulfilled, err)
		}
		var result any = map[string]any{}
		switch req.Method {
		case "Fetch.takeResponseBodyAsStream":
			var a fetch.TakeResponseBodyAsStreamArgs
			if err := json.Unmarshal(req.Params, &a); err != nil {
				return err
			}
			if a.RequestID != "sample" {
				return fmt.Errorf("wrong requestId")
			}
			result = map[string]any{"stream": "body"}
			fmt.Println("FETCH acquired sequential stream=body")
		case "IO.read":
			var a cdpio.ReadArgs
			if err := json.Unmarshal(req.Params, &a); err != nil {
				return err
			}
			if a.Handle != "body" || closed {
				return fmt.Errorf("read invalid stream")
			}
			if a.Offset != nil {
				fmt.Printf("EXISTING_READER offset=%d rejected\n", *a.Offset)
				if err := enc.Encode(map[string]any{"id": req.ID, "error": map[string]any{"code": -32602, "message": "sequential stream forbids offset"}}); err != nil {
					return err
				}
				continue
			}
			reads++
			switch reads {
			case 1:
				result = map[string]any{"data": "ab", "eof": false}
			case 2:
				result = map[string]any{"data": "AP8=", "base64Encoded": true, "eof": true}
			default:
				return fmt.Errorf("read after EOF")
			}
			fmt.Printf("RAW_READ offset=omitted block=%d\n", reads)
		case "IO.close":
			var a cdpio.CloseArgs
			if err := json.Unmarshal(req.Params, &a); err != nil {
				return err
			}
			if a.Handle != "body" || closed {
				return fmt.Errorf("invalid/duplicate close")
			}
			closed = true
		case "Fetch.fulfillRequest":
			var a fetch.FulfillRequestArgs
			if err := json.Unmarshal(req.Params, &a); err != nil {
				return err
			}
			body := a.Body
			if !closed || a.RequestID != "sample" || a.ResponseCode != 200 || !bytes.Equal(body, []byte{'a', 'b', 0, 255}) {
				return fmt.Errorf("incorrect fulfillment")
			}
			fulfilled = true
			fmt.Printf("FULFILL code=200 bytes=%d body_hex=%x\n", len(body), body)
		default:
			return fmt.Errorf("unexpected method: %s", req.Method)
		}
		if err := enc.Encode(map[string]any{"id": req.ID, "result": result}); err != nil {
			return err
		}
	}
}

func main() {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	transport, peer := net.Pipe()
	defer peer.Close()
	serverErr := make(chan error, 1)
	go func() { serverErr <- serve(peer) }()
	conn, err := rpcc.DialContext(ctx, "pipe", rpcc.WithDialer(func(context.Context, string) (io.ReadWriteCloser, error) { return transport, nil }))
	must(err)
	c := cdp.NewClient(conn)
	emptyWire, err := json.Marshal(fetch.NewFulfillRequestArgs("empty", 200).SetBody([]byte{}))
	must(err)
	var emptyFields map[string]json.RawMessage
	must(json.Unmarshal(emptyWire, &emptyFields))
	_, present := emptyFields["body"]
	fmt.Printf("EMPTY_BODY body_field_present=%t\n", present)
	stream, err := c.Fetch.TakeResponseBodyAsStream(ctx, fetch.NewTakeResponseBodyAsStreamArgs("sample"))
	must(err)
	reader := c.NewIOStreamReader(ctx, stream.Stream)
	_, err = reader.Read(make([]byte, 8))
	var protocolErr *rpcc.ResponseError
	if !errors.As(err, &protocolErr) || protocolErr.Code != -32602 {
		panic(fmt.Errorf("expected offset rejection, got %v", err))
	}
	fmt.Printf("OBSERVED protocol_error=%d errors.As=true\n", protocolErr.Code)
	var body []byte
	for {
		r, err := c.IO.Read(ctx, cdpio.NewReadArgs(stream.Stream).SetSize(1))
		must(err)
		part := []byte(r.Data)
		if r.Base64Encoded != nil && *r.Base64Encoded {
			part, err = base64.StdEncoding.DecodeString(r.Data)
			must(err)
		}
		body = append(body, part...)
		if r.EOF {
			break
		}
	}
	must(reader.Close())
	args := fetch.NewFulfillRequestArgs("sample", 200)
	args.Body = body
	must(c.Fetch.FulfillRequest(ctx, args))
	must(conn.Close())
	must(<-serverErr)
	fmt.Println("BASELINE production Fetch/IO/fulfill OK; sequential reader offset mismatch reproduced")
}

func must(err error) {
	if err != nil {
		panic(err)
	}
}
