# Receiver timing extension

Source: [RFC 3611 sections 2–4.5](https://www.rfc-editor.org/rfc/rfc3611.html#section-4.4).

Use normal compound RTCP, with SR/RR and the existing CNAME SDES retained. XR has PT 207, V=2, a zero reserved count on output, a normal RTCP length, and a sender SSRC after its four-byte header. Each XR block has BT (one byte), reserved (one byte), and a big-endian 16-bit length measured in words excluding that block header.

RRTR uses BT=4, length=2, and two 32-bit words of NTP time. DLRR uses BT=5 and length=3*n, followed by n subblocks: receiver SSRC, LRR, DLRR, each 32 bits. LRR is the middle 32 bits of the original 64-bit reference timestamp. DLRR is the elapsed time since reference arrival in units of 1/65536 second, rounded down. Empty DLRR is permitted on input. Output reserved fields are zero; input reserved fields are ignored. Unknown block types are skipped using their declared lengths.

Scope: no XR-specific padding is generated. Accept normal final RTCP padding only when its count fits that packet, and exclude it when walking blocks. Check compound bounds against InUse(), not unused packet capacity; reject truncated headers, RRTR with length other than 2, and DLRR lengths not divisible by 3. Validate every XR block before committing XR state from the datagram. Normal SR/RR semantics remain unchanged.

The injected clock supplies both the NTP sampling instant and elapsed duration. Keep the outgoing sample identity separate from the duration used for RTT: valid responses take local elapsed duration minus the decoded DLRR wait, at the wire resolution. Only references sent within the last minute are eligible; local and remote stream identities are part of the association. No RTP sending is needed for this exchange.
