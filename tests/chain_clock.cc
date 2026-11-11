/* Regression test for chained-stream boundary clocks and pre-roll packet
   reuse in libopusenc.

   This test is deliberately self-contained: it parses the Ogg container
   itself (page header, lacing, CRC, packet boundaries) and uses only the
   public libopus decoder API, so it can catch mistakes made by the
   production ogg_packer/opusenc code instead of re-deriving them.

   Two independent references are checked per case (see doc/chain_clock.md):
     1. Container + clock: page-level integrity plus the rule that the
        audible end point for cumulative input N is ceil(N*48000/rate),
        with chain lengths taken as differences of adjacent cumulative
        end points (never per-segment rounding).
     2. Sound reference: every newly produced compressed packet is fed
        exactly once, in production order, to one persistent reference
        decoder; each chain is also decoded on its own and cropped to
        [pre-skip, EOS granule).  The two must agree within tolerance.

   Usage: chain_clock --case queued-44100-20
   (queued|streamed)-(44100|48000)-(20|60) */

#include <cstdarg>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "opusenc.h"

namespace {

std::string g_case = "?";

std::string strf(const char *fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  return std::string(buf);
}

[[noreturn]] void fail(const char *tag, const std::string &detail) {
  std::printf("FAIL %s %s %s\n", g_case.c_str(), tag, detail.c_str());
  std::exit(1);
}

void check(bool ok, const char *tag, const std::string &detail) {
  if (!ok) fail(tag, detail);
}

/* ---------------- case definition ---------------- */

const int kChains = 6;
const uint32_t kFirstSerial = 17001;
const int kQueuedSegments[kChains] = {333, 0, 1001, 2209, 5003, 457};
const int kStreamedSegments[kChains] = {94001, 15, 0, 44117, 5003, 457};
const int kBitrate = 96000;

struct CaseDef {
  std::string name;
  bool streamed;
  int rate;
  int frameMs;
};

bool parseCase(const std::string &name, CaseDef *cfg) {
  for (int s = 0; s < 2; s++) {
    for (int r = 0; r < 2; r++) {
      for (int m = 0; m < 2; m++) {
        CaseDef c;
        c.streamed = s == 1;
        c.rate = r == 0 ? 44100 : 48000;
        c.frameMs = m == 0 ? 20 : 60;
        c.name = std::string(c.streamed ? "streamed" : "queued") + "-" +
                 std::to_string(c.rate) + "-" + std::to_string(c.frameMs);
        if (name == c.name) {
          *cfg = c;
          return true;
        }
      }
    }
  }
  return false;
}

/* ---------------- encoding ---------------- */

struct EncOut {
  std::vector<uint8_t> pages;                       /* every written page byte, in order */
  std::vector<std::vector<uint8_t>> packets;        /* packet callback record, production order */
};

int writeCb(void *ud, const unsigned char *ptr, opus_int32 len) {
  EncOut *o = static_cast<EncOut *>(ud);
  o->pages.insert(o->pages.end(), ptr, ptr + len);
  return 0;
}

int closeCb(void *) { return 0; }

void packetCb(void *ud, const unsigned char *ptr, opus_int32 len, opus_uint32) {
  EncOut *o = static_cast<EncOut *>(ud);
  o->packets.emplace_back(ptr, ptr + len);
}

/* method 0: one int16 write per segment
   method 1: int16 writes fragmented into 1/7/251/13-frame chunks
   method 2: same chunks as float writes (int16 values scaled by 1/32768) */
EncOut encodeCase(const CaseDef &cfg, const int *segs,
                  const std::vector<int16_t> &pcm,
                  const std::vector<float> &pcmf, int method) {
  EncOut out;
  OpusEncCallbacks cbs = {&writeCb, &closeCb};
  OggOpusComments *comments = ope_comments_create();
  check(comments != nullptr, "ENCODE", "ope_comments_create failed");
  int err = 0;
  OggOpusEnc *enc = ope_encoder_create_callbacks(&cbs, &out, comments,
                                                 cfg.rate, 2, 0, &err);
  ope_comments_destroy(comments);
  check(enc != nullptr && err == OPE_OK, "ENCODE",
        strf("ope_encoder_create_callbacks: %s", ope_strerror(err)));
  check(ope_encoder_ctl(enc, OPE_SET_SERIALNO((opus_int32)kFirstSerial)) == OPE_OK,
        "ENCODE", "OPE_SET_SERIALNO failed");
  check(ope_encoder_ctl(enc, OPUS_SET_BITRATE(kBitrate)) == OPE_OK,
        "ENCODE", "OPUS_SET_BITRATE failed");
  check(ope_encoder_ctl(enc, OPUS_SET_EXPERT_FRAME_DURATION(
        cfg.frameMs == 20 ? OPUS_FRAMESIZE_20_MS : OPUS_FRAMESIZE_60_MS)) == OPE_OK,
        "ENCODE", "OPUS_SET_EXPERT_FRAME_DURATION failed");
  check(ope_encoder_ctl(enc, OPE_SET_PACKET_CALLBACK(&packetCb, &out)) == OPE_OK,
        "ENCODE", "OPE_SET_PACKET_CALLBACK failed");

  static const int kChunkCycle[4] = {1, 7, 251, 13};
  size_t chunkIdx = 0;
  int64_t off = 0;
  OggOpusComments *chainComments = ope_comments_create();
  check(chainComments != nullptr, "ENCODE", "ope_comments_create failed");
  for (int s = 0; s < kChains; s++) {
    int len = segs[s];
    if (len == 0) {
      /* Zero-length segments still perform a zero-frame write. */
      int rc = method == 2 ? ope_encoder_write_float(enc, pcmf.data() + off * 2, 0)
                           : ope_encoder_write(enc, pcm.data() + off * 2, 0);
      check(rc == OPE_OK, "ENCODE", strf("zero-frame write segment %d: %s", s, ope_strerror(rc)));
    }
    int written = 0;
    while (written < len) {
      int n = len - written;
      if (method != 0) {
        n = std::min(n, kChunkCycle[chunkIdx]);
        chunkIdx = (chunkIdx + 1) % 4;
      }
      int rc;
      if (method == 2)
        rc = ope_encoder_write_float(enc, pcmf.data() + (off + written) * 2, n);
      else
        rc = ope_encoder_write(enc, pcm.data() + (off + written) * 2, n);
      check(rc == OPE_OK, "ENCODE", strf("write segment %d: %s", s, ope_strerror(rc)));
      written += n;
    }
    off += len;
    if (s < kChains - 1) {
      int rc = ope_encoder_chain_current(enc, chainComments);
      check(rc == OPE_OK, "ENCODE", strf("chain_current after segment %d: %s", s, ope_strerror(rc)));
    }
  }
  ope_comments_destroy(chainComments);
  check(ope_encoder_drain(enc) == OPE_OK, "ENCODE", "drain failed");
  ope_encoder_destroy(enc);
  return out;
}

/* ---------------- Ogg container parsing (independent) ---------------- */

uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint64_t rd64(const uint8_t *p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
  return v;
}

/* Ogg CRC: polynomial 0x04c11db7, no reflection, init/xorout 0. */
uint32_t oggCrc(const uint8_t *data, size_t len) {
  static uint32_t table[256];
  static bool ready = false;
  if (!ready) {
    for (int i = 0; i < 256; i++) {
      uint32_t r = (uint32_t)i << 24;
      for (int b = 0; b < 8; b++)
        r = (r << 1) ^ ((r & 0x80000000u) ? 0x04c11db7u : 0u);
      table[i] = r;
    }
    ready = true;
  }
  uint32_t crc = 0;
  for (size_t i = 0; i < len; i++)
    crc = (crc << 8) ^ table[((crc >> 24) ^ data[i]) & 0xff];
  return crc;
}

struct Page {
  uint8_t flags;
  uint64_t granule;
  uint32_t serial;
  uint32_t seq;
  std::vector<uint8_t> lacing;
  size_t payload;     /* offset of payload in the file bytes */
  size_t payloadLen;
  size_t offset;
};

std::vector<Page> parsePages(const std::vector<uint8_t> &bytes) {
  std::vector<Page> pages;
  size_t pos = 0;
  while (pos < bytes.size()) {
    check(bytes.size() - pos >= 27, "CONTAINER",
          strf("short page header at offset %zu", pos));
    const uint8_t *h = bytes.data() + pos;
    check(std::memcmp(h, "OggS", 4) == 0, "CONTAINER",
          strf("bad capture pattern at offset %zu", pos));
    check(h[4] == 0, "CONTAINER", strf("unsupported stream structure version %d at offset %zu", h[4], pos));
    Page pg;
    pg.flags = h[5];
    check((pg.flags & ~0x07) == 0, "CONTAINER",
          strf("unknown header type flag 0x%02x at offset %zu", pg.flags, pos));
    pg.granule = rd64(h + 6);
    pg.serial = rd32(h + 14);
    pg.seq = rd32(h + 18);
    uint32_t crc = rd32(h + 22);
    int nsegs = h[26];
    check(bytes.size() - pos >= 27 + (size_t)nsegs, "CONTAINER",
          strf("short segment table at offset %zu", pos));
    pg.lacing.assign(h + 27, h + 27 + nsegs);
    size_t pay = 0;
    for (uint8_t b : pg.lacing) pay += b;
    check(bytes.size() - pos >= 27 + (size_t)nsegs + pay, "CONTAINER",
          strf("page at offset %zu overruns buffer", pos));
    std::vector<uint8_t> raw(h, h + 27 + nsegs + pay);
    std::memset(raw.data() + 22, 0, 4);
    check(oggCrc(raw.data(), raw.size()) == crc, "CONTAINER",
          strf("CRC mismatch on page at offset %zu", pos));
    pg.payload = pos + 27 + nsegs;
    pg.payloadLen = pay;
    pg.offset = pos;
    pages.push_back(pg);
    pos += 27 + nsegs + pay;
  }
  return pages;
}

struct Chain {
  uint32_t serial = 0;
  std::vector<size_t> pageIdx;                     /* indices into the page list */
  std::vector<std::vector<uint8_t>> packets;       /* reassembled packets in order */
  std::vector<int> packetPage;                     /* chain-local page index completing each packet */
  std::vector<int> pageFirstPkt;                   /* per chain-local page: first/last packet */
  std::vector<int> pageLastPkt;                    /*   completed on it, or -1 */
  uint32_t preskip = 0;
  uint64_t eosGranule = 0;
  int64_t totalDuration = 0;                       /* sum of audio packet durations */
};

/* Split pages into chains and run all page-level structural checks. */
std::vector<Chain> splitChains(const std::vector<Page> &pages,
                               const std::vector<uint8_t> &bytes) {
  check(!pages.empty(), "CONTAINER", "no pages in stream");
  std::vector<Chain> chains;
  for (size_t i = 0; i < pages.size(); i++) {
    const Page &pg = pages[i];
    if (chains.empty() || pg.serial != chains.back().serial) {
      check(pg.serial == kFirstSerial + chains.size(), "CONTAINER",
            strf("page %zu: expected serial %u, got %u", i,
                 kFirstSerial + (unsigned)chains.size(), pg.serial));
      chains.emplace_back();
      chains.back().serial = pg.serial;
      check((pg.flags & 0x02) != 0, "CONTAINER",
            strf("chain %zu: first page %zu missing BOS", chains.size() - 1, i));
      check((pg.flags & 0x01) == 0, "CONTAINER",
            strf("chain %zu: first page %zu marked as continued", chains.size() - 1, i));
      check(pg.seq == 0, "CONTAINER",
            strf("chain %zu: first page number %u != 0", chains.size() - 1, pg.seq));
    } else {
      check((pg.flags & 0x02) == 0, "CONTAINER",
            strf("chain %zu: BOS repeated on page %zu", chains.size() - 1, i));
      check(pg.seq == chains.back().pageIdx.size(), "CONTAINER",
            strf("chain %zu: page number %u, expected %zu", chains.size() - 1,
                 pg.seq, chains.back().pageIdx.size()));
    }
    chains.back().pageIdx.push_back(i);
  }
  check(chains.size() == (size_t)kChains, "CONTAINER",
        strf("expected %d chains, found %zu", kChains, chains.size()));

  for (size_t c = 0; c < chains.size(); c++) {
    Chain &ch = chains[c];
    /* EOS exactly on the last page; continued-packet flag consistency. */
    bool prevEndsMidPacket = false;
    for (size_t lp = 0; lp < ch.pageIdx.size(); lp++) {
      const Page &pg = pages[ch.pageIdx[lp]];
      bool eos = (pg.flags & 0x04) != 0;
      check(eos == (lp + 1 == ch.pageIdx.size()), "CONTAINER",
            strf("chain %zu page %zu: EOS flag %d, expected %d", c, lp,
                 (int)eos, (int)(lp + 1 == ch.pageIdx.size())));
      bool cont = (pg.flags & 0x01) != 0;
      check(cont == prevEndsMidPacket, "CONTAINER",
            strf("chain %zu page %zu: continued flag %d, expected %d", c, lp,
                 (int)cont, (int)prevEndsMidPacket));
      prevEndsMidPacket = !pg.lacing.empty() && pg.lacing.back() == 255;
      if (prevEndsMidPacket)
        check(pg.granule == 0xFFFFFFFFFFFFFFFFull, "CONTAINER",
              strf("chain %zu page %zu ends mid-packet but granule is not -1", c, lp));
    }

    /* Reassemble packets from lacing values. */
    std::vector<uint8_t> cur;
    for (size_t lp = 0; lp < ch.pageIdx.size(); lp++) {
      const Page &pg = pages[ch.pageIdx[lp]];
      size_t p = pg.payload;
      int first = -1, last = -1;
      for (uint8_t lace : pg.lacing) {
        cur.insert(cur.end(), bytes.begin() + p, bytes.begin() + p + lace);
        p += lace;
        if (lace < 255) {
          if (first < 0) first = (int)ch.packets.size();
          ch.packets.push_back(cur);
          ch.packetPage.push_back((int)lp);
          last = (int)ch.packets.size() - 1;
          cur.clear();
        }
      }
      ch.pageFirstPkt.push_back(first);
      ch.pageLastPkt.push_back(last);
    }
    check(cur.empty(), "CONTAINER", strf("chain %zu ends mid-packet", c));
    check(ch.pageLastPkt.back() == (int)ch.packets.size() - 1,
          "CONTAINER", strf("chain %zu: last page completes no packet", c));
    /* The chain's EOS granule is the granule of its last (EOS) page. */
    ch.eosGranule = pages[ch.pageIdx.back()].granule;
  }
  return chains;
}

/* Parse and validate the OpusHead/OpusTags packets of one chain. */
void checkChainHeader(Chain &ch, size_t c, const CaseDef &cfg) {
  check(ch.packets.size() >= 2, "HEADER", strf("chain %zu: fewer than 2 packets", c));
  const std::vector<uint8_t> &head = ch.packets[0];
  check(head.size() == 19 && std::memcmp(head.data(), "OpusHead", 8) == 0,
        "HEADER", strf("chain %zu: bad OpusHead packet (%zu bytes)", c, head.size()));
  check(head[8] == 1, "HEADER", strf("chain %zu: OpusHead version %d", c, head[8]));
  check(head[9] == 2, "HEADER", strf("chain %zu: OpusHead channels %d", c, head[9]));
  ch.preskip = (uint32_t)head[10] | ((uint32_t)head[11] << 8);
  check(rd32(&head[12]) == (uint32_t)cfg.rate, "HEADER",
        strf("chain %zu: OpusHead input rate %u != %d", c, rd32(&head[12]), cfg.rate));
  check(head[16] == 0 && head[17] == 0, "HEADER", strf("chain %zu: nonzero gain", c));
  check(head[18] == 0, "HEADER", strf("chain %zu: mapping family %d", c, head[18]));

  const std::vector<uint8_t> &tags = ch.packets[1];
  check(tags.size() >= 16 && std::memcmp(tags.data(), "OpusTags", 8) == 0,
        "HEADER", strf("chain %zu: bad OpusTags packet", c));
  {
    uint32_t vlen = rd32(&tags[8]);
    check(12 + (size_t)vlen + 4 <= tags.size(), "HEADER",
          strf("chain %zu: OpusTags vendor string overruns packet", c));
    uint32_t ncomments = rd32(&tags[12 + vlen]);
    size_t p = 16 + vlen;
    for (uint32_t n = 0; n < ncomments; n++) {
      check(p + 4 <= tags.size(), "HEADER", strf("chain %zu: comment %u truncated", c, n));
      uint32_t l = rd32(&tags[p]);
      p += 4;
      check(p + l <= tags.size(), "HEADER", strf("chain %zu: comment %u overruns packet", c, n));
      p += l;
    }
    check(p <= tags.size(), "HEADER", strf("chain %zu: comment list overruns packet", c));
  }

  /* The head and tags packets each sit alone on the first two pages. */
  check(ch.packetPage[0] == 0 && ch.pageFirstPkt[0] == 0 && ch.pageLastPkt[0] == 0,
        "CONTAINER", strf("chain %zu: OpusHead not alone on page 0", c));
  check(ch.packetPage[1] == 1 && ch.pageFirstPkt[1] == 1 && ch.pageLastPkt[1] == 1,
        "CONTAINER", strf("chain %zu: OpusTags not alone on page 1", c));
}

/* Check the audio-page granule clock of one chain against the packet
   durations, and record the chain's total decodable duration. */
void checkChainGranuleClock(Chain &ch, size_t c, const std::vector<Page> &pages) {
  /* Walk audio packets; durations always come from the real Opus packets
     (the final drain may shorten the last packet in 60 ms mode). */
  std::vector<int64_t> cumAfter(ch.packets.size(), 0);
  std::vector<int> dur(ch.packets.size(), 0);
  int64_t cum = 0;
  for (size_t pi = 2; pi < ch.packets.size(); pi++) {
    int d = opus_packet_get_nb_samples(ch.packets[pi].data(),
                                       (opus_int32)ch.packets[pi].size(), 48000);
    check(d > 0, "CONTAINER", strf("chain %zu packet %zu: invalid Opus packet", c, pi));
    cum += d;
    dur[pi] = d;
    cumAfter[pi] = cum;
  }
  ch.totalDuration = cum;
  for (size_t lp = 0; lp < ch.pageIdx.size(); lp++) {
    const Page &pg = pages[ch.pageIdx[lp]];
    int lastPkt = ch.pageLastPkt[lp];
    if (lastPkt < 0) continue; /* mid-packet page: granule -1 checked earlier */
    if (lastPkt < 2) {
      check(pg.granule == 0, "CONTAINER",
            strf("chain %zu page %zu: header page granule %llu != 0", c, lp,
                 (unsigned long long)pg.granule));
      continue;
    }
    int64_t g = cumAfter[lastPkt];
    if ((pg.flags & 0x04) != 0) {
      /* Final page of the chain: the tail of the last packet may be cropped,
         so the granule must land inside the last packet's span. */
      check(pg.granule <= (uint64_t)g && pg.granule > (uint64_t)(g - dur[lastPkt]),
            "GRANULE", strf("chain %zu: EOS granule %llu outside last packet span (%lld, %lld]",
                            c, (unsigned long long)pg.granule,
                            (long long)(g - dur[lastPkt]), (long long)g));
      check(pg.granule == ch.eosGranule, "GRANULE",
            strf("chain %zu: EOS page granule mismatch", c));
    } else {
      /* A normal audio page granule is exactly the position decoded so far. */
      check(pg.granule == (uint64_t)g, "GRANULE",
            strf("chain %zu page %zu: granule %llu != decoded position %lld",
                 c, lp, (unsigned long long)pg.granule, (long long)g));
    }
  }
}

/* ---------------- decoding helpers ---------------- */

/* Decode one packet with the public decoder API and append float PCM. */
void decodePacket(OpusDecoder *dec, const std::vector<uint8_t> &pkt,
                  std::vector<float> *out, const char *tag, const char *what) {
  float buf[5760 * 2];
  int d = opus_packet_get_nb_samples(pkt.data(), (opus_int32)pkt.size(), 48000);
  check(d > 0, tag, strf("%s: invalid packet (%zu bytes)", what, pkt.size()));
  int got = opus_decode_float(dec, pkt.data(), (opus_int32)pkt.size(), buf, d, 0);
  check(got == d, tag,
        strf("%s: opus_decode_float returned %d, packet duration is %d", what, got, d));
  out->insert(out->end(), buf, buf + (size_t)d * 2);
}

/* ---------------- the case ---------------- */

void runCase(const CaseDef &cfg) {
  const int *segs = cfg.streamed ? kStreamedSegments : kQueuedSegments;
  int64_t total = 0;
  for (int k = 0; k < kChains; k++) total += segs[k];

  /* Cumulative audible end points on the 48 kHz granule clock.  The end
     point for cumulative input N is ceil(N*48000/rate); a chain's length
     is the difference of adjacent cumulative end points, never a
     per-segment rounding. */
  int64_t endpoint[kChains + 1];
  endpoint[0] = 0;
  {
    int64_t cum = 0;
    for (int k = 0; k < kChains; k++) {
      cum += segs[k];
      endpoint[k + 1] = (cum * 48000 + cfg.rate - 1) / cfg.rate;
    }
  }

  /* Input signal: continuous over the whole input timeline. */
  std::vector<int16_t> pcm((size_t)total * 2);
  for (int64_t i = 0; i < total; i++) {
    for (int c = 0; c < 2; c++) {
      double v = 6000.0 * std::sin(i * (0.013 + c * 0.007)) + 2500.0 * std::sin(i * 0.071);
      pcm[(size_t)i * 2 + c] = (int16_t)v; /* truncation toward zero */
    }
  }
  std::vector<float> pcmf((size_t)total * 2);
  for (size_t i = 0; i < pcmf.size(); i++)
    pcmf[i] = (float)((double)pcm[i] / 32768.0); /* exact in binary float */

  /* Three write methods; the complete page bytes must be identical. */
  EncOut out[3];
  for (int m = 0; m < 3; m++) out[m] = encodeCase(cfg, segs, pcm, pcmf, m);
  check(out[1].pages == out[0].pages, "WRITE_CONSISTENCY",
        "fragmented int16 writes changed the page bytes");
  check(out[2].pages == out[0].pages, "WRITE_CONSISTENCY",
        "float writes changed the page bytes");
  std::printf("WRITE_CONSISTENCY_OK %s methods=3\n", cfg.name.c_str());

  /* ---- independent reference, part 1: container and clock ---- */
  std::vector<Page> pages = parsePages(out[0].pages);
  std::vector<Chain> chains = splitChains(pages, out[0].pages);
  for (size_t c = 0; c < chains.size(); c++)
    checkChainHeader(chains[c], c, cfg);

  /* Per-chain audible length: [pre-skip, EOS granule) must equal the
     difference of adjacent cumulative end points.  This is the duration
     check for all six chains. */
  for (int k = 0; k < kChains; k++) {
    int64_t expect = endpoint[k + 1] - endpoint[k];
    int64_t audible = (int64_t)chains[k].eosGranule - (int64_t)chains[k].preskip;
    check(audible == expect, "CLOCK_LENGTH",
          strf("chain=%d eos=%lld preskip=%u audible=%lld expected=%lld", k,
               (long long)chains[k].eosGranule, chains[k].preskip,
               (long long)audible, (long long)expect));
  }
  std::printf("CLOCK_OK %s chains=6\n", cfg.name.c_str());

  /* ---- independent reference, part 2: the sound reference ----
     The packet callback record preserves the raw bytes and the order of
     every produced packet.  Packets reused at chain boundaries (the
     previous chain's tail packet and, when present, the chaining
     keyframe) are reported again when they are committed to the new
     chain; they are aligned back onto the single compressed timeline by
     skipping any packet identical to one of the last few produced
     packets (the chaining overlap never exceeds a couple of packets).
     Everything else is fed, exactly once, to one persistent reference
     decoder. */
  size_t audioPacketsInPages = 0;
  for (int k = 0; k < kChains; k++) audioPacketsInPages += chains[k].packets.size() - 2;
  std::vector<float> refPcm;
  {
    int err = 0;
    OpusDecoder *ref = opus_decoder_create(48000, 2, &err);
    check(ref != nullptr && err == OPUS_OK, "REFERENCE", "opus_decoder_create failed");
    std::vector<std::vector<uint8_t>> recent;
    size_t accepted = 0, reused = 0;
    for (const std::vector<uint8_t> &pkt : out[0].packets) {
      if (pkt.size() >= 8 &&
          (std::memcmp(pkt.data(), "OpusHead", 8) == 0 ||
           std::memcmp(pkt.data(), "OpusTags", 8) == 0))
        continue;
      bool dup = false;
      for (const std::vector<uint8_t> &r : recent) {
        if (r == pkt) { dup = true; break; }
      }
      if (dup) {
        reused++;
        continue;
      }
      decodePacket(ref, pkt, &refPcm, "REFERENCE", "production packet");
      recent.push_back(pkt);
      if (recent.size() > 16) recent.erase(recent.begin());
      accepted++;
    }
    opus_decoder_destroy(ref);
    check(accepted > 0, "REFERENCE", "no audio packets were produced");
    check(audioPacketsInPages == accepted + reused, "REFERENCE",
          strf("pages hold %zu audio packets but production record has %zu new + %zu reused",
               audioPacketsInPages, accepted, reused));
  }

  /* Slice the reference by the first chain's pre-skip and the cumulative
     input end points. */
  int64_t preskip0 = chains[0].preskip;
  int64_t refSamples = (int64_t)refPcm.size() / 2;
  check(refSamples - preskip0 >= endpoint[kChains], "REFERENCE",
        strf("reference timeline too short: %lld audible samples, need %lld",
             (long long)(refSamples - preskip0), (long long)endpoint[kChains]));

  /* Now walk the chains in order.  Each chain gets its granule clock
     cross-checked, is decoded with its own decoder state (zero-length
     chains included), is cropped to [pre-skip, EOS granule), and is
     compared against the matching slice of the sound reference. */
  double sumSq = 0.0, maxAbs = 0.0;
  int64_t count = 0;
  for (int k = 0; k < kChains; k++) {
    Chain &ch = chains[k];
    checkChainGranuleClock(ch, k, pages);

    int err = 0;
    OpusDecoder *dec = opus_decoder_create(48000, 2, &err);
    check(dec != nullptr && err == OPUS_OK, "DECODE",
          strf("chain=%d opus_decoder_create failed", k));
    std::vector<float> full;
    for (size_t pi = 2; pi < ch.packets.size(); pi++)
      decodePacket(dec, ch.packets[pi], &full, "DECODE",
                   strf("chain=%d packet=%zu", k, pi).c_str());
    opus_decoder_destroy(dec);
    check((int64_t)(full.size() / 2) == ch.totalDuration, "DECODE",
          strf("chain=%d decoded %zu samples, packet durations sum to %lld", k,
               full.size() / 2, (long long)ch.totalDuration));
    check((uint64_t)ch.preskip <= ch.eosGranule &&
          ch.eosGranule <= (uint64_t)(full.size() / 2), "DECODE",
          strf("chain=%d crop [%u, %llu) outside decoded length %zu", k, ch.preskip,
               (unsigned long long)ch.eosGranule, full.size() / 2));
    std::vector<float> audible(full.begin() + (size_t)ch.preskip * 2,
                               full.begin() + (size_t)ch.eosGranule * 2);

    int64_t len = endpoint[k + 1] - endpoint[k];
    check((int64_t)(audible.size() / 2) == len, "PCM_REFERENCE",
          strf("chain=%d cropped length %zu != expected %lld", k,
               audible.size() / 2, (long long)len));
    for (int64_t j = 0; j < len; j++) {
      for (int c = 0; c < 2; c++) {
        double a = audible[(size_t)j * 2 + c];
        double b = refPcm[(size_t)(preskip0 + endpoint[k] + j) * 2 + c];
        double d = std::fabs(a - b);
        if (d > maxAbs) maxAbs = d;
        sumSq += d * d;
        count++;
        if (d > 0.04)
          fail("PCM_REFERENCE",
               strf("chain=%d sample=%lld channel=%d |diff|=%.6f chain=%.6f reference=%.6f",
                    k, (long long)j, c, d, a, b));
      }
    }
  }
  double rms = count > 0 ? std::sqrt(sumSq / (double)count) : 0.0;
  check(rms <= 0.003, "PCM_REFERENCE",
        strf("RMS error %.6f exceeds 0.003 (max abs %.6f, %lld samples)", rms, maxAbs,
             (long long)count));
  std::printf("REFERENCE_OK %s samples=%lld max_abs=%.6f rms=%.6f\n",
              cfg.name.c_str(), (long long)count, maxAbs, rms);
  std::printf("PASS %s chains=6\n", cfg.name.c_str());
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 3 || std::string(argv[1]) != "--case") {
    std::fprintf(stderr, "usage: %s --case (queued|streamed)-(44100|48000)-(20|60)\n", argv[0]);
    return 2;
  }
  CaseDef cfg;
  if (!parseCase(argv[2], &cfg)) {
    std::fprintf(stderr, "unknown case: %s\n", argv[2]);
    return 2;
  }
  g_case = cfg.name;
  runCase(cfg);
  return 0;
}
