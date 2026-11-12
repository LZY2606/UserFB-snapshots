package main

import (
	"fmt"
	"github.com/tdewolff/canvas"
	"math"
)

func main() {
	family := canvas.NewFontFamily("dejavu")
	if err := family.LoadFontFile("resources/DejaVuSerif.ttf", canvas.FontRegular); err != nil {
		panic(err)
	}
	face := family.Face(12, canvas.Black, canvas.FontRegular, canvas.FontNormal)
	text := canvas.NewTextLine(face, "AV office", canvas.Left)
	glyphs := face.Glyphs("office")
	if len(glyphs) == 0 || len(glyphs) >= 6 {
		panic("ligature baseline missing")
	}
	p, w, err := face.ToPath("AV")
	if err != nil {
		panic(err)
	}
	if p.Empty() || !(w < face.TextWidth("A")+face.TextWidth("V")) {
		panic("kerning baseline missing")
	}
	c := canvas.New(80, 30)
	text.RenderAsPath(c, canvas.Identity, canvas.DefaultResolution)
	if c.Empty() {
		panic("text outline not rendered")
	}
	path := canvas.MustParseSVGPath("M0 0 C0 10 10 10 10 0")
	fmt.Printf("existing curve length %.9f\n", path.Length())
	if math.Abs(path.Length()-20) > 0.01 {
		panic("curve length")
	}
	fmt.Printf("CANVAS_TEXT_BASELINE_OK ligature_glyphs=%d curve_length=%.0f\n", len(glyphs), path.Length())
}
