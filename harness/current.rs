use tiny_skia::*;
fn main() {
    let mut pixmap = Pixmap::new(9, 9).unwrap();
    let mut paint = Paint::default();
    paint.anti_alias = false;
    paint.force_hq_pipeline = true;
    paint.shader = RadialGradient::new(
        Point::from_xy(4.0, 4.0), 0.0,
        Point::from_xy(4.0, 4.0), 4.0,
        vec![GradientStop::new(0.0, Color::from_rgba8(255,0,0,255)),
             GradientStop::new(1.0, Color::from_rgba8(0,0,255,255))],
        SpreadMode::Pad, Transform::identity()).unwrap();
    let rect = Rect::from_xywh(0.0,0.0,9.0,9.0).unwrap();
    pixmap.fill_rect(rect,&paint,Transform::identity(),None);
    for y in 0..9 { for x in 0..9 {
        let t=(((x as f64+0.5-4.0).powi(2)+(y as f64+0.5-4.0).powi(2)).sqrt()/4.0).min(1.0);
        let p=pixmap.pixel(x,y).unwrap();
        assert!((p.red() as f64 - 255.0*(1.0-t)).abs()<2.0);
        assert!((p.blue() as f64 - 255.0*t).abs()<2.0);
        assert_eq!(p.alpha(),255);
    }}
    println!("TINY_SKIA_BASELINE_OK analyticalPixels=81");
}
