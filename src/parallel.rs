//! Customizable loading and reporting for parallel XML VTK files.
//!
//! The main entry points are [`PieceResolver`], [`PieceLoader`] and
//! [`Vtk::import_with_piece_resolver`].  The loader keeps a per-run cache,
//! validates all pieces against the schema of the first leaf piece, and returns
//! a [`PieceLoadReport`] both on success and (inside [`PieceLoadError`]) on
//! failure.

use std::collections::HashMap;
use std::error::Error as StdError;
use std::fmt;
use std::fs;
use std::io;
use std::path::{Component, Path, PathBuf};
use std::sync::Arc;

use crate::model::{
    Attribute, Attributes, ByteOrder, DataArrayBase, DataSet, ElementType, Extent, ImageDataPiece,
    Piece, PolyDataPiece, RectilinearGridPiece, ScalarType, StructuredGridPiece,
    UnstructuredGridPiece,
};
use crate::Vtk;

/// Dataset kind without the piece payload.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum DataSetKind {
    ImageData,
    StructuredGrid,
    RectilinearGrid,
    UnstructuredGrid,
    PolyData,
    Field,
}

impl DataSetKind {
    pub fn as_str(self) -> &'static str {
        match self {
            DataSetKind::ImageData => "ImageData",
            DataSetKind::StructuredGrid => "StructuredGrid",
            DataSetKind::RectilinearGrid => "RectilinearGrid",
            DataSetKind::UnstructuredGrid => "UnstructuredGrid",
            DataSetKind::PolyData => "PolyData",
            DataSetKind::Field => "Field",
        }
    }
}

impl fmt::Display for DataSetKind {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.as_str())
    }
}

impl DataSet {
    /// Return the broad kind of this dataset.
    pub fn kind(&self) -> DataSetKind {
        match self {
            DataSet::ImageData { .. } => DataSetKind::ImageData,
            DataSet::StructuredGrid { .. } => DataSetKind::StructuredGrid,
            DataSet::RectilinearGrid { .. } => DataSetKind::RectilinearGrid,
            DataSet::UnstructuredGrid { .. } => DataSetKind::UnstructuredGrid,
            DataSet::PolyData { .. } => DataSetKind::PolyData,
            DataSet::Field { .. } => DataSetKind::Field,
        }
    }
}

/// Policy for piece-local failures that can be skipped.
///
/// Schema, dataset-kind and byte-order mismatches are always fatal for a load.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum ErrorPolicy {
    /// Stop at the first recoverable error and return an error.
    #[default]
    FailFast,
    /// Record missing/unreadable/unparseable pieces and continue with others.
    Continue,
}

/// Lifecycle state of one `<Piece>` declaration.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PieceStatus {
    /// Known from XML, but not reached yet.
    Pending,
    /// The resolver is currently being called.
    Resolving,
    /// Bytes were obtained from the resolver or cache.
    Resolved,
    /// The piece file was parsed.
    Parsed,
    /// The leaf schema was checked.
    Validated,
    /// Accepted for merging.
    Accepted,
    /// Actually present in the successfully returned [`Vtk`].
    Merged,
    /// Skipped under [`ErrorPolicy::Continue`].
    Skipped,
    /// Processing failed.
    Failed,
}

impl PieceStatus {
    pub fn is_processed(self) -> bool {
        !matches!(self, PieceStatus::Pending | PieceStatus::Resolving)
    }
}

/// Canonical location returned by a [`PieceResolver`].
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub enum PieceLocation {
    /// A local filesystem path.
    Path(PathBuf),
    /// A URI not represented as a filesystem path.
    Uri(String),
    /// An opaque resolver-defined key.
    Key(String),
}

impl PieceLocation {
    pub fn display(&self) -> String {
        match self {
            PieceLocation::Path(path) => path.display().to_string(),
            PieceLocation::Uri(uri) => uri.clone(),
            PieceLocation::Key(key) => key.clone(),
        }
    }

    pub fn path(&self) -> Option<&Path> {
        match self {
            PieceLocation::Path(path) => Some(path),
            _ => None,
        }
    }
}

/// A fully resolved piece origin used in reports and relative resolution.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PieceOrigin {
    /// Cache identity, usually a normalized absolute path.
    pub key: String,
    /// Resolver-reported location.
    pub location: PieceLocation,
    /// Local path when available.
    pub path: Option<PathBuf>,
    /// URI when the location is a URI.
    pub uri: Option<String>,
    /// Human-readable representation.
    pub display: String,
}

impl PieceOrigin {
    fn new(key: impl Into<String>, location: PieceLocation) -> Self {
        let display = location.display();
        let path = location.path().map(ToOwned::to_owned);
        let uri = match &location {
            PieceLocation::Uri(uri) => Some(uri.clone()),
            _ => None,
        };
        Self {
            key: key.into(),
            location,
            path,
            uri,
            display,
        }
    }

    fn from_key(key: impl Into<String>) -> Self {
        let key = key.into();
        Self::new(key.clone(), PieceLocation::Key(key))
    }
}

/// Request passed to a [`PieceResolver`].
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PieceRequest {
    /// Raw `Source` value from the XML.
    pub uri: String,
    /// File containing the `Source` attribute.
    pub referencing_file: Option<PathBuf>,
    /// Human-readable location of the referencing file.
    pub referenced_from: Option<String>,
    /// Root directory used by local resolvers, when known.
    pub root: Option<PathBuf>,
    /// Lexically normalized URI/path resolved against the referencing file.
    pub normalized_uri: String,
    /// Filesystem path when [`Self::normalized_uri`] is a path.
    pub resolved_path: Option<PathBuf>,
    /// Active reference chain, excluding this request and including the root.
    pub chain: Vec<String>,
}

impl PieceRequest {
    fn new(uri: &str, base: Option<&BaseRef>, root: Option<&Path>, chain: Vec<String>) -> Self {
        let referencing_file = base.and_then(|b| b.path.clone());
        let referenced_from = base.map(|b| b.display.clone());
        let raw_path = Path::new(uri);

        let mut resolved_path = if let Some(file_uri_path) = file_uri_to_path(uri) {
            Some(file_uri_path)
        } else if looks_like_uri(uri) {
            None
        } else if raw_path.is_absolute() {
            Some(raw_path.to_path_buf())
        } else if let Some(parent) = referencing_file.as_deref().and_then(Path::parent) {
            Some(parent.join(raw_path))
        } else {
            Some(raw_path.to_path_buf())
        };

        if let Some(path) = resolved_path.as_mut() {
            *path = normalize_path(path);
        }

        let normalized_uri = if let Some(path) = &resolved_path {
            path.display().to_string()
        } else if let Some(base_uri) = referenced_from.as_deref().filter(|s| looks_like_uri(s)) {
            resolve_reference_uri(base_uri, uri)
        } else {
            uri.to_string()
        };

        Self {
            uri: uri.to_string(),
            referencing_file,
            referenced_from,
            root: root.map(ToOwned::to_owned),
            normalized_uri,
            resolved_path,
            chain,
        }
    }

    /// Best default cache key for this request.
    pub fn cache_key(&self) -> String {
        self.normalized_uri.clone()
    }
}

/// Bytes and optional metadata returned by a [`PieceResolver`].
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ResolvedPiece {
    /// Complete XML file bytes.
    pub bytes: Vec<u8>,
    /// Actual location, if different from the request normalization.
    pub location: Option<PieceLocation>,
    /// Explicit cache key. Defaults to the request key.
    pub cache_key: Option<String>,
}

impl ResolvedPiece {
    /// Create a resolved piece from its complete XML bytes.
    pub fn new(bytes: impl Into<Vec<u8>>) -> Self {
        Self {
            bytes: bytes.into(),
            location: None,
            cache_key: None,
        }
    }

    /// Convenience constructor for bytes loaded from `path`.
    pub fn from_bytes_and_path(bytes: impl Into<Vec<u8>>, path: impl AsRef<Path>) -> Self {
        let path = path.as_ref().to_path_buf();
        Self {
            bytes: bytes.into(),
            location: Some(PieceLocation::Path(path)),
            cache_key: None,
        }
    }

    /// Read a local path into a resolution.
    pub fn from_path(path: impl AsRef<Path>) -> Result<Self, ResolveError> {
        let path = path.as_ref();
        let bytes = fs::read(path).map_err(|source| ResolveError::Io {
            path: Some(path.to_path_buf()),
            source,
        })?;
        Ok(Self::from_bytes_and_path(bytes, path))
    }

    /// Set the actual location.
    pub fn with_location(mut self, location: PieceLocation) -> Self {
        self.location = Some(location);
        self
    }

    /// Set the local path location.
    pub fn with_path(mut self, path: impl AsRef<Path>) -> Self {
        self.location = Some(PieceLocation::Path(path.as_ref().to_path_buf()));
        self
    }

    /// Set an explicit cache key.
    pub fn with_cache_key(mut self, key: impl Into<String>) -> Self {
        self.cache_key = Some(key.into());
        self
    }
}

impl From<Vec<u8>> for ResolvedPiece {
    fn from(bytes: Vec<u8>) -> Self {
        Self::new(bytes)
    }
}

impl From<&[u8]> for ResolvedPiece {
    fn from(bytes: &[u8]) -> Self {
        Self::new(bytes.to_vec())
    }
}

/// Errors that a [`PieceResolver`] can return.
#[derive(Debug)]
pub enum ResolveError {
    /// The requested piece does not exist.
    NotFound {
        uri: String,
        path: Option<PathBuf>,
        source: Option<io::Error>,
    },
    /// The request escaped the resolver's allowed root.
    AccessDenied { uri: String, path: Option<PathBuf> },
    /// A network URI was passed to a local-only resolver.
    NetworkDenied { uri: String },
    /// Generic IO failure.
    Io {
        path: Option<PathBuf>,
        source: io::Error,
    },
    /// Resolver-defined message.
    Message(String),
    /// Resolver-defined external error.
    Other(Box<dyn StdError + Send + Sync>),
}

impl ResolveError {
    pub fn message(message: impl Into<String>) -> Self {
        ResolveError::Message(message.into())
    }
}

impl fmt::Display for ResolveError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            ResolveError::NotFound { uri, path, source } => {
                write!(f, "piece {uri:?} was not found")?;
                if let Some(path) = path {
                    write!(f, " at {}", path.display())?;
                }
                if let Some(source) = source {
                    write!(f, ": {source}")?;
                }
                Ok(())
            }
            ResolveError::AccessDenied { uri, path } => {
                write!(f, "access to piece {uri:?} is not allowed")?;
                if let Some(path) = path {
                    write!(f, " ({}) is outside the resolver root", path.display())?;
                }
                Ok(())
            }
            ResolveError::NetworkDenied { uri } => {
                write!(
                    f,
                    "network piece URI {uri:?} is not allowed by the local resolver"
                )
            }
            ResolveError::Io { path, source } => {
                write!(f, "IO error while resolving piece")?;
                if let Some(path) = path {
                    write!(f, " {}", path.display())?;
                }
                write!(f, ": {source}")
            }
            ResolveError::Message(message) => f.write_str(message),
            ResolveError::Other(source) => write!(f, "{source}"),
        }
    }
}

impl StdError for ResolveError {
    fn source(&self) -> Option<&(dyn StdError + 'static)> {
        match self {
            ResolveError::NotFound { source, .. } => source.as_ref().map(|e| e as &dyn StdError),
            ResolveError::Io { source, .. } => Some(source),
            ResolveError::Other(source) => Some(source.as_ref()),
            _ => None,
        }
    }
}

impl From<io::Error> for ResolveError {
    fn from(source: io::Error) -> Self {
        ResolveError::Io { path: None, source }
    }
}

/// Strategy for obtaining piece bytes.
///
/// Implement either [`PieceResolver::resolve`] or
/// [`PieceResolver::resolve_piece`]; the provided default forwards between the
/// two names.  Simple closures can be adapted with [`ResolverFn`].
pub trait PieceResolver {
    /// Resolve one piece request to complete XML bytes.
    fn resolve(&self, request: &PieceRequest) -> Result<ResolvedPiece, ResolveError> {
        self.resolve_piece(request)
    }

    /// Alias many downstream implementations may prefer.
    fn resolve_piece(&self, request: &PieceRequest) -> Result<ResolvedPiece, ResolveError> {
        self.resolve(request)
    }
}

impl<T: PieceResolver + ?Sized> PieceResolver for &T {
    fn resolve(&self, request: &PieceRequest) -> Result<ResolvedPiece, ResolveError> {
        (**self).resolve(request)
    }

    fn resolve_piece(&self, request: &PieceRequest) -> Result<ResolvedPiece, ResolveError> {
        (**self).resolve_piece(request)
    }
}

impl<T: PieceResolver + ?Sized> PieceResolver for &mut T {
    fn resolve(&self, request: &PieceRequest) -> Result<ResolvedPiece, ResolveError> {
        (**self).resolve(request)
    }

    fn resolve_piece(&self, request: &PieceRequest) -> Result<ResolvedPiece, ResolveError> {
        (**self).resolve_piece(request)
    }
}

impl<T: PieceResolver + ?Sized> PieceResolver for Box<T> {
    fn resolve(&self, request: &PieceRequest) -> Result<ResolvedPiece, ResolveError> {
        self.as_ref().resolve(request)
    }

    fn resolve_piece(&self, request: &PieceRequest) -> Result<ResolvedPiece, ResolveError> {
        self.as_ref().resolve_piece(request)
    }
}

impl<T: PieceResolver + ?Sized> PieceResolver for Arc<T> {
    fn resolve(&self, request: &PieceRequest) -> Result<ResolvedPiece, ResolveError> {
        self.as_ref().resolve(request)
    }

    fn resolve_piece(&self, request: &PieceRequest) -> Result<ResolvedPiece, ResolveError> {
        self.as_ref().resolve_piece(request)
    }
}

/// Adapter turning a closure into a [`PieceResolver`].
pub struct ResolverFn<F>(pub F);

impl<F> ResolverFn<F> {
    pub fn new(resolve: F) -> Self {
        Self(resolve)
    }
}

impl<F> PieceResolver for ResolverFn<F>
where
    F: Fn(&PieceRequest) -> Result<ResolvedPiece, ResolveError>,
{
    fn resolve(&self, request: &PieceRequest) -> Result<ResolvedPiece, ResolveError> {
        (self.0)(request)
    }
}

/// Resolver that only reads normalized local paths under a fixed root.
#[derive(Clone, Debug)]
pub struct LocalPieceResolver {
    root: PathBuf,
}

/// Conventional name for the safe default filesystem resolver.
pub type DefaultPieceResolver = LocalPieceResolver;
/// Descriptive alias for [`LocalPieceResolver`].
pub type FileSystemPieceResolver = LocalPieceResolver;

impl LocalPieceResolver {
    /// Create a resolver rooted at `root`.
    ///
    /// The root is canonicalized when it exists.  It is an error if it does not
    /// exist or is not a directory.
    pub fn new(root: impl AsRef<Path>) -> Result<Self, ResolveError> {
        let given = root.as_ref();
        let root = fs::canonicalize(given).map_err(|source| ResolveError::Io {
            path: Some(given.to_path_buf()),
            source,
        })?;
        if !root.is_dir() {
            return Err(ResolveError::Message(format!(
                "piece resolver root {} is not a directory",
                root.display()
            )));
        }
        Ok(Self { root })
    }

    /// The canonicalized root directory.
    pub fn root(&self) -> &Path {
        &self.root
    }

    fn ensure_within_root(&self, request: &PieceRequest) -> Result<PathBuf, ResolveError> {
        if is_network_uri(&request.uri) {
            return Err(ResolveError::NetworkDenied {
                uri: request.uri.clone(),
            });
        }

        let path = request
            .resolved_path
            .clone()
            .unwrap_or_else(|| PathBuf::from(&request.normalized_uri));

        // Existing files and symlinks are checked through canonicalization.
        if path.exists() {
            let canonical = fs::canonicalize(&path).map_err(|source| ResolveError::Io {
                path: Some(path.clone()),
                source,
            })?;
            if !canonical.starts_with(&self.root) {
                return Err(ResolveError::AccessDenied {
                    uri: request.uri.clone(),
                    path: Some(canonical),
                });
            }
            return Ok(canonical);
        }

        // For missing files, canonicalize the nearest existing ancestor and
        // reattach the missing tail. This handles both `..` and symlinked
        // temporary directories without weakening root confinement.
        let normalized = normalize_path(&absolute_path(&path));
        let mut ancestor = normalized.clone();
        let mut tail = Vec::new();
        while !ancestor.exists() {
            if let Some(name) = ancestor.file_name() {
                tail.push(name.to_owned());
            } else {
                break;
            }
            if !ancestor.pop() {
                break;
            }
        }

        let canonical_ancestor =
            fs::canonicalize(&ancestor).map_err(|source| ResolveError::Io {
                path: Some(ancestor.clone()),
                source,
            })?;
        let mut candidate = canonical_ancestor;
        for part in tail.iter().rev() {
            candidate.push(part);
        }

        if !candidate.starts_with(&self.root) {
            return Err(ResolveError::AccessDenied {
                uri: request.uri.clone(),
                path: Some(candidate),
            });
        }

        Ok(candidate)
    }
}

impl PieceResolver for LocalPieceResolver {
    fn resolve(&self, request: &PieceRequest) -> Result<ResolvedPiece, ResolveError> {
        let path = self.ensure_within_root(request)?;
        match fs::read(&path) {
            Ok(bytes) => Ok(ResolvedPiece::from_bytes_and_path(bytes, path)),
            Err(source) if source.kind() == io::ErrorKind::NotFound => {
                Err(ResolveError::NotFound {
                    uri: request.uri.clone(),
                    path: Some(path),
                    source: Some(source),
                })
            }
            Err(source) => Err(ResolveError::Io {
                path: Some(path),
                source,
            }),
        }
    }
}

/// Schema of one named point/cell attribute array.
#[derive(Clone, Debug, PartialEq)]
pub struct ArraySchema {
    pub name: String,
    pub scalar_type: ScalarType,
    pub components: u32,
}

/// Schema of one point or cell attribute.
#[derive(Clone, Debug, PartialEq)]
pub struct AttributeSchema {
    pub name: String,
    pub attribute_kind: &'static str,
    pub scalar_type: Option<ScalarType>,
    pub element_type: Option<ElementType>,
    pub components: Option<u32>,
    pub arrays: Vec<ArraySchema>,
}

/// Point and cell attribute schemas for a leaf piece.
#[derive(Clone, Debug, PartialEq, Default)]
pub struct PieceSchema {
    pub point: Vec<AttributeSchema>,
    pub cell: Vec<AttributeSchema>,
}

/// One schema discrepancy.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SchemaDifference {
    /// `"point"` or `"cell"`.
    pub association: String,
    /// Attribute name.
    pub attribute: String,
    /// Human-readable expected signature.
    pub expected: String,
    /// Human-readable actual signature.
    pub actual: String,
    /// Full explanation.
    pub message: String,
}

/// One scalar value range recorded in a piece report.
#[derive(Clone, Debug, PartialEq)]
pub struct AttributeValueRange {
    /// `"point"` or `"cell"`.
    pub association: String,
    /// Attribute or field-array name.
    pub name: String,
    pub scalar_type: Option<ScalarType>,
    pub range_min: Option<f64>,
    pub range_max: Option<f64>,
}

/// Report entry for one `<Piece>` declaration.
#[derive(Clone, Debug, PartialEq)]
pub struct PieceReport {
    pub id: usize,
    pub parent: Option<usize>,
    pub depth: usize,
    /// Stable declaration path, e.g. `[1, 0]` for the first child of piece 1.
    pub order: Vec<usize>,
    pub declaration_index: usize,
    /// Raw `Source` value, if this declaration is a source reference.
    pub uri: Option<String>,
    /// Alias for [`Self::uri`].
    pub reference: Option<String>,
    pub referenced_from: Option<String>,
    pub origin: Option<PieceOrigin>,
    /// Alias for [`Self::origin`].
    pub source: Option<PieceOrigin>,
    pub status: PieceStatus,
    pub processed: bool,
    pub merged: bool,
    pub cache_hit: bool,
    pub resolver_called: bool,
    pub declared_extent: Option<Extent>,
    pub extent: Option<Extent>,
    /// Alias for [`Self::extent`].
    pub data_range: Option<Extent>,
    pub num_points: Option<usize>,
    pub num_cells: Option<usize>,
    pub kind: Option<DataSetKind>,
    pub byte_order: Option<ByteOrder>,
    pub schema: Option<PieceSchema>,
    pub value_ranges: Vec<AttributeValueRange>,
    pub error: Option<String>,
    pub children: Vec<usize>,
}

impl PieceReport {
    fn new(
        id: usize,
        parent: Option<usize>,
        depth: usize,
        order: Vec<usize>,
        declaration_index: usize,
        uri: Option<String>,
        referenced_from: Option<String>,
        declared_extent: Option<Extent>,
        kind: Option<DataSetKind>,
        byte_order: Option<ByteOrder>,
    ) -> Self {
        Self {
            id,
            parent,
            depth,
            order,
            declaration_index,
            reference: uri.clone(),
            uri,
            referenced_from,
            origin: None,
            source: None,
            status: PieceStatus::Pending,
            processed: false,
            merged: false,
            cache_hit: false,
            resolver_called: false,
            declared_extent,
            extent: None,
            data_range: None,
            num_points: None,
            num_cells: None,
            kind,
            byte_order,
            schema: None,
            value_ranges: Vec::new(),
            error: None,
            children: Vec::new(),
        }
    }

    /// Whether this declaration has reached a terminal state.
    pub fn is_processed(&self) -> bool {
        self.status.is_processed()
    }

    /// Whether this declaration was merged into the returned [`Vtk`].
    pub fn is_merged(&self) -> bool {
        self.merged
    }

    /// Return the reported data extent/range.
    pub fn data_range(&self) -> Option<&Extent> {
        self.data_range.as_ref()
    }
}

/// Statistics for one unique cache/source key.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SourceReport {
    pub key: String,
    pub origin: PieceOrigin,
    /// Number of times the resolver was called for this key.
    pub resolver_calls: usize,
    /// Number of successful XML parses for this key (at most one per run).
    pub parse_count: usize,
    /// Number of cache hits after the first load.
    pub cache_hits: usize,
    /// Number of `<Piece>` references pointing to this source.
    pub reference_count: usize,
    /// IDs of [`PieceReport`] entries using this source.
    pub references: Vec<usize>,
}

/// Report returned by [`PieceLoader`].
#[derive(Clone, Debug, Default, PartialEq)]
pub struct PieceLoadReport {
    pub root: Option<PieceOrigin>,
    pub root_path: Option<PathBuf>,
    pub root_kind: Option<DataSetKind>,
    pub pieces: Vec<PieceReport>,
    pub sources: Vec<SourceReport>,
    pub resolver_calls: usize,
    pub parse_calls: usize,
    pub cache_hits: usize,
    pub warnings: Vec<String>,
}

impl PieceLoadReport {
    fn new(root: PieceOrigin) -> Self {
        let root_path = root.path.clone();
        Self {
            root: Some(root),
            root_path,
            ..Default::default()
        }
    }

    /// Reports in stable declaration/DFS order.
    pub fn ordered_pieces(&self) -> Vec<&PieceReport> {
        let mut pieces: Vec<_> = self.pieces.iter().collect();
        pieces.sort_by(|a, b| a.order.cmp(&b.order));
        pieces
    }

    /// IDs of reports with `status == Merged`.
    pub fn merged_ids(&self) -> Vec<usize> {
        self.pieces
            .iter()
            .filter(|p| p.merged)
            .map(|p| p.id)
            .collect()
    }

    /// Reports that were merged into the returned model.
    pub fn merged_pieces(&self) -> Vec<&PieceReport> {
        self.pieces.iter().filter(|p| p.merged).collect()
    }

    /// Reports that are still pending.
    pub fn pending_pieces(&self) -> Vec<&PieceReport> {
        self.pieces
            .iter()
            .filter(|p| p.status == PieceStatus::Pending)
            .collect()
    }

    /// Reports that failed.
    pub fn failed_pieces(&self) -> Vec<&PieceReport> {
        self.pieces
            .iter()
            .filter(|p| p.status == PieceStatus::Failed)
            .collect()
    }

    /// Reports that were skipped under continue policy.
    pub fn skipped_pieces(&self) -> Vec<&PieceReport> {
        self.pieces
            .iter()
            .filter(|p| p.status == PieceStatus::Skipped)
            .collect()
    }

    /// Reports that were processed (including skipped/failed).
    pub fn processed_pieces(&self) -> Vec<&PieceReport> {
        self.pieces.iter().filter(|p| p.processed).collect()
    }

    /// Find a source by cache key or path suffix/display substring.
    pub fn source(&self, key_or_path_fragment: &str) -> Option<&SourceReport> {
        self.sources.iter().find(|s| {
            s.key == key_or_path_fragment
                || s.origin.display.contains(key_or_path_fragment)
                || s.origin
                    .path
                    .as_ref()
                    .is_some_and(|p| p.ends_with(key_or_path_fragment))
        })
    }

    /// Number of references to a source key/path fragment.
    pub fn reference_count_for(&self, key_or_path_fragment: &str) -> usize {
        self.source(key_or_path_fragment)
            .map(|s| s.reference_count)
            .unwrap_or(0)
    }

    /// Parse count for a source key/path fragment.
    pub fn parse_count_for(&self, key_or_path_fragment: &str) -> usize {
        self.source(key_or_path_fragment)
            .map(|s| s.parse_count)
            .unwrap_or(0)
    }

    /// Resolver call count for a source key/path fragment.
    pub fn resolver_calls_for(&self, key_or_path_fragment: &str) -> usize {
        self.source(key_or_path_fragment)
            .map(|s| s.resolver_calls)
            .unwrap_or(0)
    }
}

/// Fatal failure kind for a parallel piece load.
#[derive(Debug)]
pub enum PieceLoadFailure {
    /// The root file could not be imported.
    Import(crate::Error),
    /// The resolver rejected or failed to provide bytes.
    Resolve(ResolveError),
    /// The resolver returned bytes that could not be parsed.
    Parse(crate::Error),
    /// A reference cycle was found.
    Cycle { chain: Vec<String> },
    /// The dataset is not one of the supported VTK grid types.
    UnsupportedDataSet { kind: DataSetKind },
    /// A referenced dataset has the wrong kind.
    KindMismatch {
        piece: String,
        expected: DataSetKind,
        actual: DataSetKind,
    },
    /// Piece byte orders disagree.
    ByteOrderMismatch {
        piece: String,
        expected: ByteOrder,
        actual: ByteOrder,
    },
    /// Point/cell schema or component counts disagree.
    SchemaMismatch {
        piece: String,
        differences: Vec<SchemaDifference>,
    },
}

impl fmt::Display for PieceLoadFailure {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            PieceLoadFailure::Import(source) => {
                write!(f, "failed to import root VTK file: {source}")
            }
            PieceLoadFailure::Resolve(source) => write!(f, "{source}"),
            PieceLoadFailure::Parse(source) => {
                write!(f, "failed to parse resolved piece: {source}")
            }
            PieceLoadFailure::Cycle { chain } => {
                write!(f, "piece reference cycle detected: {}", chain.join(" -> "))
            }
            PieceLoadFailure::UnsupportedDataSet { kind } => {
                write!(f, "unsupported parallel dataset kind: {kind}")
            }
            PieceLoadFailure::KindMismatch {
                piece,
                expected,
                actual,
            } => write!(
                f,
                "dataset kind mismatch in {piece}: expected {expected}, found {actual}"
            ),
            PieceLoadFailure::ByteOrderMismatch {
                piece,
                expected,
                actual,
            } => write!(
                f,
                "byte order mismatch in {piece}: expected {expected:?}, found {actual:?}"
            ),
            PieceLoadFailure::SchemaMismatch { piece, differences } => {
                writeln!(f, "attribute schema mismatch in {piece}:")?;
                for diff in differences {
                    writeln!(f, "  - {}", diff.message)?;
                }
                Ok(())
            }
        }
    }
}

impl StdError for PieceLoadFailure {
    fn source(&self) -> Option<&(dyn StdError + 'static)> {
        match self {
            PieceLoadFailure::Import(source) | PieceLoadFailure::Parse(source) => Some(source),
            PieceLoadFailure::Resolve(source) => Some(source),
            _ => None,
        }
    }
}

impl From<ResolveError> for PieceLoadFailure {
    fn from(e: ResolveError) -> Self {
        PieceLoadFailure::Resolve(e)
    }
}

/// Error returned by customizable piece loading.
///
/// The report is included so callers can inspect processed, pending and failed
/// pieces even when no merged [`Vtk`] is returned.
#[derive(Debug)]
pub struct PieceLoadError {
    pub failure: PieceLoadFailure,
    pub report: PieceLoadReport,
}

impl PieceLoadError {
    fn new(failure: PieceLoadFailure, report: PieceLoadReport) -> Self {
        Self { failure, report }
    }

    /// Convenience accessor matching the public field.
    pub fn report(&self) -> &PieceLoadReport {
        &self.report
    }

    /// Consume the error and return its report.
    pub fn into_report(self) -> PieceLoadReport {
        self.report
    }
}

impl fmt::Display for PieceLoadError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.failure)
    }
}

impl StdError for PieceLoadError {
    fn source(&self) -> Option<&(dyn StdError + 'static)> {
        self.failure.source()
    }
}

/// A successful load paired with its report.
#[derive(Clone, Debug, PartialEq)]
pub struct PieceLoadOutcome {
    pub vtk: Vtk,
    pub report: PieceLoadReport,
}

/// Alias used by some downstream naming conventions.
pub type LoadReport = PieceLoadReport;
/// Alias for resolver request context.
pub type ResolverContext = PieceRequest;
/// Alias for a resolver result payload.
pub type PieceResolution = ResolvedPiece;

/// Load `path` with `resolver` and return the merged VTK plus its report.
pub fn load_with_resolver<R: PieceResolver>(
    path: impl AsRef<Path>,
    resolver: R,
) -> Result<PieceLoadOutcome, PieceLoadError> {
    PieceLoader::new(resolver).load(path)
}

/// Load `path` with a [`LocalPieceResolver`] confined to `root`.
pub fn load_local(
    path: impl AsRef<Path>,
    root: impl AsRef<Path>,
) -> Result<PieceLoadOutcome, PieceLoadError> {
    let resolver = LocalPieceResolver::new(root).map_err(|error| {
        PieceLoadError::new(PieceLoadFailure::Resolve(error), PieceLoadReport::default())
    })?;
    load_with_resolver(path, resolver)
}

/// Loader/builder for custom piece resolution.
///
/// Cached parsed files are owned by a loader and reset at the beginning of each
/// top-level load, so a failed run can never poison a later run after input is
/// fixed.
pub struct PieceLoader<R> {
    resolver: R,
    pub resolve_error_policy: ErrorPolicy,
    cache: HashMap<String, Arc<CachedSource>>,
}

#[derive(Debug)]
struct CachedSource {
    vtk: Vtk,
    origin: PieceOrigin,
}

#[derive(Clone, Debug)]
struct BaseRef {
    origin: PieceOrigin,
    display: String,
    path: Option<PathBuf>,
    byte_order: Option<ByteOrder>,
}

impl BaseRef {
    fn root(origin: &PieceOrigin, byte_order: ByteOrder) -> Self {
        Self {
            origin: origin.clone(),
            display: origin.display.clone(),
            path: origin.path.clone(),
            byte_order: Some(byte_order),
        }
    }

    fn source(origin: &PieceOrigin, byte_order: ByteOrder) -> Self {
        Self::root(origin, byte_order)
    }
}

impl<R: PieceResolver> PieceLoader<R> {
    pub fn new(resolver: R) -> Self {
        Self {
            resolver,
            resolve_error_policy: ErrorPolicy::FailFast,
            cache: HashMap::new(),
        }
    }

    /// Set the policy for missing/unreadable pieces.
    pub fn with_error_policy(mut self, policy: ErrorPolicy) -> Self {
        self.resolve_error_policy = policy;
        self
    }

    /// Skip missing/unreadable pieces and continue loading others.
    pub fn continue_on_errors(mut self) -> Self {
        self.resolve_error_policy = ErrorPolicy::Continue;
        self
    }

    /// Load `path` through [`Vtk::import`], then resolve and merge all pieces.
    pub fn load(&mut self, path: impl AsRef<Path>) -> Result<PieceLoadOutcome, PieceLoadError> {
        let path = path.as_ref();
        let root_origin = path_origin(path);
        let mut report = PieceLoadReport::new(root_origin.clone());

        let mut vtk = match Vtk::import(path) {
            Ok(vtk) => vtk,
            Err(failure) => {
                return Err(PieceLoadError::new(
                    PieceLoadFailure::Import(failure),
                    report,
                ));
            }
        };

        let root_kind = vtk.data.kind();
        report.root_kind = Some(root_kind);
        if matches!(root_kind, DataSetKind::Field) {
            report.warnings.push(format!(
                "{} is a Field dataset and has no pieces to load",
                root_origin.display
            ));
            return Ok(PieceLoadOutcome { vtk, report });
        }

        match self.load_in_place(&mut vtk) {
            Ok(load_report) => {
                report = load_report;
                Ok(PieceLoadOutcome { vtk, report })
            }
            Err(err) => Err(err),
        }
    }

    /// Resolve pieces inside an already imported [`Vtk`].
    ///
    /// On error, `vtk` is left with its original dataset.  On success, all
    /// source references are replaced by ordered inline leaf pieces.
    pub fn load_in_place(&mut self, vtk: &mut Vtk) -> Result<PieceLoadReport, PieceLoadError> {
        // Every top-level run gets a fresh cache.  This deliberately prevents
        // a failed parse/resolution from being reused after the user fixes a
        // file.
        self.cache.clear();

        let root_path = vtk.file_path.clone();
        let root_origin = root_path
            .as_deref()
            .map(path_origin)
            .unwrap_or_else(|| PieceOrigin::from_key("<memory>"));
        let mut report = PieceLoadReport::new(root_origin.clone());
        let root_kind = vtk.data.kind();
        report.root_kind = Some(root_kind);
        if root_kind == DataSetKind::Field {
            report.warnings.push(format!(
                "{} is a Field dataset and has no pieces to load",
                root_origin.display
            ));
            return Ok(report);
        }

        let base = BaseRef::root(&root_origin, vtk.byte_order);
        let mut session = LoadSession {
            resolver: &self.resolver,
            cache: &mut self.cache,
            policy: self.resolve_error_policy,
            report: &mut report,
            root_display: root_origin.display.clone(),
            expected_byte_order: vtk.byte_order,
            expected_kind: root_kind,
            schema: None,
            active_keys: Vec::new(),
            active_displays: Vec::new(),
            accepted_ids: Vec::new(),
        };

        let data = vtk.data.clone();
        match session.merge_data_set(data, None, base, &[]) {
            Ok(merged) => {
                vtk.data = merged;
                for id in &session.accepted_ids {
                    let p = &mut session.report.pieces[*id];
                    p.status = PieceStatus::Merged;
                    p.merged = true;
                    p.processed = true;
                }
                Ok(report)
            }
            Err(failure) => Err(PieceLoadError::new(failure, report)),
        }
    }
}

/// Convenience return type for [`Vtk::import_with_piece_resolver`].
pub type PieceLoadResult = Result<(Vtk, PieceLoadReport), PieceLoadError>;

impl Vtk {
    /// Import a VTK file and load parallel pieces using a custom resolver.
    pub fn import_with_piece_resolver<R: PieceResolver>(
        path: impl AsRef<Path>,
        resolver: R,
    ) -> PieceLoadResult {
        let outcome = PieceLoader::new(resolver).load(path)?;
        Ok((outcome.vtk, outcome.report))
    }

    /// Alias for [`Vtk::import_with_piece_resolver`].
    pub fn import_with_resolver<R: PieceResolver>(
        path: impl AsRef<Path>,
        resolver: R,
    ) -> PieceLoadResult {
        Self::import_with_piece_resolver(path, resolver)
    }

    /// Alias emphasizing the parallel XML use case.
    pub fn import_parallel_with_resolver<R: PieceResolver>(
        path: impl AsRef<Path>,
        resolver: R,
    ) -> PieceLoadResult {
        Self::import_with_piece_resolver(path, resolver)
    }

    /// Short alias for [`Vtk::import_with_piece_resolver`].
    pub fn import_parallel<R: PieceResolver>(
        path: impl AsRef<Path>,
        resolver: R,
    ) -> PieceLoadResult {
        Self::import_with_piece_resolver(path, resolver)
    }

    /// Resolve and merge pieces in an already imported VTK using `resolver`.
    pub fn load_pieces_with_resolver<R: PieceResolver>(
        &mut self,
        resolver: R,
    ) -> Result<PieceLoadReport, PieceLoadError> {
        PieceLoader::new(resolver).load_in_place(self)
    }

    /// Alias matching the existing [`Vtk::load_all_pieces`] naming.
    pub fn load_all_pieces_with_resolver<R: PieceResolver>(
        &mut self,
        resolver: R,
    ) -> Result<PieceLoadReport, PieceLoadError> {
        self.load_pieces_with_resolver(resolver)
    }

    /// Alias emphasizing parallel XML loading.
    pub fn load_parallel_pieces_with_resolver<R: PieceResolver>(
        &mut self,
        resolver: R,
    ) -> Result<PieceLoadReport, PieceLoadError> {
        self.load_pieces_with_resolver(resolver)
    }

    /// Use a [`LocalPieceResolver`] rooted at `root`.
    pub fn import_with_local_resolver(
        path: impl AsRef<Path>,
        root: impl AsRef<Path>,
    ) -> PieceLoadResult {
        let resolver = LocalPieceResolver::new(root).map_err(|e| {
            PieceLoadError::new(PieceLoadFailure::Resolve(e), PieceLoadReport::default())
        })?;
        Self::import_with_piece_resolver(path, resolver)
    }
}

struct LoadSession<'a, R> {
    resolver: &'a R,
    cache: &'a mut HashMap<String, Arc<CachedSource>>,
    policy: ErrorPolicy,
    report: &'a mut PieceLoadReport,
    root_display: String,
    expected_byte_order: ByteOrder,
    expected_kind: DataSetKind,
    schema: Option<PieceSchema>,
    active_keys: Vec<String>,
    active_displays: Vec<String>,
    accepted_ids: Vec<usize>,
}

impl<'a, R: PieceResolver> LoadSession<'a, R> {
    fn merge_data_set(
        &mut self,
        data: DataSet,
        parent: Option<usize>,
        base: BaseRef,
        parent_order: &[usize],
    ) -> Result<DataSet, PieceLoadFailure> {
        Ok(match data {
            DataSet::ImageData {
                extent,
                origin,
                spacing,
                meta: _,
                pieces,
            } => {
                let pieces = self.process_pieces(pieces, parent, base, parent_order)?;
                DataSet::ImageData {
                    extent,
                    origin,
                    spacing,
                    meta: None,
                    pieces,
                }
            }
            DataSet::StructuredGrid {
                extent,
                meta: _,
                pieces,
            } => {
                let pieces = self.process_pieces(pieces, parent, base, parent_order)?;
                DataSet::StructuredGrid {
                    extent,
                    meta: None,
                    pieces,
                }
            }
            DataSet::RectilinearGrid {
                extent,
                meta: _,
                pieces,
            } => {
                let pieces = self.process_pieces(pieces, parent, base, parent_order)?;
                DataSet::RectilinearGrid {
                    extent,
                    meta: None,
                    pieces,
                }
            }
            DataSet::UnstructuredGrid { meta: _, pieces } => {
                let pieces = self.process_pieces(pieces, parent, base, parent_order)?;
                DataSet::UnstructuredGrid { meta: None, pieces }
            }
            DataSet::PolyData { meta: _, pieces } => {
                let pieces = self.process_pieces(pieces, parent, base, parent_order)?;
                DataSet::PolyData { meta: None, pieces }
            }
            field @ DataSet::Field { .. } => field,
        })
    }

    fn process_pieces<P: PieceFamily>(
        &mut self,
        pieces: Vec<Piece<P>>,
        parent: Option<usize>,
        base: BaseRef,
        parent_order: &[usize],
    ) -> Result<Vec<Piece<P>>, PieceLoadFailure> {
        debug_assert_eq!(P::KIND, self.expected_kind);
        let depth = parent_order.len();
        let mut ids = Vec::with_capacity(pieces.len());

        // Pre-create all sibling reports as pending.  This is what lets an
        // error report show the later declarations that were not processed.
        for (index, piece) in pieces.iter().enumerate() {
            let order = parent_order
                .iter()
                .copied()
                .chain(std::iter::once(index))
                .collect::<Vec<_>>();
            let (uri, extent) = source_metadata(piece);
            let id = self.report.pieces.len();
            if let Some(parent_id) = parent {
                self.report.pieces[parent_id].children.push(id);
            }
            self.report.pieces.push(PieceReport::new(
                id,
                parent,
                depth,
                order,
                index,
                uri,
                Some(base.display.clone()),
                extent.cloned(),
                Some(P::KIND),
                base.byte_order,
            ));
            if self.report.pieces[id].uri.is_none() {
                let origin = base.origin.clone();
                let report = &mut self.report.pieces[id];
                report.origin = Some(origin.clone());
                report.source = Some(origin);
            }
            ids.push(id);
        }

        let mut output = Vec::with_capacity(pieces.len());
        for (piece, id) in pieces.into_iter().zip(ids) {
            match self.process_piece::<P>(piece, id, &base) {
                Ok(mut processed) => {
                    let report = &mut self.report.pieces[id];
                    report.processed = true;
                    if matches!(
                        report.status,
                        PieceStatus::Pending | PieceStatus::Resolving | PieceStatus::Resolved
                    ) {
                        report.status = PieceStatus::Parsed;
                    }
                    output.append(&mut processed)
                }
                Err(failure)
                    if failure.is_recoverable_source() && self.policy == ErrorPolicy::Continue =>
                {
                    let message = failure.to_string();
                    let report = &mut self.report.pieces[id];
                    report.status = PieceStatus::Skipped;
                    report.processed = true;
                    report.merged = false;
                    report.error = Some(message.clone());
                    self.report.warnings.push(message);
                }
                Err(failure) => {
                    self.mark_failure(id, &failure);
                    return Err(failure);
                }
            }
        }
        Ok(output)
    }

    fn process_piece<P: PieceFamily>(
        &mut self,
        piece: Piece<P>,
        id: usize,
        base: &BaseRef,
    ) -> Result<Vec<Piece<P>>, PieceLoadFailure> {
        match piece {
            Piece::Inline(data) => {
                self.accept_leaf(data.as_ref(), id)?;
                Ok(vec![Piece::Inline(data)])
            }
            Piece::Loaded(data_set) => {
                if data_set.kind() != P::KIND {
                    return Err(PieceLoadFailure::KindMismatch {
                        piece: self.piece_name(id),
                        expected: P::KIND,
                        actual: data_set.kind(),
                    });
                }
                let pieces = P::extract(*data_set)?;
                self.process_owned_content_or_children::<P>(pieces, id, base.clone())
            }
            Piece::Source(uri, _extent) => {
                let source = self.acquire_source(&uri, id, base)?;
                if source.vtk.data.kind() != P::KIND {
                    return Err(PieceLoadFailure::KindMismatch {
                        piece: self.piece_name(id),
                        expected: P::KIND,
                        actual: source.vtk.data.kind(),
                    });
                }
                self.check_byte_order(source.vtk.byte_order, id)?;
                let byte_order = source.vtk.byte_order;
                let source_base = BaseRef::source(&source.origin, byte_order);
                let pieces = P::extract(source.vtk.data.clone())?;

                self.active_keys.push(source.origin.key.clone());
                self.active_displays.push(source.origin.display.clone());
                let result = self.process_owned_content_or_children::<P>(pieces, id, source_base);
                self.active_keys.pop();
                self.active_displays.pop();
                result
            }
        }
    }

    fn process_owned_content_or_children<P: PieceFamily>(
        &mut self,
        pieces: Vec<Piece<P>>,
        id: usize,
        base: BaseRef,
    ) -> Result<Vec<Piece<P>>, PieceLoadFailure> {
        // The common serial case has exactly one inline <Piece>.  Reuse the
        // referencing report entry for that leaf to keep reports concise.
        if pieces.len() == 1 {
            let piece = pieces.into_iter().next().unwrap();
            if let Piece::Inline(data) = piece {
                self.accept_leaf(data.as_ref(), id)?;
                return Ok(vec![Piece::Inline(data)]);
            } else {
                let order = self.report.pieces[id].order.clone();
                return self.process_pieces(vec![piece], Some(id), base, &order);
            }
        }

        let order = self.report.pieces[id].order.clone();
        self.process_pieces(pieces, Some(id), base, &order)
    }

    fn acquire_source(
        &mut self,
        uri: &str,
        id: usize,
        base: &BaseRef,
    ) -> Result<Arc<CachedSource>, PieceLoadFailure> {
        let chain = self.reference_chain(uri);
        let request = PieceRequest::new(uri, Some(base), None, chain);
        // Canonicalize existing paths before consulting the cache so symlinks
        // and lexical `..` spellings share one resolver/parse result.
        let hint = request
            .resolved_path
            .as_deref()
            .map(canonical_key)
            .unwrap_or_else(|| request.cache_key());

        if self.active_keys.iter().any(|key| key == &hint) {
            return Err(PieceLoadFailure::Cycle {
                chain: self.cycle_chain(&hint),
            });
        }

        if let Some(cached) = self.cache.get(&hint) {
            let cached = Arc::clone(cached);
            self.note_cache_use(id, &cached.origin.key);
            return Ok(cached);
        }

        self.report.pieces[id].status = PieceStatus::Resolving;
        self.report.pieces[id].resolver_called = true;
        self.report.resolver_calls += 1;

        let resolved = match self.resolver.resolve(&request) {
            Ok(resolved) => resolved,
            Err(err) => {
                self.report.pieces[id].status = PieceStatus::Failed;
                self.report.pieces[id].error = Some(err.to_string());
                return Err(PieceLoadFailure::Resolve(err));
            }
        };

        let location = resolved.location.clone().unwrap_or_else(|| {
            request
                .resolved_path
                .clone()
                .map(PieceLocation::Path)
                .unwrap_or_else(|| PieceLocation::Key(request.cache_key()))
        });
        let key = resolved
            .cache_key
            .clone()
            .unwrap_or_else(|| match &location {
                PieceLocation::Path(path) => canonical_key(path),
                _ => location.display(),
            });
        let origin = PieceOrigin::new(key.clone(), location);

        if self.active_keys.iter().any(|active| active == &key) {
            return Err(PieceLoadFailure::Cycle {
                chain: self.cycle_chain(&origin.display),
            });
        }

        if let Some(cached) = self.cache.get(&key) {
            let cached = Arc::clone(cached);
            self.cache.insert(hint.clone(), Arc::clone(&cached));
            self.note_cache_use(id, &key);
            return Ok(cached);
        }

        self.report.pieces[id].status = PieceStatus::Parsed;
        let vtk = match Vtk::parse_xml(resolved.bytes.as_slice()) {
            Ok(vtk) => vtk,
            Err(err) => {
                self.report.pieces[id].status = PieceStatus::Failed;
                self.report.pieces[id].error = Some(err.to_string());
                return Err(PieceLoadFailure::Parse(err));
            }
        };
        self.report.parse_calls += 1;

        let cached = Arc::new(CachedSource {
            vtk,
            origin: origin.clone(),
        });
        self.cache.insert(key.clone(), Arc::clone(&cached));
        if hint != key {
            self.cache.insert(hint, Arc::clone(&cached));
        }

        let source_index = if let Some(index) = self
            .report
            .sources
            .iter()
            .position(|source| source.key == key)
        {
            index
        } else {
            self.report.sources.push(SourceReport {
                key: key.clone(),
                origin: origin.clone(),
                resolver_calls: 0,
                parse_count: 0,
                cache_hits: 0,
                reference_count: 0,
                references: Vec::new(),
            });
            self.report.sources.len() - 1
        };
        let source = &mut self.report.sources[source_index];
        source.resolver_calls += 1;
        source.parse_count += 1;
        source.reference_count += 1;
        source.references.push(id);

        let report = &mut self.report.pieces[id];
        report.origin = Some(origin.clone());
        report.source = Some(origin);
        report.status = PieceStatus::Parsed;

        Ok(cached)
    }

    fn note_cache_use(&mut self, id: usize, key: &str) {
        self.report.cache_hits += 1;
        let report = &mut self.report.pieces[id];
        report.cache_hit = true;
        report.status = PieceStatus::Parsed;
        report.origin = self
            .report
            .sources
            .iter()
            .find(|s| s.key == key)
            .map(|s| s.origin.clone());
        report.source = report.origin.clone();

        if let Some(source) = self.report.sources.iter_mut().find(|s| s.key == key) {
            source.cache_hits += 1;
            source.reference_count += 1;
            source.references.push(id);
        }
    }

    fn accept_leaf<P: PieceFamily>(
        &mut self,
        piece: &P,
        id: usize,
    ) -> Result<(), PieceLoadFailure> {
        let summary = piece.summary();
        let schema = piece.piece_schema();
        let name = self.piece_name(id);

        if let Some(expected) = &self.schema {
            let differences = compare_schema(expected, &schema, &name);
            if !differences.is_empty() {
                let failure = PieceLoadFailure::SchemaMismatch {
                    piece: name,
                    differences,
                };
                self.mark_failure(id, &failure);
                return Err(failure);
            }
        } else {
            self.schema = Some(schema.clone());
        }

        let report = &mut self.report.pieces[id];
        report.status = PieceStatus::Validated;
        report.processed = true;
        report.kind = Some(P::KIND);
        report.schema = Some(schema);
        report.value_ranges = collect_value_ranges(piece.attributes());
        if let Some(extent) = summary.extent {
            report.extent = Some(extent.clone());
            report.data_range = Some(extent);
        }
        report.num_points = summary.num_points;
        report.num_cells = summary.num_cells;
        self.accepted_ids.push(id);
        Ok(())
    }

    fn check_byte_order(&mut self, actual: ByteOrder, id: usize) -> Result<(), PieceLoadFailure> {
        if actual != self.expected_byte_order {
            let failure = PieceLoadFailure::ByteOrderMismatch {
                piece: self.piece_name(id),
                expected: self.expected_byte_order,
                actual,
            };
            self.mark_failure(id, &failure);
            return Err(failure);
        }
        Ok(())
    }

    fn piece_name(&self, id: usize) -> String {
        let p = &self.report.pieces[id];
        if let Some(origin) = &p.origin {
            origin.display.clone()
        } else if let Some(uri) = &p.uri {
            uri.clone()
        } else {
            format!("piece#{}", id)
        }
    }

    fn reference_chain(&self, uri: &str) -> Vec<String> {
        let mut chain = Vec::with_capacity(self.active_displays.len() + 2);
        chain.push(self.root_display.clone());
        chain.extend(self.active_displays.iter().cloned());
        chain.push(uri.to_string());
        chain
    }

    fn cycle_chain(&self, repeated: &str) -> Vec<String> {
        let mut chain = Vec::with_capacity(self.active_displays.len() + 2);
        chain.push(self.root_display.clone());
        chain.extend(self.active_displays.iter().cloned());
        chain.push(repeated.to_string());
        chain
    }

    fn mark_failure(&mut self, id: usize, failure: &PieceLoadFailure) {
        if let Some(report) = self.report.pieces.get_mut(id) {
            report.status = PieceStatus::Failed;
            report.processed = true;
            report.merged = false;
            report.error = Some(failure.to_string());
        }
    }
}

impl PieceLoadFailure {
    fn is_recoverable_source(&self) -> bool {
        matches!(
            self,
            PieceLoadFailure::Resolve(_) | PieceLoadFailure::Parse(_)
        )
    }
}

/// Piece-family-specific extraction and inspection.
trait PieceFamily: PieceInspect + Clone {
    const KIND: DataSetKind;

    fn extract(data: DataSet) -> Result<Vec<Piece<Self>>, PieceLoadFailure>
    where
        Self: Sized;
}

/// Metadata needed in reports and schema validation.
#[derive(Clone, Debug, Default)]
struct PieceSummary {
    extent: Option<Extent>,
    num_points: Option<usize>,
    num_cells: Option<usize>,
}

trait PieceInspect {
    fn inspect(&self) -> PieceSummary;
    fn attributes(&self) -> &crate::model::Attributes;

    fn piece_schema(&self) -> PieceSchema {
        let attrs = self.attributes();
        PieceSchema {
            point: attrs.point.iter().map(attribute_schema).collect(),
            cell: attrs.cell.iter().map(attribute_schema).collect(),
        }
    }
}

macro_rules! impl_family {
    ($piece:ty, $variant:ident, $kind:expr) => {
        impl PieceFamily for $piece {
            const KIND: DataSetKind = $kind;

            fn extract(data: DataSet) -> Result<Vec<Piece<Self>>, PieceLoadFailure> {
                match data {
                    DataSet::$variant { pieces, .. } => Ok(pieces),
                    other => Err(PieceLoadFailure::UnsupportedDataSet { kind: other.kind() }),
                }
            }
        }
    };
}

impl_family!(ImageDataPiece, ImageData, DataSetKind::ImageData);
impl_family!(
    StructuredGridPiece,
    StructuredGrid,
    DataSetKind::StructuredGrid
);
impl_family!(
    RectilinearGridPiece,
    RectilinearGrid,
    DataSetKind::RectilinearGrid
);
impl_family!(
    UnstructuredGridPiece,
    UnstructuredGrid,
    DataSetKind::UnstructuredGrid
);
impl_family!(PolyDataPiece, PolyData, DataSetKind::PolyData);

impl PieceInspect for ImageDataPiece {
    fn inspect(&self) -> PieceSummary {
        let dims = self.extent.clone().into_dims();
        PieceSummary {
            extent: Some(self.extent.clone()),
            num_points: Some((dims[0] * dims[1] * dims[2]) as usize),
            num_cells: Some(self.extent.clone().num_cells() as usize),
        }
    }
    fn attributes(&self) -> &crate::model::Attributes {
        &self.data
    }
}

impl PieceInspect for RectilinearGridPiece {
    fn inspect(&self) -> PieceSummary {
        let dims = self.extent.clone().into_dims();
        PieceSummary {
            extent: Some(self.extent.clone()),
            num_points: Some((dims[0] * dims[1] * dims[2]) as usize),
            num_cells: Some(self.extent.clone().num_cells() as usize),
        }
    }
    fn attributes(&self) -> &crate::model::Attributes {
        &self.data
    }
}

impl PieceInspect for StructuredGridPiece {
    fn inspect(&self) -> PieceSummary {
        let dims = self.extent.clone().into_dims();
        PieceSummary {
            extent: Some(self.extent.clone()),
            num_points: Some((dims[0] * dims[1] * dims[2]) as usize),
            num_cells: Some(self.extent.clone().num_cells() as usize),
        }
    }
    fn attributes(&self) -> &crate::model::Attributes {
        &self.data
    }
}

impl PieceInspect for PolyDataPiece {
    fn inspect(&self) -> PieceSummary {
        PieceSummary {
            extent: None,
            num_points: Some(self.num_points()),
            num_cells: Some(self.num_cells()),
        }
    }
    fn attributes(&self) -> &crate::model::Attributes {
        &self.data
    }
}

impl PieceInspect for UnstructuredGridPiece {
    fn inspect(&self) -> PieceSummary {
        let cells = self
            .cells
            .types
            .len()
            .max(self.cells.cell_verts.num_cells());
        PieceSummary {
            extent: None,
            num_points: Some(self.num_points()),
            num_cells: Some(cells),
        }
    }
    fn attributes(&self) -> &crate::model::Attributes {
        &self.data
    }
}

// `accept_leaf` needs `inspect` even though the trait method is not called
// directly elsewhere; this small extension keeps the summary construction
// explicit.
trait PieceInspectExt {
    fn summary(&self) -> PieceSummary;
}

impl<T: PieceInspect> PieceInspectExt for T {
    fn summary(&self) -> PieceSummary {
        self.inspect()
    }
}

fn source_metadata<P>(piece: &Piece<P>) -> (Option<String>, Option<&Extent>) {
    match piece {
        Piece::Source(uri, extent) => (Some(uri.clone()), extent.as_ref()),
        Piece::Inline(_) => (None, None),
        Piece::Loaded(_) => (None, None),
    }
}

fn attribute_schema(attr: &Attribute) -> AttributeSchema {
    match attr {
        Attribute::DataArray(DataArrayBase { name, elem, data }) => AttributeSchema {
            name: name.clone(),
            attribute_kind: "DataArray",
            scalar_type: Some(data.scalar_type()),
            element_type: Some(elem.clone()),
            components: Some(elem.num_comp()),
            arrays: Vec::new(),
        },
        Attribute::Field { name, data_array } => AttributeSchema {
            name: name.clone(),
            attribute_kind: "Field",
            scalar_type: None,
            element_type: None,
            components: None,
            arrays: data_array
                .iter()
                .map(|a| ArraySchema {
                    name: a.name.clone(),
                    scalar_type: a.data.scalar_type(),
                    components: a.elem,
                })
                .collect(),
        },
    }
}

fn collect_value_ranges(attributes: &Attributes) -> Vec<AttributeValueRange> {
    let mut ranges = Vec::new();
    for (association, list) in [("point", &attributes.point), ("cell", &attributes.cell)] {
        for attribute in list {
            match attribute {
                Attribute::DataArray(array) => {
                    let (min, max) = array.data.compute_range().unzip();
                    ranges.push(AttributeValueRange {
                        association: association.to_string(),
                        name: array.name.clone(),
                        scalar_type: Some(array.data.scalar_type()),
                        range_min: min,
                        range_max: max,
                    });
                }
                Attribute::Field { name, data_array } => {
                    for array in data_array {
                        let (min, max) = array.data.compute_range().unzip();
                        ranges.push(AttributeValueRange {
                            association: association.to_string(),
                            name: format!("{name}/{}", array.name),
                            scalar_type: Some(array.data.scalar_type()),
                            range_min: min,
                            range_max: max,
                        });
                    }
                }
            }
        }
    }
    ranges
}

fn compare_schema(
    expected: &PieceSchema,
    actual: &PieceSchema,
    piece_name: &str,
) -> Vec<SchemaDifference> {
    let mut diffs = Vec::new();
    compare_attribute_list("point", &expected.point, &actual.point, &mut diffs);
    compare_attribute_list("cell", &expected.cell, &actual.cell, &mut diffs);
    let _ = piece_name;
    diffs
}

fn compare_attribute_list(
    association: &str,
    expected: &[AttributeSchema],
    actual: &[AttributeSchema],
    diffs: &mut Vec<SchemaDifference>,
) {
    for exp in expected {
        let Some(act) = actual.iter().find(|a| a.name == exp.name) else {
            diffs.push(SchemaDifference {
                association: association.to_string(),
                attribute: exp.name.clone(),
                expected: format!("{exp:?}"),
                actual: "<missing>".to_string(),
                message: format!(
                    "{association} attribute {:?} is missing in this piece",
                    exp.name
                ),
            });
            continue;
        };

        if exp.attribute_kind != act.attribute_kind {
            diffs.push(SchemaDifference {
                association: association.to_string(),
                attribute: exp.name.clone(),
                expected: exp.attribute_kind.to_string(),
                actual: act.attribute_kind.to_string(),
                message: format!(
                    "{association} attribute {:?} kind mismatch: expected {}, found {}",
                    exp.name, exp.attribute_kind, act.attribute_kind
                ),
            });
        }

        if exp.scalar_type != act.scalar_type {
            diffs.push(SchemaDifference {
                association: association.to_string(),
                attribute: exp.name.clone(),
                expected: format!("{:?}", exp.scalar_type),
                actual: format!("{:?}", act.scalar_type),
                message: format!(
                    "{association} attribute {:?} scalar type mismatch: expected {}, found {}",
                    exp.name,
                    exp.scalar_type.map(|x| x.to_string()).unwrap_or_default(),
                    act.scalar_type.map(|x| x.to_string()).unwrap_or_default()
                ),
            });
        }

        if exp.components != act.components {
            diffs.push(SchemaDifference {
                association: association.to_string(),
                attribute: exp.name.clone(),
                expected: format!("{:?}", exp.components),
                actual: format!("{:?}", act.components),
                message: format!(
                    "{association} attribute {:?} component count mismatch: expected {:?}, found {:?}",
                    exp.name, exp.components, act.components
                ),
            });
        }

        for exp_array in &exp.arrays {
            let Some(act_array) = act.arrays.iter().find(|a| a.name == exp_array.name) else {
                diffs.push(SchemaDifference {
                    association: association.to_string(),
                    attribute: format!("{}/{}", exp.name, exp_array.name),
                    expected: format!("{exp_array:?}"),
                    actual: "<missing>".to_string(),
                    message: format!(
                        "{association} field array {:?} in attribute {:?} is missing",
                        exp_array.name, exp.name
                    ),
                });
                continue;
            };
            if exp_array != act_array {
                diffs.push(SchemaDifference {
                    association: association.to_string(),
                    attribute: format!("{}/{}", exp.name, exp_array.name),
                    expected: format!("{exp_array:?}"),
                    actual: format!("{act_array:?}"),
                    message: format!(
                        "{association} field array {:?} in attribute {:?} schema mismatch: expected type {} with {} components, found type {} with {} components",
                        exp_array.name,
                        exp.name,
                        exp_array.scalar_type,
                        exp_array.components,
                        act_array.scalar_type,
                        act_array.components
                    ),
                });
            }
        }
    }

    for act in actual {
        if !expected.iter().any(|e| e.name == act.name) {
            diffs.push(SchemaDifference {
                association: association.to_string(),
                attribute: act.name.clone(),
                expected: "<absent>".to_string(),
                actual: format!("{act:?}"),
                message: format!(
                    "{association} attribute {:?} is present here but absent in the reference piece",
                    act.name
                ),
            });
        }
    }
}

fn looks_like_uri(s: &str) -> bool {
    scheme(s).is_some()
}

fn is_network_uri(s: &str) -> bool {
    match scheme(s) {
        Some(scheme) => !matches!(scheme.to_ascii_lowercase().as_str(), "file" | "c" | "d"),
        None => false,
    }
}

fn scheme(s: &str) -> Option<&str> {
    let prefix = s.split_once("://")?.0;
    if prefix
        .chars()
        .next()
        .is_some_and(|c| c.is_ascii_alphabetic())
        && prefix
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || matches!(c, '+' | '.' | '-'))
    {
        Some(prefix)
    } else {
        None
    }
}

fn resolve_reference_uri(base: &str, reference: &str) -> String {
    if looks_like_uri(reference) {
        return reference.to_string();
    }

    let Some((scheme, rest)) = base.split_once("://") else {
        return reference.to_string();
    };
    let (authority, base_path) = match rest.find('/') {
        Some(index) => (&rest[..index], &rest[index..]),
        None => (rest, "/"),
    };

    let path = if reference.starts_with("//") {
        return format!("{scheme}:{reference}");
    } else if reference.starts_with('/') {
        reference.to_string()
    } else {
        let directory = base_path.rsplit_once('/').map(|(dir, _)| dir).unwrap_or("");
        format!("{directory}/{reference}")
    };

    let mut segments = Vec::new();
    for segment in path.split('/') {
        match segment {
            "" | "." => {}
            ".." => {
                segments.pop();
            }
            other => segments.push(other),
        }
    }
    format!("{scheme}://{authority}/{}", segments.join("/"))
}

fn file_uri_to_path(s: &str) -> Option<PathBuf> {
    let rest = s.strip_prefix("file://")?;
    // file:///path -> /path, file://localhost/path -> /path.
    let path = if let Some(path) = rest.strip_prefix("localhost/") {
        format!("/{path}")
    } else if rest.starts_with('/') {
        rest.to_string()
    } else {
        // file://host/path is a remote file and has no safe local path.
        return None;
    };
    Some(PathBuf::from(path))
}

fn absolute_path(path: &Path) -> PathBuf {
    if path.is_absolute() {
        path.to_path_buf()
    } else {
        std::env::current_dir()
            .unwrap_or_else(|_| PathBuf::from("."))
            .join(path)
    }
}

fn normalize_path(path: &Path) -> PathBuf {
    let mut normalized = PathBuf::new();
    for component in path.components() {
        match component {
            Component::Prefix(prefix) => normalized.push(prefix.as_os_str()),
            Component::RootDir => normalized.push(Component::RootDir.as_os_str()),
            Component::CurDir => {}
            Component::ParentDir => {
                let last_is_normal = matches!(
                    normalized.components().next_back(),
                    Some(Component::Normal(_))
                );
                if last_is_normal {
                    normalized.pop();
                } else if !path.is_absolute() {
                    normalized.push("..");
                } else {
                    // At an absolute root, "/.." is the root itself.
                }
            }
            Component::Normal(part) => normalized.push(part),
        }
    }
    if normalized.as_os_str().is_empty() {
        normalized.push(".");
    }
    normalized
}

fn canonical_key(path: &Path) -> String {
    if let Ok(canonical) = fs::canonicalize(path) {
        canonical.display().to_string()
    } else {
        normalize_path(&absolute_path(path)).display().to_string()
    }
}

fn path_origin(path: &Path) -> PieceOrigin {
    let key = canonical_key(path);
    let location = if path.exists() {
        PieceLocation::Path(fs::canonicalize(path).unwrap_or_else(|_| path.to_path_buf()))
    } else {
        PieceLocation::Path(normalize_path(&absolute_path(path)))
    };
    PieceOrigin::new(key, location)
}
