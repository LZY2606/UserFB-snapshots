package main
import("fmt";"os";"strings";"unicode/utf8";"github.com/nbutton23/zxcvbn-go";"github.com/nbutton23/zxcvbn-go/match")
func main(){
 input:="🙂abc";r:=zxcvbn.PasswordStrength(input,[]string{"abc"},func(match.Matcher)bool{return true});parts:=[]string{}
 for _,m:=range r.MatchSequence{if !utf8.ValidString(m.Token){fmt.Println("FAIL: token splits UTF-8");os.Exit(1)};parts=append(parts,m.Token)}
 if strings.Join(parts,"")!=input||r.Entropy!=5.883{fmt.Printf("FAIL: reconstruction or entropy %.3f\n",r.Entropy);os.Exit(2)}
 fmt.Println("ZXCVBN_UNICODE_MINIMAL_REPRO_FIXED")
}
