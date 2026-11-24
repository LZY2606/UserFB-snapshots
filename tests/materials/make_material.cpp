// make_material.cpp
//
// Standalone C++ material generator (NOT part of the fixed target build,
// which only compiles tests/golomb_contract.cpp).  It asks the production
// ALACEncoder to compress deterministic low-energy PCM that is guaranteed to
// take the compressed (escapeFlag == 0) branch, verifies an exact library
// round-trip, and emits a C header containing the frozen cookie, packet and
// expected PCM.  The contract test decodes that frozen packet as an
// independent compatibility reference; no audio or external library is used.
//
//   c++ -std=c++11 -Icodec tests/materials/make_material.cpp \
//       codec/*.c codec/*.cpp -o .task-build/make_material
//   ./.task-build/make_material tests/encoder_compat_material.h

#include "ALACEncoder.h"
#include "ALACDecoder.h"
#include "ALACBitUtilities.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cstring>
#include <cstdint>

struct Material {
    int bits, channels, frames;
    std::vector<unsigned char> cookie, packet, pcm;
};

static int encodeMaterial(int bits, int channels, int frames, Material& m) {
    m.bits = bits; m.channels = channels; m.frames = frames;
    int bps = bits / 8;
    m.pcm.assign(frames * channels * bps, 0);

    // Deterministic low-energy pattern: tiny bounded values with sparse
    // accents, well below the escape threshold so the result is compressed.
    uint32_t x = 12345u + (unsigned)bits * 7u + (unsigned)channels * 101u;
    for (int i = 0; i < frames; ++i) {
        for (int c = 0; c < channels; ++c) {
            x = x * 1664525u + 1013904223u;
            int32_t v;
            if ((i % 9) == (c ? 5 : 3)) {
                v = (int32_t)(((x >> 20) % 901) - 450);     // sparse accent
            } else {
                v = (int32_t)(((x >> 24) % 13) - 6);        // tiny value
            }
            if (bits == 16) {
                int16_t s = (int16_t)v;
                unsigned idx = (i * channels + c) * 2;
                m.pcm[idx] = (unsigned char)(s & 0xff);
                m.pcm[idx + 1] = (unsigned char)((s >> 8) & 0xff);
            } else { // 24 packed little-endian
                int32_t s = v;
                unsigned idx = (i * channels + c) * 3;
                m.pcm[idx] = (unsigned char)(s & 0xff);
                m.pcm[idx + 1] = (unsigned char)((s >> 8) & 0xff);
                m.pcm[idx + 2] = (unsigned char)((s >> 16) & 0xff);
            }
        }
    }

    AudioFormatDescription in = {}, out = {};
    in.mSampleRate = out.mSampleRate = 44100.0;
    in.mFormatID = kALACFormatLinearPCM;
    out.mFormatID = kALACFormatAppleLossless;
    in.mChannelsPerFrame = out.mChannelsPerFrame = channels;
    out.mChannelsPerFrame = channels;
    in.mBitsPerChannel = bits;
    in.mBytesPerFrame = channels * bps;
    in.mBytesPerPacket = in.mBytesPerFrame; // encoder divides ioNumBytes by this
    in.mFramesPerPacket = frames;
    in.mFormatFlags = kALACFormatFlagIsSignedInteger | kALACFormatFlagIsPacked;
    out.mFormatFlags = (bits == 16) ? 1u : 3u; // 1=16bit, 3=24bit

    ALACEncoder encoder;
    encoder.SetFrameSize((uint32_t)frames);
    if (encoder.InitializeEncoder(out) != 0) return 1;

    unsigned cookieSize = encoder.GetMagicCookieSize(channels);
    m.cookie.resize(cookieSize);
    unsigned got = cookieSize;
    encoder.GetMagicCookie(m.cookie.data(), &got);
    m.cookie.resize(got);

    std::vector<unsigned char> work(256u * 1024u);
    int32_t ioBytes = (int32_t)m.pcm.size();
    if (encoder.Encode(in, out, m.pcm.data(), work.data(), &ioBytes) != 0) return 2;
    m.packet.assign(work.begin(), work.begin() + ioBytes);

    // Element starts with tag(3)+instance(4)+unused(12)+partial/shift/escape(4);
    // escapeFlag is the last bit of that nibble, absolute bit 22.
    int escapeBit = -1;
    if (m.packet.size() >= 3) escapeBit = (m.packet[2] >> 1) & 1;
    if (escapeBit != 0) {
        fprintf(stderr, "material %d/%d was escape (bit=%d)\n", bits, channels, escapeBit);
        return 3;
    }

    // Verify exact library round-trip before freezing.
    ALACDecoder decoder;
    if (decoder.Init(m.cookie.data(), (uint32_t)m.cookie.size()) != 0) return 4;
    std::vector<unsigned char> decoded(m.pcm.size() + 64);
    BitBuffer bb;
    BitBufferInit(&bb, m.packet.data(), (uint32_t)m.packet.size());
    uint32_t outSamples = 0;
    if (decoder.Decode(&bb, decoded.data(), frames, channels, &outSamples) != 0) return 5;
    if (outSamples != (uint32_t)frames) return 6;
    if (memcmp(decoded.data(), m.pcm.data(), m.pcm.size()) != 0) return 7;
    return 0;
}

static void emitArray(FILE* f, const char* name, const std::vector<unsigned char>& a) {
    fprintf(f, "static const unsigned char %s[] = {", name);
    for (size_t i = 0; i < a.size(); ++i)
        fprintf(f, "%s0x%02x", (i % 12) ? "," : (i ? ",\n  " : "\n  "), a[i]);
    fprintf(f, "\n};\nstatic const unsigned %s_LEN = %zu;\n\n", name, a.size());
}

int main(int argc, char** argv) {
    const char* outPath = (argc > 1) ? argv[1] : "tests/encoder_compat_material.h";
    Material m16, m24;
    int r1 = encodeMaterial(16, 2, 64, m16);
    if (r1) { fprintf(stderr, "16-bit material failed: %d\n", r1); return 10 + r1; }
    int r2 = encodeMaterial(24, 2, 64, m24);
    if (r2) { fprintf(stderr, "24-bit material failed: %d\n", r2); return 20 + r2; }

    FILE* f = fopen(outPath, "w");
    if (!f) { perror("fopen"); return 30; }
    fprintf(f,
        "// encoder_compat_material.h\n"
        "// GENERATED by tests/materials/make_material.cpp -- do not edit by hand.\n"
        "// Frozen compressed (escapeFlag=0) packets from the production ALACEncoder\n"
        "// with their exact source PCM.  C++ generated; no downloads.\n"
        "#ifndef ENCODER_COMPAT_MATERIAL_H\n#define ENCODER_COMPAT_MATERIAL_H\n\n");
    fprintf(f, "static const int ENC_FRAMES = 64;\nstatic const int ENC_CHANNELS = 2;\n\n");
    emitArray(f, "ENC16_COOKIE", m16.cookie);
    emitArray(f, "ENC16_PACKET", m16.packet);
    emitArray(f, "ENC16_PCM", m16.pcm);
    emitArray(f, "ENC24_COOKIE", m24.cookie);
    emitArray(f, "ENC24_PACKET", m24.packet);
    emitArray(f, "ENC24_PCM", m24.pcm);
    fprintf(f, "#endif\n");
    fclose(f);
    printf("wrote %s (16bit pkt=%zu, 24bit pkt=%zu)\n", outPath, m16.packet.size(), m24.packet.size());
    return 0;
}
