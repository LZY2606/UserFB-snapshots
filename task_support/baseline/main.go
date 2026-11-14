package main

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"time"

	"github.com/mafredri/cdp"
	"github.com/mafredri/cdp/rpcc"
)

type codec struct { enc *json.Encoder; dec *json.Decoder }
func (c *codec) WriteRequest(r *rpcc.Request) error { return c.enc.Encode(r) }
func (c *codec) ReadResponse(r *rpcc.Response) error { return c.dec.Decode(r) }
func must(err error) { if err != nil { panic(err) } }
func ready(s rpcc.Stream) bool { select { case <-s.Ready(): return true; default: return false } }

func main() {
	ctx, cancel := context.WithTimeout(context.Background(), 8*time.Second)
	defer cancel()
	local, peer := net.Pipe()
	defer peer.Close()
	go func() {
		dec, enc := json.NewDecoder(peer), json.NewEncoder(peer)
		for {
			var req rpcc.Request
			if err := dec.Decode(&req); err != nil { return }
			base := 1
			if req.Method == "Page.enable" { base = 3 }
			if err := enc.Encode(map[string]interface{}{"method":"Page.domContentEventFired", "params":map[string]int{"timestamp":base}}); err != nil { return }
			if err := enc.Encode(map[string]interface{}{"method":"Page.loadEventFired", "params":map[string]int{"timestamp":base+1}}); err != nil { return }
			if err := enc.Encode(map[string]interface{}{"id":req.ID,"result":map[string]interface{}{}}); err != nil { return }
		}
	}()
	conn, err := rpcc.DialContext(ctx, "pipe", rpcc.WithDialer(func(context.Context,string)(io.ReadWriteCloser,error){return local,nil}), rpcc.WithCodec(func(r io.ReadWriter)rpcc.Codec{return &codec{json.NewEncoder(r),json.NewDecoder(r)}}))
	must(err); defer conn.Close()
	client := cdp.NewClient(conn)
	dom, err := client.Page.DOMContentEventFired(ctx); must(err); defer dom.Close()
	load, err := client.Page.LoadEventFired(ctx); must(err); defer load.Close()
	must(rpcc.Invoke(ctx, "Baseline.before", nil, nil, conn))
	if !ready(dom) || !ready(load) { panic("events were not buffered") }
	must(cdp.Sync(dom, load))
	if ready(dom) || ready(load) { panic("legacy Sync clearing changed") }
	must(client.Page.Enable(ctx, nil))
	if ready(load) { panic("later stream bypassed earlier event") }
	d, err := dom.Recv(); must(err)
	l, err := load.Recv(); must(err)
	if float64(d.Timestamp) != 3 || float64(l.Timestamp) != 4 { panic("fresh event order wrong") }
	fmt.Println("BASELINE_OK generated Page client; buffered=2 legacy Sync retained=0; fresh=3,4")
}
