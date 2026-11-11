use meshdesk::app::{App, PRESET_MESH};
use meshdesk::clock::{FixedClock, SeqClock};
use meshdesk::model::ToleranceConfig;
use meshdesk::store::Store;
use serde_json::json;
use std::sync::Arc;

fn mem_app() -> App {
    App::new(Store::open_memory().unwrap(), Arc::new(FixedClock(1_700_000_000_000))).unwrap()
}

fn tmp_db(name: &str) -> String {
    let dir = std::env::temp_dir().join("meshdesk-tests");
    std::fs::create_dir_all(&dir).unwrap();
    let p = dir.join(format!("{name}-{}.db", std::process::id()));
    let _ = std::fs::remove_file(&p);
    p.to_string_lossy().to_string()
}

/// 简单金字塔缺一个侧面:一个三角形洞 + 一致法向
const SIMPLE: &str = "\
v 0 0 0
v 1 0 0
v 1 1 0
v 0 1 0
v 0.5 0.5 1
f 1 2 5
f 2 3 5
f 3 4 5
f 1 3 2
f 1 4 3
";

fn issue_keys(app: &App, run_id: i64) -> Vec<String> {
    let run = app.get_run(run_id).unwrap();
    let mut keys: Vec<String> = run["issues"]
        .as_array()
        .unwrap()
        .iter()
        .map(|i| i["issue_key"].as_str().unwrap().to_string())
        .collect();
    keys.sort();
    keys
}

// ---------- 1. 正常流程 ----------

#[test]
fn happy_path_detect_propose_preview_accept_export() {
    let app = mem_app();
    let mesh = app.create_mesh("case", SIMPLE).unwrap();
    let mesh_id = mesh["mesh_id"].as_i64().unwrap();
    assert_eq!(mesh["digest"].as_str().unwrap().len(), 64);

    let tol = ToleranceConfig::default();
    let run = app.start_detection(mesh_id, tol, None).unwrap();
    let run_id = run["run_id"].as_i64().unwrap();

    // 检测运行固定网格 digest 与容差
    let run_info = app.get_run(run_id).unwrap();
    assert_eq!(run_info["run"]["status"], "completed");
    assert_eq!(run_info["run"]["mesh_digest"], mesh["digest"]);
    assert_eq!(run_info["run"]["tolerance"]["vertex_eps"], json!(tol.vertex_eps));
    let issues = run_info["issues"].as_array().unwrap();
    assert!(issues.iter().any(|i| i["kind"] == "boundary_loop"));
    // 证据含原字节位置
    let hole = issues.iter().find(|i| i["kind"] == "boundary_loop").unwrap();
    assert!(!hole["evidence"]["byte_positions"].as_array().unwrap().is_empty());
    assert!(!hole["evidence"]["vertices"].as_array().unwrap().is_empty());

    // 生成候选:三角洞应有 3 个补片候选
    let issue_id = hole["id"].as_i64().unwrap();
    let cands = app.generate_candidates(issue_id).unwrap();
    let proposals = cands["proposals"].as_array().unwrap();
    assert_eq!(proposals.len(), 3);
    assert!(proposals.iter().all(|p| p["kind"] == "close_hole"));

    // 预览:面数/边界差异
    let pid = proposals[0]["id"].as_i64().unwrap();
    let p = app.get_proposal(pid).unwrap();
    let preview = &p["preview"];
    assert_eq!(preview["before"]["boundary_edges"], json!(3));
    assert!(preview["delta_faces"].as_i64().unwrap() > 0);
    assert_eq!(preview["delta_boundary_edges"], json!(-3));

    // 接受:生成修订,原网格不变
    let accepted = app.accept_proposal(pid, 0).unwrap();
    assert_eq!(accepted["revision"]["seq"], json!(1));
    let raw_after = app.get_mesh(mesh_id).unwrap();
    assert_eq!(raw_after["face_count"], json!(5)); // 原始面数不变
    let state = app.mesh_state(mesh_id).unwrap();
    assert_eq!(state["mesh"]["seq"], json!(1));
    assert_eq!(state["current_stats"]["boundary_edges"], json!(0));

    // 历史修订可查
    let revs = app.list_revisions(mesh_id).unwrap();
    assert_eq!(revs["revisions"].as_array().unwrap().len(), 1);

    // 导出引用确定提案集并快照容差
    let exp = app.create_export(mesh_id, 1).unwrap();
    let exp_id = exp["id"].as_i64().unwrap();
    assert_eq!(exp["proposal_ids"], json!([pid]));
    assert_eq!(exp["tolerance_snapshot"]["vertex_eps"], json!(tol.vertex_eps));

    // 后续容差改动不影响旧产物
    let mut tol2 = tol;
    tol2.vertex_eps = 9.9;
    app.start_detection(mesh_id, tol2, None).unwrap();
    let exp_after = app.get_export(exp_id).unwrap();
    assert_eq!(exp_after["tolerance_snapshot"]["vertex_eps"], json!(tol.vertex_eps));
    assert_eq!(exp_after["proposal_set_hash"], exp["proposal_set_hash"]);
}

// ---------- 2. 输入乱序 ----------

#[test]
fn out_of_order_faces_before_vertices_same_issues() {
    let ordered = "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\nf 1 2 3\nf 2 4 3\n";
    let shuffled = "f 1 2 3\nf 2 4 3\nv 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\n";
    let app = mem_app();
    let m1 = app.create_mesh("a", ordered).unwrap();
    let m2 = app.create_mesh("b", shuffled).unwrap();
    // 字节不同 digest 不同
    assert_ne!(m1["digest"], m2["digest"]);
    let r1 = app.start_detection(m1["mesh_id"].as_i64().unwrap(), ToleranceConfig::default(), None).unwrap();
    let r2 = app.start_detection(m2["mesh_id"].as_i64().unwrap(), ToleranceConfig::default(), None).unwrap();
    // 问题集合一致(证据规范化后)
    assert_eq!(
        issue_keys(&app, r1["run_id"].as_i64().unwrap()),
        issue_keys(&app, r2["run_id"].as_i64().unwrap())
    );
}

// ---------- 3. 边界端点 ----------

#[test]
fn tolerance_endpoints_are_inclusive() {
    // 顶点距离恰好 == vertex_eps 视为重合;超出则不是
    let content = "v 0 0 0\nv 0.5 0 0\nv 10 0 0\nv 10.5000001 0 0\n";
    let app = mem_app();
    let m = app.create_mesh("eps", content).unwrap();
    let mesh_id = m["mesh_id"].as_i64().unwrap();
    let tol = ToleranceConfig { precision: 6, vertex_eps: 0.5, degenerate_area_eps: 1e-9 };
    let run = app.start_detection(mesh_id, tol, None).unwrap();
    let info = app.get_run(run["run_id"].as_i64().unwrap()).unwrap();
    let groups: Vec<&serde_json::Value> = info["issues"]
        .as_array()
        .unwrap()
        .iter()
        .filter(|i| i["kind"] == "coincident_vertices")
        .collect();
    assert_eq!(groups.len(), 1, "恰好等于容差的顶点应重合,超出的不应");
    assert_eq!(groups[0]["evidence"]["detail"]["group"], json!([1, 2]));
}

#[test]
fn degenerate_area_endpoint_inclusive() {
    // 面积恰好 == degenerate_area_eps 视为退化;略大则不是
    // 三角形 (0,0,0),(1,0,0),(0.5,h,0) 面积 = h/2
    let content = "v 0 0 0\nv 1 0 0\nv 0.5 0.5 0\nv 0.5 0.500001 0\nf 1 2 3\nf 1 2 4\n";
    let app = mem_app();
    let m = app.create_mesh("deg", content).unwrap();
    let tol = ToleranceConfig { precision: 6, vertex_eps: 1e-9, degenerate_area_eps: 0.25 };
    let run = app.start_detection(m["mesh_id"].as_i64().unwrap(), tol, None).unwrap();
    let info = app.get_run(run["run_id"].as_i64().unwrap()).unwrap();
    let degen: Vec<i64> = info["issues"]
        .as_array()
        .unwrap()
        .iter()
        .filter(|i| i["kind"] == "degenerate_face")
        .map(|i| i["evidence"]["faces"][0].as_i64().unwrap())
        .collect();
    assert_eq!(degen, vec![1], "面积恰为容差的面退化,略大的不退化");
}

// ---------- 4. 幂等重试(内容相同) ----------

#[test]
fn idempotent_retry_same_content_replays() {
    let app = mem_app();
    let body = json!({"name": "m", "content": SIMPLE});
    let (st1, r1) = app
        .idempotent(Some("key-1"), "create_mesh", &body, |app| {
            Ok((201, app.create_mesh("m", SIMPLE)?))
        })
        .unwrap();
    let (st2, r2) = app
        .idempotent(Some("key-1"), "create_mesh", &body, |app| {
            Ok((201, app.create_mesh("m", SIMPLE)?))
        })
        .unwrap();
    assert_eq!(st1, st2);
    assert_eq!(r1, r2);
    // 只创建了一个网格
    assert_eq!(r1["mesh_id"], json!(1));
    assert!(app.get_mesh(2).is_err());
}

// ---------- 5. 幂等键重用冲突 ----------

#[test]
fn idempotency_key_reuse_with_different_content_conflicts() {
    let app = mem_app();
    let body1 = json!({"name": "m1", "content": SIMPLE});
    app.idempotent(Some("key-x"), "create_mesh", &body1, |app| {
        Ok((201, app.create_mesh("m1", SIMPLE)?))
    })
    .unwrap();
    let body2 = json!({"name": "m2", "content": "v 0 0 0\n"});
    let err = app
        .idempotent(Some("key-x"), "create_mesh", &body2, |app| {
            Ok((201, app.create_mesh("m2", "v 0 0 0\n")?))
        })
        .unwrap_err();
    assert_eq!(err.code, "IDEMPOTENCY_CONFLICT");
    assert_eq!(err.status, 409);
}

// ---------- 6. 版本冲突 ----------

#[test]
fn accept_with_stale_version_conflicts() {
    let app = mem_app();
    let m = app.create_mesh("case", SIMPLE).unwrap();
    let mesh_id = m["mesh_id"].as_i64().unwrap();
    let run = app.start_detection(mesh_id, ToleranceConfig::default(), None).unwrap();
    let info = app.get_run(run["run_id"].as_i64().unwrap()).unwrap();
    let hole = info["issues"]
        .as_array()
        .unwrap()
        .iter()
        .find(|i| i["kind"] == "boundary_loop")
        .unwrap();
    let cands = app.generate_candidates(hole["id"].as_i64().unwrap()).unwrap();
    let p1 = cands["proposals"][0]["id"].as_i64().unwrap();
    let p2 = cands["proposals"][1]["id"].as_i64().unwrap();

    // 错误的期望版本
    let err = app.accept_proposal(p1, 7).unwrap_err();
    assert_eq!(err.code, "VERSION_CONFLICT");

    // 正确版本接受 p1 后,p2 用旧版本号 → 版本冲突
    app.accept_proposal(p1, 0).unwrap();
    let err = app.accept_proposal(p2, 0).unwrap_err();
    assert_eq!(err.code, "VERSION_CONFLICT");
}

// ---------- 7. 中断后恢复 ----------

#[test]
fn interrupted_detection_recovers_after_reopen() {
    let path = tmp_db("recovery");
    let run_id;
    let clean_keys;
    {
        let app = App::new(Store::open(&path).unwrap(), Arc::new(FixedClock(100))).unwrap();
        let m = app.create_mesh("case", PRESET_MESH).unwrap();
        let mesh_id = m["mesh_id"].as_i64().unwrap();
        // 模拟第 3 条证据落库后中断
        let err = app
            .start_detection(mesh_id, ToleranceConfig::default(), Some(3))
            .unwrap_err();
        assert!(err.message.contains("interrupted"));
        // 对照:完整检测的问题集合
        let full = app.start_detection(mesh_id, ToleranceConfig::default(), None).unwrap();
        clean_keys = issue_keys(&app, full["run_id"].as_i64().unwrap());
        let store = app.store.lock().unwrap();
        run_id = store
            .conn
            .query_row(
                "SELECT id FROM runs WHERE status='running' ORDER BY id LIMIT 1",
                [],
                |r| r.get::<_, i64>(0),
            )
            .unwrap();
    }
    // 重新打开:启动恢复将 running 标记为 interrupted
    let app = App::new(Store::open(&path).unwrap(), Arc::new(FixedClock(200))).unwrap();
    let info = app.get_run(run_id).unwrap();
    assert_eq!(info["run"]["status"], "interrupted");
    assert_eq!(info["issues"].as_array().unwrap().len(), 3, "部分证据保留");

    // 恢复执行:确定性重放,替换部分证据
    let resumed = app.resume_detection(run_id).unwrap();
    assert_eq!(resumed["resumed"], json!(true));
    let info = app.get_run(run_id).unwrap();
    assert_eq!(info["run"]["status"], "completed");
    assert_eq!(info["run"]["completed_at"], json!(200));
    let mut keys: Vec<String> = info["issues"]
        .as_array()
        .unwrap()
        .iter()
        .map(|i| i["issue_key"].as_str().unwrap().to_string())
        .collect();
    keys.sort();
    assert_eq!(keys, clean_keys, "恢复后结果与完整检测一致");
}

// ---------- 预置案例:冲突现场触发与可追溯失败记录 ----------

#[test]
fn preset_case_triggers_traceable_conflict() {
    let app = App::new(Store::open_memory().unwrap(), Arc::new(SeqClock::new(vec![1000, 2000, 3000, 4000]))).unwrap();
    let seed = app.seed_preset().unwrap();
    let mesh_id = seed["mesh_id"].as_i64().unwrap();
    let run_id = seed["run_id"].as_i64().unwrap();

    // 预置案例覆盖全部问题类型
    let info = app.get_run(run_id).unwrap();
    let kinds: std::collections::BTreeSet<&str> = info["issues"]
        .as_array()
        .unwrap()
        .iter()
        .map(|i| i["kind"].as_str().unwrap())
        .collect();
    for k in [
        "nonmanifold_edge", "boundary_loop", "normal_inconsistent", "degenerate_face",
        "missing_face", "normal_undeterminable", "self_intersection", "coincident_vertices",
    ] {
        assert!(kinds.contains(k), "预置案例应包含 {k}");
    }

    // 找到三角洞 [1,4,5],生成多个补片候选
    let hole = info["issues"]
        .as_array()
        .unwrap()
        .iter()
        .find(|i| i["kind"] == "boundary_loop" && i["evidence"]["detail"]["loop"].as_array().unwrap().len() == 3)
        .unwrap();
    let cands = app.generate_candidates(hole["id"].as_i64().unwrap()).unwrap();
    let proposals = cands["proposals"].as_array().unwrap();
    assert_eq!(proposals.len(), 3, "同一洞保留多个局部修复候选");
    let p1 = proposals[0]["id"].as_i64().unwrap();
    let p2 = proposals[1]["id"].as_i64().unwrap();

    // 接受第一个补片成功
    app.accept_proposal(p1, 0).unwrap();
    // 第二个补片触及相同边界 → 冲突,失败记录可追溯
    let err = app.accept_proposal(p2, 1).unwrap_err();
    assert_eq!(err.code, "PROPOSAL_CONFLICT");
    assert_eq!(err.status, 409);

    let conflicts = app.list_conflicts(mesh_id).unwrap();
    let list = conflicts["conflicts"].as_array().unwrap();
    assert_eq!(list.len(), 1);
    let c = &list[0];
    assert_eq!(c["status"], "unresolved");
    assert_eq!(c["proposal_id"], json!(p2));
    assert_eq!(c["other_proposal_id"], json!(p1));
    assert!(!c["shared_edges"].as_array().unwrap().is_empty());
    assert!(c["created_at"].as_i64().unwrap() > 0, "失败记录带注入时间");

    // 失败提案状态可查
    let p2row = app.get_proposal(p2).unwrap();
    assert_eq!(p2row["status"], "conflicted");

    // 冲突可标记解决
    let cid = c["id"].as_i64().unwrap();
    app.resolve_conflict(cid).unwrap();
    let conflicts = app.list_conflicts(mesh_id).unwrap();
    assert_eq!(conflicts["conflicts"][0]["status"], "resolved");
}

// ---------- 缩并保留来源映射 ----------

#[test]
fn contraction_keeps_source_mapping() {
    let app = mem_app();
    let m = app.create_mesh("case", PRESET_MESH).unwrap();
    let mesh_id = m["mesh_id"].as_i64().unwrap();
    let run = app.start_detection(mesh_id, ToleranceConfig::default(), None).unwrap();
    let info = app.get_run(run["run_id"].as_i64().unwrap()).unwrap();
    let coinc = info["issues"]
        .as_array()
        .unwrap()
        .iter()
        .find(|i| i["kind"] == "coincident_vertices")
        .unwrap();
    let group: Vec<i64> = coinc["evidence"]["detail"]["group"]
        .as_array()
        .unwrap()
        .iter()
        .map(|v| v.as_i64().unwrap())
        .collect();
    assert_eq!(group, vec![1, 13, 14]);

    let cands = app.generate_candidates(coinc["id"].as_i64().unwrap()).unwrap();
    let p = &cands["proposals"][0];
    assert_eq!(p["kind"], "contract_vertices");
    let pid = p["id"].as_i64().unwrap();
    app.accept_proposal(pid, 0).unwrap();

    // 来源映射:新顶点 → 全部来源顶点
    let vs = app.vertex_sources(mesh_id).unwrap();
    let srcs: std::collections::BTreeSet<i64> = vs["vertex_sources"]
        .as_array()
        .unwrap()
        .iter()
        .filter(|r| r["proposal_id"] == json!(pid))
        .map(|r| r["source_vertex_id"].as_i64().unwrap())
        .collect();
    assert_eq!(srcs, group.into_iter().collect());
    let new_id = vs["vertex_sources"][0]["new_vertex_id"].as_i64().unwrap();
    assert!(new_id >= 1_000_000_000, "新顶点使用独立 id 空间,不与原 id 合并");
}

// ---------- 三种面状态是不同状态 ----------

#[test]
fn face_states_are_distinct() {
    let app = mem_app();
    let m = app.create_mesh("case", PRESET_MESH).unwrap();
    let run = app.start_detection(m["mesh_id"].as_i64().unwrap(), ToleranceConfig::default(), None).unwrap();
    let info = app.get_run(run["run_id"].as_i64().unwrap()).unwrap();
    let issues = info["issues"].as_array().unwrap();
    let state_of = |kind: &str| {
        issues
            .iter()
            .find(|i| i["kind"] == kind)
            .map(|i| i["evidence"]["detail"]["face_state"].as_str().unwrap().to_string())
    };
    assert_eq!(state_of("degenerate_face").as_deref(), Some("degenerate"));
    assert_eq!(state_of("missing_face").as_deref(), Some("missing"));
    assert_eq!(
        state_of("normal_undeterminable").as_deref(),
        Some("normal_undeterminable")
    );
}

// ---------- 时间全部由注入时钟产生 ----------

#[test]
fn timestamps_come_from_injected_clock() {
    let app = App::new(
        Store::open_memory().unwrap(),
        Arc::new(SeqClock::new(vec![111, 222, 333])),
    )
    .unwrap();
    let m = app.create_mesh("c", SIMPLE).unwrap();
    let mesh_id = m["mesh_id"].as_i64().unwrap();
    assert_eq!(app.get_mesh(mesh_id).unwrap()["created_at"], json!(111));
    let run = app.start_detection(mesh_id, ToleranceConfig::default(), None).unwrap();
    let info = app.get_run(run["run_id"].as_i64().unwrap()).unwrap();
    assert_eq!(info["run"]["created_at"], json!(222));
    assert_eq!(info["run"]["completed_at"], json!(333));
}
