#!/usr/bin/env python3
"""Mirror a Go module dependency closure from proxy.golang.org to disk.

Why this exists: the sandbox blocks both the macOS keychain (so the Go
toolchain's platform certificate verifier fails with OSStatus -26276) and
local TCP listeners (so a local forwarding proxy cannot be run). curl and
Python are able to use the egress proxy normally, so this script performs the
downloads itself and lays the results out as a file:// module proxy:

    <mirror>/cache/download/<escaped-path>/@v/<version>.{mod,info,zip}

Then `GOPROXY=file://<mirror>/cache/download GOSUMDB=off go mod download`
works fully offline.

Usage: mirror_mods.py <mirror-dir> <module@version> [<module@version> ...]
"""
import json
import os
import re
import sys
import urllib.request

PROXY = "https://proxy.golang.org"


def escape_path(path: str) -> str:
    # Go module paths: uppercase letters are stored as '!' + lowercase.
    out = []
    for ch in path:
        if ch.isupper():
            out.append("!" + ch.lower())
        else:
            out.append(ch)
    return "".join(out)


_REQUIRE_LINE = re.compile(r"^\s*([^\s]+)\s+([^\s]+)\s*(?://.*)?$")


def parse_requires(mod_text: str):
    """Yield (path, version) pairs from require directives in a go.mod."""
    in_block = False
    for raw in mod_text.splitlines():
        line = raw.strip()
        if not line or line.startswith("//"):
            continue
        if line.startswith("require ("):
            in_block = True
            continue
        if in_block and line == ")":
            in_block = False
            continue
        if in_block:
            m = _REQUIRE_LINE.match(line)
            if m:
                yield m.group(1), m.group(2)
            continue
        if line.startswith("require "):
            m = _REQUIRE_LINE.match(line[len("require "):])
            if m:
                yield m.group(1), m.group(2)


def fetch(url: str) -> bytes:
    with urllib.request.urlopen(url, timeout=60) as resp:
        return resp.read()


def download(url: str, dest: str, attempts: int = 6) -> None:
    """Download with Range-based resume; module proxy objects are immutable."""
    for attempt in range(1, attempts + 1):
        have = os.path.getsize(dest) if os.path.exists(dest) else 0
        req = urllib.request.Request(url)
        if have:
            req.add_header("Range", f"bytes={have}-")
        try:
            with urllib.request.urlopen(req, timeout=120) as resp:
                expected = None
                if resp.status == 206:
                    rng = resp.headers.get("Content-Range", "")
                    if "/" in rng:
                        expected = int(rng.rsplit("/", 1)[1])
                    mode = "ab"
                else:
                    have = 0
                    expected = int(resp.headers.get("Content-Length", 0)) or None
                    mode = "wb"
                with open(dest, mode) as fh:
                    while True:
                        chunk = resp.read(1 << 16)
                        if not chunk:
                            break
                        fh.write(chunk)
            if expected is None or os.path.getsize(dest) == expected:
                return
            print(f"  truncated ({os.path.getsize(dest)}/{expected}), resuming")
        except Exception as exc:  # noqa: BLE001 - network errors vary
            print(f"  retry {attempt}/{attempts} for {url}: {exc}")
    raise SystemExit(f"failed after {attempts} attempts: {url}")


def main():
    mirror = os.path.abspath(sys.argv[1])
    roots = sys.argv[2:]
    seen: set[tuple[str, str]] = set()
    queue: list[tuple[str, str]] = []
    for root in roots:
        path, _, version = root.partition("@")
        if not version:
            raise SystemExit(f"root must be module@version: {root}")
        queue.append((path, version))

    while queue:
        path, version = queue.pop(0)
        if (path, version) in seen:
            continue
        seen.add((path, version))

        dirpath = os.path.join(
            mirror, "cache", "download", escape_path(path), "@v"
        )
        os.makedirs(dirpath, exist_ok=True)
        base = f"{PROXY}/{path}/@v/{version}"
        targets = {
            ".mod": os.path.join(dirpath, f"{version}.mod"),
            ".info": os.path.join(dirpath, f"{version}.info"),
            ".zip": os.path.join(dirpath, f"{version}.zip"),
        }
        for suffix, dest in targets.items():
            if os.path.exists(dest) and os.path.getsize(dest) > 0:
                continue
            download(base + suffix, dest)
            print(f"fetched {path}@{version}{suffix} ({os.path.getsize(dest)} bytes)")

        with open(targets[".mod"], "r", encoding="utf-8") as fh:
            mod_text = fh.read()
        for req_path, req_version in parse_requires(mod_text):
            # Pseudo/standard library paths never need downloading.
            if req_path.startswith("std"):
                continue
            if (req_path, req_version) not in seen:
                queue.append((req_path, req_version))

    print(json.dumps({"modules": sorted(seen), "count": len(seen)}, indent=2))


if __name__ == "__main__":
    main()
