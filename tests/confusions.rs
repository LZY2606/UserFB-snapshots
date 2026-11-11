//! Standalone test tying together three easily-confused CSS Color 4
//! concepts. Each section shows the input, the intermediate judgment,
//! and the final result.
//!
//! Run it on its own with:
//!
//! ```sh
//! cargo test --test confusions -- --nocapture
//! ```

use csscolorparser::parse;

/// Signed circular difference of two angles in degrees, in [-180, 180).
fn angle_diff_deg(a: f64, b: f64) -> f64 {
    ((a - b) % 360.0 + 540.0) % 360.0 - 180.0
}

#[test]
fn css_color4_three_confusions() {
    // =================================================================
    // 1. Syntactically valid != inside the gamut.
    // =================================================================
    println!("\n=== 1. 语法合法 != 在色域内 ===");

    // Input: every component is a valid <number>, the string parses fine.
    let s = "color(srgb 1.2 -0.1 0.5)";
    let c = parse(s).expect("syntax is valid");
    println!("输入:   {s}");
    println!(
        "中间判断: parse 成功,但 r={} > 1、g={} < 0 -> 语法合法,色域外",
        c.r, c.g
    );
    assert!(c.r > 1.0 && c.g < 0.0);
    // Result: `color(srgb ...)` preserves the raw components; clamping is
    // deferred to output (`to_rgba8`, `to_array`, ...).
    println!(
        "结果:   原始分量被保留 {:?}; 只有输出时才 clamp -> to_rgba8 = {:?}",
        [c.r, c.g, c.b, c.a],
        c.to_rgba8()
    );
    assert_eq!(c.to_rgba8(), [255, 0, 128, 255]);
    assert_eq!(
        c.to_css_color_srgb().to_string(),
        "color(srgb 1.2 -0.1 0.5)"
    );

    // Same idea, different function: `rgb()` clamps at parse time instead.
    let c = parse("rgb(300 -20 128)").unwrap();
    println!("输入:   rgb(300 -20 128)");
    println!("中间判断: rgb() 在 parse 时就把分量 clamp 到 [0,255]");
    println!("结果:   to_rgba8 = {:?}", c.to_rgba8());
    assert_eq!(c.to_rgba8(), [255, 0, 128, 255]);

    // Angles wrap, other components clamp: still valid syntax.
    let wrapped = parse("hsl(-120 50% 50%)").unwrap();
    let reference = parse("hsl(240 50% 50%)").unwrap();
    println!("输入:   hsl(-120 50% 50%)");
    println!("中间判断: 色相按 360 度 wrap (-120 == 240),s/l clamp 到 [0,100%]");
    println!(
        "结果:   与 hsl(240 50% 50%) 相同 -> {:?}",
        wrapped.to_rgba8()
    );
    assert_eq!(wrapped.to_rgba8(), reference.to_rgba8());
    let clamped = parse("hsl(720 150% -20%)").unwrap();
    println!("输入:   hsl(720 150% -20%)");
    println!(
        "结果:   h=720->0, s=150%->100%, l=-20%->0% -> {:?}",
        clamped.to_rgba8()
    );
    assert_eq!(clamped.to_rgba8(), [0, 0, 0, 255]);

    // ...while genuinely invalid syntax still fails.
    for bad in ["color(srgb 1.2 x 0.5)", "color(srgb 1.2 0.5)", "rgb(1 2)"] {
        println!(
            "对照:   {bad:?} 语法不合法 -> {:?}",
            parse(bad).unwrap_err()
        );
        assert!(parse(bad).is_err());
    }

    // =================================================================
    // 2. Different components have different tolerances.
    // =================================================================
    println!("\n=== 2. 不同 component 的容差不一样 ===");

    // Input: a color whose components need more precision than the
    // serializer keeps.
    let s0 = "hsl(123.4567 55.55% 44.44% / 53.7%)";
    let c1 = parse(s0).unwrap();
    let s1 = c1.to_css_hsl().to_string();
    let c2 = parse(&s1).unwrap();
    let [h1, s_1, l1, a1] = c1.to_hsla().map(f64::from);
    let [h2, s_2, l2, a2] = c2.to_hsla().map(f64::from);
    // Intermediate judgment: the serializer quantizes each component on
    // its own scale -- hue to 0.001 degrees, s/l/alpha to 1% -- so the
    // round-trip error budget is per-component, not one global epsilon.
    let hue_err = angle_diff_deg(h1, h2).abs();
    let s_err = (s_1 - s_2).abs();
    let l_err = (l1 - l2).abs();
    let a_err = (a1 - a2).abs();
    println!("输入:   {s0}");
    println!("中间判断: serialize 量化各不相同 -> {s1}");
    println!("        hue 量化到 0.001 度, s/l/alpha 量化到 1%");
    println!("结果:   hue 误差 {hue_err:.6}° (容差 0.001°)");
    println!("        s   误差 {s_err:.6}  (容差 0.005)");
    println!("        l   误差 {l_err:.6}  (容差 0.005)");
    println!("        a   误差 {a_err:.6}  (容差 0.005)");
    assert!(hue_err <= 1e-3, "hue err {hue_err}");
    assert!(s_err <= 5e-3 && l_err <= 5e-3 && a_err <= 5e-3);
    // And the errors genuinely differ per component: the s/l/alpha error
    // would blow straight past the hue tolerance, so no single epsilon
    // can validate a round trip.
    assert!(
        s_err > 1e-3 && l_err > 1e-3 && a_err > 1e-3,
        "s/l/alpha errors exceed the hue tolerance: one global epsilon cannot work"
    );

    // Hue tolerance also depends on chroma: at high chroma the hue is
    // stable across a round trip; as chroma approaches zero the hue
    // becomes ill-defined even though L and C still round-trip fine.
    let hi = parse("oklch(0.6 0.15 45.375)").unwrap();
    let hi2 = parse(&hi.to_css_oklch().to_string()).unwrap();
    let hue_err_hi = angle_diff_deg(
        f64::from(hi.to_oklcha()[2].to_degrees()),
        f64::from(hi2.to_oklcha()[2].to_degrees()),
    )
    .abs();
    let lo = parse("oklch(0.6 0.001 45.375)").unwrap();
    let lo_s = lo.to_css_oklch().to_string();
    let lo2 = parse(&lo_s).unwrap();
    let hue_err_lo = angle_diff_deg(
        f64::from(lo.to_oklcha()[2].to_degrees()),
        f64::from(lo2.to_oklcha()[2].to_degrees()),
    )
    .abs();
    let [l_lo, c_lo, _, _] = lo.to_oklcha();
    let [l_lo2, c_lo2, _, _] = lo2.to_oklcha();
    println!("输入:   oklch(0.6 0.15 45.375) 与 oklch(0.6 0.001 45.375)");
    println!("中间判断: hue 的误差随 chroma 缩小而放大,c->0 时 hue 无定义");
    println!("结果:   c=0.15  时 hue 往返误差 {hue_err_hi:.6}° (容差 0.1°)");
    println!("        c=0.001 时 serialize 为 {lo_s},hue 往返误差 {hue_err_lo:.6}°");
    println!("        同一颜色的 l/c 仍然各自吻合 (容差 0.001)");
    assert!(hue_err_hi <= 0.1, "hue err at c=0.15: {hue_err_hi}");
    // At c=0.001 the hue drift exceeds the 0.001-degree grid that was
    // more than enough at c=0.15 -- the hue tolerance must scale with
    // chroma (and be skipped entirely near c=0).
    assert!(hue_err_lo > 1e-3, "hue err at c=0.001: {hue_err_lo}");
    assert!((l_lo - l_lo2).abs() <= 1e-3 && (c_lo - c_lo2).abs() <= 1e-3);

    // At exactly-zero chroma the reported hue is pure rounding artifact:
    // a neutral gray has no hue, yet `atan2` of signed f32 noise
    // deterministically reports 180 degrees here.
    let gray = parse("rgb(181 181 181)").unwrap();
    let [_, gray_c, gray_h, _] = gray.to_oklcha();
    println!("输入:   rgb(181 181 181) (中性灰)");
    println!("中间判断: c={gray_c:.2e} ~= 0,atan2(f32 舍入噪声) 给出确定但无意义的 hue");
    println!(
        "结果:   to_oklcha 报告 hue = {:.1}° -- 色度为 0 时 hue 容差必须排除",
        gray_h.to_degrees()
    );
    assert!(gray_c < 1e-6);

    // =================================================================
    // 3. In relative colors, `none` is a missing component, not the
    //    number zero.
    // =================================================================
    println!("\n=== 3. relative color 里的 none 不是数值零 ===");

    // Input: the same origin color, three ways to write the red channel.
    // #bad455 = rgb(186 212 85).
    let s_kw = "rgb(from #bad455 r g b)";
    let s_none = "rgb(from #bad455 none g b)";
    let s_zero = "rgb(from #bad455 0 g b)";
    let c_kw = parse(s_kw).unwrap();
    let c_none = parse(s_none).unwrap();
    let c_zero = parse(s_zero).unwrap();
    // Intermediate judgment: per CSS Color 4, `none` marks a *missing*
    // component (carried over through conversion and interpolation), which
    // is a different concept from the number 0. This library resolves
    // `none` to 0.0 at evaluation time -- so here `none` happens to agree
    // with an explicit `0`, but it is the component keyword `r` that
    // inherits the origin's value. Missing is not inheritance.
    println!("输入:   {s_kw} / {s_none} / {s_zero}  (origin #bad455 = rgb(186 212 85))");
    println!("中间判断: 关键字 r 继承 origin 的 186; none 是缺失标记,");
    println!("        本库在求值时解析为 0.0,恰好等于显式的 0,但语义不是继承");
    println!(
        "结果:   关键字 -> {:?}, none -> {:?}, 0 -> {:?}",
        c_kw.to_rgba8(),
        c_none.to_rgba8(),
        c_zero.to_rgba8()
    );
    assert_eq!(c_kw.to_rgba8(), [186, 212, 85, 255]);
    assert_eq!(c_none.to_rgba8(), [0, 212, 85, 255]);
    assert_eq!(c_none, c_zero);
    assert_ne!(c_none, c_kw);

    // All components `none`: the result is *not* the origin color.
    let c = parse("hwb(from #bad455 none none none)").unwrap();
    println!("输入:   hwb(from #bad455 none none none)");
    println!("中间判断: 三个分量全部缺失 -> 解析为 h=0 w=0 b=0,而不是继承 origin");
    println!("结果:   hwb(0 0% 0%) = 纯红 {:?}", c.to_rgba8());
    assert_eq!(c.to_rgba8(), [255, 0, 0, 255]);

    // Alpha behaves the same way: `none` -> 0.0, keyword -> inherited.
    let a_none = parse("rgb(from #bad45580 r g b / none)").unwrap();
    let a_kw = parse("rgb(from #bad45580 r g b / alpha)").unwrap();
    let a_default = parse("rgb(from #bad45580 r g b)").unwrap();
    println!("输入:   rgb(from #bad45580 r g b / none|alpha|缺省)");
    println!(
        "结果:   none -> alpha {}, alpha 关键字 -> {}, 缺省 -> {}",
        a_none.to_rgba8()[3],
        a_kw.to_rgba8()[3],
        a_default.to_rgba8()[3]
    );
    assert_eq!(a_none.to_rgba8()[3], 0);
    assert_eq!(a_kw.to_rgba8()[3], 0x80);
    assert_eq!(a_default.to_rgba8()[3], 0x80);

    // A `none` inside the *origin* color is resolved when the origin is
    // parsed, so the inherited keyword already sees 0, not "missing".
    let c = parse("hsl(from hsl(none 100% 50%) h s l)").unwrap();
    println!("输入:   hsl(from hsl(none 100% 50%) h s l)");
    println!("中间判断: origin 的 none hue 在 parse origin 时已变成 0.0,");
    println!("        继承下来的 h 看到的是 0 度,而不是\"缺失\"本身");
    println!("结果:   hsl(0 100% 50%) = {:?}", c.to_rgba8());
    assert_eq!(c.to_rgba8(), [255, 0, 0, 255]);
}
