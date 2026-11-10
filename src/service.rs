//! 业务编排：测量校验 → 逆滤波候选 → 窗口提案 → 审议 → 发布，
//! 以及校准换版与影响清单。所有写操作经乐观锁/事务保护，可重入且幂等。

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};

use crate::db::Db;
use crate::dsp::{analyze_candidate, drift_ppm, drop_flags, FrameKind, Window};
use crate::error::{AppError, AppResult};
use crate::fixture::{
    default_spec, digest_file, read_waveform, resolve_within,
};
use crate::json::Json;
use crate::sha256::Sha256;
use crate::storage;

const FRAME_BLOCK: usize = 64;
const DRIFT_LIMIT_PPM: f64 = 200.0;

pub struct AppState {
    pub db: Mutex<Db>,
    pub fixture_dir: PathBuf,
}

impl AppState {
    pub fn new(db: Db, fixture_dir: PathBuf) -> Self {
        AppState {
            db: Mutex::new(db),
            fixture_dir,
        }
    }
}

// ---------- 小工具 ----------

fn obj(pairs: Vec<(&str, Json)>) -> Json {
    let mut m = BTreeMap::new();
    for (k, v) in pairs {
        m.insert(k.to_string(), v);
    }
    Json::Obj(m)
}

fn num_array(xs: &[f64]) -> Json {
    Json::Arr(xs.iter().map(|v| Json::Num((*v * 1e6).round() / 1e6)).collect())
}

fn int_array(xs: &[usize]) -> Json {
    Json::Arr(xs.iter().map(|v| Json::Num(*v as f64)).collect())
}

fn digest_f64(xs: &[f64]) -> String {
    let mut h = Sha256::new();
    for v in xs {
        h.update(&v.to_le_bytes());
    }
    h.finish().iter().map(|b| format!("{b:02x}")).collect()
}

fn digest_bytes(data: &[u8]) -> String {
    let mut h = Sha256::new();
    h.update(data);
    h.finish().iter().map(|b| format!("{b:02x}")).collect()
}

fn frame_audit(samples: &[f64]) -> Json {
    let frames = samples.len();
    let full = frames / FRAME_BLOCK;
    let trailing = frames % FRAME_BLOCK;
    let kind = if trailing == 0 {
        FrameKind::Complete
    } else {
        // 尾部长度不足一个完整帧：不完整采样，与“数值为零”分开归类
        FrameKind::Incomplete
    };
    let zero_count = samples.iter().filter(|v| **v == 0.0).count();
    obj(vec![
        ("frame_block_size", Json::Num(FRAME_BLOCK as f64)),
        ("full_blocks", Json::Num(full as f64)),
        ("trailing_samples", Json::Num(trailing as f64)),
        ("trailing_classification", Json::Str(format!("{kind:?}"))),
        ("zero_sample_count", Json::Num(zero_count as f64)),
        ("note", Json::Str("零值样本与不完整尾帧分别统计，绝不混为一类".into())),
    ])
}

// ---------- 启动引导（幂等） ----------

/// 从本地 fixture 建立标准演示数据集；重复执行不会生成重复衍生物。
pub fn bootstrap(state: &Arc<AppState>) -> AppResult<()> {
    crate::fixture::ensure_fixtures(&state.fixture_dir)?;
    register_calibration(state, "cal_mic_v1.json", 1, true)?;

    let scenes = [
        ("M-CLEAN", "标准短扫频：直达声+两个早期反射+噪声尾", "recording_clean.apwf", false),
        ("M-DRIFT", "采样时钟轻微漂移（约 +500 ppm）", "recording_drift.apwf", true),
        ("M-DROP", "真正丢帧：样本 3000 处丢失 64 采样", "recording_dropped.apwf", false),
    ];
    for (code, title, rec_name, drift_scene) in scenes {
        let mut db = state.db.lock().unwrap();
        if storage::measurement_exists(&db, code)? {
            continue; // 已导入，恢复后不重复生成
        }
        import_and_analyze(&mut db, &state.fixture_dir, code, title, rec_name, drift_scene)?;
    }
    Ok(())
}

fn import_and_analyze(
    db: &mut Db,
    fixture_dir: &Path,
    code: &str,
    title: &str,
    recording_name: &str,
    drift_scene: bool,
) -> AppResult<()> {
    let spec = default_spec();
    let exc_path = resolve_within(fixture_dir, "excitation_sweep.apwf")?;
    let rec_path = resolve_within(fixture_dir, recording_name)?;
    let exc = read_waveform(&exc_path)?;
    let rec = read_waveform(&rec_path)?;

    // 1. 采样率与通道时钟核验
    let rate_match = exc.sample_rate == rec.sample_rate && rec.sample_rate == spec.sample_rate;
    let channels_match = exc.channels == 1 && rec.channels == 1;
    let exc_digest = digest_file(&exc_path)?;
    let rec_digest = digest_file(&rec_path)?;

    let sweep_mono = exc.mono();
    let rec_mono = rec.mono();
    let ppm = drift_ppm(
        &rec_mono,
        &sweep_mono,
        spec.sweep_len(),
        10,
        1500,
        3000,
        200,
    );
    let clock_status = if ppm.abs() > DRIFT_LIMIT_PPM {
        "DriftSuspect"
    } else {
        "Ok"
    };
    let flags = drop_flags(&rec_mono, 100, 8.0);
    let audit = frame_audit(&rec_mono);
    let validation = obj(vec![
        ("sample_rate_match", Json::Bool(rate_match)),
        ("excitation_sample_rate", Json::Num(exc.sample_rate as f64)),
        ("recording_sample_rate", Json::Num(rec.sample_rate as f64)),
        ("expected_sample_rate", Json::Num(spec.sample_rate as f64)),
        ("channels_match", Json::Bool(channels_match)),
        ("channel_count", Json::Num(rec.channels as f64)),
        ("clock_check", Json::Str("通过多锚点局部互相关+最小二乘回归".into())),
        ("drift_scene_fixture", Json::Bool(drift_scene)),
    ]);

    if !rate_match || !channels_match {
        return Err(AppError::Validation(format!(
            "测量 {code} 采样率/通道时钟核验失败：rate_match={rate_match} channels_match={channels_match}"
        )));
    }

    // 单次测量导入整体一个事务：摘要、候选、窗口、台账要么全成功要么全回滚。
    db.transaction(|tx| {
        // 2. 落测量摘要
        let mut st = tx.prepare(
            "INSERT INTO measurements (code,title,excitation_file,recording_file,sample_rate, \
             channel_count,frame_count,excitation_digest,recording_digest,clock_ppm,clock_status, \
             drop_flags_json,frame_audit_json,validation_json) \
             VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14)",
        )?;
        st.bind_text(1, code)?
            .bind_text(2, title)?
            .bind_text(3, "excitation_sweep.apwf")?
            .bind_text(4, recording_name)?
            .bind_i64(5, rec.sample_rate as i64)?
            .bind_i64(6, rec.channels as i64)?
            .bind_i64(7, rec.frame_count() as i64)?
            .bind_text(8, &exc_digest)?
            .bind_text(9, &rec_digest)?
            .bind_f64(10, ppm)?
            .bind_text(11, clock_status)?
            .bind_text(12, &int_array(&flags).dump())?
            .bind_text(13, &audit.dump())?
            .bind_text(14, &validation.dump())?;
        st.next()?;
        let measurement_id = tx.last_insert_rowid();

        // 3. 衍生物台账：已提交则跳过（恢复幂等）
        let input_hash = format!("{exc_digest}:{rec_digest}");
        if artifact_committed(tx, measurement_id, "analysis", &input_hash)? {
            return Ok(());
        }

        // 4. 两个逆滤波候选同时保留
        let decay_window = Window::new(200, 2200);
        let candidate_specs = [("保守正则 λ=1e-3", 1e-3f64), ("低正则 λ=1e-6", 1e-6f64)];
        let mut candidate_ids = Vec::new();
        for (label, reg) in candidate_specs {
            let cand = analyze_candidate(label, &rec_mono, &sweep_mono, reg, decay_window)
                .map_err(AppError::Validation)?;
            let impulse_digest = digest_f64(&cand.impulse);
            let mut cs = tx.prepare(
                "INSERT INTO candidates (measurement_id,label,regularization,peak_index, \
                 offset_correction,reflections_json,decay_start,decay_end,decay_curve_json, \
                 impulse_len,impulse_digest) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)",
            )?;
            cs.bind_i64(1, measurement_id)?
                .bind_text(2, &cand.label)?
                .bind_f64(3, reg)?
                .bind_i64(4, cand.peak_index as i64)?
                .bind_i64(5, cand.offset_correction)?
                .bind_text(6, &int_array(&cand.reflections).dump())?
                .bind_i64(7, decay_window.start as i64)?
                .bind_i64(8, decay_window.end as i64)?
                .bind_text(9, &num_array(&cand.decay_db).dump())?
                .bind_i64(10, cand.impulse.len() as i64)?
                .bind_text(11, &impulse_digest)?;
            cs.next()?;
            candidate_ids.push((tx.last_insert_rowid(), cand));
        }

        // 5. 每个候选各生成 revision 0 自动窗口提案（闭区间，基于主峰）
        for (cid, cand) in &candidate_ids {
            let start = cand.peak_index as i64;
            let end = (cand.peak_index + 2000) as i64;
            storage::insert_window_revision_tx(
                tx,
                measurement_id,
                *cid,
                -1,
                start,
                end,
                "自动提案：主峰对齐起点，闭区间含 2001 个样本",
                "system",
            )?;
        }

        // 6. 首个候选再生成 revision 1 演示并改，使页面初始即有两版窗口证据可对比
        let (first_cid, first_cand) = &candidate_ids[0];
        storage::insert_window_revision_tx(
            tx,
            measurement_id,
            *first_cid,
            0,
            first_cand.peak_index as i64 - 10,
            first_cand.peak_index as i64 + 1980,
            "演示并改：前移起点纳入主峰前振铃，末端收窄避开噪声尾",
            "reviewer-a",
        )?;

        record_artifact(tx, measurement_id, "analysis", &input_hash, &format!("candidates={}", candidate_ids.len()))?;
        Ok(())
    })
}

fn artifact_committed(db: &Db, m: i64, kind: &str, input_hash: &str) -> AppResult<bool> {
    let mut st = db.prepare(
        "SELECT 1 FROM derived_artifacts WHERE measurement_id=?1 AND kind=?2 AND input_hash=?3 AND committed=1",
    )?;
    st.bind_i64(1, m)?.bind_text(2, kind)?.bind_text(3, input_hash)?;
    Ok(st.next()?)
}

fn record_artifact(db: &mut Db, m: i64, kind: &str, input_hash: &str, artifact_ref: &str) -> AppResult<()> {
    let mut st = db.prepare(
        "INSERT OR IGNORE INTO derived_artifacts (measurement_id,kind,input_hash,artifact_ref,committed) \
         VALUES (?1,?2,?3,?4,1)",
    )?;
    st.bind_i64(1, m)?
        .bind_text(2, kind)?
        .bind_text(3, input_hash)?
        .bind_text(4, artifact_ref)?;
    st.next()?;
    Ok(())
}

// ---------- 校准链 ----------

pub fn register_calibration(state: &Arc<AppState>, file_name: &str, version: i64, active: bool) -> AppResult<i64> {
    let path = resolve_within(&state.fixture_dir, file_name)?;
    let digest = digest_file(&path)?;
    let mut db = state.db.lock().unwrap();
    let mut sel = db.prepare("SELECT id FROM calibrations WHERE digest=?1")?;
    sel.bind_text(1, &digest)?;
    if sel.next()? {
        return Ok(sel.column_i64(0));
    }
    let mut ins = db.prepare(
        "INSERT INTO calibrations (cal_code,version,file_name,digest,active) VALUES ('mic-chain-A',?1,?2,?3,?4)",
    )?;
    ins.bind_i64(1, version)?
        .bind_text(2, file_name)?
        .bind_text(3, &digest)?
        .bind_i64(4, active as i64)?;
    ins.next()?;
    Ok(db.last_insert_rowid())
}

/// 替换校准：旧版置失效，写入换版事件，并为所有引用旧 digest 的已发布响应
/// 生成影响清单。**只登记影响，绝不静默重算。**
pub fn replace_calibration(state: &Arc<AppState>, new_file: &str) -> AppResult<Json> {
    let path = resolve_within(&state.fixture_dir, new_file)?;
    let new_digest = digest_file(&path)?;
    let mut db = state.db.lock().unwrap();
    db.transaction(|tx| {
        // 当前有效版本
        let mut old = tx.prepare("SELECT id,version,digest FROM calibrations WHERE cal_code='mic-chain-A' AND active=1")?;
        let has_old = old.next()?;
        let (old_id, old_version, old_digest) = if has_old {
            (Some(old.column_i64(0)), old.column_i64(1), Some(old.column_text(2)))
        } else {
            (None, 0, None)
        };
        drop(old);

        // 新版本是否已登记
        let mut existing = tx.prepare("SELECT id FROM calibrations WHERE digest=?1")?;
        existing.bind_text(1, &new_digest)?;
        let new_id = if existing.next()? {
            existing.column_i64(0)
        } else {
            drop(existing);
            // 版本号 = 旧版本 + 1
            let mut ins = tx.prepare(
                "INSERT INTO calibrations (cal_code,version,file_name,digest,active,superseded_by) \
                 VALUES ('mic-chain-A',?1,?2,?3,1,NULL)",
            )?;
            ins.bind_i64(1, old_version + 1)?
                .bind_text(2, new_file)?
                .bind_text(3, &new_digest)?;
            ins.next()?;
            tx.last_insert_rowid()
        };
        if let Some(oid) = old_id {
            if oid == new_id {
                return Err(AppError::Validation(
                    "该 digest 的校准已是当前生效版本，无需换版".into(),
                ));
            }
        }
        if let Some(oid) = old_id {
            let mut up = tx.prepare("UPDATE calibrations SET active=0, superseded_by=?1 WHERE id=?2")?;
            up.bind_i64(1, new_id)?.bind_i64(2, oid)?;
            up.next()?;
        }
        let mut ch = tx.prepare(
            "INSERT INTO calibration_changes (cal_code,old_calibration_id,new_calibration_id,old_digest,new_digest) \
             VALUES ('mic-chain-A',?1,?2,?3,?4)",
        )?;
        match old_id {
            Some(v) => ch.bind_i64(1, v)?,
            None => ch.bind_null(1)?,
        };
        ch.bind_i64(2, new_id)?;
        match &old_digest {
            Some(d) => ch.bind_text(3, d)?,
            None => ch.bind_null(3)?,
        };
        ch.bind_text(4, &new_digest)?;
        ch.next()?;
        let change_id = tx.last_insert_rowid();

        // 影响清单：冻结在旧 digest 上的已发布响应
        let mut impacted = Vec::new();
        if let Some(od) = &old_digest {
            let mut q = tx.prepare(
                "SELECT id,measurement_id FROM published_responses WHERE calibration_digest=?1 AND superseded=0",
            )?;
            q.bind_text(1, od)?;
            while q.next()? {
                let pub_id = q.column_i64(0);
                let m_id = q.column_i64(1);
                let mut ii = tx.prepare(
                    "INSERT OR IGNORE INTO impact_items (change_id,measurement_id,published_response_id,status,note) \
                     VALUES (?1,?2,?3,'NeedsReview','校准 digest 已换版，响应冻结在旧版本，待人工审议，不自动重算')",
                )?;
                ii.bind_i64(1, change_id)?
                    .bind_i64(2, m_id)?
                    .bind_i64(3, pub_id)?;
                ii.next()?;
                impacted.push(pub_id);
            }
        }
        let mut done = tx.prepare("UPDATE calibration_changes SET impact_generated=1 WHERE id=?1")?;
        done.bind_i64(1, change_id)?;
        done.next()?;

        Ok(obj(vec![
            ("change_id", Json::Num(change_id as f64)),
            ("old_digest", Json::Str(old_digest.unwrap_or_default())),
            ("new_digest", Json::Str(new_digest)),
            ("impact_count", Json::Num(impacted.len() as f64)),
            ("impact_published_response_ids", int_array(&impacted.iter().map(|v| *v as usize).collect::<Vec<_>>())),
            ("policy", Json::Str("只生成影响清单，不静默重算".into())),
        ]))
    })
}

// ---------- 发布 ----------

pub fn publish_response(state: &Arc<AppState>, measurement_id: i64, candidate_id: i64, window_revision_id: i64) -> AppResult<Json> {
    let mut db = state.db.lock().unwrap();
    db.transaction(|tx| {
        // 校验窗口属于该候选且端点闭区间合法
        let mut w = tx.prepare(
            "SELECT revision,start_sample,end_sample,sample_count FROM window_revisions \
             WHERE id=?1 AND measurement_id=?2 AND candidate_id=?3",
        )?;
        w.bind_i64(1, window_revision_id)?
            .bind_i64(2, measurement_id)?
            .bind_i64(3, candidate_id)?;
        if !w.next()? {
            return Err(AppError::NotFound("窗口 revision 不属于指定候选/测量".into()));
        }
        let (revision, ws, we, count) = (w.column_i64(0), w.column_i64(1), w.column_i64(2), w.column_i64(3));
        if count != we - ws + 1 {
            return Err(AppError::Validation("闭区间样本数约束不一致".into()));
        }
        drop(w);

        // 当前有效校准
        let mut c = tx.prepare("SELECT id,digest FROM calibrations WHERE cal_code='mic-chain-A' AND active=1")?;
        if !c.next()? {
            return Err(AppError::NotFound("没有生效中的校准版本，无法发布".into()));
        }
        let cal_id = c.column_i64(0);
        let cal_digest = c.column_text(1);
        drop(c);

        // 响应 digest：窗口端点 + 候选 impulse digest + 校准 digest
        let mut ci = tx.prepare("SELECT impulse_digest FROM candidates WHERE id=?1 AND measurement_id=?2")?;
        ci.bind_i64(1, candidate_id)?.bind_i64(2, measurement_id)?;
        if !ci.next()? {
            return Err(AppError::NotFound("候选不存在".into()));
        }
        let impulse_digest = ci.column_text(0);
        let response_digest = digest_bytes(
            format!("{impulse_digest}|[{ws},{we}]|rev{revision}|{cal_digest}").as_bytes(),
        );

        let mut ins = tx.prepare(
            "INSERT INTO published_responses (measurement_id,candidate_id,window_revision_id, \
             calibration_id,calibration_digest,response_digest) VALUES (?1,?2,?3,?4,?5,?6)",
        )?;
        ins.bind_i64(1, measurement_id)?
            .bind_i64(2, candidate_id)?
            .bind_i64(3, window_revision_id)?
            .bind_i64(4, cal_id)?
            .bind_text(5, &cal_digest)?
            .bind_text(6, &response_digest)?;
        ins.next()?;
        let id = tx.last_insert_rowid();
        Ok(obj(vec![
            ("published_response_id", Json::Num(id as f64)),
            ("calibration_digest_frozen", Json::Str(cal_digest)),
            ("window_closed_interval", Json::Str(format!("[{ws}, {we}]"))),
            ("window_revision", Json::Num(revision as f64)),
            ("response_digest", Json::Str(response_digest)),
        ]))
    })
}

// ---------- 查询 ----------

pub fn list_measurements(state: &Arc<AppState>) -> AppResult<Json> {
    let db = state.db.lock().unwrap();
    let mut st = db.prepare(
        "SELECT id,code,title,recording_file,sample_rate,frame_count,clock_ppm,clock_status, \
         drop_flags_json FROM measurements ORDER BY id",
    )?;
    let mut items = Vec::new();
    while st.next()? {
        items.push(obj(vec![
            ("id", Json::Num(st.column_i64(0) as f64)),
            ("code", Json::Str(st.column_text(1))),
            ("title", Json::Str(st.column_text(2))),
            ("recording_file", Json::Str(st.column_text(3))),
            ("sample_rate", Json::Num(st.column_i64(4) as f64)),
            ("frame_count", Json::Num(st.column_i64(5) as f64)),
            ("clock_ppm", Json::Num(st.column_f64(6))),
            ("clock_status", Json::Str(st.column_text(7))),
            ("drop_flags", Json::Str(st.column_text(8))),
        ]));
    }
    Ok(Json::Arr(items))
}

pub fn measurement_detail(state: &Arc<AppState>, id: i64) -> AppResult<Json> {
    let db = state.db.lock().unwrap();
    let mut st = db.prepare(
        "SELECT code,title,excitation_file,recording_file,sample_rate,channel_count,frame_count, \
         excitation_digest,recording_digest,clock_ppm,clock_status,drop_flags_json, \
         frame_audit_json,validation_json FROM measurements WHERE id=?1",
    )?;
    st.bind_i64(1, id)?;
    if !st.next()? {
        return Err(AppError::NotFound(format!("测量 {id} 不存在")));
    }
    Ok(obj(vec![
        ("id", Json::Num(id as f64)),
        ("code", Json::Str(st.column_text(0))),
        ("title", Json::Str(st.column_text(1))),
        ("excitation_file", Json::Str(st.column_text(2))),
        ("recording_file", Json::Str(st.column_text(3))),
        ("sample_rate", Json::Num(st.column_i64(4) as f64)),
        ("channel_count", Json::Num(st.column_i64(5) as f64)),
        ("frame_count", Json::Num(st.column_i64(6) as f64)),
        ("excitation_digest", Json::Str(st.column_text(7))),
        ("recording_digest", Json::Str(st.column_text(8))),
        ("clock_ppm", Json::Num(st.column_f64(9))),
        ("clock_status", Json::Str(st.column_text(10))),
        ("drop_flags", Json::Str(st.column_text(11))),
        ("frame_audit", Json::Str(st.column_text(12))),
        ("validation", Json::Str(st.column_text(13))),
    ]))
}

pub fn list_candidates(state: &Arc<AppState>, measurement_id: i64) -> AppResult<Json> {
    let db = state.db.lock().unwrap();
    let mut st = db.prepare(
        "SELECT id,label,regularization,peak_index,offset_correction,reflections_json, \
         decay_start,decay_end,decay_curve_json,impulse_len,impulse_digest \
         FROM candidates WHERE measurement_id=?1 ORDER BY id",
    )?;
    st.bind_i64(1, measurement_id)?;
    let mut items = Vec::new();
    while st.next()? {
        items.push(obj(vec![
            ("id", Json::Num(st.column_i64(0) as f64)),
            ("label", Json::Str(st.column_text(1))),
            ("regularization", Json::Num(st.column_f64(2))),
            ("peak_index", Json::Num(st.column_i64(3) as f64)),
            ("offset_correction", Json::Num(st.column_i64(4) as f64)),
            ("reflections", Json::Str(st.column_text(5))),
            ("decay_window", Json::Str(format!("[{}, {}] 闭区间", st.column_i64(6), st.column_i64(7)))),
            ("decay_start", Json::Num(st.column_i64(6) as f64)),
            ("decay_end", Json::Num(st.column_i64(7) as f64)),
            ("decay_curve", Json::Str(st.column_text(8))),
            ("impulse_len", Json::Num(st.column_i64(9) as f64)),
            ("impulse_digest", Json::Str(st.column_text(10))),
        ]));
    }
    Ok(Json::Arr(items))
}

/// 窗口并改对比：每个 revision 同时返回自身端点与所基于的基线端点。
pub fn list_windows(state: &Arc<AppState>, measurement_id: i64) -> AppResult<Json> {
    let db = state.db.lock().unwrap();
    let mut st = db.prepare(
        "SELECT w.id,w.candidate_id,w.revision,w.start_sample,w.end_sample,w.sample_count, \
         w.based_on_revision,w.rationale,w.author,c.label \
         FROM window_revisions w JOIN candidates c ON c.id=w.candidate_id \
         WHERE w.measurement_id=?1 ORDER BY w.candidate_id,w.revision",
    )?;
    st.bind_i64(1, measurement_id)?;
    let mut items = Vec::new();
    while st.next()? {
        let id = st.column_i64(0);
        let candidate_id = st.column_i64(1);
        let revision = st.column_i64(2);
        let based_on = st.column_i64(6);
        let baseline = baseline_endpoints(&db, candidate_id, based_on)?;
        items.push(obj(vec![
            ("id", Json::Num(id as f64)),
            ("candidate_id", Json::Num(candidate_id as f64)),
            ("candidate_label", Json::Str(st.column_text(9))),
            ("revision", Json::Num(revision as f64)),
            ("start_sample", Json::Num(st.column_i64(3) as f64)),
            ("end_sample", Json::Num(st.column_i64(4) as f64)),
            ("sample_count", Json::Num(st.column_i64(5) as f64)),
            ("closed_interval", Json::Str(format!("[{}, {}]", st.column_i64(3), st.column_i64(4)))),
            ("based_on_revision", Json::Num(based_on as f64)),
            ("baseline", baseline),
            ("rationale", Json::Str(st.column_text(7))),
            ("author", Json::Str(st.column_text(8))),
        ]));
    }
    Ok(Json::Arr(items))
}

fn baseline_endpoints(db: &Db, candidate_id: i64, revision: i64) -> AppResult<Json> {
    if revision < 0 {
        return Ok(obj(vec![
            ("revision", Json::Num(-1.0)),
            ("endpoint", Json::Null),
            ("note", Json::Str("首个 revision，无前置基线".into())),
        ]));
    }
    let mut st = db.prepare(
        "SELECT start_sample,end_sample,sample_count FROM window_revisions \
         WHERE candidate_id=?1 AND revision=?2",
    )?;
    st.bind_i64(1, candidate_id)?.bind_i64(2, revision)?;
    if st.next()? {
        Ok(obj(vec![
            ("revision", Json::Num(revision as f64)),
            ("start_sample", Json::Num(st.column_i64(0) as f64)),
            ("end_sample", Json::Num(st.column_i64(1) as f64)),
            ("sample_count", Json::Num(st.column_i64(2) as f64)),
            ("closed_interval", Json::Str(format!("[{}, {}]", st.column_i64(0), st.column_i64(1)))),
        ]))
    } else {
        Ok(obj(vec![("revision", Json::Num(revision as f64)), ("endpoint", Json::Null)]))
    }
}

/// 创建窗口新 revision；响应同时携带新端点与基线端点，供前端并改对比。
pub fn create_window(
    state: &Arc<AppState>,
    measurement_id: i64,
    candidate_id: i64,
    expected_revision: i64,
    start: i64,
    end: i64,
    rationale: &str,
    author: &str,
) -> AppResult<Json> {
    if start < 0 || end < start {
        return Err(AppError::Validation(format!(
            "闭区间端点非法：start={start}, end={end}，要求 0 <= start <= end"
        )));
    }
    let mut db = state.db.lock().unwrap();
    let new_id = storage::insert_window_revision(
        &mut db,
        measurement_id,
        candidate_id,
        expected_revision,
        start,
        end,
        rationale,
        author,
    )?;
    let baseline = baseline_endpoints(&db, candidate_id, expected_revision)?;
    Ok(obj(vec![
        ("window_revision_id", Json::Num(new_id as f64)),
        ("new_revision", Json::Num((expected_revision + 1) as f64)),
        ("new_endpoints", obj(vec![
            ("start_sample", Json::Num(start as f64)),
            ("end_sample", Json::Num(end as f64)),
            ("sample_count", Json::Num((end - start + 1) as f64)),
            ("closed_interval", Json::Str(format!("[{start}, {end}]"))),
        ])),
        ("baseline", baseline),
        ("note", Json::Str("旧参数保留为独立 revision，未被覆盖".into())),
    ]))
}

pub fn list_reviews(state: &Arc<AppState>, measurement_id: i64) -> AppResult<Json> {
    let db = state.db.lock().unwrap();
    let mut st = db.prepare(
        "SELECT id,version,reviewer,decision,note,candidate_id,window_revision_id,created_at \
         FROM reviews WHERE measurement_id=?1 ORDER BY version",
    )?;
    st.bind_i64(1, measurement_id)?;
    let mut items = Vec::new();
    while st.next()? {
        let mut m = BTreeMap::new();
        m.insert("id".into(), Json::Num(st.column_i64(0) as f64));
        m.insert("version".into(), Json::Num(st.column_i64(1) as f64));
        m.insert("reviewer".into(), Json::Str(st.column_text(2)));
        m.insert("decision".into(), Json::Str(st.column_text(3)));
        m.insert("note".into(), Json::Str(st.column_text(4)));
        m.insert(
            "candidate_id".into(),
            if st.column_is_null(5) { Json::Null } else { Json::Num(st.column_i64(5) as f64) },
        );
        m.insert(
            "window_revision_id".into(),
            if st.column_is_null(6) { Json::Null } else { Json::Num(st.column_i64(6) as f64) },
        );
        m.insert("created_at".into(), Json::Str(st.column_text(7)));
        items.push(Json::Obj(m));
    }
    Ok(Json::Arr(items))
}

pub fn create_review(
    state: &Arc<AppState>,
    measurement_id: i64,
    expected_version: i64,
    reviewer: &str,
    decision: &str,
    note: &str,
    candidate_id: Option<i64>,
    window_revision_id: Option<i64>,
) -> AppResult<Json> {
    if !matches!(decision, "Accepted" | "Rejected" | "ChangeRequested") {
        return Err(AppError::Validation("decision 必须是 Accepted/Rejected/ChangeRequested".into()));
    }
    let mut db = state.db.lock().unwrap();
    let id = storage::insert_review(
        &mut db,
        measurement_id,
        expected_version,
        reviewer,
        decision,
        note,
        candidate_id,
        window_revision_id,
    )?;
    Ok(obj(vec![
        ("review_id", Json::Num(id as f64)),
        ("new_version", Json::Num((expected_version + 1) as f64)),
        ("conflict_policy", Json::Str("版本号不匹配即拒绝覆盖".into())),
    ]))
}

pub fn list_calibrations(state: &Arc<AppState>) -> AppResult<Json> {
    let db = state.db.lock().unwrap();
    let mut st = db.prepare(
        "SELECT id,cal_code,version,file_name,digest,active,superseded_by FROM calibrations ORDER BY id",
    )?;
    let mut items = Vec::new();
    while st.next()? {
        items.push(obj(vec![
            ("id", Json::Num(st.column_i64(0) as f64)),
            ("cal_code", Json::Str(st.column_text(1))),
            ("version", Json::Num(st.column_i64(2) as f64)),
            ("file_name", Json::Str(st.column_text(3))),
            ("digest", Json::Str(st.column_text(4))),
            ("active", Json::Bool(st.column_i64(5) == 1)),
            (
                "superseded_by",
                if st.column_is_null(6) { Json::Null } else { Json::Num(st.column_i64(6) as f64) },
            ),
        ]));
    }
    Ok(Json::Arr(items))
}

pub fn impact_list(state: &Arc<AppState>, change_id: i64) -> AppResult<Json> {
    let db = state.db.lock().unwrap();
    let mut st = db.prepare(
        "SELECT i.id,i.measurement_id,i.published_response_id,i.status,i.note,c.new_digest,c.old_digest \
         FROM impact_items i JOIN calibration_changes c ON c.id=i.change_id \
         WHERE i.change_id=?1 ORDER BY i.id",
    )?;
    st.bind_i64(1, change_id)?;
    let mut items = Vec::new();
    while st.next()? {
        items.push(obj(vec![
            ("id", Json::Num(st.column_i64(0) as f64)),
            ("measurement_id", Json::Num(st.column_i64(1) as f64)),
            ("published_response_id", Json::Num(st.column_i64(2) as f64)),
            ("status", Json::Str(st.column_text(3))),
            ("note", Json::Str(st.column_text(4))),
            ("new_calibration_digest", Json::Str(st.column_text(5))),
            ("frozen_calibration_digest", Json::Str(st.column_text(6))),
        ]));
    }
    Ok(Json::Arr(items))
}

pub fn list_changes(state: &Arc<AppState>) -> AppResult<Json> {
    let db = state.db.lock().unwrap();
    let mut st = db.prepare(
        "SELECT id,cal_code,old_digest,new_digest,impact_generated,created_at FROM calibration_changes ORDER BY id",
    )?;
    let mut items = Vec::new();
    while st.next()? {
        items.push(obj(vec![
            ("id", Json::Num(st.column_i64(0) as f64)),
            ("cal_code", Json::Str(st.column_text(1))),
            (
                "old_digest",
                if st.column_is_null(2) { Json::Null } else { Json::Str(st.column_text(2)) },
            ),
            ("new_digest", Json::Str(st.column_text(3))),
            ("impact_generated", Json::Bool(st.column_i64(4) == 1)),
            ("created_at", Json::Str(st.column_text(5))),
        ]));
    }
    Ok(Json::Arr(items))
}

pub fn list_published(state: &Arc<AppState>, measurement_id: i64) -> AppResult<Json> {
    let db = state.db.lock().unwrap();
    let mut st = db.prepare(
        "SELECT id,candidate_id,window_revision_id,calibration_id,calibration_digest,response_digest,superseded \
         FROM published_responses WHERE measurement_id=?1 ORDER BY id",
    )?;
    st.bind_i64(1, measurement_id)?;
    let mut items = Vec::new();
    while st.next()? {
        items.push(obj(vec![
            ("id", Json::Num(st.column_i64(0) as f64)),
            ("candidate_id", Json::Num(st.column_i64(1) as f64)),
            ("window_revision_id", Json::Num(st.column_i64(2) as f64)),
            ("calibration_id", Json::Num(st.column_i64(3) as f64)),
            ("calibration_digest", Json::Str(st.column_text(4))),
            ("response_digest", Json::Str(st.column_text(5))),
            ("superseded", Json::Bool(st.column_i64(6) == 1)),
        ]));
    }
    Ok(Json::Arr(items))
}
