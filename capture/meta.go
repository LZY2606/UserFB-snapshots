package capture

import "github.com/mafredri/cdp/protocol/fetch"

// meta is the content of each sample's meta.json file.
type meta struct {
	RequestID string              `json:"request_id"`
	URL       string              `json:"url"`
	Status    int                 `json:"status"`
	Headers   []fetch.HeaderEntry `json:"headers"`
	Size      int64               `json:"size"`
	SHA256    string              `json:"sha256"`
}
