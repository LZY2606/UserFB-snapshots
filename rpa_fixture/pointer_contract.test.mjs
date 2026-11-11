/**
 * Contract tests for `pointerReachable()`.
 *
 * jsdom performs no layout and has no `elementFromPoint`, so every geometry and
 * every hit-test relation below is given explicitly. Each test states the
 * client rects of the involved nodes and the exact node returned at each layer
 * (document, then each open shadow root) for each sampled point. Nothing here
 * is evidence about real browser painting; it is a deterministic check of the
 * sampling, clipping, shadow descent, and composed-ownership rules.
 *
 * The suite runs sequentially: the module under test captures realm globals
 * (e.g. `Element`) at evaluation time, and each test installs its own JSDOM
 * realm on the global object, so concurrent realms would corrupt one another.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { JSDOM } from 'jsdom';

const VIEWPORT_WIDTH = 100;
const VIEWPORT_HEIGHT = 100;

const moduleSource = await readFile(
  new URL('../src/index.js', import.meta.url),
  'utf8'
);
let moduleSeq = 0;

// Each JSDOM realm gets its own fresh module instance (a distinct data-URL
//  fragment forces a separate evaluation) so the module's captured `Element`
//  global always belongs to the realm under test.
async function loadModule() {
  const url = `data:text/javascript;base64,${Buffer.from(moduleSource).toString(
    'base64'
  )}#m${moduleSeq++}`;
  return import(url);
}

async function setup(html) {
  const dom = new JSDOM(html);
  const { window } = dom;
  const { document } = window;
  Object.assign(globalThis, {
    window,
    document,
    Element: window.Element,
    Node: window.Node,
    getComputedStyle: window.getComputedStyle,
  });
  // jsdom reports a 0x0 viewport; the viewport is an explicit fixture value.
  Object.defineProperty(document.documentElement, 'clientWidth', {
    value: VIEWPORT_WIDTH,
    configurable: true,
  });
  Object.defineProperty(document.documentElement, 'clientHeight', {
    value: VIEWPORT_HEIGHT,
    configurable: true,
  });
  const api = await loadModule();
  return { dom, window, document, api };
}

// Stubs both rect APIs on an element. `getClientRects()` keeps one entry per
//  line box, in order; `getBoundingClientRect()` is their union (used by the
//  pre-existing focusable visibility checks, never by pointerReachable itself).
function setRects(el, rects) {
  const boxes = rects.map((r) => ({
    left: r.left,
    top: r.top,
    right: r.right,
    bottom: r.bottom,
    width: r.right - r.left,
    height: r.bottom - r.top,
    x: r.left,
    y: r.top,
  }));
  el.getClientRects = () => boxes.map((b) => ({ ...b }));
  const union = boxes.reduce(
    (u, b) => ({
      left: Math.min(u.left, b.left),
      top: Math.min(u.top, b.top),
      right: Math.max(u.right, b.right),
      bottom: Math.max(u.bottom, b.bottom),
    }),
    { left: Infinity, top: Infinity, right: -Infinity, bottom: -Infinity }
  );
  el.getBoundingClientRect = () => ({
    ...union,
    width: union.right - union.left,
    height: union.bottom - union.top,
    x: union.left,
    y: union.top,
  });
}

// A deterministic per-layer hit table: each target (the document or a shadow
//  root) answers elementFromPoint(x, y) with an explicitly configured node, and
//  every query (target + coordinates) is recorded for order assertions.
function createHitRouter() {
  const calls = [];
  const tables = new Map();
  const key = (x, y) => `${x}|${y}`;
  const tableFor = (target) => {
    let table = tables.get(target);
    if (!table) {
      table = new Map();
      tables.set(target, table);
    }
    return table;
  };
  return {
    calls,
    set(target, x, y, node) {
      tableFor(target).set(key(x, y), node);
    },
    install(target) {
      target.elementFromPoint = (x, y) => {
        calls.push({ target, x, y });
        return tableFor(target).get(key(x, y)) ?? null;
      };
    },
  };
}

const ids = (entries) => entries.map((entry) => entry.node.id);

test.describe('pointerReachable contract', { concurrency: false }, () => {
  test('rect clipped by the viewport: center of the positive-area intersection', async () => {
    // Spec example: viewport width 100; rect left=-20, right=20, top=10,
    //  bottom=30. Intersection is [0,20] x [10,30], center (10,20).
    const { document, api } = await setup('<a id="open" href="#a">one</a>');
    const open = document.getElementById('open');
    setRects(open, [{ left: -20, top: 10, right: 20, bottom: 30 }]);
    const router = createHitRouter();
    router.install(document);
    router.set(document, 10, 20, open);

    const result = api.pointerReachable(document.body);
    assert.deepEqual(result, [{ node: open, point: { x: 10, y: 20 } }]);
  });

  test('baseline scenario: reachable link reported, overlay-covered link omitted', async () => {
    // Mirrors rpa_fixture/baseline.mjs with explicit per-node geometry.
    const { document, api } = await setup(
      '<a id="open" href="#a">one</a>' +
        '<a id="covered" href="#b">two</a>' +
        '<div id="overlay"></div>'
    );
    const open = document.getElementById('open');
    const covered = document.getElementById('covered');
    const overlay = document.getElementById('overlay');
    // open: (20,20)-(120,50) clips to [20,100] x [20,50] -> center (60,35)
    setRects(open, [{ left: 20, top: 20, right: 120, bottom: 50 }]);
    // covered: fully inside the viewport -> center (50,75)
    setRects(covered, [{ left: 10, top: 60, right: 90, bottom: 90 }]);
    setRects(overlay, [{ left: 0, top: 0, right: 100, bottom: 100 }]);
    const router = createHitRouter();
    router.install(document);
    router.set(document, 60, 35, open);
    router.set(document, 50, 75, overlay);

    // focusable still lists both (keyboard focus is unaffected by coverage)
    assert.deepEqual(
      api.focusable(document.body).map((n) => n.id),
      ['open', 'covered']
    );
    const result = api.pointerReachable(document.body);
    assert.deepEqual(ids(result), ['open']);
    assert.deepEqual(result[0].point, { x: 60, y: 35 });
  });

  test('all sampled centers blocked: node is not reported', async () => {
    const { document, api } = await setup(
      '<a id="twoline" href="#a">line1<br />line2</a><div id="overlay"></div>'
    );
    const link = document.getElementById('twoline');
    const overlay = document.getElementById('overlay');
    // Two line boxes, centers (50,10) and (50,40); the overlay wins both hits.
    setRects(link, [
      { left: 0, top: 0, right: 100, bottom: 20 },
      { left: 0, top: 30, right: 100, bottom: 50 },
    ]);
    const router = createHitRouter();
    router.install(document);
    router.set(document, 50, 10, overlay);
    router.set(document, 50, 40, overlay);

    assert.deepEqual(api.pointerReachable(document.body), []);
    // both finite samples were tried; nothing else was searched
    assert.deepEqual(
      router.calls.map((c) => [c.x, c.y]),
      [
        [50, 10],
        [50, 40],
      ]
    );
  });

  test('first line covered, second line reachable: second center wins', async () => {
    const { document, api } = await setup(
      '<a id="twoline" href="#a">line1<br />line2</a><div id="overlay"></div>'
    );
    const link = document.getElementById('twoline');
    const overlay = document.getElementById('overlay');
    setRects(link, [
      { left: 0, top: 0, right: 100, bottom: 20 }, // center (50,10)
      { left: 0, top: 30, right: 100, bottom: 50 }, // center (50,40)
    ]);
    const router = createHitRouter();
    router.install(document);
    router.set(document, 50, 10, overlay); // line 1 obscured
    router.set(document, 50, 40, link); // line 2 reachable

    const result = api.pointerReachable(document.body);
    assert.deepEqual(result, [{ node: link, point: { x: 50, y: 40 } }]);
    // samples are tried in getClientRects() order, and the big bounding-box
    //  center (50,25) is never substituted
    assert.deepEqual(
      router.calls.map((c) => [c.x, c.y]),
      [
        [50, 10],
        [50, 40],
      ]
    );
    assert.equal(
      router.calls.some((c) => c.y === 25),
      false
    );
  });

  test('hit on a descendant span counts as reaching the candidate', async () => {
    const { document, api } = await setup(
      '<a id="link" href="#a"><span id="label">text</span></a>'
    );
    const link = document.getElementById('link');
    const span = document.getElementById('label');
    setRects(link, [{ left: 10, top: 10, right: 50, bottom: 50 }]); // center (30,30)
    const router = createHitRouter();
    router.install(document);
    router.set(document, 30, 30, span);

    const result = api.pointerReachable(document.body);
    assert.deepEqual(result, [{ node: link, point: { x: 30, y: 30 } }]);
  });

  test('nested open shadow roots: descent through every layer is required', async () => {
    const { document, api } = await setup('<div id="host1"></div>');
    const host1 = document.getElementById('host1');
    const sr1 = host1.attachShadow({ mode: 'open' });
    const host2 = document.createElement('div');
    host2.id = 'host2';
    sr1.appendChild(host2);
    const sr2 = host2.attachShadow({ mode: 'open' });
    const anchor = document.createElement('a');
    anchor.id = 'deep';
    anchor.href = '#a';
    sr2.appendChild(anchor);
    // anchor box (10,10)-(40,40) -> center (25,25)
    setRects(anchor, [{ left: 10, top: 10, right: 40, bottom: 40 }]);
    setRects(host2, [{ left: 10, top: 10, right: 40, bottom: 40 }]);
    setRects(host1, [{ left: 10, top: 10, right: 40, bottom: 40 }]);

    const router = createHitRouter();
    router.install(document);
    router.install(sr1);
    router.install(sr2);
    // explicit per-layer hit chain at (25,25)
    router.set(document, 25, 25, host1);
    router.set(sr1, 25, 25, host2);
    router.set(sr2, 25, 25, anchor);

    const result = api.pointerReachable(document.body, {
      getShadowRoot: true,
    });
    assert.deepEqual(result, [{ node: anchor, point: { x: 25, y: 25 } }]);
    assert.deepEqual(
      router.calls.map((c) => c.target),
      [document, sr1, sr2]
    );
  });

  test('document-level hit on a shadow host proves nothing about its interior', async () => {
    const { document, api } = await setup('<div id="host1"></div>');
    const host1 = document.getElementById('host1');
    const sr1 = host1.attachShadow({ mode: 'open' });
    const host2 = document.createElement('div');
    host2.id = 'host2';
    sr1.appendChild(host2);
    const sr2 = host2.attachShadow({ mode: 'open' });
    const anchor = document.createElement('a');
    anchor.id = 'deep';
    anchor.href = '#a';
    sr2.appendChild(anchor);
    setRects(anchor, [{ left: 10, top: 10, right: 40, bottom: 40 }]);
    setRects(host2, [{ left: 10, top: 10, right: 40, bottom: 40 }]);
    setRects(host1, [{ left: 10, top: 10, right: 40, bottom: 40 }]);

    // Variant A: the first shadow root has no hit at that point (null).
    const routerA = createHitRouter();
    routerA.install(document);
    routerA.install(sr1);
    routerA.set(document, 25, 25, host1);
    assert.deepEqual(
      api.pointerReachable(document.body, { getShadowRoot: true }),
      []
    );

    // Variant B: the inner layer hits an unrelated node instead of the anchor.
    const foreign = document.createElement('button');
    foreign.id = 'foreign';
    sr1.appendChild(foreign);
    const routerB = createHitRouter();
    routerB.install(document);
    routerB.install(sr1);
    routerB.set(document, 25, 25, host1);
    routerB.set(sr1, 25, 25, foreign);
    assert.deepEqual(
      api.pointerReachable(document.body, { getShadowRoot: true }),
      []
    );
  });

  test('slotted light node is verified through the open shadow root', async () => {
    const { document, api } = await setup(
      '<div id="host"><a id="slotted" href="#a">go</a></div>'
    );
    const host = document.getElementById('host');
    const sr = host.attachShadow({ mode: 'open' });
    sr.innerHTML = '<slot></slot>';
    const slotted = document.getElementById('slotted');
    setRects(slotted, [{ left: 5, top: 5, right: 35, bottom: 35 }]); // (20,20)
    setRects(host, [{ left: 0, top: 0, right: 100, bottom: 100 }]);
    const router = createHitRouter();
    router.install(document);
    router.install(sr);
    router.set(document, 20, 20, host);
    // the shadow root's hit resolves to the slotted light node
    router.set(sr, 20, 20, slotted);

    const result = api.pointerReachable(document.body, {
      getShadowRoot: true,
    });
    assert.deepEqual(result, [{ node: slotted, point: { x: 20, y: 20 } }]);
  });

  test('composed ownership crosses assignedSlot and the shadow host', async () => {
    // The host itself is a focusable candidate (tabindex=0); the verified hit is
    //  light content slotted into its shadow tree. Ownership must walk from the
    //  slotted node to its slot and then across the shadow host.
    const { document, api } = await setup(
      '<div id="host" tabindex="0"><a id="slotted" href="#a">go</a></div>'
    );
    const host = document.getElementById('host');
    const sr = host.attachShadow({ mode: 'open' });
    sr.innerHTML = '<slot></slot>';
    const slotted = document.getElementById('slotted');
    setRects(host, [{ left: 0, top: 0, right: 20, bottom: 20 }]); // center (10,10)
    setRects(slotted, [{ left: 30, top: 30, right: 70, bottom: 70 }]); // (50,50)
    const router = createHitRouter();
    router.install(document);
    router.install(sr);
    router.set(document, 10, 10, host);
    router.set(sr, 10, 10, slotted);
    router.set(document, 50, 50, host);
    router.set(sr, 50, 50, slotted);

    const result = api.pointerReachable(document.body, {
      getShadowRoot: true,
    });
    assert.deepEqual(ids(result).sort(), ['host', 'slotted']);
    const hostEntry = result.find((e) => e.node === host);
    assert.deepEqual(hostEntry.point, { x: 10, y: 10 });
  });

  test('undisclosed root (getShadowRoot -> true) stays a black box', async () => {
    const { document, api } = await setup(
      '<div id="blackbox" tabindex="0"><a id="inner" href="#a">x</a></div>'
    );
    const host = document.getElementById('blackbox');
    const inner = document.getElementById('inner');
    setRects(host, [{ left: 0, top: 0, right: 40, bottom: 40 }]); // (20,20)
    setRects(inner, [{ left: 60, top: 60, right: 90, bottom: 90 }]); // (75,75)
    const getShadowRoot = (node) => (node === host ? true : undefined);
    // the light anchor remains a focusable candidate (existing fallback rule)...
    assert.ok(
      api.focusable(document.body, { getShadowRoot }).some((n) => n === inner)
    );
    const router = createHitRouter();
    router.install(document);
    // ...but every pointer hit stops at the undisclosed host
    router.set(document, 20, 20, host);
    router.set(document, 75, 75, host);

    const result = api.pointerReachable(document.body, { getShadowRoot });
    // no interior candidates are fabricated; only the host verifies at its box
    assert.deepEqual(result, [{ node: host, point: { x: 20, y: 20 } }]);
  });

  test('closed shadow root is not opened and yields no interior candidates', async () => {
    const { document, api } = await setup('<div id="closedHost"></div>');
    const host = document.getElementById('closedHost');
    const closed = host.attachShadow({ mode: 'closed' });
    const secret = document.createElement('a');
    secret.id = 'secret';
    secret.href = '#a';
    closed.appendChild(secret);
    assert.equal(host.shadowRoot, null); // jsdom hides the closed root
    const getShadowRoot = (node) => (node === host ? true : undefined);

    const result = api.pointerReachable(document.body, { getShadowRoot });
    assert.deepEqual(result, []);
    assert.equal(
      result.some((e) => e.node === secret),
      false
    );
  });

  test('detached nodes never produce coordinates, even with displayCheck "none"', async () => {
    const { document, api } = await setup('<body></body>');
    const detachedContainer = document.createElement('div');
    const detachedLink = document.createElement('a');
    detachedLink.id = 'detached';
    detachedLink.href = '#a';
    detachedContainer.appendChild(detachedLink);
    document.body.appendChild(detachedContainer); // attached first so rects stub sticks
    setRects(detachedLink, [{ left: 10, top: 10, right: 40, bottom: 40 }]);
    document.body.removeChild(detachedContainer); // now detached
    const router = createHitRouter();
    router.install(document);
    router.set(document, 25, 25, detachedLink);

    // permissive displayCheck keeps the detached node focusable...
    assert.ok(
      api
        .focusable(detachedContainer, { displayCheck: 'none' })
        .some((n) => n === detachedLink)
    );
    // ...but the pointer API cannot give it coordinates and never hit-tests it
    assert.deepEqual(
      api.pointerReachable(detachedContainer, { displayCheck: 'none' }),
      []
    );
    assert.equal(router.calls.length, 0);
  });

  test('nodes without usable layout rects produce no point', async () => {
    const { document, api } = await setup(
      '<a id="empty" href="#a">a</a>' +
        '<a id="outside" href="#b">b</a>' +
        '<a id="edge" href="#c">c</a>'
    );
    const empty = document.getElementById('empty');
    const outside = document.getElementById('outside');
    const edge = document.getElementById('edge');
    // empty rect list: only survivable as a candidate via displayCheck 'none'
    empty.getClientRects = () => [];
    empty.getBoundingClientRect = () => ({
      left: 0,
      top: 0,
      right: 0,
      bottom: 0,
      width: 0,
      height: 0,
    });
    // fully outside the 100x100 viewport: empty intersection
    setRects(outside, [{ left: 150, top: 150, right: 190, bottom: 190 }]);
    // half-open viewport: a rect starting exactly at x=100 has zero intersection
    setRects(edge, [{ left: 100, top: 10, right: 140, bottom: 40 }]);
    const router = createHitRouter();
    router.install(document);

    assert.deepEqual(
      api.pointerReachable(document.body, { displayCheck: 'none' }),
      []
    );
    assert.equal(router.calls.length, 0);
  });

  test('zero-area and non-finite rects are skipped, later valid rects are used', async () => {
    const { document, api } = await setup('<a id="link" href="#a">x</a>');
    const link = document.getElementById('link');
    setRects(link, [
      { left: 10, top: 10, right: 10, bottom: 10 }, // zero area
      { left: NaN, top: 0, right: 40, bottom: 40 }, // non-finite center
      { left: 10, top: 60, right: 50, bottom: 90 }, // valid: center (30,75)
    ]);
    const router = createHitRouter();
    router.install(document);
    router.set(document, 30, 75, link);

    const result = api.pointerReachable(document.body);
    assert.deepEqual(result, [{ node: link, point: { x: 30, y: 75 } }]);
    assert.deepEqual(router.calls, [{ target: document, x: 30, y: 75 }]);
  });

  test('pointer-events:none overlay: the browser hit result is trusted', async () => {
    // The overlay geometrically covers the link, but the fixture's hit table
    //  returns the link (as a browser would for a pointer-events:none overlay).
    //  No z-index/DOM scanning is done by the API.
    const { document, api } = await setup(
      '<a id="link" href="#a">x</a><div id="overlay"></div>'
    );
    const link = document.getElementById('link');
    const overlay = document.getElementById('overlay');
    setRects(link, [{ left: 10, top: 10, right: 50, bottom: 50 }]); // (30,30)
    setRects(overlay, [{ left: 0, top: 0, right: 100, bottom: 100 }]);
    const router = createHitRouter();
    router.install(document);
    router.set(document, 30, 30, link); // browser skipped the overlay

    const result = api.pointerReachable(document.body);
    assert.deepEqual(result, [{ node: link, point: { x: 30, y: 30 } }]);
  });

  test('result order follows focusable and every node appears once', async () => {
    const { document, api } = await setup(
      '<a id="a" href="#1">a</a>' +
        '<a id="b" href="#2">b</a>' +
        '<a id="d" href="#3">d</a>' +
        '<a id="c" href="#4">c</a>' +
        '<div id="overlay"></div>'
    );
    const byId = Object.fromEntries(
      ['a', 'b', 'c', 'd'].map((id) => [id, document.getElementById(id)])
    );
    const overlay = document.getElementById('overlay');
    // distinct 20x20 boxes stacked vertically; centers at y=10,30,50,70
    setRects(byId.a, [{ left: 0, top: 0, right: 20, bottom: 20 }]);
    setRects(byId.b, [{ left: 0, top: 20, right: 20, bottom: 40 }]);
    setRects(byId.d, [{ left: 0, top: 40, right: 20, bottom: 60 }]);
    setRects(byId.c, [{ left: 0, top: 60, right: 20, bottom: 80 }]);
    const router = createHitRouter();
    router.install(document);
    router.set(document, 10, 10, byId.a);
    router.set(document, 10, 30, byId.b);
    router.set(document, 10, 50, overlay); // d is covered
    router.set(document, 10, 70, byId.c);

    const result = api.pointerReachable(document.body);
    assert.deepEqual(ids(result), ['a', 'b', 'c']);
    assert.equal(new Set(result.map((r) => r.node)).size, result.length);
    // the reported order is the focusable order with the covered node removed
    const focusableIds = api.focusable(document.body).map((n) => n.id);
    assert.deepEqual(focusableIds, ['a', 'b', 'd', 'c']);
  });

  test('includeContainer is honored, with pointer validation applied', async () => {
    const { document, api } = await setup(
      '<div id="box" tabindex="0"><a id="link" href="#a">x</a></div>'
    );
    const box = document.getElementById('box');
    const link = document.getElementById('link');
    setRects(box, [{ left: 0, top: 0, right: 100, bottom: 100 }]); // (50,50)
    setRects(link, [{ left: 10, top: 10, right: 30, bottom: 30 }]); // (20,20)
    const router = createHitRouter();
    router.install(document);
    router.set(document, 50, 50, box);
    router.set(document, 20, 20, link);

    const without = api.pointerReachable(box);
    assert.deepEqual(ids(without), ['link']);
    const withContainer = api.pointerReachable(box, {
      includeContainer: true,
    });
    assert.deepEqual(ids(withContainer), ['box', 'link']);
    assert.deepEqual(withContainer[0].point, { x: 50, y: 50 });
  });

  test('iframes are not entered', async () => {
    const { document, api } = await setup('<iframe id="frame"></iframe>');
    const frame = document.getElementById('frame');
    const innerDoc = frame.contentDocument;
    const innerLink = innerDoc.createElement('a');
    innerLink.href = '#a';
    innerDoc.body.appendChild(innerLink);
    setRects(innerLink, [{ left: 10, top: 10, right: 40, bottom: 40 }]);

    const result = api.pointerReachable(document.body);
    assert.equal(
      result.some((e) => e.node === innerLink),
      false
    );
  });

  test('repeated calls do not focus, click, scroll, or mutate the DOM', async () => {
    const { window, document, api } = await setup(
      '<a id="open" href="#a">one</a><a id="covered" href="#b">two</a>' +
        '<div id="overlay"></div>'
    );
    const open = document.getElementById('open');
    const covered = document.getElementById('covered');
    const overlay = document.getElementById('overlay');
    setRects(open, [{ left: 20, top: 20, right: 80, bottom: 50 }]); // (50,35)
    setRects(covered, [{ left: 10, top: 60, right: 90, bottom: 90 }]); // (50,75)
    const router = createHitRouter();
    router.install(document);
    router.set(document, 50, 35, open);
    router.set(document, 50, 75, overlay);

    let focusCount = 0;
    let clickCount = 0;
    let scrollCount = 0;
    window.HTMLElement.prototype.focus = function spyFocus() {
      focusCount++;
    };
    window.HTMLElement.prototype.click = function spyClick() {
      clickCount++;
    };
    window.scrollTo = function spyScrollTo() {
      scrollCount++;
    };
    window.Element.prototype.scrollIntoView = function spyScrollIntoView() {
      scrollCount++;
    };

    const domBefore = document.body.innerHTML;
    assert.equal(document.activeElement, document.body);

    const first = api.pointerReachable(document.body);
    const hitsAfterFirst = router.calls.length;
    const second = api.pointerReachable(document.body);

    assert.equal(focusCount, 0);
    assert.equal(clickCount, 0);
    assert.equal(scrollCount, 0);
    assert.equal(document.activeElement, document.body);
    assert.equal(document.body.innerHTML, domBefore);
    // deterministic re-run: same finite sampling, same verified entries
    assert.equal(router.calls.length, hitsAfterFirst * 2);
    assert.deepEqual(second, first);
    assert.deepEqual(ids(first), ['open']);
  });
});
