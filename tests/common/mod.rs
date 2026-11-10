//! 集成测试公共辅助：临时目录、服务启动、极简 HTTP 客户端。
#![allow(dead_code)]
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::path::PathBuf;
use std::sync::Arc;
use std::time::{SystemTime, UNIX_EPOCH};

use acoustic_pulse::db::Db;
use acoustic_pulse::service::{self, AppState};

pub struct TestEnv {
    pub root: PathBuf,
    pub addr: String,
    pub state: Arc<AppState>,
}

impl TestEnv {
    pub fn new() -> TestEnv {
        let nanos = SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_nanos();
        let unique = format!(
            "acoustic-pulse-test-{}-{}-{}",
            std::process::id(),
            nanos,
            COUNTER.fetch_add(1, std::sync::atomic::Ordering::SeqCst)
        );
        let root = std::env::temp_dir().join(unique);
        let fixture_dir = root.join("fixtures");
        let db_path = root.join("test.db");
        std::fs::create_dir_all(&fixture_dir).unwrap();

        let mut db = Db::open(&db_path.to_string_lossy()).unwrap();
        acoustic_pulse::storage::migrate(&mut db).unwrap();
        let state = Arc::new(AppState::new(db, fixture_dir));
        service::bootstrap(&state).unwrap();

        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = listener.local_addr().unwrap().to_string();
        let st = Arc::clone(&state);
        std::thread::spawn(move || {
            let _ = acoustic_pulse::http::serve_on(listener, st);
        });
        std::thread::sleep(std::time::Duration::from_millis(80));
        TestEnv { root, addr, state }
    }
}

static COUNTER: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);

impl Drop for TestEnv {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.root);
    }
}

pub struct HttpResponse {
    pub status: u16,
    pub body: String,
}

pub fn request(env: &TestEnv, method: &str, path: &str, body: Option<&str>) -> HttpResponse {
    let mut stream = TcpStream::connect(&env.addr).unwrap();
    let mut req = format!("{method} {path} HTTP/1.1\r\nHost: {}\r\nConnection: close\r\n", env.addr);
    if let Some(b) = body {
        req.push_str(&format!("Content-Type: application/json\r\nContent-Length: {}\r\n", b.len()));
    }
    req.push_str("\r\n");
    if let Some(b) = body {
        req.push_str(b);
    }
    stream.write_all(req.as_bytes()).unwrap();
    let mut raw = Vec::new();
    stream.read_to_end(&mut raw).unwrap();
    let text = String::from_utf8_lossy(&raw);
    let (head, body_part) = text.split_once("\r\n\r\n").unwrap_or((&text, ""));
    let status = head
        .lines()
        .next()
        .and_then(|l| l.split_whitespace().nth(1))
        .and_then(|s| s.parse().ok())
        .unwrap_or(0);
    HttpResponse {
        status,
        body: body_part.to_string(),
    }
}

pub fn json_get(env: &TestEnv, path: &str) -> acoustic_pulse::json::Json {
    let r = request(env, "GET", path, None);
    assert_eq!(r.status, 200, "GET {path} 失败: {}", r.body);
    acoustic_pulse::json::parse(&r.body).unwrap()
}

pub fn json_post(env: &TestEnv, path: &str, body: &str) -> (u16, acoustic_pulse::json::Json) {
    let r = request(env, "POST", path, Some(body));
    let parsed = acoustic_pulse::json::parse(&r.body).unwrap_or(acoustic_pulse::json::Json::Null);
    (r.status, parsed)
}
