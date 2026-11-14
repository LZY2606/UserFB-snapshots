package pt
import("testing";"os";"fmt";"strings";"sync")
var targetMutex sync.Mutex
var targetCounts=map[string]int{}
func targetCheck(t *testing.T, ok bool, args ...interface{}) { t.Helper(); n:=strings.Split(t.Name(),"/")[0]; targetMutex.Lock(); targetCounts[n]++; targetMutex.Unlock(); if !ok { t.Fatal(args...) } }
func TestMain(m *testing.M){ code:=m.Run(); if os.Getenv("CHECK_TARGET")=="1" { for _,n:=range []string{"TestSpectralSensor","TestSpectralMedia","TestSpectralTransport"} { if targetCounts[n]==0 {fmt.Println("NO_ASSERTIONS",n);os.Exit(26)};fmt.Println("GUARD",n,targetCounts[n]) };if code==0 {fmt.Println("GUARD_COMPLETE")} };os.Exit(code) }
