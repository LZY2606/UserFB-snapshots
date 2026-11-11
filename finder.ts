// License: MIT
// Author: Anton Medvedev <anton@medv.io>
// Source: https://github.com/antonmedv/finder

type Knot = {
  name: string
  penalty: number
  level?: number
}

const acceptedAttrNames = new Set(['role', 'name', 'aria-label', 'rel', 'href'])

/** Check if attribute name and value are word-like. */
export function attr(name: string, value: string): boolean {
  let nameIsOk = acceptedAttrNames.has(name)
  nameIsOk ||= name.startsWith('data-') && wordLike(name)

  let valueIsOk = wordLike(value) && value.length < 100
  valueIsOk ||= value.startsWith('#') && wordLike(value.slice(1))

  return nameIsOk && valueIsOk
}

/** Check if id name is word-like. */
export function idName(name: string): boolean {
  return wordLike(name)
}

/** Check if class name is word-like. */
export function className(name: string): boolean {
  return wordLike(name)
}

/** Check if tag name is word-like. */
export function tagName(name: string): boolean {
  return true
}

/** Configuration options for the finder. */
export type Options = {
  /** The root element to start the search from. */
  root: Element
  /** Function that determines if an id name may be used in a selector. */
  idName: (name: string) => boolean
  /** Function that determines if a class name may be used in a selector. */
  className: (name: string) => boolean
  /** Function that determines if a tag name may be used in a selector. */
  tagName: (name: string) => boolean
  /** Function that determines if an attribute may be used in a selector. */
  attr: (name: string, value: string) => boolean
  /** Timeout to search for a selector. */
  timeoutMs: number
  /** Minimum length of levels in fining selector. */
  seedMinLength: number
  /** Minimum length for optimising selector. */
  optimizedMinLength: number
  /** Maximum number of path checks. */
  maxNumberOfPathChecks: number
}

/** Finds unique CSS selectors for the given element. */
export function finder(input: Element, options?: Partial<Options>): string {
  if (input.nodeType !== Node.ELEMENT_NODE) {
    throw new Error(`Can't generate CSS selector for non-element node type.`)
  }
  if (input.tagName.toLowerCase() === 'html') {
    return 'html'
  }
  const defaults: Options = {
    root: document.body,
    idName: idName,
    className: className,
    tagName: tagName,
    attr: attr,
    timeoutMs: 1000,
    seedMinLength: 3,
    optimizedMinLength: 2,
    maxNumberOfPathChecks: Infinity,
  }

  const startTime = new Date()
  const config = { ...defaults, ...options }
  const rootDocument = findRootDocument(config.root, defaults, input)

  return findUniqueSelector(input, config, rootDocument, startTime)
}

/**
 * Finds a unique CSS selector for `input` inside `rootDocument`, applying the
 * same id/class/tag/attr filtering, search order and positional fallback as
 * {@link finder}. `rootDocument` scopes every uniqueness check, so a selector
 * that is only unique with help from outside the root is never accepted.
 */
function findUniqueSelector(
  input: Element,
  config: Options,
  rootDocument: Element | Document | ShadowRoot,
  startTime: Date = new Date(),
): string {
  let foundPath: Knot[] | undefined
  let count = 0
  for (const candidate of search(input, config, rootDocument)) {
    const elapsedTimeMs = new Date().getTime() - startTime.getTime()
    if (
      elapsedTimeMs > config.timeoutMs ||
      count >= config.maxNumberOfPathChecks
    ) {
      const fPath = fallback(input, rootDocument)
      if (!fPath) {
        throw new Error(
          `Timeout: Can't find a unique selector after ${config.timeoutMs}ms`,
        )
      }
      return selector(fPath)
    }
    count++
    if (unique(candidate, rootDocument)) {
      foundPath = candidate
      break
    }
  }

  if (!foundPath) {
    throw new Error(`Selector was not found.`)
  }

  const optimized = [
    ...optimize(foundPath, input, config, rootDocument, startTime),
  ]
  optimized.sort(byPenalty)
  if (optimized.length > 0) {
    return selector(optimized[0])
  }
  return selector(foundPath)
}

/** A node that can serve as the start (root) of a {@link finderPath} path. */
export type PathRoot = Document | Element | ShadowRoot

/** Options for {@link finderPath}: {@link Options}, but with a widened root. */
export type PathOptions = Partial<Omit<Options, 'root'>> & { root?: PathRoot }

/**
 * Builds a cross shadow DOM path from `options.root` (or the current document
 * when omitted) to `input`.
 *
 * The returned array holds one CSS selector per tree on the way. Every segment
 * except the last must match exactly one shadow host inside the current root;
 * replay then descends into that host's open shadow root. The last segment
 * matches exactly the target inside its own root. No shadow-piercing syntax is
 * emitted and each segment is scoped to its own root, so elements with the same
 * selector in different components stay distinguishable.
 *
 * The id/class/tag/attribute filtering rules and the positional
 * (`nth-of-type`/`nth-child`) fallback are the same ones {@link finder} uses,
 * so a host without a usable name is still located by its position.
 *
 * @returns the selector segments from the root to the target; an empty array
 * when the root itself is the target.
 * @throws when `input` is not an element reachable from the root (it is
 * detached, lives in a different tree, or is only reachable through a closed
 * shadow root).
 */
export function finderPath(input: Element, options?: PathOptions): string[] {
  if (input.nodeType !== Node.ELEMENT_NODE) {
    throw new Error(`Can't generate CSS selector for non-element node type.`)
  }

  const defaults: Options = {
    root: document.body,
    idName: idName,
    className: className,
    tagName: tagName,
    attr: attr,
    timeoutMs: 1000,
    seedMinLength: 3,
    optimizedMinLength: 2,
    maxNumberOfPathChecks: Infinity,
  }
  const config: Options = { ...defaults, ...options, root: defaults.root }
  const start: PathRoot = options?.root ?? document

  // Walk the real DOM tree from the target up to the requested root. The walk
  // follows parentElement inside one tree and, at a shadow boundary, the open
  // host; slotted light DOM keeps its light-tree parent, so assignedSlot is
  // never treated as a parent.
  const segments: { root: PathRoot; target: Element }[] = []
  let current: Element = input
  // Bound the loop defensively in case of a pathological node chain.
  for (let guard = 0; guard < 1_000_000; guard++) {
    if (current === start) {
      // Reaching an element start by crossing out of its own shadow root would
      // produce a path replay cannot follow: an element's querySelectorAll does
      // not pierce its own shadow root, so its shadowed content is out of reach
      // from the element itself. Callers must pass the shadow root instead.
      const innermost = segments[segments.length - 1]
      if (
        innermost &&
        isShadowRoot(innermost.root) &&
        innermost.root.host === start
      ) {
        throw new Error(
          `Can't build a path: the target lives inside the root element's own shadow root; pass that shadow root as the root.`,
        )
      }
      return buildSegmentSelectors(segments.reverse(), config)
    }

    const rootNode = current.getRootNode?.()
    if (!isContainer(rootNode)) {
      throw new Error(
        `Can't build a path: the target is not reachable from the given root.`,
      )
    }

    // The requested start is the root node of the tree the target lives in
    // (a document or a shadow root): the segment is scoped directly to it.
    if (rootNode === start) {
      segments.push({ root: start, target: current })
      return buildSegmentSelectors(segments.reverse(), config)
    }

    // An element start inside the very same tree: walk the light parent chain
    // and scope the final segment to the start element itself.
    if (
      start.nodeType === Node.ELEMENT_NODE &&
      (start as Element).getRootNode?.() === rootNode &&
      isAncestor(start as Element, current)
    ) {
      segments.push({ root: start, target: current })
      return buildSegmentSelectors(segments.reverse(), config)
    }

    if (!isShadowRoot(rootNode)) {
      // Reached the top of a tree without finding the start: the target is
      // detached or lives in another tree.
      throw new Error(
        `Can't build a path: the target is detached from or outside the given root.`,
      )
    }

    const host = rootNode.host
    if (!host || !host.shadowRoot) {
      // The only way across this boundary is a closed shadow root.
      throw new Error(`Can't build a path through a closed shadow root.`)
    }
    segments.push({ root: rootNode, target: current })
    current = host
  }
  throw new Error(`Can't build a path: element chain is too deep.`)
}

/** Checks that `ancestor` is `node` itself or one of its light-tree ancestors. */
function isAncestor(ancestor: Element, node: Element): boolean {
  let current: Element | null = node
  while (current) {
    if (current === ancestor) {
      return true
    }
    current = current.parentElement
  }
  return false
}

/** Generates and validates one unique selector per collected tree segment. */
function buildSegmentSelectors(
  segments: { root: PathRoot; target: Element }[],
  config: Options,
): string[] {
  const selectors: string[] = []
  // `segments` is already ordered from the start root down to the target.
  for (const segment of segments) {
    const css = findUniqueSelector(
      segment.target,
      config,
      segment.root,
      new Date(),
    )
    // findUniqueSelector already proves uniqueness, but re-check explicitly so
    // a stored segment can never silently match the wrong node on replay.
    const matches = segment.root.querySelectorAll(css)
    if (matches.length !== 1 || matches[0] !== segment.target) {
      throw new Error(
        `Can't build a unique selector inside this shadow root: ${css}`,
      )
    }
    selectors.push(css)
  }
  return selectors
}

/**
 * Replays a path produced by {@link finderPath}, starting from `root`.
 *
 * Each segment must match exactly one node in the current root. Between
 * segments the matched host must expose an open shadow root, which becomes the
 * root for the next segment.
 *
 * @returns the resolved element (or `root` itself for an empty path), or `null`
 * when any segment matches zero or more than one node, is not valid CSS, or an
 * intermediate host has no open shadow root.
 */
export function resolvePath(root: PathRoot, path: string[]): PathRoot | null {
  let current: PathRoot = root
  for (let i = 0; i < path.length; i++) {
    let matches: NodeListOf<Element>
    try {
      matches = current.querySelectorAll(path[i])
    } catch {
      // Invalid CSS selector.
      return null
    }
    if (matches.length !== 1) {
      return null
    }
    const node = matches[0]
    if (i === path.length - 1) {
      return node
    }
    const shadow = node.shadowRoot
    if (!shadow) {
      return null
    }
    current = shadow
  }
  // An empty path resolves to the original start.
  return root
}

function isContainer(node: unknown): node is PathRoot {
  if (!node || typeof node !== 'object') {
    return false
  }
  const nodeType = (node as Node).nodeType
  return (
    nodeType === Node.DOCUMENT_NODE ||
    nodeType === Node.ELEMENT_NODE ||
    nodeType === Node.DOCUMENT_FRAGMENT_NODE
  )
}

function isShadowRoot(node: PathRoot): node is ShadowRoot {
  // Match the codebase's existing constructor-name check so this works without
  // relying on a global ShadowRoot constructor (e.g. jsdom test globals).
  return (
    node.nodeType === Node.DOCUMENT_FRAGMENT_NODE &&
    (node as ShadowRoot).constructor?.name === 'ShadowRoot'
  )
}

function* search(
  input: Element,
  config: Options,
  rootDocument: Element | Document | ShadowRoot,
): Generator<Knot[]> {
  const stack: Knot[][] = []
  let paths: Knot[][] = []
  let current: Element | null = input
  let i = 0
  while (current && current !== rootDocument) {
    const level = tie(current, config)
    for (const node of level) {
      node.level = i
    }
    stack.push(level)
    current = current.parentElement
    i++

    paths.push(...combinations(stack))

    if (i >= config.seedMinLength) {
      paths.sort(byPenalty)
      for (const candidate of paths) {
        yield candidate
      }
      paths = []
    }
  }

  paths.sort(byPenalty)
  for (const candidate of paths) {
    yield candidate
  }
}

function wordLike(name: string): boolean {
  if (/^[a-z\-]{3,}$/i.test(name)) {
    const words = name.split(/-|[A-Z]/)
    for (const word of words) {
      if (word.length <= 2) {
        return false
      }
      if (/[^aeiou]{4,}/i.test(word)) {
        return false
      }
    }
    return true
  }
  return false
}

function tie(element: Element, config: Options): Knot[] {
  const level: Knot[] = []

  const elementId = element.getAttribute('id')
  if (elementId && config.idName(elementId)) {
    level.push({
      name: '#' + CSS.escape(elementId),
      penalty: 0,
    })
  }

  for (let i = 0; i < element.classList.length; i++) {
    const name = element.classList[i]
    if (config.className(name)) {
      level.push({
        name: '.' + CSS.escape(name),
        penalty: 1,
      })
    }
  }

  for (let i = 0; i < element.attributes.length; i++) {
    const attr = element.attributes[i]
    if (config.attr(attr.name, attr.value)) {
      level.push({
        name: `[${CSS.escape(attr.name)}="${CSS.escape(attr.value)}"]`,
        penalty: 2,
      })
    }
  }

  const tagName = element.tagName.toLowerCase()
  if (config.tagName(tagName)) {
    level.push({
      name: tagName,
      penalty: 5,
    })

    const index = indexOf(element, tagName)
    if (index !== undefined) {
      level.push({
        name: nthOfType(tagName, index),
        penalty: 10,
      })
    }
  }

  const nth = indexOf(element)
  if (nth !== undefined) {
    level.push({
      name: nthChild(tagName, nth),
      penalty: 50,
    })
  }

  return level
}

function selector(path: Knot[]): string {
  let node = path[0]
  let query = node.name
  for (let i = 1; i < path.length; i++) {
    const level = path[i].level || 0
    if (node.level === level - 1) {
      query = `${path[i].name} > ${query}`
    } else {
      query = `${path[i].name} ${query}`
    }
    node = path[i]
  }
  return query
}

function penalty(path: Knot[]): number {
  return path.map((node) => node.penalty).reduce((acc, i) => acc + i, 0)
}

function byPenalty(a: Knot[], b: Knot[]) {
  return penalty(a) - penalty(b)
}

function indexOf(input: Element, tagName?: string): number | undefined {
  const parent = input.parentNode
  if (!parent) {
    return undefined
  }
  let child = parent.firstChild
  if (!child) {
    return undefined
  }
  let i = 0
  while (child) {
    if (
      child.nodeType === Node.ELEMENT_NODE &&
      (tagName === undefined ||
        (child as Element).tagName.toLowerCase() === tagName)
    ) {
      i++
    }
    if (child === input) {
      break
    }
    child = child.nextSibling
  }
  return i
}

function fallback(
  input: Element,
  rootDocument: Element | Document | ShadowRoot,
) {
  let i = 0
  let current: Element | null = input
  const path: Knot[] = []
  while (current && current !== rootDocument) {
    const tagName = current.tagName.toLowerCase()
    const index = indexOf(current, tagName)
    if (index === undefined) {
      return
    }
    path.push({
      name: nthOfType(tagName, index),
      penalty: NaN,
      level: i,
    })
    current = current.parentElement
    i++
  }
  if (unique(path, rootDocument)) {
    return path
  }
}

function nthChild(tagName: string, index: number) {
  if (tagName === 'html') {
    return 'html'
  }
  return `${tagName}:nth-child(${index})`
}

function nthOfType(tagName: string, index: number) {
  if (tagName === 'html') {
    return 'html'
  }
  return `${tagName}:nth-of-type(${index})`
}

function* combinations(stack: Knot[][], path: Knot[] = []): Generator<Knot[]> {
  if (stack.length > 0) {
    for (let node of stack[0]) {
      yield* combinations(stack.slice(1, stack.length), path.concat(node))
    }
  } else {
    yield path
  }
}

function findRootDocument(
  rootNode: Element | Document,
  defaults: Options,
  input: Element,
) {
  const shadowRoot = input.getRootNode?.()
  if (shadowRoot?.constructor?.name === 'ShadowRoot') {
    return shadowRoot as ShadowRoot
  }
  if (rootNode.nodeType === Node.DOCUMENT_NODE) {
    return rootNode
  }
  if (rootNode === defaults.root) {
    return rootNode.ownerDocument as Document
  }
  return rootNode
}

function unique(path: Knot[], rootDocument: Element | Document | ShadowRoot) {
  const css = selector(path)
  switch (rootDocument.querySelectorAll(css).length) {
    case 0:
      throw new Error(`Can't select any node with this selector: ${css}`)
    case 1:
      return true
    default:
      return false
  }
}

function* optimize(
  path: Knot[],
  input: Element,
  config: Options,
  rootDocument: Element | Document | ShadowRoot,
  startTime: Date,
): Generator<Knot[]> {
  if (path.length > 2 && path.length > config.optimizedMinLength) {
    for (let i = 1; i < path.length - 1; i++) {
      const elapsedTimeMs = new Date().getTime() - startTime.getTime()
      if (elapsedTimeMs > config.timeoutMs) {
        return
      }
      const newPath = [...path]
      newPath.splice(i, 1)
      if (
        unique(newPath, rootDocument) &&
        rootDocument.querySelector(selector(newPath)) === input
      ) {
        yield newPath
        yield* optimize(newPath, input, config, rootDocument, startTime)
      }
    }
  }
}
