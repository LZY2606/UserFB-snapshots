// SPDX-FileCopyrightText: 2026 The Pion community <https://pion.ly>
// SPDX-License-Identifier: MIT

package codecs

import "errors"

var (
	errShortPacket          = errors.New("packet is not large enough")
	errNilPacket            = errors.New("invalid nil packet")
	errTooManyPDiff         = errors.New("too many PDiff")
	errTooManySpatialLayers = errors.New("too many spatial layers")
	errUnhandledNALUType    = errors.New("NALU Type is unhandled")
	errInvalidAccessUnit    = errors.New("invalid H264 access unit")

	errH264PayloadTypeOutOfRange = errors.New("H264 playout payload type out of range")
	errH264NegativeTime          = errors.New("H264 playout time must not be negative")
	errH264InvalidWindow         = errors.New("H264 playout window out of range")
	errH264WrongSSRC             = errors.New("H264 playout SSRC mismatch")
	errH264WrongPayloadType      = errors.New("H264 playout payload type mismatch")
	errH264WrongRTPVersion       = errors.New("H264 playout RTP version mismatch")
	errH264NegativeMediaOffset   = errors.New("H264 playout media offset is negative")
	errH264LatePacket            = errors.New("H264 playout packet arrived after its deadline")
	errH264StalePacket           = errors.New("H264 playout packet belongs to an already consumed access unit")
	errH264WindowFull            = errors.New("H264 playout packet window is full")
	errH264AccessUnitTooLarge    = errors.New("H264 playout access unit exceeds size limit")
	errH264ConflictingDuplicate  = errors.New("H264 playout duplicate packet conflicts with buffered packet")

	// AV1 Errors.
	errIsKeyframeAndFragment = errors.New(
		"bits Z and N are set. Not possible to have OBU be tail fragment and be keyframe",
	)
)
