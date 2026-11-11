//! kerbdiag 入口：离线 Kerberos 跨 realm 认证故障排查工具。
//!
//! 全程不连接任何在线服务：SQLite 本地存储，HTTP 仅监听回环地址，
//! 页面与 API 都只读写本地数据库。

use kerbdiag::db::Db;
use kerbdiag::server::App;

struct Args {
    addr: String,
    db_path: String,
}

fn parse_args() -> Args {
    let mut args = Args {
        addr: "127.0.0.1:5530".into(),
        db_path: "kerbdiag.sqlite3".into(),
    };
    let mut it = std::env::args().skip(1);
    while let Some(arg) = it.next() {
        match arg.as_str() {
            "--addr" => {
                if let Some(v) = it.next() {
                    args.addr = v;
                }
            }
            "--db" => {
                if let Some(v) = it.next() {
                    args.db_path = v;
                }
            }
            "-h" | "--help" => {
                println!("用法: kerbdiag [--addr 127.0.0.1:5530] [--db kerbdiag.sqlite3]");
                std::process::exit(0);
            }
            "--" => {} // cargo run 的参数分隔符，静默忽略
            other => eprintln!("忽略未知参数: {other}"),
        }
    }
    args
}

fn main() {
    let args = parse_args();
    let db = Db::open(&args.db_path).unwrap_or_else(|e| {
        eprintln!("打开数据库 {} 失败: {e}", args.db_path);
        std::process::exit(1);
    });
    App::new(db).run(&args.addr).unwrap_or_else(|e| {
        eprintln!("HTTP 服务启动失败: {e}");
        std::process::exit(1);
    });
}
