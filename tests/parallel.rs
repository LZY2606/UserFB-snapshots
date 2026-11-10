#![cfg(feature = "xml")]
//! Integration tests for loading parallel VTK XML files through `vtkio::parallel`.
//!
//! All tests generate their sample files in temporary directories (no Python/pygmsh involved)
//! and exercise the actual loading entry points: `parallel::load`, `parallel::load_with_options`
//! and `Vtk::import_with_resolver`.

use std::fs;
use std::path::{Path, PathBuf};
use std::sync::Mutex;

use vtkio::model::{ByteOrder, DataSet, Extent, IOBuffer, Piece, UnstructuredGridPiece};
#[cfg(all(feature = "flate2", feature = "lz4"))]
use vtkio::model::{Attribute, Attributes, CellType, Cells, VertexNumbers, Version};
use vtkio::parallel::{
    self, CompatibilityError, LoadErrorKind, LoadOptions, LocalFileResolver, PieceOutcome,
    PieceReference, PieceResolver, ResolveError,
};
use vtkio::Vtk;

/*
 * Test scaffolding
 */

/// A temporary directory that is removed when dropped.
struct TestDir(PathBuf);

impl TestDir {
    fn new(name: &str) -> TestDir {
        let dir = std::env::temp_dir().join(format!(
            "vtkio_parallel_{}_{}",
            name,
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).expect("failed to create temporary test directory");
        TestDir(dir)
    }

    fn path(&self) -> &Path {
        &self.0
    }

    /// Write a file into the test directory, creating parent directories as needed.
    fn write(&self, rel: &str, contents: &str) -> PathBuf {
        let path = self.0.join(rel);
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent).expect("failed to create parent directories");
        }
        fs::write(&path, contents).expect("failed to write test file");
        path
    }
}

impl Drop for TestDir {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

/// A resolver that records every reference it is asked to resolve, on top of the default local
/// file resolver.
struct CountingResolver {
    inner: LocalFileResolver,
    calls: Mutex<Vec<String>>,
}

impl CountingResolver {
    fn new(root: &Path) -> Self {
        CountingResolver {
            inner: LocalFileResolver::new(root),
            calls: Mutex::new(Vec::new()),
        }
    }

    /// The URIs of all references the resolver was asked to resolve, in order.
    fn calls(&self) -> Vec<String> {
        self.calls.lock().unwrap().clone()
    }
}

impl PieceResolver for CountingResolver {
    fn resolve(&self, reference: &PieceReference) -> Result<Vec<u8>, ResolveError> {
        self.calls.lock().unwrap().push(reference.uri.clone());
        self.inner.resolve(reference)
    }
}

/*
 * Sample file generators
 */

/// An ASCII `.vtu` file with a single triangle whose first point sits at `marker` on the x
/// axis, so tests can tell pieces apart by their point data. The piece carries a scalar point
/// attribute "pressure" with `num_comp` components.
fn triangle_vtu_with_components(marker: f32, num_comp: usize) -> String {
    let pressure = (0..3 * num_comp)
        .map(|_| format!("{}", marker))
        .collect::<Vec<_>>()
        .join(" ");
    format!(
        r#"<VTKFile type="UnstructuredGrid" version="2.0" byte_order="BigEndian" header_type="UInt64">
  <UnstructuredGrid>
    <Piece NumberOfPoints="3" NumberOfCells="1">
      <PointData>
        <DataArray type="Float32" Name="pressure" NumberOfComponents="{nc}" format="ascii">
          {pressure}
        </DataArray>
      </PointData>
      <Points>
        <DataArray type="Float32" Name="Points" NumberOfComponents="3" format="ascii">
          {m} 0 0 1 0 0 0 1 0
        </DataArray>
      </Points>
      <Cells>
        <DataArray type="Int64" Name="connectivity" format="ascii">
          0 1 2
        </DataArray>
        <DataArray type="Int64" Name="offsets" format="ascii">
          3
        </DataArray>
        <DataArray type="UInt8" Name="types" format="ascii">
          5
        </DataArray>
      </Cells>
    </Piece>
  </UnstructuredGrid>
</VTKFile>
"#,
        m = marker,
        nc = num_comp,
        pressure = pressure
    )
}

fn triangle_vtu(marker: f32) -> String {
    triangle_vtu_with_components(marker, 1)
}

/// An ASCII `.vtp` file with a single triangle, used to test data set kind mismatches.
fn triangle_vtp(marker: f32) -> String {
    format!(
        r#"<VTKFile type="PolyData" version="2.0" byte_order="BigEndian" header_type="UInt64">
  <PolyData>
    <Piece NumberOfPoints="3" NumberOfPolys="1">
      <Points>
        <DataArray type="Float32" Name="Points" NumberOfComponents="3" format="ascii">
          {m} 0 0 1 0 0 0 1 0
        </DataArray>
      </Points>
      <Polys>
        <DataArray type="Int64" Name="connectivity" format="ascii">
          0 1 2
        </DataArray>
        <DataArray type="Int64" Name="offsets" format="ascii">
          3
        </DataArray>
      </Polys>
    </Piece>
  </PolyData>
</VTKFile>
"#,
        m = marker
    )
}

/// A `.pvtu` file referencing the given piece sources.
fn pvtu(sources: &[&str]) -> String {
    let pieces: String = sources
        .iter()
        .map(|s| format!("    <Piece Source=\"{}\"/>\n", s))
        .collect();
    format!(
        r#"<VTKFile type="PUnstructuredGrid" version="2.0" byte_order="BigEndian" header_type="UInt64">
  <PUnstructuredGrid GhostLevel="0">
    <PPointData>
      <PDataArray type="Float32" Name="pressure" NumberOfComponents="1"/>
    </PPointData>
    <PPoints>
      <PDataArray type="Float32" Name="Points" NumberOfComponents="3"/>
    </PPoints>
{}  </PUnstructuredGrid>
</VTKFile>
"#,
        pieces
    )
}

/// A `.pvtp` file referencing the given piece sources.
fn pvtp(sources: &[&str]) -> String {
    let pieces: String = sources
        .iter()
        .map(|s| format!("    <Piece Source=\"{}\"/>\n", s))
        .collect();
    format!(
        r#"<VTKFile type="PPolyData" version="2.0" byte_order="BigEndian" header_type="UInt64">
  <PPolyData GhostLevel="0">
    <PPoints>
      <PDataArray type="Float32" Name="Points" NumberOfComponents="3"/>
    </PPoints>
{}  </PPolyData>
</VTKFile>
"#,
        pieces
    )
}

/// A `.pvtr` file with a single piece of the given extent and source.
fn pvtr(extent: &str, source: &str) -> String {
    format!(
        r#"<VTKFile type="PRectilinearGrid" version="1.0" byte_order="BigEndian" header_type="UInt64">
  <PRectilinearGrid WholeExtent="{extent}" GhostLevel="0">
    <PPointData>
      <PDataArray type="Float32" Name="pressure" NumberOfComponents="1"/>
    </PPointData>
    <PCoordinates>
      <PDataArray type="Float32"/>
      <PDataArray type="Float32"/>
      <PDataArray type="Float32"/>
    </PCoordinates>
    <Piece Extent="{extent}" Source="{source}"/>
  </PRectilinearGrid>
</VTKFile>
"#,
        extent = extent,
        source = source
    )
}

/// An ASCII `.vtr` file covering a 2x2x2 point extent.
const RECT_VTR: &str = r#"<VTKFile type="RectilinearGrid" version="1.0" byte_order="BigEndian" header_type="UInt64">
  <RectilinearGrid WholeExtent="0 1 0 1 0 1">
    <Piece Extent="0 1 0 1 0 1">
      <PointData>
        <DataArray type="Float32" Name="pressure" NumberOfComponents="1" format="ascii">
          1 2 3 4 5 6 7 8
        </DataArray>
      </PointData>
      <Coordinates>
        <DataArray type="Float32" format="ascii">
          0 1
        </DataArray>
        <DataArray type="Float32" format="ascii">
          0 1
        </DataArray>
        <DataArray type="Float32" format="ascii">
          0 1
        </DataArray>
      </Coordinates>
    </Piece>
  </RectilinearGrid>
</VTKFile>
"#;

/// The same triangle as `triangle_vtu` but as an in-memory model, used to write pieces through
/// the library's own writer (e.g. with compression).
#[cfg(all(feature = "flate2", feature = "lz4"))]
fn triangle_vtk(marker: f32) -> Vtk {
    Vtk {
        version: Version::new_xml(2, 0),
        title: String::new(),
        byte_order: ByteOrder::BigEndian,
        file_path: None,
        data: DataSet::inline(UnstructuredGridPiece {
            points: IOBuffer::F32(vec![marker, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0]),
            cells: Cells {
                cell_verts: VertexNumbers::XML {
                    connectivity: vec![0, 1, 2],
                    offsets: vec![3],
                },
                types: vec![CellType::Triangle],
            },
            data: Attributes {
                point: vec![
                    Attribute::scalars("pressure", 1).with_data(IOBuffer::F32(vec![marker; 3]))
                ],
                cell: vec![],
            },
        }),
    }
}

/*
 * Model inspection helpers
 */

fn unstructured_pieces(vtk: &Vtk) -> &[Piece<UnstructuredGridPiece>] {
    match &vtk.data {
        DataSet::UnstructuredGrid { pieces, .. } => pieces,
        other => panic!("expected an UnstructuredGrid data set, got {:?}", other),
    }
}

/// The x coordinate of the first point of each merged piece, identifying its source file.
fn merged_markers(vtk: &Vtk) -> Vec<f32> {
    unstructured_pieces(vtk)
        .iter()
        .map(|piece| match piece {
            Piece::Inline(data) => match &data.points {
                IOBuffer::F32(coords) => coords[0],
                other => panic!("expected F32 points, got {:?}", other),
            },
            other => panic!("expected an inline piece, got {:?}", other),
        })
        .collect()
}

fn polydata_markers(vtk: &Vtk) -> Vec<f32> {
    match &vtk.data {
        DataSet::PolyData { pieces, .. } => pieces
            .iter()
            .map(|piece| match piece {
                Piece::Inline(data) => match &data.points {
                    IOBuffer::F32(coords) => coords[0],
                    other => panic!("expected F32 points, got {:?}", other),
                },
                other => panic!("expected an inline piece, got {:?}", other),
            })
            .collect(),
        other => panic!("expected a PolyData data set, got {:?}", other),
    }
}

/*
 * Tests
 */

/// A `.pvtp` file referencing the same piece twice must read and parse the piece only once,
/// while the report shows both references and the merged model contains both pieces.
#[test]
fn pvtp_duplicate_piece_reference_is_resolved_once() {
    let dir = TestDir::new("duplicate");
    dir.write("piece0.vtp", &triangle_vtp(1.0));
    let root = dir.write("root.pvtp", &pvtp(&["piece0.vtp", "./piece0.vtp"]));

    let resolver = CountingResolver::new(dir.path());
    let (vtk, report) = parallel::load(&root, &resolver).expect("failed to load root.pvtp");

    assert_eq!(
        resolver.calls(),
        vec![String::from("piece0.vtp")],
        "piece0.vtp is referenced twice but must be resolved exactly once, got calls: {:?}",
        resolver.calls()
    );
    assert_eq!(
        report.pieces.len(),
        2,
        "both piece references must appear in the report: {:?}",
        report.pieces
    );
    assert!(
        !report.pieces[0].from_cache,
        "the first reference to piece0.vtp must be loaded from disk"
    );
    assert!(
        report.pieces[1].from_cache,
        "the second reference to piece0.vtp must be served from the cache"
    );
    assert!(
        report
            .pieces
            .iter()
            .all(|p| p.outcome == PieceOutcome::Merged),
        "both references must be merged: {:?}",
        report.pieces
    );
    assert_eq!(
        polydata_markers(&vtk),
        vec![1.0, 1.0],
        "the merged model must contain one piece per declared reference"
    );
}

/// Two branches of nested parallel files referencing the same piece: the shared piece is read
/// and parsed once, both references appear in the report, and merged pieces follow declaration
/// order.
#[test]
fn nested_branches_sharing_a_piece_are_resolved_once() {
    let dir = TestDir::new("shared");
    dir.write("shared.vtu", &triangle_vtu(1.0));
    dir.write("a_only.vtu", &triangle_vtu(2.0));
    dir.write("b_only.vtu", &triangle_vtu(3.0));
    dir.write("branch_a.pvtu", &pvtu(&["shared.vtu", "a_only.vtu"]));
    // The second branch reaches the shared piece through a different but equivalent path.
    dir.write("branch_b.pvtu", &pvtu(&["subdir/../shared.vtu", "b_only.vtu"]));
    let root = dir.write("root.pvtu", &pvtu(&["branch_a.pvtu", "branch_b.pvtu"]));

    let resolver = CountingResolver::new(dir.path());
    let (vtk, report) = parallel::load(&root, &resolver).expect("failed to load root.pvtu");

    let mut calls = resolver.calls();
    calls.sort();
    assert_eq!(
        calls,
        vec![
            String::from("a_only.vtu"),
            String::from("b_only.vtu"),
            String::from("branch_a.pvtu"),
            String::from("branch_b.pvtu"),
            String::from("shared.vtu"),
        ],
        "each unique piece file must be resolved exactly once, got calls: {:?}",
        calls
    );

    let shared_refs: Vec<_> = report
        .pieces
        .iter()
        .filter(|p| p.location.ends_with("shared.vtu"))
        .collect();
    assert_eq!(
        shared_refs.len(),
        2,
        "both references to shared.vtu must appear in the report: {:?}",
        report.pieces
    );
    assert_eq!(
        shared_refs.iter().filter(|p| p.from_cache).count(),
        1,
        "exactly one of the shared.vtu references must be served from the cache: {:?}",
        shared_refs
    );

    assert_eq!(
        merged_markers(&vtk),
        vec![1.0, 2.0, 1.0, 3.0],
        "merged pieces must appear in declaration order (shared, a_only, shared, b_only)"
    );
    assert!(
        report.pieces.iter().all(|p| p.outcome == PieceOutcome::Merged),
        "all references must be merged: {:?}",
        report.pieces
    );
}

/// A reference cycle must abort the load with an error containing the full reference chain.
#[test]
fn cyclic_references_report_the_full_chain() {
    let dir = TestDir::new("cycle");
    dir.write("a.pvtu", &pvtu(&["b.pvtu"]));
    dir.write("b.pvtu", &pvtu(&["a.pvtu"]));
    let root = dir.write("root.pvtu", &pvtu(&["a.pvtu"]));

    let resolver = CountingResolver::new(dir.path());
    let err = parallel::load(&root, &resolver).expect_err("a cyclic reference must fail the load");

    match &err.kind {
        LoadErrorKind::Cycle(chain) => {
            let names: Vec<String> = chain
                .iter()
                .map(|p| {
                    p.file_name()
                        .expect("cycle chain entries must be files")
                        .to_string_lossy()
                        .into_owned()
                })
                .collect();
            assert_eq!(
                names,
                vec![
                    String::from("root.pvtu"),
                    String::from("a.pvtu"),
                    String::from("b.pvtu"),
                    String::from("a.pvtu"),
                ],
                "the cycle error must contain the full reference chain from the root file to \
                 the repeated location"
            );
        }
        other => panic!("expected a cycle error, got {:?}", other),
    }

    assert_eq!(
        err.report.pieces.len(),
        3,
        "the report must contain the references that were in flight: {:?}",
        err.report.pieces
    );
    assert!(
        err.report
            .pieces
            .iter()
            .all(|p| p.outcome == PieceOutcome::Pending),
        "references caught in the cycle were never fully processed: {:?}",
        err.report.pieces
    );
}

/// A missing piece file fails a strict load; the report shows which pieces were processed,
/// which one failed and which were never reached. A partial load merges the rest, and a fresh
/// load after fixing the input re-reads the files instead of reusing a stale cache.
#[test]
fn missing_piece_file_is_reported_and_recoverable() {
    let dir = TestDir::new("missing");
    dir.write("first.vtu", &triangle_vtu(1.0));
    dir.write("last.vtu", &triangle_vtu(3.0));
    let root = dir.write(
        "root.pvtu",
        &pvtu(&["first.vtu", "missing.vtu", "last.vtu"]),
    );

    // Strict load: the first failure aborts the load.
    let resolver = CountingResolver::new(dir.path());
    let err =
        parallel::load(&root, &resolver).expect_err("a missing piece file must fail the load");
    assert!(
        matches!(err.kind, LoadErrorKind::PieceFailed),
        "expected a piece failure, got {:?}",
        err.kind
    );
    let outcomes: Vec<_> = err.report.pieces.iter().map(|p| p.outcome.clone()).collect();
    assert!(
        matches!(outcomes[0], PieceOutcome::Loaded),
        "first.vtu must have been loaded before the failure: {:?}",
        outcomes
    );
    match &outcomes[1] {
        PieceOutcome::Failed(message) => assert!(
            message.contains("missing.vtu"),
            "the failure must name the missing piece, got: {}",
            message
        ),
        other => panic!("missing.vtu must be marked as failed, got {:?}", other),
    }
    assert!(
        matches!(outcomes[2], PieceOutcome::Pending),
        "last.vtu must not have been processed after the failure: {:?}",
        outcomes
    );

    // Partial load: the failing piece is skipped and the remaining pieces are merged.
    let resolver = CountingResolver::new(dir.path());
    let (vtk, report) = parallel::load_with_options(
        &root,
        &resolver,
        LoadOptions {
            allow_partial: true,
        },
    )
    .expect("a partial load must succeed despite the missing piece");
    assert_eq!(
        merged_markers(&vtk),
        vec![1.0, 3.0],
        "only the pieces that loaded successfully may be merged"
    );
    assert_eq!(
        report.num_merged(),
        2,
        "exactly the two available pieces must be merged: {:?}",
        report.pieces
    );
    assert_eq!(
        report.num_failed(),
        1,
        "exactly one piece must be marked as failed: {:?}",
        report.pieces
    );

    // After fixing the input, a fresh load re-reads the files: no cache from the failed loads
    // may be reused.
    dir.write("missing.vtu", &triangle_vtu(2.0));
    let resolver = CountingResolver::new(dir.path());
    let (vtk, report) =
        parallel::load(&root, &resolver).expect("the load must succeed after fixing the input");
    assert_eq!(
        resolver.calls().len(),
        3,
        "a fresh load must re-resolve every piece, got calls: {:?}",
        resolver.calls()
    );
    assert_eq!(merged_markers(&vtk), vec![1.0, 2.0, 3.0]);
    assert!(
        report.is_ok(),
        "no failure may be reported after fixing the input: {:?}",
        report.pieces
    );
}

/// Pieces whose point attribute schemas disagree (here: same array name but a different number
/// of components) must fail the load before anything is merged.
#[test]
fn conflicting_attribute_schemas_fail_the_load() {
    let dir = TestDir::new("schema_conflict");
    dir.write("piece0.vtu", &triangle_vtu_with_components(1.0, 1));
    // Same attribute name but three components instead of one.
    dir.write("piece1.vtu", &triangle_vtu_with_components(2.0, 3));
    let root = dir.write("root.pvtu", &pvtu(&["piece0.vtu", "piece1.vtu"]));

    let resolver = CountingResolver::new(dir.path());
    let err = parallel::load(&root, &resolver)
        .expect_err("conflicting attribute schemas must fail the load");
    match &err.kind {
        LoadErrorKind::Compatibility(CompatibilityError::PointAttributes {
            location,
            expected,
            found,
        }) => {
            assert!(
                location.ends_with("piece1.vtu"),
                "the error must point at the conflicting piece, got {:?}",
                location
            );
            assert!(
                expected.contains("x1") && found.contains("x3"),
                "the error must describe the conflicting schemas, got expected={} found={}",
                expected,
                found
            );
        }
        other => panic!(
            "expected a point attribute compatibility error, got {:?}",
            other
        ),
    }
    assert!(
        err.report
            .pieces
            .iter()
            .all(|p| p.outcome == PieceOutcome::Loaded),
        "no piece may be merged when validation fails: {:?}",
        err.report.pieces
    );
}

/// Pieces stored with different compressors (ZLib, LZ4 and uncompressed) must be decoded and
/// merged transparently. The compressed pieces are produced by the library's own writer.
#[cfg(all(feature = "flate2", feature = "lz4"))]
#[test]
fn pieces_with_different_compression_are_merged() {
    use vtkio::xml::Compressor;

    let dir = TestDir::new("mixed_compression");
    for (marker, name, compressor) in [
        (1.0f32, "zlib.vtu", Compressor::ZLib),
        (2.0, "lz4.vtu", Compressor::LZ4),
        (3.0, "plain.vtu", Compressor::None),
    ] {
        triangle_vtk(marker)
            .try_into_xml_format(compressor, 1)
            .expect("failed to convert the piece to XML format")
            .export(dir.path().join(name))
            .expect("failed to write the piece");
    }
    let root = dir.write("root.pvtu", &pvtu(&["zlib.vtu", "lz4.vtu", "plain.vtu"]));

    let resolver = CountingResolver::new(dir.path());
    let (vtk, report) =
        parallel::load(&root, &resolver).expect("pieces with different compression must load");
    assert_eq!(
        resolver.calls().len(),
        3,
        "each compressed piece must be resolved exactly once, got calls: {:?}",
        resolver.calls()
    );
    assert_eq!(
        report.num_merged(),
        3,
        "all compressed pieces must be merged: {:?}",
        report.pieces
    );
    assert_eq!(
        merged_markers(&vtk),
        vec![1.0, 2.0, 3.0],
        "the merged points must match the originally written data"
    );
}

/// The default resolver must reject pieces that escape its root directory, even if the target
/// file exists.
#[test]
fn default_resolver_refuses_to_escape_its_root() {
    let dir = TestDir::new("escape");
    dir.write("outside.vtu", &triangle_vtu(1.0));
    let root = dir.write("sub/root.pvtu", &pvtu(&["../outside.vtu"]));

    // The resolver is rooted at the directory containing root.pvtu, so `../outside.vtu` is out
    // of bounds even though the file exists.
    let resolver = CountingResolver::new(&dir.path().join("sub"));
    let err = parallel::load(&root, &resolver)
        .expect_err("a piece outside of the resolver root must be rejected");
    assert!(
        matches!(err.kind, LoadErrorKind::PieceFailed),
        "expected a piece failure, got {:?}",
        err.kind
    );
    match &err.report.pieces[0].outcome {
        PieceOutcome::Failed(message) => assert!(
            message.contains("outside of the root directory"),
            "the failure must explain the root violation, got: {}",
            message
        ),
        other => panic!("expected a failed piece, got {:?}", other),
    }
}

/// The default resolver must reject remote piece URIs.
#[test]
fn default_resolver_refuses_remote_pieces() {
    let dir = TestDir::new("remote");
    let root = dir.write("root.pvtu", &pvtu(&["https://example.com/piece.vtu"]));

    let resolver = CountingResolver::new(dir.path());
    let err = parallel::load(&root, &resolver).expect_err("a remote piece must be rejected");
    match &err.report.pieces[0].outcome {
        PieceOutcome::Failed(message) => assert!(
            message.contains("remote"),
            "the failure must explain that remote pieces are refused, got: {}",
            message
        ),
        other => panic!("expected a failed piece, got {:?}", other),
    }
}

/// Pieces whose byte order differs from the referring file must fail the load.
#[test]
fn mismatched_byte_order_fails_the_load() {
    let dir = TestDir::new("byte_order");
    dir.write("be.vtu", &triangle_vtu(1.0));
    dir.write("le.vtu", &triangle_vtu(2.0).replace("BigEndian", "LittleEndian"));
    let root = dir.write("root.pvtu", &pvtu(&["be.vtu", "le.vtu"]));

    let resolver = CountingResolver::new(dir.path());
    let err =
        parallel::load(&root, &resolver).expect_err("mismatched byte order must fail the load");
    match &err.kind {
        LoadErrorKind::Compatibility(CompatibilityError::ByteOrder {
            location, found, ..
        }) => {
            assert!(
                location.ends_with("le.vtu"),
                "the error must point at the little endian piece, got {:?}",
                location
            );
            assert_eq!(*found, ByteOrder::LittleEndian);
        }
        other => panic!("expected a byte order compatibility error, got {:?}", other),
    }
}

/// A piece file containing a different kind of data set than the referring file must fail the
/// load.
#[test]
fn mismatched_data_set_kind_fails_the_load() {
    let dir = TestDir::new("kind_mismatch");
    dir.write("mesh.vtu", &triangle_vtu(1.0));
    dir.write("poly.vtp", &triangle_vtp(2.0));
    let root = dir.write("root.pvtu", &pvtu(&["mesh.vtu", "poly.vtp"]));

    let resolver = CountingResolver::new(dir.path());
    let err = parallel::load(&root, &resolver)
        .expect_err("a piece with the wrong data set kind must fail the load");
    match &err.kind {
        LoadErrorKind::Compatibility(CompatibilityError::DataSetKind {
            expected, found, ..
        }) => {
            assert_eq!(*expected, "UnstructuredGrid");
            assert_eq!(*found, "PolyData");
        }
        other => panic!("expected a data set kind error, got {:?}", other),
    }
}

/// A `.pvtr` load: the report carries the extent declared for the piece and the merged model
/// is a rectilinear grid.
#[test]
fn pvtr_piece_extent_is_reported() {
    let dir = TestDir::new("pvtr");
    dir.write("rect_0.vtr", RECT_VTR);
    let root = dir.write("root.pvtr", &pvtr("0 1 0 1 0 1", "rect_0.vtr"));

    let resolver = CountingResolver::new(dir.path());
    let (vtk, report) = parallel::load(&root, &resolver).expect("failed to load root.pvtr");

    assert_eq!(report.pieces.len(), 1);
    assert_eq!(
        report.pieces[0].extent,
        Some(Extent::Ranges([0..=1, 0..=1, 0..=1])),
        "the report must carry the extent declared for the piece"
    );
    assert_eq!(
        report.pieces[0].outcome,
        PieceOutcome::Merged,
        "the piece must be merged"
    );
    match &vtk.data {
        DataSet::RectilinearGrid { pieces, .. } => {
            assert_eq!(pieces.len(), 1, "expected a single merged piece");
        }
        other => panic!("expected a RectilinearGrid data set, got {:?}", other),
    }
}

/// The `Vtk::import_with_resolver` entry point must behave like `parallel::load`.
#[test]
fn vtk_import_with_resolver_entry_point() {
    let dir = TestDir::new("entry_point");
    dir.write("piece0.vtu", &triangle_vtu(1.0));
    let root = dir.write("root.pvtu", &pvtu(&["piece0.vtu"]));

    let resolver = LocalFileResolver::new(dir.path());
    let (vtk, report) =
        Vtk::import_with_resolver(&root, &resolver).expect("failed to load root.pvtu");
    assert_eq!(merged_markers(&vtk), vec![1.0]);
    assert_eq!(report.num_merged(), 1);
}

/// A serial (non-parallel) file loads without any resolver interaction.
#[test]
fn serial_files_load_without_resolution() {
    let dir = TestDir::new("serial");
    let root = dir.write("single.vtu", &triangle_vtu(1.0));

    let resolver = CountingResolver::new(dir.path());
    let (vtk, report) = parallel::load(&root, &resolver).expect("failed to load single.vtu");
    assert!(
        resolver.calls().is_empty(),
        "a serial file has no pieces to resolve, got calls: {:?}",
        resolver.calls()
    );
    assert!(
        report.pieces.is_empty(),
        "a serial file must produce an empty report: {:?}",
        report.pieces
    );
    assert_eq!(merged_markers(&vtk), vec![1.0]);
}
