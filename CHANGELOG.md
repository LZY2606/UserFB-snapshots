# Changelog

# dev

* Rewrite for C++17.
* Removed functionality concerning re-building of relocations due to its flaws
  ([#15](https://github.com/avast/pelib/issues/15),
  [#16](https://github.com/avast/pelib/pull/16)).
* Fixed detection of cut import directory
  ([#17](https://github.com/avast/pelib/pull/17)).
* Resource directory reading is now RVA-based. Directory headers, entry arrays,
  UTF-16 names, data descriptors, and payloads are read through each RVA's own
  file mapping, so they may cross virtually adjacent sections whose file
  offsets are unrelated. Resource-relative offsets are resolved against the
  resource directory RVA; a leaf's `OffsetToData` stays an independent RVA.
  Sibling entries may share a subdirectory and both paths are expanded, while
  a reference to an ancestor is rejected with `ERROR_INVALID_FILE`. Reading a
  second image into the same `ResourceDirectory` replaces the previous tree,
  occupied ranges, and loader error.

# v1.0 (2017-12-12)

Initial release.
