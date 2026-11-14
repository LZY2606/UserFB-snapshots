package main

import (
 "fmt"
 "io"
 "net"
 "os"
 "reflect"

 ldap "github.com/mavricknz/ldap"
 ber "github.com/mavricknz/asn1-ber"
)

type capture struct { entries, groups, alternatives int }
func (h *capture) ProcessDiscreteResult(r *ldap.DiscreteSearchResult, _ *ldap.ConnectionInfo) (bool, error) {
 switch r.SearchResultType {
 case ldap.SearchResultEntry: h.entries++
 case ldap.SearchResultReference: h.groups++; h.alternatives += len(r.Referrals)
 }
 return false, nil
}

func run() error {
 client, server := net.Pipe()
 serverDone := make(chan error, 1)
 go func() {
  defer server.Close()
  for i := 0; i < 2; i++ {
   req, err := ber.ReadPacket(server)
   if err != nil { serverDone <- err; return }
   if len(req.Children) < 2 || req.Children[1].Tag != ldap.ApplicationSearchRequest {
    serverDone <- fmt.Errorf("expected a production LDAP search"); return
   }
   id, ok := req.Children[0].Value.(uint64)
   if !ok { serverDone <- fmt.Errorf("missing message ID"); return }
   send := func(op *ber.Packet) error {
    packet := ber.Encode(ber.ClassUniversal, ber.TypeConstructed, ber.TagSequence, nil, "LDAP response")
    packet.AppendChild(ber.NewInteger(ber.ClassUniversal, ber.TypePrimative, ber.TagInteger, id, "message ID"))
    packet.AppendChild(op)
    _, err := server.Write(packet.Bytes()); return err
   }
   entry := ber.Encode(ber.ClassApplication, ber.TypeConstructed, ldap.SearchResultEntry, nil, "entry")
   entry.AppendChild(ber.NewString(ber.ClassUniversal, ber.TypePrimative, ber.TagOctetString, "cn=normal,dc=example", "DN"))
   entry.AppendChild(ber.Encode(ber.ClassUniversal, ber.TypeConstructed, ber.TagSequence, nil, "attributes"))
   if err := send(entry); err != nil { serverDone <- err; return }
   refs := ber.Encode(ber.ClassApplication, ber.TypeConstructed, ldap.SearchResultReference, nil, "reference")
   for _, uri := range []string{"ldap://first.invalid/dc=people??sub?(uid=*)", "ldap://second.invalid/dc=people??sub?(uid=*)"} {
    refs.AppendChild(ber.NewString(ber.ClassUniversal, ber.TypePrimative, ber.TagOctetString, uri, "URI"))
   }
   if err := send(refs); err != nil { serverDone <- err; return }
   done := ber.Encode(ber.ClassApplication, ber.TypeConstructed, ldap.SearchResultDone, nil, "done")
   done.AppendChild(ber.NewInteger(ber.ClassUniversal, ber.TypePrimative, ber.TagEnumerated, 0, "result code"))
   done.AppendChild(ber.NewString(ber.ClassUniversal, ber.TypePrimative, ber.TagOctetString, "", "matched DN"))
   done.AppendChild(ber.NewString(ber.ClassUniversal, ber.TypePrimative, ber.TagOctetString, "", "diagnostic"))
   if err := send(done); err != nil { serverDone <- err; return }
  }
  // Keep the response stream alive until the borrowed connection is closed.
  var b [1]byte
  _, err := server.Read(b[:])
  if err != nil && err != io.EOF { serverDone <- err; return }
  serverDone <- nil
 }()
 c := ldap.NewLDAPConnection("root.invalid", 389)
 c.Dialer = ldap.Dialer(func(_, _ string) (net.Conn, error) { return client, nil })
 if err := c.Connect(); err != nil { return err }
 req := ldap.NewSimpleSearchRequest("dc=example", ldap.ScopeWholeSubtree, "(objectClass=*)", []string{"cn"})
 result, err := c.Search(req)
 if err != nil { c.Close(); return err }
 if len(result.Entries) != 1 || len(result.Referrals) != 2 { c.Close(); return fmt.Errorf("unexpected raw result") }
 fmt.Println("BASELINE_RAW_ONLY entries=1 referrals=2")
 handler := new(capture)
 if err := c.SearchWithHandler(req, handler, nil); err != nil { c.Close(); return err }
 if handler.entries != 1 || handler.groups != 1 || handler.alternatives != 2 { c.Close(); return fmt.Errorf("reference group was not preserved") }
 fmt.Println("BASELINE_HANDLER entries=1 groups=1 alternatives=2")
 _, present := reflect.TypeOf(*c).FieldByName("ReferralPolicy")
 fmt.Printf("AUTO_REFERRAL_POLICY_PRESENT=%t\n", present)
 c.Close()
 return <-serverDone
}

func main() {
 if err := run(); err != nil { fmt.Fprintln(os.Stderr, err); os.Exit(1) }
}
