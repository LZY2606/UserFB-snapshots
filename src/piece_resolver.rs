//! Customizable resolution and loading of pieces referenced by "parallel" VTK XML files
//! (`.pvtu`, `.pvtp`, `.pvtr`, `.pvti` and `.pvts`).
//!
//! The [`PieceResolver`] trait gives callers full control over where the contents of a referenced
//! piece come from (a local file, an in-memory cache, an object store, ...). Every resolution is
//! recorded in a [`LoadReport`], which describes where each piece came from, how much data it
//! contains and whether it was ultimately merged into the resulting [`Vtk`] model.
//!
//! The [`LocalPieceResolver`] is used by default and keeps the historical behavior: only local
//! files underneath a configured root directory are read. Both path traversal outside of that root
//! and network style URIs are rejected.
//!
//! [`Vtk`]: crate::model::Vtk

use std::collections::HashMap;
use std::path::{Component, Path, PathBuf};

use crate::model::{Attribute, Attributes, DataSet, Piece, PieceData, PolyDataPiece, Vtk};

/// A request issued to a [`PieceResolver`].
///
/// The `uri` is exactly the string stored in the `Source` attribute of the referencing file, while
/// `base_path` is the path of the file containing the reference (if any). Relative URIs must be
/// resolved against the parent directory of `base_path`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PieceRequest<'a> {
    /// The raw `Source` URI of the referenced piece.
    pub uri: &'a str,
    /// The path of the file referencing this piece.
    pub base_path: Option<&'a Path>,
    /// The chain of normalized piece URIs leading to this request, starting at the root parallel
    /// file. The currently requested URI is *not* included.
    pub reference_chain: &'a [String],
}

/// Where the bytes of a piece came from.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum PieceOrigin {
    /// The resolver read and parsed the underlying resource.
    Loaded {
        /// The location the resolver actually read from.
        location: String,
    },
    /// The bytes were already cached, so the resolver was not contacted.
    Cached {
        /// The location from which the piece was originally loaded.
        location: String,
    },
}

/// The outcome of processing a single piece reference.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum PieceStatus {
    /// The reference has been declared but not processed yet.
    ///
    /// When loading fails, every reference after the failing one remains in this state, making the
    /// boundary between processed and unprocessed pieces visible in the [`LoadReport`].
    Pending,
    /// The piece was parsed and merged into the resulting [`Vtk`] model.
    Merged,
    /// The piece was loaded and validated, but not merged into the [`Vtk`] model.
    ///
    /// This is used for recoverable issues. The reason explains why the piece was skipped.
    Skipped {
        /// Human readable reason explaining why the piece was not merged.
        reason: String,
    },
    /// Loading or validation of this piece failed.
    Failed {
        /// Human readable description of the failure.
        reason: String,
    },
}

/// An entry in a [`LoadReport`] describing one piece reference.
///
/// A single physical piece referenced from several places produces one entry per reference: the
/// first one records the actual load while subsequent ones report a cache hit.
#[derive(Clone, Debug, PartialEq)]
pub struct PieceReport {
    /// Zero based index of the reference in the order in which references are visited.
    pub index: usize,
    /// The raw `Source` URI as written in the referencing file.
    pub source_uri: String,
    /// The normalized identity of the piece (used for de-duplication and caching).
    pub resolved_uri: String,
    /// The path of the file containing the reference, if known.
    pub referenced_from: Option<PathBuf>,
    /// The full chain of normalized piece URIs leading to this one, starting at the root file.
    pub reference_chain: Vec<String>,
    /// Whether the resource was actually read or served from a cache. `None` while the reference is
    /// pending or when loading failed before any bytes were obtained.
    pub origin: Option<PieceOrigin>,
    /// Number of points stored in the piece, if the piece was parsed successfully.
    pub num_points: Option<usize>,
    /// Number of cells stored in the piece, if the piece was parsed successfully.
    pub num_cells: Option<usize>,
    /// Final processing status of the piece.
    pub status: PieceStatus,
}

/// A report of everything that happened while loading the pieces of a parallel VTK file.
///
/// A report is returned together with the loaded [`Vtk`] model on success, and is also accessible
/// through [`LoadError::report`] when loading fails. Entries are stored in declaration order.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct LoadReport {
    /// One entry per piece reference, in the order in which the pieces are declared.
    pub pieces: Vec<PieceReport>,
}

impl LoadReport {
    /// Number of piece references recorded in this report.
    pub fn len(&self) -> usize {
        self.pieces.len()
    }

    /// Returns `true` if no piece references were recorded.
    pub fn is_empty(&self) -> bool {
        self.pieces.is_empty()
    }

    /// Number of times a resolver actually fetched a piece (as opposed to serving a cache hit).
    pub fn resolver_load_count(&self) -> usize {
        self.pieces
            .iter()
            .filter(|p| matches!(p.origin, Some(PieceOrigin::Loaded { .. })))
            .count()
    }

    /// Number of references served from the cache without contacting the resolver.
    pub fn cache_hit_count(&self) -> usize {
        self.pieces
            .iter()
            .filter(|p| matches!(p.origin, Some(PieceOrigin::Cached { .. })))
            .count()
    }

    /// Number of pieces that were ultimately merged into the [`Vtk`] model.
    pub fn merged_count(&self) -> usize {
        self.pieces
            .iter()
            .filter(|p| p.status == PieceStatus::Merged)
            .count()
    }

    /// Number of references that were loaded but deliberately not merged.
    pub fn skipped_count(&self) -> usize {
        self.pieces
            .iter()
            .filter(|p| matches!(p.status, PieceStatus::Skipped { .. }))
            .count()
    }

    /// Reports of pieces processed before the first failure, if any.
    pub fn processed_pieces(&self) -> &[PieceReport] {
        match self
            .pieces
            .iter()
            .position(|p| matches!(p.status, PieceStatus::Failed { .. }))
        {
            Some(i) => &self.pieces[..i],
            None => &self.pieces,
        }
    }

    /// Reports of pieces that had not been processed when the first failure occurred.
    pub fn pending_pieces(&self) -> &[PieceReport] {
        match self
            .pieces
            .iter()
            .position(|p| matches!(p.status, PieceStatus::Failed { .. }))
        {
            Some(i) => &self.pieces[i + 1..],
            None => &[],
        }
    }

    /// The report of the piece that caused the failure, if loading failed.
    pub fn failed_piece(&self) -> Option<&PieceReport> {
        self.pieces
            .iter()
            .find(|p| matches!(p.status, PieceStatus::Failed { .. }))
    }
}

/// Error returned when parallel piece loading fails.
///
/// The [`LoadReport`] is always populated so that callers can inspect which pieces were already
/// processed, which one failed and which were never reached.
#[derive(Debug, Clone)]
pub struct LoadError {
    /// Human readable description of the failure.
    pub message: String,
    /// The report collected up to and including the failure.
    pub report: LoadReport,
}

impl LoadError {
    /// Build a resolver-side failure from a message and an (usually empty) report.
    ///
    /// The loader replaces the report with the full partial report of the current load before
    /// returning the error.
    pub fn new(message: impl Into<String>, report: LoadReport) -> Self {
        LoadError {
            message: message.into(),
            report,
        }
    }
}

impl std::fmt::Display for LoadError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.message)
    }
}

impl std::error::Error for LoadError {}

impl From<LoadError> for crate::model::Error {
    fn from(e: LoadError) -> Self {
        crate::model::Error::PieceLoad(e.message)
    }
}

/// Resolves the contents of pieces referenced by parallel VTK XML files.
///
/// Implementors decide where the bytes of a piece come from. The loader only calls
/// [`resolve`](PieceResolver::resolve) once per normalized piece URI within a single load: repeated
/// references are served from an internal cache owned by that load. A new load always starts with
/// an empty cache, so failed loads never poison subsequent attempts.
pub trait PieceResolver {
    /// Fetch the raw bytes of the piece described by `request`.
    ///
    /// The returned [`ResolvedPiece`] identifies the concrete location the bytes were read from
    /// and carries the unmodified file contents.
    fn resolve(&self, request: &PieceRequest<'_>) -> Result<ResolvedPiece, LoadError>;
}

/// The bytes of a piece together with the location they were read from.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ResolvedPiece {
    /// Concrete location the bytes were loaded from (a file path, a URL, ...).
    pub location: String,
    /// Raw, unparsed contents of the piece.
    pub bytes: Vec<u8>,
}

impl ResolvedPiece {
    /// Build a resolved piece from a location and its contents.
    pub fn new(location: impl Into<String>, bytes: Vec<u8>) -> ResolvedPiece {
        ResolvedPiece {
            location: location.into(),
            bytes,
        }
    }
}

/// Default [`PieceResolver`] that only reads local files underneath a root directory.
///
/// This preserves the historical loading behavior of [`Vtk::import`](crate::Vtk::import) while
/// adding safety checks:
///
/// - relative URIs are joined to the directory of the referencing file,
/// - the normalized path must stay within the root directory,
/// - absolute paths escaping the root and network style URIs (`scheme://...`) are rejected.
///
/// Each load keeps its own cache, so two resolvers (or the same resolver with two different roots)
/// never share loaded pieces.
#[derive(Clone, Debug)]
pub struct LocalPieceResolver {
    root: PathBuf,
}

impl LocalPieceResolver {
    /// Create a resolver rooted at `root`.
    ///
    /// Only files whose normalized path stays within `root` can be loaded.
    pub fn new(root: impl Into<PathBuf>) -> Self {
        LocalPieceResolver { root: root.into() }
    }

    /// The root directory this resolver is confined to.
    pub fn root(&self) -> &Path {
        &self.root
    }

    /// Join a possibly relative piece URI onto the referencing file and normalize the result,
    /// rejecting network URIs and anything escaping the root directory.
    fn resolve_path(&self, uri: &str, base_path: Option<&Path>) -> Result<PathBuf, String> {
        if is_network_uri(uri) {
            return Err(format!(
                "local piece resolver refuses network URI {:?}: only local files under {:?} are allowed",
                uri, self.root
            ));
        }

        let path = Path::new(uri);
        let joined = if path.is_absolute() {
            path.to_path_buf()
        } else {
            match base_path.and_then(|p| p.parent()) {
                Some(parent) => parent.join(path),
                None => self.effective_root().join(path),
            }
        };

        let normalized = normalize_path(&joined);
        let normalized = if normalized.is_relative() {
            let cwd = std::env::current_dir().unwrap_or_else(|_| PathBuf::from("."));
            normalize_path(&cwd.join(normalized))
        } else {
            normalized
        };
        let root = self.effective_root_normalized();
        if !normalized.starts_with(&root) {
            return Err(format!(
                "piece {:?} resolves to {:?}, which is outside of the allowed root {:?}",
                uri, normalized, root
            ));
        }
        Ok(normalized)
    }

    /// Root with relative components (such as `.`) resolved against the current directory.
    fn effective_root(&self) -> PathBuf {
        if self.root.is_relative() {
            std::env::current_dir()
                .unwrap_or_else(|_| PathBuf::from("."))
                .join(&self.root)
        } else {
            self.root.clone()
        }
    }

    fn effective_root_normalized(&self) -> PathBuf {
        normalize_path(&self.effective_root())
    }
}

impl PieceResolver for LocalPieceResolver {
    fn resolve(&self, request: &PieceRequest<'_>) -> Result<ResolvedPiece, LoadError> {
        let path = self
            .resolve_path(request.uri, request.base_path)
            .map_err(|m| LoadError::new(m, LoadReport::default()))?;
        let bytes = std::fs::read(&path).map_err(|e| {
            LoadError::new(
                format!(
                    "failed to read piece {:?} at {:?}: {}",
                    request.uri, path, e
                ),
                LoadReport::default(),
            )
        })?;
        Ok(ResolvedPiece {
            location: path.to_string_lossy().into_owned(),
            bytes,
        })
    }
}

/// A network style URI contains an alphabetic scheme separator (e.g. `https://host/piece.vtu`).
fn is_network_uri(uri: &str) -> bool {
    let Some(scheme_end) = uri.find("://") else {
        return false;
    };
    scheme_end > 1
        && uri.as_bytes()[..scheme_end]
            .iter()
            .all(|b| b.is_ascii_alphabetic())
}

/// Lexically normalize a path: resolve `.` and `..` components without touching the file system.
///
/// A leading `..` that would escape the root is preserved so that containment checks can reject it.
fn normalize_path(path: &Path) -> PathBuf {
    let mut out = PathBuf::new();
    for component in path.components() {
        match component {
            Component::CurDir => {}
            Component::ParentDir => {
                if out.as_os_str().is_empty() || out.ends_with("..") {
                    out.push("..");
                } else {
                    out.pop();
                }
            }
            other => out.push(other.as_os_str()),
        }
    }
    out
}

/// Normalize a piece URI into a stable identity used for caching and cycle detection.
///
/// Relative URIs are interpreted against the parent directory of the referencing file. The
/// normalization is purely lexical, so no file system access is required.
fn normalize_uri(uri: &str, base_path: Option<&Path>) -> String {
    if is_network_uri(uri) || Path::new(uri).is_absolute() {
        return normalize_path(Path::new(uri))
            .to_string_lossy()
            .into_owned();
    }
    let joined = match base_path.and_then(|p| p.parent()) {
        Some(parent) => parent.join(uri),
        None => PathBuf::from(uri),
    };
    normalize_path(&joined).to_string_lossy().into_owned()
}

/// Snapshot of a parsed piece kept in the per-load cache.
#[derive(Clone, Debug)]
struct LoadedPiece {
    data_set: DataSet,
    byte_order: crate::model::ByteOrder,
    location: String,
    num_points: usize,
    num_cells: usize,
}

/// Uniform access to point and cell counts of the concrete piece types.
trait PieceSizes {
    fn piece_points(&self) -> usize;
    fn piece_cells(&self) -> usize;
}

impl PieceSizes for crate::model::ImageDataPiece {
    fn piece_points(&self) -> usize {
        self.extent.num_points() as usize
    }
    fn piece_cells(&self) -> usize {
        self.extent.num_cells() as usize
    }
}
impl PieceSizes for crate::model::StructuredGridPiece {
    fn piece_points(&self) -> usize {
        self.points.len() / 3
    }
    fn piece_cells(&self) -> usize {
        self.extent.num_cells() as usize
    }
}
impl PieceSizes for crate::model::RectilinearGridPiece {
    fn piece_points(&self) -> usize {
        self.extent.num_points() as usize
    }
    fn piece_cells(&self) -> usize {
        self.extent.num_cells() as usize
    }
}
impl PieceSizes for crate::model::UnstructuredGridPiece {
    fn piece_points(&self) -> usize {
        self.points.len() / 3
    }
    fn piece_cells(&self) -> usize {
        self.cells.num_cells()
    }
}
impl PieceSizes for crate::model::PolyDataPiece {
    fn piece_points(&self) -> usize {
        self.points.len() / 3
    }
    fn piece_cells(&self) -> usize {
        PolyDataPiece::num_cells(self)
    }
}

/// Uniform borrow of the point and cell attributes carried by every concrete piece type.
trait AttributeCarrier {
    fn piece_attributes(&self) -> &Attributes;
}

impl AttributeCarrier for crate::model::ImageDataPiece {
    fn piece_attributes(&self) -> &Attributes {
        &self.data
    }
}
impl AttributeCarrier for crate::model::StructuredGridPiece {
    fn piece_attributes(&self) -> &Attributes {
        &self.data
    }
}
impl AttributeCarrier for crate::model::RectilinearGridPiece {
    fn piece_attributes(&self) -> &Attributes {
        &self.data
    }
}
impl AttributeCarrier for crate::model::UnstructuredGridPiece {
    fn piece_attributes(&self) -> &Attributes {
        &self.data
    }
}
impl AttributeCarrier for crate::model::PolyDataPiece {
    fn piece_attributes(&self) -> &Attributes {
        &self.data
    }
}

/// Borrow the attributes of the first piece of a data set, recursing through loaded nested data
/// sets. Returns empty attributes for source-only or empty data sets.
fn first_attributes(data_set: &DataSet) -> Attributes {
    macro_rules! first {
        ($pieces:expr) => {
            $pieces
                .iter()
                .find_map(|piece| match piece {
                    Piece::Inline(p) => Some(p.piece_attributes().clone()),
                    Piece::Loaded(inner) => Some(first_attributes(inner)),
                    Piece::Source(..) => None,
                })
                .unwrap_or_default()
        };
    }
    match data_set {
        DataSet::ImageData { pieces, .. } => first!(pieces),
        DataSet::StructuredGrid { pieces, .. } => first!(pieces),
        DataSet::RectilinearGrid { pieces, .. } => first!(pieces),
        DataSet::UnstructuredGrid { pieces, .. } => first!(pieces),
        DataSet::PolyData { pieces, .. } => first!(pieces),
        DataSet::Field { .. } => Attributes::default(),
    }
}

/// Sum the point and cell counts over all inline/loaded pieces of a data set.
fn data_set_sizes(data_set: &DataSet) -> (usize, usize) {
    macro_rules! sizes {
        ($pieces:expr) => {{
            let mut points = 0usize;
            let mut cells = 0usize;
            for piece in $pieces.iter() {
                match piece {
                    Piece::Inline(p) => {
                        points += p.piece_points();
                        cells += p.piece_cells();
                    }
                    Piece::Loaded(inner) => {
                        let (p, c) = data_set_sizes(inner);
                        points += p;
                        cells += c;
                    }
                    Piece::Source(..) => {}
                }
            }
            (points, cells)
        }};
    }
    match data_set {
        DataSet::ImageData { pieces, .. } => sizes!(pieces),
        DataSet::StructuredGrid { pieces, .. } => sizes!(pieces),
        DataSet::RectilinearGrid { pieces, .. } => sizes!(pieces),
        DataSet::UnstructuredGrid { pieces, .. } => sizes!(pieces),
        DataSet::PolyData { pieces, .. } => sizes!(pieces),
        DataSet::Field { .. } => (0, 0),
    }
}

/// A signature entry describing one attribute array.
type ArraySignature = (
    String,
    crate::model::ElementType,
    crate::model::ScalarType,
    u32,
);

/// Signatures of the point and cell attribute arrays, preserving declaration order.
#[derive(Clone, Debug, Default, PartialEq)]
struct SchemaSignature {
    point: Vec<ArraySignature>,
    cell: Vec<ArraySignature>,
}

fn signature_of(attributes: &Attributes) -> SchemaSignature {
    fn one(attrs: &[Attribute]) -> Vec<ArraySignature> {
        attrs
            .iter()
            .map(|attr| match attr {
                Attribute::DataArray(array) => (
                    array.name.clone(),
                    array.elem.clone(),
                    array.data.scalar_type(),
                    array.elem.num_comp(),
                ),
                Attribute::Field { name, data_array } => {
                    let num_comp = data_array.first().map(|a| a.elem).unwrap_or(1);
                    let scalar = data_array
                        .first()
                        .map(|a| a.data.scalar_type())
                        .unwrap_or(crate::model::ScalarType::U8);
                    (
                        name.clone(),
                        crate::model::ElementType::Generic(num_comp),
                        scalar,
                        num_comp,
                    )
                }
            })
            .collect()
    }
    SchemaSignature {
        point: one(&attributes.point),
        cell: one(&attributes.cell),
    }
}

/// Kind of a data set, used to ensure all pieces describe the same VTK dataset type.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum DataSetKind {
    ImageData,
    StructuredGrid,
    RectilinearGrid,
    UnstructuredGrid,
    PolyData,
    Field,
}

impl DataSetKind {
    fn of(data_set: &DataSet) -> DataSetKind {
        match data_set {
            DataSet::ImageData { .. } => DataSetKind::ImageData,
            DataSet::StructuredGrid { .. } => DataSetKind::StructuredGrid,
            DataSet::RectilinearGrid { .. } => DataSetKind::RectilinearGrid,
            DataSet::UnstructuredGrid { .. } => DataSetKind::UnstructuredGrid,
            DataSet::PolyData { .. } => DataSetKind::PolyData,
            DataSet::Field { .. } => DataSetKind::Field,
        }
    }
}

/// Everything checked against the first piece before merging.
#[derive(Clone, Debug)]
struct ExpectedSchema {
    kind: DataSetKind,
    attributes: SchemaSignature,
}

/// Stateful driver that loads and merges all pieces of a parallel VTK file.
///
/// The cache lives and dies with one load, which guarantees that different resolvers, roots and
/// repeated attempts (including failed ones) never share state.
struct PieceLoader<'a, R: PieceResolver + ?Sized> {
    resolver: &'a R,
    cache: HashMap<String, LoadedPiece>,
    report: LoadReport,
    /// Normalized URIs currently being resolved, for cycle detection.
    active: Vec<String>,
}

impl<'a, R: PieceResolver + ?Sized> PieceLoader<'a, R> {
    fn new(resolver: &'a R) -> Self {
        PieceLoader {
            resolver,
            cache: HashMap::new(),
            report: LoadReport::default(),
            active: Vec::new(),
        }
    }

    /// Register a not-yet-processed reference and return its report index.
    fn register_pending(
        &mut self,
        uri: &str,
        normalized: &str,
        base_path: Option<&Path>,
        chain: &[String],
    ) -> usize {
        let index = self.report.pieces.len();
        self.report.pieces.push(PieceReport {
            index,
            source_uri: uri.to_owned(),
            resolved_uri: normalized.to_owned(),
            referenced_from: base_path.map(Path::to_path_buf),
            reference_chain: chain.to_vec(),
            origin: None,
            num_points: None,
            num_cells: None,
            status: PieceStatus::Pending,
        });
        index
    }

    fn mark_failed(&mut self, index: usize, reason: String) -> LoadError {
        if let Some(entry) = self.report.pieces.get_mut(index) {
            entry.status = PieceStatus::Failed {
                reason: reason.clone(),
            };
        }
        LoadError::new(reason, std::mem::take(&mut self.report))
    }

    /// Record that a successfully loaded reference was not merged for a recoverable reason.
    fn mark_skipped(&mut self, index: usize, reason: String) {
        if let Some(entry) = self.report.pieces.get_mut(index) {
            entry.status = PieceStatus::Skipped { reason };
        }
    }

    /// Resolve and parse a single piece reference, recursing into nested parallel files.
    ///
    /// When `slot` is `Some`, the corresponding pre-registered root entry is updated in place;
    /// otherwise a new report entry is appended (used for nested references).
    fn load_piece(
        &mut self,
        uri: &str,
        base_path: Option<&Path>,
        chain: &[String],
        slot: Option<usize>,
    ) -> Result<LoadedPiece, LoadError> {
        let normalized = normalize_uri(uri, base_path);
        let index =
            slot.unwrap_or_else(|| self.register_pending(uri, &normalized, base_path, chain));

        // Cycle detection: a normalized URI already on the active stack means a reference loop.
        if self.active.iter().any(|u| u == &normalized) {
            let mut full_chain = chain.to_vec();
            full_chain.push(normalized.clone());
            return Err(self.mark_failed(
                index,
                format!(
                    "detected a cyclic piece reference: {}",
                    full_chain.join(" -> ")
                ),
            ));
        }

        // De-duplication: a normalized URI already loaded is served from the per-load cache.
        if let Some(cached) = self.cache.get(&normalized) {
            let entry = self.report.pieces.get_mut(index).expect("registered entry");
            entry.origin = Some(PieceOrigin::Cached {
                location: cached.location.clone(),
            });
            entry.num_points = Some(cached.num_points);
            entry.num_cells = Some(cached.num_cells);
            entry.status = PieceStatus::Merged;
            return Ok(cached.clone());
        }

        let request = PieceRequest {
            uri,
            base_path,
            reference_chain: chain,
        };
        let resolved = self
            .resolver
            .resolve(&request)
            .map_err(|e| self.mark_failed(index, e.message))?;

        // Parse the piece through the public XML entry point.
        let mut piece_vtk = match Vtk::parse_xml(std::io::Cursor::new(resolved.bytes.as_slice())) {
            Ok(vtk) => vtk,
            Err(e) => {
                return Err(
                    self.mark_failed(index, format!("failed to parse piece {:?}: {}", uri, e))
                );
            }
        };
        piece_vtk.file_path = Some(PathBuf::from(&resolved.location));

        // Recurse into nested parallel pieces before validating and caching this one.
        self.active.push(normalized.clone());
        let nested = self.load_nested(&mut piece_vtk, &normalized, chain);
        self.active.pop();
        nested?;

        let (num_points, num_cells) = data_set_sizes(&piece_vtk.data);
        let loaded = LoadedPiece {
            data_set: piece_vtk.data,
            byte_order: piece_vtk.byte_order,
            location: resolved.location.clone(),
            num_points,
            num_cells,
        };
        self.cache.insert(normalized.clone(), loaded.clone());

        let entry = self.report.pieces.get_mut(index).expect("registered entry");
        entry.origin = Some(PieceOrigin::Loaded {
            location: resolved.location,
        });
        entry.num_points = Some(num_points);
        entry.num_cells = Some(num_cells);
        entry.status = PieceStatus::Merged;
        Ok(loaded)
    }

    /// Load any pieces referenced by a nested parallel VTK file in place.
    fn load_nested(
        &mut self,
        vtk: &mut Vtk,
        parent_normalized: &str,
        chain: &[String],
    ) -> Result<(), LoadError> {
        let mut child_chain = chain.to_vec();
        child_chain.push(parent_normalized.to_owned());
        let base_path = vtk.file_path.clone();
        macro_rules! load_each {
            ($pieces:expr) => {{
                let owned = std::mem::take($pieces);
                let mut out = Vec::with_capacity(owned.len());
                for piece in owned {
                    match piece {
                        Piece::Source(uri, _extent) => {
                            let loaded =
                                self.load_piece(&uri, base_path.as_deref(), &child_chain, None)?;
                            out.push(Piece::Loaded(Box::new(loaded.data_set)));
                        }
                        other => out.push(other),
                    }
                }
                *$pieces = out;
            }};
        }
        match &mut vtk.data {
            DataSet::ImageData { pieces, .. } => load_each!(pieces),
            DataSet::StructuredGrid { pieces, .. } => load_each!(pieces),
            DataSet::RectilinearGrid { pieces, .. } => load_each!(pieces),
            DataSet::UnstructuredGrid { pieces, .. } => load_each!(pieces),
            DataSet::PolyData { pieces, .. } => load_each!(pieces),
            DataSet::Field { .. } => {}
        }
        Ok(())
    }
}

/// Validate a freshly loaded piece against the schema of the first one and the root byte order.
fn validate_piece(
    expected: &ExpectedSchema,
    root_byte_order: crate::model::ByteOrder,
    index: usize,
    piece: &LoadedPiece,
) -> Result<(), String> {
    let kind = DataSetKind::of(&piece.data_set);
    if kind != expected.kind {
        return Err(format!(
            "piece {} has dataset kind {:?}, expected {:?}",
            index, kind, expected.kind
        ));
    }
    if piece.byte_order != root_byte_order {
        return Err(format!(
            "piece {} has byte order {:?}, expected root file byte order {:?}",
            index, piece.byte_order, root_byte_order
        ));
    }
    let signature = signature_of(&first_attributes(&piece.data_set));
    if signature != expected.attributes {
        return Err(format!(
            "piece {} has point/cell attribute schema {:?}, expected {:?}",
            index, signature, expected.attributes
        ));
    }
    Ok(())
}

/// Recursively flatten the pieces of a same-kind loaded data set into `out`.
fn flatten_loaded<P: PieceData>(
    out: &mut Vec<Piece<P>>,
    data_set: DataSet,
    pick: fn(DataSet) -> Option<Vec<Piece<P>>>,
) {
    if let Some(inner_pieces) = pick(data_set) {
        for piece in inner_pieces {
            match piece {
                Piece::Loaded(inner) => flatten_loaded(out, *inner, pick),
                other => out.push(other),
            }
        }
    }
}

/// Replace the root data set's source pieces by the loaded ones, in declaration order.
fn merge_loaded(data: &mut DataSet, loaded: Vec<LoadedPiece>) {
    macro_rules! merge {
        ($pieces:expr, $meta:expr, $variant:ident) => {{
            let mut flat = Vec::new();
            for piece in loaded {
                flatten_loaded(&mut flat, piece.data_set, |ds| match ds {
                    DataSet::$variant { pieces, .. } => Some(pieces),
                    _ => None,
                });
            }
            *$pieces = flat;
            *$meta = None;
        }};
    }
    match data {
        DataSet::ImageData { pieces, meta, .. } => merge!(pieces, meta, ImageData),
        DataSet::StructuredGrid { pieces, meta, .. } => merge!(pieces, meta, StructuredGrid),
        DataSet::RectilinearGrid { pieces, meta, .. } => {
            merge!(pieces, meta, RectilinearGrid)
        }
        DataSet::UnstructuredGrid { pieces, meta, .. } => {
            merge!(pieces, meta, UnstructuredGrid)
        }
        DataSet::PolyData { pieces, meta, .. } => merge!(pieces, meta, PolyData),
        DataSet::Field { .. } => {}
    }
}

/// Collect the raw source URIs declared by a (possibly parallel) data set in declaration order.
fn collect_sources(data_set: &DataSet) -> Vec<String> {
    macro_rules! collect {
        ($pieces:expr) => {
            $pieces
                .iter()
                .filter_map(|piece| match piece {
                    Piece::Source(uri, _) => Some(uri.clone()),
                    _ => None,
                })
                .collect()
        };
    }
    match data_set {
        DataSet::ImageData { pieces, .. } => collect!(pieces),
        DataSet::StructuredGrid { pieces, .. } => collect!(pieces),
        DataSet::RectilinearGrid { pieces, .. } => collect!(pieces),
        DataSet::UnstructuredGrid { pieces, .. } => collect!(pieces),
        DataSet::PolyData { pieces, .. } => collect!(pieces),
        DataSet::Field { .. } => Vec::new(),
    }
}

impl Vtk {
    /// Load and merge all pieces referenced by this parallel VTK file using the default
    /// [`LocalPieceResolver`].
    ///
    /// The resolver root is taken from the parent directory of this file's
    /// [`file_path`](Vtk::file_path). Relative piece URIs are resolved from the referencing file's
    /// directory. This is the report-producing counterpart of
    /// [`load_all_pieces`](Vtk::load_all_pieces).
    pub fn import_pieces(&mut self) -> Result<LoadReport, LoadError> {
        let root = self
            .file_path
            .clone()
            .and_then(|p| p.parent().map(Path::to_path_buf))
            .unwrap_or_else(|| PathBuf::from("."));
        let resolver = LocalPieceResolver::new(root);
        self.import_pieces_with(&resolver)
    }

    /// Load and merge all pieces referenced by this parallel VTK file using a custom
    /// [`PieceResolver`], returning a report of every reference.
    ///
    /// On success, the resulting model is the regular [`Vtk`] type with all pieces merged in
    /// declaration order. On failure, the model is left untouched and the partial report is
    /// available through [`LoadError::report`].
    ///
    /// Repeated references to the same normalized piece are read and parsed only once per call;
    /// the cache is not shared between calls, so retrying after fixing the input re-reads every
    /// file.
    pub fn import_pieces_with<R: PieceResolver>(
        &mut self,
        resolver: &R,
    ) -> Result<LoadReport, LoadError> {
        let base_path = self.file_path.clone();
        let root_chain: Vec<String> = base_path
            .as_deref()
            .map(|p| vec![p.to_string_lossy().into_owned()])
            .unwrap_or_default();

        // Snapshot the declared sources before loading anything so the model stays untouched on
        // failure and declaration order is preserved.
        let sources = collect_sources(&self.data);
        if sources.is_empty() {
            return Ok(LoadReport::default());
        }

        let mut loader = PieceLoader::new(resolver);

        // Pre-register every root reference as pending.
        let mut slots = Vec::with_capacity(sources.len());
        for uri in &sources {
            let normalized = normalize_uri(uri, base_path.as_deref());
            let index =
                loader.register_pending(uri, &normalized, base_path.as_deref(), &root_chain);
            slots.push((uri.clone(), normalized, index));
        }

        let mut expected: Option<ExpectedSchema> = None;
        let mut loaded: Vec<LoadedPiece> = Vec::with_capacity(sources.len());
        for (uri, _normalized, index) in &slots {
            let piece = loader.load_piece(uri, base_path.as_deref(), &root_chain, Some(*index))?;

            // A recoverable issue: an empty piece contributes nothing, so it is recorded and
            // skipped without aborting the whole load.
            if piece.num_points == 0 && piece.num_cells == 0 {
                loader.mark_skipped(*index, "piece contains no points and no cells".to_owned());
                continue;
            }

            // Validate incrementally: a mismatch fails before any merge happens.
            match &expected {
                None => {
                    if piece.byte_order != self.byte_order {
                        let reason = format!(
                            "piece {} has byte order {:?}, expected root file byte order {:?}",
                            index, piece.byte_order, self.byte_order
                        );
                        return Err(loader.mark_failed(*index, reason));
                    }
                    expected = Some(ExpectedSchema {
                        kind: DataSetKind::of(&piece.data_set),
                        attributes: signature_of(&first_attributes(&piece.data_set)),
                    });
                }
                Some(schema) => {
                    if let Err(reason) = validate_piece(schema, self.byte_order, *index, &piece) {
                        return Err(loader.mark_failed(*index, reason));
                    }
                }
            }
            loaded.push(piece);
        }

        // All pieces are loaded and validated. Only now is the model touched, and only when at
        // least one non-empty piece was merged (an all-empty load leaves the model untouched).
        if !loaded.is_empty() {
            merge_loaded(&mut self.data, loaded);
        }
        Ok(loader.report)
    }
}
