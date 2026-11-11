//
// Unit test for resetToConstant(): presetting the delay lines of the
// different state topologies to the steady state of a constant input.
//
// All expected values are computed here directly from the difference
// equation of the sections (see RefChain below) and never via the
// library's resetToConstant() itself, so the library entry is checked
// against an independent reference.
//

#include "Iir.h"

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <stdexcept>

static int nFailures = 0;

static void checkTrue(const char* what, bool ok) {
	if (ok) {
		fprintf(stderr, "ok: %s\n", what);
	} else {
		fprintf(stderr, "FAIL: %s\n", what);
		nFailures++;
	}
}

// absolute error 1e-12 plus relative error 1e-10
static void checkClose(const char* what, double got, double expected) {
	const double tol = 1E-12 + 1E-10 * fabs(expected);
	const double d = fabs(got - expected);
	if (d <= tol) {
		fprintf(stderr, "ok: %s: %.17g == %.17g\n", what, got, expected);
	} else {
		fprintf(stderr, "FAIL: %s: %.17g != %.17g (tol %g)\n",
			what, got, expected, tol);
		nFailures++;
	}
}

// Reference model: direct evaluation of the difference equation
//   y[n] = (b0 x[n] + b1 x[n-1] + b2 x[n-2] - a1 y[n-1] - a2 y[n-2]) / a0
// straight from the SOS coefficients [b0,b1,b2,a0,a1,a2].
struct RefSection {
	double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
	double x1 = 0, x2 = 0, y1 = 0, y2 = 0;

	void setCoefficients(double a0, double a1_, double a2_,
			     double b0_, double b1_, double b2_) {
		b0 = b0_ / a0;
		b1 = b1_ / a0;
		b2 = b2_ / a0;
		a1 = a1_ / a0;
		a2 = a2_ / a0;
	}

	void setSOS(const double* row) { // row: b0,b1,b2,a0,a1,a2
		setCoefficients(row[3], row[4], row[5],
				row[0], row[1], row[2]);
	}

	// analytic steady state for a constant input x, derived from the
	// difference equation with x[n]=x and y[n]=y for all n; returns
	// the equilibrium output
	double preset(double x) {
		const double y = x * (b0 + b1 + b2) / (1.0 + a1 + a2);
		x1 = x2 = x;
		y1 = y2 = y;
		return y;
	}

	double filter(double x) {
		const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
		x2 = x1;
		x1 = x;
		y2 = y1;
		y1 = y;
		return y;
	}
};

template <int N>
struct RefChain {
	RefSection sec[N];

	void setSOS(const double (&sos)[N][6]) {
		for (int i = 0; i < N; i++)
			sec[i].setSOS(sos[i]);
	}

	double preset(double x) {
		for (auto &s : sec)
			x = s.preset(x);
		return x;
	}

	double filter(double x) {
		for (auto &s : sec)
			x = s.filter(x);
		return x;
	}
};

static const double probeSeq[] = {3, -1, 0, 2};

// Two stages with different DC gains, run for one state topology.
// Stage 1: [.5,0,0,1,-.5,0] -> DC gain 1, stage 2: [.25,0,0,1,-.5,0]
// -> DC gain 0.5, so a constant input of 2 settles at 1.
template <class StateType>
static void testTwoStage(const char* name) {
	fprintf(stderr, "--- two stages, different gains, %s ---\n", name);
	const double sos[2][6] = {{.5, 0, 0, 1, -.5, 0},
				  {.25, 0, 0, 1, -.5, 0}};

	// a merely cleared chain climbs up from zero: with a constant
	// input of 2 the first sample is .25, not the equilibrium 1
	Iir::Custom::SOSCascade<2, StateType> cleared(sos);
	checkClose("cleared chain, first sample", cleared.filter(2.0), 0.25);

	Iir::Custom::SOSCascade<2, StateType> f(sos);
	RefChain<2> ref;
	ref.setSOS(sos);

	const double eq = f.resetToConstant(2.0);
	const double refEq = ref.preset(2.0);
	checkClose("equilibrium output is 1", eq, 1.0);
	checkClose("equilibrium output vs reference", eq, refEq);

	// the first filter(input) call and any further constant input
	// stay exactly at the equilibrium
	checkClose("first sample after preset", f.filter(2.0), ref.filter(2.0));
	checkClose("second sample after preset", f.filter(2.0), ref.filter(2.0));

	// a non-constant probe sequence must follow the difference
	// equation: no cached constant output allowed
	for (double x : probeSeq)
		checkClose("probe after preset", f.filter(x), ref.filter(x));
}

// Non-unity a0: the normalisation done by setCoefficients must be
// taken into account. DC gain = (1+2+1)/(4-2+1) = 4/3.
static void testNonUnityA0() {
	fprintf(stderr, "--- non-unity a0 ---\n");
	const double sos[1][6] = {{1, 2, 1, 4, -2, 1}};
	Iir::Custom::SOSCascade<1, Iir::DirectFormI> f(sos);
	RefChain<1> ref;
	ref.setSOS(sos);

	const double eq = f.resetToConstant(3.0);
	checkClose("equilibrium output vs reference", eq, ref.preset(3.0));
	checkClose("equilibrium output is 4", eq, 4.0);
	for (double x : probeSeq)
		checkClose("probe after preset", f.filter(x), ref.filter(x));
}

// Default constructed SOSCascade: unity gain sections, the constant
// input must come out unchanged.
static void testUnitySections() {
	fprintf(stderr, "--- default unity sections ---\n");
	Iir::Custom::SOSCascade<3> f;
	checkClose("unity equilibrium", f.resetToConstant(3.7), 3.7);
	checkClose("unity first sample", f.filter(3.7), 3.7);
	for (double x : probeSeq)
		checkClose("unity passthrough", f.filter(x), x);
}

// First order section (a2 = b2 = 0): DC gain = (0.5+0.5)/(1-0.2) = 1.25.
static void testFirstOrder() {
	fprintf(stderr, "--- first order section ---\n");
	const double sos[1][6] = {{0.5, 0.5, 0, 1, -0.2, 0}};
	Iir::Custom::SOSCascade<1, Iir::TransposedDirectFormII> f(sos);
	RefChain<1> ref;
	ref.setSOS(sos);

	const double eq = f.resetToConstant(2.0);
	checkClose("1st order equilibrium vs reference", eq, ref.preset(2.0));
	checkClose("1st order equilibrium is 2.5", eq, 2.5);
	for (double x : probeSeq)
		checkClose("probe after preset", f.filter(x), ref.filter(x));
}

// RBJ lowpass: DC gain 1, the constant input settles at itself.
static void testRBJLowPass() {
	fprintf(stderr, "--- RBJ lowpass ---\n");
	Iir::RBJ::LowPass lp;
	lp.setupN(0.1);
	RefSection ref;
	ref.setCoefficients(lp.getA0(), lp.getA1(), lp.getA2(),
			    lp.getB0(), lp.getB1(), lp.getB2());

	const double eq = lp.resetToConstant(2.5);
	checkClose("rbj lp equilibrium vs reference", eq, ref.preset(2.5));
	checkClose("rbj lp equilibrium is the input", eq, 2.5);
	checkClose("rbj lp first sample", lp.filter(2.5), ref.filter(2.5));
	for (double x : probeSeq)
		checkClose("rbj lp probe", lp.filter(x), ref.filter(x));
}

// RBJ highpass: DC gain 0, a constant input settles at (nearly) zero.
static void testRBJHighPass() {
	fprintf(stderr, "--- RBJ highpass ---\n");
	Iir::RBJ::HighPass hp;
	hp.setupN(0.1);
	RefSection ref;
	ref.setCoefficients(hp.getA0(), hp.getA1(), hp.getA2(),
			    hp.getB0(), hp.getB1(), hp.getB2());

	const double eq = hp.resetToConstant(2.5);
	checkClose("rbj hp equilibrium vs reference", eq, ref.preset(2.5));
	checkClose("rbj hp equilibrium is zero", eq, 0.0);
	checkClose("rbj hp first sample", hp.filter(2.5), ref.filter(2.5));
	for (double x : probeSeq)
		checkClose("rbj hp probe", hp.filter(x), ref.filter(x));
}

// The old reset() must still clear the delay lines to zero.
static void testResetStillClears() {
	fprintf(stderr, "--- reset() still clears ---\n");
	const double sos[2][6] = {{.5, 0, 0, 1, -.5, 0},
				  {.25, 0, 0, 1, -.5, 0}};
	Iir::Custom::SOSCascade<2> f(sos);
	f.resetToConstant(2.0);
	f.reset();
	Iir::Custom::SOSCascade<2> fresh(sos);
	const double probe[] = {2, 3, -1, 0, 2};
	for (double x : probe)
		checkClose("reset() behaves like a fresh chain",
			   f.filter(x), fresh.filter(x));

	Iir::RBJ::LowPass lp;
	lp.setupN(0.1);
	lp.resetToConstant(2.5);
	lp.reset();
	Iir::RBJ::LowPass lpFresh;
	lpFresh.setupN(0.1);
	for (double x : probe)
		checkClose("rbj reset() behaves like a fresh filter",
			   lp.filter(x), lpFresh.filter(x));
}

// Direct use of the state classes and of Biquad::resetToConstant.
static void testStateLevel() {
	fprintf(stderr, "--- state classes directly ---\n");
	Iir::Biquad bq;
	bq.setCoefficients(1, -0.5, 0, 0.5, 0, 0); // DC gain 1

	Iir::DirectFormI dfi;
	checkClose("DFI equilibrium", bq.resetToConstant(2.0, dfi), 2.0);
	checkClose("DFI first sample", bq.filter(2.0, dfi), 2.0);

	Iir::DirectFormII dfii;
	checkClose("DFII equilibrium", dfii.resetToConstant(2.0, bq), 2.0);
	checkClose("DFII first sample", bq.filter(2.0, dfii), 2.0);

	Iir::TransposedDirectFormII tdfii;
	checkClose("TDFII equilibrium", tdfii.resetToConstant(2.0, bq), 2.0);
	checkClose("TDFII first sample", bq.filter(2.0, tdfii), 2.0);
}

// Rejection of invalid input: non-finite input, non-finite
// coefficients, 1+a1+a2 == 0 and non-representable equilibria must
// all throw std::invalid_argument.
static void testExceptions() {
	fprintf(stderr, "--- exceptions ---\n");
	const double sos[1][6] = {{.5, 0, 0, 1, -.5, 0}};

	{
		Iir::Custom::SOSCascade<1> f(sos);
		bool threw = false;
		try { f.resetToConstant(NAN); }
		catch (const std::invalid_argument&) { threw = true; }
		checkTrue("NaN input throws", threw);
	}
	{
		Iir::Custom::SOSCascade<1> f(sos);
		bool threw = false;
		try { f.resetToConstant(INFINITY); }
		catch (const std::invalid_argument&) { threw = true; }
		checkTrue("infinite input throws", threw);
	}
	{
		// setCoefficients() only rejects NaN, so an infinite
		// coefficient can be set up and must be caught here
		const double bad[1][6] = {{INFINITY, 0, 0, 1, -.5, 0}};
		Iir::Custom::SOSCascade<1> f(bad);
		bool threw = false;
		try { f.resetToConstant(1.0); }
		catch (const std::invalid_argument&) { threw = true; }
		checkTrue("non-finite coefficient throws", threw);
	}
	{
		// 1 + a1 + a2 == 0 exactly
		const double zero[1][6] = {{1, 0, 0, 1, -1, 0}};
		Iir::Custom::SOSCascade<1> f(zero);
		bool threw = false;
		try { f.resetToConstant(1.0); }
		catch (const std::invalid_argument&) { threw = true; }
		checkTrue("1+a1+a2 == 0 throws", threw);
	}
	{
		// tiny but non-zero denominator: legal, no throw; the
		// result must match the difference equation reference
		const double tiny[1][6] = {{1, 0, 0, 1, -(1.0 - 1E-10), 0}};
		Iir::Custom::SOSCascade<1> f(tiny);
		RefChain<1> ref;
		ref.setSOS(tiny);
		bool threw = false;
		try {
			checkClose("tiny denominator is accepted",
				   f.resetToConstant(1.0), ref.preset(1.0));
		} catch (const std::invalid_argument&) { threw = true; }
		checkTrue("tiny denominator does not throw", !threw);
	}
	{
		// equilibrium cannot be represented by a finite double
		const double big[1][6] = {{2, 0, 0, 1, -.5, 0}}; // DC gain 4
		Iir::Custom::SOSCascade<1> f(big);
		bool threw = false;
		try { f.resetToConstant(DBL_MAX); }
		catch (const std::invalid_argument&) { threw = true; }
		checkTrue("non-finite equilibrium throws", threw);
	}
}

// A failure in a later stage must not modify the state (or the
// coefficients) of the earlier stages. Checked against a control
// instance which never called resetToConstant() by feeding both a
// non-constant probe sequence.
static void testFailureKeepsState() {
	fprintf(stderr, "--- failed preset keeps the chain untouched ---\n");
	// stage 1 is fine, stage 2 has 1+a1+a2 == 0
	const double sos[2][6] = {{.5, 0, 0, 1, -.5, 0},
				  {1, 0, 0, 1, -1, 0}};
	Iir::Custom::SOSCascade<2> f(sos);
	Iir::Custom::SOSCascade<2> control(sos);

	// give both chains the same non-zero history
	const double warm[] = {1, -2, 0.5, 3};
	for (double x : warm) {
		f.filter(x);
		control.filter(x);
	}

	bool threw = false;
	try { f.resetToConstant(2.0); }
	catch (const std::invalid_argument&) { threw = true; }
	checkTrue("failing stage throws", threw);

	// a non-constant probe must give bit-identical results to the
	// control chain; feeding zeros or comparing the return value
	// alone would not reveal a polluted state
	const double probe[] = {3, -1, 0, 2, -0.5, 1.25};
	bool identical = true;
	for (double x : probe) {
		if (f.filter(x) != control.filter(x))
			identical = false;
	}
	checkTrue("chain state untouched after failure", identical);
}

int main() {
	testTwoStage<Iir::DirectFormI>("DirectFormI");
	testTwoStage<Iir::DirectFormII>("DirectFormII");
	testTwoStage<Iir::TransposedDirectFormII>("TransposedDirectFormII");
	testNonUnityA0();
	testUnitySections();
	testFirstOrder();
	testRBJLowPass();
	testRBJHighPass();
	testResetStillClears();
	testStateLevel();
	testExceptions();
	testFailureKeepsState();

	if (nFailures == 0) {
		fprintf(stderr, "constant_state: all tests passed.\n");
		return 0;
	}
	fprintf(stderr, "constant_state: %d test(s) FAILED.\n", nFailures);
	return 1;
}
