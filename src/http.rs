//! 手写 HTTP/1.1 服务：std::net + 线程 per connection。
//! 只开放 JSON 元数据接口与静态页面；**没有任何接收波形/文件字节的路由**，
//! 波形只能从本地 fixture 目录导入，运行时不上传。

use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::Arc;

use crate::error::{AppError, AppResult};
use crate::json::Json;
use crate::service::AppState;
use crate::web;

struct Request {
    method: String,
    path: String,
    body: String,
}

pub fn serve(state: Arc<AppState>, addr: &str) -> AppResult<()> {
    let listener = TcpListener::bind(addr)
        .map_err(|e| AppError::Http(format!("绑定 {addr} 失败: {e}")))?;
    println!("声场脉络服务已启动: http://{addr}");
    serve_on(listener, state)
}

/// 在已绑定的 listener 上服务，便于测试选择临时端口。
pub fn serve_on(listener: TcpListener, state: Arc<AppState>) -> AppResult<()> {
    for stream in listener.incoming() {
        match stream {
            Ok(s) => {
                let st = Arc::clone(&state);
                std::thread::spawn(move || {
                    if let Err(e) = handle_connection(st, s) {
                        eprintln!("连接处理失败: {e}");
                    }
                });
            }
            Err(e) => eprintln!("接受连接失败: {e}"),
        }
    }
    Ok(())
}

fn handle_connection(state: Arc<AppState>, mut stream: TcpStream) -> AppResult<()> {
    let mut buf = Vec::with_capacity(4096);
    let mut tmp = [0u8; 4096];
    let header_end = loop {
        let n = stream.read(&mut tmp).map_err(AppError::from)?;
        if n == 0 {
            return Ok(());
        }
        buf.extend_from_slice(&tmp[..n]);
        if let Some(pos) = find_double_crlf(&buf) {
            break pos;
        }
        if buf.len() > 1 << 20 {
            return Err(AppError::Http("请求头过大".into()));
        }
    };
    let header_text = String::from_utf8_lossy(&buf[..header_end]).to_string();
    let mut lines = header_text.split("\r\n");
    let request_line = lines.next().unwrap_or("");
    let mut parts = request_line.split(' ');
    let method = parts.next().unwrap_or("").to_string();
    let path = parts.next().unwrap_or("").to_string();
    let mut content_length = 0usize;
    for line in lines {
        if let Some((k, v)) = line.split_once(':') {
            if k.trim().eq_ignore_ascii_case("content-length") {
                content_length = v.trim().parse().unwrap_or(0);
            }
        }
    }
    let mut body = Vec::new();
    body.extend_from_slice(&buf[header_end + 4..]);
    while body.len() < content_length {
        let n = stream.read(&mut tmp).map_err(AppError::from)?;
        if n == 0 {
            break;
        }
        body.extend_from_slice(&tmp[..n]);
    }
    body.truncate(content_length);
    let req = Request {
        method,
        path,
        body: String::from_utf8_lossy(&body).into_owned(),
    };
    let outcome = route(state, req);
    write_response(&mut stream, outcome.status, &outcome.content_type, &outcome.body)?;
    Ok(())
}

fn find_double_crlf(buf: &[u8]) -> Option<usize> {
    buf.windows(4).position(|w| w == b"\r\n\r\n")
}

fn write_response(stream: &mut TcpStream, status: u16, content_type: &str, body: &[u8]) -> AppResult<()> {
    let reason = match status {
        200 => "OK",
        400 => "Bad Request",
        404 => "Not Found",
        409 => "Conflict",
        500 => "Internal Server Error",
        _ => "OK",
    };
    let head = format!(
        "HTTP/1.1 {status} {reason}\r\nContent-Type: {content_type}\r\nContent-Length: {}\r\n\
         Connection: close\r\nCache-Control: no-store\r\n\r\n",
        body.len()
    );
    stream.write_all(head.as_bytes())?;
    stream.write_all(body)?;
    stream.flush()?;
    Ok(())
}

struct Outcome {
    status: u16,
    content_type: String,
    body: Vec<u8>,
}

fn ok_json(value: Json) -> Outcome {
    Outcome {
        status: 200,
        content_type: "application/json; charset=utf-8".into(),
        body: value.dump().into_bytes(),
    }
}

fn error_response(e: AppError) -> Outcome {
    let status = match &e {
        AppError::Validation(_) => 400,
        AppError::NotFound(_) => 404,
        AppError::Conflict(_) => 409,
        _ => 500,
    };
    let kind = match &e {
        AppError::Validation(_) => "validation_error",
        AppError::NotFound(_) => "not_found",
        AppError::Conflict(_) => "version_conflict",
        _ => "internal_error",
    };
    let body = Json::obj(vec![
        ("error", Json::Str(kind.into())),
        ("message", Json::Str(e.to_string())),
    ])
    .dump();
    Outcome {
        status,
        content_type: "application/json; charset=utf-8".into(),
        body: body.into_bytes(),
    }
}

fn parse_json_body(body: &str) -> AppResult<Json> {
    if body.is_empty() {
        return Err(AppError::Validation("请求体为空，需要 JSON".into()));
    }
    crate::json::parse(body).map_err(AppError::Json)
}

fn route(state: Arc<AppState>, req: Request) -> Outcome {
    match dispatch(&state, &req) {
        Ok(Json::RawHtml(html)) => Outcome {
            status: 200,
            content_type: "text/html; charset=utf-8".into(),
            body: html.into_bytes(),
        },
        Ok(Json::RawJs(js)) => Outcome {
            status: 200,
            content_type: "application/javascript; charset=utf-8".into(),
            body: js.into_bytes(),
        },
        Ok(v) => ok_json(v),
        Err(e) => error_response(e),
    }
}

fn dispatch(state: &Arc<AppState>, req: &Request) -> AppResult<Json> {
    let path = req.path.split('?').next().unwrap_or("/");
    let segments: Vec<&str> = path.split('/').filter(|s| !s.is_empty()).collect();

    if req.method == "GET" && (path == "/" || path == "/index.html") {
        return Ok(Json::RawHtml(web::index_html().into()));
    }
    if req.method == "GET" && path == "/app.js" {
        return Ok(Json::RawJs(web::app_js().into()));
    }

    match segments.as_slice() {
        ["api", "health"] if req.method == "GET" => Ok(Json::obj(vec![
            ("status", Json::Str("ok".into())),
            ("upload_enabled", Json::Bool(false)),
            ("waveform_source", Json::Str("本地 fixture，运行时不上传".into())),
        ])),
        ["api", "measurements"] if req.method == "GET" => {
            crate::service::list_measurements(state)
        }
        ["api", "measurements", mid] if req.method == "GET" => {
            crate::service::measurement_detail(state, mid.parse()?)
        }
        ["api", "measurements", mid, "candidates"] if req.method == "GET" => {
            crate::service::list_candidates(state, mid.parse()?)
        }
        ["api", "measurements", mid, "windows"] if req.method == "GET" => {
            crate::service::list_windows(state, mid.parse()?)
        }
        ["api", "measurements", mid, "windows"] if req.method == "POST" => {
            let mid = mid.parse()?;
            let body = parse_json_body(&req.body)?;
            let candidate_id = body
                .i64_field("candidate_id")
                .ok_or_else(|| AppError::Validation("缺少 candidate_id".into()))?;
            let expected = body
                .i64_field("expected_revision")
                .ok_or_else(|| AppError::Validation("缺少 expected_revision".into()))?;
            let start = body
                .i64_field("start_sample")
                .ok_or_else(|| AppError::Validation("缺少 start_sample".into()))?;
            let end = body
                .i64_field("end_sample")
                .ok_or_else(|| AppError::Validation("缺少 end_sample".into()))?;
            let rationale = body.str_field("rationale").unwrap_or("人工修订窗口").to_string();
            let author = body.str_field("author").unwrap_or("reviewer").to_string();
            crate::service::create_window(
                state, mid, candidate_id, expected, start, end, &rationale, &author,
            )
        }
        ["api", "measurements", mid, "reviews"] if req.method == "GET" => {
            crate::service::list_reviews(state, mid.parse()?)
        }
        ["api", "measurements", mid, "reviews"] if req.method == "POST" => {
            let mid = mid.parse()?;
            let body = parse_json_body(&req.body)?;
            let expected = body
                .i64_field("expected_version")
                .ok_or_else(|| AppError::Validation("缺少 expected_version".into()))?;
            let reviewer = body.str_field("reviewer").unwrap_or("reviewer").to_string();
            let decision = body
                .str_field("decision")
                .ok_or_else(|| AppError::Validation("缺少 decision".into()))?;
            let note = body.str_field("note").unwrap_or("").to_string();
            let candidate_id = body.get("candidate_id").and_then(|v| v.as_i64());
            let window_id = body.get("window_revision_id").and_then(|v| v.as_i64());
            crate::service::create_review(
                state, mid, expected, &reviewer, decision, &note, candidate_id, window_id,
            )
        }
        ["api", "measurements", mid, "publish"] if req.method == "POST" => {
            let mid = mid.parse()?;
            let body = parse_json_body(&req.body)?;
            let candidate_id = body
                .i64_field("candidate_id")
                .ok_or_else(|| AppError::Validation("缺少 candidate_id".into()))?;
            let window_id = body
                .i64_field("window_revision_id")
                .ok_or_else(|| AppError::Validation("缺少 window_revision_id".into()))?;
            crate::service::publish_response(state, mid, candidate_id, window_id)
        }
        ["api", "measurements", mid, "published"] if req.method == "GET" => {
            crate::service::list_published(state, mid.parse()?)
        }
        ["api", "calibrations"] if req.method == "GET" => crate::service::list_calibrations(state),
        ["api", "calibrations", "replace"] if req.method == "POST" => {
            let body = parse_json_body(&req.body)?;
            let file = body
                .str_field("file_name")
                .ok_or_else(|| AppError::Validation("缺少 file_name".into()))?;
            crate::service::replace_calibration(state, file)
        }
        ["api", "calibration-changes"] if req.method == "GET" => crate::service::list_changes(state),
        ["api", "calibration-changes", cid, "impact"] if req.method == "GET" => {
            crate::service::impact_list(state, cid.parse()?)
        }
        _ => Err(AppError::NotFound(format!(
            "无此路由: {} {}",
            req.method, path
        ))),
    }
}
