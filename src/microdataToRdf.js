const htmlparser = require('htmlparser2');
const isAbsoluteUrl = require('is-absolute-url');
const microdataDom = require('./microdataDom');
const constants = require('./constants');

/**
 * @typedef {{ id: string }} NamedNode
 * @typedef {{ id?: undefined, value: string | number | boolean, type?: string, language?: string }} LiteralNode
 * @typedef {NamedNode | LiteralNode} Object
 * @typedef {{ subject: string, predicate: string, object: Object, graph?: string }} Triple
 *
 * @typedef {{
 *   currentVocab: string | null
 *   currentType: string | null
 *   ancestors: import('./microdataDom').Item[]
 *   memory: Map<import('./microdataDom').Item, string>
 * }} Context
 */

/**
 * @param {string} html
 * @param {import('./index').Config} config
 * @returns {Triple[]}
 */
function microdataToRdf (html, config) {
  const registry = config.registry;

  const dom = microdataDom(htmlparser.parseDOM(html, {
    decodeEntities: true
  }), config);

  let nextId = 0;

  /**
   * @returns {string}
   */
  function generateBlankNode () {
    const blank = '_:' + nextId;
    nextId += 1;
    return blank;
  }

  /**
   * @param {string | null} type
   * @returns {string | null}
   */
  function getVocab (type) {
    if (!type) {
      return null;
    }
    const registryVocab = Object.keys(registry).find((prefix) => type.startsWith(prefix));
    return registryVocab || type.replace(/[^#/]*$/, '');
  }

  /**
   * @param {string} name
   * @param {Context} ctx
   * @returns {string}
   */
  function generatePredicateUri (name, ctx) {
    if (isAbsoluteUrl(name)) {
      return name;
    }
    const fragment = encodeURIComponent(name);
    if (!ctx.currentType) {
      return config.base + '#' + fragment;
    }
    if (ctx.currentVocab && /[#/]$/.test(ctx.currentVocab)) {
      return ctx.currentVocab + fragment;
    }
    return ctx.currentVocab + '#' + fragment;
  }

  const triples = /** @type {Triple[]} */([]);

  /**
   * Records the vocabulary contexts in which each item has already emitted
   * its triples. A shared element (one carrying several itemprop tokens, or
   * pulled in through several itemrefs) is visited more than once, but its
   * fields must only be generated once per vocabulary: reprocessing it under
   * the same vocabulary would duplicate every property and rdf:type, while
   * skipping it under a newly inherited vocabulary would lose the predicates
   * that belong to that vocabulary. Keyed on the DOM element itself, so two
   * independent items with identical contents are never merged. Created per
   * parse, so separate toJson calls never share this state.
   * @type {Map<import('./microdataDom').Item, Set<string | null>>}
   */
  const visitedVocabs = new Map();

  /**
   * @param {string} name
   * @param {Context} ctx
   * @returns {string[]}
   */
  function getEquivalents (name, ctx) {
    if (!ctx.currentVocab) {
      return [];
    }
    if (!registry[ctx.currentVocab]) {
      return [];
    }
    const properties = registry[ctx.currentVocab].properties;
    if (!properties) {
      return [];
    }
    const property = properties[name];
    if (!property) {
      return [];
    }
    let result = /** @type {string[]} */([]);
    if (property.subPropertyOf) {
      result = result.concat(property.subPropertyOf);
    }
    if (property.equivalentProperty) {
      result = result.concat(property.equivalentProperty);
    }
    return result;
  }

  /**
   * @param {import('./microdataDom').Item} item
   * @param {Context} ctx
   * @returns {string}
   */
  function generateTriples (item, ctx) {
    const subject = ctx.memory.get(item) || dom.getItemId(item) || generateBlankNode();
    ctx.memory.set(item, subject);

    // Ancestor cycles are evaluated before the shared-item guard so a genuine
    // cycle is never mistaken for an already visited shared item.
    if (ctx.ancestors.indexOf(item) >= 0) {
      if (config.strict) {
        throw new Error('Cyclic structure');
      } else {
        return subject;
      }
    }

    // A shared element (multiple itemprop tokens, or referenced through
    // several itemrefs) is reached more than once. Only skip it when it has
    // already emitted its body under the vocabulary inherited here: a
    // typeless shared item reached from different vocabularies still has to
    // emit its predicates once per vocabulary.
    const inheritedVocab = ctx.currentVocab;
    let visitedVocabSet = visitedVocabs.get(item);
    if (!visitedVocabSet) {
      visitedVocabSet = new Set();
      visitedVocabs.set(item, visitedVocabSet);
    }
    if (visitedVocabSet.has(inheritedVocab)) {
      return subject;
    }
    visitedVocabSet.add(inheritedVocab);

    const itemTypes = dom.getItemType(item);
    for (const itemType of itemTypes) {
      triples.push({
        subject: subject,
        predicate: constants.RDF__TYPE,
        object: { id: itemType }
      });
    }

    const type = itemTypes[0] || ctx.currentType || null;

    ctx.currentVocab = getVocab(type);

    const newCtx = {
      ancestors: ctx.ancestors.concat([item]),
      memory: ctx.memory,
      currentType: type,
      currentVocab: ctx.currentVocab
    };

    for (const element of dom.getProperties(item)) {
      for (const name of dom.getPropertyNames(element)) {
        const predicate = generatePredicateUri(name, newCtx);
        /** @type {Object | null} */
        let value = null;
        if (dom.isItem(element)) {
          value = { id: generateTriples(element, newCtx) };
        } else {
          value = /** @type {Object | null} */(dom.getItemValue(element));
        }

        if (value === null) continue;

        triples.push({
          subject: subject,
          predicate: predicate,
          object: value
        });

        for (const equiv of getEquivalents(name, ctx)) {
          triples.push({
            subject: subject,
            predicate: equiv,
            object: value
          });
        }
      }
    }

    return subject;
  }

  const context = {
    ancestors: [],
    memory: new Map(),
    currentType: null,
    currentVocab: null
  };

  for (const item of dom.getItems()) {
    generateTriples(item, context);
  }

  return triples;
}

/**
 * @param {Object} node
 * @returns {node is NamedNode}
 */
function isNamedNode (node) {
  return node.id !== undefined;
}

/**
 * @param {Object} node
 * @returns {node is LiteralNode}
 */
function isLiteralNode (node) {
  return node.id === undefined;
}

module.exports = microdataToRdf;
module.exports.isNamedNode = isNamedNode;
module.exports.isLiteralNode = isLiteralNode;
