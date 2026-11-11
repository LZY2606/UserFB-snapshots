//
// tests/probe-volume-test.cpp
//
// Numeric checks for SHL::ProbeVolume. Expected values are closed-form
// results derived from the known synthetic light fields, never copied from
// ProbeVolume::Sample itself.
//

#include "ProbeVolume.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <limits>
#include <string>
#include <vector>

using namespace SHL;

namespace
{
    int gFailures = 0;

    const float kTol = 2e-4f;

    void Check(bool condition, const char* message)
    {
        if (!condition)
        {
            std::printf("FAIL: %s\n", message);
            gFailures++;
        }
    }

    void Near(Vec3f got, Vec3f expected, float tol, const char* message)
    {
        Vec3f d = got - expected;

        if (std::fabs(d[0]) > tol || std::fabs(d[1]) > tol || std::fabs(d[2]) > tol)
        {
            std::printf
            (
                "FAIL: %s -- got (%.7f %.7f %.7f) expected (%.7f %.7f %.7f)\n",
                message, got[0], got[1], got[2], expected[0], expected[1], expected[2]
            );
            gFailures++;
        }
    }

    bool ThrowsInvalidArg(const std::function<void()>& f)
    {
        try
        {
            f();
        }
        catch (const std::invalid_argument&)
        {
            return true;
        }
        catch (...)
        {
            return false;
        }

        return false;
    }

    const float kY00 = kSH_Y[0];
    const float kY1  = kSH_Y[3];  // band-1 normalization, same for x/y/z
    const float kY2xy = kSH_Y[4]; // xy basis normalization
    const float kA1 = 2.0f / 3.0f;
    const float kA2 = 1.0f / 4.0f;

    Vec4f kZero[9];

    // This drop of VL234f declares Mat3f's copy assignment without defining it,
    // so matrices are copied element by element instead.
    void SetMat3f(Mat3f& dst, const Mat3f& src)
    {
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                dst[i][j] = src[i][j];
    }

    void SetMat3f(Mat3f& dst, float a, float b, float c,
                  float d, float e, float f,
                  float g, float h, float i)
    {
        dst[0][0] = a; dst[0][1] = b; dst[0][2] = c;
        dst[1][0] = d; dst[1][1] = e; dst[1][2] = f;
        dst[2][0] = g; dst[2][1] = h; dst[2][2] = i;
    }

    void AssignProbe(IrradianceProbe& dst, const IrradianceProbe& src)
    {
        dst.valid = src.valid;
        SetMat3f(dst.localToWorld, src.localToWorld);

        for (int c = 0; c < 9; c++)
            dst.radiance[c] = src.radiance[c];
    }

    void ClearField(Vec4f c[9])
    {
        for (int i = 0; i < 9; i++)
            c[i] = kZero[i];
    }

    // Un-convolved world-frame radiance fields. All are grey (r=g=b).

    void ConstantField(float value, Vec4f c[9])
    {
        ClearField(c);
        c[0] = Vec4f(value / kY00, value / kY00, value / kY00, 0.0f);
    }

    void LinearField(Vec3f d, Vec4f c[9])  // f(n) = n . d
    {
        ClearField(c);
        c[1] = Vec4f(d[1] / kY1, d[1] / kY1, d[1] / kY1, 0.0f);  // y
        c[2] = Vec4f(d[2] / kY1, d[2] / kY1, d[2] / kY1, 0.0f);  // z
        c[3] = Vec4f(d[0] / kY1, d[0] / kY1, d[0] / kY1, 0.0f);  // x
    }

    void XyField(float a, Vec4f c[9])  // f(n) = a * n_x n_y
    {
        ClearField(c);
        c[4] = Vec4f(a / kY2xy, a / kY2xy, a / kY2xy, 0.0f);
    }

    // Build a probe storing the given world-frame field in the local frame R.
    IrradianceProbe MakeProbe(const Mat3f& R, const Vec4f world[9])
    {
        IrradianceProbe p;
        p.valid = true;
        SetMat3f(p.localToWorld, R);
        CRotateSH(trans(R), 3, world, p.radiance);  // local(e) = world(R e)
        return p;
    }

    IrradianceProbe MakeMissing()
    {
        const float q = std::numeric_limits<float>::quiet_NaN();
        IrradianceProbe p;
        p.valid = false;
        SetMat3f(p.localToWorld, q, q, q, q, q, q, q, q, q);  // must never be read

        for (int i = 0; i < 9; i++)
            p.radiance[i] = Vec4f(q, q, q, q);

        return p;
    }

    const Mat3f kIdentity(vl_I);
    const Mat3f kRz90(0.0f, -1.0f, 0.0f,
                      1.0f,  0.0f, 0.0f,
                      0.0f,  0.0f, 1.0f);
    const Mat3f kRx90(1.0f, 0.0f,  0.0f,
                      0.0f, 0.0f, -1.0f,
                      0.0f, 1.0f,  0.0f);

    // T1: constant radiance 2 survives any orientation, normal and position.
    void TestConstant()
    {
        Vec4f field[9];
        ConstantField(2.0f, field);

        std::vector<IrradianceProbe> probes;
        Mat3f frames[] = { kIdentity, kRz90, kRx90 };

        for (int i = 0; i < 8; i++)
            probes.push_back(MakeProbe(frames[i % 3], field));

        ProbeVolume volume({2, 2, 2}, Vec3f(0.0f, 0.0f, 0.0f), Vec3f(1.0f, 1.0f, 1.0f), probes);

        Vec3f positions[] =
        {
            Vec3f(0.0f, 0.0f, 0.0f),
            Vec3f(0.5f, 0.5f, 0.5f),
            Vec3f(-10.0f, 7.0f, -3.0f),  // outside -> clamped
            Vec3f(4.0f, 4.0f, 4.0f)
        };
        Vec3f normals[] =
        {
            Vec3f(1.0f, 0.0f, 0.0f),
            Vec3f(0.0f, -1.0f, 0.0f),
            Vec3f(0.3f, -0.4f, 1.2f),     // non-unit
            Vec3f(-2.0f, 3.0f, -5.0f)
        };

        for (const Vec3f& pos : positions)
            for (const Vec3f& nrm : normals)
            {
                Vec3f rgb(99.0f, 99.0f, 99.0f);
                bool ok = volume.Sample(pos, nrm, rgb);
                Check(ok, "constant field: Sample should succeed");
                Near(rgb, Vec3f(2.0f, 2.0f, 2.0f), kTol, "constant field must reconstruct to 2");
            }
    }

    // T2: one world light field stored under identity and Rz(+90), including
    // non-axis first- and second-harmonic terms. Moving between cells and
    // clamping outside must not change the reconstructed field.
    void TestOrientationInvariance()
    {
        Vec3f d = norm(Vec3f(1.0f, 2.0f, 3.0f));

        Vec4f lin[9];
        LinearField(d, lin);

        Vec4f xy[9];
        XyField(3.0f, xy);

        Vec4f field[9];

        for (int i = 0; i < 9; i++)
            field[i] = lin[i] + xy[i];

        field[0] = field[0] + Vec4f(2.0f / kY00, 2.0f / kY00, 2.0f / kY00, 0.0f);  // plus constant 2

        std::vector<IrradianceProbe> probes;

        for (int i = 0; i < 8; i++)
            probes.push_back(MakeProbe((i % 2) ? kRz90 : kIdentity, field));

        ProbeVolume volume({2, 2, 2}, Vec3f(0.0f, 0.0f, 0.0f), Vec3f(1.0f, 1.0f, 1.0f), probes);

        Vec3f n = norm(Vec3f(2.0f, 1.0f, 0.0f));

        // Closed-form diffuse result for f = 2 + n.d + 3 n_x n_y:
        //   2 + A1 (n.d) + 3 A2 n_x n_y
        float expected = 2.0f
                       + kA1 * dot(n, d)
                       + 3.0f * kA2 * n[0] * n[1];

        for (int step = 0; step <= 10; step++)
        {
            float t = 0.1f * step;
            Vec3f rgb(0.0f, 0.0f, 0.0f);
            bool ok = volume.Sample(Vec3f(t, 0.3f, 0.7f), n, rgb);
            Check(ok, "orientation test: Sample should succeed");
            Near(rgb, Vec3f(expected, expected, expected), kTol, "field must be invariant while moving between cells");
        }

        Vec3f outside[] = { Vec3f(-5.0f, 0.5f, 0.5f), Vec3f(8.0f, 2.0f, -9.0f) };

        for (const Vec3f& pos : outside)
        {
            Vec3f rgb(0.0f, 0.0f, 0.0f);
            bool ok = volume.Sample(pos, n, rgb);
            Check(ok, "clamped query should succeed");
            Near(rgb, Vec3f(expected, expected, expected), kTol, "clamped query must match the field at the boundary");
        }

        // A transposed-rotation implementation rotates onto n_y instead of n_x:
        // check the directional signature explicitly at two normals.
        Vec3f rx(0.0f, 0.0f, 0.0f);
        Vec3f ry(0.0f, 0.0f, 0.0f);
        volume.Sample(Vec3f(0.5f, 0.5f, 0.5f), Vec3f(1.0f, 0.0f, 0.0f), rx);
        volume.Sample(Vec3f(0.5f, 0.5f, 0.5f), Vec3f(0.0f, 1.0f, 0.0f), ry);

        float ex = 2.0f + kA1 * d[0];                 // n_x n_y term vanishes on axis normals
        float ey = 2.0f + kA1 * d[1];
        Near(rx, Vec3f(ex, ex, ex), kTol, "+x normal sees the x component, not a transposed y component");
        Near(ry, Vec3f(ey, ey, ey), kTol, "+y normal sees the y component");
    }

    // T3: signed values must survive blending. Two equal-weight probes whose
    // convolved values at n0 are 2 and -1 must average to 0.5, not 1.
    void TestSignedBlending()
    {
        Vec3f n0 = norm(Vec3f(1.0f, 1.0f, 0.0f));

        Vec4f fieldA[9];
        LinearField(n0 * 3.0f, fieldA);       // diffuse at n0: A1 * 3 = 2
        Vec4f fieldB[9];
        LinearField(n0 * -1.5f, fieldB);      // diffuse at n0: A1 * -1.5 = -1

        std::vector<IrradianceProbe> probes;
        probes.push_back(MakeProbe(kIdentity, fieldA));
        probes.push_back(MakeProbe(kRz90, fieldB));

        ProbeVolume volume({2, 1, 1}, Vec3f(0.0f, 0.0f, 0.0f), Vec3f(1.0f, 1.0f, 1.0f), probes);

        Vec3f rgb(0.0f, 0.0f, 0.0f);
        bool ok = volume.Sample(Vec3f(0.5f, 0.0f, 0.0f), n0, rgb);
        Check(ok, "signed blend: Sample should succeed");
        Near(rgb, Vec3f(0.5f, 0.5f, 0.5f), kTol, "signed blend (2 + -1)/2 must be 0.5, no early negative clamp");

        // At -n0 the blend is -0.5: only the final RGB clamp may fire, giving 0.
        Vec3f neg(0.0f, 0.0f, 0.0f);
        ok = volume.Sample(Vec3f(0.5f, 0.0f, 0.0f), n0 * -1.0f, neg);
        Check(ok, "negative result query should succeed");
        Near(neg, Vec3f(0.0f, 0.0f, 0.0f), kTol, "negative outgoing RGB is clamped to zero at the end");
    }

    // T4: missing corners only remove their own weight; remaining positive
    // weights are renormalized. No nearest-neighbour substitution.
    void TestMissingProbes()
    {
        // 2x1x1: first probe missing, second holds constant radiance 2.
        Vec4f field2[9];
        ConstantField(2.0f, field2);

        std::vector<IrradianceProbe> probes;
        probes.push_back(MakeMissing());
        probes.push_back(MakeProbe(kIdentity, field2));

        ProbeVolume volume({2, 1, 1}, Vec3f(0.0f, 0.0f, 0.0f), Vec3f(1.0f, 1.0f, 1.0f), probes);

        Vec3f rgb(0.0f, 0.0f, 0.0f);
        bool ok = volume.Sample(Vec3f(0.25f, 0.0f, 0.0f), Vec3f(1.0f, 0.0f, 0.0f), rgb);
        Check(ok, "missing corner: Sample should succeed on the valid one");
        Near(rgb, Vec3f(2.0f, 2.0f, 2.0f), kTol, "missing .75-weight corner: survivor contributes the full result, not .25 of it");

        // The .25/.75 case from the spec: the .25 corner is missing.
        Vec4f linX[9];
        LinearField(Vec3f(1.0f, 0.0f, 0.0f), linX);
        AssignProbe(probes[1], MakeProbe(kRz90, linX));
        ProbeVolume volumeLin({2, 1, 1}, Vec3f(0.0f, 0.0f, 0.0f), Vec3f(1.0f, 1.0f, 1.0f), probes);

        Vec3f linRgb(0.0f, 0.0f, 0.0f);
        ok = volumeLin.Sample(Vec3f(0.75f, 0.0f, 0.0f), Vec3f(1.0f, 0.0f, 0.0f), linRgb);        Check(ok, "renormalized linear query should succeed");
        Near(linRgb, Vec3f(kA1, kA1, kA1), kTol, "surviving probe is renormalized to full weight");

        // 2x2x1 with one missing interior corner: hand-computed renormalized average.
        const float c00 = 1.0f, c10 = 2.0f, c01 = 3.0f;
        Vec4f f00[9], f10[9], f01[9];
        ConstantField(c00, f00);
        ConstantField(c10, f10);
        ConstantField(c01, f01);

        std::vector<IrradianceProbe> four;
        four.push_back(MakeProbe(kIdentity, f00));  // (0,0)
        four.push_back(MakeProbe(kIdentity, f10));  // (1,0)
        four.push_back(MakeProbe(kRz90, f01));      // (0,1)
        four.push_back(MakeMissing());              // (1,1) missing

        ProbeVolume volume2d({2, 2, 1}, Vec3f(0.0f, 0.0f, 0.0f), Vec3f(1.0f, 1.0f, 1.0f), four);

        Vec3f rgb2d(0.0f, 0.0f, 0.0f);
        ok = volume2d.Sample(Vec3f(0.3f, 0.6f, 0.0f), Vec3f(0.0f, 0.0f, 1.0f), rgb2d);
        Check(ok, "2D missing-corner query should succeed");

        // weights: .7*.4=.28, .3*.4=.12, .7*.6=.42, missing .3*.6=.18
        float expected2d = (0.28f * c00 + 0.12f * c10 + 0.42f * c01) / (0.28f + 0.12f + 0.42f);
        Near(rgb2d, Vec3f(expected2d, expected2d, expected2d), kTol, "2D renormalized blend matches hand calculation");
    }

    // T5: all-missing volume and bad inputs fail and leave rgb untouched.
    void TestFailuresPreserveOutput()
    {
        std::vector<IrradianceProbe> none;

        for (int i = 0; i < 8; i++)
            none.push_back(MakeMissing());

        ProbeVolume empty({2, 2, 2}, Vec3f(0.0f, 0.0f, 0.0f), Vec3f(1.0f, 1.0f, 1.0f), none);

        Vec4f field[9];
        ConstantField(2.0f, field);
        std::vector<IrradianceProbe> one;
        one.push_back(MakeProbe(kIdentity, field));
        ProbeVolume single({1, 1, 1}, Vec3f(0.0f, 0.0f, 0.0f), Vec3f(1.0f, 1.0f, 1.0f), one);

        const float inf = std::numeric_limits<float>::infinity();
        const float nan = std::numeric_limits<float>::quiet_NaN();

        struct Case { Vec3f pos; Vec3f nrm; const char* name; };
        Case cases[] =
        {
            { Vec3f(0.0f, 0.0f, 0.0f), Vec3f(1.0f, 0.0f, 0.0f), "all-missing volume" },
            { Vec3f(inf, 0.0f, 0.0f), Vec3f(1.0f, 0.0f, 0.0f), "infinite position" },
            { Vec3f(nan, 0.0f, 0.0f), Vec3f(1.0f, 0.0f, 0.0f), "nan position" },
            { Vec3f(0.0f, 0.0f, 0.0f), Vec3f(0.0f, 0.0f, 0.0f), "zero normal" },
            { Vec3f(0.0f, 0.0f, 0.0f), Vec3f(inf, 1.0f, 0.0f), "infinite normal" },
            { Vec3f(0.0f, 0.0f, 0.0f), Vec3f(nan, 1.0f, 0.0f), "nan normal" }
        };

        for (const Case& c : cases)
        {
            ProbeVolume& vol = (std::string(c.name) == "all-missing volume") ? empty : single;
            Vec3f sentinel(7.0f, 8.0f, 9.0f);
            Vec3f rgb = sentinel;
            bool ok = vol.Sample(c.pos, c.nrm, rgb);
            Check(!ok, c.name);
            Near(rgb, sentinel, 0.0f, "failed Sample must leave rgb untouched");
        }
    }

    // T6: singleton axes use the only grid point; outside positions clamp.
    void TestSingletonAndClamp()
    {
        std::vector<IrradianceProbe> probes;

        for (int i = 0; i < 3; i++)
        {
            Vec4f f[9];
            ConstantField(float(i + 1), f);
            probes.push_back(MakeProbe((i % 2) ? kRz90 : kIdentity, f));
        }

        ProbeVolume volume({3, 1, 1}, Vec3f(0.0f, 0.0f, 0.0f), Vec3f(1.0f, 2.0f, 3.0f), probes);

        struct Point { float x; float expected; };
        Point points[] =
        {
            { -5.0f, 1.0f },
            {  0.0f, 1.0f },
            {  0.5f, 1.5f },
            {  1.0f, 2.0f },
            {  1.5f, 2.5f },
            {  2.0f, 3.0f },
            {  9.0f, 3.0f }
        };

        for (const Point& p : points)
        {
            Vec3f rgb(0.0f, 0.0f, 0.0f);
            bool ok = volume.Sample(Vec3f(p.x, 17.0f, -4.0f), Vec3f(0.0f, 1.0f, 0.0f), rgb);
            Check(ok, "singleton-axis query should succeed");
            Near(rgb, Vec3f(p.expected, p.expected, p.expected), kTol, "singleton y/z axes and x clamp");
        }

        // True 1x1x1 singleton.
        std::vector<IrradianceProbe> one;
        Vec4f f[9];
        ConstantField(2.0f, f);
        one.push_back(MakeProbe(kRz90, f));
        ProbeVolume solo({1, 1, 1}, Vec3f(4.0f, 5.0f, 6.0f), Vec3f(1.0f, 1.0f, 1.0f), one);

        Vec3f rgb(0.0f, 0.0f, 0.0f);
        bool ok = solo.Sample(Vec3f(-100.0f, 100.0f, 0.0f), Vec3f(0.0f, 0.0f, 1.0f), rgb);
        Check(ok, "1x1x1 query should succeed");
        Near(rgb, Vec3f(2.0f, 2.0f, 2.0f), kTol, "singleton volume ignores position");
    }

    // T7: non-unit normals are normalized; zero/non-finite normals fail.
    void TestNonUnitNormal()
    {
        Vec4f lin[9];
        LinearField(Vec3f(1.0f, 0.0f, 0.0f), lin);
        Vec4f cst[9];
        ConstantField(2.0f, cst);

        Vec4f field[9];

        for (int i = 0; i < 9; i++)
            field[i] = lin[i] + cst[i];

        std::vector<IrradianceProbe> probes;
        probes.push_back(MakeProbe(kIdentity, field));
        ProbeVolume volume({1, 1, 1}, Vec3f(0.0f, 0.0f, 0.0f), Vec3f(1.0f, 1.0f, 1.0f), probes);

        Vec3f scaled(0.0f, 0.0f, 0.0f);
        Vec3f unit(0.0f, 0.0f, 0.0f);
        volume.Sample(Vec3f(0.0f), Vec3f(3.0f, 0.0f, 0.0f), scaled);
        volume.Sample(Vec3f(0.0f), Vec3f(1.0f, 0.0f, 0.0f), unit);
        Near(scaled, unit, kTol, "scaled normal matches unit normal");
        Near(scaled, Vec3f(2.0f + kA1, 2.0f + kA1, 2.0f + kA1), kTol, "diffuse value with scaled +x normal");

        Vec3f neg(0.0f, 0.0f, 0.0f);
        volume.Sample(Vec3f(0.0f), Vec3f(0.0f, -2.0f, 0.0f), neg);
        Near(neg, Vec3f(2.0f, 2.0f, 2.0f), kTol, "scaled -y normal has no linear contribution");
    }

    // T8: the volume owns a copy; later caller edits must not affect results.
    void TestCallerCopyIsolation()
    {
        Vec4f f1[9], f2[9];
        ConstantField(1.0f, f1);
        ConstantField(2.0f, f2);

        std::vector<IrradianceProbe> probes;
        probes.push_back(MakeProbe(kIdentity, f1));
        probes.push_back(MakeProbe(kRz90, f2));

        ProbeVolume volume({2, 1, 1}, Vec3f(0.0f, 0.0f, 0.0f), Vec3f(1.0f, 1.0f, 1.0f), probes);

        for (IrradianceProbe& p : probes)
        {
            p.valid = false;
            SetMat3f(p.localToWorld, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);

            for (int i = 0; i < 9; i++)
                p.radiance[i] = Vec4f(1000.0f, 1000.0f, 1000.0f, 0.0f);
        }

        Vec3f rgb(0.0f, 0.0f, 0.0f);
        bool ok = volume.Sample(Vec3f(0.5f, 0.0f, 0.0f), Vec3f(1.0f, 0.0f, 0.0f), rgb);
        Check(ok, "copy isolation query should succeed");
        Near(rgb, Vec3f(1.5f, 1.5f, 1.5f), kTol, "mutating the caller's probes does not affect the volume");
    }

    // T9: constructor validation.
    void TestValidation()
    {
        Vec4f f[9];
        ConstantField(2.0f, f);

        auto makeVec = [&](int n) -> std::vector<IrradianceProbe>
        {
            std::vector<IrradianceProbe> v;

            for (int i = 0; i < n; i++)
                v.push_back(MakeProbe(kIdentity, f));

            return v;
        };

        Vec3f o(0.0f, 0.0f, 0.0f);
        Vec3f s(1.0f, 1.0f, 1.0f);

        Check(ThrowsInvalidArg([&]{ ProbeVolume v({0, 1, 1}, o, s, makeVec(1)); }), "zero size rejected");
        Check(ThrowsInvalidArg([&]{ ProbeVolume v({2, -1, 1}, o, s, makeVec(2)); }), "negative size rejected");
        Check(ThrowsInvalidArg([&]{ ProbeVolume v({2, 1, 1}, o, Vec3f(0.0f, 1.0f, 1.0f), makeVec(2)); }), "zero spacing rejected");
        Check(ThrowsInvalidArg([&]{ ProbeVolume v({2, 1, 1}, o, Vec3f(-1.0f, 1.0f, 1.0f), makeVec(2)); }), "negative spacing rejected");

        const float inf = std::numeric_limits<float>::infinity();
        const float nan = std::numeric_limits<float>::quiet_NaN();
        Check(ThrowsInvalidArg([&]{ ProbeVolume v({2, 1, 1}, o, Vec3f(inf, 1.0f, 1.0f), makeVec(2)); }), "infinite spacing rejected");
        Check(ThrowsInvalidArg([&]{ ProbeVolume v({2, 1, 1}, Vec3f(nan, 0.0f, 0.0f), s, makeVec(2)); }), "non-finite origin rejected");
        Check(ThrowsInvalidArg([&]{ ProbeVolume v({2, 1, 1}, o, s, makeVec(3)); }), "probe count mismatch rejected");
        Check(ThrowsInvalidArg([&]{ ProbeVolume v({1 << 20, 1 << 20, 1 << 20}, o, s, makeVec(1)); }), "overflowing grid product rejected");

        // Bad frames on an otherwise valid probe.
        Check(ThrowsInvalidArg([&]{
            auto p = makeVec(1);
            SetMat3f(p[0].localToWorld, 1,0,0, 0,1,0, 0,0,-1);  // reflection, det -1
            ProbeVolume v({1,1,1}, o, s, p);
        }), "reflection rejected");
        Check(ThrowsInvalidArg([&]{
            auto p = makeVec(1);
            SetMat3f(p[0].localToWorld, 2,0,0, 0,1,0, 0,0,1);  // scale
            ProbeVolume v({1,1,1}, o, s, p);
        }), "scale rejected");
        Check(ThrowsInvalidArg([&]{
            auto p = makeVec(1);
            SetMat3f(p[0].localToWorld, 1,0,0, 1,1,0, 0,0,1);  // shear
            ProbeVolume v({1,1,1}, o, s, p);
        }), "shear rejected");
        Check(ThrowsInvalidArg([&]{
            auto p = makeVec(1);
            SetMat3f(p[0].localToWorld, nan,0,0, 0,1,0, 0,0,1);
            ProbeVolume v({1,1,1}, o, s, p);
        }), "non-finite matrix rejected");
        Check(ThrowsInvalidArg([&]{
            auto p = makeVec(1);
            p[0].radiance[3][1] = inf;
            ProbeVolume v({1,1,1}, o, s, p);
        }), "non-finite radiance rejected");

        // Tolerance boundary: 4e-5 perturbation stays within 1e-4, 6e-4 does not.
        Check([&]{
            auto p = makeVec(1);
            SetMat3f(p[0].localToWorld, 1.0f + 4e-5f,0,0, 0,1,0, 0,0,1);
            try { ProbeVolume v({1,1,1}, o, s, p); return true; }
            catch (const std::invalid_argument&) { return false; }
        }(), "rotation within 1e-4 tolerance accepted");
        Check(ThrowsInvalidArg([&]{
            auto p = makeVec(1);
            SetMat3f(p[0].localToWorld, 1.0f + 6e-4f,0,0, 0,1,0, 0,0,1);
            ProbeVolume v({1,1,1}, o, s, p);
        }), "rotation beyond 1e-4 tolerance rejected");

        // Invalid probes are not inspected at all: NaN frame/coeffs must be fine.
        try
        {
            std::vector<IrradianceProbe> p;
            p.push_back(MakeMissing());
            ProbeVolume v({1, 1, 1}, o, s, p);
            Check(true, "missing probe with NaN data accepted");
        }
        catch (...)
        {
            Check(false, "missing probe with NaN data accepted");
        }
    }

    // T10: PPM visualization of three blend positions on synthetic spheres.
    void WritePpm(const std::string& path)
    {
        std::vector<IrradianceProbe> probes;

        for (int i = 0; i < 8; i++)
        {
            // Spatial ramp: each grid corner carries a different constant level,
            // plus a shared directional term. Frames alternate between identity
            // and Rz(+90) so orientation handling is exercised on every corner.
            int gx = i & 1;
            int gy = (i >> 1) & 1;
            int gz = (i >> 2) & 1;
            float level = 0.15f + 0.45f * float(gx + gy + gz) / 3.0f;

            Vec4f field[9];
            ConstantField(level, field);
            Vec4f lin[9];
            LinearField(Vec3f(0.0f, 0.0f, 0.3f), lin);

            for (int c = 0; c < 9; c++)
                field[c] = field[c] + lin[c];

            probes.push_back(MakeProbe((i % 2) ? kRz90 : kIdentity, field));
        }

        AssignProbe(probes[7], MakeMissing());  // gap at the (1,1,1) corner

        ProbeVolume volume({2, 2, 2}, Vec3f(0.0f, 0.0f, 0.0f), Vec3f(1.0f, 1.0f, 1.0f), probes);

        const int kD = 96;
        const int kWidth = 3 * kD;
        const int kHeight = kD;

        Vec3f positions[3] =
        {
            Vec3f(0.0f, 0.0f, 0.0f),
            Vec3f(0.5f, 0.5f, 0.5f),
            Vec3f(0.85f, 0.85f, 0.85f)  // near the missing corner, handled by renormalization
        };

        std::ofstream out(path.c_str(), std::ios::binary);
        Check(bool(out), "ppm open");
        out << "P6\n# probe-volume preview\n" << kWidth << " " << kHeight << "\n255\n";

        int missCount = 0;

        for (int y = 0; y < kHeight; y++)
        {
            for (int x = 0; x < kWidth; x++)
            {
                int sphere = x / kD;
                float u = 2.0f * (float(x % kD) + 0.5f) / float(kD) - 1.0f;
                float v = 1.0f - 2.0f * (float(y) + 0.5f) / float(kD);
                float r2 = u * u + v * v;
                unsigned char rgb[3] = { 12, 12, 12 };

                if (r2 <= 1.0f)
                {
                    Vec3f n(u, v, std::sqrt(1.0f - r2));
                    Vec3f sample(0.0f, 0.0f, 0.0f);

                    if (volume.Sample(positions[sphere], n, sample))
                    {
                        for (int k = 0; k < 3; k++)
                        {
                            int b = int(std::floor(sample[k] * 255.0f + 0.5f));

                            if (b < 0) b = 0;
                            if (b > 255) b = 255;

                            rgb[k] = (unsigned char)b;
                        }
                    }
                    else
                    {
                        rgb[0] = 255; rgb[1] = 0; rgb[2] = 255;  // magenta flags an unexpected miss
                        missCount++;
                    }
                }

                out.write(reinterpret_cast<char*>(rgb), 3);
            }
        }

        Check(bool(out), "ppm write");
        Check(missCount == 0, "every sphere pixel must sample successfully (no magenta misses)");
    }
}

int main()
{
    for (int i = 0; i < 9; i++)
        kZero[i] = Vec4f(vl_0);

    TestConstant();
    TestOrientationInvariance();
    TestSignedBlending();
    TestMissingProbes();
    TestFailuresPreserveOutput();
    TestSingletonAndClamp();
    TestNonUnitNormal();
    TestCallerCopyIsolation();
    TestValidation();
    WritePpm("tests/out/probes.ppm");

    if (gFailures == 0)
    {
        std::printf("probe-volume: all checks passed\n");
        return 0;
    }

    std::printf("probe-volume: %d check(s) failed\n", gFailures);
    return 1;
}
