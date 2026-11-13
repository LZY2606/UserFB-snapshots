package links

import "net/url"

const jsScheme = "javascript"

// CleanURL resolves and normalizes a reference found in a document addressed
// by base, following the same rules as html and css extraction. It returns
// false for references that cannot be used.
func CleanURL(base *url.URL, link string) (rv string, ok bool) {
	return cleanURL(base, link)
}

func cleanURL(base *url.URL, link string) (rv string, ok bool) {
	u, err := url.Parse(link)
	if err != nil {
		return
	}

	if u.Host == "" {
		if u = base.ResolveReference(u); u.Host == "" {
			return
		}
	}

	switch u.Scheme {
	case jsScheme:
		return
	case "":
		u.Scheme = base.Scheme
	}

	if u.Path == "" {
		u.Path = "/"
	}

	u.Fragment = ""

	return u.String(), true
}
