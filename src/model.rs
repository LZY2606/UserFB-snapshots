//! 领域模型：导入记录、票据谱系、规则版本、假设分支、失败诊断。
//!
//! 所有时间字段统一为 Unix 时间戳（秒）。采集到的时间戳是事实，
//! 时钟偏移只在 [`Diagnosis`] 里作为候选解释出现，绝不回写这些字段。

use serde::{Deserialize, Serialize};

/// 票据分层：TGT、跨 realm 转诊 TGT、应用服务票据。
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum TicketLayer {
    /// AS-REQ/AS-REP 得到的票据授予票据。
    Tgt,
    /// 跨 realm referral 过程中产生的转诊 TGT。
    ReferralTgt,
    /// TGS-REP 发给应用服务的服务票据。
    Service,
}

impl TicketLayer {
    pub fn as_str(self) -> &'static str {
        match self {
            TicketLayer::Tgt => "tgt",
            TicketLayer::ReferralTgt => "referral_tgt",
            TicketLayer::Service => "service",
        }
    }

    pub fn parse(s: &str) -> Option<Self> {
        match s {
            "tgt" => Some(TicketLayer::Tgt),
            "referral_tgt" => Some(TicketLayer::ReferralTgt),
            "service" => Some(TicketLayer::Service),
            _ => None,
        }
    }
}

/// 一条导入的 Kerberos 消息摘要（AS / TGS / 应用服务票据）。
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RecordInput {
    /// 消息来源类型：`as`、`tgs`、`ap`。
    pub source: String,
    pub realm: String,
    pub principal: String,
    /// 加密类型，例如 aes256-cts-hmac-sha1-96。
    pub etype: String,
    pub kvno: i64,
    /// 票据标志，例如 forwardable、renewable、ok-as-delegate。
    #[serde(default)]
    pub flags: Vec<String>,
    /// 认证时间（采集值，秒）。
    pub auth_time: i64,
    /// 生效时间（采集值，秒）。
    pub start_time: i64,
    /// 过期时间（采集值，秒）。
    pub end_time: i64,
    /// 续期截止时间（采集值，秒）；不可续期时为空。
    #[serde(default)]
    pub renew_till: Option<i64>,
    pub nonce: i64,
    /// transited 编码路径，元素为 realm 名称。
    #[serde(default)]
    pub transited: Vec<String>,
    /// AP-REQ authenticator 的指纹（十六进制 SHA-256）。
    #[serde(default)]
    pub authenticator_fp: Option<String>,
    /// 指回原始字节的引用（抓包文件/偏移/长度等），原样保存。
    #[serde(default)]
    pub raw_ref: String,
    /// 客户端观测到的时间（采集值）；与服务端时间比较以评估时钟偏移。
    #[serde(default)]
    pub client_observed_time: Option<i64>,
    /// 所属服务实例（replay cache 分区用），例如 host/web1@EAST.EXAMPLE。
    #[serde(default)]
    pub service_instance: Option<String>,
    /// 服务启动代次（replay cache 分区用）。
    #[serde(default)]
    pub boot_generation: Option<i64>,
    /// 谱系父记录 id：TGS 由哪张 TGT/referral TGT 推出；AS 为空。
    #[serde(default)]
    pub parent_id: Option<i64>,
    /// 该消息是否被观察到失败。
    #[serde(default)]
    pub failed: bool,
    /// 观察到的 Kerberos 错误码（如 KRB_AP_ERR_REPEAT=34）。
    #[serde(default)]
    pub error_code: Option<i64>,
}

/// 一条密钥代次记录。同一 kvno 下可以存在多把密钥（轮转中间态）。
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct KeyInput {
    pub realm: String,
    pub principal: String,
    pub etype: String,
    pub kvno: i64,
    /// 密钥指纹，不存密钥本体。
    pub fingerprint: String,
    /// 所属 keytab 标识。
    pub keytab: String,
    /// 该代次生效时间（采集/配置值）。
    pub valid_from: i64,
}

/// 一次服务重启事件，用来给 replay cache 划代次。
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct BootEvent {
    pub service_instance: String,
    pub generation: i64,
    pub started_at: i64,
}

/// 一版分析规则（假设）。结论永远指向生成它的那版规则，规则本身不可变。
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RulesetInput {
    pub name: String,
    /// 假设的信任路径，元素为 realm，按引用顺序排列。
    pub trust_path: Vec<String>,
    /// 假设使用的 keytab。
    pub keytab: String,
    /// 允许的时钟偏移（秒）。只用于生成候选解释。
    pub clock_skew_secs: i64,
}

/// 导入文件的整体结构。
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct ImportFile {
    #[serde(default)]
    pub records: Vec<RecordInput>,
    #[serde(default)]
    pub keys: Vec<KeyInput>,
    #[serde(default)]
    pub boots: Vec<BootEvent>,
}

/// 落库后的记录行。
#[derive(Debug, Clone, Serialize)]
pub struct Record {
    pub id: i64,
    pub source: String,
    pub layer: String,
    pub realm: String,
    pub principal: String,
    pub etype: String,
    pub kvno: i64,
    pub flags: Vec<String>,
    pub auth_time: i64,
    pub start_time: i64,
    pub end_time: i64,
    pub renew_till: Option<i64>,
    pub nonce: i64,
    pub transited: Vec<String>,
    pub authenticator_fp: Option<String>,
    pub raw_ref: String,
    pub client_observed_time: Option<i64>,
    pub service_instance: Option<String>,
    pub boot_generation: Option<i64>,
    pub parent_id: Option<i64>,
    pub failed: bool,
    pub error_code: Option<i64>,
}

/// 落库后的密钥代次行。
#[derive(Debug, Clone, Serialize)]
pub struct KeyRow {
    pub id: i64,
    pub realm: String,
    pub principal: String,
    pub etype: String,
    pub kvno: i64,
    pub fingerprint: String,
    pub keytab: String,
    pub valid_from: i64,
}

/// 失败原因分类。
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum FailureKind {
    /// replay cache 命中：同服务实例、同启动代次内 authenticator 指纹重复。
    Replay,
    /// 票据在服务端观测时间点已过期（或尚未生效）。
    TicketExpired,
    /// 带 renewable 标志且在 renew_till 内，但没有续期动作。
    RenewableNotRenewed,
    /// 候选密钥里没有 etype/kvno 能对上的。
    NoMatchingKey,
    /// 有候选但首选密钥指纹对不上（同一 kvno 多密钥的典型情形）。
    KeyMismatch,
    /// 信任路径与 transited 不一致。
    TrustPathViolation,
    /// referral 在信任路径内走成了环。
    ReferralLoop,
}

impl FailureKind {
    pub fn as_str(self) -> &'static str {
        match self {
            FailureKind::Replay => "replay",
            FailureKind::TicketExpired => "ticket_expired",
            FailureKind::RenewableNotRenewed => "renewable_not_renewed",
            FailureKind::NoMatchingKey => "no_matching_key",
            FailureKind::KeyMismatch => "key_mismatch",
            FailureKind::TrustPathViolation => "trust_path_violation",
            FailureKind::ReferralLoop => "referral_loop",
        }
    }
}

/// 一把候选密钥及其排序依据。
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CandidateKey {
    pub key_id: i64,
    pub kvno: i64,
    pub etype: String,
    pub fingerprint: String,
    pub keytab: String,
    pub valid_from: i64,
    /// 排序名次（0 为首选）。
    pub rank: usize,
    /// 排序理由。
    pub reason: String,
    /// 是否与票据 kvno/etype 精确匹配。
    pub exact: bool,
}

/// 单条判断依据。
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Evidence {
    pub code: String,
    pub detail: String,
}

/// 一次失败诊断结论，绑定到某个假设分支与规则版本。
#[derive(Debug, Clone, Serialize)]
pub struct Diagnosis {
    pub failure_id: i64,
    pub record_id: i64,
    pub branch_id: i64,
    pub ruleset_id: i64,
    pub kind: String,
    pub summary: String,
    pub evidence: Vec<Evidence>,
    pub candidates: Vec<CandidateKey>,
    /// 时钟偏移候选解释（秒）。为 None 表示数据不足；不改变任何采集时间。
    pub clock_skew_candidate_secs: Option<i64>,
    /// replay cache 分区键。
    pub replay_partition: Option<String>,
}
