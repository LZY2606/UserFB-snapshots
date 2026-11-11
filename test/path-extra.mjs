import assert from 'node:assert/strict';
import {JSDOM} from '../.deps/node_modules/jsdom/lib/api.js';
import cssEscape from '../.deps/node_modules/css.escape/css.escape.js';
import * as api from '../finder.ts';

const {window} = new JSDOM(
  '<body></body>',
);
Object.assign(globalThis, {
  document: window.document,
  Node: window.Node,
  CSS: {escape: cssEscape},
});

const el = (html) => {
  const wrap = document.createElement('div');
  wrap.innerHTML = html;
  return wrap.firstElementChild;
};
const disableNames = {
  idName: () => false,
  className: () => false,
  attr: () => false,
};

// --- Positional fallback when id, class and attr filters are all disabled ---
{
  const section = el(
    '<section id="wrap" data-test="alpha"><div class="box" role="note">one</div><div class="box" role="note">two</div></section>',
  );
  document.body.append(section);
  const target = section.querySelectorAll('div')[1];
  const path = api.finderPath(target, {root: document, ...disableNames});
  assert.equal(path.length, 1);
  assert.match(path[0], /nth/);
  assert.equal(api.resolvePath(document, path), target);

  // The same target still resolves when the scoping root is the section.
  const scoped = api.finderPath(target, {root: section, ...disableNames});
  assert.equal(api.resolvePath(section, scoped), target);
  assert.match(scoped[0], /nth/);
}

// --- A shadow host without any usable name is located by position ---
{
  const list = el('<div></div>');
  document.body.append(list);
  const hosts = [el('<x-card></x-card>'), el('<x-card></x-card>')];
  list.append(...hosts);
  const prices = hosts.map((host) => {
    host.attachShadow({mode: 'open'}).innerHTML =
      '<span class="price">0</span>';
    return host.shadowRoot.firstElementChild;
  });

  const path0 = api.finderPath(prices[0], {root: document, ...disableNames});
  const path1 = api.finderPath(prices[1], {root: document, ...disableNames});
  // The host segment must be positional since no name is allowed.
  assert.match(path0[0], /nth/);
  assert.match(path1[0], /nth/);
  assert.notDeepEqual(path0, path1);
  assert.equal(api.resolvePath(document, path0), prices[0]);
  assert.equal(api.resolvePath(document, path1), prices[1]);
}

// --- Two identical components: same inner selector, distinct replay ---
{
  const deck = el('<div></div>');
  document.body.append(deck);
  const cards = [el('<x-card></x-card>'), el('<x-card></x-card>')];
  deck.append(...cards);
  for (const card of cards) {
    card.attachShadow({mode: 'open'}).innerHTML =
      '<section><span class="price"></span></section>';
  }
  const priceA = cards[0].shadowRoot.querySelector('.price');
  const priceB = cards[1].shadowRoot.querySelector('.price');

  const pathA = api.finderPath(priceA, {root: document});
  const pathB = api.finderPath(priceB, {root: document});
  assert.notDeepEqual(pathA, pathB);
  assert.equal(api.resolvePath(document, pathA), priceA);
  assert.equal(api.resolvePath(document, pathB), priceB);

  // Rooting inside the second card (its shadow root) cannot borrow the first
  // card's selector to pass the uniqueness check.
  const secondRoot = cards[1].shadowRoot;
  assert.equal(
    api.resolvePath(secondRoot, api.finderPath(priceB, {root: secondRoot})),
    priceB,
  );
  // The first card's price is not reachable from the second card at all.
  assert.throws(() => api.finderPath(priceA, {root: cards[1]}));
  assert.throws(() => api.finderPath(priceA, {root: secondRoot}));
}

// --- Uniqueness is scoped to the segment root, not the whole document ---
{
  const card1 = el('<section><div class="panel"></div></section>');
  const card2 = el('<section><div class="panel"></div></section>');
  document.body.append(card1, card2);
  const panel2 = card2.querySelector('.panel');
  assert.equal(document.querySelectorAll('.panel').length, 2);
  assert.equal(card2.querySelectorAll('.panel').length, 1);

  const path = api.finderPath(panel2, {root: card2});
  assert.deepEqual(path, ['.panel']);
  assert.equal(api.resolvePath(card2, path), panel2);
  // The same segment is ambiguous at document level, proving the scope matters.
  assert.equal(api.resolvePath(document, ['.panel']), null);
}

// --- resolvePath never returns a first match to paper over ambiguity ---
{
  document.body.append(el('<div><i class="dup"></i><i class="dup"></i></div>'));
  assert.equal(api.resolvePath(document, ['.dup']), null); // multiple
  assert.equal(api.resolvePath(document, ['.does-not-exist']), null); // zero
  assert.equal(api.resolvePath(document, ['[']), null); // invalid CSS
  // Intermediate node without an open shadow root.
  assert.equal(api.resolvePath(document, ['body', 'div']), null);
}

// --- An element root cannot reach inside its own shadow root ---
{
  const host = el('<x-host></x-host>');
  document.body.append(host);
  host.attachShadow({mode: 'open'}).innerHTML = '<b>inside</b>';
  const inside = host.shadowRoot.firstElementChild;
  assert.throws(() => api.finderPath(inside, {root: host}));
  // Passing the shadow root itself works and yields an empty-ish scoped path.
  const fromShadow = api.finderPath(inside, {root: host.shadowRoot});
  assert.equal(api.resolvePath(host.shadowRoot, fromShadow), inside);
}

// --- Closed shadow roots block path building, also when nested ---
{
  const outer = el('<x-outer></x-outer>');
  document.body.append(outer);
  const outerRoot = outer.attachShadow({mode: 'open'});
  outerRoot.innerHTML = '<x-inner></x-inner>';
  const innerHost = outerRoot.firstElementChild;
  const closedRoot = innerHost.attachShadow({mode: 'closed'});
  closedRoot.innerHTML = '<em>hidden</em>';
  assert.throws(() =>
    api.finderPath(closedRoot.firstElementChild, {root: document}),
  );
}

// --- Slotted light DOM stays in the light tree ---
{
  const host = el('<x-slot></x-slot>');
  document.body.append(host);
  const slotted = el('<strong class="slotted">light</strong>');
  host.append(slotted);
  host.attachShadow({mode: 'open'}).innerHTML = '<slot></slot>';
  assert.ok(slotted.assignedSlot);

  const path = api.finderPath(slotted, {root: document});
  // No segment enters the host: the slotted node is reached purely in light DOM.
  assert.equal(api.resolvePath(document, path), slotted);
  for (const segment of path) {
    assert.equal(host.shadowRoot.querySelectorAll(segment).length, 0);
  }

  // A node living in the shadow root still takes the boundary-crossing path.
  const slot = host.shadowRoot.querySelector('slot');
  const slotPath = api.finderPath(slot, {root: document});
  assert.equal(api.resolvePath(document, slotPath), slot);
  assert.ok(slotPath.length >= 2);
}

// --- An element root reaches shadow content nested underneath it ---
{
  const main = el('<main><x-widget></x-widget></main>');
  document.body.append(main);
  const widget = main.firstElementChild;
  widget.attachShadow({mode: 'open'}).innerHTML =
    '<div class="inner"><b>value</b></div>';
  const value = widget.shadowRoot.querySelector('b');

  const path = api.finderPath(value, {root: main});
  assert.ok(path.length >= 2);
  assert.equal(api.resolvePath(main, path), value);
}

// --- Empty path semantics ---
{
  const box = el('<div id="self"></div>');
  document.body.append(box);
  assert.deepEqual(api.finderPath(box, {root: box}), []);
  assert.equal(api.resolvePath(box, []), box);
  // An empty path always returns the original start, whatever its type.
  assert.equal(api.resolvePath(document, []), document);
  const host = el('<x-empty></x-empty>');
  document.body.append(host);
  const shadowRoot = host.attachShadow({mode: 'open'});
  assert.equal(api.resolvePath(shadowRoot, []), shadowRoot);
}

// --- Detached or unrelated targets ---
{
  const mounted = el('<div></div>');
  document.body.append(mounted);
  assert.throws(() => api.finderPath(document.createElement('span')));
  const otherTree = el('<section><p></p></section>');
  assert.throws(() =>
    api.finderPath(otherTree.querySelector('p'), {root: mounted}),
  );
  assert.throws(() => api.finderPath(document, {root: document}));
}

// --- Regression: the original finder() API keeps its old behaviour ---
{
  const article = el(
    '<article><h2 class="title">a</h2><h2 class="title">b</h2></article>',
  );
  document.body.append(article);
  const heading = article.querySelectorAll('h2')[1];
  const css = api.finder(heading, {root: document});
  assert.equal(document.querySelectorAll(css).length, 1);
  assert.equal(document.querySelector(css), heading);

  const host = el('<x-reg></x-reg>');
  document.body.append(host);
  const sr = host.attachShadow({mode: 'open'});
  sr.innerHTML = '<button id="go">Go</button>';
  const button = sr.querySelector('#go');
  const shadowCss = api.finder(button, {root: sr});
  assert.equal(sr.querySelectorAll(shadowCss).length, 1);
  assert.equal(sr.querySelector(shadowCss), button);
}

console.log(
  'shadow path: fallback, scoping, ambiguity, replay and regression cases passed',
);
