package gos7

// Copyright 2018 Trung Hieu Le. All rights reserved.
// This software may be modified and distributed under the terms
// of the BSD license. See the LICENSE file for details.
import (
	"encoding/binary"
	"fmt"
)

//S7DataItem which expose as S7DataItem to use in Multiple read/write
type S7DataItem struct {
	Area     int
	WordLen  int
	DBNumber int
	Start    int
	Bit      int
	Amount   int
	Data     []byte
	Error    string
}

//implement WriteMulti
func (mb *client) AGWriteMulti(dataItems []S7DataItem, itemsCount int) (err error) {
	// Checks items
	if itemsCount > 20 { //max variable is 20
		err = fmt.Errorf(ErrorText(errCliTooManyItems))
		return
	}
	//fills header
	s7Multi := make([]byte, len(s7MultiWriteHeaderTelegram))
	copy(s7Multi, s7MultiWriteHeaderTelegram)

	parLength := itemsCount*len(s7MultiWriteItemTelegram) + 2
	binary.BigEndian.PutUint16(s7Multi[13:], uint16(parLength))
	s7Multi[18] = byte(itemsCount)
	// Fills Params
	offset := len(s7MultiWriteHeaderTelegram)
	for i := 0; i < itemsCount; i++ {
		s7ParamItem := make([]byte, len(s7MultiWriteItemTelegram))
		copy(s7ParamItem, s7MultiWriteItemTelegram)
		s7ParamItem[3] = byte(dataItems[i].WordLen)                                //word length
		s7ParamItem[8] = byte(dataItems[i].Area)                                   //area
		binary.BigEndian.PutUint16(s7ParamItem[4:], uint16(dataItems[i].Amount))   //amount
		binary.BigEndian.PutUint16(s7ParamItem[6:], uint16(dataItems[i].DBNumber)) //DBNo

		// Adjusts the offset
		var addr int
		if dataItems[i].WordLen == s7wlbit || dataItems[i].WordLen == s7wlcounter || dataItems[i].WordLen == s7wltimer {
			addr = dataItems[i].Start
		} else {
			addr = dataItems[i].Start * 8
		}

		// Build the offset
		s7ParamItem[11] = byte(addr & 0x0FF)
		addr = addr >> 8
		s7ParamItem[10] = byte(addr & 0x0FF)
		addr = addr >> 8
		s7ParamItem[9] = byte(addr & 0x0FF)
		// copy(s7Multi[offset:offset+len(s7ParamItem)], s7ParamItem[0:])
		s7Multi = append(s7Multi[:offset], append(s7ParamItem, s7Multi[offset:]...)...)
		offset += len(s7ParamItem)
	}
	dataLength := 0
	for i := 0; i < itemsCount; i++ {
		s7ItemWrite := make([]byte, 1024)
		s7ItemWrite[0] = 0
		itemDataSize := 0
		switch dataItems[i].WordLen {
		case s7wlbit:
			s7ItemWrite[1] = tsResBit
			itemDataSize = dataItems[i].Amount
			binary.BigEndian.PutUint16(s7ItemWrite[2:], uint16(itemDataSize))
			break
		case s7wlcounter:
		case s7wltimer:
			s7ItemWrite[1] = tsResOctet
			itemDataSize = dataItems[i].Amount * 2
			binary.BigEndian.PutUint16(s7ItemWrite[2:], uint16(itemDataSize))
			break
		case s7wlreal:
			s7ItemWrite[1] = tsResReal // real
			itemDataSize = dataItems[i].Amount * dataSizeByte(dataItems[i].WordLen)
			binary.BigEndian.PutUint16(s7ItemWrite[2:], uint16(itemDataSize))
			break
		default:
			s7ItemWrite[1] = tsResByte // byte/word/dword etc.
			itemDataSize = dataItems[i].Amount * dataSizeByte(dataItems[i].WordLen)
			binary.BigEndian.PutUint16(s7ItemWrite[2:], uint16(itemDataSize*8))
			break

		}
		copy(s7ItemWrite[4:4+itemDataSize], dataItems[i].Data)
		if itemDataSize%2 != 0 {
			s7ItemWrite[itemDataSize+4] = 0
			itemDataSize++
		}
		// copy(s7Multi[offset:offset+itemDataSize+4], s7ItemWrite[0:itemDataSize+4])
		s7Multi = append(s7Multi, s7ItemWrite[0:itemDataSize+4]...)
		offset = offset + itemDataSize + 4
		dataLength = dataLength + itemDataSize + 4
	}
	tt, _ := interface{}(mb.transporter).(*TCPClientHandler)
	//Checks the size
	if offset > tt.PDULength {
		err = fmt.Errorf(ErrorText(errCliSizeOverPDU))
		return
	}
	binary.BigEndian.PutUint16(s7Multi[2:], uint16(offset))      // Whole size
	binary.BigEndian.PutUint16(s7Multi[15:], uint16(dataLength)) // Whole size
	request := NewProtocolDataUnit(s7Multi)
	//send
	response, err := mb.send(&request)
	if err == nil {
		// Check Global Operation Result
		cpuErr := CPUError(uint(binary.BigEndian.Uint16(response.Data[17:])))
		if cpuErr != 0 {
			err = fmt.Errorf(ErrorText(cpuErr))
			return
		}
		if itemsWritten := int(response.Data[20]); itemsWritten != itemsCount || itemsWritten > 20 { //max var = 20
			err = fmt.Errorf(ErrorText(errCliInvalidPlcAnswer))
			return
		}
		for i := 0; i < itemsCount; i++ {
			if response.Data[i+21] == 0xFF {

				dataItems[i].Error = ""
			} else {
				dataItems[i].Error = ErrorText(CPUError(uint(response.Data[i+21])))
			}
		}
	}
	return
}

// validateReadMultiItems checks every caller parameter before any request is
// sent. An invalid call must not put a single byte on the wire.
func validateReadMultiItems(dataItems []S7DataItem, itemsCount int) error {
	if itemsCount < 1 || itemsCount > len(dataItems) {
		return fmt.Errorf(ErrorText(errCliInvalidParams))
	}
	if itemsCount > 20 { //max variable is 20
		return fmt.Errorf(ErrorText(errCliTooManyItems))
	}
	for i := 0; i < itemsCount; i++ {
		item := dataItems[i]
		if item.Amount <= 0 {
			return fmt.Errorf(ErrorText(errCliInvalidParams))
		}
		wordSize := dataSizeByte(item.WordLen)
		if wordSize == 0 {
			return fmt.Errorf(ErrorText(errCliInvalidWordLen))
		}
		if item.WordLen == s7wlbit && item.Amount != 1 {
			// A bit transfer is limited to one bit and is delivered in one byte.
			return fmt.Errorf(ErrorText(errCliInvalidParams))
		}
		if cap(item.Data) < readItemDataBytes(item.WordLen, item.Amount) {
			return fmt.Errorf(ErrorText(errCliBufferTooSmall))
		}
	}
	return nil
}

// readItemDataBytes returns the number of payload bytes a successful read item
// with the given request WordLen/Amount occupies in the response (padding
// excluded). Bit reads always use a single output byte holding 0 or 1.
func readItemDataBytes(wordLen int, amount int) int {
	if wordLen == s7wlbit {
		return 1
	}
	return dataSizeByte(wordLen) * amount
}

// expectedTransportSize maps a request WordLen to the transport size the PLC
// has to use for the matching result item.
func expectedTransportSize(wordLen int) byte {
	switch wordLen {
	case s7wlbit:
		return tsResBit
	case s7wlreal:
		return tsResReal
	case s7wlcounter, s7wltimer:
		return tsResOctet
	default:
		return tsResByte
	}
}

// responseItemBodyBytes converts the declared length of a result item into the
// number of payload bytes. Byte/integer transports declare bits, octet/real
// transports declare bytes and bit transports declare bits that occupy whole
// bytes on the wire.
func responseItemBodyBytes(transportSize byte, declaredLength int) (int, error) {
	switch transportSize {
	case tsResBit:
		if declaredLength <= 0 {
			return 0, fmt.Errorf(ErrorText(errCliInvalidPlcAnswer))
		}
		return (declaredLength + 7) / 8, nil
	case tsResByte, tsResInt:
		if declaredLength <= 0 || declaredLength%8 != 0 {
			return 0, fmt.Errorf(ErrorText(errCliInvalidPlcAnswer))
		}
		return declaredLength / 8, nil
	case tsResOctet, tsResReal:
		if declaredLength <= 0 {
			return 0, fmt.Errorf(ErrorText(errCliInvalidPlcAnswer))
		}
		return declaredLength, nil
	default:
		return 0, fmt.Errorf(ErrorText(errCliInvalidTransportSize))
	}
}

// parsedReadItem is the staged result of one response item. Nothing is copied
// into the caller's S7DataItem values until the whole frame has been validated.
type parsedReadItem struct {
	deviceError bool
	status      byte
	body        []byte
	bodyBytes   int
}

// parseReadMultiResponse validates the complete multi read response first and
// only commits the results to dataItems afterwards, so a structurally invalid
// frame can never leave a partial "successful prefix" behind.
func parseReadMultiResponse(pdu []byte, dataItems []S7DataItem) error {
	if len(pdu) < 22 {
		return fmt.Errorf(ErrorText(errIsoInvalidPDU)) // PDU too Small
	}
	// Check Global Operation Result
	cpuErr := CPUError(uint(binary.BigEndian.Uint16(pdu[17:])))
	if cpuErr != 0 {
		return fmt.Errorf(ErrorText(cpuErr))
	}
	itemsCount := len(dataItems)
	if int(pdu[20]) != itemsCount || itemsCount > 20 { //max var
		return fmt.Errorf(ErrorText(errCliInvalidPlcAnswer))
	}
	declaredDataLen := int(binary.BigEndian.Uint16(pdu[15:]))
	dataEnd := 21 + declaredDataLen // TPKT+COTP (7) + S7 header (14)
	if len(pdu) < dataEnd {
		return fmt.Errorf(ErrorText(errCliInvalidPlcAnswer))
	}

	parsed := make([]parsedReadItem, 0, itemsCount)
	pos := 21
	for i := 0; i < itemsCount; i++ {
		// Every item, successful or not, starts with a 4 byte header.
		if dataEnd-pos < 4 {
			return fmt.Errorf(ErrorText(errCliInvalidPlcAnswer))
		}
		status := pdu[pos]
		transportSize := pdu[pos+1]
		declaredLength := int(binary.BigEndian.Uint16(pdu[pos+2:]))
		pos += 4

		if status != 0xFF {
			// Legal per-item CPU failure: the header carries no body. It is
			// staged separately and never touches the item's Data.
			parsed = append(parsed, parsedReadItem{deviceError: true, status: status})
			continue
		}

		// Successful item: the transport size has to match the request and the
		// declared length has to describe exactly the bytes the caller asked for.
		wantTransport := expectedTransportSize(dataItems[i].WordLen)
		if transportSize != wantTransport && !(wantTransport == tsResByte && transportSize == tsResInt) {
			return fmt.Errorf(ErrorText(errCliInvalidTransportSize))
		}
		bodyBytes, err := responseItemBodyBytes(transportSize, declaredLength)
		if err != nil {
			return err
		}
		wantBytes := readItemDataBytes(dataItems[i].WordLen, dataItems[i].Amount)
		if bodyBytes != wantBytes {
			return fmt.Errorf(ErrorText(errCliInvalidPlcAnswer))
		}
		if dataEnd-pos < bodyBytes {
			return fmt.Errorf(ErrorText(errCliInvalidPlcAnswer))
		}
		body := pdu[pos : pos+bodyBytes]
		pos += bodyBytes

		// Odd sized items are followed by one padding byte. Between items the
		// padding is mandatory; the final item is allowed to omit it.
		if bodyBytes%2 == 1 && i < itemsCount-1 {
			if dataEnd-pos < 1 {
				return fmt.Errorf(ErrorText(errCliInvalidPlcAnswer))
			}
			pos++
		}
		parsed = append(parsed, parsedReadItem{body: body, bodyBytes: bodyBytes})
	}

	// A trailing padding byte after the final odd item is optional: consume it
	// when present so omission and inclusion both work. Anything else left over
	// is an unassignable byte and invalidates the whole frame.
	if dataEnd-pos == 1 {
		last := parsed[itemsCount-1]
		if !last.deviceError && last.bodyBytes%2 == 1 {
			pos++
		}
	}

	// The declared data section must be consumed exactly, and no extra bytes
	// may follow it: otherwise item count or declared length do not match the
	// actual frame.
	if pos != dataEnd || len(pdu) != dataEnd {
		return fmt.Errorf(ErrorText(errCliInvalidPlcAnswer))
	}

	// Frame is complete: commit the staged results. Successful items update the
	// valid Data prefix only and clear stale errors; failed items keep their
	// previous Data and only refresh their mapped CPU error text.
	for i, item := range parsed {
		if item.deviceError {
			dataItems[i].Error = ErrorText(CPUError(uint(item.status)))
			continue
		}
		dst := dataItems[i].Data
		if len(dst) < len(item.body) {
			dst = dst[:cap(dst)]
		}
		copy(dst[:len(item.body)], item.body)
		dataItems[i].Error = ""
	}
	return nil
}

//implement ReadMulti
func (mb *client) AGReadMulti(dataItems []S7DataItem, itemsCount int) (err error) {
	// Validate parameters first: invalid calls must not send any request.
	if err = validateReadMultiItems(dataItems, itemsCount); err != nil {
		return
	}
	s7Item := make([]byte, 12)
	s7Multi := make([]byte, len(s7MultiReadHeaderTelegram))
	copy(s7Multi, s7MultiReadHeaderTelegram)
	// Fills Header
	binary.BigEndian.PutUint16(s7Multi[13:], uint16(itemsCount*len(s7Item)+2))
	s7Multi[18] = byte(itemsCount)
	// Fills the Items
	offset := 19
	for i := 0; i < itemsCount; i++ {
		copy(s7Item, s7MultiReadItemTelegram)
		s7Item[3] = byte(dataItems[i].WordLen)
		binary.BigEndian.PutUint16(s7Item[4:], uint16(dataItems[i].Amount))
		if dataItems[i].Area == s7areadb {
			binary.BigEndian.PutUint16(s7Item[6:], uint16(dataItems[i].DBNumber))
		}
		s7Item[8] = byte(dataItems[i].Area)

		// Adjusts the offset
		var addr int
		if dataItems[i].WordLen == s7wlcounter || dataItems[i].WordLen == s7wltimer {
			addr = dataItems[i].Start
		} else if dataItems[i].WordLen == s7wlbit {
			addr = dataItems[i].Start << 3
			addr += dataItems[i].Bit // Add Bit addr
		} else {
			addr = dataItems[i].Start * 8
		}

		// Build the offset
		s7Item[11] = byte(addr & 0x0FF)
		addr = addr >> 8
		s7Item[10] = byte(addr & 0x0FF)
		addr = addr >> 8
		s7Item[9] = byte(addr & 0x0FF)
		//now expand array then put item into
		s7Multi = append(s7Multi, s7Item...)
		offset += len(s7Item)
	}
	tt, _ := interface{}(mb.transporter).(*TCPClientHandler)
	if offset > tt.PDULength {
		err = fmt.Errorf(ErrorText(errCliSizeOverPDU))
		return
	}
	binary.BigEndian.PutUint16(s7Multi[2:], uint16(offset)) // Whole size
	request := NewProtocolDataUnit(s7Multi)
	//send
	response, err := mb.send(&request)
	if err != nil {
		return
	}
	return parseReadMultiResponse(response.Data, dataItems[:itemsCount])
}
