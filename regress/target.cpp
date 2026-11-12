// Model-based regression tests for XSD identity constraints
// (xs:key / xs:unique / xs:keyref) validated through libxml2.
//
// A bounded logical data model is generated first (fixed seed 1383);
// the schema and the XML instances are derived from that model.  The
// expected verdict is computed on the model itself, using integer
// values and whitespace-collapsed token values -- never on XML text
// and never through libxml2 type-conversion helpers.  Every model is
// validated through xmlSchemaValidateDoc and through
// xmlSchemaValidateStream fed by a short-read callback (whole buffer
// at once, 1 byte, and fixed uneven chunks).  Structured errors are
// recorded and classified so that parse failures or internal/API
// errors can never masquerade as the invalid data the model predicts.

#include "cases.hpp"

#include <libxml/parser.h>
#include <libxml/xmlIO.h>
#include <libxml/xmlerror.h>
#include <libxml/xmlschemas.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kSeed = 1383;
constexpr int kModelCount = 136; // 17 scenarios x 8 rounds, bounded size
constexpr int kRetParseFail = -1000;

const char* kTargetNs = "urn:idc:Z1383";
const char* kOtherNs = "urn:idc:other";

// ---------------------------------------------------------------------------
// deterministic PRNG (splitmix64)
// ---------------------------------------------------------------------------
struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed) {}
    std::uint64_t next() {
        std::uint64_t z = (s += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    int below(int n) { return (int)(next() % (std::uint64_t)n); }
    bool chance(int pct) { return below(100) < pct; }
};

// ---------------------------------------------------------------------------
// logical model
// ---------------------------------------------------------------------------
struct Tuple {
    bool hasA = true;      // first field: xs:integer
    long a = 0;
    bool hasB = true;      // second field: xs:token (logical, collapsed value)
    std::string b;
};
struct Item { bool isRef = false; Tuple t; };
struct Batch { std::vector<Item> items; bool interference = false; };
enum class SchemaKind : int { KeyOnly = 0, UniqueOnly = 1, Both = 2 };

struct Model {
    int index = 0;
    std::string scene;
    SchemaKind kind = SchemaKind::Both;
    int prefixMode = 0; // 0: default xmlns, 1: prefix "p", 2: prefix "zq"
    std::vector<Batch> batches;
};

// XML 1.0 whitespace, collapsed the way xs:token requires.  Implemented
// here on purpose: expectations must not come from libxml2 helpers.
static bool isWs(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}
static std::string wsCollapse(const std::string& in) {
    std::string out;
    bool started = false, pending = false;
    for (char c : in) {
        if (isWs(c)) { if (started) pending = true; continue; }
        if (pending) { out += ' '; pending = false; }
        out += c; started = true;
    }
    return out;
}

using Key = std::pair<long, std::string>;

struct BatchExpect {
    bool keyOk = true;     // xs:key: all fields present, no duplicates
    bool uniqueOk = true;  // xs:unique: incomplete tuples excluded
    bool keyrefOk = true;  // xs:keyref: complete tuples must resolve in scope
    bool valid() const { return keyOk && uniqueOk && keyrefOk; }
};

static BatchExpect expectBatch(const Batch& b) {
    BatchExpect e;
    std::set<Key> table; // target table of this batch's scope only
    for (const Item& it : b.items) {
        if (it.isRef) continue;
        if (it.t.hasA && it.t.hasB) {
            if (!table.insert(Key(it.t.a, it.t.b)).second) {
                e.keyOk = false;    // duplicate complete tuple: key invalid
                e.uniqueOk = false; // ... and unique invalid too
            }
        } else {
            e.keyOk = false;        // key requires every field
            // unique simply excludes the incomplete tuple
        }
    }
    for (const Item& it : b.items) {
        if (!it.isRef) continue;
        if (it.t.hasA && it.t.hasB) {
            if (!table.count(Key(it.t.a, it.t.b)))
                e.keyrefOk = false; // orphan reference inside this scope
        }
        // incomplete references do not participate in matching
    }
    return e;
}

static bool expectModel(const Model& m) {
    for (const Batch& b : m.batches) {
        BatchExpect e = expectBatch(b);
        // only the constraints actually present in the schema are enforced
        if (m.kind != SchemaKind::UniqueOnly && !e.keyOk) return false;
        if (m.kind != SchemaKind::KeyOnly && !e.uniqueOk) return false;
        if (!e.keyrefOk) return false;
    }
    return true;
}

static std::string dumpModel(const Model& m) {
    std::ostringstream o;
    o << "kind=" << (m.kind == SchemaKind::KeyOnly ? "key" :
                     m.kind == SchemaKind::UniqueOnly ? "unique" : "both")
      << " prefixMode=" << m.prefixMode
      << " batches=" << m.batches.size();
    for (std::size_t i = 0; i < m.batches.size(); i++) {
        o << "\n batch[" << i << "] interference=" << m.batches[i].interference;
        for (const Item& it : m.batches[i].items) {
            o << "\n  " << (it.isRef ? "ref" : "rec") << " a=";
            if (it.t.hasA) o << it.t.a; else o << "<absent>";
            o << " b=";
            if (it.t.hasB) o << "'" << it.t.b << "'"; else o << "<absent>";
        }
    }
    return o.str();
}

// ---------------------------------------------------------------------------
// coverage bookkeeping
// ---------------------------------------------------------------------------
struct Coverage {
    int validModels = 0, invalidModels = 0;
    bool intSpellings[5] = {false, false, false, false, false};
    bool tokenWs = false;
    bool secondFieldDiff = false, swapped = false;
    bool forwardRef = false, orphanRef = false;
    bool emptyBatch = false;
    bool crossBatchRepeat = false, crossBatchBorrow = false;
    bool missingKeyInvalid = false, missingUniqueValid = false;
    bool missingRefSkipped = false;
    bool dupCompleteKey = false, dupCompleteUnique = false;
    bool keyFalseUniqueTrueBatch = false;
    bool interference = false;
    bool defaultNs = false, prefixedNs = false;
    bool reuseTrio = false;
};

// ---------------------------------------------------------------------------
// XML lexical spellings: same logical value, different legal surface forms
// ---------------------------------------------------------------------------
static std::string spellInt(long v, int variant) {
    std::string c = std::to_string(v);
    switch (variant) {
        case 1: return v >= 0 ? "+" + c : c;
        case 2: return v < 0 ? "-0" + c.substr(1) : "0" + c;
        case 3: return " " + c + " ";
        case 4: return "\t" + c + "\n";
        default: return c;
    }
}

static std::string spellToken(const std::string& logical, int variant) {
    static const char* seps[] = {" ", "  ", " \t ", "\n", " \n  ", "   "};
    static const char* pre[]  = {"", " ", "\t", "", "  ", " "};
    static const char* post[] = {"", " ", "", "\n", "  ", "\t"};
    if (logical.empty()) {
        static const char* empty[] = {"", " ", "  ", "\t", " \n", "   "};
        return empty[variant % 6];
    }
    std::string out = pre[variant % 6];
    bool first = true;
    std::string cur;
    for (char c : logical) {
        if (c == ' ') {
            out += cur; cur.clear();
            out += seps[variant % 6];
            first = false;
        } else {
            cur += c;
        }
    }
    (void)first;
    out += cur;
    out += post[variant % 6];
    return out;
}

// ---------------------------------------------------------------------------
// schema generation: fixed selector/field prefixes ("t"), one constraint
// instance per batch element
// ---------------------------------------------------------------------------
static std::string genSchema(SchemaKind kind) {
    std::string s;
    s += "<xs:schema xmlns:xs='http://www.w3.org/2001/XMLSchema'";
    s += std::string(" xmlns:t='") + kTargetNs + "'";
    s += std::string(" targetNamespace='") + kTargetNs + "'";
    s += " elementFormDefault='qualified'>";
    s += "<xs:element name='collection'><xs:complexType><xs:sequence>";
    s += "<xs:element name='batch' maxOccurs='unbounded'><xs:complexType>";
    s += "<xs:choice minOccurs='0' maxOccurs='unbounded'>";
    for (const char* e : {"rec", "ref"}) {
        s += "<xs:element name='"; s += e;
        s += "'><xs:complexType><xs:sequence>";
        s += "<xs:element name='a' type='xs:integer' minOccurs='0'/>";
        s += "<xs:element name='b' type='xs:token' minOccurs='0'/>";
        s += "<xs:any namespace='##other' processContents='lax'"
             " minOccurs='0' maxOccurs='unbounded'/>";
        s += "</xs:sequence></xs:complexType></xs:element>";
    }
    s += "<xs:any namespace='##other' processContents='lax'/>";
    s += "</xs:choice></xs:complexType>";
    if (kind != SchemaKind::UniqueOnly)
        s += "<xs:key name='zk'><xs:selector xpath='t:rec'/>"
             "<xs:field xpath='t:a'/><xs:field xpath='t:b'/></xs:key>";
    if (kind != SchemaKind::KeyOnly)
        s += "<xs:unique name='zu'><xs:selector xpath='t:rec'/>"
             "<xs:field xpath='t:a'/><xs:field xpath='t:b'/></xs:unique>";
    s += "<xs:keyref name='zkr' refer='t:";
    s += (kind == SchemaKind::UniqueOnly) ? "zu" : "zk";
    s += "'><xs:selector xpath='t:ref'/>"
         "<xs:field xpath='t:a'/><xs:field xpath='t:b'/></xs:keyref>";
    s += "</xs:element></xs:sequence></xs:complexType></xs:element></xs:schema>";
    return s;
}

// ---------------------------------------------------------------------------
// XML generation: prefixes vary per model; interference nodes share local
// names but live in a foreign namespace
// ---------------------------------------------------------------------------
static std::string genXml(const Model& m, Coverage& cov) {
    std::string p, decl;
    switch (m.prefixMode) {
        case 0:
            decl = std::string(" xmlns='") + kTargetNs + "'";
            cov.defaultNs = true;
            break;
        case 1:
            p = "p:";
            decl = std::string(" xmlns:p='") + kTargetNs + "'";
            cov.prefixedNs = true;
            break;
        default:
            p = "zq:";
            decl = std::string(" xmlns:zq='") + kTargetNs + "'";
            cov.prefixedNs = true;
            break;
    }
    std::string x = "<" + p + "collection" + decl + ">";
    int bi = 0;
    for (const Batch& b : m.batches) {
        x += "<" + p + "batch>";
        int ii = 0;
        for (const Item& it : b.items) {
            std::string e = p + (it.isRef ? "ref" : "rec");
            x += "<" + e + ">";
            if (it.t.hasA) {
                int var = (m.index + bi + ii) % 5;
                cov.intSpellings[var] = true;
                x += "<" + p + "a>" + spellInt(it.t.a, var) + "</" + p + "a>";
            }
            if (it.t.hasB) {
                int var = (m.index * 2 + bi + ii * 3) % 6;
                if (var != 0) cov.tokenWs = true;
                x += "<" + p + "b>" + spellToken(it.t.b, var) + "</" + p + "b>";
            }
            if (b.interference) {
                // same local names, foreign namespace: must be ignored
                x += std::string("<n:a xmlns:n='") + kOtherNs + "'>777</n:a>";
                x += std::string("<n:b xmlns:n='") + kOtherNs + "'>pad</n:b>";
            }
            x += "</" + e + ">";
            ii++;
        }
        if (b.interference)
            x += std::string("<n:rec xmlns:n='") + kOtherNs +
                 "'><n:a>1</n:a></n:rec>";
        x += "</" + p + "batch>";
        bi++;
    }
    x += "</" + p + "collection>";
    return x;
}

// ---------------------------------------------------------------------------
// model generation: 17 scenarios, bounded size
// ---------------------------------------------------------------------------
static const std::vector<std::string>& tokPool() {
    static const std::vector<std::string> pool =
        {"x", "y", "zz", "a b", "k", "w", "t u", "q0"};
    return pool;
}

struct ModelResult {
    Model m;
    bool expected = true;
    std::string xml;
};

static Item makeRec(long a, std::string b) {
    Item it; it.isRef = false;
    it.t.hasA = true; it.t.a = a; it.t.hasB = true; it.t.b = std::move(b);
    return it;
}
static Item makeRef(long a, std::string b) {
    Item it = makeRec(a, std::move(b)); it.isRef = true; return it;
}

static ModelResult genModelResult(int idx, Rng& r, long& ks, Coverage& cov) {
    Model m;
    m.index = idx;
    m.prefixMode = idx % 3; // every prefix mode is guaranteed to appear
    auto kindAny = [&]() { return (SchemaKind)r.below(3); };

    auto recNoB = [&](long a) {
        Item it = makeRec(a, ""); it.t.hasB = false; return it;
    };
    auto refNoA = [&](std::string b) {
        Item it = makeRef(0, std::move(b)); it.t.hasA = false; return it;
    };
    auto filler = [&](bool withRef) {
        Batch b;
        int n = 1 + r.below(2);
        long a0 = ks;
        std::string t0;
        for (int i = 0; i < n; i++) {
            std::string tk = tokPool()[(std::size_t)ks % tokPool().size()];
            if (i == 0) t0 = tk;
            b.items.push_back(makeRec(ks++, tk));
        }
        if (withRef) b.items.push_back(makeRef(a0, t0));
        return b;
    };

    switch (idx % 17) {
        case 0:
            m.scene = "valid_basic";
            m.kind = kindAny();
            for (int i = 0, nb = 1 + r.below(3); i < nb; i++)
                m.batches.push_back(filler(true));
            break;
        case 1: {
            // same tuple twice; the XML spells the repeated integer and
            // the repeated token differently -> typed duplicate
            m.scene = "dupkey_spelling";
            m.kind = r.chance(50) ? SchemaKind::KeyOnly : SchemaKind::Both;
            Batch b;
            b.items.push_back(makeRec(7, "x"));
            b.items.push_back(makeRec(7, "x"));
            m.batches.push_back(b);
            if (r.chance(50)) m.batches.push_back(filler(true));
            cov.dupCompleteKey = true;
            break;
        }
        case 2: {
            // "007" and "8" are different values: no false duplicate
            m.scene = "typed_distinct_int";
            m.kind = kindAny();
            Batch b;
            b.items.push_back(makeRec(7, "x"));
            b.items.push_back(makeRec(8, "x"));
            b.items.push_back(makeRef(8, "x"));
            m.batches.push_back(b);
            break;
        }
        case 3: {
            // same collapsed token, different raw whitespace -> duplicate
            m.scene = "token_ws_dup";
            m.kind = kindAny();
            Batch b;
            b.items.push_back(makeRec(1, "a b"));
            b.items.push_back(makeRec(1, "a b"));
            m.batches.push_back(b);
            if (r.chance(50)) m.batches.push_back(filler(false));
            break;
        }
        case 4: {
            m.scene = "token_distinct";
            m.kind = kindAny();
            Batch b;
            b.items.push_back(makeRec(1, "ab"));
            b.items.push_back(makeRec(1, "a b"));
            m.batches.push_back(b);
            break;
        }
        case 5: {
            m.scene = "second_field_diff";
            m.kind = kindAny();
            Batch b;
            b.items.push_back(makeRec(1, "x"));
            b.items.push_back(makeRec(1, "y"));
            m.batches.push_back(b);
            cov.secondFieldDiff = true;
            break;
        }
        case 6: {
            m.scene = "swapped_fields";
            m.kind = kindAny();
            Batch b;
            b.items.push_back(makeRec(1, "2"));
            b.items.push_back(makeRec(2, "1"));
            m.batches.push_back(b);
            cov.swapped = true;
            break;
        }
        case 7: {
            // reference appears before the record that defines the key
            m.scene = "forward_keyref";
            m.kind = kindAny();
            Batch b;
            b.items.push_back(makeRef(5, "k"));
            b.items.push_back(makeRec(5, "k"));
            m.batches.push_back(b);
            if (r.chance(50)) m.batches.push_back(filler(false));
            cov.forwardRef = true;
            break;
        }
        case 8: {
            m.scene = "orphan_keyref";
            m.kind = kindAny();
            Batch b;
            b.items.push_back(makeRec(1, "x"));
            b.items.push_back(makeRef(9, "zz"));
            m.batches.push_back(b);
            if (r.chance(50)) m.batches.push_back(filler(true));
            cov.orphanRef = true;
            break;
        }
        case 9: {
            m.scene = "empty_batch";
            m.kind = kindAny();
            Batch empty;
            empty.interference = true; // only foreign-namespace nodes
            m.batches.push_back(empty);
            m.batches.push_back(filler(true));
            cov.emptyBatch = true;
            break;
        }
        case 10: {
            // same key in two batches: separate scopes, valid
            m.scene = "cross_batch_repeat";
            m.kind = kindAny();
            Batch b1, b2;
            b1.items.push_back(makeRec(3, "k"));
            b2.items.push_back(makeRec(3, "k"));
            b2.items.push_back(makeRef(3, "k"));
            m.batches.push_back(b1);
            m.batches.push_back(b2);
            cov.crossBatchRepeat = true;
            break;
        }
        case 11: {
            // reference must not borrow a key from a sibling batch
            m.scene = "cross_batch_borrow";
            m.kind = kindAny();
            Batch b1, b2;
            b1.items.push_back(makeRec(3, "k"));
            b2.items.push_back(makeRef(3, "k"));
            m.batches.push_back(b1);
            m.batches.push_back(b2);
            cov.crossBatchBorrow = true;
            break;
        }
        case 12: {
            // key with a missing field is invalid (unique would pass)
            m.scene = "missing_key";
            m.kind = r.chance(50) ? SchemaKind::KeyOnly : SchemaKind::Both;
            Batch b;
            b.items.push_back(makeRec(1, "x"));
            b.items.push_back(recNoB(2));
            b.items.push_back(makeRef(1, "x"));
            m.batches.push_back(b);
            cov.missingKeyInvalid = true;
            break;
        }
        case 13: {
            // unique excludes the incomplete tuple: valid
            m.scene = "missing_unique";
            m.kind = SchemaKind::UniqueOnly;
            Batch b;
            b.items.push_back(makeRec(1, "x"));
            b.items.push_back(recNoB(2));
            b.items.push_back(makeRef(1, "x"));
            m.batches.push_back(b);
            cov.missingUniqueValid = true;
            break;
        }
        case 14: {
            // duplicate complete tuples: unique invalid too
            m.scene = "dup_unique";
            m.kind = SchemaKind::UniqueOnly;
            Batch b;
            b.items.push_back(makeRec(4, "d"));
            b.items.push_back(makeRec(4, "d"));
            m.batches.push_back(b);
            cov.dupCompleteUnique = true;
            break;
        }
        case 15: {
            // incomplete reference does not participate in matching
            m.scene = "missing_ref";
            m.kind = kindAny();
            Batch b;
            b.items.push_back(makeRec(1, "x"));
            b.items.push_back(refNoA("x"));
            m.batches.push_back(b);
            cov.missingRefSkipped = true;
            break;
        }
        default: {
            // foreign-namespace nodes reusing local names a/b/rec
            m.scene = "interference";
            m.kind = kindAny();
            Batch b;
            b.interference = true;
            b.items.push_back(makeRec(4, "w"));
            b.items.push_back(makeRef(4, "w"));
            m.batches.push_back(b);
            if (r.chance(50)) m.batches.push_back(filler(false));
            cov.interference = true;
            break;
        }
    }

    ModelResult mr;
    mr.m = m;
    mr.expected = expectModel(m);
    mr.xml = genXml(m, cov);
    if (mr.expected) cov.validModels++; else cov.invalidModels++;
    for (const Batch& b : m.batches) {
        BatchExpect be = expectBatch(b);
        if (!be.keyOk && be.uniqueOk) cov.keyFalseUniqueTrueBatch = true;
    }
    return mr;
}

// ---------------------------------------------------------------------------
// structured error capture and classification
// ---------------------------------------------------------------------------
struct ErrRec { int domain; int code; std::string message; };
struct Sink { std::vector<ErrRec> errs; };

static void onStructError(void* user, const xmlError* e) {
    Sink* s = static_cast<Sink*>(user);
    ErrRec r;
    r.domain = e->domain;
    r.code = e->code;
    r.message = e->message ? e->message : "";
    s->errs.push_back(std::move(r));
}

static bool isIdcError(const ErrRec& e) {
    return e.domain == XML_FROM_SCHEMASV && e.code == XML_SCHEMAV_CVC_IDC;
}

enum class Cat { Valid, IdcInvalid, Unparseable, Internal };

static const char* catName(Cat c) {
    switch (c) {
        case Cat::Valid: return "valid";
        case Cat::IdcInvalid: return "idc-invalid";
        case Cat::Unparseable: return "unparseable";
        default: return "internal";
    }
}

static Cat categorize(int ret, const std::vector<ErrRec>& errs) {
    if (ret == kRetParseFail) return Cat::Unparseable;
    if (ret < 0) return Cat::Internal;
    for (const ErrRec& e : errs)
        if (e.domain == XML_FROM_PARSER) return Cat::Unparseable;
    if (ret == 0) return Cat::Valid;
    if (!errs.empty()) {
        bool allIdc = true;
        for (const ErrRec& e : errs)
            if (!isIdcError(e)) { allIdc = false; break; }
        if (allIdc) return Cat::IdcInvalid;
    }
    return Cat::Internal;
}

struct Outcome {
    int ret;
    Cat cat;
    std::vector<ErrRec> errs;
};

// ---------------------------------------------------------------------------
// validation entries
// ---------------------------------------------------------------------------
static Outcome runDom(xmlSchemaValidCtxtPtr vc, const std::string& xml) {
    Sink vsink, psink;
    xmlSchemaSetValidStructuredErrors(vc, onStructError, &vsink);
    xmlSetStructuredErrorFunc(&psink, onStructError);
    xmlDocPtr doc = xmlReadMemory(xml.data(), (int)xml.size(),
                                  "model.xml", NULL, XML_PARSE_NONET);
    xmlSetStructuredErrorFunc(NULL, NULL);
    if (doc == NULL)
        return {kRetParseFail, Cat::Unparseable, psink.errs};
    int ret = xmlSchemaValidateDoc(vc, doc);
    xmlFreeDoc(doc);
    return {ret, categorize(ret, vsink.errs), std::move(vsink.errs)};
}

struct Reader {
    const char* data;
    std::size_t len, pos;
    const std::vector<std::size_t>* chunks;
    std::size_t ci;
};

static int readCb(void* v, char* buf, int len) {
    Reader& r = *static_cast<Reader*>(v);
    if (r.pos >= r.len) return 0;
    std::size_t want = (*r.chunks)[r.ci++ % r.chunks->size()];
    std::size_t n = want;
    if (n > (std::size_t)len) n = (std::size_t)len;
    if (n > r.len - r.pos) n = r.len - r.pos;
    std::memcpy(buf, r.data + r.pos, n);
    r.pos += n;
    return (int)n;
}

static Outcome runStream(xmlSchemaValidCtxtPtr vc, const std::string& xml,
                         const std::vector<std::size_t>& chunks) {
    Reader r{xml.data(), xml.size(), 0, &chunks, 0};
    Sink vsink, psink;
    xmlSchemaSetValidStructuredErrors(vc, onStructError, &vsink);
    xmlSetStructuredErrorFunc(&psink, onStructError);
    xmlParserInputBufferPtr in =
        xmlParserInputBufferCreateIO(readCb, NULL, &r, XML_CHAR_ENCODING_NONE);
    if (in == NULL) {
        xmlSetStructuredErrorFunc(NULL, NULL);
        return {-1, Cat::Internal, {}};
    }
    // The input buffer is adopted by the validator's parser context;
    // it must not be freed here.
    int ret = xmlSchemaValidateStream(vc, in, XML_CHAR_ENCODING_NONE,
                                      NULL, NULL);
    xmlSetStructuredErrorFunc(NULL, NULL);
    std::vector<ErrRec> errs = std::move(vsink.errs);
    errs.insert(errs.end(), psink.errs.begin(), psink.errs.end());
    return {ret, categorize(ret, errs), std::move(errs)};
}

// ---------------------------------------------------------------------------
// diagnostics: seed, model, XML, schema, entry, first differing conclusion
// ---------------------------------------------------------------------------
[[noreturn]] static void failMismatch(const ModelResult& mr,
                                      const std::string& schema,
                                      const char* entry, const Outcome& o) {
    std::ostringstream s;
    s << "seed=" << kSeed << " model#" << mr.m.index
      << " scenario=" << mr.m.scene
      << "\n--- model ---\n" << dumpModel(mr.m)
      << "\n--- xml ---\n" << mr.xml
      << "\n--- schema ---\n" << schema
      << "\nentry=" << entry
      << " expected=" << (mr.expected ? "valid" : "invalid")
      << " got: ret=" << o.ret << " category=" << catName(o.cat);
    if (!o.errs.empty()) {
        s << "\nfirst-error domain=" << o.errs[0].domain
          << " code=" << o.errs[0].code
          << " message=" << o.errs[0].message;
    }
    throw std::runtime_error(s.str());
}

// ---------------------------------------------------------------------------
// the suite: generated once, exercised through every entry
// ---------------------------------------------------------------------------
struct Suite {
    std::vector<ModelResult> results;
    std::string schemaText[3];
    Coverage cov;
    int entryRuns = 0;
    bool ready = false;
};

static void buildSuite(Suite& s) {
    xmlInitParser();

    const SchemaKind kinds[3] =
        {SchemaKind::KeyOnly, SchemaKind::UniqueOnly, SchemaKind::Both};
    xmlSchemaPtr schemas[3] = {NULL, NULL, NULL};
    xmlSchemaValidCtxtPtr vctxt[3][4] = {};

    for (int k = 0; k < 3; k++) {
        s.schemaText[k] = genSchema(kinds[k]);
        Sink psink;
        xmlSchemaParserCtxtPtr pc = xmlSchemaNewMemParserCtxt(
            s.schemaText[k].data(), (int)s.schemaText[k].size());
        if (pc == NULL) throw std::runtime_error("no schema parser ctxt");
        xmlSchemaSetParserStructuredErrors(pc, onStructError, &psink);
        schemas[k] = xmlSchemaParse(pc);
        xmlSchemaFreeParserCtxt(pc);
        if (schemas[k] == NULL) {
            std::string msg = "generated schema does not parse (category "
                "unparseable must not masquerade as model-invalid):";
            for (const ErrRec& e : psink.errs)
                msg += "\n  code=" + std::to_string(e.code) + " " + e.message;
            msg += "\n" + s.schemaText[k];
            throw std::runtime_error(msg);
        }
        for (int e = 0; e < 4; e++) {
            vctxt[k][e] = xmlSchemaNewValidCtxt(schemas[k]);
            if (vctxt[k][e] == NULL)
                throw std::runtime_error("no valid ctxt");
        }
    }

    Rng r(kSeed);
    long ks = 1;
    std::vector<ModelResult> all;
    for (int i = 0; i < kModelCount; i++)
        all.push_back(genModelResult(i, r, ks, s.cov));

    // interleave valid and invalid models so every reused validation
    // context sees valid -> invalid -> valid sequences
    std::vector<int> validIdx, invalidIdx, order;
    for (int i = 0; i < (int)all.size(); i++)
        (all[i].expected ? validIdx : invalidIdx).push_back(i);
    for (std::size_t i = 0; i < validIdx.size() || i < invalidIdx.size(); i++) {
        if (i < validIdx.size()) order.push_back(validIdx[i]);
        if (i < invalidIdx.size()) order.push_back(invalidIdx[i]);
    }

    static const char* entryNames[4] =
        {"dom", "stream-all", "stream-1byte", "stream-chunks"};
    const std::vector<std::vector<std::size_t>> chunkSets = {
        {},
        {std::size_t(1) << 30},      // read everything in one call
        {1},                          // one byte at a time
        {3, 7, 64, 5, 128},           // fixed uneven chunks
    };

    int trio[3][4] = {}; // per-context valid->invalid->valid state machine
    for (int idx : order) {
        ModelResult& mr = all[idx];
        int k = (int)mr.m.kind;
        for (int e = 0; e < 4; e++) {
            Outcome o = (e == 0)
                ? runDom(vctxt[k][e], mr.xml)
                : runStream(vctxt[k][e], mr.xml, chunkSets[e]);
            s.entryRuns++;
            bool ok;
            if (mr.expected)
                ok = (o.ret == 0) && (o.cat == Cat::Valid) && o.errs.empty();
            else
                ok = (o.cat == Cat::IdcInvalid);
            if (!ok)
                failMismatch(mr, s.schemaText[k], entryNames[e], o);
            int& st = trio[k][e];
            if (st == 0 && mr.expected) st = 1;
            else if (st == 1 && !mr.expected) st = 2;
            else if (st == 2 && mr.expected) { st = 1; s.cov.reuseTrio = true; }
        }
    }

    for (int k = 0; k < 3; k++) {
        for (int e = 0; e < 4; e++)
            xmlSchemaFreeValidCtxt(vctxt[k][e]);
        xmlSchemaFree(schemas[k]);
    }
    s.results = std::move(all);
}

static Suite& suite() {
    static Suite s;
    if (!s.ready) {
        buildSuite(s);
        s.ready = true;
    }
    return s;
}

static const ModelResult* findScenario(const Suite& s, const char* name) {
    for (const ModelResult& mr : s.results)
        if (mr.m.scene == name) return &mr;
    return NULL;
}

} // namespace

// ---------------------------------------------------------------------------
// CASE groups
// ---------------------------------------------------------------------------
CASE(typed_tuple_model) {
    Suite& s = suite();
    CHECK(s.results.size() >= 120);
    CHECK(s.cov.validModels > 20);
    CHECK(s.cov.invalidModels > 20);
    // same numeric value appeared in several distinct legal spellings
    for (int v = 0; v < 5; v++) CHECK(s.cov.intSpellings[v]);
    CHECK(spellInt(7, 0) != spellInt(7, 1));
    CHECK(spellInt(7, 1) != spellInt(7, 2));
    // token whitespace collapsing is exercised and matches the model rule
    CHECK(s.cov.tokenWs);
    CHECK(wsCollapse("  a   b ") == "a b");
    CHECK(wsCollapse("\tx\n") == "x");
    CHECK(s.cov.secondFieldDiff);
    CHECK(s.cov.swapped);
    CHECK(s.cov.dupCompleteKey);
    CHECK(s.cov.dupCompleteUnique);
    // every model went through every entry with matching conclusions
    CHECK(s.entryRuns == (int)s.results.size() * 4);
    // validation contexts were reused valid -> invalid -> valid cleanly
    CHECK(s.cov.reuseTrio);
    // model content: the duplicate-pair model really carries one logical
    // tuple twice, and the model verdict is invalid
    const ModelResult* dup = findScenario(s, "dupkey_spelling");
    CHECK(dup != NULL);
    CHECK(dup->m.batches[0].items.size() >= 2);
    CHECK(dup->m.batches[0].items[0].t.a == dup->m.batches[0].items[1].t.a);
    CHECK(dup->m.batches[0].items[0].t.b == dup->m.batches[0].items[1].t.b);
    CHECK(!dup->expected);
    const ModelResult* dist = findScenario(s, "typed_distinct_int");
    CHECK(dist != NULL);
    CHECK(dist->expected);
}

CASE(scope_and_namespace) {
    Suite& s = suite();
    CHECK(s.cov.emptyBatch);
    CHECK(s.cov.crossBatchRepeat);
    CHECK(s.cov.crossBatchBorrow);
    CHECK(s.cov.interference);
    CHECK(s.cov.defaultNs);
    CHECK(s.cov.prefixedNs);
    // schema selector/field prefixes stay fixed no matter the XML prefix
    for (int k = 0; k < 3; k++) {
        CHECK(s.schemaText[k].find("xpath='t:rec'") != std::string::npos);
        CHECK(s.schemaText[k].find("xpath='t:a'") != std::string::npos);
        CHECK(s.schemaText[k].find("xpath='t:b'") != std::string::npos);
        CHECK(s.schemaText[k].find("xpath='rec'") == std::string::npos);
    }
    // model content: the repeated key sits in two different batches and
    // the model verdict is valid (per-batch constraint instances)
    const ModelResult* rep = findScenario(s, "cross_batch_repeat");
    CHECK(rep != NULL);
    CHECK(rep->m.batches.size() >= 2);
    CHECK(rep->expected);
    const ModelResult* bor = findScenario(s, "cross_batch_borrow");
    CHECK(bor != NULL);
    CHECK(!bor->expected);
    // interference nodes share local names but live in another namespace
    const ModelResult* inf = findScenario(s, "interference");
    CHECK(inf != NULL);
    CHECK(inf->expected);
    CHECK(inf->xml.find("n:a") != std::string::npos);
    // default-namespace XML carries no element prefix at all
    const ModelResult* defNs = NULL;
    for (const ModelResult& mr : s.results)
        if (mr.m.prefixMode == 0) { defNs = &mr; break; }
    CHECK(defNs != NULL);
    CHECK(defNs->xml.find("xmlns='urn:idc:Z1383'") != std::string::npos);
    CHECK(defNs->xml.find("<batch>") != std::string::npos);
}

CASE(missing_vs_unique) {
    Suite& s = suite();
    CHECK(s.cov.missingKeyInvalid);
    CHECK(s.cov.missingUniqueValid);
    CHECK(s.cov.missingRefSkipped);
    CHECK(s.cov.keyFalseUniqueTrueBatch);
    // model content: same shape, different constraint kind, opposite verdicts
    const ModelResult* mk = findScenario(s, "missing_key");
    CHECK(mk != NULL);
    CHECK(!mk->expected);
    CHECK(mk->m.kind != SchemaKind::UniqueOnly);
    BatchExpect be = expectBatch(mk->m.batches[0]);
    CHECK(!be.keyOk);    // key requires every field
    CHECK(be.uniqueOk);  // unique excludes the incomplete tuple
    CHECK(be.keyrefOk);
    const ModelResult* mu = findScenario(s, "missing_unique");
    CHECK(mu != NULL);
    CHECK(mu->expected);
    CHECK(mu->m.kind == SchemaKind::UniqueOnly);
    const ModelResult* mref = findScenario(s, "missing_ref");
    CHECK(mref != NULL);
    CHECK(mref->expected);
}

CASE(forward_keyref) {
    Suite& s = suite();
    CHECK(s.cov.forwardRef);
    CHECK(s.cov.orphanRef);
    CHECK(s.cov.crossBatchBorrow);
    // model content: the reference literally precedes its definition
    const ModelResult* fw = findScenario(s, "forward_keyref");
    CHECK(fw != NULL);
    CHECK(fw->expected);
    CHECK(fw->m.batches[0].items.size() >= 2);
    CHECK(fw->m.batches[0].items[0].isRef);
    CHECK(!fw->m.batches[0].items[1].isRef);
    const ModelResult* orf = findScenario(s, "orphan_keyref");
    CHECK(orf != NULL);
    CHECK(!orf->expected);
}

CASE(semantic_mutants) {
    const char* tmp = std::getenv("TMPDIR");
    std::string log = std::string(tmp ? tmp : "/tmp") + "/idc_mutants_run.log";
    std::string cmd =
        "bash regress/idc_mutants/run_mutants.sh > '" + log + "' 2>&1";
    int rc = std::system(cmd.c_str());
    std::ifstream in(log);
    std::string out((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    if (rc != 0)
        std::fprintf(stderr, "mutant run failed (rc=%d):\n%s\n", rc,
                     out.c_str());
    CHECK(rc == 0);
    CHECK(out.find("MUTANT dupkey DEVIATION_CONFIRMED") != std::string::npos);
    CHECK(out.find("MUTANT keymissing DEVIATION_CONFIRMED") !=
          std::string::npos);
    CHECK(out.find("MUTANT keyref DEVIATION_CONFIRMED") != std::string::npos);
}
