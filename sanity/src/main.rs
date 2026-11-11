use glyph_brush::{ab_glyph::*, *};
fn main() {
    let font = FontArc::try_from_slice(include_bytes!("../../fonts/DejaVuSans.ttf")).unwrap();
    let text = [SectionText { text: "co\u{ad}operate", scale: PxScale::from(24.0), font_id: FontId(0) }];
    let glyphs = Layout::default_wrap().calculate_glyphs(&[font.clone()], &SectionGeometry {
        screen_position: (0.0, 0.0), bounds: (1000.0, 1000.0),
    }, &text);
    assert_eq!(glyphs.len(), 10);
    assert!(glyphs.iter().all(|g| g.glyph.position.x.is_finite()));
    let mut brush: GlyphBrush<[f32; 4]> = GlyphBrushBuilder::using_font(font).build();
    brush.queue(Section::default().add_text(Text::new("AVA").with_scale(24.0)));
    let mut nonzero = false;
    let action = brush.process_queued(|_, bytes| { nonzero |= bytes.iter().any(|&b| b > 0); },
        |v| [v.pixel_coords.min.x, v.pixel_coords.min.y, v.pixel_coords.max.x, v.pixel_coords.max.y]).unwrap();
    match action { BrushAction::Draw(v) => assert_eq!(v.len(), 3), _ => panic!("initial frame was not Draw") }
    assert!(nonzero);
    println!("GLYPH_LAYOUT_AND_ATLAS_OK");
    println!("CURRENT_SOFT_HYPHEN_GLYPHS=10");
}
