package gohlslib

import (
	"context"
	"crypto/aes"
	"crypto/cipher"
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"

	"github.com/bluenviron/gohlslib/v2/pkg/playlist"
)

func validateMediaKeySupport(key *playlist.MediaKey) error {
	if key == nil {
		return nil
	}

	switch key.Method {
	case playlist.MediaKeyMethodNone:
		return nil

	case playlist.MediaKeyMethodAES128:
		if key.KeyFormat != "" && key.KeyFormat != "identity" {
			return fmt.Errorf("unsupported AES-128 KEYFORMAT: %q", key.KeyFormat)
		}
		if key.KeyFormatVersions != "" && key.KeyFormatVersions != "1" {
			return fmt.Errorf("unsupported AES-128 KEYFORMATVERSIONS: %q", key.KeyFormatVersions)
		}
		return nil

	default:
		return fmt.Errorf("unsupported HLS encryption method: %q", key.Method)
	}
}

func parseAES128IV(v string) ([]byte, error) {
	if !strings.HasPrefix(v, "0x") {
		return nil, fmt.Errorf("invalid IV %q: missing 0x prefix", v)
	}

	digits := v[2:]
	if len(digits) == 0 {
		return nil, fmt.Errorf("invalid IV %q: empty value", v)
	}
	if len(digits) > 32 {
		return nil, fmt.Errorf("invalid IV %q: too long", v)
	}

	padded := strings.Repeat("0", 32-len(digits)) + digits
	iv, err := hex.DecodeString(padded)
	if err != nil {
		return nil, fmt.Errorf("invalid IV %q: %w", v, err)
	}

	return iv, nil
}

func mediaSequenceIV(sequence int) []byte {
	iv := make([]byte, aes.BlockSize)
	binary.BigEndian.PutUint64(iv[aes.BlockSize-8:], uint64(sequence))
	return iv
}

func fetchAES128Key(
	ctx context.Context,
	httpClient *http.Client,
	onRequest ClientOnRequestFunc,
	playlistURL *url.URL,
	key *playlist.MediaKey,
) ([]byte, error) {
	u, err := clientAbsoluteURL(playlistURL, key.URI)
	if err != nil {
		return nil, err
	}

	req, err := http.NewRequestWithContext(ctx, http.MethodGet, u.String(), nil)
	if err != nil {
		return nil, err
	}

	if onRequest != nil {
		onRequest(req)
	}

	res, err := httpClient.Do(req)
	if err != nil {
		return nil, err
	}
	defer func() {
		if res.Body != nil {
			_ = res.Body.Close()
		}
	}()

	if res.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("bad key response status code: %d", res.StatusCode)
	}
	if res.Body == nil {
		return nil, fmt.Errorf("key response body is empty")
	}

	type readResult struct {
		body []byte
		err  error
	}
	readDone := make(chan readResult, 1)
	go func() {
		body, err := io.ReadAll(res.Body)
		readDone <- readResult{body: body, err: err}
	}()

	var body []byte
	select {
	case result := <-readDone:
		if result.err != nil {
			return nil, result.err
		}
		body = result.body
	case <-ctx.Done():
		return nil, ctx.Err()
	}

	if len(body) != aes.BlockSize {
		return nil, fmt.Errorf("AES-128 key must be exactly %d bytes, got %d", aes.BlockSize, len(body))
	}

	return body, nil
}

func decryptAES128(data []byte, key []byte, iv []byte) ([]byte, error) {
	if len(data) == 0 {
		return nil, fmt.Errorf("encrypted object is empty")
	}
	if len(data)%aes.BlockSize != 0 {
		return nil, fmt.Errorf("ciphertext length %d is not a multiple of %d", len(data), aes.BlockSize)
	}
	if len(iv) != aes.BlockSize {
		return nil, fmt.Errorf("invalid IV length: %d", len(iv))
	}

	block, err := aes.NewCipher(key)
	if err != nil {
		return nil, err
	}

	plaintext := make([]byte, len(data))
	cipher.NewCBCDecrypter(block, iv).CryptBlocks(plaintext, data)

	if len(plaintext) == 0 {
		return nil, fmt.Errorf("decrypted object is empty")
	}

	paddingLen := int(plaintext[len(plaintext)-1])
	if paddingLen < 1 || paddingLen > aes.BlockSize || paddingLen > len(plaintext) {
		return nil, fmt.Errorf("invalid PKCS#7 padding length: %d", paddingLen)
	}

	for i := len(plaintext) - paddingLen; i < len(plaintext); i++ {
		if plaintext[i] != byte(paddingLen) {
			return nil, fmt.Errorf("invalid PKCS#7 padding")
		}
	}

	return plaintext[:len(plaintext)-paddingLen], nil
}

func (d *clientStreamDownloader) decryptMediaObject(
	ctx context.Context,
	key *playlist.MediaKey,
	data []byte,
	sequence int,
	isMap bool,
) ([]byte, error) {
	if key == nil || key.Method == playlist.MediaKeyMethodNone {
		return data, nil
	}

	err := validateMediaKeySupport(key)
	if err != nil {
		return nil, err
	}

	var iv []byte
	if key.IV != "" {
		iv, err = parseAES128IV(key.IV)
		if err != nil {
			return nil, err
		}
	} else {
		if isMap {
			return nil, fmt.Errorf("encrypted EXT-X-MAP requires an explicit IV")
		}
		iv = mediaSequenceIV(sequence)
	}

	keyBytes, err := fetchAES128Key(ctx, d.httpClient, d.onRequest, d.playlistURL, key)
	if err != nil {
		return nil, err
	}

	return decryptAES128(data, keyBytes, iv)
}
