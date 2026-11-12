var describe = require('node:test').describe;
var it = require('node:test').it;
var assert = require('node:assert/strict');
var jp = require('../jmespath');
var tokenize = jp.tokenize;
var compile = jp.compile;
var search = jp.search;

function throwsWith(name, expression, errorName) {
  assert.throws(function() { search({}, expression); }, function(error) {
    return error.name === errorName;
  }, expression + ' should throw ' + errorName);
}

// Recursively confirm that the environment chain is never stored on the
// shared AST nodes.
function assertNoEnvLeak(node) {
  if (node === null || typeof node !== 'object') {
    return;
  }
  if (Array.isArray(node)) {
    node.forEach(assertNoEnvLeak);
    return;
  }
  assert.equal(node.env, undefined, 'AST node must not carry an environment');
  Object.keys(node).forEach(function(key) {
    if (key !== 'jmespathType') {
      assertNoEnvLeak(node[key]);
    }
  });
}

describe('binding-order', function() {
  it('keeps outer configuration reachable inside a projection', function() {
    var data = {threshold: 2,
                rows: [{id: 'a', n: 1}, {id: 'b', n: 3}]};
    assert.deepEqual(
      search(data, 'let $limit = threshold in rows[?n > $limit].id'),
      ['b']);
  });

  it('evaluates bindings left to right so later right-hand sides see earlier ones',
     function() {
       assert.equal(search({base: 2}, 'let $a = base, $b = $a in $b'), 2);
       assert.equal(search({}, 'let $a = `10`, $b = $a in $b'), 10);
     });

  it('accepts identifier characters including underscores and digits', function() {
    assert.equal(search({}, 'let $a_1 = `1`, $b2 = $a_1 in $b2'), 1);
  });

  it('lets an inner binding shadow an outer binding of the same name', function() {
    assert.equal(search({x: 10}, 'let $x = x in let $x = `1` in $x'), 1);
    assert.deepEqual(
      search({x: 10}, 'let $x = x in [let $x = `1` in $x, $x]'),
      [1, 10]);
  });

  it('preserves bindings across filter projections and their right-hand side',
     function() {
       var data = {limit: 2, rows: [{n: 1}, {n: 3}, {n: 4}]};
       assert.deepEqual(
         search(data, 'let $l = limit in rows[?n > $l].n'),
         [3, 4]);
     });

  it('works over empty arrays and null current nodes', function() {
    assert.deepEqual(
      search({rows: []}, 'let $l = `1` in rows[?n > $l]'),
      []);
    assert.equal(search(null, 'let $l = `1` in $l'), 1);
  });

  it('treats a null binding as a declared value, not a missing variable',
     function() {
       assert.equal(search({x: null}, 'let $v = x in $v'), null);
       assert.equal(search({}, 'let $v = `null` in $v'), null);
     });

  it('allows let forms everywhere an expression is allowed', function() {
    assert.equal(search({x: 5}, '(let $y = x in $y)'), 5);
    assert.deepEqual(search({}, '[let $z = `1` in $z, `2`]'), [1, 2]);
    assert.deepEqual(search({}, '{a: let $x = `1` in $x}'), {a: 1});
    assert.equal(search({}, "length(let $s = 'abc' in $s)"), 3);
    assert.deepEqual(
      search({rows: [{n: 1}, {n: 2}]},
             'rows[?let $m = `1` in n > $m].n'),
      [2]);
    var data = {rows: [1]};
    assert.equal(
      search(data, 'let $config = @ in rows | $config'),
      data);
  });

  it('evaluates right-hand sides and the body against the entry current node',
     function() {
       var data = {config: {v: 2}, rows: [{n: 1}, {n: 3}]};
       assert.deepEqual(
         search(data, 'let $c = config in rows[?n > $c.v].n'),
         [3]);
     });
});

describe('lexical-expref', function() {
  it('captures the creation environment while @ stays dynamic in map', function() {
    var data = {cfg: 'outer', rows: [{n: 1}, {n: 2}]};
    assert.deepEqual(
      search(data, 'let $cfg = cfg in map(&[n, $cfg], rows)'),
      [[1, 'outer'], [2, 'outer']]);
    // The captured @ is not frozen: &@ still sees each processed element.
    assert.deepEqual(
      search(data, 'let $marker = `1` in map(&@, rows)'),
      data.rows);
  });

  it('distinguishes lexical capture from dynamic lookup under shadowing',
     function() {
       // The outer reference is created before the inner binding shadows
       // the same name; executing it later must still see the outer value.
       var data = {cfg: 'outer', rows: [{n: 1}, {n: 2}]};
       var expression = 'let $cfg = cfg in ' +
                        'let $ref = &$cfg in ' +
                        'let $cfg = `"inner"` in map($ref, rows)';
       assert.deepEqual(search(data, expression), ['outer', 'outer']);
       // Without the captured reference, plain lookup is dynamically shadowed.
       assert.equal(
         search(data, 'let $cfg = cfg in let $cfg = `"inner"` in $cfg'),
         'inner');
     });

  it('keeps closures alive across nested projections', function() {
    var data = {limit: 2,
                groups: [[{n: 1}, {n: 3}], [{n: 4}]]};
    assert.deepEqual(
      search(data, 'let $l = limit in map(&[?n > $l].n, groups)'),
      [[3], [4]]);
  });

  it('allows variables to hold expression references for by-functions',
     function() {
       var data = {rows: [{n: 3}, {n: 1}, {n: 2}]};
       assert.deepEqual(
         search(data, 'let $key = &n in max_by(rows, $key)'),
         {n: 3});
       assert.deepEqual(
         search(data, 'let $key = &n in min_by(rows, $key)'),
         {n: 1});
       assert.deepEqual(
         search(data, 'let $key = &n in sort_by(rows, $key)'),
         [{n: 1}, {n: 2}, {n: 3}]);
     });

  it('reuses one captured reference for every element', function() {
    var data = {factor: 10, rows: [1, 2, 3]};
    assert.deepEqual(
      search(data, 'let $f = factor in map(&[@, $f], rows)'),
      [[1, 10], [2, 10], [3, 10]]);
  });
});

describe('scope-errors', function() {
  it('rejects duplicate declarations in the same binding group', function() {
    throwsWith('duplicate', 'let $a = `1`, $a = `2` in $a', 'ParserError');
  });

  it('rejects references to undeclared variables with their name', function() {
    assert.throws(function() { search({}, 'let $a = `1` in $b'); },
                  function(error) {
                    return error.name === 'UndefinedVariable' &&
                           error.message.indexOf('$b') >= 0;
                  });
  });

  it('rejects a missing binding right-hand side', function() {
    throwsWith('missing rhs', 'let $a = in $a', 'ParserError');
    throwsWith('missing rhs comma', 'let $a = , $b = `1` in $b',
               'ParserError');
  });

  it('rejects a missing in keyword', function() {
    throwsWith('missing in', 'let $a = `1` $a', 'ParserError');
    throwsWith('trailing let', 'let $a = `1`', 'ParserError');
  });

  it('keeps a lone dollar sign a lexer error', function() {
    throwsWith('lone dollar', 'let $ = `1` in @', 'LexerError');
  });

  it('does not leak bindings into sibling expressions', function() {
    throwsWith('sibling scope', '[let $x = `1` in $x, $x]',
               'UndefinedVariable');
  });

  it('keeps null bindings distinct from undeclared variables', function() {
    assert.doesNotThrow(function() {
      assert.equal(search({x: null}, 'let $v = x in $v'), null);
    });
    throwsWith('undeclared beside null',
               'let $v = `null` in $missing', 'UndefinedVariable');
  });

  it('leaves no state behind after a failed search', function() {
    assert.throws(function() {
      search({}, 'let $a = `1` in $undeclared');
    }, function(error) { return error.name === 'UndefinedVariable'; });
    assert.throws(function() {
      search({rows: []}, 'let $l = `1` in no_such_function(rows)');
    });
    // New searches, including one reusing the same variable names, work.
    assert.equal(search({}, 'let $a = `1` in $a'), 1);
    assert.deepEqual(
      search({threshold: 2, rows: [{id: 'b', n: 3}]},
             'let $limit = threshold in rows[?n > $limit].id'),
      ['b']);
  });
});

describe('legacy-compat', function() {
  it('still treats let and in as ordinary field names outside binding forms',
     function() {
       assert.equal(search({let: 1, in: 2}, 'let'), 1);
       assert.equal(search({let: 1, in: 2}, 'in'), 2);
       assert.equal(search({let: {in: 3}}, 'let.in'), 3);
       assert.deepEqual(
         search({rows: [{let: 'x'}]}, 'rows[].let'),
         ['x']);
     });

  it('leaves dollar signs inside quoted strings alone', function() {
    assert.equal(search({'$foo': 9}, '"$foo"'), 9);
    assert.equal(search({}, "'price: $5'"), 'price: $5');
  });

  it('keeps legacy token streams identical', function() {
    assert.deepEqual(tokenize('foo.bar'), [
      {type: 'UnquotedIdentifier', value: 'foo', start: 0},
      {type: 'Dot', value: '.', start: 3},
      {type: 'UnquotedIdentifier', value: 'bar', start: 4}
    ]);
    assert.deepEqual(tokenize('let'),
                     [{type: 'UnquotedIdentifier', value: 'let', start: 0}]);
  });

  it('keeps legacy parse trees and return structures compatible', function() {
    assert.deepEqual(compile('foo'), {type: 'Field', name: 'foo'});
    var data = {threshold: 2,
                rows: [{id: 'a', n: 1}, {id: 'b', n: 3}]};
    assert.deepEqual(search(data, 'rows[?n > `2`].id'), ['b']);
    assert.deepEqual(search(data, 'map(&id, rows)'), ['a', 'b']);
  });

  it('never stores environments on the shared AST', function() {
    var expression = 'let $cfg = cfg in let $ref = &$cfg in map($ref, rows)';
    var ast = compile(expression);
    assertNoEnvLeak(ast);
    // Compiling again yields the same tree shape.
    assert.deepEqual(compile(expression), ast);
  });

  it('does not mutate bound values and isolates repeated searches', function() {
    var config = {v: 2};
    var data = {config: config, rows: []};
    assert.strictEqual(
      search(data, 'let $c = config in $c'), config);
    assert.deepEqual(
      search({x: 1}, 'let $x = x in $x'),
      search({x: 1}, 'let $x = x in $x'));
    assert.equal(search({x: 7}, 'let $x = x in $x'), 7);
    assert.equal(search({x: 8}, 'let $x = x in $x'), 8);
  });
});
