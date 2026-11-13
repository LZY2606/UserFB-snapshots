package gohlslib

import (
	"context"
	"crypto/aes"
	"crypto/cipher"
	"encoding/hex"
	"fmt"
	"io"
	"net/http"
	"strings"

	"github.com/bluenviron/gohlslib/v2/pkg/playlist"
)

// checkMediaKeySupport checks whether an EXT-X-KEY can be handled by the client.
// A nil key means that the object is not encrypted.
func checkMediaKeySupport(key *playlist.MediaKey) error {
	if key == nil {
		return nil
	}

	switch key.Method {
	case playlist.MediaKeyMethodNone:
		return nil

	case playlist.MediaKeyMethodAES128:
		if key.KeyFormat != "" && key.KeyFormat != "identity" {
			return fmt.Errorf("unsupported KEYFORMAT: %s", key.KeyFormat)
		}

		if key.KeyFormatVersions != "" && key.KeyFormatVersions != "1" {
			return fmt.Errorf("unsupported KEYFORMATVERSIONS: %s", key.KeyFormatVersions)
		}

		return nil

	case playlist.MediaKeyMethodSampleAES:
		return fmt.Errorf("unsupported encryption method: %s", key.Method)

	default:
		return fmt.Errorf("unsupported encryption method: %s", key.Method)
	}
}

// mediaKeyIsEncrypted reports whether a key actually encrypts its object.
func mediaKeyIsEncrypted(key *playlist.MediaKey) bool {
	return key != nil && key.Method == playlist.MediaKeyMethodAES128
}

// parseMediaKeyIV parses an explicit IV (0x followed by 1 to 32 hex digits)
// and returns it left-padded with zeros to 16 bytes.
func parseMediaKeyIV(v string) ([]byte, error) {
	if len(v) < 3 || v[0] != '0' || v[1] != 'x' {
		return nil, fmt.Errorf("invalid IV: %q", v)
	}

	digits := v[2:]
	if len(digits) > 32 {
		return nil, fmt.Errorf("invalid IV: %q", v)
	}

	if len(digits)%2 != 0 {
		digits = "0" + digits
	}

	digits = strings.Repeat("0", 32-len(digits)) + digits

	ret, err := hex.DecodeString(digits)
	if err != nil {
		return nil, fmt.Errorf("invalid IV: %q", v)
	}

	return ret, nil
}

// segmentIV computes the implicit IV of a segment:
// the 128-bit big-endian representation of (MEDIA-SEQUENCE + segment index).
func segmentIV(mediaSequence int, segIndex int) []byte {
	seq := uint64(mediaSequence) + uint64(segIndex)

	ret := make([]byte, aes.BlockSize)
	for i := 0; i < 8; i++ {
		ret[aes.BlockSize-1-i] = byte(seq >> (8 * uint(i)))
	}
	return ret
}

// decryptAES128 decrypts a single object with AES-128-CBC and validates PKCS#7 padding.
// It never returns partial plaintext.
func decryptAES128(key []byte, iv []byte, ciphertext []byte) ([]byte, error) {
	if len(key) != aes.BlockSize {
		return nil, fmt.Errorf("invalid AES-128 key length: %d", len(key))
	}

	if len(iv) != aes.BlockSize {
		return nil, fmt.Errorf("invalid AES-128 IV length: %d", len(iv))
	}

	if len(ciphertext) == 0 {
		return nil, fmt.Errorf("encrypted object is empty")
	}

	if len(ciphertext)%aes.BlockSize != 0 {
		return nil, fmt.Errorf("encrypted object length %d is not a multiple of %d",
			len(ciphertext), aes.BlockSize)
	}

	block, err := aes.NewCipher(key)
	if err != nil {
		return nil, err
	}

	plaintext := make([]byte, len(ciphertext))
	cipher.NewCBCDecrypter(block, iv).CryptBlocks(plaintext, ciphertext)

	// validate PKCS#7 padding; indices are always within bounds thanks to
	// the length checks above and the guard on pad.
	pad := int(plaintext[len(plaintext)-1])
	if pad == 0 || pad > aes.BlockSize {
		return nil, fmt.Errorf("invalid PKCS#7 padding")
	}
	for _, b := range plaintext[len(plaintext)-pad:] {
		if int(b) != pad {
			return nil, fmt.Errorf("invalid PKCS#7 padding")
		}
	}

	return plaintext[:len(plaintext)-pad], nil
}

// objectIV returns the IV to use for an object: the explicit one when set,
// the implicit one otherwise. Encrypted maps must always carry an explicit IV.
func objectIV(key *playlist.MediaKey, mediaSequence int, segIndex int, isMap bool) ([]byte, error) {
	if key.IV != "" {
		return parseMediaKeyIV(key.IV)
	}

	if isMap {
		return nil, fmt.Errorf("encrypted EXT-X-MAP requires an explicit IV")
	}

	return segmentIV(mediaSequence, segIndex), nil
}

// downloadAndDecryptObject downloads a complete object and, when it is encrypted,
// decrypts it. In case of error, no data is returned.
func (d *clientStreamDownloader) downloadAndDecryptObject(
	ctx context.Context,
	uri string,
	start *uint64,
	length *uint64,
	key *playlist.MediaKey,
	iv []byte,
) ([]byte, error) {
	if err := checkMediaKeySupport(key); err != nil {
		return nil, err
	}

	if !mediaKeyIsEncrypted(key) {
		return d.downloadSegment(ctx, uri, start, length)
	}

	keyBody, err := d.downloadMediaKey(ctx, key)
	if err != nil {
		return nil, err
	}

	byts, err := d.downloadSegment(ctx, uri, start, length)
	if err != nil {
		return nil, err
	}

	return decryptAES128(keyBody, iv, byts)
}

// downloadMediaKey downloads the key referenced by an EXT-X-KEY.
func (d *clientStreamDownloader) downloadMediaKey(
	ctx context.Context,
	key *playlist.MediaKey,
) ([]byte, error) {
	u, err := clientAbsoluteURL(d.playlistURL, key.URI)
	if err != nil {
		return nil, err
	}

	req, err := http.NewRequestWithContext(ctx, http.MethodGet, u.String(), nil)
	if err != nil {
		return nil, err
	}

	d.onRequest(req)

	res, err := d.httpClient.Do(req)
	if err != nil {
		return nil, err
	}
	defer res.Body.Close()

	if res.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("bad status code: %d", res.StatusCode)
	}

	byts, err := io.ReadAll(res.Body)
	if err != nil {
		return nil, err
	}

	if len(byts) != aes.BlockSize {
		return nil, fmt.Errorf("invalid key length: %d", len(byts))
	}

	return byts, nil
}
