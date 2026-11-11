//! 集成测试：referral 终止、票据时间窗口、replay key 分区、
//! 候选密钥排序、事务回滚、导出再导入，以及分支结论保留。

use kerbdiag::db::Db;
use kerbdiag::engine;
use kerbdiag::model::*;
use kerbdiag::sample;

const ET: &str = "aes256-cts-hmac-sha1-96";
const T: i64 = 1_700_000_000;

fn mem_db() -> Db {
    Db::open_memory().unwrap()
}

fn record_by_nonce(records: &[Record], nonce: i64) -> &Record {
    records.iter().find(|r| r.nonce == nonce).unwrap()
}

fn ruleset_row(db: &mut Db, input: &RulesetInput) -> kerbdiag::db::RulesetRow {
    let id = db.upsert_ruleset(input).unwrap();
    db.list_rulesets().unwrap().into_iter().find(|r| r.id == id).unwrap()
}

fn make_record(nonce: i64, parent: Option<i64>, source: &str, principal: &str) -> RecordInput {
    RecordInput {
        source: source.into(),
        realm: "A.EXAMPLE".into(),
        principal: principal.into(),
        etype: ET.into(),
        kvno: 1,
        flags: vec![],
        auth_time: T,
        start_time: T,
        end_time: T + 3600,
        renew_till: None,
        nonce,
        transited: vec!["A.EXAMPLE".into()],
        authenticator_fp: None,
        raw_ref: format!("capture://test/{nonce}"),
        client_observed_time: Some(T + 100),
        service_instance: None,
        boot_generation: None,
        parent_id: parent,
        failed: false,
        error_code: None,
    }
}

// ---------- 1. referral 能终止 / 成环 ----------

#[test]
fn referral_loop_is_detected_and_healthy_chain_terminates() {
    let mut db = mem_db();
    db.import_file(&sample::bundled_import()).unwrap();
    let records = db.all_records().unwrap();
    let ruleset = ruleset_row(&mut db, &sample::baseline_ruleset());

    // 样例场景 1：nonce 1003 的 transited 为 A,B,A —— 必须判环且不能终止。
    let looped_rec = record_by_nonce(&records, 1003);
    let report = engine::referral_report(&records, &ruleset, looped_rec.id);
    assert!(report.looped, "A→B→A 应被识别为 referral 环");
    assert!(!report.terminates);
    assert!(report.loop_realms.contains(&"A.EXAMPLE".to_string()));

    // 健康链：AS TGT → 一张服务票据，必须正常终止。
    let mut healthy = ImportFile::default();
    let mut tgt = make_record(1, None, "as", "krbtgt/A.EXAMPLE");
    tgt.transited.clear(); // AS 阶段 transited 为空
    healthy.records.push(tgt);
    let mut svc = make_record(2, Some(1), "tgs", "http/app.A.EXAMPLE");
    svc.transited = vec!["A.EXAMPLE".into()];
    healthy.records.push(svc);
    let mut db2 = mem_db();
    db2.import_file(&healthy).unwrap();
    let recs2 = db2.all_records().unwrap();
    let rs2 = ruleset_row(&mut db2, &sample::baseline_ruleset());
    let svc_rec = recs2.iter().find(|r| r.nonce == 2).unwrap();
    let ok_report = engine::referral_report(&recs2, &rs2, svc_rec.id);    assert!(!ok_report.looped);
    assert!(ok_report.terminates, "无环且终点为 service 层时应正常终止");
    assert_eq!(ok_report.chain.len(), 2);
}

// ---------- 2. 票据时间窗口 + 时钟偏移仅作候选 ----------

#[test]
fn time_window_and_clock_skew_candidate_never_rewrites_facts() {
    let base = Record {
        id: 0, source: "ap".into(), layer: "service".into(),
        realm: "A.EXAMPLE".into(), principal: "http/app.A.EXAMPLE".into(),
        etype: ET.into(), kvno: 1, flags: vec![],
        auth_time: T, start_time: T + 100, end_time: T + 200, renew_till: None,
        nonce: 9, transited: vec![], authenticator_fp: None, raw_ref: "x".into(),
        client_observed_time: Some(T + 250), service_instance: None,
        boot_generation: None, parent_id: None, failed: true, error_code: None,
    };

    // 过期 50 秒，规则只允许 30 秒：判过期，且不给 skew 候选。
    let (status, skew) = engine::check_window(&base, 30);
    assert_eq!(status, engine::WindowStatus::Expired);
    assert_eq!(skew, None);

    // 同一事实，规则允许 60 秒：仍判过期，但产生 50 秒的候选解释。
    let (status, skew) = engine::check_window(&base, 60);
    assert_eq!(status, engine::WindowStatus::Expired);
    assert_eq!(skew, Some(50));

    // 采集时间必须保持原值，引擎不允许回写。
    assert_eq!(base.client_observed_time, Some(T + 250));
    assert_eq!(base.end_time, T + 200);

    // 尚未生效。
    let early = Record { client_observed_time: Some(T + 50), ..base.clone() };
    let (status, skew) = engine::check_window(&early, 30);
    assert_eq!(status, engine::WindowStatus::NotYet);
    assert_eq!(skew, None); // 差 50 秒 > 30

    // 窗口内。
    let inside = Record { client_observed_time: Some(T + 150), ..base.clone() };
    assert_eq!(engine::check_window(&inside, 30).0, engine::WindowStatus::Inside);

    // 缺观测时间。
    let unknown = Record { client_observed_time: None, ..base.clone() };
    assert_eq!(engine::check_window(&unknown, 30).0, engine::WindowStatus::Unknown);
}

#[test]
fn renewable_ticket_expired_without_renewal_is_flagged() {
    let mut db = mem_db();
    db.import_file(&sample::bundled_import()).unwrap();
    let records = db.all_records().unwrap();
    let keys = db.all_keys().unwrap();
    let ruleset = ruleset_row(&mut db, &sample::baseline_ruleset());
    let rec = record_by_nonce(&records, 4001);

    let d = engine::diagnose(&records, &keys, &engine::replay_hits(&records), &ruleset, rec);
    assert_eq!(d.kind, FailureKind::RenewableNotRenewed.as_str());
    assert!(d.evidence.iter().any(|e| e.code == "renewable_not_renewed"));
}

// ---------- 3. replay key：服务实例 + 启动代次分区 ----------

#[test]
fn replay_cache_is_partitioned_by_instance_and_boot_generation() {
    let mut db = mem_db();
    db.import_file(&sample::bundled_import()).unwrap();
    let records = db.all_records().unwrap();
    let hits = engine::replay_hits(&records);

    let first = record_by_nonce(&records, 3001);
    let repeat_same_gen = record_by_nonce(&records, 3002);
    let after_restart = record_by_nonce(&records, 3003);

    // 首次出现不算 replay；同分区第二次才算。
    assert!(!hits.contains_key(&first.id));
    assert!(hits.contains_key(&repeat_same_gen.id));
    assert_eq!(
        engine::replay_partition(repeat_same_gen).as_deref(),
        Some("host/web1.A.EXAMPLE#gen1")
    );

    // 服务重启进入 gen2：分区不同，同指纹重现不构成 replay。
    assert!(!hits.contains_key(&after_restart.id));
    assert_eq!(
        engine::replay_partition(after_restart).as_deref(),
        Some("host/web1.A.EXAMPLE#gen2")
    );

    // 诊断结论必须是 replay，且证据里带分区键。
    let ruleset = ruleset_row(&mut db, &sample::baseline_ruleset());
    let d = engine::diagnose(&records, &db.all_keys().unwrap(), &hits, &ruleset, repeat_same_gen);
    assert_eq!(d.kind, FailureKind::Replay.as_str());
    assert_eq!(d.replay_partition.as_deref(), Some("host/web1.A.EXAMPLE#gen1"));
}

// ---------- 4. 候选密钥排序 ----------

#[test]
fn candidate_keys_are_ranked_exact_first_and_ambiguity_flagged() {
    let mut db = mem_db();
    db.import_file(&sample::bundled_import()).unwrap();
    let records = db.all_records().unwrap();
    let keys = db.all_keys().unwrap();
    let ruleset = ruleset_row(&mut db, &sample::baseline_ruleset());
    let rec = record_by_nonce(&records, 2001); // kvno=7，lab.keytab 下有两把 kvno=7

    let candidates = engine::rank_candidates(&keys, &ruleset, rec);
    assert_eq!(candidates.len(), 3, "lab.keytab 下该主体有 kvno7×2 + kvno6×1");
    assert!(candidates[0].exact && candidates[1].exact, "精确匹配排最前");
    assert_eq!(candidates[0].rank, 0);
    assert_eq!(candidates[2].kvno, 6, "旧代次 kvno=6 排最后");
    // 排序确定性：同档内 valid_from 新的优先。
    assert!(candidates[0].valid_from >= candidates[1].valid_from);

    let d = engine::diagnose(&records, &keys, &engine::replay_hits(&records), &ruleset, rec);
    assert_eq!(d.kind, FailureKind::KeyMismatch.as_str());
    assert!(d.evidence.iter().any(|e| e.code == "ambiguous_key_generation"));
}

#[test]
fn rollback_keytab_has_no_exact_candidate() {
    let mut db = mem_db();
    db.import_file(&sample::bundled_import()).unwrap();
    let records = db.all_records().unwrap();
    let keys = db.all_keys().unwrap();
    let ruleset = ruleset_row(&mut db, &sample::alternate_ruleset());
    let rec = record_by_nonce(&records, 2001);

    let candidates = engine::rank_candidates(&keys, &ruleset, rec);
    assert!(!candidates.is_empty());
    assert!(candidates.iter().all(|c| !c.exact), "回滚 keytab 只有 kvno6，无精确匹配");
    assert!(candidates.iter().all(|c| c.keytab == "lab-rollback.keytab"));
}

// ---------- 5. 事务回滚 ----------

#[test]
fn bad_import_rolls_back_the_whole_batch() {
    let mut db = mem_db();
    db.import_file(&sample::bundled_import()).unwrap();
    let before = db.count_records().unwrap();

    let mut bad = ImportFile::default();
    bad.records.push(make_record(5001, None, "ap", "http/x.A.EXAMPLE"));
    bad.records.push(make_record(5002, Some(999999), "ap", "http/y.A.EXAMPLE")); // 悬空父引用

    let err = db.import_file(&bad);
    assert!(err.is_err(), "悬空 parent_id 必须导致导入失败");
    assert_eq!(db.count_records().unwrap(), before, "回滚后事实表行数不变");
    let nonces: Vec<i64> = db.all_records().unwrap().iter().map(|r| r.nonce).collect();
    assert!(!nonces.contains(&5001) && !nonces.contains(&5002), "坏批次一行都不能落库");
}

// ---------- 6. 导出再导入 ----------

#[test]
fn export_then_import_roundtrip_preserves_everything() {
    let mut db1 = mem_db();
    db1.import_file(&sample::bundled_import()).unwrap();
    let exported = db1.export_all().unwrap();

    let mut db2 = mem_db();
    db2.import_file(&exported).unwrap();
    let re_exported = db2.export_all().unwrap();

    let j1 = serde_json::to_value(&exported).unwrap();
    let j2 = serde_json::to_value(&re_exported).unwrap();
    assert_eq!(j1, j2, "导出 → 导入 → 再导出必须完全一致");

    // 谱系边也要保留：1003 的父是 1002，1004 的父是 1003。
    let recs = db2.all_records().unwrap();
    let by_nonce = |n: i64| recs.iter().find(|r| r.nonce == n).unwrap();
    let parent_nonce = |n: i64| {
        let r = by_nonce(n);
        r.parent_id.map(|pid| recs.iter().find(|x| x.id == pid).unwrap().nonce)
    };
    assert_eq!(parent_nonce(1003), Some(1002));
    assert_eq!(parent_nonce(1004), Some(1003));
}

// ---------- 7. 假设分支与规则版本保留 ----------

#[test]
fn new_branch_keeps_prior_conclusions_and_ruleset_versions() {
    let mut db = mem_db();
    db.import_file(&sample::bundled_import()).unwrap();

    let base_id = db.upsert_ruleset(&sample::baseline_ruleset()).unwrap();
    let alt_id = db.upsert_ruleset(&sample::alternate_ruleset()).unwrap();
    assert_ne!(base_id, alt_id, "不同假设必须是不同规则版本");

    // 同内容重复写入应命中哈希去重。
    let base_again = db.upsert_ruleset(&sample::baseline_ruleset()).unwrap();
    assert_eq!(base_id, base_again);

    let b1 = db.create_branch("baseline", None, base_id, T).unwrap();
    let branch1 = db.list_branches().unwrap().into_iter().find(|b| b.id == b1).unwrap();
    engine::run_branch(&mut db, &branch1).unwrap();

    let b2 = db.create_branch("alt", Some(b1), alt_id, T + 1).unwrap();
    let branch2 = db.list_branches().unwrap().into_iter().find(|b| b.id == b2).unwrap();
    engine::run_branch(&mut db, &branch2).unwrap();

    // 同一条失败记录在两个分支各有结论，且分别钉在各自的规则版本上。
    let records = db.all_records().unwrap();
    let target = record_by_nonce(&records, 2001).id;
    let d1 = db.diagnoses_for_record(target).unwrap();
    assert_eq!(d1.len(), 2, "两个分支的结论都必须保留");
    let rulesets: std::collections::HashSet<i64> = d1.iter().map(|d| d.ruleset_id).collect();
    assert!(rulesets.contains(&base_id) && rulesets.contains(&alt_id));

    // 重跑分支 1 不能污染分支 2 的结论数量。
    engine::run_branch(&mut db, &branch1).unwrap();
    assert_eq!(db.diagnoses_for_record(target).unwrap().len(), 2);
    assert_eq!(db.diagnoses_for_branch(b2).unwrap().len(),
               db.all_records().unwrap().iter().filter(|r| r.failed).count() as i64
                   as usize);
}

// ---------- 8. 四组样例场景的端到端结论 ----------

#[test]
fn bundled_scenarios_produce_expected_failure_kinds() {
    let mut db = mem_db();
    db.import_file(&sample::bundled_import()).unwrap();
    let ruleset_id = db.upsert_ruleset(&sample::baseline_ruleset()).unwrap();
    let branch_id = db.create_branch("baseline", None, ruleset_id, T).unwrap();
    let branch = db.list_branches().unwrap().into_iter().find(|b| b.id == branch_id).unwrap();
    let produced = engine::run_branch(&mut db, &branch).unwrap();

    let expected = [
        (1003i64, FailureKind::ReferralLoop),
        (2001, FailureKind::KeyMismatch),
        (3002, FailureKind::Replay),
        (4001, FailureKind::RenewableNotRenewed),
    ];
    let records = db.all_records().unwrap();
    let all = db.all_diagnoses().unwrap();
    assert_eq!(produced, expected.len());
    for (nonce, kind) in expected {
        let rid = record_by_nonce(&records, nonce).id;
        let d = all.iter().find(|d| d.record_id == rid).unwrap();
        assert_eq!(d.kind, kind.as_str(), "nonce {nonce} 的结论应为 {:?}", kind);
        assert!(!d.evidence.is_empty(), "每条结论都要带判断依据");
    }
}
