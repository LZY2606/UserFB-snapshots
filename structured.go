package opengraph

/**
 * Structured Properties.
 * https://ogp.me/#structured
 */

import "fmt"

// Image represents a structure of "og:image".
// "og:image" might have following properties:
//   - og:image:url
//   - og:image:secure_url
//   - og:image:type
//   - og:image:width
//   - og:image:height
//   - og:image:alt
type Image struct {
	URL       string `json:"url"`
	SecureURL string `json:"secure_url"`
	Type      string `json:"type"` // Content-Type
	Width     int    `json:"width"`
	Height    int    `json:"height"`
	Alt       string `json:"alt"`
}

// Video represents a structure of "og:video".
// "og:video" might have following properties:
//   - og:video:url
//   - og:video:secure_url
//   - og:video:type
//   - og:video:width
//   - og:video:height
type Video struct {
	URL       string `json:"url"`
	SecureURL string `json:"secure_url"`
	Type      string `json:"type"` // Content-Type
	Width     int    `json:"width"`
	Height    int    `json:"height"`
	// Duration in seconds
	Duration int `json:"duration"`
}

// Audio represents a structure of "og:audio".
// "og:audio" might have following properties:
//   - og:audio:url
//   - og:audio:secure_url
//   - og:audio:type
type Audio struct {
	URL       string `json:"url"`
	SecureURL string `json:"secure_url"`
	Type      string `json:"type"` // Content-Type
}

// Favicon represents an extra structure for "shortcut icon".
type Favicon struct {
	URL string `json:"url"`
}

// mediaKind identifies one of the structured media types defined by OGP.
type mediaKind string

const (
	mediaKindImage mediaKind = "image"
	mediaKindVideo mediaKind = "video"
	mediaKindAudio mediaKind = "audio"
)

// NumericPropertyError reports that a width, height, or duration meta property
// did not hold a non-negative decimal integer fitting into int. It carries the
// offending property and its original content so callers can locate the tag.
// The previously collected value of that field is kept untouched.
type NumericPropertyError struct {
	// Property is the full OGP property name, e.g. "og:image:width".
	Property string
	// Content is the raw meta content that could not be parsed.
	Content string
}

// Error implements the error interface.
func (e *NumericPropertyError) Error() string {
	return fmt.Sprintf("opengraph: invalid non-negative integer for %s: %q", e.Property, e.Content)
}
