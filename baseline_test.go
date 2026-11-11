package twitch

import (
	"crypto/rand"
	"crypto/rsa"
	"crypto/tls"
	"crypto/x509"
	"math/big"
	"reflect"
	"sort"
	"testing"
	"time"
)

func ephemeralTLSForTest() (tls.Certificate, error) {
	key, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		return tls.Certificate{}, err
	}
	tmpl := &x509.Certificate{SerialNumber: big.NewInt(1), NotBefore: time.Unix(0, 0), NotAfter: time.Date(2099, 1, 1, 0, 0, 0, 0, time.UTC), DNSNames: []string{"localhost"}, KeyUsage: x509.KeyUsageDigitalSignature | x509.KeyUsageKeyEncipherment, ExtKeyUsage: []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth}}
	der, err := x509.CreateCertificate(rand.Reader, tmpl, tmpl, &key.PublicKey, key)
	return tls.Certificate{Certificate: [][]byte{der}, PrivateKey: key}, err
}
func TestNamesBaseline(t *testing.T) {
	c := NewClient("collector", "placeholder")
	c.Join("alpha")
	lines := []string{":srv 353 collector = #alpha :alice bob", ":srv 366 collector #alpha :End", ":srv 353 collector = #alpha :bob", ":srv 366 collector #alpha :End"}
	for _, line := range lines {
		if err := c.handleLine(line); err != nil {
			t.Fatal(err)
		}
	}
	got, err := c.Userlist("alpha")
	if err != nil {
		t.Fatal(err)
	}
	sort.Strings(got)
	if !reflect.DeepEqual(got, []string{"alice", "bob"}) {
		t.Fatalf("got %v", got)
	}
	if m := ParseMessage(":srv 366 collector #alpha :End"); m.GetType() != END_NAMES {
		t.Fatalf("end marker unexpectedly typed: %T", m)
	}
	if _, ok := ParseMessage(":srv 366 collector #alpha :End").(*EndNamesMessage); !ok {
		t.Fatal("end marker did not parse as EndNamesMessage")
	}
	t.Log("BASELINE_TWITCH_NAMES_ACCUMULATES_STALE_ALICE END_366_TYPED")
}
