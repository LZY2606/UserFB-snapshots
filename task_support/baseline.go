package main

import (
 "bytes"
 "fmt"
 "strings"
 "github.com/asticode/go-astisub"
)

func main() {
 input := "WEBVTT\n\nSTYLE\n::cue(.brand) { color: #123456; font-weight: bold; }\n\n00:00:01.000 --> 00:00:03.000\n<c.brand>A</c><c.red>B</c>\n"
 s,err:=astisub.ReadFromWebVTT(strings.NewReader(input)); if err!=nil {panic(err)}
 if len(s.Items)!=1 || len(s.Items[0].Lines)!=1 || len(s.Items[0].Lines[0].Items)!=2 {panic("unexpected runs")}
 a:=s.Items[0].Lines[0].Items[0]; b:=s.Items[0].Lines[0].Items[1]
 if a.Text!="A" || b.Text!="B" || a.InlineStyle.TTMLColor!=nil || b.InlineStyle.TTMLColor==nil || b.InlineStyle.TTMLColor.HTMLString()!="#ff0000" {panic("capability probe changed")}
 var ttml, vtt bytes.Buffer
 if err=s.WriteToTTML(&ttml);err!=nil{panic(err)}
 cssState:="unresolved"; if strings.Contains(strings.ToLower(ttml.String()),"#123456"){cssState="resolved"}
 if !strings.Contains(ttml.String(),"#ff0000"){panic("built-in color missing")}
 if err=s.WriteToWebVTT(&vtt);err!=nil{panic(err)}
 if !strings.Contains(vtt.String(),"::cue(.brand)"){panic("STYLE lost")}
 reread,err:=astisub.ReadFromTTML(bytes.NewReader(ttml.Bytes()));if err!=nil{panic(err)}
 if reread.Items[0].StartAt!=s.Items[0].StartAt || reread.Items[0].Lines[0].String()!="AB"{panic("roundtrip failed")}
 fmt.Printf("BASELINE_OK runs=2 raw_style=retained built_in_red=exported css_brand=%s ttml_roundtrip=AB\n",cssState)
}
