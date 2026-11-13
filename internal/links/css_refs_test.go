package links

import (
	"strings"
	"testing"
)

func TestCSSImportTokens(t *testing.T) {
	t.Parallel()

	type wantRef struct {
		URL    string
		Import bool
	}

	tests := []struct {
		name string
		css  string
		want []wantRef
	}{
		{
			name: "string-import",
			css:  `@import "child.css";`,
			want: []wantRef{{URL: "child.css", Import: true}},
		},
		{
			name: "single-quoted-import",
			css:  `@import 'child.css';`,
			want: []wantRef{{URL: "child.css", Import: true}},
		},
		{
			name: "url-import",
			css:  `@import url(child.css);`,
			want: []wantRef{{URL: "child.css", Import: true}},
		},
		{
			name: "quoted-url-import",
			css:  `@import url("child.css");`,
			want: []wantRef{{URL: "child.css", Import: true}},
		},
		{
			name: "case-insensitive-keyword",
			css:  `@IMPORT "child.css";`,
			want: []wantRef{{URL: "child.css", Import: true}},
		},
		{
			name: "escaped-string-import",
			css:  `@import "th\65 me";`,
			want: []wantRef{{URL: "theme", Import: true}},
		},
		{
			name: "escaped-url-import",
			css:  `@import url(a\2e css);`,
			want: []wantRef{{URL: "a.css", Import: true}},
		},
		{
			name: "conditions-not-evaluated",
			css:  `@import "child.css" layer(base) supports(display: grid) screen and (min-width: 30em);`,
			want: []wantRef{{URL: "child.css", Import: true}},
		},
		{
			name: "condition-urls-not-sources",
			css:  `@import "child.css" supports(--x: url(cond.png)) screen;`,
			want: []wantRef{{URL: "child.css", Import: true}},
		},
		{
			name: "import-once-per-statement",
			css:  `@import url("a.css") print; @import "b.css";`,
			want: []wantRef{
				{URL: "a.css", Import: true},
				{URL: "b.css", Import: true},
			},
		},
		{
			name: "ordinary-resources",
			css:  `a { background: url(pic.png); } @font-face { src: url(font.woff2); }`,
			want: []wantRef{
				{URL: "pic.png", Import: false},
				{URL: "font.woff2", Import: false},
			},
		},
		{
			name: "duplicates-and-order",
			css:  `a { background: url(x.png); } b { background: url(x.png); } c { background: url(y.png); }`,
			want: []wantRef{
				{URL: "x.png", Import: false},
				{URL: "x.png", Import: false},
				{URL: "y.png", Import: false},
			},
		},
		{
			name: "mixed-order",
			css:  `a { background: url(first.png); } @import "imp.css"; b { background: url(last.png); }`,
			want: []wantRef{
				{URL: "first.png", Import: false},
				{URL: "imp.css", Import: true},
				{URL: "last.png", Import: false},
			},
		},
		{
			name: "nested-import-ignored",
			css:  `@media screen { @import "nested.css"; a { background: url(deep.png); } }`,
			want: []wantRef{{URL: "deep.png", Import: false}},
		},
		{
			name: "comments-not-resources",
			css:  `/* url(comment.png) */ a { background: url(real.png); }`,
			want: []wantRef{{URL: "real.png", Import: false}},
		},
		{
			name: "plain-string-not-resource",
			css:  `a { content: "url(nope.png)"; }`,
			want: nil,
		},
		{
			name: "empty-addresses",
			css:  `a { background: url(); } @import ""; b { background: url(""); }`,
			want: nil,
		},
		{
			name: "whitespace-in-url",
			css:  `a { background: url(  spaced.png  ); }`,
			want: []wantRef{{URL: "spaced.png", Import: false}},
		},
		{
			name: "escape-line-continuation",
			css:  "@import \"foo\\\nbar.css\";",
			want: []wantRef{{URL: "foobar.css", Import: true}},
		},
		{
			name: "unicode-escape",
			css:  `@import "\1F600.css";`,
			want: []wantRef{{URL: "\U0001F600.css", Import: true}},
		},
		{
			name: "import-after-rule-no-validation",
			css:  `a { color: red; } @import "late.css";`,
			want: []wantRef{{URL: "late.css", Import: true}},
		},
		{
			name: "semicolon-in-string",
			css:  `@import "a;b.css"; c { background: url(d.png); }`,
			want: []wantRef{
				{URL: "a;b.css", Import: true},
				{URL: "d.png", Import: false},
			},
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			t.Parallel()

			var got []wantRef

			ExtractCSSReferences(strings.NewReader(tc.css), func(ref CSSReference) {
				got = append(got, wantRef{URL: ref.URL, Import: ref.Import})
			})

			if len(got) != len(tc.want) {
				t.Fatalf("refs = %v, want %v", got, tc.want)
			}

			for i := range tc.want {
				if got[i] != tc.want[i] {
					t.Fatalf("refs[%d] = %v, want %v (all: %v)", i, got[i], tc.want[i], got)
				}
			}
		})
	}
}

func TestCSSImportTokensLegacyUnaffected(t *testing.T) {
	t.Parallel()

	// the legacy extractor keeps its old behaviour and is still used when the
	// dependency collection mode is off.
	var got []string

	ExtractCSS(strings.NewReader(`@import "child.css"; a {background:url(pic.png)}`), func(s string) {
		got = append(got, s)
	})

	if len(got) != 1 || got[0] != "pic.png" {
		t.Fatalf("legacy extraction = %v", got)
	}
}
