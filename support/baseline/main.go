package main
import("fmt";"strings";"unicode/utf8";"github.com/nbutton23/zxcvbn-go";"github.com/nbutton23/zxcvbn-go/match")
func main(){
 filter:=func(match.Matcher)bool{return true}
 ascii:=zxcvbn.PasswordStrength("abc",[]string{"abc"},filter)
 if ascii.Password!="abc"||len(ascii.MatchSequence)!=1||ascii.MatchSequence[0].Token!="abc" {panic("ASCII dictionary baseline changed")}
 input:="🙂abc"
 r:=zxcvbn.PasswordStrength(input,[]string{"abc"},filter)
 tokens:=[]string{};valid:=true
 for _,m:=range r.MatchSequence{tokens=append(tokens,m.Token);valid=valid&&utf8.ValidString(m.Token);fmt.Printf("OBSERVED pattern=%s I=%d J=%d token=%q\n",m.Pattern,m.I,m.J,m.Token)}
 reconstructed:=strings.Join(tokens,"")
 fmt.Printf("OBSERVED unicode_tokens_valid=%v reconstructed_equals_input=%v\n",valid,reconstructed==input)
 if valid||reconstructed==input {panic("recorded starting defect no longer reproduces")}
 fmt.Println("ZXCVBN_ASCII_OK_UNICODE_COORDINATE_DEFECT_REPRODUCED")
}
