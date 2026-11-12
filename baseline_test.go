package robotstxt
import (
 "fmt"
 "testing"
)
func TestRobotsBaseline(t *testing.T) {
 r,e:=FromString("User-agent: bot\nDisallow: /private/\nAllow: /private/public/\nDisallow: /café\nDisallow: /x*end$\n")
 if e!=nil {t.Fatal(e)}
 g:=r.FindGroup("bot/1.0")
 for _,x:=range []struct{p string;want bool}{{"/private/a",false},{"/private/public/a",true},{"/other",true}} {
  if v:=g.Test(x.p);v!=x.want {t.Fatalf("%s = %v",x.p,v)}
 }
 fmt.Printf("LEGACY_ENCODED_CAFE_ALLOWED=%v\n",g.Test("/caf%C3%A9"))
 fmt.Printf("LEGACY_MIDPATH_PATTERN_ALLOWED=%v\n",g.Test("/other/x-end"))
 fmt.Println("BASELINE_ROBOTS_RULES_OK")
}
