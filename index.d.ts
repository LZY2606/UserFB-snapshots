type FocusableElement = HTMLElement | SVGElement;

export type CheckOptions = {
  displayCheck?:
    'full' | 'full-native' | 'legacy-full' | 'non-zero-area' | 'none';
  getShadowRoot?:
    boolean | ((node: FocusableElement) => ShadowRoot | boolean | undefined);
};

export type TabbableOptions = {
  includeContainer?: boolean;
};

export type PointerPoint = {
  x: number;
  y: number;
};

export type PointerReachableEntry = {
  node: FocusableElement;
  point: PointerPoint;
};

export type PointerReachableOptions = TabbableOptions & CheckOptions;

export declare function tabbable(
  container: Element,
  options?: TabbableOptions & CheckOptions
): FocusableElement[];

export declare function focusable(
  container: Element,
  options?: TabbableOptions & CheckOptions
): FocusableElement[];

/**
 * Returns the focusable candidates within `container` that are verifiably
 * reachable by a pointer, each paired with the concrete viewport point whose
 * hit-testing resolves to the candidate itself or to one of its composed
 * descendants. Candidate selection and ordering follow `focusable()`; the page
 * is never scrolled, focused, clicked, or mutated.
 */
export declare function pointerReachable(
  container: Element,
  options?: PointerReachableOptions
): PointerReachableEntry[];

export declare function isTabbable(
  node: Element,
  options?: CheckOptions
): boolean;

export declare function isFocusable(
  node: Element,
  options?: CheckOptions
): boolean;

export declare function getTabIndex(node: Element): number;
