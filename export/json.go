package export

import (
	"bytes"
	"encoding/json"
	"fmt"
	"github.com/geziyor/geziyor/internal"
	"os"
)

// JSONLine exports response data as JSON streaming file
type JSONLine struct {
	FileName   string
	EscapeHTML bool
	Prefix     string
	Indent     string
}

// Export exports response data as JSON streaming file
func (e *JSONLine) Export(exports chan interface{}) error {

	// Create or append file
	file, err := os.OpenFile(internal.DefaultString(e.FileName, "out.json"), os.O_RDWR|os.O_CREATE|os.O_APPEND, 0666)
	if err != nil {
		return fmt.Errorf("output file creation error: %w", err)
	}
	defer file.Close()

	// Export data as responses came
	for res := range exports {
		// Encode first so encoding failures and file write failures can be
		// reported distinctly, and neither is silently treated as success.
		var buf bytes.Buffer
		encoder := json.NewEncoder(&buf)
		encoder.SetEscapeHTML(e.EscapeHTML)
		encoder.SetIndent(e.Prefix, e.Indent)
		if err := encoder.Encode(res); err != nil {
			return fmt.Errorf("json encoding error: %w", err)
		}
		if _, err := file.Write(buf.Bytes()); err != nil {
			return fmt.Errorf("json write error: %w", err)
		}
	}

	if err := file.Close(); err != nil {
		return fmt.Errorf("json close error: %w", err)
	}

	return nil
}

// JSON exports response data as JSON
type JSON struct {
	FileName   string
	EscapeHTML bool
}

// Export exports response data as JSON
func (e *JSON) Export(exports chan interface{}) error {

	// Create or append file
	file, err := os.OpenFile(internal.DefaultString(e.FileName, "out.json"), os.O_RDWR|os.O_CREATE|os.O_APPEND, 0666)
	if err != nil {
		return fmt.Errorf("output file creation error: %w", err)
	}
	defer file.Close()

	_, err = file.Write([]byte("[\n"))
	if err != nil {
		return fmt.Errorf("file write error: %w", err)
	}

	// Export data as responses came
	for res := range exports {
		data, err := jsonMarshalLine(res, e.EscapeHTML)
		if err != nil {
			internal.Logger.Printf("JSON encoding error on exporter: %v\n", err)
			continue
		}
		_, err = file.Write(data)
		if err != nil {
			return fmt.Errorf("file write error: %w", err)
		}
	}

	_, err = file.Write([]byte("]\n"))
	if err != nil {
		return fmt.Errorf("file write error: %w", err)
	}

	return nil
}

// jsonMarshalLine adds tab and comma around actual data
func jsonMarshalLine(t interface{}, escapeHTML bool) ([]byte, error) {
	buffer := &bytes.Buffer{}
	encoder := json.NewEncoder(buffer)
	encoder.SetEscapeHTML(escapeHTML)

	buffer.Write([]byte("	"))         // Tab char
	err := encoder.Encode(t)          // Write actual data
	buffer.Truncate(buffer.Len() - 1) // Remove last newline char
	buffer.Write([]byte(",\n"))       // Write comma and newline char

	return buffer.Bytes(), err
}
