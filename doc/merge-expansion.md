# Merge key expansion (`yaml_document_expand_merges`)

## Purpose

A document loaded with `yaml_parser_load()` keeps YAML merge keys (`<<`)
as ordinary mapping pairs; aliases are resolved to the same node, but a
reader still has to interpret `<<` itself. `yaml_document_expand_merges()`
produces a new, independently owned document in which every merge key has
been expanded, together with provenance describing where each output pair
came from.

The default behavior of loading and dumping is unchanged: the new
function is opt-in and never modifies the document you pass to it.

## Data types

- `yaml_merge_result_t` — owns the expanded `yaml_document_t` and, for
  every expanded output mapping, a `yaml_merge_pair_info_t` array of
  per-pair `yaml_merge_origin_t` records.
- `yaml_merge_origin_t` — the source mapping id, the source pair index,
  the source key/value node ids, and the chain of `yaml_merge_hop_t`
  merge references crossed to reach the winning pair. The chain is empty
  for a field written locally.
- `yaml_merge_error_t` — filled on failure with an error type, the
  relevant source node ids/pair index and marks.

All node and pair indices inside provenance refer to the **source**
document, never to the expanded document.

## Initialization, failure and release

Zero-initialize both output objects before calling the function:

```c
yaml_merge_result_t result;
yaml_merge_error_t  error;
memset(&result, 0, sizeof(result));
memset(&error,  0, sizeof(error));

if (!yaml_document_expand_merges(&document, &result, &error)) {
    /* error.type describes the failure; document is untouched. */
    yaml_merge_result_delete(&result);   /* safe: result is still empty */
    return;
}

/* ... use result.document and the provenance arrays ... */

yaml_merge_result_delete(&result);
```

- On success the function returns `1`. Release the result with
  `yaml_merge_result_delete()`.
- On failure it returns `0`, fills `error` (which may be `NULL` if you do
  not need details), leaves the result empty and never modifies the input
  document. A partially built expanded document is cleaned up internally
  and never returned.
- `yaml_merge_result_delete()` is safe on a zeroed or failed result and on
  `NULL`.
- The expanded document owns all of its memory (scalar bytes and tags are
  copied, the version and tag directives are copied). The source document
  may be deleted while the result is still in use; the result remains
  readable and can be emitted with `yaml_emitter_dump()`.

## Merge semantics

Only scalar keys are expanded. A key is treated as a merge key when it
either carries the explicit `tag:yaml.org,2002:merge` tag, or carries the
default string tag (`tag:yaml.org,2002:str`) and is a **plain** scalar
whose bytes are exactly `<<` (length 2). A quoted `"<<"` keeps the string
tag and stays an ordinary field.

Key identity is the tag plus the scalar bytes compared by length; the
bytes are never treated as NUL-terminated strings, so embedded NULs and
different lengths are distinguished.

- A merge value must be a mapping or a sequence of mappings; anything
  else is `YAML_MERGE_INVALID_VALUE_ERROR`. Earlier sequence entries take
  precedence.
- Local fields always win over merged fields, regardless of whether they
  appear before or after the `<<` entry. Two local pairs with the same key
  are `YAML_MERGE_DUPLICATE_KEY_ERROR` rather than silently resolved.
- Output order is: local non-merge fields in original order, then merged
  fields not already present, in merge order and in each source mapping's
  field order. Several `<<` entries of one mapping are processed left to
  right.
- Ordinary aliases keep sharing one node identity, including ordinary
  cycles such as `loop: &c [*c]`. Only a cycle formed through merge
  dependencies is reported, as `YAML_MERGE_CYCLE_ERROR`.

## Reading provenance

Given an output mapping node id, fetch its pair info and index it by the
output pair number:

```c
const yaml_merge_pair_info_t *info =
    yaml_merge_result_get_mapping_info(&result, output_mapping_id);

for (int i = 0; i < info->pair_count; i++) {
    const yaml_merge_origin_t *origin = &info->origins[i];
    /* origin->mapping / origin->pair identify the winning source pair. */
    /* origin->hop_count is 0 for a local field. */
    for (int h = 0; h < origin->hop_count; h++) {
        /* origin->hops[h].mapping, .pair, .target describe one << hop. */
    }
}
```

The recorded `mapping`/`pair` always name the mapping that actually
supplies the winning value, even when several merge layers are involved;
the `hops` array records the path through the `<<` references. For
example, with

```yaml
base: &b {id: 7, unit: kg}
row:  {<<: *b, id: 9}
```

`row`'s `id` is a local field (empty hop chain), while `unit` points at
the original `base` pair and carries one hop from `row`'s `<<` entry to
`base`.

## Emitting the result

`result.document` is a normal `yaml_document_t`, so it can be emitted
directly. Note that `yaml_emitter_dump()` consumes the document; call
`yaml_merge_result_delete()` afterwards regardless, which remains safe:

```c
yaml_emitter_t emitter;
yaml_emitter_initialize(&emitter);
yaml_emitter_set_output_file(&emitter, stdout);
yaml_emitter_open(&emitter);
yaml_emitter_dump(&emitter, &result.document);
yaml_emitter_close(&emitter);
yaml_emitter_delete(&emitter);

yaml_merge_result_delete(&result);
```

## Error types

| Type                                  | Meaning                                            |
|---------------------------------------|----------------------------------------------------|
| `YAML_MERGE_OK`                       | No error.                                          |
| `YAML_MERGE_MEMORY_ERROR`             | An allocation failed.                              |
| `YAML_MERGE_NON_SCALAR_KEY_ERROR`     | A mapping key is not a scalar.                     |
| `YAML_MERGE_DUPLICATE_KEY_ERROR`      | Two local pairs share one key.                     |
| `YAML_MERGE_INVALID_VALUE_ERROR`      | A merge value is not a mapping/sequence of maps.   |
| `YAML_MERGE_CYCLE_ERROR`              | Merge dependencies form a cycle.                   |
