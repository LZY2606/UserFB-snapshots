# 声场脉络 · acoustic-pulse

离线扫频测量的可追溯脉冲响应分析服务（Rust 服务端 + 内嵌网页 + SQLite）。
把**激励、录音、设备校准、分析窗口**组织成一条可审计的证据链：

- 波形只从本地 `fixtures/` 目录导入，**运行时没有任何上传接口**；
- 每次分析先核验采样率与通道时钟，再做扫频逆滤波得到候选响应；
- 保留主峰对齐、起止窗口、偏移校正、丢帧标记等全部中间证据；
- 窗口修改不覆盖旧参数，而是追加新 **revision**；两个正则强度的候选同时保留；
- 校准文件以 SHA-256 digest 固定，发布响应冻结当时有效校准版本；
  事后换版只生成**影响清单**，绝不静默重算；
- 并行审议与窗口并改用版本号乐观锁，冲突返回 `409` 拒绝覆盖；
- 衍生物有台账，进程恢复后不重复生成已提交产物。

## 零外部依赖（可完全离线构建）

工程不使用任何第三方 crate：

| 能力 | 实现方式 |
| --- | --- |
| 持久化 | FFI 直链系统 `libsqlite3`（见 `build.rs`、`src/db.rs`） |
| HTTP/1.1 | `std::net` 手写（`src/http.rs`） |
| JSON | 手写解析/序列化（`src/json.rs`） |
| SHA-256 | 手写 FIPS 180-4（`src/sha256.rs`，含 NIST 已知向量测试） |
| FFT/逆滤波 | 手写基-2 Cooley–Tukey（`src/fft.rs`、`src/dsp.rs`） |
| 前端 | 原生 JS + Canvas，内嵌二进制，无 CDN |

因此在装有 Rust 工具链与系统 libsqlite3 的机器上，无需联网即可构建。

## 构建与运行

```bash
cargo build --locked --all-targets
cargo test --locked
cargo run --locked -- --addr 127.0.0.1:5251
```

打开 <http://127.0.0.1:5251> 即可看到「声场脉络」、反射标记与两版窗口证据。

可选参数：`--db <sqlite 路径>`（默认 `data/acoustic-pulse.db`）、
`--fixtures <目录>`（默认 `fixtures`）。

## 窗口端点约定（全链路统一）

所有窗口端点使用**从 0 开始的闭区间 `[start, end]`**：

- 输入、SQLite 存储、API、页面、测试一律闭区间；
- 样本数恒为 `end - start + 1`（数据库有 `CHECK` 约束保证）；
- 仅当内部需要 Rust 切片时，经 `Window::half_open()` 换算为 `start .. end+1`；
- 不完整采样（长度不足帧长）与数值为零（`Zero`）是两类，绝不混淆。

## 演示数据（确定性，首次启动自动生成）

一段 0.5 s、80→6000 Hz 的对数正弦扫频，经过含**直达声（样本 200）+
两个早期反射（290、410）+ 指数噪声尾**的房间脉冲响应合成，并派生出：

| 测量 | 场景 | 预期证据 |
| --- | --- | --- |
| `M-CLEAN` | 标准短扫频 | 时钟 `Ok`，无丢帧，主峰=200，反射=290/410 |
| `M-DRIFT` | 录音时钟约 +500 ppm 漂移 | `clock_status=DriftSuspect`，ppm 超 ±200 阈值 |
| `M-DROP` | 样本 3000 处真实丢失 64 采样 | `drop_flags` 精确标出 3000，干净/漂移数据零误报 |

校准文件 `cal_mic_v1.json` / `cal_mic_v2.json` 内容不同、digest 不同。

## 主要 HTTP 接口

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/api/health` | 健康检查，明示 `upload_enabled=false` |
| GET | `/api/measurements` | 测量摘要列表 |
| GET | `/api/measurements/{id}` | 含时钟核验、丢帧、帧审计明细 |
| GET | `/api/measurements/{id}/candidates` | 两个逆滤波候选（衰减曲线/反射/digest） |
| GET | `/api/measurements/{id}/windows` | 窗口 revision，每条带回指基线端点 |
| POST | `/api/measurements/{id}/windows` | 追加窗口 revision（`expected_revision` 乐观锁） |
| GET/POST | `/api/measurements/{id}/reviews` | 审议列表/提交（`expected_version` 乐观锁） |
| POST | `/api/measurements/{id}/publish` | 发布并冻结当前校准 digest |
| GET | `/api/measurements/{id}/published` | 已发布响应（换版后原样保留） |
| GET | `/api/calibrations` | 校准链与生效状态 |
| POST | `/api/calibrations/replace` | 换版，返回影响清单，不重算 |
| GET | `/api/calibration-changes` / `.../{id}/impact` | 换版事件与影响清单 |

任何上传/写入波形的尝试都返回 `404`——服务端根本不存在这类路由。

## 测试矩阵

- `tests/dsp_contract.rs`：逆滤波、主峰对齐、**闭区间与半开切片端点换算**、
  Schroeder 衰减、漂移检测符号与量级、丢帧定位与零误报、零值/不完整分类；
- `tests/full_stack.rs`：fixture 导入、HTTP API、窗口并改返回双方端点与基线、
  409 乐观锁冲突、审议版本拒绝、校准 digest 换版影响清单、发布冻结、恢复幂等；
- 模块内单测：SHA-256 NIST 向量、FFT 配套、JSON 往返、窗口算术。

## 目录结构

```
src/
  main.rs       二进制入口（参数解析）
  lib.rs        库入口
  db.rs         libsqlite3 FFI 安全封装
  storage.rs    schema 迁移与乐观锁写入
  fixture.rs    本地波形容器(APWF)与确定性场景生成
  dsp.rs        扫频/逆滤波/对齐/衰减/漂移/丢帧
  fft.rs        基-2 FFT
  sha256.rs     摘要
  json.rs       JSON
  service.rs    业务编排与事务
  http.rs       HTTP 服务与路由
  web.rs        内嵌页面
static/app.js  前端逻辑（Canvas 曲线/反射/窗口证据）
tests/         集成与合同测试
```
