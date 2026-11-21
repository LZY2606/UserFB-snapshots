/**
 * VideoForge — JavaScript library for building video editors,
 * automation tools, and AI-powered editing systems.
 *
 * @module videoforge
 *
 * @example
 * import { Project, TRACK_TYPES } from 'videoforge';
 *
 * const project = new Project({ name: 'My Edit' });
 * const track   = project.addTrack('video');
 * const clip    = track.addVideo('./intro.mp4');
 *
 * clip.trim(5, 20).fadeIn(1);
 *
 * await project.export({ type: 'json', output: './project.vfp' });
 */

// ─── Core ────────────────────────────────────────────────────────────────────
export { default as Project }  from './core/Project.js';
export { default as Track }    from './core/Track.js';
export { default as Clip }     from './core/Clip.js';
export { default as Asset }    from './core/Asset.js';
export { default as Timeline } from './core/Timeline.js';

// ─── Clip types ───────────────────────────────────────────────────────────────
export { default as VideoClip } from './clips/VideoClip.js';
export { default as AudioClip } from './clips/AudioClip.js';
export { default as ImageClip } from './clips/ImageClip.js';
export { default as TextClip }  from './clips/TextClip.js';
export { default as ShapeClip } from './clips/ShapeClip.js';

// ─── Effects ──────────────────────────────────────────────────────────────────
export { default as Effect }     from './effects/Effect.js';
export { default as FadeEffect } from './effects/FadeEffect.js';
export { default as Transition } from './effects/Transition.js';
export { default as CropEffect } from './effects/CropEffect.js';
export { default as ChromaKeyEffect } from './effects/ChromaKeyEffect.js';

// ─── Exporters ────────────────────────────────────────────────────────────────
export { default as Exporter }            from './exporters/Exporter.js';
export { default as JsonExporter }        from './exporters/JsonExporter.js';
export { default as PremiereXmlExporter } from './exporters/PremiereXmlExporter.js';
export { default as FcpxmlExporter }      from './exporters/FcpxmlExporter.js';
export { default as EdlExporter }         from './exporters/EdlExporter.js';
export { default as Mp4Exporter }         from './exporters/Mp4Exporter.js';

// ─── Interchange (ITR) ────────────────────────────────────────────────────────
export { default as IntermediateTimeline, ITR_VERSION } from './interchange/IntermediateTimeline.js';
export { default as TimelineConverter }    from './interchange/TimelineConverter.js';
export { default as AssetReference }       from './interchange/AssetReference.js';
export { default as TrackRepresentation }  from './interchange/TrackRepresentation.js';
export { default as ClipRepresentation }   from './interchange/ClipRepresentation.js';
export { default as EffectRepresentation } from './interchange/EffectRepresentation.js';
export { default as TransitionRepresentation, TRANSITION_ALIGNMENT } from './interchange/TransitionRepresentation.js';
export { default as CaptionRepresentation } from './interchange/CaptionRepresentation.js';
export { default as InterchangeValidator } from './interchange/validation/InterchangeValidator.js';

// Interchange utilities
export { default as TimeCode, PREMIERE_TICKS_PER_SECOND, RATE_TABLE } from './interchange/utils/TimeCode.js';
export { default as XmlBuilder }            from './interchange/utils/XmlBuilder.js';
export { escapeText, escapeAttr, escapeUrl, unescapeText, cdata } from './interchange/utils/XmlEscaper.js';
export { default as XmlValidator }          from './interchange/utils/XmlValidator.js';
export { default as XmlNamespaceManager, VF_NAMESPACE, VF_PREFIX } from './interchange/utils/XmlNamespaceManager.js';

// ─── Preview ─────────────────────────────────────────────────────────────────
export { default as PreviewPlayer }   from './preview/PreviewPlayer.js';
export { default as PreviewRenderer } from './preview/PreviewRenderer.js';
export { default as CpuBackend }      from './preview/CpuBackend.js';

// ─── Caption & Motion Typography Engine ──────────────────────────────────────

// Core caption classes
export { default as CaptionClip }      from './captions/CaptionClip.js';
export { default as CaptionSegment }   from './captions/CaptionSegment.js';
export { default as CaptionWord }      from './captions/CaptionWord.js';
export { default as CaptionCharacter } from './captions/CaptionCharacter.js';
export { default as CaptionStyle }     from './captions/CaptionStyle.js';
export { default as CaptionLayout }    from './captions/CaptionLayout.js';

// Keyframe engine
export {
  default as CaptionKeyframe,
  KeyframeTrack,
  KeyframeSet,
  KEYFRAMEABLE_PROPERTIES,
  CAPTION_EASING,
  computeEasing,
  lerp,
} from './captions/CaptionKeyframe.js';

// Animation hierarchy (base + all 22 types)
export {
  default as CaptionAnimation,
  FadeAnimation, SlideAnimation, ScaleAnimation, RotateAnimation,
  BounceAnimation, PopAnimation, PulseAnimation, ShakeAnimation,
  WobbleAnimation, WaveAnimation, SwingAnimation, FlipAnimation,
  TypewriterAnimation, KaraokeAnimation, RevealAnimation, ScrambleAnimation,
  ElasticAnimation, GlitchAnimation, HighlightAnimation, ZoomAnimation,
  BlurRevealAnimation, StaggerAnimation,
  ANIMATION_TYPES, ANIMATION_TARGET, STAGGER_ORDER, ANIMATION_REGISTRY,
} from './captions/CaptionAnimation.js';

// Effect hierarchy (base + all 19 types)
export {
  default as CaptionEffect,
  GlowEffect, ShadowEffect, OutlineEffect, GradientEffect, NeonEffect,
  GlassEffect, BlurEffect, MotionBlurEffect, BackgroundBoxEffect,
  RoundedBoxEffect, HighlightEffect, UnderlineEffect, StrikeThroughEffect,
  NoiseEffect, GrainEffect, ChromaticAberrationEffect, BloomEffect,
  DistortionEffect, ReflectionEffect,
  EFFECT_TYPES as CAPTION_EFFECT_TYPES, EFFECT_REGISTRY,
} from './captions/CaptionEffect.js';

// Presets (base + 10 built-in presets)
export {
  default as CaptionPreset,
  HormoziPreset, MrBeastPreset, PodcastPreset, NewsPreset,
  DocumentaryPreset, KaraokePreset, MinimalPreset, GamingPreset,
  LuxuryPreset, CorporatePreset,
  PRESET_REGISTRY, createPreset,
} from './captions/CaptionPreset.js';

// Layout constants
export { WRAP_MODE, ANCHOR_POINT, SOCIAL_SAFE_ZONES } from './captions/CaptionLayout.js';

// Rendering interfaces
export { default as CaptionRenderer }       from './captions/CaptionRenderer.js';
export { default as MotionTypographyEngine, SEGMENTATION_STRATEGY, TIMING_FORMAT } from './captions/MotionTypographyEngine.js';

// ─── Utilities ────────────────────────────────────────────────────────────────
export { default as IdGenerator } from './utils/IdGenerator.js';
export { resolveFps, resolveWidth, resolveHeight, resolveSampleRate, resolveChannels, resolveSequenceParams } from './utils/FpsResolver.js';

export {
  TRACK_TYPES,
  CLIP_TYPES,
  ASSET_TYPES,
  EFFECT_TYPES,
  TRANSITION_TYPES,
  EXPORT_TYPES,
  TEXT_ALIGN,
  SHAPE_TYPES,
  PLAYER_STATE,
  EASING,
  DEFAULTS,
  CROP_ALIGNMENT,
} from './utils/Constants.js';
