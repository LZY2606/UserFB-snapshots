package main

import (
	"errors"
	"fmt"
	"go/ast"
	"go/parser"
	"go/token"
	"io"
	"os"
	"strings"

	"github.com/PuerkitoBio/goquery"
)

type failReader struct{ err error }

func (r failReader) Read([]byte) (int, error) { return 0, r.err }

type traceReader struct {
	r   io.Reader
	eof bool
}

func (r *traceReader) Read(p []byte) (int, error) {
	n, err := r.r.Read(p)
	if err == io.EOF {
		r.eof = true
	}
	return n, err
}

func main() {
	r := &traceReader{r: strings.NewReader(`<body><p id="first">A&amp;B</p><table id="t">outside<tr><td>cell</td></tr></table><p id="last">Z</p>`)}
	doc, err := goquery.NewDocumentFromReader(r)
	if err != nil {
		panic(err)
	}
	if !r.eof || doc.Find("#first").Text() != "A&B" || doc.Find("#t td").Text() != "cell" {
		panic("unexpected production parse")
	}
	body, err := doc.Find("body").Html()
	if err != nil {
		panic(err)
	}
	fault := errors.New("source interrupted")
	partial, got := goquery.NewDocumentFromReader(io.MultiReader(strings.NewReader(`<body><p>A</p>`), failReader{fault}))
	if got != fault || partial != nil {
		panic("unexpected reader failure contract")
	}
	state := "missing"
	files, err := parser.ParseDir(token.NewFileSet(), ".", func(info os.FileInfo) bool { return !strings.HasSuffix(info.Name(), "_test.go") }, 0)
	if err != nil {
		panic(err)
	}
	for _, pkg := range files {
		for _, file := range pkg.Files {
			for _, decl := range file.Decls {
				if fn, ok := decl.(*ast.FuncDecl); ok && fn.Recv == nil && fn.Name.Name == "StreamBody" {
					state = "present"
				}
			}
		}
	}
	fmt.Printf("FULL_PARSE_READER_EOF=%t\n", r.eof)
	fmt.Println("BODY=" + body)
	fmt.Printf("INTERRUPTED_PREFIX_DOC_NIL=%t ERROR_IDENTITY=%t\n", partial == nil, got == fault)
	fmt.Println("STREAM_BODY_API=" + state)
}
