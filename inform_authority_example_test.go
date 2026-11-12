// Copyright 2026 The GoSNMP Authors. All rights reserved.  Use of this
// source code is governed by a BSD-style license that can be found in the
// LICENSE file.

//go:build all || trap

package gosnmp

import (
	"log"
	"net"
)

// ExampleTrapListener_SetAuthoritativeEngine shows how to run a
// TrapListener as the authoritative SNMP engine for incoming SNMPv3
// Informs: engine discovery and time synchronization are answered
// automatically, and verified Informs are confirmed with a GetResponse
// carrying the local engine clock.
func ExampleTrapListener_SetAuthoritativeEngine() {
	// The caller owns engine-boots persistence: keep the last value the
	// listener used (in a file, a database, ...) and pass an increased
	// value after every restart, so the authoritative clock never moves
	// backwards. gosnmp deliberately does not write it to disk.
	engineBoots := uint32(1) // load from your persistent store

	tl := NewTrapListener()
	if err := tl.SetAuthoritativeEngine("\x80\x00\x1f\x88\x04collector", engineBoots); err != nil {
		log.Fatal(err)
	}
	tl.OnNewTrap = func(packet *SnmpPacket, addr *net.UDPAddr) {
		log.Printf("inform from %s with %d varbinds", addr, len(packet.Variables))
	}

	// Authoritative Inform reception is UDP-only; the engine clock starts
	// at zero seconds when Listen begins.
	log.Fatal(tl.Listen("udp://0.0.0.0:9162"))
}
