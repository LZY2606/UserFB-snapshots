/**
 * @module EffectRepresentation
 * Canonical representation of a clip effect in the Intermediate Timeline.
 */

import IdGenerator from '../utils/IdGenerator.js';

class EffectRepresentation {
  /**
   * @param {object} [data={}]
   * @param {string} [data.id]
   * @param {string} [data.type='']         - Interchange effect type identifier.
   * @param {boolean} [data.enabled=true]
   * @param {string} [data.videoForgeType=''] - Original VideoForge effect class name.
   * @param {object} [data.parameters={}]   - Key/value parameters (all values serializable).
   * @param {Array}  [data.keyframes=[]]    - Array of {time, property, value} keyframe objects.
   * @param {object} [data.metadata={}]
   */
  constructor(data = {}) {
    this.id = data.id ?? IdGenerator.generate('effect');
    this.type = data.type ?? '';
    this.enabled = data.enabled ?? true;
    this.videoForgeType = data.videoForgeType ?? '';

    /** @type {Map<string, *>} */
    this.parameters = data.parameters instanceof Map
      ? data.parameters
      : new Map(Object.entries(data.parameters ?? {}));

    /** @type {Array<{time:number, property:string, value:*}>} */
    this.keyframes = data.keyframes ?? [];

    this.metadata = data.metadata ?? {};
  }

  /**
   * Get a parameter value.
   * @param {string} key
   * @param {*} [defaultValue]
   * @returns {*}
   */
  getParam(key, defaultValue = undefined) {
    return this.parameters.has(key) ? this.parameters.get(key) : defaultValue;
  }

  /**
   * Set a parameter value.
   * @param {string} key
   * @param {*} value
   * @returns {EffectRepresentation}
   */
  setParam(key, value) {
    this.parameters.set(key, value);
    return this;
  }

  /**
   * Build from a VideoForge Effect instance.
   * @param {import('../effects/Effect.js').default} effect
   * @returns {EffectRepresentation}
   */
  static fromEffect(effect) {
    const params = {};
    if (effect.params && typeof effect.params === 'object') {
      Object.assign(params, effect.params);
    }
    // Common effect properties
    if (effect.duration != null) params.duration = effect.duration;
    if (effect.easing != null) params.easing = effect.easing;

    return new EffectRepresentation({
      id: effect.id,
      type: _mapEffectType(effect.type ?? effect.constructor.name),
      enabled: effect.enabled ?? true,
      videoForgeType: effect.type ?? effect.constructor.name,
      parameters: params,
    });
  }

  toJSON() {
    return {
      id: this.id,
      type: this.type,
      enabled: this.enabled,
      videoForgeType: this.videoForgeType,
      parameters: Object.fromEntries(this.parameters),
      keyframes: this.keyframes,
      metadata: this.metadata,
    };
  }
}

/** Map VideoForge effect type to a generic interchange name. */
function _mapEffectType(vfType) {
  const map = {
    fade:            'crossDissolve',
    fadein:          'dissolveIn',
    fadeout:         'dissolveOut',
    blur:            'gaussianBlur',
    colorgrade:      'lumetriColor',
    colorcorrection: 'lumetriColor',
    sharpen:         'sharpen',
    glow:            'glow',
    crop:            'crop',
    chromakey:       'chromaKey',
  };
  return map[String(vfType).toLowerCase()] ?? vfType;
}

export default EffectRepresentation;
