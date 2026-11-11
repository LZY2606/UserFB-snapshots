//! Fault-matrix regression tests for the layered X.509 parsers.
//!
//! # Rationale
//!
//! A successful *outer* DER parse of a Certificate / CRL / CSR does **not**
//! imply that the values nested inside it (extension `extnValue` payloads,
//! time fields, ...) are well-formed. This file builds minimal DER objects by
//! hand, injects exactly one structural fault at a time, and records — for
//! every fault — two things:
//!
//! 1. the **layer** at which the fault is expected to surface
//!    ([`Layer`]), and
//! 2. the **absolute offset** of the mutated byte(s) in the buffer
//!    ([`Fault::offset`]).
//!
//! Expectations are derived from DER syntax (X.690, esp. §10.1 canonical
//! length, §8.6 boolean, §8.19/8.20 time) and from the strictness the library
//! publicly exposes (`ParsedExtension::ParseError`,
//! `X509CertificateParser::with_deep_parse_extensions`, `Validity::is_valid`,
//! `extensions_map`, ...). Where the library is deliberately lenient, the
//! leniency is pinned with a `Layer::Accepted` expectation so it cannot
//! silently drift in either direction.
//!
//! # Layer model
//!
//! ```text
//!   bytes ──OuterTlv──► Certificate/CRL/CSR SEQUENCE envelope
//!            ──Tbs────► TBSCertificate / TBSCertList / CertificationRequestInfo
//!            ──Time───► UTCTime / GeneralizedTime decoding
//!            ──ExtensionEnvelope──► SEQUENCE { OID, critical, OCTET STRING }
//!            ──InnerDer──► DER object *inside* extnValue
//!            ──Access──► accessor-time checks (extensions_map, ...)
//!            ──Semantic► non-parse checks (validity window, ...)
//! ```

use x509_parser::asn1_rs::{DerParser, FromDer, Input};
use x509_parser::certificate::X509CertificateParser;
use x509_parser::certification_request::X509CertificationRequest;
use x509_parser::cri_attributes::ParsedCriAttribute;
use x509_parser::error::X509Error;
use x509_parser::extensions::{ParsedExtension, X509Extension};
use x509_parser::nom::Parser;
use x509_parser::time::ASN1Time;
use x509_parser::{parse_x509_certificate, parse_x509_crl};

// ---------------------------------------------------------------------------
// Layer model
// ---------------------------------------------------------------------------

/// The layer at which a fault is expected to surface.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Layer {
    /// Top-level TLV envelope (tag/length of the outer SEQUENCE).
    OuterTlv,
    /// Fields of the TBS structure (version, serial, algorithms, names).
    Tbs,
    /// UTCTime / GeneralizedTime decoding.
    Time,
    /// Extension envelope: `SEQUENCE { OID, critical, OCTET STRING }`.
    ExtensionEnvelope,
    /// DER object *inside* the extnValue OCTET STRING.
    InnerDer,
    /// Not a parse error: surfaces only through an accessor
    /// (`extensions_map`, `get_extension_unique`, ...).
    Access,
    /// Not a parse error: the object parses, but fails a semantic check
    /// (e.g. `Validity::is_valid_at`).
    Semantic,
    /// Accepted by the library: documented leniency, pinned so it cannot
    /// silently change in either direction.
    Accepted,
}

impl Layer {
    fn name(&self) -> &'static str {
        match self {
            Layer::OuterTlv => "outer-tlv",
            Layer::Tbs => "tbs",
            Layer::Time => "time",
            Layer::ExtensionEnvelope => "extension-envelope",
            Layer::InnerDer => "inner-der",
            Layer::Access => "access",
            Layer::Semantic => "semantic",
            Layer::Accepted => "accepted(lenient)",
        }
    }
}

/// One fault-injection case: the mutated bytes plus where (absolute offset)
/// and at which layer the fault is expected to show up.
struct Fault {
    /// Short name of the fault operator.
    name: &'static str,
    /// Why the expectation follows from DER syntax / library strictness.
    rationale: &'static str,
    der: Vec<u8>,
    /// Absolute offset of the first mutated byte in `der`.
    offset: usize,
    layer: Layer,
}

// ---------------------------------------------------------------------------
// Minimal DER builder (shortest-form lengths, X.690 §10.1)
// ---------------------------------------------------------------------------

/// Encode a length in shortest definite form.
fn der_len(len: usize) -> Vec<u8> {
    if len < 0x80 {
        vec![len as u8]
    } else if len < 0x100 {
        vec![0x81, len as u8]
    } else {
        vec![0x82, (len >> 8) as u8, (len & 0xff) as u8]
    }
}

fn tlv(tag: u8, content: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(content.len() + 4);
    out.push(tag);
    out.extend(der_len(content.len()));
    out.extend_from_slice(content);
    out
}

fn sequence(content: &[u8]) -> Vec<u8> {
    tlv(0x30, content)
}
fn set(content: &[u8]) -> Vec<u8> {
    tlv(0x31, content)
}
fn oid(content: &[u8]) -> Vec<u8> {
    tlv(0x06, content)
}
fn integer(content: &[u8]) -> Vec<u8> {
    tlv(0x02, content)
}
fn boolean(raw: u8) -> Vec<u8> {
    tlv(0x01, &[raw])
}
fn octet_string(content: &[u8]) -> Vec<u8> {
    tlv(0x04, content)
}
/// BIT STRING with 0 unused bits.
fn bit_string(data: &[u8]) -> Vec<u8> {
    let mut content = vec![0x00];
    content.extend_from_slice(data);
    tlv(0x03, &content)
}
fn utc_time(s: &str) -> Vec<u8> {
    tlv(0x17, s.as_bytes())
}
fn utf8_string(s: &str) -> Vec<u8> {
    tlv(0x0c, s.as_bytes())
}
fn explicit(n: u8, inner: &[u8]) -> Vec<u8> {
    tlv(0xa0 + n, inner)
}

// OID content bytes (without tag/length).
const OID_SHA256_WITH_RSA: &[u8] = &[0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0b];
const OID_RSA_ENCRYPTION: &[u8] = &[0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01];
const OID_CN: &[u8] = &[0x55, 0x04, 0x03];
const OID_EXT_SUBJECT_KEY_ID: &[u8] = &[0x55, 0x1d, 0x0e]; // 2.5.29.14
const OID_EXT_KEY_USAGE: &[u8] = &[0x55, 0x1d, 0x0f]; // 2.5.29.15
const OID_EXT_CRL_NUMBER: &[u8] = &[0x55, 0x1d, 0x14]; // 2.5.29.20
const OID_EXT_NAME_CONSTRAINTS: &[u8] = &[0x55, 0x1d, 0x1e]; // 2.5.29.30
const OID_UNKNOWN_1_2_3_4: &[u8] = &[0x2a, 0x03, 0x04];
const OID_PKCS9_EXTENSION_REQUEST: &[u8] = &[0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x09, 0x0e];

fn alg_id() -> Vec<u8> {
    sequence(&[oid(OID_SHA256_WITH_RSA), tlv(0x05, &[])].concat())
}

fn rsa_spki() -> Vec<u8> {
    let alg = sequence(&[oid(OID_RSA_ENCRYPTION), tlv(0x05, &[])].concat());
    sequence(&[alg, bit_string(&[0x00])].concat())
}

fn name(cn: &str) -> Vec<u8> {
    let atv = sequence(&[oid(OID_CN), utf8_string(cn)].concat());
    sequence(&set(&atv))
}

/// A well-formed KeyUsage inner value: BIT STRING, first bit (digitalSignature) set.
fn key_usage_value() -> Vec<u8> {
    bit_string(&[0x80])
}

// ---------------------------------------------------------------------------
// Extension spec + object builders
// ---------------------------------------------------------------------------

/// One extension to embed in a built object. `value` is the *content* of the
/// extnValue OCTET STRING, i.e. the inner DER object.
#[derive(Clone)]
struct ExtSpec {
    oid_content: &'static [u8],
    /// Raw content byte of the critical BOOLEAN (`0xff` is the DER encoding
    /// of TRUE); `None` = field absent (DEFAULT FALSE).
    critical: Option<u8>,
    value: Vec<u8>,
}

impl ExtSpec {
    fn new(oid_content: &'static [u8], value: &[u8]) -> Self {
        ExtSpec {
            oid_content,
            critical: None,
            value: value.to_vec(),
        }
    }
    fn critical(mut self, raw_bool: u8) -> Self {
        self.critical = Some(raw_bool);
        self
    }
}

fn extension(spec: &ExtSpec) -> Vec<u8> {
    let mut content = oid(spec.oid_content);
    if let Some(b) = spec.critical {
        content.extend(boolean(b));
    }
    content.extend(octet_string(&spec.value));
    sequence(&content)
}

fn extensions_sequence(exts: &[ExtSpec]) -> Vec<u8> {
    sequence(&exts.iter().map(extension).collect::<Vec<_>>().concat())
}

// ---------------------------------------------------------------------------
// TLV walker: turns the built bytes back into absolute offsets.
// This is the "how we locate the fault" part — every offset recorded in a
// `Fault` comes out of this walker, not from a hand-computed constant.
// ---------------------------------------------------------------------------

/// A raw TLV read from a buffer; all offsets are absolute.
#[derive(Debug, Clone, Copy)]
struct Tlv {
    tag: u8,
    header: usize,  // offset of the tag byte
    content: usize, // offset of the first content byte
    end: usize,     // offset one past the last content byte
}

fn read_tlv(buf: &[u8], at: usize) -> Tlv {
    let tag = buf[at];
    let b0 = buf[at + 1];
    let (len, hdr_len) = if b0 & 0x80 == 0 {
        (b0 as usize, 2)
    } else {
        let n = (b0 & 0x7f) as usize;
        assert!((1..=2).contains(&n), "builder only emits <= 2-byte lengths");
        let mut len = 0usize;
        for i in 0..n {
            len = (len << 8) | buf[at + 2 + i] as usize;
        }
        (len, 2 + n)
    };
    Tlv {
        tag,
        header: at,
        content: at + hdr_len,
        end: at + hdr_len + len,
    }
}

#[derive(Debug)]
struct ExtOffsets {
    ext: usize, // Extension SEQUENCE
    oid_header: usize, // extnID OID tag
    oid_content: usize,
    critical_content: Option<usize>,
    value_header: usize, // extnValue OCTET STRING tag
    value_content: usize, // start of the inner DER object
}

fn walk_extensions(buf: &[u8], seq_content: usize, seq_end: usize) -> Vec<ExtOffsets> {
    let mut out = Vec::new();
    let mut p = seq_content;
    while p < seq_end {
        let ext = read_tlv(buf, p);
        assert_eq!(ext.tag, 0x30, "Extension must be a SEQUENCE");
        let oid_tlv = read_tlv(buf, ext.content);
        assert_eq!(oid_tlv.tag, 0x06, "extnID must be an OID");
        let mut next = read_tlv(buf, oid_tlv.end);
        let critical_content = if next.tag == 0x01 {
            let c = next.content;
            next = read_tlv(buf, next.end);
            Some(c)
        } else {
            None
        };
        assert_eq!(next.tag, 0x04, "extnValue must be an OCTET STRING");
        out.push(ExtOffsets {
            ext: ext.header,
            oid_header: oid_tlv.header,
            oid_content: oid_tlv.content,
            critical_content,
            value_header: next.header,
            value_content: next.content,
        });
        p = ext.end;
    }
    out
}

// --- Certificate -----------------------------------------------------------

#[derive(Debug)]
struct CertOffsets {
    version_content: usize, // content of the version INTEGER inside [0]
    serial_header: usize,   // tag byte of the serialNumber INTEGER
    not_before_content: usize,
    not_after_content: usize,
    exts: Vec<ExtOffsets>,
}

struct BuiltCert {
    der: Vec<u8>,
    off: CertOffsets,
}

fn build_certificate(not_before: &str, not_after: &str, exts: &[ExtSpec]) -> BuiltCert {
    let version = explicit(0, &integer(&[2])); // v3
    let serial = integer(&[1]);
    let validity = sequence(&[utc_time(not_before), utc_time(not_after)].concat());
    let tbs = sequence(
        &[
            version,
            serial,
            alg_id(),
            name("Issuer"),
            validity,
            name("Subject"),
            rsa_spki(),
            explicit(3, &extensions_sequence(exts)),
        ]
        .concat(),
    );
    let der = sequence(&[tbs, alg_id(), bit_string(&[0x00])].concat());
    let off = locate_cert(&der);
    BuiltCert { der, off }
}

fn locate_cert(der: &[u8]) -> CertOffsets {
    let outer = read_tlv(der, 0);
    assert_eq!(outer.tag, 0x30, "Certificate must be a SEQUENCE");
    let tbs = read_tlv(der, outer.content);
    // TBS children, in RFC 5280 §4.1 order.
    let version = read_tlv(der, tbs.content);
    assert_eq!(version.tag, 0xa0, "expected [0] EXPLICIT version");
    let version_int = read_tlv(der, version.content);
    let serial = read_tlv(der, version.end);
    let sig_alg = read_tlv(der, serial.end);
    let issuer = read_tlv(der, sig_alg.end);
    let validity = read_tlv(der, issuer.end);
    let not_before = read_tlv(der, validity.content);
    let not_after = read_tlv(der, not_before.end);
    let subject = read_tlv(der, validity.end);
    let spki = read_tlv(der, subject.end);
    let ext_expl = read_tlv(der, spki.end);
    assert_eq!(ext_expl.tag, 0xa3, "expected [3] EXPLICIT extensions");
    let ext_seq = read_tlv(der, ext_expl.content);
    let exts = walk_extensions(der, ext_seq.content, ext_seq.end);
    CertOffsets {
        version_content: version_int.content,
        serial_header: serial.header,
        not_before_content: not_before.content,
        not_after_content: not_after.content,
        exts,
    }
}

// --- CRL -------------------------------------------------------------------

#[derive(Debug)]
struct CrlOffsets {
    this_update_content: usize,
    exts: Vec<ExtOffsets>,
}

struct BuiltCrl {
    der: Vec<u8>,
    off: CrlOffsets,
}

fn build_crl(this_update: &str, exts: &[ExtSpec]) -> BuiltCrl {
    let tbs = sequence(
        &[
            integer(&[1]), // version v2
            alg_id(),
            name("Issuer"),
            utc_time(this_update),
            explicit(0, &extensions_sequence(exts)), // [0] EXPLICIT crlExtensions
        ]
        .concat(),
    );
    let der = sequence(&[tbs, alg_id(), bit_string(&[0x00])].concat());
    let off = locate_crl(&der);
    BuiltCrl { der, off }
}

fn locate_crl(der: &[u8]) -> CrlOffsets {
    let outer = read_tlv(der, 0);
    assert_eq!(outer.tag, 0x30);
    let tbs = read_tlv(der, outer.content);
    let version = read_tlv(der, tbs.content);
    let sig_alg = read_tlv(der, version.end);
    let issuer = read_tlv(der, sig_alg.end);
    let this_update = read_tlv(der, issuer.end);
    assert_eq!(this_update.tag, 0x17, "thisUpdate must be UTCTime here");
    let ext_expl = read_tlv(der, this_update.end);
    assert_eq!(ext_expl.tag, 0xa0, "expected [0] EXPLICIT crlExtensions");
    let ext_seq = read_tlv(der, ext_expl.content);
    let exts = walk_extensions(der, ext_seq.content, ext_seq.end);
    CrlOffsets {
        this_update_content: this_update.content,
        exts,
    }
}

// --- CSR -------------------------------------------------------------------

#[derive(Debug)]
struct CsrOffsets {
    attr_oid_content: usize, // content of the attribute OID
    values_header: usize,    // tag byte of the attribute values SET
    exts: Vec<ExtOffsets>,   // extensions inside the extensionRequest value
}

struct BuiltCsr {
    der: Vec<u8>,
    off: CsrOffsets,
}

/// Attribute ::= SEQUENCE { OID, SET OF AttributeValue }; the single value of
/// an extensionRequest attribute is an Extensions SEQUENCE (RFC 5272 §3.1).
fn extension_request_attribute(exts: &[ExtSpec]) -> Vec<u8> {
    sequence(&[oid(OID_PKCS9_EXTENSION_REQUEST), set(&extensions_sequence(exts))].concat())
}

fn build_csr(attributes: &[Vec<u8>]) -> BuiltCsr {
    // attributes [0] IMPLICIT: tag a0, content = concatenated Attribute SEQUENCEs
    let cri = sequence(
        &[
            integer(&[0]), // v1
            name("Subject"),
            rsa_spki(),
            tlv(0xa0, &attributes.concat()),
        ]
        .concat(),
    );
    let der = sequence(&[cri, alg_id(), bit_string(&[0x00])].concat());
    let off = locate_csr(&der);
    BuiltCsr { der, off }
}

fn locate_csr(der: &[u8]) -> CsrOffsets {
    let outer = read_tlv(der, 0);
    assert_eq!(outer.tag, 0x30);
    let cri = read_tlv(der, outer.content);
    let version = read_tlv(der, cri.content);
    let subject = read_tlv(der, version.end);
    let spki = read_tlv(der, subject.end);
    let attrs = read_tlv(der, spki.end);
    assert_eq!(attrs.tag, 0xa0, "expected [0] IMPLICIT attributes");
    let attr = read_tlv(der, attrs.content);
    assert_eq!(attr.tag, 0x30, "Attribute must be a SEQUENCE");
    let attr_oid = read_tlv(der, attr.content);
    let values = read_tlv(der, attr_oid.end);
    assert_eq!(values.tag, 0x31, "attribute values must be a SET");
    let ext_seq = read_tlv(der, values.content);
    assert_eq!(ext_seq.tag, 0x30, "extensionRequest value must be a SEQUENCE");
    let exts = walk_extensions(der, ext_seq.content, ext_seq.end);
    CsrOffsets {
        attr_oid_content: attr_oid.content,
        values_header: values.header,
        exts,
    }
}

// ---------------------------------------------------------------------------
// Fault operators (certificate)
//
// Each operator takes the baseline object, applies exactly one structural
// mutation, and returns the `Fault` recording the layer and the absolute
// offset. In-place mutations reuse the baseline offsets; mutations that
// change lengths rebuild the object and re-locate the offsets, so the
// recorded offset always refers to the mutated buffer.
// ---------------------------------------------------------------------------

/// Flip the outer SEQUENCE tag to SET. DER: a Certificate *is* a SEQUENCE.
fn fault_outer_tag_not_sequence(b: &BuiltCert) -> Fault {
    let mut der = b.der.clone();
    der[0] = 0x31;
    Fault {
        name: "outer-tag-not-sequence",
        rationale: "Certificate ::= SEQUENCE; tag 0x31 (SET) must be rejected at the envelope",
        der,
        offset: 0,
        layer: Layer::OuterTlv,
    }
}

/// Re-encode the outer length with more octets than necessary.
/// X.690 §10.1 requires the shortest form in DER.
fn fault_outer_length_non_minimal(b: &BuiltCert) -> Fault {
    // Baseline uses the 1-octet long form `30 81 <len>` (total size is
    // 128..=255 by construction); `30 82 00 <len>` encodes the same length
    // with a superfluous leading zero octet: legal BER, forbidden in DER.
    assert_eq!(b.der[1], 0x81, "baseline must use 1-octet long-form length");
    let len = b.der[2];
    let mut der = vec![0x30, 0x82, 0x00, len];
    der.extend_from_slice(&b.der[3..]);
    Fault {
        name: "outer-length-non-minimal",
        rationale: "DER §10.1 requires the shortest length form, but the BER/DER header \
                    parser accepts non-minimal long forms: parsed leniently"
        ,
        der,
        offset: 1,
        layer: Layer::Accepted,
    }
}

/// Flip the serialNumber tag from INTEGER to NULL.
fn fault_serial_tag_not_integer(b: &BuiltCert) -> Fault {
    let offset = b.off.serial_header;
    assert_eq!(b.der[offset], 0x02);
    let mut der = b.der.clone();
    der[offset] = 0x05;
    Fault {
        name: "serial-tag-not-integer",
        rationale: "serialNumber must be an INTEGER; the parser maps this to InvalidSerial",
        der,
        offset,
        layer: Layer::Tbs,
    }
}

/// Set the version INTEGER to 5 (only 0..=2 are defined by RFC 5280).
fn fault_version_out_of_range(b: &BuiltCert) -> Fault {
    let offset = b.off.version_content;
    assert_eq!(b.der[offset], 0x02);
    let mut der = b.der.clone();
    der[offset] = 0x05;
    Fault {
        name: "version-out-of-range",
        rationale: "X509Version is a plain u32 newtype: no range check, parsed leniently",
        der,
        offset,
        layer: Layer::Accepted,
    }
}

/// Write month = 13 into notBefore ("251301..."): well-formed length, invalid date.
fn fault_not_before_month_13(b: &BuiltCert) -> Fault {
    let offset = b.off.not_before_content;
    assert_eq!(&b.der[offset..offset + 2], b"25");
    let mut der = b.der.clone();
    der[offset + 2..offset + 4].copy_from_slice(b"13");
    Fault {
        name: "not-before-month-13",
        rationale: "UTCTime with month 13 is not a valid date; rejected while decoding Time",
        der,
        offset: offset + 2,
        layer: Layer::Time,
    }
}

/// Encode critical as 0x01. BER accepts any non-zero byte for TRUE;
/// DER §8.2 requires exactly 0xff.
fn fault_critical_boolean_not_der(b: &BuiltCert) -> Fault {
    let offset = b.off.exts[0]
        .critical_content
        .expect("baseline extension must be critical");
    assert_eq!(b.der[offset], 0xff);
    let mut der = b.der.clone();
    der[offset] = 0x01;
    Fault {
        name: "critical-boolean-not-der",
        rationale: "library reads critical with parse_ber_optional (see der_read_critical), \
                    so a non-DER boolean is accepted"
        ,
        der,
        offset,
        layer: Layer::Accepted,
    }
}

/// Flip the extnID tag from OID to NULL.
fn fault_extnid_tag_not_oid(b: &BuiltCert) -> Fault {
    let offset = b.off.exts[0].oid_header;
    assert_eq!(b.der[offset], 0x06);
    let mut der = b.der.clone();
    der[offset] = 0x05;
    Fault {
        name: "extnid-tag-not-oid",
        rationale: "extnID must be an OBJECT IDENTIFIER; the envelope parser rejects \
                    anything else"
        ,
        der,
        offset,
        layer: Layer::ExtensionEnvelope,
    }
}

/// Set the continuation bit on the last OID arc. DER (X.690 §8.19) requires
/// the last arc octet to have the continuation bit clear — but the OID parser
/// used by the library does not enforce this, so the mutated OID is accepted
/// and decoded as a *different* (unknown) OID.
fn fault_ext_oid_continuation_bit(b: &BuiltCert) -> Fault {
    // KeyUsage OID content is 55 1d 0f; 0x8f has the continuation bit set.
    let offset = b.off.exts[0].oid_content + 2;
    assert_eq!(b.der[offset], 0x0f);
    let mut der = b.der.clone();
    der[offset] = 0x8f;
    Fault {
        name: "ext-oid-continuation-bit",
        rationale: "OID parser is lenient about a trailing continuation bit: the OID is \
                    accepted, decodes to an unknown OID, extension becomes UnsupportedExtension"
        ,
        der,
        offset,
        layer: Layer::Accepted,
    }
}

/// Flip the extnValue tag from OCTET STRING to NULL.
fn fault_extnvalue_tag_not_octet_string(b: &BuiltCert) -> Fault {
    let offset = b.off.exts[0].value_header;
    assert_eq!(b.der[offset], 0x04);
    let mut der = b.der.clone();
    der[offset] = 0x05;
    Fault {
        name: "extnvalue-tag-not-octet-string",
        rationale: "extnValue must be an OCTET STRING; the envelope parser rejects anything else",
        der,
        offset,
        layer: Layer::ExtensionEnvelope,
    }
}

/// Replace the KeyUsage inner value with a BOOLEAN: outer bytes stay valid,
/// the inner DER is wrong for this OID.
fn fault_keyusage_inner_wrong_type(b: &BuiltCert) -> Fault {
    let der = rebuild_with_exts(b, &[ExtSpec::new(OID_EXT_KEY_USAGE, &boolean(0xff))]);
    let off = locate_cert(&der);
    Fault {
        name: "keyusage-inner-not-bitstring",
        rationale: "KeyUsage ::= BIT STRING; a BOOLEAN inside extnValue fails the inner parser \
                    and is captured as ParsedExtension::ParseError"
        ,
        offset: off.exts[0].value_content,
        der,
        layer: Layer::InnerDer,
    }
}

/// Truncate the inner BIT STRING: declared content length exceeds the data.
fn fault_keyusage_inner_truncated(b: &BuiltCert) -> Fault {
    // BIT STRING claiming 5 content bytes but carrying only 1.
    let der = rebuild_with_exts(b, &[ExtSpec::new(OID_EXT_KEY_USAGE, &[0x03, 0x05, 0x00])]);
    let off = locate_cert(&der);
    Fault {
        name: "keyusage-inner-truncated",
        rationale: "inner BIT STRING declares 5 content bytes, provides 1: incomplete inner DER",
        offset: off.exts[0].value_content,
        der,
        layer: Layer::InnerDer,
    }
}

/// Append garbage after a well-formed inner BIT STRING.
fn fault_keyusage_inner_trailing_garbage(b: &BuiltCert) -> Fault {
    let mut value = key_usage_value();
    value.extend([0xff, 0xff]);
    let der = rebuild_with_exts(b, &[ExtSpec::new(OID_EXT_KEY_USAGE, &value)]);
    let off = locate_cert(&der);
    Fault {
        name: "keyusage-inner-trailing-garbage",
        rationale: "extension inner parsers are not all_consuming: bytes after the inner \
                    object are silently dropped"
        ,
        offset: off.exts[0].value_content + key_usage_value().len(),
        der,
        layer: Layer::Accepted,
    }
}

/// NameConstraints whose permittedSubtrees subtree is cut short.
fn fault_nameconstraints_bad_subtree(b: &BuiltCert) -> Fault {
    // 30 03 a0 01 30: SEQUENCE{ [0]{ <single byte 0x30> } } — the GeneralSubtree
    // SEQUENCE is truncated (tag without length/content).
    let der = rebuild_with_exts(
        b,
        &[ExtSpec::new(OID_EXT_NAME_CONSTRAINTS, &[0x30, 0x03, 0xa0, 0x01, 0x30])],
    );
    let off = locate_cert(&der);
    Fault {
        name: "nameconstraints-truncated-subtree",
        rationale: "GeneralSubtree inside permittedSubtrees is truncated; fails one level \
                    below the extension envelope"
        ,
        offset: off.exts[0].value_content + 4,
        der,
        layer: Layer::InnerDer,
    }
}

/// Rebuild the baseline certificate with a different extension list,
/// keeping the same (valid) validity window.
fn rebuild_with_exts(b: &BuiltCert, exts: &[ExtSpec]) -> Vec<u8> {
    // The baseline was built with these fixed dates; re-derive them from the
    // bytes so this helper stays honest if the baseline changes.
    let nb = &b.der[b.off.not_before_content..b.off.not_before_content + 13];
    let na = &b.der[b.off.not_after_content..b.off.not_after_content + 13];
    build_certificate(
        std::str::from_utf8(nb).unwrap(),
        std::str::from_utf8(na).unwrap(),
        exts,
    )
    .der
}

/// All certificate faults that go through the generic matrix driver.
fn cert_faults() -> Vec<Fault> {
    // Baseline: valid window, one critical KeyUsage extension.
    let ku_critical = || ExtSpec::new(OID_EXT_KEY_USAGE, &key_usage_value()).critical(0xff);
    let baseline = build_certificate("250101000000Z", "301231235959Z", &[ku_critical()]);
    let mut faults = vec![
        fault_outer_tag_not_sequence(&baseline),
        fault_outer_length_non_minimal(&baseline),
        fault_serial_tag_not_integer(&baseline),
        fault_version_out_of_range(&baseline),
        fault_not_before_month_13(&baseline),
        fault_critical_boolean_not_der(&baseline),
        fault_extnid_tag_not_oid(&baseline),
        fault_ext_oid_continuation_bit(&baseline),
        fault_extnvalue_tag_not_octet_string(&baseline),
        fault_keyusage_inner_wrong_type(&baseline),
        fault_keyusage_inner_truncated(&baseline),
        fault_keyusage_inner_trailing_garbage(&baseline),
        fault_nameconstraints_bad_subtree(&baseline),
    ];

    // Duplicate extension: two SubjectKeyIdentifier extensions.
    // RFC 5280 §4.2: "A certificate MUST NOT include more than one instance
    // of a particular extension." The parser accepts them; the duplication is
    // reported only when indexing extensions (extensions_map /
    // get_extension_unique) with X509Error::DuplicateExtensions.
    let skid = || ExtSpec::new(OID_EXT_SUBJECT_KEY_ID, &octet_string(&[0xaa; 20]));
    let dup = build_certificate("250101000000Z", "301231235959Z", &[skid(), skid()]);
    faults.push(Fault {
        name: "duplicate-extension",
        rationale: "duplicate extnID is not rejected at parse time; \
                    extensions_map() reports DuplicateExtensions"
        ,
        offset: dup.off.exts[1].ext,
        der: dup.der,
        layer: Layer::Access,
    });

    // Expired certificate: both times are well-formed UTCTime, but the
    // validity window lies entirely in the past.
    let expired = build_certificate(
        "190101000000Z",
        "200101000000Z",
        &[ExtSpec::new(OID_EXT_KEY_USAGE, &key_usage_value())],
    );
    faults.push(Fault {
        name: "validity-window-expired",
        rationale: "encoding is valid DER; only the semantic check Validity::is_valid_at fails",
        offset: expired.off.not_after_content,
        der: expired.der,
        layer: Layer::Semantic,
    });

    // Inverted window: notBefore > notAfter. The parser performs no
    // cross-field check; every is_valid_at() call comes out false.
    let inverted = build_certificate(
        "260101000000Z",
        "200101000000Z",
        &[ExtSpec::new(OID_EXT_KEY_USAGE, &key_usage_value())],
    );
    faults.push(Fault {
        name: "validity-window-inverted",
        rationale: "no notBefore<=notAfter check at parse time; is_valid_at is always false",
        offset: inverted.off.not_before_content,
        der: inverted.der,
        layer: Layer::Semantic,
    });

    faults
}

// ---------------------------------------------------------------------------
// Matrix driver
// ---------------------------------------------------------------------------

/// Verify one certificate fault against its recorded layer, and print one
/// matrix row (visible with `cargo test -- --nocapture`).
fn check_cert_fault(f: &Fault) {
    let res = parse_x509_certificate(&f.der);
    let outcome = match f.layer {
        Layer::OuterTlv | Layer::Tbs | Layer::Time | Layer::ExtensionEnvelope => {
            let err = res.expect_err(&format!(
                "{}: fault at offset {} must fail parsing (layer {})",
                f.name, f.offset, f.layer.name()
            ));
            format!("parse error: {err:?}")
        }
        Layer::InnerDer => {
            let (_, cert) = res.unwrap_or_else(|e| {
                panic!(
                    "{}: outer parse must succeed; inner fault is reported per-extension: {e:?}",
                    f.name
                )
            });
            let ext = cert.tbs_certificate.extensions()[0].parsed_extension();
            assert!(
                ext.error().is_some(),
                "{}: expected ParsedExtension::ParseError, got {ext:?}",
                f.name
            );
            format!("outer ok, inner error: {:?}", ext.error().unwrap())
        }
        Layer::Access => {
            let (_, cert) = res.unwrap_or_else(|e| panic!("{}: parse must succeed: {e:?}", f.name));
            let err = cert
                .tbs_certificate
                .extensions_map()
                .expect_err(&format!("{}: extensions_map must fail", f.name));
            assert_eq!(err, X509Error::DuplicateExtensions, "{}", f.name);
            format!("parse ok, extensions_map: {err:?}")
        }
        Layer::Semantic => {
            let (_, cert) = res.unwrap_or_else(|e| panic!("{}: parse must succeed: {e:?}", f.name));
            let v = &cert.tbs_certificate.validity;
            // Deterministic check: one second past notAfter, the certificate
            // must not be valid (covers both "expired" and "inverted").
            let after_end = ASN1Time::from_timestamp(v.not_after.timestamp() + 1).unwrap();
            assert!(
                !v.is_valid_at(after_end),
                "{}: certificate must not be valid past its window",
                f.name
            );
            "parse ok, is_valid_at(not_after + 1s) == false".to_string()
        }
        Layer::Accepted => {
            assert!(
                res.is_ok(),
                "{}: documented leniency — parse must succeed: {:?}",
                f.name,
                res.err()
            );
            "parse ok (documented leniency)".to_string()
        }
    };
    eprintln!(
        "matrix| {:32} | offset {:4} | layer {:20} | {}\n  why| {}",
        f.name,
        f.offset,
        f.layer.name(),
        outcome,
        f.rationale
    );
}

#[test]
fn fault_matrix_certificate() {
    // Sanity: the unmutated baseline parses and its extension deep-parses.
    let baseline = build_certificate(
        "250101000000Z",
        "301231235959Z",
        &[ExtSpec::new(OID_EXT_KEY_USAGE, &key_usage_value()).critical(0xff)],
    );
    let (rem, cert) = parse_x509_certificate(&baseline.der).expect("baseline must parse");
    assert!(rem.is_empty(), "baseline must be fully consumed");
    assert!(matches!(
        cert.tbs_certificate.extensions()[0].parsed_extension(),
        ParsedExtension::KeyUsage(_)
    ));

    for f in cert_faults() {
        check_cert_fault(&f);
    }
}

// ---------------------------------------------------------------------------
// Distinction 1: outer DER and inner DER are two separate layers
// ---------------------------------------------------------------------------

#[test]
fn outer_and_inner_der_are_two_layers() {
    // KeyUsage extnValue contains a BOOLEAN: the outer envelope is fine,
    // the inner DER is not a BIT STRING.
    let built = build_certificate(
        "250101000000Z",
        "301231235959Z",
        &[ExtSpec::new(OID_EXT_KEY_USAGE, &boolean(0xff))],
    );
    let inner_offset = built.off.exts[0].value_content;
    assert_eq!(built.der[inner_offset], 0x01, "inner object is a BOOLEAN");

    // Layer 1 (outer): the certificate parses.
    let (rem, cert) = parse_x509_certificate(&built.der).expect("outer layer must succeed");
    assert!(rem.is_empty());

    // Layer 2 (inner): the failure is reported on the extension, not on the
    // certificate.
    let ext = &cert.tbs_certificate.extensions()[0];
    assert!(
        matches!(ext.parsed_extension(), ParsedExtension::ParseError { .. }),
        "inner layer must surface as ParseError, got {:?}",
        ext.parsed_extension()
    );
    // The raw inner bytes are preserved for inspection.
    assert_eq!(ext.value.as_bytes2(), &boolean(0xff)[..]);

    // The two layers are independently switchable: with deep parsing off,
    // the exact same bytes yield neither an error nor a parsed value.
    let mut shallow = X509CertificateParser::new().with_deep_parse_extensions(false);
    let (_, cert) = shallow
        .parse(Input::from(&built.der))
        .expect("shallow parse must succeed");
    assert!(matches!(
        cert.tbs_certificate.extensions()[0].parsed_extension(),
        ParsedExtension::Unparsed
    ));

    // And the inner layer can be re-run on its own: parsing the extension
    // bytes directly reproduces the same ParseError at the same offset.
    let ext_start = built.off.exts[0].ext;
    let ext_tlv = read_tlv(&built.der, ext_start);
    let (_, ext) = X509Extension::parse_der(Input::from(&built.der[ext_start..ext_tlv.end]))
        .expect("extension envelope alone must parse");
    assert!(ext.parsed_extension().error().is_some());
}

// ---------------------------------------------------------------------------
// Distinction 2: an unknown extension is not a corrupted extension
// ---------------------------------------------------------------------------

#[test]
fn unknown_extension_is_not_a_corrupted_extension() {
    let garbage = &[0xde, 0xad, 0xbe, 0xef];

    // (a) Unknown OID + garbage value: not an error. The library does not
    //     claim to understand the OID, so the value is kept raw.
    let unknown = build_certificate(
        "250101000000Z",
        "301231235959Z",
        &[ExtSpec::new(OID_UNKNOWN_1_2_3_4, garbage)],
    );
    let value_offset = unknown.off.exts[0].value_content;
    assert_eq!(&unknown.der[value_offset..value_offset + 4], garbage);
    let (_, cert) = parse_x509_certificate(&unknown.der).expect("unknown extension must parse");
    let ext = &cert.tbs_certificate.extensions()[0];
    assert!(ext.parsed_extension().unsupported());
    assert!(ext.parsed_extension().error().is_none());
    match ext.parsed_extension() {
        ParsedExtension::UnsupportedExtension(u) => {
            assert_eq!(u.value, garbage);
            assert!(!u.critical);
        }
        other => panic!("expected UnsupportedExtension, got {:?}", other),
    }

    // (b) Known OID + the very same garbage value: the library *does* claim
    //     to understand this OID, so the garbage is a parse error.
    let corrupted = build_certificate(
        "250101000000Z",
        "301231235959Z",
        &[ExtSpec::new(OID_EXT_KEY_USAGE, garbage)],
    );
    let (_, cert) = parse_x509_certificate(&corrupted.der).expect("outer layer must still parse");
    let ext = &cert.tbs_certificate.extensions()[0];
    assert!(!ext.parsed_extension().unsupported());
    assert!(
        ext.parsed_extension().error().is_some(),
        "corrupted known extension must carry a ParseError"
    );
}

// ---------------------------------------------------------------------------
// Distinction 3: a valid time encoding is not the same as being in validity
// ---------------------------------------------------------------------------

#[test]
fn valid_time_encoding_is_not_validity() {
    // (a) Well-formed UTCTime, window entirely in the past: parses fine,
    //     fails only the semantic window check.
    let expired = build_certificate(
        "190101000000Z",
        "200101000000Z",
        &[ExtSpec::new(OID_EXT_KEY_USAGE, &key_usage_value())],
    );
    let (_, cert) = parse_x509_certificate(&expired.der).expect("valid encoding must parse");
    let v = &cert.tbs_certificate.validity;
    // Intermediate steps, not just the conclusion:
    let inside = ASN1Time::from_timestamp(v.not_before.timestamp() + 1).unwrap();
    let after = ASN1Time::from_timestamp(v.not_after.timestamp() + 1).unwrap();
    assert!(v.is_valid_at(inside), "inside the window: valid");
    assert!(!v.is_valid_at(after), "past the window: invalid");
    assert!(!v.is_valid(), "2019-2020 is in the past relative to any run date >= 2020");
    assert!(v.time_to_expiration().is_none());

    // (b) Same field, invalid encoding (month 13): fails one layer earlier,
    //     while decoding Time — the certificate never reaches a semantic check.
    let bad_encoding = build_certificate(
        "191301000000Z",
        "200101000000Z",
        &[ExtSpec::new(OID_EXT_KEY_USAGE, &key_usage_value())],
    );
    let offset = bad_encoding.off.not_before_content + 2;
    assert_eq!(&bad_encoding.der[offset..offset + 2], b"13");
    let err = parse_x509_certificate(&bad_encoding.der)
        .expect_err("invalid time encoding must fail at parse time");
    eprintln!("matrix| invalid-time-encoding error: {err:?}");
}

// ---------------------------------------------------------------------------
// CRL faults
// ---------------------------------------------------------------------------

#[test]
fn fault_matrix_crl() {
    // Baseline: v2 CRL, one cRLNumber extension.
    let crl_number = || ExtSpec::new(OID_EXT_CRL_NUMBER, &integer(&[5]));
    let baseline = build_crl("250101000000Z", &[crl_number()]);
    let (rem, crl) = parse_x509_crl(&baseline.der).expect("baseline CRL must parse");
    assert!(rem.is_empty());
    assert!(matches!(
        crl.tbs_cert_list.extensions()[0].parsed_extension(),
        ParsedExtension::CRLNumber(_)
    ));

    // Time layer: thisUpdate with month 13 must fail parsing.
    let bad_time = build_crl("251301000000Z", &[crl_number()]);
    let offset = bad_time.off.this_update_content + 2;
    assert_eq!(&bad_time.der[offset..offset + 2], b"13");
    let err = parse_x509_crl(&bad_time.der).expect_err("invalid thisUpdate must fail");
    eprintln!(
        "matrix| {:32} | offset {:4} | layer {:20} | parse error: {err:?}",
        "crl-this-update-month-13",
        offset,
        Layer::Time.name()
    );

    // Inner layer: cRLNumber ::= INTEGER; an OCTET STRING inside extnValue
    // parses the CRL envelope but fails the inner parser.
    let bad_inner = build_crl("250101000000Z", &[ExtSpec::new(OID_EXT_CRL_NUMBER, &octet_string(&[1]))]);
    let offset = bad_inner.off.exts[0].value_content;
    let (_, crl) = parse_x509_crl(&bad_inner.der).expect("outer CRL layer must succeed");
    let ext = crl.tbs_cert_list.extensions()[0].parsed_extension();
    assert!(
        ext.error().is_some(),
        "corrupted cRLNumber must surface as ParseError, got {:?}",
        ext
    );
    eprintln!(
        "matrix| {:32} | offset {:4} | layer {:20} | outer ok, inner error recorded",
        "crlnumber-inner-not-integer",
        offset,
        Layer::InnerDer.name()
    );
}

// ---------------------------------------------------------------------------
// CSR faults
// ---------------------------------------------------------------------------

#[test]
fn fault_matrix_csr() {
    // Baseline: CSR with an extensionRequest attribute asking for KeyUsage.
    let baseline = build_csr(&[extension_request_attribute(&[ExtSpec::new(
        OID_EXT_KEY_USAGE,
        &key_usage_value(),
    )])]);
    let (rem, csr) =
        X509CertificationRequest::from_der(&baseline.der).expect("baseline CSR must parse");
    assert!(rem.is_empty());
    let attrs = csr.certification_request_info.attributes();
    assert_eq!(attrs.len(), 1);
    match &attrs[0].parsed_attributes()[0] {
        ParsedCriAttribute::ExtensionRequest(er) => {
            assert!(matches!(
                er.extensions[0].parsed_extension(),
                ParsedExtension::KeyUsage(_)
            ));
        }
        other => panic!("expected ExtensionRequest, got {:?}", other),
    }

    // Inner layer: garbage inside the requested KeyUsage extnValue. The CSR
    // parses; the failure is recorded on the nested extension.
    let bad_inner = build_csr(&[extension_request_attribute(&[ExtSpec::new(
        OID_EXT_KEY_USAGE,
        &boolean(0xff),
    )])]);
    let offset = bad_inner.off.exts[0].value_content;
    let (_, csr) = X509CertificationRequest::from_der(&bad_inner.der)
        .expect("outer CSR layer must succeed");
    match &csr.certification_request_info.attributes()[0].parsed_attributes()[0] {
        ParsedCriAttribute::ExtensionRequest(er) => {
            assert!(
                er.extensions[0].parsed_extension().error().is_some(),
                "corrupted inner extension must carry a ParseError"
            );
        }
        other => panic!("expected ExtensionRequest, got {:?}", other),
    }
    eprintln!(
        "matrix| {:32} | offset {:4} | layer {:20} | outer ok, inner error recorded",
        "csr-extreq-inner-garbage",
        offset,
        Layer::InnerDer.name()
    );

    // Envelope layer: flip the attribute-values SET tag to SEQUENCE. CSR
    // attributes are parsed eagerly (unlike certificate extensions, which
    // are collected with many0), so the error aborts the whole parse.
    let mut der = baseline.der.clone();
    let offset = baseline.off.values_header;
    assert_eq!(der[offset], 0x31);
    der[offset] = 0x30;
    let err = X509CertificationRequest::from_der(&der)
        .expect_err("attribute values must be a SET; tag flip must fail the CSR parse");
    eprintln!(
        "matrix| {:32} | offset {:4} | layer {:20} | parse error: {err:?}",
        "csr-attr-values-not-set",
        offset,
        Layer::ExtensionEnvelope.name()
    );

    // Documented leniency: a trailing continuation bit in the attribute OID
    // is *not* rejected. The mutated OID decodes to a different (unknown)
    // OID, so the attribute silently becomes UnsupportedAttribute.
    let mut der = baseline.der.clone();
    let offset = baseline.off.attr_oid_content + 8;
    assert_eq!(der[offset], 0x0e);
    der[offset] = 0x8e;
    let (_, csr) = X509CertificationRequest::from_der(&der)
        .expect("lenient OID parsing must accept the continuation bit");
    assert!(matches!(
        csr.certification_request_info.attributes()[0].parsed_attributes()[0],
        ParsedCriAttribute::UnsupportedAttribute
    ));
    eprintln!(
        "matrix| {:32} | offset {:4} | layer {:20} | parse ok, attribute became UnsupportedAttribute",
        "csr-attr-oid-continuation-bit",
        offset,
        Layer::Accepted.name()
    );
}
