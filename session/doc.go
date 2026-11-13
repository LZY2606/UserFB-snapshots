/*
Package session implements a session Manager for establishing session
connections to targets (via the Target domain). Session connections allow a
single websocket connection (from the provided cdp.Client) to be used for
communicating with multiple targets.

Initialize a new session Manager.

	c := cdp.NewClient(conn) // cdp.Client with websocket connection.

	m, err := session.NewManager(c)
	if err != nil {
		// Handle error.
	}
	defer m.Close() // Cleanup.

Establish a new session connection to targetID.

	pageConn, err := m.Dial(context.TODO(), targetID)
	if err != nil {
		// Handle error.
	}
	defer pageConn.Close()

Use the session connection.

	pageClient := cdp.NewClient(pageConn)
	err = pageClient.Page.Enable(context.TODO())
	// ...

If session connections are behaving unexpectedly, you can debug the session
Manager by checking the error channel.

	go func() {
		for err := range m.Err() {
			log.Println(err)
		}
		// Manager is closed.
	}()

AutoAttach takes over targets that the browser attaches automatically.
Targets are paused at startup (waitForDebuggerOnStart), the Init callback
runs against the paused target, and its scripts are resumed with
Runtime.runIfWaitingForDebugger before the Ready callback hands over the
session connection. AutoAttach can only be attempted once per Manager;
Dial keeps working alongside it.

	err = m.AutoAttach(context.TODO(), session.AutoAttachOptions{
		Init: func(ctx context.Context, info target.Info, c *cdp.Client) error {
			// Initialize the target while its scripts are paused.
			return nil
		},
		Ready: func(info target.Info, conn *rpcc.Conn) {
			// The target is running; conn is its session connection.
		},
	})
	if err != nil {
		// Handle error.
	}
*/
package session
