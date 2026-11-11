package twitch

import (
	"reflect"
	"sort"
	"testing"
)

func namesSnapshotClient() *Client {
	c := NewClient("collector", "placeholder")
	c.EnableUserlistSnapshots = true
	return c
}

func namesLines(t *testing.T, c *Client, lines ...string) {
	t.Helper()
	for _, line := range lines {
		if err := c.handleLine(line); err != nil {
			t.Fatalf("handleLine(%q): %v", line, err)
		}
	}
}

func namesUserlist(t *testing.T, c *Client, channel string) []string {
	t.Helper()
	got, err := c.Userlist(channel)
	if err != nil {
		t.Fatalf("Userlist(%q): %v", channel, err)
	}
	sort.Strings(got)
	return got
}

func assertNamesEqual(t *testing.T, got, want []string) {
	t.Helper()
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("got %v, want %v", got, want)
	}
}

func TestNamesSnapshotInterleave(t *testing.T) {
	t.Run("two rounds replace the list", func(t *testing.T) {
		c := namesSnapshotClient()
		c.Join("alpha")

		namesCount := 0
		c.OnNamesMessage(func(message NamesMessage) {
			namesCount++
		})

		namesLines(t, c,
			":srv 353 collector = #alpha :alice bob",
			":srv 366 collector #alpha :End",
		)
		assertNamesEqual(t, namesUserlist(t, c, "alpha"), []string{"alice", "bob"})

		// While the second round is being collected, the previous complete list stays visible
		namesLines(t, c, ":srv 353 collector = #alpha :carol")
		assertNamesEqual(t, namesUserlist(t, c, "alpha"), []string{"alice", "bob"})

		namesLines(t, c,
			":srv 353 collector = #alpha :dave",
			":srv 366 collector #alpha :End",
		)
		assertNamesEqual(t, namesUserlist(t, c, "alpha"), []string{"carol", "dave"})

		if namesCount != 3 {
			t.Fatalf("OnNamesMessage called %d times, want 3", namesCount)
		}
	})

	t.Run("late fragments and realtime events", func(t *testing.T) {
		c := namesSnapshotClient()
		c.Join("alpha")

		namesLines(t, c,
			":srv 353 collector = #alpha :alice bob",
			":srv 366 collector #alpha :End",
		)

		namesLines(t, c,
			":srv 353 collector = #alpha :alice",
			":alice!alice@srv PART #alpha",
			":carol!carol@srv JOIN #alpha",
		)

		// Real-time JOIN/PART are visible while the round is still being collected
		assertNamesEqual(t, namesUserlist(t, c, "alpha"), []string{"bob", "carol"})

		// A late fragment must not resurrect alice, and carol must not be lost
		namesLines(t, c,
			":srv 353 collector = #alpha :alice bob",
			":srv 366 collector #alpha :End",
		)
		assertNamesEqual(t, namesUserlist(t, c, "alpha"), []string{"bob", "carol"})
	})

	t.Run("interleaved channels stay independent", func(t *testing.T) {
		c := namesSnapshotClient()
		c.Join("alpha", "beta")

		namesLines(t, c,
			":srv 353 collector = #alpha :alice",
			":srv 353 collector = #beta :bob",
			":srv 353 collector = #alpha :carol",
			":srv 366 collector #beta :End",
		)
		assertNamesEqual(t, namesUserlist(t, c, "beta"), []string{"bob"})
		// alpha's round is still collecting, its previous (empty) complete list is visible
		assertNamesEqual(t, namesUserlist(t, c, "alpha"), []string{})

		namesLines(t, c,
			":eve!eve@srv JOIN #alpha",
			":bob!bob@srv PART #beta",
			":srv 353 collector = #beta :dave",
			":srv 366 collector #alpha :End",
		)
		assertNamesEqual(t, namesUserlist(t, c, "alpha"), []string{"alice", "carol", "eve"})
		// beta's new round is still collecting; bob parted from the visible list
		assertNamesEqual(t, namesUserlist(t, c, "beta"), []string{})

		namesLines(t, c, ":srv 366 collector #beta :End")
		assertNamesEqual(t, namesUserlist(t, c, "beta"), []string{"dave"})
	})

	t.Run("empty round and unknown channel", func(t *testing.T) {
		c := namesSnapshotClient()
		c.Join("alpha")

		completeCount := 0
		var completed []string
		c.OnNamesComplete(func(channel string, users []string) {
			completeCount++
			completed = append(completed, channel)
		})

		// A joined channel that receives 366 without any 353 completes with an empty list
		namesLines(t, c, ":srv 366 collector #alpha :End")
		if completeCount != 1 {
			t.Fatalf("OnNamesComplete called %d times, want 1", completeCount)
		}
		assertNamesEqual(t, completed, []string{"alpha"})
		assertNamesEqual(t, namesUserlist(t, c, "alpha"), []string{})

		// An end of names for an unknown channel must not create a userlist
		namesLines(t, c, ":srv 366 collector #ghost :End")
		if completeCount != 1 {
			t.Fatalf("OnNamesComplete called %d times, want 1", completeCount)
		}
		if _, err := c.Userlist("ghost"); err == nil {
			t.Fatal("unknown channel 366 created a userlist")
		}

		// Names for a channel we have not joined must not create a userlist either
		namesLines(t, c, ":srv 353 collector = #ghost :alice")
		if _, err := c.Userlist("ghost"); err == nil {
			t.Fatal("unknown channel 353 created a userlist")
		}
	})

	t.Run("late messages after depart are dropped", func(t *testing.T) {
		c := namesSnapshotClient()
		c.Join("alpha")

		namesLines(t, c, ":srv 353 collector = #alpha :alice")
		c.Depart("alpha")

		// Late 353/366 must not rebuild the dropped round
		namesLines(t, c,
			":srv 353 collector = #alpha :bob",
			":srv 366 collector #alpha :End",
		)
		if _, err := c.Userlist("alpha"); err == nil {
			t.Fatal("late names rebuilt the userlist after depart")
		}

		// Rejoining allows a fresh round to be collected
		c.Join("alpha")
		namesLines(t, c,
			":srv 353 collector = #alpha :carol",
			":srv 366 collector #alpha :End",
		)
		assertNamesEqual(t, namesUserlist(t, c, "alpha"), []string{"carol"})
	})

	t.Run("case and membership markers", func(t *testing.T) {
		c := namesSnapshotClient()
		c.Join("alpha")

		namesLines(t, c,
			":srv 353 collector = #alpha :@Alice +Bob %Carol &Dave ~Eve",
			":BOB!BOB@srv PART #alpha",
			":FRANK!FRANK@srv JOIN #alpha",
			":srv 353 collector = #alpha :alice ALICE @alice",
			":srv 366 collector #alpha :End",
		)
		assertNamesEqual(t, namesUserlist(t, c, "alpha"), []string{"alice", "carol", "dave", "eve", "frank"})
	})

	t.Run("completion callback is reentrant", func(t *testing.T) {
		c := namesSnapshotClient()
		c.Join("alpha", "beta")

		completeCount := 0
		var sawInCallback []string
		c.OnNamesComplete(func(channel string, users []string) {
			completeCount++

			// The completed list must already be readable, without deadlocking
			visible, err := c.Userlist(channel)
			if err != nil {
				t.Errorf("Userlist(%q) in callback: %v", channel, err)
			}
			sort.Strings(visible)

			if channel == "alpha" {
				sawInCallback = visible

				// The callback slice is an independent copy
				if len(users) > 0 {
					users[0] = "mutated"
				}

				// Departing from inside the callback must not deadlock
				c.Depart("beta")
			}
		})

		namesLines(t, c,
			":srv 353 collector = #alpha :alice",
			":srv 366 collector #alpha :End",
		)
		if completeCount != 1 {
			t.Fatalf("OnNamesComplete called %d times, want 1", completeCount)
		}
		assertNamesEqual(t, sawInCallback, []string{"alice"})
		// Mutating the callback slice did not leak into the internal list
		assertNamesEqual(t, namesUserlist(t, c, "alpha"), []string{"alice"})
		if _, err := c.Userlist("beta"); err == nil {
			t.Fatal("Depart inside callback did not remove beta")
		}

		// Late messages for the departed channel are ignored
		namesLines(t, c,
			":srv 353 collector = #beta :bob",
			":srv 366 collector #beta :End",
		)
		if completeCount != 1 {
			t.Fatalf("OnNamesComplete called %d times, want 1", completeCount)
		}
	})

	t.Run("unfinished round is dropped on reconnect", func(t *testing.T) {
		c := namesSnapshotClient()
		c.Join("alpha")

		namesLines(t, c, ":srv 353 collector = #alpha :alice")

		// Simulate a new connection: the unfinished round must not carry over
		c.initialJoins()

		namesLines(t, c, ":srv 366 collector #alpha :End")
		assertNamesEqual(t, namesUserlist(t, c, "alpha"), []string{})
	})

	t.Run("end of names parses as EndNamesMessage", func(t *testing.T) {
		msg := ParseMessage(":srv 366 collector #alpha :End of /NAMES list")
		end, ok := msg.(*EndNamesMessage)
		if !ok {
			t.Fatalf("366 parsed as %T, want *EndNamesMessage", msg)
		}
		if end.Channel != "alpha" {
			t.Fatalf("channel %q, want %q", end.Channel, "alpha")
		}
		if msg.GetType() != END_NAMES {
			t.Fatalf("type %v, want END_NAMES", msg.GetType())
		}
	})

	t.Run("snapshots disabled keeps legacy behavior", func(t *testing.T) {
		c := NewClient("collector", "placeholder")
		c.Join("alpha")

		namesCount := 0
		completeCount := 0
		c.OnNamesMessage(func(message NamesMessage) {
			namesCount++
		})
		c.OnNamesComplete(func(channel string, users []string) {
			completeCount++
		})

		namesLines(t, c,
			":srv 353 collector = #alpha :alice bob",
			":srv 366 collector #alpha :End",
			":srv 353 collector = #alpha :bob",
			":srv 366 collector #alpha :End",
		)

		// Without snapshots the userlist keeps accumulating across rounds
		assertNamesEqual(t, namesUserlist(t, c, "alpha"), []string{"alice", "bob"})
		if namesCount != 2 {
			t.Fatalf("OnNamesMessage called %d times, want 2", namesCount)
		}
		if completeCount != 0 {
			t.Fatalf("OnNamesComplete called %d times, want 0", completeCount)
		}
	})
}
