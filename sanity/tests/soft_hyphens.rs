use glyph_brush::{ab_glyph::*, *};

const SH: char = '\u{00AD}';

#[derive(Debug, Clone, Copy, PartialEq)]
struct G {
    c: char,
    font: usize,
    scale: f32,
    section: usize,
    byte: usize,
}

fn close(a: f32, b: f32) -> bool {
    (a - b).abs() < 0.01
}

fn metrics(fonts: &[FontArc], g: G) -> (f32, f32, f32) {
    let f = fonts[g.font].as_scaled(PxScale::from(g.scale));
    (f.ascent(), f.descent(), f.line_gap())
}

fn height(m: (f32, f32, f32)) -> f32 {
    m.0 - m.1 + m.2
}

/// The line's VMetrics: maximum by height, keeping the first on ties, as the
/// layout crate does.
fn line_metrics(fonts: &[FontArc], specs: &[G]) -> (f32, f32, f32) {
    let mut best = (0.0, 0.0, 0.0);
    let mut any = false;
    for &g in specs {
        let m = metrics(fonts, g);
        if !any || height(m) > height(best) {
            best = m;
            any = true;
        }
    }
    best
}

fn kern(fonts: &[FontArc], prev: G, next: G) -> f32 {
    if prev.font == next.font && prev.scale == next.scale {
        let sf = fonts[next.font].as_scaled(PxScale::from(next.scale));
        sf.kern(
            fonts[prev.font].glyph_id(prev.c),
            fonts[next.font].glyph_id(next.c),
        )
    } else {
        0.0
    }
}

fn advance(fonts: &[FontArc], g: G) -> f32 {
    let sf = fonts[g.font].as_scaled(PxScale::from(g.scale));
    sf.h_advance(fonts[g.font].glyph_id(g.c))
}

/// Width of a contiguous word, kerning between adjacent glyphs of the same
/// font & scale.
fn word_width(fonts: &[FontArc], specs: &[G]) -> f32 {
    let mut width = 0.0;
    let mut prev = None;
    for &g in specs {
        if let Some(p) = prev {
            width += kern(fonts, p, g);
        }
        width += advance(fonts, g);
        prev = Some(g);
    }
    width
}

struct ExpectedLine {
    glyphs: Vec<(G, f32)>,
    /// Width used for alignment: trailing whitespace is excluded unless the
    /// line ends on a hard break.
    width: f32,
    metrics: (f32, f32, f32),
}

/// Independently lay out one line from the glyphs selected for it.
///
/// `groups` are words separated by normal line-break opportunities: kerning
/// never crosses a group boundary.
fn expected_line(fonts: &[FontArc], groups: &[Vec<G>], include_trailing: bool) -> ExpectedLine {
    let mut caret = 0.0;
    let mut caret_no_trail = 0.0;
    let mut glyphs = Vec::new();
    let mut all_specs = Vec::new();

    for group in groups {
        let mut prev: Option<G> = None;
        for &g in group {
            if let Some(p) = prev {
                caret += kern(fonts, p, g);
            }
            glyphs.push((g, caret));
            caret += advance(fonts, g);
            if !g.c.is_whitespace() {
                caret_no_trail = caret;
            }
            all_specs.push(g);
            prev = Some(g);
        }
    }

    ExpectedLine {
        glyphs,
        width: if include_trailing {
            caret
        } else {
            caret_no_trail
        },
        metrics: line_metrics(fonts, &all_specs),
    }
}

fn assert_line(
    label: &str,
    actual: &[SectionGlyph],
    expected: &ExpectedLine,
    line_top: f32,
    screen_x: f32,
    h_align: HorizontalAlign,
    fonts: &[FontArc],
) {
    assert_eq!(actual.len(), expected.glyphs.len(), "{label}: glyph count");
    let y = line_top + expected.metrics.0;
    let x_shift = match h_align {
        HorizontalAlign::Left => 0.0,
        HorizontalAlign::Center => screen_x - expected.width / 2.0,
        HorizontalAlign::Right => screen_x - expected.width,
    };

    for (sg, (g, expected_x)) in actual.iter().zip(&expected.glyphs) {
        assert_eq!(sg.font_id, FontId(g.font), "{label}: font id");
        assert_eq!(sg.section_index, g.section, "{label}: section index");
        assert_eq!(sg.byte_index, g.byte, "{label}: byte index");
        assert_eq!(
            sg.glyph.id,
            fonts[g.font].glyph_id(g.c),
            "{label}: glyph id for {:?}",
            g.c
        );
        assert!(
            close(sg.glyph.scale.x, g.scale) && close(sg.glyph.scale.y, g.scale),
            "{label}: glyph scale {:?} != {}",
            sg.glyph.scale,
            g.scale
        );
        assert!(
            close(sg.glyph.position.x, x_shift + *expected_x),
            "{label}: x {} != {}",
            sg.glyph.position.x,
            x_shift + *expected_x
        );
        assert!(
            close(sg.glyph.position.y, y),
            "{label}: y {} != {}",
            sg.glyph.position.y,
            y
        );
    }
}

fn line_advance(fonts: &[FontArc], expected: &ExpectedLine) -> f32 {
    expected
        .glyphs
        .last()
        .map(|(g, x)| *x + advance(fonts, *g))
        .unwrap_or(0.0)
}

#[test]
fn soft_hyphen_contract() {
    let dejavu =
        FontArc::try_from_slice(include_bytes!("../../fonts/DejaVuSans.ttf") as &[u8]).unwrap();
    let garamond =
        FontArc::try_from_slice(include_bytes!("../../fonts/GaramondNo8-Reg.ttf") as &[u8])
            .unwrap();
    let fonts = [dejavu, garamond];

    // ------------------------------------------------------------------
    // 1. A wide line hides the soft hyphen, no glyph & no width, and keeps
    //    kerning as if the soft hyphen was removed.
    // ------------------------------------------------------------------
    {
        let layout = Layout::default_wrap().with_soft_hyphens();
        let text = [SectionText {
            text: "A\u{ad}V",
            scale: PxScale::from(30.0),
            font_id: FontId(0),
        }];
        let geometry = SectionGeometry {
            screen_position: (0.0, 0.0),
            bounds: (1000.0, f32::INFINITY),
        };
        let glyphs = layout.calculate_glyphs(&fonts, &geometry, &text);

        // Independently: removing U+00AD leaves the contiguous word "AV".
        let hidden = vec![
            G {
                c: 'A',
                font: 0,
                scale: 30.0,
                section: 0,
                byte: 0,
            },
            G {
                c: 'V',
                font: 0,
                scale: 30.0,
                section: 0,
                byte: 3,
            },
        ];
        let expected = expected_line(&fonts, &[hidden], true);
        assert_line("wide hide", &glyphs, &expected, 0.0, 0.0, HorizontalAlign::Left, &fonts);

        assert!(
            !glyphs.iter().any(|g| g.glyph.id == fonts[0].glyph_id(SH)),
            "hidden soft hyphen must not be rendered"
        );
        assert!(
            !glyphs.iter().any(|g| g.glyph.id == fonts[0].glyph_id('-')),
            "no hyphen on a wide line"
        );
        assert!(line_advance(&fonts, &expected) < 1000.0);
    }

    // ------------------------------------------------------------------
    // 2. A narrow line replaces the selected soft hyphen with '-' using the
    //    same font/scale, preserving original section & byte indexes.
    // ------------------------------------------------------------------
    let narrow_text = "co\u{ad}operate";
    let co = vec![
        G {
            c: 'c',
            font: 0,
            scale: 24.0,
            section: 0,
            byte: 0,
        },
        G {
            c: 'o',
            font: 0,
            scale: 24.0,
            section: 0,
            byte: 1,
        },
    ];
    let hyphen = G {
        c: '-',
        font: 0,
        scale: 24.0,
        section: 0,
        byte: 2,
    };
    let operate: Vec<G> = "operate"
        .char_indices()
        .map(|(i, c)| G {
            c,
            font: 0,
            scale: 24.0,
            section: 0,
            byte: 4 + i,
        })
        .collect();

    let hidden_cooperate: Vec<G> = "cooperate"
        .char_indices()
        .map(|(i, c)| G {
            c,
            font: 0,
            scale: 24.0,
            section: 0,
            // "co" 0,1 then SH 2,3 then "operate" starts at 4
            byte: if i < 2 { i } else { i + 2 },
        })
        .collect();

    let hidden_width = word_width(&fonts, &hidden_cooperate);
    let co_hyphen_specs = {
        let mut specs = co.clone();
        specs.push(hyphen);
        specs
    };
    let co_hyphen_width = word_width(&fonts, &co_hyphen_specs);
    let operate_width = word_width(&fonts, &operate);
    let narrow_bound = co_hyphen_width.max(operate_width) + 0.5;
    assert!(co_hyphen_width <= narrow_bound);
    assert!(operate_width <= narrow_bound);
    assert!(
        hidden_width > narrow_bound,
        "the unhyphenated word must overflow the narrow width"
    );

    let narrow_layout = Layout::default_wrap().with_soft_hyphens();
    let narrow_text_section = [SectionText {
        text: narrow_text,
        scale: PxScale::from(24.0),
        font_id: FontId(0),
    }];
    let narrow_geometry = SectionGeometry {
        screen_position: (0.0, 0.0),
        bounds: (narrow_bound, f32::INFINITY),
    };
    let narrow_glyphs = narrow_layout.calculate_glyphs(
        &fonts,
        &narrow_geometry,
        &narrow_text_section,
    );

    let line1 = expected_line(&fonts, &[co_hyphen_specs], false);
    let line2 = expected_line(&fonts, &[operate.clone()], true);
    let line1_height = height(line1.metrics);

    assert_eq!(narrow_glyphs.len(), line1.glyphs.len() + line2.glyphs.len());

    // Line 1: "co-" (line1 width excludes no trailing non-space; the hyphen is
    // the selected break, so it is included).
    assert_line(
        "narrow line 1",
        &narrow_glyphs[..line1.glyphs.len()],
        &line1,
        0.0,
        0.0,
        HorizontalAlign::Left,
        &fonts,
    );
    // The '-' carries the soft hyphen's source index, not the following byte.
    let hyphen_glyph = &narrow_glyphs[line1.glyphs.len() - 1];
    assert_eq!(hyphen_glyph.section_index, 0);
    assert_eq!(hyphen_glyph.byte_index, 2);
    assert_eq!(hyphen_glyph.glyph.id, fonts[0].glyph_id('-'));

    // Line 2: "operate", original source byte indexes intact.
    assert_line(
        "narrow line 2",
        &narrow_glyphs[line1.glyphs.len()..],
        &line2,
        line1_height,
        0.0,
        HorizontalAlign::Left,
        &fonts,
    );

    // ------------------------------------------------------------------
    // 3. A soft-hyphen candidate that fits without '-' but not with '-' must
    //    not widen line 1; back off to the earlier normal break.
    // ------------------------------------------------------------------
    {
        let layout = Layout::default_wrap().with_soft_hyphens();
        let text = [SectionText {
            text: "A A\u{ad}B",
            scale: PxScale::from(24.0),
            font_id: FontId(0),
        }];
        let a = G {
            c: 'A',
            font: 0,
            scale: 24.0,
            section: 0,
            byte: 0,
        };
        let space = G {
            c: ' ',
            font: 0,
            scale: 24.0,
            section: 0,
            byte: 1,
        };
        let a2 = G {
            c: 'A',
            font: 0,
            scale: 24.0,
            section: 0,
            byte: 2,
        };
        let b = G {
            c: 'B',
            font: 0,
            scale: 24.0,
            section: 0,
            byte: 5,
        };
        let sh_hyphen = G {
            c: '-',
            font: 0,
            scale: 24.0,
            section: 0,
            byte: 3,
        };

        let earlier_candidate_width = word_width(&fonts, &[a]);
        let first_word = vec![a, space];

        let sh_width = word_width(&fonts, &first_word)
            + word_width(&fonts, &[a2])
            + kern(&fonts, a2, sh_hyphen)
            + advance(&fonts, sh_hyphen);
        let end_width = word_width(&fonts, &first_word) + word_width(&fonts, &[a2, b]);

        let bound = (earlier_candidate_width + sh_width) / 2.0;
        assert!(bound > earlier_candidate_width, "earlier candidate fits");
        assert!(bound < sh_width, "candidate with '-' does not fit");
        assert!(bound < end_width, "unhyphenated word does not fit");

        let geometry = SectionGeometry {
            screen_position: (0.0, 0.0),
            bounds: (bound, f32::INFINITY),
        };
        let glyphs = layout.calculate_glyphs(&fonts, &geometry, &text);

        let line1 = expected_line(&fonts, &[first_word], false);
        assert!(glyphs.len() >= 2, "line 1 should contain at least 'A '");
        assert_line(
            "fallback line 1",
            &glyphs[..2],
            &line1,
            0.0,
            0.0,
            HorizontalAlign::Left,
            &fonts,
        );
        assert!(
            glyphs[..2]
                .iter()
                .all(|g| g.glyph.id != fonts[0].glyph_id('-')),
            "the overflowing hyphen candidate must not widen line 1"
        );
        assert!(
            line_advance(&fonts, &line1) <= bound + 1e-3,
            "line 1 visible content fits its bound"
        );

        // The moved word continues greedily; compute which of its candidates
        // fit independently from the font advances above.
        let sh2_width = word_width(&fonts, &[a2])
            + kern(&fonts, a2, sh_hyphen)
            + advance(&fonts, sh_hyphen);
        let line2_ab_width = word_width(&fonts, &[a2, b]);
        let line2_start = 2;
        if line2_ab_width <= bound {
            // "AB" fits on line 2.
            let exp2 = expected_line(&fonts, &[vec![a2, b]], true);
            assert_line(
                "fallback line 2 (unhyphenated)",
                &glyphs[line2_start..],
                &exp2,
                height(line1.metrics),
                0.0,
                HorizontalAlign::Left,
                &fonts,
            );
        } else {
            assert!(sh2_width <= bound, "the moved word can hyphenate");
            let exp2 = expected_line(&fonts, &[vec![a2, sh_hyphen]], false);
            assert_line(
                "fallback line 2 (hyphenated)",
                &glyphs[line2_start..line2_start + exp2.glyphs.len()],
                &exp2,
                height(line1.metrics),
                0.0,
                HorizontalAlign::Left,
                &fonts,
            );
            let exp3 = expected_line(&fonts, &[vec![b]], true);
            assert_line(
                "fallback line 3",
                &glyphs[line2_start + exp2.glyphs.len()..],
                &exp3,
                height(line1.metrics) + height(exp2.metrics),
                0.0,
                HorizontalAlign::Left,
                &fonts,
            );
        }
    }

    // ------------------------------------------------------------------
    // 3b. If the unhyphenated prefix fits but the '-' does not, and there is
    //     no earlier break, do not widen the line: break at the SH without a
    //     temporary hyphen.
    // ------------------------------------------------------------------
    {
        let layout = Layout::default_wrap().with_soft_hyphens();
        let prefix_width = word_width(&fonts, &co);
        assert!(prefix_width < co_hyphen_width);
        let bound = (prefix_width + co_hyphen_width) / 2.0;

        let geometry = SectionGeometry {
            screen_position: (0.0, 0.0),
            bounds: (bound, f32::INFINITY),
        };
        let glyphs = layout.calculate_glyphs(&fonts, &geometry, &narrow_text_section);

        let line1 = expected_line(&fonts, &[co.clone()], true);
        assert_eq!(glyphs.len(), co.len() + operate.len());
        assert_line(
            "no-earlier line 1",
            &glyphs[..co.len()],
            &line1,
            0.0,
            0.0,
            HorizontalAlign::Left,
            &fonts,
        );
        assert!(
            !glyphs.iter().any(|g| g.glyph.id == fonts[0].glyph_id('-')),
            "no hyphen when the candidate with '-' does not fit"
        );
        assert!(
            line_advance(&fonts, &line1) <= bound + 1e-3,
            "line 1 must not be widened"
        );

        let line2 = expected_line(&fonts, &[operate.clone()], true);
        assert_line(
            "no-earlier line 2",
            &glyphs[co.len()..],
            &line2,
            height(line1.metrics),
            0.0,
            HorizontalAlign::Left,
            &fonts,
        );
    }

    // ------------------------------------------------------------------
    // 3c. Leading, trailing, consecutive and hard-break-adjacent soft
    //     hyphens draw no hyphen and create no empty lines.
    // ------------------------------------------------------------------
    {
        let layout = Layout::default_wrap().with_soft_hyphens();
        let cases: [(&str, Vec<Vec<G>>); 5] = [
            (
                "\u{ad}AB",
                vec![vec![
                    G {
                        c: 'A',
                        font: 0,
                        scale: 24.0,
                        section: 0,
                        byte: 2,
                    },
                    G {
                        c: 'B',
                        font: 0,
                        scale: 24.0,
                        section: 0,
                        byte: 3,
                    },
                ]],
            ),
            (
                "AB\u{ad}",
                vec![vec![
                    G {
                        c: 'A',
                        font: 0,
                        scale: 24.0,
                        section: 0,
                        byte: 0,
                    },
                    G {
                        c: 'B',
                        font: 0,
                        scale: 24.0,
                        section: 0,
                        byte: 1,
                    },
                ]],
            ),
            (
                "A\u{ad}\u{ad}B",
                vec![vec![
                    G {
                        c: 'A',
                        font: 0,
                        scale: 24.0,
                        section: 0,
                        byte: 0,
                    },
                    G {
                        c: 'B',
                        font: 0,
                        scale: 24.0,
                        section: 0,
                        byte: 5,
                    },
                ]],
            ),
            (
                "A\u{ad}\nB",
                vec![
                    vec![G {
                        c: 'A',
                        font: 0,
                        scale: 24.0,
                        section: 0,
                        byte: 0,
                    }],
                    vec![G {
                        c: 'B',
                        font: 0,
                        scale: 24.0,
                        section: 0,
                        byte: 4,
                    }],
                ],
            ),
            (
                "A\n\u{ad}B",
                vec![
                    vec![G {
                        c: 'A',
                        font: 0,
                        scale: 24.0,
                        section: 0,
                        byte: 0,
                    }],
                    vec![G {
                        c: 'B',
                        font: 0,
                        scale: 24.0,
                        section: 0,
                        byte: 4,
                    }],
                ],
            ),
        ];

        for (text, lines) in cases {
            let section_text = [SectionText {
                text,
                scale: PxScale::from(24.0),
                font_id: FontId(0),
            }];
            let geometry = SectionGeometry {
                screen_position: (0.0, 0.0),
                bounds: (1000.0, f32::INFINITY),
            };
            let glyphs = layout.calculate_glyphs(&fonts, &geometry, &section_text);

            let mut offset = 0;
            let mut line_top = 0.0;
            for (i, specs) in lines.iter().enumerate() {
                let expected = expected_line(&fonts, std::slice::from_ref(specs), true);
                assert_line(
                    &format!("edge case {text:?} line {i}"),
                    &glyphs[offset..offset + expected.glyphs.len()],
                    &expected,
                    line_top,
                    0.0,
                    HorizontalAlign::Left,
                    &fonts,
                );
                offset += expected.glyphs.len();
                line_top += height(expected.metrics);
            }
            assert_eq!(offset, glyphs.len(), "edge case {text:?}: glyph count");
            assert!(
                !glyphs
                    .iter()
                    .any(|g| g.glyph.id == fonts[0].glyph_id('-')),
                "edge case {text:?}: no invented hyphen"
            );
        }
    }

    // ------------------------------------------------------------------
    // 3d. `default_single_line` with the adapter hides SH and never adds a
    //     hyphen when truncating.
    // ------------------------------------------------------------------
    {
        let layout = Layout::default_single_line().with_soft_hyphens();
        let text = [SectionText {
            text: "A\u{ad}B",
            scale: PxScale::from(24.0),
            font_id: FontId(0),
        }];
        let a = G {
            c: 'A',
            font: 0,
            scale: 24.0,
            section: 0,
            byte: 0,
        };
        let b = G {
            c: 'B',
            font: 0,
            scale: 24.0,
            section: 0,
            byte: 3,
        };

        // Wide: both visible glyphs, SH hidden.
        let wide_geometry = SectionGeometry {
            screen_position: (0.0, 0.0),
            bounds: (1000.0, f32::INFINITY),
        };
        let glyphs = layout.calculate_glyphs(&fonts, &wide_geometry, &text);
        let expected = expected_line(&fonts, &[vec![a, b]], true);
        assert_line(
            "single line wide",
            &glyphs,
            &expected,
            0.0,
            0.0,
            HorizontalAlign::Left,
            &fonts,
        );
        assert!(!glyphs.iter().any(|g| g.glyph.id == fonts[0].glyph_id('-')));

        // Truncated at the SH break: only "A", still no temporary hyphen.
        let trunc_bound = advance(&fonts, a) + 0.5;
        assert!(trunc_bound < word_width(&fonts, &[a, b]));
        let trunc_geometry = SectionGeometry {
            screen_position: (0.0, 0.0),
            bounds: (trunc_bound, f32::INFINITY),
        };
        let glyphs = layout.calculate_glyphs(&fonts, &trunc_geometry, &text);
        let expected = expected_line(&fonts, &[vec![a]], false);
        assert_line(
            "single line truncated",
            &glyphs,
            &expected,
            0.0,
            0.0,
            HorizontalAlign::Left,
            &fonts,
        );
        assert_eq!(glyphs.len(), 1);
        assert!(!glyphs.iter().any(|g| g.glyph.id == fonts[0].glyph_id('-')));
    }

    // ------------------------------------------------------------------
    // 4. Cross text-run font, scale & extra: the selected hyphen uses the run
    //    containing the soft hyphen, and a run boundary is not a line break.
    // ------------------------------------------------------------------
    let run0 = vec![
        G {
            c: 'c',
            font: 0,
            scale: 16.0,
            section: 0,
            byte: 0,
        },
        G {
            c: 'o',
            font: 0,
            scale: 16.0,
            section: 0,
            byte: 1,
        },
    ];
    let mixed_hyphen = G {
        c: '-',
        font: 1,
        scale: 24.0,
        section: 1,
        byte: 0,
    };
    let run1_operate: Vec<G> = "operate"
        .char_indices()
        .map(|(i, c)| G {
            c,
            font: 1,
            scale: 24.0,
            section: 1,
            byte: 2 + i,
        })
        .collect();
    let mixed_hidden = {
        let mut specs = run0.clone();
        specs.extend(run1_operate.iter().copied());
        specs
    };
    let mixed_hidden_width = word_width(&fonts, &mixed_hidden);
    let mixed_line1_specs = {
        let mut specs = run0.clone();
        specs.push(mixed_hyphen);
        specs
    };
    let mixed_line1_width = word_width(&fonts, &mixed_line1_specs);
    let mixed_operate_width = word_width(&fonts, &run1_operate);
    let mixed_narrow_bound = mixed_line1_width.max(mixed_operate_width) + 0.5;
    assert!(mixed_line1_width <= mixed_narrow_bound);
    assert!(mixed_operate_width <= mixed_narrow_bound);
    assert!(mixed_hidden_width > mixed_narrow_bound);

    let mixed_texts = [
        SectionText {
            text: "co",
            scale: PxScale::from(16.0),
            font_id: FontId(0),
        },
        SectionText {
            text: "\u{ad}operate",
            scale: PxScale::from(24.0),
            font_id: FontId(1),
        },
    ];

    let mixed_layout = Layout::default_wrap().with_soft_hyphens();

    // Wide: one line "cooperate", SH hidden, run boundary not a break.
    {
        let geometry = SectionGeometry {
            screen_position: (0.0, 0.0),
            bounds: (1000.0, f32::INFINITY),
        };
        let glyphs = mixed_layout.calculate_glyphs(&fonts, &geometry, &mixed_texts);
        let expected = expected_line(&fonts, &[run0.clone(), run1_operate.clone()], true);
        assert_line(
            "mixed wide",
            &glyphs,
            &expected,
            0.0,
            0.0,
            HorizontalAlign::Left,
            &fonts,
        );
        assert_eq!(glyphs.len(), 2 + 7);
        assert!(!glyphs.iter().any(|g| g.glyph.id == fonts[1].glyph_id('-')));
    }

    // Narrow: line 1 "co-" with hyphen in run 1's font/scale/extra.
    {
        let geometry = SectionGeometry {
            screen_position: (0.0, 0.0),
            bounds: (mixed_narrow_bound, f32::INFINITY),
        };
        let glyphs = mixed_layout.calculate_glyphs(&fonts, &geometry, &mixed_texts);

        let line1 = expected_line(&fonts, &[mixed_line1_specs.clone()], false);
        let line2 = expected_line(&fonts, &[run1_operate.clone()], true);
        let line1_height = height(line1.metrics);

        assert_eq!(glyphs.len(), line1.glyphs.len() + line2.glyphs.len());
        assert_line(
            "mixed narrow line 1",
            &glyphs[..line1.glyphs.len()],
            &line1,
            0.0,
            0.0,
            HorizontalAlign::Left,
            &fonts,
        );
        let h = &glyphs[line1.glyphs.len() - 1];
        assert_eq!(h.font_id, FontId(1), "hyphen uses the SH run font");
        assert_eq!(h.section_index, 1, "hyphen uses the SH run section");
        assert_eq!(h.byte_index, 0, "hyphen keeps the SH byte index");
        assert!(close(h.glyph.scale.x, 24.0) && close(h.glyph.scale.y, 24.0));
        assert_eq!(h.glyph.id, fonts[1].glyph_id('-'));

        assert_line(
            "mixed narrow line 2",
            &glyphs[line1.glyphs.len()..],
            &line2,
            line1_height,
            0.0,
            HorizontalAlign::Left,
            &fonts,
        );

        // Line 1 height includes the actually rendered hyphen's VMetrics. The
        // 24px run dominates the 16px run, so line 2 starts after its height.
        let hyphen_metrics = metrics(
            &fonts,
            G {
                c: '-',
                font: 1,
                scale: 24.0,
                section: 1,
                byte: 0,
            },
        );
        assert!(height(hyphen_metrics) >= height(line1.metrics));
        assert!(close(
            glyphs[line1.glyphs.len()].glyph.position.y,
            line1_height + line1.metrics.0
        ));
    }

    // Same word split across two adjacent runs of the same font & scale: the
    // run boundary is not a break, kerning continues, and the hyphen keeps the
    // second run as its source.
    {
        let run1_same: Vec<G> = "operate"
            .char_indices()
            .map(|(i, c)| G {
                c,
                font: 0,
                scale: 24.0,
                section: 1,
                byte: 2 + i,
            })
            .collect();
        let same_hyphen = G {
            c: '-',
            font: 0,
            scale: 24.0,
            section: 1,
            byte: 0,
        };
        let texts = [
            SectionText {
                text: "co",
                scale: PxScale::from(24.0),
                font_id: FontId(0),
            },
            SectionText {
                text: "\u{ad}operate",
                scale: PxScale::from(24.0),
                font_id: FontId(0),
            },
        ];
        let layout = Layout::default_wrap().with_soft_hyphens();

        let all_hidden: Vec<G> = {
            let mut specs = co.clone();
            specs.extend(run1_same.iter().copied());
            specs
        };
        let line1_specs = {
            let mut specs = co.clone();
            specs.push(same_hyphen);
            specs
        };
        let line1_width = word_width(&fonts, &line1_specs);
        let operate_width = word_width(&fonts, &run1_same);
        let narrow = line1_width.max(operate_width) + 0.5;
        assert!(word_width(&fonts, &all_hidden) > narrow);

        // Wide: one contiguous word, no break at the run boundary, SH hidden.
        let wide_geometry = SectionGeometry {
            screen_position: (0.0, 0.0),
            bounds: (1000.0, f32::INFINITY),
        };
        let glyphs = layout.calculate_glyphs(&fonts, &wide_geometry, &texts);
        let expected = expected_line(&fonts, &[all_hidden], true);
        assert_line(
            "split run wide",
            &glyphs,
            &expected,
            0.0,
            0.0,
            HorizontalAlign::Left,
            &fonts,
        );
        assert_eq!(glyphs.len(), 2 + 7);
        assert!(!glyphs.iter().any(|g| g.glyph.id == fonts[0].glyph_id('-')));
        assert!(glyphs.iter().any(|g| g.section_index == 1));

        // Narrow: "co-" from run 2, then "operate" from run 2.
        let narrow_geometry = SectionGeometry {
            screen_position: (0.0, 0.0),
            bounds: (narrow, f32::INFINITY),
        };
        let glyphs = layout.calculate_glyphs(&fonts, &narrow_geometry, &texts);
        let line1 = expected_line(&fonts, &[line1_specs], false);
        let line2 = expected_line(&fonts, &[run1_same], true);
        assert_eq!(glyphs.len(), line1.glyphs.len() + line2.glyphs.len());
        assert_line(
            "split run narrow line 1",
            &glyphs[..line1.glyphs.len()],
            &line1,
            0.0,
            0.0,
            HorizontalAlign::Left,
            &fonts,
        );
        let h = &glyphs[line1.glyphs.len() - 1];
        assert_eq!(h.section_index, 1);
        assert_eq!(h.byte_index, 0);
        assert_eq!(h.font_id, FontId(0));
        assert_eq!(h.glyph.id, fonts[0].glyph_id('-'));
        assert_line(
            "split run narrow line 2",
            &glyphs[line1.glyphs.len()..],
            &line2,
            height(line1.metrics),
            0.0,
            HorizontalAlign::Left,
            &fonts,
        );
    }

    // ------------------------------------------------------------------
    // 5. Right & center alignment use the final visible line width
    //    (including the rendered hyphen, excluding hidden SH).
    // ------------------------------------------------------------------
    for h_align in [HorizontalAlign::Right, HorizontalAlign::Center] {
        let layout = Layout::default_wrap()
            .with_soft_hyphens()
            .h_align(h_align);
        let screen_x = 100.0;
        let geometry = SectionGeometry {
            screen_position: (screen_x, 0.0),
            bounds: (narrow_bound, f32::INFINITY),
        };
        let glyphs = layout.calculate_glyphs(&fonts, &geometry, &narrow_text_section);

        let line1 = expected_line(&fonts, &[{
            let mut specs = co.clone();
            specs.push(hyphen);
            specs
        }], false);
        let line2 = expected_line(&fonts, &[operate.clone()], true);
        assert_eq!(glyphs.len(), line1.glyphs.len() + line2.glyphs.len());
        assert_line(
            "aligned line 1",
            &glyphs[..line1.glyphs.len()],
            &line1,
            0.0,
            screen_x,
            h_align,
            &fonts,
        );
        assert_line(
            "aligned line 2",
            &glyphs[line1.glyphs.len()..],
            &line2,
            height(line1.metrics),
            screen_x,
            h_align,
            &fonts,
        );

        // The rendered hyphen is the rightmost/centrally relevant glyph.
        let hyphen_glyph = &glyphs[line1.glyphs.len() - 1];
        let rendered_right = hyphen_glyph.glyph.position.x + advance(&fonts, hyphen);
        match h_align {
            HorizontalAlign::Right => assert!(close(rendered_right, screen_x)),
            HorizontalAlign::Center => {
                assert!(close(glyphs[0].glyph.position.x, screen_x - line1.width / 2.0))
            }
            HorizontalAlign::Left => unreachable!(),
        }
    }

    // ------------------------------------------------------------------
    // 6. Cache correctness: reuse one brush, queue narrow -> wide -> narrow,
    //    then the identical narrow content again for a cache redraw.
    // ------------------------------------------------------------------
    type Vertex = [f32; 4];
    let red = [1.0, 0.0, 0.0, 1.0];
    let blue = [0.0, 0.0, 1.0, 1.0];

    let mixed_section = |bound: f32| {
        Section::default()
            .with_screen_position((0.0, 0.0))
            .with_bounds((bound, f32::INFINITY))
            .add_text(
                Text::new("co")
                    .with_scale(16.0)
                    .with_font_id(FontId(0))
                    .with_color(red),
            )
            .add_text(
                Text::new("\u{ad}operate")
                    .with_scale(24.0)
                    .with_font_id(FontId(1))
                    .with_color(blue),
            )
    };

    let mut brush: GlyphBrush<Vertex> =
        GlyphBrushBuilder::using_fonts(vec![fonts[0].clone(), fonts[1].clone()]).build();

    let color_counts = |verts: &[Vertex]| {
        let mut reds = 0;
        let mut blues = 0;
        for v in verts {
            if v == &red {
                reds += 1;
            } else if v == &blue {
                blues += 1;
            }
        }
        (reds, blues)
    };

    let process = |brush: &mut GlyphBrush<Vertex>| {
        brush
            .process_queued(|_, _| {}, |v| v.extra.color)
            .expect("process_queued")
    };

    let mut narrow_expected_lines_checked = false;
    let wide_expected = expected_line(&fonts, &[run0.clone(), run1_operate.clone()], true);
    let wide_expected_lines = vec![wide_expected];

    for (stage, bound) in [("narrow", mixed_narrow_bound), ("wide", 1000.0), ("narrow", mixed_narrow_bound)]
    {
        let section = mixed_section(bound);
        let brush_glyphs: Vec<SectionGlyph> = brush
            .glyphs_custom_layout(&section, &mixed_layout)
            .cloned()
            .collect();

        if stage == "narrow" {
            let line1 = expected_line(&fonts, &[mixed_line1_specs.clone()], false);
            let line2 = expected_line(&fonts, &[run1_operate.clone()], true);
            let expected_lines = vec![line1, line2];
            assert_eq!(
                brush_glyphs.len(),
                expected_lines.iter().map(|l| l.glyphs.len()).sum::<usize>()
            );
            let mut offset = 0;
            let mut line_top = 0.0;
            for (i, line) in expected_lines.iter().enumerate() {
                assert_line(
                    &format!("cache narrow {i}"),
                    &brush_glyphs[offset..offset + line.glyphs.len()],
                    line,
                    line_top,
                    0.0,
                    HorizontalAlign::Left,
                    &fonts,
                );
                offset += line.glyphs.len();
                line_top += height(line.metrics);
            }
            narrow_expected_lines_checked = true;
        } else {
            assert_eq!(brush_glyphs.len(), wide_expected_lines[0].glyphs.len());
            assert_line(
                "cache wide",
                &brush_glyphs,
                &wide_expected_lines[0],
                0.0,
                0.0,
                HorizontalAlign::Left,
                &fonts,
            );
        }

        brush.queue_custom_layout(&section, &mixed_layout);
        let action = process(&mut brush);
        let BrushAction::Draw(verts) = action else {
            panic!("stage {stage}: expected Draw after a geometry/content change");
        };
        assert_eq!(
            verts.len(),
            brush_glyphs.len(),
            "stage {stage}: vertex count"
        );
        let (reds, blues) = color_counts(&verts);
        if stage == "narrow" {
            // run 0: "co" -> 2 red vertices; run 1: '-' + "operate" -> 8 blue
            assert_eq!((reds, blues), (2, 8), "stage {stage}: extras");
        } else {
            assert_eq!((reds, blues), (2, 7), "stage {stage}: extras");
        }
    }
    let _ = narrow_expected_lines_checked;

    // Same content submitted again must hit the existing redraw cache.
    let section = mixed_section(mixed_narrow_bound);
    brush.queue_custom_layout(&section, &mixed_layout);
    match process(&mut brush) {
        BrushAction::ReDraw => {}
        BrushAction::Draw(_) => panic!("identical narrow resubmission did not reuse the cache"),
    }

    // A different layout mode (plain Layout) must not reuse the soft-hyphen
    // adapter's narrow vertices: plain Layout keeps the SH glyph.
    {
        let section = mixed_section(1000.0);
        let plain = Layout::default_wrap();
        let plain_glyphs: Vec<SectionGlyph> = brush
            .glyphs_custom_layout(&section, &plain)
            .cloned()
            .collect();
        assert_eq!(plain_glyphs.len(), 2 + 1 + 7);
        assert!(
            plain_glyphs
                .iter()
                .any(|g| g.glyph.id == fonts[1].glyph_id(SH)),
            "plain Layout still renders the soft hyphen glyph"
        );

        brush.queue_custom_layout(&section, &plain);
        let BrushAction::Draw(verts) = process(&mut brush) else {
            panic!("plain layout after adapter must be a fresh Draw");
        };
        assert_eq!(verts.len(), plain_glyphs.len());
    }

    println!("SOFT_HYPHEN_CONTRACT_OK");
}
