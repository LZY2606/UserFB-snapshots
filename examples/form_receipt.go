//go:build ignore
// +build ignore

package main

import "context"
import "fmt"
import "log"
import "time"

import "github.com/raff/godet"

// This example shows how to save the submission receipt of a form: the
// original POST, every redirect hop and the final response, including the
// wire-level status reported via Network.responseReceivedExtraInfo.
func main() {
	// connect to Chrome instance
	remote, err := godet.Connect("localhost:9222", false)
	if err != nil {
		fmt.Println("cannot connect to Chrome instance:", err)
		return
	}

	// disconnect when done
	defer remote.Close()

	// network events must be enabled by the caller
	_ = remote.NetworkEvents(true)

	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	match := godet.FormRequestMatch{
		FrameID: "FRAME-ID", // frame issuing the form request
		URL:     "https://example.com/apply",
		Method:  "POST",
	}

	receipt, err := remote.WaitFormReceipt(ctx, match, func() error {
		// trigger the submission; native input methods work too
		_, err := remote.Evaluate(`document.querySelector("form").requestSubmit()`)
		return err
	})
	if err != nil {
		log.Println("form submission not completed:", err)
		return
	}

	fmt.Println("request:", receipt.RequestID)
	for i, hop := range receipt.Hops {
		fmt.Printf("hop %d: %s %s -> %d %v\n", i, hop.Method, hop.URL, hop.Status, hop.Headers)
		if hop.HasExtraInfo {
			fmt.Printf("   wire: %d %v\n", hop.WireStatus, hop.ExtraHeaders)
		}
	}
}
