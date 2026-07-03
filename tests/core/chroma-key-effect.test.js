/**
 * @file chroma-key-effect.test.js
 * Unit tests for ChromaKeyEffect and its Clip integration.
 */

import { describe, test, expect } from '@jest/globals';
import Project from '../../src/core/Project.js';
import ChromaKeyEffect from '../../src/effects/ChromaKeyEffect.js';
import { EFFECT_TYPES } from '../../src/utils/Constants.js';

describe('ChromaKeyEffect', () => {
  test('defaults to green screen removal params', () => {
    const fx = new ChromaKeyEffect();
    expect(fx.type).toBe(EFFECT_TYPES.CHROMA_KEY);
    expect(fx.color).toBe('#00FF00');
    expect(fx.similarity).toBe(0.10);
    expect(fx.blend).toBe(0.10);
    expect(fx.yuv).toBe(false);
    expect(fx.enabled).toBe(true);
  });

  test('accepts custom params', () => {
    const fx = new ChromaKeyEffect({ color: '#0000FF', similarity: 0.25, blend: 0.05, yuv: true });
    expect(fx.color).toBe('#0000FF');
    expect(fx.similarity).toBe(0.25);
    expect(fx.blend).toBe(0.05);
    expect(fx.yuv).toBe(true);
  });

  test('accessors update params', () => {
    const fx = new ChromaKeyEffect();
    fx.color = '#FFFFFF';
    fx.similarity = 0.4;
    fx.blend = 0.2;
    fx.yuv = true;
    expect(fx.params).toEqual({ color: '#FFFFFF', similarity: 0.4, blend: 0.2, yuv: true });
  });

  test('rejects out-of-range similarity/blend', () => {
    const fx = new ChromaKeyEffect();
    expect(() => { fx.similarity = 1.5; }).toThrow(RangeError);
    expect(() => { fx.similarity = -0.1; }).toThrow(RangeError);
    expect(() => { fx.blend = 1.5; }).toThrow(RangeError);
    expect(() => { fx.blend = -0.1; }).toThrow(RangeError);
  });

  test('disabled effect is skipped by apply()', () => {
    const fx = new ChromaKeyEffect().disable();
    const context = { time: 0 };
    expect(fx.apply(context)).toBe(context);
  });

  test('enabled effect attaches chromaKey params via apply()', () => {
    const fx = new ChromaKeyEffect({ color: '#00FF00' });
    const result = fx.apply({ time: 0 });
    expect(result.chromaKey.color).toBe('#00FF00');
  });

  test('toJSON/fromJSON round-trips params', () => {
    const fx = new ChromaKeyEffect({ color: '#00FF00', similarity: 0.2, blend: 0.15, yuv: true });
    const json = fx.toJSON();
    const restored = ChromaKeyEffect.fromJSON(json);
    expect(restored.id).toBe(fx.id);
    expect(restored.enabled).toBe(fx.enabled);
    expect(restored.params).toEqual(fx.params);
  });
});

describe('Clip.addChromaKey()', () => {
  test('attaches a ChromaKeyEffect to the clip', () => {
    const p = new Project({ fps: 30 });
    const clip = p.addTrack('video').addVideo('/v.mp4', { inPoint: 0, outPoint: 10 });
    clip.addChromaKey({ color: '#00FF00' });

    expect(clip.effects).toHaveLength(1);
    expect(clip.effects[0]).toBeInstanceOf(ChromaKeyEffect);
    expect(clip.effects[0].type).toBe(EFFECT_TYPES.CHROMA_KEY);
  });

  test('is chainable', () => {
    const p = new Project({ fps: 30 });
    const clip = p.addTrack('video').addVideo('/v.mp4', { inPoint: 0, outPoint: 10 });
    expect(clip.addChromaKey()).toBe(clip);
  });
});
