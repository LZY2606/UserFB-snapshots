import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

import { taskCase } from "../task_support/check.mjs";
import { find, generateISD, ns, parse, wrap } from "../task_support/xml.mjs";
import * as animation from "../src/main/js/animation.js";

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const OPACITY = ns + "opacity";
const FONT_SIZE = ns + "fontSize";
const RUBY = ns + "ruby";
const CELL_FONT = 1 / 15; /* default 1c with the default 15-row cell resolution */
const EPS = 1e-8;

function near(check, actual, expected, message, tolerance = 1e-7) {
    check(
        typeof actual === "number" && Number.isFinite(actual) &&
        Math.abs(actual - expected) <= tolerance,
        `${message}: expected ${expected}, got ${actual}`,
    );
}

function sameNumberList(actual, expected, tolerance = 1e-8) {
    return Array.isArray(actual) &&
        actual.length === expected.length &&
        actual.every((value, i) =>
            typeof value === "number" && Math.abs(value - expected[i]) <= tolerance);
}

function sameRanges(actual, expected, tolerance = 1e-8) {
    return Array.isArray(actual) &&
        actual.length === expected.length &&
        actual.every((range, i) =>
            Array.isArray(range) && range.length === 2 &&
            Math.abs(range[0] - expected[i][0]) <= tolerance &&
            Math.abs(range[1] - expected[i][1]) <= tolerance);
}

function opacityAt(doc, time, id = "r") {
    const node = find(generateISD(doc, time), id);
    return node ? node.styleAttrs[OPACITY] : undefined;
}

function fontSizeRHAt(doc, time, id = "p") {
    const node = find(generateISD(doc, time), id);
    return node && node.styleAttrs[FONT_SIZE] ? node.styleAttrs[FONT_SIZE].rh : undefined;
}

function expectThrow(check, fn, pattern, message) {
    let thrown = null;

    try {
        fn();
    } catch (error) {
        thrown = error;
    }

    check(thrown !== null, `${message}: expected an error`);
    check(
        thrown !== null && pattern.test(String(thrown.message || thrown)),
        `${message}: expected ${pattern}, got ${thrown && (thrown.message || thrown)}`,
    );
}

function findStyleValue(isd, styleName, value) {
    let result = null;

    function visit(node) {
        if (!node || result !== null) return;

        if (node.styleAttrs && node.styleAttrs[styleName] === value) {
            result = node;
            return;
        }

        for (const child of node.contents || []) visit(child);
    }

    visit(isd);
    return result;
}

const repeatXML = `
<tt xmlns="http://www.w3.org/ns/ttml" xmlns:tts="http://www.w3.org/ns/ttml#styling">
  <head><layout><region xml:id="r" dur="10s">
    <animate dur="2s" repeatCount="2.25" fill="freeze" tts:opacity="0;1"/>
  </region></layout></head>
  <body><div region="r"><p dur="10s">A</p></div></body>
</tt>`;

const endClipXML = `
<tt xmlns="http://www.w3.org/ns/ttml" xmlns:tts="http://www.w3.org/ns/ttml#styling">
  <head><layout><region xml:id="r" dur="10s">
    <animate dur="2s" repeatCount="2.25" end="3s" fill="freeze" tts:opacity="0;1"/>
  </region></layout></head>
  <body><div region="r"><p dur="10s">A</p></div></body>
</tt>`;

const regionClipXML = `
<tt xmlns="http://www.w3.org/ns/ttml" xmlns:tts="http://www.w3.org/ns/ttml#styling">
  <head><layout><region xml:id="r" begin="1s" end="3s"/></layout></head>
  <body><div region="r"><p dur="6s">
    <animate dur="2s" repeatCount="3" tts:fontSize="10%;110%"/>A
  </p></div></body>
</tt>`;

taskCase("clock", (check) => {
    /* Partial repeat drives a seq successor from the full active end. */
    const seqDoc = parse(wrap(
        '<div timeContainer="seq">' +
        '<animate dur="2s" repeatCount="2.25" tts:fontSize="10px;20px"/>' +
        '<p xml:id="next" dur="1s">N</p>' +
        "</div>",
    ));
    const seqP = seqDoc.body.contents[0].contents[0];
    near(check, seqP.begin, 4.5, "seq successor starts after dur*repeatCount");
    near(check, seqP.end, 5.5, "seq successor end");
    check(
        sameNumberList(seqDoc.getMediaTimeEvents(), [0, 2, 4, 4.5, 5.5]),
        `seq events got ${JSON.stringify(seqDoc.getMediaTimeEvents())}`,
    );
    check(
        sameRanges(seqDoc.getContinuousAnimationRanges(), [[0, 4.5]]),
        `seq ranges got ${JSON.stringify(seqDoc.getContinuousAnimationRanges())}`,
    );

    /* Repeat boundaries, interpolation and partial-repeat freeze. */
    const repeatDoc = parse(repeatXML);
    const repeatAnim = repeatDoc.head.layout.regions.r.animations[0];
    check(repeatAnim.repeatCount === 2.25, "repeatCount parsed");
    near(check, repeatAnim.end, 4.5, "partial repeat active end");
    near(check, opacityAt(repeatDoc, 0), 0, "first value at start");
    near(check, opacityAt(repeatDoc, 1), 0.5, "linear midpoint");
    near(check, opacityAt(repeatDoc, 2 - 1e-8), 1, "left of repeat boundary", 1e-6);
    near(check, opacityAt(repeatDoc, 2), 0, "exact repeat boundary returns first value");
    near(check, opacityAt(repeatDoc, 2 + 1e-8), 5e-9, "right of repeat boundary", 1e-6);
    near(check, opacityAt(repeatDoc, 4), 0, "second repeat boundary");
    near(check, opacityAt(repeatDoc, 4.5), 0.25, "partial repeat freeze value");
    near(check, opacityAt(repeatDoc, 5), 0.25, "freeze persists after active end");
    check(
        sameNumberList(repeatDoc.getMediaTimeEvents(), [0, 2, 4, 4.5, 10]),
        `repeat events got ${JSON.stringify(repeatDoc.getMediaTimeEvents())}`,
    );
    check(
        sameRanges(repeatDoc.getContinuousAnimationRanges(), [[0, 4.5]]),
        "repeat continuous range",
    );

    /* Explicit end truncates the active interval and changes freeze progress. */
    const endDoc = parse(endClipXML);
    near(check, endDoc.head.layout.regions.r.animations[0].end, 3, "explicit end truncates active end");
    near(check, opacityAt(endDoc, 1), 0.5, "end clip midpoint");
    near(check, opacityAt(endDoc, 2), 0, "end clip repeat boundary");
    near(check, opacityAt(endDoc, 2.5), 0.25, "end clip partial cycle");
    near(check, opacityAt(endDoc, 3), 0.5, "truncated freeze value");
    near(check, opacityAt(endDoc, 4), 0.5, "truncated freeze persists");
    check(
        sameNumberList(endDoc.getMediaTimeEvents(), [0, 2, 3, 10]),
        `end events got ${JSON.stringify(endDoc.getMediaTimeEvents())}`,
    );
    check(
        sameRanges(endDoc.getContinuousAnimationRanges(), [[0, 3]]),
        "end range",
    );

    /* Ancestor/region visibility clips output, but does not rephase cycles. */
    const clipDoc = parse(regionClipXML);
    near(check, fontSizeRHAt(clipDoc, 1), 0.6 * CELL_FONT, "phase at clip start is absolute", 1e-6);
    near(check, fontSizeRHAt(clipDoc, 1.5), 0.85 * CELL_FONT, "absolute phase through region clip", 1e-6);
    near(check, fontSizeRHAt(clipDoc, 2), 0.1 * CELL_FONT, "absolute cycle boundary in clip");
    near(check, fontSizeRHAt(clipDoc, 2.5), 0.35 * CELL_FONT, "post-boundary phase in clip");
    check(fontSizeRHAt(clipDoc, 3) === undefined, "region end clips ISD");
    check(
        sameNumberList(clipDoc.getMediaTimeEvents(), [0, 1, 2, 3, 6]),
        `clip events got ${JSON.stringify(clipDoc.getMediaTimeEvents())}`,
    );
    check(
        sameRanges(clipDoc.getContinuousAnimationRanges(), [[1, 3]]),
        `clip ranges got ${JSON.stringify(clipDoc.getContinuousAnimationRanges())}`,
    );

    const ancestorDoc = parse(wrap(
        '<div end="3s"><p dur="6s">' +
        '<animate dur="2s" repeatCount="3" tts:fontSize="10%;110%"/>A' +
        "</p></div>",
    ));
    near(check, fontSizeRHAt(ancestorDoc, 2.5), 0.35 * CELL_FONT, "XML ancestor end preserves phase");
    check(
        sameNumberList(ancestorDoc.getMediaTimeEvents(), [0, 2, 3, 6]),
        `ancestor events got ${JSON.stringify(ancestorDoc.getMediaTimeEvents())}`,
    );
    check(
        sameRanges(ancestorDoc.getContinuousAnimationRanges(), [[0, 3]]),
        "XML ancestor range",
    );

    /* Discrete keyTimes switch exactly on the key point and make no range. */
    const discreteXML = `
    <tt xmlns="http://www.w3.org/ns/ttml" xmlns:tts="http://www.w3.org/ns/ttml#styling">
      <head><layout><region xml:id="r" dur="10s" tts:opacity="0.2">
        <animate dur="2s" repeatCount="2" calcMode="discrete"
                 keyTimes="0;0.5;1" tts:opacity="0.1;0.5;0.9"/>
      </region></layout></head>
      <body><div region="r"><p dur="10s">A</p></div></body>
    </tt>`;
    const discreteDoc = parse(discreteXML);
    near(check, opacityAt(discreteDoc, 0), 0.1, "discrete first");
    near(check, opacityAt(discreteDoc, 1 - 1e-7), 0.1, "before discrete key point", 1e-6);
    near(check, opacityAt(discreteDoc, 1), 0.5, "at discrete key point");
    near(check, opacityAt(discreteDoc, 2), 0.1, "discrete repeat resets to first");
    near(check, opacityAt(discreteDoc, 3), 0.5, "second cycle key point");
    near(check, opacityAt(discreteDoc, 4), 0.2, "remove restores static value");
    check(
        sameNumberList(discreteDoc.getMediaTimeEvents(), [0, 1, 2, 3, 4, 10]),
        `discrete events got ${JSON.stringify(discreteDoc.getMediaTimeEvents())}`,
    );
    check(
        discreteDoc.getContinuousAnimationRanges().length === 0,
        "discrete animations do not produce continuous ranges",
    );

    const freezeFullDoc = parse(discreteXML.replace(
        'keyTimes="0;0.5;1" tts:opacity="0.1;0.5;0.9"',
        'fill="freeze" keyTimes="0;0.5;1" tts:opacity="0.1;0.5;0.9"',
    ));
    near(check, opacityAt(freezeFullDoc, 4), 0.9, "complete cycles freeze on final value");
    near(check, opacityAt(freezeFullDoc, 5), 0.9, "full-cycle freeze persists");

    /* Reverse/interleaved queries must be pure and stable. */
    const queryTimes = [5, 0, 4.5, 2, 1, 4, 3, 2 - 1e-8];
    const firstPass = queryTimes.map((time) => opacityAt(repeatDoc, time));
    const secondPass = [3, 4, 1, 5, 2, 4.5, 0].map((time) => opacityAt(repeatDoc, time));
    queryTimes.forEach((time, i) => {
        if ([3, 4, 1, 5, 2, 4.5, 0].includes(time)) {
            const j = [3, 4, 1, 5, 2, 4.5, 0].indexOf(time);
            check(secondPass[j] === firstPass[i], `interleaved query stable at ${time}`);
        }
    });

    const oldISD = generateISD(repeatDoc, 1);
    const oldOpacity = find(oldISD, "r").styleAttrs[OPACITY];
    generateISD(repeatDoc, 5);
    near(check, find(oldISD, "r").styleAttrs[OPACITY], oldOpacity, "previous ISD is not rewritten");

    const events = repeatDoc.getMediaTimeEvents();
    events.push(999);
    events[0] = -1;
    check(
        sameNumberList(repeatDoc.getMediaTimeEvents(), [0, 2, 4, 4.5, 10]),
        "getMediaTimeEvents returns an isolated array",
    );

    const ranges = repeatDoc.getContinuousAnimationRanges();
    ranges.push([99, 100]);
    ranges[0][0] = -1;
    check(
        sameRanges(repeatDoc.getContinuousAnimationRanges(), [[0, 4.5]]),
        "getContinuousAnimationRanges returns isolated arrays",
    );
});

taskCase("cascade", (check) => {
    /* Sets apply first; active animations then override in XML order. */
    const setOrderDoc = parse(`
    <tt xmlns="http://www.w3.org/ns/ttml" xmlns:tts="http://www.w3.org/ns/ttml#styling">
      <head><layout><region xml:id="r" dur="10s" tts:opacity="0.2">
        <set dur="10s" tts:opacity="0.4"/>
        <animate begin="2s" dur="2s" tts:opacity="0;1"/>
      </region></layout></head>
      <body><div region="r"><p dur="10s">A</p></div></body>
    </tt>`);
    near(check, opacityAt(setOrderDoc, 1), 0.4, "set applies before animation");
    near(check, opacityAt(setOrderDoc, 3), 0.5, "animation overrides active set");
    near(check, opacityAt(setOrderDoc, 5), 0.4, "removed animation yields to set again");

    /* A later active animation on the same property wins. */
    const multipleDoc = parse(`
    <tt xmlns="http://www.w3.org/ns/ttml" xmlns:tts="http://www.w3.org/ns/ttml#styling">
      <head><layout><region xml:id="r" dur="10s">
        <animate dur="4s" tts:opacity="0.2;0.6"/>
        <animate begin="1s" dur="2s" tts:opacity="0;1"/>
      </region></layout></head>
      <body><div region="r"><p dur="10s">A</p></div></body>
    </tt>`);
    near(check, opacityAt(multipleDoc, 0), 0.2, "first animation before second begins");
    near(check, opacityAt(multipleDoc, 1), 0, "later animation wins at its start");
    near(check, opacityAt(multipleDoc, 2), 0.5, "later animation wins while active");
    near(check, opacityAt(multipleDoc, 3), 0.5, "earlier animation after later one is removed");

    /* Animated parent percentage is computed before child percentage fonts. */
    const percentDoc = parse(wrap(
        '<div><p dur="4s">' +
        '<animate dur="2s" fill="freeze" tts:fontSize="50%;150%"/>' +
        '<span xml:id="child" tts:fontSize="50%">X</span>' +
        "</p></div>",
    ));
    near(check, fontSizeRHAt(percentDoc, 0), 0.5 * CELL_FONT, "animated p font at 0");
    near(
        check,
        find(generateISD(percentDoc, 0), "span").styleAttrs[FONT_SIZE].rh,
        0.25 * CELL_FONT,
        "child percentage uses animated parent at 0",
    );
    near(check, fontSizeRHAt(percentDoc, 1), CELL_FONT, "animated p font at midpoint");
    near(
        check,
        find(generateISD(percentDoc, 1), "span").styleAttrs[FONT_SIZE].rh,
        0.5 * CELL_FONT,
        "child percentage uses animated parent at midpoint",
    );
    near(check, fontSizeRHAt(percentDoc, 2), 1.5 * CELL_FONT, "frozen p font");
    near(
        check,
        find(generateISD(percentDoc, 3), "span").styleAttrs[FONT_SIZE].rh,
        0.75 * CELL_FONT,
        "child percentage uses frozen parent",
    );

    /* body/div are inheritance sources even though fontSize is not their output style. */
    const divSourceDoc = parse(wrap(
        '<div dur="4s"><animate dur="2s" tts:fontSize="50%;150%"/>' +
        '<p><span tts:fontSize="50%">X</span></p></div>',
    ));
    near(
        check,
        find(generateISD(divSourceDoc, 0), "span").styleAttrs[FONT_SIZE].rh,
        0.25 * CELL_FONT,
        "animated div feeds child percentage at start",
    );
    near(
        check,
        find(generateISD(divSourceDoc, 1), "span").styleAttrs[FONT_SIZE].rh,
        0.5 * CELL_FONT,
        "animated div feeds child percentage midpoint",
    );

    const bodySourceDoc = parse(`
    <tt xmlns="http://www.w3.org/ns/ttml" xmlns:tts="http://www.w3.org/ns/ttml#styling">
      <body dur="4s"><animate dur="2s" tts:fontSize="50%;150%"/>
        <div><p><span tts:fontSize="50%">X</span></p></div>
      </body>
    </tt>`);
    near(
        check,
        find(generateISD(bodySourceDoc, 1), "span").styleAttrs[FONT_SIZE].rh,
        0.5 * CELL_FONT,
        "animated body feeds descendant percentage font",
    );

    const spanHostDoc = parse(wrap(
        '<div><p dur="4s"><span><animate dur="2s" tts:fontSize="50%;150%"/>X</span></p></div>',
    ));
    near(
        check,
        find(generateISD(spanHostDoc, 1), "span").styleAttrs[FONT_SIZE].rh,
        CELL_FONT,
        "animate directly on span enters font computation",
    );

    /* Ruby textContainer gets half of the animated computed parent font. */
    const rubyDoc = parse(wrap(
        "<div><p dur=\"4s\">" +
        '<animate dur="2s" fill="freeze" tts:fontSize="100%;200%"/>' +
        '<span tts:ruby="container">' +
        '<span tts:ruby="base">B</span>' +
        '<span tts:ruby="textContainer">' +
        '<span tts:ruby="text">T</span>' +
        "</span></span></p></div>",
    ));
    const rubyISD0 = generateISD(rubyDoc, 0);
    const textContainer0 = findStyleValue(rubyISD0, RUBY, "textContainer");
    const text0 = findStyleValue(rubyISD0, RUBY, "text");
    near(check, textContainer0.styleAttrs[FONT_SIZE].rh, 0.5 * CELL_FONT, "ruby half font at start");
    near(check, text0.styleAttrs[FONT_SIZE].rh, 0.5 * CELL_FONT, "ruby text inherits half text-container font");

    const rubyISD1 = generateISD(rubyDoc, 1);
    const textContainer1 = findStyleValue(rubyISD1, RUBY, "textContainer");
    near(check, textContainer1.styleAttrs[FONT_SIZE].rh, 0.75 * CELL_FONT, "ruby half follows animation");

    /* px interpolation enters the normal computed-length chain. */
    const pxDoc = parse(`
    <tt xmlns="http://www.w3.org/ns/ttml" xmlns:tts="http://www.w3.org/ns/ttml#styling"
        tts:extent="1280px 720px">
      <body><div><p dur="4s"><animate dur="2s" tts:fontSize="20px;40px"/>X</p></div></body>
    </tt>`);
    near(check, fontSizeRHAt(pxDoc, 0), 20 / 720, "px font start");
    near(check, fontSizeRHAt(pxDoc, 1), 30 / 720, "px font interpolated");
});

taskCase("output", (check) => {
    /* Invalid and unsupported subsets fail loudly at parse time. */
    expectThrow(
        check,
        () => parse(wrap('<div><p><animate dur="1s" tts:opacity="0;1" tts:fontSize="10px;20px"/></p></div>')),
        /only one style property/,
        "two target properties",
    );
    expectThrow(
        check,
        () => parse(`<tt xmlns="http://www.w3.org/ns/ttml" xmlns:tts="http://www.w3.org/ns/ttml#styling">
          <body><div><p><animate dur="1s" tts:color="red;blue"/></p></div></body></tt>`),
        /color|not supported/,
        "unsupported animated style",
    );
    expectThrow(
        check,
        () => parse(wrap('<div><p><animate dur="1s" calcMode="paced" tts:fontSize="10px;20px"/></p></div>')),
        /paced/,
        "paced calcMode",
    );
    expectThrow(
        check,
        () => parse(wrap('<div><p><animate dur="1s" calcMode="spline" tts:fontSize="10px;20px"/></p></div>')),
        /spline/,
        "spline calcMode",
    );
    expectThrow(
        check,
        () => parse(`<tt xmlns="http://www.w3.org/ns/ttml"><head><layout><animation src="x.xml"/></layout></head><body/></tt>`),
        /Out-of-line|external/,
        "external animation",
    );
    expectThrow(
        check,
        () => parse(`<tt xmlns="http://www.w3.org/ns/ttml" xmlns:tts="http://www.w3.org/ns/ttml#styling">
          <head><layout><region xml:id="r"><animate repeatCount="0" dur="1s" tts:opacity="0;1"/></region></layout></head>
          <body/></tt>`),
        /repeatCount/,
        "zero repeatCount",
    );
    expectThrow(
        check,
        () => parse(`<tt xmlns="http://www.w3.org/ns/ttml" xmlns:tts="http://www.w3.org/ns/ttml#styling">
          <head><layout><region xml:id="r"><animate dur="0s" tts:opacity="0;1"/></region></layout></head>
          <body/></tt>`),
        /positive finite/,
        "zero dur",
    );
    expectThrow(
        check,
        () => parse(`<tt xmlns="http://www.w3.org/ns/ttml" xmlns:tts="http://www.w3.org/ns/ttml#styling">
          <head><layout><region xml:id="r"><animate dur="1s" keyTimes="0;0.5;1" tts:opacity="0;1"/></region></layout></head>
          <body/></tt>`),
        /same number/,
        "keyTimes count",
    );
    expectThrow(
        check,
        () => parse(`<tt xmlns="http://www.w3.org/ns/ttml" xmlns:tts="http://www.w3.org/ns/ttml#styling">
          <head><layout><region xml:id="r"><animate dur="1s" keyTimes="0;0.4;0.3;1" tts:opacity="0;0.4;0.3;1"/></region></layout></head>
          <body/></tt>`),
        /strictly increasing/,
        "keyTimes order",
    );
    expectThrow(
        check,
        () => parse(`<tt xmlns="http://www.w3.org/ns/ttml" xmlns:tts="http://www.w3.org/ns/ttml#styling">
          <head><layout><region xml:id="r"><animate dur="1s" tts:opacity="-0.1;1"/></region></layout></head>
          <body/></tt>`),
        /\[0,1\]/,
        "opacity range",
    );
    expectThrow(
        check,
        () => parse(wrap('<div><p><animate dur="1s" tts:fontSize="10px;20%"/></p></div>')),
        /same .*unit/,
        "mixed font units",
    );
    expectThrow(
        check,
        () => parse(wrap('<div><p><animate dur="1s" tts:fontSize="0px;20px"/></p></div>')),
        /positive/,
        "non-positive font length",
    );

    /* The example CLI writes events, ranges and JSON/ISD-compatible frames. */
    const outputPath = path.join(
        process.env.TMPDIR || os.tmpdir(),
        "animation-frames-test-output.json",
    );

    try {
        fs.rmSync(outputPath, { force: true });
    } catch {
        /* leftover temporary file is harmless */
    }

    const run = spawnSync(
        process.execPath,
        [
            path.join("examples", "animate_frames.mjs"),
            "task_support/example.ttml",
            "task_support/times.json",
            outputPath,
        ],
        { cwd: repoRoot, encoding: "utf8" },
    );

    check(run.status === 0, `example exited ${run.status}: ${run.stderr || run.stdout}`);
    check(fs.existsSync(outputPath), "example created output file");

    const output = JSON.parse(fs.readFileSync(outputPath, "utf8"));
    check(sameNumberList(output.events, [0, 2, 4, 4.5, 6]), `example events ${JSON.stringify(output.events)}`);
    check(sameRanges(output.ranges, [[0, 4.5]]), "example ranges");
    check(Array.isArray(output.frames) && output.frames.length === 2, "example has two frames");
    check(output.frames[0].time === 1 && output.frames[1].time === 5, "example frame times");
    near(
        check,
        find(output.frames[0].isd, "r").styleAttrs[OPACITY],
        0.5,
        "example frame 1 opacity",
    );
    near(
        check,
        find(output.frames[1].isd, "r").styleAttrs[OPACITY],
        0.25,
        "example frame 5 opacity",
    );

    const exampleDoc = parse(fs.readFileSync(path.join(repoRoot, "task_support", "example.ttml"), "utf8"));
    let jsonMatchesLibrary = false;
    let mismatchMessage = "";

    try {
        assert.deepEqual(
            output.frames,
            [1, 5].map((time) => ({ time, isd: generateISD(exampleDoc, time) })),
        );
        jsonMatchesLibrary = true;
    } catch (error) {
        mismatchMessage = error.message;
    }

    check(jsonMatchesLibrary, "example JSON frames match direct library calls: " + mismatchMessage);

    const badRun = spawnSync(
        process.execPath,
        [
            path.join("examples", "animate_frames.mjs"),
            "task_support/does-not-exist.ttml",
            "task_support/times.json",
            path.join(process.env.TMPDIR || os.tmpdir(), "animation-bad-output.json"),
        ],
        { cwd: repoRoot, encoding: "utf8" },
    );
    check(badRun.status !== 0, "example exits non-zero on missing input");

    /* The module is imported by the regression as well as force-loaded by target. */
    check(typeof animation.collectContinuousAnimationRanges === "function", "animation module exports range collector");
});
