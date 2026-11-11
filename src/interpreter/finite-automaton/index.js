/**
 * The MIT License (MIT)
 * Copyright (c) 2017-present Dmitry Soshnikov <dmitry.soshnikov@gmail.com>
 */

'use strict';

const NFA = require('./nfa/nfa');
const DFA = require('./dfa/dfa');

const nfaFromRegExp = require('./nfa/nfa-from-regexp');
const builders = require('./nfa/builders');
const analyzeAmbiguity = require('./ambiguity');

module.exports = {

  /**
   * Export NFA and DFA classes.
   */
  NFA,
  DFA,

  /**
   * Expose builders.
   */
  builders,

  /**
   * Builds an NFA for the passed regexp.
   *
   * @param string | AST | RegExp:
   *
   *   a regular expression in different representations: a string,
   *   a RegExp object, or an AST.
   */
  toNFA(regexp) {
    return nfaFromRegExp.build(regexp);
  },

  /**
   * Builds DFA for the passed regexp.
   *
   * @param string | AST | RegExp:
   *
   *   a regular expression in different representations: a string,
   *   a RegExp object, or an AST.
   */
  toDFA(regexp) {
    return new DFA(this.toNFA(regexp));
  },

  /**
   * Returns true if regexp accepts the string.
   */
  test(regexp, string) {
    return this.toDFA(regexp).matches(string);
  },

  /**
   * Statically analyzes a regexp for ambiguity: whether some string
   * has two genuinely different derivations (parse trees).
   *
   * @param string | AST | RegExp:
   *
   *   a regular expression in different representations: a string,
   *   a RegExp object, or an AST.
   *
   * @return Object:
   *
   *   a JSON-serializable report with `status` of `unambiguous`,
   *   `ambiguous` (with a shortest witness and two distinct
   *   derivations), or `unsupported` (with a reason and the source
   *   range or structural path of the unsupported construct).
   */
  analyzeAmbiguity(regexp) {
    return analyzeAmbiguity(regexp);
  },
};