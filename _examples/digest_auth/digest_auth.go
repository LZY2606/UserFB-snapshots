package main

import (
	"fmt"
	"log"

	"github.com/gocolly/colly/v2"
)

// The example targets a local endpoint. Point it at your own HTTP Digest
// protected API; credentials are only sent after a supported 401 challenge.
func main() {
	c := colly.NewCollector()

	if err := c.SetDigestAuth("http://localhost:8080", "collector", "s3cret"); err != nil {
		log.Fatal(err)
	}

	c.OnResponse(func(r *colly.Response) {
		fmt.Println("authenticated response:", r.StatusCode, string(r.Body))
	})

	c.OnError(func(r *colly.Response, err error) {
		if r != nil && r.StatusCode == 401 {
			fmt.Println("challenge not supported, status:", r.StatusCode)
			return
		}
		log.Println("error:", err)
	})

	if err := c.Visit("http://localhost:8080/reports/monthly"); err != nil {
		log.Fatal(err)
	}
}
