# safearchive

**This is not an officially supported Google product.**

Safe-by-construction libraries for processing `tar` and `zip` archives, to
replace unsafe alternatives like `archive/tar` and `archive/zip` that are at
risk of path traversal attacks. Besides crafted filename entries in the archive,
this library also protects from symbolic link attacks.

## Usage

These libraries are fully compatible with their golang core counterpart, so
switching to them is as easy as changing the library import at the top, no
further modifications are needed.

The built-in security measures can be turned on or off one by one. Only those
security checks are enabled by default that do not break existing setups.

You may enable the other features individually like this:

```
tr := tar.NewReader(buf)
tr.SetSecurityMode(tr.GetSecurityMode() | tar.SanitizeFileMode | tar.DropXattrs)
```

or

```
tr.SetSecurityMode(tar.MaximumSecurityMode)
```

You may opt out from a certain feature like this:

```
tr.SetSecurityMode(tr.GetSecurityMode() &^ tar.SanitizeFileMode)
```

## Dropping owner identity

The build machine's user identity can travel inside an archive even when only
the file contents are meant to be shared: tar headers carry `Uid`/`Gid`,
`Uname`/`Gname` and their PAX records, while zip headers carry Unix owner
information in little-endian extra field records. Both packages expose an
independent `DropUserIdentity` security bit that strips this identity. It is
not part of `DefaultSecurityMode`, but it is part of `MaximumSecurityMode`, and
it can be combined freely with the other switches such as `SanitizeFileMode`
and `DropXattrs`.

For tar, enabling it zeroes `Header.Uid` and `Header.Gid`, empties
`Header.Uname` and `Header.Gname`, and removes the `uid`, `gid`, `uname` and
`gname` entries from `Header.PAXRecords`. The PAX records have to be removed as
well, otherwise the old values would be restored when the header is handed back
to the standard library's writer. Other PAX entries, timestamps, permission
bits, link targets and file contents are unchanged.

```
tr := tar.NewReader(buf)
tr.SetSecurityMode(tr.GetSecurityMode() | tar.DropUserIdentity)
```

For zip, enabling it deletes every extra field record tagged `0x000d`, `0x5855`,
`0x7855`, `0x7875` or `0x756e` (duplicates included), while all other complete
records — such as ZIP64, extended timestamp and Unicode path entries — are kept
in their original order and byte for byte. A malformed extra field whose
records can no longer be bounded is cleared entirely while the option is on.
Switching the mode off restores the original extra field, since the sanitized
view never modifies the archived bytes or shares storage with them.

```
zr.SetSecurityMode(zr.GetSecurityMode() | zip.DropUserIdentity)
```
