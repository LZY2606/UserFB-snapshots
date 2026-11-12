package qframe_test

import (
	"fmt"

	"github.com/tobgu/qframe"
	"github.com/tobgu/qframe/config/newqf"
)

// ExampleNewTemporalRollup shows an offline ETL backfill (补录). Events and
// classification versions both arrive late and are corrected through Apply;
// the rollup keeps living association and aggregate state and reports the net
// differential against the previous snapshot instead of recomputing history.
func ExampleNewTemporalRollup() {
	facts := qframe.New(map[string]interface{}{
		"id":     []int{1, 2},
		"key":    []string{"k", "k"},
		"at":     []int{7, 12},
		"amount": []int{5, 6},
		"entity": []int{9, 9},
	}, newqf.ColumnOrder("id", "key", "at", "amount", "entity"))

	versions := qframe.New(map[string]interface{}{
		"key":   []string{"k", "k"},
		"from":  []int{0, 10},
		"label": []string{"A", "B"},
	}, newqf.ColumnOrder("key", "from", "label"))

	rollup, err := qframe.NewTemporalRollup(facts, versions)
	if err != nil {
		panic(err)
	}
	printSnapshot(rollup.Snapshot())

	// A late-arriving correction: classification B is backfilled to take
	// effect at time 5, reclassifying the event at time 7 without touching
	// the event already inside the later B interval.
	backfilledVersions := qframe.New(map[string]interface{}{
		"key":   []string{"k"},
		"from":  []int{5},
		"label": []string{"B"},
	}, newqf.ColumnOrder("key", "from", "label"))

	diff, err := rollup.Apply(emptyFactFrame(), backfilledVersions, nil, nil)
	if err != nil {
		panic(err)
	}
	printDifferential(diff)
	printSnapshot(rollup.Snapshot())

	// Output:
	// A: rows=1 sum=5 entities=1
	// B: rows=1 sum=6 entities=1
	// A: d_rows=-1 d_sum=-5 d_entities=-1
	// B: d_rows=1 d_sum=5 d_entities=0
	// B: rows=2 sum=11 entities=1
}

func exampleLabel(qf qframe.QFrame, i int) string {
	if p := qf.MustStringView("label").ItemAt(i); p != nil {
		return *p
	}
	return "NULL"
}

func printSnapshot(qf qframe.QFrame) {
	rows := qf.MustIntView("rows")
	sums := qf.MustIntView("sum")
	entities := qf.MustIntView("entities")
	for i := 0; i < qf.Len(); i++ {
		fmt.Printf("%s: rows=%d sum=%d entities=%d\n",
			exampleLabel(qf, i), rows.ItemAt(i), sums.ItemAt(i), entities.ItemAt(i))
	}
}

func printDifferential(qf qframe.QFrame) {
	dRows := qf.MustIntView("d_rows")
	dSum := qf.MustIntView("d_sum")
	dEntities := qf.MustIntView("d_entities")
	for i := 0; i < qf.Len(); i++ {
		fmt.Printf("%s: d_rows=%d d_sum=%d d_entities=%d\n",
			exampleLabel(qf, i), dRows.ItemAt(i), dSum.ItemAt(i), dEntities.ItemAt(i))
	}
}
