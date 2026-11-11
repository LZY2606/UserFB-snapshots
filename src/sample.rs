//! 内置四组样例数据，覆盖需求里的四个典型故障形态。
//! 时间全部用固定 Unix 时间戳，保证离线可复现。

use crate::model::*;

/// 场景元数据。
pub struct ScenarioInfo {
    pub id: &'static str,
    pub title: &'static str,
    pub description: &'static str,
    /// 该场景里预期失败的记录 nonce。
    pub failed_nonces: Vec<i64>,
}

pub fn scenarios() -> Vec<ScenarioInfo> {
    vec![
        ScenarioInfo {
            id: "circular_referral",
            title: "循环 referral",
            description: "A→B→A 的跨 realm 转诊成环，transited 中 realm 重复，referral 无法终止到服务票据。",
            failed_nonces: vec![1003],
        },
        ScenarioInfo {
            id: "duplicate_kvno_keys",
            title: "同一 kvno 下多把密钥",
            description: "轮转中间态里 kvno=7 存在两把不同指纹的密钥，无法唯一确定密钥代次。",
            failed_nonces: vec![2001],
        },
        ScenarioInfo {
            id: "replay_after_restart",
            title: "服务重启后 authenticator 重现",
            description: "同指纹在 gen1 内重复即 replay；服务重启进入 gen2 后分区不同，重现不算 replay。",
            failed_nonces: vec![3002],
        },
        ScenarioInfo {
            id: "renewable_not_renewed",
            title: "票据可续期但没续",
            description: "观测时间已过 end_time 但仍在 renew_till 内，带 renewable 标志却没有续期动作。",
            failed_nonces: vec![4001],
        },
    ]
}

const A: &str = "A.EXAMPLE";
const B: &str = "B.EXAMPLE";
const C: &str = "C.EXAMPLE";
const BASE_T: i64 = 1_700_000_000;

#[allow(clippy::too_many_arguments)]
fn rec(
    source: &str,
    realm: &str,
    principal: &str,
    etype: &str,
    kvno: i64,
    nonce: i64,
    parent: Option<i64>,
    flags: &[&str],
    start: i64,
    end: i64,
    renew_till: Option<i64>,
    transited: &[&str],
    fp: Option<&str>,
    svc: Option<&str>,
    gen: Option<i64>,
    observed: Option<i64>,
    failed: bool,
    error_code: Option<i64>,
) -> RecordInput {
    RecordInput {
        source: source.into(),
        realm: realm.into(),
        principal: principal.into(),
        etype: etype.into(),
        kvno,
        flags: flags.iter().map(|s| s.to_string()).collect(),
        auth_time: BASE_T + start,
        start_time: BASE_T + start,
        end_time: BASE_T + end,
        renew_till: renew_till.map(|t| BASE_T + t),
        nonce,
        transited: transited.iter().map(|s| s.to_string()).collect(),
        authenticator_fp: fp.map(|s| s.to_string()),
        raw_ref: format!("capture://{source}/{nonce}.bin#offset=0"),
        client_observed_time: observed.map(|t| BASE_T + t),
        service_instance: svc.map(|s| s.to_string()),
        boot_generation: gen,
        parent_id: parent,
        failed,
        error_code,
    }
}

/// 四组合并成一个导入文件（主体互不重叠，可共存在同一库中）。
pub fn bundled_import() -> ImportFile {
    let mut f = ImportFile::default();

    // ---- 场景 1：循环 referral ----
    f.records.push(rec(
        "as", A, &format!("krbtgt/{A}"), "aes256-cts-hmac-sha1-96", 1,
        1001, None, &["forwardable"], 0, 3600, None, &[],
        None, None, None, Some(10), false, None,
    ));
    f.records.push(rec(
        "tgs", B, &format!("krbtgt/{B}"), "aes256-cts-hmac-sha1-96", 2,
        1002, Some(1001), &["forwardable"], 20, 3620, None, &[A],
        None, None, None, Some(30), false, None,
    ));
    // 转诊回到 A：transited 里 A 第二次出现，构成环。
    f.records.push(rec(
        "tgs", A, &format!("krbtgt/{A}"), "aes256-cts-hmac-sha1-96", 1,
        1003, Some(1002), &["forwardable"], 40, 3640, None, &[A, B, A],
        None, None, None, Some(50), true, Some(0x28),
    ));
    f.records.push(rec(
        "tgs", C, "http/app.C.EXAMPLE", "aes256-cts-hmac-sha1-96", 3,
        1004, Some(1003), &["forwardable"], 60, 3660, None, &[A, B, A, C],
        Some("9f1c2d"), Some("http/app.C.EXAMPLE"), None, Some(70), false, None,
    ));

    // ---- 场景 2：同一 kvno 多把密钥 ----
    f.records.push(rec(
        "ap", A, "http/app.A.EXAMPLE", "aes256-cts-hmac-sha1-96", 7,
        2001, None, &["forwardable"], 0, 7200, None, &[A],
        Some("aa11bb22"), Some("http/app.A.EXAMPLE"), Some(1), Some(100), true, Some(41),
    ));

    // ---- 场景 3：重启后 authenticator 重现 ----
    f.records.push(rec(
        "ap", A, "host/web1.A.EXAMPLE", "aes256-cts-hmac-sha1-96", 3,
        3001, None, &[], 2900, 6500, None, &[A],
        Some("fp-replay-77"), Some("host/web1.A.EXAMPLE"), Some(1), Some(3000), false, None,
    ));
    // 同分区（gen1）内同指纹第二次出现：replay 命中。
    f.records.push(rec(
        "ap", A, "host/web1.A.EXAMPLE", "aes256-cts-hmac-sha1-96", 3,
        3002, None, &[], 2900, 6500, None, &[A],
        Some("fp-replay-77"), Some("host/web1.A.EXAMPLE"), Some(1), Some(3010), true, Some(34),
    ));
    // 服务重启进入 gen2，新 replay 分区：同指纹重现不构成 replay。
    f.records.push(rec(
        "ap", A, "host/web1.A.EXAMPLE", "aes256-cts-hmac-sha1-96", 3,
        3003, None, &[], 5050, 8650, None, &[A],
        Some("fp-replay-77"), Some("host/web1.A.EXAMPLE"), Some(2), Some(5100), false, None,
    ));

    // ---- 场景 4：可续期但未续 ----
    f.records.push(rec(
        "tgs", A, "ldap/db.A.EXAMPLE", "aes256-cts-hmac-sha1-96", 5,
        4001, None, &["forwardable", "renewable"], 0, 2000, Some(10_000), &[A],
        Some("ldap-auth-55"), Some("ldap/db.A.EXAMPLE"), Some(1), Some(3000), true, Some(32),
    ));

    // ---- 密钥代次 ----
    let lab = "lab.keytab";
    let old = "lab-rollback.keytab";
    let key = |realm: &str, principal: &str, etype: &str, kvno: i64, fp: &str, kt: &str, vf: i64| KeyInput {
        realm: realm.into(),
        principal: principal.into(),
        etype: etype.into(),
        kvno,
        fingerprint: fp.into(),
        keytab: kt.into(),
        valid_from: BASE_T + vf,
    };
    let et = "aes256-cts-hmac-sha1-96";

    // 场景 1 的跨 realm 密钥
    f.keys.push(key(A, &format!("krbtgt/{A}"), et, 1, "key-a-krbtgt-1", lab, -3600));
    f.keys.push(key(B, &format!("krbtgt/{B}"), et, 2, "key-b-krbtgt-2", lab, -1800));
    f.keys.push(key(C, "http/app.C.EXAMPLE", et, 3, "key-c-http-3", lab, -1800));

    // 场景 2：kvno=7 两把指纹不同的密钥（轮转中间态），外加旧代次
    f.keys.push(key(A, "http/app.A.EXAMPLE", et, 7, "fp-key-7-alpha", lab, -500));
    f.keys.push(key(A, "http/app.A.EXAMPLE", et, 7, "fp-key-7-bravo", lab, -100));
    f.keys.push(key(A, "http/app.A.EXAMPLE", et, 6, "fp-key-6-old", lab, -7200));

    // 场景 3
    f.keys.push(key(A, "host/web1.A.EXAMPLE", et, 3, "fp-web1-key-3", lab, 2000));

    // 场景 4
    f.keys.push(key(A, "ldap/db.A.EXAMPLE", et, 5, "fp-ldap-key-5", lab, -1000));

    // 回滚 keytab：只有旧代次，用来演示换假设后候选排序变化
    f.keys.push(key(A, "http/app.A.EXAMPLE", et, 6, "fp-key-6-old", old, -7200));
    f.keys.push(key(A, "ldap/db.A.EXAMPLE", et, 4, "fp-ldap-key-4", old, -9000));
    f.keys.push(key(A, "host/web1.A.EXAMPLE", et, 2, "fp-web1-key-2", old, 0));

    // ---- 服务重启事件 ----
    f.boots.push(BootEvent {
        service_instance: "host/web1.A.EXAMPLE".into(),
        generation: 1,
        started_at: BASE_T + 2000,
    });
    f.boots.push(BootEvent {
        service_instance: "host/web1.A.EXAMPLE".into(),
        generation: 2,
        started_at: BASE_T + 5000,
    });

    f
}

/// 基线规则与替代假设规则。
pub fn baseline_ruleset() -> RulesetInput {
    RulesetInput {
        name: "baseline (A→B→C, lab.keytab, skew 300s)".into(),
        trust_path: vec![A.into(), B.into(), C.into()],
        keytab: "lab.keytab".into(),
        clock_skew_secs: 300,
    }
}

pub fn alternate_ruleset() -> RulesetInput {
    RulesetInput {
        name: "alt hypothesis (A→C, rollback keytab, skew 900s)".into(),
        trust_path: vec![A.into(), C.into()],
        keytab: "lab-rollback.keytab".into(),
        clock_skew_secs: 900,
    }
}
