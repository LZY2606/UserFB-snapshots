//! HTTP 冒烟测试：起一个真实的回环服务，走原生 TCP 发请求。
//!
//! 在禁止本地端口绑定的沙箱里（EPERM）自动跳过；
//! 在普通开发机上会实际验证页面与 API 端到端可用。

use std::io::{Read, Write};
use std::net::TcpStream;
use std::time::Duration;

use kerbdiag::db::Db;
use kerbdiag::server::App;

fn http_request(addr: &std::net::SocketAddr, raw: &str) -> Option<String> {
    let mut stream = TcpStream::connect(addr).ok()?;
    stream.set_read_timeout(Some(Duration::from_secs(5))).ok()?;
    stream.write_all(raw.as_bytes()).ok()?;
    let mut buf = String::new();
    stream.read_to_string(&mut buf).ok()?;
    Some(buf)
}

#[test]
fn http_end_to_end_smoke() {
    let dir = tempfile::tempdir().unwrap();
    let db_path = dir.path().join("smoke.sqlite3");
    let db = Db::open(db_path.to_str().unwrap()).unwrap();
    let app = App::new(db);

    // 端口 0 = 让系统分配；无绑定权限（受限沙箱）时跳过本测试。
    let server = match App::bind("127.0.0.1:0") {
        Ok(s) => s,
        Err(e) => {
            eprintln!("跳过 HTTP 冒烟测试：当前环境禁止本地端口绑定（{e}）");
            return;
        }
    };
    let addr = server.server_addr().to_ip().expect("应为 IP 监听地址");
    let worker = std::thread::spawn(move || app.serve(server));

    // 1. 首页 HTML 必须包含 Ticket lineage 板块。
    let html = http_request(&addr, "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
        .expect("连接首页失败");
    assert!(html.starts_with("HTTP/1.1 200"));
    assert!(html.contains("Ticket lineage"));

    // 2. 加载样例。
    let loaded = http_request(
        &addr,
        "POST /api/load-sample HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
    )
    .expect("加载样例失败");
    assert!(loaded.starts_with("HTTP/1.1 200"));
    assert!(loaded.contains("\"diagnoses\":4"), "四组样例应产生 4 条失败结论: {loaded}");

    // 3. 全局状态。
    let state = http_request(&addr, "GET /api/state HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
        .expect("获取状态失败");
    assert!(state.contains("circular_referral"));
    assert!(state.contains("referral_loop"));
    assert!(state.contains("replay"));

    // 4. 记录详情里必须有谱系与证据。
    let detail = http_request(
        &addr,
        "GET /api/records/1 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
    )
    .expect("获取记录详情失败");
    assert!(detail.starts_with("HTTP/1.1 200"));
    assert!(detail.contains("ancestors") || detail.contains("descendants"));

    // 5. 导出接口有数据。
    let exported = http_request(&addr, "GET /api/export HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
        .expect("导出失败");
    assert!(exported.contains("\"records\"") && exported.contains("\"keys\""));

    drop(worker); // 服务线程随测试结束退出
}
