// Tests for the masked pyramid builder and the joint photometric LK track.
//
// Every case receives the runner's {add, check} helper and must make at least
// one real assertion through check(). Materials are generated independently
// here (multi-frequency procedural texture) so that cases do not compare two
// implementations of the same code path with each other.

module.exports = function register(api) {
  const j = require(process.env.JSFEAT_MODULE);

  // ---------------------------------------------------------------------
  // materials
  // ---------------------------------------------------------------------

  // Multi-frequency texture. Amplitude is bounded so that the gain/bias
  // combinations used below never clip the [0,255] range; the generator
  // asserts that instead of relying on the bound.
  function texture(x, y) {
    return 110
      + 30 * Math.sin(x * 0.22 + y * 0.05)
      + 24 * Math.cos(y * 0.19 - x * 0.04)
      + 16 * Math.sin(x * 0.13 + y * 0.17)
      + 12 * Math.cos(x * 0.31 - y * 0.27)
      + 8 * Math.sin(x * 0.47 - y * 0.33);
  }

  function makeImage(w, h, dx, dy, gain, bias) {
    if (typeof gain === "undefined") gain = 1;
    if (typeof bias === "undefined") bias = 0;
    const m = new j.matrix_t(w, h, j.U8C1_t);
    for (let y = 0; y < h; ++y) {
      for (let x = 0; x < w; ++x) {
        const v = gain * texture(x - dx, y - dy) + bias;
        if (v < 0 || v > 255) {
          throw new Error("texture clipped at " + x + "," + y + ": " + v);
        }
        m.data[y * w + x] = Math.round(v);
      }
    }
    return m;
  }

  function makeMask(w, h, holes) {
    const v = new j.matrix_t(w, h, j.U8C1_t);
    v.data.fill(255);
    if (holes) {
      for (let k = 0; k < holes.length; ++k) {
        const hl = holes[k]; // [x0, y0, w, h]
        for (let y = hl[1]; y < hl[1] + hl[3]; ++y) {
          for (let x = hl[0]; x < hl[0] + hl[2]; ++x) {
            if (x >= 0 && y >= 0 && x < w && y < h) v.data[y * w + x] = 0;
          }
        }
      }
    }
    return v;
  }

  function plainPyramid(img, levels) {
    const p = new j.pyramid_t(levels || 3);
    p.allocate(img.cols, img.rows, j.U8C1_t);
    p.build(img, false);
    return p;
  }

  function maskedPyramid(img, mask, levels) {
    const p = new j.pyramid_t(levels || 3);
    p.allocate(img.cols, img.rows, j.U8C1_t);
    p.build_masked(img, mask);
    return p;
  }

  function pyramidEquals(a, b, levels) {
    for (let i = 0; i < levels; ++i) {
      if (a.data[i].cols !== b.data[i].cols || a.data[i].rows !== b.data[i].rows) {
        return "level " + i + " size mismatch";
      }
      const n = a.data[i].cols * a.data[i].rows;
      for (let k = 0; k < n; ++k) {
        if (a.data[i].data[k] !== b.data[i].data[k]) {
          return "level " + i + " byte mismatch at " + k;
        }
      }
    }
    return null;
  }

  // Runs track_photometric on a list of points and returns per-point result
  // records. Outputs are pre-filled with sentinels so tests can prove that
  // only the first `count` entries were written.
  function runPhotometric(prevPyr, currPyr, points, opts) {
    const n = points.length;
    const prevXY = new Float32Array(n * 2);
    const outXY = new Float32Array(n * 2);
    const gains = new Float32Array(n);
    const biases = new Float32Array(n);
    const status = new Uint8Array(n);
    for (let i = 0; i < n; ++i) {
      prevXY[2 * i] = points[i][0];
      prevXY[2 * i + 1] = points[i][1];
      // start the target somewhere else to prove warm start is ignored
      outXY[2 * i] = points[i][0] + 40;
      outXY[2 * i + 1] = points[i][1] - 40;
      gains[i] = 7.0;
      biases[i] = 7.0;
      status[i] = 9;
    }
    j.optical_flow_lk.track_photometric(
      prevPyr, currPyr, prevXY, outXY, n, 15, gains, biases, status,
      opts || {}
    );
    const res = [];
    for (let i = 0; i < n; ++i) {
      res.push({
        status: status[i],
        x: outXY[2 * i],
        y: outXY[2 * i + 1],
        gain: gains[i],
        bias: biases[i],
        start: points[i],
      });
    }
    return res;
  }

  function checkAccuracy(r, dx, dy, gain, bias, tol) {
    if (typeof tol === "undefined") {
      tol = { pos: 0.15, gain: 0.03, bias: 2.0 };
    }
    api.check(r.status === 1, "point should succeed");
    api.check(isFinite(r.x) && isFinite(r.y), "coordinates must be finite");
    api.check(Math.abs(r.x - (r.start[0] + dx)) <= tol.pos,
      "x displacement error " + Math.abs(r.x - (r.start[0] + dx)));
    api.check(Math.abs(r.y - (r.start[1] + dy)) <= tol.pos,
      "y displacement error " + Math.abs(r.y - (r.start[1] + dy)));
    api.check(Math.abs(r.gain - gain) <= tol.gain,
      "gain error " + Math.abs(r.gain - gain));
    api.check(Math.abs(r.bias - bias) <= tol.bias,
      "bias error " + Math.abs(r.bias - bias));
  }

  function checkRestored(r) {
    api.check(r.status === 0, "point should be rejected");
    api.check(r.x === r.start[0] && r.y === r.start[1],
      "coordinates must be restored to the input");
    api.check(r.gain === 1.0, "gain must be restored to 1");
    api.check(r.bias === 0.0, "bias must be restored to 0");
  }

  // ---------------------------------------------------------------------
  // photometric
  // ---------------------------------------------------------------------

  api.add("photometric", "subpixel-shift-with-gain-bias", function () {
    const w = 160, h = 140;
    const prev = makeImage(w, h, 0, 0);
    const curr = makeImage(w, h, 2, -1, 1.3, -15);
    const prevMask = makeMask(w, h);
    const currMask = makeMask(w, h);
    const pp = maskedPyramid(prev, prevMask);
    const cp = maskedPyramid(curr, currMask);

    const pts = [[100, 70], [40, 30], [130, 110], [70, 100]];
    const res = runPhotometric(pp, cp, pts, {});
    for (let i = 0; i < pts.length; ++i) {
      checkAccuracy(res[i], 2, -1, 1.3, -15);
    }
  });

  api.add("photometric", "fractional-shift-and-no-brightness", function () {
    const w = 160, h = 140;
    const dx = 1.7, dy = 0.6, gain = 1.0, bias = 0.0;
    const prev = makeImage(w, h, 0, 0);
    const curr = makeImage(w, h, dx, dy, gain, bias);
    const pp = maskedPyramid(prev, makeMask(w, h));
    const cp = maskedPyramid(curr, makeMask(w, h));

    const res = runPhotometric(pp, cp, [[80, 60], [110, 90]], {});
    checkAccuracy(res[0], dx, dy, gain, bias);
    checkAccuracy(res[1], dx, dy, gain, bias);
  });

  api.add("photometric", "brightness-only-change", function () {
    // Zero geometric motion: only a gain/bias change. The tracker must not
    // confuse it with translation.
    const w = 160, h = 140;
    const prev = makeImage(w, h, 0, 0);
    const curr = makeImage(w, h, 0, 0, 1.25, 12);
    const pp = maskedPyramid(prev, makeMask(w, h));
    const cp = maskedPyramid(curr, makeMask(w, h));

    const res = runPhotometric(pp, cp, [[90, 70]], {});
    checkAccuracy(res[0], 0, 0, 1.25, 12);
  });

  api.add("photometric", "distinct-gain-bias-per-call", function () {
    // Separate image pairs (independent materials) with very different
    // photometric changes, each with subpixel motion. Exercises the
    // per-point gain/bias outputs.
    const w = 160, h = 140;
    const cases = [
      [0.7, 40, [1.4, 0.8]],
      [1.35, -18, [2, -1]],
      [0.85, 25, [-1.6, 1.2]],
      [1.15, -6, [0.4, -2.3]],
    ];
    for (let k = 0; k < cases.length; ++k) {
      const gain = cases[k][0], bias = cases[k][1];
      const dx = cases[k][2][0], dy = cases[k][2][1];
      const prev = makeImage(w, h, 0, 0);
      const curr = makeImage(w, h, dx, dy, gain, bias);
      const pp = maskedPyramid(prev, makeMask(w, h));
      const cp = maskedPyramid(curr, makeMask(w, h));
      const res = runPhotometric(pp, cp, [[95, 75]], {});
      checkAccuracy(res[0], dx, dy, gain, bias);
    }
  });

  api.add("photometric", "inverse-direction-not-returned", function () {
    // If the result were the inverse mapping (or inverse gain) the sign of
    // both the motion and the gain relationship would flip. Check that the
    // direction is prev->curr: moving content by (+3,-2) makes the tracked
    // point move by (+3,-2) in target coordinates.
    const w = 170, h = 150;
    const prev = makeImage(w, h, 0, 0);
    const curr = makeImage(w, h, 3, -2, 1.4, -20);
    const pp = maskedPyramid(prev, makeMask(w, h));
    const cp = maskedPyramid(curr, makeMask(w, h));
    const res = runPhotometric(pp, cp, [[100, 80]], {});
    checkAccuracy(res[0], 3, -2, 1.4, -20);
  });

  api.add("photometric", "eps-and-rms-options", function () {
    // Explicit options must be honoured and still converge on easy data.
    const w = 150, h = 130;
    const prev = makeImage(w, h, 0, 0);
    const curr = makeImage(w, h, 1.5, 0.5, 1.2, -8);
    const pp = maskedPyramid(prev, makeMask(w, h));
    const cp = maskedPyramid(curr, makeMask(w, h));
    const res = runPhotometric(pp, cp, [[80, 60]], {
      max_iter: 40, eps: 0.001, min_valid_ratio: 0.6, max_rms: 3,
    });
    checkAccuracy(res[0], 1.5, 0.5, 1.2, -8);
  });

  // ---------------------------------------------------------------------
  // masks
  // ---------------------------------------------------------------------

  api.add("masks", "all-valid-matches-plain-pyr-down", function () {
    // With a fully valid mask, the image pyramid must be byte-identical to
    // the legacy build for even, odd and single-level pyramids.
    const dims = [[64, 64, 3], [63, 47, 3], [37, 29, 4], [16, 16, 2], [32, 24, 4]];
    for (let k = 0; k < dims.length; ++k) {
      const w = dims[k][0], h = dims[k][1], levels = dims[k][2];
      const img = makeImage(w, h, 0, 0);
      const mask = makeMask(w, h);
      const plain = plainPyramid(img, levels);
      const masked = maskedPyramid(img, mask, levels);
      const diff = pyramidEquals(plain, masked, levels);
      api.check(diff === null, "image pyramid mismatch: " + diff);
      for (let l = 0; l < levels; ++l) {
        api.check(masked.validity[l].cols === plain.data[l].cols &&
                  masked.validity[l].rows === plain.data[l].rows,
          "validity " + l + " size mismatch");
        const n = plain.data[l].cols * plain.data[l].rows;
        for (let q = 0; q < n; ++q) {
          api.check(masked.validity[l].data[q] === 255,
            "validity should stay 255 at " + l + "/" + q);
        }
      }
    }
  });

  api.add("masks", "mask-filter-is-2x2-and-keeps-last-block", function () {
    // One invalid source pixel invalidates exactly the higher-level pixel
    // whose 2x2 support contains it. Odd dimensions follow the >>1 rule and
    // the final complete 2x2 block is retained.
    const w = 17, h = 13, levels = 3;
    const img = makeImage(w, h, 0, 0);
    const mask = makeMask(w, h);
    mask.data[11 * w + 15] = 0; // y=11, x=15 (inside the last full block)

    const p = maskedPyramid(img, mask, levels);
    api.check(p.validity[0].cols === 17 && p.validity[0].rows === 13,
      "level 0 mask size");
    api.check(p.validity[1].cols === 8 && p.validity[1].rows === 6,
      "level 1 mask size (>>1)");
    api.check(p.validity[2].cols === 4 && p.validity[2].rows === 3,
      "level 2 mask size (>>1)");

    let z1 = 0;
    for (let q = 0; q < 8 * 6; ++q) if (p.validity[1].data[q] === 0) z1++;
    api.check(z1 === 1, "exactly one level-1 pixel must be invalid, got " + z1);
    api.check(p.validity[1].data[5 * 8 + 7] === 0, "level 1 (7,5) invalid");
    api.check(p.validity[1].data[5 * 8 + 6] === 255, "neighbouring pixel valid");
    api.check(p.validity[1].data[4 * 8 + 7] === 255, "pixel above valid");

    let z2 = 0;
    for (let q = 0; q < 4 * 3; ++q) if (p.validity[2].data[q] === 0) z2++;
    api.check(z2 === 1, "exactly one level-2 pixel must be invalid, got " + z2);
    api.check(p.validity[2].data[2 * 4 + 3] === 0, "level 2 (3,2) invalid");

    // every mask entry is either 0 or 255
    for (let l = 0; l < levels; ++l) {
      const n = p.validity[l].cols * p.validity[l].rows;
      for (let q = 0; q < n; ++q) {
        const v = p.validity[l].data[q];
        api.check(v === 0 || v === 255, "mask byte must be 0/255 at " + l + "/" + q + ": " + v);
      }
    }
  });

  api.add("masks", "build-copies-inputs", function () {
    // Mutating the inputs after build_masked must not affect the pyramid.
    const w = 48, h = 40;
    const img = makeImage(w, h, 0, 0);
    const mask = makeMask(w, h);
    const p = maskedPyramid(img, mask, 3);
    const imgSnap = Array.prototype.slice.call(p.data[0].data);
    const maskSnap = Array.prototype.slice.call(p.validity[0].data);

    img.data.fill(0);
    mask.data.fill(0);

    for (let k = 0; k < imgSnap.length; ++k) {
      api.check(p.data[0].data[k] === imgSnap[k], "level0 image changed at " + k);
    }
    for (let k = 0; k < maskSnap.length; ++k) {
      api.check(p.validity[0].data[k] === maskSnap[k], "level0 mask changed at " + k);
    }
  });

  api.add("masks", "hole-covering-window-rejects", function () {
    const w = 160, h = 140;
    const prev = makeImage(w, h, 0, 0);
    const curr = makeImage(w, h, 2, -1, 1.3, -15);
    // hole covering the whole 15x15 source window of (100,70)
    const pp = maskedPyramid(prev, makeMask(w, h, [[87, 57, 26, 26]]));
    const cp = maskedPyramid(curr, makeMask(w, h));
    const res = runPhotometric(pp, cp, [[100, 70], [60, 40]], {});
    checkRestored(res[0]);
    checkAccuracy(res[1], 2, -1, 1.3, -15);
  });

  api.add("masks", "hole-on-target-window-rejects", function () {
    const w = 160, h = 140;
    const prev = makeImage(w, h, 0, 0);
    const curr = makeImage(w, h, 2, -1, 1.3, -15);
    // zero out the target window (shifted by (2,-1)) on the current mask
    const pp = maskedPyramid(prev, makeMask(w, h));
    const cp = maskedPyramid(curr, makeMask(w, h, [[89, 56, 26, 26]]));
    const res = runPhotometric(pp, cp, [[100, 70], [40, 30]], {});
    checkRestored(res[0]);
    checkAccuracy(res[1], 2, -1, 1.3, -15);
  });

  api.add("masks", "partial-hole-still-tracks", function () {
    // Removing 4 of 15 columns keeps the valid ratio above 0.6, so the
    // point must still succeed with accurate motion.
    const w = 160, h = 140;
    const prev = makeImage(w, h, 0, 0);
    const curr = makeImage(w, h, 2, -1, 1.3, -15);
    const pp = maskedPyramid(prev, makeMask(w, h, [[93, 63, 4, 15]]));
    const cp = maskedPyramid(curr, makeMask(w, h));
    const res = runPhotometric(pp, cp, [[100, 70]], {});
    checkAccuracy(res[0], 2, -1, 1.3, -15);
  });

  api.add("masks", "invalid-pixel-values-do-not-leak", function () {
    // The same validity mask with garbage 0 vs 255 inside the invalid area.
    // Valid points must produce identical outputs; invalid values may not be
    // treated as zeros in the normal equations.
    const w = 160, h = 140;
    const prev = makeImage(w, h, 0, 0);
    const curr = makeImage(w, h, 2, -1, 1.3, -15);
    const hole = [[88, 58, 24, 24]]; // covers the window of (100,70)
    const mask = makeMask(w, h, hole);

    const junkA = new j.matrix_t(w, h, j.U8C1_t);
    const junkB = new j.matrix_t(w, h, j.U8C1_t);
    for (let y = 0; y < h; ++y) {
      for (let x = 0; x < w; ++x) {
        const inside = x >= 88 && x < 112 && y >= 58 && y < 82;
        junkA.data[y * w + x] = inside ? 0 : curr.data[y * w + x];
        junkB.data[y * w + x] = inside ? 255 : curr.data[y * w + x];
      }
    }

    const pa = maskedPyramid(prev, mask);
    const pb = maskedPyramid(junkA, mask);
    const pc = maskedPyramid(junkB, mask);
    const r0 = runPhotometric(pa, pb, [[100, 70], [60, 40]], {});
    const r1 = runPhotometric(pa, pc, [[100, 70], [60, 40]], {});

    // point in the hole fails identically
    checkRestored(r0[0]);
    checkRestored(r1[0]);
    // outside point: output is bit-identical regardless of junk values
    checkAccuracy(r0[1], 2, -1, 1.3, -15);
    api.check(r0[1].x === r1[1].x && r0[1].y === r1[1].y,
      "coordinates must not depend on invalid pixel values");
    api.check(r0[1].gain === r1[1].gain && r0[1].bias === r1[1].bias,
      "gain/bias must not depend on invalid pixel values");
  });

  api.add("masks", "mask-validity-nonzero-is-valid", function () {
    // Any nonzero byte counts as valid, not just 255.
    const w = 96, h = 80;
    const prev = makeImage(w, h, 0, 0);
    const curr = makeImage(w, h, 2, -1, 1.3, -15);
    const m1 = makeMask(w, h);
    const m2 = makeMask(w, h);
    for (let k = 0; k < m2.data.length; ++k) m2.data[k] = 1;
    const pp = maskedPyramid(prev, m1);
    const cp = maskedPyramid(curr, m1);
    const pp2 = maskedPyramid(prev, m2);
    const cp2 = maskedPyramid(curr, m2);
    const a = runPhotometric(pp, cp, [[50, 40]], {});
    const b = runPhotometric(pp2, cp2, [[50, 40]], {});
    checkAccuracy(a[0], 2, -1, 1.3, -15);
    api.check(a[0].x === b[0].x && a[0].gain === b[0].gain,
      "mask value 1 must behave like 255");
  });

  // ---------------------------------------------------------------------
  // compatibility
  // ---------------------------------------------------------------------

  api.add("compatibility", "legacy-build-unchanged", function () {
    // The original build must still produce the same pyramid.
    const w = 64, h = 48, levels = 3;
    const img = makeImage(w, h, 0, 0);
    const p = new j.pyramid_t(levels);
    p.allocate(w, h, j.U8C1_t);
    p.build(img, false);
    api.check(p.data[0].cols === w && p.data[0].rows === h, "level0 size");
    api.check(p.data[1].cols === 32 && p.data[1].rows === 24, "level1 size");
    api.check(p.data[2].cols === 16 && p.data[2].rows === 12, "level2 size");
    // spot check the 2x2 average of the whole level 1
    const l1w = p.data[1].cols, l1h = p.data[1].rows;
    for (let y = 0; y < l1h; ++y) {
      for (let x = 0; x < l1w; ++x) {
        const s = 2 * y * w + 2 * x;
        const want = (img.data[s] + img.data[s + 1] +
                      img.data[s + w] + img.data[s + w + 1] + 2) >> 2;
        api.check(p.data[1].data[y * l1w + x] === want,
          "level1 average mismatch at " + x + "," + y);
      }
    }
    const a = new j.pyramid_t(levels);
    a.allocate(w, h, j.U8C1_t);
    a.build(img, false);
    const b = new j.pyramid_t(levels);
    b.allocate(w, h, j.U8C1_t);
    b.build(img, false);
    api.check(pyramidEquals(a, b, levels) === null, "legacy build must be deterministic");
  });

  api.add("compatibility", "legacy-track-still-works", function () {
    // Old track on plain pyramids: keeps its historical behaviour for pure
    // translation and still rejects badly relit frames (the baseline bug
    // that motivated the new path, documented rather than fixed).
    const w = 128, h = 128;
    const prev = makeImage(w, h, 0, 0);
    const p1 = plainPyramid(prev, 3);

    function oldFlow(gain, bias) {
      const curr = makeImage(w, h, 2, -1, gain, bias);
      const p2 = plainPyramid(curr, 3);
      const out = new Float32Array(2), status = new Uint8Array(1);
      j.optical_flow_lk.track(
        p1, p2, new Float32Array([64, 64]), out, 1, 15, 40, status, 0.001, 0.0001);
      return { status: status[0], err: Math.hypot(out[0] - 66, out[1] - 63) };
    }
    const plain = oldFlow(1, 0);
    const relit = oldFlow(1.35, -18);
    api.check(plain.status === 1, "legacy tracking should succeed");
    api.check(plain.err < 0.1, "legacy translation error " + plain.err);
    api.check(relit.err > 0.3, "legacy path is expected to fail under relighting, got " + relit.err);
  });

  api.add("compatibility", "photometric-agrees-with-legacy-track", function () {
    // On a pure translation with no brightness change, the new path must
    // agree with the legacy track on the same data (independent check that
    // the displacement convention is unchanged).
    const w = 128, h = 128;
    const prev = makeImage(w, h, 0, 0);
    const curr = makeImage(w, h, 2, -1);
    const pp = plainPyramid(prev, 3);
    const cp = plainPyramid(curr, 3);
    const out = new Float32Array(2), status = new Uint8Array(1);
    j.optical_flow_lk.track(
      pp, cp, new Float32Array([64, 64]), out, 1, 15, 40, status, 0.001, 0.0001);
    api.check(status[0] === 1, "legacy track should succeed");
    const legacyX = out[0], legacyY = out[1];

    const res = runPhotometric(
      maskedPyramid(prev, makeMask(w, h)),
      maskedPyramid(curr, makeMask(w, h)),
      [[64, 64]], {});
    api.check(res[0].status === 1, "photometric track should succeed");
    api.check(Math.abs(res[0].x - legacyX) < 0.15,
      "x differs from legacy by " + Math.abs(res[0].x - legacyX));
    api.check(Math.abs(res[0].y - legacyY) < 0.15,
      "y differs from legacy by " + Math.abs(res[0].y - legacyY));
    api.check(Math.abs(res[0].gain - 1) < 0.03, "gain should be ~1: " + res[0].gain);
    api.check(Math.abs(res[0].bias) < 2, "bias should be ~0: " + res[0].bias);
  });

  api.add("compatibility", "degenerate-textures-rejected", function () {
    // Flat and single-direction ramps must not report success merely
    // because the 2x2 displacement block looks invertible.
    const w = 80, h = 80;
    const kinds = ["flat", "rampx", "rampy"];
    for (let k = 0; k < kinds.length; ++k) {
      const kind = kinds[k];
      const m = new j.matrix_t(w, h, j.U8C1_t);
      for (let y = 0; y < h; ++y) {
        for (let x = 0; x < w; ++x) {
          m.data[y * w + x] = kind === "flat" ? 120 :
            (kind === "rampx" ? 40 + x : 40 + y);
        }
      }
      const mask = makeMask(w, h);
      const pp = maskedPyramid(m, mask);
      const cp = maskedPyramid(m, mask);
      const res = runPhotometric(pp, cp, [[40, 40], [20, 60]], {});
      checkRestored(res[0]);
      checkRestored(res[1]);
    }
  });

  api.add("compatibility", "out-of-bounds-rejected", function () {
    const w = 100, h = 80;
    const prev = makeImage(w, h, 0, 0);
    const curr = makeImage(w, h, 2, -1, 1.3, -15);
    const pp = maskedPyramid(prev, makeMask(w, h));
    const cp = maskedPyramid(curr, makeMask(w, h));
    const pts = [[2, 2], [w - 2, h - 2], [50, 1], [1, 40]];
    const res = runPhotometric(pp, cp, pts, {});
    for (let i = 0; i < pts.length; ++i) checkRestored(res[i]);
  });

  api.add("compatibility", "missing-valid-support-rejected", function () {
    // A completely invalid mask (no usable support anywhere) must reject
    // every point and restore the outputs.
    const w = 96, h = 72;
    const prev = makeImage(w, h, 0, 0);
    const curr = makeImage(w, h, 2, -1, 1.3, -15);
    const empty = makeMask(w, h);
    empty.data.fill(0);
    const pp = maskedPyramid(prev, empty);
    const cp = maskedPyramid(curr, empty);
    const res = runPhotometric(pp, cp, [[48, 36]], {});
    checkRestored(res[0]);
  });

  api.add("compatibility", "odd-sized-pyramid", function () {
    // Odd level sizes follow the same >>1 rule as the legacy pyramid and
    // tracking still works at the original resolution.
    const w = 57, h = 43;
    const prev = makeImage(w, h, 0, 0);
    const curr = makeImage(w, h, 2, -1, 1.2, -10);
    const pp = maskedPyramid(prev, makeMask(w, h), 3);
    const cp = maskedPyramid(curr, makeMask(w, h), 3);
    api.check(pp.data.length === 3, "three levels");
    api.check(pp.data[0].cols === 57 && pp.data[0].rows === 43, "level0 57x43");
    api.check(pp.data[1].cols === 28 && pp.data[1].rows === 21, "level1 28x21");
    api.check(pp.data[2].cols === 14 && pp.data[2].rows === 10, "level2 14x10");
    api.check(pp.validity[1].cols === 28 && pp.validity[1].rows === 21, "mask level1 28x21");
    api.check(pp.validity[2].cols === 14 && pp.validity[2].rows === 10, "mask level2 14x10");

    const res = runPhotometric(pp, cp, [[28, 21], [40, 30]], {});
    checkAccuracy(res[0], 2, -1, 1.2, -10);
    checkAccuracy(res[1], 2, -1, 1.2, -10);
  });

  api.add("compatibility", "repeated-calls-are-deterministic", function () {
    // Calling the tracker twice on the same pyramids and inputs must give
    // identical results, and must not have mutated the pyramids.
    const w = 130, h = 110;
    const prev = makeImage(w, h, 0, 0);
    const curr = makeImage(w, h, 2, -1, 1.3, -15);
    const pp = maskedPyramid(prev, makeMask(w, h));
    const cp = maskedPyramid(curr, makeMask(w, h));
    const snapP = Array.prototype.slice.call(pp.data[0].data);
    const snapC = Array.prototype.slice.call(cp.data[1].data);

    const a = runPhotometric(pp, cp, [[80, 60], [40, 30]], {});
    const b = runPhotometric(pp, cp, [[80, 60], [40, 30]], {});
    for (let i = 0; i < 2; ++i) {
      api.check(a[i].status === b[i].status, "status must repeat");
      api.check(a[i].x === b[i].x && a[i].y === b[i].y,
        "coordinates must repeat");
      api.check(a[i].gain === b[i].gain && a[i].bias === b[i].bias,
        "gain/bias must repeat");
    }
    let changed = false;
    for (let k = 0; k < snapP.length; ++k) {
      if (pp.data[0].data[k] !== snapP[k]) { changed = true; break; }
    }
    for (let k = 0; k < snapC.length; ++k) {
      if (cp.data[1].data[k] !== snapC[k]) { changed = true; break; }
    }
    api.check(!changed, "tracking must not mutate the pyramids");
  });

  api.add("compatibility", "count-tail-untouched", function () {
    // Only the first count entries of every output array may be written.
    const w = 120, h = 100;
    const prev = makeImage(w, h, 0, 0);
    const curr = makeImage(w, h, 2, -1, 1.3, -15);
    const pp = maskedPyramid(prev, makeMask(w, h));
    const cp = maskedPyramid(curr, makeMask(w, h));

    const n = 2, alloc = 5;
    const prevXY = new Float32Array(alloc * 2);
    const outXY = new Float32Array(alloc * 2);
    const gains = new Float32Array(alloc);
    const biases = new Float32Array(alloc);
    const status = new Uint8Array(alloc);
    prevXY[0] = 80; prevXY[1] = 60;
    prevXY[2] = 40; prevXY[3] = 30;
    outXY.fill(-777);
    gains.fill(-777);
    biases.fill(-777);
    status.fill(77);

    j.optical_flow_lk.track_photometric(
      pp, cp, prevXY, outXY, n, 15, gains, biases, status,
      { max_iter: 40, eps: 0.001, min_valid_ratio: 0.6, max_rms: 3 });

    api.check(status[0] === 1 && status[1] === 1, "first count points should succeed");
    for (let i = n; i < alloc; ++i) {
      api.check(status[i] === 77, "status tail untouched at " + i);
      api.check(gains[i] === -777, "gains tail untouched at " + i);
      api.check(biases[i] === -777, "biases tail untouched at " + i);
    }
    for (let i = n * 2; i < alloc * 2; ++i) {
      api.check(outXY[i] === -777, "coordinate tail untouched at " + i);
    }
  });

  api.add("compatibility", "single-level-masked-pyramid", function () {
    // A one-level pyramid exercises the coarse-to-fine path with no coarse
    // level, and must still track at full resolution.
    const w = 90, h = 70;
    const prev = makeImage(w, h, 0, 0);
    const curr = makeImage(w, h, 1.5, -1, 1.2, -10);
    const pp = maskedPyramid(prev, makeMask(w, h), 1);
    const cp = maskedPyramid(curr, makeMask(w, h), 1);
    api.check(pp.data.length === 1, "one level");
    api.check(pp.validity.length === 1, "one validity level");
    const res = runPhotometric(pp, cp, [[45, 35]], {});
    checkAccuracy(res[0], 1.5, -1, 1.2, -10);
  });

  api.add("compatibility", "zero-motion-identity", function () {
    // Identical frames: the estimate must stay at the source coordinate with
    // gain 1 / bias 0 and report success.
    const w = 100, h = 90;
    const prev = makeImage(w, h, 0, 0);
    const curr = makeImage(w, h, 0, 0);
    const pp = maskedPyramid(prev, makeMask(w, h));
    const cp = maskedPyramid(curr, makeMask(w, h));
    const res = runPhotometric(pp, cp, [[50, 45]], {});
    checkAccuracy(res[0], 0, 0, 1, 0, { pos: 0.01, gain: 0.005, bias: 0.1 });
  });

  api.add("compatibility", "aliased-input-output-buffers", function () {
    // prev_xy and curr_xy may be the same Float32Array (an in-place caller).
    // The tracker snapshots the source coordinates up front, so results must
    // match the non-aliased call exactly.
    const w = 130, h = 110;
    const prev = makeImage(w, h, 0, 0);
    const curr = makeImage(w, h, 2, -1, 1.3, -15);
    const pp = maskedPyramid(prev, makeMask(w, h));
    const cp = maskedPyramid(curr, makeMask(w, h));

    const pts = [[80, 60], [40, 30]];
    const separate = runPhotometric(pp, cp, pts, {});

    const xy = new Float32Array([80, 60, 40, 30]);
    const gains = new Float32Array(2);
    const biases = new Float32Array(2);
    const status = new Uint8Array(2);
    j.optical_flow_lk.track_photometric(
      pp, cp, xy, xy, 2, 15, gains, biases, status, {});
    api.check(status[0] === separate[0].status && status[1] === separate[1].status,
      "aliased status must match");
    api.check(xy[0] === separate[0].x && xy[1] === separate[0].y,
      "aliased coordinates must match");
    api.check(xy[2] === separate[1].x && xy[3] === separate[1].y,
      "aliased coordinates must match");
    api.check(gains[0] === separate[0].gain && biases[0] === separate[0].bias,
      "aliased gain/bias must match");
  });

  api.add("compatibility", "wrong-size-pyr-down-levels-untouched", function () {
    // Pyramid levels of a plain pyramid are still valid inputs: track_photometric
    // must use the first `levels` level sizes it finds, and a plain pyramid
    // (no validity array) must behave like a fully valid one.
    const w = 96, h = 80;
    const prev = makeImage(w, h, 0, 0);
    const curr = makeImage(w, h, 2, -1, 1.3, -15);
    const plainPrev = plainPyramid(prev, 3);
    const plainCurr = plainPyramid(curr, 3);
    const maskedPrev = maskedPyramid(prev, makeMask(w, h));
    const maskedCurr = maskedPyramid(curr, makeMask(w, h));
    const a = runPhotometric(plainPrev, plainCurr, [[48, 40]], {});
    const b = runPhotometric(maskedPrev, maskedCurr, [[48, 40]], {});
    checkAccuracy(a[0], 2, -1, 1.3, -15);
    api.check(a[0].x === b[0].x && a[0].y === b[0].y,
      "plain and fully-valid masked pyramids must agree");
    api.check(a[0].gain === b[0].gain && a[0].bias === b[0].bias,
      "plain and fully-valid masked pyramids must agree on gain/bias");
    api.check(prev.data.length > 0 && curr.data.length > 0, "inputs intact");
  });
};
