// Task test cases for "sfotool --subset". Only C/C++ standard library and the
// production tsf/sfotool binaries are used. Fixtures are synthesized in code.
#include "check.h"
#include "tsf.h"

#include <vector>
#include <string>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <iterator>

typedef std::vector<unsigned char> Bytes;

/* ------------------------------------------------------------------ */
/* byte builders                                                      */
/* ------------------------------------------------------------------ */

static void u16(Bytes& b, unsigned x) { b.push_back((unsigned char)x); b.push_back((unsigned char)(x >> 8)); }
static void u32(Bytes& b, unsigned x) { u16(b, x); u16(b, x >> 16); }
static void name(Bytes& b, const char* s) { size_t n = strlen(s); for (int i = 0; i < 20; i++) b.push_back((unsigned char)(i < (int)n ? s[i] : 0)); }
static void tag(Bytes& b, const char* s) { b.insert(b.end(), s, s + 4); }
static void chunk(Bytes& b, const char* s, const Bytes& v) { tag(b, s); u32(b, (unsigned)v.size()); b.insert(b.end(), v.begin(), v.end()); if (v.size() % 2) b.push_back(0); }
static Bytes list(const char* s, const Bytes& v) { Bytes b; tag(b, s); b.insert(b.end(), v.begin(), v.end()); return b; }

static unsigned ru16(const Bytes& b, size_t o) { return b[o] | ((unsigned)b[o + 1] << 8); }
static unsigned ru32(const Bytes& b, size_t o) { return ru16(b, o) | ((unsigned)ru16(b, o + 2) << 16); }

/* ------------------------------------------------------------------ */
/* fixture font                                                       */
/* ------------------------------------------------------------------ */

// PCM pool layout (16-bit sample points):
//   SineA   [0,100)    loop 10..90     mono
//   SineB   [120,220)  loop 130..210   mono      (guard range overlaps SineA's)
//   LeftC   [1000,1100) loop 1010..1090 left, linked to RightD
//   RightD  [2000,2100) loop 2010..2090 right, linked to LeftC, no zone uses it
//   UnusedE [3000,3100) loop 3010..3090 mono, only used by the dropped preset
//   EOS at 3146 (pool size)
#define FIX_POOL_SIZE 3146
#define FIX_SM24      1
#define FIX_SMPO      2
#define FIX_ROM       4
#define FIX_BADLINK   8
#define FIX_PAST_SMPL 16
#define FIX_BAG_DISORDER 32
#define FIX_BAD_INSTREF  64

static int16_t fix_pcm(unsigned i) { return (int16_t)(((uint32_t)i * 1103515245u + 12345u) >> 16); }

static void rec_phdr(Bytes& b, const char* nm, unsigned preset, unsigned bank, unsigned bag)
{ name(b, nm); u16(b, preset); u16(b, bank); u16(b, bag); u32(b, 0); u32(b, 0); u32(b, 0); }
static void rec_bag(Bytes& b, unsigned gen, unsigned mod) { u16(b, gen); u16(b, mod); }
static void rec_mod(Bytes& b, unsigned src, unsigned dst, int amt, unsigned amtsrc, unsigned trans)
{ u16(b, src); u16(b, dst); u16(b, (unsigned)(uint16_t)(int16_t)amt); u16(b, amtsrc); u16(b, trans); }
static void rec_gen(Bytes& b, unsigned oper, unsigned amount) { u16(b, oper); u16(b, amount); }
static void rec_inst(Bytes& b, const char* nm, unsigned bag) { name(b, nm); u16(b, bag); }
static void rec_shdr(Bytes& b, const char* nm, unsigned st, unsigned en, unsigned ls, unsigned le,
                     unsigned rate, unsigned pitch, int pc, unsigned link, unsigned type)
{ name(b, nm); u32(b, st); u32(b, en); u32(b, ls); u32(b, le); u32(b, rate);
  b.push_back((unsigned char)pitch); b.push_back((unsigned char)(int8_t)pc); u16(b, link); u16(b, type); }

static Bytes fixture_font(int flags)
{
    Bytes pdta, sdta, info, all, sm, b;
    unsigned i;
    for (i = 0; i < FIX_POOL_SIZE; i++) u16(sm, (unsigned)(uint16_t)fix_pcm(i));
    chunk(sdta, "smpl", sm);
    if (flags & FIX_SM24) chunk(sdta, "sm24", sm);
    if (flags & FIX_SMPO) { Bytes og; for (i = 0; i < 64; i++) og.push_back((unsigned char)i); chunk(sdta, "smpo", og); }
    { Bytes x; x.push_back(9); x.push_back(8); x.push_back(7); x.push_back(6); chunk(sdta, "xtr2", x); }

    rec_phdr(b, "PianoA", 0, 0, 0);
    rec_phdr(b, "PianoB", 1, 0, (flags & FIX_BAG_DISORDER) ? 5 : 2);
    rec_phdr(b, "Dropped", 0, 1, 4);
    rec_phdr(b, "EOP", 0, 0, 5);
    chunk(pdta, "phdr", b); b.clear();

    rec_bag(b, 0, 0); rec_bag(b, 1, 1); rec_bag(b, 2, 1); rec_bag(b, 3, 1); rec_bag(b, 4, 2); rec_bag(b, 5, 2);
    chunk(pdta, "pbag", b); b.clear();

    rec_mod(b, 0x0102, 48, 250, 0x0007, 1);   // PianoA global zone modulator
    rec_mod(b, 0x000D, 8, -300, 0x0081, 2);   // PianoB second zone modulator
    rec_mod(b, 0, 0, 0, 0, 0);
    chunk(pdta, "pmod", b); b.clear();

    rec_gen(b, 48, 3);                                             // PianoA global: attenuation
    rec_gen(b, 41, (flags & FIX_BAD_INSTREF) ? 99 : 0);            // PianoA -> SharedInst
    rec_gen(b, 41, 0);                                             // PianoB -> SharedInst
    rec_gen(b, 41, 1);                                             // PianoB -> DualInst
    rec_gen(b, 41, 2);                                             // Dropped -> DroppedInst
    rec_gen(b, 0, 0);
    chunk(pdta, "pgen", b); b.clear();

    rec_inst(b, "SharedInst", 0); rec_inst(b, "DualInst", 2); rec_inst(b, "DroppedInst", 5); rec_inst(b, "EOI", 6);
    chunk(pdta, "inst", b); b.clear();

    rec_bag(b, 0, 0); rec_bag(b, 1, 1); rec_bag(b, 3, 1); rec_bag(b, 4, 2); rec_bag(b, 7, 2); rec_bag(b, 10, 2); rec_bag(b, 11, 2);
    chunk(pdta, "ibag", b); b.clear();

    rec_mod(b, 0x0081, 48, 100, 0x0002, 0);   // SharedInst global zone modulator
    rec_mod(b, 0x0002, 8, 77, 0x0081, 1);     // DualInst global zone modulator
    rec_mod(b, 0, 0, 0, 0, 0);
    chunk(pdta, "imod", b); b.clear();

    rec_gen(b, 48, 5);                        // SharedInst global: attenuation
    rec_gen(b, 43, 0 | (127 << 8));           // SharedInst local: keys 0-127
    rec_gen(b, 53, 0);                        //   -> SineA
    rec_gen(b, 48, 2);                        // DualInst global: attenuation
    rec_gen(b, 43, 0 | (59 << 8));            // DualInst zone 1: keys 0-59
    rec_gen(b, 54, 1);                        //   loop continuous
    rec_gen(b, 53, 1);                        //   -> SineB
    rec_gen(b, 43, 60 | (127 << 8));          // DualInst zone 2: keys 60-127
    rec_gen(b, 54, 1);                        //   loop continuous
    rec_gen(b, 53, 2);                        //   -> LeftC
    rec_gen(b, 53, 4);                        // DroppedInst -> UnusedE
    rec_gen(b, 0, 0);
    chunk(pdta, "igen", b); b.clear();

    rec_shdr(b, "SineA", 0, 100, 10, 90, 22050, 60, 0, 0, 1);
    rec_shdr(b, "SineB", 120, 220, 130, 210, 22050, 60, 0, 0, (flags & FIX_ROM) ? 0x8001 : 1);
    rec_shdr(b, "LeftC", 1000, 1100, 1010, 1090, 22050, 60, 0, 3, 4);
    rec_shdr(b, "RightD", 2000, 2100, 2010, 2090, 22050, 60, 0, (flags & FIX_BADLINK) ? 9 : 2, 2);
    rec_shdr(b, "UnusedE", 3000, (flags & FIX_PAST_SMPL) ? 40000 : 3100, 3010, 3090, 22050, 60, 0, 0, 1);
    rec_shdr(b, "EOS", FIX_POOL_SIZE, FIX_POOL_SIZE, FIX_POOL_SIZE, FIX_POOL_SIZE, 0, 0, 0, 0, 0);
    chunk(pdta, "shdr", b); b.clear();

    { Bytes x; for (i = 1; i <= 6; i++) x.push_back((unsigned char)i); chunk(pdta, "xtra", x); } // unknown pdta chunk

    { Bytes v; u16(v, 2); u16(v, 1); chunk(info, "ifil", v); }
    { Bytes v; const char* s = "FixTure"; for (i = 0; i < 7; i++) v.push_back((unsigned char)s[i]); chunk(info, "INAM", v); } // odd payload

    chunk(all, "LIST", list("INFO", info));
    chunk(all, "LIST", list("sdta", sdta));
    chunk(all, "LIST", list("pdta", pdta));
    { Bytes x; for (i = 0; i < 5; i++) x.push_back((unsigned char)(0xF0 + i)); chunk(all, "Xtra", x); } // unknown odd top-level chunk
    { Bytes body = list("sfbk", all), out; chunk(out, "RIFF", body); return out; }
}

/* ------------------------------------------------------------------ */
/* strict SF2 parser used to verify the repacked output               */
/* ------------------------------------------------------------------ */

struct Sf2
{
    std::vector<Bytes> phdr, pbag, pmod, pgen, inst, ibag, imod, igen, shdr;
    Bytes smpl, info_raw, xtra_raw, xtra_pdta, xtr2_sdta;
};

static bool parse_records(const Bytes& d, size_t off, unsigned size, unsigned recsize, std::vector<Bytes>& out)
{
    unsigned i;
    if (size % recsize) return false;
    for (i = 0; i < size; i += recsize)
        out.push_back(Bytes(d.begin() + off + i, d.begin() + off + i + recsize));
    return true;
}

static bool parse_sf2(const Bytes& d, Sf2& o)
{
    size_t off, end;
    int have_sdta = 0, have_pdta = 0;
    if (d.size() < 12 || memcmp(d.data(), "RIFF", 4) || memcmp(d.data() + 8, "sfbk", 4)) return false;
    if (ru32(d, 4) != d.size() - 8) return false;
    off = 12; end = d.size();
    while (off < end)
    {
        unsigned size, i;
        size_t total, co, ce;
        if (end - off < 8) return false;
        size = ru32(d, off + 4);
        total = 8 + (size_t)size + (size & 1);
        if (total > end - off) return false;
        if (!memcmp(d.data() + off, "LIST", 4) && size >= 4 && !memcmp(d.data() + off + 8, "sdta", 4))
        {
            if (have_sdta) return false;
            have_sdta = 1;
            co = off + 12; ce = off + 8 + size;
            while (co < ce)
            {
                unsigned cs; size_t ct;
                if (ce - co < 8) return false;
                cs = ru32(d, co + 4);
                ct = 8 + (size_t)cs + (cs & 1);
                if (ct > ce - co) return false;
                if (!memcmp(d.data() + co, "smpl", 4)) o.smpl.assign(d.begin() + co + 8, d.begin() + co + 8 + cs);
                else if (!memcmp(d.data() + co, "xtr2", 4)) o.xtr2_sdta.assign(d.begin() + co, d.begin() + co + ct);
                co += ct;
            }
        }
        else if (!memcmp(d.data() + off, "LIST", 4) && size >= 4 && !memcmp(d.data() + off + 8, "pdta", 4))
        {
            static const struct { const char* id; unsigned recsize; } tabs[9] = {
                { "phdr", 38 }, { "pbag", 4 }, { "pmod", 10 }, { "pgen", 4 }, { "inst", 22 },
                { "ibag", 4 }, { "imod", 10 }, { "igen", 4 }, { "shdr", 46 } };
            if (have_pdta) return false;
            have_pdta = 1;
            co = off + 12; ce = off + 8 + size;
            while (co < ce)
            {
                unsigned cs; size_t ct;
                if (ce - co < 8) return false;
                cs = ru32(d, co + 4);
                ct = 8 + (size_t)cs + (cs & 1);
                if (ct > ce - co) return false;
                for (i = 0; i < 9; i++)
                {
                    if (memcmp(d.data() + co, tabs[i].id, 4)) continue;
                    if (!parse_records(d, co + 8, cs, tabs[i].recsize,
                            i == 0 ? o.phdr : i == 1 ? o.pbag : i == 2 ? o.pmod : i == 3 ? o.pgen :
                            i == 4 ? o.inst : i == 5 ? o.ibag : i == 6 ? o.imod : i == 7 ? o.igen : o.shdr))
                        return false;
                    break;
                }
                if (i == 9 && !memcmp(d.data() + co, "xtra", 4)) o.xtra_pdta.assign(d.begin() + co, d.begin() + co + ct);
                co += ct;
            }
        }
        else if (!memcmp(d.data() + off, "LIST", 4) && size >= 4 && !memcmp(d.data() + off + 8, "INFO", 4))
            o.info_raw.assign(d.begin() + off, d.begin() + off + total);
        else if (!memcmp(d.data() + off, "Xtra", 4))
            o.xtra_raw.assign(d.begin() + off, d.begin() + off + total);
        off += total;
    }
    return have_sdta && have_pdta && !o.smpl.empty();
}

static std::string rec_name(const Bytes& r)
{
    size_t n = 0;
    while (n < 20 && r[n]) n++;
    return std::string((const char*)r.data(), n);
}

static bool check_shdr(const Bytes& r, const char* nm, unsigned st, unsigned en, unsigned ls, unsigned le,
                       unsigned rate, unsigned pitch, unsigned link, unsigned type)
{
    return rec_name(r) == nm && ru32(r, 20) == st && ru32(r, 24) == en && ru32(r, 28) == ls && ru32(r, 32) == le
        && ru32(r, 36) == rate && r[40] == pitch && ru16(r, 42) == link && ru16(r, 44) == type;
}

static bool check_bag(const Bytes& r, unsigned gen, unsigned mod) { return ru16(r, 0) == gen && ru16(r, 2) == mod; }
static bool check_gen(const Bytes& r, unsigned oper, unsigned amount) { return ru16(r, 0) == oper && ru16(r, 2) == amount; }

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static bool write_file(const char* path, const Bytes& b)
{
    std::ofstream o(path, std::ios::binary);
    o.write((const char*)b.data(), (std::streamsize)b.size());
    return !!o;
}

static Bytes read_file(const char* path)
{
    std::ifstream in(path, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static int run_tool(const std::string& args)
{
    std::string cmd = ".task-build/sfotool " + args + " >/dev/null 2>&1";
    return std::system(cmd.c_str());
}

static int run_subset(const char* in, const char* out, const char* list)
{
    std::string cmd = "--subset ";
    cmd += in; cmd += " "; cmd += out; cmd += " \""; cmd += list; cmd += "\"";
    return run_tool(cmd);
}

#define FIXTURE ".task-build/sub_fixture.sf2"
#define SUBOUT  ".task-build/sub_out.sf2"

static int write_fixture(int flags)
{
    Bytes f = fixture_font(flags);
    if (f.empty() || !write_file(FIXTURE, f)) return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* closure group                                                      */
/* ------------------------------------------------------------------ */

static int case_closure_indices(void)
{
    Sf2 src, s;
    Bytes out;
    if (write_fixture(0)) return 1;
    TASK_CHECK(parse_sf2(read_file(FIXTURE), src));
    TASK_CHECK(src.pmod.size() == 3 && src.pmod[0] != Bytes(10, 0) && src.pmod[1] != Bytes(10, 0)); // fixture has non-empty modulators
    TASK_CHECK(run_subset(FIXTURE, SUBOUT, "0:0,0:1") == 0);
    out = read_file(SUBOUT);
    TASK_CHECK(!out.empty());
    TASK_CHECK(parse_sf2(out, s));

    // presets: kept in phdr order, identities unchanged, bag indices re-closed
    TASK_CHECK(s.phdr.size() == 3);
    TASK_CHECK(rec_name(s.phdr[0]) == "PianoA" && ru16(s.phdr[0], 20) == 0 && ru16(s.phdr[0], 22) == 0 && ru16(s.phdr[0], 24) == 0);
    TASK_CHECK(rec_name(s.phdr[1]) == "PianoB" && ru16(s.phdr[1], 20) == 1 && ru16(s.phdr[1], 22) == 0 && ru16(s.phdr[1], 24) == 2);
    TASK_CHECK(rec_name(s.phdr[2]) == "EOP" && ru16(s.phdr[2], 24) == 4);

    // preset bags: PianoA global+local, PianoB local+local, terminal
    TASK_CHECK(s.pbag.size() == 5);
    TASK_CHECK(check_bag(s.pbag[0], 0, 0) && check_bag(s.pbag[1], 1, 1) && check_bag(s.pbag[2], 2, 1)
            && check_bag(s.pbag[3], 3, 1) && check_bag(s.pbag[4], 4, 2));

    // modulators of the kept bags survive byte-for-byte, plus terminal record
    TASK_CHECK(s.pmod.size() == 3);
    TASK_CHECK(s.pmod[0] == src.pmod[0] && s.pmod[1] == src.pmod[1] && s.pmod[2] == src.pmod[2]);

    // preset generators: global zone kept, instrument refs remapped, terminal kept
    TASK_CHECK(s.pgen.size() == 5);
    TASK_CHECK(check_gen(s.pgen[0], 48, 3));
    TASK_CHECK(check_gen(s.pgen[1], 41, 0) && check_gen(s.pgen[2], 41, 0) && check_gen(s.pgen[3], 41, 1));
    TASK_CHECK(check_gen(s.pgen[4], 0, 0));

    // instruments: shared one kept once, dropped one gone
    TASK_CHECK(s.inst.size() == 3);
    TASK_CHECK(rec_name(s.inst[0]) == "SharedInst" && ru16(s.inst[0], 20) == 0);
    TASK_CHECK(rec_name(s.inst[1]) == "DualInst" && ru16(s.inst[1], 20) == 2);
    TASK_CHECK(rec_name(s.inst[2]) == "EOI" && ru16(s.inst[2], 20) == 5);

    // instrument bags: global+local of SharedInst, global+2 locals of DualInst, terminal
    TASK_CHECK(s.ibag.size() == 6);
    TASK_CHECK(check_bag(s.ibag[0], 0, 0) && check_bag(s.ibag[1], 1, 1) && check_bag(s.ibag[2], 3, 1)
            && check_bag(s.ibag[3], 4, 2) && check_bag(s.ibag[4], 7, 2) && check_bag(s.ibag[5], 10, 2));

    // instrument modulators survive byte-for-byte
    TASK_CHECK(s.imod.size() == 3);
    TASK_CHECK(s.imod[0] == src.imod[0] && s.imod[1] == src.imod[1] && s.imod[2] == src.imod[2]);

    // instrument generators: globals, key ranges, loop modes and remapped sample ids
    TASK_CHECK(s.igen.size() == 11);
    TASK_CHECK(check_gen(s.igen[0], 48, 5) && check_gen(s.igen[1], 43, 0 | (127 << 8)) && check_gen(s.igen[2], 53, 0));
    TASK_CHECK(check_gen(s.igen[3], 48, 2) && check_gen(s.igen[4], 43, 0 | (59 << 8)) && check_gen(s.igen[5], 54, 1));
    TASK_CHECK(check_gen(s.igen[6], 53, 1) && check_gen(s.igen[7], 43, 60 | (127 << 8)) && check_gen(s.igen[8], 54, 1));
    TASK_CHECK(check_gen(s.igen[9], 53, 2) && check_gen(s.igen[10], 0, 0));

    // samples: UnusedE dropped, linked RightD kept via sampleLink, addresses translated
    TASK_CHECK(s.shdr.size() == 5);
    TASK_CHECK(check_shdr(s.shdr[0], "SineA", 0, 100, 10, 90, 22050, 60, 0, 1));
    TASK_CHECK(check_shdr(s.shdr[1], "SineB", 120, 220, 130, 210, 22050, 60, 0, 1));
    TASK_CHECK(check_shdr(s.shdr[2], "LeftC", 266, 366, 276, 356, 22050, 60, 3, 4));
    TASK_CHECK(check_shdr(s.shdr[3], "RightD", 412, 512, 422, 502, 22050, 60, 2, 2));
    TASK_CHECK(check_shdr(s.shdr[4], "EOS", 558, 558, 558, 558, 0, 0, 0, 0));

    // INFO and unknown chunks survive verbatim, pad included
    TASK_CHECK(!src.info_raw.empty() && s.info_raw == src.info_raw);
    TASK_CHECK(!src.xtra_raw.empty() && s.xtra_raw == src.xtra_raw);
    TASK_CHECK(!src.xtra_pdta.empty() && s.xtra_pdta == src.xtra_pdta);
    TASK_CHECK(!src.xtr2_sdta.empty() && s.xtr2_sdta == src.xtr2_sdta);
    return 0;
}

static int case_closure_dedupe_and_order(void)
{
    if (write_fixture(0)) return 1;
    TASK_CHECK(run_subset(FIXTURE, ".task-build/sub_a.sf2", "0:0,0:1") == 0);
    // repeated selections merge into one, output order follows phdr not the list
    TASK_CHECK(run_subset(FIXTURE, ".task-build/sub_b.sf2", "0:1,0:0,0:1") == 0);
    TASK_CHECK(read_file(".task-build/sub_a.sf2") == read_file(".task-build/sub_b.sf2"));
    return 0;
}

static int case_closure_keep_all(void)
{
    Sf2 s;
    if (write_fixture(0)) return 1;
    TASK_CHECK(run_subset(FIXTURE, SUBOUT, "0:0,0:1,1:0") == 0);
    TASK_CHECK(parse_sf2(read_file(SUBOUT), s));
    TASK_CHECK(s.phdr.size() == 4 && s.inst.size() == 4 && s.shdr.size() == 6);
    TASK_CHECK(rec_name(s.phdr[3]) == "EOP" && ru16(s.phdr[3], 24) == 5);
    TASK_CHECK(rec_name(s.inst[2]) == "DroppedInst" && ru16(s.inst[2], 20) == 5);
    TASK_CHECK(rec_name(s.inst[3]) == "EOI" && ru16(s.inst[3], 20) == 6);
    // all generators/modulators of the source survive when everything is kept
    TASK_CHECK(s.pgen.size() == 6 && s.pmod.size() == 3 && s.igen.size() == 12 && s.imod.size() == 3);
    TASK_CHECK(check_gen(s.igen[10], 53, 4)); // DroppedInst now points at remapped UnusedE
    // UnusedE lands after the other intervals, EOS covers the whole new pool
    TASK_CHECK(check_shdr(s.shdr[4], "UnusedE", 558, 658, 568, 648, 22050, 60, 0, 1));
    TASK_CHECK(check_shdr(s.shdr[5], "EOS", 704, 704, 704, 704, 0, 0, 0, 0));
    return 0;
}

static int case_closure_invalid_inputs(void)
{
    static const char* bad_lists[] = { "", "0", "0:1,", "x:1", "0:1:2", "65536:0", "5:5", "0:2", ",0:1" };
    Bytes sentinel;
    unsigned i;
    if (write_fixture(0)) return 1;
    sentinel.push_back(0xAA); sentinel.push_back(0x55); sentinel.push_back(0x10);
    for (i = 0; i < sizeof(bad_lists) / sizeof(bad_lists[0]); i++)
    {
        TASK_CHECK(write_file(SUBOUT, sentinel));
        TASK_CHECK(run_subset(FIXTURE, SUBOUT, bad_lists[i]) != 0);
        TASK_CHECK(read_file(SUBOUT) == sentinel); // failed runs must not touch the target
    }
    // missing list argument entirely
    TASK_CHECK(write_file(SUBOUT, sentinel));
    TASK_CHECK(run_tool("--subset " FIXTURE " " SUBOUT) != 0);
    TASK_CHECK(read_file(SUBOUT) == sentinel);
    // malformed fonts are rejected without touching the target
    {
        static const int bad_flags[] = { FIX_SM24, FIX_SMPO, FIX_ROM, FIX_BADLINK, FIX_PAST_SMPL, FIX_BAG_DISORDER, FIX_BAD_INSTREF };
        for (i = 0; i < sizeof(bad_flags) / sizeof(bad_flags[0]); i++)
        {
            if (write_fixture(bad_flags[i])) return 1;
            TASK_CHECK(write_file(SUBOUT, sentinel));
            TASK_CHECK(run_subset(FIXTURE, SUBOUT, "0:0") != 0);
            TASK_CHECK(read_file(SUBOUT) == sentinel);
        }
    }
    // missing input file
    TASK_CHECK(write_file(SUBOUT, sentinel));
    TASK_CHECK(run_subset(".task-build/sub_no_such.sf2", SUBOUT, "0:0") != 0);
    TASK_CHECK(read_file(SUBOUT) == sentinel);
    return 0;
}

/* ------------------------------------------------------------------ */
/* samples group                                                      */
/* ------------------------------------------------------------------ */

static int case_samples_pool_bytes(void)
{
    Sf2 src, s;
    Bytes expect;
    if (write_fixture(0)) return 1;
    TASK_CHECK(parse_sf2(read_file(FIXTURE), src));
    TASK_CHECK(src.smpl.size() == FIX_POOL_SIZE * 2);
    TASK_CHECK(run_subset(FIXTURE, SUBOUT, "0:0,0:1") == 0);
    TASK_CHECK(parse_sf2(read_file(SUBOUT), s));

    // merged intervals, each copied exactly once:
    //   [0,266) from SineA+SineB (overlap -> shared PCM copied once), then LeftC, then RightD
    expect.insert(expect.end(), src.smpl.begin() + 0, src.smpl.begin() + 266 * 2);
    expect.insert(expect.end(), src.smpl.begin() + 1000 * 2, src.smpl.begin() + 1146 * 2);
    expect.insert(expect.end(), src.smpl.begin() + 2000 * 2, src.smpl.begin() + 2146 * 2);
    TASK_CHECK(expect.size() == 558 * 2);
    TASK_CHECK(s.smpl.size() == expect.size());
    TASK_CHECK(s.smpl == expect);

    // guard points right after each sample end came along
    TASK_CHECK(memcmp(&s.smpl[100 * 2], &src.smpl[100 * 2], 46 * 2) == 0); // SineA guard
    TASK_CHECK(memcmp(&s.smpl[366 * 2], &src.smpl[1100 * 2], 46 * 2) == 0); // LeftC guard
    TASK_CHECK(memcmp(&s.smpl[512 * 2], &src.smpl[2100 * 2], 46 * 2) == 0); // RightD guard
    // the SineA/SineB overlap region exists exactly once in the new pool
    TASK_CHECK(memcmp(&s.smpl[120 * 2], &src.smpl[120 * 2], 26 * 2) == 0);
    TASK_CHECK(s.smpl.size() == (266 + 146 + 146) * 2);
    return 0;
}

static int case_samples_container(void)
{
    Sf2 s;
    Bytes out;
    size_t off, end;
    int chunks = 0;
    if (write_fixture(0)) return 1;
    TASK_CHECK(run_subset(FIXTURE, SUBOUT, "0:0,0:1") == 0);
    out = read_file(SUBOUT);
    TASK_CHECK(parse_sf2(out, s)); // strict parse already validated all chunk sizes and pads

    // RIFF size covers the whole file, file size stays even
    TASK_CHECK(out.size() % 2 == 0);
    TASK_CHECK(ru32(out, 4) == out.size() - 8);

    // top-level chunks tile the RIFF payload exactly (pad included)
    off = 12; end = out.size();
    while (off < end)
    {
        unsigned size;
        TASK_CHECK(end - off >= 8);
        size = ru32(out, off + 4);
        TASK_CHECK((size_t)size + 8 + (size & 1) <= end - off);
        off += 8 + size + (size & 1);
        chunks++;
    }
    TASK_CHECK(off == end && chunks == 4); // INFO, sdta, pdta, Xtra

    // the unknown odd chunk sits at the very end with its pad byte
    TASK_CHECK(out.size() >= 14 && !memcmp(&out[out.size() - 14], "Xtra", 4));
    TASK_CHECK(ru32(out, out.size() - 10) == 5);
    TASK_CHECK(out[out.size() - 1] == 0);

    // EOS points at the end of the new pool and the smpl chunk matches it
    TASK_CHECK(s.shdr.size() == 5 && ru32(s.shdr[4], 20) * 2 == s.smpl.size());
    return 0;
}

static int case_samples_old_commands(void)
{
    Sf2 src;
    Bytes wav, wav2;
    if (write_fixture(0)) return 1;
    TASK_CHECK(parse_sf2(read_file(FIXTURE), src));

    // the classic PCM dump still works
    TASK_CHECK(run_tool(FIXTURE " .task-build/sub_dump.wav") == 0);
    wav = read_file(".task-build/sub_dump.wav");
    TASK_CHECK(wav.size() == 44 + (size_t)FIX_POOL_SIZE * 2);
    TASK_CHECK(!memcmp(&wav[0], "RIFF", 4) && !memcmp(&wav[8], "WAVE", 4) && !memcmp(&wav[12], "fmt ", 4));
    TASK_CHECK(!memcmp(&wav[36], "data", 4) && ru32(wav, 40) == FIX_POOL_SIZE * 2);
    TASK_CHECK(ru32(wav, 4) == wav.size() - 8);
    TASK_CHECK(memcmp(&wav[44], src.smpl.data(), src.smpl.size()) == 0);

    // the classic sample replacement still works and round-trips
    TASK_CHECK(run_tool(FIXTURE " .task-build/sub_dump.wav .task-build/sub_rebuilt.sf2") == 0);
    TASK_CHECK(run_tool(".task-build/sub_rebuilt.sf2 .task-build/sub_dump2.wav") == 0);
    wav2 = read_file(".task-build/sub_dump2.wav");
    TASK_CHECK(wav2 == wav);
    return 0;
}

/* ------------------------------------------------------------------ */
/* roundtrip group                                                    */
/* ------------------------------------------------------------------ */

static int render_plan_compare(const Bytes& src_bytes, const Bytes& sub_bytes, int bank, int preset, int key)
{
    tsf* a = tsf_load_memory(src_bytes.data(), (int)src_bytes.size());
    tsf* b = tsf_load_memory(sub_bytes.data(), (int)sub_bytes.size());
    float ba[173], bb[173];
    double energy = 0;
    int ia, ib, blk, i;
    TASK_CHECK(a != NULL);
    TASK_CHECK(b != NULL);
    tsf_set_output(a, TSF_MONO, 22050, 0);
    tsf_set_output(b, TSF_MONO, 22050, 0);
    ia = tsf_get_presetindex(a, bank, preset);
    ib = tsf_get_presetindex(b, bank, preset);
    TASK_CHECK(ia >= 0 && ib >= 0);
    TASK_CHECK(tsf_note_on(a, ia, key, 1.0f) == 1);
    TASK_CHECK(tsf_note_on(b, ib, key, 1.0f) == 1);
    // 6*173 = 1038 frames at 1:1 playback: crosses the (shifted) loop seams
    // of SineA (10..90) and LeftC (1010..1090 -> 276..356) many times
    for (blk = 0; blk < 6; blk++)
    {
        tsf_render_float(a, ba, 173, 0);
        tsf_render_float(b, bb, 173, 0);
        for (i = 0; i < 173; i++)
        {
            TASK_CHECK(ba[i] == bb[i]); // sample-exact comparison
            energy += (double)ba[i] * (double)ba[i];
        }
    }
    tsf_note_off(a, ia, key);
    tsf_note_off(b, ib, key);
    for (blk = 0; blk < 2; blk++)
    {
        tsf_render_float(a, ba, 173, 0);
        tsf_render_float(b, bb, 173, 0);
        for (i = 0; i < 173; i++) TASK_CHECK(ba[i] == bb[i]);
    }
    TASK_CHECK(energy > 0.001); // make sure something actually played
    tsf_close(a);
    tsf_close(b);
    return 0;
}

static int case_roundtrip_subset_render(void)
{
    Bytes src_bytes, sub_bytes;
    tsf* sub;
    if (write_fixture(0)) return 1;
    src_bytes = read_file(FIXTURE);
    TASK_CHECK(run_subset(FIXTURE, SUBOUT, "0:0,0:1") == 0);
    sub_bytes = read_file(SUBOUT);
    TASK_CHECK(!sub_bytes.empty());

    // the dropped preset is gone from the subset
    sub = tsf_load_memory(sub_bytes.data(), (int)sub_bytes.size());
    TASK_CHECK(sub != NULL);
    TASK_CHECK(tsf_get_presetcount(sub) == 2);
    TASK_CHECK(tsf_get_presetindex(sub, 1, 0) == -1);
    tsf_close(sub);

    // key 60: SineA (loop 10..90) + LeftC (loop shifted to 276..356)
    if (render_plan_compare(src_bytes, sub_bytes, 0, 0, 60)) return 1;
    if (render_plan_compare(src_bytes, sub_bytes, 0, 1, 60)) return 1;
    // key 30: SineA + SineB, key 100: SineA + LeftC far above its root key
    if (render_plan_compare(src_bytes, sub_bytes, 0, 1, 30)) return 1;
    if (render_plan_compare(src_bytes, sub_bytes, 0, 1, 100)) return 1;
    return 0;
}

static int case_roundtrip_keep_all_render(void)
{
    Bytes src_bytes, sub_bytes;
    tsf* sub;
    if (write_fixture(0)) return 1;
    src_bytes = read_file(FIXTURE);
    TASK_CHECK(run_subset(FIXTURE, SUBOUT, "1:0,0:0,0:1") == 0);
    sub_bytes = read_file(SUBOUT);
    sub = tsf_load_memory(sub_bytes.data(), (int)sub_bytes.size());
    TASK_CHECK(sub != NULL);
    TASK_CHECK(tsf_get_presetcount(sub) == 3);
    tsf_close(sub);

    // UnusedE (one-shot, no loop) through the kept Dropped preset
    if (render_plan_compare(src_bytes, sub_bytes, 1, 0, 60)) return 1;
    if (render_plan_compare(src_bytes, sub_bytes, 0, 0, 45)) return 1;
    return 0;
}

/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
    { "closure", "indices", case_closure_indices },
    { "closure", "dedupe_and_order", case_closure_dedupe_and_order },
    { "closure", "keep_all", case_closure_keep_all },
    { "closure", "invalid_inputs", case_closure_invalid_inputs },
    { "samples", "pool_bytes", case_samples_pool_bytes },
    { "samples", "container", case_samples_container },
    { "samples", "old_commands", case_samples_old_commands },
    { "roundtrip", "subset_render", case_roundtrip_subset_render },
    { "roundtrip", "keep_all_render", case_roundtrip_keep_all_render },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
