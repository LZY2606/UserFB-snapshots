import { expect, test, beforeAll } from 'vitest';

import {
  preprocessAst,
  PreprocessorProgram,
  PreprocessorOptions,
} from './preprocessor.js';
import generatePreprocessed from './generator.js';
import preprocess from './index.js';
import { generate as generateGlsl } from '../index.js';

import {
  buildPreprocessorParser,
  buildParser,
} from '../parser/test-helpers.js';
import { visit } from '../ast/visit.js';
import { Program } from '../ast/index.js';

let parsePreprocessor!: (
  src: string,
  options?: PreprocessorOptions
) => PreprocessorProgram;

let parseGlsl!: (src: string, options?: any) => Program;

beforeAll(async () => {
  parsePreprocessor = (await buildPreprocessorParser()).parse;
  parseGlsl = (await buildParser()).parse;
});

// Split preprocessed output into significant tokens (identifiers, numbers and
// individual punctuation) so recursive output can be asserted token by token,
// rather than only checking that expansion terminates.
const tokenizeOutput = (src: string): string[] =>
  src.match(/[A-Za-z_][A-Za-z_0-9]*|[0-9]+(?:\.[0-9]+)?|\S/g) ?? [];

const expand = (src: string): string => {
  const ast = parsePreprocessor(src);
  preprocessAst(ast);
  return generatePreprocessed(ast);
};

// Collect every node of a given GLSL AST type, in visit order.
const collectNodes = (ast: Program, type: string): any[] => {
  const found: any[] = [];
  visit(ast, {
    [type]: { enter: (path: any) => found.push(path.node) },
  } as any);
  return found;
};

// Ordered list of every AST node type, used to prove two parses describe the
// same structure. Walks the node and its array/object children generically.
const nodeTypeTrace = (node: any): string[] => {
  const trace: string[] = [];
  const walk = (value: any) => {
    if (!value || typeof value !== 'object') {
      return;
    }
    if (typeof value.type === 'string') {
      trace.push(value.type);
    }
    Object.values(value).forEach((child) => {
      if (Array.isArray(child)) {
        child.forEach(walk);
      } else if (child && typeof child === 'object') {
        walk(child);
      }
    });
  };
  walk(node);
  return trace;
};

test('alias-after-expansion', () => {
  // The two macros are defined independently, so swapping their definition
  // order must not change the result. An object-like macro that expands to a
  // function-like macro name rescans together with the following "(".
  const functionFirst = `
#define B(x) (x + 1)
#define A B
A(2)
`;
  const aliasFirst = `
#define A B
#define B(x) (x + 1)
A(2)
`;

  expect(expand(functionFirst).trim()).toBe('(2 + 1)');
  expect(expand(aliasFirst).trim()).toBe('(2 + 1)');

  // The string entry point must behave identically to the AST entry point.
  expect(preprocess(functionFirst).trim()).toBe('(2 + 1)');
  expect(preprocess(aliasFirst).trim()).toBe('(2 + 1)');

  // A long but finite alias chain runs to completion, again ending in an
  // object-like macro aliasing a function-like macro.
  const longChain = `
#define M0 M1
#define M1 M2
#define M2 M3
#define M3 M4
#define M4(x) (x)
M0(7)
`;
  expect(expand(longChain).trim()).toBe('(7)');

  // Replacement text and a "(" later in the source form one invocation.
  const replacedNameThenParen = `
#define NAME F
#define F(x) x * x
NAME(3)
`;
  expect(expand(replacedNameThenParen).trim()).toBe('3 * 3');
});

test('recursive-token-suppression', () => {
  // Mutual recursion: neither macro can be re-expanded while its own
  // invocation is in flight, so the whole invocation is left untouched,
  // token for token.
  const recursive = expand(`
#define foo(x) bar(x)
#define bar(x) foo(x)
bar(foo(1.0))
`).trim();
  expect(tokenizeOutput(recursive)).toEqual([
    'bar',
    '(',
    'foo',
    '(',
    '1.0',
    ')',
    ')',
  ]);

  // Suppression is scoped to the invocation: a later, independent invocation
  // expands normally, including when the same macro name is called again.
  const followedByIndependent = expand(`
#define foo(x) bar(x)
#define bar(x) foo(x)
bar(foo(1.0))
#define BAZ(x) (x + 3)
BAZ(4)
BAZ(5)
`);
  const lines = followedByIndependent
    .split('\n')
    .map((line) => line.trim())
    .filter(Boolean);
  expect(tokenizeOutput(lines[0])).toEqual([
    'bar',
    '(',
    'foo',
    '(',
    '1.0',
    ')',
    ')',
  ]);
  expect(tokenizeOutput(lines[1])).toEqual(['(', '4', '+', '3', ')']);
  expect(tokenizeOutput(lines[2])).toEqual(['(', '5', '+', '3', ')']);

  // A self referencing function macro terminates and keeps its name.
  const selfReferential = expand(`
#define self() self()
self()
`).trim();
  expect(tokenizeOutput(selfReferential)).toEqual(['self', '(', ')']);

  // A finite chain that looks recursive at first still terminates with the
  // real expansion.
  const finiteChain = expand(`
#define A(x) B(x)
#define B(x) (x)
A(5)
`).trim();
  expect(tokenizeOutput(finiteChain)).toEqual(['(', '5', ')']);
});

test('conditional-rescan', () => {
  // An aliased function-like macro has to actually invoke the function macro
  // inside the #if expression to select the branch.
  const aliasPicksBranch = `
#define PICK(A) A
#define CHOICE PICK
#if CHOICE(1)
float picked = 1.0;
#else
float picked = 0.0;
#endif
`;
  const ifOutput = expand(aliasPicksBranch);
  expect(ifOutput).toContain('float picked = 1.0;');
  expect(ifOutput).not.toContain('float picked = 0.0;');

  // The same rescan rules drive #elif selection.
  const elifSource = `
#define SEL(x) x
#define AL SEL
#if AL(0)
float branch = 0.0;
#elif AL(1)
float branch = 1.0;
#else
float branch = 2.0;
#endif
`;
  const elifOutput = expand(elifSource);
  expect(elifOutput).toContain('float branch = 1.0;');
  expect(elifOutput).not.toContain('float branch = 0.0;');
  expect(elifOutput).not.toContain('float branch = 2.0;');

  // defined() operands are not expanded and an undefined identifier is zero.
  const definedAndZero = `
#define Q 3
#if defined(Q) && UNKNOWN_IDENTIFIER == 0
float conditional = 1.0;
#endif
`;
  expect(expand(definedAndZero)).toContain('float conditional = 1.0;');

  // #undef followed by a redefinition changes the branch selected later.
  const undefRedefine = `
#define M(x) 0
#if M(5)
float first = 1.0;
#else
float first = 0.0;
#endif
#undef M
#define M(x) 1
#if M(5)
float second = 1.0;
#else
float second = 0.0;
#endif
`;
  const redefined = expand(undefRedefine);
  expect(redefined).toContain('float first = 0.0;');
  expect(redefined).not.toContain('float first = 1.0;');
  expect(redefined).toContain('float second = 1.0;');
  expect(redefined).not.toContain('float second = 0.0;');
});

test('shader-ast-roundtrip', () => {
  // A valid expansion feeds straight into the GLSL parser.
  const expanded = preprocess(`
#define SCALE(x) ((x) * 2.0)
#define TYPE float
TYPE value = SCALE(3.0);
`).trim();
  expect(expanded).toBe('float value = ((3.0) * 2.0);');

  const ast = parseGlsl(expanded, { quiet: true });

  // The object-like macro selected the declaration's type.
  const declarators = collectNodes(ast, 'declarator_list');
  expect(declarators).toHaveLength(1);
  expect(declarators[0].declarations[0].identifier.identifier).toBe('value');
  expect(declarators[0].specified_type.specifier.specifier.token).toBe('float');

  // The function-like macro produced a multiplicative binary expression with
  // both numeric constants.
  const binary = collectNodes(ast, 'binary');
  expect(binary).toHaveLength(1);
  expect(binary[0].operator.literal).toBe('*');
  const constants = collectNodes(ast, 'float_constant').map(
    (node) => node.token
  );
  expect(constants).toEqual(['3.0', '2.0']);

  // Generate and re-parse: the declaration and expression stay identical.
  const generated = generateGlsl(ast);
  expect(generated.trim()).toBe(expanded);

  const reparsed = parseGlsl(generated, { quiet: true });
  expect(generateGlsl(reparsed)).toBe(generated);
  expect(nodeTypeTrace(reparsed)).toEqual(nodeTypeTrace(ast));

  // Macro names left behind by recursion are ordinary GLSL function names to
  // the parser; it does not have to evaluate them.
  const recursive = preprocess(`
#define foo(x) bar(x)
#define bar(x) foo(x)
float v = bar(foo(1.0));
`).trim();
  const recursiveAst = parseGlsl(recursive, { quiet: true });
  const calls = collectNodes(recursiveAst, 'function_call');
  const callNames = calls.map((call) => call.identifier.identifier);
  expect(callNames).toContain('bar');
  expect(callNames).toContain('foo');
  expect(
    generateGlsl(parseGlsl(generateGlsl(recursiveAst), { quiet: true }))
  ).toBe(generateGlsl(recursiveAst));
});
