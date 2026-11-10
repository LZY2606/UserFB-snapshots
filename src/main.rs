//! 声场脉络二进制入口。
//!
//! 运行：`cargo run --locked -- --addr 127.0.0.1:5251`
//! 整个工程零第三方 crate：SQLite 经 FFI 直链系统库，
//! HTTP/JSON/SHA-256/FFT 均为手写实现，可完全离线构建。

use std::path::PathBuf;
use std::sync::Arc;

use acoustic_pulse::db::Db;
use acoustic_pulse::error::AppResult;
use acoustic_pulse::service::{self, AppState};

struct Args {
    addr: String,
    db_path: PathBuf,
    fixture_dir: PathBuf,
}

fn parse_args() -> Args {
    let mut addr = "127.0.0.1:5251".to_string();
    let mut db_path = PathBuf::from("data/acoustic-pulse.db");
    let mut fixture_dir = PathBuf::from("fixtures");
    let mut it = std::env::args().skip(1);
    while let Some(arg) = it.next() {
        match arg.as_str() {
            "--addr" => {
                if let Some(v) = it.next() {
                    addr = v;
                }
            }
            "--db" => {
                if let Some(v) = it.next() {
                    db_path = PathBuf::from(v);
                }
            }
            "--fixtures" => {
                if let Some(v) = it.next() {
                    fixture_dir = PathBuf::from(v);
                }
            }
            "-h" | "--help" => {
                println!(
                    "声场脉络 acoustic-pulse\n\n\
                     USAGE:\n  acoustic-pulse [--addr 127.0.0.1:5251] [--db data/acoustic-pulse.db] \
                     [--fixtures fixtures]\n\n\
                     波形只从本地 fixture 目录导入，服务不提供任何上传接口。"
                );
                std::process::exit(0);
            }
            other => {
                eprintln!("忽略未知参数: {other}");
            }
        }
    }
    Args {
        addr,
        db_path,
        fixture_dir,
    }
}

fn main() -> AppResult<()> {
    let args = parse_args();
    if let Some(parent) = args.db_path.parent() {
        if !parent.as_os_str().is_empty() {
            std::fs::create_dir_all(parent)?;
        }
    }
    std::fs::create_dir_all(&args.fixture_dir)?;

    let mut db = Db::open(&args.db_path.to_string_lossy())?;
    acoustic_pulse::storage::migrate(&mut db)?;

    let state = Arc::new(AppState::new(db, args.fixture_dir));
    service::bootstrap(&state)?;
    println!(
        "fixture 目录: {}（波形仅本地导入，运行时不上传）",
        state.fixture_dir.display()
    );
    acoustic_pulse::http::serve(state, &args.addr)
}
