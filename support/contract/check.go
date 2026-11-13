package contract
import("testing";"sync";"fmt")
var mu sync.Mutex
var counts=map[string]int{}
func Check(t *testing.T, ok bool){t.Helper();mu.Lock();counts[t.Name()]++;mu.Unlock();if !ok{t.Fatal("contract assertion failed")}}
func Run(t *testing.T,fn func(*testing.T)){t.Helper();fn(t);mu.Lock();n:=counts[t.Name()];mu.Unlock();if n==0{t.Fatal("zero assertions")};fmt.Printf("UNIT_ASSERT %s checks=%d\n",t.Name(),n)}
