#!/usr/bin/env node

/*
 * Offline TTML2 inline-animation frame sampler.
 *
 * Usage:
 *   node examples/animate_frames.mjs input.ttml times.json output.json
 *
 * - input.ttml: an IMSC/TTML2 document containing inline <animate> elements
 * - times.json: a JSON array of media times (seconds) to sample
 * - output.json: written as { events, ranges, frames }, where each frame is
 *   { time, isd } and the ISDs are produced by the same public
 *   fromParser()/generateISD() entry points used by the library itself.
 *
 * The script uses the bundled SAX parser and no DOM. Any failure results in a
 * non-zero process exit.
 */

import fs from "node:fs";
import sax from "../vendor/sax/lib/sax.js";
import { fromParser } from "../src/main/js/doc.js";
import { generateISD } from "../src/main/js/isd.js";

function main() {

    const [, , inputPath, timesPath, outputPath] = process.argv;

    if (!inputPath || !timesPath || !outputPath) {
        throw new Error("Usage: animate_frames.mjs <input.ttml> <times.json> <output.json>");
    }

    const xml = fs.readFileSync(inputPath, "utf8");

    const times = JSON.parse(fs.readFileSync(timesPath, "utf8"));

    if (!Array.isArray(times) || times.some(t => typeof t !== "number" || !Number.isFinite(t))) {
        throw new Error(timesPath + " must contain a JSON array of finite numbers");
    }

    const errorHandler = {
        error: m => { throw new Error(m); },
        fatal: m => { throw new Error(m); },
    };

    const parser = sax.parser(true, { xmlns: true });

    const doc = fromParser(xml, errorHandler, null, parser);

    const events = doc.getMediaTimeEvents();
    const ranges = doc.getContinuousAnimationRanges();

    const frames = times.map(time => ({
        time,
        isd: generateISD(doc, time, errorHandler),
    }));

    fs.writeFileSync(outputPath, JSON.stringify({ events, ranges, frames }));
}

try {
    main();
} catch (err) {
    console.error(err && err.message ? err.message : String(err));
    process.exit(1);
}
