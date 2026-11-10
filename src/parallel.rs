//!
//! Loading of "parallel" VTK XML files with pluggable piece resolution.
//!
//! Parallel XML files (`.pvtu`, `.pvtp`, `.pvtr`, `.pvts`, `.pvti`) describe a data set whose
//! pieces live in separate files referenced through `Source` attributes. The [`load`] function
//! resolves, reads, parses, validates and merges those pieces into a single
//! [`Vtk`](crate::model::Vtk) model in one shot, returning the familiar model type used
//! everywhere else in this crate.
//!
//! Callers decide where pieces are found by supplying their own [`PieceResolver`]
//! implementation. The provided [`LocalFileResolver`] resolves pieces as local files relative
//! to the file referencing them and refuses to read anything outside of a configured root
//! directory or from the network.
//!
//! Every load produces a [`LoadReport`] describing, for each piece reference encountered, where
//! the piece came from, the extent declared for it and whether it ended up merged into the
//! returned model. When a load fails, the partially filled report is returned alongside the
//! error so callers can tell which pieces were processed, which one failed and which were never
//! reached.
//!
//! Pieces are merged in the order they are declared in the referring files (depth first). If
//! two references normalize to the same location, the referenced file is read and parsed only
//! once per load, but both references show up in the report and in the merged model. The piece
//! cache is scoped to a single [`load`] call: caches are never shared between different
//! resolvers or root directories, and a failed load leaves no cache behind, so rerunning a load
//! after fixing the input re-reads the files from scratch.
//!
//! # Example
//!
//! ```no_run
//! use vtkio::parallel::{self, LocalFileResolver};
//!
//! // Pieces are looked up as local files underneath the current directory.
//! let resolver = LocalFileResolver::new(".");
//! let (vtk, report) = parallel::load("mesh.pvtu", &resolver)?;
//! assert!(report.is_ok());
//! # Ok::<(), vtkio::parallel::LoadError>(())
//! ```

use std::collections::HashMap;
use std::fmt;
use std::path::{Component, Path, PathBuf};

use crate::model::{self, Attribute, ByteOrder, DataSet, Piece, ScalarType, Vtk};
use crate::xml;

/// A reference to a piece of a parallel VTK XML data set.
///
/// This is the input given to a [`PieceResolver`] when the loader needs the contents of a
/// referenced piece.
#[derive(Clone, Debug)]
pub struct PieceReference {
    /// The URI of the piece exactly as written in the `Source` attribute of the referring file.
    pub uri: String,
    /// The location of the file containing this reference.
    pub referrer: PathBuf,
    /// Normalized location of the piece.
    ///
    /// For local files this is the `uri` resolved against the `referrer`'s directory with `.`
    /// and `..` components removed lexically. It uniquely identifies the piece within a single
    /// load operation: two references with the same `location` are read and parsed only once.
    pub location: PathBuf,
    /// The extent of the piece as declared in the referring file (if any).
    pub extent: Option<model::Extent>,
}

/// A strategy for locating and reading pieces referenced by parallel VTK XML files.
///
/// Implementations receive the [`PieceReference`] exactly as declared (through
/// [`uri`](PieceReference::uri)) along with a normalized
/// [`location`](PieceReference::location) they can use as a default lookup key, and return the
/// raw bytes of the referenced file. Parsing and merging is always performed by the library.
pub trait PieceResolver {
    /// Fetch the raw contents of the file referenced by `reference`.
    fn resolve(&self, reference: &PieceReference) -> Result<Vec<u8>, ResolveError>;
}

/// Error produced by a [`PieceResolver`] when a piece cannot be provided.
#[derive(Clone, Debug)]
pub struct ResolveError {
    message: String,
}

impl ResolveError {
    /// Create a new resolve error with the given message.
    pub fn new(message: impl Into<String>) -> Self {
        ResolveError {
            message: message.into(),
        }
    }
}

impl fmt::Display for ResolveError {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(f, "{}", self.message)
    }
}

impl std::error::Error for ResolveError {}

/// The default [`PieceResolver`]: reads local files from underneath a fixed root directory.
///
/// Relative piece URIs are resolved against the directory of the file referencing them. Any
/// reference that escapes the root directory (e.g. through `..` components or an absolute
/// path) or that points at a remote resource (any URI with a scheme such as `https://`) is
/// rejected.
#[derive(Clone, Debug)]
pub struct LocalFileResolver {
    root: PathBuf,
}

impl LocalFileResolver {
    /// Create a resolver that only serves files underneath `root`.
    pub fn new(root: impl AsRef<Path>) -> Self {
        LocalFileResolver {
            root: absolutize(root.as_ref()),
        }
    }

    /// The normalized absolute root directory that pieces must live under.
    pub fn root(&self) -> &Path {
        &self.root
    }
}

impl PieceResolver for LocalFileResolver {
    fn resolve(&self, reference: &PieceReference) -> Result<Vec<u8>, ResolveError> {
        let uri = &reference.uri;
        if is_remote_uri(uri) {
            return Err(ResolveError::new(format!(
                "refusing to load remote piece {:?}",
                uri
            )));
        }
        let location = &reference.location;
        if !location.starts_with(&self.root) {
            return Err(ResolveError::new(format!(
                "piece {:?} resolves to {:?} which is outside of the root directory {:?}",
                uri, location, self.root
            )));
        }
        std::fs::read(location).map_err(|e| {
            ResolveError::new(format!(
                "failed to read piece {:?} from {:?}: {}",
                uri, location, e
            ))
        })
    }
}

/// Options controlling how pieces are loaded and merged.
#[derive(Clone, Copy, Debug, Default)]
pub struct LoadOptions {
    /// When `true`, pieces that fail to load are skipped and the remaining pieces are merged
    /// into the returned model. The report still records every failure.
    ///
    /// When `false` (the default), the first failing piece aborts the load and no model is
    /// returned.
    pub allow_partial: bool,
}

/// Outcome of a single piece reference in a [`LoadReport`].
#[derive(Clone, Debug, PartialEq)]
pub enum PieceOutcome {
    /// The reference has not been processed (yet). This is the state of pieces that were never
    /// reached because an earlier piece aborted the load.
    Pending,
    /// The piece was loaded and validated but the load failed afterwards, so it was not merged.
    Loaded,
    /// The piece was loaded and merged into the returned model.
    Merged,
    /// The piece failed to load; the payload describes why.
    Failed(String),
}

/// A report entry describing what happened to a single piece reference.
#[derive(Clone, Debug)]
pub struct PieceReport {
    /// The URI exactly as written in the `Source` attribute of the referring file.
    pub uri: String,
    /// The location of the file containing this reference.
    pub referrer: PathBuf,
    /// The normalized location the piece was (or would have been) loaded from.
    pub location: PathBuf,
    /// The extent declared for this piece in the referring file (if any).
    pub extent: Option<model::Extent>,
    /// Whether the piece data was reused from an earlier identical reference instead of being
    /// read and parsed again.
    pub from_cache: bool,
    /// What happened to this reference in the end.
    pub outcome: PieceOutcome,
}

/// A record of every piece reference encountered during a load.
///
/// Entries appear in the order the references were processed: all pieces of a file are recorded
/// (as [`Pending`](PieceOutcome::Pending)) before its first piece is resolved, and nested
/// parallel files contribute their entries when they are loaded.
#[derive(Clone, Debug, Default)]
pub struct LoadReport {
    /// One entry per piece reference encountered during the load.
    pub pieces: Vec<PieceReport>,
}

impl LoadReport {
    /// `true` if no piece reference failed to load.
    pub fn is_ok(&self) -> bool {
        self.pieces
            .iter()
            .all(|p| !matches!(p.outcome, PieceOutcome::Failed(_)))
    }

    /// Number of piece references that were merged into the returned model.
    pub fn num_merged(&self) -> usize {
        self.pieces
            .iter()
            .filter(|p| p.outcome == PieceOutcome::Merged)
            .count()
    }

    /// Number of piece references that failed to load.
    pub fn num_failed(&self) -> usize {
        self.pieces
            .iter()
            .filter(|p| matches!(p.outcome, PieceOutcome::Failed(_)))
            .count()
    }
}

/// Reason why a load failed.
#[derive(Debug)]
pub enum LoadErrorKind {
    /// The root file itself could not be imported.
    Root(crate::Error),
    /// A cyclic piece reference was detected. The payload is the full chain of referenced
    /// locations, from the root file down to (and including) the repeated location.
    Cycle(Vec<PathBuf>),
    /// One or more pieces failed to load. See the [`LoadReport`] attached to the error for
    /// details.
    PieceFailed,
    /// The loaded pieces are not compatible with each other and cannot be merged.
    Compatibility(CompatibilityError),
}

/// A consistency violation detected between the pieces of a parallel data set.
///
/// Pieces are checked before merging: they must all contain the same kind of data set as the
/// referring file, use the same byte order and agree on the point and cell attribute schema
/// (array names, scalar types and number of components).
#[derive(Debug)]
pub enum CompatibilityError {
    /// A piece file contains a different kind of data set than the referring file.
    DataSetKind {
        /// Location of the offending piece.
        location: PathBuf,
        /// The data set kind of the referring file.
        expected: &'static str,
        /// The data set kind found in the piece.
        found: &'static str,
    },
    /// A piece uses a different byte order than the referring file.
    ByteOrder {
        /// Location of the offending piece.
        location: PathBuf,
        /// Byte order of the referring file.
        expected: ByteOrder,
        /// Byte order of the piece.
        found: ByteOrder,
    },
    /// The point attribute schema of a piece doesn't match the other pieces.
    PointAttributes {
        /// Location of the offending piece.
        location: PathBuf,
        /// Schema of the other pieces.
        expected: String,
        /// Schema of the offending piece.
        found: String,
    },
    /// The cell attribute schema of a piece doesn't match the other pieces.
    CellAttributes {
        /// Location of the offending piece.
        location: PathBuf,
        /// Schema of the other pieces.
        expected: String,
        /// Schema of the offending piece.
        found: String,
    },
}

impl fmt::Display for CompatibilityError {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match self {
            CompatibilityError::DataSetKind {
                location,
                expected,
                found,
            } => write!(
                f,
                "piece {:?} contains a {} data set but {} was expected",
                location, found, expected
            ),
            CompatibilityError::ByteOrder {
                location,
                expected,
                found,
            } => write!(
                f,
                "piece {:?} has byte order {:?} but {:?} was expected",
                location, found, expected
            ),
            CompatibilityError::PointAttributes {
                location,
                expected,
                found,
            } => write!(
                f,
                "piece {:?} has point attributes {} but {} were expected",
                location, found, expected
            ),
            CompatibilityError::CellAttributes {
                location,
                expected,
                found,
            } => write!(
                f,
                "piece {:?} has cell attributes {} but {} were expected",
                location, found, expected
            ),
        }
    }
}

impl std::error::Error for CompatibilityError {}

/// Error returned by [`load`] and [`load_with_options`].
///
/// The partially filled [`report`](LoadError::report) shows which pieces were processed before
/// the failure, which piece caused it and which pieces were never reached.
#[derive(Debug)]
pub struct LoadError {
    /// Why the load failed.
    pub kind: LoadErrorKind,
    /// The state of every piece reference at the time of the failure.
    pub report: LoadReport,
}

impl fmt::Display for LoadError {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match &self.kind {
            LoadErrorKind::Root(source) => {
                write!(f, "failed to load the root VTK file: {}", source)
            }
            LoadErrorKind::Cycle(chain) => {
                let chain = chain
                    .iter()
                    .map(|p| format!("{:?}", p))
                    .collect::<Vec<_>>()
                    .join(" -> ");
                write!(f, "cyclic piece reference detected: {}", chain)
            }
            LoadErrorKind::PieceFailed => {
                write!(f, "failed to load one or more pieces (see the load report)")
            }
            LoadErrorKind::Compatibility(source) => write!(f, "incompatible pieces: {}", source),
        }
    }
}

impl std::error::Error for LoadError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match &self.kind {
            LoadErrorKind::Root(source) => Some(source),
            LoadErrorKind::Compatibility(source) => Some(source),
            _ => None,
        }
    }
}

/// Load a VTK file, resolving the pieces of "parallel" XML files with the given `resolver`.
///
/// This behaves like [`Vtk::import`](crate::Vtk::import) followed by
/// [`load_all_pieces`](crate::model::Vtk::load_all_pieces), except that piece locations are
/// provided by `resolver` instead of being read from disk directly, and a [`LoadReport`] is
/// produced describing the fate of every piece reference.
///
/// The returned [`Vtk`] has all its pieces merged inline in declaration order. Pieces are
/// validated before merging (data set kind, byte order and attribute schemas must agree); any
/// mismatch fails the load and no partially merged model is returned.
///
/// This is a strict load: the first failing piece aborts it. Use [`load_with_options`] with
/// [`LoadOptions::allow_partial`] to skip failing pieces instead.
pub fn load(
    file_path: impl AsRef<Path>,
    resolver: &dyn PieceResolver,
) -> Result<(Vtk, LoadReport), LoadError> {
    load_with_options(file_path, resolver, LoadOptions::default())
}

/// Load a VTK file like [`load`], with additional control over failure handling.
///
/// The piece cache is scoped to this call: nothing is shared with other loads, other resolvers
/// or other root directories, and a failed load leaves no cache behind, so rerunning the load
/// after fixing the input re-reads the files from scratch.
pub fn load_with_options(
    file_path: impl AsRef<Path>,
    resolver: &dyn PieceResolver,
    options: LoadOptions,
) -> Result<(Vtk, LoadReport), LoadError> {
    let path = file_path.as_ref();
    let root_location = absolutize(path);
    let mut vtk = Vtk::import(path).map_err(|error| LoadError {
        kind: LoadErrorKind::Root(error),
        report: LoadReport::default(),
    })?;
    let byte_order = vtk.byte_order;
    let mut loader = Loader {
        resolver,
        allow_partial: options.allow_partial,
        cache: HashMap::new(),
        stack: vec![root_location.clone()],
        report: LoadReport::default(),
    };
    let loaded = match loader.process_data_set(vtk.data.clone(), &root_location, byte_order) {
        Ok(loaded) => loaded,
        Err(failure) => return Err(loader.into_error(failure)),
    };
    if let Err(compatibility) = validate_compatibility(&loaded, byte_order) {
        return Err(loader.into_error(LoadFailure::Fatal(LoadErrorKind::Compatibility(
            compatibility,
        ))));
    }
    // Everything checked out: mark all loaded pieces as merged.
    for entry in loader.report.pieces.iter_mut() {
        if entry.outcome == PieceOutcome::Loaded {
            entry.outcome = PieceOutcome::Merged;
        }
    }
    vtk.data = loaded.data_set;
    Ok((vtk, loader.report))
}

/// A data set whose pieces have all been loaded inline, along with the origin of each piece.
#[derive(Clone)]
struct LoadedDataSet {
    data_set: DataSet,
    /// One entry per inline piece of `data_set`, in the same order.
    provenance: Vec<PieceProvenance>,
}

/// Where a single merged piece came from.
#[derive(Clone)]
struct PieceProvenance {
    location: PathBuf,
    byte_order: ByteOrder,
}

/// Internal failure type used while loading.
///
/// `Piece` failures are recorded in the report and may be survivable (see
/// [`LoadOptions::allow_partial`]); `Fatal` failures abort the load immediately.
enum LoadFailure {
    Fatal(LoadErrorKind),
    Piece(String),
}

struct Loader<'a> {
    resolver: &'a dyn PieceResolver,
    allow_partial: bool,
    /// Successfully loaded files by normalized location. Failed loads are never cached.
    cache: HashMap<PathBuf, LoadedDataSet>,
    /// Locations currently being loaded, from the root file down. Used for cycle detection.
    stack: Vec<PathBuf>,
    report: LoadReport,
}

impl<'a> Loader<'a> {
    /// Recursively load all `Source` pieces of a data set, flattening the results inline.
    fn process_data_set(
        &mut self,
        data_set: DataSet,
        location: &Path,
        byte_order: ByteOrder,
    ) -> Result<LoadedDataSet, LoadFailure> {
        macro_rules! process {
            ($pieces:expr, $kind:literal, $pick:expr, $rebuild:expr) => {{
                let (pieces, provenance) =
                    self.load_piece_vec($pieces, location, byte_order, $kind, $pick)?;
                LoadedDataSet {
                    data_set: $rebuild(pieces),
                    provenance,
                }
            }};
        }
        Ok(match data_set {
            DataSet::ImageData {
                extent,
                origin,
                spacing,
                pieces,
                ..
            } => process!(
                pieces,
                "ImageData",
                |ds| match ds {
                    DataSet::ImageData { pieces, .. } => Some(pieces),
                    _ => None,
                },
                move |pieces| DataSet::ImageData {
                    extent,
                    origin,
                    spacing,
                    meta: None,
                    pieces,
                }
            ),
            DataSet::StructuredGrid {
                extent, pieces, ..
            } => process!(
                pieces,
                "StructuredGrid",
                |ds| match ds {
                    DataSet::StructuredGrid { pieces, .. } => Some(pieces),
                    _ => None,
                },
                move |pieces| DataSet::StructuredGrid {
                    extent,
                    meta: None,
                    pieces,
                }
            ),
            DataSet::RectilinearGrid {
                extent, pieces, ..
            } => process!(
                pieces,
                "RectilinearGrid",
                |ds| match ds {
                    DataSet::RectilinearGrid { pieces, .. } => Some(pieces),
                    _ => None,
                },
                move |pieces| DataSet::RectilinearGrid {
                    extent,
                    meta: None,
                    pieces,
                }
            ),
            DataSet::UnstructuredGrid { pieces, .. } => process!(
                pieces,
                "UnstructuredGrid",
                |ds| match ds {
                    DataSet::UnstructuredGrid { pieces, .. } => Some(pieces),
                    _ => None,
                },
                move |pieces| DataSet::UnstructuredGrid { meta: None, pieces }
            ),
            DataSet::PolyData { pieces, .. } => process!(
                pieces,
                "PolyData",
                |ds| match ds {
                    DataSet::PolyData { pieces, .. } => Some(pieces),
                    _ => None,
                },
                move |pieces| DataSet::PolyData { meta: None, pieces }
            ),
            // Field data sets have no pieces to load.
            other => LoadedDataSet {
                data_set: other,
                provenance: Vec::new(),
            },
        })
    }

    /// Load a vector of pieces, resolving `Source` references recursively.
    ///
    /// Returns the flattened pieces in declaration order along with their provenance.
    fn load_piece_vec<P, F>(
        &mut self,
        pieces: Vec<Piece<P>>,
        referrer: &Path,
        byte_order: ByteOrder,
        expected_kind: &'static str,
        pick: F,
    ) -> Result<(Vec<Piece<P>>, Vec<PieceProvenance>), LoadFailure>
    where
        F: Fn(DataSet) -> Option<Vec<Piece<P>>>,
    {
        // Record all references of this file up front so the report also shows pieces that
        // end up never being processed because an earlier piece aborts the load.
        let entry_indices: Vec<Option<usize>> = pieces
            .iter()
            .map(|piece| match piece {
                Piece::Source(uri, extent) => {
                    let index = self.report.pieces.len();
                    self.report.pieces.push(PieceReport {
                        uri: uri.clone(),
                        referrer: referrer.to_path_buf(),
                        location: normalize_location(uri, referrer),
                        extent: extent.clone(),
                        from_cache: false,
                        outcome: PieceOutcome::Pending,
                    });
                    Some(index)
                }
                _ => None,
            })
            .collect();

        let mut out_pieces = Vec::new();
        let mut out_provenance = Vec::new();
        for (piece, entry_index) in pieces.into_iter().zip(entry_indices) {
            match piece {
                Piece::Inline(data) => {
                    out_pieces.push(Piece::Inline(data));
                    out_provenance.push(PieceProvenance {
                        location: referrer.to_path_buf(),
                        byte_order,
                    });
                }
                Piece::Loaded(data_set) => {
                    let loaded = self.process_data_set(*data_set, referrer, byte_order)?;
                    let found_kind = data_set_kind_name(&loaded.data_set);
                    match pick(loaded.data_set) {
                        Some(mut nested) => {
                            out_pieces.append(&mut nested);
                            out_provenance.extend(loaded.provenance);
                        }
                        None => {
                            return Err(LoadFailure::Fatal(LoadErrorKind::Compatibility(
                                CompatibilityError::DataSetKind {
                                    location: referrer.to_path_buf(),
                                    expected: expected_kind,
                                    found: found_kind,
                                },
                            )));
                        }
                    }
                }
                Piece::Source(uri, extent) => {
                    let entry_index = entry_index.expect("source pieces have a report entry");
                    let reference = PieceReference {
                        uri,
                        referrer: referrer.to_path_buf(),
                        location: self.report.pieces[entry_index].location.clone(),
                        extent,
                    };
                    match self.load_source(&reference) {
                        Ok((loaded, from_cache)) => {
                            let found_kind = data_set_kind_name(&loaded.data_set);
                            match pick(loaded.data_set) {
                                Some(mut nested) => {
                                    self.report.pieces[entry_index].from_cache = from_cache;
                                    self.report.pieces[entry_index].outcome = PieceOutcome::Loaded;
                                    out_pieces.append(&mut nested);
                                    out_provenance.extend(loaded.provenance);
                                }
                                None => {
                                    self.report.pieces[entry_index].outcome =
                                        PieceOutcome::Failed(format!(
                                            "expected a {} data set but found {}",
                                            expected_kind, found_kind
                                        ));
                                    return Err(LoadFailure::Fatal(LoadErrorKind::Compatibility(
                                        CompatibilityError::DataSetKind {
                                            location: reference.location.clone(),
                                            expected: expected_kind,
                                            found: found_kind,
                                        },
                                    )));
                                }
                            }
                        }
                        Err(LoadFailure::Piece(message)) => {
                            self.report.pieces[entry_index].outcome =
                                PieceOutcome::Failed(message.clone());
                            if !self.allow_partial {
                                return Err(LoadFailure::Piece(message));
                            }
                        }
                        Err(fatal @ LoadFailure::Fatal(_)) => return Err(fatal),
                    }
                }
            }
        }
        Ok((out_pieces, out_provenance))
    }

    /// Load a single referenced file, using the cache and detecting cycles.
    ///
    /// The returned flag is `true` when the data was served from the cache.
    fn load_source(
        &mut self,
        reference: &PieceReference,
    ) -> Result<(LoadedDataSet, bool), LoadFailure> {
        let location = &reference.location;
        if self.stack.iter().any(|l| l == location) {
            let mut chain = self.stack.clone();
            chain.push(location.clone());
            return Err(LoadFailure::Fatal(LoadErrorKind::Cycle(chain)));
        }
        if let Some(loaded) = self.cache.get(location) {
            return Ok((loaded.clone(), true));
        }
        let bytes = self
            .resolver
            .resolve(reference)
            .map_err(|e| LoadFailure::Piece(e.to_string()))?;
        let vtk = parse_piece(location, &bytes).map_err(|e| {
            LoadFailure::Piece(format!("failed to parse {:?}: {}", location, e))
        })?;
        let byte_order = vtk.byte_order;
        self.stack.push(location.clone());
        let result = self.process_data_set(vtk.data, location, byte_order);
        self.stack.pop();
        let loaded = result?;
        // Only successful loads are cached; a failed load must not leave a stale entry behind.
        self.cache.insert(location.clone(), loaded.clone());
        Ok((loaded, false))
    }

    /// Convert a failure into the public error type, keeping the report.
    fn into_error(self, failure: LoadFailure) -> LoadError {
        let kind = match failure {
            LoadFailure::Fatal(kind) => kind,
            LoadFailure::Piece(_) => LoadErrorKind::PieceFailed,
        };
        LoadError {
            kind,
            report: self.report,
        }
    }
}

/// Parse the contents of a piece file the same way [`Vtk::import`](crate::Vtk::import) would.
fn parse_piece(location: &Path, bytes: &[u8]) -> Result<Vtk, crate::Error> {
    let ext = location
        .extension()
        .and_then(|s| s.to_str())
        .ok_or(crate::Error::UnknownFileExtension(None))?;
    if ext == "vtk" {
        return Vtk::parse_legacy_be(bytes);
    }
    let file_type = xml::FileType::try_from_ext(ext)
        .ok_or_else(|| crate::Error::UnknownFileExtension(Some(ext.to_string())))?;
    let vtk_file = xml::VTKFile::parse(bytes)?;
    let expected_type = xml::FileType::from(vtk_file.data_set_type);
    if file_type != expected_type {
        return Err(crate::Error::XML(xml::Error::TypeExtensionMismatch));
    }
    let mut vtk = Vtk::try_from(vtk_file)?;
    vtk.file_path = Some(location.to_path_buf());
    Ok(vtk)
}

/// The name of the data set kind, used in error messages.
fn data_set_kind_name(data_set: &DataSet) -> &'static str {
    match data_set {
        DataSet::ImageData { .. } => "ImageData",
        DataSet::StructuredGrid { .. } => "StructuredGrid",
        DataSet::RectilinearGrid { .. } => "RectilinearGrid",
        DataSet::UnstructuredGrid { .. } => "UnstructuredGrid",
        DataSet::PolyData { .. } => "PolyData",
        DataSet::Field { .. } => "Field",
    }
}

/// Check that all loaded pieces can be merged into a single data set.
fn validate_compatibility(
    loaded: &LoadedDataSet,
    byte_order: ByteOrder,
) -> Result<(), CompatibilityError> {
    for provenance in &loaded.provenance {
        if provenance.byte_order != byte_order {
            return Err(CompatibilityError::ByteOrder {
                location: provenance.location.clone(),
                expected: byte_order,
                found: provenance.byte_order,
            });
        }
    }
    let mut attributes = Vec::new();
    collect_piece_attributes(&loaded.data_set, &mut attributes);
    if let Some((first, rest)) = attributes.split_first() {
        let reference = AttributeSchema::new(first);
        for (attrs, provenance) in rest.iter().zip(loaded.provenance[1..].iter()) {
            let schema = AttributeSchema::new(attrs);
            if schema.point != reference.point {
                return Err(CompatibilityError::PointAttributes {
                    location: provenance.location.clone(),
                    expected: describe_schema(&reference.point),
                    found: describe_schema(&schema.point),
                });
            }
            if schema.cell != reference.cell {
                return Err(CompatibilityError::CellAttributes {
                    location: provenance.location.clone(),
                    expected: describe_schema(&reference.cell),
                    found: describe_schema(&schema.cell),
                });
            }
        }
    }
    Ok(())
}

/// Collect the attributes of every inline piece of a (fully loaded) data set, in order.
fn collect_piece_attributes<'a>(data_set: &'a DataSet, out: &mut Vec<&'a model::Attributes>) {
    macro_rules! collect {
        ($pieces:expr) => {
            for piece in $pieces {
                match piece {
                    Piece::Inline(data) => out.push(&data.data),
                    Piece::Loaded(data_set) => collect_piece_attributes(data_set, out),
                    Piece::Source(..) => {}
                }
            }
        };
    }
    match data_set {
        DataSet::ImageData { pieces, .. } => collect!(pieces),
        DataSet::StructuredGrid { pieces, .. } => collect!(pieces),
        DataSet::RectilinearGrid { pieces, .. } => collect!(pieces),
        DataSet::UnstructuredGrid { pieces, .. } => collect!(pieces),
        DataSet::PolyData { pieces, .. } => collect!(pieces),
        DataSet::Field { .. } => {}
    }
}

/// The point and cell attribute schema of a piece, used for compatibility checks.
#[derive(PartialEq, Debug)]
struct AttributeSchema {
    point: Vec<ArraySchema>,
    cell: Vec<ArraySchema>,
}

/// The schema of a single attribute array: name, scalar type and number of components.
#[derive(PartialEq, Debug)]
enum ArraySchema {
    DataArray {
        name: String,
        num_comp: u32,
        scalar_type: ScalarType,
    },
    Field {
        name: String,
        arrays: Vec<(String, u32, ScalarType)>,
    },
}

impl AttributeSchema {
    fn new(attributes: &model::Attributes) -> Self {
        AttributeSchema {
            point: attributes.point.iter().map(array_schema).collect(),
            cell: attributes.cell.iter().map(array_schema).collect(),
        }
    }
}

fn array_schema(attribute: &Attribute) -> ArraySchema {
    match attribute {
        Attribute::DataArray(data) => ArraySchema::DataArray {
            name: data.name.clone(),
            num_comp: data.elem.num_comp(),
            scalar_type: data.data.scalar_type(),
        },
        Attribute::Field { name, data_array } => ArraySchema::Field {
            name: name.clone(),
            arrays: data_array
                .iter()
                .map(|a| (a.name.clone(), a.elem, a.data.scalar_type()))
                .collect(),
        },
    }
}

fn describe_schema(schema: &[ArraySchema]) -> String {
    let arrays: Vec<String> = schema
        .iter()
        .map(|array| match array {
            ArraySchema::DataArray {
                name,
                num_comp,
                scalar_type,
            } => format!("{}: {:?} x{}", name, scalar_type, num_comp),
            ArraySchema::Field { name, arrays } => {
                format!("{}: field ({} arrays)", name, arrays.len())
            }
        })
        .collect();
    format!("[{}]", arrays.join(", "))
}

/// Lexically normalize a path, removing `.` components and resolving `..` components without
/// touching the file system.
fn normalize_path(path: &Path) -> PathBuf {
    let mut normalized = PathBuf::new();
    for component in path.components() {
        match component {
            Component::CurDir => {}
            Component::ParentDir => match normalized.file_name() {
                Some(last) if last != ".." => {
                    normalized.pop();
                }
                Some(_) => {
                    normalized.push("..");
                }
                None => {
                    // Either the empty path or the root directory; only the former can go up.
                    if normalized.as_os_str().is_empty() {
                        normalized.push("..");
                    }
                }
            },
            component => normalized.push(component.as_os_str()),
        }
    }
    normalized
}

/// Make a path absolute (relative to the current directory) and normalize it.
fn absolutize(path: &Path) -> PathBuf {
    let absolute = if path.is_absolute() {
        path.to_path_buf()
    } else {
        std::env::current_dir()
            .unwrap_or_else(|_| PathBuf::from("."))
            .join(path)
    };
    normalize_path(&absolute)
}

/// Compute the normalized location of a piece referenced by `uri` from the file at `referrer`.
fn normalize_location(uri: &str, referrer: &Path) -> PathBuf {
    if is_remote_uri(uri) {
        return PathBuf::from(uri);
    }
    let uri_path = Path::new(uri);
    let joined = if uri_path.is_absolute() {
        uri_path.to_path_buf()
    } else {
        match referrer.parent() {
            Some(dir) => dir.join(uri_path),
            None => uri_path.to_path_buf(),
        }
    };
    absolutize(&joined)
}

/// Detect URIs with a scheme (e.g. `https://...` or `file:...`).
///
/// Single letter schemes are treated as Windows drive letters, not as URI schemes.
fn is_remote_uri(uri: &str) -> bool {
    if uri.contains("://") {
        return true;
    }
    let first_separator = uri.find(['/', '\\']).unwrap_or(uri.len());
    match uri.find(':') {
        Some(colon) if colon > 1 && colon < first_separator => {
            uri[..colon].chars().enumerate().all(|(i, c)| {
                if i == 0 {
                    c.is_ascii_alphabetic()
                } else {
                    c.is_ascii_alphanumeric() || matches!(c, '+' | '-' | '.')
                }
            })
        }
        _ => false,
    }
}
