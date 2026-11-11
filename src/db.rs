//! SQLite 存储层。
//!
//! 事实表（records/keys/boots）只增不改；规则版本（rulesets）按内容哈希去重且不可变；
//! 分支（branches）引用某版规则；结论（diagnoses）同时绑定分支与规则版本，
//! 因此新开分支重推不会覆盖旧结论，旧结论永远指回当时那版规则。

use rusqlite::{params, Connection, OptionalExtension};
use std::collections::HashSet;

use crate::model::*;

pub struct Db {
    pub conn: Connection,
}

/// 导入统计。
#[derive(Debug, Clone, Default, serde::Serialize)]
pub struct ImportStats {
    pub records: usize,
    pub keys: usize,
    pub boots: usize,
}

const MIGRATION: &str = r#"
CREATE TABLE IF NOT EXISTS meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS records (
    id                   INTEGER PRIMARY KEY,
    source               TEXT NOT NULL,
    layer                TEXT NOT NULL,
    realm                TEXT NOT NULL,
    principal            TEXT NOT NULL,
    etype                TEXT NOT NULL,
    kvno                 INTEGER NOT NULL,
    flags_json           TEXT NOT NULL,
    auth_time            INTEGER NOT NULL,
    start_time           INTEGER NOT NULL,
    end_time             INTEGER NOT NULL,
    renew_till           INTEGER,
    nonce                INTEGER NOT NULL,
    transited_json       TEXT NOT NULL,
    authenticator_fp     TEXT,
    raw_ref              TEXT NOT NULL,
    client_observed_time INTEGER,
    service_instance     TEXT,
    boot_generation      INTEGER,
    parent_id            INTEGER REFERENCES records(id),
    failed               INTEGER NOT NULL,
    error_code           INTEGER
);
CREATE INDEX IF NOT EXISTS idx_records_parent ON records(parent_id);
CREATE INDEX IF NOT EXISTS idx_records_principal ON records(realm, principal);
CREATE INDEX IF NOT EXISTS idx_records_replay
    ON records(service_instance, boot_generation, authenticator_fp);

CREATE TABLE IF NOT EXISTS keys (
    id          INTEGER PRIMARY KEY,
    realm       TEXT NOT NULL,
    principal   TEXT NOT NULL,
    etype       TEXT NOT NULL,
    kvno        INTEGER NOT NULL,
    fingerprint TEXT NOT NULL,
    keytab      TEXT NOT NULL,
    valid_from  INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_keys_lookup ON keys(keytab, realm, principal);

CREATE TABLE IF NOT EXISTS boots (
    id               INTEGER PRIMARY KEY,
    service_instance TEXT NOT NULL,
    generation       INTEGER NOT NULL,
    started_at       INTEGER NOT NULL,
    UNIQUE(service_instance, generation)
);

-- 不可变规则版本，内容哈希去重。
CREATE TABLE IF NOT EXISTS rulesets (
    id              INTEGER PRIMARY KEY,
    content_hash    TEXT NOT NULL UNIQUE,
    name            TEXT NOT NULL,
    trust_path_json TEXT NOT NULL,
    keytab          TEXT NOT NULL,
    clock_skew_secs INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS branches (
    id               INTEGER PRIMARY KEY,
    name             TEXT NOT NULL,
    parent_branch_id INTEGER REFERENCES branches(id),
    ruleset_id       INTEGER NOT NULL REFERENCES rulesets(id),
    created_at       INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS diagnoses (
    id                          INTEGER PRIMARY KEY,
    record_id                   INTEGER NOT NULL REFERENCES records(id),
    branch_id                   INTEGER NOT NULL REFERENCES branches(id),
    ruleset_id                  INTEGER NOT NULL REFERENCES rulesets(id),
    kind                        TEXT NOT NULL,
    summary                     TEXT NOT NULL,
    evidence_json               TEXT NOT NULL,
    candidates_json             TEXT NOT NULL,
    clock_skew_candidate_secs   INTEGER,
    replay_partition            TEXT,
    UNIQUE(record_id, branch_id)
);
CREATE INDEX IF NOT EXISTS idx_diag_branch ON diagnoses(branch_id);
"#;

fn derive_layer(source: &str, principal: &str) -> &'static str {
    if source == "as" {
        TicketLayer::Tgt.as_str()
    } else if principal.starts_with("krbtgt/") {
        TicketLayer::ReferralTgt.as_str()
    } else {
        TicketLayer::Service.as_str()
    }
}

impl Db {
    /// 打开（或创建）数据库并执行迁移。
    pub fn open(path: &str) -> rusqlite::Result<Db> {
        let conn = Connection::open(path)?;
        let mut db = Db { conn };
        db.init()?;
        Ok(db)
    }

    /// 内存数据库，测试用。
    pub fn open_memory() -> rusqlite::Result<Db> {
        let conn = Connection::open_in_memory()?;
        let mut db = Db { conn };
        db.init()?;
        Ok(db)
    }

    fn init(&mut self) -> rusqlite::Result<()> {
        self.conn.pragma_update(None, "foreign_keys", "ON")?;
        self.conn.execute_batch(MIGRATION)?;
        Ok(())
    }

    /// 在单个事务内导入；任何一行不合法则整体回滚，事实表保持原状。
    pub fn import_file(&mut self, data: &ImportFile) -> rusqlite::Result<ImportStats> {
        let tx = self.conn.transaction()?;
        let mut stats = ImportStats::default();

        // 预检：批次内 parent_id 必须指向批次内记录或已存在记录。
        let batch_ids: HashSet<i64> = data.records.iter().map(|r| r.nonce).collect();
        // nonce 在导入模型里承担外部 id 职责：parent_id 实际引用批次内的 nonce。
        for r in &data.records {
            if let Some(parent_ref) = r.parent_id {
                let exists_in_batch = batch_ids.contains(&parent_ref);
                let exists_in_db = tx
                    .query_row(
                        "SELECT 1 FROM records WHERE nonce = ?1",
                        params![parent_ref],
                        |_| Ok(()),
                    )
                    .optional()?
                    .is_some();
                if !exists_in_batch && !exists_in_db {
                    return Err(rusqlite::Error::ToSqlConversionFailure(Box::new(
                        std::io::Error::new(
                            std::io::ErrorKind::InvalidInput,
                            format!(
                                "record nonce {} 引用了不存在的 parent_id {}",
                                r.nonce, parent_ref
                            ),
                        ),
                    )));
                }
            }
        }

        // nonce -> 新 rowid 映射，用于重建谱系边。
        let mut nonce_to_rowid: std::collections::HashMap<i64, i64> = std::collections::HashMap::new();
        for r in &data.records {
            let layer = derive_layer(&r.source, &r.principal);
            let flags_json = serde_json::to_string(&r.flags).unwrap();
            let transited_json = serde_json::to_string(&r.transited).unwrap();
            tx.execute(
                "INSERT INTO records (source, layer, realm, principal, etype, kvno,
                    flags_json, auth_time, start_time, end_time, renew_till, nonce,
                    transited_json, authenticator_fp, raw_ref, client_observed_time,
                    service_instance, boot_generation, parent_id, failed, error_code)
                 VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,?21)",
                params![
                    r.source,
                    layer,
                    r.realm,
                    r.principal,
                    r.etype,
                    r.kvno,
                    flags_json,
                    r.auth_time,
                    r.start_time,
                    r.end_time,
                    r.renew_till,
                    r.nonce,
                    transited_json,
                    r.authenticator_fp,
                    r.raw_ref,
                    r.client_observed_time,
                    r.service_instance,
                    r.boot_generation,
                    Option::<i64>::None, // 先置空，拿到 rowid 后回填
                    r.failed as i64,
                    r.error_code,
                ],
            )?;
            nonce_to_rowid.insert(r.nonce, tx.last_insert_rowid());
        }
        for r in &data.records {
            if let Some(parent_ref) = r.parent_id {
                let parent_rowid = match nonce_to_rowid.get(&parent_ref) {
                    Some(id) => *id,
                    None => tx.query_row(
                        "SELECT id FROM records WHERE nonce = ?1",
                        params![parent_ref],
                        |row| row.get(0),
                    )?,
                };
                let child_rowid = nonce_to_rowid[&r.nonce];
                tx.execute(
                    "UPDATE records SET parent_id = ?1 WHERE id = ?2",
                    params![parent_rowid, child_rowid],
                )?;
            }
        }
        stats.records = data.records.len();

        for k in &data.keys {
            tx.execute(
                "INSERT INTO keys (realm, principal, etype, kvno, fingerprint, keytab, valid_from)
                 VALUES (?1,?2,?3,?4,?5,?6,?7)",
                params![
                    k.realm,
                    k.principal,
                    k.etype,
                    k.kvno,
                    k.fingerprint,
                    k.keytab,
                    k.valid_from
                ],
            )?;
        }
        stats.keys = data.keys.len();

        for b in &data.boots {
            tx.execute(
                "INSERT OR IGNORE INTO boots (service_instance, generation, started_at)
                 VALUES (?1,?2,?3)",
                params![b.service_instance, b.generation, b.started_at],
            )?;
        }
        stats.boots = data.boots.len();

        tx.commit()?;
        Ok(stats)
    }

    /// 导出全部事实数据，可在另一个空库重新导入。
    pub fn export_all(&self) -> rusqlite::Result<ImportFile> {
        let mut out = ImportFile::default();
        let mut stmt = self.conn.prepare(
            "SELECT source, realm, principal, etype, kvno, flags_json, auth_time,
                    start_time, end_time, renew_till, nonce, transited_json,
                    authenticator_fp, raw_ref, client_observed_time, service_instance,
                    boot_generation, parent_id, failed, error_code, id
             FROM records ORDER BY id",
        )?;
        // 先把所有列取成 owned 数据，避免在收集结果时持有 Row 引用。
        struct ExportRow {
            source: String,
            realm: String,
            principal: String,
            etype: String,
            kvno: i64,
            flags: Vec<String>,
            auth_time: i64,
            start_time: i64,
            end_time: i64,
            renew_till: Option<i64>,
            nonce: i64,
            transited: Vec<String>,
            authenticator_fp: Option<String>,
            raw_ref: String,
            client_observed_time: Option<i64>,
            service_instance: Option<String>,
            boot_generation: Option<i64>,
            parent_rowid: Option<i64>,
            failed: bool,
            error_code: Option<i64>,
            id: i64,
        }
        let rows = stmt.query_map([], |row| {
            let flags_json: String = row.get(5)?;
            let transited_json: String = row.get(11)?;
            Ok(ExportRow {
                source: row.get(0)?,
                realm: row.get(1)?,
                principal: row.get(2)?,
                etype: row.get(3)?,
                kvno: row.get(4)?,
                flags: serde_json::from_str(&flags_json).unwrap(),
                auth_time: row.get(6)?,
                start_time: row.get(7)?,
                end_time: row.get(8)?,
                renew_till: row.get(9)?,
                nonce: row.get(10)?,
                transited: serde_json::from_str(&transited_json).unwrap(),
                authenticator_fp: row.get(12)?,
                raw_ref: row.get(13)?,
                client_observed_time: row.get(14)?,
                service_instance: row.get(15)?,
                boot_generation: row.get(16)?,
                parent_rowid: row.get(17)?,
                failed: row.get::<_, i64>(18)? != 0,
                error_code: row.get(19)?,
                id: row.get(20)?,
            })
        })?;
        let raw_rows: Vec<ExportRow> = rows.collect::<rusqlite::Result<_>>()?;

        // rowid -> nonce，导出时把谱系边还原成外部引用（nonce）。
        let rowid_to_nonce: std::collections::HashMap<i64, i64> =
            raw_rows.iter().map(|r| (r.id, r.nonce)).collect();
        for r in raw_rows {
            out.records.push(RecordInput {
                source: r.source,
                realm: r.realm,
                principal: r.principal,
                etype: r.etype,
                kvno: r.kvno,
                flags: r.flags,
                auth_time: r.auth_time,
                start_time: r.start_time,
                end_time: r.end_time,
                renew_till: r.renew_till,
                nonce: r.nonce,
                transited: r.transited,
                authenticator_fp: r.authenticator_fp,
                raw_ref: r.raw_ref,
                client_observed_time: r.client_observed_time,
                service_instance: r.service_instance,
                boot_generation: r.boot_generation,
                parent_id: r.parent_rowid.map(|p| rowid_to_nonce[&p]),
                failed: r.failed,
                error_code: r.error_code,
            });
        }

        let mut stmt = self.conn.prepare(
            "SELECT realm, principal, etype, kvno, fingerprint, keytab, valid_from
             FROM keys ORDER BY id",
        )?;
        let rows = stmt.query_map([], |row| {
            Ok(KeyInput {
                realm: row.get(0)?,
                principal: row.get(1)?,
                etype: row.get(2)?,
                kvno: row.get(3)?,
                fingerprint: row.get(4)?,
                keytab: row.get(5)?,
                valid_from: row.get(6)?,
            })
        })?;
        out.keys = rows.collect::<rusqlite::Result<_>>()?;

        let mut stmt = self
            .conn
            .prepare("SELECT service_instance, generation, started_at FROM boots ORDER BY id")?;
        let rows = stmt.query_map([], |row| {
            Ok(BootEvent {
                service_instance: row.get(0)?,
                generation: row.get(1)?,
                started_at: row.get(2)?,
            })
        })?;
        out.boots = rows.collect::<rusqlite::Result<_>>()?;

        Ok(out)
    }

    /// 重置数据集：清掉事实、结论与分支（规则版本内容寻址、保留可复用）。
    pub fn wipe_facts(&mut self) -> rusqlite::Result<()> {
        let tx = self.conn.transaction()?;
        tx.execute("DELETE FROM diagnoses", [])?;
        tx.execute("DELETE FROM branches", [])?;
        tx.execute("DELETE FROM records", [])?;
        tx.execute("DELETE FROM keys", [])?;
        tx.execute("DELETE FROM boots", [])?;
        tx.commit()?;
        Ok(())
    }

    pub fn count_records(&self) -> rusqlite::Result<i64> {
        self.conn.query_row("SELECT COUNT(*) FROM records", [], |row| row.get(0))
    }

    // ---- 规则版本与分支 ----

    /// 写入（或复用同哈希的）不可变规则版本，返回 ruleset id。
    pub fn upsert_ruleset(&mut self, input: &RulesetInput) -> rusqlite::Result<i64> {
        use sha2::{Digest, Sha256};
        let trust_path_json = serde_json::to_string(&input.trust_path).unwrap();
        let mut hasher = Sha256::new();
        hasher.update(b"ruleset/v1\n");
        hasher.update(trust_path_json.as_bytes());
        hasher.update(b"\n");
        hasher.update(input.keytab.as_bytes());
        hasher.update(b"\n");
        hasher.update(input.clock_skew_secs.to_string().as_bytes());
        let hash = hex::encode(hasher.finalize());

        if let Some(id) = self
            .conn
            .query_row(
                "SELECT id FROM rulesets WHERE content_hash = ?1",
                params![hash],
                |row| row.get(0),
            )
            .optional()?
        {
            return Ok(id);
        }
        self.conn.execute(
            "INSERT INTO rulesets (content_hash, name, trust_path_json, keytab, clock_skew_secs)
             VALUES (?1,?2,?3,?4,?5)",
            params![hash, input.name, trust_path_json, input.keytab, input.clock_skew_secs],
        )?;
        Ok(self.conn.last_insert_rowid())
    }

    pub fn create_branch(
        &mut self,
        name: &str,
        parent_branch_id: Option<i64>,
        ruleset_id: i64,
        now: i64,
    ) -> rusqlite::Result<i64> {
        self.conn.execute(
            "INSERT INTO branches (name, parent_branch_id, ruleset_id, created_at)
             VALUES (?1,?2,?3,?4)",
            params![name, parent_branch_id, ruleset_id, now],
        )?;
        Ok(self.conn.last_insert_rowid())
    }

    pub fn list_rulesets(&self) -> rusqlite::Result<Vec<RulesetRow>> {
        let mut stmt = self.conn.prepare(
            "SELECT id, name, trust_path_json, keytab, clock_skew_secs, content_hash
             FROM rulesets ORDER BY id",
        )?;
        let rows = stmt.query_map([], row_to_ruleset)?;
        rows.collect::<rusqlite::Result<_>>()
    }

    pub fn list_branches(&self) -> rusqlite::Result<Vec<BranchRow>> {
        let mut stmt = self.conn.prepare(
            "SELECT b.id, b.name, b.parent_branch_id, b.ruleset_id, b.created_at,
                    r.id, r.name, r.trust_path_json, r.keytab, r.clock_skew_secs, r.content_hash
             FROM branches b JOIN rulesets r ON r.id = b.ruleset_id
             ORDER BY b.id",
        )?;
        let rows = stmt.query_map([], |row| {
            let trust_path_json: String = row.get(7)?;
            Ok(BranchRow {
                id: row.get(0)?,
                name: row.get(1)?,
                parent_branch_id: row.get(2)?,
                ruleset_id: row.get(3)?,
                created_at: row.get(4)?,
                ruleset: RulesetRow {
                    id: row.get(5)?,
                    name: row.get(6)?,
                    trust_path: serde_json::from_str(&trust_path_json).unwrap(),
                    keytab: row.get(8)?,
                    clock_skew_secs: row.get(9)?,
                    content_hash: row.get(10)?,
                },
            })
        })?;
        rows.collect::<rusqlite::Result<_>>()
    }

    // ---- 事实读取 ----

    pub fn all_records(&self) -> rusqlite::Result<Vec<Record>> {
        let mut stmt = self.conn.prepare(
            "SELECT id, source, layer, realm, principal, etype, kvno, flags_json,
                    auth_time, start_time, end_time, renew_till, nonce, transited_json,
                    authenticator_fp, raw_ref, client_observed_time, service_instance,
                    boot_generation, parent_id, failed, error_code
             FROM records ORDER BY id",
        )?;
        let rows = stmt.query_map([], row_to_record)?;
        rows.collect::<rusqlite::Result<_>>()
    }

    pub fn record_by_id(&self, id: i64) -> rusqlite::Result<Option<Record>> {
        let mut stmt = self.conn.prepare(
            "SELECT id, source, layer, realm, principal, etype, kvno, flags_json,
                    auth_time, start_time, end_time, renew_till, nonce, transited_json,
                    authenticator_fp, raw_ref, client_observed_time, service_instance,
                    boot_generation, parent_id, failed, error_code
             FROM records WHERE id = ?1",
        )?;
        stmt.query_row(params![id], row_to_record).optional()
    }

    pub fn all_keys(&self) -> rusqlite::Result<Vec<KeyRow>> {
        let mut stmt = self.conn.prepare(
            "SELECT id, realm, principal, etype, kvno, fingerprint, keytab, valid_from
             FROM keys ORDER BY id",
        )?;
        let rows = stmt.query_map([], |row| {
            Ok(KeyRow {
                id: row.get(0)?,
                realm: row.get(1)?,
                principal: row.get(2)?,
                etype: row.get(3)?,
                kvno: row.get(4)?,
                fingerprint: row.get(5)?,
                keytab: row.get(6)?,
                valid_from: row.get(7)?,
            })
        })?;
        rows.collect::<rusqlite::Result<_>>()
    }

    pub fn all_boots(&self) -> rusqlite::Result<Vec<BootEvent>> {
        let mut stmt = self
            .conn
            .prepare("SELECT service_instance, generation, started_at FROM boots ORDER BY id")?;
        let rows = stmt.query_map([], |row| {
            Ok(BootEvent {
                service_instance: row.get(0)?,
                generation: row.get(1)?,
                started_at: row.get(2)?,
            })
        })?;
        rows.collect::<rusqlite::Result<_>>()
    }

    pub fn diagnoses_for_branch(&self, branch_id: i64) -> rusqlite::Result<Vec<Diagnosis>> {
        let mut stmt = self.conn.prepare(
            "SELECT id, record_id, branch_id, ruleset_id, kind, summary, evidence_json,
                    candidates_json, clock_skew_candidate_secs, replay_partition
             FROM diagnoses WHERE branch_id = ?1 ORDER BY record_id",
        )?;
        let rows = stmt.query_map(params![branch_id], row_to_diagnosis)?;
        rows.collect::<rusqlite::Result<_>>()
    }

    pub fn diagnoses_for_record(&self, record_id: i64) -> rusqlite::Result<Vec<Diagnosis>> {
        let mut stmt = self.conn.prepare(
            "SELECT id, record_id, branch_id, ruleset_id, kind, summary, evidence_json,
                    candidates_json, clock_skew_candidate_secs, replay_partition
             FROM diagnoses WHERE record_id = ?1 ORDER BY branch_id",
        )?;
        let rows = stmt.query_map(params![record_id], row_to_diagnosis)?;
        rows.collect::<rusqlite::Result<_>>()
    }

    pub fn all_diagnoses(&self) -> rusqlite::Result<Vec<Diagnosis>> {
        let mut stmt = self.conn.prepare(
            "SELECT id, record_id, branch_id, ruleset_id, kind, summary, evidence_json,
                    candidates_json, clock_skew_candidate_secs, replay_partition
             FROM diagnoses ORDER BY branch_id, record_id",
        )?;
        let rows = stmt.query_map([], row_to_diagnosis)?;
        rows.collect::<rusqlite::Result<_>>()
    }

    /// 持久化一条诊断结论（同分支同记录覆盖本分支结论，不影响其他分支）。
    pub fn replace_diagnosis(&mut self, d: &Diagnosis) -> rusqlite::Result<()> {
        let evidence_json = serde_json::to_string(&d.evidence).unwrap();
        let candidates_json = serde_json::to_string(&d.candidates).unwrap();
        self.conn.execute(
            "INSERT INTO diagnoses (record_id, branch_id, ruleset_id, kind, summary,
                    evidence_json, candidates_json, clock_skew_candidate_secs, replay_partition)
             VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9)
             ON CONFLICT(record_id, branch_id) DO UPDATE SET
                ruleset_id=excluded.ruleset_id, kind=excluded.kind, summary=excluded.summary,
                evidence_json=excluded.evidence_json, candidates_json=excluded.candidates_json,
                clock_skew_candidate_secs=excluded.clock_skew_candidate_secs,
                replay_partition=excluded.replay_partition",
            params![
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
        Ok(())
    }
}

/// 规则版本行。
#[derive(Debug, Clone, serde::Serialize)]
pub struct RulesetRow {
    pub id: i64,
    pub name: String,
    pub trust_path: Vec<String>,
    pub keytab: String,
    pub clock_skew_secs: i64,
    pub content_hash: String,
}

/// 分支行（内嵌当时的规则版本快照）。
#[derive(Debug, Clone, serde::Serialize)]
pub struct BranchRow {
    pub id: i64,
    pub name: String,
    pub parent_branch_id: Option<i64>,
    pub ruleset_id: i64,
    pub created_at: i64,
    pub ruleset: RulesetRow,
}

// rusqlite 的 Row 读取统一走这些映射函数。

fn row_to_ruleset(row: &rusqlite::Row) -> rusqlite::Result<RulesetRow> {
    let trust_path_json: String = row.get(2)?;
    Ok(RulesetRow {
        id: row.get(0)?,
        name: row.get(1)?,
        trust_path: serde_json::from_str(&trust_path_json).unwrap(),
        keytab: row.get(3)?,
        clock_skew_secs: row.get(4)?,
        content_hash: row.get(5)?,
    })
}

fn row_to_record(row: &rusqlite::Row) -> rusqlite::Result<Record> {
    let flags_json: String = row.get(7)?;
    let transited_json: String = row.get(13)?;
    Ok(Record {
        id: row.get(0)?,
        source: row.get(1)?,
        layer: row.get(2)?,
        realm: row.get(3)?,
        principal: row.get(4)?,
        etype: row.get(5)?,
        kvno: row.get(6)?,
        flags: serde_json::from_str(&flags_json).unwrap(),
        auth_time: row.get(8)?,
        start_time: row.get(9)?,
        end_time: row.get(10)?,
        renew_till: row.get(11)?,
        nonce: row.get(12)?,
        transited: serde_json::from_str(&transited_json).unwrap(),
        authenticator_fp: row.get(14)?,
        raw_ref: row.get(15)?,
        client_observed_time: row.get(16)?,
        service_instance: row.get(17)?,
        boot_generation: row.get(18)?,
        parent_id: row.get(19)?,
        failed: row.get::<_, i64>(20)? != 0,
        error_code: row.get(21)?,
    })
}

fn row_to_diagnosis(row: &rusqlite::Row) -> rusqlite::Result<Diagnosis> {
    let evidence_json: String = row.get(6)?;
    let candidates_json: String = row.get(7)?;
    Ok(Diagnosis {
        failure_id: row.get(0)?,
        record_id: row.get(1)?,
        branch_id: row.get(2)?,
        ruleset_id: row.get(3)?,
        kind: row.get(4)?,
        summary: row.get(5)?,
        evidence: serde_json::from_str(&evidence_json).unwrap(),
        candidates: serde_json::from_str(&candidates_json).unwrap(),
        clock_skew_candidate_secs: row.get(8)?,
        replay_partition: row.get(9)?,
    })
}
