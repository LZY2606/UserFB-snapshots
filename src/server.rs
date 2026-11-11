//! 本地 HTTP 服务与单页界面。只监听回环地址，不发起任何外部连接。

use std::sync::{Arc, Mutex};

use tiny_http::{Header, Method, Request, Response, Server};

use crate::db::Db;
use crate::engine;
use crate::model::*;
use crate::sample;

/// tiny_http 的绑定错误、rusqlite 错误、io 错误统一装箱。
type AppResult<T> = Result<T, Box<dyn std::error::Error + Send + Sync>>;

const INDEX_HTML: &str = include_str!("index.html");

pub struct App {
    db: Arc<Mutex<Db>>,
}

#[derive(serde::Serialize)]
struct StatePayload {
    branches: Vec<crate::db::BranchRow>,
    rulesets: Vec<crate::db::RulesetRow>,
    records: Vec<Record>,
    keys: Vec<KeyRow>,
    boots: Vec<BootEvent>,
    diagnoses: Vec<Diagnosis>,
    scenarios: Vec<ScenarioMeta>,
}

#[derive(serde::Serialize)]
struct ScenarioMeta {
    id: String,
    title: String,
    description: String,
    failed_nonces: Vec<i64>,
}

#[derive(serde::Serialize)]
struct LineagePayload {
    record: Record,
    /// 从 TGT 根到当前记录的祖先链（含自身）。
    ancestors: Vec<Record>,
    /// 由当前记录衍生的后代。
    descendants: Vec<Record>,
    diagnoses: Vec<Diagnosis>,
}

#[derive(serde::Deserialize)]
struct NewBranchReq {
    name: String,
    #[serde(default)]
    parent_branch_id: Option<i64>,
    ruleset: RulesetInput,
}

impl App {
    pub fn new(db: Db) -> Self {
        App {
            db: Arc::new(Mutex::new(db)),
        }
    }

    /// 绑定回环地址，返回底层 server（测试可用端口 0 让系统分配）。
    pub fn bind(addr: &str) -> AppResult<Server> {
        Server::http(addr)
    }

    /// 在已绑定的 server 上持续服务。
    pub fn serve(&self, server: Server) -> AppResult<()> {
        eprintln!(
            "kerbdiag 监听 http://{} （离线模式，无外部连接）",
            server.server_addr()
        );
        for request in server.incoming_requests() {
            if let Err(e) = self.handle(request) {
                eprintln!("请求处理失败: {e}");
            }
        }
        Ok(())
    }

    pub fn run(self, addr: &str) -> AppResult<()> {
        let server = Self::bind(addr)?;
        self.serve(server)
    }

    fn handle(&self, mut req: Request) -> AppResult<()> {
        let method = req.method().clone();
        let raw_url = req.url().to_string();
        let (path, _query) = raw_url.split_once('?').unwrap_or((&raw_url, ""));

        // 读取 body（JSON 接口用）。
        let mut body = Vec::new();
        if method == Method::Post || method == Method::Put {
            req.as_reader().read_to_end(&mut body)?;
        }

        match (&method, path) {
            (Method::Get, "/") => self.respond_html(req, INDEX_HTML),
            (Method::Get, "/api/state") => self.state(req),
            (Method::Get, "/api/export") => self.export(req),
            (Method::Post, "/api/load-sample") => self.load_sample(req),
            (Method::Post, "/api/import") => self.import(req, &body),
            (Method::Post, "/api/branches") => self.new_branch(req, &body),
            (Method::Get, p) if p.starts_with("/api/records/") => {
                let id: i64 = p["/api/records/".len()..].parse().unwrap_or(-1);
                self.record_detail(req, id)
            }
            _ => self.respond_text(req, 404, "not found"),
        }
    }

    fn respond_html(&self, req: Request, html: &str) -> AppResult<()> {
        req.respond(
            Response::from_string(html).with_header(Header::from_bytes(&b"Content-Type"[..], &b"text/html; charset=utf-8"[..]).unwrap()),
        )?;
        Ok(())
    }

    fn respond_json(&self, req: Request, value: &impl serde::Serialize) -> AppResult<()> {
        let data = serde_json::to_vec(value).unwrap();
        req.respond(
            Response::from_data(data).with_header(
                Header::from_bytes(&b"Content-Type"[..], &b"application/json; charset=utf-8"[..])
                    .unwrap(),
            ),
        )?;
        Ok(())
    }

    fn respond_text(&self, req: Request, code: u16, msg: &str) -> AppResult<()> {
        let resp = Response::from_string(msg).with_status_code(code);
        req.respond(resp)?;
        Ok(())
    }

    fn state(&self, req: Request) -> AppResult<()> {
        let db = self.db.lock().unwrap();
        let payload = StatePayload {
            branches: db.list_branches()?,
            rulesets: db.list_rulesets()?,
            records: db.all_records()?,
            keys: db.all_keys()?,
            boots: db.all_boots()?,
            diagnoses: db.all_diagnoses()?,
            scenarios: sample::scenarios()
                .into_iter()
                .map(|s| ScenarioMeta {
                    id: s.id.into(),
                    title: s.title.into(),
                    description: s.description.into(),
                    failed_nonces: s.failed_nonces,
                })
                .collect(),
        };
        self.respond_json(req, &payload)
    }

    fn export(&self, req: Request) -> AppResult<()> {
        let db = self.db.lock().unwrap();
        let data = db.export_all()?;
        self.respond_json(req, &data)
    }

    fn load_sample(&self, req: Request) -> AppResult<()> {
        let mut db = self.db.lock().unwrap();
        db.wipe_facts()?;
        let file = sample::bundled_import();
        let stats = db.import_file(&file)?;

        // 重建基线分支并跑一遍分析，保证页面开箱即有结论。
        let ruleset_id = db.upsert_ruleset(&sample::baseline_ruleset())?;
        let now = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_secs() as i64)
            .unwrap_or(0);
        let branch_id = db.create_branch("baseline", None, ruleset_id, now)?;
        let branch = db
            .list_branches()?
            .into_iter()
            .find(|b| b.id == branch_id)
            .unwrap();
        let produced = engine::run_branch(&mut db, &branch)?;
        self.respond_json(
            req,
            &serde_json::json!({ "imported": stats, "branch_id": branch_id, "diagnoses": produced }),
        )
    }

    fn import(&self, req: Request, body: &[u8]) -> AppResult<()> {
        let file: ImportFile = match serde_json::from_slice(body) {
            Ok(f) => f,
            Err(e) => return self.respond_text(req, 400, &format!("JSON 解析失败: {e}")),
        };
        let mut db = self.db.lock().unwrap();
        match db.import_file(&file) {
            Ok(stats) => self.respond_json(req, &stats),
            Err(e) => self.respond_text(req, 400, &format!("导入失败（事务已回滚）: {e}")),
        }
    }

    fn new_branch(&self, req: Request, body: &[u8]) -> AppResult<()> {
        let input: NewBranchReq = match serde_json::from_slice(body) {
            Ok(v) => v,
            Err(e) => return self.respond_text(req, 400, &format!("JSON 解析失败: {e}")),
        };
        let mut db = self.db.lock().unwrap();
        let ruleset_id = db.upsert_ruleset(&input.ruleset)?;
        let now = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_secs() as i64)
            .unwrap_or(0);
        let branch_id = db.create_branch(&input.name, input.parent_branch_id, ruleset_id, now)?;
        let branch = db
            .list_branches()?
            .into_iter()
            .find(|b| b.id == branch_id)
            .unwrap();
        let produced = engine::run_branch(&mut db, &branch)?;
        self.respond_json(
            req,
            &serde_json::json!({ "branch_id": branch_id, "ruleset_id": ruleset_id, "diagnoses": produced }),
        )
    }

    fn record_detail(&self, req: Request, id: i64) -> AppResult<()> {
        let db = self.db.lock().unwrap();
        let record = match db.record_by_id(id)? {
            Some(r) => r,
            None => return self.respond_text(req, 404, "record not found"),
        };
        let all = db.all_records()?;
        let by_id: std::collections::HashMap<i64, &Record> =
            all.iter().map(|r| (r.id, r)).collect();

        // 祖先链：沿 parent_id 回溯到根。
        let mut ancestors = Vec::new();
        let mut cur = record.parent_id;
        while let Some(pid) = cur {
            if let Some(p) = by_id.get(&pid) {
                ancestors.push((*p).clone());
                cur = p.parent_id;
            } else {
                break;
            }
        }
        ancestors.reverse();

        // 后代：BFS 子节点。
        let mut descendants = Vec::new();
        let mut frontier = vec![record.id];
        while let Some(pid) = frontier.pop() {
            for r in &all {
                if r.parent_id == Some(pid) {
                    descendants.push(r.clone());
                    frontier.push(r.id);
                }
            }
        }

        let payload = LineagePayload {
            record,
            ancestors,
            descendants,
            diagnoses: db.diagnoses_for_record(id)?,
        };
        self.respond_json(req, &payload)
    }
}
