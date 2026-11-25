#!/usr/bin/env node

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import sax from "../vendor/sax/lib/sax.js";
import { fromParser } from "../src/main/js/doc.js";
import { generateISD } from "../src/main/js/isd.js";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");

function fail(message) {
    console.error("animate_frames: " + message);
    process.exit(1);
}

async function main() {
    if (process.argv.length !== 5) {
        fail("usage: node examples/animate_frames.mjs input.ttml times.json output.json");
    }

    const [inputPath, timesPath, outputPath] = process.argv.slice(2);

    let xml;
    let timesInput;

    try {
        xml = fs.readFileSync(path.resolve(process.cwd(), inputPath), "utf8");
        timesInput = JSON.parse(
            fs.readFileSync(path.resolve(process.cwd(), timesPath), "utf8"),
        );
    } catch (error) {
        fail(error && error.message ? error.message : String(error));
    }

    if (!Array.isArray(timesInput) ||
        !timesInput.every((t) => typeof t === "number" && Number.isFinite(t))) {
        fail("times.json must contain an array of finite numbers");
    }

    const errorHandler = {
        error(message) {
            throw new Error(message);
        },
        fatal(message) {
            throw new Error(message);
        },
    };

    let doc;

    try {
        doc = fromParser(
            xml,
            errorHandler,
            null,
            sax.parser(true, { xmlns: true }),
        );
    } catch (error) {
        fail(error && error.message ? error.message : String(error));
    }

    const result = {
        events: doc.getMediaTimeEvents(),
        ranges: doc.getContinuousAnimationRanges(),
        frames: timesInput.map((time) => ({
            time,
            isd: generateISD(doc, time, errorHandler),
        })),
    };

    try {
        const resolvedOutput = path.resolve(process.cwd(), outputPath);
        fs.mkdirSync(path.dirname(resolvedOutput), { recursive: true });
        fs.writeFileSync(resolvedOutput, JSON.stringify(result, null, 2));
    } catch (error) {
        fail(error && error.message ? error.message : String(error));
    }
}

main().catch((error) => fail(error && error.message ? error.message : String(error)));
