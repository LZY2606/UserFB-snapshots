package main

import (
	"fmt"
	"net/url"

	"github.com/studio-b12/gowebdav"
)

func main() {
	out := gowebdav.PathEscape(gowebdav.Join("http://dav.invalid/base/", gowebdav.FixSlashes("docs")))
	fmt.Println(out)
	u, err := url.Parse(out)
	fmt.Println(u, err)
	fmt.Println(u.String())
}
