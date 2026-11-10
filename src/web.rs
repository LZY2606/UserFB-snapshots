//! 内嵌静态网页：单页应用，原生 JS + Canvas，无外部资源、无 CDN、无上传控件。
//! 页面展示“声场脉络”、反射标记、两版窗口证据、衰减曲线对比与校准影响清单。

pub fn index_html() -> &'static str {
    r#"<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>声场脉络 · 扫频脉冲响应可追溯分析</title>
<style>
  :root { --bg:#0f1420; --panel:#182033; --line:#2a3550; --fg:#e6ecf7; --mut:#8fa0bf;
          --accent:#5db0ff; --warn:#ffb454; --bad:#ff6b6b; --ok:#57d9a3; }
  * { box-sizing: border-box; }
  body { margin:0; background:var(--bg); color:var(--fg);
         font-family:"PingFang SC","Hiragino Sans GB","Microsoft YaHei",system-ui,sans-serif; }
  header { padding:18px 26px; border-bottom:1px solid var(--line);
           background:linear-gradient(120deg,#16233d,#101725); }
  header h1 { margin:0; font-size:24px; letter-spacing:2px; }
  header p { margin:6px 0 0; color:var(--mut); font-size:13px; }
  .wrap { padding:20px 26px; max-width:1280px; margin:0 auto; }
  .row { display:flex; gap:16px; flex-wrap:wrap; }
  .card { background:var(--panel); border:1px solid var(--line); border-radius:10px;
          padding:16px; margin-bottom:16px; flex:1 1 360px; min-width:320px; }
  .card h2 { font-size:16px; margin:0 0 12px; color:var(--accent); }
  .card h3 { font-size:14px; margin:14px 0 8px; color:var(--fg); }
  table { width:100%; border-collapse:collapse; font-size:12.5px; }
  th,td { text-align:left; padding:6px 8px; border-bottom:1px solid var(--line); }
  th { color:var(--mut); font-weight:600; }
  code { background:#0c1120; padding:1px 6px; border-radius:5px; color:#a8d3ff; }
  .pill { display:inline-block; padding:2px 9px; border-radius:999px; font-size:12px; }
  .pill.ok { background:rgba(87,217,163,.15); color:var(--ok); }
  .pill.warn { background:rgba(255,180,84,.15); color:var(--warn); }
  .pill.bad { background:rgba(255,107,107,.15); color:var(--bad); }
  button { background:var(--accent); color:#08111f; border:0; border-radius:7px;
           padding:8px 14px; font-size:13px; cursor:pointer; margin:4px 6px 4px 0; font-weight:600; }
  button.secondary { background:#2c3a57; color:var(--fg); }
  button:disabled { opacity:.5; cursor:not-allowed; }
  input,select,textarea { width:100%; background:#0c1120; color:var(--fg);
           border:1px solid var(--line); border-radius:7px; padding:8px; margin:4px 0 10px; font-size:13px; }
  label { font-size:12px; color:var(--mut); }
  canvas { width:100%; background:#0c1120; border-radius:8px; border:1px solid var(--line); }
  .muted { color:var(--mut); font-size:12px; }
  .grid2 { display:grid; grid-template-columns:1fr 1fr; gap:14px; }
  .tag { font-size:11px; padding:1px 7px; border-radius:5px; background:#24314d; color:var(--mut); }
  .rev-baseline { border-left:3px solid var(--mut); padding-left:10px; }
  .rev-new { border-left:3px solid var(--accent); padding-left:10px; }
  .flash { position:fixed; right:20px; bottom:20px; max-width:420px; padding:12px 16px;
           border-radius:9px; font-size:13px; display:none; z-index:9; }
  .flash.err { background:#3a1d24; color:#ffb3b3; border:1px solid var(--bad); display:block; }
  .flash.ok { background:#153026; color:#b6f0d8; border:1px solid var(--ok); display:block; }
  @media (max-width:860px){ .grid2{grid-template-columns:1fr;} }
</style>
</head>
<body>
<header>
  <h1>声场脉络</h1>
  <p>离线扫频测量 → 时钟核验 → 逆滤波脉冲响应 → 窗口 revision 审议 → 校准 digest 冻结发布。波形仅来自本地 fixture，运行时不上传。</p>
</header>
<div class="wrap">
  <div class="card">
    <h2>测量选择</h2>
    <div class="row">
      <select id="measSelect" style="max-width:520px"></select>
      <button class="secondary" onclick="refreshAll()">刷新</button>
    </div>
    <div id="summary"></div>
  </div>

  <div class="card">
    <h2>时钟与丢帧证据</h2>
    <div id="clockPanel"></div>
  </div>

  <div class="card">
    <h2>候选脉冲响应对比（两个正则强度同时保留）</h2>
    <div class="grid2" id="candidateCards"></div>
    <h3>能量衰减曲线对比（Schroeder 反向积分，闭区间逐点）</h3>
    <canvas id="decayCanvas" height="260"></canvas>
    <h3>反射到达时间标记与两版窗口证据</h3>
    <canvas id="timelineCanvas" height="200"></canvas>
    <p class="muted">红竖线＝主峰（直达声），橙竖线＝早期反射；括号线段＝窗口 revision，端点为闭区间 [start, end]。</p>
  </div>

  <div class="card">
    <h2>窗口 revision（只追加，不覆盖）</h2>
    <div id="windowList"></div>
    <h3>新建窗口 revision（闭区间，样本数 = end − start + 1）</h3>
    <div class="grid2">
      <div>
        <label>候选</label><select id="wCandidate"></select>
        <label>期望基线 expected_revision（并行并改冲突时拒绝）</label><input id="wExpected" value="-1">
        <label>作者</label><input id="wAuthor" value="reviewer-a">
      </div>
      <div>
        <label>start_sample（从 0 开始）</label><input id="wStart" value="190">
        <label>end_sample（闭区间，含该样本）</label><input id="wEnd" value="2190">
        <label>修订理由</label><textarea id="wRationale" rows="2">前移起点以纳入主峰前振铃，缩短末端避开噪声尾</textarea>
      </div>
    </div>
    <button onclick="submitWindow()">提交新 revision</button>
    <button class="secondary" onclick="demoWindow()">一键生成演示并改（基于最新 revision）</button>
  </div>

  <div class="card">
    <h2>并行审议（版本号乐观锁）</h2>
    <div id="reviewList"></div>
    <div class="grid2">
      <div>
        <label>expected_version</label><input id="rExpected" value="0">
        <label>审议人</label><input id="rReviewer" value="reviewer-b">
      </div>
      <div>
        <label>结论</label>
        <select id="rDecision"><option>Accepted</option><option>ChangeRequested</option><option>Rejected</option></select>
        <label>意见</label><textarea id="rNote" rows="2">两版窗口端点与衰减证据一致，接受第二版。</textarea>
      </div>
    </div>
    <button onclick="submitReview()">提交审议</button>
    <button class="secondary" onclick="simulateParallelConflict()">模拟并行冲突（旧版本号提交）</button>
  </div>

  <div class="card">
    <h2>校准链与发布冻结</h2>
    <div id="calibrationList"></div>
    <button onclick="publishCurrent()">发布当前测量响应（冻结当前校准 digest）</button>
    <button class="secondary" onclick="replaceCalibration()">替换为校准 v2 并生成影响清单（不静默重算）</button>
    <h3>换版事件与影响清单</h3>
    <div id="changeList"></div>
    <h3>本测量已发布响应</h3>
    <div id="publishedList"></div>
  </div>
</div>
<div id="flash" class="flash"></div>
<script src="/app.js"></script>
</body>
</html>
"#;
}

pub fn app_js() -> &'static str {
    include_str!("../static/app.js");
}
