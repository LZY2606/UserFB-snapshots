# Geometric color-font input

The 968-byte font and its generator are original MIT-licensed test material, not derived from a real font. `node colrcheck/make-font.mjs --check` compares all bytes; without `--check` it regenerates the TTF and table/node manifest. Requires Node.js >=18 built-ins only. No npm packages, system fonts, submodules or network services are used.

Glyph 2 (`@`, `d`, `u`) is an 800-unit square; glyph 3 is its inset square; glyph 4 is a half-width square. Glyphs 1 (`A`), 5, 6 and 7 have empty glyf data and COLRv1 roots. Root 1 has solid red with a half-alpha blue inset. Root 5 has a three-point linear gradient; root 6 references it twice under distinct affine contexts; root 7 combines an inset gradient with a half-alpha square using SRC_IN. Two CPAL v0 palettes include different RGB and alpha entries. `font-manifest.json` lists relative COLR node offsets and absolute sfnt table offsets.

`baseline.c` calls existing stb_truetype APIs and reports the old monochrome behavior. It contains no COLRv1 evaluator. `check-old.sh` executes the upstream TrueType sample (including SDF, atlas and bitmap calls) with this font, then compiles the original sample in both C99 and C++11 declaration modes. This is an existing example smoke check, not a claim that upstream has a comprehensive TrueType assertion suite.

The requested color API, contract tests and preview example are intentionally absent from the starting tree. The target runner rejects missing tests, no executed checks and skip output.
