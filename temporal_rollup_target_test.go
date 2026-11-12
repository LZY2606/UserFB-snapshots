package qframe_test

import (
	"fmt"
	"math"
	"reflect"
	"sort"
	"testing"

	"github.com/tobgu/qframe"
	"github.com/tobgu/qframe/config/newqf"
	"github.com/tobgu/qframe/types"
)

// This file contains the four required top-level temporal rollup tests.
// Alongside them it defines a small, deliberately naive full-scan temporal
// association used as an independent reference. The reference never touches
// the production TemporalRollup: it keeps plain Go maps and rescans every
// stored fact on every batch.

// ---------- Plain records shared with the naive reference ----------

type plainFact struct {
	id     int
	key    string
	at     int
	amount int
	entity int
}

type plainVersion struct {
	key   string
	from  int
	label string
}

func strPtr(s string) *string { return &s }

// ---------- QFrame builders ----------

func emptyFactFrame() qframe.QFrame {
	return qframe.New(map[string]interface{}{
		"id":     []int{},
		"key":    []*string{},
		"at":     []int{},
		"amount": []int{},
		"entity": []int{},
	}, newqf.ColumnOrder("id", "key", "at", "amount", "entity"))
}

func emptyVersionFrame() qframe.QFrame {
	return qframe.New(map[string]interface{}{
		"key":   []*string{},
		"from":  []int{},
		"label": []*string{},
	}, newqf.ColumnOrder("key", "from", "label"))
}

func factFrame(recs []plainFact, keyEnum bool, extra bool) qframe.QFrame {
	ids := make([]int, len(recs))
	keys := make([]*string, len(recs))
	ats := make([]int, len(recs))
	amounts := make([]int, len(recs))
	entities := make([]int, len(recs))
	for i, r := range recs {
		ids[i], ats[i], amounts[i], entities[i] = r.id, r.at, r.amount, r.entity
		k := r.key
		keys[i] = &k
	}
	data := map[string]interface{}{
		"id": ids, "key": keys, "at": ats, "amount": amounts, "entity": entities,
	}
	order := []string{"id", "key", "at", "amount", "entity"}
	if extra {
		data["note"] = make([]int, len(recs))
		order = append(order, "note")
	}
	fns := []newqf.ConfigFunc{newqf.ColumnOrder(order...)}
	if keyEnum {
		fns = append(fns, newqf.Enums(map[string][]string{"key": nil}))
	}
	return qframe.New(data, fns...)
}

// versionFrame builds a version frame. A nil entry in labels produces a NULL.
func versionFrame(recs []plainVersion, labels []*string, enumCols map[string][]string) qframe.QFrame {
	keys := make([]*string, len(recs))
	froms := make([]int, len(recs))
	for i, r := range recs {
		k := r.key
		keys[i] = &k
		froms[i] = r.from
	}
	data := map[string]interface{}{"key": keys, "from": froms, "label": labels}
	fns := []newqf.ConfigFunc{newqf.ColumnOrder("key", "from", "label")}
	if len(enumCols) > 0 {
		fns = append(fns, newqf.Enums(enumCols))
	}
	return qframe.New(data, fns...)
}

func versionLabels(recs []plainVersion) []*string {
	out := make([]*string, len(recs))
	for i, r := range recs {
		s := r.label
		out[i] = &s
	}
	return out
}

func factsFromFrame(qf qframe.QFrame) []plainFact {
	n := qf.Len()
	idV := qf.MustIntView("id")
	atV := qf.MustIntView("at")
	amountV := qf.MustIntView("amount")
	entityV := qf.MustIntView("entity")
	keyV := qf.MustStringView("key")
	enumKey, err := qf.EnumView("key")
	recs := make([]plainFact, n)
	for i := 0; i < n; i++ {
		var key string
		if err == nil {
			key = *enumKey.ItemAt(i)
		} else {
			key = *keyV.ItemAt(i)
		}
		recs[i] = plainFact{
			id: idV.ItemAt(i), key: key, at: atV.ItemAt(i),
			amount: amountV.ItemAt(i), entity: entityV.ItemAt(i),
		}
	}
	return recs
}

// ---------- Naive full-scan reference ----------

type naiveKey struct {
	key  string
	from int
}

type naiveRollup struct {
	facts       map[int]plainFact
	versions    map[naiveKey]string
	sortedByKey map[string][]plainVersion // rebuilt from versions on every batch
}

func (n *naiveRollup) rebuildIndex() {
	n.sortedByKey = map[string][]plainVersion{}
	for vk, label := range n.versions {
		n.sortedByKey[vk.key] = append(n.sortedByKey[vk.key], plainVersion{key: vk.key, from: vk.from, label: label})
	}
	for key := range n.sortedByKey {
		sort.Slice(n.sortedByKey[key], func(a, b int) bool {
			return n.sortedByKey[key][a].from < n.sortedByKey[key][b].from
		})
	}
}

func newNaiveRollup(facts []plainFact, versions []plainVersion) *naiveRollup {
	n := &naiveRollup{
		facts:    map[int]plainFact{},
		versions: map[naiveKey]string{},
	}
	for _, v := range versions {
		n.versions[naiveKey{v.key, v.from}] = v.label
	}
	for _, f := range facts {
		n.facts[f.id] = f
	}
	n.rebuildIndex()
	return n
}

// classify returns the label and whether a version matched. A false result
// means the single NULL (unmatched) group. It rescans the whole version set
// of the key and binary-searches the predecessor; the reference keeps no
// incremental association state between batches.
func (n *naiveRollup) classify(key string, at int) (string, bool) {
	candidates := n.sortedByKey[key]
	i := sort.Search(len(candidates), func(i int) bool { return candidates[i].from > at })
	if i == 0 {
		return "", false
	}
	return candidates[i-1].label, true
}

type naiveRow struct {
	label          *string
	rows, sum, ent int
}

// groupKey identifies a label group by value; the unmatched NULL group is
// represented by isNull=true so the empty string label stays distinct.
type groupKey struct {
	isNull bool
	label  string
}

func keyFor(label *string) groupKey {
	if label == nil {
		return groupKey{isNull: true}
	}
	return groupKey{label: *label}
}

// snapshot recomputes the whole rollup from scratch, the expensive reference
// behaviour the production index is meant to avoid.
func (n *naiveRollup) snapshot() []naiveRow {
	type agg struct {
		rows, sum int
		entities  map[int]struct{}
	}
	groups := map[groupKey]*agg{}
	for _, f := range n.facts {
		label, matched := n.classify(f.key, f.at)
		var key groupKey
		if matched {
			key = groupKey{label: label}
		} else {
			key = groupKey{isNull: true}
		}
		g := groups[key]
		if g == nil {
			g = &agg{entities: map[int]struct{}{}}
			groups[key] = g
		}
		g.rows++
		g.sum += f.amount
		g.entities[f.entity] = struct{}{}
	}
	out := make([]naiveRow, 0, len(groups))
	for key, g := range groups {
		var lp *string
		if !key.isNull {
			s := key.label
			lp = &s
		}
		out = append(out, naiveRow{label: lp, rows: g.rows, sum: g.sum, ent: len(g.entities)})
	}
	sortNaiveRows(out)
	return out
}

func sortNaiveRows(rows []naiveRow) {
	sort.SliceStable(rows, func(a, b int) bool {
		la, lb := rows[a].label, rows[b].label
		if la == nil || lb == nil {
			return la == nil && lb != nil
		}
		return *la < *lb
	})
}

// apply mirrors the production batch semantics (deletes first, then upserts,
// duplicate deletes merged) and returns the post snapshot and the net diff.
func (n *naiveRollup) apply(facts []plainFact, versions []plainVersion, factDel []int, versionDel []qframe.VersionKey) ([]naiveRow, []naiveRow) {
	before := n.snapshot()

	seenF := map[int]struct{}{}
	for _, id := range factDel {
		if _, done := seenF[id]; done {
			continue
		}
		seenF[id] = struct{}{}
		delete(n.facts, id)
	}
	seenV := map[naiveKey]struct{}{}
	for _, dk := range versionDel {
		k := naiveKey{dk.Key, dk.From}
		if _, done := seenV[k]; done {
			continue
		}
		seenV[k] = struct{}{}
		delete(n.versions, k)
	}
	for _, v := range versions {
		n.versions[naiveKey{v.key, v.from}] = v.label
	}
	for _, f := range facts {
		n.facts[f.id] = f
	}
	n.rebuildIndex()

	after := n.snapshot()
	diff := diffNaive(before, after)
	return after, diff
}

func diffNaive(before, after []naiveRow) []naiveRow {
	byLabel := map[groupKey][2]naiveRow{}
	for _, r := range before {
		byLabel[keyFor(r.label)] = [2]naiveRow{r, {}}
	}
	for _, r := range after {
		pair := byLabel[keyFor(r.label)]
		pair[1] = r
		byLabel[keyFor(r.label)] = pair
	}
	out := make([]naiveRow, 0)
	for key, pair := range byLabel {
		dr := pair[1].rows - pair[0].rows
		ds := pair[1].sum - pair[0].sum
		de := pair[1].ent - pair[0].ent
		if dr == 0 && ds == 0 && de == 0 {
			continue
		}
		var lp *string
		if !key.isNull {
			s := key.label
			lp = &s
		}
		out = append(out, naiveRow{label: lp, rows: dr, sum: ds, ent: de})
	}
	sortNaiveRows(out)
	return out
}

// ---------- Frame comparisons ----------

type gotRow struct {
	label          *string
	rows, sum, ent int
}

func readRows(qf qframe.QFrame) []gotRow {
	if qf.Err != nil {
		panic(qf.Err)
	}
	n := qf.Len()
	labelV := qf.MustStringView("label")
	rowsV := qf.MustIntView("rows")
	sumV := qf.MustIntView("sum")
	entV := qf.MustIntView("entities")
	out := make([]gotRow, n)
	for i := 0; i < n; i++ {
		if p := labelV.ItemAt(i); p != nil {
			s := *p
			out[i].label = &s
		}
		out[i].rows = rowsV.ItemAt(i)
		out[i].sum = sumV.ItemAt(i)
		out[i].ent = entV.ItemAt(i)
	}
	return out
}

func readDiffRows(qf qframe.QFrame) []gotRow {
	if qf.Err != nil {
		panic(qf.Err)
	}
	n := qf.Len()
	labelV := qf.MustStringView("label")
	r1 := qf.MustIntView("d_rows")
	r2 := qf.MustIntView("d_sum")
	r3 := qf.MustIntView("d_entities")
	out := make([]gotRow, n)
	for i := 0; i < n; i++ {
		if p := labelV.ItemAt(i); p != nil {
			s := *p
			out[i].label = &s
		}
		out[i].rows = r1.ItemAt(i)
		out[i].sum = r2.ItemAt(i)
		out[i].ent = r3.ItemAt(i)
	}
	return out
}

func sameLabel(a, b *string) bool {
	if a == nil || b == nil {
		return a == nil && b == nil
	}
	return *a == *b
}

func assertRows(t *testing.T, name string, got []gotRow, want []naiveRow) {
	t.Helper()
	if len(got) != len(want) {
		t.Fatalf("%s: row count mismatch: got %d want %d\ngot=%v\nwant=%v", name, len(got), len(want), got, want)
	}
	for i := range got {
		if !sameLabel(got[i].label, want[i].label) ||
			got[i].rows != want[i].rows || got[i].sum != want[i].sum || got[i].ent != want[i].ent {
			t.Fatalf("%s: row %d mismatch: got %v want %v", name, i, got[i], want[i])
		}
	}
}

func assertGroupSchema(t *testing.T, qf qframe.QFrame, intCols [3]string) {
	t.Helper()
	if qf.Err != nil {
		t.Fatalf("unexpected frame error: %v", qf.Err)
	}
	wantNames := []string{"label", intCols[0], intCols[1], intCols[2]}
	if !reflect.DeepEqual(qf.ColumnNames(), wantNames) {
		t.Fatalf("column names = %v, want %v", qf.ColumnNames(), wantNames)
	}
	wantTypes := []types.DataType{types.String, types.Int, types.Int, types.Int}
	if !reflect.DeepEqual(qf.ColumnTypes(), wantTypes) {
		t.Fatalf("column types = %v, want %v", qf.ColumnTypes(), wantTypes)
	}
}

// ---------- Test 1: timeline association boundaries ----------

func TestTemporalTimeline(t *testing.T) {
	t.Run("left-closed right-open intervals", func(t *testing.T) {
		facts := []plainFact{
			{1, "k", 0, 10, 7},
			{2, "k", 9, 20, 7},
			{3, "k", 10, 30, 8},
			{4, "k", 19, 40, 8},
			{5, "k", 20, 50, 9},
		}
		versions := []plainVersion{{"k", 0, "A"}, {"k", 10, "B"}, {"k", 20, "C"}}
		rollup, err := qframe.NewTemporalRollup(factFrame(facts, false, false), versionFrame(versions, versionLabels(versions), nil))
		if err != nil {
			t.Fatal(err)
		}
		naive := newNaiveRollup(facts, versions)
		assertRows(t, "snapshot", readRows(rollup.Snapshot()), naive.snapshot())
	})

	t.Run("negative min and max event times", func(t *testing.T) {
		facts := []plainFact{
			{1, "k", math.MinInt, 1, 1},
			{2, "k", -1, 2, 1},
			{3, "k", math.MaxInt, 4, 2},
		}
		versions := []plainVersion{{"k", math.MinInt, "early"}, {"k", 0, "late"}}
		rollup, err := qframe.NewTemporalRollup(factFrame(facts, false, false), versionFrame(versions, versionLabels(versions), nil))
		if err != nil {
			t.Fatal(err)
		}
		naive := newNaiveRollup(facts, versions)
		assertRows(t, "extremes", readRows(rollup.Snapshot()), naive.snapshot())

		// A version at MaxInt must still govern an event at MaxInt (no from+1).
		up := []plainVersion{{"k", math.MaxInt, "edge"}}
		diff, err := rollup.Apply(emptyFactFrame(), versionFrame(up, versionLabels(up), nil), nil, nil)
		if err != nil {
			t.Fatal(err)
		}
		naive.apply(nil, up, nil, nil)
		assertRows(t, "edge diff", readDiffRows(diff), diffNaive(newNaiveRollup(facts, versions).snapshot(), naive.snapshot()))
		assertRows(t, "edge snapshot", readRows(rollup.Snapshot()), naive.snapshot())
	})

	t.Run("unmatched null differs from empty label", func(t *testing.T) {
		facts := []plainFact{
			{1, "a", 5, 3, 1},
			{2, "b", 5, 4, 1},
		}
		versions := []plainVersion{{"b", 0, ""}} // empty string label, key a unmatched
		rollup, err := qframe.NewTemporalRollup(factFrame(facts, false, false), versionFrame(versions, versionLabels(versions), nil))
		if err != nil {
			t.Fatal(err)
		}
		naive := newNaiveRollup(facts, versions)
		got := readRows(rollup.Snapshot())
		assertRows(t, "null vs empty", got, naive.snapshot())
		if got[0].label != nil {
			t.Fatalf("first label must be NULL, got %q", *got[0].label)
		}
		if got[1].label == nil || *got[1].label != "" {
			t.Fatalf("second label must be empty string, got %v", got[1].label)
		}
	})

	t.Run("same label across different versions stays one group", func(t *testing.T) {
		facts := []plainFact{{1, "k", 1, 5, 1}, {2, "k", 11, 7, 2}}
		versions := []plainVersion{{"k", 0, "A"}, {"k", 10, "A"}}
		rollup, err := qframe.NewTemporalRollup(factFrame(facts, false, false), versionFrame(versions, versionLabels(versions), nil))
		if err != nil {
			t.Fatal(err)
		}
		naive := newNaiveRollup(facts, versions)
		assertRows(t, "one group", readRows(rollup.Snapshot()), naive.snapshot())
	})

	t.Run("enum keys read by text with independent dictionaries", func(t *testing.T) {
		facts := []plainFact{{1, "x", 1, 5, 1}, {2, "y", 1, 6, 1}}
		versions := []plainVersion{{"x", 0, "X"}, {"y", 0, "Y"}}
		// Facts use an enum dictionary in reverse insertion order, versions
		// use plain strings; text equality must still associate correctly.
		fq := factFrame(facts, true, false)
		vq := versionFrame(versions, versionLabels(versions), nil)
		rollup, err := qframe.NewTemporalRollup(fq, vq)
		if err != nil {
			t.Fatal(err)
		}
		naive := newNaiveRollup(facts, versions)
		assertRows(t, "enum text", readRows(rollup.Snapshot()), naive.snapshot())
	})

	t.Run("filtered sorted sliced and zero-row typed inputs", func(t *testing.T) {
		facts := make([]plainFact, 12)
		for i := range facts {
			facts[i] = plainFact{i, "k", i, i, i % 3}
		}
		versions := []plainVersion{{"k", 0, "A"}, {"k", 6, "B"}}
		full := factFrame(facts, false, true) // extra column must be ignored
		view := full.Filter(qframe.Filter{Column: "at", Comparator: ">", Arg: 2}).
			Sort(qframe.Order{Column: "at", Reverse: true}).
			Slice(1, 8)
		if view.Err != nil {
			t.Fatal(view.Err)
		}
		rollup, err := qframe.NewTemporalRollup(view, versionFrame(versions, versionLabels(versions), nil))
		if err != nil {
			t.Fatal(err)
		}
		kept := factsFromFrame(view)
		naive := newNaiveRollup(kept, versions)
		assertRows(t, "view input", readRows(rollup.Snapshot()), naive.snapshot())

		empty, err := qframe.NewTemporalRollup(emptyFactFrame(), emptyVersionFrame())
		if err != nil {
			t.Fatal(err)
		}
		snap := empty.Snapshot()
		assertGroupSchema(t, snap, [3]string{"rows", "sum", "entities"})
		if snap.Len() != 0 {
			t.Fatalf("empty snapshot must have zero rows, got %d", snap.Len())
		}
	})

	t.Run("randomized version churn keeps the trees correct", func(t *testing.T) {
		// Exercise the AVL trees (including two-child deletions) and the span
		// propagation through many small, random, order-independent batches,
		// comparing the maintained rollup with the full-scan reference.
		rng := &lcg{state: 0xabc123}
		keys := []string{"p", "q", "r"}
		froms := []int{0, 5, 10, 15, 20}
		labels := []string{"L0", "L1", "L2", "L3", "L4"}

		seedFacts := make([]plainFact, 0, 40)
		for i := 0; i < 40; i++ {
			seedFacts = append(seedFacts, plainFact{
				id:     i + 1,
				key:    keys[rng.next()%uint64(len(keys))],
				at:     int(rng.next() % 25),
				amount: int(rng.next()%11) - 5,
				entity: int(rng.next() % 6),
			})
		}
		seedVersions := []plainVersion{
			{keys[0], 0, labels[0]}, {keys[1], 0, labels[1]}, {keys[2], 0, labels[2]},
		}
		rollup, err := qframe.NewTemporalRollup(factFrame(seedFacts, false, false),
			versionFrame(seedVersions, versionLabels(seedVersions), nil))
		if err != nil {
			t.Fatal(err)
		}
		naive := newNaiveRollup(seedFacts, seedVersions)
		assertRows(t, "seed", readRows(rollup.Snapshot()), naive.snapshot())

		nextFactID := 1000
		for batch := 0; batch < 200; batch++ {
			factRecs := []plainFact(nil)
			versionRecs := []plainVersion(nil)
			factDel := []int(nil)
			versionDel := []qframe.VersionKey(nil)
			// Upsert inputs must not contain a repeated identity within a
			// batch; track the generated ones so the random stream stays valid.
			seenVid := map[naiveKey]struct{}{}
			seenFid := map[int]struct{}{}
			addVersion := func(v plainVersion) {
				if _, ok := seenVid[naiveKey{v.key, v.from}]; ok {
					return
				}
				seenVid[naiveKey{v.key, v.from}] = struct{}{}
				versionRecs = append(versionRecs, v)
			}
			addFact := func(f plainFact) {
				if _, ok := seenFid[f.id]; ok {
					return
				}
				seenFid[f.id] = struct{}{}
				factRecs = append(factRecs, f)
			}

			switch batch % 4 {
			case 0: // version inserts/replacements
				for j := 0; j < 1+int(rng.next()%3); j++ {
					addVersion(plainVersion{
						key:   keys[rng.next()%uint64(len(keys))],
						from:  froms[rng.next()%uint64(len(froms))],
						label: labels[rng.next()%uint64(len(labels))],
					})
				}
			case 1: // version deletes (including possibly missing identities)
				for j := 0; j < 1+int(rng.next()%3); j++ {
					versionDel = append(versionDel, qframe.VersionKey{
						Key:  keys[rng.next()%uint64(len(keys))],
						From: froms[rng.next()%uint64(len(froms))],
					})
				}
			case 2: // fact upserts (mix of corrections and inserts)
				for j := 0; j < 1+int(rng.next()%3); j++ {
					id := 1 + int(rng.next()%uint64(len(seedFacts)+8))
					if id > len(seedFacts) {
						id = nextFactID
						nextFactID++
					}
					addFact(plainFact{
						id:     id,
						key:    keys[rng.next()%uint64(len(keys))],
						at:     int(rng.next() % 25),
						amount: int(rng.next()%11) - 5,
						entity: int(rng.next() % 6),
					})
				}
			case 3: // mixed: everything at once, including duplicate deletes
				versionDel = append(versionDel,
					qframe.VersionKey{Key: keys[0], From: froms[rng.next()%uint64(len(froms))]},
					qframe.VersionKey{Key: keys[0], From: froms[rng.next()%uint64(len(froms))]},
				)
				addVersion(plainVersion{
					key: keys[1], from: froms[rng.next()%uint64(len(froms))], label: labels[rng.next()%uint64(len(labels))],
				})
				factDel = append(factDel, 1+int(rng.next()%uint64(len(seedFacts))))
				addFact(plainFact{
					id: nextFactID, key: keys[2], at: int(rng.next() % 25),
					amount: int(rng.next()%11) - 5, entity: int(rng.next() % 6),
				})
				nextFactID++
			}

			beforeNaive := naive.snapshot()
			diff, err := rollup.Apply(factFrame(factRecs, false, false),
				versionFrame(versionRecs, versionLabels(versionRecs), nil), factDel, versionDel)
			if err != nil {
				t.Fatalf("batch %d: unexpected error %v", batch, err)
			}
			afterNaive, _ := naive.apply(factRecs, versionRecs, factDel, versionDel)
			assertRows(t, fmt.Sprintf("batch %d snapshot", batch), readRows(rollup.Snapshot()), afterNaive)
			assertRows(t, fmt.Sprintf("batch %d diff", batch), readDiffRows(diff), diffNaive(beforeNaive, afterNaive))
		}
	})
}

// ---------- Test 2: differential semantics against the naive reference ----------

func TestTemporalDifferential(t *testing.T) {
	// Deterministic splitmix64 stream so the batches are reproducible.
	rng := &lcg{state: 0x1234abcd}
	next := rng.next

	facts := make([]plainFact, 0, 40)
	versions := []plainVersion{
		{"k1", 0, "A"}, {"k1", 10, "B"},
		{"k2", 0, "C"}, {"k2", 10, "D"},
	}
	for i := 0; i < 24; i++ {
		key := []string{"k1", "k2"}[next()%2]
		facts = append(facts, plainFact{
			id:     i + 1,
			key:    key,
			at:     int(next() % 25),
			amount: int(next()%21) - 10,
			entity: int(next() % 5),
		})
	}

	production, err := qframe.NewTemporalRollup(factFrame(facts, false, false), versionFrame(versions, versionLabels(versions), nil))
	if err != nil {
		t.Fatal(err)
	}
	naive := newNaiveRollup(facts, versions)
	assertRows(t, "initial", readRows(production.Snapshot()), naive.snapshot())

	nextID := 100
	runBatch := func(batch int, factRecs []plainFact, versionRecs []plainVersion, factDel []int, versionDel []qframe.VersionKey, enumInput bool) {
		t.Helper()
		beforeNaive := naive.snapshot()
		var vq qframe.QFrame
		if enumInput {
			vq = versionFrame(versionRecs, versionLabels(versionRecs), map[string][]string{"label": nil})
		} else {
			vq = versionFrame(versionRecs, versionLabels(versionRecs), nil)
		}
		// Shuffle the fact input order: results must be order independent.
		shuffled := append([]plainFact(nil), factRecs...)
		for i := len(shuffled) - 1; i > 0; i-- {
			j := int(next() % uint64(i+1))
			shuffled[i], shuffled[j] = shuffled[j], shuffled[i]
		}
		diff, err := production.Apply(factFrame(shuffled, enumInput, false), vq, factDel, versionDel)
		if err != nil {
			t.Fatalf("batch %d: unexpected error: %v", batch, err)
		}
		afterNaive, _ := naive.apply(factRecs, versionRecs, factDel, versionDel)
		assertRows(t, fmt.Sprintf("batch %d snapshot", batch), readRows(production.Snapshot()), afterNaive)
		assertRows(t, fmt.Sprintf("batch %d diff", batch), readDiffRows(diff), diffNaive(beforeNaive, afterNaive))
		assertGroupSchema(t, diff, [3]string{"d_rows", "d_sum", "d_entities"})
	}

	// Batch 1: overlapping fact and version corrections on the same timeline.
	runBatch(1,
		[]plainFact{{1, "k1", 12, 100, 1}, {2, "k2", 3, -7, 2}},
		[]plainVersion{{"k1", 5, "B"}, {"k2", 10, "C"}},
		nil, nil, false)

	// Batch 2: unmatched -> matched and matched -> unmatched together.
	runBatch(2,
		[]plainFact{{3, "newKey", 4, 9, 3}, {4, "k1", 50, 2, 4}},
		[]plainVersion{{"newKey", 0, "Z"}},
		nil,
		[]qframe.VersionKey{{Key: "k1", From: 10}},
		true)

	// Batch 3: same-label version replacement must net to zero noise.
	runBatch(3,
		nil,
		[]plainVersion{{"k2", 0, "C"}}, // identical re-upsert
		nil, nil, false)

	// Batch 4: delete then upsert the same fact identity in one batch.
	runBatch(4,
		[]plainFact{{5, "k1", 7, 11, 1}},
		nil, []int{5}, nil, false)

	// Batch 5: last copy of an entity leaves a group.
	runBatch(5, nil, nil, []int{3}, nil, false)

	// Batch 6: duplicate deletes merge, deleting missing identities is a no-op.
	runBatch(6,
		[]plainFact{{6, "k2", 11, 4, 2}},
		nil,
		[]int{6, 6, 99999}, []qframe.VersionKey{{Key: "ghost", From: 0}, {Key: "ghost", From: 0}},
		false)

	// Batch 7: out-and-back fact (change then change back within one batch is
	// not possible via upsert semantics, so emulate migrate out and the
	// version sweep pulling it straight back to its original group).
	runBatch(7,
		[]plainFact{{7, "k1", 2, 8, 4}},
		[]plainVersion{{"k1", 2, "A"}}, // fact 7 stays under label A, no noise
		nil, nil, false)

	// Batch 8: pure inserts with negative amounts and repeated entities.
	inserts := []plainFact{
		{nextID, "k1", 1, -5, 1},
		{nextID + 1, "k1", 2, -5, 1},
		{nextID + 2, "k2", 20, 3, 2},
	}
	nextID += 3
	runBatch(8, inserts, nil, nil, nil, true)

	// Batch 9: wide version delete that hands an interval back to its predecessor.
	runBatch(9,
		nil, nil, nil,
		[]qframe.VersionKey{{Key: "k1", From: 5}},
		false)

	// Batch 10: several independent corrections at once, including a brand new key.
	runBatch(10,
		[]plainFact{{8, "k2", 0, 1, 0}, {nextID, "brand", math.MinInt, 2, 0}},
		[]plainVersion{{"brand", math.MinInt, "N"}, {"k2", 15, "E"}},
		[]int{9}, nil, false)
	nextID++

	// Batch 11: delete a non-existent version and upsert the same identity in
	// one batch. The delete is a no-op but the upsert must still dominate the
	// interval it now owns.
	runBatch(11,
		nil,
		[]plainVersion{{"k1", 30, "LATE"}},
		nil,
		[]qframe.VersionKey{{Key: "k1", From: 30}},
		false)

	t.Run("zero sum group is retained", func(t *testing.T) {
		facts := []plainFact{{1, "k", 1, 5, 1}, {2, "k", 2, -5, 2}}
		versions := []plainVersion{{"k", 0, "Z"}}
		r, err := qframe.NewTemporalRollup(factFrame(facts, false, false), versionFrame(versions, versionLabels(versions), nil))
		if err != nil {
			t.Fatal(err)
		}
		got := readRows(r.Snapshot())
		want := newNaiveRollup(facts, versions).snapshot()
		assertRows(t, "zero sum", got, want)
		if len(got) != 1 || got[0].rows != 2 || got[0].sum != 0 || got[0].ent != 2 {
			t.Fatalf("zero-sum group must be retained with both entities, got %v", got)
		}
	})

	t.Run("same label version replacement makes no noise", func(t *testing.T) {
		facts := []plainFact{{1, "k", 5, 3, 1}, {2, "k", 15, 4, 1}}
		versions := []plainVersion{{"k", 0, "A"}, {"k", 10, "A"}}
		r, err := qframe.NewTemporalRollup(factFrame(facts, false, false), versionFrame(versions, versionLabels(versions), nil))
		if err != nil {
			t.Fatal(err)
		}
		// Re-upsert both versions with the same labels: no group changes.
		diff, err := r.Apply(emptyFactFrame(), versionFrame(versions, versionLabels(versions), nil), nil, nil)
		if err != nil {
			t.Fatal(err)
		}
		if diff.Len() != 0 {
			t.Fatalf("identical re-upsert must yield an empty diff, got %v", readDiffRows(diff))
		}
		assertRows(t, "unchanged", readRows(r.Snapshot()), newNaiveRollup(facts, versions).snapshot())
	})

	t.Run("entity count drops only when the last copy leaves", func(t *testing.T) {
		// Two facts of the same entity in group A, one in group B.
		facts := []plainFact{
			{1, "k", 1, 3, 7},
			{2, "k", 2, 4, 7},
			{3, "k", 12, 5, 7},
		}
		versions := []plainVersion{{"k", 0, "A"}, {"k", 10, "B"}}
		r, err := qframe.NewTemporalRollup(factFrame(facts, false, false), versionFrame(versions, versionLabels(versions), nil))
		if err != nil {
			t.Fatal(err)
		}
		naive := newNaiveRollup(facts, versions)

		// Remove one of the two A copies: A keeps entity 7 (one copy remains).
		beforeNaive := naive.snapshot()
		diff, err := r.Apply(emptyFactFrame(), emptyVersionFrame(), []int{1}, nil)
		if err != nil {
			t.Fatal(err)
		}
		after, _ := naive.apply(nil, nil, []int{1}, nil)
		assertRows(t, "first copy", readRows(r.Snapshot()), after)
		assertRows(t, "first copy diff", readDiffRows(diff), diffNaive(beforeNaive, after))
		aRow := findRow(readRows(r.Snapshot()), strPtr("A"))
		if aRow == nil || aRow.ent != 1 {
			t.Fatalf("group A must still count entity 7, got %v", aRow)
		}

		// Remove the last A copy: now entity 7 disappears from A.
		beforeNaive = after
		diff, err = r.Apply(emptyFactFrame(), emptyVersionFrame(), []int{2}, nil)
		if err != nil {
			t.Fatal(err)
		}
		after, _ = naive.apply(nil, nil, []int{2}, nil)
		assertRows(t, "last copy", readRows(r.Snapshot()), after)
		assertRows(t, "last copy diff", readDiffRows(diff), diffNaive(beforeNaive, after))
		if row := findRow(readRows(r.Snapshot()), strPtr("A")); row != nil {
			t.Fatalf("group A should be empty and therefore absent, got %v", row)
		}
	})

	t.Run("joint fact and version correction withdraws and adds once", func(t *testing.T) {
		// One fact under A; in the same batch it changes time and the governing
		// versions change, so old and new timelines both differ. The fact must
		// be withdrawn once from its old group and added once to the final one.
		facts := []plainFact{{1, "k", 5, 9, 1}}
		versions := []plainVersion{{"k", 0, "A"}, {"k", 10, "B"}}
		r, err := qframe.NewTemporalRollup(factFrame(facts, false, false), versionFrame(versions, versionLabels(versions), nil))
		if err != nil {
			t.Fatal(err)
		}
		naive := newNaiveRollup(facts, versions)

		// Move the fact to t=15 and insert B@5 (so t=15 stays B) while
		// relabeling the early interval to "C". Every event still resolves.
		newFacts := []plainFact{{1, "k", 15, 9, 1}}
		newVersions := []plainVersion{{"k", 5, "B"}, {"k", 0, "C"}}
		before := naive.snapshot()
		diff, err := r.Apply(factFrame(newFacts, false, false), versionFrame(newVersions, versionLabels(newVersions), nil), nil, nil)
		if err != nil {
			t.Fatal(err)
		}
		after, _ := naive.apply(newFacts, newVersions, nil, nil)
		assertRows(t, "joint snapshot", readRows(r.Snapshot()), after)
		assertRows(t, "joint diff", readDiffRows(diff), diffNaive(before, after))

		// Exactly one involved stored fact: one withdrawal and one addition.
		s := r.Stats()
		if s.FactVisits != 1 {
			t.Fatalf("expected one fact visit, got %d", s.FactVisits)
		}
		if s.AggregateOps != 2 {
			t.Fatalf("expected exactly one withdrawal and one addition (2 ops), got %d", s.AggregateOps)
		}
	})

	t.Run("fact changing key at and entity contributes once on each side", func(t *testing.T) {
		// The fact starts under key k1/A and is corrected in one batch to a
		// different key, time, amount and entity. Its old contribution must be
		// withdrawn exactly once even though every identifying field changed.
		facts := []plainFact{{1, "k1", 5, 9, 1}}
		versions := []plainVersion{
			{"k1", 0, "A"},
			{"k2", 0, "C"},
		}
		r, err := qframe.NewTemporalRollup(factFrame(facts, false, false), versionFrame(versions, versionLabels(versions), nil))
		if err != nil {
			t.Fatal(err)
		}
		naive := newNaiveRollup(facts, versions)

		corrected := []plainFact{{1, "k2", 5, -3, 42}}
		before := naive.snapshot()
		diff, err := r.Apply(factFrame(corrected, false, false), emptyVersionFrame(), nil, nil)
		if err != nil {
			t.Fatal(err)
		}
		after, _ := naive.apply(corrected, nil, nil, nil)
		assertRows(t, "moved snapshot", readRows(r.Snapshot()), after)
		assertRows(t, "moved diff", readDiffRows(diff), diffNaive(before, after))
		s := r.Stats()
		if s.FactVisits != 1 || s.AggregateOps != 2 {
			t.Fatalf("one moved fact: visits=%d ops=%d, want 1 and 2", s.FactVisits, s.AggregateOps)
		}
		if row := findRow(readRows(r.Snapshot()), strPtr("C")); row == nil || row.ent != 1 || row.sum != -3 {
			t.Fatalf("final C group must hold the moved contribution, got %v", row)
		}
	})
}

func findRow(rows []gotRow, label *string) *gotRow {
	for i := range rows {
		if sameLabel(rows[i].label, label) {
			return &rows[i]
		}
	}
	return nil
}

// ---------- Test 3: atomicity, views and immutability ----------

func TestTemporalAtomicViews(t *testing.T) {
	goodFacts := []plainFact{{1, "k", 1, 5, 7}, {2, "k", 11, 6, 7}}
	goodVersions := []plainVersion{{"k", 0, "A"}, {"k", 10, "B"}}

	t.Run("constructor validation failures return no object", func(t *testing.T) {
		cases := map[string]struct {
			facts qframe.QFrame
			vers  qframe.QFrame
		}{
			"missing fact column": {
				qframe.New(map[string]interface{}{"id": []int{1}, "key": []string{"k"}, "at": []int{1}, "amount": []int{5}},
					newqf.ColumnOrder("id", "key", "at", "amount")),
				emptyVersionFrame(),
			},
			"wrong fact column type": {
				qframe.New(map[string]interface{}{
					"id": []string{"x"}, "key": []string{"k"}, "at": []int{1}, "amount": []int{5}, "entity": []int{7},
				}, newqf.ColumnOrder("id", "key", "at", "amount", "entity")),
				emptyVersionFrame(),
			},
			"null fact key": {
				factFrameWithNullKey(),
				emptyVersionFrame(),
			},
			"null version label": {
				emptyFactFrame(),
				versionFrame([]plainVersion{{"k", 0, "A"}}, []*string{nil}, nil),
			},
			"duplicate version identity": {
				emptyFactFrame(),
				versionFrame([]plainVersion{{"k", 0, "A"}, {"k", 0, "B"}},
					[]*string{strPtr("A"), strPtr("B")}, nil),
			},
			"duplicate fact id": {
				factFrame([]plainFact{{1, "k", 1, 1, 1}, {1, "k", 2, 1, 1}}, false, false),
				emptyVersionFrame(),
			},
		}
		for name, c := range cases {
			r, err := qframe.NewTemporalRollup(c.facts, c.vers)
			if err == nil {
				t.Errorf("%s: expected error, got rollup %v", name, r != nil)
			}
			if r != nil {
				t.Errorf("%s: constructor failure must not return a usable object", name)
			}
		}
	})

	t.Run("failed apply keeps state and index reusable", func(t *testing.T) {
		r, err := qframe.NewTemporalRollup(factFrame(goodFacts, false, false), versionFrame(goodVersions, versionLabels(goodVersions), nil))
		if err != nil {
			t.Fatal(err)
		}
		naive := newNaiveRollup(goodFacts, goodVersions)
		want := naive.snapshot()
		before := readRows(r.Snapshot())
		assertRows(t, "before", before, want)

		// A successful batch first, so the later reset of a failed Apply is
		// actually observable against non-zero counters.
		seedFacts := []plainFact{{3, "k", 15, 2, 5}}
		if _, err = r.Apply(factFrame(seedFacts, false, false), emptyVersionFrame(), nil, nil); err != nil {
			t.Fatal(err)
		}
		seedNaive, _ := naive.apply(seedFacts, nil, nil, nil)
		if s := r.Stats(); s.FactVisits == 0 && s.IndexSteps == 0 && s.AggregateOps == 0 {
			t.Fatalf("successful batch must leave non-zero counters, got %+v", s)
		}
		want = seedNaive

		// Frame already carrying an Err.
		errFrame := factFrame(goodFacts, false, false).
			Filter(qframe.Filter{Column: "missing", Comparator: "=", Arg: 1})
		if errFrame.Err == nil {
			t.Fatal("test setup: expected errored frame")
		}
		d, err := r.Apply(errFrame, emptyVersionFrame(), nil, nil)
		if err == nil || d.Err == nil {
			t.Fatal("expected errored Apply result")
		}
		// Duplicate upsert identity.
		dup := factFrame([]plainFact{{3, "k", 1, 1, 1}, {3, "k", 2, 1, 1}}, false, false)
		if d, err = r.Apply(dup, emptyVersionFrame(), nil, nil); err == nil || d.Err == nil {
			t.Fatal("expected duplicate id error")
		}
		// NULL key in upsert.
		if d, err = r.Apply(factFrameWithNullKey(), emptyVersionFrame(), nil, nil); err == nil || d.Err == nil {
			t.Fatal("expected NULL key error")
		}
		// Wrong type in version upsert.
		badType := qframe.New(map[string]interface{}{
			"key": []string{"k"}, "from": []string{"zero"}, "label": []string{"A"},
		}, newqf.ColumnOrder("key", "from", "label"))
		if d, err = r.Apply(emptyFactFrame(), badType, nil, nil); err == nil || d.Err == nil {
			t.Fatal("expected type error")
		}

		// State and counters after failure: state intact, stats reset.
		assertRows(t, "after failures", readRows(r.Snapshot()), want)
		if s := r.Stats(); s.FactVisits != 0 || s.IndexSteps != 0 || s.AggregateOps != 0 {
			t.Fatalf("failed batch must leave zeroed stats, got %+v", s)
		}

		// A subsequent valid batch still works and matches the reference.
		fix := []plainFact{{3, "k", 15, 9, 2}}
		d, err = r.Apply(factFrame(fix, false, false), emptyVersionFrame(), nil, nil)
		if err != nil {
			t.Fatal(err)
		}
		afterNaive, diffNaiveRows := naive.apply(fix, nil, nil, nil)
		assertRows(t, "recovered snapshot", readRows(r.Snapshot()), afterNaive)
		assertRows(t, "recovered diff", readDiffRows(d), diffNaiveRows)
	})

	t.Run("returned and input frames stay immutable", func(t *testing.T) {
		ids := []int{1, 2}
		keys := []string{"k", "k"}
		ats := []int{1, 11}
		amounts := []int{5, 6}
		entities := []int{7, 7}
		input := qframe.New(map[string]interface{}{
			"id": ids, "key": keys, "at": ats, "amount": amounts, "entity": entities,
		}, newqf.ColumnOrder("id", "key", "at", "amount", "entity"))
		r, err := qframe.NewTemporalRollup(input, versionFrame(goodVersions, versionLabels(goodVersions), nil))
		if err != nil {
			t.Fatal(err)
		}
		snap1 := readRows(r.Snapshot())
		diff1, err := r.Apply(emptyFactFrame(), emptyVersionFrame(), nil, nil)
		if err != nil {
			t.Fatal(err)
		}
		// Mutate the caller's backing slices after handoff.
		ids[0], ats[0], amounts[0], entities[0] = 999, 999, 999, 999
		keys[0] = "mutated"
		// Drive more batches so any aliased internal storage would surface.
		up := []plainVersion{{"k", 5, "B"}}
		if _, err = r.Apply(emptyFactFrame(), versionFrame(up, versionLabels(up), nil), nil, nil); err != nil {
			t.Fatal(err)
		}
		if _, err = r.Apply(factFrame([]plainFact{{9, "k", 6, 3, 7}}, false, false), emptyVersionFrame(), nil, nil); err != nil {
			t.Fatal(err)
		}
		snap2 := readRows(r.Snapshot())
		if reflect.DeepEqual(snap1, snap2) {
			t.Fatal("expected later batches to change the snapshot, sanity check failed")
		}
		// The earlier returned frames keep their original content.
		assertRows(t, "old snapshot frozen", snap1, newNaiveRollup(goodFacts, goodVersions).snapshot())
		if diff1.Len() != 0 || diff1.Err != nil {
			t.Fatalf("empty batch diff must stay empty, got %v", diff1)
		}
		if input.Err != nil {
			t.Fatal(input.Err)
		}
	})

	t.Run("snapshot ordering and empty result types", func(t *testing.T) {
		facts := []plainFact{
			{1, "z", 1, 1, 1}, {2, "m", 1, 1, 1}, {3, "none", 1, 1, 1},
			{4, "a", 1, 1, 1}, {5, "", 1, 1, 1},
		}
		versions := []plainVersion{{"z", 0, "Z"}, {"m", 0, "M"}, {"a", 0, "A"}, {"", 0, "EMPTY"}}
		r, err := qframe.NewTemporalRollup(factFrame(facts, false, false), versionFrame(versions, versionLabels(versions), nil))
		if err != nil {
			t.Fatal(err)
		}
		naive := newNaiveRollup(facts, versions)
		got := readRows(r.Snapshot())
		assertRows(t, "ordered", got, naive.snapshot())
		if got[0].label != nil {
			t.Fatal("NULL label must sort first")
		}
		labels := make([]string, 0, len(got)-1)
		for _, row := range got[1:] {
			labels = append(labels, *row.label)
		}
		if !sort.StringsAreSorted(labels) {
			t.Fatalf("non-null labels not sorted: %v", labels)
		}

		// Remove every fact: the empty snapshot still carries typed columns.
		empty, err := r.Apply(emptyFactFrame(), emptyVersionFrame(), []int{1, 2, 3, 4, 5}, nil)
		if err != nil {
			t.Fatal(err)
		}
		snap := r.Snapshot()
		assertGroupSchema(t, snap, [3]string{"rows", "sum", "entities"})
		if snap.Len() != 0 {
			t.Fatalf("expected no groups after deleting all facts, got %d", snap.Len())
		}
		assertGroupSchema(t, empty, [3]string{"d_rows", "d_sum", "d_entities"})
	})
}

func factFrameWithNullKey() qframe.QFrame {
	return qframe.New(map[string]interface{}{
		"id":     []int{1},
		"key":    []*string{nil},
		"at":     []int{1},
		"amount": []int{5},
		"entity": []int{7},
	}, newqf.ColumnOrder("id", "key", "at", "amount", "entity"))
}

// ---------- Test 4: work accounting on large, dense timelines ----------

type lcg struct{ state uint64 }

// next returns the next splitmix64 value. Unlike a low-bit LCG, splitmix64
// spreads entropy across all bits, so modulo key/entity counts cover every
// bucket instead of collapsing onto two values.
func (l *lcg) next() uint64 {
	l.state += 0x9e3779b97f4a7c15
	z := l.state
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9
	z = (z ^ (z >> 27)) * 0x94d049bb133111eb
	return z ^ (z >> 31)
}

func ceilLog2(x int) int {
	k, p := 0, 1
	for p < x {
		p <<= 1
		k++
	}
	return k
}

func buildLargeStore(t *testing.T, nFact, nKey int) (*qframe.TemporalRollup, *naiveRollup, []plainFact, []plainVersion) {
	t.Helper()
	rng := &lcg{state: 0x5151aaaa}
	facts := make([]plainFact, nFact)
	entities := nFact / 16
	if entities < 8 {
		entities = 8
	}
	for i := 0; i < nFact; i++ {
		facts[i] = plainFact{
			id:     i + 1,
			key:    fmt.Sprintf("k%03d", rng.next()%uint64(nKey)),
			at:     int(rng.next() % 4096),
			amount: int(rng.next()%201) - 100,
			entity: int(rng.next() % uint64(entities)),
		}
	}
	// Version timelines: key k000 is deliberately sparse so its [0, 2048)
	// interval is wide, while the other keys are dense (a version every 16
	// time units). This lets one test exercise both narrow, local interval
	// edits and a wide edit that propagates to thousands of facts.
	versions := make([]plainVersion, 0, 2+(nKey-1)*256)
	versions = append(versions,
		plainVersion{key: "k000", from: 0, label: "k000-early"},
		plainVersion{key: "k000", from: 2048, label: "k000-late"},
	)
	for k := 1; k < nKey; k++ {
		for from := 0; from < 4096; from += 16 {
			versions = append(versions, plainVersion{
				key:   fmt.Sprintf("k%03d", k),
				from:  from,
				label: fmt.Sprintf("k%03d-L%03d", k, from/16),
			})
		}
	}
	rollup, err := qframe.NewTemporalRollup(factFrame(facts, false, false), versionFrame(versions, versionLabels(versions), nil))
	if err != nil {
		t.Fatal(err)
	}
	return rollup, newNaiveRollup(facts, versions), facts, versions
}

// affectedCount computes A exactly as the production ledger defines it: the
// deduplicated facts dominated by the edited version spans on the pre-batch
// timeline plus every directly corrected/deleted fact.
func affectedCount(naive *naiveRollup, factRecs []plainFact, versionRecs []plainVersion, factDel []int, versionDel []qframe.VersionKey) int {
	involved := map[int]struct{}{}
	addKeyFacts := func(key string, from int) {
		// Pre-batch successor of "from" on this key.
		succ := math.MaxInt
		unbounded := true
		for vk := range naive.versions {
			if vk.key == key && vk.from > from && (unbounded || vk.from < succ) {
				succ, unbounded = vk.from, false
			}
		}
		for id, f := range naive.facts {
			if f.key != key || f.at < from {
				continue
			}
			if unbounded || f.at < succ {
				involved[id] = struct{}{}
			}
		}
	}
	seenV := map[naiveKey]struct{}{}
	for _, dk := range versionDel {
		k := naiveKey{dk.Key, dk.From}
		if _, done := seenV[k]; done {
			continue
		}
		seenV[k] = struct{}{}
		if _, ok := naive.versions[k]; ok {
			addKeyFacts(dk.Key, dk.From)
		}
	}
	for _, v := range versionRecs {
		addKeyFacts(v.key, v.from)
	}
	seenF := map[int]struct{}{}
	for _, id := range factDel {
		if _, done := seenF[id]; done {
			continue
		}
		seenF[id] = struct{}{}
		if _, ok := naive.facts[id]; ok {
			involved[id] = struct{}{}
		}
	}
	for _, f := range factRecs {
		// A counts corrections of stored events and dominated events. A pure
		// insert is batch input (part of E) but is not a corrected stored fact.
		if _, ok := naive.facts[f.id]; ok {
			involved[f.id] = struct{}{}
		}
	}
	return len(involved)
}

func assertWorkBounds(t *testing.T, name string, stats qframe.TemporalStats, e, storedA, inserts, n int) {
	t.Helper()
	// The bound's A counts every distinct event whose contribution is
	// touched: corrected/dominated stored events plus freshly inserted ones.
	aBound := storedA + inserts
	stepBound := 64 * (e + aBound + 1) * (ceilLog2(n+2) + 1)
	if stats.IndexSteps > stepBound {
		t.Fatalf("%s: IndexSteps %d exceeds bound %d (E=%d A=%d N=%d)", name, stats.IndexSteps, stepBound, e, aBound, n)
	}
	if stats.AggregateOps > 2*aBound {
		t.Fatalf("%s: AggregateOps %d exceeds 2*A=%d (storedA=%d inserts=%d)", name, stats.AggregateOps, 2*aBound, storedA, inserts)
	}
	// FactVisits only counts reads/copies of events that were already stored.
	if stats.FactVisits < storedA {
		t.Fatalf("%s: FactVisits %d smaller than stored involved count %d", name, stats.FactVisits, storedA)
	}
}

// pureInserts counts upsert rows whose id is not yet stored in the pre-batch
// reference. It must be called before the reference applies the batch.
func pureInserts(naive *naiveRollup, factRecs []plainFact) int {
	c := 0
	for _, f := range factRecs {
		if _, ok := naive.facts[f.id]; !ok {
			c++
		}
	}
	return c
}

func TestTemporalWork(t *testing.T) {
	const nKey = 8
	rollup, naive, facts, versions := buildLargeStore(t, 32768, nKey)
	if len(facts) < 32768 {
		t.Fatal("need at least 32768 facts")
	}
	assertRows(t, "large initial snapshot", readRows(rollup.Snapshot()), naive.snapshot())
	storeN := len(facts) + len(versions)

	t.Run("empty batch does no work", func(t *testing.T) {
		diff, err := rollup.Apply(emptyFactFrame(), emptyVersionFrame(), nil, nil)
		if err != nil {
			t.Fatal(err)
		}
		if diff.Len() != 0 {
			t.Fatalf("empty batch diff must be empty, got %d rows", diff.Len())
		}
		s := rollup.Stats()
		if s.FactVisits != 0 || s.IndexSteps != 0 || s.AggregateOps != 0 {
			t.Fatalf("empty batch must do zero work, got %+v", s)
		}
		assertWorkBounds(t, "empty", s, 0, 0, 0, storeN)
	})

	t.Run("local correction does not touch distant facts", func(t *testing.T) {
		// Same narrow correction on two differently sized stores: the number
		// of visited facts must stay constant instead of scaling with N.
		localFacts := []plainFact{{42, "k001", 100, 1234, 3}}
		e := len(localFacts)
		a := affectedCount(naive, localFacts, nil, nil, nil)
		if a != 1 {
			t.Fatalf("one corrected fact must give A=1, got %d", a)
		}

		diff, err := rollup.Apply(factFrame(localFacts, false, false), emptyVersionFrame(), nil, nil)
		if err != nil {
			t.Fatal(err)
		}
		afterNaive, wantDiff := naive.apply(localFacts, nil, nil, nil)
		assertRows(t, "local snapshot", readRows(rollup.Snapshot()), afterNaive)
		assertRows(t, "local diff", readDiffRows(diff), wantDiff)
		small := rollup.Stats()
		assertWorkBounds(t, "local", small, e, a, 0, storeN)
		if small.FactVisits != 1 {
			t.Fatalf("one corrected fact must mean one fact visit, got %d", small.FactVisits)
		}

		bigRollup, bigNaive, bigFacts, bigVersions := buildLargeStore(t, 65536, nKey)
		bigN := len(bigFacts) + len(bigVersions)
		diff2, err := bigRollup.Apply(factFrame(localFacts, false, false), emptyVersionFrame(), nil, nil)
		if err != nil {
			t.Fatal(err)
		}
		bigAfter, bigDiff := bigNaive.apply(localFacts, nil, nil, nil)
		assertRows(t, "big local snapshot", readRows(bigRollup.Snapshot()), bigAfter)
		assertRows(t, "big local diff", readDiffRows(diff2), bigDiff)
		bigStats := bigRollup.Stats()
		assertWorkBounds(t, "big local", bigStats, e, 1, 0, bigN)
		if bigStats.FactVisits != small.FactVisits {
			t.Fatalf("local work must not scale with store size: %d vs %d visits", small.FactVisits, bigStats.FactVisits)
		}
		// Index depth grows only logarithmically; allow a generous factor.
		if bigStats.IndexSteps > small.IndexSteps*2+8 {
			t.Fatalf("index work grew super-logarithmically: %d vs %d", small.IndexSteps, bigStats.IndexSteps)
		}
	})

	t.Run("narrow version edit propagates only inside its interval", func(t *testing.T) {
		versionRecs := []plainVersion{{"k001", 100, "RELABEL"}}
		a := affectedCount(naive, nil, versionRecs, nil, nil)
		beforeNaive := naive.snapshot()
		diff, err := rollup.Apply(emptyFactFrame(), versionFrame(versionRecs, versionLabels(versionRecs), nil), nil, nil)
		if err != nil {
			t.Fatal(err)
		}
		afterNaive, _ := naive.apply(nil, versionRecs, nil, nil)
		assertRows(t, "narrow snapshot", readRows(rollup.Snapshot()), afterNaive)
		assertRows(t, "narrow diff", readDiffRows(diff), diffNaive(beforeNaive, afterNaive))
		s := rollup.Stats()
		// A is a small slice of one key, nowhere near the full 32768 facts.
		if a >= 32768/nKey {
			t.Fatalf("narrow interval unexpectedly swept a whole key: A=%d", a)
		}
		if s.FactVisits != a {
			t.Fatalf("narrow sweep visited %d facts, expected exactly A=%d", s.FactVisits, a)
		}
		assertWorkBounds(t, "narrow", s, len(versionRecs), a, 0, storeN)
	})

	t.Run("wide version edit propagates to every dominated fact", func(t *testing.T) {
		// k000 has a sparse timeline: versions only at 0 and 2048. Replacing
		// the label of the [0, 2048) interval must reach every fact inside it
		// (thousands), while facts on every other key are never visited.
		versionRecs := []plainVersion{{"k000", 0, "WIDE"}}
		var intervalFacts int
		for _, f := range naive.facts {
			if f.key == "k000" && f.at >= 0 && f.at < 2048 {
				intervalFacts++
			}
		}
		if intervalFacts < 1000 {
			t.Fatalf("test setup: expected thousands of wide interval facts, got %d", intervalFacts)
		}
		a := affectedCount(naive, nil, versionRecs, nil, nil)
		if a != intervalFacts {
			t.Fatalf("wide A must equal the interval facts: got %d want %d", a, intervalFacts)
		}
		beforeNaive := naive.snapshot()
		diff, err := rollup.Apply(emptyFactFrame(), versionFrame(versionRecs, versionLabels(versionRecs), nil), nil, nil)
		if err != nil {
			t.Fatal(err)
		}
		afterNaive, _ := naive.apply(nil, versionRecs, nil, nil)
		assertRows(t, "wide snapshot", readRows(rollup.Snapshot()), afterNaive)
		assertRows(t, "wide diff", readDiffRows(diff), diffNaive(beforeNaive, afterNaive))
		s := rollup.Stats()
		if s.FactVisits != intervalFacts {
			t.Fatalf("wide sweep must reach all %d interval facts, visited %d", intervalFacts, s.FactVisits)
		}
		assertWorkBounds(t, "wide", s, len(versionRecs), intervalFacts, 0, storeN)

		// Reverse the edit: every fact migrates back through the same span.
		revert := []plainVersion{{"k000", 0, "k000-early"}}
		beforeNaive = naive.snapshot()
		diff, err = rollup.Apply(emptyFactFrame(), versionFrame(revert, versionLabels(revert), nil), nil, nil)
		if err != nil {
			t.Fatal(err)
		}
		afterNaive, _ = naive.apply(nil, revert, nil, nil)
		assertRows(t, "wide revert snapshot", readRows(rollup.Snapshot()), afterNaive)
		assertRows(t, "wide revert diff", readDiffRows(diff), diffNaive(beforeNaive, afterNaive))
		if rollup.Stats().FactVisits != intervalFacts {
			t.Fatalf("revert sweep must again reach all %d interval facts, got %d", intervalFacts, rollup.Stats().FactVisits)
		}
	})

	t.Run("cross interval fact correction matches reference and bounds", func(t *testing.T) {
		factRecs := []plainFact{
			{7, "k002", 8, 5, 1},
			{8, "k002", 2000, -9, 2},
			{900000, "k005", 4095, 7, 3},
		}
		versionRecs := []plainVersion{{"k002", 1000, "CROSS"}}
		a := affectedCount(naive, factRecs, versionRecs, nil, nil)
		inserts := pureInserts(naive, factRecs)
		beforeNaive := naive.snapshot()
		diff, err := rollup.Apply(factFrame(factRecs, false, false),
			versionFrame(versionRecs, versionLabels(versionRecs), nil), nil, nil)
		if err != nil {
			t.Fatal(err)
		}
		afterNaive, _ := naive.apply(factRecs, versionRecs, nil, nil)
		assertRows(t, "cross snapshot", readRows(rollup.Snapshot()), afterNaive)
		assertRows(t, "cross diff", readDiffRows(diff), diffNaive(beforeNaive, afterNaive))
		assertWorkBounds(t, "cross", rollup.Stats(), len(factRecs)+len(versionRecs), a, inserts, storeN)
	})

	t.Run("many dense version edits keep logarithmic index work after merging", func(t *testing.T) {
		// A fresh, isolated store so this subtest does not depend on the
		// ordering of the earlier ones.
		denseRollup, denseNaive, denseFacts, denseVersions := buildLargeStore(t, 32768, nKey)
		denseN := len(denseFacts) + len(denseVersions)

		// Relabel a long run of adjacent versions on one dense key in a single
		// batch. Their spans abut and merge, so swept facts are visited once.
		versionRecs := make([]plainVersion, 0, 128)
		for from := 0; from < 2048; from += 16 {
			versionRecs = append(versionRecs, plainVersion{key: "k001", from: from, label: "DENSE"})
		}
		a := affectedCount(denseNaive, nil, versionRecs, nil, nil)
		beforeNaive := denseNaive.snapshot()
		diff, err := denseRollup.Apply(emptyFactFrame(), versionFrame(versionRecs, versionLabels(versionRecs), nil), nil, nil)
		if err != nil {
			t.Fatal(err)
		}
		afterNaive, _ := denseNaive.apply(nil, versionRecs, nil, nil)
		assertRows(t, "dense snapshot", readRows(denseRollup.Snapshot()), afterNaive)
		assertRows(t, "dense diff", readDiffRows(diff), diffNaive(beforeNaive, afterNaive))

		s := denseRollup.Stats()
		if s.FactVisits != a {
			t.Fatalf("merged spans must visit each affected fact once: visits=%d A=%d", s.FactVisits, a)
		}
		assertWorkBounds(t, "dense", s, len(versionRecs), a, 0, denseN)
		// With E large and A bounded, the realized step count must sit well
		// inside the generous 64x ceiling, i.e. index work stays logarithmic.
		ceiling := 64 * (len(versionRecs) + a + 1) * (ceilLog2(denseN+2) + 1)
		if s.IndexSteps > ceiling/4 {
			t.Fatalf("index work %d exceeds a quarter of the allowed ceiling %d", s.IndexSteps, ceiling)
		}
	})
}
