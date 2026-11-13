package session_test

import (
	"context"
	"log"

	"github.com/mafredri/cdp"
	"github.com/mafredri/cdp/protocol/runtime"
	"github.com/mafredri/cdp/protocol/target"
	"github.com/mafredri/cdp/rpcc"
	"github.com/mafredri/cdp/session"
)

// This example shows how to take over targets (e.g. pages or workers)
// that the browser attaches automatically. The collection script is
// installed while the target is still paused, and only afterwards are
// its scripts resumed. The session connection handed to Ready is the
// same kind of connection Manager.Dial returns, tunneled over the
// parent connection.
func ExampleManager_AutoAttach() {
	// conn is the websocket connection to the browser (or another
	// debug target), see rpcc.DialContext.
	var conn *rpcc.Conn

	parent := cdp.NewClient(conn)
	manager, err := session.NewManager(parent)
	if err != nil {
		log.Fatal(err)
	}
	defer manager.Close()

	// Surface targets that could not be initialized or resumed.
	go func() {
		for err := range manager.Err() {
			log.Println("auto-attach:", err)
		}
	}()

	err = manager.AutoAttach(context.Background(), session.AutoAttachOptions{
		// Init runs against every auto-attached target, before the
		// target's scripts are resumed (when it reported
		// waitingForDebugger). The client is already connected to the
		// target, so regular CDP commands work here.
		Init: func(ctx context.Context, info target.Info, c *cdp.Client) error {
			_, err := c.Runtime.Evaluate(ctx,
				runtime.NewEvaluateArgs(`window.__collector = installCollector();`).
					SetReturnByValue(true))
			return err
		},
		// Ready is called after Init succeeded and, when needed, after
		// Runtime.runIfWaitingForDebugger. Ownership of the session
		// connection is handed over here; closing it detaches the
		// target.
		Ready: func(info target.Info, conn *rpcc.Conn) {
			client := cdp.NewClient(conn)
			log.Printf("target %s (%s) is ready", info.TargetID, info.Type)
			_ = client // Keep the connection for the lifetime of the target.
		},
	})
	if err != nil {
		log.Fatal(err)
	}
}
