//! 本地 fixture：波形用自定义二进制容器 APWF 存储，校准数据用 JSON 文本。
//! 运行时只从本地目录按文件名读取，**不提供任何上传路径**。
//!
//! APWF 容器（小端）：
//! magic(10B) = `APWF0001\n`，u32 sample_rate，u32 channels，
//! u64 frame_count，随后 f64 交错采样。

use std::fs;
use std::path::{Path, PathBuf};

use crate::dsp::SweepSpec;
use crate::error::{AppError, AppResult};
use crate::fft::{next_pow2, rfft_padded, Cplx};
use crate::sha256::Sha256;

const MAGIC: &[u8; 10] = b"APWF0001\n";

#[derive(Debug, Clone)]
pub struct Waveform {
    pub sample_rate: u32,
    pub channels: u32,
    /// 交错采样；单通道时长度 = frame_count。
    pub samples: Vec<f64>,
}

impl Waveform {
    pub fn frame_count(&self) -> usize {
        if self.channels == 0 {
            0
        } else {
            self.samples.len() / self.channels as usize
        }
    }

    pub fn mono(&self) -> Vec<f64> {
        if self.channels <= 1 {
            return self.samples.clone();
        }
        let ch = self.channels as usize;
        (0..self.frame_count())
            .map(|f| self.samples[f * ch])
            .collect()
    }

    pub fn encode(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(26 + self.samples.len() * 8);
        out.extend_from_slice(MAGIC);
        out.extend_from_slice(&self.sample_rate.to_le_bytes());
        out.extend_from_slice(&self.channels.to_le_bytes());
        out.extend_from_slice(&(self.frame_count() as u64).to_le_bytes());
        for v in &self.samples {
            out.extend_from_slice(&v.to_le_bytes());
        }
        out
    }

    pub fn decode(bytes: &[u8]) -> AppResult<Waveform> {
        if bytes.len() < 26 || &bytes[..10] != MAGIC {
            return Err(AppError::Validation("APWF magic 不匹配".into()));
        }
        let sample_rate = u32::from_le_bytes(bytes[10..14].try_into().unwrap());
        let channels = u32::from_le_bytes(bytes[14..18].try_into().unwrap());
        let frames = u64::from_le_bytes(bytes[18..26].try_into().unwrap()) as usize;
        let need = frames * channels as usize;
        if bytes.len() < 26 + need * 8 {
            return Err(AppError::Validation("APWF 采样数据被截断".into()));
        }
        let mut samples = Vec::with_capacity(need);
        for i in 0..need {
            let off = 26 + i * 8;
            samples.push(f64::from_le_bytes(bytes[off..off + 8].try_into().unwrap()));
        }
        Ok(Waveform {
            sample_rate,
            channels,
            samples,
        })
    }
}

/// 确定性 xorshift 伪随机源，保证 fixture 在任何机器上逐字节一致。
pub struct Rng(u64);

impl Rng {
    pub fn new(seed: u64) -> Self {
        Rng(seed.max(1))
    }
    pub fn next_f64(&mut self) -> f64 {
        let mut x = self.0;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        self.0 = x;
        x as f64 / u64::MAX as f64 - 0.5
    }
}

pub fn default_spec() -> SweepSpec {
    SweepSpec {
        sample_rate: 16000,
        f0: 80.0,
        f1: 6000.0,
        duration_s: 0.5,
        amplitude: 0.8,
    }
}

/// 房间场景：直达声（样本 200）+ 两个早期反射（290、410）+ 指数噪声尾。
fn room_impulse(len: usize, rng: &mut Rng) -> Vec<f64> {
    let mut ir = vec![0.0f64; len];
    ir[200] = 1.0;
    ir[290] = 0.6;
    ir[410] = 0.35;
    for i in 600..len {
        ir[i] = 0.02 * rng.next_f64() * (-((i - 600) as f64) / 900.0).exp();
    }
    ir
}

fn convolve(x: &[f64], h: &[f64], out_len: usize) -> Vec<f64> {
    let n = next_pow2(x.len() + h.len() - 1);
    let xf = rfft_padded(x, n);
    let hf = rfft_padded(h, n);
    let prod: Vec<Cplx> = xf.iter().zip(&hf).map(|(a, b)| a.mul(*b)).collect();
    crate::fft::ifft(&prod)
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

/// 确保 fixture 目录存在且包含全部标准文件；已存在则保持原样（幂等）。
pub fn ensure_fixtures(dir: &Path) -> AppResult<Vec<String>> {
    fs::create_dir_all(dir)?;
    let spec = default_spec();
    let total = 12095usize;

    let mut made = Vec::new();

    // 1. 激励扫频
    let sweep_path = dir.join("excitation_sweep.apwf");
    if !sweep_path.exists() {
        let w = Waveform {
            sample_rate: spec.sample_rate,
            channels: 1,
            samples: spec.generate(total),
        };
        write_atomic(&sweep_path, &w.encode())?;
        made.push(file_name(&sweep_path));
    }

    // 2. 干净录音（直达 + 两个早期反射 + 噪声尾）
    let clean_path = dir.join("recording_clean.apwf");
    if !clean_path.exists() {
        let sweep = spec.generate(total);
        let mut rng = Rng::new(0x1234_5678);
        let ir = room_impulse(4096, &mut rng);
        let mut rec = convolve(&sweep, &ir, total);
        for v in rec.iter_mut() {
            *v += 0.004 * rng.next_f64();
        }
        write_atomic(
            &clean_path,
            &Waveform {
                sample_rate: spec.sample_rate,
                channels: 1,
                samples: rec,
            }
            .encode(),
        )?;
        made.push(file_name(&clean_path));
    }

    // 3. 时钟漂移录音（录音时钟偏快约 500 ppm）
    let drift_path = dir.join("recording_drift.apwf");
    if !drift_path.exists() {
        let clean = read_waveform(&clean_path)?.mono();
        let drifted = resample(&clean, 1.0 + 500e-6);
        write_atomic(
            &drift_path,
            &Waveform {
                sample_rate: spec.sample_rate,
                channels: 1,
                samples: drifted,
            }
            .encode(),
        )?;
        made.push(file_name(&drift_path));
    }

    // 4. 真正丢帧录音（在样本 3000 处丢失 64 个采样）
    let drop_path = dir.join("recording_dropped.apwf");
    if !drop_path.exists() {
        let clean = read_waveform(&clean_path)?.mono();
        let dropped: Vec<f64> = clean
            .iter()
            .take(3000)
            .chain(clean.iter().skip(3064))
            .copied()
            .collect();
        write_atomic(
            &drop_path,
            &Waveform {
                sample_rate: spec.sample_rate,
                channels: 1,
                samples: dropped,
            }
            .encode(),
        )?;
        made.push(file_name(&drop_path));
    }

    // 5. 两版麦克风/链路校准文件（JSON 文本，digest 不同）
    let cal1 = dir.join("cal_mic_v1.json");
    if !cal1.exists() {
        write_atomic(
            &cal1,
            br##"{
  "cal_code": "mic-chain-A",
  "version": 1,
  "microphone": "mems-reference-01",
  "preamp_gain_db": 24.0,
  "sensitivity_dbv_pa": -38.0,
  "frequency_response_hz_db": [
    [63, 0.2], [125, 0.1], [250, 0.0], [500, -0.1],
    [1000, 0.0], [2000, 0.1], [4000, -0.3], [8000, -0.8]
  ],
  "valid_from": "2026-01-01T00:00:00Z"
}
"##,
        )?;
        made.push(file_name(&cal1));
    }
    let cal2 = dir.join("cal_mic_v2.json");
    if !cal2.exists() {
        write_atomic(
            &cal2,
            br##"{
  "cal_code": "mic-chain-A",
  "version": 2,
  "microphone": "mems-reference-01",
  "preamp_gain_db": 23.5,
  "sensitivity_dbv_pa": -37.4,
  "frequency_response_hz_db": [
    [63, 0.3], [125, 0.2], [250, 0.1], [500, 0.0],
    [1000, 0.0], [2000, 0.0], [4000, -0.2], [8000, -0.6]
  ],
  "valid_from": "2026-06-01T00:00:00Z"
}
"##,
        )?;
        made.push(file_name(&cal2));
    }
    Ok(made)
}

fn file_name(p: &Path) -> String {
    p.file_name().map(|s| s.to_string_lossy().into_owned()).unwrap_or_default()
}

/// 同目录临时文件 + rename，保证 fixture 写入要么完整要么不存在。
fn write_atomic(path: &Path, bytes: &[u8]) -> AppResult<()> {
    let tmp: PathBuf = path.with_extension("tmp");
    fs::write(&tmp, bytes)?;
    fs::rename(&tmp, path)?;
    Ok(())
}

pub fn read_waveform(path: &Path) -> AppResult<Waveform> {
    let bytes = fs::read(path).map_err(|e| AppError::Io(format!("读取 fixture {} 失败: {e}", path.display())))?;
    Waveform::decode(&bytes)
}

/// 只允许读取 fixture 根目录内的普通文件名，阻断路径穿越。
pub fn resolve_within(root: &Path, name: &str) -> AppResult<PathBuf> {
    if name.is_empty()
        || name.contains('\0')
        || name.contains("..")
        || name.contains('/')
        || name.contains('\\')
    {
        return Err(AppError::Validation(format!("非法 fixture 文件名: {name}")));
    }
    Ok(root.join(name))
}

pub fn digest_file(path: &Path) -> AppResult<String> {
    let bytes = fs::read(path)?;
    let mut h = Sha256::new();
    h.update(&bytes);
    Ok(h.finish().iter().map(|b| format!("{b:02x}")).collect())
}
