package graphql

import (
	"bytes"
	"context"
	"encoding/json"
	stderrors "errors"
	"fmt"
	"io"
	"mime"
	"net/http"
	"strings"

	"github.com/shurcooL/graphql/internal/jsonutil"
)

// Location is a location in a GraphQL request document where an error occurred.
//
// See https://spec.graphql.org/October2021/#sec-Errors.
type Location struct {
	Line   int `json:"line"`
	Column int `json:"column"`
}

// GraphQLError is a single entry of the "errors" array in a GraphQL response.
//
// See https://spec.graphql.org/October2021/#sec-Errors.
type GraphQLError struct {
	Message   string     `json:"message"`
	Path      []any      `json:"path,omitempty"`
	Locations []Location `json:"locations,omitempty"`
}

// IncrementalResult is the outcome of one fully applied response part:
// the first MIME part or one batch of incremental items, or the single
// document of an application/json response.
type IncrementalResult struct {
	// HasNext reports whether the server promised another part.
	HasNext bool
	// Errors contains the GraphQL business errors carried by this part.
	// Their paths are absolute response paths.
	Errors []GraphQLError
}

// graphQLBusinessError aggregates every GraphQL business error seen across
// the whole response. Protocol, transport and callback errors are returned
// directly instead.
type graphQLBusinessError struct {
	errs []GraphQLError
}

func (e *graphQLBusinessError) Error() string {
	var b strings.Builder
	for i, err := range e.errs {
		if i != 0 {
			b.WriteString("; ")
		}
		b.WriteString(err.Message)
	}
	return fmt.Sprintf("graphql: response contained %d error(s): %s", len(e.errs), b.String())
}

// envelope is the JSON document carried by one response part.
type envelope struct {
	Data        *json.RawMessage `json:"data"`
	Errors      []GraphQLError   `json:"errors"`
	HasNext     bool             `json:"hasNext"`
	Incremental []incrementalItem
}

// incrementalItem is one entry of an "incremental" array. Exactly one of
// Data and Items is set, and Path is always present.
type incrementalItem struct {
	Data   *json.RawMessage `json:"data"`
	Items  *json.RawMessage `json:"items"`
	Errors []GraphQLError   `json:"errors"`
	Path   []any            `json:"path"`
}

// QueryIncremental executes a single GraphQL query request and streams the
// response into q, invoking onPart once after every fully applied part.
//
// The query is derived from q exactly as in Query, and exactly one POST is
// sent through the configured http.Client. Two response content types are
// supported:
//
//   - application/json: a single ordinary GraphQL document, delivered through
//     one onPart call with HasNext false.
//   - multipart/mixed: a streaming incremental response. The first part carries
//     data and hasNext; later parts carry an "incremental" array and hasNext.
//
// All incremental items of one part are applied as a transaction: if one of
// them is invalid, the whole part is rolled back and onPart is not invoked for
// it. GraphQL business errors do not stop delivery; they are surfaced through
// IncrementalResult.Errors and, once the response has been read completely,
// through the returned error. A callback error is returned unchanged so that
// errors.Is keeps working.
func (c *Client) QueryIncremental(ctx context.Context, q any, variables map[string]any, onPart func(IncrementalResult) error) error {
	req, err := c.newRequest(ctx, queryOperation, q, variables)
	if err != nil {
		return err
	}
	resp, err := c.httpClient.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(resp.Body)
		return fmt.Errorf("non-200 OK status code: %v body: %q", resp.Status, body)
	}

	mediaType, params, err := mime.ParseMediaType(resp.Header.Get("Content-Type"))
	if err != nil {
		return fmt.Errorf("invalid response Content-Type: %w", err)
	}
	switch mediaType {
	case "application/json":
		return c.deliverJSON(resp.Body, q, onPart)
	case "multipart/mixed":
		boundary := params["boundary"]
		if boundary == "" {
			return fmt.Errorf("multipart/mixed response is missing a boundary")
		}
		return c.deliverMultipart(resp.Body, boundary, q, onPart)
	default:
		return fmt.Errorf("unsupported response Content-Type %q", mediaType)
	}
}

// deliverJSON handles an ordinary single-document application/json response.
// It goes through the same callback entry point as a multipart response,
// invoking onPart exactly once with HasNext false.
func (c *Client) deliverJSON(r io.Reader, q any, onPart func(IncrementalResult) error) error {
	dec := json.NewDecoder(r)
	dec.UseNumber()
	var env envelope
	if err := dec.Decode(&env); err != nil {
		return fmt.Errorf("decoding GraphQL response: %w", err)
	}
	if tok, err := dec.Token(); err != io.EOF {
		if err == nil {
			return fmt.Errorf("invalid token '%v' after top-level GraphQL response", tok)
		}
		return err
	}

	var business []GraphQLError
	if env.Data != nil {
		if err := jsonutil.UnmarshalGraphQL(*env.Data, q); err != nil {
			return err
		}
	}
	for i := range env.Errors {
		if err := jsonutil.ParsePath(env.Errors[i].Path); err != nil {
			return err
		}
	}
	business = appendBusinessErrors(business, env.Errors)

	result := IncrementalResult{HasNext: false, Errors: env.Errors}
	if err := onPart(result); err != nil {
		return err
	}
	if len(business) > 0 {
		return &graphQLBusinessError{errs: business}
	}
	return nil
}

// deliverMultipart streams a multipart/mixed response, applying and delivering
// one MIME part at a time without buffering the whole response.
func (c *Client) deliverMultipart(r io.Reader, boundary string, q any, onPart func(IncrementalResult) error) error {
	reader := newMixedStreamReader(r, boundary)
	var business []GraphQLError
	completed := false
	partNo := 0
	for {
		body, err := reader.NextPart()
		if err == io.EOF {
			if !completed {
				return fmt.Errorf("incremental stream ended before a final part with hasNext=false")
			}
			break
		}
		if err != nil {
			if completed && stderrors.Is(err, io.ErrUnexpectedEOF) {
				return fmt.Errorf("incremental stream was truncated after the final part: %w", err)
			}
			if !completed && stderrors.Is(err, io.ErrUnexpectedEOF) {
				return fmt.Errorf("incremental stream was truncated before a final part with hasNext=false: %w", err)
			}
			return fmt.Errorf("reading multipart part %d: %w", partNo+1, err)
		}
		if completed {
			return fmt.Errorf("received another MIME part after the final part with hasNext=false")
		}

		result, applyErr := applyPart(q, body, partNo)
		if applyErr != nil {
			return fmt.Errorf("applying multipart part %d: %w", partNo+1, applyErr)
		}
		if err := onPart(result); err != nil {
			// Return the callback error unchanged to preserve errors.Is identity.
			return err
		}
		business = appendBusinessErrors(business, result.Errors)
		completed = !result.HasNext
		partNo++
	}
	if len(business) > 0 {
		return &graphQLBusinessError{errs: business}
	}
	return nil
}

// applyPart decodes one MIME part and applies it to q. The first part is the
// initial response document; later parts contain incremental items, which are
// applied as a single transaction.
func applyPart(q any, body []byte, partNo int) (IncrementalResult, error) {
	dec := json.NewDecoder(bytes.NewReader(body))
	dec.UseNumber()
	var env envelope
	if err := dec.Decode(&env); err != nil {
		return IncrementalResult{}, fmt.Errorf("part is not valid JSON: %w", err)
	}
	if tok, err := dec.Token(); err != io.EOF {
		if err == nil {
			return IncrementalResult{}, fmt.Errorf("invalid token '%v' after part document", tok)
		}
		return IncrementalResult{}, err
	}

	if partNo == 0 {
		return applyInitialPart(q, env)
	}
	return applyIncrementalPart(q, env)
}

// applyInitialPart applies the first part, which carries data and hasNext.
func applyInitialPart(q any, env envelope) (IncrementalResult, error) {
	if len(env.Incremental) != 0 {
		return IncrementalResult{}, fmt.Errorf("unexpected \"incremental\" array in the first part")
	}
	if env.Data != nil {
		if err := jsonutil.UnmarshalGraphQL(*env.Data, q); err != nil {
			return IncrementalResult{}, err
		}
	}
	for i := range env.Errors {
		if err := jsonutil.ParsePath(env.Errors[i].Path); err != nil {
			return IncrementalResult{}, err
		}
	}
	return IncrementalResult{HasNext: env.HasNext, Errors: env.Errors}, nil
}

// applyIncrementalPart applies one part's incremental items atomically.
func applyIncrementalPart(q any, env envelope) (IncrementalResult, error) {
	if env.Data != nil {
		return IncrementalResult{}, fmt.Errorf("unexpected \"data\" in an incremental part")
	}
	if env.HasNext && len(env.Incremental) == 0 {
		return IncrementalResult{}, fmt.Errorf("part with hasNext=true carries no incremental items")
	}

	txn := jsonutil.NewTransaction(q)
	var partErrors []GraphQLError
	// Part-level errors (outside any incremental item) carry absolute paths;
	// validate them and keep them alongside the item-level ones.
	for i := range env.Errors {
		if err := jsonutil.ParsePath(env.Errors[i].Path); err != nil {
			txn.Rollback()
			return IncrementalResult{}, err
		}
		partErrors = append(partErrors, env.Errors[i])
	}
	for _, item := range env.Incremental {
		if (item.Data == nil) == (item.Items == nil) {
			txn.Rollback()
			return IncrementalResult{}, fmt.Errorf("incremental item must contain exactly one of data or items")
		}
		if item.Path == nil {
			txn.Rollback()
			return IncrementalResult{}, fmt.Errorf("incremental item is missing a path")
		}
		if err := jsonutil.ParsePath(item.Path); err != nil {
			txn.Rollback()
			return IncrementalResult{}, err
		}
		for i := range item.Errors {
			if err := jsonutil.ParsePath(item.Errors[i].Path); err != nil {
				txn.Rollback()
				return IncrementalResult{}, err
			}
			abs := make([]any, 0, len(item.Path)+len(item.Errors[i].Path))
			abs = append(abs, item.Path...)
			abs = append(abs, item.Errors[i].Path...)
			item.Errors[i].Path = abs
		}
		partErrors = append(partErrors, item.Errors...)

		var err error
		if item.Data != nil {
			err = txn.ApplyData(*item.Data, item.Path)
		} else {
			err = txn.ApplyItems(*item.Items, item.Path)
		}
		if err != nil {
			txn.Rollback()
			return IncrementalResult{}, err
		}
	}
	return IncrementalResult{HasNext: env.HasNext, Errors: partErrors}, nil
}

// appendBusinessErrors appends src to dst, normalizing nil to keep the
// aggregate slice nil when no error ever occurred.
func appendBusinessErrors(dst, src []GraphQLError) []GraphQLError {
	if len(src) == 0 {
		return dst
	}
	return append(dst, src...)
}
