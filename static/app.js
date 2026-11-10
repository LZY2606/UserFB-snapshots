// 声场脉络前端：原生 JS，无框架无 CDN 无上传。
let STATE = { measurements: [], current: null, candidates: [], windows: [], reviews: [],
              calibrations: [], changes: [], published: [] };

const $ = (id) => document.getElementById(id);

function flash(msg, ok) {
  const el = $("flash");
  el.textContent = msg;
  el.className = "flash " + (ok ? "ok" : "err");
  clearTimeout(flash._t);
  flash._t = setTimeout(() => { el.className = "flash"; }, 6000);
}

async function api(method, url, body) {
  const opt = { method, headers: { "Content-Type": "application/json" } };
  if (body !== undefined) opt.body = JSON.stringify(body);
  const res = await fetch(url, opt);
  const text = await res.text();
  let data = null;
  try { data = text ? JSON.parse(text) : null; } catch (e) { throw new Error("响应不是 JSON: " + text.slice(0, 200)); }
  if (!res.ok) throw new Error((data && data.message) || ("HTTP " + res.status));
  return data;
}

function parseMaybe(s) {
  if (typeof s !== "string") return s;
  try { return JSON.parse(s); } catch (e) { return s; }
}

function esc(s) {
  return String(s ?? "").replace(/[&<>"']/g, (c) =>
    ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
}

async function loadMeasurements() {
  STATE.measurements = await api("GET", "/api/measurements");
  const sel = $("measSelect");
  sel.innerHTML = STATE.measurements.map((m) =>
    `<option value="${m.id}">${esc(m.code)} · ${esc(m.title)}（时钟 ${m.clock_status}，${m.frame_count} 样本）</option>`).join("");
  if (!STATE.current && STATE.measurements.length) STATE.current = STATE.measurements[0].id;
  sel.value = STATE.current;
}

async function refreshAll() {
  try {
    STATE.current = Number($("measSelect").value);
    const [detail, cands, wins, revs, cals, changes, published] = await Promise.all([
      api("GET", `/api/measurements/${STATE.current}`),
      api("GET", `/api/measurements/${STATE.current}/candidates`),
      api("GET", `/api/measurements/${STATE.current}/windows`),
      api("GET", `/api/measurements/${STATE.current}/reviews`),
      api("GET", "/api/calibrations"),
      api("GET", "/api/calibration-changes"),
      api("GET", `/api/measurements/${STATE.current}/published`),
    ]);
    STATE.detail = detail; STATE.candidates = cands; STATE.windows = wins;
    STATE.reviews = revs; STATE.calibrations = cals; STATE.changes = changes; STATE.published = published;
    renderAll();
  } catch (e) { flash(e.message, false); }
}

function renderAll() {
  renderSummary(); renderClock(); renderCandidates();
  drawDecay(); drawTimeline(); renderWindows(); renderReviews();
  renderCalibrations(); renderChanges(); renderPublished();
}

function renderSummary() {
  const d = STATE.detail;
  $("summary").innerHTML = `<table>
    <tr><th>编码</th><td>${esc(d.code)}</td><th>录音文件</th><td><code>${esc(d.recording_file)}</code></td></tr>
    <tr><th>采样率</th><td>${d.sample_rate} Hz</td><th>通道数</th><td>${d.channel_count}（单通道时钟核验 ${parseMaybe(d.validation).channels_match ? "通过" : "失败"}）</td></tr>
    <tr><th>帧总数</th><td>${d.frame_count}</td><th>激励 digest</th><td><code>${esc(d.excitation_digest.slice(0, 16))}…</code></td></tr>
    <tr><th>录音 digest</th><td colspan="3"><code>${esc(d.recording_digest)}</code></td></tr>
  </table>`;
}

function renderClock() {
  const d = STATE.detail;
  const audit = parseMaybe(d.frame_audit);
  const flags = parseMaybe(d.drop_flags);
  const drift = Math.abs(d.clock_ppm) > 200;
  $("clockPanel").innerHTML = `
    <p>时钟漂移估计：<b>${d.clock_ppm.toFixed(1)} ppm</b>
      <span class="pill ${drift ? "warn" : "ok"}">${esc(d.clock_status)}</span>
      <span class="muted">（多锚点局部互相关 + 最小二乘回归，阈值 ±200 ppm）</span></p>
    <p>丢帧标记：${flags.length ? `<span class="pill bad">异常样本 ${flags.join(", ")}</span>` : '<span class="pill ok">无异常</span>'}
       <span class="muted">（局部窗口中位数 + 8·MAD，安静段不误报）</span></p>
    <table>
      <tr><th>帧块大小</th><th>完整块数</th><th>尾部样本</th><th>尾部归类</th><th>零值样本</th></tr>
      <tr><td>${audit.frame_block_size}</td><td>${audit.full_blocks}</td><td>${audit.trailing_samples}</td>
          <td><span class="tag">${esc(audit.trailing_classification)}</span></td><td>${audit.zero_sample_count}</td></tr>
    </table>
    <p class="muted">${esc(audit.note)}</p>`;
}

function renderCandidates() {
  $("candidateCards").innerHTML = STATE.candidates.map((c) => {
    const refs = parseMaybe(c.reflections);
    return `<div>
      <h3>${esc(c.label)} <span class="tag">id ${c.id}</span></h3>
      <table>
        <tr><th>正则系数 λ</th><td>${c.regularization}</td></tr>
        <tr><th>主峰索引（直达声）</th><td><b>${c.peak_index}</b></td></tr>
        <tr><th>偏移校正</th><td>${c.offset_correction} 样本</td></tr>
        <tr><th>早期反射到达</th><td>${refs.map((r) => `<span class="pill warn">${r}</span>`).join(" ")}</td></tr>
        <tr><th>衰减闭区间</th><td><code>${esc(c.decay_window)}</code>（${(c.decay_end - c.decay_start + 1)} 点）</td></tr>
        <tr><th>脉冲响应 digest</th><td><code>${esc(c.impulse_digest.slice(0, 20))}…</code></td></tr>
      </table>
    </div>`;
  }).join("");
  $("wCandidate").innerHTML = STATE.candidates.map((c) =>
    `<option value="${c.id}">${esc(c.label)}（id ${c.id}）</option>`).join("");
  if (STATE.windows.length) {
    const cid = Number($("wCandidate").value);
    const revs = STATE.windows.filter((w) => w.candidate_id === cid);
    if (revs.length) $("wExpected").value = revs[revs.length - 1].revision;
  }
}

function drawDecay() {
  const cv = $("decayCanvas"); const ctx = cv.getContext("2d");
  const W = cv.width = cv.clientWidth * devicePixelRatio, H = cv.height = 260 * devicePixelRatio;
  ctx.clearRect(0, 0, W, H);
  const pad = { l: 56 * devicePixelRatio, r: 16 * devicePixelRatio, t: 16 * devicePixelRatio, b: 30 * devicePixelRatio };
  const curves = STATE.candidates.map((c) => parseMaybe(c.decay_curve));
  let minDb = 0;
  curves.forEach((arr) => arr.forEach((v) => { if (v < minDb) minDb = v; }));
  minDb = Math.floor(minDb / 10) * 10;
  const x = (i, n) => pad.l + (W - pad.l - pad.r) * (i / Math.max(1, n - 1));
  const y = (db) => pad.t + (H - pad.t - pad.b) * (db / minDb);
  ctx.strokeStyle = "#2a3550"; ctx.fillStyle = "#8fa0bf"; ctx.lineWidth = devicePixelRatio;
  ctx.font = `${11 * devicePixelRatio}px sans-serif`;
  for (let db = 0; db >= minDb; db -= 10) {
    ctx.beginPath(); ctx.moveTo(pad.l, y(db)); ctx.lineTo(W - pad.r, y(db)); ctx.stroke();
    ctx.fillText(db + " dB", 8 * devicePixelRatio, y(db) + 4 * devicePixelRatio);
  }
  ctx.fillText("样本索引（闭区间内逐点）", W / 2 - 60 * devicePixelRatio, H - 8 * devicePixelRatio);
  const colors = ["#5db0ff", "#ffb454"];
  STATE.candidates.forEach((c, ci) => {
    const arr = curves[ci];
    ctx.strokeStyle = colors[ci % 2]; ctx.lineWidth = 2 * devicePixelRatio;
    ctx.beginPath();
    arr.forEach((v, i) => { const px = x(i, arr.length), py = y(v); i ? ctx.lineTo(px, py) : ctx.moveTo(px, py); });
    ctx.stroke();
    ctx.fillStyle = colors[ci % 2];
    ctx.fillText(esc(c.label), pad.l + ci * 220 * devicePixelRatio, pad.t + 14 * devicePixelRatio);
  });
}

function drawTimeline() {
  const cv = $("timelineCanvas"); const ctx = cv.getContext("2d");
  const W = cv.width = cv.clientWidth * devicePixelRatio, H = cv.height = 200 * devicePixelRatio;
  ctx.clearRect(0, 0, W, H);
  const pad = { l: 30 * devicePixelRatio, r: 16 * devicePixelRatio, t: 18 * devicePixelRatio, b: 26 * devicePixelRatio };
  let maxX = 0;
  STATE.windows.forEach((w) => { maxX = Math.max(maxX, w.end_sample); });
  STATE.candidates.forEach((c) => { maxX = Math.max(maxX, c.peak_index, ...parseMaybe(c.reflections)); });
  maxX = Math.ceil(maxX * 1.08);
  const x = (s) => pad.l + (W - pad.l - pad.r) * (s / maxX);
  // 轴
  ctx.strokeStyle = "#2a3550"; ctx.lineWidth = devicePixelRatio;
  ctx.beginPath(); ctx.moveTo(pad.l, H - pad.b); ctx.lineTo(W - pad.r, H - pad.b); ctx.stroke();
  ctx.fillStyle = "#8fa0bf"; ctx.font = `${11 * devicePixelRatio}px sans-serif`;
  for (let s = 0; s <= maxX; s += Math.ceil(maxX / 8 / 100) * 100) {
    ctx.fillText(String(s), x(s) - 10 * devicePixelRatio, H - 8 * devicePixelRatio);
  }
  // 主峰与反射
  STATE.candidates.forEach((c, ci) => {
    const marks = [{ i: c.peak_index, color: "#ff6b6b", name: "主峰" }]
      .concat(parseMaybe(c.reflections).map((r) => ({ i: r, color: "#ffb454", name: "反射" })));
    marks.forEach((m) => {
      ctx.strokeStyle = m.color; ctx.lineWidth = 2 * devicePixelRatio;
      ctx.beginPath(); ctx.moveTo(x(m.i), pad.t); ctx.lineTo(x(m.i), H - pad.b); ctx.stroke();
      ctx.fillStyle = m.color;
      ctx.fillText(`${m.name}@${m.i}`, x(m.i) + 3 * devicePixelRatio, pad.t + (12 + ci * 14) * devicePixelRatio);
    });
  });
  // 窗口 revision 括号，按候选分泳道
  const laneH = 26 * devicePixelRatio;
  STATE.windows.forEach((w, idx) => {
    const lane = STATE.candidates.findIndex((c) => c.id === w.candidate_id);
    const yy = pad.t + 18 * devicePixelRatio + lane * laneH * 1.4 + (w.revision % 2) * 6 * devicePixelRatio;
    const color = w.revision === 0 ? "#8fa0bf" : "#57d9a3";
    ctx.strokeStyle = color; ctx.lineWidth = 2 * devicePixelRatio;
    ctx.beginPath();
    ctx.moveTo(x(w.start_sample), yy); ctx.lineTo(x(w.end_sample), yy);
    ctx.moveTo(x(w.start_sample), yy - 5 * devicePixelRatio); ctx.lineTo(x(w.start_sample), yy + 5 * devicePixelRatio);
    ctx.moveTo(x(w.end_sample), yy - 5 * devicePixelRatio); ctx.lineTo(x(w.end_sample), yy + 5 * devicePixelRatio);
    ctx.stroke();
    ctx.fillStyle = color;
    ctx.fillText(`rev${w.revision} [${w.start_sample}, ${w.end_sample}] 闭区间·${w.sample_count}点`,
      x(w.start_sample), yy - 7 * devicePixelRatio);
  });
}

function renderWindows() {
  $("windowList").innerHTML = STATE.windows.length ? `<table>
    <tr><th>ID</th><th>候选</th><th>revision</th><th>闭区间端点</th><th>样本数</th><th>基于基线</th><th>理由</th><th>作者</th></tr>
    ${STATE.windows.map((w) => `<tr>
      <td>${w.id}</td><td>${esc(w.candidate_label)}</td><td><b>${w.revision}</b></td>
      <td><code>${esc(w.closed_interval)}</code></td><td>${w.sample_count}</td>
      <td>${w.baseline.endpoint === null && w.baseline.revision === -1
        ? '<span class="tag">首个 revision</span>'
        : `<span class="tag">rev${w.baseline.revision}</span> <code>${esc(w.baseline.closed_interval)}</code>（${w.baseline.sample_count} 点）`}</td>
      <td>${esc(w.rationale)}</td><td>${esc(w.author)}</td></tr>`).join("")}
  </table>` : "<p class='muted'>暂无窗口提案</p>";
}

async function submitWindow() {
  try {
    const cid = Number($("wCandidate").value);
    const body = {
      candidate_id: cid,
      expected_revision: Number($("wExpected").value),
      start_sample: Number($("wStart").value),
      end_sample: Number($("wEnd").value),
      rationale: $("wRationale").value,
      author: $("wAuthor").value,
    };
    const r = await api("POST", `/api/measurements/${STATE.current}/windows`, body);
    flash(`新 revision ${r.new_revision} 已保存：新端点 ${r.new_endpoints.closed_interval}（${r.new_endpoints.sample_count} 点）；基线 rev${r.baseline.revision} 保留未覆盖`, true);
    await refreshAll();
  } catch (e) { flash(e.message, false); }
}

async function demoWindow() {
  const cid = Number($("wCandidate").value);
  const revs = STATE.windows.filter((w) => w.candidate_id === cid);
  const latest = revs[revs.length - 1];
  $("wExpected").value = latest.revision;
  $("wStart").value = latest.start_sample - 10;
  $("wEnd").value = latest.end_sample - 20;
  await submitWindow();
}

function renderReviews() {
  $("reviewList").innerHTML = STATE.reviews.length ? `<table>
    <tr><th>版本</th><th>审议人</th><th>结论</th><th>意见</th><th>候选/窗口</th><th>时间</th></tr>
    ${STATE.reviews.map((r) => `<tr><td><b>v${r.version}</b></td><td>${esc(r.reviewer)}</td>
      <td><span class="pill ${r.decision === "Accepted" ? "ok" : "warn"}">${esc(r.decision)}</span></td>
      <td>${esc(r.note)}</td><td>candidate ${r.candidate_id ?? "—"} / window ${r.window_revision_id ?? "—"}</td>
      <td>${esc(r.created_at)}</td></tr>`).join("")}</table>`
    : "<p class='muted'>尚无审议，当前版本 v0</p>";
  $("rExpected").value = STATE.reviews.length;
}

async function submitReview() {
  try {
    const body = {
      expected_version: Number($("rExpected").value),
      reviewer: $("rReviewer").value,
      decision: $("rDecision").value,
      note: $("rNote").value,
    };
    const r = await api("POST", `/api/measurements/${STATE.current}/reviews`, body);
    flash(`审议已接受为版本 v${r.new_version}`, true);
    await refreshAll();
  } catch (e) { flash(e.message, false); }
}

async function simulateParallelConflict() {
  try {
    const current = STATE.reviews.length; // 最新版本号
    await api("POST", `/api/measurements/${STATE.current}/reviews`, {
      expected_version: Math.max(0, current - 1), // 故意用过时版本号
      reviewer: "parallel-agent",
      decision: "Rejected",
      note: "并行审议冲突演示",
    });
    flash("冲突未被拦截？请检查乐观锁", false);
  } catch (e) { flash("并行提交已被版本号拒绝：" + e.message, true); }
  await refreshAll();
}

function renderCalibrations() {
  $("calibrationList").innerHTML = `<table>
    <tr><th>ID</th><th>校准码</th><th>版本</th><th>文件</th><th>digest</th><th>状态</th><th>被替代</th></tr>
    ${STATE.calibrations.map((c) => `<tr><td>${c.id}</td><td>${esc(c.cal_code)}</td><td>v${c.version}</td>
      <td><code>${esc(c.file_name)}</code></td><td><code>${esc(c.digest.slice(0, 18))}…</code></td>
      <td>${c.active ? '<span class="pill ok">生效中</span>' : '<span class="tag">已失效</span>'}</td>
      <td>${c.superseded_by ?? "—"}</td></tr>`).join("")}</table>`;
}

async function publishCurrent() {
  try {
    const w = STATE.windows.filter((x) => x.candidate_id === STATE.candidates[0].id).slice(-1)[0];
    const r = await api("POST", `/api/measurements/${STATE.current}/publish`, {
      candidate_id: STATE.candidates[0].id,
      window_revision_id: w.id,
    });
    flash(`响应已发布 #${r.published_response_id}，冻结校准 digest：${r.calibration_digest_frozen.slice(0, 18)}…`, true);
    await refreshAll();
  } catch (e) { flash(e.message, false); }
}

async function replaceCalibration() {
  try {
    const r = await api("POST", "/api/calibrations/replace", { file_name: "cal_mic_v2.json" });
    flash(`校准换版完成：影响清单 ${r.impact_count} 项，仅登记不重算`, true);
    await refreshAll();
    if (r.impact_count) await loadImpact(r.change_id);
  } catch (e) { flash(e.message, false); }
}

async function loadImpact(changeId) {
  const items = await api("GET", `/api/calibration-changes/${changeId}/impact`);
  flash(`影响清单：${items.map((i) => `响应#${i.published_response_id}（测量 ${i.measurement_id}）→ ${i.status}`).join("；")}`, true);
}

function renderChanges() {
  $("changeList").innerHTML = STATE.changes.length ? `<table>
    <tr><th>事件</th><th>旧 digest</th><th>新 digest</th><th>影响清单</th><th>时间</th></tr>
    ${STATE.changes.map((c) => `<tr><td>#${c.id}</td>
      <td><code>${esc((c.old_digest ?? "无").slice(0, 16))}…</code></td>
      <td><code>${esc(c.new_digest.slice(0, 16))}…</code></td>
      <td>${c.impact_generated ? '<span class="pill warn">已生成（不重算）</span>' : "—"}</td>
      <td>${esc(c.created_at)}</td></tr>`).join("")}</table>`
    : "<p class='muted'>尚未发生校准换版</p>";
}

function renderPublished() {
  $("publishedList").innerHTML = STATE.published.length ? `<table>
    <tr><th>发布 ID</th><th>候选</th><th>窗口 revision</th><th>冻结校准 digest</th><th>响应 digest</th><th>状态</th></tr>
    ${STATE.published.map((p) => `<tr><td>#${p.id}</td><td>${p.candidate_id}</td><td>${p.window_revision_id}</td>
      <td><code>${esc(p.calibration_digest.slice(0, 18))}…</code></td>
      <td><code>${esc(p.response_digest.slice(0, 18))}…</code></td>
      <td>${p.superseded ? '<span class="tag">已标记过时</span>' : '<span class="pill ok">有效</span>'}</td></tr>`).join("")}
    </table>` : "<p class='muted'>本测量尚未发布响应</p>";
}

$("measSelect").addEventListener("change", refreshAll);
(async function init() {
  try { await loadMeasurements(); await refreshAll(); }
  catch (e) { flash("初始化失败：" + e.message, false); }
})();
