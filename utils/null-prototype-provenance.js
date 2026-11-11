/**
 * Flow-sensitive provenance analysis for `Object.create(null)` dictionaries.
 *
 * The analysis is intentionally conservative: a dynamic property access is
 * only allowed when every object that may reach the receiver at that point is
 * known to have been created by an unshadowed `Object.create(null)` and has
 * not escaped or had its prototype changed.
 *
 * This module never executes the analysed code.
 */

'use strict';

const FUNCTION_NODE_TYPES = new Set(['ArrowFunctionExpression', 'FunctionDeclaration', 'FunctionExpression']);

const UNKNOWN = null;

/**
 * @typedef {object} ProvenanceState
 * @property {Map<import('eslint').Scope.Variable, Set<number>>} env
 *   Variables of the current scope that definitely refer to tracked objects.
 * @property {Map<number, boolean>} taint
 *   Tracked object ids whose prototype may have changed or that escaped.
 */

/**
 * Return the statically known name of a member expression, if any.
 * @param {import('estree').MemberExpression} node
 * @returns {string | null}
 */
function getStaticMemberName(node) {
  if (!node.computed && node.property.type === 'Identifier') {
    return node.property.name;
  }
  if (node.computed) {
    if (node.property.type === 'Literal' && typeof node.property.value === 'string') {
      return node.property.value;
    }
    if (node.property.type === 'TemplateLiteral' && node.property.expressions.length === 0) {
      return node.property.quasis[0].value.cooked;
    }
  }
  return null;
}

/**
 * Strip transparent wrappers that do not affect the value of an expression.
 * @param {import('estree').Node} node
 * @returns {import('estree').Node}
 */
function unwrapExpression(node) {
  while (node && (node.type === 'ParenthesizedExpression' || node.type === 'ChainExpression')) {
    node = node.expression;
  }
  return node;
}

/**
 * Whether a node is a direct `null` literal.
 * @param {import('estree').Node | null} node
 * @returns {boolean}
 */
function isNullLiteral(node) {
  node = unwrapExpression(node);
  return Boolean(node && node.type === 'Literal' && node.value === null);
}

/**
 * @param {import('estree').Node} node
 * @returns {node is import('estree').Function}
 */
function isFunctionNode(node) {
  return Boolean(node && FUNCTION_NODE_TYPES.has(node.type));
}

class NullPrototypeProvenance {
  /**
   * @param {import('eslint').SourceCode} sourceCode
   */
  constructor(sourceCode) {
    this.sourceCode = sourceCode;
    this.decisions = new Map();
    this.objects = new Map();
    this.nextObjectId = 0;
    this.variableCache = new WeakMap();
    this.capturedVariables = this.collectCapturedVariables();
  }

  /**
   * Resolve an identifier to the variable binding it references, if any.
   * @param {import('estree').Identifier} node
   * @returns {import('eslint').Scope.Variable | null}
   */
  resolveVariable(node) {
    if (this.variableCache.has(node)) {
      return this.variableCache.get(node);
    }
    let scope = this.sourceCode.getScope(node);
    while (scope) {
      const variable = scope.set.get(node.name);
      if (variable) {
        this.variableCache.set(node, variable);
        return variable;
      }
      scope = scope.upper;
    }
    this.variableCache.set(node, null);
    return null;
  }

  /**
   * Collect variables referenced from a nested function: closure capture
   * makes every object stored in such a binding escape its proof.
   * @returns {Set<import('eslint').Scope.Variable>}
   */
  collectCapturedVariables() {
    /** @type {Set<import('eslint').Scope.Variable>} */
    const captured = new Set();
    for (const scope of this.sourceCode.scopeManager.scopes) {
      for (const variable of scope.variables) {
        for (const reference of variable.references) {
          const nearestFunction = this.nearestFunctionScope(reference.from);
          if (nearestFunction && this.isStrictAncestorScope(variable.scope, nearestFunction)) {
            captured.add(variable);
          }
        }
      }
    }
    return captured;
  }

  /**
   * @param {import('eslint').Scope.Scope} scope
   * @returns {import('eslint').Scope.Scope | null}
   */
  nearestFunctionScope(scope) {
    let current = scope;
    while (current) {
      if (current.type === 'function') {
        return current;
      }
      if (current.type === 'global' || current.type === 'module') {
        return null;
      }
      current = current.upper;
    }
    return null;
  }

  /**
   * @param {import('eslint').Scope.Scope} maybeAncestor
   * @param {import('eslint').Scope.Scope} scope
   * @returns {boolean}
   */
  isStrictAncestorScope(maybeAncestor, scope) {
    let current = scope.upper;
    while (current) {
      if (current === maybeAncestor) {
        return true;
      }
      current = current.upper;
    }
    return false;
  }

  /**
   * @param {import('eslint').Scope.Variable} variable
   * @param {import('eslint').Scope.Scope} rootScope
   * @returns {boolean}
   */
  isLocalVariable(variable, rootScope) {
    let current = variable.scope;
    while (current) {
      if (current === rootScope) {
        return true;
      }
      current = current.upper;
    }
    return false;
  }

  /**
   * @returns {ProvenanceState}
   */
  createState() {
    return { env: new Map(), taint: new Map() };
  }

  /**
   * @param {ProvenanceState} state
   * @returns {ProvenanceState}
   */
  cloneState(state) {
    return { env: new Map(state.env), taint: new Map(state.taint) };
  }

  /**
   * @param {ProvenanceState} target
   * @param {ProvenanceState} source
   */
  replaceState(target, source) {
    target.env = source.env;
    target.taint = source.taint;
  }

  /**
   * Merge states that may all reach the same program point. A variable is
   * only known when every path agrees on a tracked value, so any path with an
   * unknown value makes the merged value unknown.
   * @param {ProvenanceState[]} states
   * @returns {ProvenanceState}
   */
  mergeStates(states) {
    const merged = this.createState();
    if (states.length === 0) {
      return merged;
    }

    for (const state of states) {
      for (const [id, tainted] of state.taint) {
        if (tainted) {
          merged.taint.set(id, true);
        }
      }
    }

    const variables = new Set();
    for (const state of states) {
      for (const variable of state.env.keys()) {
        variables.add(variable);
      }
    }

    for (const variable of variables) {
      let value = UNKNOWN;
      let knownOnEveryPath = true;
      for (const state of states) {
        const pathValue = state.env.get(variable) || UNKNOWN;
        if (!pathValue) {
          knownOnEveryPath = false;
          break;
        }
        value = value ? this.unionValues(value, pathValue) : new Set(pathValue);
      }
      if (knownOnEveryPath) {
        merged.env.set(variable, value);
      }
    }

    return merged;
  }

  /**
   * @param {Set<number>} left
   * @param {Set<number>} right
   * @returns {Set<number>}
   */
  unionValues(left, right) {
    return new Set([...left, ...right]);
  }

  /**
   * @param {Set<number> | null} left
   * @param {Set<number> | null} right
   * @returns {Set<number> | null}
   */
  unionExpressionValues(left, right) {
    if (!left || !right) {
      return UNKNOWN;
    }
    return this.unionValues(left, right);
  }

  /**
   * @returns {Set<number>}
   */
  allocateNullPrototypeObject() {
    const id = this.nextObjectId++;
    this.objects.set(id, { nullPrototype: true });
    return new Set([id]);
  }

  /**
   * Whether every object that may reach this value is still a pristine
   * `Object.create(null)` dictionary.
   * @param {ProvenanceState} state
   * @param {Set<number> | null} value
   * @returns {boolean}
   */
  isSafeValue(state, value) {
    if (!value || value.size === 0) {
      return false;
    }
    for (const id of value) {
      const object = this.objects.get(id);
      if (!object || !object.nullPrototype || state.taint.get(id)) {
        return false;
      }
    }
    return true;
  }

  /**
   * Mark every tracked object reachable through the value as escaped.
   * @param {ProvenanceState} state
   * @param {Set<number> | null} value
   */
  escapeValue(state, value) {
    if (!value) {
      return;
    }
    for (const id of value) {
      state.taint.set(id, true);
    }
  }

  /**
   * @param {import('estree').Identifier} node
   * @param {ProvenanceState} state
   * @param {import('eslint').Scope.Scope} rootScope
   * @returns {Set<number> | null}
   */
  readIdentifier(node, state, rootScope) {
    const variable = this.resolveVariable(node);
    if (!variable || !this.isLocalVariable(variable, rootScope)) {
      return UNKNOWN;
    }
    return state.env.get(variable) || UNKNOWN;
  }

  /**
   * Assign a value to a binding. Assignments to captured or outer bindings
   * escape the assigned object; assignments to local bindings do not.
   * @param {import('estree').Identifier} node
   * @param {ProvenanceState} state
   * @param {import('eslint').Scope.Scope} rootScope
   * @param {Set<number> | null} value
   */
  writeIdentifier(node, state, rootScope, value) {
    const variable = this.resolveVariable(node);
    if (!variable || !this.isLocalVariable(variable, rootScope)) {
      this.escapeValue(state, value);
      return;
    }
    if (this.capturedVariables.has(variable)) {
      this.escapeValue(state, value);
    }
    if (value) {
      state.env.set(variable, value);
    } else {
      state.env.delete(variable);
    }
  }

  /**
   * Run the analysis for every program/function scope in the file.
   * @returns {Map<import('estree').MemberExpression, boolean>}
   */
  run() {
    const programNode = this.sourceCode.ast;
    const programScope = this.sourceCode.getScope(programNode);
    const programState = this.createState();
    this.analyzeStatements(programNode.body, programState, programScope);

    for (const scope of this.sourceCode.scopeManager.scopes) {
      if (scope.type !== 'function' || !isFunctionNode(scope.block)) {
        continue;
      }
      const state = this.createState();
      this.analyzeFunction(scope.block, scope, state);
    }

    return this.decisions;
  }

  /**
   * @param {import('estree').Function} functionNode
   * @param {import('eslint').Scope.Scope} scope
   * @param {ProvenanceState} state
   */
  analyzeFunction(functionNode, scope, state) {
    for (const parameter of functionNode.params) {
      this.analyzePatternDefaults(parameter, state, scope);
    }
    if (functionNode.body.type === 'BlockStatement') {
      this.analyzeStatements(functionNode.body.body, state, scope);
    } else {
      this.evaluateExpression(functionNode.body, state, scope, { asCallee: false });
    }
  }

  /**
   * Evaluate default expressions inside a parameter/destructuring pattern
   * for their side effects. The bound values themselves stay unknown because
   * callers can supply anything.
   * @param {import('estree').Node} pattern
   * @param {ProvenanceState} state
   * @param {import('eslint').Scope.Scope} rootScope
   */
  analyzePatternDefaults(pattern, state, rootScope) {
    if (!pattern) {
      return;
    }
    switch (pattern.type) {
      case 'AssignmentPattern':
        this.evaluateExpression(pattern.right, state, rootScope, { asCallee: false });
        this.analyzePatternDefaults(pattern.left, state, rootScope);
        break;
      case 'ArrayPattern':
        for (const element of pattern.elements) {
          this.analyzePatternDefaults(element, state, rootScope);
        }
        break;
      case 'ObjectPattern':
        for (const property of pattern.properties) {
          if (property.type === 'RestElement') {
            this.analyzePatternDefaults(property.argument, state, rootScope);
          } else {
            this.analyzePatternDefaults(property.value, state, rootScope);
          }
        }
        break;
      case 'RestElement':
        this.analyzePatternDefaults(pattern.argument, state, rootScope);
        break;
      default:
        break;
    }
  }

  /**
   * Bind the identifiers of a declaration/assignment pattern. Destructuring
   * reads properties, which are always unknown values.
   * @param {import('estree').Node} pattern
   * @param {ProvenanceState} state
   * @param {import('eslint').Scope.Scope} rootScope
   * @param {Set<number> | null} value
   */
  bindPattern(pattern, state, rootScope, value) {
    if (!pattern) {
      return;
    }
    switch (pattern.type) {
      case 'Identifier':
        this.writeIdentifier(pattern, state, rootScope, value);
        break;
      case 'AssignmentPattern':
        this.evaluateExpression(pattern.right, state, rootScope, { asCallee: false });
        this.bindPattern(pattern.left, state, rootScope, UNKNOWN);
        break;
      case 'ArrayPattern':
        for (const element of pattern.elements) {
          this.bindPattern(element, state, rootScope, UNKNOWN);
        }
        break;
      case 'ObjectPattern':
        for (const property of pattern.properties) {
          if (property.type === 'RestElement') {
            this.bindPattern(property.argument, state, rootScope, UNKNOWN);
          } else {
            if (property.computed) {
              this.evaluateExpression(property.key, state, rootScope, { asCallee: false });
            }
            this.bindPattern(property.value, state, rootScope, UNKNOWN);
          }
        }
        break;
      case 'RestElement':
        this.bindPattern(pattern.argument, state, rootScope, UNKNOWN);
        break;
      default:
        break;
    }
  }

  /**
   * Collect identifiers assigned by statements in a loop/try body, so a proof
   * from before the construct cannot be reused on a later iteration/path.
   * Nested functions are analysed separately and are not descended into.
   * @param {import('estree').Node} node
   * @param {Set<import('eslint').Scope.Variable>} result
   */
  collectAssignedVariables(node, result) {
    if (!node || typeof node.type !== 'string') {
      return;
    }
    if (isFunctionNode(node) || node.type === 'ClassDeclaration' || node.type === 'ClassExpression') {
      return;
    }
    switch (node.type) {
      case 'AssignmentExpression':
        this.collectPatternVariables(node.left, result);
        break;
      case 'UpdateExpression':
        this.collectPatternVariables(node.argument, result);
        break;
      case 'VariableDeclarator':
        this.collectPatternVariables(node.id, result);
        break;
      default:
        break;
    }
    for (const key of Object.keys(node)) {
      if (key === 'parent') {
        continue;
      }
      const child = node[key];
      if (Array.isArray(child)) {
        for (const item of child) {
          if (item && typeof item.type === 'string') {
            this.collectAssignedVariables(item, result);
          }
        }
      } else if (child && typeof child.type === 'string') {
        this.collectAssignedVariables(child, result);
      }
    }
  }

  /**
   * @param {import('estree').Node} pattern
   * @param {Set<import('eslint').Scope.Variable>} result
   */
  collectPatternVariables(pattern, result) {
    if (!pattern) {
      return;
    }
    switch (pattern.type) {
      case 'Identifier': {
        const variable = this.resolveVariable(pattern);
        if (variable) {
          result.add(variable);
        }
        break;
      }
      case 'AssignmentPattern':
        this.collectPatternVariables(pattern.left, result);
        break;
      case 'ArrayPattern':
        for (const element of pattern.elements) {
          this.collectPatternVariables(element, result);
        }
        break;
      case 'ObjectPattern':
        for (const property of pattern.properties) {
          if (property.type === 'RestElement') {
            this.collectPatternVariables(property.argument, result);
          } else {
            this.collectPatternVariables(property.value, result);
          }
        }
        break;
      case 'RestElement':
        this.collectPatternVariables(pattern.argument, result);
        break;
      default:
        break;
    }
  }

  /**
   * Invalidate proofs for variables assigned inside a loop/try body before
   * analysing that body. The pre-construct state is kept separately so a
   * path that never runs the body can still contribute to the merge.
   * @param {import('estree').Node} node
   * @param {ProvenanceState} bodyState
   * @param {import('eslint').Scope.Scope} rootScope
   */
  invalidateAssignedVariables(node, bodyState, rootScope) {
    const assigned = new Set();
    this.collectAssignedVariables(node, assigned);
    for (const variable of assigned) {
      if (this.isLocalVariable(variable, rootScope)) {
        bodyState.env.delete(variable);
      }
    }
  }

  /**
   * @param {import('estree').Statement[]} statements
   * @param {ProvenanceState} state
   * @param {import('eslint').Scope.Scope} rootScope
   * @returns {{terminated: boolean}}
   */
  analyzeStatements(statements, state, rootScope) {
    for (const statement of statements) {
      const result = this.analyzeStatement(statement, state, rootScope);
      if (result.terminated) {
        return { terminated: true };
      }
    }
    return { terminated: false };
  }

  /**
   * @param {import('estree').Statement} node
   * @param {ProvenanceState} state
   * @param {import('eslint').Scope.Scope} rootScope
   * @returns {{terminated: boolean}}
   */
  analyzeStatement(node, state, rootScope) {
    if (!node) {
      return { terminated: false };
    }

    switch (node.type) {
      case 'BlockStatement':
        return this.analyzeStatements(node.body, state, rootScope);

      case 'VariableDeclaration':
        for (const declarator of node.declarations) {
          const value = declarator.init
            ? this.evaluateExpression(declarator.init, state, rootScope, { asCallee: false })
            : UNKNOWN;
          this.bindPattern(declarator.id, state, rootScope, value);
        }
        return { terminated: false };

      case 'ExpressionStatement':
        this.evaluateExpression(node.expression, state, rootScope, { asCallee: false });
        return { terminated: false };

      case 'IfStatement':
        return this.analyzeIfStatement(node, state, rootScope);

      case 'ReturnStatement':
      case 'ThrowStatement': {
        const argument = node.argument
          ? this.evaluateExpression(node.argument, state, rootScope, { asCallee: false })
          : UNKNOWN;
        this.escapeValue(state, argument);
        return { terminated: true };
      }

      case 'WhileStatement':
      case 'DoWhileStatement':
      case 'ForStatement':
      case 'ForInStatement':
      case 'ForOfStatement':
        return this.analyzeLoop(node, state, rootScope);

      case 'TryStatement':
        return this.analyzeTryStatement(node, state, rootScope);

      case 'SwitchStatement':
        return this.analyzeSwitchStatement(node, state, rootScope);

      case 'LabeledStatement':
        return this.analyzeStatement(node.body, state, rootScope);

      case 'BreakStatement':
      case 'ContinueStatement':
        return { terminated: true };

      case 'WithStatement':
        this.analyzeStatement(node.body, state, rootScope);
        return { terminated: false };

      case 'FunctionDeclaration':
      case 'ClassDeclaration':
      case 'EmptyStatement':
      case 'DebuggerStatement':
      case 'ImportDeclaration':
      case 'ExportAllDeclaration':
      case 'MetaProperty':
        return { terminated: false };

      case 'ExportNamedDeclaration':
        return node.declaration ? this.analyzeStatement(node.declaration, state, rootScope) : { terminated: false };

      case 'ExportDefaultDeclaration':
        if (!node.declaration) {
          return { terminated: false };
        }
        if (isFunctionNode(node.declaration) || node.declaration.type === 'ClassDeclaration') {
          return { terminated: false };
        }
        this.evaluateExpression(node.declaration, state, rootScope, { asCallee: false });
        return { terminated: false };

      default:
        return { terminated: false };
    }
  }

  /**
   * @param {import('estree').IfStatement} node
   * @param {ProvenanceState} state
   * @param {import('eslint').Scope.Scope} rootScope
   * @returns {{terminated: boolean}}
   */
  analyzeIfStatement(node, state, rootScope) {
    this.evaluateExpression(node.test, state, rootScope, { asCallee: false });
    const base = this.cloneState(state);

    const consequentState = this.cloneState(base);
    const consequent = this.analyzeStatement(node.consequent, consequentState, rootScope);

    const alternateState = this.cloneState(base);
    const alternate = node.alternate
      ? this.analyzeStatement(node.alternate, alternateState, rootScope)
      : { terminated: false };

    const survivors = [];
    if (!consequent.terminated) {
      survivors.push(consequentState);
    }
    if (!alternate.terminated) {
      survivors.push(alternateState);
    }

    if (survivors.length === 0) {
      return { terminated: true };
    }

    this.replaceState(state, this.mergeStates(survivors));
    return { terminated: false };
  }

  /**
   * @param {import('estree').WhileStatement | import('estree').DoWhileStatement | import('estree').ForStatement | import('estree').ForInStatement | import('estree').ForOfStatement} node
   * @param {ProvenanceState} state
   * @param {import('eslint').Scope.Scope} rootScope
   * @returns {{terminated: boolean}}
   */
  analyzeLoop(node, state, rootScope) {
    if (node.type === 'ForStatement' && node.init) {
      if (node.init.type === 'VariableDeclaration') {
        this.analyzeStatement(node.init, state, rootScope);
      } else {
        this.evaluateExpression(node.init, state, rootScope, { asCallee: false });
      }
    }

    if (node.type === 'ForInStatement' || node.type === 'ForOfStatement') {
      this.evaluateExpression(node.right, state, rootScope, { asCallee: false });
      if (node.left.type === 'VariableDeclaration') {
        this.analyzeStatement(node.left, state, rootScope);
      } else {
        this.bindPattern(node.left, state, rootScope, UNKNOWN);
      }
    }

    if (node.type === 'WhileStatement' || node.type === 'ForStatement') {
      if (node.test) {
        this.evaluateExpression(node.test, state, rootScope, { asCallee: false });
      }
    }

    const base = this.cloneState(state);
    const bodyState = this.cloneState(base);
    this.invalidateAssignedVariables(node.body, bodyState, rootScope);
    this.analyzeStatement(node.body, bodyState, rootScope);

    if (node.type === 'ForStatement' && node.update) {
      this.evaluateExpression(node.update, bodyState, rootScope, { asCallee: false });
    }

    if (node.type === 'DoWhileStatement') {
      this.evaluateExpression(node.test, bodyState, rootScope, { asCallee: false });
    }

    // Loops may execute zero or many times. Merge the pre-loop state with one
    // analysed iteration; this deliberately stays conservative. Any variable
    // assigned inside the loop loses its proof after the loop, because a
    // branch may have left through `break` (or a later iteration may have
    // rebound it) without being represented in the single merged state.
    const merged = this.mergeStates([base, bodyState]);
    const assigned = new Set();
    this.collectAssignedVariables(node.body, assigned);
    for (const variable of assigned) {
      if (this.isLocalVariable(variable, rootScope)) {
        merged.env.delete(variable);
      }
    }
    this.replaceState(state, merged);
    return { terminated: false };
  }

  /**
   * @param {import('estree').TryStatement} node
   * @param {ProvenanceState} state
   * @param {import('eslint').Scope.Scope} rootScope
   * @returns {{terminated: boolean}}
   */
  analyzeTryStatement(node, state, rootScope) {
    const base = this.cloneState(state);

    const blockState = this.cloneState(base);
    this.invalidateAssignedVariables(node.block, blockState, rootScope);
    const block = this.analyzeStatement(node.block, blockState, rootScope);

    const handlerState = this.mergeStates([base, blockState]);
    let handler = null;
    if (node.handler) {
      this.invalidateAssignedVariables(node.handler.body, handlerState, rootScope);
      handler = this.analyzeStatement(node.handler.body, handlerState, rootScope);
    }

    const afterBlock = block.terminated ? null : blockState;
    const afterHandler = node.handler && handler && !handler.terminated ? handlerState : null;
    const survivors = [afterBlock, afterHandler].filter(Boolean);

    if (node.finalizer) {
      const finalizerState = survivors.length > 0 ? this.mergeStates(survivors) : this.cloneState(base);
      const finalizer = this.analyzeStatement(node.finalizer, finalizerState, rootScope);
      if (finalizer.terminated) {
        return { terminated: true };
      }
      if (survivors.length > 0) {
        this.replaceState(state, finalizerState);
      }
      return { terminated: false };
    }

    if (survivors.length === 0) {
      return { terminated: true };
    }

    this.replaceState(state, this.mergeStates(survivors));
    return { terminated: false };
  }

  /**
   * @param {import('estree').SwitchStatement} node
   * @param {ProvenanceState} state
   * @param {import('eslint').Scope.Scope} rootScope
   * @returns {{terminated: boolean}}
   */
  analyzeSwitchStatement(node, state, rootScope) {
    this.evaluateExpression(node.discriminant, state, rootScope, { asCallee: false });
    const base = this.cloneState(state);
    const survivors = [];
    let hasDefault = false;

    for (const switchCase of node.cases) {
      if (switchCase.test) {
        this.evaluateExpression(switchCase.test, state, rootScope, { asCallee: false });
      } else {
        hasDefault = true;
      }
      const caseState = this.cloneState(base);
      const result = this.analyzeStatements(switchCase.consequent, caseState, rootScope);
      if (!result.terminated) {
        survivors.push(caseState);
      }
    }

    if (!hasDefault) {
      survivors.push(base);
    }

    if (survivors.length === 0) {
      return { terminated: true };
    }

    this.replaceState(state, this.mergeStates(survivors));
    return { terminated: false };
  }

  /**
   * @param {import('estree').Node} node
   * @returns {boolean}
   */
  isUnshadowedObjectIdentifier(node) {
    if (!node || node.type !== 'Identifier' || node.name !== 'Object') {
      return false;
    }
    const variable = this.resolveVariable(node);
    return Boolean(variable && variable.defs.length === 0);
  }

  /**
   * @param {import('estree').CallExpression} node
   * @returns {boolean}
   */
  isNullPrototypeFactoryCall(node) {
    const callee = unwrapExpression(node.callee);
    if (!callee || callee.type !== 'MemberExpression') {
      return false;
    }
    if (getStaticMemberName(callee) !== 'create' || !this.isUnshadowedObjectIdentifier(callee.object)) {
      return false;
    }
    return isNullLiteral(node.arguments[0]);
  }

  /**
   * @param {import('estree').CallExpression} node
   * @returns {boolean}
   */
  isSetPrototypeOfCall(node) {
    const callee = unwrapExpression(node.callee);
    if (!callee || callee.type !== 'MemberExpression') {
      return false;
    }
    return getStaticMemberName(callee) === 'setPrototypeOf' && this.isUnshadowedObjectIdentifier(callee.object);
  }

  /**
   * Evaluate a member expression, optionally as the callee of a call (in
   * which case the receiver escapes as `this`).
   * @param {import('estree').MemberExpression} node
   * @param {ProvenanceState} state
   * @param {import('eslint').Scope.Scope} rootScope
   * @param {boolean} asCallee
   * @returns {{receiver: Set<number> | null, name: string | null}}
   */
  evaluateMember(node, state, rootScope, asCallee) {
    const receiver = this.evaluateExpression(node.object, state, rootScope, { asCallee: false });
    if (node.computed) {
      this.evaluateExpression(node.property, state, rootScope, { asCallee: false });
    }
    if (asCallee) {
      this.escapeValue(state, receiver);
    }
    if (node.computed && node.property.type === 'Identifier') {
      this.decisions.set(node, this.isSafeValue(state, receiver));
    }
    return { receiver, name: getStaticMemberName(node) };
  }

  /**
   * @param {import('estree').SetPrototypeOfLike} node
   * @param {ProvenanceState} state
   * @param {import('eslint').Scope.Scope} rootScope
   */
  evaluateCallArguments(node, state, rootScope) {
    for (const argument of node.arguments) {
      if (argument.type === 'SpreadElement') {
        const value = this.evaluateExpression(argument.argument, state, rootScope, { asCallee: false });
        this.escapeValue(state, value);
      } else {
        const value = this.evaluateExpression(argument, state, rootScope, { asCallee: false });
        this.escapeValue(state, value);
      }
    }
  }

  /**
   * @param {import('estree').CallExpression | import('estree').NewExpression} node
   * @param {ProvenanceState} state
   * @param {import('eslint').Scope.Scope} rootScope
   * @returns {Set<number> | null}
   */
  evaluateCall(node, state, rootScope) {
    if (node.type === 'CallExpression' && this.isNullPrototypeFactoryCall(node)) {
      // Evaluate any property-descriptor arguments for side effects.
      for (let index = 1; index < node.arguments.length; index++) {
        this.evaluateExpression(node.arguments[index], state, rootScope, { asCallee: false });
      }
      return this.allocateNullPrototypeObject();
    }

    if (node.type === 'CallExpression' && this.isSetPrototypeOfCall(node)) {
      const target = this.evaluateExpression(node.arguments[0], state, rootScope, { asCallee: false });
      for (let index = 1; index < node.arguments.length; index++) {
        this.evaluateExpression(node.arguments[index], state, rootScope, { asCallee: false });
      }
      // The target object (through every alias) is no longer proven safe.
      this.escapeValue(state, target);
      return target;
    }

    const callee = node.callee;
    if (callee && callee.type === 'MemberExpression') {
      this.evaluateMember(callee, state, rootScope, true);
    } else {
      this.evaluateExpression(callee, state, rootScope, { asCallee: false });
    }
    this.evaluateCallArguments(node, state, rootScope);
    return UNKNOWN;
  }

  /**
   * @param {import('estree').AssignmentExpression} node
   * @param {ProvenanceState} state
   * @param {import('eslint').Scope.Scope} rootScope
   * @returns {Set<number> | null}
   */
  evaluateAssignment(node, state, rootScope) {
    const left = unwrapExpression(node.left);

    if (left.type === 'Identifier') {
      const value = node.operator === '=' ? this.evaluateExpression(node.right, state, rootScope, { asCallee: false }) : UNKNOWN;
      if (node.operator !== '=') {
        this.evaluateExpression(node.right, state, rootScope, { asCallee: false });
        this.writeIdentifier(left, state, rootScope, UNKNOWN);
      } else {
        this.writeIdentifier(left, state, rootScope, value);
      }
      return node.operator === '=' ? value : UNKNOWN;
    }

    if (left.type === 'MemberExpression') {
      const member = this.evaluateMember(left, state, rootScope, false);
      let rightValue = UNKNOWN;
      if (node.operator === '=') {
        rightValue = this.evaluateExpression(node.right, state, rootScope, { asCallee: false });
      } else {
        this.evaluateExpression(node.right, state, rootScope, { asCallee: false });
      }

      if (member.name === '__proto__') {
        this.escapeValue(state, member.receiver);
      }
      // The value is written into another object, so it leaves this proof.
      this.escapeValue(state, rightValue);
      return rightValue;
    }

    const value = this.evaluateExpression(node.right, state, rootScope, { asCallee: false });
    this.bindPattern(left, state, rootScope, UNKNOWN);
    return value;
  }

  /**
   * Evaluate an expression and return the set of tracked null-prototype
   * objects it may evaluate to (or `null` when unknown).
   * @param {import('estree').Node} node
   * @param {ProvenanceState} state
   * @param {import('eslint').Scope.Scope} rootScope
   * @param {{asCallee: boolean}} context
   * @returns {Set<number> | null}
   */
  evaluateExpression(node, state, rootScope, context) {
    if (!node) {
      return UNKNOWN;
    }

    switch (node.type) {
      case 'Identifier':
        return this.readIdentifier(node, state, rootScope);

      case 'MemberExpression': {
        const member = this.evaluateMember(node, state, rootScope, context.asCallee);
        return context.asCallee ? member.receiver : UNKNOWN;
      }

      case 'AssignmentExpression':
        return this.evaluateAssignment(node, state, rootScope);

      case 'CallExpression':
      case 'NewExpression':
        return this.evaluateCall(node, state, rootScope);

      case 'ConditionalExpression':
        return this.evaluateConditional(node, state, rootScope);

      case 'LogicalExpression':
        return this.evaluateLogical(node, state, rootScope);

      case 'SequenceExpression': {
        let value = UNKNOWN;
        for (const expression of node.expressions) {
          value = this.evaluateExpression(expression, state, rootScope, { asCallee: false });
        }
        return value;
      }

      case 'ParenthesizedExpression':
      case 'ChainExpression':
        return this.evaluateExpression(node.expression, state, rootScope, context);

      case 'UnaryExpression':
        this.evaluateExpression(node.argument, state, rootScope, { asCallee: false });
        return UNKNOWN;

      case 'UpdateExpression':
        if (node.argument.type === 'Identifier') {
          this.writeIdentifier(node.argument, state, rootScope, UNKNOWN);
        } else if (node.argument.type === 'MemberExpression') {
          this.evaluateMember(node.argument, state, rootScope, false);
        } else {
          this.evaluateExpression(node.argument, state, rootScope, { asCallee: false });
        }
        return UNKNOWN;

      case 'BinaryExpression':
        this.evaluateExpression(node.left, state, rootScope, { asCallee: false });
        this.evaluateExpression(node.right, state, rootScope, { asCallee: false });
        return UNKNOWN;

      case 'ArrayExpression':
        for (const element of node.elements) {
          if (!element) {
            continue;
          }
          if (element.type === 'SpreadElement') {
            this.evaluateExpression(element.argument, state, rootScope, { asCallee: false });
          } else {
            this.evaluateExpression(element, state, rootScope, { asCallee: false });
          }
        }
        return UNKNOWN;

      case 'ObjectExpression':
        for (const property of node.properties) {
          if (property.type === 'SpreadElement') {
            this.evaluateExpression(property.argument, state, rootScope, { asCallee: false });
          } else if (property.computed) {
            this.evaluateExpression(property.key, state, rootScope, { asCallee: false });
            this.evaluateExpression(property.value, state, rootScope, { asCallee: false });
          } else if (property.value) {
            this.evaluateExpression(property.value, state, rootScope, { asCallee: false });
          }
        }
        return UNKNOWN;

      case 'TemplateLiteral':
        for (const expression of node.expressions) {
          this.evaluateExpression(expression, state, rootScope, { asCallee: false });
        }
        return UNKNOWN;

      case 'TaggedTemplateExpression':
        this.evaluateExpression(node.tag, state, rootScope, { asCallee: false });
        for (const expression of node.quasi.expressions) {
          this.evaluateExpression(expression, state, rootScope, { asCallee: false });
        }
        return UNKNOWN;

      case 'AwaitExpression':
      case 'YieldExpression':
      case 'SpreadElement':
        if (node.argument) {
          this.evaluateExpression(node.argument, state, rootScope, { asCallee: false });
        }
        return UNKNOWN;

      case 'FunctionExpression':
      case 'ArrowFunctionExpression':
      case 'ClassExpression':
        // The body is analysed in its own scope; closure captures are
        // collected up front so the captured binding taints its objects.
        return UNKNOWN;

      case 'ImportExpression':
        this.evaluateExpression(node.source, state, rootScope, { asCallee: false });
        return UNKNOWN;

      case 'Literal':
      case 'ThisExpression':
      case 'Super':
      case 'MetaProperty':
      case 'PrivateIdentifier':
        return UNKNOWN;

      default:
        return UNKNOWN;
    }
  }

  /**
   * @param {import('estree').ConditionalExpression} node
   * @param {ProvenanceState} state
   * @param {import('eslint').Scope.Scope} rootScope
   * @returns {Set<number> | null}
   */
  evaluateConditional(node, state, rootScope) {
    this.evaluateExpression(node.test, state, rootScope, { asCallee: false });

    const base = this.cloneState(state);
    const consequentState = this.cloneState(base);
    const consequentValue = this.evaluateExpression(node.consequent, consequentState, rootScope, { asCallee: false });

    const alternateState = this.cloneState(base);
    const alternateValue = this.evaluateExpression(node.alternate, alternateState, rootScope, { asCallee: false });

    this.replaceState(state, this.mergeStates([consequentState, alternateState]));
    return this.unionExpressionValues(consequentValue, alternateValue);
  }

  /**
   * @param {import('estree').LogicalExpression} node
   * @param {ProvenanceState} state
   * @param {import('eslint').Scope.Scope} rootScope
   * @returns {Set<number> | null}
   */
  evaluateLogical(node, state, rootScope) {
    this.evaluateExpression(node.left, state, rootScope, { asCallee: false });
    const leftState = this.cloneState(state);
    const rightState = this.cloneState(state);
    this.evaluateExpression(node.right, rightState, rootScope, { asCallee: false });
    this.replaceState(state, this.mergeStates([leftState, rightState]));
    // Short-circuiting yields either operand; do not claim either one alone.
    return UNKNOWN;
  }
}

/**
 * Analyse a file and return a map from every dynamic member expression
 * (`object[identifier]`) to whether its receiver is proven to be a pristine
 * `Object.create(null)` dictionary at that program point.
 *
 * Only `true` entries are positive proofs; every other access must be
 * reported as usual.
 *
 * @param {import('eslint').SourceCode} sourceCode
 * @returns {Map<import('estree').MemberExpression, boolean>}
 */
function analyzeNullPrototypeProvenance(sourceCode) {
  return new NullPrototypeProvenance(sourceCode).run();
}

module.exports = { analyzeNullPrototypeProvenance };
