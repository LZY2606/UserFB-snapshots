//! 声学信号处理核心：扫频生成、逆滤波脉冲响应、主峰对齐、反射拾取、
//! Schroeder 能量衰减、时钟漂移估计与丢帧检测。
//!
//! 窗口端点在整个系统中统一为**从 0 开始的闭区间 [start, end]**：
//! 样本数 = `end - start + 1`；内部需要 Rust 半开切片时一律换算为
//! `start .. end + 1`。见 [`Window`]。

use crate::fft::{next_pow2, rfft_padded, Cplx};

/// 闭区间采样窗口。存储、输入、展示、测试都使用该类型；
/// 只有真正切片时才通过 [`Window::half_open`] 换算。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Window {
    pub start: usize,
    pub end: usize,
}

impl Window {
    pub fn new(start: usize, end: usize) -> Self {
        Window { start, end }
    }

    /// 校验闭区间合法性（end >= start）。
    pub fn validate(&self) -> Result<(), String> {
        if self.end >= self.start {
            Ok(())
        } else {
            Err(format!(
                "窗口端点非法：闭区间 [{}, {}] 中 end 小于 start",
                self.start, self.end
            ))
        }
    }

    /// 闭区间样本数：end - start + 1。
    pub fn sample_count(&self) -> usize {
        self.end - self.start + 1
    }

    /// 换算为 Rust 半开区间 start .. end+1，仅用于切片。
    pub fn half_open(&self) -> std::ops::Range<usize> {
        self.start..(self.end + 1)
    }

    /// 按闭区间从序列取片段（内部用半开切片实现）。
    pub fn slice<'a>(&self, data: &'a [f64]) -> &'a [f64] {
        let r = self.half_open();
        &data[r.start.min(data.len())..r.end.min(data.len())]
    }
}

/// 扫频激励参数（对数正弦扫频 exponential swept-sine）。
#[derive(Debug, Clone)]
pub struct SweepSpec {
    pub sample_rate: u32,
    pub f0: f64,
    pub f1: f64,
    pub duration_s: f64,
    pub amplitude: f64,
}

impl SweepSpec {
    pub fn sweep_len(&self) -> usize {
        (self.sample_rate as f64 * self.duration_s).round() as usize
    }

    /// 生成激励信号；超过时长的部分补零到 total_len。
    pub fn generate(&self, total_len: usize) -> Vec<f64> {
        let n = self.sweep_len();
        let k = (self.f1 / self.f0).ln() / self.duration_s;
        let sr = self.sample_rate as f64;
        let mut out = vec![0.0f64; total_len];
        for i in 0..n.min(total_len) {
            let t = i as f64 / sr;
            let phase = 2.0 * std::f64::consts::PI * self.f0 * ((k * t).exp() - 1.0) / k;
            out[i] = self.amplitude * phase.sin();
        }
        out
    }
}

/// 逆滤波候选结果。两个不同正则强度各产生一个候选，可同时保留审议。
#[derive(Debug, Clone)]
pub struct Candidate {
    pub label: String,
    pub regularization: f64,
    /// 候选脉冲响应（与录音同长度的 FFT 网格，前导零保留时间位置）。
    pub impulse: Vec<f64>,
    /// 主峰（直达声）在 impulse 中的样本索引。
    pub peak_index: usize,
    /// 主峰对齐时施加的整体偏移校正（样本）；
    /// 对齐后主峰逻辑位置 = peak_index - offset_correction。
    pub offset_correction: i64,
    /// 早期反射到达时间（样本索引，绝对坐标）。
    pub reflections: Vec<usize>,
    /// 分析窗口内的 Schroeder 衰减曲线（dB），按闭区间逐点。
    pub decay_db: Vec<f64>,
    /// 衰减曲线使用的闭区间窗口。
    pub decay_window: Window,
}

/// 扫频逆滤波：频域正则化逆滤波器 H = Y·conj(X) / (|X|² + λ·peak|X|²)。
/// 返回完整网格长度的脉冲响应。
pub fn deconvolve(recording: &[f64], sweep: &[f64], regularization: f64) -> Vec<f64> {
    let n = next_pow2(recording.len().max(sweep.len()));
    let y = rfft_padded(recording, n);
    let x = rfft_padded(sweep, n);
    let peak_energy = x.iter().map(|c| c.norm_sq()).fold(0.0f64, f64::max);
    let floor = regularization * peak_energy;
    let mut h = vec![Cplx::new(0.0, 0.0); n];
    for i in 0..n {
        let denom = x[i].norm_sq() + floor;
        // Y * conj(X) / denom
        let num = y[i].mul(x[i].conj());
        h[i] = Cplx::new(num.re / denom, num.im / denom);
    }
    crate::fft::ifft(&h).iter().map(|c| c.re).collect()
}

/// 绝对坐标下幅度最大的样本索引。
pub fn peak_index(impulse: &[f64]) -> usize {
    let mut best = 0usize;
    let mut best_v = -1.0f64;
    for (i, v) in impulse.iter().enumerate() {
        let a = v.abs();
        if a > best_v {
            best_v = a;
            best = i;
        }
    }
    best
}

/// 拾取主峰之后的早期反射：在 [peak+min_gap, peak+search] 内找局部极大，
/// 按幅度排序取前 top 个，再按到达时间升序返回。
pub fn reflection_picks(impulse: &[f64], peak: usize, min_gap: usize, search: usize, top: usize) -> Vec<usize> {
    let lo = peak + min_gap;
    let hi = (peak + search).min(impulse.len().saturating_sub(1));
    let mut cands: Vec<(f64, usize)> = Vec::new();
    if lo >= impulse.len() || hi <= lo {
        return cands.iter().map(|(_, i)| *i).collect();
    }
    for i in lo..hi {
        if i == 0 || i + 1 >= impulse.len() {
            continue;
        }
        let a = impulse[i].abs();
        if a > impulse[i - 1].abs() && a >= impulse[i + 1].abs() {
            cands.push((a, i));
        }
    }
    cands.sort_by(|a, b| b.0.partial_cmp(&a.0).unwrap_or(std::cmp::Ordering::Equal));
    cands.truncate(top);
    let mut picks: Vec<usize> = cands.iter().map(|(_, i)| *i).collect();
    picks.sort_unstable();
    picks
}

/// Schroeder 反向积分能量衰减曲线，严格按闭区间 [s, e] 逐点计算，
/// 输出 dB，曲线单调不增。
pub fn schroeder_decay(impulse: &[f64], window: Window) -> Result<Vec<f64>, String> {
    window.validate()?;
    if window.end >= impulse.len() {
        return Err(format!(
            "衰减窗口 [{}, {}] 超出脉冲响应长度 {}",
            window.start, window.end, impulse.len()
        ));
    }
    let seg = window.slice(impulse); // 闭区间 -> 半开切片
    let total: f64 = seg.iter().map(|v| v * v).sum();
    if total <= 0.0 {
        return Err("衰减窗口内能量为零，无法计算 Schroeder 曲线".into());
    }
    let n = seg.len(); // = end-start+1
    let mut db = vec![0.0f64; n];
    let mut acc = 0.0f64;
    for k in (0..n).rev() {
        acc += seg[k] * seg[k];
        db[k] = 10.0 * (acc / total).log10();
    }
    Ok(db)
}

/// 生成一个完整候选：逆滤波 + 主峰对齐 + 反射 + 衰减。
pub fn analyze_candidate(
    label: &str,
    recording: &[f64],
    sweep: &[f64],
    regularization: f64,
    decay_window: Window,
) -> Result<Candidate, String> {
    let impulse = deconvolve(recording, sweep, regularization);
    let peak = peak_index(&impulse);
    let reflections = reflection_picks(&impulse, peak, 30, 400, 2);
    let decay_db = schroeder_decay(&impulse, decay_window)?;
    Ok(Candidate {
        label: label.to_string(),
        regularization,
        impulse,
        peak_index: peak,
        // 偏移校正以主峰为基准：把主峰校正到时间轴零点参考
        offset_correction: peak as i64,
        reflections,
        decay_db,
        decay_window,
    })
}

// ---------- 时钟漂移 ----------

/// 局部互相关：在 rec 的 [center-R, center+R] 内滑动模板 win，
/// 返回相关峰的亚样本位置（抛物线插值）。
fn local_lag(rec: &[f64], win: &[f64], center: usize, radius: usize) -> f64 {
    let mut best: isize = center as isize;
    let mut best_v = f64::NEG_INFINITY;
    let start = (center as isize - radius as isize).max(0) as usize;
    let end = (center + radius).min(rec.len().saturating_sub(win.len()));
    if start > end || win.is_empty() {
        // 模板超出录音范围：放弃插值，直接返回中心位置，避免 NaN 污染回归
        return center as f64;
    }
    for s in start..=end {
        let mut v = 0.0f64;
        for i in 0..win.len() {
            v += win[i] * rec[s + i];
        }
        if v > best_v {
            best_v = v;
            best = s as isize;
        }
    }
    let corr = |s: isize| -> f64 {
        if s < 0 || s as usize + win.len() > rec.len() {
            return f64::NEG_INFINITY;
        }
        let s = s as usize;
        win.iter().zip(&rec[s..s + win.len()]).map(|(a, b)| a * b).sum()
    };
    let v0 = corr(best - 1);
    let v2 = corr(best + 1);
    let denom = v0 - 2.0 * best_v + v2;
    let sub = if denom != 0.0 {
        0.5 * (v0 - v2) / denom
    } else {
        0.0
    };
    best as f64 + sub
}

/// 时钟漂移估计：在扫频高频段取多个锚点，局部互相关定位其在录音中的
/// 实际位置，对 (模板位置, 录音位置) 做最小二乘线性回归，
/// 斜率 ≈ 1/(1+ε)，由此换算 ppm。正 ppm 表示录音时钟偏快。
pub fn drift_ppm(
    recording: &[f64],
    sweep: &[f64],
    sweep_len: usize,
    anchors: usize,
    win_len: usize,
    p_lo: usize,
    room_delay_hint: usize,
) -> f64 {
    let p_hi = sweep_len.saturating_sub(win_len);
    let mut pos = Vec::with_capacity(anchors);
    let mut lag = Vec::with_capacity(anchors);
    let mut prev: Option<(f64, f64)> = None;
    for k in 0..anchors {
        let p = if anchors == 1 {
            p_lo
        } else {
            p_lo + k * (p_hi - p_lo) / (anchors - 1)
        };
        let template: Vec<f64> = sweep[p..p + win_len].to_vec();
        let center = match prev {
            None => p + room_delay_hint,
            Some((pp, gg)) => (gg + (p as f64 - pp)).round() as usize,
        };
        let g = local_lag(recording, &template, center, 300);
        pos.push(p as f64);
        lag.push(g);
        prev = Some((p as f64, g));
    }
    let mx = pos.iter().sum::<f64>() / pos.len() as f64;
    let my = lag.iter().sum::<f64>() / lag.len() as f64;
    let num: f64 = pos.iter().zip(&lag).map(|(x, y)| (x - mx) * (y - my)).sum();
    let den: f64 = pos.iter().map(|x| (x - mx).powi(2)).sum();
    let slope = num / den;
    (1.0 / slope - 1.0) * 1e6
}

// ---------- 丢帧 ----------

/// 丢帧/不连续点检测：对一阶差分做局部窗口（±half）鲁棒统计，
/// 超过中位数 + k·MAD 且高于绝对下限的位置标记为异常。
/// 返回异常样本索引（从 0 开始）。
pub fn drop_flags(recording: &[f64], half: usize, k: f64) -> Vec<usize> {
    let n = recording.len();
    if n < 3 {
        return Vec::new();
    }
    let rms = (recording.iter().map(|v| v * v).sum::<f64>() / n as f64).sqrt();
    let floor = 0.05 * rms;
    let diffs: Vec<f64> = (0..n)
        .map(|i| if i == 0 { 0.0 } else { (recording[i] - recording[i - 1]).abs() })
        .collect();
    let mut flags = Vec::new();
    for i in 1..n {
        let lo = i.saturating_sub(half).max(1);
        let hi = (i + half + 1).min(n);
        let mut local: Vec<f64> = diffs[lo..hi].to_vec();
        local.sort_by(|a, b| a.partial_cmp(b).unwrap_or(std::cmp::Ordering::Equal));
        let med = local[local.len() / 2];
        let mut dev: Vec<f64> = local.iter().map(|v| (v - med).abs()).collect();
        dev.sort_by(|a, b| a.partial_cmp(b).unwrap_or(std::cmp::Ordering::Equal));
        let mad = dev[dev.len() / 2];
        if diffs[i] > med + k * mad.max(1e-6) && diffs[i] > floor {
            flags.push(i);
        }
    }
    flags
}

/// 帧完整性分类。零值与不完整采样严格区分：
/// - `Incomplete`：长度不足预期帧长（截断/空段），优先按长度判定；
/// - `Zero`：长度完整但整段数值全为 0（静默/丢数据）；
/// - `Complete`：长度与内容都正常。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FrameKind {
    Zero,
    Incomplete,
    Complete,
}

pub fn classify_frame(segment: &[f64], expected_frame: usize) -> FrameKind {
    if segment.len() < expected_frame {
        FrameKind::Incomplete
    } else if segment.iter().all(|v| *v == 0.0) {
        FrameKind::Zero
    } else {
        FrameKind::Complete
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn spec() -> SweepSpec {
        SweepSpec {
            sample_rate: 16000,
            f0: 80.0,
            f1: 6000.0,
            duration_s: 0.5,
            amplitude: 0.8,
        }
    }

    fn build_scene() -> (Vec<f64>, Vec<f64>) {
        // 确定性伪随机，避免测试依赖 rand crate
        let mut state = 0x1234_5678u64;
        let mut noise = || {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            (state as f64 / u64::MAX as f64) - 0.5
        };
        let sp = spec();
        let total = 12095;
        let sweep = sp.generate(total);
        // 房间脉冲响应：直达 + 两个早期反射 + 指数噪声尾
        let ir_len = 4096usize;
        let mut ir = vec![0.0f64; ir_len];
        ir[200] = 1.0;
        ir[290] = 0.6;
        ir[410] = 0.35;
        for i in 600..ir_len {
            ir[i] = 0.02 * noise() * (-((i - 600) as f64) / 900.0).exp();
        }
        // 线性卷积（FFT）
        let n = next_pow2(total + ir_len - 1);
        let s = rfft_padded(&sweep, n);
        let h = rfft_padded(&ir, n);
        let prod: Vec<Cplx> = s.iter().zip(&h).map(|(a, b)| a.mul(*b)).collect();
        let mut rec: Vec<f64> = crate::fft::ifft(&prod).iter().take(total).map(|c| c.re).collect();
        for v in rec.iter_mut() {
            *v += 0.004 * noise();
        }
        (sweep, rec)
    }

    #[test]
    fn closed_window_count_and_half_open() {
        let w = Window::new(10, 19);
        assert_eq!(w.sample_count(), 10);
        assert_eq!(w.half_open(), 10..20);
        let data: Vec<f64> = (0..100).map(|i| i as f64).collect();
        let seg = w.slice(&data);
        assert_eq!(seg.len(), 10);
        assert_eq!(seg[0], 10.0);
        assert_eq!(seg[9], 19.0);
        assert!(Window::new(5, 4).validate().is_err());
    }

    #[test]
    fn deconvolution_recovers_peak_and_reflections() {
        let (sweep, rec) = build_scene();
        let cand = analyze_candidate(
            "基线",
            &rec,
            &sweep,
            1e-3,
            Window::new(200, 2200),
        )
        .unwrap();
        assert_eq!(cand.peak_index, 200, "主峰必须对齐到直达声样本 200");
        assert!(
            cand.reflections.contains(&290) && cand.reflections.contains(&410),
            "应拾取两个早期反射 290/410，实际 {:?}",
            cand.reflections
        );
    }

    #[test]
    fn two_regularizations_kept_distinct() {
        let (sweep, rec) = build_scene();
        let a = analyze_candidate("A", &rec, &sweep, 1e-3, Window::new(200, 2200)).unwrap();
        let b = analyze_candidate("B", &rec, &sweep, 1e-6, Window::new(200, 2200)).unwrap();
        assert_ne!(a.regularization, b.regularization);
        // 两个候选主峰一致，但脉冲响应数值不同（正则强度不同）
        assert_eq!(a.peak_index, b.peak_index);
        assert!(a.impulse.iter().zip(&b.impulse).any(|(x, y)| (x - y).abs() > 1e-9));
    }

    #[test]
    fn decay_is_monotonic_and_closed_interval_sized() {
        let (sweep, rec) = build_scene();
        let w = Window::new(200, 2200);
        let cand = analyze_candidate("基线", &rec, &sweep, 1e-3, w).unwrap();
        assert_eq!(cand.decay_db.len(), w.sample_count());
        for pair in cand.decay_db.windows(2) {
            assert!(pair[1] <= pair[0] + 1e-9, "Schroeder 曲线必须单调不增");
        }
        assert!(cand.decay_db[0].abs() < 1e-6);
        assert!(cand.decay_db.last().unwrap() < &-40.0);
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

    #[test]
    fn drift_detection_sign_and_magnitude() {
        let (sweep, rec) = build_scene();
        let sp = spec();
        let drifted = resample(&rec, 1.0 + 500e-6); // 录音时钟偏快 500ppm
        let ppm = drift_ppm(&drifted, &sweep, sp.sweep_len(), 10, 1500, 3000, 200);
        assert!(ppm > 200.0 && ppm < 800.0, "500ppm 漂移实测 {ppm}");
        let ppm0 = drift_ppm(&rec, &sweep, sp.sweep_len(), 10, 1500, 3000, 200);
        assert!(ppm0.abs() < 150.0, "干净数据漂移应接近 0，实测 {ppm0}");
    }

    #[test]
    fn drop_detection_localizes_frame_loss() {
        let (_, rec) = build_scene();
        let clean = drop_flags(&rec, 100, 8.0);
        assert!(clean.is_empty(), "干净录音不应有丢帧标记，实际 {clean:?}");
        let dropped: Vec<f64> = rec.iter().take(3000).chain(rec.iter().skip(3064)).cloned().collect();
        let flags = drop_flags(&dropped, 100, 8.0);
        assert!(flags.contains(&3000), "应在 3000 处标出丢帧，实际 {flags:?}");
    }

    #[test]
    fn zero_vs_incomplete_are_distinct() {
        assert_eq!(classify_frame(&[0.0; 10], 10), FrameKind::Zero, "完整长度的全零帧才算 Zero");
        assert_eq!(classify_frame(&[0.1, 0.2, 0.3], 64), FrameKind::Incomplete);
        assert_eq!(classify_frame(&[0.0; 64], 64), FrameKind::Zero);
        assert_eq!(classify_frame(&vec![0.5; 64], 64), FrameKind::Complete);
    }
}
