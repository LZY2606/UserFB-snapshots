package shp

import (
	"archive/zip"
	"bytes"
	"fmt"
	"io"
	"path"
	"strings"
)

// ZipReader provides an interface for reading Shapefiles that are compressed in a ZIP archive.
type ZipReader struct {
	sr SequentialReader
	z  *zip.ReadCloser
}

// openFromZIP is convenience function for opening the file called name that is
// compressed in z for reading.
func openFromZIP(z *zip.ReadCloser, name string) (io.ReadCloser, error) {
	f := findInZIP(z, name)
	if f == nil {
		return nil, fmt.Errorf("No such file in archive: %s", name)
	}
	return f.Open()
}

// findInZIP returns the ZIP entry called name, or nil if there is none.
func findInZIP(z *zip.ReadCloser, name string) *zip.File {
	for _, f := range z.File {
		if f.Name == name {
			return f
		}
	}
	return nil
}

// zipFpt adapts a single compressed FPT entry of a ZIP archive to
// io.ReaderAt. The entry is decompressed into memory on first use; the rest
// of the archive is left untouched and nothing is written to disk.
type zipFpt struct {
	f    *zip.File
	done bool
	data *bytes.Reader
	err  error
}

func (z *zipFpt) ReadAt(p []byte, off int64) (int, error) {
	if !z.done {
		z.done = true
		rc, err := z.f.Open()
		if err != nil {
			z.err = err
		} else {
			var buf []byte
			buf, err = io.ReadAll(rc)
			rc.Close()
			if err != nil {
				z.err = err
			} else {
				z.data = bytes.NewReader(buf)
			}
		}
	}
	if z.err != nil {
		return 0, z.err
	}
	return z.data.ReadAt(p, off)
}

// memoSourceFromZIP returns a lazily materialized ReaderAt for the FPT
// entry that belongs to the shapefile group with the given prefix (full
// directory and basename inside the archive), or nil if the archive does
// not contain that entry.
func memoSourceFromZIP(z *zip.ReadCloser, prefix string) (io.ReaderAt, int64) {
	f := findInZIP(z, prefix+".fpt")
	if f == nil {
		return nil, 0
	}
	return &zipFpt{f: f}, int64(f.UncompressedSize64)
}

// OpenZip opens a ZIP file that contains a single shapefile.
func OpenZip(zipFilePath string) (*ZipReader, error) {
	z, err := zip.OpenReader(zipFilePath)
	if err != nil {
		return nil, err
	}
	zr := &ZipReader{
		z: z,
	}
	shapeFiles := shapesInZip(z)
	if len(shapeFiles) == 0 {
		return nil, fmt.Errorf("archive does not contain a .shp file")
	}
	if len(shapeFiles) > 1 {
		return nil, fmt.Errorf("archive does contain multiple .shp files")
	}

	shp, err := openFromZIP(zr.z, shapeFiles[0].Name)
	if err != nil {
		return nil, err
	}
	withoutExt := strings.TrimSuffix(shapeFiles[0].Name, ".shp")
	// dbf is optional, so no error checking here
	dbf, _ := openFromZIP(zr.z, withoutExt+".dbf")
	fpt, fptSize := memoSourceFromZIP(zr.z, withoutExt)
	zr.sr = SequentialReaderFromExtWithMemo(shp, dbf, fpt, fptSize)
	return zr, nil
}

// ShapesInZip returns a string-slice with the names (i.e. relatives paths in
// archive file tree) of all shapes that are in the ZIP archive at zipFilePath.
func ShapesInZip(zipFilePath string) ([]string, error) {
	var names []string
	z, err := zip.OpenReader(zipFilePath)
	if err != nil {
		return nil, err
	}
	shapeFiles := shapesInZip(z)
	for i := range shapeFiles {
		names = append(names, shapeFiles[i].Name)
	}
	return names, nil
}

func shapesInZip(z *zip.ReadCloser) []*zip.File {
	var shapeFiles []*zip.File
	for _, f := range z.File {
		if strings.HasSuffix(f.Name, ".shp") {
			shapeFiles = append(shapeFiles, f)
		}
	}
	return shapeFiles
}

// OpenShapeFromZip opens a shape file that is contained in a ZIP archive. The
// parameter name is name of the shape file.
// The name of the shapefile must be a relative path: it must not start with a
// drive letter (e.g. C:) or leading slash, and only forward slashes are
// allowed. These rules are the same as in
// https://golang.org/pkg/archive/zip/#FileHeader.
func OpenShapeFromZip(zipFilePath string, name string) (*ZipReader, error) {
	z, err := zip.OpenReader(zipFilePath)
	if err != nil {
		return nil, err
	}
	zr := &ZipReader{
		z: z,
	}

	shp, err := openFromZIP(zr.z, name)
	if err != nil {
		return nil, err
	}
	// dbf is optional, so no error checking here
	prefix := strings.TrimSuffix(name, path.Ext(name))
	dbf, _ := openFromZIP(zr.z, prefix+".dbf")
	fpt, fptSize := memoSourceFromZIP(zr.z, prefix)
	zr.sr = SequentialReaderFromExtWithMemo(shp, dbf, fpt, fptSize)
	return zr, nil
}

// Close closes the ZipReader and frees the allocated resources.
func (zr *ZipReader) Close() error {
	s := ""
	err := zr.sr.Close()
	if err != nil {
		s += err.Error() + ". "
	}
	err = zr.z.Close()
	if err != nil {
		s += err.Error() + ". "
	}
	if s != "" {
		return fmt.Errorf(s)
	}
	return nil
}

// Next reads the next shape in the shapefile and the next row in the DBF. Call
// Shape() and Attribute() to access the values.
func (zr *ZipReader) Next() bool {
	return zr.sr.Next()
}

// Shape returns the shape that was last read as well as the current index.
func (zr *ZipReader) Shape() (int, Shape) {
	return zr.sr.Shape()
}

// Attribute returns the n-th field of the last row that was read. If there
// were any errors before, the empty string is returned.
func (zr *ZipReader) Attribute(n int) string {
	return zr.sr.Attribute(n)
}

// Fields returns a slice of Fields that are present in the
// DBF table.
func (zr *ZipReader) Fields() []Field {
	return zr.sr.Fields()
}

// Err returns the last non-EOF error that was encountered by this ZipReader.
func (zr *ZipReader) Err() error {
	return zr.sr.Err()
}

// Memo returns the memo value of the n-th field of the physical row that
// was read by the most recent successful call to Next. The memo content is
// read from the FPT entry that shares the directory and basename of the
// selected .shp entry inside the archive. A missing FPT entry does not
// affect geometry or plain attribute reads; only Memo fails then.
func (zr *ZipReader) Memo(field int) (MemoValue, error) {
	if m, ok := zr.sr.(SequentialReaderWithMemo); ok {
		return m.Memo(field)
	}
	return MemoValue{}, fmt.Errorf("the underlying reader has no memo support: %w",
		ErrMemoUnavailable)
}
