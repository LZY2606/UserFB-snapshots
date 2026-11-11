// Regression test: canonical Ed25519 signature verification.
//
// Verifies that both nacl.js and nacl-fast.js reject:
//   - non-canonical S (S >= L), including the S + L malleability variant
//     that used to verify because S was silently reduced mod L;
//   - non-canonical point encodings (y >= 2^255-19) for R and A;
//   - encodings that do not decode to a curve point;
//   - x = 0 with a non-zero sign bit;
//   - public keys / R values outside the prime-order subgroup
//     (small-order and mixed-order points), and the identity as A;
// while still accepting genuinely valid signatures and keeping the
// original failure shapes (verify -> false, sign.open -> null,
// lowlevel crypto_sign_open -> -1) and not mutating caller input.
//
// Run: node test/sign-canonical-regression.js

'use strict';

const assert = require('node:assert');

const implementations = [
  ['nacl.js', require('../nacl.js')],
  ['nacl-fast.js', require('../nacl-fast.js')]
];

// The group order L (little-endian).
const L = Uint8Array.from(
  Buffer.from('edd3f55c1a631258d69cf7a2def9de1400000000000000000000000000000010', 'hex'));

// p = 2^255 - 19.
const P_BIG = (1n << 255n) - 19n;

// ---------------------------------------------------------------------------
// Byte/bigint helpers (synthetic data only, no external vectors).
// ---------------------------------------------------------------------------

function bytesToBigintLE(bytes) {
  let v = 0n;
  for (let i = bytes.length - 1; i >= 0; i--) v = (v << 8n) | BigInt(bytes[i]);
  return v;
}

function bigintToBytesLE(v) {
  const out = new Uint8Array(32);
  for (let i = 0; i < 32; i++) { out[i] = Number(v & 0xffn); v >>= 8n; }
  return out;
}

function modpow(b, e, m) {
  let r = 1n;
  b %= m;
  while (e > 0n) {
    if (e & 1n) r = r * b % m;
    b = b * b % m;
    e >>= 1n;
  }
  return r;
}

// 32-byte little-endian addition (mod 2^256), to exercise carry propagation.
function addLE(a, b) {
  const out = new Uint8Array(32);
  let carry = 0;
  for (let i = 0; i < 32; i++) {
    const s = a[i] + b[i] + carry;
    out[i] = s & 0xff;
    carry = s >> 8;
  }
  return out;
}

function leConstant(v) {
  return bigintToBytesLE(v);
}

// Maps a point encoding (x, y) to (-x, -y), i.e. adds the order-2
// torsion point (0, -1). The result is on the curve and decodable, but
// has a non-trivial torsion component, so it is outside the prime-order
// subgroup (a "mixed-order" point whenever the input is in the subgroup).
function addTorsionComponent(enc) {
  const v = bytesToBigintLE(enc);
  const sign = (v >> 255n) & 1n;
  const y = v & ((1n << 255n) - 1n);
  const y2 = (P_BIG - y) % P_BIG;
  return bigintToBytesLE(y2 | ((sign ^ 1n) << 255n));
}

// Smallest y >= 2 whose encoding does not decode to a curve point
// (x^2 = (y^2-1)/(d*y^2+1) is a non-residue mod p).
function findUndecodableEncoding() {
  const d = (P_BIG - 121665n) * modpow(121666n, P_BIG - 2n, P_BIG) % P_BIG;
  for (let y = 2n; ; y++) {
    const y2 = y * y % P_BIG;
    const num = (y2 - 1n + P_BIG) % P_BIG;
    const denInv = modpow((d * y2 + 1n) % P_BIG, P_BIG - 2n, P_BIG);
    const x2 = num * denInv % P_BIG;
    if (x2 !== 0n && modpow(x2, (P_BIG - 1n) / 2n, P_BIG) === P_BIG - 1n) {
      return bigintToBytesLE(y); // sign bit 0
    }
  }
}

// ---------------------------------------------------------------------------
// Assertion helpers.
// ---------------------------------------------------------------------------

function concat(sig, msg) {
  const sm = new Uint8Array(sig.length + msg.length);
  sm.set(sig, 0);
  sm.set(msg, 64);
  return sm;
}

// All three entry points must reject, keeping their original failure shapes.
function assertRejected(nacl, msg, sig, pk, what) {
  assert.strictEqual(nacl.sign.detached.verify(msg, sig, pk), false,
    `${what}: sign.detached.verify must return false`);
  assert.strictEqual(nacl.sign.open(concat(sig, msg), pk), null,
    `${what}: sign.open must return null`);
  const sm = concat(sig, msg);
  const m = new Uint8Array(sm.length);
  assert.strictEqual(nacl.lowlevel.crypto_sign_open(m, sm, sm.length, pk), -1,
    `${what}: lowlevel crypto_sign_open must return -1`);
}

// Same, but passing signature and public key as subviews surrounded by
// sentinel bytes; a failed verification must not modify caller input.
function assertRejectedWithSentinels(nacl, msg, sig, pk, what) {
  const sigStore = new Uint8Array(66);
  const pkStore = new Uint8Array(34);
  sigStore[0] = 0xa5; sigStore[65] = 0x5a;
  pkStore[0] = 0xc3; pkStore[33] = 0x3c;
  const sigView = sigStore.subarray(1, 65);
  const pkView = pkStore.subarray(1, 33);
  sigView.set(sig);
  pkView.set(pk);
  const sigSnap = Uint8Array.from(sigStore);
  const pkSnap = Uint8Array.from(pkStore);

  assert.strictEqual(nacl.sign.detached.verify(msg, sigView, pkView), false,
    `${what}: sign.detached.verify (subviews) must return false`);
  assert.deepStrictEqual(sigStore, sigSnap, `${what}: signature input was mutated`);
  assert.deepStrictEqual(pkStore, pkSnap, `${what}: public key input was mutated`);

  const smStore = new Uint8Array(64 + msg.length + 2);
  smStore[0] = 0x11; smStore[smStore.length - 1] = 0x22;
  const smView = smStore.subarray(1, smStore.length - 1);
  smView.set(sig, 0);
  smView.set(msg, 64);
  const smSnap = Uint8Array.from(smStore);
  const m = new Uint8Array(smView.length);
  assert.strictEqual(nacl.lowlevel.crypto_sign_open(m, smView, smView.length, pkView), -1,
    `${what}: lowlevel crypto_sign_open (subviews) must return -1`);
  assert.deepStrictEqual(smStore, smSnap, `${what}: signed message input was mutated`);
  assert.deepStrictEqual(pkStore, pkSnap, `${what}: public key input was mutated (lowlevel)`);
}

// ---------------------------------------------------------------------------
// Test suite, run against both implementations.
// ---------------------------------------------------------------------------

function runSuite(name, nacl) {
  const tag = `[${name}]`;

  // Deterministic key pair from a fixed test seed.
  const seed = new Uint8Array(32);
  for (let i = 0; i < 32; i++) seed[i] = (i * 7 + 3) & 0xff;
  const kp = nacl.sign.keyPair.fromSeed(seed);

  const messages = [
    ['empty message', new Uint8Array(0)],
    ['non-empty message', Uint8Array.from(Buffer.from('offline signature record #42', 'utf8'))]
  ];

  const undecodable = findUndecodableEncoding();

  for (const [msgLabel, msg] of messages) {
    const sig = nacl.sign.detached(msg, kp.secretKey);
    const validS = sig.subarray(32, 64);

    // -- Valid signature still verifies through every entry point. --------
    assert.strictEqual(nacl.sign.detached.verify(msg, sig, kp.publicKey), true,
      `${tag} ${msgLabel}: valid signature must verify`);
    const opened = nacl.sign.open(concat(sig, msg), kp.publicKey);
    assert.ok(opened !== null, `${tag} ${msgLabel}: sign.open must succeed`);
    assert.deepStrictEqual(opened, msg, `${tag} ${msgLabel}: opened message mismatch`);
    {
      const sm = concat(sig, msg);
      const m = new Uint8Array(sm.length);
      assert.strictEqual(nacl.lowlevel.crypto_sign_open(m, sm, sm.length, kp.publicKey),
        msg.length, `${tag} ${msgLabel}: lowlevel crypto_sign_open must return message length`);
      assert.deepStrictEqual(m.subarray(0, msg.length), msg,
        `${tag} ${msgLabel}: lowlevel opened message mismatch`);
    }

    const withS = (s) => { const c = Uint8Array.from(sig); c.set(s, 32); return c; };
    const withR = (r) => { const c = Uint8Array.from(sig); c.set(r, 0); return c; };

    // -- S scalar cases. ---------------------------------------------------
    //
    // Note: an in-range S (e.g. L-1) only passes the range pre-check; it is
    // NOT a valid signature for this R and message, so rejection below comes
    // from the actual curve equation check. The range check must never
    // replace verification itself.
    const sCases = [
      ['S + L (malleability variant)', addLE(validS, L)],
      ['S = L', Uint8Array.from(L)],
      ['S = L + 1', addLE(L, leConstant(1n))],
      ['S = L + 0x13 (low-byte carry into byte 1)', addLE(L, leConstant(0x13n))],
      ['S = 2*L', addLE(L, L)],
      ['S = all 0xff', new Uint8Array(32).fill(0xff)],
      // In-range values: rejected by the signature equation, not the range check.
      ['S = L - 1 (in range, equation must fail)', addLE(L, leConstant((1n << 256n) - 1n))],
      ['S = 2^252 (in range, equation must fail)', leConstant(1n << 252n)],
      ['S = 2^252 - 1 (in range, borrow boundary)', leConstant((1n << 252n) - 1n)],
      ['S = 0 (in range, equation must fail)', new Uint8Array(32)]
    ];
    for (const [desc, s] of sCases) {
      assertRejected(nacl, msg, withS(s), kp.publicKey, `${tag} ${msgLabel}: ${desc}`);
    }
    // Sentinel/subview coverage on the most important S case.
    assertRejectedWithSentinels(nacl, msg, withS(addLE(validS, L)), kp.publicKey,
      `${tag} ${msgLabel}: S + L with sentinel subviews`);

    // -- Public key A cases. ------------------------------------------------
    const identity = new Uint8Array(32); identity[0] = 1;
    const yEqualsP = leConstant(P_BIG);                 // y = p, non-canonical
    const yEqualsPplus1 = leConstant(P_BIG + 1n);       // y = p + 1, non-canonical
    const yAllOnes = leConstant((1n << 255n) - 1n);     // y = 2^255 - 1, non-canonical
    const yEqualsPsign1 = leConstant(P_BIG | (1n << 255n)); // non-canonical, sign bit set
    const identitySign1 = leConstant(1n | (1n << 255n));    // x = 0 with sign bit 1
    const smallOrder4 = new Uint8Array(32);             // y = 0: point of order 4
    const smallOrder2 = leConstant(P_BIG - 1n);         // y = -1: point of order 2
    const mixedOrderA = addTorsionComponent(kp.publicKey); // subgroup point + torsion

    // Rejecting the known small-order points below is necessary but NOT
    // sufficient: a blacklist of known points cannot cover all mixed-order
    // points. The implementation must do a full [L]P subgroup check, which
    // is what the mixed-order case exercises.
    const aCases = [
      ['A = identity element', identity],
      ['A with y = p (non-canonical)', yEqualsP],
      ['A with y = p + 1 (non-canonical)', yEqualsPplus1],
      ['A with y = 2^255 - 1 (non-canonical)', yAllOnes],
      ['A with y = p and sign bit set (non-canonical)', yEqualsPsign1],
      ['A = identity with sign bit 1 (x = 0, non-canonical sign)', identitySign1],
      ['A = known small-order point (y = 0, order 4)', smallOrder4],
      ['A = known small-order point (y = -1, order 2)', smallOrder2],
      ['A with torsion component (mixed order)', mixedOrderA],
      ['A not decodable to a curve point', undecodable]
    ];
    for (const [desc, pk] of aCases) {
      assertRejected(nacl, msg, sig, pk, `${tag} ${msgLabel}: ${desc}`);
    }
    assertRejectedWithSentinels(nacl, msg, sig, identity,
      `${tag} ${msgLabel}: identity public key with sentinel subviews`);

    // -- R (first half of the signature) cases. -----------------------------
    //
    // The neutral element IS allowed as R by the pre-checks; the case below
    // is rejected by the signature equation itself, not by the encoding or
    // subgroup checks.
    const rCases = [
      ['R = identity element (passes pre-checks, equation fails)', identity],
      ['R with y = p (non-canonical)', yEqualsP],
      ['R with y = 2^255 - 1 (non-canonical)', yAllOnes],
      ['R = identity with sign bit 1 (x = 0, non-canonical sign)', identitySign1],
      ['R = known small-order point (y = 0, order 4)', smallOrder4],
      ['R with torsion component (mixed order)', addTorsionComponent(sig.subarray(0, 32))],
      ['R not decodable to a curve point', undecodable]
    ];
    for (const [desc, r] of rCases) {
      assertRejected(nacl, msg, withR(r), kp.publicKey, `${tag} ${msgLabel}: ${desc}`);
    }
  }

  // Valid signature passed as subviews surrounded by sentinels must verify
  // and leave the surrounding bytes untouched.
  {
    const msg = Uint8Array.from(Buffer.from('sentinel acceptance', 'utf8'));
    const sig = nacl.sign.detached(msg, kp.secretKey);
    const sigStore = new Uint8Array(66);
    const pkStore = new Uint8Array(34);
    sigStore[0] = 0xa5; sigStore[65] = 0x5a;
    pkStore[0] = 0xc3; pkStore[33] = 0x3c;
    sigStore.subarray(1, 65).set(sig);
    pkStore.subarray(1, 33).set(kp.publicKey);
    assert.strictEqual(
      nacl.sign.detached.verify(msg, sigStore.subarray(1, 65), pkStore.subarray(1, 33)),
      true, `${tag}: valid signature via subviews must verify`);
    assert.strictEqual(sigStore[0], 0xa5);
    assert.strictEqual(sigStore[65], 0x5a);
    assert.strictEqual(pkStore[0], 0xc3);
    assert.strictEqual(pkStore[33], 0x3c);
  }

  console.log(`${tag} OK`);
}

for (const [name, nacl] of implementations) {
  runSuite(name, nacl);
}

console.log('sign-canonical-regression: all tests passed');
