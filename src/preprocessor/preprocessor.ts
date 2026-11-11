import { NodeVisitor, Path, visit } from '../ast/visit.js';
import {
  PreprocessorAstNode,
  PreprocessorElseIfNode,
  PreprocessorIdentifierNode,
  PreprocessorLiteralNode,
  PreprocessorSegmentNode,
} from './preprocessor-node.js';

import * as parser from './preprocessor-parser.js';
import { formatError } from '../error.js';

export type PreprocessorProgram = {
  type: string;
  program: PreprocessorSegmentNode[];
  wsEnd?: string;
};

// ---------------------------------------------------------------------------
// Macro expansion ("rescan" / paint-it-blue)
//
// Expansion is a single rescan loop over a list of preprocessing tokens,
// modeled on the C preprocessor algorithm instead of doing one full-string
// find-and-replace pass per macro. Every macro replacement list is tokenized,
// spliced into the token stream at the invocation site, and rescanned
// together with the tokens that followed the invocation, so:
//
// - An object-like macro that expands to a function-like macro name combines
//   with the "(" following it, regardless of definition order.
// - Each token carries a "hidden" set of macro names that are not allowed to
//   expand it. When a macro is invoked its name is added to the set for the
//   tokens produced by that invocation only, which makes self and mutual
//   recursion terminate. The set is scoped to the invocation, so later,
//   independent invocations of the same macros expand normally.
// - Long but finite alias chains run to completion; true cycles leave the
//   recursive macro references in the output as ordinary identifiers.
// ---------------------------------------------------------------------------

type PpToken = {
  text: string;
  // Macro names currently ineligible to expand this token ("painted blue").
  hidden: Set<string>;
};

const IDENTIFIER_START = /[A-Za-z_]/;
const IDENTIFIER_PART = /[A-Za-z_0-9]/;
const isIdentifierStart = (char: string): boolean =>
  IDENTIFIER_START.test(char);
const isIdentifierPart = (char: string): boolean => IDENTIFIER_PART.test(char);
const isWhitespace = (char: string): boolean => /\s/.test(char);
const isWhitespaceToken = (token: PpToken): boolean =>
  token.text.length > 0 && isWhitespace(token.text.charAt(0));
const isIdentifierToken = (token: PpToken): boolean =>
  token.text.length > 0 && isIdentifierStart(token.text.charAt(0));

const unionHidden = (a: Set<string>, b: Set<string>): Set<string> => {
  if (!b.size) {
    return a;
  }
  const merged = new Set<string>(a);
  b.forEach((name) => merged.add(name));
  return merged;
};

// Split replacement text into tokens. Whitespace runs are a single token so
// they are preserved verbatim, and every punctuation character is its own
// token, which is enough to scan parentheses and commas.
const tokenize = (src: string, hidden: Set<string>): PpToken[] => {
  const tokens: PpToken[] = [];
  let i = 0;

  while (i < src.length) {
    const char = src.charAt(i);
    if (isWhitespace(char)) {
      let end = i + 1;
      while (end < src.length && isWhitespace(src.charAt(end))) {
        end++;
      }
      tokens.push({ text: src.slice(i, end), hidden });
      i = end;
    } else if (isIdentifierStart(char)) {
      let end = i + 1;
      while (end < src.length && isIdentifierPart(src.charAt(end))) {
        end++;
      }
      tokens.push({ text: src.slice(i, end), hidden });
      i = end;
    } else {
      tokens.push({ text: char, hidden });
      i++;
    }
  }
  return tokens;
};

// Token pasting, matching the historical behavior of joining the tokens
// around " ## " into one token.
const pasteTokens = (tokens: PpToken[]): PpToken[] => {
  const pasted: PpToken[] = [];
  let i = 0;

  while (i < tokens.length) {
    // The pattern is whitespace, '#', '#', whitespace between two ordinary
    // tokens, mirroring the old /\s+##\s+/ regular expression.
    if (
      isWhitespaceToken(tokens[i]) &&
      tokens[i + 1]?.text === '#' &&
      tokens[i + 2]?.text === '#' &&
      isWhitespaceToken(tokens[i + 3])
    ) {
      const left = pasted.pop();
      const right = tokens[i + 4];
      if (
        left &&
        right &&
        !isWhitespaceToken(left) &&
        !isWhitespaceToken(right)
      ) {
        pasted.push({
          text: left.text + right.text,
          hidden: unionHidden(left.hidden, right.hidden),
        });
        i += 5;
        continue;
      }
    }
    pasted.push(tokens[i]);
    i++;
  }
  return pasted;
};

// From glsl2s https://github.com/cimaron/glsl2js/blob/4046611ac4f129a9985d74704159c41a402564d0/preprocessor/comments.js
const preprocessComments = (src: string): string => {
  let i;
  let chr;
  let la;
  let out = '';
  let line = 1;
  let in_single = 0;
  let in_multi = 0;

  for (i = 0; i < src.length; i++) {
    chr = src.substring(i, i + 1);
    la = src.substring(i + 1, i + 2);

    // Enter single line comment
    if (chr == '/' && la == '/' && !in_single && !in_multi) {
      in_single = line;
      i++;
      continue;
    }

    // Exit single line comment
    if (chr == '\n' && in_single) {
      in_single = 0;
    }

    // Enter multi line comment
    if (chr == '/' && la == '*' && !in_multi && !in_single) {
      in_multi = line;
      i++;
      continue;
    }

    // Exit multi line comment
    if (chr == '*' && la == '/' && in_multi) {
      // Treat single line multi-comment as space
      if (in_multi == line) {
        out += ' ';
      }

      in_multi = 0;
      i++;
      continue;
    }

    // Newlines are preserved
    if ((!in_multi && !in_single) || chr == '\n') {
      out += chr;
      line++;
    }
  }

  return out;
};

type NodeEvaluator<NodeType> = (
  node: NodeType,
  visit: (node: PreprocessorAstNode) => any
) => any;

export type NodeEvaluators = Partial<
  {
    [NodeType in PreprocessorAstNode['type']]: NodeEvaluator<
      Extract<PreprocessorAstNode, { type: NodeType }>
    >;
  }
>;

// Use the same parser, but starting from the expression entry point, to parse
// expression strings on the fly after macro expansion
const expressionParser = (src: string): PreprocessorAstNode =>
  formatError(parser)(src, {
    grammarSource: 'expression',
    startRule: 'constant_expression',
  });

const evaluate = (ast: PreprocessorAstNode, evaluators: NodeEvaluators) => {
  const visit = (node: PreprocessorAstNode) => {
    const evaluator = evaluators[node.type];
    if (!evaluator) {
      throw new Error(`No evaluate() evaluator for ${node.type}`);
    }

    // I can't figure out why evalutor has node type never here
    // @ts-ignore
    return evaluator(node, visit);
  };
  return visit(ast);
};

export type Macro = {
  args?: PreprocessorAstNode[];
  body: string;
};

export type Macros = {
  [name: string]: Macro;
};

// Index of the next non-whitespace token at or after `start`, or -1.
const nextSignificantToken = (tokens: PpToken[], start: number): number => {
  for (let i = start; i < tokens.length; i++) {
    if (!isWhitespaceToken(tokens[i])) {
      return i;
    }
  }
  return -1;
};

// Scan the arguments of a function-like macro invocation. `start` is the index
// just after the opening "(". Parentheses are balanced while scanning, so a
// comma inside a nested call does not terminate an argument early.
type ScannedArgs = {
  args: PpToken[][];
  closeIndex: number;
};

const scanFunctionArgs = (
  tokens: PpToken[],
  start: number
): ScannedArgs | null => {
  let depth = 0;
  const args: PpToken[][] = [];
  let current: PpToken[] = [];

  for (let i = start; i < tokens.length; i++) {
    const { text } = tokens[i];

    if (text === '(') {
      depth++;
      current.push(tokens[i]);
    } else if (text === ')') {
      if (depth === 0) {
        // In the case of "()", there is no empty-string argument, but there is
        // one in "(,)" and "(asdf)". Only capture an empty argument when a
        // previous comma implies one, which we can infer from a previous arg.
        if (current.length || args.length) {
          args.push(current);
        }
        return { args, closeIndex: i };
      }
      depth--;
      current.push(tokens[i]);
    } else if (text === ',' && depth === 0) {
      args.push(current);
      current = [];
    } else {
      current.push(tokens[i]);
    }
  }

  return null;
};

// Drop leading and trailing whitespace tokens, matching the historical
// behavior of trimming raw argument strings before substituting them.
const trimTokens = (tokens: PpToken[]): PpToken[] => {
  let start = 0;
  let end = tokens.length;
  while (start < end && isWhitespaceToken(tokens[start])) {
    start++;
  }
  while (end > start && isWhitespaceToken(tokens[end - 1])) {
    end--;
  }
  return tokens.slice(start, end);
};

// Rescan a token list, expanding any macros found in it. `hidden` is the set
// of macro names ineligible for expansion while processing this list (the
// names of macro invocations currently being expanded).
const expandTokens = (
  input: PpToken[],
  macros: Macros,
  hidden: Set<string>
): PpToken[] => {
  const output: PpToken[] = [];
  let tokens = input;
  let i = 0;

  while (i < tokens.length) {
    const token = tokens[i];

    if (!isIdentifierToken(token)) {
      output.push(token);
      i++;
      continue;
    }

    // A token's own hidden set comes from the invocation that produced it; the
    // passed-in hidden set comes from invocations still being rescanned. Both
    // apply while the token is under consideration.
    const names = unionHidden(hidden, token.hidden);
    const macro = macros[token.text];
    if (!macro || names.has(token.text)) {
      // Keep an unexpanded identifier painted with every name currently
      // hidden so the suppression survives later splicing and rescanning.
      output.push({ text: token.text, hidden: names });
      i++;
      continue;
    }

    if (macro.args) {
      // A function-like macro name is only a macro invocation when an opening
      // "(" follows it (ignoring whitespace). Otherwise it stays as-is, e.g.
      // a function macro name used as a plain identifier.
      const openParen = nextSignificantToken(tokens, i + 1);
      if (openParen === -1 || tokens[openParen].text !== '(') {
        output.push({ text: token.text, hidden: names });
        i++;
        continue;
      }

      const scanned = scanFunctionArgs(tokens, openParen + 1);
      if (scanned === null) {
        const invocation = tokens
          .slice(i, openParen + 1)
          .map((part) => part.text)
          .join('');
        throw new Error(`${invocation} unterminated macro invocation`);
      }

      const macroArgs = macro.args.filter(
        (arg) => (arg as PreprocessorLiteralNode).literal !== ','
      );
      const { args, closeIndex } = scanned;

      if (args.length > macroArgs.length) {
        throw new Error(`'${token.text}': Too many arguments for macro`);
      }
      if (args.length < macroArgs.length) {
        throw new Error(`'${token.text}': Not enough arguments for macro`);
      }

      // The self-reference rule: while this invocation's replacement list is
      // being rescanned, its own name cannot be expanded again.
      const invocationHidden = new Set(names);
      invocationHidden.add(token.text);

      // Arguments are fully macro expanded before substitution, using the
      // outer hide set only. The macro currently being invoked is not hidden
      // yet, so a nested invocation of the same macro inside an argument still
      // expands; each argument gets its own expansion, so a hidden name
      // un-hides for the next argument and everything after the invocation.
      const argIdentifiers = macroArgs.map(
        (arg) => (arg as PreprocessorIdentifierNode).identifier
      );
      const argReplacements: Record<string, PpToken[]> = {};
      argIdentifiers.forEach((identifier, index) => {
        argReplacements[identifier] = expandTokens(
          trimTokens(args[index]),
          macros,
          names
        );
      });

      // Substitute arguments into the macro body, then apply token pasting.
      // Every replacement-list token is painted with this invocation's hidden
      // set. Argument tokens are additionally painted with this invocation's
      // name: an argument that could not expand during prescan (for example a
      // function-like macro name with no "(" next to it) must not gain a "("
      // from the tokens after the invocation and expand during the rescan.
      const bodyTokens = tokenize(macro.body || '', invocationHidden);
      const substituted = bodyTokens.map((bodyToken) =>
        isIdentifierToken(bodyToken) && bodyToken.text in argReplacements
          ? argReplacements[bodyToken.text].map((argToken) => ({
              text: argToken.text,
              hidden: unionHidden(argToken.hidden, invocationHidden),
            }))
          : [bodyToken]
      );
      const replacement = pasteTokens(substituted.flat());

      // Splice the replacement list in front of the tokens that followed the
      // closing ")" and keep rescanning from the start of it. The replacement
      // rescans together with the source tokens after the invocation, so a
      // function-like macro name produced by the expansion combines with a
      // following "(". The invocation's own name is hidden only on the
      // replacement tokens, so the tokens after the invocation (and the next
      // independent invocation) are unaffected.
      tokens = [...replacement, ...tokens.slice(closeIndex + 1)];
      i = 0;
    } else {
      // Object-like macro: splice its replacement list in front of the
      // remaining tokens and keep scanning, so the replacement rescans
      // together with whatever followed the macro (for example a "(" that
      // turns the expanded name into a function macro invocation).
      const invocationHidden = new Set(names);
      invocationHidden.add(token.text);
      const replacement = pasteTokens(
        tokenize(macro.body || '', invocationHidden)
      );
      tokens = [...replacement, ...tokens.slice(i + 1)];
      i = 0;
    }
  }

  return output;
};

const expandMacros = (text: string, macros: Macros): string =>
  expandTokens(tokenize(text, new Set<string>()), macros, new Set<string>())
    .map((token) => token.text)
    .join('');

const isTruthy = (x: any): boolean => !!x;

const evaluateIfPart = (macros: Macros, ifPart: PreprocessorAstNode) => {
  if (ifPart.type === 'if') {
    return ifPart.expression
      ? isTruthy(evaluateExpressionString(ifPart.expression, macros))
      : false;
  } else if (ifPart.type === 'ifdef') {
    return ifPart.identifier.identifier in macros;
  } else if (ifPart.type === 'ifndef') {
    return !(ifPart.identifier.identifier in macros);
  }
};

// TODO: Are all of these operators equivalent between javascript and GLSL?
const evaluteExpression = (node: PreprocessorAstNode, macros: Macros) =>
  evaluate(node, {
    // TODO: Handle non-base-10 numbers. Should these be parsed in the peg grammar?
    int_constant: (node) => parseInt(node.token, 10),
    unary_defined: (node) => node.identifier.identifier in macros,
    identifier: (node) => 0, // Undefined macros evaluate to 0 per spec
    group: (node, visit) => visit(node.expression),
    binary: ({ left, right, operator: { literal } }, visit) => {
      switch (literal) {
        // multiplicative
        case '*': {
          return visit(left) * visit(right);
        }
        // division
        case '/': {
          return visit(left) / visit(right);
        }
        // modulo
        case '%': {
          return visit(left) % visit(right);
        }
        // addition
        case '+': {
          return visit(left) + visit(right);
        }
        // subtraction
        case '-': {
          return visit(left) - visit(right);
        }
        // bit-wise shift
        case '<<': {
          return visit(left) << visit(right);
        }
        // bit-wise shift
        case '>>': {
          return visit(left) >> visit(right);
        }
        case '<': {
          return visit(left) < visit(right);
        }
        case '>': {
          return visit(left) > visit(right);
        }
        case '<=': {
          return visit(left) <= visit(right);
        }
        case '>=': {
          return visit(left) >= visit(right);
        }
        case '==': {
          return visit(left) == visit(right);
        }
        case '!=': {
          return visit(left) != visit(right);
        }
        // bit-wise and
        case '&': {
          return visit(left) & visit(right);
        }
        // bit-wise exclusive or
        case '^': {
          return visit(left) ^ visit(right);
        }
        // bit-wise inclusive or
        case '|': {
          return visit(left) | visit(right);
        }
        case '&&': {
          return visit(left) && visit(right);
        }
        case '||': {
          return visit(left) || visit(right);
        }
        default: {
          throw new Error(
            `Preprocessing error: Unknown binary operator ${literal}`
          );
        }
      }
    },
    unary: (node, visit) => {
      switch (node.operator.literal) {
        case '+': {
          return visit(node.expression);
        }
        case '-': {
          return -1 * visit(node.expression);
        }
        case '!': {
          return !visit(node.expression);
        }
        case '~': {
          return ~visit(node.expression);
        }
        default: {
          throw new Error(
            `Preprocessing error: Unknown unary operator ${node.operator.literal}`
          );
        }
      }
    },
  });

const shouldPreserve = (preserve: NodePreservers = {}) => (
  path: PathOverride<PreprocessorAstNode>
) => {
  const test = preserve?.[path.node.type];
  return typeof test === 'function' ? test(path) : test;
};

// HACK: The AST visitors are hard coded to the GLSL AST (not preprocessor AST)
// types. I'm not clever enough to make the core AST type geneeric so that both
// GLSL AST (in ast.ts) and the preprocessed AST can use the same
// visitor/evaluator/path pattern. I took a stab at it but it become tricky to
// track all the nested generics. Instead, I hack re-cast the visit function
// here, which at least gives some minor type safety.
type VisitorOverride = (
  ast: PreprocessorAstNode | PreprocessorProgram,
  visitors: {
    [NodeType in PreprocessorAstNode['type']]?: NodeVisitor<
      Extract<PreprocessorAstNode, { type: NodeType }>
    >;
  }
) => void;

// @ts-ignore
export const visitPreprocessedAst = visit as VisitorOverride;

type PathOverride<NodeType> = {
  node: NodeType;
  parent: PreprocessorAstNode | undefined;
  parentPath: Path<any> | undefined;
  key: string | undefined;
  index: number | undefined;
  skip: () => void;
  remove: () => void;
  replaceWith: (replacer: PreprocessorAstNode) => void;
  findParent: (test: (p: Path<any>) => boolean) => Path<any> | undefined;

  skipped?: boolean;
  removed?: boolean;
  replaced?: any;
};
const convertPath = (p: Path<any>) =>
  (p as unknown) as PathOverride<typeof p['node']>;

/**
 * Perform the preprocessing logic, aka the "preprocessing" phase of the compiler.
 * Expand macros, evaluate conditionals, etc
 * TODO: Define the strategy for conditionally removing certain macro types
 * and conditionally expanding certain expressions. And take in optiona list
 * of pre defined thigns?
 * TODO: Handle __LINE__ and other constants.
 */

export type NodePreservers = {
  [nodeType: string]: (path: PathOverride<any>) => boolean;
};

export type PreprocessorOptions = {
  defines?: { [definitionName: string]: Object };
  preserve?: NodePreservers;
  preserveComments?: boolean;
  stopOnError?: boolean;
  grammarSource?: string;
  startRule?: 'constant_expression' | 'program';
};

// Expressions are stored as strings in the AST, since their contents may be
// affected by macro expansion. This function performs macro expansion, then
// parses and evaluates the expanded string. It uses the same grammar, but
// starting from the "constant expression" rule to parse, aka the rule for the
// expression of an if / elif
const evaluateExpressionString = (expr: string, macros: Macros): any => {
  // Strip inline comments so "defined/**/A" is treated as "defined A"
  const stripped = preprocessComments(expr);

  // In the input
  //     #define A
  //     #if !defined(A)
  // If we expand macros then evaluate the expression it will break. The spec
  // says that identifiers inside defined() are not eligible for expansion. So
  // hacky way to evaluate them first before macro expansion and parsing
  const defined = stripped
    .replace(/defined\s*\(\s*([A-Za-z_][A-Za-z_0-9]*)\s*\)/g, (_, id) =>
      id in macros ? '1' : '0'
    )
    .replace(/defined\s+([A-Za-z_][A-Za-z_0-9]*)/g, (_, id) =>
      id in macros ? '1' : '0'
    );

  // Macro expansion in #if / #elif expressions follows the same rescan rules
  // as macro expansion in the program body. Any identifiers still unexpanded
  // are treated as zero by the expression evaluator below.
  const expanded = expandMacros(defined, macros);
  return evaluteExpression(expressionParser(expanded.trim()), macros);
};

// Remove escaped newlines, rather than try to handle them in the grammar
const unescapeSrc = (src: string, options: PreprocessorOptions = {}) =>
  src.replace(/\\[\n\r]/g, '');

const preprocessAst = (
  program: PreprocessorProgram,
  options: PreprocessorOptions = {}
) => {
  const macros: Macros = Object.entries(options.defines || {}).reduce(
    (defines, [name, body]) => ({ ...defines, [name]: { body } }),
    {}
  );

  const { preserve } = options;
  const preserveNode = shouldPreserve(preserve);

  visitPreprocessedAst(program, {
    conditional: {
      enter: (initialPath) => {
        const path = convertPath(initialPath);
        const { node } = path;
        // TODO: Determining if we need to handle edge case conditionals here
        if (preserveNode(path)) {
          return;
        }

        if (evaluateIfPart(macros, node.ifPart)) {
          path.replaceWith(node.ifPart.body);
        } else {
          const elseBranchHit = node.elseIfParts.reduce(
            (res: boolean, elif: PreprocessorElseIfNode) =>
              res ||
              (isTruthy(evaluateExpressionString(elif.expression, macros)) &&
                // path/visit hack to remove type error
                (path.replaceWith(elif.body as PreprocessorAstNode), true)),
            false
          );
          if (!elseBranchHit) {
            if (node.elsePart) {
              path.replaceWith(node.elsePart.body as PreprocessorAstNode);
            } else {
              path.remove();
            }
          }
        }
      },
    },
    text: {
      enter: (initialPath) => {
        const path = convertPath(initialPath);
        path.node.text = expandMacros(path.node.text, macros);
      },
    },
    define_arguments: {
      enter: (initialPath) => {
        const path = convertPath(initialPath);
        const {
          identifier: { identifier },
          body,
          args,
        } = path.node;

        macros[identifier] = { args, body };
        !preserveNode(path) && path.remove();
      },
    },
    define: {
      enter: (initialPath) => {
        const path = convertPath(initialPath);
        const {
          identifier: { identifier },
          body,
        } = path.node;

        macros[identifier] = { body };
        !preserveNode(path) && path.remove();
      },
    },
    undef: {
      enter: (initialPath) => {
        const path = convertPath(initialPath);
        delete macros[path.node.identifier.identifier];
        !preserveNode(path) && path.remove();
      },
    },
    error: {
      enter: (initialPath) => {
        const path = convertPath(initialPath);
        if (options.stopOnError) {
          throw new Error(path.node.message);
        }
        !preserveNode(path) && path.remove();
      },
    },
    pragma: {
      enter: (initialPath) => {
        const path = convertPath(initialPath);
        !preserveNode(path) && path.remove();
      },
    },
    version: {
      enter: (initialPath) => {
        const path = convertPath(initialPath);
        !preserveNode(path) && path.remove();
      },
    },
    extension: {
      enter: (initialPath) => {
        const path = convertPath(initialPath);
        !preserveNode(path) && path.remove();
      },
    },
    // TODO: Causes a failure
    line: {
      enter: (initialPath) => {
        const path = convertPath(initialPath);
        !preserveNode(path) && path.remove();
      },
    },
  });

  // Even though it mutates, useful for passing around functions
  return program;
};

export { preprocessAst, preprocessComments, unescapeSrc };
