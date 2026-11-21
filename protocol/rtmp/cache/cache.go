package cache

import (
	"github.com/gwuhaolin/livego/av"
	"github.com/gwuhaolin/livego/configure"
)

// Cache keeps the rolling window of complete historical GOPs together with the
// metadata and sequence headers that were active when each GOP started.
type Cache struct {
	gop *GopCache
}

func NewCache() *Cache {
	return &Cache{
		gop: NewGopCache(configure.Config.GetInt("gop_num")),
	}
}

func (cache *Cache) Write(p av.Packet) {
	cache.gop.Write(&p)
}

// Send writes the retained cache in live mode (no playback position).
func (cache *Cache) Send(w av.WriteCloser) error {
	return cache.gop.Send(w)
}

// SendFrom writes the retained cache, optionally rebased on a playback start
// position in milliseconds. It returns the offset that was applied.
func (cache *Cache) SendFrom(w av.WriteCloser, start uint32, requested bool) (uint32, error) {
	return cache.gop.SendFrom(w, start, requested)
}
