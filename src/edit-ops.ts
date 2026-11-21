// Pure cue-list editing operations, factored out of the editor so they can be unit-tested
// without a DOM. Each takes plain data and returns new data (no mutation of the input array).

import { type AssStyle, type Cue, type SubtitleDoc, type SubtitleFormat, blankCue, cps, newCueId } from "./cue";
import { DEFAULT_STYLE_FORMAT } from "./formats/ass";

function cloneCue(c: Cue): Cue {
  const out = { ...c, id: newCueId(), assFields: c.assFields ? { ...c.assFields } : undefined };
  // Absent optional keys stay absent, so a JSON round trip yields an identical object.
  if (c.notesBefore === undefined) delete out.notesBefore;
  if (c.assFields === undefined) delete out.assFields;
  return out;
}

// Merge the cue at index i with the one after it: join their text with a space, span the
// combined time, and drop the second cue. Returns a new array, or null if there's no next cue.
export function mergeCuesAt(cues: Cue[], i: number): Cue[] | null {
  const cur = cues[i];
  const next = cues[i + 1];
  if (!cur || !next) return null;
  const merged: Cue = {
    ...cur,
    text: `${cur.text.trimEnd()} ${next.text.trimStart()}`.trim(),
    endMs: Math.max(cur.endMs, next.endMs),
  };
  const out = cues.slice();
  out.splice(i, 2, merged);
  return out;
}

// Split the cue at index i at character offset `at` (clamped to a sensible midpoint), dividing
// the duration in proportion to each part's length. ASS style/fields carry to the second cue.
// Returns the new array plus the two resulting ids, or null if the cue can't be split.
export function splitCueAt(
  cues: Cue[],
  i: number,
  at: number,
  format: SubtitleFormat,
): { cues: Cue[]; firstId: string; secondId: string } | null {
  const cue = cues[i];
  if (!cue) return null;
  const text = cue.text;
  let pos = at;
  if (pos <= 0 || pos >= text.length) pos = Math.floor(text.length / 2);
  if (pos <= 0) return null;
  const first = text.slice(0, pos).trimEnd();
  const rest = text.slice(pos).trimStart();
  const total = cue.endMs - cue.startMs;
  const frac = first.length / Math.max(1, first.length + rest.length);
  const mid = cue.startMs + Math.max(1, Math.min(total - 1, Math.round(total * frac)));
  const second = blankCue(mid, cue.endMs, rest);
  if (format === "ass") {
    second.assKind = cue.assKind;
    if (cue.assFields) second.assFields = { ...cue.assFields };
  }
  const firstCue: Cue = { ...cue, text: first, endMs: mid };
  const out = cues.slice();
  out.splice(i, 1, firstCue, second);
  return { cues: out, firstId: firstCue.id, secondId: second.id };
}

// Clamp a timing edge to the playhead, keeping at least a 1ms span against the other edge.
export function clampStart(cue: Cue, ms: number): number {
  return Math.min(ms, cue.endMs - 1);
}
export function clampEnd(cue: Cue, ms: number): number {
  return Math.max(ms, cue.startMs + 1);
}

export type ProblemKind = "overlap" | "tooFast" | "tooLong";
export interface Problem {
  id: string;
  index: number;
  kind: ProblemKind;
  cps?: number;
}

// Detect the common subtitle issues: overlap with the next cue, too-fast reading speed, and
// over-long duration. In document order, each with its cue's id/index for jumping.
export function findProblems(cues: Cue[], opts: { cpsBad: number; maxDurMs: number }): Problem[] {
  const out: Problem[] = [];
  for (let i = 0; i < cues.length; i += 1) {
    const c = cues[i];
    const next = cues[i + 1];
    if (next && c.endMs > next.startMs) out.push({ id: c.id, index: i, kind: "overlap" });
    const speed = cps(c);
    if (speed > opts.cpsBad) out.push({ id: c.id, index: i, kind: "tooFast", cps: speed });
    if (c.endMs - c.startMs > opts.maxDurMs) out.push({ id: c.id, index: i, kind: "tooLong" });
  }
  return out;
}

// Duplicate each selected cue, inserting a copy right after it that starts at the original's
// end (same duration/text/style). Returns the new array and the ids of the copies.
export function duplicateCues(cues: Cue[], ids: Set<string>): { cues: Cue[]; newIds: string[] } {
  const out: Cue[] = [];
  const newIds: string[] = [];
  for (const c of cues) {
    out.push(c);
    if (ids.has(c.id)) {
      const dur = c.endMs - c.startMs;
      const copy = { ...cloneCue(c), startMs: c.endMs, endMs: c.endMs + dur };
      out.push(copy);
      newIds.push(copy.id);
    }
  }
  return { cues: out, newIds };
}

// Insert `clip` cues after `afterIndex`, rebasing their times so the first starts at the end
// of the cue there (preserving the offsets between clip cues). Fresh ids are assigned.
export function pasteCues(cues: Cue[], clip: Cue[], afterIndex: number): { cues: Cue[]; newIds: string[] } {
  if (!clip.length) return { cues, newIds: [] };
  const anchor = cues[afterIndex];
  const base = anchor ? anchor.endMs : cues.length ? cues[cues.length - 1].endMs : 0;
  const clipStart = clip[0].startMs;
  const pasted = clip.map((c) => {
    const off = c.startMs - clipStart;
    const dur = c.endMs - c.startMs;
    return { ...cloneCue(c), startMs: base + off, endMs: base + off + dur };
  });
  const out = cues.slice();
  out.splice(afterIndex + 1, 0, ...pasted);
  return { cues: out, newIds: pasted.map((c) => c.id) };
}

// The index range (inclusive) spanned by an anchor id and a target id, for shift-click ranges.
export function rangeIds(cues: Cue[], anchorId: string | null, targetId: string): string[] {
  const a = cues.findIndex((c) => c.id === anchorId);
  const b = cues.findIndex((c) => c.id === targetId);
  if (b < 0) return anchorId ? [anchorId] : [];
  if (a < 0) return [targetId];
  const [lo, hi] = a <= b ? [a, b] : [b, a];
  return cues.slice(lo, hi + 1).map((c) => c.id);
}

export interface TimingFixOptions {
  minGapMs: number; // minimum gap enforced between consecutive cues
  minDurMs: number; // minimum cue duration (short cues are extended if there's room)
  maxDurMs: number; // maximum cue duration (long cues are capped)
}

// Normalize cue timing: clamp each duration to [minDur, maxDur], then trim any overlap so
// consecutive cues keep at least minGap between them. Returns a new array; inputs untouched.
// (Reading speed isn't auto-extended, that would require shifting neighbours; it's advisory.)
export function autoFixTiming(cues: Cue[], opts: TimingFixOptions): Cue[] {
  const out = cues.map((c) => ({ ...c }));
  for (const c of out) {
    const d = c.endMs - c.startMs;
    if (d < opts.minDurMs) c.endMs = c.startMs + opts.minDurMs;
    else if (d > opts.maxDurMs) c.endMs = c.startMs + opts.maxDurMs;
  }
  for (let i = 0; i < out.length - 1; i += 1) {
    const a = out[i];
    const b = out[i + 1];
    if (a.endMs > b.startMs - opts.minGapMs) {
      a.endMs = Math.max(a.startMs + 1, b.startMs - opts.minGapMs);
    }
  }
  return out;
}

// Ids of the cues whose text contains `query` (case-insensitive); empty for an empty query.
export function matchCues(cues: Cue[], query: string): string[] {
  const q = query.toLowerCase();
  return q ? cues.filter((c) => c.text.toLowerCase().includes(q)).map((c) => c.id) : [];
}

// Replace every case-insensitive occurrence of `query` with `replacement` across all cues,
// mutating each matching cue's text in place. Returns how many cues changed.
export function replaceAllInCues(cues: Cue[], query: string, replacement: string): number {
  if (!query) return 0;
  const rx = new RegExp(query.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"), "gi");
  let n = 0;
  for (const c of cues) {
    const next = c.text.replace(rx, replacement);
    if (next !== c.text) {
      c.text = next;
      n += 1;
    }
  }
  return n;
}

// --- ASS style-aware copy/paste -------------------------------------------------------
//
// Copying cues between ASS tracks has to carry the style *definitions* they reference,
// not just the names: the target track usually has a style with the same name but a
// different definition, and pasting by name alone would silently restyle the dialogue.
// A clipboard therefore snapshot-copies the cues plus every source style in their
// reference closure (the event Style field, plus top-level `\rName` resets inside
// override blocks). Pasting allocates target names for that closure up front, rewrites
// the event Style fields and the named resets, and appends missing definitions.

export interface AssStyleClipboard {
  // Snapshot of the copied cues in source-document order (fresh ids, detached fields),
  // taken at copy time so later edits to the source track do not leak into the paste.
  cues: Cue[];
  // The source document's Format layouts as they were at copy time. The style and event
  // field maps are keyed by these names, so the layout travels with the snapshot.
  styleFormat: string[];
  eventFormat: string[];
  // Every referenced source style, in source stylesheet order, deep-copied without the
  // notes attached to it in the source file.
  styles: AssStyle[];
}

const DEFAULT_EVENT_FORMAT = ["Layer", "Start", "End", "Style", "Name", "MarginL", "MarginR", "MarginV", "Effect", "Text"];
const CORE_EVENT_FIELD = /^(Start|End|Text)$/i;

// Attribute names compare ASCII case-insensitively; matching is done on this folded key.
function fieldKey(name: string): string {
  return name.replace(/[A-Z]/g, (ch) => ch.toLowerCase());
}

function hasField(fields: Record<string, string> | undefined, name: string): boolean {
  const want = fieldKey(name);
  return Object.keys(fields ?? {}).some((k) => fieldKey(k) === want);
}

// The value of a Format field, resolving the field name case-insensitively.
function fieldValue(fields: Record<string, string> | undefined, name: string): string {
  const want = fieldKey(name);
  const key = Object.keys(fields ?? {}).find((k) => fieldKey(k) === want);
  return key === undefined ? "" : fields?.[key] ?? "";
}

function formatName(format: string[], canonical: RegExp): string | undefined {
  return format.find((f) => canonical.test(f));
}

// The style an event's Style field names.
function styleFieldValue(cue: Cue, format: string[]): string {
  const name = formatName(format, /^Style$/i);
  return name === undefined ? "" : fieldValue(cue.assFields, name);
}

// Split text into its top-level `{...}` override blocks. A block never absorbs a later
// one (the first `}` closes it); an unclosed brace runs to the end, as libass treats it.
function overrideBlocks(text: string): { inner: string; start: number; end: number }[] {
  const blocks: { inner: string; start: number; end: number }[] = [];
  let i = 0;
  while (i < text.length) {
    const open = text.indexOf("{", i);
    if (open < 0) break;
    const close = text.indexOf("}", open + 1);
    if (close < 0) {
      blocks.push({ inner: text.slice(open + 1), start: open, end: text.length });
      break;
    }
    blocks.push({ inner: text.slice(open + 1, close), start: open, end: close });
    i = close + 1;
  }
  return blocks;
}

// Walk an override block's tags, yielding each backslash tag and its argument range.
// `\t(...)` is one tag whose parenthesised argument is consumed whole, so tags nested in
// it (including `\rName`) never count as top-level resets. For other tags the argument
// runs to the next backslash or the block end. Text outside braces is never scanned.
function tagsInBlock(block: string): { tag: string; argStart: number; argEnd: number }[] {
  const tags: { tag: string; argStart: number; argEnd: number }[] = [];
  let i = 0;
  while (i < block.length) {
    if (block[i] !== "\\") {
      i += 1;
      continue;
    }
    const tag = block[i + 1] ?? "";
    i += 2;
    const argStart = i;
    let argEnd: number;
    if (tag === "t") {
      // \t(style modifiers): consume the balanced parentheses as the tag's argument.
      let j = i;
      while (j < block.length && block[j] !== "\\" && /[^\s(]/.test(block[j])) j += 1; // optional scale factor
      if (block[j] === "(") {
        let depth = 0;
        while (j < block.length) {
          if (block[j] === "(") depth += 1;
          else if (block[j] === ")") {
            depth -= 1;
            if (depth === 0) break;
          }
          j += 1;
        }
        argEnd = Math.min(j + 1, block.length);
      } else {
        argEnd = j;
      }
    } else {
      let j = i;
      while (j < block.length && block[j] !== "\\") j += 1;
      argEnd = j;
    }
    tags.push({ tag, argStart, argEnd });
    i = argEnd;
  }
  return tags;
}

// A named reset's style name: the argument up to a parenthesis (names may contain spaces,
// but not parentheses), or the whole argument. Empty means an anonymous `\r`.
function resetNameAt(inner: string, argStart: number, argEnd: number): string {
  const raw = inner.slice(argStart, argEnd);
  const stop = raw.search(/[()]/);
  const name = stop < 0 ? raw : raw.slice(0, stop);
  return name.trim() === "" ? "" : name;
}

// The style names referenced by top-level named resets in a cue's text. A bare `\r` is
// not a reference; literal `\r` outside braces, font names and `\t(...)` contents yield
// nothing because only tag starts inside a block are examined.
function resetStyleNames(text: string): string[] {
  const names: string[] = [];
  for (const { inner } of overrideBlocks(text)) {
    for (const { tag, argStart, argEnd } of tagsInBlock(inner)) {
      if (tag !== "r" && tag !== "R") continue;
      const name = resetNameAt(inner, argStart, argEnd);
      if (name) names.push(name);
    }
  }
  return names;
}

// Rewrite only the names of top-level named resets; every other byte is left untouched.
function rewriteNamedResets(text: string, nameOf: (name: string) => string): string {
  const blocks = overrideBlocks(text);
  if (!blocks.length) return text;
  let out = "";
  let last = 0;
  for (const { inner, start, end } of blocks) {
    out += text.slice(last, start + 1);
    let innerOut = "";
    let cursor = 0;
    for (const { tag, argStart, argEnd } of tagsInBlock(inner)) {
      if (tag !== "r" && tag !== "R") continue;
      const name = resetNameAt(inner, argStart, argEnd);
      if (!name) continue;
      innerOut += inner.slice(cursor, argStart) + nameOf(name);
      cursor = argStart + name.length;
    }
    innerOut += inner.slice(cursor);
    out += innerOut + (end < text.length ? "}" : "");
    last = Math.min(end + 1, text.length);
  }
  return out + text.slice(last);
}

// Every style a copied cue set references: event Style fields plus named resets, in
// source stylesheet order. Throws before returning if a reference has no definition in
// the source, so a failed copy leaves neither a partial clipboard nor a modified doc.
function styleClosure(source: SubtitleDoc, cues: Cue[]): AssStyle[] {
  const styles = source.styles ?? [];
  const known = new Set(styles.map((s) => s.name));
  const eventFormat = source.assFormat ?? DEFAULT_EVENT_FORMAT;
  const missing: string[] = [];
  const wanted = new Set<string>();
  const add = (name: string): void => {
    if (!name) return;
    if (!known.has(name)) {
      if (!missing.includes(name)) missing.push(name);
      return;
    }
    wanted.add(name);
  };
  for (const cue of cues) {
    add(styleFieldValue(cue, eventFormat));
    for (const name of resetStyleNames(cue.text)) add(name);
  }
  if (missing.length) throw new Error(`copy: source style not defined: ${missing.join(", ")}`);
  return styles.filter((s) => wanted.has(s.name));
}

function cloneAssStyle(style: AssStyle): AssStyle {
  return { name: style.name, fields: { ...style.fields } };
}

// Snapshot the selected cues (in source document order) plus every source style they
// reference. The result aliases nothing in `sourceDoc`, so it survives JSON.stringify /
// JSON.parse and cannot be changed by later edits to the source track. Non-ASS documents
// get an empty style closure, which makes pasting follow the plain cue rules.
export function copyCueClipboard(sourceDoc: SubtitleDoc, ids: Iterable<string>): AssStyleClipboard {
  const selected = new Set(ids);
  const isAss = sourceDoc.format === "ass";
  const cues = sourceDoc.cues.filter((c) => selected.has(c.id)).map((c) => cloneCue(c));
  const styles = isAss ? styleClosure(sourceDoc, cues).map(cloneAssStyle) : [];
  return {
    cues,
    styleFormat: [...(sourceDoc.assStyleFormat ?? DEFAULT_STYLE_FORMAT)],
    eventFormat: [...(sourceDoc.assFormat ?? DEFAULT_EVENT_FORMAT)],
    styles,
  };
}

// One comparable entry per style attribute: folded name -> value ("" when absent).
// Attribute names are matched ASCII case-insensitively, values compared verbatim, and
// the Format's field order and the Name are ignored.
function styleAttributes(style: AssStyle, format: string[]): Map<string, string> {
  const attrs = new Map<string, string>();
  for (const name of format) {
    if (/^Name$/i.test(name)) continue;
    const key = fieldKey(name);
    if (attrs.has(key)) continue;
    attrs.set(key, fieldValue(style.fields, name));
  }
  // Fields outside the Format still belong to the definition.
  for (const name of Object.keys(style.fields)) {
    const key = fieldKey(name);
    if (!attrs.has(key)) attrs.set(key, style.fields[name]);
  }
  return attrs;
}

// A missing attribute counts as "", so differently laid-out Formats still compare.
function sameStyleDefinition(a: AssStyle, b: AssStyle, formatA: string[], formatB: string[]): boolean {
  const fa = styleAttributes(a, formatA);
  const fb = styleAttributes(b, formatB);
  for (const key of new Set([...fa.keys(), ...fb.keys()])) {
    if ((fa.get(key) ?? "") !== (fb.get(key) ?? "")) return false;
  }
  return true;
}

// Build a new target style carrying every source attribute. Target Format fields keep the
// target's key spelling; missing source fields follow in the source Format's order, then
// any source-only field names.
function newTargetStyle(name: string, source: AssStyle, sourceFormat: string[], targetFormat: string[]): AssStyle {
  const sourceNames: string[] = [];
  for (const n of sourceFormat) {
    if (!/^Name$/i.test(n) && !sourceNames.some((k) => fieldKey(k) === fieldKey(n))) sourceNames.push(n);
  }
  for (const n of Object.keys(source.fields)) {
    if (!sourceNames.some((k) => fieldKey(k) === fieldKey(n))) sourceNames.push(n);
  }
  const fields: Record<string, string> = {};
  const used = new Set<string>();
  for (const n of targetFormat) {
    if (/^Name$/i.test(n)) continue;
    const key = fieldKey(n);
    const from = sourceNames.find((s) => fieldKey(s) === key);
    fields[n] = from === undefined ? "" : fieldValue(source.fields, from);
    used.add(key);
  }
  for (const n of sourceNames) {
    const key = fieldKey(n);
    if (used.has(key)) continue;
    fields[n] = fieldValue(source.fields, n);
    used.add(key);
  }
  return { name, fields };
}

// Allocate a target name for every source style, in source stylesheet order (independent
// of the order the ids were selected in). Every target name and every pending source
// name is booked first, so a rename can never steal a name another pending style owns.
// An existing target style with an identical definition is reused when its name is a
// candidate; otherwise the original name or the first free `name (paste N)` is taken.
function allocateStyleNames(targetDoc: SubtitleDoc, styles: AssStyle[], sourceFormat: string[]): Map<string, string> {
  const target = targetDoc.styles ?? [];
  const targetFormat = targetDoc.assStyleFormat ?? DEFAULT_STYLE_FORMAT;
  const byName = new Map(target.map((s) => [s.name, s] as const));
  // Each pending source name is owned by its own style; other styles must not take it.
  const owner = new Map<string, string>(styles.map((s) => [s.name, s.name] as const));
  const assigned = new Set<string>();
  const mapping = new Map<string, string>();
  for (const style of styles) {
    for (let n = 1; ; n += 1) {
      const candidate = n === 1 ? style.name : `${style.name} (paste ${n})`;
      const existing = byName.get(candidate);
      if (existing) {
        if (sameStyleDefinition(style, existing, sourceFormat, targetFormat)) {
          mapping.set(style.name, candidate);
          break;
        }
        continue;
      }
      if (assigned.has(candidate)) continue;
      const ownedBy = owner.get(candidate);
      if (ownedBy !== undefined && ownedBy !== style.name) continue;
      mapping.set(style.name, candidate);
      break;
    }
    assigned.add(mapping.get(style.name)!);
  }
  return mapping;
}

// Source-only Style fields (in source Format order) absent from the target Format.
function missingStyleFields(styles: AssStyle[], sourceFormat: string[], targetFormat: string[]): string[] {
  const have = new Set(targetFormat.map(fieldKey));
  const extra: string[] = [];
  const push = (name: string): void => {
    const key = fieldKey(name);
    if (!have.has(key)) {
      have.add(key);
      extra.push(name);
    }
  };
  for (const name of sourceFormat) {
    if (!/^Name$/i.test(name) && styles.some((s) => hasField(s.fields, name))) push(name);
  }
  for (const style of styles) for (const name of Object.keys(style.fields)) push(name);
  return extra;
}

// Source-only Event fields, ordered by the source Format, then any stray keys.
function missingEventFields(cues: Cue[], sourceFormat: string[], targetFormat: string[]): string[] {
  const have = new Set(targetFormat.map(fieldKey));
  const extra: string[] = [];
  const push = (name: string): void => {
    const key = fieldKey(name);
    if (!have.has(key)) {
      have.add(key);
      extra.push(name);
    }
  };
  for (const name of sourceFormat) {
    if (!CORE_EVENT_FIELD.test(name) && cues.some((c) => hasField(c.assFields, name))) push(name);
  }
  for (const cue of cues) for (const name of Object.keys(cue.assFields ?? {})) push(name);
  return extra;
}

// Insert new event fields just before Text, which always stays last.
function insertBeforeText(format: string[], extra: string[]): string[] {
  if (!extra.length) return format;
  const at = format.findIndex((f) => /^Text$/i.test(f));
  const pos = at < 0 ? format.length : at;
  return [...format.slice(0, pos), ...extra, ...format.slice(pos)];
}

// Re-emit the last Format line of a verbatim chunk, preserving the line prefix and EOL.
function replaceLastFormatLine(text: string, eol: string, format: string[]): string {
  if (!text) return text;
  const lines = text.split(eol);
  for (let i = lines.length - 1; i >= 0; i -= 1) {
    if (/^\s*Format\s*:/i.test(lines[i])) {
      lines[i] = `Format: ${format.join(", ")}`;
      return lines.join(eol);
    }
  }
  return text;
}

// Re-key a pasted event's fields to the target Format (case-insensitively, so differently
// spelled keys survive), then set its Style through the name map.
function remapEventFields(cue: Cue, eventFormat: string[], styleName: string): Cue {
  const assFields: Record<string, string> = {};
  for (const name of eventFormat) {
    if (CORE_EVENT_FIELD.test(name)) continue;
    assFields[name] = fieldValue(cue.assFields, name);
  }
  const styleKey = formatName(eventFormat, /^Style$/i) ?? "Style";
  assFields[styleKey] = styleName;
  for (const name of Object.keys(cue.assFields ?? {})) {
    if (!hasField(assFields, name)) assFields[name] = cue.assFields![name];
  }
  return { ...cue, assFields };
}

export interface PasteCueClipboardResult {
  doc: SubtitleDoc;
  newIds: string[];
  // original source style name -> target style name, covering the whole closure.
  styleMap: Record<string, string>;
}

// Paste the clipboard cues into `targetDoc`, after `afterIndex`. Timing, insertion point
// and fresh ids follow pasteCues. ASS -> ASS also migrates the referenced styles; other
// target formats fall back to the plain cue paste. Neither input object is modified.
export function pasteCueClipboard(
  targetDoc: SubtitleDoc,
  clipboard: AssStyleClipboard,
  afterIndex: number,
): PasteCueClipboardResult {
  if (!clipboard.cues.length) return { doc: targetDoc, newIds: [], styleMap: {} };
  if (targetDoc.format !== "ass" || !clipboard.styles.length || targetDoc.styles === undefined) {
    const { cues, newIds } = pasteCues(targetDoc.cues, clipboard.cues, afterIndex);
    return { doc: { ...targetDoc, cues }, newIds, styleMap: {} };
  }

  // Fix the whole name mapping first, so every rewrite below uses the final names.
  const styleMap = allocateStyleNames(targetDoc, clipboard.styles, clipboard.styleFormat);
  const nameOf = (name: string): string => styleMap.get(name) ?? name;

  // Grow the target's Formats only as far as migrated definitions/events require, so a
  // custom field survives save -> parse. The verbatim Format text is kept in sync.
  const targetStyleFormat = targetDoc.assStyleFormat ?? DEFAULT_STYLE_FORMAT;
  const styleExtra = missingStyleFields(clipboard.styles, clipboard.styleFormat, targetStyleFormat);
  const effectiveStyleFormat = [...targetStyleFormat, ...styleExtra];
  const targetEventFormat = targetDoc.assFormat ?? DEFAULT_EVENT_FORMAT;
  const eventExtra = missingEventFields(clipboard.cues, clipboard.eventFormat, targetEventFormat);
  const effectiveEventFormat = insertBeforeText(targetEventFormat, eventExtra);

  // Reused definitions stay exactly as they are; missing ones are appended in source order.
  const styles = (targetDoc.styles ?? []).map((s) => ({ ...s, fields: { ...s.fields } }));
  const present = new Set(styles.map((s) => s.name));
  for (const source of clipboard.styles) {
    const name = nameOf(source.name);
    if (present.has(name)) continue;
    styles.push(newTargetStyle(name, source, clipboard.styleFormat, effectiveStyleFormat));
    present.add(name);
  }

  const rewritten = clipboard.cues.map((c) => {
    const mapped = remapEventFields(c, effectiveEventFormat, nameOf(styleFieldValue(c, clipboard.eventFormat)));
    // Style/event notes belong to the source file, not to the dialogue being migrated.
    delete mapped.notesBefore;
    return { ...mapped, text: rewriteNamedResets(mapped.text, nameOf) };
  });
  const { cues, newIds } = pasteCues(targetDoc.cues, rewritten, afterIndex);

  const doc: SubtitleDoc = { ...targetDoc, cues, styles };
  if (styleExtra.length) {
    doc.assStyleFormat = effectiveStyleFormat;
    doc.assScriptInfo = replaceLastFormatLine(doc.assScriptInfo ?? "", doc.eol, effectiveStyleFormat);
  }
  if (eventExtra.length) {
    doc.assFormat = effectiveEventFormat;
    // The Events Format line ends whichever chunk holds the Events header.
    if (doc.assStylesTail !== undefined) {
      doc.assStylesTail = replaceLastFormatLine(doc.assStylesTail, doc.eol, effectiveEventFormat);
    } else {
      doc.assScriptInfo = replaceLastFormatLine(doc.assScriptInfo ?? "", doc.eol, effectiveEventFormat);
    }
  }
  return { doc, newIds, styleMap: Object.fromEntries(styleMap) };
}
