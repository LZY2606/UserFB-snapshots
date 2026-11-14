package main
import (
 "fmt"
 "strings"
 "github.com/andybalholm/cascadia"
 "golang.org/x/net/html"
)
func main() {
 d,err:=html.Parse(strings.NewReader("<table><colgroup><col><col class=money></colgroup><tbody><tr><td rowspan=2>A</td><td id=top><a>10</a></td></tr><tr><td id=bottom><a>20</a></td></tr></tbody></table>"))
 if err!=nil {panic(err)}
 old,err:=cascadia.Parse("td:nth-child(2)")
 if err!=nil {panic(err)}
 cells:=cascadia.QueryAll(d,old)
 if len(cells)!=1 {panic("normal parser/selector baseline failed")}
 for _,a:=range cells[0].Attr {if a.Key=="id" {fmt.Println("BASELINE_OK DOM_SECOND="+a.Val)}}
 for _,s:=range []string{"col.money || td", "td:nth-col(2)", "td:nth-last-col(1)"} {
  sel,parseErr:=cascadia.Parse(s)
  if parseErr!=nil {panic("column syntax failed to parse: "+s)}
  fmt.Printf("COLUMN_SUPPORTED %q nodes=%d\n",s,len(cascadia.QueryAll(d,sel)))
 }
}
