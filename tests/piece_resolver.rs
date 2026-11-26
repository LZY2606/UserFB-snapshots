#![cfg(feature = "xml")]

use std::fs;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::{SystemTime, UNIX_EPOCH};

use vtkio::parallel::*;
use vtkio::xml::{Compressor, VTKFile};
use vtkio::{model::*, Vtk};

type BoxError = Box<dyn std::error::Error>;

fn temp_dir(name: &str) -> PathBuf {
    let nanos = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let dir = std::env::temp_dir().join(format!(
        "vtkio-piece-resolver-{}-{nanos}-{name}",
        std::process::id()
    ));
    fs::create_dir_all(&dir).expect("create temporary test directory");
    dir
}

fn write_file(path: &Path, contents: &str) {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).expect("create sample parent directory");
    }
    fs::write(path, contents).expect("write XML sample");
}

/// Resolver wrapper that records each resolver call while delegating to the
/// safe local resolver.
struct CountingResolver {
    local: LocalPieceResolver,
    calls: Arc<Mutex<Vec<String>>>,
}

impl CountingResolver {
    fn new(root: impl AsRef<Path>, calls: Arc<Mutex<Vec<String>>>) -> Self {
        Self {
            local: LocalPieceResolver::new(root).expect("create local resolver"),
            calls,
        }
    }

    fn call_count_for(&self, file_name: &str) -> usize {
        self.calls
            .lock()
            .unwrap()
            .iter()
            .filter(|call| call.ends_with(file_name))
            .count()
    }
}

impl PieceResolver for CountingResolver {
    fn resolve(&self, request: &PieceRequest) -> Result<ResolvedPiece, ResolveError> {
        let label = request
            .resolved_path
            .as_ref()
            .and_then(|p| p.file_name())
            .map(|n| n.to_string_lossy().to_string())
            .unwrap_or_else(|| request.normalized_uri.clone());
        self.calls.lock().unwrap().push(label);
        self.local.resolve(request)
    }
}

fn assert_source_parsed_once(report: &PieceLoadReport, file_name: &str) {
    let actual = report.parse_count_for(file_name);
    assert_eq!(
        actual, 1,
        "piece {file_name} was expected to be parsed exactly once, but report records {actual} parse(s)"
    );
}

fn assert_resolver_called_once(resolver: &CountingResolver, file_name: &str) {
    let actual = resolver.call_count_for(file_name);
    assert_eq!(
        actual, 1,
        "piece {file_name} was expected to call the resolver exactly once, but it was called {actual} time(s)"
    );
}

fn triangle_piece(temperature: Vec<f32>, components: u32, material: i32) -> UnstructuredGridPiece {
    UnstructuredGridPiece {
        points: IOBuffer::F32(vec![0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0]),
        cells: Cells {
            cell_verts: VertexNumbers::XML {
                connectivity: vec![0, 1, 2],
                offsets: vec![3],
            },
            types: vec![CellType::Triangle],
        },
        data: Attributes {
            point: vec![
                Attribute::generic("temperature", components).with_data(IOBuffer::F32(temperature))
            ],
            cell: vec![Attribute::scalars("material", 1).with_data(IOBuffer::I32(vec![material]))],
        },
    }
}

fn triangle_vtk(temperature: Vec<f32>, material: i32) -> Vtk {
    triangle_vtk_with_components(temperature, 1, material)
}

fn triangle_vtk_with_components(temperature: Vec<f32>, components: u32, material: i32) -> Vtk {
    Vtk {
        version: Version::new_xml(2, 0),
        title: "resolver-test".to_string(),
        byte_order: ByteOrder::LittleEndian,
        file_path: None,
        data: DataSet::inline(triangle_piece(temperature, components, material)),
    }
}

fn write_vtu(path: &Path, vtk: Vtk, compressor: Compressor) {
    let level = if compressor == Compressor::None { 0 } else { 6 };
    let xml: VTKFile = vtk
        .try_into_xml_format(compressor, level)
        .expect("convert sample Vtk to XML model");
    xml.export(path)
        .unwrap_or_else(|e| panic!("write sample VTU {}: {e}", path.display()));
}

fn write_plain_vtu(path: &Path, temperature: Vec<f32>, material: i32) {
    write_vtu(path, triangle_vtk(temperature, material), Compressor::None);
}

fn pvtu_xml(sources: &[&str]) -> String {
    let piece_elements = sources
        .iter()
        .map(|source| format!(r#"    <Piece Source="{source}"/>"#))
        .collect::<Vec<_>>()
        .join("\n");
    format!(
        r#"<VTKFile type="PUnstructuredGrid" version="2.0" byte_order="LittleEndian">
  <PUnstructuredGrid GhostLevel="0">
    <PPointData>
      <PDataArray type="Float32" Name="temperature" NumberOfComponents="1"/>
    </PPointData>
    <PCellData>
      <PDataArray type="Int32" Name="material" NumberOfComponents="1"/>
    </PCellData>
    <PPoints>
      <PDataArray type="Float32" Name="Points" NumberOfComponents="3"/>
    </PPoints>
{piece_elements}
  </PUnstructuredGrid>
</VTKFile>
"#
    )
}

fn write_pvtu(path: &Path, sources: &[&str]) {
    write_file(path, &pvtu_xml(sources));
}

fn unstructured_pieces(vtk: &Vtk) -> &[Piece<UnstructuredGridPiece>] {
    match &vtk.data {
        DataSet::UnstructuredGrid { pieces, .. } => pieces,
        other => panic!("expected UnstructuredGrid, got {}", other.kind()),
    }
}

fn expect_inline_triangle(piece: &Piece<UnstructuredGridPiece>) -> (Vec<f32>, i32) {
    let Piece::Inline(piece) = piece else {
        panic!("expected merged Piece::Inline, got {piece:?}");
    };
    let temperature = piece
        .data
        .point
        .iter()
        .find(|attr| attr.name() == "temperature")
        .expect("temperature point attribute exists");
    let Attribute::DataArray(array) = temperature else {
        panic!("temperature should be a DataArray");
    };
    let values = array
        .data
        .clone()
        .into_vec::<f32>()
        .expect("temperature values are f32");
    let material = piece
        .data
        .cell
        .iter()
        .find(|attr| attr.name() == "material")
        .expect("material cell attribute exists");
    let Attribute::DataArray(array) = material else {
        panic!("material should be a DataArray");
    };
    let material = array
        .data
        .clone()
        .into_vec::<i32>()
        .expect("material values are i32")[0];
    (values, material)
}

#[test]
fn duplicate_reference_is_resolved_and_parsed_once_but_reported_twice() -> Result<(), BoxError> {
    let dir = temp_dir("duplicate");
    let piece_path = dir.join("same.vtu");
    write_plain_vtu(&piece_path, vec![10.0, 20.0, 30.0], 7);

    let pvt_path = dir.join("duplicate.pvtu");
    write_pvtu(&pvt_path, &["./same.vtu", "nested/../same.vtu"]);

    let calls = Arc::new(Mutex::new(Vec::new()));
    let resolver = CountingResolver::new(&dir, calls.clone());
    let (vtk, report) = Vtk::import_with_piece_resolver(&pvt_path, resolver)?;

    assert_resolver_called_once(&CountingResolver::new(&dir, calls.clone()), "same.vtu");
    assert_eq!(
        calls
            .lock()
            .unwrap()
            .iter()
            .filter(|x| x.as_str() == "same.vtu")
            .count(),
        1,
        "same.vtu expected one resolver call; calls were {:?}",
        *calls.lock().unwrap()
    );
    assert_source_parsed_once(&report, "same.vtu");
    assert_eq!(
        report.reference_count_for("same.vtu"),
        2,
        "report must retain both references to same.vtu"
    );
    assert_eq!(report.resolver_calls, 1);
    assert_eq!(report.parse_calls, 1);
    assert_eq!(report.cache_hits, 1);
    assert_eq!(report.merged_ids().len(), 2);

    let pieces = unstructured_pieces(&vtk);
    assert_eq!(
        pieces.len(),
        2,
        "both declarations should be merged in declaration order"
    );
    for piece in pieces {
        let (temperature, material) = expect_inline_triangle(piece);
        assert_eq!(temperature, [10.0, 20.0, 30.0]);
        assert_eq!(material, 7);
    }
    Ok(())
}

#[test]
fn two_branches_sharing_piece_parse_shared_source_once() -> Result<(), BoxError> {
    let dir = temp_dir("shared-branches");
    fs::create_dir_all(dir.join("branches"))?;
    fs::create_dir_all(dir.join("shared"))?;

    write_plain_vtu(&dir.join("shared/shared.vtu"), vec![1.5, 2.5, 3.5], 11);
    write_pvtu(
        &dir.join("branches/branch_a.pvtu"),
        &["../shared/shared.vtu"],
    );
    write_pvtu(
        &dir.join("branches/branch_b.pvtu"),
        &["../shared/shared.vtu"],
    );

    let root_path = dir.join("shared_root.pvtu");
    write_pvtu(
        &root_path,
        &["branches/branch_a.pvtu", "branches/branch_b.pvtu"],
    );

    let calls = Arc::new(Mutex::new(Vec::new()));
    let resolver = CountingResolver::new(&dir, calls.clone());
    let (vtk, report) = Vtk::import_with_piece_resolver(&root_path, resolver)?;

    let shared_calls = calls
        .lock()
        .unwrap()
        .iter()
        .filter(|call| call.as_str() == "shared.vtu")
        .count();
    assert_eq!(
        shared_calls, 1,
        "shared.vtu was expected to call the resolver exactly once from two branches, actual calls: {shared_calls}; all calls: {:?}",
        *calls.lock().unwrap()
    );
    assert_source_parsed_once(&report, "shared.vtu");
    assert_eq!(
        report.reference_count_for("shared.vtu"),
        2,
        "report should show both branch references to shared.vtu"
    );
    assert_eq!(report.merged_pieces().len(), 2);

    let pieces = unstructured_pieces(&vtk);
    assert_eq!(pieces.len(), 2);
    for piece in pieces {
        let (temperature, material) = expect_inline_triangle(piece);
        assert_eq!(temperature, [1.5, 2.5, 3.5]);
        assert_eq!(material, 11);
    }
    Ok(())
}

#[test]
fn merged_pieces_follow_declaration_order_not_filesystem_order() -> Result<(), BoxError> {
    let dir = temp_dir("declaration-order");
    write_plain_vtu(&dir.join("z_first.vtu"), vec![100.0, 101.0, 102.0], 1);
    write_plain_vtu(&dir.join("a_second.vtu"), vec![200.0, 201.0, 202.0], 2);
    let pvt = dir.join("order.pvtu");
    write_pvtu(&pvt, &["z_first.vtu", "a_second.vtu"]);

    let (vtk, report) = Vtk::import_with_piece_resolver(&pvt, LocalPieceResolver::new(&dir)?)?;

    let pieces = unstructured_pieces(&vtk);
    assert_eq!(pieces.len(), 2);
    let (first, first_material) = expect_inline_triangle(&pieces[0]);
    let (second, second_material) = expect_inline_triangle(&pieces[1]);
    assert_eq!(
        first,
        [100.0, 101.0, 102.0],
        "first declared piece must be first merged"
    );
    assert_eq!(first_material, 1);
    assert_eq!(
        second,
        [200.0, 201.0, 202.0],
        "second declared piece must be second merged"
    );
    assert_eq!(second_material, 2);

    let mut legacy = Vtk::import(&pvt)?;
    legacy.load_all_pieces()?;
    assert_eq!(
        vtk, legacy,
        "resolver-based merge should produce the same existing Vtk model as load_all_pieces"
    );

    assert_eq!(
        report
            .merged_pieces()
            .iter()
            .map(|p| p.id)
            .collect::<Vec<_>>(),
        report.merged_ids()
    );
    Ok(())
}

#[test]
fn cycle_error_contains_complete_reference_chain_and_pending_later_piece() -> Result<(), BoxError> {
    let dir = temp_dir("cycle");
    write_pvtu(
        &dir.join("cycle_a.pvtu"),
        &["cycle_b.pvtu", "not_reached.vtu"],
    );
    write_pvtu(&dir.join("cycle_b.pvtu"), &["./cycle_a.pvtu"]);

    let result =
        Vtk::import_with_piece_resolver(dir.join("cycle_a.pvtu"), LocalPieceResolver::new(&dir)?);

    let err = result.expect_err("cyclic references must fail");
    let message = err.to_string();
    let chain_text = message
        .split("piece reference cycle detected: ")
        .nth(1)
        .expect("cycle error should include the chain prefix");
    let chain_names: Vec<String> = chain_text
        .split(" -> ")
        .map(|entry| {
            Path::new(entry.trim())
                .file_name()
                .expect("chain entry has a file name")
                .to_string_lossy()
                .to_string()
        })
        .collect();
    assert_eq!(
        chain_names,
        [
            "cycle_a.pvtu",
            "cycle_b.pvtu",
            "cycle_a.pvtu",
            "cycle_b.pvtu"
        ],
        "cycle error must contain the complete reference chain; full message: {message}"
    );
    assert!(
        matches!(err.failure, PieceLoadFailure::Cycle { .. }),
        "expected PieceLoadFailure::Cycle, got {:?}",
        err.failure
    );
    let pending = err.report.pending_pieces();
    assert!(
        pending
            .iter()
            .any(|p| p.uri.as_deref() == Some("not_reached.vtu")),
        "report must show not_reached.vtu as pending; report: {:#?}",
        err.report.pieces
    );
    Ok(())
}

#[test]
fn missing_file_reports_failed_current_processed_previous_and_pending_next() -> Result<(), BoxError>
{
    let dir = temp_dir("missing");
    write_plain_vtu(&dir.join("good.vtu"), vec![1.0, 2.0, 3.0], 1);
    write_plain_vtu(&dir.join("later.vtu"), vec![4.0, 5.0, 6.0], 3);
    let pvt = dir.join("missing.pvtu");
    write_pvtu(&pvt, &["good.vtu", "missing.vtu", "later.vtu"]);

    let calls = Arc::new(Mutex::new(Vec::new()));
    let resolver = CountingResolver::new(&dir, calls.clone());
    let result = Vtk::import_with_piece_resolver(&pvt, resolver);

    let err = result.expect_err("missing piece must fail under FailFast policy");
    assert!(
        err.to_string().contains("missing.vtu") && err.to_string().contains("not found"),
        "error should identify missing.vtu and not-found cause; got: {}",
        err
    );
    assert_eq!(err.report.pieces.len(), 3);
    assert_eq!(err.report.pieces[0].status, PieceStatus::Validated);
    assert!(err.report.pieces[0].processed);
    assert!(
        !err.report.pieces[0].merged,
        "fatal load returns no merged Vtk"
    );
    assert_eq!(err.report.pieces[1].status, PieceStatus::Failed);
    assert!(err.report.pieces[1]
        .error
        .as_deref()
        .unwrap_or("")
        .contains("missing.vtu"));
    assert_eq!(err.report.pieces[2].status, PieceStatus::Pending);
    assert!(!err.report.pieces[2].processed);
    assert_eq!(
        calls
            .lock()
            .unwrap()
            .iter()
            .filter(|x| x.as_str() == "missing.vtu")
            .count(),
        1
    );

    // The existing already-imported entry point must not be left half-merged.
    let mut vtk = Vtk::import(&pvt)?;
    let before = vtk.data.clone();
    let resolver = CountingResolver::new(&dir, Arc::new(Mutex::new(Vec::new())));
    let in_place_err = vtk.load_pieces_with_resolver(resolver).unwrap_err();
    assert!(in_place_err.report.pieces[1].error.is_some());
    assert_eq!(
        vtk.data, before,
        "failed in-place load must preserve original Vtk data"
    );
    Ok(())
}

#[test]
fn continue_policy_skips_missing_piece_and_reports_actual_merges() -> Result<(), BoxError> {
    let dir = temp_dir("continue-missing");
    write_plain_vtu(&dir.join("good.vtu"), vec![1.0, 2.0, 3.0], 1);
    write_plain_vtu(&dir.join("later.vtu"), vec![4.0, 5.0, 6.0], 3);
    let pvt = dir.join("continue.pvtu");
    write_pvtu(&pvt, &["good.vtu", "missing.vtu", "later.vtu"]);

    let calls = Arc::new(Mutex::new(Vec::new()));
    let resolver = CountingResolver::new(&dir, calls);
    let outcome = PieceLoader::new(resolver).continue_on_errors().load(&pvt)?;

    assert_eq!(outcome.report.merged_pieces().len(), 2);
    assert_eq!(outcome.report.skipped_pieces().len(), 1);
    assert_eq!(
        outcome.report.skipped_pieces()[0].uri.as_deref(),
        Some("missing.vtu")
    );
    let pieces = unstructured_pieces(&outcome.vtk);
    assert_eq!(
        pieces.len(),
        2,
        "only good.vtu and later.vtu should be merged"
    );
    let (first, _) = expect_inline_triangle(&pieces[0]);
    let (second, _) = expect_inline_triangle(&pieces[1]);
    assert_eq!(first, [1.0, 2.0, 3.0]);
    assert_eq!(second, [4.0, 5.0, 6.0]);
    Ok(())
}

#[test]
fn schema_component_mismatch_fails_before_merge_and_rerun_rereads_fixed_input(
) -> Result<(), BoxError> {
    let dir = temp_dir("schema-conflict");
    write_plain_vtu(&dir.join("good1.vtu"), vec![1.0, 2.0, 3.0], 1);
    write_vtu(
        &dir.join("bad_components.vtu"),
        triangle_vtk_with_components(vec![10.0, 11.0, 20.0, 21.0, 30.0, 31.0], 2, 2),
        Compressor::None,
    );
    write_plain_vtu(&dir.join("good3.vtu"), vec![4.0, 5.0, 6.0], 3);

    let pvt = dir.join("schema.pvtu");
    write_pvtu(&pvt, &["good1.vtu", "bad_components.vtu", "good3.vtu"]);

    let calls = Arc::new(Mutex::new(Vec::new()));
    let resolver = CountingResolver::new(&dir, calls.clone());
    let mut loader = PieceLoader::new(resolver);
    let first = loader.load(&pvt);
    let err = first.expect_err("component count conflict must fail");
    let message = err.to_string();
    assert!(
        message.contains("temperature") && message.contains("component count mismatch"),
        "schema error should name temperature and component mismatch; got: {message}"
    );
    assert_eq!(err.report.pieces[0].status, PieceStatus::Validated);
    assert_eq!(err.report.pieces[1].status, PieceStatus::Failed);
    assert_eq!(err.report.pieces[2].status, PieceStatus::Pending);
    assert!(err.report.pieces[1]
        .error
        .as_deref()
        .unwrap_or("")
        .contains("component count mismatch"));

    let calls_after_failure = calls.lock().unwrap().len();
    assert_eq!(
        calls_after_failure, 2,
        "first run should resolve first two pieces"
    );

    // Fix the bad input after the failed run.  The loader must not retain a
    // cache from the failed load.
    write_plain_vtu(&dir.join("bad_components.vtu"), vec![10.0, 20.0, 30.0], 22);
    let outcome = loader.load(&pvt)?;

    let calls_after_rerun = calls.lock().unwrap().len();
    assert_eq!(
        calls_after_rerun,
        calls_after_failure + 3,
        "fixed rerun was expected to re-resolve all three declarations instead of using the previous run's cache; before rerun={calls_after_failure}, after={calls_after_rerun}"
    );
    assert_eq!(outcome.report.merged_pieces().len(), 3);
    assert_eq!(unstructured_pieces(&outcome.vtk).len(), 3);
    Ok(())
}

#[test]
fn pieces_with_different_compression_are_parsed_and_merged() -> Result<(), BoxError> {
    let dir = temp_dir("compression");
    let plain = dir.join("plain.vtu");
    let zlib = dir.join("zlib.vtu");
    let lz4 = dir.join("lz4.vtu");

    write_vtu(
        &plain,
        triangle_vtk(vec![1.0, 2.0, 3.0], 1),
        Compressor::None,
    );
    write_vtu(
        &zlib,
        triangle_vtk(vec![4.0, 5.0, 6.0], 2),
        Compressor::ZLib,
    );
    write_vtu(&lz4, triangle_vtk(vec![7.0, 8.0, 9.0], 3), Compressor::LZ4);

    // Prove through the library-written XML that the samples really use
    // different compressors.
    let zlib_xml = fs::read_to_string(&zlib)?;
    let lz4_xml = fs::read_to_string(&lz4)?;
    assert!(zlib_xml.contains("vtkZLibDataCompressor"));
    assert!(lz4_xml.contains("vtkLZ4DataCompressor"));

    let pvt = dir.join("compressed.pvtu");
    write_pvtu(&pvt, &["plain.vtu", "zlib.vtu", "lz4.vtu"]);

    let (vtk, report) = Vtk::import_with_piece_resolver(&pvt, LocalPieceResolver::new(&dir)?)?;

    assert_eq!(report.merged_pieces().len(), 3);
    let pieces = unstructured_pieces(&vtk);
    assert_eq!(pieces.len(), 3);
    let expected = [
        ([1.0, 2.0, 3.0], 1),
        ([4.0, 5.0, 6.0], 2),
        ([7.0, 8.0, 9.0], 3),
    ];
    for ((values, material), piece) in expected.into_iter().zip(pieces) {
        let (actual_values, actual_material) = expect_inline_triangle(piece);
        assert_eq!(actual_values, values);
        assert_eq!(actual_material, material);
    }
    Ok(())
}

#[test]
fn local_resolver_rejects_root_escape_and_network_uris() -> Result<(), BoxError> {
    let base = temp_dir("root-boundary");
    let root = base.join("root");
    fs::create_dir_all(&root)?;
    write_plain_vtu(&base.join("outside.vtu"), vec![1.0, 2.0, 3.0], 1);
    write_pvtu(&root.join("escape.pvtu"), &["../outside.vtu"]);

    let err =
        Vtk::import_with_piece_resolver(root.join("escape.pvtu"), LocalPieceResolver::new(&root)?)
            .expect_err("relative path escaping root must be rejected");
    assert!(
        err.to_string().contains("outside.vtu") && err.to_string().contains("not allowed"),
        "expected root escape denial, got: {err}"
    );

    write_pvtu(
        &root.join("network.pvtu"),
        &["https://example.invalid/piece.vtu"],
    );
    let err =
        Vtk::import_with_piece_resolver(root.join("network.pvtu"), LocalPieceResolver::new(&root)?)
            .expect_err("network URI must be rejected by LocalPieceResolver");
    assert!(
        err.to_string().contains("network") || err.to_string().contains("Network"),
        "expected network denial, got: {err}"
    );
    Ok(())
}

#[test]
fn pvtr_and_pvtp_entries_load_through_same_resolver_api() -> Result<(), BoxError> {
    let dir = temp_dir("pvtr-pvtp");

    let extent = Extent::Ranges([0..=1, 0..=0, 0..=0]);
    let rect = Vtk {
        version: Version::new_xml(2, 0),
        title: String::new(),
        byte_order: ByteOrder::LittleEndian,
        file_path: None,
        data: DataSet::RectilinearGrid {
            extent: extent.clone(),
            meta: None,
            pieces: vec![Piece::Inline(Box::new(RectilinearGridPiece {
                extent,
                coords: Coordinates {
                    x: IOBuffer::F32(vec![10.0, 20.0]),
                    y: IOBuffer::F32(vec![0.0]),
                    z: IOBuffer::F32(vec![0.0]),
                },
                data: Attributes::new(),
            }))],
        },
    };
    rect.export(dir.join("grid.vtr"))?;
    write_file(
        &dir.join("grid.pvtr"),
        r#"<VTKFile type="PRectilinearGrid" version="2.0" byte_order="LittleEndian">
  <PRectilinearGrid GhostLevel="0" WholeExtent="0 1 0 0 0 0">
    <PCoordinates>
      <PDataArray type="Float32" Name="x" NumberOfComponents="1"/>
      <PDataArray type="Float32" Name="y" NumberOfComponents="1"/>
      <PDataArray type="Float32" Name="z" NumberOfComponents="1"/>
    </PCoordinates>
    <Piece Extent="0 1 0 0 0 0" Source="grid.vtr"/>
  </PRectilinearGrid>
</VTKFile>"#,
    );

    let (vtk, report) =
        Vtk::import_with_piece_resolver(dir.join("grid.pvtr"), LocalPieceResolver::new(&dir)?)?;
    let DataSet::RectilinearGrid { pieces, .. } = vtk.data else {
        panic!("expected merged RectilinearGrid");
    };
    assert_eq!(pieces.len(), 1);
    assert!(matches!(pieces[0], Piece::Inline(_)));
    assert_eq!(report.merged_pieces().len(), 1);

    let poly = Vtk {
        version: Version::new_xml(2, 0),
        title: String::new(),
        byte_order: ByteOrder::LittleEndian,
        file_path: None,
        data: DataSet::PolyData {
            meta: None,
            pieces: vec![Piece::Inline(Box::new(PolyDataPiece {
                points: IOBuffer::F32(vec![0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0]),
                polys: Some(VertexNumbers::XML {
                    connectivity: vec![0, 1, 2],
                    offsets: vec![3],
                }),
                ..Default::default()
            }))],
        },
    };
    poly.export(dir.join("mesh.vtp"))?;
    write_file(
        &dir.join("mesh.pvtp"),
        r#"<VTKFile type="PPolyData" version="2.0" byte_order="LittleEndian">
  <PPolyData GhostLevel="0">
    <PPoints>
      <PDataArray type="Float32" Name="Points" NumberOfComponents="3"/>
    </PPoints>
    <Piece Source="mesh.vtp"/>
  </PPolyData>
</VTKFile>"#,
    );

    let (vtk, report) =
        Vtk::import_with_piece_resolver(dir.join("mesh.pvtp"), LocalPieceResolver::new(&dir)?)?;
    let DataSet::PolyData { pieces, .. } = vtk.data else {
        panic!("expected merged PolyData");
    };
    assert_eq!(pieces.len(), 1);
    assert!(matches!(pieces[0], Piece::Inline(_)));
    assert_eq!(report.merged_pieces().len(), 1);
    Ok(())
}

#[test]
fn byte_order_and_dataset_kind_mismatches_are_fatal_and_reported() -> Result<(), BoxError> {
    let dir = temp_dir("validation");
    write_plain_vtu(&dir.join("good.vtu"), vec![1.0, 2.0, 3.0], 1);

    let mut big_endian = triangle_vtk(vec![4.0, 5.0, 6.0], 2);
    big_endian.byte_order = ByteOrder::BigEndian;
    write_vtu(&dir.join("big_endian.vtu"), big_endian, Compressor::None);
    write_plain_vtu(&dir.join("later.vtu"), vec![7.0, 8.0, 9.0], 3);

    let pvt = dir.join("byte-order.pvtu");
    write_pvtu(&pvt, &["good.vtu", "big_endian.vtu", "later.vtu"]);
    let err = Vtk::import_with_piece_resolver(&pvt, LocalPieceResolver::new(&dir)?)
        .expect_err("different byte orders must be rejected");
    assert!(
        matches!(err.failure, PieceLoadFailure::ByteOrderMismatch { .. }),
        "expected byte order mismatch, got {err:?}"
    );
    assert_eq!(err.report.pieces[0].status, PieceStatus::Validated);
    assert_eq!(err.report.pieces[1].status, PieceStatus::Failed);
    assert_eq!(err.report.pieces[2].status, PieceStatus::Pending);

    let wrong_kind = Vtk {
        version: Version::new_xml(2, 0),
        title: String::new(),
        byte_order: ByteOrder::LittleEndian,
        file_path: None,
        data: DataSet::PolyData {
            meta: None,
            pieces: vec![Piece::Inline(Box::new(PolyDataPiece {
                points: IOBuffer::F32(vec![0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0]),
                polys: Some(VertexNumbers::XML {
                    connectivity: vec![0, 1, 2],
                    offsets: vec![3],
                }),
                ..Default::default()
            }))],
        },
    };
    write_vtu(&dir.join("wrong_kind.vtp"), wrong_kind, Compressor::None);
    let kind_pvt = dir.join("kind.pvtu");
    write_pvtu(&kind_pvt, &["good.vtu", "wrong_kind.vtp"]);
    let err = Vtk::import_with_piece_resolver(&kind_pvt, LocalPieceResolver::new(&dir)?)
        .expect_err("dataset kind mismatch must be rejected");
    assert!(
        matches!(err.failure, PieceLoadFailure::KindMismatch { .. }),
        "expected dataset kind mismatch, got {err:?}"
    );
    assert_eq!(err.report.pieces[1].status, PieceStatus::Failed);
    Ok(())
}

#[test]
fn different_resolver_instances_keep_independent_call_counts() -> Result<(), BoxError> {
    let base = temp_dir("independent-caches");
    let root_a = base.join("root-a");
    let root_b = base.join("root-b");
    fs::create_dir_all(&root_a)?;
    fs::create_dir_all(&root_b)?;

    write_plain_vtu(&root_a.join("piece.vtu"), vec![1.0, 2.0, 3.0], 1);
    write_plain_vtu(&root_b.join("piece.vtu"), vec![4.0, 5.0, 6.0], 2);
    write_pvtu(&root_a.join("parallel.pvtu"), &["piece.vtu"]);
    write_pvtu(&root_b.join("parallel.pvtu"), &["piece.vtu"]);

    let calls_a = Arc::new(Mutex::new(Vec::new()));
    let calls_b = Arc::new(Mutex::new(Vec::new()));
    let resolver_a = CountingResolver::new(&root_a, calls_a.clone());
    let resolver_b = CountingResolver::new(&root_b, calls_b.clone());

    let (vtk_a, report_a) =
        Vtk::import_with_piece_resolver(root_a.join("parallel.pvtu"), &resolver_a)?;
    let (vtk_b, report_b) =
        Vtk::import_with_piece_resolver(root_b.join("parallel.pvtu"), &resolver_b)?;

    assert_eq!(calls_a.lock().unwrap().len(), 1);
    assert_eq!(calls_b.lock().unwrap().len(), 1);
    assert_eq!(report_a.parse_calls, 1);
    assert_eq!(report_b.parse_calls, 1);

    let piece_a = unstructured_pieces(&vtk_a);
    let piece_b = unstructured_pieces(&vtk_b);
    let (values_a, _) = expect_inline_triangle(&piece_a[0]);
    let (values_b, _) = expect_inline_triangle(&piece_b[0]);
    assert_eq!(values_a, [1.0, 2.0, 3.0]);
    assert_eq!(values_b, [4.0, 5.0, 6.0]);
    Ok(())
}
