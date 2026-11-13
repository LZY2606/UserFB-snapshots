package main

import (
	"bytes"
	"fmt"
	"github.com/asticode/go-astisub"
	"strings"
)

func main() {
	s, err := astisub.ReadFromWebVTT(strings.NewReader("WEBVTT\n\n00:00:01.000 --> 00:00:03.000\n<c.red>東京</c>駅\n次へ\n"))
	must(err)
	if len(s.Items) != 1 || len(s.Items[0].Lines) != 2 || s.Items[0].Lines[0].String() != "東京駅" {
		panic("unexpected normal subtitle")
	}
	var ttml bytes.Buffer
	must(s.WriteToTTML(&ttml))
	if !strings.Contains(ttml.String(), "#ff0000") {
		panic("red color not exported")
	}
	back, err := astisub.ReadFromTTML(strings.NewReader(ttml.String()))
	must(err)
	if len(back.Items) != 1 || len(back.Items[0].Lines) != 2 || back.Items[0].Lines[0].String() != "東京駅" {
		panic("TTML normal roundtrip lost text")
	}
	var srt bytes.Buffer
	must(back.WriteToSRT(&srt))
	if !strings.Contains(srt.String(), "東京") || !strings.Contains(srt.String(), "次へ") {
		panic("SRT normal output missing")
	}
	fmt.Printf("NORMAL WebVTT->TTML->SRT cues=%d lines=%d text=%q color=red\n", len(back.Items), len(back.Items[0].Lines), back.Items[0].Lines[0].String())
	fmt.Print(srt.String())
}

func must(err error) {
	if err != nil {
		panic(err)
	}
}
