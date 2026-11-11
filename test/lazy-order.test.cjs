'use strict';

const { test } = require('node:test');
const assert = require('node:assert/strict');
const unfluff = require('..');

// All pages below are synthetic and self-contained: no fixture files and
// no network access are involved.
const sentence =
  'This is the collected article and it has a detailed account of the work with the people in the study. ';

const leadSentence =
  'This guide explains the background of the study and what readers should expect from the reporting.';

const articleHtml = `<html lang="en">
  <head>
    <title>Collected study</title>
    <meta name="description" content="A study report">
    <meta name="keywords" content="study, report">
    <link rel="canonical" href="https://example.com/a">
  </head>
  <body>
    <span class="byline">Reporter A</span>
    <article>
      <p>${sentence.repeat(5)} <a href="/source">the primary source</a> and more details in the report.</p>
      <p>${sentence.repeat(5)}</p>
    </article>
  </body>
</html>`;

const mediaHtml = `<html lang="en">
  <head><title>Video study</title></head>
  <body>
    <p>${leadSentence}</p>
    <article>
      <p>${sentence.repeat(5)}</p>
      <iframe src="https://videos.example.com/embed/1" width="640" height="360"></iframe>
      <p>${sentence.repeat(5)} See <a href="/one">the first source</a> and <a href="/two">the second source</a> then <a href="/one">the first source</a> again.</p>
      <p>Find more under <a rel="tag" href="/topics/report">report</a>.</p>
    </article>
  </body>
</html>`;

const expectedArticle = {
  title: 'Collected study',
  softTitle: 'Collected study',
  author: ['Reporter A'],
  description: 'A study report',
  keywords: 'study, report',
  canonicalLink: 'https://example.com/a',
  tags: [],
  lang: 'en',
  date: null,
  publisher: null,
  copyright: null,
  image: null,
  videos: [],
  links: [{ text: 'the primary source', href: '/source' }],
};

const expectedMedia = {
  title: 'Video study',
  author: [],
  tags: ['report'],
  lang: 'en',
  videos: [
    {
      src: 'https://videos.example.com/embed/1',
      height: '360',
      width: '640',
    },
  ],
  // Order and the duplicated /one entry must be preserved exactly.
  links: [
    { text: 'the first source', href: '/one' },
    { text: 'the second source', href: '/two' },
    { text: 'the first source', href: '/one' },
    { text: 'report', href: '/topics/report' },
  ],
};

const metadataGetters = [
  'title',
  'softTitle',
  'date',
  'author',
  'publisher',
  'copyright',
  'favicon',
  'description',
  'keywords',
  'lang',
  'canonicalLink',
  'tags',
  'image',
];

const contentGetters = ['text', 'links', 'videos'];

function permutations(items) {
  if (items.length <= 1) return [items.slice()];
  const result = [];
  items.forEach((item, index) => {
    const rest = items.slice(0, index).concat(items.slice(index + 1));
    permutations(rest).forEach((restPermutation) => {
      result.push([item].concat(restPermutation));
    });
  });
  return result;
}

function readAll(lazy, order) {
  const values = {};
  order.forEach((getterName) => {
    values[getterName] = lazy[getterName]();
  });
  return values;
}

function assertMatchesEager(values, eager) {
  Object.keys(values).forEach((field) => {
    assert.deepEqual(values[field], eager[field], `field ${field}`);
  });
}

test('lazy extraction order', () => {
  // Every ordering of the three content getters, each combined with the
  // metadata getters running either before or after, must yield exactly
  // what a standalone full extraction returns.
  const contentOrders = permutations(contentGetters);
  const metadataOrders = [
    metadataGetters.slice(),
    metadataGetters.slice().reverse(),
  ];

  const cases = [
    { html: articleHtml, expected: expectedArticle },
    { html: mediaHtml, expected: expectedMedia },
  ];

  cases.forEach(({ html, expected }) => {
    const eager = unfluff(html);

    // The full extraction itself matches the concrete expectations.
    Object.keys(expected).forEach((field) => {
      assert.deepEqual(eager[field], expected[field], `eager field ${field}`);
    });

    contentOrders.forEach((contentOrder) => {
      metadataOrders.forEach((metadataOrder) => {
        // Metadata first, then content in this order.
        let lazy = unfluff.lazy(html);
        let values = readAll(lazy, metadataOrder.concat(contentOrder));
        assertMatchesEager(values, eager);

        // Content first, then metadata. This is the ordering that used to
        // erase links, videos, byline authors and tags.
        lazy = unfluff.lazy(html);
        values = readAll(lazy, contentOrder.concat(metadataOrder));
        assertMatchesEager(values, eager);

        // Metadata, content, then metadata again: repeated reads return
        // the same values instead of degrading as the doc gets mutated.
        lazy = unfluff.lazy(html);
        readAll(lazy, metadataOrder);
        readAll(lazy, contentOrder);
        metadataOrder.forEach((getterName) => {
          assert.deepEqual(
            lazy[getterName](),
            eager[getterName],
            `re-read metadata ${getterName}`
          );
        });
      });
    });

    // Repeated calls to the same getter are stable and memoized.
    const lazy = unfluff.lazy(html);
    contentGetters.forEach((getterName) => {
      const first = lazy[getterName]();
      const second = lazy[getterName]();
      assert.deepEqual(first, eager[getterName]);
      assert.equal(second, first, `getter ${getterName} is memoized`);
    });
  });

  // Two lazy instances on different pages, read interleaved, never bleed
  // state into each other.
  const articleLazy = unfluff.lazy(articleHtml);
  const mediaLazy = unfluff.lazy(mediaHtml);

  assert.deepEqual(mediaLazy.text().split('\n\n')[0], leadSentence);
  assert.deepEqual(articleLazy.links(), expectedArticle.links);
  assert.deepEqual(mediaLazy.links(), expectedMedia.links);
  assert.deepEqual(articleLazy.author(), expectedArticle.author);
  assert.deepEqual(mediaLazy.videos(), expectedMedia.videos);
  assert.deepEqual(mediaLazy.tags(), expectedMedia.tags);
  assert.equal(articleLazy.title(), expectedArticle.title);
  assert.equal(mediaLazy.title(), expectedMedia.title);

  // Explicit and auto-detected language arguments are independent and
  // agree here, and a normal full extraction between reads changes
  // nothing on either lazy instance.
  const explicitEn = unfluff.lazy(mediaHtml, 'en');
  const autoEn = unfluff.lazy(mediaHtml);
  assert.equal(explicitEn.lang(), 'en');
  assert.equal(autoEn.lang(), 'en');
  assert.equal(explicitEn.text(), autoEn.text());
  unfluff(articleHtml);
  unfluff(mediaHtml);
  assert.deepEqual(explicitEn.links(), expectedMedia.links);
  assert.deepEqual(autoEn.videos(), expectedMedia.videos);
});

test('lead paragraph is included once and body links keep their representation', () => {
  const eager = unfluff(mediaHtml);

  // The sibling lead paragraph is part of the extracted text...
  assert.ok(eager.text.startsWith(leadSentence));
  // ...exactly once, even though links and text each run post-cleanup.
  assert.equal(eager.text.split(leadSentence).length - 1, 1);
  // Anchors are expanded to plain text during text formatting.
  assert.ok(eager.text.includes('the first source'));
  // But the links getter still exposes text/href pairs in document order,
  // duplicates included.
  assert.deepEqual(eager.links, expectedMedia.links);

  const lazy = unfluff.lazy(mediaHtml);
  assert.deepEqual(lazy.links(), expectedMedia.links);
  const text = lazy.text();
  assert.ok(text.startsWith(leadSentence));
  assert.equal(text.split(leadSentence).length - 1, 1);
  // A second text read must not prepend the lead paragraph again.
  assert.equal(lazy.text(), text);

  // The byline is still removed from the article body text...
  const articleLazy = unfluff.lazy(articleHtml);
  assert.equal(articleLazy.text().includes('Reporter A'), false);
  // ...while the author metadata remains available in any read order.
  assert.deepEqual(articleLazy.author(), expectedArticle.author);
  const otherArticleLazy = unfluff.lazy(articleHtml);
  otherArticleLazy.text();
  otherArticleLazy.links();
  assert.deepEqual(otherArticleLazy.author(), expectedArticle.author);
  assert.deepEqual(otherArticleLazy.links(), expectedArticle.links);
});

test('lazy getters do not run the content pipeline early', () => {
  const lazy = unfluff.lazy(articleHtml);

  // Metadata-only access parses the document but never cleans it, never
  // selects a best node and never produces content results.
  metadataGetters.forEach((getterName) => lazy[getterName]());
  assert.ok(lazy.metadataDoc_);
  assert.equal(lazy.cleanedDoc_, undefined);
  assert.equal(lazy.topNode_, undefined);
  assert.equal(lazy.contentState_, undefined);
  assert.equal(lazy.text_, undefined);
  assert.equal(lazy.links_, undefined);
  assert.equal(lazy.videos_, undefined);

  // Reading one content field initializes the shared pipeline once and
  // leaves the other content fields uncomputed.
  const text = lazy.text();
  assert.ok(text.length > 0);
  assert.ok(lazy.cleanedDoc_);
  assert.ok(lazy.topNode_);
  assert.ok(lazy.contentState_);
  assert.equal(lazy.links_, undefined);
  assert.equal(lazy.videos_, undefined);

  const cleanedDoc = lazy.cleanedDoc_;
  const contentState = lazy.contentState_;
  const topNode = lazy.topNode_;

  // Further content getters reuse the same single cleaning pass and
  // best-node selection instead of running them again.
  assert.deepEqual(lazy.links(), expectedArticle.links);
  assert.deepEqual(lazy.videos(), expectedArticle.videos);
  assert.equal(lazy.cleanedDoc_, cleanedDoc);
  assert.equal(lazy.contentState_, contentState);
  assert.equal(lazy.topNode_, topNode);

  // A links-only instance never computes text or videos.
  const linksOnly = unfluff.lazy(mediaHtml);
  assert.deepEqual(linksOnly.links(), expectedMedia.links);
  assert.equal(linksOnly.text_, undefined);
  assert.equal(linksOnly.videos_, undefined);

  // A videos-only instance never computes text or links.
  const videosOnly = unfluff.lazy(mediaHtml);
  assert.deepEqual(videosOnly.videos(), expectedMedia.videos);
  assert.equal(videosOnly.text_, undefined);
  assert.equal(videosOnly.links_, undefined);
});
