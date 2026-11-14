package capture

// Options configures a capture Run.
type Options struct {
	// Dir is an existing, empty directory where published sample
	// directories are written.
	Dir string

	// Workers is the maximum number of paused responses processed
	// concurrently. It must be positive.
	Workers int

	// MaxBodyBytes is the maximum accepted decoded response body size
	// in bytes. It must be positive.
	MaxBodyBytes int64
}
