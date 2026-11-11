# kerbdiag — 离线 Kerberos 跨 realm 认证故障排查

域迁移期间，同一个 principal 走跨 realm referral 后会对应多个 kvno，抓包里又混着
客户端重试和服务重启前的 authenticator，人工对不上"哪次失败用的是哪一代密钥、哪一次会话"。
kerbdiag 把 AS / TGS / 应用服务票据的摘要导入本地 SQLite，建成票据谱系图，
在**不可变的规则版本**上给出带证据链的失败诊断；换假设时另开分支重推，旧结论原样保留。

全程离线：只监听回环地址，不发起任何外部网络连接，不依赖任何在线服务。

## 构建与运行

```bash
cargo build --locked
cargo test --locked
cargo run --locked -- --addr 127.0.0.1:5530
# 然后浏览器打开 http://127.0.0.1:5530
```

可选参数：`--db <路径>`（默认 `./kerbdiag.sqlite3`）。

## 数据模型

每条导入记录保留：realm、principal、etype、kvno、ticket flags、有效期
（auth/start/end/renew_till）、nonce、transited 路径、AP-REQ authenticator 指纹、
指回原始字节的引用（`raw_ref`，如抓包文件+偏移）、客户端观测时间、服务实例与启动代次。

票据按 `parent_id` 分层建图：

- `tgt`：AS-REQ/AS-REP 得到的 TGT；
- `referral_tgt`：跨 realm referral 过程中的 `krbtgt/...` 转诊票据；
- `service`：TGS-REP 发给应用服务的服务票据。

页面详情里的 **Ticket lineage** 沿父边从 TGT 根一路展示到当前票据，节点可点击跳转。

## 关键设计约束

1. **replay cache 分区键 = 服务实例 + 启动代次**（`host/web1@A.EXAMPLE#gen1`）。
   同一分区内 authenticator 指纹重复才判 replay；服务重启进入新代次后，旧指纹重现不算 replay，
   客户端重试也不豁免。
2. **时钟偏移只是候选解释**。`clock_skew_candidate_secs` 只在偏移落在规则阈值内时给出，
   采集到的时间字段（auth/start/end/观测时间）永远不会被回写或"校正"。
3. **规则版本内容寻址、不可变**。trust path、keytab、clock skew 组成 ruleset，按 SHA-256
   去重；分支引用某版 ruleset，诊断结论同时钉住 `branch_id` 与 `ruleset_id`。
   换假设另开分支重推，旧分支结论和它当时用的规则 hash 都保留。
4. **导入是单事务**。任何一行不合法（如悬空 `parent_id`）整批回滚，事实表保持原状。
5. **候选密钥确定性排序**：etype+kvno 精确匹配优先 → 仅 kvno 匹配 → 同主体相邻代次；
   同档内按 valid_from、指纹兜底。同一 kvno 存在多把不同指纹密钥时判 `key_mismatch`
   并给出 `ambiguous_key_generation` 证据。

## 失败结论类型

| kind | 含义 |
| --- | --- |
| `referral_loop` | referral 链成环（谱系节点或 transited realm 重复），未终止到服务票据 |
| `trust_path_violation` | transited realm 不符合该分支假设的信任路径 |
| `replay` | 同服务实例、同启动代次内 authenticator 指纹重复 |
| `ticket_expired` | 观测时间点票据已过期或尚未生效（采集时间不变） |
| `renewable_not_renewed` | 带 renewable 且在 renew_till 内，但没有续期动作 |
| `no_matching_key` | 所选 keytab 下没有该主体的任何密钥代次 |
| `key_mismatch` | 无精确匹配，或同一 kvno/etype 下密钥不唯一 |

## 内置四组样例

页面点"加载内置四组样例"（或 `POST /api/load-sample`）：

1. **循环 referral**：A→B→A，transited 中 realm 重复，referral 无法终止；
2. **同一 kvno 多把密钥**：kvno=7 轮转中间态存在两把指纹不同的密钥；
3. **服务重启后 authenticator 重现**：gen1 内重复判 replay，gen2 新分区不判；
4. **票据可续期但没续**：过期但仍在 renew_till 内，无续期请求。

加载后自动建立 `baseline` 分支（trust path A→B→C、`lab.keytab`、skew 300s）。
可在左侧另开分支，改用回滚 keytab `lab-rollback.keytab`、不同 trust path 或更大 skew，
对比两分支对同一失败事件的不同结论。

## HTTP 接口

- `GET /`：单页界面；
- `GET /api/state`：分支、规则、记录、密钥、重启事件、全部诊断；
- `GET /api/records/{id}`：记录详情 + Ticket lineage（祖先/后代）+ 各分支诊断；
- `POST /api/load-sample`：重置并加载内置样例；
- `POST /api/import`：导入 JSON（`{records, keys, boots}`），失败整体回滚；
- `GET /api/export`：导出全部事实数据，可在另一个空库重新导入；
- `POST /api/branches`：用新 ruleset 另开假设分支并立即重跑诊断。

## 测试

```bash
cargo test --locked
```

覆盖：referral 成环识别与健康链终止、票据时间窗口（含 skew 候选不回写事实）、
replay 分区键、候选密钥排序与同 kvno 歧义、事务回滚、导出再导入一致性、
分支结论与规则版本保留，以及 HTTP 端到端冒烟（环境禁止本地端口绑定时自动跳过）。
