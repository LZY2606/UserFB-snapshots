import { transform } from "./index.js";

// Record errors eligible for resynchronization. Errors thrown by user
// callbacks (`cast`, `on_record`, `on_quarantine`) are plain errors without
// one of these codes and therefore always terminate the parsing.
const recoverable_codes = new Set([
  "CSV_INVALID_CLOSING_QUOTE",
  "INVALID_OPENING_QUOTE",
  "CSV_QUOTE_NOT_CLOSED",
  "CSV_NON_TRIMABLE_CHAR_AFTER_CLOSING_QUOTE",
  "CSV_RECORD_INCONSISTENT_FIELDS_LENGTH",
  "CSV_RECORD_INCONSISTENT_COLUMNS",
  "CSV_MAX_RECORD_SIZE",
]);

const noop = function () {};

// Encapsulate the record resynchronization logic. A single instance is
// attached to the parser and stays dormant until a recoverable error is
// encountered.
const create_recovery = function (api) {
  const options = api.options;
  const recover_options = options.recover_records;
  const recovery = {
    // True between the detection of a damaged record and the delivery of the
    // corresponding quarantine report.
    active: false,
    // Absolute byte offset at which the damaged record starts.
    start: 0,
    // Absolute byte offset of the character triggering the error.
    error_offset: 0,
    // The original error delivered through the quarantine report.
    error: null,
    // Retention chain of the original, immutable input chunks, each described
    // by its absolute half open byte range.
    chain: [],
    // Physical record delimiter endings already discovered, absolute offsets.
    candidates: [],
    // Bytes up to which the physical delimiter scan has been performed.
    scanned_up_to: 0,
    // Probe state
    probe: null,
    probe_index: -1,
    probe_start: 0,
    probe_fed: 0,
    probe_pushed: 0,
    probe_failed: false,
    probe_error: undefined,
    isRecoverable: function (err) {
      return err !== null && recoverable_codes.has(err.code);
    },
    chainEnd: function () {
      const chain = this.chain;
      return chain.length === 0 ? 0 : chain[chain.length - 1].end;
    },
    // Keep a reference (never a copy) on every incoming chunk. Chunks are
    // contiguous and are received in order.
    register: function (buf) {
      const start = this.chainEnd();
      this.chain.push({ buf: buf, start: start, end: start + buf.length });
    },
    // Release chunks fully located before `keep_abs`, i.e. already covered by
    // successfully published records. Nothing is released while a quarantine
    // is in progress.
    release: function (keep_abs) {
      if (this.active) return;
      this.chain = this.chain.filter((entry) => entry.end > keep_abs);
    },
    // Build a contiguous, standalone buffer for an absolute half open range.
    slice: function (from, to) {
      const parts = [];
      for (const entry of this.chain) {
        if (entry.end <= from) continue;
        if (entry.start >= to) break;
        const start = Math.max(from, entry.start) - entry.start;
        const end = Math.min(to, entry.end) - entry.start;
        parts.push(entry.buf.subarray(start, end));
      }
      return Buffer.concat(parts);
    },
    // Resolve the record delimiter through the same auto-discovery rules as
    // the main parser when the first record fails before any newline.
    ensure_record_delimiter: function () {
      if (options.record_delimiter.length > 0) return true;
      const { encoding } = options;
      const candidates = [
        Buffer.from("\r\n", encoding),
        Buffer.from("\n", encoding),
        Buffer.from("\r", encoding),
      ];
      const end = this.chainEnd();
      let found = null;
      let foundAt = Infinity;
      for (const rd of candidates) {
        const at = this.firstIndexOf(rd, 0, end);
        if (at !== -1 && at < foundAt) {
          foundAt = at;
          found = rd;
        }
      }
      if (found === null) return false;
      options.record_delimiter.push(found);
      api.state.recordDelimiterMaxLength = found.length;
      return true;
    },
    firstIndexOf: function (needle, from, to) {
      const buf = this.slice(from, to);
      const limit = buf.length - needle.length;
      for (let i = 0; i <= limit; i++) {
        let matched = true;
        for (let j = 0; j < needle.length; j++) {
          if (buf[i + j] !== needle[j]) {
            matched = false;
            break;
          }
        }
        if (matched) return from + i;
      }
      return -1;
    },
    // Enumerate every physical record delimiter ending located after the
    // error position, quoted newlines included.
    scan_candidates: function () {
      if (this.ensure_record_delimiter() === false) {
        // No record delimiter discovered yet, wait for more bytes or EOF.
        return;
      }
      const rd = options.record_delimiter[0];
      const rd_length = rd.length;
      const end = this.chainEnd();
      // Restart a few bytes before the previous position to catch a
      // delimiter split across two chunks.
      const from = Math.max(
        this.error_offset,
        this.scanned_up_to - (rd_length - 1),
      );
      if (end < from + rd_length) {
        this.scanned_up_to = end;
        return;
      }
      const buf = this.slice(from, end);
      const limit = buf.length - rd_length;
      for (let i = 0; i <= limit; i++) {
        let matched = true;
        for (let j = 0; j < rd_length; j++) {
          if (buf[i + j] !== rd[j]) {
            matched = false;
            break;
          }
        }
        if (matched) {
          const candidate = from + i + rd_length;
          if (candidate > this.error_offset) {
            const previous = this.candidates[this.candidates.length - 1];
            if (previous !== candidate) this.candidates.push(candidate);
          }
        }
      }
      this.scanned_up_to = end;
    },
    // A probe is a full, isolated parser running on a tentative buffer. It
    // never receives the user callbacks and stops as soon as `confirm`
    // records have been observed, without looking any further.
    create_probe: function () {
      const probe = transform({
        ...api.original_options,
        cast: false,
        on_record: undefined,
        on_skip: undefined,
        recover_records: undefined,
        // Neutralize record windowing so the tentative records themselves
        // decide the outcome of the probe.
        from: 1,
        from_line: 1,
        to_line: -1,
        to: recover_options.confirm,
      });
      return probe;
    },
    probe_push: () => {
      recovery.probe_pushed++;
      // With `columns: false`, the probe only learns the expected length from
      // its first record, enforce the configured `field_count` explicitly.
      if (recovery.probe.state.record.length !== recover_options.field_count) {
        recovery.probe_failed = true;
      }
    },
    advance_probe: function () {
      // Do not consume an index while no candidate is available yet.
      if (this.probe_index + 1 >= this.candidates.length) {
        this.probe = null;
        return false;
      }
      this.probe_index++;
      this.probe_start = this.candidates[this.probe_index];
      this.probe = this.create_probe();
      this.probe_fed = this.probe_start;
      this.probe_pushed = 0;
      this.probe_failed = false;
      this.probe_error = undefined;
      this.feed_probe(false);
      return true;
    },
    feed_probe: function (end) {
      const chain_end = this.chainEnd();
      if (chain_end > this.probe_fed) {
        const err = this.probe.parse(
          this.slice(this.probe_fed, chain_end),
          false,
          this.probe_push,
          noop,
        );
        this.probe_fed = chain_end;
        if (err !== undefined) this.probe_error = err;
      }
      if (end) {
        const err = this.probe.parse(undefined, true, this.probe_push, noop);
        if (err !== undefined) this.probe_error = err;
      }
    },
    probe_succeeded: function () {
      return (
        this.probe_error === undefined &&
        this.probe_failed === false &&
        this.probe_pushed >= recover_options.confirm
      );
    },
    begin: function (err, end, push, close) {
      const state = api.state;
      this.active = true;
      this.start = state.recordStartOffset;
      this.error = err;
      if (end === true && err.code === "CSV_QUOTE_NOT_CLOSED") {
        // An unclosed quote is only reported at EOF, where the parse position
        // is past the last byte; its origin is the opening quote instead.
        this.error_offset = state.recordStartOffset;
      } else {
        this.error_offset = state.bufBytesStart + state.parsePos;
      }
      this.scanned_up_to = this.error_offset;
      this.candidates = [];
      this.probe = null;
      this.probe_index = -1;
      if (end) return this.handle_input(true, push, close);
      return this.run_probes(push, close);
    },
    handle_input: function (end, push, close) {
      if (end) return this.handle_eof(push, close);
      return this.run_probes(push, close);
    },
    run_probes: function (push, close) {
      for (;;) {
        this.scan_candidates();
        if (this.probe === null && !this.advance_probe()) {
          // Wait for more bytes, no candidate can be confirmed yet.
          return undefined;
        }
        this.feed_probe(false);
        if (this.probe_error !== undefined || this.probe_failed) {
          this.probe = null;
          continue;
        }
        if (this.probe_succeeded()) {
          return this.finalize(push, close);
        }
        return undefined;
      }
    },
    handle_eof: function (push, close) {
      for (;;) {
        this.scan_candidates();
        if (this.probe === null && !this.advance_probe()) break;
        this.feed_probe(true);
        if (this.probe_succeeded()) {
          // Reparse the confirmed tail through the official parser, including
          // its final, newline-less record.
          const err = this.finalize(push, close);
          if (err !== undefined) return err;
          return api.__parse(undefined, true, push, close);
        }
        this.probe = null;
      }
      // Not enough complete confirmation records: quarantine until EOF and
      // end successfully, previously published records are preserved.
      return this.quarantine_to_eof(push);
    },
    reset_main_state: function (offset) {
      const state = api.state;
      state.field.reset();
      state.record = [];
      state.record_length = 0;
      state.quoting = false;
      state.wasQuoting = false;
      state.escaping = false;
      state.commenting = false;
      state.wasRowDelimiter = false;
      state.recordHasError = false;
      state.error = undefined;
      state.parsePos = 0;
      state.rawBuffer.reset();
      state.bufBytesStart = offset;
      state.recordStartOffset = offset;
    },
    // Deliver one quarantine report and hand the confirmed tail back to the
    // official parser. The tail is re-parsed exactly once.
    finalize: function (push, close) {
      const boundary = this.probe_start;
      const err = this.emit_quarantine(boundary, push);
      if (err !== undefined) return err;
      const tail = this.slice(boundary, this.chainEnd());
      this.reset_main_state(boundary);
      api.state.previousBuf = tail.length === 0 ? undefined : tail;
      this.active = false;
      this.chain =
        tail.length === 0
          ? []
          : [{ buf: tail, start: boundary, end: boundary + tail.length }];
      this.candidates = [];
      this.probe = null;
      return undefined;
    },
    quarantine_to_eof: function (push) {
      const end = this.chainEnd();
      const err = this.emit_quarantine(end, push);
      if (err !== undefined) return err;
      this.reset_main_state(end);
      api.state.previousBuf = undefined;
      this.active = false;
      this.chain = [];
      this.candidates = [];
      this.probe = null;
      return undefined;
    },
    emit_quarantine: function (end, push) {
      // `Buffer.from` guarantees an independent copy, even for a single chunk.
      const report = {
        error: this.error,
        start: this.start,
        end: end,
        buffer: Buffer.from(this.slice(this.start, end)),
      };
      try {
        recover_options.on_quarantine.call(null, report);
      } catch (err) {
        return err;
      }
      return undefined;
    },
  };
  return recovery;
};

export { create_recovery, recoverable_codes };
