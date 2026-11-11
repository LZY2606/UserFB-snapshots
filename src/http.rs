use crate::app::App;
use crate::model::{ApiError, ToleranceConfig};
use serde_json::{json, Value};
use std::collections::HashMap;
use std::io::{Read, Write};
use std::net::TcpListener;
use std::sync::Arc;

/// 启动 HTTP 服务(每连接一线程,仅本地,无外部依赖)。
pub fn serve(addr: &str, app: Arc<App>) -> std::io::Result<()> {
    let listener = TcpListener::bind(addr)?;
    eprintln!("网格修复证据台 listening on http://{addr}");
    for stream in listener.incoming() {
        match stream {
            Ok(s) => {
                let app = app.clone();
                std::thread::spawn(move || {
                    let _ = handle_conn(s, app);
                });
            }
            Err(e) => eprintln!("accept error: {e}"),
        }
    }
    Ok(())
}

fn handle_conn(mut stream: std::net::TcpStream, app: Arc<App>) -> std::io::Result<()> {
    let mut buf = Vec::new();
    let mut tmp = [0u8; 8192];
    let header_end;
    loop {
        let n = stream.read(&mut tmp)?;
        if n == 0 {
            return Ok(());
        }
        buf.extend_from_slice(&tmp[..n]);
        if let Some(pos) = find(&buf, b"\r\n\r\n") {
            header_end = pos;
            break;
        }
        if buf.len() > 1 << 20 {
            return Ok(());
        }
    }
    let head = String::from_utf8_lossy(&buf[..header_end]).to_string();
    let mut lines = head.lines();
    let req_line = lines.next().unwrap_or("");
    let mut parts = req_line.split_whitespace();
    let method = parts.next().unwrap_or("").to_string();
    let target = parts.next().unwrap_or("/").to_string();
    let mut headers: HashMap<String, String> = HashMap::new();
    for line in lines {
        if let Some((k, v)) = line.split_once(':') {
            headers.insert(k.trim().to_ascii_lowercase(), v.trim().to_string());
        }
    }
    let content_len: usize = headers
        .get("content-length")
        .and_then(|v| v.parse().ok())
        .unwrap_or(0);
    let mut body = buf[header_end + 4..].to_vec();
    while body.len() < content_len {
        let n = stream.read(&mut tmp)?;
        if n == 0 {
            break;
        }
        body.extend_from_slice(&tmp[..n]);
    }
    body.truncate(content_len);

    let (path, query) = match target.split_once('?') {
        Some((p, q)) => (p.to_string(), parse_query(q)),
        None => (target.clone(), HashMap::new()),
    };
    let idem_key = headers.get("idempotency-key").cloned();
    let (status, content_type, resp) = route(&app, &method, &path, &query, &body, idem_key.as_deref());
    let head = format!(
        "HTTP/1.1 {status} {}\r\nContent-Type: {content_type}\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
        reason(status),
        resp.len()
    );
    stream.write_all(head.as_bytes())?;
    stream.write_all(&resp)?;
    Ok(())
}

fn find(hay: &[u8], needle: &[u8]) -> Option<usize> {
    hay.windows(needle.len()).position(|w| w == needle)
}

fn parse_query(q: &str) -> HashMap<String, String> {
    q.split('&')
        .filter_map(|kv| kv.split_once('='))
        .map(|(k, v)| (k.to_string(), urldecode(v)))
        .collect()
}

fn urldecode(s: &str) -> String {
    let mut out = Vec::new();
    let b = s.as_bytes();
    let mut i = 0;
    while i < b.len() {
        if b[i] == b'%' && i + 2 < b.len() {
            if let Ok(v) = u8::from_str_radix(&s[i + 1..i + 3], 16) {
                out.push(v);
                i += 3;
                continue;
            }
        }
        out.push(if b[i] == b'+' { b' ' } else { b[i] });
        i += 1;
    }
    String::from_utf8_lossy(&out).to_string()
}

fn reason(status: u16) -> &'static str {
    match status {
        200 => "OK",
        201 => "Created",
        400 => "Bad Request",
        404 => "Not Found",
        409 => "Conflict",
        _ => "Internal Server Error",
    }
}

fn json_resp(status: u16, v: &Value) -> (u16, String, Vec<u8>) {
    (
        status,
        "application/json; charset=utf-8".into(),
        serde_json::to_vec_pretty(v).unwrap_or_default(),
    )
}

fn err_resp(e: ApiError) -> (u16, String, Vec<u8>) {
    json_resp(
        e.status,
        &json!({"error": {"code": e.code, "message": e.message}}),
    )
}

fn route(
    app: &App,
    method: &str,
    path: &str,
    query: &HashMap<String, String>,
    body: &[u8],
    idem_key: Option<&str>,
) -> (u16, String, Vec<u8>) {
    match dispatch(app, method, path, query, body, idem_key) {
        Ok(r) => r,
        Err(e) => err_resp(e),
    }
}

fn body_json(body: &[u8]) -> Result<Value, ApiError> {
    if body.is_empty() {
        return Ok(json!({}));
    }
    serde_json::from_slice(body).map_err(|e| ApiError::bad_request(format!("JSON 解析失败: {e}")))
}

type Resp = (u16, String, Vec<u8>);

fn dispatch(
    app: &App,
    method: &str,
    path: &str,
    query: &HashMap<String, String>,
    body: &[u8],
    idem_key: Option<&str>,
) -> Result<Resp, ApiError> {
    let seg: Vec<&str> = path.trim_matches('/').split('/').collect();
    let id_of = |i: usize| -> Result<i64, ApiError> {
        seg.get(i)
            .and_then(|s| s.parse::<i64>().ok())
            .ok_or_else(|| ApiError::bad_request("路径 id 无效"))
    };
    match (method, seg.as_slice()) {
        ("GET", []) | ("GET", [""]) => Ok((
            200,
            "text/html; charset=utf-8".into(),
            crate::web::INDEX_HTML.as_bytes().to_vec(),
        )),
        ("POST", ["api", "preset"]) => {
            let v = app.seed_preset()?;
            Ok(json_resp(200, &v))
        }
        ("POST", ["api", "meshes"]) => {
            let b = body_json(body)?;
            let name = b["name"].as_str().unwrap_or("未命名").to_string();
            let content = b["content"].as_str().unwrap_or("").to_string();
            let (st, v) = app.idempotent(idem_key, "create_mesh", &b, |app| {
                Ok((201, app.create_mesh(&name, &content)?))
            })?;
            Ok(json_resp(st, &v))
        }
        ("GET", ["api", "meshes", _]) => Ok(json_resp(200, &app.get_mesh(id_of(2)?)?)),
        ("GET", ["api", "meshes", _, "state"]) => Ok(json_resp(200, &app.mesh_state(id_of(2)?)?)),
        ("GET", ["api", "meshes", _, "revisions"]) => {
            Ok(json_resp(200, &app.list_revisions(id_of(2)?)?))
        }
        ("GET", ["api", "meshes", _, "conflicts"]) => {
            Ok(json_resp(200, &app.list_conflicts(id_of(2)?)?))
        }
        ("GET", ["api", "meshes", _, "proposals"]) => {
            Ok(json_resp(200, &app.list_proposals(id_of(2)?)?))
        }
        ("GET", ["api", "meshes", _, "exports"]) => {
            Ok(json_resp(200, &app.list_exports(id_of(2)?)?))
        }
        ("GET", ["api", "meshes", _, "vertex-sources"]) => {
            Ok(json_resp(200, &app.vertex_sources(id_of(2)?)?))
        }
        ("GET", ["api", "meshes", _, "snippet"]) => {
            let offset: u64 = query.get("offset").and_then(|v| v.parse().ok()).unwrap_or(0);
            let len: u64 = query.get("len").and_then(|v| v.parse().ok()).unwrap_or(0);
            Ok(json_resp(200, &app.mesh_snippet(id_of(2)?, offset, len)?))
        }
        ("POST", ["api", "meshes", _, "detections"]) => {
            let mesh_id = id_of(2)?;
            let b = body_json(body)?;
            let tol = ToleranceConfig {
                precision: b["precision"].as_u64().unwrap_or(6) as u32,
                vertex_eps: b["vertex_eps"].as_f64().unwrap_or(1e-3),
                degenerate_area_eps: b["degenerate_area_eps"].as_f64().unwrap_or(1e-9),
            };
            let (st, v) = app.idempotent(idem_key, "start_detection", &json!({"mesh_id": mesh_id, "body": b}), |app| {
                Ok((201, app.start_detection(mesh_id, tol, None)?))
            })?;
            Ok(json_resp(st, &v))
        }
        ("POST", ["api", "meshes", _, "exports"]) => {
            let mesh_id = id_of(2)?;
            let b = body_json(body)?;
            let seq = b["revision_seq"]
                .as_i64()
                .ok_or_else(|| ApiError::bad_request("缺少 revision_seq"))?;
            let (st, v) = app.idempotent(idem_key, "create_export", &json!({"mesh_id": mesh_id, "body": b}), |app| {
                Ok((201, app.create_export(mesh_id, seq)?))
            })?;
            Ok(json_resp(st, &v))
        }
        ("GET", ["api", "runs", _]) => Ok(json_resp(200, &app.get_run(id_of(2)?)?)),
        ("POST", ["api", "runs", _, "resume"]) => {
            Ok(json_resp(200, &app.resume_detection(id_of(2)?)?))
        }
        ("GET", ["api", "issues", _]) => Ok(json_resp(200, &app.get_issue(id_of(2)?)?)),
        ("POST", ["api", "issues", _, "candidates"]) => {
            let issue_id = id_of(2)?;
            let (st, v) = app.idempotent(idem_key, "gen_candidates", &json!({"issue_id": issue_id}), |app| {
                Ok((201, app.generate_candidates(issue_id)?))
            })?;
            Ok(json_resp(st, &v))
        }
        ("GET", ["api", "proposals", _]) => Ok(json_resp(200, &app.get_proposal(id_of(2)?)?)),
        ("POST", ["api", "proposals", _, "accept"]) => {
            let pid = id_of(2)?;
            let b = body_json(body)?;
            let seq = b["expected_seq"]
                .as_i64()
                .ok_or_else(|| ApiError::bad_request("缺少 expected_seq"))?;
            let (st, v) = app.idempotent(idem_key, "accept_proposal", &json!({"pid": pid, "body": b}), |app| {
                Ok((200, app.accept_proposal(pid, seq)?))
            })?;
            Ok(json_resp(st, &v))
        }
        ("POST", ["api", "proposals", _, "reject"]) => {
            Ok(json_resp(200, &app.reject_proposal(id_of(2)?)?))
        }
        ("POST", ["api", "conflicts", _, "resolve"]) => {
            Ok(json_resp(200, &app.resolve_conflict(id_of(2)?)?))
        }
        ("GET", ["api", "exports", _]) => Ok(json_resp(200, &app.get_export(id_of(2)?)?)),
        _ => Err(ApiError::not_found(format!("{method} {path}"))),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::clock::FixedClock;
    use crate::store::Store;
    use std::collections::HashMap;

    fn app() -> App {
        App::new(Store::open_memory().unwrap(), Arc::new(FixedClock(42))).unwrap()
    }

    fn call(app: &App, method: &str, path: &str, body: Option<&str>) -> (u16, Value) {
        let (st, _ct, bytes) = route(
            app,
            method,
            path,
            &HashMap::new(),
            body.map(|b| b.as_bytes()).unwrap_or(&[]),
            None,
        );
        (st, serde_json::from_slice(&bytes).unwrap_or(Value::Null))
    }

    #[test]
    fn index_page_served() {
        let app = app();
        let (st, ct, body) = route(&app, "GET", "/", &HashMap::new(), &[], None);
        assert_eq!(st, 200);
        assert!(ct.contains("text/html"));
        assert!(String::from_utf8_lossy(&body).contains("网格修复证据台"));
    }

    #[test]
    fn http_flow_preset_to_conflict() {
        let app = app();
        let (st, seed) = call(&app, "POST", "/api/preset", None);
        assert_eq!(st, 200);
        let mesh_id = seed["mesh_id"].as_i64().unwrap();
        let run_id = seed["run_id"].as_i64().unwrap();

        let (st, run) = call(&app, "GET", &format!("/api/runs/{run_id}"), None);
        assert_eq!(st, 200);
        let hole = run["issues"]
            .as_array()
            .unwrap()
            .iter()
            .find(|i| i["kind"] == "boundary_loop" && i["evidence"]["detail"]["loop"].as_array().unwrap().len() == 3)
            .unwrap()
            .clone();
        let issue_id = hole["id"].as_i64().unwrap();

        let (st, cands) = call(&app, "POST", &format!("/api/issues/{issue_id}/candidates"), None);
        assert_eq!(st, 201);
        let p1 = cands["proposals"][0]["id"].as_i64().unwrap();
        let p2 = cands["proposals"][1]["id"].as_i64().unwrap();

        let (st, _) = call(&app, "POST", &format!("/api/proposals/{p1}/accept"), Some("{\"expected_seq\":0}"));
        assert_eq!(st, 200);
        // 第二个补片触及相同边界 → 409,冲突记录可查
        let (st, err) = call(&app, "POST", &format!("/api/proposals/{p2}/accept"), Some("{\"expected_seq\":1}"));
        assert_eq!(st, 409);
        assert_eq!(err["error"]["code"], "PROPOSAL_CONFLICT");

        let (st, conflicts) = call(&app, "GET", &format!("/api/meshes/{mesh_id}/conflicts"), None);
        assert_eq!(st, 200);
        assert_eq!(conflicts["conflicts"].as_array().unwrap().len(), 1);

        let (st, _) = call(&app, "POST", &format!("/api/meshes/{mesh_id}/exports"), Some("{\"revision_seq\":1}"));
        assert_eq!(st, 201);
    }

    #[test]
    fn unknown_route_404() {
        let app = app();
        let (st, body) = call(&app, "GET", "/api/nope", None);
        assert_eq!(st, 404);
        assert_eq!(body["error"]["code"], "NOT_FOUND");
    }
}
