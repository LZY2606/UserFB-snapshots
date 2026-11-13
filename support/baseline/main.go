package main
import("fmt";"bytes";"encoding/json";g "github.com/twpayne/go-geom";"github.com/twpayne/go-geom/encoding/wkt";"github.com/twpayne/go-geom/encoding/geojson")
func main(){
 a,e:=wkt.Unmarshal("POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0), (1 1, 1 2, 2 2, 2 1, 1 1))");if e!=nil{panic(e)}
 p:=a.(*g.Polygon);if p.NumLinearRings()!=2||p.Ends()[0]!=10||p.Ends()[1]!=20{panic("ring ends")}
 b,e:=geojson.Marshal(a);if e!=nil{panic(e)};if !bytes.Contains(b,[]byte("Polygon"))||!json.Valid(b){panic("json")}
 var c g.T;e=geojson.Unmarshal(b,&c);if e!=nil{panic(e)};s,e:=wkt.Marshal(c);if e!=nil||s!="POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0), (1 1, 1 2, 2 2, 2 1, 1 1))"{panic(s)}
 fmt.Println("GEOM_BASELINE polygon_rings=2 ends=10,20 geojson_wkt_roundtrip=ok")
}
