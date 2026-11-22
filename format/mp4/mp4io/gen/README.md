# mp4io atom generator

`atoms.go` is generated from `pattern.go`, which is parsed as DSL input
and is not part of the compiled package (it carries a `//go:build ignore`
tag). `gen.go` is the generator itself.

Regenerate after changing `pattern.go` (run from the repository root):

```
go run ./format/mp4/mp4io/gen/gen.go gen \
    ./format/mp4/mp4io/gen/pattern.go \
    ./format/mp4/mp4io/atoms.go
gofmt -w ./format/mp4/mp4io/atoms.go
```

## DSL notes

- `atoms(Field, Type)` keeps the legacy unordered repeated-atom behaviour.
- `atomslist(Field, Type)` emits an ordered list: child atoms stay in file
  order, unknown tags fail parsing instead of being ignored, and the
  generated `EntryCount` header is checked against the number of parsed
  children.
- Several `atomslist` calls may share one field to model polymorphic
  ordered lists (for example stsd holds both avc1 and mp4a descriptions in
  one `Descs []Atom` slice).
- `bytesleft(Field)` stores the remaining atom payload verbatim, used by
  version-0 `url ` entries whose location is a NUL-terminated string; a
  self-contained url entry has an empty payload.

Do not hand-edit `atoms.go`: regenerate it so newly added fields survive.
