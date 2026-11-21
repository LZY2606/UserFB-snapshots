package render
import("math/rand";"github.com/hunterloftis/pbr/pkg/geom";"github.com/hunterloftis/pbr/pkg/rgb")
func SeedProductionTrace(s *Scene,r *geom.Ray,seed int64,depth int,direct bool) rgb.Energy {t:=newTracer(s,nil,1,1,depth,direct);t.rnd=rand.New(rand.NewSource(seed));return t.trace(r,depth)}
