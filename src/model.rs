use serde::{Deserialize, Serialize};

/// 容差与数值精度配置:检测运行时固定,导出产物保存其快照。
#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq)]
pub struct ToleranceConfig {
    /// 顶点坐标小数精度(用于量化比较)
    pub precision: u32,
    /// 顶点距离容差(含端点):距离 <= vertex_eps 视为坐标重合
    pub vertex_eps: f64,
    /// 退化面面积容差(含端点):面积 <= degenerate_area_eps 视为退化
    pub degenerate_area_eps: f64,
}

impl Default for ToleranceConfig {
    fn default() -> Self {
        ToleranceConfig {
            precision: 6,
            vertex_eps: 1e-3,
            degenerate_area_eps: 1e-9,
        }
    }
}

/// 面的三种异常状态:退化面、缺失面、法向无法确定。
#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum FaceState {
    Degenerate,
    Missing,
    NormalUndeterminable,
}

impl FaceState {
    pub fn as_str(&self) -> &'static str {
        match self {
            FaceState::Degenerate => "degenerate",
            FaceState::Missing => "missing",
            FaceState::NormalUndeterminable => "normal_undeterminable",
        }
    }
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum IssueKind {
    NonmanifoldEdge,
    BoundaryLoop,
    NormalInconsistent,
    DegenerateFace,
    MissingFace,
    NormalUndeterminable,
    SelfIntersection,
    CoincidentVertices,
}

impl IssueKind {
    pub fn as_str(&self) -> &'static str {
        match self {
            IssueKind::NonmanifoldEdge => "nonmanifold_edge",
            IssueKind::BoundaryLoop => "boundary_loop",
            IssueKind::NormalInconsistent => "normal_inconsistent",
            IssueKind::DegenerateFace => "degenerate_face",
            IssueKind::MissingFace => "missing_face",
            IssueKind::NormalUndeterminable => "normal_undeterminable",
            IssueKind::SelfIntersection => "self_intersection",
            IssueKind::CoincidentVertices => "coincident_vertices",
        }
    }
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum RunStatus {
    Running,
    Interrupted,
    Completed,
}

impl RunStatus {
    pub fn as_str(&self) -> &'static str {
        match self {
            RunStatus::Running => "running",
            RunStatus::Interrupted => "interrupted",
            RunStatus::Completed => "completed",
        }
    }
    pub fn parse(s: &str) -> Option<Self> {
        match s {
            "running" => Some(RunStatus::Running),
            "interrupted" => Some(RunStatus::Interrupted),
            "completed" => Some(RunStatus::Completed),
            _ => None,
        }
    }
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum ProposalStatus {
    Draft,
    Accepted,
    Rejected,
    Conflicted,
}

impl ProposalStatus {
    pub fn as_str(&self) -> &'static str {
        match self {
            ProposalStatus::Draft => "draft",
            ProposalStatus::Accepted => "accepted",
            ProposalStatus::Rejected => "rejected",
            ProposalStatus::Conflicted => "conflicted",
        }
    }
    pub fn parse(s: &str) -> Option<Self> {
        match s {
            "draft" => Some(ProposalStatus::Draft),
            "accepted" => Some(ProposalStatus::Accepted),
            "rejected" => Some(ProposalStatus::Rejected),
            "conflicted" => Some(ProposalStatus::Conflicted),
            _ => None,
        }
    }
}

/// 网格统计:用于预览差异与修订结论。
#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq, Default)]
pub struct MeshStats {
    pub face_count: i64,
    pub boundary_edges: i64,
    pub nonmanifold_edges: i64,
    pub manifold: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct StatsPreview {
    pub before: MeshStats,
    pub after: MeshStats,
    pub delta_faces: i64,
    pub delta_boundary_edges: i64,
    pub delta_nonmanifold_edges: i64,
    pub manifold_change: String,
}

/// API 错误,带稳定错误码。
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ApiError {
    pub code: String,
    pub message: String,
    pub status: u16,
}

impl ApiError {
    pub fn new(status: u16, code: &str, message: impl Into<String>) -> Self {
        ApiError {
            code: code.to_string(),
            message: message.into(),
            status,
        }
    }
    pub fn not_found(msg: impl Into<String>) -> Self {
        Self::new(404, "NOT_FOUND", msg)
    }
    pub fn bad_request(msg: impl Into<String>) -> Self {
        Self::new(400, "BAD_REQUEST", msg)
    }
    pub fn version_conflict(msg: impl Into<String>) -> Self {
        Self::new(409, "VERSION_CONFLICT", msg)
    }
    pub fn idempotency_conflict(msg: impl Into<String>) -> Self {
        Self::new(409, "IDEMPOTENCY_CONFLICT", msg)
    }
    pub fn conflict(msg: impl Into<String>) -> Self {
        Self::new(409, "PROPOSAL_CONFLICT", msg)
    }
    pub fn internal(msg: impl Into<String>) -> Self {
        Self::new(500, "INTERNAL", msg)
    }
}

impl From<rusqlite::Error> for ApiError {
    fn from(e: rusqlite::Error) -> Self {
        ApiError::internal(format!("sqlite: {e}"))
    }
}

impl From<serde_json::Error> for ApiError {
    fn from(e: serde_json::Error) -> Self {
        ApiError::internal(format!("json: {e}"))
    }
}
