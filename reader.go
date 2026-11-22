package m3u8

/*
 Part of M3U8 parser & generator library.
 This file defines functions related to playlist parsing.

 Copyright 2013-2019 The Project Developers.
 See the AUTHORS and LICENSE files at the top-level directory of this distribution
 and at https://github.com/grafov/m3u8/

 ॐ तारे तुत्तारे तुरे स्व
*/

import (
	"bytes"
	"errors"
	"fmt"
	"io"
	"math"
	"regexp"
	"strconv"
	"strings"
	"time"
)

// implicitRangeOffset resolves an omitted BYTERANGE offset of the media
// segment that was just appended. Per HLS the range starts immediately
// after the preceding media segment's range, which must reference the same
// URI and already carry a range itself.
func (p *MediaPlaylist) implicitRangeOffset(uri string) (int64, error) {
	if p.count < 2 {
		return 0, errors.New("implicit byterange offset without a preceding media segment")
	}
	cur := p.last()
	prev := (p.capacity + cur - 1) % p.capacity
	prevSeg := p.Segments[prev]
	if prevSeg == nil || prevSeg.Limit <= 0 {
		return 0, errors.New("implicit byterange offset requires a preceding ranged segment")
	}
	if prevSeg.URI != uri {
		return 0, fmt.Errorf("implicit byterange offset follows a different URI: %q != %q", uri, prevSeg.URI)
	}
	if prevSeg.Offset > math.MaxInt64-prevSeg.Limit {
		return 0, errors.New("implicit byterange offset overflows int64")
	}
	return prevSeg.Offset + prevSeg.Limit, nil
}

var reKeyValue = regexp.MustCompile(`([a-zA-Z0-9_-]+)=("[^"]+"|[^",]+)`)

// TimeParse allows globally apply and/or override Time Parser function.
// Available variants:
//   - FullTimeParse - implements full featured ISO/IEC 8601:2004
//   - StrictTimeParse - implements only RFC3339 Nanoseconds format
var TimeParse func(value string) (time.Time, error) = FullTimeParse

// Decode parses a master playlist passed from the buffer. If `strict`
// parameter is true then it returns first syntax error.
func (p *MasterPlaylist) Decode(data bytes.Buffer, strict bool) error {
	return p.decode(&data, strict)
}

// DecodeFrom parses a master playlist passed from the io.Reader
// stream.  If `strict` parameter is true then it returns first syntax
// error.
func (p *MasterPlaylist) DecodeFrom(reader io.Reader, strict bool) error {
	buf := new(bytes.Buffer)
	_, err := buf.ReadFrom(reader)
	if err != nil {
		return err
	}
	return p.decode(buf, strict)
}

// WithCustomDecoders adds custom tag decoders to the master playlist for decoding
func (p *MasterPlaylist) WithCustomDecoders(customDecoders []CustomDecoder) Playlist {
	// Create the map if it doesn't already exist
	if p.Custom == nil {
		p.Custom = make(map[string]CustomTag)
	}

	p.customDecoders = customDecoders

	return p
}

// Parse master playlist. Internal function.
func (p *MasterPlaylist) decode(buf *bytes.Buffer, strict bool) error {
	var eof bool

	state := new(decodingState)

	for !eof {
		line, err := buf.ReadString('\n')
		if err == io.EOF {
			eof = true
		} else if err != nil {
			break
		}
		err = decodeLineOfMasterPlaylist(p, state, line, strict)
		if strict && err != nil {
			return err
		}
	}

	p.attachRenditionsToVariants(state.alternatives)

	if strict && !state.m3u {
		return errors.New("#EXTM3U absent")
	}
	return nil
}

func (p *MasterPlaylist) attachRenditionsToVariants(alternatives []*Alternative) {
	for _, variant := range p.Variants {
		for _, alt := range alternatives {
			if alt == nil {
				continue
			}
			if variant.Video != "" && alt.Type == "VIDEO" && variant.Video == alt.GroupId {
				variant.Alternatives = append(variant.Alternatives, alt)
			}
			if variant.Audio != "" && alt.Type == "AUDIO" && variant.Audio == alt.GroupId {
				variant.Alternatives = append(variant.Alternatives, alt)
			}
			if variant.Captions != "" && alt.Type == "CLOSED-CAPTIONS" && variant.Captions == alt.GroupId {
				variant.Alternatives = append(variant.Alternatives, alt)
			}
			if variant.Subtitles != "" && alt.Type == "SUBTITLES" && variant.Subtitles == alt.GroupId {
				variant.Alternatives = append(variant.Alternatives, alt)
			}
		}
	}
}

// Decode parses a media playlist passed from the buffer. If `strict`
// parameter is true then return first syntax error.
func (p *MediaPlaylist) Decode(data bytes.Buffer, strict bool) error {
	return p.decode(&data, strict)
}

// DecodeFrom parses a media playlist passed from the io.Reader
// stream. If `strict` parameter is true then it returns first syntax
// error.
func (p *MediaPlaylist) DecodeFrom(reader io.Reader, strict bool) error {
	buf := new(bytes.Buffer)
	_, err := buf.ReadFrom(reader)
	if err != nil {
		return err
	}
	return p.decode(buf, strict)
}

// WithCustomDecoders adds custom tag decoders to the media playlist for decoding
func (p *MediaPlaylist) WithCustomDecoders(customDecoders []CustomDecoder) Playlist {
	// Create the map if it doesn't already exist
	if p.Custom == nil {
		p.Custom = make(map[string]CustomTag)
	}

	p.customDecoders = customDecoders

	return p
}

func (p *MediaPlaylist) decode(buf *bytes.Buffer, strict bool) error {
	var eof bool
	var line string
	var err error

	state := new(decodingState)
	wv := new(WV)

	for !eof {
		if line, err = buf.ReadString('\n'); err == io.EOF {
			eof = true
		} else if err != nil {
			break
		}

		err = decodeLineOfMediaPlaylist(p, wv, state, line, strict)
		if strict && err != nil {
			return err
		}

	}
	if state.tagWV {
		p.WV = wv
	}
	if strict && !state.m3u {
		return errors.New("#EXTM3U absent")
	}
	return nil
}

// Decode detects type of playlist and decodes it. It accepts bytes
// buffer as input.
func Decode(data bytes.Buffer, strict bool) (Playlist, ListType, error) {
	return decode(&data, strict, nil)
}

// DecodeFrom detects type of playlist and decodes it. It accepts data
// conformed with io.Reader.
func DecodeFrom(reader io.Reader, strict bool) (Playlist, ListType, error) {
	buf := new(bytes.Buffer)
	_, err := buf.ReadFrom(reader)
	if err != nil {
		return nil, 0, err
	}
	return decode(buf, strict, nil)
}

// DecodeWith detects the type of playlist and decodes it. It accepts either bytes.Buffer
// or io.Reader as input. Any custom decoders provided will be used during decoding.
func DecodeWith(input interface{}, strict bool, customDecoders []CustomDecoder) (Playlist, ListType, error) {
	switch v := input.(type) {
	case bytes.Buffer:
		return decode(&v, strict, customDecoders)
	case io.Reader:
		buf := new(bytes.Buffer)
		_, err := buf.ReadFrom(v)
		if err != nil {
			return nil, 0, err
		}
		return decode(buf, strict, customDecoders)
	default:
		return nil, 0, errors.New("input must be bytes.Buffer or io.Reader type")
	}
}

// Detect playlist type and decode it. May be used as decoder for both
// master and media playlists.
func decode(buf *bytes.Buffer, strict bool, customDecoders []CustomDecoder) (Playlist, ListType, error) {
	var eof bool
	var line string
	var master *MasterPlaylist
	var media *MediaPlaylist
	var listType ListType
	var err error

	state := new(decodingState)
	wv := new(WV)

	master = NewMasterPlaylist()
	media, err = NewMediaPlaylist(8, 1024) // Winsize for VoD will become 0, capacity auto extends
	if err != nil {
		return nil, 0, fmt.Errorf("Create media playlist failed: %s", err)
	}

	// If we have custom tags to parse
	if customDecoders != nil {
		media = media.WithCustomDecoders(customDecoders).(*MediaPlaylist)
		master = master.WithCustomDecoders(customDecoders).(*MasterPlaylist)
		state.custom = make(map[string]CustomTag)
	}

	for !eof {
		if line, err = buf.ReadString('\n'); err == io.EOF {
			eof = true
		} else if err != nil {
			break
		}

		// fixes the issues https://github.com/grafov/m3u8/issues/25
		// TODO: the same should be done in decode functions of both Master- and MediaPlaylists
		// so some DRYing would be needed.
		if len(line) < 1 || line == "\r" {
			continue
		}

		err = decodeLineOfMasterPlaylist(master, state, line, strict)
		master.attachRenditionsToVariants(state.alternatives)
		if strict && err != nil {
			return master, state.listType, err
		}

		err = decodeLineOfMediaPlaylist(media, wv, state, line, strict)
		if strict && err != nil {
			return media, state.listType, err
		}

	}
	if state.listType == MEDIA && state.tagWV {
		media.WV = wv
	}

	if strict && !state.m3u {
		return nil, listType, errors.New("#EXTM3U absent")
	}

	switch state.listType {
	case MASTER:
		return master, MASTER, nil
	case MEDIA:
		if media.Closed || media.MediaType == EVENT {
			// VoD and Event's should show the entire playlist
			media.SetWinSize(0)
		}
		return media, MEDIA, nil
	}
	return nil, state.listType, errors.New("Can't detect playlist type")
}

// DecodeAttributeList turns an attribute list into a key, value map. You should trim
// any characters not part of the attribute list, such as the tag and ':'.
func DecodeAttributeList(line string) map[string]string {
	return decodeParamsLine(line)
}

func decodeParamsLine(line string) map[string]string {
	out := make(map[string]string)
	for _, kv := range reKeyValue.FindAllStringSubmatch(line, -1) {
		k, v := kv[1], kv[2]
		out[k] = strings.Trim(v, ` "`)
	}
	return out
}

// Parse one line of master playlist.
func decodeLineOfMasterPlaylist(p *MasterPlaylist, state *decodingState, line string, strict bool) error {
	var err error

	line = strings.TrimSpace(line)

	// check for custom tags first to allow custom parsing of existing tags
	if p.Custom != nil {
		for _, v := range p.customDecoders {
			if strings.HasPrefix(line, v.TagName()) {
				t, err := v.Decode(line)

				if strict && err != nil {
					return err
				}

				p.Custom[t.TagName()] = t
			}
		}
	}

	switch {
	case line == "#EXTM3U": // start tag first
		state.m3u = true
	case strings.HasPrefix(line, "#EXT-X-VERSION:"): // version tag
		state.listType = MASTER
		_, err = fmt.Sscanf(line, "#EXT-X-VERSION:%d", &p.ver)
		if strict && err != nil {
			return err
		}
	case line == "#EXT-X-INDEPENDENT-SEGMENTS":
		p.SetIndependentSegments(true)
	case strings.HasPrefix(line, "#EXT-X-MEDIA:"):
		var alt Alternative
		state.listType = MASTER
		for k, v := range decodeParamsLine(line[13:]) {
			switch k {
			case "TYPE":
				alt.Type = v
			case "GROUP-ID":
				alt.GroupId = v
			case "LANGUAGE":
				alt.Language = v
			case "NAME":
				alt.Name = v
			case "DEFAULT":
				if strings.ToUpper(v) == "YES" {
					alt.Default = true
				} else if strings.ToUpper(v) == "NO" {
					alt.Default = false
				} else if strict {
					return errors.New("value must be YES or NO")
				}
			case "AUTOSELECT":
				alt.Autoselect = v
			case "FORCED":
				alt.Forced = v
			case "CHARACTERISTICS":
				alt.Characteristics = v
			case "SUBTITLES":
				alt.Subtitles = v
			case "URI":
				alt.URI = v
			}
		}
		state.alternatives = append(state.alternatives, &alt)
	case !state.tagStreamInf && strings.HasPrefix(line, "#EXT-X-STREAM-INF:"):
		state.tagStreamInf = true
		state.listType = MASTER
		state.variant = new(Variant)
		p.Variants = append(p.Variants, state.variant)
		for k, v := range decodeParamsLine(line[18:]) {
			switch k {
			case "PROGRAM-ID":
				var val int
				val, err = strconv.Atoi(v)
				if strict && err != nil {
					return err
				}
				state.variant.ProgramId = uint32(val)
			case "BANDWIDTH":
				var val int
				val, err = strconv.Atoi(v)
				if strict && err != nil {
					return err
				}
				state.variant.Bandwidth = uint32(val)
			case "CODECS":
				state.variant.Codecs = v
			case "RESOLUTION":
				state.variant.Resolution = v
			case "AUDIO":
				state.variant.Audio = v
			case "VIDEO":
				state.variant.Video = v
			case "SUBTITLES":
				state.variant.Subtitles = v
			case "CLOSED-CAPTIONS":
				state.variant.Captions = v
			case "NAME":
				state.variant.Name = v
			case "AVERAGE-BANDWIDTH":
				var val int
				val, err = strconv.Atoi(v)
				if strict && err != nil {
					return err
				}
				state.variant.AverageBandwidth = uint32(val)
			case "FRAME-RATE":
				if state.variant.FrameRate, err = strconv.ParseFloat(v, 64); strict && err != nil {
					return err
				}
			case "VIDEO-RANGE":
				state.variant.VideoRange = v
			case "HDCP-LEVEL":
				state.variant.HDCPLevel = v
			}
		}
	case state.tagStreamInf && !strings.HasPrefix(line, "#"):
		state.tagStreamInf = false
		state.variant.URI = line
	case strings.HasPrefix(line, "#EXT-X-I-FRAME-STREAM-INF:"):
		state.listType = MASTER
		state.variant = new(Variant)
		state.variant.Iframe = true
		if len(state.alternatives) > 0 {
			state.variant.Alternatives = state.alternatives
			state.alternatives = nil
		}
		p.Variants = append(p.Variants, state.variant)
		for k, v := range decodeParamsLine(line[26:]) {
			switch k {
			case "URI":
				state.variant.URI = v
			case "PROGRAM-ID":
				var val int
				val, err = strconv.Atoi(v)
				if strict && err != nil {
					return err
				}
				state.variant.ProgramId = uint32(val)
			case "BANDWIDTH":
				var val int
				val, err = strconv.Atoi(v)
				if strict && err != nil {
					return err
				}
				state.variant.Bandwidth = uint32(val)
			case "CODECS":
				state.variant.Codecs = v
			case "RESOLUTION":
				state.variant.Resolution = v
			case "AUDIO":
				state.variant.Audio = v
			case "VIDEO":
				state.variant.Video = v
			case "AVERAGE-BANDWIDTH":
				var val int
				val, err = strconv.Atoi(v)
				if strict && err != nil {
					return err
				}
				state.variant.AverageBandwidth = uint32(val)
			case "VIDEO-RANGE":
				state.variant.VideoRange = v
			case "HDCP-LEVEL":
				state.variant.HDCPLevel = v
			}
		}
	case strings.HasPrefix(line, "#"):
		// comments are ignored
	}
	return err
}

// Parse one line of media playlist.
func decodeLineOfMediaPlaylist(p *MediaPlaylist, wv *WV, state *decodingState, line string, strict bool) error {
	var err error

	line = strings.TrimSpace(line)

	// check for custom tags first to allow custom parsing of existing tags
	if p.Custom != nil {
		for _, v := range p.customDecoders {
			if strings.HasPrefix(line, v.TagName()) {
				t, err := v.Decode(line)

				if strict && err != nil {
					return err
				}

				if v.SegmentTag() {
					state.tagCustom = true
					state.custom[v.TagName()] = t
				} else {
					p.Custom[v.TagName()] = t
				}
			}
		}
	}

	switch {
	case !state.tagInf && strings.HasPrefix(line, "#EXTINF:"):
		state.tagInf = true
		state.listType = MEDIA
		sepIndex := strings.Index(line, ",")
		if sepIndex == -1 {
			if strict {
				return fmt.Errorf("could not parse: %q", line)
			}
			sepIndex = len(line)
		}
		duration := line[8:sepIndex]
		if len(duration) > 0 {
			if state.duration, err = strconv.ParseFloat(duration, 64); strict && err != nil {
				return fmt.Errorf("Duration parsing error: %s", err)
			}
		}
		if len(line) > sepIndex {
			state.title = line[sepIndex+1:]
		}
	case !strings.HasPrefix(line, "#"):
		appended := false
		if state.tagInf {
			err := p.Append(line, state.duration, state.title)
			if err == ErrPlaylistFull {
				// Extend playlist by doubling size, reset internal state, try again.
				// If the second Append fails, the if err block will handle it.
				// Retrying instead of being recursive was chosen as the state maybe
				// modified non-idempotently.
				p.Segments = append(p.Segments, make([]*MediaSegment, p.Count())...)
				p.capacity = uint(len(p.Segments))
				p.tail = p.count
				err = p.Append(line, state.duration, state.title)
			}
			// Check err for first or subsequent Append()
			if err != nil {
				return err
			}
			state.tagInf = false
			appended = true
		}
		if state.tagRange {
			offset := state.offset
			if state.rangeImplicit {
				offset, err = p.implicitRangeOffset(line)
				if err != nil {
					return err
				}
			}
			if state.limit <= 0 {
				return fmt.Errorf("byterange length must be positive, got %d", state.limit)
			}
			if offset < 0 {
				return fmt.Errorf("byterange offset must not be negative, got %d", offset)
			}
			if offset > math.MaxInt64-state.limit {
				return errors.New("byterange offset plus length overflows int64")
			}
			if err = p.SetRange(state.limit, offset); strict && err != nil {
				return err
			}
			state.tagRange = false
			state.rangeImplicit = false
		}
		if state.tagSCTE35 {
			state.tagSCTE35 = false
			if err = p.SetSCTE35(state.scte); strict && err != nil {
				return err
			}
		}
		if state.tagDiscontinuity {
			state.tagDiscontinuity = false
			if err = p.SetDiscontinuity(); strict && err != nil {
				return err
			}
		}
		if state.tagProgramDateTime && p.Count() > 0 {
			state.tagProgramDateTime = false
			if err = p.SetProgramDateTime(state.programDateTime); strict && err != nil {
				return err
			}
		}
		// If EXT-X-KEY appeared before reference to segment (EXTINF) then it linked to this segment
		if state.tagKey {
			p.Segments[p.last()].Key = &Key{state.xkey.Method, state.xkey.URI, state.xkey.IV, state.xkey.Keyformat, state.xkey.Keyformatversions}
			// First EXT-X-KEY may appeared in the header of the playlist and linked to first segment
			// but for convenient playlist generation it also linked as default playlist key
			if p.Key == nil {
				p.Key = state.xkey
			}
			state.tagKey = false
		}
		// If EXT-X-MAP appeared before reference to segment (EXTINF) then it linked to this segment
		if state.tagMap {
			// A playlist that once carried an EXT-X-MAP may not switch back to
			// segments without an initialization section.
			if p.Map != nil && state.xmap == nil {
				return errors.New("media segment without EXT-X-MAP after an initialization section was already in effect")
			}
			p.Segments[p.last()].Map = state.xmap
			// First EXT-X-MAP may appeared in the header of the playlist and linked to first segment
			// but for convenient playlist generation it also linked as default playlist map
			if p.Map == nil {
				p.Map = state.xmap
			}
			state.tagMap = false
			state.mapImplicit = false
		} else if appended && p.Map != nil {
			// The tag stays in effect until the next EXT-X-MAP, so a segment
			// without its own MAP inherits the initialization section in
			// effect for the immediately preceding segment (which may be a
			// later MAP switch, not the playlist default). A trailing blank
			// line is not a segment and must not rewrite the last segment.
			cur := p.last()
			prev := (p.capacity + cur - 1) % p.capacity
			if p.count >= 2 && p.Segments[prev] != nil && p.Segments[prev].Map != nil {
				p.Segments[cur].Map = p.Segments[prev].Map
			} else {
				p.Segments[cur].Map = p.Map
			}
		}

		// if segment custom tag appeared before EXTINF then it links to this segment
		if state.tagCustom {
			p.Segments[p.last()].Custom = state.custom
			state.custom = make(map[string]CustomTag)
			state.tagCustom = false
		}
	// start tag first
	case line == "#EXTM3U":
		state.m3u = true
	case line == "#EXT-X-ENDLIST":
		state.listType = MEDIA
		p.Closed = true
	case strings.HasPrefix(line, "#EXT-X-VERSION:"):
		state.listType = MEDIA
		if _, err = fmt.Sscanf(line, "#EXT-X-VERSION:%d", &p.ver); strict && err != nil {
			return err
		}
	case strings.HasPrefix(line, "#EXT-X-TARGETDURATION:"):
		state.listType = MEDIA
		if _, err = fmt.Sscanf(line, "#EXT-X-TARGETDURATION:%f", &p.TargetDuration); strict && err != nil {
			return err
		}
	case strings.HasPrefix(line, "#EXT-X-MEDIA-SEQUENCE:"):
		state.listType = MEDIA
		if _, err = fmt.Sscanf(line, "#EXT-X-MEDIA-SEQUENCE:%d", &p.SeqNo); strict && err != nil {
			return err
		}
	case strings.HasPrefix(line, "#EXT-X-PLAYLIST-TYPE:"):
		state.listType = MEDIA
		var playlistType string
		_, err = fmt.Sscanf(line, "#EXT-X-PLAYLIST-TYPE:%s", &playlistType)
		if err != nil {
			if strict {
				return err
			}
		} else {
			switch playlistType {
			case "EVENT":
				p.MediaType = EVENT
			case "VOD":
				p.MediaType = VOD
			}
		}
	case strings.HasPrefix(line, "#EXT-X-DISCONTINUITY-SEQUENCE:"):
		state.listType = MEDIA
		if _, err = fmt.Sscanf(line, "#EXT-X-DISCONTINUITY-SEQUENCE:%d", &p.DiscontinuitySeq); strict && err != nil {
			return err
		}
	case strings.HasPrefix(line, "#EXT-X-START:"):
		state.listType = MEDIA
		for k, v := range decodeParamsLine(line[13:]) {
			switch k {
			case "TIME-OFFSET":
				st, err := strconv.ParseFloat(v, 64)
				if err != nil {
					return fmt.Errorf("Invalid TIME-OFFSET: %s: %v", v, err)
				}
				p.StartTime = st
			case "PRECISE":
				p.StartTimePrecise = v == "YES"
			}
		}
	case strings.HasPrefix(line, "#EXT-X-KEY:"):
		state.listType = MEDIA
		state.xkey = new(Key)
		for k, v := range decodeParamsLine(line[11:]) {
			switch k {
			case "METHOD":
				state.xkey.Method = v
			case "URI":
				state.xkey.URI = v
			case "IV":
				state.xkey.IV = v
			case "KEYFORMAT":
				state.xkey.Keyformat = v
			case "KEYFORMATVERSIONS":
				state.xkey.Keyformatversions = v
			}
		}
		state.tagKey = true
	case strings.HasPrefix(line, "#EXT-X-MAP:"):
		state.listType = MEDIA
		state.xmap = new(Map)
		state.mapImplicit = false
		for k, v := range decodeParamsLine(line[11:]) {
			switch k {
			case "URI":
				state.xmap.URI = v
			case "BYTERANGE":
				params := strings.SplitN(v, "@", 2)
				state.xmap.Limit, err = strconv.ParseInt(params[0], 10, 64)
				if strict && err != nil {
					return fmt.Errorf("Byterange sub-range length value parsing error: %s", err)
				}
				if state.xmap.Limit <= 0 {
					return fmt.Errorf("map byterange length must be positive, got %d", state.xmap.Limit)
				}
				if len(params) > 1 {
					state.xmap.Offset, err = strconv.ParseInt(params[1], 10, 64)
					if strict && err != nil {
						return fmt.Errorf("Byterange sub-range offset value parsing error: %s", err)
					}
					if state.xmap.Offset < 0 {
						return fmt.Errorf("map byterange offset must not be negative, got %d", state.xmap.Offset)
					}
					if state.xmap.Offset > math.MaxInt64-state.xmap.Limit {
						return errors.New("map byterange offset plus length overflows int64")
					}
				} else {
					state.mapImplicit = true
				}
			}
		}
		if state.mapImplicit {
			if p.Map == nil || p.Map.Limit <= 0 {
				return errors.New("implicit map byterange offset without a preceding ranged initialization section")
			}
			if state.xmap.URI != p.Map.URI {
				return fmt.Errorf("implicit map byterange offset follows a different URI: %q != %q", state.xmap.URI, p.Map.URI)
			}
			if p.Map.Offset > math.MaxInt64-p.Map.Limit {
				return errors.New("implicit map byterange offset overflows int64")
			}
			state.xmap.Offset = p.Map.Offset + p.Map.Limit
		}
		state.tagMap = true
	case !state.tagProgramDateTime && strings.HasPrefix(line, "#EXT-X-PROGRAM-DATE-TIME:"):
		state.tagProgramDateTime = true
		state.listType = MEDIA
		if state.programDateTime, err = TimeParse(line[25:]); strict && err != nil {
			return err
		}
	case !state.tagRange && strings.HasPrefix(line, "#EXT-X-BYTERANGE:"):
		state.tagRange = true
		state.rangeImplicit = false
		state.listType = MEDIA
		params := strings.SplitN(line[17:], "@", 2)
		if state.limit, err = strconv.ParseInt(params[0], 10, 64); strict && err != nil {
			return fmt.Errorf("Byterange sub-range length value parsing error: %s", err)
		}
		if state.limit <= 0 {
			return fmt.Errorf("byterange length must be positive, got %d", state.limit)
		}
		state.offset = 0
		if len(params) > 1 {
			if state.offset, err = strconv.ParseInt(params[1], 10, 64); strict && err != nil {
				return fmt.Errorf("Byterange sub-range offset value parsing error: %s", err)
			}
			if state.offset < 0 {
				return fmt.Errorf("byterange offset must not be negative, got %d", state.offset)
			}
		} else {
			// An omitted offset is not the same as an explicit zero: it must be
			// resolved against the preceding segment once the URI is known.
			state.rangeImplicit = true
		}
	case !state.tagSCTE35 && strings.HasPrefix(line, "#EXT-SCTE35:"):
		state.tagSCTE35 = true
		state.listType = MEDIA
		state.scte = new(SCTE)
		state.scte.Syntax = SCTE35_67_2014
		for attribute, value := range decodeParamsLine(line[12:]) {
			switch attribute {
			case "CUE":
				state.scte.Cue = value
			case "ID":
				state.scte.ID = value
			case "TIME":
				state.scte.Time, _ = strconv.ParseFloat(value, 64)
			}
		}
	case !state.tagSCTE35 && strings.HasPrefix(line, "#EXT-OATCLS-SCTE35:"):
		// EXT-OATCLS-SCTE35 contains the SCTE35 tag, EXT-X-CUE-OUT contains duration
		state.tagSCTE35 = true
		state.scte = new(SCTE)
		state.scte.Syntax = SCTE35_OATCLS
		state.scte.Cue = line[19:]
	case state.tagSCTE35 && state.scte.Syntax == SCTE35_OATCLS && strings.HasPrefix(line, "#EXT-X-CUE-OUT:"):
		// EXT-OATCLS-SCTE35 contains the SCTE35 tag, EXT-X-CUE-OUT contains duration
		state.scte.Time, _ = strconv.ParseFloat(line[15:], 64)
		state.scte.CueType = SCTE35Cue_Start
	case !state.tagSCTE35 && strings.HasPrefix(line, "#EXT-X-CUE-OUT-CONT:"):
		state.tagSCTE35 = true
		state.scte = new(SCTE)
		state.scte.Syntax = SCTE35_OATCLS
		state.scte.CueType = SCTE35Cue_Mid
		for attribute, value := range decodeParamsLine(line[20:]) {
			switch attribute {
			case "SCTE35":
				state.scte.Cue = value
			case "Duration":
				state.scte.Time, _ = strconv.ParseFloat(value, 64)
			case "ElapsedTime":
				state.scte.Elapsed, _ = strconv.ParseFloat(value, 64)
			}
		}
	case !state.tagSCTE35 && strings.HasPrefix(line, "#EXT-X-CUE-OUT"):
		state.tagSCTE35 = true
		state.scte = new(SCTE)
		state.scte.Syntax = SCTE35_OATCLS
		state.scte.CueType = SCTE35Cue_Start
		lenLine := len(line)
		if lenLine > 14 {
			state.scte.Time, _ = strconv.ParseFloat(line[15:], 64)
		}
	case !state.tagSCTE35 && line == "#EXT-X-CUE-IN":
		state.tagSCTE35 = true
		state.scte = new(SCTE)
		state.scte.Syntax = SCTE35_OATCLS
		state.scte.CueType = SCTE35Cue_End
	case !state.tagDiscontinuity && strings.HasPrefix(line, "#EXT-X-DISCONTINUITY"):
		state.tagDiscontinuity = true
		state.listType = MEDIA
	case strings.HasPrefix(line, "#EXT-X-I-FRAMES-ONLY"):
		state.listType = MEDIA
		p.Iframe = true
	case strings.HasPrefix(line, "#WV-AUDIO-CHANNELS"):
		state.listType = MEDIA
		if _, err = fmt.Sscanf(line, "#WV-AUDIO-CHANNELS %d", &wv.AudioChannels); strict && err != nil {
			return err
		}
		if err == nil {
			state.tagWV = true
		}
	case strings.HasPrefix(line, "#WV-AUDIO-FORMAT"):
		state.listType = MEDIA
		if _, err = fmt.Sscanf(line, "#WV-AUDIO-FORMAT %d", &wv.AudioFormat); strict && err != nil {
			return err
		}
		if err == nil {
			state.tagWV = true
		}
	case strings.HasPrefix(line, "#WV-AUDIO-PROFILE-IDC"):
		state.listType = MEDIA
		if _, err = fmt.Sscanf(line, "#WV-AUDIO-PROFILE-IDC %d", &wv.AudioProfileIDC); strict && err != nil {
			return err
		}
		if err == nil {
			state.tagWV = true
		}
	case strings.HasPrefix(line, "#WV-AUDIO-SAMPLE-SIZE"):
		state.listType = MEDIA
		if _, err = fmt.Sscanf(line, "#WV-AUDIO-SAMPLE-SIZE %d", &wv.AudioSampleSize); strict && err != nil {
			return err
		}
		if err == nil {
			state.tagWV = true
		}
	case strings.HasPrefix(line, "#WV-AUDIO-SAMPLING-FREQUENCY"):
		state.listType = MEDIA
		if _, err = fmt.Sscanf(line, "#WV-AUDIO-SAMPLING-FREQUENCY %d", &wv.AudioSamplingFrequency); strict && err != nil {
			return err
		}
		if err == nil {
			state.tagWV = true
		}
	case strings.HasPrefix(line, "#WV-CYPHER-VERSION"):
		state.listType = MEDIA
		wv.CypherVersion = line[19:]
		state.tagWV = true
	case strings.HasPrefix(line, "#WV-ECM"):
		state.listType = MEDIA
		if _, err = fmt.Sscanf(line, "#WV-ECM %s", &wv.ECM); strict && err != nil {
			return err
		}
		if err == nil {
			state.tagWV = true
		}
	case strings.HasPrefix(line, "#WV-VIDEO-FORMAT"):
		state.listType = MEDIA
		if _, err = fmt.Sscanf(line, "#WV-VIDEO-FORMAT %d", &wv.VideoFormat); strict && err != nil {
			return err
		}
		if err == nil {
			state.tagWV = true
		}
	case strings.HasPrefix(line, "#WV-VIDEO-FRAME-RATE"):
		state.listType = MEDIA
		if _, err = fmt.Sscanf(line, "#WV-VIDEO-FRAME-RATE %d", &wv.VideoFrameRate); strict && err != nil {
			return err
		}
		if err == nil {
			state.tagWV = true
		}
	case strings.HasPrefix(line, "#WV-VIDEO-LEVEL-IDC"):
		state.listType = MEDIA
		if _, err = fmt.Sscanf(line, "#WV-VIDEO-LEVEL-IDC %d", &wv.VideoLevelIDC); strict && err != nil {
			return err
		}
		if err == nil {
			state.tagWV = true
		}
	case strings.HasPrefix(line, "#WV-VIDEO-PROFILE-IDC"):
		state.listType = MEDIA
		if _, err = fmt.Sscanf(line, "#WV-VIDEO-PROFILE-IDC %d", &wv.VideoProfileIDC); strict && err != nil {
			return err
		}
		if err == nil {
			state.tagWV = true
		}
	case strings.HasPrefix(line, "#WV-VIDEO-RESOLUTION"):
		state.listType = MEDIA
		wv.VideoResolution = line[21:]
		state.tagWV = true
	case strings.HasPrefix(line, "#WV-VIDEO-SAR"):
		state.listType = MEDIA
		if _, err = fmt.Sscanf(line, "#WV-VIDEO-SAR %s", &wv.VideoSAR); strict && err != nil {
			return err
		}
		if err == nil {
			state.tagWV = true
		}
	case strings.HasPrefix(line, "#"):
		// comments are ignored
	}
	return err
}

// StrictTimeParse implements RFC3339 with Nanoseconds accuracy.
func StrictTimeParse(value string) (time.Time, error) {
	return time.Parse(DATETIME, value)
}

// FullTimeParse implements ISO/IEC 8601:2004.
func FullTimeParse(value string) (time.Time, error) {
	layouts := []string{
		"2006-01-02T15:04:05.999999999Z0700",
		"2006-01-02T15:04:05.999999999Z07:00",
		"2006-01-02T15:04:05.999999999Z07",
	}
	var (
		err error
		t   time.Time
	)
	for _, layout := range layouts {
		if t, err = time.Parse(layout, value); err == nil {
			return t, nil
		}
	}
	return t, err
}
