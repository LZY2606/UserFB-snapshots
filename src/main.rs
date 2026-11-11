use meshdesk::app::App;
use meshdesk::clock::SystemClock;
use meshdesk::http;
use meshdesk::store::Store;
use std::sync::Arc;

fn main() {
    let mut addr = "127.0.0.1:5270".to_string();
    let mut db = "meshdesk.db".to_string();
    let args: Vec<String> = std::env::args().collect();
    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "--addr" if i + 1 < args.len() => {
                addr = args[i + 1].clone();
                i += 2;
            }
            "--db" if i + 1 < args.len() => {
                db = args[i + 1].clone();
                i += 2;
            }
            other => {
                eprintln!("未知参数 {other};用法: meshdesk [--addr 127.0.0.1:5270] [--db meshdesk.db]");
                std::process::exit(2);
            }
        }
    }
    let store = Store::open(&db).unwrap_or_else(|e| {
        eprintln!("打开数据库 {db} 失败: {}", e.message);
        std::process::exit(1);
    });
    let app = Arc::new(App::new(store, Arc::new(SystemClock)).unwrap_or_else(|e| {
        eprintln!("初始化失败: {}", e.message);
        std::process::exit(1);
    }));
    if let Err(e) = http::serve(&addr, app) {
        eprintln!("服务启动失败: {e}");
        std::process::exit(1);
    }
}
