package xmlquery

import (
	"bufio"
	"bytes"
	"encoding/xml"
	"fmt"
	"io"
	"sort"
	"strings"

	"golang.org/x/net/html/charset"
)

// RecordRule names a record extraction rule for RecordStream. Path is a
// restricted XPath-like expression selecting the elements to capture:
//
//	/catalog//p:item[@kind='part']
//
// Each step is an element QName or '*', separated by '/' (child of the
// previous step) or '//' (any descendant of the previous step). The path
// must start with '/' (child of the document) or '//' (any descendant of
// the document). A step may carry a single attribute equality predicate
// [@name='value']. Prefixed names are resolved through the namespaces map
// given to NewRecordStream; the alias used by the document itself is
// irrelevant. Unprefixed element names only match elements without a
// namespace. Text predicates, positional predicates, functions, unions and
// other axes are not supported and are rejected by NewRecordStream.
type RecordRule struct {
	Name string
	Path string
}

// RecordMatch is a single record delivered by RecordStream.Read. Rule is
// the name of the RecordRule that matched. Node is the captured element as
// an independent tree (its Parent, PrevSibling and NextSibling are nil);
// the in-scope namespace declarations of the original document are
// repeated on Node so Node.OutputXML(true) reparses to the same expanded
// names. Location is the 1-based position of the element among its element
// siblings, from the document element downwards: the document element is
// [1], its second element child is [1,2], and so on. Only elements are
// counted, and the numbering never changes as the stream advances.
type RecordMatch struct {
	Rule     string
	Node     *Node
	Location []int
}

// recordAttrCond is a single [@name='value'] predicate on a path step.
type recordAttrCond struct {
	uri   string
	local string
	value string
}

// recordStep is one compiled path step.
type recordStep struct {
	descendant bool // reached via '//' instead of '/'
	any        bool // '*'
	uri        string
	local      string
	attr       *recordAttrCond
}

type compiledRecordRule struct {
	name  string
	steps []recordStep
}

// recordCapture remembers, for one open element, everything needed to
// build the records once the element closes.
type recordCapture struct {
	rules    []int             // indexes into RecordStream.rules, in rule order
	location []int             // element position, frozen at the start tag
	scope    map[string]string // namespace URI -> prefix, in scope at the element
	xmlSpace string            // nearest inherited xml:space value
	hasSpace bool
}

type recordFrame struct {
	node    *Node
	capture *recordCapture
}

// RecordStream extracts nested, independently usable records from an XML
// stream. Unlike StreamParser it can deliver an element and, later, any of
// its ancestors that also match a rule: matching is decided when the start
// tag is complete, and each record is delivered as its own detached tree
// when the corresponding end tag is reached. Records therefore come out in
// closing order (inner elements before outer ones), and a node matching
// several rules is delivered once per rule, in rule order.
//
// RecordStream reads the input incrementally: it never reads to EOF before
// returning, never seeks, and only keeps the currently open ancestors, the
// subtrees still being captured, and the records waiting to be delivered
// at the current closing point. Closed subtrees that did not match are
// released as soon as their end tag is seen.
type RecordStream struct {
	decoder      *xml.Decoder
	reader       *cachedReader
	doc          *Node
	stack        []recordFrame
	counts       []int
	rules        []compiledRecordRule
	space2prefix map[string]*xmlnsPrefix
	prefixScopes []map[string]*xmlnsPrefix
	ready        []*RecordMatch
	termErr      error
}

// NewRecordStream creates a RecordStream reading from r. All configuration
// errors (empty or duplicate rule names, unsupported paths, unknown
// namespace prefixes) are reported before anything is read from r. The
// rules and namespaces arguments are copied: mutating them afterwards does
// not affect the stream.
func NewRecordStream(r io.Reader, rules []RecordRule, namespaces map[string]string) (*RecordStream, error) {
	if r == nil {
		return nil, fmt.Errorf("xmlquery: nil reader")
	}
	ns := make(map[string]string, len(namespaces))
	for prefix, uri := range namespaces {
		ns[prefix] = uri
	}
	seen := make(map[string]bool, len(rules))
	compiled := make([]compiledRecordRule, 0, len(rules))
	for _, rule := range rules {
		if rule.Name == "" {
			return nil, fmt.Errorf("xmlquery: record rule name must not be empty")
		}
		if seen[rule.Name] {
			return nil, fmt.Errorf("xmlquery: duplicate record rule name %q", rule.Name)
		}
		seen[rule.Name] = true
		steps, err := parseRecordPath(rule.Path, ns)
		if err != nil {
			return nil, err
		}
		compiled = append(compiled, compiledRecordRule{name: rule.Name, steps: steps})
	}
	reader := newCachedReader(bufio.NewReader(r))
	decoder := xml.NewDecoder(reader)
	if decoder.CharsetReader == nil {
		decoder.CharsetReader = charset.NewReaderLabel
	}
	return &RecordStream{
		decoder: decoder,
		reader:  reader,
		doc:     &Node{Type: DocumentNode},
		rules:   compiled,
		space2prefix: map[string]*xmlnsPrefix{
			"http://www.w3.org/XML/1998/namespace": {name: "xml", level: 0},
		},
	}, nil
}

// Read returns the next matched record. Records are delivered in closing
// order of the matched elements; an element matching several rules yields
// one record per rule, in the order the rules were given to
// NewRecordStream. Once the input is exhausted Read returns io.EOF. If the
// XML or the underlying reader fails, records already delivered stay
// valid, records whose element was not fully read are dropped, and every
// subsequent Read returns the same terminal error.
func (rs *RecordStream) Read() (*RecordMatch, error) {
	if rs.termErr != nil {
		return nil, rs.termErr
	}
	for len(rs.ready) == 0 {
		if err := rs.advance(); err != nil {
			rs.termErr = err
			return nil, err
		}
	}
	m := rs.ready[0]
	rs.ready[0] = nil
	rs.ready = rs.ready[1:]
	return m, nil
}

// advance consumes a single XML token.
func (rs *RecordStream) advance() error {
	rs.reader.StartCaching()
	tok, err := rs.decoder.Token()
	rs.reader.StopCaching()
	if err != nil {
		return err
	}
	switch tok := tok.(type) {
	case xml.StartElement:
		return rs.startElement(tok)
	case xml.EndElement:
		rs.endElement()
	case xml.CharData:
		// Normalize the cache to detect CDATA sections.
		cached := bytes.ToUpper(rs.reader.CacheWithLimit(9))
		nodeType := TextNode
		if bytes.HasPrefix(cached, []byte("<![CDATA[")) || bytes.HasPrefix(cached, []byte("![CDATA[")) {
			nodeType = CharDataNode
		}
		rs.attach(&Node{Type: nodeType, Data: string(tok), level: len(rs.stack) + 1})
	case xml.Comment:
		rs.attach(&Node{Type: CommentNode, Data: string(tok), level: len(rs.stack) + 1})
	case xml.ProcInst:
		node := &Node{Type: DeclarationNode, Data: tok.Target, level: len(rs.stack) + 1}
		pairs := strings.Split(string(tok.Inst), " ")
		for _, pair := range pairs {
			pair = strings.TrimSpace(pair)
			if i := strings.Index(pair, "="); i > 0 {
				AddAttr(node, pair[:i], strings.Trim(pair[i+1:], `"'`))
			}
		}
		if tok.Target != "xml" {
			node.Type = ProcessingInstruction
			node.ProcInst = &ProcInstData{Target: tok.Target, Inst: strings.TrimSpace(string(tok.Inst))}
		}
		rs.attach(node)
	case xml.Directive:
		rs.attach(&Node{Type: NotationNode, Data: string(tok), level: len(rs.stack) + 1})
	}
	return nil
}

// attach appends a non-element node to the current open element (or to the
// document when no element is open).
func (rs *RecordStream) attach(n *Node) {
	parent := rs.doc
	if len(rs.stack) > 0 {
		parent = rs.stack[len(rs.stack)-1].node
	}
	AddChild(parent, n)
}

func (rs *RecordStream) startElement(tok xml.StartElement) error {
	// Namespace aliases declared by this element apply only in its subtree.
	var previous map[string]*xmlnsPrefix
	remember := func(space string) {
		if previous == nil {
			previous = make(map[string]*xmlnsPrefix)
		}
		if _, recorded := previous[space]; !recorded {
			previous[space] = rs.space2prefix[space]
		}
	}
	for _, att := range tok.Attr {
		if att.Name.Local == "xmlns" {
			if prefix, ok := rs.space2prefix[att.Value]; !ok || (ok && prefix.level >= len(rs.stack)) {
				remember(att.Value)
				rs.space2prefix[att.Value] = &xmlnsPrefix{name: "", level: len(rs.stack)}
			}
		} else if att.Name.Space == "xmlns" {
			remember(att.Value)
			rs.space2prefix[att.Value] = &xmlnsPrefix{name: att.Name.Local, level: len(rs.stack)}
		}
	}
	rs.prefixScopes = append(rs.prefixScopes, previous)

	if space := tok.Name.Space; space != "" {
		if _, found := rs.space2prefix[space]; !found && rs.decoder.Strict {
			return fmt.Errorf("xmlquery: invalid XML document, namespace %s is missing", space)
		}
	}

	attributes := make([]Attr, len(tok.Attr))
	for i, att := range tok.Attr {
		name := att.Name
		if prefix, ok := rs.space2prefix[name.Space]; ok {
			name.Space = prefix.name
		}
		attributes[i] = Attr{
			Name:         name,
			Value:        att.Value,
			NamespaceURI: att.Name.Space,
		}
	}

	node := &Node{
		Type:         ElementNode,
		Data:         tok.Name.Local,
		NamespaceURI: tok.Name.Space,
		Attr:         attributes,
		level:        len(rs.stack) + 1,
	}
	rs.attach(node)

	if node.NamespaceURI != "" {
		if v, ok := rs.space2prefix[node.NamespaceURI]; ok {
			cached := string(rs.reader.CacheWithLimit(len(v.name) + len(node.Data) + 2))
			if strings.HasPrefix(cached, fmt.Sprintf("%s:%s", v.name, node.Data)) || strings.HasPrefix(cached, fmt.Sprintf("<%s:%s", v.name, node.Data)) {
				node.Prefix = v.name
			}
		}
	}

	// Position among element siblings, counting every element whether or
	// not it matches, and never renumbering released subtrees.
	depth := len(rs.stack) + 1
	for len(rs.counts) < depth {
		rs.counts = append(rs.counts, 0)
	}
	rs.counts = rs.counts[:depth]
	rs.counts[depth-1]++

	// Whether the element matches is fully decided by its start tag.
	var capture *recordCapture
	for i := range rs.rules {
		if matchRecordSteps(rs.rules[i].steps, node) {
			if capture == nil {
				capture = &recordCapture{
					location: append([]int(nil), rs.counts...),
					scope:    snapshotRecordScope(rs.space2prefix),
				}
				capture.xmlSpace, capture.hasSpace = inheritedXMLSpace(node)
			}
			capture.rules = append(capture.rules, i)
		}
	}
	rs.stack = append(rs.stack, recordFrame{node: node, capture: capture})
	return nil
}

func (rs *RecordStream) endElement() {
	if last := len(rs.prefixScopes) - 1; last >= 0 {
		for space, prefix := range rs.prefixScopes[last] {
			if prefix == nil {
				delete(rs.space2prefix, space)
			} else {
				rs.space2prefix[space] = prefix
			}
		}
		rs.prefixScopes[last] = nil
		rs.prefixScopes = rs.prefixScopes[:last]
	}
	top := rs.stack[len(rs.stack)-1]
	rs.stack[len(rs.stack)-1] = recordFrame{}
	rs.stack = rs.stack[:len(rs.stack)-1]
	if top.capture != nil {
		for _, idx := range top.capture.rules {
			rs.ready = append(rs.ready, buildRecordMatch(rs.rules[idx].name, top.node, top.capture))
		}
	}
	// Release the closed subtree from the live tree, unless an open
	// ancestor is still being captured: then this subtree is part of
	// that pending record and must stay until the ancestor closes.
	// Records were built from independent copies, so nothing delivered
	// refers to the released nodes.
	for i := range rs.stack {
		if rs.stack[i].capture != nil {
			return
		}
	}
	RemoveFromTree(top.node)
}

// matchRecordSteps reports whether node satisfies the compiled path: the
// last step must match node itself, and the remaining steps must match its
// ancestors according to their child/descendant separators.
func matchRecordSteps(steps []recordStep, node *Node) bool {
	return matchRecordStepAt(steps, len(steps)-1, node)
}

func matchRecordStepAt(steps []recordStep, i int, node *Node) bool {
	if node == nil || node.Type != ElementNode {
		return false
	}
	step := steps[i]
	if !step.any {
		if node.Data != step.local || node.NamespaceURI != step.uri {
			return false
		}
	}
	if step.attr != nil && !hasRecordAttr(node, step.attr) {
		return false
	}
	if i == 0 {
		if step.descendant {
			return true
		}
		return node.Parent != nil && node.Parent.Type == DocumentNode
	}
	if step.descendant {
		for p := node.Parent; p != nil && p.Type == ElementNode; p = p.Parent {
			if matchRecordStepAt(steps, i-1, p) {
				return true
			}
		}
		return false
	}
	return matchRecordStepAt(steps, i-1, node.Parent)
}

func hasRecordAttr(node *Node, cond *recordAttrCond) bool {
	for _, attr := range node.Attr {
		if attr.Name.Local == cond.local && attr.NamespaceURI == cond.uri && attr.Value == cond.value {
			return true
		}
	}
	return false
}

// snapshotRecordScope copies the in-scope namespace URI -> prefix mapping.
func snapshotRecordScope(m map[string]*xmlnsPrefix) map[string]string {
	out := make(map[string]string, len(m))
	for uri, prefix := range m {
		out[uri] = prefix.name
	}
	return out
}

// inheritedXMLSpace finds the nearest xml:space value among the open
// ancestors of node.
func inheritedXMLSpace(node *Node) (string, bool) {
	for p := node.Parent; p != nil && p.Type == ElementNode; p = p.Parent {
		if v := p.SelectAttr("xml:space"); v != "" {
			return v, true
		}
	}
	return "", false
}

// buildRecordMatch copies the matched subtree into a standalone tree and
// restores the namespace and xml:space context the subtree inherited from
// ancestors that are not part of the record.
func buildRecordMatch(rule string, src *Node, capture *recordCapture) *RecordMatch {
	root := copyRecordNode(src, src.level)
	addRecordScopeDeclarations(root, capture.scope)
	if capture.hasSpace && root.SelectAttr("xml:space") == "" {
		root.Attr = append(root.Attr, Attr{
			Name:         xml.Name{Space: "xml", Local: "space"},
			Value:        capture.xmlSpace,
			NamespaceURI: "http://www.w3.org/XML/1998/namespace",
		})
	}
	return &RecordMatch{
		Rule:     rule,
		Node:     root,
		Location: append([]int(nil), capture.location...),
	}
}

// copyRecordNode deep-copies a subtree. The copy shares no node or
// attribute slice with the source, and the copied root has no parent or
// siblings.
func copyRecordNode(src *Node, rootLevel int) *Node {
	dst := &Node{
		Type:         src.Type,
		Data:         src.Data,
		Prefix:       src.Prefix,
		NamespaceURI: src.NamespaceURI,
		level:        src.level - rootLevel + 1,
		LineNumber:   src.LineNumber,
	}
	if src.Attr != nil {
		dst.Attr = make([]Attr, len(src.Attr))
		copy(dst.Attr, src.Attr)
	}
	if src.ProcInst != nil {
		procInst := *src.ProcInst
		dst.ProcInst = &procInst
	}
	for child := src.FirstChild; child != nil; child = child.NextSibling {
		AddChild(dst, copyRecordNode(child, rootLevel))
	}
	return dst
}

// addRecordScopeDeclarations repeats the namespace declarations that were
// in scope at the record root but are not declared by the record itself,
// so the detached record still resolves to the same expanded names.
func addRecordScopeDeclarations(root *Node, scope map[string]string) {
	uris := make([]string, 0, len(scope))
	for uri := range scope {
		uris = append(uris, uri)
	}
	sort.Strings(uris)
	for _, uri := range uris {
		if uri == "" || uri == "http://www.w3.org/XML/1998/namespace" {
			continue
		}
		if recordDeclaresURI(root, uri) {
			continue
		}
		prefix := scope[uri]
		if recordPrefixTaken(root, prefix) {
			// The original prefix is rebound on the record root itself;
			// pick a fresh one and point the subtree's uses at it.
			fresh := "rns1"
			for n := 2; recordPrefixTaken(root, fresh); n++ {
				fresh = fmt.Sprintf("rns%d", n)
			}
			rebindRecordPrefix(root, uri, prefix, fresh)
			prefix = fresh
		}
		if prefix == "" {
			root.Attr = append(root.Attr, Attr{Name: xml.Name{Local: "xmlns"}, Value: uri})
		} else {
			root.Attr = append(root.Attr, Attr{
				Name:         xml.Name{Space: "xmlns", Local: prefix},
				Value:        uri,
				NamespaceURI: "xmlns",
			})
		}
	}
}

func recordDeclaresURI(n *Node, uri string) bool {
	for _, attr := range n.Attr {
		if attr.Name.Space == "xmlns" && attr.Value == uri {
			return true
		}
		if attr.Name.Space == "" && attr.Name.Local == "xmlns" && attr.Value == uri {
			return true
		}
	}
	return false
}

func recordPrefixTaken(n *Node, prefix string) bool {
	for _, attr := range n.Attr {
		if prefix == "" {
			if attr.Name.Space == "" && attr.Name.Local == "xmlns" {
				return true
			}
		} else if attr.Name.Space == "xmlns" && attr.Name.Local == prefix {
			return true
		}
	}
	return false
}

// rebindRecordPrefix rewrites uses of oldPrefix for uri inside the copied
// record to newPrefix.
func rebindRecordPrefix(root *Node, uri, oldPrefix, newPrefix string) {
	var walk func(n *Node)
	walk = func(n *Node) {
		if n.Type == ElementNode && n.NamespaceURI == uri && n.Prefix == oldPrefix {
			n.Prefix = newPrefix
		}
		for i := range n.Attr {
			if n.Attr[i].NamespaceURI == uri && n.Attr[i].Name.Space == oldPrefix {
				n.Attr[i].Name.Space = newPrefix
			}
		}
		for child := n.FirstChild; child != nil; child = child.NextSibling {
			walk(child)
		}
	}
	walk(root)
}

func isRecordQName(s string) bool {
	if s == "" {
		return false
	}
	colons := 0
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case c == ':':
			colons++
			if i == 0 || i == len(s)-1 || colons > 1 {
				return false
			}
		case c == '.' || c == '-':
			if i == 0 {
				return false
			}
		case c == '_' || c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9' || c >= 0x80:
		default:
			return false
		}
	}
	return true
}

func skipRecordSpaces(s string, i int) int {
	for i < len(s) && (s[i] == ' ' || s[i] == '\t') {
		i++
	}
	return i
}

// parseRecordPredicate parses a single [@name='value'] predicate starting
// at path[i] == '[' and returns the condition and the offset just past
// ']'. Every other predicate form is rejected.
func parseRecordPredicate(path string, i int, namespaces map[string]string) (*recordAttrCond, int, error) {
	unsupported := func() (*recordAttrCond, int, error) {
		return nil, 0, fmt.Errorf("xmlquery: record path %q: only a single [@name='value'] predicate is supported", path)
	}
	i = skipRecordSpaces(path, i+1)
	if i >= len(path) || path[i] != '@' {
		return unsupported()
	}
	i++
	start := i
	for i < len(path) && path[i] != '=' && path[i] != ']' && path[i] != ' ' && path[i] != '\t' {
		i++
	}
	name := path[start:i]
	if !isRecordQName(name) {
		return unsupported()
	}
	i = skipRecordSpaces(path, i)
	if i >= len(path) || path[i] != '=' {
		return unsupported()
	}
	i = skipRecordSpaces(path, i+1)
	if i >= len(path) || (path[i] != '\'' && path[i] != '"') {
		return unsupported()
	}
	quote := path[i]
	i++
	start = i
	for i < len(path) && path[i] != quote {
		i++
	}
	if i >= len(path) {
		return nil, 0, fmt.Errorf("xmlquery: record path %q: unterminated attribute value", path)
	}
	cond := &recordAttrCond{value: path[start:i]}
	i++
	if i = skipRecordSpaces(path, i); i >= len(path) || path[i] != ']' {
		return unsupported()
	}
	i++
	cond.local = name
	if j := strings.IndexByte(name, ':'); j >= 0 {
		uri, ok := namespaces[name[:j]]
		if !ok {
			return nil, 0, fmt.Errorf("xmlquery: record path %q: unknown namespace prefix %q", path, name[:j])
		}
		cond.uri = uri
		cond.local = name[j+1:]
	}
	return cond, i, nil
}

// parseRecordPath compiles the restricted record path syntax.
func parseRecordPath(path string, namespaces map[string]string) ([]recordStep, error) {
	if path == "" {
		return nil, fmt.Errorf("xmlquery: record path is empty")
	}
	var steps []recordStep
	i := 0
	for {
		if i >= len(path) || path[i] != '/' {
			return nil, fmt.Errorf("xmlquery: record path %q: expected '/' at offset %d", path, i)
		}
		step := recordStep{}
		i++
		if i < len(path) && path[i] == '/' {
			step.descendant = true
			i++
		}
		start := i
		for i < len(path) && path[i] != '/' && path[i] != '[' {
			i++
		}
		name := path[start:i]
		switch {
		case name == "*":
			step.any = true
		case isRecordQName(name):
			step.local = name
			if j := strings.IndexByte(name, ':'); j >= 0 {
				uri, ok := namespaces[name[:j]]
				if !ok {
					return nil, fmt.Errorf("xmlquery: record path %q: unknown namespace prefix %q", path, name[:j])
				}
				step.uri = uri
				step.local = name[j+1:]
			}
		default:
			return nil, fmt.Errorf("xmlquery: record path %q: unsupported step %q", path, name)
		}
		if i < len(path) && path[i] == '[' {
			cond, next, err := parseRecordPredicate(path, i, namespaces)
			if err != nil {
				return nil, err
			}
			step.attr = cond
			i = next
		}
		steps = append(steps, step)
		if i == len(path) {
			return steps, nil
		}
	}
}
