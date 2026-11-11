// Command rtpfreeze-server serves the offline RTP/RTCP freeze review tool.
// It binds only to the local loopback address, talks to no online service, and
// stores everything in a local SQLite file.
package main

import (
	"bytes"
	"context"
	"flag"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"time"

	"rtpfreeze/internal/capture"
	"rtpfreeze/internal/store"
	"rtpfreeze/internal/web"
)

func main() {
	addr := flag.String("addr", "127.0.0.1:5532", "HTTP listen address")
	dbPath := flag.String("db", "data/rtpfreeze.db", "SQLite database file")
	reseed := flag.Bool("reseed", false, "re-import the bundled sample even if the database already has captures")
	flag.Parse()

	if err := os.MkdirAll(filepath.Dir(*dbPath), 0o755); err != nil {
		log.Fatalf("create data dir: %v", err)
	}
	st, err := store.Open(*dbPath)
	if err != nil {
		log.Fatalf("open database: %v", err)
	}
	defer st.Close()

	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	existing, err := st.ListCaptures(ctx)
	if err != nil {
		log.Fatalf("list captures: %v", err)
	}
	if *reseed || len(existing) == 0 {
		jsonl, err := capture.JSONL()
		if err != nil {
			log.Fatalf("build sample capture: %v", err)
		}
		res, err := st.Import(ctx, "bundled-sample.jsonl", bytes.NewReader(jsonl))
		if err != nil {
			log.Fatalf("import sample: %v", err)
		}
		log.Printf("imported bundled sample: %+v", res)
	}

	srv := &http.Server{
		Addr:              *addr,
		Handler:           web.NewServer(st),
		ReadHeaderTimeout: 10 * time.Second,
	}
	log.Printf("RTP freeze review tool listening on http://%s", *addr)
	if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		log.Fatalf("server: %v", err)
	}
}
