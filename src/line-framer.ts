// Splits a byte stream into physical lines as the bytes arrive, so that a ReadableStream
// is parsed line by line instead of being copied into one message sized buffer first.
//
// The framing mirrors PostalMime.readLine on a whole buffer: a line ends at an LF, and the
// run of CRs immediately in front of it belongs to the line break together with the LF, so
// `a\r\n` is the line `a` with the break `\r\n` and `a\r\r\n` the line `a` with `\r\r\n`.
// A bare CR anywhere else is line content. At the physical end a line without a break gets
// an LF, and CRs trailing that last line are dropped, the way readLine drops them.
//
// The framer owns the bytes it keeps: every piece of a partial line is copied out of the
// source chunk, because a source may hand over a view into a working buffer it refills on
// the next pull. Nothing that outlives a chunk keeps a reference into it.

export interface FramedLine {
    /** the line without its line break */
    bytes: Uint8Array<ArrayBuffer>;
    /** the exact bytes between the line and the next one, written back out by the decoders */
    lineBreak: Uint8Array<ArrayBuffer>;
}

const LINE_FEED = new Uint8Array([0x0a]);
const CR_LF = new Uint8Array([0x0d, 0x0a]);
const CARRIAGE_RETURN = 0x0d;
const LINE_FEED_CODE = 0x0a;

export default class LineFramer {
    // The pieces of the line currently being assembled, each an independent copy. Kept as
    // pieces and joined when the line closes: growing one prefix by recopying it for every
    // chunk would be quadratic in the length of a long line split across many chunks.
    private pieces: Uint8Array<ArrayBuffer>[] = [];
    private lineLength = 0;
    private finished = false;

    private append(bytes: Uint8Array<ArrayBuffer>): void {
        if (bytes.length) {
            this.pieces.push(bytes);
            this.lineLength += bytes.length;
        }
    }

    // Joins the pieces of a line into one owned buffer and peels the CR run at its end.
    // The peeled CRs are the CR part of the line break, not line content.
    private closeLine(withLineFeed: boolean): FramedLine {
        let joined: Uint8Array<ArrayBuffer>;
        if (this.pieces.length === 1) {
            joined = this.pieces[0];
        } else {
            joined = new Uint8Array(this.lineLength);
            let offset = 0;
            for (const piece of this.pieces) {
                joined.set(piece, offset);
                offset += piece.length;
            }
        }
        this.pieces = [];
        this.lineLength = 0;

        let peel = 0;
        while (peel < joined.length && joined[joined.length - 1 - peel] === CARRIAGE_RETURN) {
            peel++;
        }

        const bytes = peel ? (joined.slice(0, joined.length - peel) as Uint8Array<ArrayBuffer>) : joined;
        const lineBreak = this.buildBreak(peel, withLineFeed);
        return { bytes, lineBreak };
    }

    private buildBreak(carriageReturns: number, withLineFeed: boolean): Uint8Array<ArrayBuffer> {
        if (!withLineFeed) {
            // the physical end adds a single LF, any trailing CRs were content that readLine trims
            return LINE_FEED;
        }
        if (carriageReturns === 0) {
            return LINE_FEED;
        }
        if (carriageReturns === 1) {
            return CR_LF;
        }
        const lineBreak = new Uint8Array(carriageReturns + 1);
        lineBreak.fill(CARRIAGE_RETURN, 0, carriageReturns);
        lineBreak[carriageReturns] = LINE_FEED_CODE;
        return lineBreak;
    }

    /**
     * Feeds one source chunk and returns the lines that closed inside it. An empty chunk
     * produces nothing and never ends the message: only end() does.
     */
    push(chunk: Uint8Array): FramedLine[] {
        if (this.finished || !chunk.length) {
            return [];
        }

        const lines: FramedLine[] = [];
        // first index of the chunk not yet copied into the open line
        let segmentStart = 0;
        const len = chunk.length;

        for (let i = 0; i < len; i++) {
            if (chunk[i] !== LINE_FEED_CODE) {
                continue;
            }
            this.append(chunk.slice(segmentStart, i) as Uint8Array<ArrayBuffer>);
            lines.push(this.closeLine(true));
            segmentStart = i + 1;
        }

        if (segmentStart < len) {
            this.append(chunk.slice(segmentStart, len) as Uint8Array<ArrayBuffer>);
        }

        return lines;
    }

    /**
     * Closes the source. A line still open is emitted once with an LF, as the buffered
     * reader does for a message that ends without a line break, its trailing CRs trimmed.
     */
    end(): FramedLine[] {
        if (this.finished) {
            return [];
        }
        this.finished = true;

        // Nothing followed the last LF, so the last line was already emitted
        if (!this.lineLength) {
            return [];
        }

        return [this.closeLine(false)];
    }
}
