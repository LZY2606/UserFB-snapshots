use crate::model::{FaceState, IssueKind, ToleranceConfig};
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;

/// 顶点:保留原始字节位置,id 为文件内 1-based 顺序。
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Vertex {
    pub idx: i64,
    pub x: f64,
    pub y: f64,
    pub z: f64,
    /// 坐标无法解析时为 false,法向无法确定
    pub valid: bool,
    pub byte_offset: u64,
    pub byte_len: u64,
}

/// 面:索引为文件内 1-based 顶点 id;引用缺失顶点时相应分量为 0 或越界值。
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Face {
    pub idx: i64,
    pub a: i64,
    pub b: i64,
    pub c: i64,
    pub byte_offset: u64,
    pub byte_len: u64,
}

#[derive(Debug, Clone, Default)]
pub struct Mesh {
    pub vertices: Vec<Vertex>,
    pub faces: Vec<Face>,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq, PartialOrd, Ord)]
pub struct ByteRange {
    pub offset: u64,
    pub len: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct IssueEvidence {
    pub vertices: Vec<i64>,
    pub edges: Vec<(i64, i64)>,
    pub faces: Vec<i64>,
    pub byte_positions: Vec<ByteRange>,
    pub detail: serde_json::Value,
}

#[derive(Debug, Clone)]
pub struct DetectedIssue {
    pub kind: IssueKind,
    pub evidence: IssueEvidence,
}

/// 解析文本网格:`v x y z` 与 `f a b c`,允许乱序(面可先于顶点出现)。
/// 坐标不可解析时顶点标记为 invalid;索引不足/越界的面记录为缺失。
pub fn parse_mesh(content: &str) -> Mesh {
    let mut mesh = Mesh::default();
    let mut offset: u64 = 0;
    for raw in content.split_inclusive('\n') {
        let line = raw.trim_end_matches(['\n', '\r']);
        let start = offset;
        offset += raw.len() as u64;
        let len = line.len() as u64;
        let toks: Vec<&str> = line.split_whitespace().collect();
        if toks.is_empty() || toks[0].starts_with('#') {
            continue;
        }
        match toks[0] {
            "v" => {
                let idx = mesh.vertices.len() as i64 + 1;
                let parse_c = |i: usize| -> Option<f64> { toks.get(i).and_then(|t| t.parse::<f64>().ok()).filter(|v| v.is_finite()) };
                let (x, y, z) = (parse_c(1), parse_c(2), parse_c(3));
                let valid = x.is_some() && y.is_some() && z.is_some();
                mesh.vertices.push(Vertex {
                    idx,
                    x: x.unwrap_or(f64::NAN),
                    y: y.unwrap_or(f64::NAN),
                    z: z.unwrap_or(f64::NAN),
                    valid,
                    byte_offset: start,
                    byte_len: len,
                });
            }
            "f" => {
                let idx = mesh.faces.len() as i64 + 1;
                let mut ids = [0i64; 3];
                for (i, slot) in ids.iter_mut().enumerate() {
                    *slot = toks.get(i + 1).and_then(|t| t.parse::<i64>().ok()).unwrap_or(0);
                }
                mesh.faces.push(Face {
                    idx,
                    a: ids[0],
                    b: ids[1],
                    c: ids[2],
                    byte_offset: start,
                    byte_len: len,
                });
            }
            _ => {}
        }
    }
    mesh
}

/// 按精度量化坐标。
pub fn quantize(c: f64, precision: u32) -> f64 {
    let m = 10f64.powi(precision as i32);
    (c * m).round() / m
}

impl Mesh {
    pub fn vertex(&self, idx: i64) -> Option<&Vertex> {
        if idx >= 1 && (idx as usize) <= self.vertices.len() {
            Some(&self.vertices[(idx - 1) as usize])
        } else {
            None
        }
    }

    /// 面的三种异常状态:缺失(引用不存在的顶点)、法向无法确定(坐标无效)、
    /// 退化(重复索引或量化面积 <= 容差,含端点)。
    pub fn face_state(&self, f: &Face, tol: &ToleranceConfig) -> Option<FaceState> {
        let (va, vb, vc) = (self.vertex(f.a), self.vertex(f.b), self.vertex(f.c));
        if va.is_none() || vb.is_none() || vc.is_none() {
            return Some(FaceState::Missing);
        }
        let (va, vb, vc) = (va.unwrap(), vb.unwrap(), vc.unwrap());
        if !va.valid || !vb.valid || !vc.valid {
            return Some(FaceState::NormalUndeterminable);
        }
        if f.a == f.b || f.b == f.c || f.a == f.c {
            return Some(FaceState::Degenerate);
        }
        let area = quantized_area(va, vb, vc, tol.precision);
        if area <= tol.degenerate_area_eps {
            return Some(FaceState::Degenerate);
        }
        None
    }

    fn qpoint(&self, idx: i64, precision: u32) -> Option<[f64; 3]> {
        let v = self.vertex(idx)?;
        if !v.valid {
            return None;
        }
        Some([
            quantize(v.x, precision),
            quantize(v.y, precision),
            quantize(v.z, precision),
        ])
    }
}

fn quantized_area(a: &Vertex, b: &Vertex, c: &Vertex, precision: u32) -> f64 {
    let pa = [
        quantize(a.x, precision),
        quantize(a.y, precision),
        quantize(a.z, precision),
    ];
    let pb = [
        quantize(b.x, precision),
        quantize(b.y, precision),
        quantize(b.z, precision),
    ];
    let pc = [
        quantize(c.x, precision),
        quantize(c.y, precision),
        quantize(c.z, precision),
    ];
    let u = [pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]];
    let v = [pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2]];
    let n = [
        u[1] * v[2] - u[2] * v[1],
        u[2] * v[0] - u[0] * v[2],
        u[0] * v[1] - u[1] * v[0],
    ];
    0.5 * (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]).sqrt()
}

/// 边关联:无向边 -> 各面中的有向出现 (face_idx, from, to)。
type EdgeMap = BTreeMap<(i64, i64), Vec<(i64, i64, i64)>>;

fn build_edges(mesh: &Mesh, tol: &ToleranceConfig) -> EdgeMap {
    let mut map: EdgeMap = BTreeMap::new();
    for f in &mesh.faces {
        // 缺失面无法定位,不参与拓扑
        if self_face_missing(mesh, f) {
            continue;
        }
        for (u, v) in [(f.a, f.b), (f.b, f.c), (f.c, f.a)] {
            let key = if u <= v { (u, v) } else { (v, u) };
            map.entry(key).or_default().push((f.idx, u, v));
        }
    }
    let _ = tol;
    map
}

fn self_face_missing(mesh: &Mesh, f: &Face) -> bool {
    mesh.vertex(f.a).is_none() || mesh.vertex(f.b).is_none() || mesh.vertex(f.c).is_none()
}

fn face_range(mesh: &Mesh, idx: i64) -> Option<ByteRange> {
    mesh.faces
        .iter()
        .find(|f| f.idx == idx)
        .map(|f| ByteRange { offset: f.byte_offset, len: f.byte_len })
}

fn vertex_range(mesh: &Mesh, idx: i64) -> Option<ByteRange> {
    mesh.vertex(idx)
        .map(|v| ByteRange { offset: v.byte_offset, len: v.byte_len })
}

fn collect_ranges(mesh: &Mesh, faces: &[i64], vertices: &[i64]) -> Vec<ByteRange> {
    let mut out: Vec<ByteRange> = Vec::new();
    for &f in faces {
        if let Some(r) = face_range(mesh, f) {
            out.push(r);
        }
    }
    for &v in vertices {
        if let Some(r) = vertex_range(mesh, v) {
            out.push(r);
        }
    }
    out.sort();
    out.dedup();
    out
}

/// 完整检测:结果仅由网格内容、精度与容差决定,可确定性重放。
pub fn detect(mesh: &Mesh, tol: &ToleranceConfig) -> Vec<DetectedIssue> {
    let mut issues = Vec::new();
    let edges = build_edges(mesh, tol);

    // 1. 面状态:退化 / 缺失 / 法向无法确定
    for f in &mesh.faces {
        let state = mesh.face_state(f, tol);
        let kind = match state {
            Some(FaceState::Degenerate) => IssueKind::DegenerateFace,
            Some(FaceState::Missing) => IssueKind::MissingFace,
            Some(FaceState::NormalUndeterminable) => IssueKind::NormalUndeterminable,
            None => continue,
        };
        let mut vertices = vec![f.a, f.b, f.c];
        vertices.retain(|&v| v > 0);
        issues.push(DetectedIssue {
            kind,
            evidence: IssueEvidence {
                vertices: vertices.clone(),
                edges: vec![],
                faces: vec![f.idx],
                byte_positions: collect_ranges(mesh, &[f.idx], &vertices),
                detail: serde_json::json!({
                    "face_state": state.unwrap().as_str(),
                    "referenced": [f.a, f.b, f.c],
                }),
            },
        });
    }

    // 2. 非流形边(关联面 > 2)
    for ((u, v), uses) in &edges {
        if uses.len() > 2 {
            let faces: Vec<i64> = uses.iter().map(|u| u.0).collect();
            issues.push(DetectedIssue {
                kind: IssueKind::NonmanifoldEdge,
                evidence: IssueEvidence {
                    vertices: vec![*u, *v],
                    edges: vec![(*u, *v)],
                    faces: faces.clone(),
                    byte_positions: collect_ranges(mesh, &faces, &[*u, *v]),
                    detail: serde_json::json!({
                        "edge": [u, v],
                        "incident_faces": faces,
                    }),
                },
            });
        }
    }

    // 3. 法向不一致:边恰被两个面共享且方向相同
    for ((u, v), uses) in &edges {
        if uses.len() == 2 {
            let (f1, from1, to1) = uses[0];
            let (f2, from2, to2) = uses[1];
            if from1 == from2 && to1 == to2 {
                issues.push(DetectedIssue {
                    kind: IssueKind::NormalInconsistent,
                    evidence: IssueEvidence {
                        vertices: vec![*u, *v],
                        edges: vec![(*u, *v)],
                        faces: vec![f1, f2],
                        byte_positions: collect_ranges(mesh, &[f1, f2], &[*u, *v]),
                        detail: serde_json::json!({
                            "edge": [u, v],
                            "same_direction": [from1, to1],
                            "faces": [f1, f2],
                        }),
                    },
                });
            }
        }
    }

    // 4. 边界环(洞)
    for loopv in boundary_loops(&edges) {
        let n = loopv.len();
        let mut bedges = Vec::new();
        for i in 0..n {
            let (a, b) = (loopv[i], loopv[(i + 1) % n]);
            bedges.push(if a <= b { (a, b) } else { (b, a) });
        }
        let mut faces: Vec<i64> = Vec::new();
        for e in &bedges {
            if let Some(uses) = edges.get(e) {
                for u in uses {
                    if !faces.contains(&u.0) {
                        faces.push(u.0);
                    }
                }
            }
        }
        faces.sort();
        issues.push(DetectedIssue {
            kind: IssueKind::BoundaryLoop,
            evidence: IssueEvidence {
                vertices: loopv.clone(),
                edges: bedges.clone(),
                faces: faces.clone(),
                byte_positions: collect_ranges(mesh, &faces, &loopv),
                detail: serde_json::json!({
                    "loop": loopv,
                    "boundary_edges": bedges,
                    "length": n,
                }),
            },
        });
    }

    // 5. 自交:状态正常、无公共顶点的三角形对
    let ok_faces: Vec<&Face> = mesh
        .faces
        .iter()
        .filter(|f| mesh.face_state(f, tol).is_none())
        .collect();
    for i in 0..ok_faces.len() {
        for j in (i + 1)..ok_faces.len() {
            let (fa, fb) = (ok_faces[i], ok_faces[j]);
            if [fa.a, fa.b, fa.c].iter().any(|v| [fb.a, fb.b, fb.c].contains(v)) {
                continue;
            }
            if triangles_intersect(mesh, fa, fb, tol.precision) {
                issues.push(DetectedIssue {
                    kind: IssueKind::SelfIntersection,
                    evidence: IssueEvidence {
                        vertices: vec![fa.a, fa.b, fa.c, fb.a, fb.b, fb.c],
                        edges: vec![],
                        faces: vec![fa.idx, fb.idx],
                        byte_positions: collect_ranges(mesh, &[fa.idx, fb.idx], &[]),
                        detail: serde_json::json!({
                            "face_pair": [fa.idx, fb.idx],
                        }),
                    },
                });
            }
        }
    }

    // 6. 容差内坐标重合的顶点组(不自动合并 id)
    for group in coincident_groups(mesh, tol) {
        issues.push(DetectedIssue {
            kind: IssueKind::CoincidentVertices,
            evidence: IssueEvidence {
                vertices: group.clone(),
                edges: vec![],
                faces: vec![],
                byte_positions: collect_ranges(mesh, &[], &group),
                detail: serde_json::json!({
                    "group": group,
                    "note": "坐标在容差内相等,顶点 id 不自动合并;缩并须保留来源映射",
                }),
            },
        });
    }

    issues
}

/// 边界边(仅被一个面引用)链接为有序顶点环。
fn boundary_loops(edges: &EdgeMap) -> Vec<Vec<i64>> {
    let mut adj: BTreeMap<i64, Vec<(i64, i64)>> = BTreeMap::new(); // vertex -> [(other, edge_key_hash)]
    let mut bedges: Vec<(i64, i64)> = Vec::new();
    for ((u, v), uses) in edges {
        if uses.len() == 1 {
            bedges.push((*u, *v));
            adj.entry(*u).or_default().push((*v, bedges.len() as i64 - 1));
            adj.entry(*v).or_default().push((*u, bedges.len() as i64 - 1));
        }
    }
    let mut used = vec![false; bedges.len()];
    let mut loops = Vec::new();
    for start in 0..bedges.len() {
        if used[start] {
            continue;
        }
        used[start] = true;
        let (a, b) = bedges[start];
        let mut loopv = vec![a, b];
        // 从 b 端延伸
        loop {
            let last = *loopv.last().unwrap();
            let prev = loopv[loopv.len() - 2];
            let next = adj.get(&last).and_then(|cands| {
                cands.iter().find(|(other, ei)| {
                    !used[*ei as usize] && (*other == loopv[0] || !loopv.contains(other)) && *other != prev
                })
            });
            match next {
                Some(&(other, ei)) => {
                    used[ei as usize] = true;
                    if other == loopv[0] {
                        break; // 闭环
                    }
                    loopv.push(other);
                }
                None => break, // 开链(非简单边界),保留已得序列
            }
        }
        if loopv.len() >= 3 {
            loops.push(loopv);
        }
    }
    loops
}

/// 容差内坐标重合的顶点分组(并查集,距离 <= vertex_eps 含端点)。
fn coincident_groups(mesh: &Mesh, tol: &ToleranceConfig) -> Vec<Vec<i64>> {
    let valid: Vec<&Vertex> = mesh.vertices.iter().filter(|v| v.valid).collect();
    let n = valid.len();
    let mut parent: Vec<usize> = (0..n).collect();
    fn find(p: &mut Vec<usize>, x: usize) -> usize {
        if p[x] != x {
            p[x] = find(p, p[x]);
        }
        p[x]
    }
    let eps2 = tol.vertex_eps * tol.vertex_eps;
    for i in 0..n {
        for j in (i + 1)..n {
            let (a, b) = (valid[i], valid[j]);
            let d2 = (a.x - b.x).powi(2) + (a.y - b.y).powi(2) + (a.z - b.z).powi(2);
            // 含端点:距离恰好等于容差也算重合
            if d2 <= eps2 + 1e-18 * eps2.max(1.0) {
                let (ri, rj) = (find(&mut parent, i), find(&mut parent, j));
                if ri != rj {
                    parent[ri] = rj;
                }
            }
        }
    }
    let mut groups: BTreeMap<usize, Vec<i64>> = BTreeMap::new();
    for (i, v) in valid.iter().enumerate() {
        let r = find(&mut parent, i);
        groups.entry(r).or_default().push(v.idx);
    }
    groups.into_values().filter(|g| g.len() >= 2).collect()
}

fn sub(a: [f64; 3], b: [f64; 3]) -> [f64; 3] {
    [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
}
fn cross(a: [f64; 3], b: [f64; 3]) -> [f64; 3] {
    [
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    ]
}
fn dot(a: [f64; 3], b: [f64; 3]) -> f64 {
    a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
}

/// 线段与三角形相交(Möller–Trumbore,t 取严格内部避免端点误报)。
fn seg_tri_intersect(p0: [f64; 3], p1: [f64; 3], a: [f64; 3], b: [f64; 3], c: [f64; 3]) -> bool {
    const TOL: f64 = 1e-9;
    let dir = sub(p1, p0);
    let e1 = sub(b, a);
    let e2 = sub(c, a);
    let h = cross(dir, e2);
    let det = dot(e1, h);
    if det.abs() < TOL {
        return false; // 平行或共面:不计为穿越
    }
    let inv = 1.0 / det;
    let s = sub(p0, a);
    let u = dot(s, h) * inv;
    if u < -TOL || u > 1.0 + TOL {
        return false;
    }
    let q = cross(s, e1);
    let v = dot(dir, q) * inv;
    if v < -TOL || u + v > 1.0 + TOL {
        return false;
    }
    let t = dot(e2, q) * inv;
    t > TOL && t < 1.0 - TOL
}

fn triangles_intersect(mesh: &Mesh, fa: &Face, fb: &Face, precision: u32) -> bool {
    let (pa, pb) = (
        [fa.a, fa.b, fa.c].map(|i| mesh.qpoint(i, precision)),
        [fb.a, fb.b, fb.c].map(|i| mesh.qpoint(i, precision)),
    );
    if pa.iter().any(|p| p.is_none()) || pb.iter().any(|p| p.is_none()) {
        return false;
    }
    let ta: Vec<[f64; 3]> = pa.iter().map(|p| p.unwrap()).collect();
    let tb: Vec<[f64; 3]> = pb.iter().map(|p| p.unwrap()).collect();
    for i in 0..3 {
        let (a0, a1) = (ta[i], ta[(i + 1) % 3]);
        if seg_tri_intersect(a0, a1, tb[0], tb[1], tb[2]) {
            return true;
        }
        let (b0, b1) = (tb[i], tb[(i + 1) % 3]);
        if seg_tri_intersect(b0, b1, ta[0], ta[1], ta[2]) {
            return true;
        }
    }
    false
}
