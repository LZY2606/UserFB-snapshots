"""Stdlib-only web UI for template epoch evidence."""

from __future__ import annotations

import html
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import parse_qs, urlparse

from .sample import load_sample
from .store import Store

PAGE = """<!doctype html>
<html><head><meta charset="utf-8"><title>ipfixline</title>
<style>
body { font-family: ui-monospace, Menlo, monospace; margin: 2rem; background: #fafaf7; color: #222; }
h1 { font-size: 1.4rem; } h2 { font-size: 1.05rem; margin-top: 2rem; }
table { border-collapse: collapse; margin: 0.5rem 0 1.5rem; font-size: 0.85rem; }
th, td { border: 1px solid #ccc; padding: 0.25rem 0.6rem; text-align: left; vertical-align: top; }
th { background: #eee; }
.gap, .no_template, .truncated { color: #a00000; font-weight: bold; }
.resolved, .ok { color: #106010; }
code { background: #efefef; padding: 0 3px; }
form { display: inline; }
input[type=number] { width: 5rem; }
</style></head><body>
<h1>Template epoch evidence</h1>
%s
</body></html>"""


def _esc(v) -> str:
    return html.escape(str(v))


def _table(headers, rows) -> str:
    out = ["<table><tr>" + "".join(f"<th>{_esc(h)}</th>" for h in headers) + "</tr>"]
    for row in rows:
        out.append("<tr>" + "".join(f"<td>{c}</td>" for c in row) + "</tr>")
    out.append("</table>")
    return "".join(out)


def render_capture(store: Store, capture_id: int, revision: int) -> str:
    parts: list[str] = []
    rev_row = store.db.execute(
        "SELECT reason FROM revisions WHERE capture_id=? AND revision=?",
        (capture_id, revision),
    ).fetchone()
    parts.append(f"<p>capture #{capture_id} &middot; revision {revision}"
                 f" ({_esc(rev_row['reason'] if rev_row else '')})</p>")

    msgs = store.list_messages(capture_id)
    parts.append("<h2>Imported messages</h2>")
    parts.append(_table(
        ["arrival", "exporter", "domain", "export time", "sequence", "sets", "digest"],
        [[m["arrival"], _esc(m["exporter"]), m["domain_id"], m["export_time"],
          m["sequence"], f'{m["set_first"]}..{m["set_last"]}',
          f"<code>{_esc(m['digest'][:16])}&hellip;</code>"] for m in msgs],
    ))

    sessions = store.current_sessions(capture_id)
    rows = []
    for s in sessions:
        rows.append([
            s["id"], _esc(s["exporter"]), s["version"],
            s["start_arrival"], s["end_arrival"], _esc(s["note"]),
            f"""<form method="post" action="/capture/{capture_id}/sessions/{s['id']}/revise">
                <input type="number" name="start" value="{s['start_arrival']}">
                <input type="number" name="end" value="{s['end_arrival']}">
                <input type="text" name="note" placeholder="note">
                <button type="submit">revise</button></form>""",
        ])
    parts.append("<h2>Capture sessions (current versions)</h2>")
    parts.append(_table(["id", "exporter", "version", "start", "end", "note", "revise"], rows))

    epochs = store.resolutions(capture_id, revision, "epoch")
    parts.append("<h2>Template epochs</h2>")
    parts.append(_table(
        ["exporter", "domain", "template", "kind", "generation", "defined@", "withdrawn@", "fields"],
        [[_esc(e["exporter"]), e["domain_id"], e["template_id"], e["kind"],
          e["generation"], e["defined_at"],
          e["withdrawn_at"] if e["withdrawn_at"] is not None else "-",
          "<br>".join(_esc(f) for f in e["fields"])] for e in epochs],
    ))

    records = store.resolutions(capture_id, revision, "record")
    rec_rows = []
    for r in records:
        field_txt = "<br>".join(
            f'{_esc(f["description"])} = <code>{_esc(f["value_hex"])}</code>'
            + (" [truncated]" if f["truncated"] else "")
            for f in r["fields"]
        )
        rec_rows.append([
            r.get("arrival", ""), r["template_id"],
            r["generation"] if r["generation"] is not None else "-",
            f'<span class="{_esc(r["status"])}">{_esc(r["status"])}</span>',
            field_txt or "-",
            f"<code>{_esc(r['padding'])}</code>" if r["padding"] else "-",
            f"<code>{_esc(r['tail'])}</code>" if r["tail"] else "-",
            ", ".join(str(c) for c in r["candidates"]) or "-",
            _esc(r["note"]),
        ])
    parts.append("<h2>Data record resolutions</h2>")
    parts.append(_table(
        ["arrival", "template", "gen", "status", "fields", "padding",
         "unparsed tail", "candidates", "note"], rec_rows))

    seqs = store.resolutions(capture_id, revision, "sequence")
    parts.append("<h2>Sequence events (per data record)</h2>")
    parts.append(_table(
        ["exporter", "domain", "expected", "observed", "kind"],
        [[_esc(s["exporter"]), s["domain_id"], s["expected"], s["observed"],
          f'<span class="{_esc(s["kind"])}">{_esc(s["kind"])}</span>'] for s in seqs],
    ))
    return PAGE % "".join(parts)


def make_handler(store: Store):
    class Handler(BaseHTTPRequestHandler):
        def _send(self, body: str, status: int = 200, location: str | None = None):
            data = body.encode()
            self.send_response(status)
            if location:
                self.send_header("Location", location)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def _default_capture(self) -> int:
            caps = store.list_captures()
            if caps:
                return caps[0]["id"]
            return load_sample(store)

        def do_GET(self):
            path = urlparse(self.path).path
            if path == "/":
                cid = self._default_capture()
                rev = store.latest_revision(cid)
                self._send(render_capture(store, cid, rev))
            else:
                self._send(PAGE % "<p>not found</p>", status=404)

        def do_POST(self):
            path = urlparse(self.path).path
            parts = [p for p in path.split("/") if p]
            if (len(parts) == 5 and parts[0] == "capture"
                    and parts[2] == "sessions" and parts[4] == "revise"):
                cid = int(parts[1])
                sid = int(parts[3])
                length = int(self.headers.get("Content-Length", 0))
                form = parse_qs(self.rfile.read(length).decode())
                start = int(form["start"][0])
                end = int(form["end"][0])
                note = form.get("note", [""])[0]
                store.revise_session(cid, sid, start, end, note)
                self._send("", status=303, location="/")
            else:
                self._send(PAGE % "<p>not found</p>", status=404)

        def log_message(self, *args):
            pass

    return Handler


def serve(store: Store, port: int):
    server = HTTPServer(("127.0.0.1", port), make_handler(store))
    print(f"serving on http://127.0.0.1:{port}")
    server.serve_forever()
