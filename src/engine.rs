//! 离线分析引擎。
//!
//! 所有判定只读事实表与某版不可变规则，输出带证据链的 [`Diagnosis`]。
//! 关键约束：时钟偏移只产生 `clock_skew_candidate_secs` 这类候选解释，
//! 不会修改任何采集到的时间字段。

use std::collections::{HashMap, HashSet};

use crate::db::{BranchRow, Db, RulesetRow};
use crate::model::*;

/// referral 链分析结果。
#[derive(Debug, Clone)]
pub struct ReferralReport {
    /// 链上的记录 id（沿谱系父边从服务票据回溯到 TGT）。
    pub chain: Vec<i64>,
    /// 是否成环（记录节点或 transited realm 重复）。
    pub looped: bool,
    /// 环上的 realm 序列（looped 时有值）。
    pub loop_realms: Vec<String>,
    /// 是否最终落到服务票据（正常终止）。
    pub terminates: bool,
    /// transited 是否符合规则假设的信任路径（子序列匹配）。
    pub trust_path_ok: bool,
    /// 不符合信任路径的 realm。
    pub trust_path_violators: Vec<String>,
}

/// 沿谱系父边回溯整条票据链，并做 referral 环与信任路径检查。
pub fn referral_report(records: &[Record], ruleset: &RulesetRow, start_id: i64) -> ReferralReport {
    let by_id: HashMap<i64, &Record> = records.iter().map(|r| (r.id, r)).collect();
    let mut chain = Vec::new();
    let mut seen_ids: HashSet<i64> = HashSet::new();
    let mut cur = Some(start_id);
    let mut looped = false;

    while let Some(id) = cur {
        if !seen_ids.insert(id) {
            looped = true;
            break;
        }
        let rec = match by_id.get(&id) {
            Some(r) => *r,
            None => break,
        };
        chain.push(id);
        cur = rec.parent_id;
    }
    chain.reverse();

    // 汇总链上 transited realm，检查 realm 级重复（跨 realm referral 环）。
    let mut seen_realms: HashSet<String> = HashSet::new();
    let mut loop_realms: Vec<String> = Vec::new();
    for id in &chain {
        for realm in &by_id[id].transited {
            if !seen_realms.insert(realm.clone()) && !loop_realms.contains(realm) {
                loop_realms.push(realm.clone());
                looped = true;
            }
        }
    }

    // 链头（起点侧，即回溯的最后一张/起始那张）决定是否终止：
    // 起点记录本身是 service 层且没有继续向下游要票，即正常终止。
    let start_rec = by_id[&start_id];
    let terminates = !looped && start_rec.layer == TicketLayer::Service.as_str();

    // 信任路径：链上所有 transited realm 必须是规则 trust_path 的子序列。
    let mut violators: Vec<String> = Vec::new();
    let mut cursor = 0usize;
    for id in &chain {
        for realm in &by_id[id].transited {
            match ruleset.trust_path.iter().position(|r| r == realm) {
                Some(idx) if idx >= cursor => cursor = idx + 1,
                _ => {
                    if !violators.contains(realm) {
                        violators.push(realm.clone());
                    }
                }
            }
        }
    }

    ReferralReport {
        chain,
        looped,
        loop_realms,
        terminates,
        trust_path_ok: violators.is_empty(),
        trust_path_violators: violators,
    }
}

/// replay cache 分区键：服务实例 + 启动代次。
/// 不同代次（服务重启）之间 authenticator 指纹重复不算 replay。
pub fn replay_partition(rec: &Record) -> Option<String> {
    match (&rec.service_instance, rec.boot_generation) {
        (Some(svc), Some(gen)) => Some(format!("{svc}#gen{gen}")),
        _ => None,
    }
}

/// 在全量记录里找出每个 replay 分区内的 authenticator 指纹重复组。
/// 返回 `(record_id, 分区键)`，这些记录构成 replay 命中。
pub fn replay_hits(records: &[Record]) -> HashMap<i64, String> {
    let mut seen: HashMap<(String, String), i64> = HashMap::new();
    let mut hits = HashMap::new();
    for r in records {
        if let (Some(partition), Some(fp)) = (replay_partition(r), &r.authenticator_fp) {
            let key = (partition.clone(), fp.clone());
            if let Some(_first) = seen.get(&key) {
                hits.insert(r.id, partition.clone());
            } else {
                seen.insert(key, r.id);
            }
        }
    }
    hits
}

/// 候选密钥排序。
///
/// 排序规则（确定性）：
/// 1. etype 与 kvno 同时精确匹配优先；
/// 2. 仅 kvno 匹配次之；
/// 3. 同主体的其它代次按 kvno 差距升序（保留的旧密钥也能被看到）；
/// 4. 同档内认证时刻已生效（valid_from <= auth_time）的优先，再按 valid_from 新到旧；
/// 5. 最后以指纹字典序兜底。
pub fn rank_candidates(keys: &[KeyRow], ruleset: &RulesetRow, rec: &Record) -> Vec<CandidateKey> {
    let mut pool: Vec<&KeyRow> = keys
        .iter()
        .filter(|k| {
            k.keytab == ruleset.keytab && k.realm == rec.realm && k.principal == rec.principal
        })
        .collect();

    pool.sort_by(|a, b| {
        let tier = |k: &KeyRow| -> u8 {
            match (k.kvno == rec.kvno, k.etype == rec.etype) {
                (true, true) => 0,
                (true, false) => 1,
                _ => 2,
            }
        };
        let ta = tier(a);
        let tb = tier(b);
        ta.cmp(&tb)
            .then_with(|| (a.kvno - rec.kvno).abs().cmp(&(b.kvno - rec.kvno).abs()))
            .then_with(|| b.valid_from.cmp(&a.valid_from))
            .then_with(|| a.fingerprint.cmp(&b.fingerprint))
    });

    pool.into_iter()
        .enumerate()
        .map(|(idx, k)| {
            let active = k.valid_from <= rec.auth_time;
            let exact = k.kvno == rec.kvno && k.etype == rec.etype;
            let reason = if exact {
                format!(
                    "etype/kvno 精确匹配（kvno={}, etype={}），认证时刻{}已生效",
                    k.kvno,
                    k.etype,
                    if active { "前" } else { "后" }
                )
            } else if k.kvno == rec.kvno {
                format!("kvno={} 相同但 etype 不同（票据 {} vs 密钥 {}）", k.kvno, rec.etype, k.etype)
            } else {
                format!(
                    "同主体相邻代次 kvno={}（票据 kvno={}），valid_from={}",
                    k.kvno, k.kvno, k.valid_from
                )
            };
            CandidateKey {
                key_id: k.id,
                kvno: k.kvno,
                etype: k.etype.clone(),
                fingerprint: k.fingerprint.clone(),
                keytab: k.keytab.clone(),
                valid_from: k.valid_from,
                rank: idx,
                reason,
                exact,
            }
        })
        .collect()
}

/// 票据时间窗口检查。只读取采集时间，返回窗口状态与（可能的）时钟偏移候选。
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum WindowStatus {
    /// 在有效期内。
    Inside,
    /// 尚未生效。
    NotYet,
    /// 已过期。
    Expired,
    /// 缺少观测时间，无法判断。
    Unknown,
}

pub fn check_window(rec: &Record, skew_secs: i64) -> (WindowStatus, Option<i64>) {
    let obs = match rec.client_observed_time {
        Some(t) => t,
        None => return (WindowStatus::Unknown, None),
    };
    if obs < rec.start_time {
        let delta = rec.start_time - obs;
        let candidate = (delta <= skew_secs).then_some(delta);
        return (WindowStatus::NotYet, candidate);
    }
    if obs > rec.end_time {
        let delta = obs - rec.end_time;
        let candidate = (delta <= skew_secs).then_some(delta);
        return (WindowStatus::Expired, candidate);
    }
    (WindowStatus::Inside, None)
}

/// 是否存在续期动作：同主体、带 renewed 标志、时间晚于本票据 auth_time 的记录。
fn renewed_after(records: &[Record], rec: &Record) -> bool {
    records.iter().any(|other| {
        other.id != rec.id
            && other.realm == rec.realm
            && other.principal == rec.principal
            && other.flags.iter().any(|f| f == "renewed")
            && other.auth_time >= rec.auth_time
    })
}

/// 对一条失败记录做完整诊断。
pub fn diagnose(
    records: &[Record],
    keys: &[KeyRow],
    replay: &HashMap<i64, String>,
    ruleset: &RulesetRow,
    rec: &Record,
) -> Diagnosis {
    let mut evidence: Vec<Evidence> = Vec::new();
    let mut kind: Option<FailureKind> = None;
    let partition = replay_partition(rec);

    // 1. referral 环 / 信任路径。
    let report = referral_report(records, ruleset, rec.id);
    if report.looped {
        kind.get_or_insert(FailureKind::ReferralLoop);
        evidence.push(Evidence {
            code: "referral_loop".into(),
            detail: format!(
                "票据谱系成环，重复 realm：{}；链上记录：{}",
                if report.loop_realms.is_empty() {
                    "(节点重复)".to_string()
                } else {
                    report.loop_realms.join(" -> ")
                },
                report.chain.iter().map(|i| i.to_string()).collect::<Vec<_>>().join(" -> ")
            ),
        });
    }
    if !report.trust_path_ok {
        if kind.is_none() {
            kind = Some(FailureKind::TrustPathViolation);
        }
        evidence.push(Evidence {
            code: "trust_path_violation".into(),
            detail: format!(
                "transited 中的 realm 不在假设信任路径 {:?} 内：{}",
                ruleset.trust_path,
                report.trust_path_violators.join(", ")
            ),
        });
    }

    // 2. replay（同服务实例、同启动代次内 authenticator 指纹重复）。
    if let Some(part) = replay.get(&rec.id) {
        if kind.is_none() {
            kind = Some(FailureKind::Replay);
        }
        evidence.push(Evidence {
            code: "replay_hit".into(),
            detail: format!(
                "replay 分区 {part} 内 authenticator 指纹 {} 重复出现（客户端重试不豁免）",
                rec.authenticator_fp.clone().unwrap_or_else(|| "?".into())
            ),
        });
    } else if let Some(part) = &partition {
        evidence.push(Evidence {
            code: "replay_partition".into(),
            detail: format!("replay 分区 {part} 内无 authenticator 指纹冲突"),
        });
    }

    // 3. 时间窗口（采集时间不变，skew 只做候选解释）。
    let (status, skew_candidate) = check_window(rec, ruleset.clock_skew_secs);
    match status {
        WindowStatus::Expired => {
            let renewable = rec.flags.iter().any(|f| f == "renewable");
            let within_renew = rec.renew_till.map(|t| {
                rec.client_observed_time.map(|obs| obs <= t).unwrap_or(false)
            }).unwrap_or(false);
            if renewable && within_renew && !renewed_after(records, rec) {
                if kind.is_none() {
                    kind = Some(FailureKind::RenewableNotRenewed);
                }
                evidence.push(Evidence {
                    code: "renewable_not_renewed".into(),
                    detail: format!(
                        "票据 {} 已过期，renew_till={} 内仍可续期，但未观察到续期请求",
                        rec.end_time,
                        rec.renew_till.unwrap_or(0)
                    ),
                });
            } else {
                if kind.is_none() {
                    kind = Some(FailureKind::TicketExpired);
                }
                evidence.push(Evidence {
                    code: "ticket_expired".into(),
                    detail: format!(
                        "采集观测时间 {} 晚于 end_time {}（原始时间未被修改）",
                        rec.client_observed_time.unwrap_or(0),
                        rec.end_time
                    ),
                });
            }
            if let Some(delta) = skew_candidate {
                evidence.push(Evidence {
                    code: "clock_skew_candidate".into(),
                    detail: format!(
                        "候选解释：时钟偏移 {delta} 秒，落在规则允许的 {} 秒内；仅为候选，不回写采集时间",
                        ruleset.clock_skew_secs
                    ),
                });
            }
        }
        WindowStatus::NotYet => {
            if kind.is_none() {
                kind = Some(FailureKind::TicketExpired);
            }
            evidence.push(Evidence {
                code: "ticket_not_yet_valid".into(),
                detail: format!(
                    "采集观测时间 {} 早于 start_time {}（原始时间未被修改）",
                    rec.client_observed_time.unwrap_or(0),
                    rec.start_time
                ),
            });
            if let Some(delta) = skew_candidate {
                evidence.push(Evidence {
                    code: "clock_skew_candidate".into(),
                    detail: format!("候选解释：时钟偏移 {delta} 秒，在允许阈值 {} 秒内", ruleset.clock_skew_secs),
                });
            }
        }
        WindowStatus::Inside => evidence.push(Evidence {
            code: "window_ok".into(),
            detail: "观测时间落在票据有效期内".into(),
        }),
        WindowStatus::Unknown => evidence.push(Evidence {
            code: "window_unknown".into(),
            detail: "缺少客户端观测时间，无法评估时间窗口".into(),
        }),
    }

    // 4. 候选密钥。
    let candidates = rank_candidates(keys, ruleset, rec);
    if candidates.is_empty() {
        if kind.is_none() {
            kind = Some(FailureKind::NoMatchingKey);
        }
        evidence.push(Evidence {
            code: "no_matching_key".into(),
            detail: format!(
                "keytab {} 中找不到主体 {}@{} 的任何密钥代次",
                ruleset.keytab, rec.principal, rec.realm
            ),
        });
    } else {
        let exact_count = candidates.iter().filter(|c| c.exact).count();
        if exact_count == 0 {
            if kind.is_none() {
                kind = Some(FailureKind::KeyMismatch);
            }
            evidence.push(Evidence {
                code: "no_exact_key".into(),
                detail: format!(
                    "没有 etype={} 且 kvno={} 的密钥，首选候选 kvno={} etype={}",
                    rec.etype, rec.kvno, candidates[0].kvno, candidates[0].etype
                ),
            });
        } else if exact_count > 1 {
            if kind.is_none() {
                kind = Some(FailureKind::KeyMismatch);
            }
            evidence.push(Evidence {
                code: "ambiguous_key_generation".into(),
                detail: format!(
                    "kvno={} etype={} 下存在 {exact_count} 把不同指纹的密钥，无法唯一确定密钥代次",
                    rec.kvno, rec.etype
                ),
            });
        } else {
            evidence.push(Evidence {
                code: "unique_key".into(),
                detail: format!(
                    "唯一精确匹配密钥代次 kvno={} 指纹 {}…",
                    candidates[0].kvno,
                    candidates[0].fingerprint.chars().take(12).collect::<String>()
                ),
            });
        }
    }

    let resolved = kind.unwrap_or(FailureKind::NoMatchingKey);
    let summary = summarize(resolved, rec, ruleset);

    Diagnosis {
        failure_id: 0,
        record_id: rec.id,
        branch_id: 0,
        ruleset_id: ruleset.id,
        kind: resolved.as_str().into(),
        summary,
        evidence,
        candidates,
        clock_skew_candidate_secs: skew_candidate,
        replay_partition: partition,
    }
}

fn summarize(kind: FailureKind, rec: &Record, ruleset: &RulesetRow) -> String {
    match kind {
        FailureKind::ReferralLoop => format!(
            "{}@{} 的 referral 链成环，未取得服务票据",
            rec.principal, rec.realm
        ),
        FailureKind::TrustPathViolation => format!(
            "{}@{} 的 transited 路径与规则「{}」假设的信任路径不符",
            rec.principal, rec.realm, ruleset.name
        ),
        FailureKind::Replay => format!(
            "{}@{} 命中 replay cache（同服务实例同启动代次内 authenticator 重复）",
            rec.principal, rec.realm
        ),
        FailureKind::TicketExpired => format!("{}@{} 的票据不在有效时间窗口内", rec.principal, rec.realm),
        FailureKind::RenewableNotRenewed => {
            format!("{}@{} 的可续期票据过期且未续期", rec.principal, rec.realm)
        }
        FailureKind::NoMatchingKey => format!(
            "keytab {} 中没有 {}@{} 可用的密钥代次",
            ruleset.keytab, rec.principal, rec.realm
        ),
        FailureKind::KeyMismatch => format!(
            "{}@{} 的 kvno={} 无法定位到唯一匹配的密钥代次",
            rec.principal, rec.realm, rec.kvno
        ),
    }
}

/// 在一个假设分支上重跑全部失败记录，结论绑定该分支与当前规则版本。
pub fn run_branch(db: &mut Db, branch: &BranchRow) -> rusqlite::Result<usize> {
    let records = db.all_records()?;
    let keys = db.all_keys()?;
    let replay = replay_hits(&records);
    let ruleset = &branch.ruleset;

    let failed: Vec<Record> = records.iter().filter(|r| r.failed).cloned().collect();
    let tx = db.conn.transaction()?;

    // 先清掉本分支旧结论（其它分支与旧规则版本的结论保持不动）。
    tx.execute("DELETE FROM diagnoses WHERE branch_id = ?1", rusqlite::params![branch.id])?;
    let mut produced = 0usize;
    for rec in &failed {
        let mut d = diagnose(&records, &keys, &replay, ruleset, rec);
        d.branch_id = branch.id;
        d.ruleset_id = ruleset.id;
        let evidence_json = serde_json::to_string(&d.evidence).unwrap();
        let candidates_json = serde_json::to_string(&d.candidates).unwrap();
        tx.execute(
            "INSERT INTO diagnoses (record_id, branch_id, ruleset_id, kind, summary,
                    evidence_json, candidates_json, clock_skew_candidate_secs, replay_partition)
             VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9)",
            rusqlite::params![
                d.record_id,
                d.branch_id,
                d.ruleset_id,
                d.kind,
                d.summary,
                evidence_json,
                candidates_json,
                d.clock_skew_candidate_secs,
                d.replay_partition,
            ],
        )?;
        produced += 1;
    }
    tx.commit()?;
    Ok(produced)
}
