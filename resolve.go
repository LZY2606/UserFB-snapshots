package ssh_config

import "strings"

// ResolveContext supplies the offline context used to evaluate an extended
// configuration. No value is derived from the local machine: callers provide
// every field explicitly.
//
// CanonicalHost is the result of canonicalization (for example, after applying
// a CanonicalizeHostname rule followed by DNS resolution). An empty
// CanonicalHost means no canonical result is available, which suppresses the
// canonical condition.
type ResolveContext struct {
	// OriginalHost is the alias exactly as the caller supplied it. Host blocks
	// and the originalhost condition match against it.
	OriginalHost string
	// Host is the host name used for the host condition during the normal
	// resolution round.
	Host string
	// CanonicalHost is the canonicalized host name. When non-empty it is used
	// for Host blocks and the host condition during the final round.
	CanonicalHost string
	// RemoteUser is the user name requested for the remote host. The user
	// condition matches it directly; it is never inferred from configuration.
	RemoteUser string
	// LocalUser is the name of the user running the client. The localuser
	// condition matches it.
	LocalUser string
}

// ResolveResult holds the values collected by Resolve. Values are keyword
// case-insensitive at lookup time. Missing keywords return the empty value or
// nil; system defaults are never filled in.
type ResolveResult struct {
	values map[string]string
	all    map[string][]string
}

// Get returns the first value collected for keyword, or the empty string if
// none was found. Keyword matching is case-insensitive. The first value is
// retained across resolution rounds: a later, more specific round does not
// overwrite it.
func (r *ResolveResult) Get(keyword string) string {
	if r == nil {
		return ""
	}
	return r.values[strings.ToLower(keyword)]
}

// GetAll returns a copy of every value collected for keyword, in the order the
// matching configuration directives appeared. A directive (configuration line)
// that matches in both resolution rounds contributes once. It returns nil if
// none were found. Keyword matching is case-insensitive.
//
// The returned slice is a copy: it can be modified without affecting the
// configuration tree or any other query.
func (r *ResolveResult) GetAll(keyword string) []string {
	if r == nil {
		return nil
	}
	vals := r.all[strings.ToLower(keyword)]
	if len(vals) == 0 {
		return nil
	}
	out := make([]string, len(vals))
	copy(out, vals)
	return out
}

// Values returns a copy of all first values collected by Resolve, keyed by
// lower-case keyword.
func (r *ResolveResult) Values() map[string]string {
	if r == nil {
		return nil
	}
	out := make(map[string]string, len(r.values))
	for k, v := range r.values {
		out[k] = v
	}
	return out
}

// resolveRound describes the phase state for match condition evaluation.
type resolveRound struct {
	final     bool
	canonical bool
	// originalHost is the original alias, used by original-syntax Match
	// blocks, which the non-extended parser only matched against the alias.
	originalHost string
	// hostBlock is the name Host declarations match: the original alias in
	// the normal round; the canonical name (falling back to ctx.Host) in the
	// final round.
	hostBlock string
	// host is the name extended host conditions match: ctx.Host in the normal
	// round; the canonical name (falling back to ctx.Host) in the final round.
	host string
}

// Resolve evaluates the configuration against ctx. It never executes ssh,
// runs Match Exec commands, performs DNS lookups, or reads local account
// information.
//
// Resolution runs a normal round in which Host blocks match ctx.OriginalHost
// and Match host conditions match ctx.Host. If ctx.CanonicalHost is non-empty,
// or if any final condition appears anywhere in the configuration (including
// included files), a final round also runs: Host blocks and the host condition
// use ctx.CanonicalHost when non-empty and ctx.Host otherwise, the canonical
// condition holds only when a canonical name was supplied, and the final
// condition holds. The originalhost condition always matches
// ctx.OriginalHost.
//
// The first value found for an ordinary keyword across both rounds is kept and
// never overwritten by a later, more specific round. IdentityFile values
// accumulate in configuration order: the same directive line matching in both
// rounds contributes once, while different directive lines contribute even if
// they hold the same value.
func (c *Config) Resolve(ctx ResolveContext) *ResolveResult {
	res := &ResolveResult{
		values: make(map[string]string),
		all:    make(map[string][]string),
	}
	if c == nil {
		return res
	}
	finalRequested := c.hasFinalCriterion()

	// seenDirectives tracks configuration lines (KV nodes) that already
	// contributed, so a line matching in both rounds is only added once. Each
	// Resolve call gets its own map, keeping resolutions independent.
	seenDirectives := make(map[*KV]bool)
	c.resolveRound(ctx, res, resolveRound{
		final:        false,
		originalHost: ctx.OriginalHost,
		hostBlock:    ctx.OriginalHost,
		host:         ctx.Host,
	}, seenDirectives)
	if ctx.CanonicalHost != "" || finalRequested {
		roundHost := ctx.Host
		if ctx.CanonicalHost != "" {
			roundHost = ctx.CanonicalHost
		}
		c.resolveRound(ctx, res, resolveRound{
			final:        true,
			canonical:    ctx.CanonicalHost != "",
			originalHost: ctx.OriginalHost,
			hostBlock:    roundHost,
			host:         roundHost,
		}, seenDirectives)
	}
	return res
}

// resolveRound walks one round of the configuration tree. seen tracks directive
// identities that already contributed, so a configuration line is only added
// once across rounds.
func (c *Config) resolveRound(ctx ResolveContext, res *ResolveResult, round resolveRound, seen map[*KV]bool) {
	if seen == nil {
		seen = make(map[*KV]bool)
	}
	for _, h := range c.Hosts {
		var matches bool
		if h.extendedMatch {
			matches = matchCriteria(h.matchCriteria, ctx, round.host, round)
		} else if h.isMatch {
			// Original-syntax Host patterns were matched against the original
			// alias by the non-extended parser; extended resolution gives
			// them the same behavior ("Match all" matches every host).
			matches = h.Matches(round.originalHost)
		} else {
			// Host declarations match the round's host block name: the
			// original alias in the normal round, the canonical name in the
			// final round.
			matches = h.Matches(round.hostBlock)
		}
		if !matches {
			continue
		}
		for _, node := range h.Nodes {
			switch t := node.(type) {
			case *Empty:
				continue
			case *KV:
				key := strings.ToLower(t.Key)
				if !seen[t] {
					seen[t] = true
					res.all[key] = append(res.all[key], t.Value)
				}
				// First value wins across rounds; later rounds never
				// overwrite an already-collected keyword.
				if _, ok := res.values[key]; !ok {
					res.values[key] = t.Value
				}
			case *Include:
				t.resolveInclude(ctx, res, round, seen)
			default:
				// Unknown node kinds do not contribute values.
			}
		}
	}
}

// resolveInclude evaluates the files of an Include directive in order. Each
// file is evaluated with a fresh Host/Match state: a Match at the end of a
// child file cannot affect the parent's subsequent lines, and the parent's
// matching state is restored when the child returns.
func (inc *Include) resolveInclude(ctx ResolveContext, res *ResolveResult, round resolveRound, seen map[*KV]bool) {
	inc.mu.Lock()
	defer inc.mu.Unlock()
	for i := range inc.matches {
		cfg := inc.files[inc.matches[i]]
		if cfg == nil {
			continue
		}
		// Each included file is evaluated with its own Host/Match state; the
		// caller's state is restored when it returns.
		cfg.resolveRound(ctx, res, round, seen)
	}
}
