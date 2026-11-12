//go:build trap && baselinecheck

package gosnmp

import (
	"bytes"
	"fmt"
	"net"
	"strconv"
	"testing"
	"time"

	"github.com/stretchr/testify/require"
)

// This test records the existing behavior; it is not a regression for a new API.
func TestInformBaseline(t *testing.T) {
	const engine = "\x80\x00\x1f\x88\x04collector"
	credentials := func() *UsmSecurityParameters {
		sp := secParamsList[1].Copy().(*UsmSecurityParameters)
		sp.AuthenticationProtocol = SHA
		sp.AuthoritativeEngineID = engine
		sp.AuthoritativeEngineBoots = 7
		sp.AuthoritativeEngineTime = 900
		sp.SecretKey, sp.PrivacyKey = nil, nil
		require.NoError(t, sp.InitSecurityKeys())
		return sp
	}
	connect := func(tl *TrapListener) net.Conn {
		t.Helper()
		host, port := listenOnEphemeral(t, tl)
		conn, err := net.Dial("udp", net.JoinHostPort(host, strconv.Itoa(int(port))))
		require.NoError(t, err)
		t.Cleanup(func() { conn.Close(); tl.Close() })
		require.NoError(t, conn.SetDeadline(time.Now().Add(2*time.Second)))
		return conn
	}
	t.Run("normal_and_stale_informs", func(t *testing.T) {
		tl := NewTrapListener()
		tl.Params = newTestGoSNMPv3(AuthPriv, credentials())
		received := make(chan uint32, 2)
		tl.OnNewTrap = func(p *SnmpPacket, _ *net.UDPAddr) { received <- p.RequestID }
		conn := connect(tl)
		for i, boots := range []uint32{7, 1} {
			sender := credentials()
			sender.AuthoritativeEngineBoots = boots
			sender.AuthoritativeEngineTime = 900
			if i == 1 {
				sender.AuthoritativeEngineTime = 1
			}
			sender.PrivacyParameters = []byte{1, 2, 3, 4, 5, 6, 7, 8}
			request := &SnmpPacket{Version: Version3, MsgFlags: AuthPriv | Reportable,
				SecurityModel: UserSecurityModel, SecurityParameters: sender, PDUType: InformRequest,
				MsgMaxSize: 65507, MsgID: uint32(i + 10), RequestID: uint32(i + 100), ContextEngineID: engine,
				Variables: []SnmpPDU{{Name: trapTestOid, Type: OctetString, Value: trapTestPayload}}, Logger: tl.Params.Logger}
			raw, err := request.MarshalMsg()
			require.NoError(t, err)
			_, err = conn.Write(raw)
			require.NoError(t, err)
			buf := make([]byte, 4096)
			n, err := conn.Read(buf)
			require.NoError(t, err)
			decoder := newTestGoSNMPv3(AuthPriv, credentials())
			response, err := decoder.UnmarshalTrap(buf[:n], true)
			require.NoError(t, err)
			require.Equal(t, GetResponse, response.PDUType)
			require.Equal(t, request.RequestID, response.RequestID)
			require.Equal(t, request.MsgID, response.MsgID)
			require.Equal(t, request.Variables[0].Value, string(response.Variables[0].Value.([]byte)))
			select {
			case id := <-received:
				require.Equal(t, request.RequestID, id)
			case <-time.After(2 * time.Second):
				t.Fatal("missing callback")
			}
			rsp := response.SecurityParameters.(*UsmSecurityParameters)
			require.Equal(t, boots, rsp.AuthoritativeEngineBoots)
			require.Equal(t, sender.AuthoritativeEngineTime, rsp.AuthoritativeEngineTime)
			require.True(t, bytes.Equal(sender.PrivacyParameters, rsp.PrivacyParameters))
			if i == 0 {
				fmt.Println("BASELINE_NORMAL_AUTHPRIV_INFORM response=GetResponse callback=1")
			} else {
				fmt.Println("BASELINE_STALE_INFORM delivered=1 boots=1 time=1 local_boots=7 local_time=900")
			}
			fmt.Println("BASELINE_RESPONSE_SALT copied_from_request=true")
		}
	})
	t.Run("table_discovery", func(t *testing.T) {
		tl := NewTrapListener()
		tl.Params = newTestGoSNMPv3(AuthPriv, credentials())
		table := NewSnmpV3SecurityParametersTable(tl.Params.Logger)
		sp := credentials()
		require.NoError(t, table.Add(sp.UserName, sp))
		tl.Params.SetTrapSecurityParametersTable(table)
		received := make(chan struct{}, 1)
		tl.OnNewTrap = func(*SnmpPacket, *net.UDPAddr) { received <- struct{}{} }
		conn := connect(tl)
		probe := (&UsmSecurityParameters{Logger: tl.Params.Logger}).discoveryRequired()
		probe.MsgID, probe.RequestID = 55, 66
		raw, err := probe.MarshalMsg()
		require.NoError(t, err)
		_, err = conn.Write(raw)
		require.NoError(t, err)
		require.NoError(t, conn.SetReadDeadline(time.Now().Add(250*time.Millisecond)))
		_, err = conn.Read(make([]byte, 4096))
		require.Error(t, err)
		ne, ok := err.(net.Error)
		require.True(t, ok && ne.Timeout())
		select {
		case <-received:
			t.Fatal("discovery was delivered as notification")
		default:
		}
		fmt.Println("BASELINE_TABLE_DISCOVERY response=none callback=0")
	})
}
