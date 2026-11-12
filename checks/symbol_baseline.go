package main
import (
 "fmt"
 d "github.com/ianlancetaylor/demangle"
)
func main() {
 cases:=map[string]string{
  "_Z1fPN1A1BES1_":"f(A::B*, A::B*)",
  "_Z1fPKiS0_":"f(int const*, int const*)",
  "_Z1fPFiiES0_":"f(int (*)(int), int (*)(int))",
 }
 for symbol,want:=range cases {
  ast,e:=d.ToAST(symbol);if e!=nil {panic(e)}
  got:=d.ASTToString(ast);if got!=want {panic(got+" != "+want)}
  s,e:=d.ToString(symbol);if e!=nil || s!=want {panic("entry mismatch")}
 }
 fmt.Println("ITANIUM_BASELINE_OK symbols=3")
}
