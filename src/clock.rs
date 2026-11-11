use std::time::{SystemTime, UNIX_EPOCH};

/// 所有时间来源都经过此 trait,测试可注入固定或序列时钟,不依赖真实等待。
pub trait Clock: Send + Sync {
    fn now_ms(&self) -> i64;
}

#[derive(Debug, Clone, Copy, Default)]
pub struct SystemClock;

impl Clock for SystemClock {
    fn now_ms(&self) -> i64 {
        SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map(|d| d.as_millis() as i64)
            .unwrap_or(0)
    }
}

/// 固定时钟:每次调用返回同一时刻。
#[derive(Debug, Clone, Copy)]
pub struct FixedClock(pub i64);

impl Clock for FixedClock {
    fn now_ms(&self) -> i64 {
        self.0
    }
}

/// 序列时钟:按预设序列依次返回,用尽后重复最后一个值。
pub struct SeqClock {
    values: Vec<i64>,
    idx: std::sync::Mutex<usize>,
}

impl SeqClock {
    pub fn new(values: Vec<i64>) -> Self {
        SeqClock {
            values,
            idx: std::sync::Mutex::new(0),
        }
    }
}

impl Clock for SeqClock {
    fn now_ms(&self) -> i64 {
        let mut i = self.idx.lock().unwrap();
        let idx = (*i).min(self.values.len() - 1);
        let v = self.values[idx];
        if *i < self.values.len() - 1 {
            *i += 1;
        }
        v
    }
}
