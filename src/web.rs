pub const INDEX_HTML: &str = r#"<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<title>网格修复证据台</title>
<style>
  body { font-family: -apple-system, "PingFang SC", "Microsoft YaHei", sans-serif; margin: 0; background: #f5f6f8; color: #222; }
  header { background: #1f3a5f; color: #fff; padding: 14px 22px; }
  header h1 { margin: 0; font-size: 20px; }
  header .sub { font-size: 12px; opacity: .8; margin-top: 4px; }
  main { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; padding: 12px; }
  section { background: #fff; border: 1px solid #ddd; border-radius: 6px; padding: 12px; }
  section h2 { margin: 0 0 8px; font-size: 15px; border-bottom: 1px solid #eee; padding-bottom: 6px; }
  .full { grid-column: 1 / -1; }
  button { background: #1f3a5f; color: #fff; border: 0; border-radius: 4px; padding: 5px 12px; cursor: pointer; font-size: 13px; }
  button:hover { background: #2c5282; }
  button.ghost { background: #e2e8f0; color: #222; }
  button.warn { background: #9b2c2c; }
  table { border-collapse: collapse; width: 100%; font-size: 12.5px; }
  th, td { border: 1px solid #e2e8f0; padding: 4px 7px; text-align: left; vertical-align: top; }
  th { background: #edf2f7; }
  tr.sel { background: #ebf5ff; }
  tr.clickable { cursor: pointer; }
  tr.clickable:hover { background: #f0f7ff; }
  pre { background: #f7fafc; border: 1px solid #e2e8f0; padding: 8px; font-size: 12px; overflow: auto; max-height: 260px; white-space: pre-wrap; }
  .tag { display: inline-block; padding: 1px 7px; border-radius: 9px; font-size: 11px; background: #e2e8f0; }
  .tag.err { background: #fed7d7; color: #9b2c2c; }
  .tag.ok { background: #c6f6d5; color: #276749; }
  .tag.warn { background: #feebc8; color: #9c4221; }
  .muted { color: #718096; font-size: 12px; }
  #toast { position: fixed; right: 16px; bottom: 16px; max-width: 420px; }
  .toast-item { background: #2d3748; color: #fff; padding: 9px 14px; border-radius: 5px; margin-top: 8px; font-size: 13px; }
  .toast-item.error { background: #9b2c2c; }
  .kv { font-size: 12.5px; }
  .kv b { display: inline-block; min-width: 110px; color: #4a5568; }
</style>
</head>
<body>
<header>
  <h1>网格修复证据台</h1>
  <div class="sub">非流形边 · 自交 · 洞 · 法向一致性复核 — 原始证据 → 当前结论 → 历史修订 → 未解冲突</div>
</header>
<main>
  <section class="full">
    <h2>1. 预置案例与检测</h2>
    <button onclick="loadPreset()">载入预置案例并检测</button>
    <span id="preset-info" class="muted"></span>
    <div id="mesh-info" class="kv" style="margin-top:8px"></div>
  </section>

  <section>
    <h2>2. 检测问题(原始证据)</h2>
    <div id="issues"><span class="muted">尚未检测</span></div>
  </section>

  <section>
    <h2>3. 证据明细(顶点 / 边 / 面 / 原字节位置)</h2>
    <div id="evidence"><span class="muted">点击左侧问题查看</span></div>
  </section>

  <section>
    <h2>4. 修复提案(候选)</h2>
    <div id="proposals"><span class="muted">选择问题后生成候选提案</span></div>
  </section>

  <section>
    <h2>5. 当前结论</h2>
    <div id="state"><span class="muted">—</span></div>
  </section>

  <section>
    <h2>6. 历史修订</h2>
    <div id="revisions"><span class="muted">—</span></div>
  </section>

  <section>
    <h2>7. 未解冲突(可追溯失败记录)</h2>
    <div id="conflicts"><span class="muted">—</span></div>
  </section>

  <section class="full">
    <h2>8. 导出产物(引用确定提案集,容差快照)</h2>
    <button onclick="doExport()">导出当前修订</button>
    <div id="exports" style="margin-top:8px"><span class="muted">—</span></div>
  </section>
</main>
<div id="toast"></div>
<script>
let MESH_ID = null, RUN_ID = null, CUR_SEQ = 0, CUR_ISSUE = null;

function toast(msg, isErr) {
  const d = document.createElement('div');
  d.className = 'toast-item' + (isErr ? ' error' : '');
  d.textContent = msg;
  document.getElementById('toast').appendChild(d);
  setTimeout(() => d.remove(), 6000);
}

async function api(method, path, body) {
  const opt = { method, headers: {} };
  if (body !== undefined) {
    opt.headers['Content-Type'] = 'application/json';
    opt.body = JSON.stringify(body);
  }
  const r = await fetch(path, opt);
  const j = await r.json().catch(() => ({}));
  if (!r.ok) {
    const e = j.error || { code: 'HTTP_' + r.status, message: r.statusText };
    const err = new Error(e.message); err.code = e.code; err.status = r.status;
    throw err;
  }
  return j;
}

const KIND_CN = {
  nonmanifold_edge: '非流形边', boundary_loop: '洞(边界环)', normal_inconsistent: '法向不一致',
  degenerate_face: '退化面', missing_face: '缺失面', normal_undeterminable: '法向无法确定',
  self_intersection: '自交', coincident_vertices: '容差内重合顶点'
};
const PROP_CN = {
  close_hole: '补片闭洞', split_nonmanifold_edge: '拆分非流形边', flip_orientation: '翻转局部定向',
  remove_face: '移除面', contract_vertices: '缩并顶点(保留来源映射)'
};

async function loadPreset() {
  try {
    const j = await api('POST', '/api/preset');
    MESH_ID = j.mesh_id; RUN_ID = j.run_id;
    document.getElementById('preset-info').textContent =
      ' 网格 #' + j.mesh_id + ' · 检测运行 #' + j.run_id;
    await refreshAll();
    toast('预置案例已载入并完成检测');
  } catch (e) { toast('载入失败: ' + e.message, true); }
}

async function refreshAll() {
  if (!MESH_ID) return;
  await Promise.all([refreshMesh(), refreshIssues(), refreshState(), refreshRevisions(), refreshConflicts(), refreshExports()]);
}

async function refreshMesh() {
  const m = await api('GET', '/api/meshes/' + MESH_ID);
  CUR_SEQ = m.seq;
  document.getElementById('mesh-info').innerHTML =
    '<div><b>digest</b> ' + m.digest + '</div>' +
    '<div><b>顶点 / 面</b> ' + m.vertex_count + ' / ' + m.face_count +
    ' · <b>当前版本 seq</b> ' + m.seq + '</div>';
}

async function refreshIssues() {
  const j = await api('GET', '/api/runs/' + RUN_ID);
  const tol = j.run.tolerance;
  let h = '<div class="muted">容差: precision=' + tol.precision + ', vertex_eps=' + tol.vertex_eps +
          ', degenerate_area_eps=' + tol.degenerate_area_eps + ' · 状态: ' + j.run.status + '</div>';
  h += '<table><tr><th>#</th><th>类型</th><th>涉及面</th><th>涉及顶点</th></tr>';
  for (const it of j.issues) {
    h += '<tr class="clickable" onclick="showIssue(' + it.id + ')"><td>' + it.id + '</td><td>' +
         (KIND_CN[it.kind] || it.kind) + '</td><td>[' + (it.evidence.faces || []).join(', ') +
         ']</td><td>[' + (it.evidence.vertices || []).join(', ') + ']</td></tr>';
  }
  h += '</table>';
  document.getElementById('issues').innerHTML = h;
}

async function showIssue(id) {
  CUR_ISSUE = id;
  const it = await api('GET', '/api/issues/' + id);
  const ev = it.evidence;
  let h = '<div class="kv"><b>问题 #' + it.id + '</b> ' + (KIND_CN[it.kind] || it.kind) + '</div>';
  h += '<div class="kv"><b>顶点</b> [' + (ev.vertices || []).join(', ') + ']</div>';
  h += '<div class="kv"><b>边</b> ' + (ev.edges || []).map(e => '(' + e[0] + ',' + e[1] + ')').join(' ') + '</div>';
  h += '<div class="kv"><b>面</b> [' + (ev.faces || []).join(', ') + ']</div>';
  h += '<div class="kv"><b>原字节位置</b></div>';
  for (const bp of (ev.byte_positions || [])) {
    const snip = await api('GET', '/api/meshes/' + MESH_ID + '/snippet?offset=' + bp.offset + '&len=' + bp.len);
    h += '<div class="kv">&nbsp;&nbsp;@' + bp.offset + ' (+' + bp.len + '): <code>' + escapeHtml(snip.text) + '</code></div>';
  }
  h += '<div class="kv"><b>细节</b></div><pre>' + escapeHtml(JSON.stringify(ev.detail, null, 2)) + '</pre>';
  h += '<button onclick="genCandidates()">生成候选提案</button>';
  document.getElementById('evidence').innerHTML = h;
  document.getElementById('proposals').innerHTML = '<span class="muted">点击"生成候选提案"</span>';
}

async function genCandidates() {
  if (!CUR_ISSUE) return;
  try {
    const j = await api('POST', '/api/issues/' + CUR_ISSUE + '/candidates');
    renderProposals(j.proposals);
    toast(j.generated ? '已生成 ' + j.proposals.length + ' 个候选' : '候选已存在(幂等返回)');
  } catch (e) { toast('生成失败: ' + e.message, true); }
}

function renderProposals(list) {
  if (!list.length) {
    document.getElementById('proposals').innerHTML = '<span class="muted">该问题类型无自动候选</span>';
    return;
  }
  let h = '<table><tr><th>#</th><th>类型</th><th>候选</th><th>状态</th><th>操作</th></tr>';
  for (const p of list) {
    const cand = p.payload.candidate || '';
    h += '<tr><td>' + p.id + '</td><td>' + (PROP_CN[p.kind] || p.kind) + '</td><td>' + cand +
         '</td><td><span class="tag ' + (p.status === 'accepted' ? 'ok' : p.status === 'conflicted' ? 'err' : '') + '">' +
         p.status + '</span></td><td>' +
         '<button class="ghost" onclick="previewProposal(' + p.id + ')">预览</button> ' +
         '<button onclick="acceptProposal(' + p.id + ')">接受</button></td></tr>';
  }
  h += '</table><div id="preview"></div>';
  document.getElementById('proposals').innerHTML = h;
}

async function previewProposal(id) {
  try {
    const p = await api('GET', '/api/proposals/' + id);
    const v = p.preview;
    let h = '<h3 style="font-size:13px">提案 #' + id + ' 预览差异</h3><table>' +
      '<tr><th></th><th>面数</th><th>边界边</th><th>非流形边</th><th>流形</th></tr>' +
      '<tr><td>修复前</td><td>' + v.before.face_count + '</td><td>' + v.before.boundary_edges +
      '</td><td>' + v.before.nonmanifold_edges + '</td><td>' + v.before.manifold + '</td></tr>' +
      '<tr><td>修复后</td><td>' + v.after.face_count + '</td><td>' + v.after.boundary_edges +
      '</td><td>' + v.after.nonmanifold_edges + '</td><td>' + v.after.manifold + '</td></tr>' +
      '<tr><td>差异</td><td>' + fmtDelta(v.delta_faces) + '</td><td>' + fmtDelta(v.delta_boundary_edges) +
      '</td><td>' + fmtDelta(v.delta_nonmanifold_edges) + '</td><td>' + v.manifold_change + '</td></tr></table>';
    document.getElementById('preview').innerHTML = h;
  } catch (e) { toast('预览失败: ' + e.message, true); }
}

async function acceptProposal(id) {
  try {
    const j = await api('POST', '/api/proposals/' + id + '/accept', { expected_seq: CUR_SEQ });
    toast('提案 #' + id + ' 已接受,新修订 seq=' + j.revision.seq);
    await refreshAll();
    if (CUR_ISSUE) {
      const ps = await api('GET', '/api/meshes/' + MESH_ID + '/proposals');
      renderProposals(ps.proposals.filter(p => p.issue_id === CUR_ISSUE));
    }
  } catch (e) {
    if (e.code === 'PROPOSAL_CONFLICT') {
      toast('冲突(已记录): ' + e.message, true);
    } else if (e.code === 'VERSION_CONFLICT') {
      toast('版本冲突: ' + e.message, true);
    } else {
      toast('接受失败: ' + e.message, true);
    }
    await refreshConflicts();
    if (CUR_ISSUE) {
      const ps = await api('GET', '/api/meshes/' + MESH_ID + '/proposals');
      renderProposals(ps.proposals.filter(p => p.issue_id === CUR_ISSUE));
    }
  }
}

async function refreshState() {
  const s = await api('GET', '/api/meshes/' + MESH_ID + '/state');
  CUR_SEQ = s.mesh.seq;
  const st = s.current_stats;
  let counts = Object.entries(s.issue_counts).map(([k, v]) =>
    '<span class="tag warn">' + (KIND_CN[k] || k) + ' × ' + v + '</span>').join(' ');
  document.getElementById('state').innerHTML =
    '<div class="kv"><b>版本 seq</b> ' + s.mesh.seq + '</div>' +
    '<div class="kv"><b>面数 / 边界边 / 非流形边</b> ' + st.face_count + ' / ' + st.boundary_edges + ' / ' + st.nonmanifold_edges + '</div>' +
    '<div class="kv"><b>流形</b> ' + st.manifold + '</div>' +
    '<div class="kv"><b>未解冲突</b> ' + s.unresolved_conflicts + '</div>' +
    '<div class="kv"><b>问题计数</b> ' + (counts || '—') + '</div>';
}

async function refreshRevisions() {
  const j = await api('GET', '/api/meshes/' + MESH_ID + '/revisions');
  if (!j.revisions.length) {
    document.getElementById('revisions').innerHTML = '<span class="muted">尚无修订</span>'; return;
  }
  let h = '<table><tr><th>seq</th><th>提案集</th><th>面数</th><th>边界边</th><th>非流形边</th><th>流形</th><th>时间</th></tr>';
  for (const r of j.revisions) {
    h += '<tr><td>' + r.seq + '</td><td>[' + r.proposal_ids.join(', ') + ']</td><td>' + r.stats.face_count +
         '</td><td>' + r.stats.boundary_edges + '</td><td>' + r.stats.nonmanifold_edges + '</td><td>' +
         r.stats.manifold + '</td><td>' + new Date(r.created_at).toLocaleString() + '</td></tr>';
  }
  h += '</table>';
  document.getElementById('revisions').innerHTML = h;
}

async function refreshConflicts() {
  const j = await api('GET', '/api/meshes/' + MESH_ID + '/conflicts');
  if (!j.conflicts.length) {
    document.getElementById('conflicts').innerHTML = '<span class="muted">无冲突</span>'; return;
  }
  let h = '<table><tr><th>#</th><th>提案</th><th>冲突对象</th><th>共享边界</th><th>原因</th><th>状态</th><th>时间</th><th></th></tr>';
  for (const c of j.conflicts) {
    h += '<tr><td>' + c.id + '</td><td>#' + c.proposal_id + '</td><td>#' + c.other_proposal_id + '</td><td>' +
         c.shared_edges.map(e => '(' + e[0] + ',' + e[1] + ')').join(' ') + '</td><td>' + c.reason +
         '</td><td><span class="tag ' + (c.status === 'unresolved' ? 'err' : 'ok') + '">' + c.status + '</span></td><td>' +
         new Date(c.created_at).toLocaleString() + '</td><td>' +
         (c.status === 'unresolved' ? '<button class="ghost" onclick="resolveConflict(' + c.id + ')">标记解决</button>' : '') +
         '</td></tr>';
  }
  h += '</table>';
  document.getElementById('conflicts').innerHTML = h;
}

async function resolveConflict(id) {
  try {
    await api('POST', '/api/conflicts/' + id + '/resolve');
    toast('冲突 #' + id + ' 已标记解决');
    await refreshConflicts(); await refreshState();
  } catch (e) { toast(e.message, true); }
}

async function doExport() {
  try {
    const j = await api('POST', '/api/meshes/' + MESH_ID + '/exports', { revision_seq: CUR_SEQ });
    toast('导出 #' + j.id + ' 完成,提案集哈希 ' + j.proposal_set_hash.slice(0, 12) + '…');
    await refreshExports();
  } catch (e) { toast('导出失败: ' + e.message, true); }
}

async function refreshExports() {
  const j = await api('GET', '/api/meshes/' + MESH_ID + '/exports');
  if (!j.exports.length) {
    document.getElementById('exports').innerHTML = '<span class="muted">尚无导出</span>'; return;
  }
  let h = '<table><tr><th>#</th><th>修订 seq</th><th>提案集哈希</th><th>时间</th></tr>';
  for (const e of j.exports) {
    h += '<tr class="clickable" onclick="showExport(' + e.id + ')"><td>' + e.id + '</td><td>' + e.revision_seq +
         '</td><td><code>' + e.proposal_set_hash.slice(0, 16) + '…</code></td><td>' +
         new Date(e.created_at).toLocaleString() + '</td></tr>';
  }
  h += '</table><div id="export-detail"></div>';
  document.getElementById('exports').innerHTML = h;
}

async function showExport(id) {
  const e = await api('GET', '/api/exports/' + id);
  document.getElementById('export-detail').innerHTML =
    '<pre>' + escapeHtml(JSON.stringify(e, null, 2)) + '</pre>';
}

function fmtDelta(d) { return (d > 0 ? '+' : '') + d; }
function escapeHtml(s) {
  return String(s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
}
</script>
</body>
</html>
"#;
