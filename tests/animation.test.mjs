/*
 * Regression tests for offline TTML2 inline <animate> support.
 *
 * Registered through task_support/check.mjs in three groups: clock, cascade
 * and output. Every case starts from real XML parsed through the public
 * fromParser()/generateISD() entry points using the bundled SAX parser, and
 * performs concrete numerical assertions on ISD styles, media time events and
 * continuous animation ranges.
 */

import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { execFileSync } from "node:child_process";

import sax from "../vendor/sax/lib/sax.js";
import { fromParser } from "../src/main/js/doc.js";
import { generateISD } from "../src/main/js/isd.js";
import { taskCase } from "../task_support/check.mjs";

const TTS = "http://www.w3.org/ns/ttml#styling ";
const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");

const ERROR_HANDLER = {
    error: m => { throw new Error(m); },
    fatal: m => { throw new Error(m); },
};

function parse(xml) {
    return fromParser(xml, ERROR_HANDLER, null, sax.parser(true, { xmlns: true }));
}

function wrap(body, head = "") {
    return "<tt xmlns=\"http://www.w3.org/ns/ttml\" xmlns:tts=\"http://www.w3.org/ns/ttml#styling\">" +
        head + "<body>" + body + "</body></tt>";
}

function regionDoc(regionInner, regionAttrs = "dur=\"6s\"", bodyInner = "<div region=\"r\"><p dur=\"6s\">A</p></div>") {
    return "<tt xmlns=\"http://www.w3.org/ns/ttml\" xmlns:tts=\"http://www.w3.org/ns/ttml#styling\">" +
        "<head><layout><region xml:id=\"r\" " + regionAttrs + ">" + regionInner +
        "</region></layout></head><body>" + bodyInner + "</body></tt>";
}

function walk(element, visit) {
    visit(element);
    for (const child of element.contents || []) walk(child, visit);
}

function findById(isd, id) {
    let found = null;
    walk(isd, e => { if (e.id === id) found = e; });
    return found;
}

function findKind(isd, kind, nth = 0) {
    const matches = [];
    walk(isd, e => { if (e.kind === kind) matches.push(e); });
    return matches[nth] || null;
}

const near = (a, b, eps = 1e-9) => typeof a === "number" && Math.abs(a - b) <= eps;

/* ------------------------------------------------------------------ */
/* clock group: timing, repetition, clipping, events and ranges       */
/* ------------------------------------------------------------------ */

taskCase("clock", check => {

    /* seq successor is pushed by the full active interval, not one cycle */

    const seq = parse(wrap(
        "<div timeContainer=\"seq\">" +
        "<p><animate dur=\"2s\" repeatCount=\"2.25\" fill=\"freeze\" tts:fontSize=\"10%;20%\"/>A</p>" +
        "<p dur=\"1s\">B</p>" +
        "</div>"));

    const seqDiv = seq.body.contents[0];
    const first = seqDiv.contents[0];
    const second = seqDiv.contents[1];

    check(first.begin === 0 && first.end === 4.5, "partial repeat extends seq host end to 4.5");
    check(second.begin === 4.5 && second.end === 5.5, "seq successor starts at the active end");

    /* explicit end truncates the active interval and freeze holds that point */

    const truncated = parse(regionDoc(
        "<animate dur=\"2s\" repeatCount=\"2.25\" end=\"3s\" fill=\"freeze\" tts:opacity=\"0;1\"/>"));
    const truncAnim = truncated.head.layout.regions.r.animates[0];

    check(truncAnim.activeEnd === 3, "explicit end truncates active duration to 3s");
    check(near(findById(generateISD(truncated, 3 - 1e-9), "r").styleAttrs[TTS + "opacity"], 0.5),
        "truncated freeze left-limit is 0.5 (half cycle)");
    check(near(findById(generateISD(truncated, 5), "r").styleAttrs[TTS + "opacity"], 0.5),
        "freeze keeps 0.5 until the host ends");

    /* full partial-cycle freeze value: 2.25 repeats of 0->1 freezes at 0.25 */

    const partial = parse(regionDoc(
        "<animate dur=\"2s\" repeatCount=\"2.25\" fill=\"freeze\" tts:opacity=\"0;1\"/>"));

    check(near(findById(generateISD(partial, 1), "r").styleAttrs[TTS + "opacity"], 0.5),
        "linear half-cycle opacity is 0.5");
    check(near(findById(generateISD(partial, 5), "r").styleAttrs[TTS + "opacity"], 0.25),
        "partial repeat freezes at 0.25");

    /* repeat boundary: left limit keeps the last value, entering a new cycle
     * restarts at the first value */

    check(near(findById(generateISD(partial, 2 - 1e-9), "r").styleAttrs[TTS + "opacity"], 1 - 5e-10),
        "left of a cycle boundary is the last key value");
    check(near(findById(generateISD(partial, 2), "r").styleAttrs[TTS + "opacity"], 0),
        "on the cycle boundary the first value returns");
    check(near(findById(generateISD(partial, 4), "r").styleAttrs[TTS + "opacity"], 0),
        "second cycle boundary also restarts at the first value");

    /* remove fill: after the active interval the animation no longer applies,
     * so the region falls back to its initial opacity of 1 */

    const removed = parse(regionDoc("<animate dur=\"2s\" tts:opacity=\"0;1\"/>"));

    check(near(findById(generateISD(removed, 1), "r").styleAttrs[TTS + "opacity"], 0.5),
        "remove-fill animation applies while active");
    check(near(findById(generateISD(removed, 2), "r").styleAttrs[TTS + "opacity"], 1),
        "remove-fill restores the initial opacity after the active interval");

    /* phase is preserved across an ancestor visibility clip: a region that
     * only becomes visible at t=2 must not shift or rescale the cycle, which
     * started at t=0 */

    const clipped = parse(
        "<tt xmlns=\"http://www.w3.org/ns/ttml\" xmlns:tts=\"http://www.w3.org/ns/ttml#styling\">" +
        "<head><layout><region xml:id=\"r\" begin=\"2s\" end=\"6s\"/></layout></head>" +
        "<body><div region=\"r\"><p dur=\"10s\">" +
        "<animate dur=\"4s\" repeatCount=\"2\" tts:fontSize=\"10%;20%\"/>A</p></div></body></tt>");

    check(near(findKind(generateISD(clipped, 2), "p").styleAttrs[TTS + "fontSize"].rh, 0.01),
        "clipped entry at t=2 evaluates phase 0.5 (15%), not the first value");
    check(near(findKind(generateISD(clipped, 4), "p").styleAttrs[TTS + "fontSize"].rh, 1 / 150),
        "cycle boundary inside the clip restarts at 10%");

    /* animation events are clipped to the visibility window: the active
     * interval 0..8 contributes only 2,4,6 (0 and 8 fall outside the clip);
     * 0 and 10 remain as ordinary host/body events */

    const clippedEvents = clipped.getMediaTimeEvents();

    check(JSON.stringify(clippedEvents) === JSON.stringify([0, 2, 4, 6, 10]),
        "cycle/keyTimes events outside the ancestor clip are not registered");
    check(!clippedEvents.some(t => t > 0 && t < 2),
        "no animation event is registered before the region becomes visible");

    /* media time events: clipped begin/end, cycle boundaries and keyTimes,
     * sorted and de-duplicated, never enumerating video frames */

    const probe = parse(regionDoc(
        "<animate dur=\"2s\" repeatCount=\"2.25\" fill=\"freeze\" tts:opacity=\"0;1\"/>"));

    check(JSON.stringify(probe.getMediaTimeEvents()) === JSON.stringify([0, 2, 4, 4.5, 6]),
        "events list clipped begin, cycle boundaries, active end and host end");

    const keyed = parse(wrap("<div><p dur=\"10s\">" +
        "<animate dur=\"2s\" repeatCount=\"2\" keyTimes=\"0;0.5;1\" tts:fontSize=\"10%;20%;30%\"/>X</p></div>"));

    check(JSON.stringify(keyed.getMediaTimeEvents()) === JSON.stringify([0, 1, 2, 3, 4, 10]),
        "keyTimes instants are registered for every cycle");

    /* ranges: linear effective intervals, freeze tails and discrete excluded */

    check(JSON.stringify(partial.getContinuousAnimationRanges()) === JSON.stringify([[0, 4.5]]),
        "linear range covers the active interval only");
    check(JSON.stringify(truncated.getContinuousAnimationRanges()) === JSON.stringify([[0, 3]]),
        "truncated range ends at the active end, excluding the freeze tail");
    check(JSON.stringify(clipped.getContinuousAnimationRanges()) === JSON.stringify([[2, 6]]),
        "range is clipped by ancestor visibility without shifting the phase");

    const discrete = parse(wrap("<div><p dur=\"3s\">" +
        "<animate dur=\"2s\" repeatCount=\"2\" calcMode=\"discrete\" tts:fontSize=\"10%;20%\"/>X</p></div>"));

    check(discrete.getContinuousAnimationRanges().length === 0,
        "discrete animations produce no continuous ranges");

    /* adjacent/overlapping linear ranges are merged */

    const adjacent = parse(regionDoc(
        "<animate dur=\"2s\" tts:opacity=\"0;1\"/>" +
        "<animate dur=\"2s\" begin=\"2s\" tts:opacity=\"1;0\"/>"));

    check(JSON.stringify(adjacent.getContinuousAnimationRanges()) === JSON.stringify([[0, 4]]),
        "adjacent animation ranges are merged");
});

/* ------------------------------------------------------------------ */
/* cascade group: value resolution, inheritance, ordering and ruby    */
/* ------------------------------------------------------------------ */

taskCase("cascade", check => {

    /* linear interpolation of fontSize */

    const linear = parse(wrap("<div><p dur=\"10s\">" +
        "<animate dur=\"2s\" tts:fontSize=\"10%;20%\"/>X</p></div>"));

    check(near(findKind(generateISD(linear, 0.5), "p").styleAttrs[TTS + "fontSize"].rh, 0.008333333333333333),
        "linear interpolation at 25% progress");
    check(near(findKind(generateISD(linear, 1.999), "p").styleAttrs[TTS + "fontSize"].rh, 0.01333),
        "linear interpolation near the end of a cycle");

    /* discrete switches immediately at key times */

    const discrete = parse(wrap("<div><p dur=\"10s\">" +
        "<animate dur=\"2s\" repeatCount=\"2\" calcMode=\"discrete\" keyTimes=\"0;0.5;1\" " +
        "tts:fontSize=\"10%;20%;30%\"/>X</p></div>"));

    check(near(findKind(generateISD(discrete, 0), "p").styleAttrs[TTS + "fontSize"].rh, 1 / 150),
        "discrete first value at t=0");
    check(near(findKind(generateISD(discrete, 1), "p").styleAttrs[TTS + "fontSize"].rh, 0.013333333333333334),
        "discrete switches to the second value exactly at its key time");
    check(near(findKind(generateISD(discrete, 2 - 1e-9), "p").styleAttrs[TTS + "fontSize"].rh, 0.013333333333333334),
        "discrete holds the second value until the cycle boundary (the end key is not reached)");
    check(near(findKind(generateISD(discrete, 2), "p").styleAttrs[TTS + "fontSize"].rh, 1 / 150),
        "discrete restarts at the first value on a new cycle");

    /* child percentage fontSize resolves against the animated parent value */

    const percents = parse(wrap("<div><p dur=\"10s\">" +
        "<animate dur=\"10s\" tts:fontSize=\"20%;40%\"/>" +
        "<span tts:fontSize=\"50%\">x</span></p></div>"));

    check(near(findKind(generateISD(percents, 0), "span").styleAttrs[TTS + "fontSize"].rh, 1 / 150),
        "child 50% resolves against animated parent 20%");
    check(near(findKind(generateISD(percents, 5), "span").styleAttrs[TTS + "fontSize"].rh, 0.01),
        "child percentage tracks the parent animation at half progress");

    /* body/div animated fontSize acts as an inheritance source */

    const divInherit = parse(wrap("<div><animate dur=\"10s\" tts:fontSize=\"20%;40%\"/>" +
        "<p dur=\"10s\">x</p></div>"));

    check(near(findKind(generateISD(divInherit, 0), "p").styleAttrs[TTS + "fontSize"].rh, 0.013333333333333334),
        "p inherits the div animated fontSize (20%)");
    check(near(findKind(generateISD(divInherit, 5), "p").styleAttrs[TTS + "fontSize"].rh, 0.02),
        "inherited div fontSize follows the animation (30% at half progress)");

    /* ruby default half-size follows the animated parent fontSize */

    const ruby = parse(wrap("<div><p dur=\"10s\">" +
        "<animate dur=\"10s\" tts:fontSize=\"20%;40%\"/>" +
        "<span tts:ruby=\"container\"><span tts:ruby=\"textContainer\">" +
        "<span tts:ruby=\"text\">rt</span></span>" +
        "<span tts:ruby=\"base\">base</span></span></p></div>"));

    const rubyAt0 = generateISD(ruby, 0);
    const rubyAt5 = generateISD(ruby, 5);

    check(near(findKind(rubyAt0, "p").styleAttrs[TTS + "fontSize"].rh, 0.013333333333333332),
        "ruby parent animated fontSize at t=0");
    check(near(findKind(rubyAt0, "span", 1).styleAttrs[TTS + "fontSize"].rh, 0.006666666666666666),
        "ruby text container defaults to half the animated parent size");
    check(near(findKind(rubyAt5, "span", 1).styleAttrs[TTS + "fontSize"].rh, 0.01),
        "ruby half-size tracks the parent animation");

    /* multiple animations on one host: XML order, later one overrides */

    const multi = parse(wrap("<div><p dur=\"12s\">" +
        "<animate dur=\"12s\" tts:fontSize=\"10%;20%\"/>" +
        "<animate dur=\"10s\" begin=\"2s\" tts:fontSize=\"30%;40%\"/>X</p></div>"));

    check(near(findKind(generateISD(multi, 1), "p").styleAttrs[TTS + "fontSize"].rh, 0.007222222222222222),
        "only the first animation is active before the second begins");
    check(near(findKind(generateISD(multi, 3), "p").styleAttrs[TTS + "fontSize"].rh, 0.020666666666666663),
        "the later animation overrides the earlier one for the same property");

    /* set is applied first, then the active animate overrides it */

    const ordered = parse(wrap("<div><p dur=\"10s\">" +
        "<set dur=\"10s\" tts:fontSize=\"80%\"/>" +
        "<animate dur=\"10s\" tts:fontSize=\"10%;20%\"/>X</p></div>"));

    check(near(findKind(generateISD(ordered, 5), "p").styleAttrs[TTS + "fontSize"].rh, 0.01),
        "active animate overrides the earlier set value for the same property");

    /* unsupported constructs are rejected rather than treated as static */

    const rejects = [
        wrap("<div><p dur=\"1s\"><animate dur=\"1s\" calcMode=\"paced\" tts:fontSize=\"10px;20px\"/></p></div>"),
        wrap("<div><p dur=\"1s\"><animateMotion dur=\"1s\"/></p></div>"),
        regionDoc("<animate dur=\"1s\" tts:fontSize=\"10px;20px\"/>"),
        regionDoc("<animate dur=\"1s\" tts:opacity=\"0;2\"/>"),
    ];

    for (const bad of rejects) {
        let threw = false;
        try { parse(bad); } catch { threw = true; }
        check(threw, "unsupported animation construct raises a parse error");
    }
});

/* ------------------------------------------------------------------ */
/* output group: ISD values, query isolation, events and the CLI      */
/* ------------------------------------------------------------------ */

taskCase("output", check => {

    const doc = parse(regionDoc(
        "<animate dur=\"2s\" repeatCount=\"2.25\" fill=\"freeze\" tts:opacity=\"0;1\"/>"));

    /* concrete ISD style values at sampled frames */

    check(near(findById(generateISD(doc, 1), "r").styleAttrs[TTS + "opacity"], 0.5),
        "sampled ISD opacity at t=1");
    check(near(findById(generateISD(doc, 5), "r").styleAttrs[TTS + "opacity"], 0.25),
        "sampled ISD opacity at t=5 (frozen)");

    /* reverse and interleaved queries are deterministic and order-independent */

    const forward = [0, 1, 2, 4.5, 5].map(t => JSON.stringify(generateISD(doc, t)));
    const shuffled = [5, 0, 4.5, 1, 2].map(t => JSON.stringify(generateISD(doc, t)));

    check(JSON.stringify(shuffled) === JSON.stringify([forward[4], forward[0], forward[3], forward[1], forward[2]]),
        "interleaved query order returns identical ISDs");
    check(JSON.stringify(generateISD(doc, 1)) === forward[1],
        "repeated query at the same time is identical");

    /* queries do not mutate the document or previously returned ISDs */

    const eventsBefore = JSON.stringify(doc.getMediaTimeEvents());
    const rangesBefore = JSON.stringify(doc.getContinuousAnimationRanges());
    const animSnapshot = JSON.stringify(doc.head.layout.regions.r.animates);

    const first = generateISD(doc, 1);
    const region = findById(first, "r");
    region.styleAttrs[TTS + "opacity"] = 999;
    region.contents.push({ tampered: true });

    const second = generateISD(doc, 1);

    check(near(findById(second, "r").styleAttrs[TTS + "opacity"], 0.5),
        "mutating a returned ISD does not affect later ISDs");
    check(findById(second, "r").contents.length !== findById(first, "r").contents.length + 1,
        "returned ISD content arrays are independent");
    check(JSON.stringify(doc.getMediaTimeEvents()) === eventsBefore,
        "media time events are unchanged by queries");
    check(JSON.stringify(doc.getContinuousAnimationRanges()) === rangesBefore,
        "animation ranges are unchanged by queries");
    check(JSON.stringify(doc.head.layout.regions.r.animates) === animSnapshot,
        "the parsed animation objects are unchanged by queries");

    /* a document without animation reports empty ranges and unchanged events */

    const staticDoc = parse(wrap("<div><p dur=\"2s\">plain</p></div>"));

    check(JSON.stringify(staticDoc.getContinuousAnimationRanges()) === "[]",
        "static document has no continuous animation ranges");
    check(JSON.stringify(staticDoc.getMediaTimeEvents()) === JSON.stringify([0, 2]),
        "static document events are unaffected");

    /* the example CLI produces output identical to calling the library */

    const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "animate-frames-"));
    const outPath = path.join(tmp, "output.json");
    const examplePath = path.join(ROOT, "examples", "animate_frames.mjs");
    const ttmlPath = path.join(ROOT, "task_support", "example.ttml");
    const timesPath = path.join(ROOT, "task_support", "times.json");

    execFileSync(process.execPath, [examplePath, ttmlPath, timesPath, outPath]);

    check(fs.existsSync(outPath) && fs.statSync(outPath).size > 0, "example writes a non-empty output file");

    const written = JSON.parse(fs.readFileSync(outPath, "utf8"));
    const libDoc = parse(fs.readFileSync(ttmlPath, "utf8"));
    const times = JSON.parse(fs.readFileSync(timesPath, "utf8"));
    const expected = {
        events: libDoc.getMediaTimeEvents(),
        ranges: libDoc.getContinuousAnimationRanges(),
        frames: times.map(time => ({ time, isd: generateISD(libDoc, time) })),
    };

    check(JSON.stringify(written) === JSON.stringify(expected),
        "example JSON matches direct fromParser/generateISD output");
    check(written.frames.length === times.length &&
        written.frames.every((f, i) => f.time === times[i] && "isd" in f),
        "example emits one {time, isd} frame per requested time");

    /* the CLI exits non-zero on error */

    let status = 0;
    try {
        execFileSync(process.execPath,
            [examplePath, path.join(tmp, "missing.ttml"), timesPath, path.join(tmp, "x.json")]);
    } catch (err) {
        status = err.status;
    }

    check(status === 1, "example exits non-zero when the input document is missing");

    fs.rmSync(tmp, { recursive: true, force: true });
});
