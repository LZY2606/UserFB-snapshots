use crate::{
    characters::{Character, Characters},
    linebreak::LineBreak,
    words::VMetrics,
    BuiltInLineBreaker, FontId, GlyphPositioner, HorizontalAlign, Layout, LineBreaker,
    SectionGeometry, SectionGlyph, ToSectionText, VerticalAlign,
};
use ab_glyph::*;

const SOFT_HYPHEN: char = '\u{00AD}';

/// A [`GlyphPositioner`] created from a [`Layout`] via
/// [`Layout::with_soft_hyphens`](enum.Layout.html#method.with_soft_hyphens) that treats
/// the soft hyphen (`U+00AD`) as an optional invisible line break.
///
/// * When a soft hyphen is not selected as a line break it emits no glyph and takes
///   no width. Adjacent visible characters of the same font & scale are kerned as if
///   the soft hyphen was not there.
/// * When selected as a line break an ASCII `-` is drawn at the end of the line using
///   the soft hyphen's own text run font, scale & extra. The returned `SectionGlyph`
///   keeps the soft hyphen's original `section_index` & `byte_index`.
///
/// [`Layout::with_soft_hyphens`]: enum.Layout.html#method.with_soft_hyphens
#[derive(Debug, Clone, Copy, Hash, PartialEq, Eq)]
pub struct SoftHyphenLayout<L: LineBreaker> {
    layout: Layout<L>,
}

impl<L: LineBreaker> SoftHyphenLayout<L> {
    #[inline]
    pub(crate) fn new(layout: Layout<L>) -> Self {
        Self { layout }
    }

    /// Returns an identical `SoftHyphenLayout` but with the input `h_align`
    #[inline]
    pub fn h_align(self, h_align: HorizontalAlign) -> Self {
        Self {
            layout: self.layout.h_align(h_align),
        }
    }

    /// Returns an identical `SoftHyphenLayout` but with the input `v_align`
    #[inline]
    pub fn v_align(self, v_align: VerticalAlign) -> Self {
        Self {
            layout: self.layout.v_align(v_align),
        }
    }

    /// Returns an identical `SoftHyphenLayout` but with the input `line_breaker`
    #[inline]
    pub fn line_breaker<L2: LineBreaker>(self, line_breaker: L2) -> SoftHyphenLayout<L2> {
        SoftHyphenLayout {
            layout: self.layout.line_breaker(line_breaker),
        }
    }
}

impl Default for SoftHyphenLayout<BuiltInLineBreaker> {
    #[inline]
    fn default() -> Self {
        Layout::default_wrap().with_soft_hyphens()
    }
}

impl<L: LineBreaker> GlyphPositioner for SoftHyphenLayout<L> {
    fn calculate_glyphs<F, S>(
        &self,
        fonts: &[F],
        geometry: &SectionGeometry,
        sections: &[S],
    ) -> Vec<SectionGlyph>
    where
        F: Font,
        S: ToSectionText,
    {
        calculate(&self.layout, fonts, geometry, sections)
    }

    fn bounds_rect(&self, geometry: &SectionGeometry) -> Rect {
        self.layout.bounds_rect(geometry)
    }
}

/// Single character info needed by the soft-hyphen layout.
struct SoftChar<'b, F: Font> {
    character: char,
    glyph: Glyph,
    scale_font: PxScaleFont<&'b F>,
    scale: PxScale,
    font_id: FontId,
    /// Line break following this character.
    line_break: Option<LineBreak>,
    control: bool,
    whitespace: bool,
    section_index: usize,
    byte_index: usize,
}

impl<'b, F: Font> From<Character<'b, F>> for SoftChar<'b, F> {
    #[inline]
    fn from(c: Character<'b, F>) -> Self {
        Self {
            character: c.character,
            glyph: c.glyph,
            scale_font: c.scale_font,
            scale: c.scale,
            font_id: c.font_id,
            line_break: c.line_break,
            control: c.control,
            whitespace: c.whitespace,
            section_index: c.section_index,
            byte_index: c.byte_index,
        }
    }
}

#[derive(Clone, Copy)]
struct LastVisible {
    font_id: FontId,
    scale: PxScale,
    id: GlyphId,
}

struct HyphenGlyph {
    glyph: Glyph,
    font_id: FontId,
    section_index: usize,
    byte_index: usize,
}

struct Candidate {
    /// Index of the character at/after which the line breaks. The next line starts
    /// at `end_index + 1`.
    end_index: usize,
    /// Number of glyphs output on the line if this candidate is selected.
    glyph_len: usize,
    /// Width used for fitting/alignment when the soft hyphen is rendered.
    width: f32,
    /// Width used when the soft hyphen stays invisible.
    width_no_hyphen: f32,
    max_v_metrics: VMetrics,
    max_v_metrics_no_hyphen: VMetrics,
    hyphen: Option<HyphenGlyph>,
}

struct LineScan {
    glyphs: Vec<SectionGlyph>,
    caret: f32,
    caret_no_trail: f32,
    max_v_metrics: VMetrics,
    last_visible: Option<LastVisible>,
    has_non_space: bool,
    candidates: Vec<Candidate>,
}

impl Default for LineScan {
    fn default() -> Self {
        Self {
            glyphs: Vec::new(),
            caret: 0.0,
            caret_no_trail: 0.0,
            max_v_metrics: VMetrics::default(),
            last_visible: None,
            has_non_space: false,
            candidates: Vec::new(),
        }
    }
}

#[inline]
fn same_scale(a: PxScale, b: PxScale) -> bool {
    a.x == b.x && a.y == b.y
}

#[inline]
fn same_font_scale(a: &LastVisible, font_id: FontId, scale: PxScale) -> bool {
    a.font_id == font_id && same_scale(a.scale, scale)
}

/// True if there is some visible, non-soft-hyphen, non-empty content following
/// `index` before the next hard break. This stops a soft hyphen that is trailing or
/// adjacent to a hard break from rendering a stray hyphen.
fn has_following_content<F: Font>(chars: &[SoftChar<'_, F>], index: usize) -> bool {
    for item in &chars[index + 1..] {
        if item.character == SOFT_HYPHEN {
            continue;
        }
        if !item.control && !item.whitespace {
            return true;
        }
        if matches!(item.line_break, Some(LineBreak::Hard(_))) {
            return false;
        }
    }
    false
}

/// Scan one line from `start`, collecting all candidate break points up to the first
/// hard break (or end of input).
fn scan_line<'b, F: Font>(chars: &[SoftChar<'b, F>], start: usize) -> LineScan {
    let mut scan = LineScan::default();
    let mut i = start;
    let last_index = chars.len().saturating_sub(1);

    while i < chars.len() {
        let item = &chars[i];
        let is_soft_hyphen = item.character == SOFT_HYPHEN;

        if is_soft_hyphen {
            // Soft hyphens are never rendered directly. They only create an optional
            // break point.
            if scan.has_non_space && has_following_content(chars, i) {
                let hyphen_glyph = item.scale_font.scaled_glyph('-');
                let kern = match scan.last_visible {
                    // Glyph ids are only meaningful within the same font. The
                    // temporary hyphen is measured using its own run's scale.
                    Some(prev) if prev.font_id == item.font_id => {
                        item.scale_font.kern(prev.id, hyphen_glyph.id)
                    }
                    _ => 0.0,
                };
                let advance = item.scale_font.h_advance(hyphen_glyph.id);
                let hyphen_x = scan.caret + kern;

                let mut positioned_hyphen = hyphen_glyph;
                positioned_hyphen.position = point(hyphen_x, 0.0);
                let hyphen = HyphenGlyph {
                    glyph: positioned_hyphen,
                    font_id: item.font_id,
                    section_index: item.section_index,
                    byte_index: item.byte_index,
                };

                let hyphen_metrics: VMetrics = item.scale_font.into();
                let max_with_hyphen = scan.max_v_metrics.clone().max(hyphen_metrics);

                scan.candidates.push(Candidate {
                    end_index: i,
                    glyph_len: scan.glyphs.len(),
                    width: hyphen_x + advance,
                    width_no_hyphen: scan.caret,
                    max_v_metrics: max_with_hyphen,
                    max_v_metrics_no_hyphen: scan.max_v_metrics.clone(),
                    hyphen: Some(hyphen),
                });
            }

            if i == last_index {
                // Count the end of input as a hard break, without a hyphen.
                scan.candidates.push(Candidate {
                    end_index: i,
                    glyph_len: scan.glyphs.len(),
                    width: scan.caret,
                    width_no_hyphen: scan.caret,
                    max_v_metrics: scan.max_v_metrics.clone(),
                    max_v_metrics_no_hyphen: scan.max_v_metrics.clone(),
                    hyphen: None,
                });
                break;
            }

            // A U+00AD can never cause a hard break itself. Any adjacent hard-break
            // character is handled by that character, so no empty line is produced.
            i += 1;
            continue;
        }

        // Kerning is only valid between glyphs of the same font at the same scale.
        if let Some(prev) = scan.last_visible {
            if same_font_scale(&prev, item.font_id, item.scale) {
                scan.caret += item.scale_font.kern(prev.id, item.glyph.id);
            }
        }

        let glyph_metrics: VMetrics = item.scale_font.into();
        scan.max_v_metrics = scan.max_v_metrics.clone().max(glyph_metrics);

        if !item.control {
            let advance = item.scale_font.h_advance(item.glyph.id);
            let mut glyph = item.glyph.clone();
            glyph.position = point(scan.caret, 0.0);
            scan.glyphs.push(SectionGlyph {
                section_index: item.section_index,
                byte_index: item.byte_index,
                glyph,
                font_id: item.font_id,
            });
            scan.caret += advance;

            if !item.whitespace {
                scan.has_non_space = true;
                scan.caret_no_trail = scan.caret;
            }
        }

        scan.last_visible = Some(LastVisible {
            font_id: item.font_id,
            scale: item.scale,
            id: item.glyph.id,
        });

        let is_last = i == last_index;
        if let Some(LineBreak::Hard(_)) = item.line_break {
            scan.candidates.push(Candidate {
                end_index: i,
                glyph_len: scan.glyphs.len(),
                width: scan.caret,
                width_no_hyphen: scan.caret,
                max_v_metrics: scan.max_v_metrics.clone(),
                max_v_metrics_no_hyphen: scan.max_v_metrics.clone(),
                hyphen: None,
            });
            break;
        } else if is_last {
            scan.candidates.push(Candidate {
                end_index: i,
                glyph_len: scan.glyphs.len(),
                width: scan.caret,
                width_no_hyphen: scan.caret,
                max_v_metrics: scan.max_v_metrics.clone(),
                max_v_metrics_no_hyphen: scan.max_v_metrics.clone(),
                hyphen: None,
            });
            break;
        } else if matches!(item.line_break, Some(LineBreak::Soft(_))) {
            scan.candidates.push(Candidate {
                end_index: i,
                glyph_len: scan.glyphs.len(),
                width: scan.caret_no_trail,
                width_no_hyphen: scan.caret_no_trail,
                max_v_metrics: scan.max_v_metrics.clone(),
                max_v_metrics_no_hyphen: scan.max_v_metrics.clone(),
                hyphen: None,
            });
        }

        i += 1;
    }

    scan
}

#[inline]
fn within_bound(width: f32, bound: f32) -> bool {
    width < bound || approx::relative_eq!(width, bound)
}

/// Select the candidate to end the line with. Greedy chooses the furthest break
/// point whose rendered width fits. If none fits, the library's long-word overflow
/// strategy applies to the first non-empty candidate, except that a soft-hyphen
/// candidate whose unhyphenated prefix fits but whose `-` does not fit never widens
/// the line.
///
/// Returns the candidate index and whether its temporary `-` should be rendered.
fn select_candidate(
    candidates: &[Candidate],
    bound: f32,
    render_hyphen: bool,
) -> Option<(usize, bool)> {
    let mut chosen = None;
    for (i, c) in candidates.iter().enumerate() {
        let width = if render_hyphen {
            c.width
        } else {
            c.width_no_hyphen
        };
        if within_bound(width, bound) {
            chosen = Some((i, render_hyphen));
        }
    }

    if chosen.is_none() {
        let first = candidates
            .iter()
            .position(|c| c.glyph_len > 0)
            .or_else(|| (!candidates.is_empty()).then_some(0))?;
        let candidate = &candidates[first];
        // If the prefix fits without the temporary hyphen, break there without
        // stretching the line beyond its bounds.
        let render = render_hyphen
            && (candidate.hyphen.is_none() || !within_bound(candidate.width_no_hyphen, bound));
        chosen = Some((first, render));
    }

    chosen
}

fn align_line(
    mut glyphs: Vec<SectionGlyph>,
    rightmost: f32,
    line_height: f32,
    (screen_x, screen_y): (f32, f32),
    h_align: HorizontalAlign,
    v_align: VerticalAlign,
) -> Vec<SectionGlyph> {
    if glyphs.is_empty() {
        return Vec::new();
    }

    let screen_left = match h_align {
        HorizontalAlign::Left => point(screen_x, screen_y),
        HorizontalAlign::Center | HorizontalAlign::Right => {
            let mut shift_left = rightmost;
            if h_align == HorizontalAlign::Center {
                shift_left /= 2.0;
            }
            point(screen_x - shift_left, screen_y)
        }
    };

    let screen_pos = match v_align {
        VerticalAlign::Top => screen_left,
        VerticalAlign::Center => {
            let mut p = screen_left;
            p.y -= line_height / 2.0;
            p
        }
        VerticalAlign::Bottom => {
            let mut p = screen_left;
            p.y -= line_height;
            p
        }
    };

    glyphs
        .iter_mut()
        .for_each(|sg| sg.glyph.position += screen_pos);
    glyphs
}

fn calculate<L, F, S>(
    layout: &Layout<L>,
    fonts: &[F],
    geometry: &SectionGeometry,
    sections: &[S],
) -> Vec<SectionGlyph>
where
    L: LineBreaker,
    F: Font,
    S: ToSectionText,
{
    use crate::Layout::{SingleLine, Wrap};

    let SectionGeometry {
        screen_position,
        bounds: (bound_w, bound_h),
    } = *geometry;

    let (line_breaker, single_line, h_align, v_align) = match *layout {
        SingleLine {
            line_breaker,
            h_align,
            v_align,
        } => (line_breaker, true, h_align, v_align),
        Wrap {
            line_breaker,
            h_align,
            v_align,
        } => (line_breaker, false, h_align, v_align),
    };

    let chars: Vec<SoftChar<'_, F>> = Characters::new(
        fonts,
        sections.iter().map(|s| s.to_section_text()),
        line_breaker,
    )
    .map(SoftChar::from)
    .collect();

    if single_line {
        // A single line only hides the soft hyphen; truncation never renders a
        // temporary hyphen.
        let scan = scan_line(&chars, 0);
        let Some((candidate_index, _)) = select_candidate(&scan.candidates, bound_w, false) else {
            return Vec::new();
        };

        let LineScan {
            glyphs,
            mut candidates,
            ..
        } = scan;
        let candidate = candidates.swap_remove(candidate_index);

        let mut line_glyphs = glyphs;
        line_glyphs.truncate(candidate.glyph_len);
        let line_metrics = candidate.max_v_metrics_no_hyphen;
        for sg in &mut line_glyphs {
            sg.glyph.position.y = line_metrics.ascent;
        }

        return align_line(
            line_glyphs,
            candidate.width_no_hyphen,
            line_metrics.ascent - line_metrics.descent + line_metrics.line_gap,
            screen_position,
            h_align,
            v_align,
        );
    }

    let mut out = vec![];
    let mut caret = screen_position;
    let v_align_top = v_align == VerticalAlign::Top;
    let mut start = 0;

    while start < chars.len() {
        if v_align_top && caret.1 >= screen_position.1 + bound_h {
            break;
        }

        let scan = scan_line(&chars, start);
        let Some((candidate_index, render_hyphen)) =
            select_candidate(&scan.candidates, bound_w, true)
        else {
            break;
        };

        let LineScan {
            glyphs,
            mut candidates,
            ..
        } = scan;
        let candidate = candidates.swap_remove(candidate_index);

        let mut line_glyphs = glyphs;
        line_glyphs.truncate(candidate.glyph_len);
        let line_metrics = if render_hyphen {
            candidate.max_v_metrics
        } else {
            candidate.max_v_metrics_no_hyphen
        };
        let line_width = if render_hyphen {
            candidate.width
        } else {
            candidate.width_no_hyphen
        };
        if render_hyphen {
            if let Some(hyphen) = candidate.hyphen {
                line_glyphs.push(SectionGlyph {
                    section_index: hyphen.section_index,
                    byte_index: hyphen.byte_index,
                    glyph: hyphen.glyph,
                    font_id: hyphen.font_id,
                });
            }
        }
        for sg in &mut line_glyphs {
            sg.glyph.position.y = line_metrics.ascent;
        }

        out.extend(align_line(
            line_glyphs,
            line_width,
            0.0,
            caret,
            h_align,
            VerticalAlign::Top,
        ));
        caret.1 += line_metrics.ascent - line_metrics.descent + line_metrics.line_gap;

        let next_start = candidate.end_index + 1;
        start = if next_start > start {
            next_start
        } else {
            start + 1
        };
    }

    if !out.is_empty() && v_align != VerticalAlign::Top {
        let shift_up = if v_align == VerticalAlign::Center {
            (caret.1 - screen_position.1) / 2.0
        } else {
            caret.1 - screen_position.1
        };

        let (min_x, max_x) = h_align.x_bounds(screen_position.0, bound_w);
        let (min_y, max_y) = v_align.y_bounds(screen_position.1, bound_h);

        out = out
            .drain(..)
            .filter_map(|mut sg| {
                sg.glyph.position.y -= shift_up;

                let sfont = fonts[sg.font_id].as_scaled(sg.glyph.scale);
                let h_advance = sfont.h_advance(sg.glyph.id);
                let h_side_bearing = sfont.h_side_bearing(sg.glyph.id);
                let height = sfont.height();

                Some(sg).filter(|sg| {
                    sg.glyph.position.x - h_side_bearing <= max_x
                        && sg.glyph.position.x + h_advance >= min_x
                        && sg.glyph.position.y - height <= max_y
                        && sg.glyph.position.y + height >= min_y
                })
            })
            .collect();
    }

    out
}
