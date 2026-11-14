# Network interception API

All declarations belong to package selenium. Keep the WebDriver interface source compatible. Add `network_bidi.go` (additional production files are allowed).

```
func OpenNetwork(wd WebDriver) (*Network, error)
func (n *Network) Add(phases []string, patterns []string) (*NetworkIntercept, error)
func (n *Network) Next(ctx context.Context) (*NetworkPause, error)
func (n *Network) Close() error
func (i *NetworkIntercept) ID() string
func (i *NetworkIntercept) Remove() error

type NetworkBytes struct { Type string `json:"type"`; Value string `json:"value"` }
type NetworkHeader struct { Name string `json:"name"`; Value NetworkBytes `json:"value"` }
type NetworkPause struct {
    RequestID string
    RedirectCount int
    Phase string
    Context string
    Intercepts []string
    Raw json.RawMessage
    // private fields allowed
}
func (p *NetworkPause) Continue(headers []NetworkHeader) error
func (p *NetworkPause) Fail() error
```

`NewRemote` with `webSocketUrl:true` must preserve the flag in W3C alwaysMatch and retain the returned `value.capabilities.webSocketUrl`. OpenNetwork uses this exact negotiated endpoint without GET capabilities. Allow one successful opening per driver, only for its initial session; SwitchSession and reconnect are outside this feature. OpenNetwork subscribes to `network.beforeRequestSent` and `network.responseStarted`, then returns after the subscription success. Closing Network closes its socket and pending operations, not the HTTP session. Missing negotiation reports an error. No browser launcher is required.

Add accepts nonempty phases containing only `beforeRequestSent`/`responseStarted` and optional URL strings. Each pattern becomes `{"type":"string","pattern":s}` in `urlPatterns`; nil/empty patterns omit that field. Send network.addIntercept with phases. Its success result contains `intercept`. Copy caller slices. Remove sends network.removeIntercept with intercept. Confirmed success is idempotent; confirmed error retains the rule for a later explicit retry. Concurrent Remove calls share one outstanding command. Removal never consumes an already received pause, including a pause queued but not yet read by Next. Rules have no local URL matcher.

Events carry `type:"event"`, method and params, for example:
```
{"type":"event","method":"network.beforeRequestSent","params":{"context":"tab","isBlocked":true,"redirectCount":0,"intercepts":["A","B"],"request":{"request":"r1","url":"https://form.invalid/send","method":"POST","headers":[]}}}
```
Response events use `network.responseStarted` and additionally have `response`. Phase is the suffix of method. Ignore events with isBlocked=false. Preserve params byte-for-byte in Raw, including unknown fields, without aliasing user slices. One pause per `(request.request, redirectCount, phase)`, even with multiple intercept IDs or duplicate events. Arrival order defines Next order. Receiving a different phase or redirectCount for the same request retires its older pause for future decisions. Different request IDs remain independent. Assume one monotonic redirect sequence per request, no recycled request IDs or out-of-order event stream. Reply order may differ from event order.

Continue uses network.continueRequest for beforeRequestSent, network.continueResponse for responseStarted, with request plus optional headers. Nil headers omits the member, nonnil empty headers sends `[]`. Header order, duplicate names and BytesValue type/value must survive unchanged. Fail uses network.failRequest with request only. Commands never include the local redirectCount/phase. A second decision on the same completed, in-flight or superseded pause is rejected locally, with no frame. A protocol error leaves the same pause available for an explicit retry only if no successor has arrived. A successful reply completes exactly its own pause: if a successor event arrived while awaiting that reply, the successor must remain usable. No automatic replay after errors. Transport failure ends the connection and all waiters. Next's context cancels only that read. Never synchronously await a command response in the receive loop.

Command IDs are unique per connection; responses contain type success/result or error/error/message. Preserve protocol code and message in returned errors. Serialize WebSocket writers. One blocked Continue must not prevent another request's event and decision. The supplied Gorilla source handles framing; it contains no BiDi client.

Regression entry: task_support/network_target_test.go (external package selenium_test), groups TestNetworkRegistration, TestNetworkPausedPhases, TestNetworkRuleRemoval. Use task_support/taskcheck.Run and c.Check in real content assertions. Use channel handshakes with a local Gorilla peer, not sleeps. Include actual NewRemote -> OpenNetwork negotiation, early event before addIntercept reply, multi-rule deduplication, successive phases/redirects while old reply is held, nil/empty/repeated headers, rejected decision followed by retry, failed removal followed by success with a queued pause still usable, independent requests, connection loss and preserved HTTP behavior. Each group must return normally. Add a Go README example of registering, receiving, deciding and removing.

Protocol reference: https://www.w3.org/TR/2026/WD-webdriver-bidi-20261008/#module-network
