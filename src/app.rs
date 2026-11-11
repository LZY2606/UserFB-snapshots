use crate::clock::Clock;
use crate::mesh::{detect, parse_mesh, ByteRange, DetectedIssue, Mesh};
use crate::model::{
    ApiError, MeshStats, ProposalStatus, RunStatus, StatsPreview, ToleranceConfig,
};
use crate::store::Store;
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::sync::{Arc, Mutex};

pub struct App {
    pub store: Mutex<Store>,
    pub clock: Arc<dyn Clock>,
}

/// 检测执行被外部中断(模拟崩溃),运行保持 running 待恢复。
#[derive(Debug)]
pub struct DetectionInterrupted;

pub fn sha256_hex(data: &[u8]) -> String {
    let mut h = Sha256::new();
    h.update(data);
    hex(&h.finalize())
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

#[derive(Debug, Clone)]
pub struct ProposalRow {
    pub id: i64,
    pub issue_id: i64,
    pub mesh_id: i64,
    pub kind: String,
    pub payload: Value,
    pub boundary_edges: Vec<(i64, i64)>,
    pub status: ProposalStatus,
    pub created_at: i64,
    pub decided_at: Option<i64>,
}

const NEW_VERTEX_BASE: i64 = 1_000_000_000;

impl App {
    pub fn new(store: Store, clock: Arc<dyn Clock>) -> Result<App, ApiError> {
        store.recover_interrupted()?;
        Ok(App {
            store: Mutex::new(store),
            clock,
        })
    }

    fn now(&self) -> i64 {
        self.clock.now_ms()
    }

    // ---------- 幂等 ----------

    /// 幂等执行:相同 key + 相同请求内容 → 回放首个响应;相同 key + 不同内容 → 409。
    pub fn idempotent<F>(
        &self,
        key: Option<&str>,
        endpoint: &str,
        request: &Value,
        f: F,
    ) -> Result<(u16, Value), ApiError>
    where
        F: FnOnce(&App) -> Result<(u16, Value), ApiError>,
    {
        let Some(key) = key else {
            return f(self);
        };
        let req_hash = sha256_hex(format!("{endpoint}:{}", canonical(request)).as_bytes());
        let store = self.store.lock().unwrap();
        let existing: Option<(String, String, i64, String)> = store
            .conn
            .query_row(
                "SELECT endpoint, request_hash, status, response_json FROM idempotency WHERE key=?1",
                [key],
                |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?, r.get(3)?)),
            )
            .ok();
        if let Some((ep, hash, status, resp)) = existing {
            if ep == endpoint && hash == req_hash {
                let v: Value = serde_json::from_str(&resp)?;
                return Ok((status as u16, v));
            }
            return Err(ApiError::idempotency_conflict(format!(
                "幂等键 '{key}' 已用于不同请求内容"
            )));
        }
        drop(store);
        let (status, resp) = f(self)?;
        let store = self.store.lock().unwrap();
        store.conn.execute(
            "INSERT OR IGNORE INTO idempotency(key,endpoint,request_hash,status,response_json,created_at)
             VALUES(?1,?2,?3,?4,?5,?6)",
            rusqlite::params![key, endpoint, req_hash, status as i64, resp.to_string(), self.now()],
        )?;
        Ok((status, resp))
    }

    // ---------- 网格 ----------

    pub fn create_mesh(&self, name: &str, content: &str) -> Result<Value, ApiError> {
        let mesh = parse_mesh(content);
        if mesh.vertices.is_empty() && mesh.faces.is_empty() {
            return Err(ApiError::bad_request("网格内容为空或无法解析"));
        }
        let digest = sha256_hex(content.as_bytes());
        let mut store = self.store.lock().unwrap();
        let id = store.insert_mesh(name, &digest, "tmesh", content, &mesh, self.now())?;
        Ok(json!({
            "mesh_id": id,
            "digest": digest,
            "vertex_count": mesh.vertices.len(),
            "face_count": mesh.faces.len(),
        }))
    }

    pub fn get_mesh(&self, mesh_id: i64) -> Result<Value, ApiError> {
        let store = self.store.lock().unwrap();
        mesh_row(&store, mesh_id)
    }

    pub fn mesh_snippet(&self, mesh_id: i64, offset: u64, len: u64) -> Result<Value, ApiError> {
        let store = self.store.lock().unwrap();
        let raw: String = store
            .conn
            .query_row("SELECT raw FROM meshes WHERE id=?1", [mesh_id], |r| r.get(0))
            .map_err(|_| ApiError::not_found(format!("mesh {mesh_id}")))?;
        let bytes = raw.as_bytes();
        let start = (offset as usize).min(bytes.len());
        let end = ((offset + len) as usize).min(bytes.len());
        let text = String::from_utf8_lossy(&bytes[start..end]).to_string();
        Ok(json!({"offset": offset, "len": len, "text": text}))
    }

    // ---------- 检测 ----------

    /// 启动检测:固定网格 digest、精度与容差。crash_after 仅供测试注入中断。
    pub fn start_detection(
        &self,
        mesh_id: i64,
        tol: ToleranceConfig,
        crash_after: Option<usize>,
    ) -> Result<Value, ApiError> {
        let (digest,): (String,) = {
            let store = self.store.lock().unwrap();
            store
                .conn
                .query_row("SELECT digest FROM meshes WHERE id=?1", [mesh_id], |r| {
                    r.get(0).map(|d: String| (d,))
                })
                .map_err(|_| ApiError::not_found(format!("mesh {mesh_id}")))?
        };
        let run_id = {
            let store = self.store.lock().unwrap();
            store.conn.execute(
                "INSERT INTO runs(mesh_id,mesh_digest,precision,vertex_eps,degenerate_area_eps,status,created_at)
                 VALUES(?1,?2,?3,?4,?5,'running',?6)",
                rusqlite::params![mesh_id, digest, tol.precision as i64, tol.vertex_eps, tol.degenerate_area_eps, self.now()],
            )?;
            store.conn.last_insert_rowid()
        };
        self.execute_detection(run_id, crash_after)?;
        Ok(json!({"run_id": run_id, "status": "completed"}))
    }

    /// 执行检测:逐条写入证据(每条独立事务),crash_after 模拟第 N 条后中断。
    fn execute_detection(&self, run_id: i64, crash_after: Option<usize>) -> Result<(), ApiError> {
        let (mesh_id, tol) = {
            let store = self.store.lock().unwrap();
            run_row_meta(&store, run_id)?
        };
        let mesh = {
            let store = self.store.lock().unwrap();
            store.load_mesh(mesh_id)?
        };
        let issues = detect(&mesh, &tol);
        for (i, issue) in issues.iter().enumerate() {
            {
                let store = self.store.lock().unwrap();
                store.conn.execute(
                    "INSERT INTO issues(run_id,seq,kind,issue_key,evidence_json) VALUES(?1,?2,?3,?4,?5)",
                    rusqlite::params![
                        run_id,
                        i as i64,
                        issue.kind.as_str(),
                        issue_key(issue),
                        serde_json::to_string(&issue.evidence)?,
                    ],
                )?;
            }
            if crash_after == Some(i + 1) {
                // 模拟中断:运行保持 running,部分证据已落库
                return Err(ApiError::internal("detection interrupted (simulated crash)"));
            }
        }
        let store = self.store.lock().unwrap();
        store.conn.execute(
            "UPDATE runs SET status='completed', completed_at=?2 WHERE id=?1",
            rusqlite::params![run_id, self.now()],
        )?;
        Ok(())
    }

    /// 中断后恢复:仅允许恢复 interrupted 运行;重放确定性检测并替换部分证据。
    pub fn resume_detection(&self, run_id: i64) -> Result<Value, ApiError> {
        {
            let store = self.store.lock().unwrap();
            let status: String = store
                .conn
                .query_row("SELECT status FROM runs WHERE id=?1", [run_id], |r| r.get(0))
                .map_err(|_| ApiError::not_found(format!("run {run_id}")))?;
            match RunStatus::parse(&status) {
                Some(RunStatus::Interrupted) => {}
                Some(RunStatus::Completed) => {
                    return Err(ApiError::bad_request("运行已完成,无需恢复"))
                }
                _ => return Err(ApiError::bad_request("运行不在可恢复状态")),
            }
            store
                .conn
                .execute("UPDATE runs SET status='running' WHERE id=?1", [run_id])?;
            store
                .conn
                .execute("DELETE FROM issues WHERE run_id=?1", [run_id])?;
        }
        self.execute_detection(run_id, None)?;
        Ok(json!({"run_id": run_id, "status": "completed", "resumed": true}))
    }

    pub fn get_run(&self, run_id: i64) -> Result<Value, ApiError> {
        let store = self.store.lock().unwrap();
        let (mesh_id, digest, precision, veps, deps, status, created_at, completed_at): (
            i64, String, i64, f64, f64, String, i64, Option<i64>,
        ) = store
            .conn
            .query_row(
                "SELECT mesh_id,mesh_digest,precision,vertex_eps,degenerate_area_eps,status,created_at,completed_at FROM runs WHERE id=?1",
                [run_id],
                |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?, r.get(3)?, r.get(4)?, r.get(5)?, r.get(6)?, r.get(7)?)),
            )
            .map_err(|_| ApiError::not_found(format!("run {run_id}")))?;
        let mut stmt = store.conn.prepare(
            "SELECT id,kind,issue_key,evidence_json FROM issues WHERE run_id=?1 ORDER BY seq",
        )?;
        let issues: Vec<Value> = stmt
            .query_map([run_id], |r| {
                let evidence: String = r.get(3)?;
                Ok(json!({
                    "id": r.get::<_, i64>(0)?,
                    "kind": r.get::<_, String>(1)?,
                    "issue_key": r.get::<_, String>(2)?,
                    "evidence": serde_json::from_str::<Value>(&evidence).unwrap_or(Value::Null),
                }))
            })?
            .collect::<Result<_, _>>()?;
        Ok(json!({
            "run": {
                "id": run_id, "mesh_id": mesh_id, "mesh_digest": digest,
                "tolerance": {"precision": precision, "vertex_eps": veps, "degenerate_area_eps": deps},
                "status": status, "created_at": created_at, "completed_at": completed_at,
            },
            "issues": issues,
        }))
    }

    pub fn get_issue(&self, issue_id: i64) -> Result<Value, ApiError> {
        let store = self.store.lock().unwrap();
        store
            .conn
            .query_row(
                "SELECT id,run_id,kind,issue_key,evidence_json FROM issues WHERE id=?1",
                [issue_id],
                |r| {
                    let evidence: String = r.get(4)?;
                    Ok(json!({
                        "id": r.get::<_, i64>(0)?,
                        "run_id": r.get::<_, i64>(1)?,
                        "kind": r.get::<_, String>(2)?,
                        "issue_key": r.get::<_, String>(3)?,
                        "evidence": serde_json::from_str::<Value>(&evidence).unwrap_or(Value::Null),
                    }))
                },
            )
            .map_err(|_| ApiError::not_found(format!("issue {issue_id}")))
    }

    // ---------- 提案 ----------

    /// 为问题生成修复候选提案(幂等:已生成则返回现有)。
    pub fn generate_candidates(&self, issue_id: i64) -> Result<Value, ApiError> {
        let existing = {
            let store = self.store.lock().unwrap();
            load_proposals_for_issue(&store, issue_id)?
        };
        if !existing.is_empty() {
            return Ok(json!({
                "proposals": existing.iter().map(proposal_json).collect::<Vec<_>>(),
                "generated": false,
            }));
        }
        let issue = self.get_issue(issue_id)?;
        let kind = issue["kind"].as_str().unwrap_or("");
        let evidence = &issue["evidence"];
        let run_id = issue["run_id"].as_i64().unwrap();
        let mesh_id = {
            let store = self.store.lock().unwrap();
            store
                .conn
                .query_row("SELECT mesh_id FROM runs WHERE id=?1", [run_id], |r| r.get(0))
                .map_err(|_| ApiError::not_found("run"))?
        };
        let mesh = {
            let store = self.store.lock().unwrap();
            store.load_mesh(mesh_id)?
        };
        let mut created: Vec<ProposalRow> = Vec::new();
        let mut make = |kind: &str, payload: Value, boundary: Vec<(i64, i64)>| -> Result<(), ApiError> {
            let store = self.store.lock().unwrap();
            store.conn.execute(
                "INSERT INTO proposals(issue_id,mesh_id,kind,payload_json,boundary_edges_json,status,created_at)
                 VALUES(?1,?2,?3,?4,?5,'draft',?6)",
                rusqlite::params![
                    issue_id, mesh_id, kind,
                    payload.to_string(),
                    serde_json::to_string(&boundary)?,
                    self.now()
                ],
            )?;
            let id = store.conn.last_insert_rowid();
            created.push(ProposalRow {
                id, issue_id, mesh_id,
                kind: kind.to_string(),
                payload, boundary_edges: boundary,
                status: ProposalStatus::Draft,
                created_at: self.now(),
                decided_at: None,
            });
            Ok(())
        };
        match kind {
            "boundary_loop" => {
                let loopv: Vec<i64> = evidence["detail"]["loop"]
                    .as_array()
                    .map(|a| a.iter().filter_map(|v| v.as_i64()).collect())
                    .unwrap_or_default();
                let bedges = loop_edges(&loopv);
                // 候选 1:质心扇(新增顶点,来源 = 环上全部顶点)
                let coords: Vec<[f64; 3]> = loopv
                    .iter()
                    .filter_map(|&i| mesh.vertex(i).filter(|v| v.valid).map(|v| [v.x, v.y, v.z]))
                    .collect();
                if !coords.is_empty() {
                    let n = coords.len() as f64;
                    let c = [
                        coords.iter().map(|p| p[0]).sum::<f64>() / n,
                        coords.iter().map(|p| p[1]).sum::<f64>() / n,
                        coords.iter().map(|p| p[2]).sum::<f64>() / n,
                    ];
                    let mut faces = Vec::new();
                    for i in 0..loopv.len() {
                        faces.push(vec![loopv[i], loopv[(i + 1) % loopv.len()], -1]);
                    }
                    make(
                        "close_hole",
                        json!({
                            "candidate": "centroid_fan",
                            "loop": loopv,
                            "new_vertices": [{"ref": -1, "sources": loopv, "coords": c}],
                            "new_faces": faces,
                        }),
                        bedges.clone(),
                    )?;
                }
                // 候选 2:首顶点扇
                let mut faces = Vec::new();
                for i in 1..loopv.len().saturating_sub(1) {
                    faces.push(vec![loopv[0], loopv[i], loopv[i + 1]]);
                }
                if !faces.is_empty() {
                    make(
                        "close_hole",
                        json!({
                            "candidate": "vertex_fan",
                            "loop": loopv,
                            "new_vertices": [],
                            "new_faces": faces,
                        }),
                        bedges.clone(),
                    )?;
                }
                // 候选 3:末顶点扇
                let last = *loopv.last().unwrap_or(&0);
                let mut faces = Vec::new();
                for i in 0..loopv.len().saturating_sub(2) {
                    faces.push(vec![last, loopv[i], loopv[i + 1]]);
                }
                if !faces.is_empty() {
                    make(
                        "close_hole",
                        json!({
                            "candidate": "tail_fan",
                            "loop": loopv,
                            "new_vertices": [],
                            "new_faces": faces,
                        }),
                        bedges,
                    )?;
                }
            }
            "nonmanifold_edge" => {
                let edge: Vec<i64> = evidence["detail"]["edge"]
                    .as_array()
                    .map(|a| a.iter().filter_map(|v| v.as_i64()).collect())
                    .unwrap_or_default();
                let faces: Vec<i64> = evidence["detail"]["incident_faces"]
                    .as_array()
                    .map(|a| a.iter().filter_map(|v| v.as_i64()).collect())
                    .unwrap_or_default();
                if edge.len() == 2 && faces.len() > 2 {
                    let (u, v) = (edge[0], edge[1]);
                    let keep: Vec<i64> = faces.iter().take(2).cloned().collect();
                    let moved: Vec<i64> = faces.iter().skip(2).cloned().collect();
                    let cu = mesh.vertex(u).map(|v| [v.x, v.y, v.z]);
                    let cv = mesh.vertex(v).map(|v| [v.x, v.y, v.z]);
                    make(
                        "split_nonmanifold_edge",
                        json!({
                            "edge": [u, v],
                            "keep_faces": keep,
                            "moved_faces": moved,
                            "new_vertices": [
                                {"ref": -1, "sources": [u], "coords": cu},
                                {"ref": -2, "sources": [v], "coords": cv},
                            ],
                        }),
                        vec![],
                    )?;
                }
            }
            "normal_inconsistent" => {
                let faces: Vec<i64> = evidence["detail"]["faces"]
                    .as_array()
                    .map(|a| a.iter().filter_map(|v| v.as_i64()).collect())
                    .unwrap_or_default();
                for f in faces {
                    make("flip_orientation", json!({"faces": [f]}), vec![])?;
                }
            }
            "degenerate_face" | "missing_face" => {
                let f = evidence["faces"][0].as_i64().unwrap_or(0);
                make("remove_face", json!({"faces": [f]}), vec![])?;
            }
            "coincident_vertices" => {
                let group: Vec<i64> = evidence["detail"]["group"]
                    .as_array()
                    .map(|a| a.iter().filter_map(|v| v.as_i64()).collect())
                    .unwrap_or_default();
                let coords: Vec<[f64; 3]> = group
                    .iter()
                    .filter_map(|&i| mesh.vertex(i).filter(|v| v.valid).map(|v| [v.x, v.y, v.z]))
                    .collect();
                if group.len() >= 2 && !coords.is_empty() {
                    let n = coords.len() as f64;
                    let c = [
                        coords.iter().map(|p| p[0]).sum::<f64>() / n,
                        coords.iter().map(|p| p[1]).sum::<f64>() / n,
                        coords.iter().map(|p| p[2]).sum::<f64>() / n,
                    ];
                    make(
                        "contract_vertices",
                        json!({
                            "sources": group,
                            "new_vertices": [{"ref": -1, "sources": group, "coords": c}],
                        }),
                        vec![],
                    )?;
                }
            }
            _ => {}
        }
        Ok(json!({
            "proposals": created.iter().map(proposal_json).collect::<Vec<_>>(),
            "generated": true,
        }))
    }

    pub fn get_proposal(&self, proposal_id: i64) -> Result<Value, ApiError> {
        let row = {
            let store = self.store.lock().unwrap();
            load_proposal(&store, proposal_id)?
        };
        let preview = self.preview(proposal_id).ok();
        let mut v = proposal_json(&row);
        v["preview"] = serde_json::to_value(preview)?;
        Ok(v)
    }

    /// 预览:在当前已接受提案集上应用本提案前后的统计差异。
    pub fn preview(&self, proposal_id: i64) -> Result<StatsPreview, ApiError> {
        let (row, mesh, accepted) = {
            let store = self.store.lock().unwrap();
            let row = load_proposal(&store, proposal_id)?;
            let mesh = store.load_mesh(row.mesh_id)?;
            let accepted = accepted_proposals(&store, row.mesh_id)?;
            (row, mesh, accepted)
        };
        let before = replay(&mesh, &accepted).0;
        let mut with = accepted.clone();
        with.push(row);
        let after = replay(&mesh, &with).0;
        Ok(StatsPreview {
            before,
            after,
            delta_faces: after.face_count - before.face_count,
            delta_boundary_edges: after.boundary_edges - before.boundary_edges,
            delta_nonmanifold_edges: after.nonmanifold_edges - before.nonmanifold_edges,
            manifold_change: format!("{} -> {}", before.manifold, after.manifold),
        })
    }

    /// 接受提案:校验版本与边界冲突,成功则生成新修订。原网格不被修改。
    pub fn accept_proposal(&self, proposal_id: i64, expected_seq: i64) -> Result<Value, ApiError> {
        let row = {
            let store = self.store.lock().unwrap();
            load_proposal(&store, proposal_id)?
        };
        if row.status != ProposalStatus::Draft {
            return Err(ApiError::bad_request(format!(
                "提案状态为 {},不可接受",
                row.status.as_str()
            )));
        }
        // 版本冲突检查
        let current_seq: i64 = {
            let store = self.store.lock().unwrap();
            store
                .conn
                .query_row("SELECT seq FROM meshes WHERE id=?1", [row.mesh_id], |r| r.get(0))
                .map_err(|_| ApiError::not_found("mesh"))?
        };
        if current_seq != expected_seq {
            return Err(ApiError::version_conflict(format!(
                "期望版本 {expected_seq},当前版本 {current_seq}"
            )));
        }
        // 边界冲突检查:与已接受提案触及相同边界
        if !row.boundary_edges.is_empty() {
            let accepted = {
                let store = self.store.lock().unwrap();
                accepted_proposals(&store, row.mesh_id)?
            };
            for other in &accepted {
                let shared: Vec<&(i64, i64)> = row
                    .boundary_edges
                    .iter()
                    .filter(|e| other.boundary_edges.contains(e))
                    .collect();
                if !shared.is_empty() {
                    let now = self.now();
                    let store = self.store.lock().unwrap();
                    store.conn.execute(
                        "INSERT INTO conflicts(mesh_id,proposal_id,other_proposal_id,shared_edges_json,reason,status,created_at)
                         VALUES(?1,?2,?3,?4,?5,'unresolved',?6)",
                        rusqlite::params![
                            row.mesh_id, row.id, other.id,
                            serde_json::to_string(&shared)?,
                            format!("补片触及相同边界,与提案 {} 冲突", other.id),
                            now
                        ],
                    )?;
                    let conflict_id = store.conn.last_insert_rowid();
                    store.conn.execute(
                        "UPDATE proposals SET status='conflicted', decided_at=?2 WHERE id=?1",
                        rusqlite::params![row.id, now],
                    )?;
                    return Err(ApiError {
                        code: "PROPOSAL_CONFLICT".into(),
                        message: format!("补片触及相同边界,冲突记录 #{conflict_id}"),
                        status: 409,
                    });
                }
            }
        }
        // 接受:标记提案、推进版本、重放生成新修订
        let now = self.now();
        let new_seq = current_seq + 1;
        let (stats, proposal_ids, sources) = {
            let store = self.store.lock().unwrap();
            store.conn.execute(
                "UPDATE proposals SET status='accepted', decided_at=?2 WHERE id=?1",
                rusqlite::params![row.id, now],
            )?;
            store.conn.execute(
                "UPDATE meshes SET seq=?2 WHERE id=?1",
                rusqlite::params![row.mesh_id, new_seq],
            )?;
            let mesh = store.load_mesh(row.mesh_id)?;
            let mut accepted = accepted_proposals(&store, row.mesh_id)?;
            accepted.retain(|p| p.id != row.id);
            let mut all = accepted;
            all.push(row.clone());
            all.sort_by_key(|p| p.id);
            let (stats, srcs) = replay(&mesh, &all);
            let ids: Vec<i64> = all.iter().map(|p| p.id).collect();
            store.conn.execute(
                "INSERT INTO revisions(mesh_id,seq,proposal_ids_json,stats_json,created_at)
                 VALUES(?1,?2,?3,?4,?5)",
                rusqlite::params![
                    row.mesh_id, new_seq,
                    serde_json::to_string(&ids)?,
                    serde_json::to_string(&stats)?,
                    now
                ],
            )?;
            (stats, ids, srcs)
        };
        // 记录缩并/新增顶点的来源映射
        {
            let store = self.store.lock().unwrap();
            for (pid, new_id, src) in sources.iter().filter(|(pid, _, _)| *pid == row.id) {
                store.conn.execute(
                    "INSERT INTO vertex_sources(mesh_id,proposal_id,new_vertex_id,source_vertex_id)
                     VALUES(?1,?2,?3,?4)",
                    rusqlite::params![row.mesh_id, pid, new_id, src],
                )?;
            }
        }
        Ok(json!({
            "accepted": proposal_id,
            "revision": {"mesh_id": row.mesh_id, "seq": new_seq, "proposal_ids": proposal_ids, "stats": stats},
        }))
    }

    pub fn reject_proposal(&self, proposal_id: i64) -> Result<Value, ApiError> {
        let store = self.store.lock().unwrap();
        let now = self.now();
        let n = store.conn.execute(
            "UPDATE proposals SET status='rejected', decided_at=?2 WHERE id=?1 AND status='draft'",
            rusqlite::params![proposal_id, now],
        )?;
        if n == 0 {
            return Err(ApiError::bad_request("提案不存在或已决定"));
        }
        drop(store);
        self.get_proposal(proposal_id)
    }

    pub fn list_proposals(&self, mesh_id: i64) -> Result<Value, ApiError> {
        let store = self.store.lock().unwrap();
        let mut stmt = store.conn.prepare(
            "SELECT id,issue_id,mesh_id,kind,payload_json,boundary_edges_json,status,created_at,decided_at
             FROM proposals WHERE mesh_id=?1 ORDER BY id",
        )?;
        let rows = stmt
            .query_map([mesh_id], |r| Ok(proposal_from_row(r)?))?
            .collect::<Result<Vec<_>, _>>()?;
        Ok(json!({"proposals": rows.iter().map(proposal_json).collect::<Vec<_>>()}))
    }

    // ---------- 修订 / 冲突 / 导出 ----------

    pub fn list_revisions(&self, mesh_id: i64) -> Result<Value, ApiError> {
        let store = self.store.lock().unwrap();
        let mut stmt = store.conn.prepare(
            "SELECT id,seq,proposal_ids_json,stats_json,created_at FROM revisions WHERE mesh_id=?1 ORDER BY seq",
        )?;
        let rows: Vec<Value> = stmt
            .query_map([mesh_id], |r| {
                let p: String = r.get(2)?;
                let s: String = r.get(3)?;
                Ok(json!({
                    "id": r.get::<_, i64>(0)?, "seq": r.get::<_, i64>(1)?,
                    "proposal_ids": serde_json::from_str::<Value>(&p).unwrap_or(Value::Null),
                    "stats": serde_json::from_str::<Value>(&s).unwrap_or(Value::Null),
                    "created_at": r.get::<_, i64>(4)?,
                }))
            })?
            .collect::<Result<_, _>>()?;
        Ok(json!({"revisions": rows}))
    }

    pub fn list_conflicts(&self, mesh_id: i64) -> Result<Value, ApiError> {
        let store = self.store.lock().unwrap();
        let mut stmt = store.conn.prepare(
            "SELECT id,proposal_id,other_proposal_id,shared_edges_json,reason,status,created_at,resolved_at
             FROM conflicts WHERE mesh_id=?1 ORDER BY id",
        )?;
        let rows: Vec<Value> = stmt
            .query_map([mesh_id], |r| {
                let s: String = r.get(3)?;
                Ok(json!({
                    "id": r.get::<_, i64>(0)?, "proposal_id": r.get::<_, i64>(1)?,
                    "other_proposal_id": r.get::<_, i64>(2)?,
                    "shared_edges": serde_json::from_str::<Value>(&s).unwrap_or(Value::Null),
                    "reason": r.get::<_, String>(4)?, "status": r.get::<_, String>(5)?,
                    "created_at": r.get::<_, i64>(6)?, "resolved_at": r.get::<_, Option<i64>>(7)?,
                }))
            })?
            .collect::<Result<_, _>>()?;
        Ok(json!({"conflicts": rows}))
    }

    pub fn resolve_conflict(&self, conflict_id: i64) -> Result<Value, ApiError> {
        let store = self.store.lock().unwrap();
        let n = store.conn.execute(
            "UPDATE conflicts SET status='resolved', resolved_at=?2 WHERE id=?1 AND status='unresolved'",
            rusqlite::params![conflict_id, self.now()],
        )?;
        if n == 0 {
            return Err(ApiError::bad_request("冲突不存在或已解决"));
        }
        Ok(json!({"conflict_id": conflict_id, "status": "resolved"}))
    }

    /// 导出:引用确定的提案集并快照容差配置;产物不可变,后续容差改动不影响。
    pub fn create_export(&self, mesh_id: i64, revision_seq: i64) -> Result<Value, ApiError> {
        let store = self.store.lock().unwrap();
        let (proposal_ids, stats): (String, String) = store
            .conn
            .query_row(
                "SELECT proposal_ids_json, stats_json FROM revisions WHERE mesh_id=?1 AND seq=?2",
                rusqlite::params![mesh_id, revision_seq],
                |r| Ok((r.get(0)?, r.get(1)?)),
            )
            .map_err(|_| ApiError::not_found(format!("revision {revision_seq}")))?;
        let mut ids: Vec<i64> = serde_json::from_str(&proposal_ids)?;
        ids.sort();
        let set_hash = sha256_hex(
            ids.iter().map(|i| i.to_string()).collect::<Vec<_>>().join(",").as_bytes(),
        );
        // 快照最近一次完成检测的容差配置
        let tol: Option<(i64, f64, f64)> = store
            .conn
            .query_row(
                "SELECT precision,vertex_eps,degenerate_area_eps FROM runs
                 WHERE mesh_id=?1 AND status='completed' ORDER BY id DESC LIMIT 1",
                [mesh_id],
                |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)),
            )
            .ok();
        let tol_json = match tol {
            Some((p, ve, de)) => json!({"precision": p, "vertex_eps": ve, "degenerate_area_eps": de}),
            None => json!(null),
        };
        store.conn.execute(
            "INSERT INTO exports(mesh_id,revision_seq,proposal_ids_json,proposal_set_hash,tol_json,stats_json,created_at)
             VALUES(?1,?2,?3,?4,?5,?6,?7)",
            rusqlite::params![mesh_id, revision_seq, serde_json::to_string(&ids)?, set_hash, tol_json.to_string(), stats, self.now()],
        )?;
        let id = store.conn.last_insert_rowid();
        drop(store);
        self.get_export(id)
    }

    pub fn get_export(&self, export_id: i64) -> Result<Value, ApiError> {
        let store = self.store.lock().unwrap();
        store
            .conn
            .query_row(
                "SELECT id,mesh_id,revision_seq,proposal_ids_json,proposal_set_hash,tol_json,stats_json,created_at
                 FROM exports WHERE id=?1",
                [export_id],
                |r| {
                    Ok(json!({
                        "id": r.get::<_, i64>(0)?, "mesh_id": r.get::<_, i64>(1)?,
                        "revision_seq": r.get::<_, i64>(2)?,
                        "proposal_ids": serde_json::from_str::<Value>(&r.get::<_, String>(3)?).unwrap_or(Value::Null),
                        "proposal_set_hash": r.get::<_, String>(4)?,
                        "tolerance_snapshot": serde_json::from_str::<Value>(&r.get::<_, String>(5)?).unwrap_or(Value::Null),
                        "stats": serde_json::from_str::<Value>(&r.get::<_, String>(6)?).unwrap_or(Value::Null),
                        "created_at": r.get::<_, i64>(7)?,
                    }))
                },
            )
            .map_err(|_| ApiError::not_found(format!("export {export_id}")))
    }

    pub fn list_exports(&self, mesh_id: i64) -> Result<Value, ApiError> {
        let store = self.store.lock().unwrap();
        let mut stmt = store.conn.prepare(
            "SELECT id,revision_seq,proposal_set_hash,created_at FROM exports WHERE mesh_id=?1 ORDER BY id",
        )?;
        let rows: Vec<Value> = stmt
            .query_map([mesh_id], |r| {
                Ok(json!({
                    "id": r.get::<_, i64>(0)?, "revision_seq": r.get::<_, i64>(1)?,
                    "proposal_set_hash": r.get::<_, String>(2)?, "created_at": r.get::<_, i64>(3)?,
                }))
            })?
            .collect::<Result<_, _>>()?;
        Ok(json!({"exports": rows}))
    }

    /// 当前结论:网格版本、工作集统计、问题计数、未解冲突。
    pub fn mesh_state(&self, mesh_id: i64) -> Result<Value, ApiError> {
        let (mesh_json, stats, issue_counts, unresolved) = {
            let store = self.store.lock().unwrap();
            let mesh_json = mesh_row(&store, mesh_id)?;
            let mesh = store.load_mesh(mesh_id)?;
            let accepted = accepted_proposals(&store, mesh_id)?;
            let (stats, _) = replay(&mesh, &accepted);
            let mut counts: std::collections::BTreeMap<String, i64> = Default::default();
            let mut stmt = store.conn.prepare(
                "SELECT kind, COUNT(*) FROM issues
                 WHERE run_id=(SELECT id FROM runs WHERE mesh_id=?1 AND status='completed' ORDER BY id DESC LIMIT 1)
                 GROUP BY kind",
            )?;
            let rows = stmt.query_map([mesh_id], |r| {
                Ok((r.get::<_, String>(0)?, r.get::<_, i64>(1)?))
            })?;
            for r in rows {
                let (k, c) = r?;
                counts.insert(k, c);
            }
            let unresolved: i64 = store.conn.query_row(
                "SELECT COUNT(*) FROM conflicts WHERE mesh_id=?1 AND status='unresolved'",
                [mesh_id],
                |r| r.get(0),
            )?;
            (mesh_json, stats, counts, unresolved)
        };
        Ok(json!({
            "mesh": mesh_json,
            "current_stats": stats,
            "issue_counts": issue_counts,
            "unresolved_conflicts": unresolved,
        }))
    }

    pub fn vertex_sources(&self, mesh_id: i64) -> Result<Value, ApiError> {
        let store = self.store.lock().unwrap();
        let mut stmt = store.conn.prepare(
            "SELECT proposal_id,new_vertex_id,source_vertex_id FROM vertex_sources WHERE mesh_id=?1 ORDER BY new_vertex_id",
        )?;
        let rows: Vec<Value> = stmt
            .query_map([mesh_id], |r| {
                Ok(json!({
                    "proposal_id": r.get::<_, i64>(0)?,
                    "new_vertex_id": r.get::<_, i64>(1)?,
                    "source_vertex_id": r.get::<_, i64>(2)?,
                }))
            })?
            .collect::<Result<_, _>>()?;
        Ok(json!({"vertex_sources": rows}))
    }

    /// 预置案例:包含非流形边、洞、法向翻转、退化面、缺失面、法向无法确定、自交、重合顶点。
    pub fn seed_preset(&self) -> Result<Value, ApiError> {
        let created = self.idempotent(
            Some("preset-seed"),
            "preset",
            &json!({"case": "preset-v1"}),
            |app| {
                let mesh = app.create_mesh("预置案例", PRESET_MESH)?;
                let mesh_id = mesh["mesh_id"].as_i64().unwrap();
                let run = app.start_detection(mesh_id, ToleranceConfig::default(), None)?;
                Ok((200, json!({
                    "mesh_id": mesh_id,
                    "run_id": run["run_id"],
                    "digest": mesh["digest"],
                })))
            },
        )?;
        Ok(created.1)
    }
}

pub const PRESET_MESH: &str = "\
# 预置案例:非流形边/洞/法向翻转/退化面/缺失面/法向无法确定/自交/重合顶点
v 0 0 0
v 1 0 0
v 1 1 0
v 0 1 0
v 0.5 0.5 1
v 1.5 0.2 0.3
v 1.6 0.8 0.4
v 2 0 0
v 0.5 0.5 ?
v 0.85 0.4 -0.5
v 0.85 0.4 1.5
v 0.85 1.4 0.5
v 0 0 0
v 0 0 0.0005
v 3 0 0
v 4 0 0
f 1 2 5
f 2 3 5
f 3 5 4
f 1 3 2
f 1 4 3
f 2 3 6
f 3 2 7
f 8 15 16
f 5 6 99
f 9 10 11
f 10 11 12
";

// ---------- 内部助手 ----------

fn canonical(v: &Value) -> String {
    serde_json::to_string(v).unwrap_or_default()
}

fn issue_key(issue: &DetectedIssue) -> String {
    let mut ev = serde_json::to_value(&issue.evidence).unwrap_or(Value::Null);
    // 证据规范化:排序顶点/面/边,保证乱序输入得到相同 key
    normalize(&mut ev);
    let s = format!("{}:{}", issue.kind.as_str(), canonical(&ev));
    sha256_hex(s.as_bytes())[..16].to_string()
}

fn normalize(v: &mut Value) {
    match v {
        Value::Array(a) => {
            for x in a.iter_mut() {
                normalize(x);
            }
            a.sort_by(|x, y| canonical(x).cmp(&canonical(y)));
        }
        Value::Object(m) => {
            for (_, x) in m.iter_mut() {
                normalize(x);
            }
        }
        _ => {}
    }
}

fn loop_edges(loopv: &[i64]) -> Vec<(i64, i64)> {
    let mut out = Vec::new();
    for i in 0..loopv.len() {
        let (a, b) = (loopv[i], loopv[(i + 1) % loopv.len()]);
        out.push(if a <= b { (a, b) } else { (b, a) });
    }
    out
}

fn mesh_row(store: &Store, mesh_id: i64) -> Result<Value, ApiError> {
    store
        .conn
        .query_row(
            "SELECT id,name,digest,format,vertex_count,face_count,seq,created_at FROM meshes WHERE id=?1",
            [mesh_id],
            |r| {
                Ok(json!({
                    "id": r.get::<_, i64>(0)?, "name": r.get::<_, String>(1)?,
                    "digest": r.get::<_, String>(2)?, "format": r.get::<_, String>(3)?,
                    "vertex_count": r.get::<_, i64>(4)?, "face_count": r.get::<_, i64>(5)?,
                    "seq": r.get::<_, i64>(6)?, "created_at": r.get::<_, i64>(7)?,
                }))
            },
        )
        .map_err(|_| ApiError::not_found(format!("mesh {mesh_id}")))
}

fn run_row_meta(store: &Store, run_id: i64) -> Result<(i64, ToleranceConfig), ApiError> {
    store
        .conn
        .query_row(
            "SELECT mesh_id,precision,vertex_eps,degenerate_area_eps FROM runs WHERE id=?1",
            [run_id],
            |r| {
                Ok((
                    r.get::<_, i64>(0)?,
                    ToleranceConfig {
                        precision: r.get::<_, i64>(1)? as u32,
                        vertex_eps: r.get(2)?,
                        degenerate_area_eps: r.get(3)?,
                    },
                ))
            },
        )
        .map_err(|_| ApiError::not_found(format!("run {run_id}")))
}

fn proposal_from_row(r: &rusqlite::Row) -> rusqlite::Result<ProposalRow> {
    let payload: String = r.get(4)?;
    let bedges: String = r.get(5)?;
    let status: String = r.get(6)?;
    Ok(ProposalRow {
        id: r.get(0)?,
        issue_id: r.get(1)?,
        mesh_id: r.get(2)?,
        kind: r.get(3)?,
        payload: serde_json::from_str(&payload).unwrap_or(Value::Null),
        boundary_edges: serde_json::from_str(&bedges).unwrap_or_default(),
        status: ProposalStatus::parse(&status).unwrap_or(ProposalStatus::Draft),
        created_at: r.get(7)?,
        decided_at: r.get(8)?,
    })
}

fn load_proposal(store: &Store, id: i64) -> Result<ProposalRow, ApiError> {
    store
        .conn
        .query_row(
            "SELECT id,issue_id,mesh_id,kind,payload_json,boundary_edges_json,status,created_at,decided_at
             FROM proposals WHERE id=?1",
            [id],
            |r| proposal_from_row(r),
        )
        .map_err(|_| ApiError::not_found(format!("proposal {id}")))
}

fn load_proposals_for_issue(store: &Store, issue_id: i64) -> Result<Vec<ProposalRow>, ApiError> {
    let mut stmt = store.conn.prepare(
        "SELECT id,issue_id,mesh_id,kind,payload_json,boundary_edges_json,status,created_at,decided_at
         FROM proposals WHERE issue_id=?1 ORDER BY id",
    )?;
    let rows = stmt
        .query_map([issue_id], |r| proposal_from_row(r))?
        .collect::<Result<Vec<_>, _>>()?;
    Ok(rows)
}

fn accepted_proposals(store: &Store, mesh_id: i64) -> Result<Vec<ProposalRow>, ApiError> {
    let mut stmt = store.conn.prepare(
        "SELECT id,issue_id,mesh_id,kind,payload_json,boundary_edges_json,status,created_at,decided_at
         FROM proposals WHERE mesh_id=?1 AND status='accepted' ORDER BY id",
    )?;
    let rows = stmt
        .query_map([mesh_id], |r| proposal_from_row(r))?
        .collect::<Result<Vec<_>, _>>()?;
    Ok(rows)
}

fn proposal_json(p: &ProposalRow) -> Value {
    json!({
        "id": p.id,
        "issue_id": p.issue_id,
        "mesh_id": p.mesh_id,
        "kind": p.kind,
        "payload": p.payload,
        "boundary_edges": p.boundary_edges,
        "status": p.status.as_str(),
        "created_at": p.created_at,
        "decided_at": p.decided_at,
    })
}

/// 工作网格重放:应用已接受提案,统计面数/边界/流形性,并输出来源映射。
/// 返回 (stats, [(proposal_id, new_vertex_id, source_vertex_id)])。
fn replay(mesh: &Mesh, proposals: &[ProposalRow]) -> (MeshStats, Vec<(i64, i64, i64)>) {
    #[derive(Clone)]
    struct WFace {
        orig: i64,
        tri: [i64; 3],
    }
    let mut faces: Vec<WFace> = mesh
        .faces
        .iter()
        .filter(|f| {
            // 缺失面(引用不存在的顶点)不进入拓扑
            mesh.vertex(f.a).is_some() && mesh.vertex(f.b).is_some() && mesh.vertex(f.c).is_some()
        })
        .map(|f| WFace { orig: f.idx, tri: [f.a, f.b, f.c] })
        .collect();
    let mut sources: Vec<(i64, i64, i64)> = Vec::new();
    let mut next_new = NEW_VERTEX_BASE;

    for p in proposals {
        // 新顶点占位符 -> 实际 id(确定性顺序)
        let mut placeholder: std::collections::BTreeMap<i64, i64> = Default::default();
        if let Some(nvs) = p.payload["new_vertices"].as_array() {
            for nv in nvs {
                let ref_id = nv["ref"].as_i64().unwrap_or(0);
                let fresh = next_new;
                next_new += 1;
                placeholder.insert(ref_id, fresh);
                if let Some(srcs) = nv["sources"].as_array() {
                    for s in srcs {
                        if let Some(sid) = s.as_i64() {
                            sources.push((p.id, fresh, sid));
                        }
                    }
                }
            }
        }
        let map_id = |id: i64| -> i64 { *placeholder.get(&id).unwrap_or(&id) };
        match p.kind.as_str() {
            "flip_orientation" => {
                if let Some(list) = p.payload["faces"].as_array() {
                    let ids: Vec<i64> = list.iter().filter_map(|v| v.as_i64()).collect();
                    for f in faces.iter_mut() {
                        if ids.contains(&f.orig) {
                            f.tri.swap(1, 2);
                        }
                    }
                }
            }
            "remove_face" => {
                if let Some(list) = p.payload["faces"].as_array() {
                    let ids: Vec<i64> = list.iter().filter_map(|v| v.as_i64()).collect();
                    faces.retain(|f| !ids.contains(&f.orig));
                }
            }
            "split_nonmanifold_edge" => {
                let edge: Vec<i64> = p.payload["edge"]
                    .as_array()
                    .map(|a| a.iter().filter_map(|v| v.as_i64()).collect())
                    .unwrap_or_default();
                let moved: Vec<i64> = p.payload["moved_faces"]
                    .as_array()
                    .map(|a| a.iter().filter_map(|v| v.as_i64()).collect())
                    .unwrap_or_default();
                if edge.len() == 2 {
                    let (nu, nv) = (map_id(-1), map_id(-2));
                    for f in faces.iter_mut() {
                        if moved.contains(&f.orig) {
                            for slot in f.tri.iter_mut() {
                                if *slot == edge[0] {
                                    *slot = nu;
                                } else if *slot == edge[1] {
                                    *slot = nv;
                                }
                            }
                        }
                    }
                }
            }
            "close_hole" => {
                if let Some(nfs) = p.payload["new_faces"].as_array() {
                    for nf in nfs {
                        let tri: Vec<i64> = nf
                            .as_array()
                            .map(|a| a.iter().filter_map(|v| v.as_i64()).collect())
                            .unwrap_or_default();
                        if tri.len() == 3 {
                            faces.push(WFace {
                                orig: 0,
                                tri: [map_id(tri[0]), map_id(tri[1]), map_id(tri[2])],
                            });
                        }
                    }
                }
            }
            "contract_vertices" => {
                let srcs: Vec<i64> = p.payload["sources"]
                    .as_array()
                    .map(|a| a.iter().filter_map(|v| v.as_i64()).collect())
                    .unwrap_or_default();
                let new_id = map_id(-1);
                for f in faces.iter_mut() {
                    for slot in f.tri.iter_mut() {
                        if srcs.contains(slot) {
                            *slot = new_id;
                        }
                    }
                }
            }
            _ => {}
        }
    }

    // 统计
    let mut edges: std::collections::BTreeMap<(i64, i64), i64> = Default::default();
    for f in &faces {
        for i in 0..3 {
            let (a, b) = (f.tri[i], f.tri[(i + 1) % 3]);
            let key = if a <= b { (a, b) } else { (b, a) };
            *edges.entry(key).or_default() += 1;
        }
    }
    let mut boundary = 0i64;
    let mut nonmanifold = 0i64;
    for &c in edges.values() {
        if c == 1 {
            boundary += 1;
        } else if c > 2 {
            nonmanifold += 1;
        }
    }
    (
        MeshStats {
            face_count: faces.len() as i64,
            boundary_edges: boundary,
            nonmanifold_edges: nonmanifold,
            manifold: boundary == 0 && nonmanifold == 0,
        },
        sources,
    )
}

// ByteRange 序列化辅助(供测试与证据展示)
#[allow(dead_code)]
fn ranges_json(ranges: &[ByteRange]) -> Value {
    json!(ranges.iter().map(|r| json!({"offset": r.offset, "len": r.len})).collect::<Vec<_>>())
}
