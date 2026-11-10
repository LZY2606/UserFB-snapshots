//! DSP 合同测试：逆滤波、主峰对齐、闭区间窗口与半开切片换算、
//! 衰减曲线、漂移检测、丢帧与零值/不完整分类。
use acoustic_pulse::dsp::{
    analyze_candidate, classify_frame, deconvolve, drift_ppm, drop_flags, peak_index,
    reflection_picks, schroeder_decay, FrameKind, SweepSpec, Window,
};
use acoustic_pulse::fft::{next_pow2, rfft_padded, Cplx};

fn spec() -> SweepSpec {
    SweepSpec {
        sample_rate: 16000,
        f0: 80.0,
        f1: 6000.0,
        duration_s: 0.5,
        amplitude: 0.8,
    }
}

struct Rng(u64);
impl Rng {
    fn next_f64(&mut self) -> f64 {
        let mut x = self.0;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        self.0 = x;
        x as f64 / u64::MAX as f64 - 0.5
    }
}

fn convolve(x: &[f64], h: &[f64], out_len: usize) -> Vec<f64> {
    let n = next_pow2(x.len() + h.len() - 1);
    let xf = rfft_padded(x, n);
    let hf = rfft_padded(h, n);
    let prod: Vec<Cplx> = xf.iter().zip(&hf).map(|(a, b)| a.mul(*b)).collect();
    acoustic_pulse::fft::ifft(&prod)
        .iter()
        .take(out_len)
        .map(|c| c.re)
        .collect()
}

fn resample(sig: &[f64], ratio: f64) -> Vec<f64> {
    let m = (sig.len() as f64 / ratio) as usize;
    let mut out = Vec::with_capacity(m);
    for i in 0..m {
        let t = i as f64 * ratio;
        let j = t as usize;
        let f = t - j as f64;
        let a = sig[j];
        let b = sig.get(j + 1).copied().unwrap_or(a);
        out.push(a * (1.0 - f) + b * f);
    }
    out
}

fn scene() -> (SweepSpec, Vec<f64>, Vec<f64>) {
    let sp = spec();
    let total = 12095usize;
    let sweep = sp.generate(total);
    let mut rng = Rng(0x1234_5678);
    let ir_len = 4096usize;
    let mut ir = vec![0.0f64; ir_len];
    ir[200] = 1.0;
    ir[290] = 0.6;
    ir[410] = 0.35;
    for i in 600..ir_len {
        ir[i] = 0.02 * rng.next_f64() * (-((i - 600) as f64) / 900.0).exp();
    }
    let mut rec = convolve(&sweep, &ir, total);
    for v in rec.iter_mut() {
        *v += 0.004 * rng.next_f64();
    }
    (sp, sweep, rec)
}

#[test]
fn closed_interval_endpoint_arithmetic() {
    // 输入、存储、展示统一闭区间 [start,end]，样本数 end-start+1
    let w = Window::new(100, 200);
    assert_eq!(w.sample_count(), 101);
    assert_eq!(w.half_open(), 100..201, "内部半开切片必须换算为 [start,end+1)");
    let data: Vec<f64> = (0..500).map(|i| i as f64).collect();
    let seg = w.slice(&data);
    assert_eq!(seg.len(), 101);
    assert_eq!(seg.first(), Some(&100.0));
    assert_eq!(seg.last(), Some(&200.0), "闭区间末端样本必须包含在内");
    assert!(Window::new(10, 9).validate().is_err());
}

#[test]
fn inverse_filter_and_peak_alignment() {
    let (_, sweep, rec) = scene();
    let raw = deconvolve(&rec, &sweep, 1e-3);
    assert_eq!(peak_index(&raw), 200, "逆滤波主峰必须对齐直达声样本 200");

    let w = Window::new(200, 2200);
    let cand = analyze_candidate("基线", &rec, &sweep, 1e-3, w).unwrap();
    assert_eq!(cand.peak_index, 200);
    assert_eq!(cand.offset_correction, 200, "偏移校正以主峰为基准");
    let picks = reflection_picks(&cand.impulse, cand.peak_index, 30, 400, 2);
    assert!(picks.contains(&290) && picks.contains(&410), "反射拾取 {picks:?}");
}

#[test]
fn two_candidates_are_both_retained() {
    let (_, sweep, rec) = scene();
    let w = Window::new(200, 2200);
    let a = analyze_candidate("A", &rec, &sweep, 1e-3, w).unwrap();
    let b = analyze_candidate("B", &rec, &sweep, 1e-6, w).unwrap();
    assert_eq!(a.peak_index, b.peak_index);
    let differ = a.impulse.iter().zip(&b.impulse).any(|(x, y)| (x - y).abs() > 1e-9);
    assert!(differ, "两个正则强度必须产生不同候选，且同时保留");
}

#[test]
fn schroeder_curve_uses_closed_interval() {
    let (_, sweep, rec) = scene();
    let w = Window::new(200, 2200);
    let db = schroeder_decay(&deconvolve(&rec, &sweep, 1e-3), w).unwrap();
    assert_eq!(db.len(), w.sample_count(), "曲线点数必须等于闭区间样本数");
    assert!(db[0].abs() < 1e-6, "曲线起点归一到 0 dB");
    for pair in db.windows(2) {
        assert!(pair[1] <= pair[0] + 1e-9, "Schroeder 曲线必须单调不增");
    }
    assert!(*db.last().unwrap() < -40.0);
}

#[test]
fn clock_drift_is_detected_with_sign() {
    let (sp, sweep, rec) = scene();
    let drifted = resample(&rec, 1.0 + 500e-6);
    let ppm = drift_ppm(&drifted, &sweep, sp.sweep_len(), 10, 1500, 3000, 200);
    assert!((200.0..800.0).contains(&ppm), "500ppm 漂移实测 {ppm}");
    let clean = drift_ppm(&rec, &sweep, sp.sweep_len(), 10, 1500, 3000, 200);
    assert!(clean.abs() < 150.0, "干净录音漂移估计应接近零，实测 {clean}");
}

#[test]
fn dropped_frame_is_flagged_without_false_positives() {
    let (_, _, rec) = scene();
    assert!(drop_flags(&rec, 100, 8.0).is_empty(), "干净录音零误报");
    let drifted = resample(&rec, 1.0005);
    assert!(drop_flags(&drifted, 100, 8.0).is_empty(), "单纯漂移不应被当成丢帧");
    let dropped: Vec<f64> = rec
        .iter()
        .take(3000)
        .chain(rec.iter().skip(3064))
        .copied()
        .collect();
    let flags = drop_flags(&dropped, 100, 8.0);
    assert!(flags.contains(&3000), "必须在 3000 处标出丢帧，实际 {flags:?}");
}

#[test]
fn zero_and_incomplete_are_different_classes() {
    // 不完整采样与数值为零不能混为一类
    assert_eq!(classify_frame(&[], 64), FrameKind::Incomplete);
    assert_eq!(classify_frame(&[0.0; 64], 64), FrameKind::Zero);
    assert_eq!(classify_frame(&[0.0, 0.0, 0.1], 64), FrameKind::Incomplete);
    assert_eq!(classify_frame(&vec![0.25; 64], 64), FrameKind::Complete);
}
