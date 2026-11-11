package middleware

import (
	"github.com/geziyor/geziyor/client"
	"github.com/geziyor/geziyor/internal"
	"sync"
)

// AllowedDomains checks for request host if it exists in AllowedDomains
type AllowedDomains struct {
	AllowedDomains []string
	logOnlyOnce    sync.Map
}

func (a *AllowedDomains) ProcessRequest(r *client.Request) {
	if err := a.Check(r); err != nil {
		if _, logged := a.logOnlyOnce.LoadOrStore(r.Host, struct{}{}); !logged {
			internal.Logger.Printf("Domain not allowed: %s\n", r.Host)
		}
		r.Cancel()
	}
}

// Check reports whether r is allowed. AllowedDomains are matched exactly
// against r.Host (host:port); an empty list allows every domain.
// It returns a *client.RequestDeniedError when the domain is not allowed.
func (a *AllowedDomains) Check(r *client.Request) error {
	if len(a.AllowedDomains) == 0 || internal.ContainsString(a.AllowedDomains, r.Host) {
		return nil
	}
	return &client.RequestDeniedError{
		URL:    r.URL.String(),
		Reason: "domain not allowed: " + r.Host,
	}
}
