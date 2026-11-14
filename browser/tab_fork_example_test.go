package browser

import (
	"fmt"
	"net/http"
	"strings"
)

// ExampleBrowser_ForkTab demonstrates recording an application form with
// two branched tabs: after opening the confirmation page, the browser is
// forked, and each branch fills in the form and submits it on its own,
// without changing the page of the other branch.
func ExampleBrowser_ForkTab() {
	ts, transport := newPipeServer(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/submit" {
			if err := r.ParseForm(); err != nil {
				http.Error(w, err.Error(), http.StatusBadRequest)
				return
			}
			fmt.Fprintf(w, `<html><body>submitted name=%s</body></html>`, r.Form.Get("name"))
			return
		}
		fmt.Fprint(w, `<html><body><form method="post" action="/submit"><input name="name" value="seed"></form></body></html>`)
	})
	defer ts.Close()

	bow := newDefaultTestBrowser()
	bow.SetTransport(transport)
	if err := bow.Open(ts.URL + "/apply"); err != nil {
		fmt.Println("open:", err)
		return
	}

	// Branch the tab before submitting; the fork continues from the same
	// page, but fills in and submits its own copy of the form.
	fork, err := bow.ForkTab()
	if err != nil {
		fmt.Println("fork:", err)
		return
	}

	formA, err := bow.Form("form")
	if err != nil {
		fmt.Println("form A:", err)
		return
	}
	if err := formA.Input("name", "alice"); err != nil {
		fmt.Println("input A:", err)
		return
	}

	formB, err := fork.Form("form")
	if err != nil {
		fmt.Println("form B:", err)
		return
	}
	if err := formB.Input("name", "bob"); err != nil {
		fmt.Println("input B:", err)
		return
	}

	if err := formB.Submit(); err != nil {
		fmt.Println("submit B:", err)
		return
	}
	if err := formA.Submit(); err != nil {
		fmt.Println("submit A:", err)
		return
	}

	fmt.Println("first branch:", strings.TrimSpace(bow.Find("body").Text()))
	fmt.Println("second branch:", strings.TrimSpace(fork.Find("body").Text()))

	// Output:
	// first branch: submitted name=alice
	// second branch: submitted name=bob
}
