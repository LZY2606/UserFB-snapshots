import KernProcessor from './KernProcessor';
import UnicodeLayoutEngine from './UnicodeLayoutEngine';
import GlyphRun from './GlyphRun';
import GlyphPosition from './GlyphPosition';
import * as Script from './Script';
import AATLayoutEngine from '../aat/AATLayoutEngine';
import OTLayoutEngine from '../opentype/OTLayoutEngine';

export default class LayoutEngine {
  constructor(font) {
    this.font = font;
    this.unicodeLayoutEngine = null;
    this.kernProcessor = null;

    // Choose an advanced layout engine. We try the AAT morx table first since more
    // scripts are currently supported because the shaping logic is built into the font.
    if (this.font.morx) {
      this.engine = new AATLayoutEngine(this.font);
    } else if (this.font.GSUB || this.font.GPOS) {
      this.engine = new OTLayoutEngine(this.font);
    }
  }

  layout(string, features, script, language, direction) {
    // Make the features parameter optional
    if (typeof features === 'string') {
      direction = language;
      language = script;
      script = features;
      features = [];
    }

    return this._layout(string, features, script, language, direction, false);
  }

  // Like layout, but also records which UTF-16 code unit ranges of the input
  // string each output glyph comes from, exposed as glyphRun.sourceRanges.
  // Only plain cmap mapping and OpenType latn/arab shaping are supported;
  // anything else throws rather than returning an incomplete mapping.
  layoutWithSource(string, features, script, language, direction) {
    // Make the features parameter optional
    if (typeof features === 'string') {
      direction = language;
      language = script;
      script = features;
      features = [];
    }

    if (typeof string !== 'string') {
      throw new Error(
        'layoutWithSource requires a string; glyph arrays are not supported',
      );
    }

    // Reject lone surrogates, which have no well-defined source range.
    for (let i = 0; i < string.length; i++) {
      let code = string.charCodeAt(i);
      if (0xd800 <= code && code <= 0xdbff) {
        let next = string.charCodeAt(i + 1);
        if (!(0xdc00 <= next && next <= 0xdfff)) {
          throw new Error(
            'layoutWithSource does not support strings with lone surrogates',
          );
        }

        i++;
      } else if (0xdc00 <= code && code <= 0xdfff) {
        throw new Error(
          'layoutWithSource does not support strings with lone surrogates',
        );
      }
    }

    if (this.font.morx) {
      throw new Error('layoutWithSource does not support AAT (morx) fonts');
    }

    let resolvedScript = script;
    if (resolvedScript == null && string.length > 0) {
      resolvedScript = Script.forString(string);
    }

    if (
      string.length > 0 &&
      resolvedScript !== 'latn' &&
      resolvedScript !== 'arab'
    ) {
      throw new Error(
        `layoutWithSource does not support script '${resolvedScript}'; only 'latn' and 'arab' are supported`,
      );
    }

    return this._layout(string, features, script, language, direction, true);
  }

  _layout(string, features, script, language, direction, trackSource) {
    let ranges = null;

    // Map string to glyphs if needed
    if (typeof string === 'string') {
      // Attempt to detect the script from the string if not provided.
      if (script == null) {
        script = Script.forString(string);
      }

      var glyphs = trackSource
        ? this.font._glyphsForString(string, (ranges = []))
        : this.font.glyphsForString(string);
    } else {
      // Attempt to detect the script from the glyph code points if not provided.
      if (script == null) {
        let codePoints = [];
        for (let glyph of string) {
          codePoints.push(...glyph.codePoints);
        }

        script = Script.forCodePoints(codePoints);
      }

      var glyphs = string;
    }

    let glyphRun = new GlyphRun(glyphs, features, script, language, direction);

    // Return early if there are no glyphs
    if (glyphs.length === 0) {
      glyphRun.positions = [];
      if (trackSource) {
        glyphRun.sourceRanges = [];
      }

      return glyphRun;
    }

    if (trackSource) {
      // Each glyph starts out with a single source range from the cmap mapping.
      glyphRun.sourceRanges = ranges.map((range) => [range]);
    }

    // Setup the advanced layout engine
    if (this.engine && this.engine.setup) {
      this.engine.setup(glyphRun);
    }

    // Substitute and position the glyphs
    this.substitute(glyphRun);
    this.position(glyphRun);

    this.hideDefaultIgnorables(glyphRun.glyphs, glyphRun.positions);

    // Collect the final source ranges from the layout engine, which keeps
    // them on its per-run GlyphInfo objects (never on shared Glyph objects).
    if (trackSource && this.engine && this.engine.glyphInfos) {
      glyphRun.sourceRanges = this.engine.glyphInfos.map(
        (glyphInfo) => glyphInfo.sourceRanges,
      );
    }

    // Let the layout engine clean up any state it might have
    if (this.engine && this.engine.cleanup) {
      this.engine.cleanup();
    }

    return glyphRun;
  }

  substitute(glyphRun) {
    // Call the advanced layout engine to make substitutions
    if (this.engine && this.engine.substitute) {
      this.engine.substitute(glyphRun);
    }
  }

  position(glyphRun) {
    // Get initial glyph positions
    glyphRun.positions = glyphRun.glyphs.map(
      (glyph) => new GlyphPosition(glyph.advanceWidth),
    );
    let positioned = null;

    // Call the advanced layout engine. Returns the features applied.
    if (this.engine && this.engine.position) {
      positioned = this.engine.position(glyphRun);
    }

    // if there is no GPOS table, use unicode properties to position marks.
    if (!positioned && (!this.engine || this.engine.fallbackPosition)) {
      if (!this.unicodeLayoutEngine) {
        this.unicodeLayoutEngine = new UnicodeLayoutEngine(this.font);
      }

      this.unicodeLayoutEngine.positionGlyphs(
        glyphRun.glyphs,
        glyphRun.positions,
        this.engine ? 'ltr' : glyphRun.direction,
      );
    }

    // The advanced layout engines reverse right-to-left glyphs themselves.
    // Without one, reverse them here, before kerning, so that kerning pairs
    // are visual like in HarfBuzz.
    if (!this.engine && glyphRun.direction === 'rtl') {
      glyphRun.glyphs.reverse();
      glyphRun.positions.reverse();
      if (glyphRun.sourceRanges) {
        glyphRun.sourceRanges.reverse();
      }
    }

    // if kerning is not supported by GPOS, do kerning with the TrueType/AAT kern table
    if (
      (!positioned || !positioned.kern) &&
      glyphRun.features.kern !== false &&
      this.font.kern
    ) {
      if (!this.kernProcessor) {
        this.kernProcessor = new KernProcessor(this.font);
      }

      this.kernProcessor.process(glyphRun.glyphs, glyphRun.positions);
      glyphRun.features.kern = true;
    }
  }

  hideDefaultIgnorables(glyphs, positions) {
    let space = this.font.glyphForCodePoint(0x20);
    for (let i = 0; i < glyphs.length; i++) {
      let glyph = glyphs[i];
      let codePoint = glyph.codePoints[0];
      // Like HarfBuzz, keep the ones that GSUB substituted, e.g. a soft hyphen
      // replaced by its `case` form:
      if (
        this.isDefaultIgnorable(codePoint) &&
        glyph.id === this.font.glyphForCodePoint(codePoint).id
      ) {
        glyphs[i] = space;
        positions[i].xAdvance = 0;
        positions[i].yAdvance = 0;
      }
    }
  }

  isDefaultIgnorable(ch) {
    // From DerivedCoreProperties.txt in the Unicode database,
    // minus U+115F, U+1160, U+3164 and U+FFA0, which is what
    // Harfbuzz and Uniscribe do.
    let plane = ch >> 16;
    if (plane === 0) {
      // BMP
      switch (ch >> 8) {
        case 0x00:
          return ch === 0x00ad;
        case 0x03:
          return ch === 0x034f;
        case 0x06:
          return ch === 0x061c;
        case 0x17:
          return 0x17b4 <= ch && ch <= 0x17b5;
        case 0x18:
          return 0x180b <= ch && ch <= 0x180e;
        case 0x20:
          return (
            (0x200b <= ch && ch <= 0x200f) ||
            (0x202a <= ch && ch <= 0x202e) ||
            (0x2060 <= ch && ch <= 0x206f)
          );
        case 0xfe:
          return (0xfe00 <= ch && ch <= 0xfe0f) || ch === 0xfeff;
        case 0xff:
          return 0xfff0 <= ch && ch <= 0xfff8;
        default:
          return false;
      }
    } else {
      // Other planes
      switch (plane) {
        case 0x01:
          return (
            (0x1bca0 <= ch && ch <= 0x1bca3) || (0x1d173 <= ch && ch <= 0x1d17a)
          );
        case 0x0e:
          return 0xe0000 <= ch && ch <= 0xe0fff;
        default:
          return false;
      }
    }
  }

  getAvailableFeatures(script, language) {
    let features = [];

    if (this.engine) {
      features.push(...this.engine.getAvailableFeatures(script, language));
    }

    if (this.font.kern && features.indexOf('kern') === -1) {
      features.push('kern');
    }

    return features;
  }

  stringsForGlyph(gid) {
    let result = new Set();

    let codePoints = this.font._cmapProcessor.codePointsForGlyph(gid);
    for (let codePoint of codePoints) {
      result.add(String.fromCodePoint(codePoint));
    }

    if (this.engine && this.engine.stringsForGlyph) {
      for (let string of this.engine.stringsForGlyph(gid)) {
        result.add(string);
      }
    }

    return Array.from(result);
  }
}
