package core

import (
	"crypto/sha256"
	"encoding/hex"
	"net/url"
	"strings"
	"sync"
	"time"

	"github.com/go-rod/rod/lib/proto"
	browserprofile "github.com/karust/openserp/core/browser"
)

const DefaultProxyLaneMaxLanes = 100

type ProxyLanesConfig struct {
	Enabled                bool `json:"enabled" mapstructure:"enabled"`
	MaxLanes               int  `json:"max_lanes" mapstructure:"max_lanes"`
	DropCookiesOnChallenge bool `json:"drop_cookies_on_challenge" mapstructure:"drop_cookies_on_challenge"`
}

type ProxyLaneKey struct {
	Tenant    string
	Engine    string
	SessionID string
}

type LaneStats struct {
	Active         int `json:"active"`
	EvictedLRU     int `json:"evicted_lru"`
	CookiesDropped int `json:"cookies_dropped"`
}

// BrowserPoolStats describes the live state of the per-process browser pool that
// keeps one Chrome per authenticated upstream proxy identity. Reported via
// /stats/proxy as `browser_processes`.
type BrowserPoolStats struct {
	Active      int `json:"active"`
	Max         int `json:"max"`
	EvictedLRU  int `json:"evicted_lru"`
	EvictedIdle int `json:"evicted_idle"`
}

type laneState struct {
	Key     ProxyLaneKey
	Profile browserprofile.Profile
	Cookies map[cookieKey]*proto.NetworkCookie
	// generation fences off page snapshots taken before the last full replace,
	// cookie drop, or eviction. It is drawn from the store-wide counter so a
	// recreated lane never reuses a previous generation.
	Generation uint64
	LastUsedAt time.Time
}

type LaneStore struct {
	mu             sync.Mutex
	maxLanes       int
	lanes          map[ProxyLaneKey]*laneState
	nextGeneration uint64
	evictedLRU     int
	cookiesDropped int
}

func DefaultProxyLanesConfig() ProxyLanesConfig {
	return ProxyLanesConfig{
		Enabled:                true,
		MaxLanes:               DefaultProxyLaneMaxLanes,
		DropCookiesOnChallenge: true,
	}
}

func NormalizeProxyLanesConfig(cfg ProxyLanesConfig) ProxyLanesConfig {
	if cfg.MaxLanes <= 0 {
		cfg.MaxLanes = DefaultProxyLaneMaxLanes
	}
	return cfg
}

func NewLaneStore(maxLanes int) *LaneStore {
	if maxLanes <= 0 {
		maxLanes = DefaultProxyLaneMaxLanes
	}
	return &LaneStore{
		maxLanes:       maxLanes,
		lanes:          map[ProxyLaneKey]*laneState{},
		nextGeneration: 1,
	}
}

func (s *LaneStore) bumpGenerationLocked() uint64 {
	generation := s.nextGeneration
	s.nextGeneration++
	return generation
}

func cookieMap(cookies []*proto.NetworkCookie) map[cookieKey]*proto.NetworkCookie {
	if len(cookies) == 0 {
		return map[cookieKey]*proto.NetworkCookie{}
	}
	out := make(map[cookieKey]*proto.NetworkCookie, len(cookies))
	for _, cookie := range cookies {
		if cookie == nil {
			continue
		}
		out[cookieIdentity(cookie)] = cloneCookie(cookie)
	}
	return out
}

func (s *LaneStore) Profile(key ProxyLaneKey, create func() browserprofile.Profile) browserprofile.Profile {
	if s == nil || key.Empty() {
		if create == nil {
			return browserprofile.Profile{}
		}
		return create()
	}

	now := time.Now()
	s.mu.Lock()
	defer s.mu.Unlock()

	if state, ok := s.lanes[key]; ok {
		state.LastUsedAt = now
		return state.Profile
	}

	profile := browserprofile.Profile{}
	if create != nil {
		profile = create()
	}
	s.lanes[key] = &laneState{
		Key:        key,
		Profile:    profile,
		Cookies:    map[cookieKey]*proto.NetworkCookie{},
		Generation: s.bumpGenerationLocked(),
		LastUsedAt: now,
	}
	s.evictLRULocked()
	return profile
}

func (s *LaneStore) Cookies(key ProxyLaneKey) []*proto.NetworkCookie {
	if s == nil || key.Empty() {
		return nil
	}

	s.mu.Lock()
	defer s.mu.Unlock()

	state, ok := s.lanes[key]
	if !ok {
		return nil
	}
	state.LastUsedAt = time.Now()
	return cookiesFromMap(state.Cookies)
}

// beginPage returns an isolated snapshot for a page starting on this lane.
// The returned session only commits while its generation remains current.
func (s *LaneStore) beginPage(key ProxyLaneKey) *lanePageSession {
	if s == nil || key.Empty() {
		return nil
	}

	s.mu.Lock()
	defer s.mu.Unlock()

	state, ok := s.lanes[key]
	if !ok {
		state = &laneState{
			Key:        key,
			Cookies:    map[cookieKey]*proto.NetworkCookie{},
			Generation: s.bumpGenerationLocked(),
			LastUsedAt: time.Now(),
		}
		s.lanes[key] = state
		s.evictLRULocked()
		// Eviction could in theory remove the just-created lane when the
		// capacity is zero-derived; the capacity is always >= 1 here, so the
		// new entry survives. Re-read defensively anyway.
		state, ok = s.lanes[key]
		if !ok {
			return nil
		}
	}

	snapshot := make(map[cookieKey]*proto.NetworkCookie, len(state.Cookies))
	for identity, cookie := range state.Cookies {
		snapshot[identity] = cloneCookie(cookie)
	}
	state.LastUsedAt = time.Now()
	return &lanePageSession{
		store:      s,
		key:        key,
		generation: state.Generation,
		snapshot:   snapshot,
	}
}

// commitPage merges only the page's own delta (the diff between its starting
// snapshot and the final context cookies) into the lane. Stale sessions -
// dropped, fully replaced, or evicted-and-recreated lanes - are rejected
// without recreating the lane or touching any other lane.
func (s *LaneStore) commitPage(session *lanePageSession, finalCookies []*proto.NetworkCookie) bool {
	if s == nil || session == nil || session.key.Empty() {
		return false
	}

	final := cookieMap(finalCookies)

	s.mu.Lock()
	defer s.mu.Unlock()

	state, ok := s.lanes[session.key]
	if !ok || state.Generation != session.generation {
		return false
	}

	merged := make(map[cookieKey]*proto.NetworkCookie, len(state.Cookies))
	for identity, cookie := range state.Cookies {
		merged[identity] = cloneCookie(cookie)
	}

	// Edits observed by this page: added entries, changed entries, and entries
	// present in the snapshot but gone from the final context (deleted).
	for identity, start := range session.snapshot {
		current, exists := final[identity]
		if !exists {
			delete(merged, identity)
			continue
		}
		if !cookieRestorableEqual(start, current) {
			merged[identity] = cloneCookie(current)
		}
	}
	for identity, cookie := range final {
		if _, inherited := session.snapshot[identity]; inherited {
			continue
		}
		merged[identity] = cloneCookie(cookie)
	}

	state.Cookies = merged
	state.LastUsedAt = time.Now()
	return true
}

// SaveCookies replaces the whole lane state on behalf of the manager and
// invalidates every page snapshot taken before it.
func (s *LaneStore) SaveCookies(key ProxyLaneKey, cookies []*proto.NetworkCookie) {
	if s == nil || key.Empty() {
		return
	}

	s.mu.Lock()
	defer s.mu.Unlock()

	state, ok := s.lanes[key]
	if !ok {
		state = &laneState{Key: key}
		s.lanes[key] = state
	}
	state.Cookies = cookieMap(cookies)
	state.Generation = s.bumpGenerationLocked()
	state.LastUsedAt = time.Now()
	s.evictLRULocked()
}

// DropCookies clears the lane's cookies and fences off all pages that already
// started, even when no cookies were currently stored. The profile survives.
func (s *LaneStore) DropCookies(key ProxyLaneKey) {
	if s == nil || key.Empty() {
		return
	}

	s.mu.Lock()
	defer s.mu.Unlock()

	state, ok := s.lanes[key]
	if !ok {
		return
	}
	if len(state.Cookies) > 0 {
		s.cookiesDropped++
	}
	state.Cookies = map[cookieKey]*proto.NetworkCookie{}
	state.Generation = s.bumpGenerationLocked()
	state.LastUsedAt = time.Now()
}

func (s *LaneStore) Stats() LaneStats {
	if s == nil {
		return LaneStats{}
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	return LaneStats{
		Active:         len(s.lanes),
		EvictedLRU:     s.evictedLRU,
		CookiesDropped: s.cookiesDropped,
	}
}

func (s *LaneStore) evictLRULocked() {
	for len(s.lanes) > s.maxLanes {
		var (
			oldestKey ProxyLaneKey
			oldest    time.Time
			hasOldest bool
		)
		for key, state := range s.lanes {
			if !hasOldest || state.LastUsedAt.Before(oldest) {
				oldestKey = key
				oldest = state.LastUsedAt
				hasOldest = true
			}
		}
		if !hasOldest {
			return
		}
		delete(s.lanes, oldestKey)
		s.evictedLRU++
	}
}

func (k ProxyLaneKey) Empty() bool {
	return strings.TrimSpace(k.Engine) == "" || strings.TrimSpace(k.SessionID) == ""
}

func (k ProxyLaneKey) ID() string {
	k = NormalizeProxyLaneKey(k)
	if k.Empty() {
		return ""
	}
	if k.Tenant != "" {
		return k.Tenant + ":" + k.Engine + ":" + k.SessionID
	}
	return k.Engine + ":" + k.SessionID
}

func NormalizeProxyLaneKey(key ProxyLaneKey) ProxyLaneKey {
	return ProxyLaneKey{
		Tenant:    strings.TrimSpace(key.Tenant),
		Engine:    normalizeEngineName(key.Engine),
		SessionID: strings.TrimSpace(key.SessionID),
	}
}

func ProxyLaneKeyForTenant(engine string, tenant string, q Query, proxyURL string) ProxyLaneKey {
	sessionID := strings.TrimSpace(q.ProxySessionID)
	if sessionID == "" {
		sessionID = proxyLaneIDFromProxyURL(proxyURL)
	}
	if sessionID == "" {
		sessionID = "direct"
	}
	return NormalizeProxyLaneKey(ProxyLaneKey{Tenant: tenant, Engine: engine, SessionID: sessionID})
}

func proxyLaneIDFromProxyURL(raw string) string {
	normalized, err := NormalizeProxyURL(raw)
	if err != nil || normalized == "" {
		return ""
	}
	parsed, err := url.Parse(normalized)
	if err != nil {
		return ""
	}
	userInfo := ""
	if parsed.User != nil {
		userInfo = parsed.User.String()
	}
	sum := sha256.Sum256([]byte(parsed.Scheme + "|" + parsed.Host + "|" + userInfo))
	return hex.EncodeToString(sum[:])[:16]
}

func cookieParams(cookies []*proto.NetworkCookie) []*proto.NetworkCookieParam {
	if len(cookies) == 0 {
		return nil
	}
	params := make([]*proto.NetworkCookieParam, 0, len(cookies))
	for _, cookie := range cookies {
		if cookie == nil {
			continue
		}
		sourcePort := cookie.SourcePort
		params = append(params, &proto.NetworkCookieParam{
			Name:         cookie.Name,
			Value:        cookie.Value,
			Domain:       cookie.Domain,
			Path:         cookie.Path,
			Secure:       cookie.Secure,
			HTTPOnly:     cookie.HTTPOnly,
			SameSite:     cookie.SameSite,
			Expires:      cookie.Expires,
			Priority:     cookie.Priority,
			SameParty:    cookie.SameParty,
			SourceScheme: cookie.SourceScheme,
			SourcePort:   &sourcePort,
			PartitionKey: cookie.PartitionKey,
		})
	}
	return params
}
