use crate::mesh::{Face, Mesh, Vertex};
use crate::model::ApiError;
use rusqlite::{params, Connection};

pub struct Store {
    pub conn: Connection,
}

const SCHEMA: &str = r#"
PRAGMA journal_mode=WAL;
CREATE TABLE IF NOT EXISTS meshes(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  name TEXT NOT NULL,
  digest TEXT NOT NULL,
  format TEXT NOT NULL,
  raw TEXT NOT NULL,
  vertex_count INTEGER NOT NULL,
  face_count INTEGER NOT NULL,
  seq INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS vertices(
  mesh_id INTEGER NOT NULL, idx INTEGER NOT NULL,
  x REAL NOT NULL, y REAL NOT NULL, z REAL NOT NULL, valid INTEGER NOT NULL,
  byte_offset INTEGER NOT NULL, byte_len INTEGER NOT NULL,
  PRIMARY KEY(mesh_id, idx)
);
CREATE TABLE IF NOT EXISTS faces(
  mesh_id INTEGER NOT NULL, idx INTEGER NOT NULL,
  a INTEGER NOT NULL, b INTEGER NOT NULL, c INTEGER NOT NULL,
  byte_offset INTEGER NOT NULL, byte_len INTEGER NOT NULL,
  PRIMARY KEY(mesh_id, idx)
);
CREATE TABLE IF NOT EXISTS runs(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  mesh_id INTEGER NOT NULL,
  mesh_digest TEXT NOT NULL,
  precision INTEGER NOT NULL,
  vertex_eps REAL NOT NULL,
  degenerate_area_eps REAL NOT NULL,
  status TEXT NOT NULL,
  created_at INTEGER NOT NULL,
  completed_at INTEGER
);
CREATE TABLE IF NOT EXISTS issues(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  run_id INTEGER NOT NULL,
  seq INTEGER NOT NULL,
  kind TEXT NOT NULL,
  issue_key TEXT NOT NULL,
  evidence_json TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS proposals(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  issue_id INTEGER NOT NULL,
  mesh_id INTEGER NOT NULL,
  kind TEXT NOT NULL,
  payload_json TEXT NOT NULL,
  boundary_edges_json TEXT NOT NULL DEFAULT '[]',
  status TEXT NOT NULL DEFAULT 'draft',
  created_at INTEGER NOT NULL,
  decided_at INTEGER
);
CREATE TABLE IF NOT EXISTS revisions(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  mesh_id INTEGER NOT NULL,
  seq INTEGER NOT NULL,
  proposal_ids_json TEXT NOT NULL,
  stats_json TEXT NOT NULL,
  created_at INTEGER NOT NULL,
  UNIQUE(mesh_id, seq)
);
CREATE TABLE IF NOT EXISTS conflicts(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  mesh_id INTEGER NOT NULL,
  proposal_id INTEGER NOT NULL,
  other_proposal_id INTEGER NOT NULL,
  shared_edges_json TEXT NOT NULL,
  reason TEXT NOT NULL,
  status TEXT NOT NULL DEFAULT 'unresolved',
  created_at INTEGER NOT NULL,
  resolved_at INTEGER
);
CREATE TABLE IF NOT EXISTS exports(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  mesh_id INTEGER NOT NULL,
  revision_seq INTEGER NOT NULL,
  proposal_ids_json TEXT NOT NULL,
  proposal_set_hash TEXT NOT NULL,
  tol_json TEXT NOT NULL,
  stats_json TEXT NOT NULL,
  created_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS idempotency(
  key TEXT PRIMARY KEY,
  endpoint TEXT NOT NULL,
  request_hash TEXT NOT NULL,
  status INTEGER NOT NULL,
  response_json TEXT NOT NULL,
  created_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS vertex_sources(
  mesh_id INTEGER NOT NULL,
  proposal_id INTEGER NOT NULL,
  new_vertex_id INTEGER NOT NULL,
  source_vertex_id INTEGER NOT NULL
);
"#;

impl Store {
    pub fn open(path: &str) -> Result<Store, ApiError> {
        let conn = Connection::open(path)?;
        conn.execute_batch(SCHEMA)?;
        let store = Store { conn };
        Ok(store)
    }

    pub fn open_memory() -> Result<Store, ApiError> {
        let conn = Connection::open_in_memory()?;
        conn.execute_batch(SCHEMA)?;
        Ok(Store { conn })
    }

    /// 启动恢复:上次中断时遗留的 running 运行标记为 interrupted,可再恢复执行。
    pub fn recover_interrupted(&self) -> Result<u64, ApiError> {
        let n = self.conn.execute(
            "UPDATE runs SET status='interrupted' WHERE status='running'",
            [],
        )?;
        Ok(n as u64)
    }

    pub fn insert_mesh(
        &mut self,
        name: &str,
        digest: &str,
        format: &str,
        raw: &str,
        mesh: &Mesh,
        created_at: i64,
    ) -> Result<i64, ApiError> {
        let tx = self.conn.transaction()?;
        tx.execute(
            "INSERT INTO meshes(name,digest,format,raw,vertex_count,face_count,seq,created_at)
             VALUES(?1,?2,?3,?4,?5,?6,0,?7)",
            params![
                name,
                digest,
                format,
                raw,
                mesh.vertices.len() as i64,
                mesh.faces.len() as i64,
                created_at
            ],
        )?;
        let mesh_id = tx.last_insert_rowid();
        {
            let mut sv = tx.prepare(
                "INSERT INTO vertices(mesh_id,idx,x,y,z,valid,byte_offset,byte_len)
                 VALUES(?1,?2,?3,?4,?5,?6,?7,?8)",
            )?;
            for v in &mesh.vertices {
                let Vertex { idx, x, y, z, valid, byte_offset, byte_len } = v;
                // SQLite 不支持 NaN,无效坐标存 0 并以 valid 标记
                let (sx, sy, sz) = if *valid { (*x, *y, *z) } else { (0.0, 0.0, 0.0) };
                sv.execute(params![
                    mesh_id, idx, sx, sy, sz, *valid as i64, *byte_offset as i64, *byte_len as i64
                ])?;
            }
        }
        {
            let mut sf = tx.prepare(
                "INSERT INTO faces(mesh_id,idx,a,b,c,byte_offset,byte_len)
                 VALUES(?1,?2,?3,?4,?5,?6,?7)",
            )?;
            for f in &mesh.faces {
                let Face { idx, a, b, c, byte_offset, byte_len } = f;
                sf.execute(params![
                    mesh_id, idx, a, b, c, *byte_offset as i64, *byte_len as i64
                ])?;
            }
        }
        tx.commit()?;
        Ok(mesh_id)
    }

    pub fn load_mesh(&self, mesh_id: i64) -> Result<Mesh, ApiError> {
        let mut mesh = Mesh::default();
        {
            let mut sv = self.conn.prepare(
                "SELECT idx,x,y,z,valid,byte_offset,byte_len FROM vertices WHERE mesh_id=?1 ORDER BY idx",
            )?;
            let rows = sv.query_map(params![mesh_id], |r| {
                let valid: i64 = r.get(4)?;
                Ok(Vertex {
                    idx: r.get(0)?,
                    x: r.get(1)?,
                    y: r.get(2)?,
                    z: r.get(3)?,
                    valid: valid != 0,
                    byte_offset: r.get::<_, i64>(5)? as u64,
                    byte_len: r.get::<_, i64>(6)? as u64,
                })
            })?;
            for v in rows {
                mesh.vertices.push(v?);
            }
        }
        {
            let mut sf = self.conn.prepare(
                "SELECT idx,a,b,c,byte_offset,byte_len FROM faces WHERE mesh_id=?1 ORDER BY idx",
            )?;
            let rows = sf.query_map(params![mesh_id], |r| {
                Ok(Face {
                    idx: r.get(0)?,
                    a: r.get(1)?,
                    b: r.get(2)?,
                    c: r.get(3)?,
                    byte_offset: r.get::<_, i64>(4)? as u64,
                    byte_len: r.get::<_, i64>(5)? as u64,
                })
            })?;
            for f in rows {
                mesh.faces.push(f?);
            }
        }
        if mesh.vertices.is_empty() && mesh.faces.is_empty() {
            return Err(ApiError::not_found(format!("mesh {mesh_id} not found")));
        }
        Ok(mesh)
    }
}
