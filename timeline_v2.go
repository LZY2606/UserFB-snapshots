package twitterscraper

import (
	"strconv"
)

type result struct {
	Typename string  `json:"__typename"`
	Tweet    *result `json:"tweet"`
	Core     struct {
		UserResults struct {
			Result struct {
				IsBlueVerified bool       `json:"is_blue_verified"`
				Legacy         legacyUser `json:"legacy"`
			} `json:"result"`
		} `json:"user_results"`
	} `json:"core"`
	Views struct {
		Count string `json:"count"`
	} `json:"views"`
	NoteTweet struct {
		NoteTweetResults struct {
			Result struct {
				Text string `json:"text"`
			} `json:"result"`
		} `json:"note_tweet_results"`
	} `json:"note_tweet"`
	QuotedStatusResult struct {
		Result *result `json:"result"`
	} `json:"quoted_status_result"`
	Legacy legacyTweet `json:"legacy"`
}

// unwrap recursively removes result wrappers such as
// TweetWithVisibilityResults, which store the real result in the tweet field.
func (result *result) unwrap() *result {
	for result != nil && result.Tweet != nil && result.Typename != "Tweet" {
		result = result.Tweet
	}
	return result
}

func (result *result) parse() *Tweet {
	result = result.unwrap()
	if result == nil || result.Typename == "TweetUnavailable" || result.Typename == "TweetTombstone" {
		return nil
	}
	legacy := result.Legacy
	if result.NoteTweet.NoteTweetResults.Result.Text != "" {
		legacy.FullText = result.NoteTweet.NoteTweetResults.Result.Text
	}
	tw := parseLegacyTweet(&result.Core.UserResults.Result.Legacy, &legacy)
	if tw == nil {
		return nil
	}
	if tw.Views == 0 && result.Views.Count != "" {
		tw.Views, _ = strconv.Atoi(result.Views.Count)
	}
	if result.QuotedStatusResult.Result != nil {
		tw.QuotedStatus = result.QuotedStatusResult.Result.parse()
	}
	return tw
}

// itemContent is the shared content of a timeline entry or of a module item.
type itemContent struct {
	TweetDisplayType string `json:"tweetDisplayType"`
	TweetResults     struct {
		Result *result `json:"result"`
	} `json:"tweet_results"`
	UserDisplayType string `json:"userDisplayType"`
	UserResults     struct {
		Result struct {
			RestID string     `json:"rest_id"`
			Legacy legacyUser `json:"legacy"`
		} `json:"result"`
	} `json:"user_results"`
}

// moduleItem is a single item inside a TimelineTimelineModule entry.
type moduleItem struct {
	EntryID string `json:"entryId"`
	Item    struct {
		ItemContent itemContent `json:"itemContent"`
	} `json:"item"`
}

// entry is a timeline display slot.
type entry struct {
	EntryID string `json:"entryId"`
	Content struct {
		EntryType   string       `json:"entryType"`
		CursorType  string       `json:"cursorType"`
		Value       string       `json:"value"`
		Items       []moduleItem `json:"items"`
		ItemContent itemContent  `json:"itemContent"`
	} `json:"content"`
}

// timelineInstruction is a single instruction from any v2 timeline response.
type timelineInstruction struct {
	Type             string       `json:"type"`
	Entries          []entry      `json:"entries"`
	Entry            entry        `json:"entry"`
	EntryIDToReplace string       `json:"entryIdToReplace"`
	ModuleEntryID    string       `json:"moduleEntryId"`
	ModuleItems      []moduleItem `json:"moduleItems"`
}

// timelineV2 JSON object
type timelineV2 struct {
	Data struct {
		User struct {
			Result struct {
				TimelineV2 struct {
					Timeline struct {
						Instructions []timelineInstruction `json:"instructions"`
					} `json:"timeline"`
				} `json:"timeline_v2"`
			} `json:"result"`
		} `json:"user"`
	} `json:"data"`
}

func (timeline *timelineV2) parseTweets() ([]*Tweet, string) {
	slots := newTimelineSlots()
	slots.apply(timeline.Data.User.Result.TimelineV2.Timeline.Instructions)
	return slots.expand(false), slots.cursor()
}

// timelineSlot is the in-response state of one display identity.
type timelineSlot struct {
	entryID   string
	slotEntry entry
	items     []moduleItem
	itemIndex map[string]int
}

func (slot *timelineSlot) isModule() bool {
	return slot.slotEntry.Content.EntryType == "TimelineTimelineModule"
}

// resetContent replaces the slot content and rebuilds its module item order.
func (slot *timelineSlot) resetContent(slotEntry entry) {
	slot.slotEntry = slotEntry
	slot.items = nil
	slot.itemIndex = make(map[string]int)
	for _, item := range slotEntry.Content.Items {
		slot.addItem(item)
	}
}

// addItem appends a module item on first appearance and updates it in place on
// a repeated identity.
func (slot *timelineSlot) addItem(item moduleItem) {
	if item.EntryID == "" {
		return
	}
	if index, ok := slot.itemIndex[item.EntryID]; ok {
		slot.items[index] = item
		return
	}
	slot.itemIndex[item.EntryID] = len(slot.items)
	slot.items = append(slot.items, item)
}

// timelineSlots holds the ordered slots resulting from applying instructions.
type timelineSlots struct {
	slots []*timelineSlot
	index map[string]*timelineSlot
}

func newTimelineSlots() *timelineSlots {
	return &timelineSlots{index: make(map[string]*timelineSlot)}
}

func (timeline *timelineSlots) apply(instructions []timelineInstruction) {
	for _, instruction := range instructions {
		switch instruction.Type {
		case "TimelineAddEntries":
			for _, entry := range instruction.Entries {
				timeline.addEntry(entry)
			}
		case "TimelineReplaceEntry":
			timeline.replaceEntry(instruction.EntryIDToReplace, instruction.Entry)
		case "TimelineAddToModule":
			timeline.addToModule(instruction.ModuleEntryID, instruction.ModuleItems)
		}
	}
}

func (timeline *timelineSlots) addEntry(entry entry) {
	if entry.EntryID == "" {
		return
	}
	if slot, ok := timeline.index[entry.EntryID]; ok {
		slot.resetContent(entry)
		return
	}
	slot := &timelineSlot{entryID: entry.EntryID}
	slot.resetContent(entry)
	timeline.index[entry.EntryID] = slot
	timeline.slots = append(timeline.slots, slot)
}

func (timeline *timelineSlots) replaceEntry(entryIDToReplace string, replacement entry) {
	if entryIDToReplace == "" || replacement.EntryID == "" {
		return
	}
	slot, ok := timeline.index[entryIDToReplace]
	if !ok {
		return
	}
	if replacement.EntryID != entryIDToReplace {
		if _, exists := timeline.index[replacement.EntryID]; exists {
			return
		}
	}
	delete(timeline.index, entryIDToReplace)
	slot.entryID = replacement.EntryID
	slot.resetContent(replacement)
	timeline.index[replacement.EntryID] = slot
}

func (timeline *timelineSlots) addToModule(moduleEntryID string, items []moduleItem) {
	if moduleEntryID == "" {
		return
	}
	slot, ok := timeline.index[moduleEntryID]
	if !ok || !slot.isModule() {
		return
	}
	for _, item := range items {
		slot.addItem(item)
	}
}

// expand turns the final slots into tweets in display order. Module items are
// expanded in their own order and normal slots expose their entry content.
func (timeline *timelineSlots) expand(selfThread bool) []*Tweet {
	var tweets []*Tweet
	for _, slot := range timeline.slots {
		if slot.isModule() {
			for index := range slot.items {
				if tweet := parseItemContent(&slot.items[index].Item.ItemContent, selfThread); tweet != nil {
					tweets = append(tweets, tweet)
				}
			}
			continue
		}
		if tweet := parseItemContent(&slot.slotEntry.Content.ItemContent, selfThread); tweet != nil {
			tweets = append(tweets, tweet)
		}
	}
	return tweets
}

// cursor returns the value of the last Bottom cursor among the final slots.
func (timeline *timelineSlots) cursor() string {
	var cursor string
	for _, slot := range timeline.slots {
		if slot.slotEntry.Content.CursorType == "Bottom" {
			cursor = slot.slotEntry.Content.Value
		}
	}
	return cursor
}

func parseItemContent(content *itemContent, selfThread bool) *Tweet {
	if content.TweetResults.Result == nil {
		return nil
	}
	tweet := content.TweetResults.Result.parse()
	if tweet == nil {
		return nil
	}
	if selfThread && content.TweetDisplayType == "SelfThread" {
		tweet.IsSelfThread = true
	}
	return tweet
}

type threadedConversation struct {
	Data struct {
		ThreadedConversationWithInjectionsV2 struct {
			Instructions []timelineInstruction `json:"instructions"`
		} `json:"threaded_conversation_with_injections_v2"`
	} `json:"data"`
}

func (conversation *threadedConversation) parse() []*Tweet {
	slots := newTimelineSlots()
	slots.apply(conversation.Data.ThreadedConversationWithInjectionsV2.Instructions)
	tweets := slots.expand(true)
	for _, tweet := range tweets {
		if tweet.InReplyToStatusID != "" {
			for _, parentTweet := range tweets {
				if parentTweet.ID == tweet.InReplyToStatusID {
					tweet.InReplyToStatus = parentTweet
					break
				}
			}
		}
		if tweet.IsSelfThread && tweet.ConversationID == tweet.ID {
			for _, childTweet := range tweets {
				if childTweet.IsSelfThread && childTweet.ID != tweet.ID {
					tweet.Thread = append(tweet.Thread, childTweet)
				}
			}
			if len(tweet.Thread) == 0 {
				tweet.IsSelfThread = false
			}
		}
	}
	return tweets
}
