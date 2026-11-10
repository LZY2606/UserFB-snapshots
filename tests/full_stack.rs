//! 全栈集成测试：fixture 导入、HTTP API、窗口 revision 乐观锁、
//! 并行审议冲突、校准 digest 换版影响清单、发布冻结与恢复幂等。
mod common;

use common::{json_get, json_post, request, TestEnv};
use acoustic_pulse::json::Json;

fn as_array(v: &Json) -> &Vec<Json> {
    match v {
        Json::Arr(a) => a,
        other => panic!("期望数组，得到 {other:?}"),
    }
}

fn find_measurement(env: &TestEnv, code: &str) -> i64 {
    let list = json_get(env, "/api/measurements");
    as_array(&list)
        .iter()
        .find(|m| m.str_field("code") == Some(code))
        .and_then(|m| m.i64_field("id"))
        .unwrap_or_else(|| panic!("缺少测量 {code}"))
}

#[test]
fn bootstrap_creates_three_scenes_and_no_upload_route() {
    let env = TestEnv::new();
    let list = json_get(&env, "/api/measurements");
    let codes: Vec<&str> = as_array(&list)
        .iter()
        .map(|m| m.str_field("code").unwrap())
        .collect();
    assert!(codes.contains(&"M-CLEAN"));
    assert!(codes.contains(&"M-DRIFT"));
    assert!(codes.contains(&"M-DROP"));

    // fixture 文件确实落在本地目录
    let fx = env.root.join("fixtures");
    for name in [
        "excitation_sweep.apwf",
        "recording_clean.apwf",
        "recording_drift.apwf",
        "recording_dropped.apwf",
        "cal_mic_v1.json",
        "cal_mic_v2.json",
    ] {
        assert!(fx.join(name).exists(), "缺少 fixture {name}");
    }

    // 任意上传尝试都必须失败：服务根本没有上传路由
    let r = request(&env, "POST", "/api/upload", Some("{\"file\":\"x\"}"));
    assert_eq!(r.status, 404);
    let r2 = request(&env, "PUT", "/api/waveforms", Some("{}"));
    assert_eq!(r2.status, 404);

    let health = json_get(&env, "/api/health");
    assert_eq!(health.str_field("waveform_source"), Some("本地 fixture，运行时不上传"));
}

#[test]
fn drift_and_drop_scenes_are_evidenced() {
    let env = TestEnv::new();
    let drift_id = find_measurement(&env, "M-DRIFT");
    let drop_id = find_measurement(&env, "M-DROP");
    let clean_id = find_measurement(&env, "M-CLEAN");

    let drift = json_get(&env, &format!("/api/measurements/{drift_id}"));
    assert_eq!(drift.str_field("clock_status"), Some("DriftSuspect"));
    if let Json::Num(ppm) = drift.get("clock_ppm").unwrap() {
        assert!(ppm.abs() > 200.0, "漂移场景 ppm={ppm}");
    } else {
        panic!("clock_ppm 应为数字");
    }

    let drop = json_get(&env, &format!("/api/measurements/{drop_id}"));
    let flags: Vec<usize> = match acoustic_pulse::json::parse(drop.str_field("drop_flags").unwrap()).unwrap() {
        Json::Arr(a) => a.iter().map(|v| v.as_i64().unwrap() as usize).collect(),
        _ => panic!(),
    };
    assert!(flags.contains(&3000), "丢帧场景必须标记样本 3000，实际 {flags:?}");

    let clean = json_get(&env, &format!("/api/measurements/{clean_id}"));
    assert_eq!(clean.str_field("clock_status"), Some("Ok"));
    let clean_flags = acoustic_pulse::json::parse(clean.str_field("drop_flags").unwrap()).unwrap();
    assert_eq!(as_array(&clean_flags).len(), 0);
}

#[test]
fn candidates_peaks_reflections_and_decay() {
    let env = TestEnv::new();
    let mid = find_measurement(&env, "M-CLEAN");
    let cands = json_get(&env, &format!("/api/measurements/{mid}/candidates"));
    let arr = as_array(&cands);
    assert_eq!(arr.len(), 2, "两个逆滤波候选必须同时保留");
    for c in arr {
        assert_eq!(c.i64_field("peak_index"), Some(200));
        let refs = acoustic_pulse::json::parse(c.str_field("reflections").unwrap()).unwrap();
        let picks: Vec<i64> = as_array(&refs).iter().map(|v| v.as_i64().unwrap()).collect();
        assert!(picks.contains(&290) && picks.contains(&410), "反射 {picks:?}");
        let curve = acoustic_pulse::json::parse(c.str_field("decay_curve").unwrap()).unwrap();
        let n = as_array(&curve).len() as i64;
        assert_eq!(n, c.i64_field("decay_end").unwrap() - c.i64_field("decay_start").unwrap() + 1);
    }
}

#[test]
fn window_revisions_append_with_baseline_and_reject_stale_writes() {
    let env = TestEnv::new();
    let mid = find_measurement(&env, "M-CLEAN");
    let cands = json_get(&env, &format!("/api/measurements/{mid}/candidates"));
    let cid = as_array(&cands)[0].i64_field("id").unwrap();

    let initial = json_get(&env, &format!("/api/measurements/{mid}/windows"));
    // 引导后：首个候选已有 rev0/rev1 两版，第二个候选有 rev0，共 3 条
    let own: Vec<i64> = as_array(&initial)
        .iter()
        .filter(|w| w.i64_field("candidate_id") == Some(cid))
        .map(|w| w.i64_field("revision").unwrap())
        .collect();
    assert_eq!(own, vec![0, 1], "引导即生成两版窗口证据");
    let rev0 = as_array(&initial)
        .iter()
        .find(|w| w.i64_field("candidate_id") == Some(cid) && w.i64_field("revision") == Some(0))
        .unwrap();
    assert_eq!(rev0.i64_field("sample_count"), Some(2001), "闭区间 200..2200 共 2001 点");
    let rev1 = as_array(&initial)
        .iter()
        .find(|w| w.i64_field("candidate_id") == Some(cid) && w.i64_field("revision") == Some(1))
        .unwrap();
    // rev1 必须回指基线 rev0 的端点
    assert_eq!(rev1.get("baseline").unwrap().i64_field("revision"), Some(0));
    assert_eq!(rev1.get("baseline").unwrap().i64_field("start_sample"), Some(200));

    // 并改：基于最新 rev1 提交 rev2，响应同时返回新端点与基线
    let body = format!(
        r#"{{"candidate_id":{cid},"expected_revision":1,"start_sample":190,"end_sample":2190,
           "rationale":"测试并改","author":"reviewer-a"}}"#
    );
    let (status, resp) = json_post(&env, &format!("/api/measurements/{mid}/windows"), &body);
    assert_eq!(status, 200, "{resp:?}");
    assert_eq!(resp.i64_field("new_revision"), Some(2));
    let new_eps = resp.get("new_endpoints").unwrap();
    assert_eq!(new_eps.i64_field("start_sample"), Some(190));
    assert_eq!(new_eps.i64_field("end_sample"), Some(2190));
    assert_eq!(new_eps.i64_field("sample_count"), Some(2001), "190..2190 闭区间仍为 2001 点");
    let baseline = resp.get("baseline").unwrap();
    assert_eq!(baseline.i64_field("revision"), Some(1));
    assert_eq!(baseline.i64_field("start_sample"), Some(190 - 10));

    // 旧 revision 仍然存在、未被覆盖
    let after = json_get(&env, &format!("/api/measurements/{mid}/windows"));
    let revs: Vec<i64> = as_array(&after)
        .iter()
        .filter(|w| w.i64_field("candidate_id") == Some(cid))
        .map(|w| w.i64_field("revision").unwrap())
        .collect();
    assert_eq!(revs, vec![0, 1, 2]);

    // 用过时 expected_revision 并行提交 → 409 拒绝覆盖
    let stale = format!(
        r#"{{"candidate_id":{cid},"expected_revision":0,"start_sample":180,"end_sample":2180,
           "rationale":"过时基线","author":"parallel-agent"}}"#
    );
    let (status2, err) = json_post(&env, &format!("/api/measurements/{mid}/windows"), &stale);
    assert_eq!(status2, 409);
    assert_eq!(err.str_field("error"), Some("version_conflict"));

    // 非法闭区间 → 400
    let bad = format!(
        r#"{{"candidate_id":{cid},"expected_revision":2,"start_sample":99,"end_sample":50,
           "rationale":"端点反转","author":"x"}}"#
    );
    let (status3, _) = json_post(&env, &format!("/api/measurements/{mid}/windows"), &bad);
    assert_eq!(status3, 400);
}

#[test]
fn parallel_reviews_conflict_on_version() {
    let env = TestEnv::new();
    let mid = find_measurement(&env, "M-CLEAN");
    let ok = r#"{"expected_version":0,"reviewer":"r1","decision":"Accepted","note":"首次审议"}"#;
    let (s1, r1) = json_post(&env, &format!("/api/measurements/{mid}/reviews"), ok);
    assert_eq!(s1, 200, "{r1:?}");
    assert_eq!(r1.i64_field("new_version"), Some(1));

    let stale = r#"{"expected_version":0,"reviewer":"r2","decision":"Rejected","note":"并行过时提交"}"#;
    let (s2, err) = json_post(&env, &format!("/api/measurements/{mid}/reviews"), stale);
    assert_eq!(s2, 409);
    assert!(err.str_field("message").unwrap().contains("并行提交被拒绝"));

    let list = json_get(&env, &format!("/api/measurements/{mid}/reviews"));
    assert_eq!(as_array(&list).len(), 1, "被拒绝的审议不得入库");
}

#[test]
fn publish_freezes_calibration_and_replacement_makes_impact_list() {
    let env = TestEnv::new();
    let mid = find_measurement(&env, "M-CLEAN");
    let cands = json_get(&env, &format!("/api/measurements/{mid}/candidates"));
    let cid = as_array(&cands)[0].i64_field("id").unwrap();
    let wins = json_get(&env, &format!("/api/measurements/{mid}/windows"));
    let wid = as_array(&wins)
        .iter()
        .find(|w| w.i64_field("candidate_id") == Some(cid))
        .unwrap()
        .i64_field("id")
        .unwrap();

    let cals_before = json_get(&env, "/api/calibrations");
    let v1 = as_array(&cals_before)
        .iter()
        .find(|c| c.i64_field("version") == Some(1))
        .unwrap();
    let v1_digest = v1.str_field("digest").unwrap().to_string();
    assert_eq!(v1.get("active"), Some(&Json::Bool(true)));

    let pub_body = format!(r#"{{"candidate_id":{cid},"window_revision_id":{wid}}}"#);
    let (s, published) = json_post(&env, &format!("/api/measurements/{mid}/publish"), &pub_body);
    assert_eq!(s, 200, "{published:?}");
    assert_eq!(published.str_field("calibration_digest_frozen"), Some(v1_digest.as_str()));
    let response_digest_before = published.str_field("response_digest").unwrap().to_string();

    // 换校准：生成影响清单，不静默重算
    let (s2, change) = json_post(
        &env,
        "/api/calibrations/replace",
        r#"{"file_name":"cal_mic_v2.json"}"#,
    );
    assert_eq!(s2, 200, "{change:?}");
    assert_eq!(change.i64_field("impact_count"), Some(1));
    let change_id = change.i64_field("change_id").unwrap();

    let impact = json_get(&env, &format!("/api/calibration-changes/{change_id}/impact"));
    let items = as_array(&impact);
    assert_eq!(items.len(), 1);
    assert_eq!(items[0].i64_field("measurement_id"), Some(mid));
    assert_eq!(items[0].str_field("status"), Some("NeedsReview"));
    assert_eq!(items[0].str_field("frozen_calibration_digest"), Some(v1_digest.as_str()));

    // 已发布响应原样保留：digest 不变，校准 digest 仍冻结在 v1
    let published_list = json_get(&env, &format!("/api/measurements/{mid}/published"));
    let p = &as_array(&published_list)[0];
    assert_eq!(p.str_field("calibration_digest"), Some(v1_digest.as_str()));
    assert_eq!(p.str_field("response_digest"), Some(response_digest_before.as_str()));

    // 新版本校准处于生效中
    let cals_after = json_get(&env, "/api/calibrations");
    let v2 = as_array(&cals_after)
        .iter()
        .find(|c| c.i64_field("version") == Some(2))
        .unwrap();
    assert_eq!(v2.get("active"), Some(&Json::Bool(true)));
    let v1_after = as_array(&cals_after)
        .iter()
        .find(|c| c.i64_field("version") == Some(1))
        .unwrap();
    assert_eq!(v1_after.get("active"), Some(&Json::Bool(false)));
}

#[test]
fn restart_does_not_duplicate_derivatives() {
    let env = TestEnv::new();
    let mid = find_measurement(&env, "M-CLEAN");
    let before = as_array(&json_get(&env, &format!("/api/measurements/{mid}/candidates"))).len();
    assert_eq!(before, 2);

    // 模拟进程恢复：对同一数据库与 fixture 目录再跑一次 bootstrap
    let same_db = acoustic_pulse::db::Db::open(
        &env.root.join("test.db").to_string_lossy(),
    )
    .unwrap();
    let state2 = std::sync::Arc::new(acoustic_pulse::service::AppState::new(
        same_db,
        env.root.join("fixtures"),
    ));
    acoustic_pulse::service::bootstrap(&state2).unwrap();

    let after = json_get(&env, &format!("/api/measurements/{mid}/candidates"));
    assert_eq!(as_array(&after).len(), 2, "恢复后不得重复生成候选衍生物");
    let measurements = json_get(&env, "/api/measurements");
    assert_eq!(as_array(&measurements).len(), 3, "测量摘要不得重复导入");
}

#[test]
fn index_page_shows_title_and_evidence() {
    let env = TestEnv::new();
    let page = request(&env, "GET", "/", None);
    assert_eq!(page.status, 200);
    assert!(page.body.contains("声场脉络"), "首页必须出现“声场脉络”");
    assert!(page.body.contains("反射到达时间标记与两版窗口证据"));
    let js = request(&env, "GET", "/app.js", None);
    assert_eq!(js.status, 200);
    assert!(js.body.contains("drawTimeline"));
}
