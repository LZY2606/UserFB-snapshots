package twitterscraper

import (
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strings"
	"testing"

	"github.com/n0madic/twitter-scraper/task_support/taskcheck"
)

// Local JSON fixtures used by the timeline instruction tests. All payloads are
// hand-built and benign; no account or online API is involved.

func tiTweetJSON(id, text string) map[string]interface{} {
	return map[string]interface{}{
		"__typename": "Tweet",
		"legacy": map[string]interface{}{
			"id_str":              id,
			"full_text":           text,
			"user_id_str":         "u",
			"conversation_id_str": id,
		},
		"core": map[string]interface{}{
			"user_results": map[string]interface{}{
				"result": map[string]interface{}{
					"legacy": map[string]interface{}{"screen_name": "local", "name": "Local"},
				},
			},
		},
	}
}

func tiLegacy(tweet map[string]interface{}) map[string]interface{} {
	return tweet["legacy"].(map[string]interface{})
}

func tiWithNote(tweet map[string]interface{}, text string) map[string]interface{} {
	tweet["note_tweet"] = map[string]interface{}{
		"note_tweet_results": map[string]interface{}{
			"result": map[string]interface{}{"text": text},
		},
	}
	return tweet
}

func tiWrapper(tweet interface{}, typename string) map[string]interface{} {
	wrapper := map[string]interface{}{"__typename": typename}
	if tweet != nil {
		wrapper["tweet"] = tweet
	}
	return wrapper
}

func tiContent(display string, result interface{}) map[string]interface{} {
	return map[string]interface{}{
		"tweetDisplayType": display,
		"tweet_results":    map[string]interface{}{"result": result},
	}
}

func tiFlat(entryID, display string, result interface{}) map[string]interface{} {
	return map[string]interface{}{
		"entryId": entryID,
		"content": map[string]interface{}{
			"itemContent": tiContent(display, result),
		},
	}
}

func tiCursor(entryID, cursorType, value string) map[string]interface{} {
	return map[string]interface{}{
		"entryId": entryID,
		"content": map[string]interface{}{"cursorType": cursorType, "value": value},
	}
}

func tiModuleItem(itemID, display string, result interface{}) map[string]interface{} {
	return map[string]interface{}{
		"entryId": itemID,
		"item":    map[string]interface{}{"itemContent": tiContent(display, result)},
	}
}

func tiModule(entryID string, items []interface{}) map[string]interface{} {
	content := map[string]interface{}{"entryType": "TimelineTimelineModule"}
	if items != nil {
		content["items"] = items
	}
	return map[string]interface{}{"entryId": entryID, "content": content}
}

func tiInstruction(kind string, fields map[string]interface{}) map[string]interface{} {
	instruction := map[string]interface{}{"type": kind}
	for key, value := range fields {
		instruction[key] = value
	}
	return instruction
}

func tiAddEntries(entries ...interface{}) map[string]interface{} {
	return tiInstruction("TimelineAddEntries", map[string]interface{}{"entries": entries})
}

func tiReplaceEntry(entryIDToReplace string, replacement map[string]interface{}) map[string]interface{} {
	return tiInstruction("TimelineReplaceEntry", map[string]interface{}{
		"entryIdToReplace": entryIDToReplace,
		"entry":            replacement,
	})
}

func tiAddToModule(moduleEntryID string, items ...interface{}) map[string]interface{} {
	return tiInstruction("TimelineAddToModule", map[string]interface{}{
		"moduleEntryId": moduleEntryID,
		"moduleItems":   items,
	})
}

func tiUserRoot(instructions []interface{}) map[string]interface{} {
	return map[string]interface{}{
		"data": map[string]interface{}{
			"user": map[string]interface{}{
				"result": map[string]interface{}{
					"timeline_v2": map[string]interface{}{
						"timeline": map[string]interface{}{"instructions": instructions},
					},
				},
			},
		},
	}
}

func tiSearchRoot(instructions []interface{}) map[string]interface{} {
	return map[string]interface{}{
		"data": map[string]interface{}{
			"search_by_raw_query": map[string]interface{}{
				"search_timeline": map[string]interface{}{
					"timeline": map[string]interface{}{"instructions": instructions},
				},
			},
		},
	}
}

func tiThreadRoot(instructions []interface{}) map[string]interface{} {
	return map[string]interface{}{
		"data": map[string]interface{}{
			"threaded_conversation_with_injections_v2": map[string]interface{}{
				"instructions": instructions,
			},
		},
	}
}

func tiDecode(c *taskcheck.C, root map[string]interface{}, target interface{}) bool {
	data, err := json.Marshal(root)
	if err != nil {
		c.Check(false, "marshal fixture: %v", err)
		return false
	}
	if err := json.Unmarshal(data, target); err != nil {
		c.Check(false, "unmarshal fixture: %v", err)
		return false
	}
	return true
}

func tiIDs(tweets []*Tweet) string {
	ids := make([]string, len(tweets))
	for i, tweet := range tweets {
		ids[i] = tweet.ID
	}
	return strings.Join(ids, ",")
}

// TestTimelineSlots covers add/update/replace semantics of display slots.
func TestTimelineSlots(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		first := tiWithNote(tiTweetJSON("101", "one"), "long one")
		duplicate := tiTweetJSON("101", "one updated")
		duplicate["views"] = map[string]interface{}{"count": "23"}

		instructions := []interface{}{
			tiAddEntries(
				tiFlat("tweet-1", "Tweet", first),
				tiFlat("tweet-2", "Tweet", tiTweetJSON("102", "two")),
				tiFlat("tweet-3", "Tweet", tiTweetJSON("103", "three")),
			),
			// Repeated identity updates content in place, keeping position.
			tiAddEntries(tiFlat("tweet-1", "Tweet", duplicate)),
			// Replacement swaps identity and content in place.
			tiReplaceEntry("tweet-2", tiFlat("tweet-2", "Tweet", tiTweetJSON("202", "replaced"))),
			// Unknown target and conflicting identity are both ignored.
			tiReplaceEntry("tweet-missing", tiFlat("tweet-9", "Tweet", tiTweetJSON("909", "ghost"))),
			tiReplaceEntry("tweet-3", tiFlat("tweet-1", "Tweet", tiTweetJSON("111", "conflict"))),
			// Unknown instruction and entries without identity produce nothing.
			tiInstruction("TimelineUnknown", map[string]interface{}{
				"entries": []interface{}{tiFlat("tweet-unknown", "Tweet", tiTweetJSON("998", "unknown"))},
			}),
			tiAddEntries(map[string]interface{}{
				"content": map[string]interface{}{"itemContent": tiContent("Tweet", tiTweetJSON("997", "no identity"))},
			}),
		}

		var timeline timelineV2
		if !tiDecode(c, tiUserRoot(instructions), &timeline) {
			return
		}
		tweets, cursor := timeline.parseTweets()
		c.Check(tiIDs(tweets) == "101,202,103", "slot order/content = %s", tiIDs(tweets))
		c.Check(cursor == "", "unexpected cursor %q without a Bottom cursor", cursor)
		if len(tweets) == 3 {
			c.Check(tweets[0].Text == "one updated", "duplicate identity did not update content: %q", tweets[0].Text)
			c.Check(tweets[0].Views == 23, "duplicate identity lost views: %d", tweets[0].Views)
			c.Check(tweets[1].Text == "replaced", "replacement content = %q", tweets[1].Text)
			c.Check(tweets[2].Text == "three", "conflicting replacement applied: %q", tweets[2].Text)
		}
		c.Check(!strings.Contains(tiIDs(tweets), "909"), "missing replacement target was appended")
		c.Check(!strings.Contains(tiIDs(tweets), "111"), "conflicting replacement identity was applied")
		c.Check(!strings.Contains(tiIDs(tweets), "998"), "unknown instruction produced a tweet")
		c.Check(!strings.Contains(tiIDs(tweets), "997"), "entry without identity produced a tweet")

		// Parsing the same decoded object twice yields the same result and does
		// not rewrite the decoded note tweet/legacy fields.
		again, againCursor := timeline.parseTweets()
		c.Check(tiIDs(again) == tiIDs(tweets), "repeated parse changed ids: %s vs %s", tiIDs(again), tiIDs(tweets))
		c.Check(againCursor == cursor, "repeated parse changed cursor: %q vs %q", againCursor, cursor)
		rawResult := timeline.Data.User.Result.TimelineV2.Timeline.Instructions[0].Entries[0].
			Content.ItemContent.TweetResults.Result
		c.Check(rawResult.Legacy.FullText == "one", "decoded object was rewritten: %q", rawResult.Legacy.FullText)

		// Cursor: last Bottom among the final slots wins, Top is ignored, and a
		// replaced Bottom cursor does not survive.
		var cursorTimeline timelineV2
		if !tiDecode(c, tiUserRoot([]interface{}{
			tiAddEntries(tiCursor("cursor-bottom", "Bottom", "old"), tiCursor("cursor-top", "Top", "top-one")),
			tiReplaceEntry("cursor-bottom", tiCursor("cursor-bottom", "Bottom", "new")),
			tiAddEntries(tiCursor("cursor-top-2", "Top", "top-two")),
		}), &cursorTimeline) {
			return
		}
		_, bottom := cursorTimeline.parseTweets()
		c.Check(bottom == "new", "bottom cursor = %q", bottom)

		var noBottomTimeline timelineV2
		if !tiDecode(c, tiUserRoot([]interface{}{
			tiAddEntries(tiCursor("cursor-bottom", "Bottom", "old")),
			tiReplaceEntry("cursor-bottom", tiCursor("cursor-bottom", "Top", "demoted")),
		}), &noBottomTimeline) {
			return
		}
		_, noBottom := noBottomTimeline.parseTweets()
		c.Check(noBottom == "", "removed Bottom cursor survived: %q", noBottom)
	})
}

// TestTimelineModules covers module creation, item appends/updates and slot
// expansion order.
func TestTimelineModules(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		instructions := []interface{}{
			tiAddEntries(
				tiModule("module-1", []interface{}{
					tiModuleItem("item-a", "Tweet", tiTweetJSON("301", "a")),
					tiModuleItem("item-b", "Tweet", tiWrapper(tiTweetJSON("302", "b"), "TweetWithVisibilityResults")),
				}),
				tiFlat("tweet-1", "Tweet", tiTweetJSON("303", "plain")),
			),
			// New item appended at the end, repeated item updated in place.
			tiAddToModule("module-1",
				tiModuleItem("item-c", "Tweet", tiTweetJSON("303", "c")),
				tiModuleItem("item-a", "Tweet", tiTweetJSON("311", "a-updated")),
			),
			// Empty item list is valid and changes nothing.
			tiAddToModule("module-1"),
			// A normal slot and a missing module do not accept items.
			tiAddToModule("tweet-1", tiModuleItem("item-x", "Tweet", tiTweetJSON("399", "x"))),
			tiAddToModule("module-ghost", tiModuleItem("item-y", "Tweet", tiTweetJSON("398", "y"))),
			// A module with empty content can still receive items.
			tiAddEntries(tiModule("module-empty", nil)),
			tiAddToModule("module-empty", tiModuleItem("item-d", "Tweet", tiTweetJSON("305", "d"))),
			// Replacing the module identity keeps its position and content, and
			// only the new identity accepts later items.
			tiReplaceEntry("module-empty", tiModule("module-renamed", []interface{}{
				tiModuleItem("item-e", "Tweet", tiTweetJSON("306", "e")),
			})),
			tiAddToModule("module-empty", tiModuleItem("item-f", "Tweet", tiTweetJSON("307", "f"))),
			tiAddToModule("module-renamed", tiModuleItem("item-f", "Tweet", tiTweetJSON("307", "f"))),
		}

		var timeline timelineV2
		if !tiDecode(c, tiUserRoot(instructions), &timeline) {
			return
		}
		tweets, cursor := timeline.parseTweets()
		c.Check(tiIDs(tweets) == "311,302,303,303,306,307", "module expansion order = %s", tiIDs(tweets))
		c.Check(cursor == "", "unexpected cursor %q", cursor)
		if len(tweets) != 6 {
			return
		}
		c.Check(tweets[0].Text == "a-updated", "updated module item = %q", tweets[0].Text)
		c.Check(tweets[1].Text == "b", "wrapped module item = %q", tweets[1].Text)
		c.Check(tweets[2].Text == "c", "appended module item = %q", tweets[2].Text)
		c.Check(tweets[3].Text == "plain", "normal slot content = %q", tweets[3].Text)
		// Same tweet ID in a module item and in a normal slot stays two positions.
		c.Check(tweets[2].ID == tweets[3].ID, "expected the same tweet ID in two slots: %q vs %q", tweets[2].ID, tweets[3].ID)
		c.Check(tweets[4].Text == "e", "replaced module first item = %q", tweets[4].Text)
		c.Check(tweets[5].Text == "f", "item appended after identity change = %q", tweets[5].Text)
		c.Check(!strings.Contains(tiIDs(tweets), "399"), "normal slot accepted module items")
		c.Check(!strings.Contains(tiIDs(tweets), "398"), "unknown module accepted items")

		again, againCursor := timeline.parseTweets()
		c.Check(tiIDs(again) == tiIDs(tweets), "repeated module parse changed ids: %s vs %s", tiIDs(again), tiIDs(tweets))
		c.Check(againCursor == cursor, "repeated module parse changed cursor: %q vs %q", againCursor, cursor)
	})
}

type tiTransport func(*http.Request) (*http.Response, error)

func (f tiTransport) RoundTrip(request *http.Request) (*http.Response, error) {
	return f(request)
}

type tiBody struct {
	io.Reader
	closed *int
}

func (body *tiBody) Close() error {
	*body.closed++
	return nil
}

// TestTimelineEntrypoints drives the real collection methods through a local
// RoundTripper and checks the unified instruction processing end to end.
func TestTimelineEntrypoints(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		userTweet := tiWithNote(tiTweetJSON("401", "short user text"), "long user text")
		userTweet["views"] = map[string]interface{}{"count": "99"}
		userTweet["quoted_status_result"] = map[string]interface{}{
			"result": tiTweetJSON("451", "quoted user text"),
		}
		userInstructions := []interface{}{
			tiAddEntries(
				tiFlat("tweet-401", "Tweet", userTweet),
				tiCursor("cursor-bottom", "Bottom", "user-old"),
				tiCursor("cursor-top", "Top", "user-top-one"),
				tiFlat("tweet-402", "Tweet", tiTweetJSON("402", "second")),
			),
			tiReplaceEntry("cursor-bottom", tiCursor("cursor-bottom", "Bottom", "user-new")),
			tiReplaceEntry("tweet-402", tiFlat("tweet-402", "Tweet",
				tiWrapper(tiTweetJSON("412", "user replaced"), "TweetWithVisibilityResults"))),
			tiAddEntries(tiCursor("cursor-top-2", "Top", "user-top-two")),
			tiInstruction("TimelineUnknown", map[string]interface{}{}),
		}

		searchTweet := tiWithNote(tiTweetJSON("501", "short search text"), "long search text")
		searchTweet["quoted_status_result"] = map[string]interface{}{
			"result": tiWrapper(tiTweetJSON("551", "quoted search text"), "TweetWithVisibilityResults"),
		}
		searchInstructions := []interface{}{
			tiAddEntries(
				tiFlat("tweet-501", "Tweet", searchTweet),
				tiFlat("tweet-502", "Tweet", tiWrapper(tiTweetJSON("502", "wrapped search text"), "TweetWithVisibilityResults")),
				tiFlat("tweet-504", "Tweet", map[string]interface{}{"__typename": "TweetUnavailable"}),
				tiFlat("tweet-505", "Tweet", tiWrapper(nil, "TweetWithVisibilityResults")),
				tiCursor("cursor-bottom", "Bottom", "search-old"),
				tiCursor("cursor-top", "Top", "search-top"),
				tiModule("search-module", []interface{}{
					tiModuleItem("search-item", "Tweet", tiTweetJSON("503", "module search text")),
				}),
			),
			tiReplaceEntry("cursor-bottom", tiCursor("cursor-bottom", "Bottom", "search-new")),
		}

		root := tiTweetJSON("601", "root")
		tiLegacy(root)["conversation_id_str"] = "601"
		root["legacy"].(map[string]interface{})["self_thread"] = map[string]interface{}{"id_str": "601"}
		child := tiTweetJSON("602", "child")
		tiLegacy(child)["conversation_id_str"] = "601"
		tiLegacy(child)["in_reply_to_status_id_str"] = "601"
		removed := tiTweetJSON("603", "removed")
		tiLegacy(removed)["in_reply_to_status_id_str"] = "601"
		replacement := tiTweetJSON("613", "replacement")
		tiLegacy(replacement)["in_reply_to_status_id_str"] = "601"
		dangling := tiTweetJSON("604", "dangling")
		tiLegacy(dangling)["in_reply_to_status_id_str"] = "603"
		bound := tiTweetJSON("605", "bound")
		tiLegacy(bound)["in_reply_to_status_id_str"] = "613"
		threadInstructions := []interface{}{
			tiAddEntries(
				tiFlat("tweet-601", "SelfThread", root),
				tiFlat("tweet-602", "SelfThread", child),
				tiFlat("tweet-603", "Tweet", removed),
				tiFlat("tweet-604", "Tweet", dangling),
				tiFlat("tweet-605", "Tweet", bound),
			),
			tiReplaceEntry("tweet-603", tiFlat("tweet-613", "Tweet", replacement)),
		}

		responses := map[string]map[string]interface{}{
			"/UserTweets":     tiUserRoot(userInstructions),
			"/SearchTimeline": tiSearchRoot(searchInstructions),
			"/TweetDetail":    tiThreadRoot(threadInstructions),
		}

		scraper := New()
		scraper.isLogged = true
		hits := 0
		closed := 0
		scraper.client.Transport = tiTransport(func(request *http.Request) (*http.Response, error) {
			hits++
			var payload map[string]interface{}
			for suffix, candidate := range responses {
				if strings.HasSuffix(request.URL.Path, suffix) {
					payload = candidate
					break
				}
			}
			if request.Method != "GET" || request.URL.Query().Get("variables") == "" {
				c.Check(false, "request lost method or variables: %s %s", request.Method, request.URL)
			}
			if payload == nil {
				return nil, fmt.Errorf("unexpected local request path %s", request.URL.Path)
			}
			data, err := json.Marshal(payload)
			if err != nil {
				return nil, err
			}
			return &http.Response{
				StatusCode: http.StatusOK,
				Header:     make(http.Header),
				Request:    request,
				Body:       &tiBody{Reader: strings.NewReader(string(data)), closed: &closed},
			}, nil
		})

		userTweets, userCursor, err := scraper.FetchTweetsByUserID("u", 10, "")
		c.Check(err == nil, "FetchTweetsByUserID error: %v", err)
		c.Check(tiIDs(userTweets) == "401,412", "user timeline = %s", tiIDs(userTweets))
		c.Check(userCursor == "user-new", "user cursor = %q", userCursor)
		if len(userTweets) == 2 {
			c.Check(userTweets[0].Text == "long user text", "user note text = %q", userTweets[0].Text)
			c.Check(userTweets[0].Views == 99, "user views = %d", userTweets[0].Views)
			if userTweets[0].QuotedStatus != nil {
				c.Check(userTweets[0].QuotedStatus.ID == "451", "user quote id = %q", userTweets[0].QuotedStatus.ID)
				c.Check(userTweets[0].QuotedStatus.Text == "quoted user text", "user quote text = %q", userTweets[0].QuotedStatus.Text)
			} else {
				c.Check(false, "user quote was not parsed")
			}
			c.Check(userTweets[1].Text == "user replaced", "replaced user tweet = %q", userTweets[1].Text)
		}

		searchTweets, searchCursor, err := scraper.FetchSearchTweets("local", 10, "")
		c.Check(err == nil, "FetchSearchTweets error: %v", err)
		c.Check(tiIDs(searchTweets) == "501,502,503", "search timeline = %s", tiIDs(searchTweets))
		c.Check(searchCursor == "search-new", "search cursor = %q", searchCursor)
		if len(searchTweets) == 3 {
			c.Check(searchTweets[0].Text == "long search text", "search note text = %q", searchTweets[0].Text)
			if searchTweets[0].QuotedStatus != nil {
				c.Check(searchTweets[0].QuotedStatus.ID == "551", "search quote id = %q", searchTweets[0].QuotedStatus.ID)
			} else {
				c.Check(false, "search quote was not parsed")
			}
			c.Check(searchTweets[0].Views == 0, "unexpected search views: %d", searchTweets[0].Views)
			c.Check(searchTweets[1].Text == "wrapped search text", "wrapped search tweet = %q", searchTweets[1].Text)
			c.Check(searchTweets[2].Text == "module search text", "search module item = %q", searchTweets[2].Text)
		}

		thread, err := scraper.GetTweet("601")
		c.Check(err == nil, "GetTweet error: %v", err)
		if thread != nil {
			c.Check(thread.ID == "601", "thread root id = %q", thread.ID)
			c.Check(thread.IsSelfThread, "thread root is not a self thread")
			c.Check(len(thread.Thread) == 1, "thread children = %d", len(thread.Thread))
			if len(thread.Thread) == 1 {
				c.Check(thread.Thread[0].ID == "602", "thread child id = %q", thread.Thread[0].ID)
			}
		}

		// The replaced reply must not be reachable from the final conversation.
		replaced, err := scraper.GetTweet("603")
		c.Check(err != nil, "replaced tweet 603 is still present: %v", err)
		c.Check(replaced == nil, "replaced tweet 603 returned: %+v", replaced)

		c.Check(hits == 4, "local requests = %d, want 4", hits)
		c.Check(closed == 4, "closed bodies = %d, want 4", closed)
	})
}
