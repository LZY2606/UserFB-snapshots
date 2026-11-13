package topojson_test

import (
	"encoding/json"
	"fmt"
	"log"

	geom "github.com/twpayne/go-geom"
	"github.com/twpayne/go-geom/encoding/topojson"
	"github.com/twpayne/go-geom/encoding/wkt"
)

// ExampleMarshal reads administrative boundaries from WKT and encodes a group
// of them as one TopoJSON topology with a shared arc table. The line starts on
// the polygon's right edge, so the shared boundary is split at the junction
// instead of being stored twice.
func ExampleMarshal() {
	polygon, err := wkt.Unmarshal("POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))")
	if err != nil {
		log.Fatal(err)
	}
	line, err := wkt.Unmarshal("LINESTRING (4 2, 8 2)")
	if err != nil {
		log.Fatal(err)
	}

	data, err := topojson.Marshal(map[string]geom.T{
		"district": polygon,
		"road":     line,
	}, 5)
	if err != nil {
		log.Fatal(err)
	}

	var top struct {
		Type    string `json:"type"`
		Objects map[string]struct {
			Type string `json:"type"`
		} `json:"objects"`
		Arcs [][][2]int64 `json:"arcs"`
	}
	if err := json.Unmarshal(data, &top); err != nil {
		log.Fatal(err)
	}

	fmt.Println(top.Type)
	fmt.Println(top.Objects["district"].Type, top.Objects["road"].Type)
	fmt.Println("arcs:", len(top.Arcs))
	// Output:
	// Topology
	// Polygon LineString
	// arcs: 2
}
