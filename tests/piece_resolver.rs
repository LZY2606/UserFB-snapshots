#![cfg(feature = "xml")]
//! Integration tests for the customizable parallel-piece loading entry point
//! (`Vtk::import_pieces` / `Vtk::import_pieces_with`).
//!
//! Every test builds its own VTK XML samples in a temporary directory and drives loading through
//! the public `Vtk::import` entry point. No Python, pygmsh or external generators are involved:
//! hand-written ASCII XML is used for the simple pieces, while the compressed piece is produced by
//! the library itself.

use std::cell::Cell;
use std::collections::HashMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::rc::Rc;
use std::sync::atomic::{AtomicU64, Ordering};

use vtkio::model::*;
use vtkio::piece_resolver::*;
use vtkio::xml::Compressor;

static TEMP_COUNTER: AtomicU64 = AtomicU64::new(0);

/// A uniquely named temporary directory that is removed when the test ends.
struct TempDir {
    path: PathBuf,
}

impl TempDir {
    fn new(tag: &str) -> TempDir {
        let path = std::env::temp_dir().join(format!(
            "vtkio_piece_resolver_{}_{}_{}",
            tag,
            std::process::id(),
            TEMP_COUNTER.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir_all(&path).expect("create temporary test directory");
        TempDir { path }
    }

    fn path(&self) -> &Path {
        &self.path
    }

    /// Write a UTF-8 file relative to the temporary directory.
    fn write(&self, name: &str, contents: &str) -> PathBuf {
        let full = self.path.join(name);
        if let Some(parent) = full.parent() {
            fs::create_dir_all(parent).expect("create parent directories of test file");
        }
        fs::write(&full, contents).expect("write test file");
        full
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.path);
    }
}

/// A resolver delegating to the safe local resolver while recording every URI it was asked for.
#[derive(Clone)]
struct CountingResolver {
    inner: LocalPieceResolver,
    calls: Rc<std::cell::RefCell<Vec<String>>>,
}

impl CountingResolver {
    fn new(root: impl Into<PathBuf>) -> Self {
        CountingResolver {
            inner: LocalPieceResolver::new(root),
            calls: Rc::new(std::cell::RefCell::new(Vec::new())),
        }
    }

    fn call_count(&self) -> usize {
        self.calls.borrow().len()
    }

    fn called_uris(&self) -> Vec<String> {
        self.calls.borrow().clone()
    }
}

impl PieceResolver for CountingResolver {
    fn resolve(&self, request: &PieceRequest<'_>) -> Result<ResolvedPiece, LoadError> {
        self.calls.borrow_mut().push(request.uri.to_owned());
        self.inner.resolve(request)
    }
}

/// A fully in-memory resolver: piece bytes never touch the file system.
struct InMemoryResolver {
    files: HashMap<String, Vec<u8>>,
    calls: Cell<usize>,
}

impl InMemoryResolver {
    fn new(files: HashMap<String, Vec<u8>>) -> Self {
        InMemoryResolver {
            files,
            calls: Cell::new(0),
        }
    }

    fn call_count(&self) -> usize {
        self.calls.get()
    }
}

impl PieceResolver for InMemoryResolver {
    fn resolve(&self, request: &PieceRequest<'_>) -> Result<ResolvedPiece, LoadError> {
        self.calls.set(self.calls.get() + 1);
        match self.files.get(request.uri) {
            Some(bytes) => Ok(ResolvedPiece::new(request.uri, bytes.clone())),
            None => Err(LoadError::new(
                format!("in-memory resolver has no piece {:?}", request.uri),
                LoadReport::default(),
            )),
        }
    }
}

/// A minimal ASCII `.vtu` with one tetrahedron, a point scalar `pressure` (one component) and a
/// cell scalar `cell_id`. `pressure` holds one value per point and lets tests identify the piece.
fn tetra_vtu(pressure: &[f32], cell_id: i32, num_components: u32, scalar_type: &str) -> String {
    let point_values = pressure
        .iter()
        .map(|x| x.to_string())
        .collect::<Vec<_>>()
        .join(" ");
    let per_point = 4 * num_components as usize;
    assert_eq!(
        pressure.len(),
        per_point,
        "test sample must declare one value per component for its four points"
    );
    let _ = per_point;
    format!(
        "\
<VTKFile type=\"UnstructuredGrid\" version=\"1.0\" byte_order=\"LittleEndian\" header_type=\"UInt64\">
  <UnstructuredGrid>
    <Piece NumberOfPoints=\"4\" NumberOfCells=\"1\">
      <PointData Scalars=\"pressure\">
        <DataArray type=\"{scalar_type}\" Name=\"pressure\" NumberOfComponents=\"{num_components}\" format=\"ascii\">
          {point_values}
        </DataArray>
      </PointData>
      <CellData Scalars=\"cell_id\">
        <DataArray type=\"Int32\" Name=\"cell_id\" format=\"ascii\">
          {cell_id}
        </DataArray>
      </CellData>
      <Points>
        <DataArray type=\"Float32\" Name=\"Points\" NumberOfComponents=\"3\" format=\"ascii\">
          0 0 0 1 0 0 0 1 0 0 0 1
        </DataArray>
      </Points>
      <Cells>
        <DataArray type=\"Int64\" Name=\"connectivity\" format=\"ascii\">0 1 2 3</DataArray>
        <DataArray type=\"Int64\" Name=\"offsets\" format=\"ascii\">4</DataArray>
        <DataArray type=\"UInt8\" Name=\"types\" format=\"ascii\">10</DataArray>
      </Cells>
    </Piece>
  </UnstructuredGrid>
</VTKFile>"
    )
}

/// Parallel `.pvtu` referencing the given sources in order.
fn pvtu(sources: &[&str]) -> String {
    let pieces = sources
        .iter()
        .map(|s| format!("    <Piece Source=\"{}\"/>", s))
        .collect::<Vec<_>>()
        .join("\n");
    format!(
        "\
<VTKFile type=\"PUnstructuredGrid\" version=\"1.0\" byte_order=\"LittleEndian\" header_type=\"UInt64\">
  <PUnstructuredGrid GhostLevel=\"0\">
    <PPointData>
      <PDataArray type=\"Float32\" Name=\"pressure\"/>
    </PPointData>
    <PCellData>
      <PDataArray type=\"Int32\" Name=\"cell_id\"/>
    </PCellData>
    <PPoints>
      <PDataArray type=\"Float32\" Name=\"Points\" NumberOfComponents=\"3\"/>
    </PPoints>
{pieces}
  </PUnstructuredGrid>
</VTKFile>"
    )
}

/// Extract the inline unstructured-grid pieces of a loaded model.
fn inline_unstructured_pieces(vtk: &Vtk) -> Vec<&UnstructuredGridPiece> {
    match &vtk.data {
        DataSet::UnstructuredGrid { pieces, meta, .. } => {
            assert!(
                meta.is_none(),
                "parallel metadata must be cleared after merge"
            );
            pieces
                .iter()
                .map(|piece| match piece {
                    Piece::Inline(p) => p.as_ref(),
                    other => panic!("expected merged piece to be Inline, got {:?}", other),
                })
                .collect()
        }
        other => panic!("expected UnstructuredGrid dataset, got {:?}", other),
    }
}

fn point_pressure(piece: &UnstructuredGridPiece) -> Vec<f32> {
    let attr = piece
        .data
        .point
        .iter()
        .find(|a| a.name() == "pressure")
        .expect("merged piece must keep its pressure attribute");
    match attr {
        Attribute::DataArray(array) => array
            .data
            .clone()
            .cast_into::<f32>()
            .expect("pressure must be f32 in the baseline pieces"),
        other => panic!("expected DataArray attribute, got {:?}", other),
    }
}

/// Duplicate references are parsed once, reported twice and merged in declaration order.
#[test]
fn duplicate_reference_is_loaded_once_and_kept_in_order() {
    let dir = TempDir::new("duplicate");
    dir.write("a.vtu", &tetra_vtu(&[1.0, 1.0, 1.0, 1.0], 11, 1, "Float32"));
    dir.write("b.vtu", &tetra_vtu(&[2.0, 2.0, 2.0, 2.0], 22, 1, "Float32"));
    // `a.vtu` is declared twice and `b.vtu` once, deliberately out of alphabetical order.
    let root = dir.write("root.pvtu", &pvtu(&["a.vtu", "b.vtu", "a.vtu"]));

    let resolver = CountingResolver::new(dir.path().to_path_buf());
    let mut vtk = Vtk::import(&root).expect("import parallel root file");
    let report = vtk
        .import_pieces_with(&resolver)
        .expect("loading duplicated pieces must succeed");

    assert_eq!(
        resolver.call_count(),
        2,
        "a.vtu is referenced twice but must be resolved exactly once; b.vtu once -> 2 resolver calls, got {} ({:?})",
        resolver.call_count(),
        resolver.called_uris()
    );
    assert_eq!(report.len(), 3, "every reference gets its own report entry");
    assert_eq!(
        report.resolver_load_count(),
        2,
        "two distinct files must be physically loaded"
    );
    assert_eq!(
        report.cache_hit_count(),
        1,
        "the second reference to a.vtu must be recorded as a cache hit"
    );
    assert_eq!(
        report.merged_count(),
        3,
        "all three references must be merged into the resulting model"
    );

    let origins: Vec<_> = report.pieces.iter().map(|p| &p.origin).collect();
    assert!(matches!(origins[0], Some(PieceOrigin::Loaded { .. })));
    assert!(matches!(origins[1], Some(PieceOrigin::Loaded { .. })));
    assert!(
        matches!(origins[2], Some(PieceOrigin::Cached { .. })),
        "third entry (duplicate a.vtu) must be a cache hit, got {:?}",
        origins[2]
    );
    assert_eq!(
        report.pieces[0].resolved_uri, report.pieces[2].resolved_uri,
        "duplicate references must normalize to the same resolved URI"
    );
    assert_eq!(
        report.pieces[0].source_uri, report.pieces[2].source_uri,
        "the raw source URI must be preserved on both report entries"
    );
    for entry in &report.pieces {
        assert_eq!(entry.num_points, Some(4), "tetrahedron piece has 4 points");
        assert_eq!(entry.num_cells, Some(1), "tetrahedron piece has 1 cell");
        assert_eq!(entry.status, PieceStatus::Merged);
    }

    let pieces = inline_unstructured_pieces(&vtk);
    assert_eq!(
        pieces.len(),
        3,
        "merged model keeps one piece per reference"
    );
    assert_eq!(point_pressure(pieces[0]), vec![1.0; 4]);
    assert_eq!(point_pressure(pieces[1]), vec![2.0; 4]);
    assert_eq!(
        point_pressure(pieces[2]),
        vec![1.0; 4],
        "pieces must follow declaration order a, b, a (not de-duplicated away)"
    );
}

/// Two nested parallel branches referencing the same normalized piece only parse it once.
#[test]
fn two_branches_sharing_one_piece_parse_it_once() {
    let dir = TempDir::new("shared_branch");
    dir.write(
        "shared.vtu",
        &tetra_vtu(&[7.0, 7.0, 7.0, 7.0], 77, 1, "Float32"),
    );
    // Both branches live in different directories and use different relative spellings which
    // normalize to the same physical file.
    dir.write("branch_a.pvtu", &pvtu(&["shared.vtu"]));
    dir.write("nested/branch_b.pvtu", &pvtu(&["../shared.vtu"]));
    let root = dir.write(
        "root.pvtu",
        &pvtu(&["branch_a.pvtu", "nested/branch_b.pvtu"]),
    );

    let resolver = CountingResolver::new(dir.path().to_path_buf());
    let mut vtk = Vtk::import(&root).expect("import root parallel file");
    let report = vtk
        .import_pieces_with(&resolver)
        .expect("nested parallel loading must succeed");

    let called = resolver.called_uris();
    assert_eq!(
        called.iter().filter(|u| u.ends_with("shared.vtu")).count(),
        1,
        "shared.vtu is referenced from both branches but must be resolved exactly once; resolver saw {:?}",
        called
    );
    assert_eq!(
        called.len(),
        3,
        "resolver must see branch_a, branch_b and shared exactly once each; got {:?}",
        called
    );

    let shared_entries: Vec<_> = report
        .pieces
        .iter()
        .filter(|p| p.source_uri.ends_with("shared.vtu"))
        .collect();
    assert_eq!(
        shared_entries.len(),
        2,
        "both branch references must remain visible in the report"
    );
    assert_eq!(
        shared_entries[0].resolved_uri, shared_entries[1].resolved_uri,
        "the two relative spellings must normalize to the same resolved URI"
    );
    assert!(matches!(
        shared_entries[0].origin,
        Some(PieceOrigin::Loaded { .. })
    ));
    assert!(
        matches!(shared_entries[1].origin, Some(PieceOrigin::Cached { .. })),
        "second branch must be served from cache, got {:?}",
        shared_entries[1].origin
    );
    assert_ne!(
        shared_entries[0].reference_chain, shared_entries[1].reference_chain,
        "the report must show which branch each reference came from"
    );

    let pieces = inline_unstructured_pieces(&vtk);
    assert_eq!(pieces.len(), 2, "both branches contribute the shared piece");
    assert_eq!(point_pressure(pieces[0]), vec![7.0; 4]);
    assert_eq!(point_pressure(pieces[1]), vec![7.0; 4]);
}

/// A reference loop fails with the full chain and leaves the root model untouched.
#[test]
fn cyclic_reference_reports_full_chain_and_keeps_model_intact() {
    let dir = TempDir::new("cycle");
    dir.write("a.pvtu", &pvtu(&["b.pvtu"]));
    dir.write("b.pvtu", &pvtu(&["a.pvtu"]));
    // A healthy second piece proves that references past the failing one stay pending.
    dir.write("c.vtu", &tetra_vtu(&[3.0, 3.0, 3.0, 3.0], 33, 1, "Float32"));
    let root = dir.write("root.pvtu", &pvtu(&["a.pvtu", "c.vtu"]));

    let resolver = CountingResolver::new(dir.path().to_path_buf());
    let mut vtk = Vtk::import(&root).expect("import root parallel file");
    let before = vtk.clone();

    let err = vtk
        .import_pieces_with(&resolver)
        .expect_err("a reference cycle must fail the whole load");

    assert!(
        err.message.contains("cyclic"),
        "error must identify a cyclic reference, got: {}",
        err.message
    );
    assert!(
        err.message.contains("a.pvtu") && err.message.contains("b.pvtu"),
        "error must carry the full reference chain, got: {}",
        err.message
    );
    let failed = err
        .report
        .failed_piece()
        .expect("the report must identify the failing piece");
    assert!(
        matches!(failed.status, PieceStatus::Failed { .. }),
        "failing entry must be marked Failed"
    );
    assert!(
        err.report
            .pieces
            .iter()
            .any(|p| p.status == PieceStatus::Pending),
        "references not reached before the failure must remain Pending: {:?}",
        err.report.pieces
    );
    assert_eq!(
        vtk, before,
        "a failed load must not return a half-merged Vtk model"
    );
}

/// A missing piece fails cleanly, and a fixed input reloads from disk rather than stale cache.
#[test]
fn missing_piece_fails_then_reloads_after_fix() {
    let dir = TempDir::new("missing");
    dir.write(
        "ok.vtu",
        &tetra_vtu(&[4.0, 4.0, 4.0, 4.0], 44, 1, "Float32"),
    );
    let root = dir.write("root.pvtu", &pvtu(&["missing.vtu", "ok.vtu"]));

    let resolver = CountingResolver::new(dir.path().to_path_buf());
    let mut vtk = Vtk::import(&root).expect("import root parallel file");
    let before = vtk.clone();
    let err = vtk
        .import_pieces_with(&resolver)
        .expect_err("loading a missing piece must fail");

    let failed = err
        .report
        .failed_piece()
        .expect("report must identify the missing piece");
    assert_eq!(failed.source_uri, "missing.vtu");
    assert_eq!(
        err.report.pending_pieces().len(),
        1,
        "the piece declared after the failure must still be pending"
    );
    assert_eq!(
        err.report.pending_pieces()[0].source_uri,
        "ok.vtu",
        "pending report must name the unprocessed piece"
    );
    assert_eq!(vtk, before, "failed load must not mutate the Vtk model");
    assert_eq!(
        resolver.call_count(),
        1,
        "only the missing piece was attempted before the failure; ok.vtu must not be read"
    );

    // Fix the input and retry through the SAME resolver instance. Because the cache lives in the
    // per-load driver rather than the resolver, the failed attempt must not leave any state that
    // short-circuits the retry.
    dir.write(
        "missing.vtu",
        &tetra_vtu(&[5.0, 5.0, 5.0, 5.0], 55, 1, "Float32"),
    );
    let mut vtk2 = Vtk::import(&root).expect("re-import fixed root file");
    let report = vtk2
        .import_pieces_with(&resolver)
        .expect("fixed input must now load");
    assert_eq!(
        resolver.call_count(),
        3,
        "retry re-reads every piece from disk with no cross-load cache; expected 3 total resolver calls (1 failed + 2), got {}",
        resolver.call_count()
    );
    assert_eq!(report.merged_count(), 2);
    let pieces = inline_unstructured_pieces(&vtk2);
    assert_eq!(point_pressure(pieces[0]), vec![5.0; 4]);
    assert_eq!(point_pressure(pieces[1]), vec![4.0; 4]);
}

/// An attribute component-count mismatch aborts before merging and names the offending piece.
#[test]
fn attribute_schema_conflict_aborts_before_merge() {
    let dir = TempDir::new("schema_conflict");
    dir.write(
        "good.vtu",
        &tetra_vtu(&[1.0, 1.0, 1.0, 1.0], 1, 1, "Float32"),
    );
    // Same attribute name and scalar type, but two components instead of one.
    dir.write(
        "bad.vtu",
        &tetra_vtu(&[1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0], 2, 2, "Float32"),
    );
    let root = dir.write("root.pvtu", &pvtu(&["good.vtu", "bad.vtu"]));

    let resolver = CountingResolver::new(dir.path().to_path_buf());
    let mut vtk = Vtk::import(&root).expect("import root parallel file");
    let before = vtk.clone();
    let err = vtk
        .import_pieces_with(&resolver)
        .expect_err("schema mismatch must fail the load");

    assert!(
        err.message.contains("schema"),
        "error must describe the schema mismatch, got: {}",
        err.message
    );
    let failed = err.report.failed_piece().expect("failing piece in report");
    assert_eq!(failed.index, 1, "the second piece must be the failing one");
    assert_eq!(failed.source_uri, "bad.vtu");
    assert_eq!(
        err.report.processed_pieces().len(),
        1,
        "the first piece must show as processed before the failure"
    );
    assert_eq!(err.report.processed_pieces()[0].source_uri, "good.vtu");
    assert_eq!(
        err.report.merged_count(),
        1,
        "only the successfully validated piece is recorded as merged in the report"
    );
    assert_eq!(vtk, before, "no partial merge must leak into the Vtk model");
}

/// Pieces using different compression strategies still merge when their schemas match.
#[cfg(feature = "flate2")]
#[test]
fn pieces_with_different_compression_merge() {
    let dir = TempDir::new("compression");
    // Piece one: plain ASCII, no compression, written by hand.
    dir.write(
        "ascii.vtu",
        &tetra_vtu(&[8.0, 8.0, 8.0, 8.0], 88, 1, "Float32"),
    );

    // Piece two: ZLib compressed binary, generated by the library itself.
    let compressed_vtk = Vtk {
        version: Version::new_xml(1, 0),
        title: String::new(),
        byte_order: ByteOrder::LittleEndian,
        file_path: None,
        data: DataSet::inline(UnstructuredGridPiece {
            points: IOBuffer::F32(vec![
                0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0,
            ]),
            cells: Cells {
                cell_verts: VertexNumbers::XML {
                    connectivity: vec![0, 1, 2, 3],
                    offsets: vec![4],
                },
                types: vec![CellType::Tetra],
            },
            data: Attributes {
                point: vec![Attribute::DataArray(DataArray {
                    name: "pressure".into(),
                    elem: ElementType::Scalars {
                        num_comp: 1,
                        lookup_table: None,
                    },
                    data: IOBuffer::F32(vec![9.0, 9.0, 9.0, 9.0]),
                })],
                cell: vec![Attribute::DataArray(DataArray {
                    name: "cell_id".into(),
                    elem: ElementType::Scalars {
                        num_comp: 1,
                        lookup_table: None,
                    },
                    data: IOBuffer::I32(vec![99]),
                })],
            },
        }),
    };
    let xml_file = compressed_vtk
        .try_into_xml_format(Compressor::ZLib, 6)
        .expect("build zlib compressed vtu");
    let mut bytes = Vec::new();
    xml_file
        .write(&mut bytes)
        .expect("serialize compressed vtu");
    let compressed_path = dir.path.join("zlib.vtu");
    fs::write(&compressed_path, bytes).expect("write compressed piece");
    // Sanity check: the produced sample really is compressed.
    let raw = fs::read_to_string(&compressed_path).expect("read back compressed sample");
    assert!(
        raw.contains("vtkZLibDataCompressor"),
        "test fixture must actually use ZLib compression"
    );

    let root = dir.write("root.pvtu", &pvtu(&["ascii.vtu", "zlib.vtu"]));
    let resolver = CountingResolver::new(dir.path().to_path_buf());
    let mut vtk = Vtk::import(&root).expect("import root parallel file");
    let report = vtk
        .import_pieces_with(&resolver)
        .expect("mixed compression pieces must merge when schemas match");

    assert_eq!(resolver.call_count(), 2);
    assert_eq!(report.merged_count(), 2);
    let pieces = inline_unstructured_pieces(&vtk);
    assert_eq!(pieces.len(), 2);
    assert_eq!(point_pressure(pieces[0]), vec![8.0; 4]);
    assert_eq!(
        point_pressure(pieces[1]),
        vec![9.0; 4],
        "compressed piece must be decoded through the library and merged"
    );
}

/// The default local resolver rejects network URIs and paths escaping the root directory.
#[test]
fn local_resolver_rejects_escaping_and_network_references() {
    let dir = TempDir::new("sandbox");
    dir.write(
        "inside.vtu",
        &tetra_vtu(&[1.0, 1.0, 1.0, 1.0], 1, 1, "Float32"),
    );
    let outside = dir.write(
        "../escape.vtu",
        &tetra_vtu(&[2.0, 2.0, 2.0, 2.0], 2, 1, "Float32"),
    );
    assert!(
        outside.exists(),
        "fixture: the escaping file does exist on disk"
    );

    let resolver = LocalPieceResolver::new(dir.path().to_path_buf());

    let traversal = resolver.resolve(&PieceRequest {
        uri: "../escape.vtu",
        base_path: Some(dir.path().join("root.pvtu").as_path()),
        reference_chain: &[],
    });
    assert!(
        traversal.is_err(),
        "a piece escaping the root directory must be rejected"
    );

    let network = resolver.resolve(&PieceRequest {
        uri: "https://example.test/piece.vtu",
        base_path: Some(dir.path().join("root.pvtu").as_path()),
        reference_chain: &[],
    });
    assert!(
        network.is_err(),
        "a network URI must be rejected by the local resolver"
    );

    // An in-root relative reference still works.
    let ok = resolver
        .resolve(&PieceRequest {
            uri: "inside.vtu",
            base_path: Some(dir.path().join("root.pvtu").as_path()),
            reference_chain: &[],
        })
        .expect("in-root relative reference must be allowed");
    assert!(ok.location.contains("inside.vtu"));
}

/// End-to-end `.pvtp` loading driven by a fully custom in-memory resolver.
#[test]
fn pvtp_loads_through_custom_resolver() {
    let leaf_vtp = "\
<VTKFile type=\"PolyData\" version=\"1.0\" byte_order=\"LittleEndian\" header_type=\"UInt64\">
  <PolyData>
    <Piece NumberOfPoints=\"2\" NumberOfVerts=\"2\">
      <PointData>
        <DataArray type=\"Float32\" Name=\"pressure\" format=\"ascii\">1 2</DataArray>
      </PointData>
      <CellData></CellData>
      <Points>
        <DataArray type=\"Float32\" Name=\"Points\" NumberOfComponents=\"3\" format=\"ascii\">
          0 0 0 1 0 0
        </DataArray>
      </Points>
      <Verts>
        <DataArray type=\"Int64\" Name=\"connectivity\" format=\"ascii\">0 1</DataArray>
        <DataArray type=\"Int64\" Name=\"offsets\" format=\"ascii\">1 2</DataArray>
      </Verts>
    </Piece>
  </PolyData>
</VTKFile>";
    let root_pvtp = "\
<VTKFile type=\"PPolyData\" version=\"1.0\" byte_order=\"LittleEndian\" header_type=\"UInt64\">
  <PPolyData GhostLevel=\"0\">
    <PPointData>
      <PDataArray type=\"Float32\" Name=\"pressure\"/>
    </PPointData>
    <PPoints>
      <PDataArray type=\"Float32\" NumberOfComponents=\"3\"/>
    </PPoints>
    <Piece Source=\"mem://leaf.vtp\"/>
  </PPolyData>
</VTKFile>";

    let dir = TempDir::new("pvtp");
    let root = dir.write("root.pvtp", root_pvtp);
    let mut files = HashMap::new();
    files.insert("mem://leaf.vtp".to_string(), leaf_vtp.as_bytes().to_vec());
    let resolver = InMemoryResolver::new(files);

    let mut vtk = Vtk::import(&root).expect("import pvtp root");
    let report = vtk
        .import_pieces_with(&resolver)
        .expect("custom resolver must serve the polydata piece");
    assert_eq!(resolver.call_count(), 1, "resolver must be called once");
    assert_eq!(report.merged_count(), 1);
    assert_eq!(report.pieces[0].num_points, Some(2));
    assert_eq!(report.pieces[0].num_cells, Some(2));

    let pieces = match &vtk.data {
        DataSet::PolyData { pieces, .. } => pieces,
        other => panic!("expected PolyData, got {:?}", other),
    };
    let piece = match &pieces[0] {
        Piece::Inline(p) => p.as_ref(),
        other => panic!("expected inline polydata piece, got {:?}", other),
    };
    assert_eq!(piece.num_points(), 2);
    assert_eq!(piece.num_cells(), 2);
}

/// End-to-end `.pvtr` loading through the default resolver-backed `import_pieces` entry point.
#[test]
fn pvtr_loads_with_default_entry_point() {
    let leaf_vtr = "\
<VTKFile type=\"RectilinearGrid\" version=\"1.0\" byte_order=\"LittleEndian\" header_type=\"UInt64\">
  <RectilinearGrid WholeExtent=\"0 1 0 1 0 1\">
    <Piece Extent=\"0 1 0 1 0 1\">
      <PointData></PointData>
      <CellData>
        <DataArray type=\"Float32\" Name=\"Pressure\" format=\"ascii\">42</DataArray>
      </CellData>
      <Coordinates>
        <DataArray type=\"Float64\" format=\"ascii\">0 1</DataArray>
        <DataArray type=\"Float64\" format=\"ascii\">0 2</DataArray>
        <DataArray type=\"Float64\" format=\"ascii\">0 3</DataArray>
      </Coordinates>
    </Piece>
  </RectilinearGrid>
</VTKFile>";
    let root_pvtr = "\
<VTKFile type=\"PRectilinearGrid\" version=\"1.0\" byte_order=\"LittleEndian\" header_type=\"UInt64\">
  <PRectilinearGrid WholeExtent=\"0 1 0 1 0 1\" GhostLevel=\"0\">
    <PCellData>
      <PDataArray type=\"Float32\" Name=\"Pressure\"/>
    </PCellData>
    <PCoordinates>
      <PDataArray type=\"Float64\"/>
      <PDataArray type=\"Float64\"/>
      <PDataArray type=\"Float64\"/>
    </PCoordinates>
    <Piece Extent=\"0 1 0 1 0 1\" Source=\"leaf.vtr\"/>
  </PRectilinearGrid>
</VTKFile>";

    let dir = TempDir::new("pvtr");
    dir.write("leaf.vtr", leaf_vtr);
    let root = dir.write("root.pvtr", root_pvtr);

    let mut vtk = Vtk::import(&root).expect("import pvtr root");
    let report = vtk
        .import_pieces()
        .expect("default local entry point must load the vtr piece");
    assert_eq!(report.len(), 1);
    assert_eq!(report.pieces[0].num_points, Some(8));
    assert_eq!(report.pieces[0].num_cells, Some(1));

    let piece = match &vtk.data {
        DataSet::RectilinearGrid { pieces, meta, .. } => {
            assert!(meta.is_none(), "parallel metadata must be cleared");
            match &pieces[0] {
                Piece::Inline(p) => p.as_ref(),
                other => panic!("expected inline rectilinear piece, got {:?}", other),
            }
        }
        other => panic!("expected RectilinearGrid, got {:?}", other),
    };
    assert_eq!(
        piece.coords.x.clone().cast_into::<f64>().unwrap(),
        vec![0.0, 1.0]
    );
    assert_eq!(
        piece.coords.y.clone().cast_into::<f64>().unwrap(),
        vec![0.0, 2.0]
    );
    assert_eq!(
        piece.coords.z.clone().cast_into::<f64>().unwrap(),
        vec![0.0, 3.0]
    );
    let pressure = match &piece.data.cell[0] {
        Attribute::DataArray(array) => array.data.clone().cast_into::<f32>().unwrap(),
        other => panic!("expected DataArray, got {:?}", other),
    };
    assert_eq!(pressure, vec![42.0]);
}

/// A byte-order mismatch between pieces is rejected before merging.
#[test]
fn byte_order_mismatch_is_rejected() {
    let dir = TempDir::new("byte_order");
    dir.write("le.vtu", &tetra_vtu(&[1.0; 4], 1, 1, "Float32"));
    let be_vtu = tetra_vtu(&[1.0; 4], 1, 1, "Float32")
        .replace("byte_order=\"LittleEndian\"", "byte_order=\"BigEndian\"");
    dir.write("be.vtu", &be_vtu);
    let root = dir.write("root.pvtu", &pvtu(&["le.vtu", "be.vtu"]));

    let mut vtk = Vtk::import(&root).expect("import root");
    let before = vtk.clone();
    let err = vtk.import_pieces().expect_err("mixed byte order must fail");
    assert!(
        err.message.contains("byte order"),
        "error must mention byte order, got: {}",
        err.message
    );
    assert_eq!(vtk, before);
}

/// An empty piece (no points and no cells) is skipped and reported without aborting the load.
#[test]
fn empty_piece_is_skipped_while_others_merge() {
    let dir = TempDir::new("empty_piece");
    dir.write(
        "data.vtu",
        &tetra_vtu(&[6.0, 6.0, 6.0, 6.0], 66, 1, "Float32"),
    );
    let empty_vtu = "\
<VTKFile type=\"UnstructuredGrid\" version=\"1.0\" byte_order=\"LittleEndian\" header_type=\"UInt64\">
  <UnstructuredGrid>
    <Piece NumberOfPoints=\"0\" NumberOfCells=\"0\">
      <PointData></PointData>
      <CellData></CellData>
      <Points>
        <DataArray type=\"Float32\" Name=\"Points\" NumberOfComponents=\"3\" format=\"ascii\"></DataArray>
      </Points>
      <Cells>
        <DataArray type=\"Int64\" Name=\"connectivity\" format=\"ascii\"></DataArray>
        <DataArray type=\"Int64\" Name=\"offsets\" format=\"ascii\"></DataArray>
        <DataArray type=\"UInt8\" Name=\"types\" format=\"ascii\"></DataArray>
      </Cells>
    </Piece>
  </UnstructuredGrid>
</VTKFile>";
    dir.write("empty.vtu", empty_vtu);
    let root = dir.write("root.pvtu", &pvtu(&["data.vtu", "empty.vtu"]));

    let resolver = CountingResolver::new(dir.path().to_path_buf());
    let mut vtk = Vtk::import(&root).expect("import root");
    let report = vtk
        .import_pieces_with(&resolver)
        .expect("an empty piece must not abort an otherwise valid load");

    assert_eq!(resolver.call_count(), 2, "both files must still be read");
    assert_eq!(
        report.skipped_count(),
        1,
        "the empty piece must be recorded as skipped"
    );
    assert_eq!(
        report.merged_count(),
        1,
        "only the non-empty piece must be merged"
    );
    let skipped = &report.pieces[1];
    assert_eq!(skipped.source_uri, "empty.vtu");
    assert_eq!(skipped.num_points, Some(0));
    assert_eq!(skipped.num_cells, Some(0));
    assert!(
        matches!(&skipped.status, PieceStatus::Skipped { reason } if reason.contains("no points")),
        "skipped entry must explain why, got {:?}",
        skipped.status
    );

    let pieces = inline_unstructured_pieces(&vtk);
    assert_eq!(
        pieces.len(),
        1,
        "the empty piece must not appear in the model"
    );
    assert_eq!(point_pressure(pieces[0]), vec![6.0; 4]);
}

/// A piece of the wrong dataset kind (PolyData inside a PUnstructuredGrid) is rejected.
#[test]
fn dataset_kind_conflict_is_rejected() {
    let dir = TempDir::new("kind_conflict");
    dir.write("good.vtu", &tetra_vtu(&[1.0; 4], 1, 1, "Float32"));
    let polydata_vtp = "\
<VTKFile type=\"PolyData\" version=\"1.0\" byte_order=\"LittleEndian\" header_type=\"UInt64\">
  <PolyData>
    <Piece NumberOfPoints=\"1\" NumberOfVerts=\"1\">
      <PointData></PointData>
      <CellData></CellData>
      <Points>
        <DataArray type=\"Float32\" Name=\"Points\" NumberOfComponents=\"3\" format=\"ascii\">
          0 0 0
        </DataArray>
      </Points>
      <Verts>
        <DataArray type=\"Int64\" Name=\"connectivity\" format=\"ascii\">0</DataArray>
        <DataArray type=\"Int64\" Name=\"offsets\" format=\"ascii\">1</DataArray>
      </Verts>
    </Piece>
  </PolyData>
</VTKFile>";
    dir.write("wrong_kind.vtp", polydata_vtp);
    let root = dir.write("root.pvtu", &pvtu(&["good.vtu", "wrong_kind.vtp"]));

    let mut vtk = Vtk::import(&root).expect("import root");
    let before = vtk.clone();
    let err = vtk
        .import_pieces()
        .expect_err("a piece of the wrong dataset kind must fail");
    assert!(
        err.message.contains("kind"),
        "error must identify the dataset kind mismatch, got: {}",
        err.message
    );
    let failed = err.report.failed_piece().expect("failing piece in report");
    assert_eq!(failed.index, 1);
    assert_eq!(failed.source_uri, "wrong_kind.vtp");
    assert_eq!(vtk, before, "model must stay untouched on failure");
}

/// Two root directories use independent resolvers and never see each other's same-named piece.
#[test]
fn different_roots_use_independent_resolvers() {
    let root_a = TempDir::new("root_a");
    let root_b = TempDir::new("root_b");
    // Both roots contain a piece with the identical relative name but different contents.
    root_a.write(
        "piece.vtu",
        &tetra_vtu(&[10.0, 10.0, 10.0, 10.0], 101, 1, "Float32"),
    );
    root_b.write(
        "piece.vtu",
        &tetra_vtu(&[20.0, 20.0, 20.0, 20.0], 202, 1, "Float32"),
    );
    let pvtu_a = root_a.write("root.pvtu", &pvtu(&["piece.vtu"]));
    let pvtu_b = root_b.write("root.pvtu", &pvtu(&["piece.vtu"]));

    let resolver_a = CountingResolver::new(root_a.path().to_path_buf());
    let resolver_b = CountingResolver::new(root_b.path().to_path_buf());

    let mut vtk_a = Vtk::import(&pvtu_a).expect("import root a");
    let report_a = vtk_a
        .import_pieces_with(&resolver_a)
        .expect("load under root a");
    let mut vtk_b = Vtk::import(&pvtu_b).expect("import root b");
    let report_b = vtk_b
        .import_pieces_with(&resolver_b)
        .expect("load under root b");

    assert_eq!(resolver_a.call_count(), 1);
    assert_eq!(
        resolver_b.call_count(),
        1,
        "each resolver keeps its own load"
    );
    assert!(
        report_a.pieces[0].resolved_uri.contains(
            &root_a
                .path()
                .file_name()
                .unwrap()
                .to_string_lossy()
                .to_string()
        ),
        "root A must resolve its own piece, got {:?}",
        report_a.pieces[0].resolved_uri
    );
    assert!(
        report_b.pieces[0].resolved_uri.contains(
            &root_b
                .path()
                .file_name()
                .unwrap()
                .to_string_lossy()
                .to_string()
        ),
        "root B must resolve its own piece, got {:?}",
        report_b.pieces[0].resolved_uri
    );
    assert_ne!(
        report_a.pieces[0].resolved_uri, report_b.pieces[0].resolved_uri,
        "the same relative name under two roots must be distinct identities"
    );

    assert_eq!(
        point_pressure(inline_unstructured_pieces(&vtk_a)[0]),
        vec![10.0; 4]
    );
    assert_eq!(
        point_pressure(inline_unstructured_pieces(&vtk_b)[0]),
        vec![20.0; 4],
        "root B must not receive root A's cached piece"
    );

    // Resolver A is confined to root A: it must refuse root B's file even when handed it directly.
    let cross = resolver_a.resolve(&PieceRequest {
        uri: &report_b.pieces[0].resolved_uri,
        base_path: None,
        reference_chain: &[],
    });
    assert!(
        cross.is_err(),
        "a root-confined resolver must reject files belonging to another root"
    );
}
