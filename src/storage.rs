//! SQLite schema 与数据访问。所有表都带显式约束，窗口端点以闭区间整数存储。

use crate::db::Db;
use crate::error::{AppError, AppResult};

pub fn migrate(db: &mut Db) -> AppResult<()> {
    db.transaction(|tx| {
        tx.exec(SCHEMA)?;
        Ok(())
    })
}

const SCHEMA: &str = r#"
PRAGMA foreign_keys=ON;

-- 原始测量摘要：只存摘要与证据指针，波形本身留在本地 fixture，不入库不外传。
CREATE TABLE IF NOT EXISTS measurements (
    id                 INTEGER PRIMARY KEY,
    code               TEXT NOT NULL UNIQUE,
    title              TEXT NOT NULL,
    excitation_file    TEXT NOT NULL,
    recording_file     TEXT NOT NULL,
    sample_rate        INTEGER NOT NULL,
    channel_count      INTEGER NOT NULL,
    frame_count        INTEGER NOT NULL,
    excitation_digest  TEXT NOT NULL,
    recording_digest   TEXT NOT NULL,
    clock_ppm          REAL NOT NULL,
    clock_status       TEXT NOT NULL,            -- Ok | DriftSuspect
    drop_flags_json    TEXT NOT NULL,            -- 丢帧样本索引数组
    frame_audit_json   TEXT NOT NULL,            -- 帧完整性审计（零值/不完整区分）
    validation_json    TEXT NOT NULL,            -- 采样率与通道时钟核验明细
    created_at         TEXT NOT NULL DEFAULT (datetime('now'))
);

-- 逆滤波候选：两个正则强度各一行，允许同时保留。
CREATE TABLE IF NOT EXISTS candidates (
    id                 INTEGER PRIMARY KEY,
    measurement_id     INTEGER NOT NULL REFERENCES measurements(id),
    label              TEXT NOT NULL,
    regularization     REAL NOT NULL,
    peak_index         INTEGER NOT NULL,
    offset_correction  INTEGER NOT NULL,
    reflections_json   TEXT NOT NULL,
    decay_start        INTEGER NOT NULL,        -- 闭区间起点
    decay_end          INTEGER NOT NULL,        -- 闭区间终点
    decay_curve_json   TEXT NOT NULL,           -- Schroeder dB 曲线
    impulse_len        INTEGER NOT NULL,        -- 脉冲响应网格长度，供窗口端点校验
    impulse_digest     TEXT NOT NULL,
    created_at         TEXT NOT NULL DEFAULT (datetime('now')),
    UNIQUE(measurement_id, label)
);

-- 窗口提案 revision：只追加，不更新不覆盖。
CREATE TABLE IF NOT EXISTS window_revisions (
    id                   INTEGER PRIMARY KEY,
    measurement_id       INTEGER NOT NULL REFERENCES measurements(id),
    candidate_id         INTEGER NOT NULL REFERENCES candidates(id),
    revision             INTEGER NOT NULL,
    start_sample         INTEGER NOT NULL,      -- 闭区间 [start, end]
    end_sample           INTEGER NOT NULL,
    sample_count         INTEGER NOT NULL,      -- 冗余但显式：end-start+1
    based_on_revision    INTEGER NOT NULL,      -- 基线 revision，首个为 -1
    rationale            TEXT NOT NULL,
    author               TEXT NOT NULL,
    created_at           TEXT NOT NULL DEFAULT (datetime('now')),
    CHECK (end_sample >= start_sample),
    CHECK (sample_count = end_sample - start_sample + 1),
    UNIQUE(measurement_id, candidate_id, revision)
);

-- 校准链：每个校准文件按 digest 固定，active 标记当前有效版本。
CREATE TABLE IF NOT EXISTS calibrations (
    id            INTEGER PRIMARY KEY,
    cal_code      TEXT NOT NULL,
    version       INTEGER NOT NULL,
    file_name     TEXT NOT NULL,
    digest        TEXT NOT NULL UNIQUE,
    active        INTEGER NOT NULL,
    superseded_by INTEGER,
    created_at    TEXT NOT NULL DEFAULT (datetime('now')),
    UNIQUE(cal_code, version)
);

-- 校准替换事件。
CREATE TABLE IF NOT EXISTS calibration_changes (
    id                   INTEGER PRIMARY KEY,
    cal_code             TEXT NOT NULL,
    old_calibration_id   INTEGER,
    new_calibration_id   INTEGER NOT NULL,
    old_digest           TEXT,
    new_digest           TEXT NOT NULL,
    impact_generated     INTEGER NOT NULL DEFAULT 0,
    created_at           TEXT NOT NULL DEFAULT (datetime('now'))
);

-- 已发布脉冲响应：冻结当时有效的校准 digest。
CREATE TABLE IF NOT EXISTS published_responses (
    id                  INTEGER PRIMARY KEY,
    measurement_id      INTEGER NOT NULL REFERENCES measurements(id),
    candidate_id        INTEGER NOT NULL REFERENCES candidates(id),
    window_revision_id  INTEGER NOT NULL REFERENCES window_revisions(id),
    calibration_id      INTEGER NOT NULL REFERENCES calibrations(id),
    calibration_digest  TEXT NOT NULL,          -- 发布时刻快照
    response_digest     TEXT NOT NULL,
    superseded          INTEGER NOT NULL DEFAULT 0,
    created_at          TEXT NOT NULL DEFAULT (datetime('now'))
);

-- 影响清单：引用旧校准的已发布响应，只登记不重算。
CREATE TABLE IF NOT EXISTS impact_items (
    id                     INTEGER PRIMARY KEY,
    change_id              INTEGER NOT NULL REFERENCES calibration_changes(id),
    measurement_id         INTEGER NOT NULL REFERENCES measurements(id),
    published_response_id  INTEGER NOT NULL REFERENCES published_responses(id),
    status                 TEXT NOT NULL,       -- Stale | NeedsReview
    note                   TEXT NOT NULL,
    UNIQUE(change_id, published_response_id)
);

-- 并行审议：version 单调，写入带期望版本号，冲突即拒绝。
CREATE TABLE IF NOT EXISTS reviews (
    id                 INTEGER PRIMARY KEY,
    measurement_id     INTEGER NOT NULL REFERENCES measurements(id),
    version            INTEGER NOT NULL,
    reviewer           TEXT NOT NULL,
    decision           TEXT NOT NULL,           -- Accepted | Rejected | ChangeRequested
    note               TEXT NOT NULL,
    candidate_id       INTEGER REFERENCES candidates(id),
    window_revision_id INTEGER REFERENCES window_revisions(id),
    created_at         TEXT NOT NULL DEFAULT (datetime('now')),
    UNIQUE(measurement_id, version)
);

-- 衍生物台账：服务恢复后据此跳过已提交产物，保证幂等不重复生成。
CREATE TABLE IF NOT EXISTS derived_artifacts (
    id              INTEGER PRIMARY KEY,
    measurement_id  INTEGER NOT NULL REFERENCES measurements(id),
    kind            TEXT NOT NULL,
    input_hash      TEXT NOT NULL,
    artifact_ref    TEXT NOT NULL,
    committed       INTEGER NOT NULL DEFAULT 1,
    created_at      TEXT NOT NULL DEFAULT (datetime('now')),
    UNIQUE(measurement_id, kind, input_hash)
);

CREATE INDEX IF NOT EXISTS idx_candidates_m ON candidates(measurement_id);
CREATE INDEX IF NOT EXISTS idx_windows_mc ON window_revisions(measurement_id, candidate_id);
CREATE INDEX IF NOT EXISTS idx_reviews_m ON reviews(measurement_id);
CREATE INDEX IF NOT EXISTS idx_published_m ON published_responses(measurement_id);
CREATE INDEX IF NOT EXISTS idx_impact_change ON impact_items(change_id);
"#;

/// 取某测量下某候选当前最大窗口 revision；无记录返回 -1。
pub fn max_window_revision(db: &Db, measurement_id: i64, candidate_id: i64) -> AppResult<i64> {
    let mut st = db.prepare(
        "SELECT COALESCE(MAX(revision), -1) FROM window_revisions \
         WHERE measurement_id=?1 AND candidate_id=?2",
    )?;
    st.bind_i64(1, measurement_id)?.bind_i64(2, candidate_id)?;
    if st.next()? {
        Ok(st.column_i64(0))
    } else {
        Ok(-1)
    }
}

/// 取某测量当前审议版本号；无记录返回 0。
pub fn current_review_version(db: &Db, measurement_id: i64) -> AppResult<i64> {
    let mut st = db.prepare("SELECT COALESCE(MAX(version), 0) FROM reviews WHERE measurement_id=?1")?;
    st.bind_i64(1, measurement_id)?;
    if st.next()? {
        Ok(st.column_i64(0))
    } else {
        Ok(0)
    }
}

pub fn measurement_exists(db: &Db, code: &str) -> AppResult<bool> {
    let mut st = db.prepare("SELECT 1 FROM measurements WHERE code=?1")?;
    st.bind_text(1, code)?;
    Ok(st.next()?)
}

/// 乐观锁插入窗口 revision；期望版本不符返回 Conflict。
/// 自带事务，供单步 HTTP 写入使用。
pub fn insert_window_revision(
    db: &mut Db,
    measurement_id: i64,
    candidate_id: i64,
    expected_revision: i64,
    start: i64,
    end: i64,
    rationale: &str,
    author: &str,
) -> AppResult<i64> {
    db.transaction(|tx| {
        insert_window_revision_tx(
            tx,
            measurement_id,
            candidate_id,
            expected_revision,
            start,
            end,
            rationale,
            author,
        )
    })
}

/// 事务内版本：不自行开关事务，供批量引导在同一事务内调用。
#[allow(clippy::too_many_arguments)]
pub fn insert_window_revision_tx(
    tx: &mut Db,
    measurement_id: i64,
    candidate_id: i64,
    expected_revision: i64,
    start: i64,
    end: i64,
    rationale: &str,
    author: &str,
) -> AppResult<i64> {
    let current = max_window_revision(tx, measurement_id, candidate_id)?;
    if current != expected_revision {
        return Err(AppError::Conflict(format!(
            "窗口 revision 冲突：期望基线 {expected_revision}，当前最新为 {current}，已拒绝覆盖"
        )));
    }
    if end < start {
        return Err(AppError::Validation(format!(
            "闭区间端点非法：start={start} 大于 end={end}"
        )));
    }
    let new_rev = current + 1;
    let count = end - start + 1;
    let mut st = tx.prepare(
        "INSERT INTO window_revisions \
         (measurement_id, candidate_id, revision, start_sample, end_sample, sample_count, \
          based_on_revision, rationale, author) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9)",
    )?;
    st.bind_i64(1, measurement_id)?
        .bind_i64(2, candidate_id)?
        .bind_i64(3, new_rev)?
        .bind_i64(4, start)?
        .bind_i64(5, end)?
        .bind_i64(6, count)?
        .bind_i64(7, current)?
        .bind_text(8, rationale)?
        .bind_text(9, author)?;
    st.next()?;
    Ok(tx.last_insert_rowid())
}

/// 乐观锁插入审议；期望版本不符返回 Conflict。自带事务。
pub fn insert_review(
    db: &mut Db,
    measurement_id: i64,
    expected_version: i64,
    reviewer: &str,
    decision: &str,
    note: &str,
    candidate_id: Option<i64>,
    window_revision_id: Option<i64>,
) -> AppResult<i64> {
    db.transaction(|tx| {
        insert_review_tx(
            tx,
            measurement_id,
            expected_version,
            reviewer,
            decision,
            note,
            candidate_id,
            window_revision_id,
        )
    })
}

/// 事务内版本的审议插入。
pub fn insert_review_tx(
    tx: &mut Db,
    measurement_id: i64,
    expected_version: i64,
    reviewer: &str,
    decision: &str,
    note: &str,
    candidate_id: Option<i64>,
    window_revision_id: Option<i64>,
) -> AppResult<i64> {
    let current = current_review_version(tx, measurement_id)?;
    if current != expected_version {
        return Err(AppError::Conflict(format!(
            "审议版本冲突：期望基于版本 {expected_version}，当前已到 {current}，并行提交被拒绝"
        )));
    }
    let new_version = current + 1;
    let mut st = tx.prepare(
        "INSERT INTO reviews \
         (measurement_id, version, reviewer, decision, note, candidate_id, window_revision_id) \
         VALUES (?1,?2,?3,?4,?5,?6,?7)",
    )?;
    st.bind_i64(1, measurement_id)?
        .bind_i64(2, new_version)?
        .bind_text(3, reviewer)?
        .bind_text(4, decision)?
        .bind_text(5, note)?;
    match candidate_id {
        Some(v) => st.bind_i64(6, v)?,
        None => st.bind_null(6)?,
    };
    match window_revision_id {
        Some(v) => st.bind_i64(7, v)?,
        None => st.bind_null(7)?,
    };
    st.next()?;
    Ok(tx.last_insert_rowid())
}
