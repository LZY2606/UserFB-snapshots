//! 手写基-2 迭代 FFT（Cooley–Tukey，原地蝶形），仅依赖标准库。
//! 输入长度必须是 2 的幂；逆变换通过共轭 trick 实现。

#[derive(Clone, Copy, Debug)]
pub struct Cplx {
    pub re: f64,
    pub im: f64,
}

impl Cplx {
    pub const fn new(re: f64, im: f64) -> Self {
        Cplx { re, im }
    }
    pub fn mul(self, o: Cplx) -> Cplx {
        Cplx {
            re: self.re * o.re - self.im * o.im,
            im: self.re * o.im + self.im * o.re,
        }
    }
    pub fn conj(self) -> Cplx {
        Cplx {
            re: self.re,
            im: -self.im,
        }
    }
    pub fn norm_sq(self) -> f64 {
        self.re * self.re + self.im * self.im
    }
}

/// 向上取整到最近的 2 的幂。
pub fn next_pow2(n: usize) -> usize {
    let mut p = 1usize;
    while p < n {
        p <<= 1;
    }
    p
}

/// 原地基-2 FFT。`a.len()` 必须是 2 的幂。
pub fn fft(a: &mut [Cplx]) {
    let n = a.len();
    debug_assert!(n.is_power_of_two());
    // 位反转置换
    let mut j = 0usize;
    for i in 1..n {
        let mut bit = n >> 1;
        while j & bit != 0 {
            j ^= bit;
            bit >>= 1;
        }
        j ^= bit;
        if i < j {
            a.swap(i, j);
        }
    }
    let mut len = 2usize;
    while len <= n {
        let ang = -2.0 * std::f64::consts::PI / (len as f64);
        let w = Cplx::new(ang.cos(), ang.sin());
        let half = len / 2;
        let mut i = 0;
        while i < n {
            let mut wn = Cplx::new(1.0, 0.0);
            for k in 0..half {
                let u = a[i + k];
                let v = a[i + k + half].mul(wn);
                a[i + k] = Cplx::new(u.re + v.re, u.im + v.im);
                a[i + k + half] = Cplx::new(u.re - v.re, u.im - v.im);
                wn = wn.mul(w);
            }
            i += len;
        }
        len <<= 1;
    }
}

/// 逆 FFT，返回长度不变的时域序列。
pub fn ifft(a: &[Cplx]) -> Vec<Cplx> {
    let mut b: Vec<Cplx> = a.iter().map(|c| c.conj()).collect();
    fft(&mut b);
    let n = b.len() as f64;
    b.iter().map(|c| Cplx::new(c.re / n, -c.im / n)).collect()
}

/// 把实数序列零填充到 n（2 的幂）后做 FFT。
pub fn rfft_padded(x: &[f64], n: usize) -> Vec<Cplx> {
    debug_assert!(n.is_power_of_two());
    let mut buf = vec![Cplx::new(0.0, 0.0); n];
    for (i, v) in x.iter().enumerate() {
        buf[i] = Cplx::new(*v, 0.0);
    }
    fft(&mut buf);
    buf
}
