import com.badlogic.gdx.graphics.g3d.Model;
import com.badlogic.gdx.graphics.g3d.ModelInstance;
import com.badlogic.gdx.graphics.g3d.model.Animation;
import com.badlogic.gdx.graphics.g3d.model.Node;
import com.badlogic.gdx.graphics.g3d.model.NodeAnimation;
import com.badlogic.gdx.graphics.g3d.model.NodeKeyframe;
import com.badlogic.gdx.graphics.g3d.model.NodePart;
import com.badlogic.gdx.graphics.g3d.utils.AnimationController;
import com.badlogic.gdx.graphics.g3d.utils.BaseAnimationController;
import com.badlogic.gdx.graphics.g3d.utils.BlendWeightSolver;
import com.badlogic.gdx.graphics.g3d.utils.BlendWeightSolver.Evaluation;
import com.badlogic.gdx.graphics.g3d.utils.BlendWeightSolver.Observation;
import com.badlogic.gdx.graphics.g3d.utils.BlendWeightSolver.Preparation;
import com.badlogic.gdx.graphics.g3d.utils.BlendWeightSolver.Result;
import com.badlogic.gdx.math.Matrix4;
import com.badlogic.gdx.math.Quaternion;
import com.badlogic.gdx.math.Vector3;
import com.badlogic.gdx.utils.Array;
import com.badlogic.gdx.utils.ArrayMap;

import task_support.Checks;
import task_support.TaskSuite;

/** Verifies {@link BlendWeightSolver}: the shared multi-clip forward pose against an independent scalar reference, the
 * analytic Jacobian against that reference and central differences, simplex optimization behavior, real palette
 * integration and input validation. */
public final class BlendWeightSolverTest implements TaskSuite {
	private static final int SLOTS = BlendWeightSolver.BONE_SLOTS;

	/** A small rig: non-commuting parent rotations, missing channels, shuffled bone slot order, non-unit inverse binds and an
	 * affine instance transform. */
	static final class Rig {
		Model model;
		ModelInstance instance;
		Node root, spine, hand, foot;
		NodePart part;
		Animation clipA, clipB, clipC;
		Animation[] clips;
		float[] times;
		Observation[] observations;
		double[] starWeights;
	}

	@Override
	public void run (final Checks checks) throws Exception {
		final Rig rig = buildRig();
		final Preparation prep = BlendWeightSolver.prepare(rig.instance, rig.clips, rig.times, rig.observations);

		checks.group("forward", () -> {
			final double[] w = rig.starWeights.clone();
			final Evaluation evaluation = prep.evaluate(w);
			checks.check(evaluation.worldPoints.length == rig.observations.length * 3, "world point layout");
			checks.check(evaluation.jacobian.length == rig.observations.length, "jacobian row count");
			for (int o = 0; o < rig.observations.length; o++)
				checks.check(evaluation.jacobian[o].length == rig.clips.length * 3, "jacobian row layout");

			final RefPose ref = new RefPose(rig, rig.clips, rig.times, w);
			for (int o = 0; o < rig.observations.length; o++) {
				final double[] expected = ref.point(rig.observations[o]);
				for (int a = 0; a < 3; a++)
					checks.check(Math.abs(evaluation.worldPoints[3 * o + a] - expected[a]) < 1e-9,
						"forward world point matches scalar reference obs=" + o + " axis=" + a);
			}

			// Evaluation must not require simplex weights (feasibility is enforced only by the optimizer).
			final double[] unconstrained = {0.6, 0.6, -0.2};
			final Evaluation loose = prep.evaluate(unconstrained);
			final RefPose refLoose = new RefPose(rig, rig.clips, rig.times, unconstrained);
			for (int o = 0; o < rig.observations.length; o++) {
				final double[] expected = refLoose.point(rig.observations[o]);
				for (int a = 0; a < 3; a++)
					checks.check(Math.abs(loose.worldPoints[3 * o + a] - expected[a]) < 1e-9,
						"non-simplex weights still evaluate consistently obs=" + o);
			}

			// The instance must be untouched by evaluation: nodes and palette keep their pre-evaluation values.
			final float rootTx = rig.root.localTransform.val[Matrix4.M03];
			final float paletteTx = rig.part.bones[0].val[Matrix4.M03];
			prep.evaluate(new double[] {0.1, 0.4, 0.5});
			checks.check(rig.root.localTransform.val[Matrix4.M03] == rootTx, "evaluation does not write node transforms");
			checks.check(rig.part.bones[0].val[Matrix4.M03] == paletteTx, "evaluation does not write the bone palette");

			// inheritTransform = false: the child's global transform is its local transform, ignoring the parent.
			checks.check(checkDetachedInheritance(rig), "inheritTransform=false detaches the child global transform");
		});

		checks.group("jacobian", () -> {
			final double[] w = {0.35, 0.4, 0.25};
			final Evaluation evaluation = prep.evaluate(w);
			final double h = 1e-6;
			for (int k = 0; k < rig.clips.length; k++) {
				final double[] wp = w.clone();
				final double[] wm = w.clone();
				wp[k] += h;
				wm[k] -= h;
				final RefPose plus = new RefPose(rig, rig.clips, rig.times, wp);
				final RefPose minus = new RefPose(rig, rig.clips, rig.times, wm);
				final Evaluation evalPlus = prep.evaluate(wp);
				final Evaluation evalMinus = prep.evaluate(wm);
				for (int o = 0; o < rig.observations.length; o++) {
					final double[] pp = plus.point(rig.observations[o]);
					final double[] pm = minus.point(rig.observations[o]);
					for (int a = 0; a < 3; a++) {
						final double refDerivative = (pp[a] - pm[a]) / (2.0 * h);
						final double solverDerivative = (evalPlus.worldPoints[3 * o + a] - evalMinus.worldPoints[3 * o + a])
							/ (2.0 * h);
						final double analytic = evaluation.jacobian[o][3 * k + a];
						final double scale = Math.max(1.0, Math.abs(refDerivative));
						checks.check(Math.abs(analytic - refDerivative) < 1e-5 * scale,
							"analytic jacobian vs scalar central difference o=" + o + " k=" + k + " a=" + a);
						checks.check(Math.abs(analytic - solverDerivative) < 1e-5 * scale,
							"analytic jacobian vs solver central difference o=" + o + " k=" + k + " a=" + a);
					}
				}
			}
		});

		checks.group("simplex", () -> {
			// This group commits poses, so use a fresh rig to keep the shared preparation's rest values intact.
			final Rig sRig = buildRig();
			// Projection: known interior projection, negatives and an already feasible point.
			assertArrayClose(checks, BlendWeightSolver.projectSimplex(new double[] {0.6, 0.3, 0.1}),
				new double[] {0.6, 0.3, 0.1}, 1e-12, "simplex identity projection");
			assertArrayClose(checks, BlendWeightSolver.projectSimplex(new double[] {2, 2, 2}),
				new double[] {1.0 / 3, 1.0 / 3, 1.0 / 3}, 1e-12, "uniform projection");
			final double[] projectedNeg = BlendWeightSolver.projectSimplex(new double[] {-1, 1, 0.5});
			checks.check(projectedNeg[0] == 0.0 && Math.abs(sum(projectedNeg) - 1) < 1e-12 && projectedNeg[1] >= projectedNeg[2],
				"negative component projects to the boundary");

			// Small two-clip system with a known interior optimum.
			final Animation[] twoClips = {sRig.clipA, sRig.clipB};
			final float[] twoTimes = {sRig.times[0], sRig.times[1]};
			final RefPose starPose = new RefPose(sRig, twoClips, twoTimes, new double[] {0.7, 0.3});
			final Observation[] one = {observation(sRig, 0, new double[] {0.2, -0.1, 0.4}, new int[] {0, 1, 2, 3},
				new float[] {0.4f, 0.3f, 0.2f, 0.1f}, starPose, 1f)};
			final Preparation prep2 = BlendWeightSolver.prepare(sRig.instance, twoClips, twoTimes, one);
			final double[] star2 = {0.7, 0.3};
			final double fAtStar = objective(prep2, prep2.evaluate(star2), star2, new double[] {0.5, 0.5}, 0.01, one);
			final Result interior = BlendWeightSolver.solveAndApply(prep2, new double[] {0.5, 0.5}, new double[] {0.5, 0.5},
				0.01, 1e-7, 300);
			checks.check(interior.converged, "interior problem reports convergence");
			checks.check(Math.abs(interior.weights[0] - 0.7) < 0.03 && Math.abs(interior.weights[1] - 0.3) < 0.03,
				"interior weights recover the generating point");
			checks.check(interior.objective <= fAtStar + 1e-10, "interior optimum reaches at least the generating point value");
			checks.check(interior.projectedGradientResidual <= 1e-7, "reported residual matches tolerance");
			checks.check(isSimplex(interior.weights), "interior result stays on the simplex");
			checks.check(interior.acceptedIterations > 0, "interior solve actually iterated");

			// Boundary optimum: extrapolated target and a boundary prior pin one weight to zero.
			final RefPose p0 = new RefPose(sRig, twoClips, twoTimes, new double[] {1, 0});
			final RefPose p1 = new RefPose(sRig, twoClips, twoTimes, new double[] {0, 1});
			final double[] a0 = p0.point(one[0]);
			final double[] a1 = p1.point(one[0]);
			final Observation boundaryObs = new Observation(sRig.part, new Vector3(0.2f, -0.1f, 0.4f),
				new int[] {0, 1, 2, 3}, new float[] {0.4f, 0.3f, 0.2f, 0.1f},
				new Vector3((float)(a0[0] + 0.5 * (a0[0] - a1[0])), (float)(a0[1] + 0.5 * (a0[1] - a1[1])),
					(float)(a0[2] + 0.5 * (a0[2] - a1[2]))),
				1f);
			final Preparation prepBoundary = BlendWeightSolver.prepare(sRig.instance, twoClips, twoTimes,
				new Observation[] {boundaryObs});
			final Result boundary = BlendWeightSolver.solveAndApply(prepBoundary, new double[] {0.8, 0.2}, new double[] {1, 0},
				0.01, 1e-8, 300);
			checks.check(boundary.weights[1] <= 1e-10, "boundary optimum keeps the inactive weight at zero");
			checks.check(boundary.weights[0] > 0.999, "boundary optimum keeps the active weight");
			checks.check(boundary.converged, "boundary optimum converges on the projected residual");

			// Duplicate clips: rank deficient Jacobian is a legal case. The split between the two identical A clips is
			// undetermined, but the achievable data fit is independent of it.
			final Animation[] dupClips = {sRig.clipA, sRig.clipA, sRig.clipB};
			final float[] dupTimes = {sRig.times[0], sRig.times[0], sRig.times[1]};
			final RefPose dupStar = new RefPose(sRig, dupClips, dupTimes, new double[] {0.25, 0.25, 0.5});
			final Observation[] dupObs = {observation(sRig, 0, new double[] {0.2, -0.1, 0.4}, new int[] {0, 1, 2, 3},
				new float[] {0.4f, 0.3f, 0.2f, 0.1f}, dupStar, 1f)};
			final Preparation prepDup = BlendWeightSolver.prepare(sRig.instance, dupClips, dupTimes, dupObs);
			final double[] dupPrior = {1.0 / 3, 1.0 / 3, 1.0 / 3};
			final Result duplicated = BlendWeightSolver.solveAndApply(prepDup, dupPrior, dupPrior, 0.01, 1e-8, 400);
			checks.check(isSimplex(duplicated.weights), "duplicate-clip result is feasible");
			checks.check(duplicated.converged, "duplicate-clip solve converges");
			final double fAtDupStar = objective(prepDup, prepDup.evaluate(new double[] {0.25, 0.25, 0.5}),
				new double[] {0.25, 0.25, 0.5}, dupPrior, 0.01, dupObs);
			checks.check(duplicated.objective <= fAtDupStar + 1e-9,
				"rank deficient solve does no worse than the generating split");
			// Independent grid over the two distinct clip masses (A mass alpha, B mass 1-alpha): best achievable data
			// residual through the real nonlinear forward pose (one[0] and dupObs[0] use the same skinned point).
			double gridBest = Double.POSITIVE_INFINITY;
			for (int step = 0; step <= 2000; step++) {
				final double alpha = step / 2000.0;
				final Evaluation gridEval = prep2.evaluate(new double[] {alpha, 1 - alpha});
				double residual = 0;
				for (int a = 0; a < 3; a++) {
					final double d = gridEval.worldPoints[a] - targetComponent(dupObs[0], a);
					residual += d * d;
				}
				gridBest = Math.min(gridBest, 0.5 * dupObs[0].weight * residual);
			}
			final Evaluation dupEval = prepDup.evaluate(duplicated.weights);
			double dupData = 0;
			for (int a = 0; a < 3; a++) {
				final double d = dupEval.worldPoints[a] - targetComponent(dupObs[0], a);
				dupData += dupObs[0].weight * d * d;
			}
			dupData *= 0.5;
			checks.check(dupData <= gridBest + 1e-6, "duplicate clips reach the grid-optimal achievable data fit");

			// Starved iterations: last feasible point (the initial one) is applied, convergence is not claimed.
			final Result starved = BlendWeightSolver.solveAndApply(prep2, new double[] {0.55, 0.45}, new double[] {0.5, 0.5},
				0.01, 1e-12, 0);
			checks.check(starved.acceptedIterations == 0, "no accepted steps without iterations");
			checks.check(!starved.converged, "starved solve must not claim convergence");
			assertArrayClose(checks, starved.weights, new double[] {0.55, 0.45}, 1e-15, "starved solve keeps the initial weights");
			final Evaluation starvedEval = prep2.evaluate(starved.weights);
			final double[] palettePoint = pointFromPalette(sRig, one[0]);
			double starvedGap = 0;
			for (int a = 0; a < 3; a++)
				starvedGap = Math.max(starvedGap, Math.abs(starvedEval.worldPoints[a] - palettePoint[a]));
			checks.check(starvedGap < 2e-4, "starved solve still commits its feasible pose to the palette");

			// Monotonicity: the accepted final objective cannot exceed the objective at the initial point.
			final double[] initial = {0.1, 0.9};
			final Evaluation initialEval = prep2.evaluate(initial);
			final double initialF = objective(prep2, initialEval, initial, new double[] {0.5, 0.5}, 0.01, one);
			final Result monotonic = BlendWeightSolver.solveAndApply(prep2, initial, new double[] {0.5, 0.5}, 0.01, 1e-10, 200);
			checks.check(monotonic.objective <= initialF + 1e-12, "objective never increases across accepted steps");
		});

		checks.group("integration", () -> {
			// Full solve on the three-clip rig (its middle target deliberately conflicts), then rebuild points from the
			// real committed palette.
			final Result result = BlendWeightSolver.solveAndApply(prep, new double[] {1.0 / 3, 1.0 / 3, 1.0 / 3},
				new double[] {1.0 / 3, 1.0 / 3, 1.0 / 3}, 0.05, 1e-9, 500);
			checks.check(isSimplex(result.weights), "integrated result is feasible");
			final Evaluation committed = prep.evaluate(result.weights);
			for (int o = 0; o < rig.observations.length; o++) {
				final double[] palettePoint = pointFromPalette(rig, rig.observations[o]);
				for (int a = 0; a < 3; a++) {
					checks.check(Math.abs(committed.worldPoints[3 * o + a] - palettePoint[a]) < 2e-4,
						"real palette matches candidate evaluation obs=" + o);
				}
			}

			// A fully consistent system (all targets generated from one weight vector) reaches its targets through the
			// real palette, including a zero-weight skin slot.
			final Rig exact = buildRig();
			final RefPose exactStar = new RefPose(exact, exact.clips, exact.times, new double[] {0.4, 0.35, 0.25});
			final Observation[] exactObs = new Observation[2];
			exactObs[0] = observation(exact, 0, new double[] {0.3, -0.2, 0.5}, new int[] {0, 1, 2, 3},
				new float[] {0.4f, 0.3f, 0.2f, 0.1f}, exactStar, 1f);
			exactObs[1] = observation(exact, 1, new double[] {-0.4, 0.25, 0.8}, new int[] {3, 0, 1, 2},
				new float[] {0.1f, 0.4f, 0.3f, 0.2f}, exactStar, 1f);
			final Preparation prepExact = BlendWeightSolver.prepare(exact.instance, exact.clips, exact.times, exactObs);
			final Result exactResult = BlendWeightSolver.solveAndApply(prepExact,
				new double[] {1.0 / 3, 1.0 / 3, 1.0 / 3}, new double[] {1.0 / 3, 1.0 / 3, 1.0 / 3}, 1e-6, 1e-10, 500);
			checks.check(exactResult.converged, "consistent system converges");
			for (int o = 0; o < exactObs.length; o++) {
				final double[] palettePoint = pointFromPalette(exact, exactObs[o]);
				for (int a = 0; a < 3; a++)
					checks.check(Math.abs(palettePoint[a] - targetComponent(exactObs[o], a)) < 2e-4,
						"committed palette reaches the consistent target obs=" + o);
			}

			// BaseAnimationController shares the exact same forward pose. Use a pristine rig: the commits above already
			// overwrote the shared rig's node TRS, which is the intended apply() behavior.
			final Rig ctrl = buildRig();
			final Preparation prepCtrl = BlendWeightSolver.prepare(ctrl.instance, ctrl.clips, ctrl.times, ctrl.observations);
			final Player player = new Player(ctrl.instance);
			final double[] w = {0.25, 0.55, 0.2};
			player.applyWeightedAnimations(ctrl.clips, ctrl.times, w);
			final Evaluation shared = prepCtrl.evaluate(w);
			for (int o = 0; o < ctrl.observations.length; o++) {
				final double[] palettePoint = pointFromPalette(ctrl, ctrl.observations[o]);
				for (int a = 0; a < 3; a++)
					checks.check(Math.abs(shared.worldPoints[3 * o + a] - palettePoint[a]) < 2e-4,
						"controller entry and solver use one forward pose obs=" + o);
			}

			// Normal animation playback can take over again afterwards.
			final Matrix4 handBefore = ctrl.hand.localTransform.cpy();
			player.sample(ctrl.clipA, 0.5f);
			checks.check(!ctrl.hand.localTransform.equals(handBefore), "ordinary animation still drives the nodes afterwards");

			// Two instances interleave without sharing writable workspace.
			final Rig other = buildRig();
			final Preparation prepOther = BlendWeightSolver.prepare(other.instance, other.clips, other.times,
				other.observations);
			final Evaluation firstPoints = prepCtrl.evaluate(new double[] {0.5, 0.2, 0.3});
			final float otherFootTx = other.foot.translation.x;
			BlendWeightSolver.solveAndApply(prepOther, new double[] {1.0 / 3, 1.0 / 3, 1.0 / 3},
				new double[] {1.0 / 3, 1.0 / 3, 1.0 / 3}, 0.05, 1e-9, 200);
			final Evaluation firstPointsAgain = prepCtrl.evaluate(new double[] {0.5, 0.2, 0.3});
			for (int i = 0; i < firstPoints.worldPoints.length; i++)
				checks.check(firstPoints.worldPoints[i] == firstPointsAgain.worldPoints[i],
					"interleaved preparations keep independent workspace");
			checks.check(other.foot.translation.x == otherFootTx, "solving one instance does not touch the other instance");

			// An AnimationController (the existing public type) still works on top of the solved instance.
			final AnimationController controller = new AnimationController(ctrl.instance);
			controller.setAnimation(ctrl.clipA.id, 1);
			controller.update(0.25f);
			checks.check(true, "AnimationController remains usable after weighted solving");
		});

		checks.group("invalid", () -> {
			// Keep a known-good report and caller arrays; failed calls must leave everything untouched.
			final Result good = BlendWeightSolver.solveAndApply(prep, new double[] {1.0 / 3, 1.0 / 3, 1.0 / 3},
				new double[] {1.0 / 3, 1.0 / 3, 1.0 / 3}, 0.05, 1e-6, 5);
			final double[] goodWeights = good.weights.clone();
			final float[] rootLocal = rig.root.localTransform.val.clone();
			final float[] palette0 = rig.part.bones[0].val.clone();
			final Animation[] clipsClone = rig.clips.clone();
			final float[] timesClone = rig.times.clone();

			// Duplicate node track inside one clip.
			final Animation dupTrack = new Animation();
			dupTrack.id = "dup";
			final NodeAnimation t1 = new NodeAnimation();
			t1.node = rig.spine;
			t1.translation = vecKeys(0, new Vector3(), 1, new Vector3(1, 0, 0));
			final NodeAnimation t2 = new NodeAnimation();
			t2.node = rig.spine;
			t2.translation = vecKeys(0, new Vector3(), 1, new Vector3(2, 0, 0));
			dupTrack.nodeAnimations.addAll(t1, t2);
			expectIae(checks, "duplicate node track",
				() -> BlendWeightSolver.prepare(rig.instance, new Animation[] {dupTrack}, new float[] {0.5f},
					rig.observations));

			// Illegal key ordering.
			final Animation badOrder = new Animation();
			badOrder.id = "badOrder";
			final NodeAnimation bo = new NodeAnimation();
			bo.node = rig.hand;
			bo.translation = vecKeys(1, new Vector3(), 0, new Vector3(1, 0, 0));
			badOrder.nodeAnimations.add(bo);
			expectIae(checks, "illegal key order",
				() -> BlendWeightSolver.prepare(rig.instance, new Animation[] {badOrder}, new float[] {0.5f},
					rig.observations));

			// Node part and animated node from another instance.
			final Rig foreign = buildRig();
			final Observation foreignPartObs = rig.observations[0];
			foreignPartObs.part = foreign.part;
			expectIae(checks, "wrong instance part",
				() -> BlendWeightSolver.prepare(rig.instance, rig.clips, rig.times, new Observation[] {foreignPartObs}));
			foreignPartObs.part = rig.part;

			final Animation foreignNodeClip = new Animation();
			final NodeAnimation fn = new NodeAnimation();
			fn.node = foreign.spine;
			fn.translation = vecKeys(0, new Vector3(), 1, new Vector3(1, 0, 0));
			foreignNodeClip.nodeAnimations.add(fn);
			expectIae(checks, "wrong instance animated node",
				() -> BlendWeightSolver.prepare(rig.instance, new Animation[] {foreignNodeClip}, new float[] {0.5f},
					rig.observations));

			// Bad bone slots and dimensions.
			final Observation badSlot = copyObs(rig.observations[0]);
			badSlot.boneSlots[0] = SLOTS;
			expectIae(checks, "bone slot out of range",
				() -> BlendWeightSolver.prepare(rig.instance, rig.clips, rig.times, new Observation[] {badSlot}));
			badSlot.boneSlots[0] = -1;
			expectIae(checks, "negative bone slot",
				() -> BlendWeightSolver.prepare(rig.instance, rig.clips, rig.times, new Observation[] {badSlot}));
			final Observation badDim = copyObs(rig.observations[0]);
			badDim.boneSlots = new int[3];
			expectIae(checks, "bone slot dimension",
				() -> BlendWeightSolver.prepare(rig.instance, rig.clips, rig.times, new Observation[] {badDim}));

			// Non-finite values, illegal skin and observation weights.
			final Observation nanObs = copyObs(rig.observations[0]);
			nanObs.target.x = Float.NaN;
			expectIae(checks, "non-finite target",
				() -> BlendWeightSolver.prepare(rig.instance, rig.clips, rig.times, new Observation[] {nanObs}));
			final Observation negSkin = copyObs(rig.observations[0]);
			negSkin.skinWeights[0] = -0.1f;
			expectIae(checks, "negative skin weight",
				() -> BlendWeightSolver.prepare(rig.instance, rig.clips, rig.times, new Observation[] {negSkin}));
			final Observation badSum = copyObs(rig.observations[0]);
			badSum.skinWeights[0] = 0.5f;
			expectIae(checks, "skin weights do not sum to one",
				() -> BlendWeightSolver.prepare(rig.instance, rig.clips, rig.times, new Observation[] {badSum}));
			final Observation zeroWeight = copyObs(rig.observations[0]);
			zeroWeight.weight = 0f;
			expectIae(checks, "zero observation weight",
				() -> BlendWeightSolver.prepare(rig.instance, rig.clips, rig.times, new Observation[] {zeroWeight}));

			// Non-invertible instance transform.
			final Rig singular = buildRig();
			for (int i = 0; i < 16; i++)
				singular.instance.transform.val[i] = 0f;
			expectIae(checks, "singular instance transform",
				() -> BlendWeightSolver.prepare(singular.instance, singular.clips, singular.times,
					singular.observations));

			// Hemisphere ambiguity: a 180 degree rotation key is orthogonal to the identity rest rotation.
			final Rig hemi = buildRig();
			final Animation hemiClip = new Animation();
			final NodeAnimation hemiTrack = new NodeAnimation();
			hemiTrack.node = hemi.hand;
			hemiTrack.rotation = quatKeys(0, new Quaternion(1, 0, 0, 0));
			hemiClip.nodeAnimations.add(hemiTrack);
			expectIae(checks, "rotation hemisphere boundary",
				() -> BlendWeightSolver.prepare(hemi.instance, new Animation[] {hemiClip}, new float[] {0.5f},
					hemi.observations));

			// Non-positive sampled scale.
			final Rig badScale = buildRig();
			final Animation scaleClip = new Animation();
			final NodeAnimation scaleTrack = new NodeAnimation();
			scaleTrack.node = badScale.hand;
			scaleTrack.scaling = vecKeys(0, new Vector3(1, 1, 1), 1, new Vector3(1, -1, 1));
			scaleClip.nodeAnimations.add(scaleTrack);
			expectIae(checks, "negative sampled scale",
				() -> BlendWeightSolver.prepare(badScale.instance, new Animation[] {scaleClip}, new float[] {0.5f},
					badScale.observations));
			badScale.hand.scale.set(0, 1, 1);
			expectIae(checks, "non-positive rest scale",
				() -> BlendWeightSolver.prepare(badScale.instance, badScale.clips, badScale.times, badScale.observations));

			// Optimizer argument validation; the instance must not be modified.
			expectIae(checks, "non-positive lambda",
				() -> BlendWeightSolver.solveAndApply(prep, new double[] {0.5, 0.3, 0.2},
					new double[] {1.0 / 3, 1.0 / 3, 1.0 / 3}, 0.0, 1e-8, 10));
			expectIae(checks, "non-positive tolerance",
				() -> BlendWeightSolver.solveAndApply(prep, new double[] {0.5, 0.3, 0.2},
					new double[] {1.0 / 3, 1.0 / 3, 1.0 / 3}, 0.05, 0.0, 10));
			expectIae(checks, "negative iteration count",
				() -> BlendWeightSolver.solveAndApply(prep, new double[] {0.5, 0.3, 0.2},
					new double[] {1.0 / 3, 1.0 / 3, 1.0 / 3}, 0.05, 1e-8, -1));
			expectIae(checks, "negative initial weight",
				() -> BlendWeightSolver.solveAndApply(prep, new double[] {-0.1, 0.5, 0.6},
					new double[] {1.0 / 3, 1.0 / 3, 1.0 / 3}, 0.05, 1e-8, 10));
			expectIae(checks, "initial weights off simplex",
				() -> BlendWeightSolver.solveAndApply(prep, new double[] {1, 1, 0},
					new double[] {1.0 / 3, 1.0 / 3, 1.0 / 3}, 0.05, 1e-8, 10));
			expectIae(checks, "prior dimension mismatch",
				() -> BlendWeightSolver.solveAndApply(prep, new double[] {0.5, 0.3, 0.2}, new double[] {0.5, 0.5},
					0.05, 1e-8, 10));

			// State preservation after the failed calls.
			assertArrayClose(checks, good.weights, goodWeights, 0.0, "existing report stays intact");
			assertFloatArrayClose(checks, rig.root.localTransform.val, rootLocal, 0.0f, "node transforms stay intact");
			assertFloatArrayClose(checks, rig.part.bones[0].val, palette0, 0.0f, "palette stays intact");
			checks.check(clipsClone.length == rig.clips.length && timesClone.length == rig.times.length,
				"caller arrays stay intact");
		});
	}

	// ------------------------------------------------------------------
	// Rig construction
	// ------------------------------------------------------------------

	private Rig buildRig () {
		final Rig rig = new Rig();
		rig.model = new Model();
		rig.root = new Node();
		rig.root.id = "root";
		rig.root.rotation.setFromAxis(0, 0, 1, 30f);
		rig.root.scale.set(1.2f, 0.8f, 1.5f);
		rig.spine = new Node();
		rig.spine.id = "spine";
		rig.spine.translation.set(1, 0, 0);
		rig.spine.rotation.setFromAxis(1, 0, 0, 45f);
		rig.hand = new Node();
		rig.hand.id = "hand";
		rig.hand.translation.set(0, 1, 0);
		rig.foot = new Node();
		rig.foot.id = "foot";
		rig.foot.translation.set(0, 0, 1);
		rig.foot.rotation.setFromAxis(0, 1, 0, -60f);
		rig.root.addChild(rig.spine);
		rig.spine.addChild(rig.hand);
		rig.root.addChild(rig.foot);
		rig.model.nodes.add(rig.root);
		rig.model.calculateTransforms();

		rig.instance = new ModelInstance(rig.model);
		rig.root = rig.instance.getNode("root");
		rig.spine = rig.instance.getNode("spine");
		rig.hand = rig.instance.getNode("hand");
		rig.foot = rig.instance.getNode("foot");
		rig.instance.transform.setToRotation(new Vector3(0, 1, 0), 50f).scale(1.1f, 0.9f, 1.05f).trn(3, -2, 4);

		rig.part = new NodePart();
		rig.part.invBoneBindTransforms = new ArrayMap<>(Node.class, Matrix4.class);
		// Deliberately non-hierarchical slot order and non-unit inverse bind matrices.
		rig.part.invBoneBindTransforms.put(rig.hand,
			new Matrix4().setToTranslation(-1, -1, 0).mul(new Matrix4().setToRotation(new Vector3(0, 0, 1), 20f)));
		rig.part.invBoneBindTransforms.put(rig.foot, new Matrix4().setToTranslation(0, 2, -2));
		rig.part.invBoneBindTransforms.put(rig.spine, new Matrix4().setToScaling(2, 2, 2));
		rig.part.invBoneBindTransforms.put(rig.root,
			new Matrix4().setToRotation(new Vector3(0, 1, 0), 35f).mul(new Matrix4().setToTranslation(1, 1, 1)));
		rig.part.bones = new Matrix4[SLOTS];
		for (int i = 0; i < SLOTS; i++)
			rig.part.bones[i] = new Matrix4();
		rig.root.parts.add(rig.part);

		rig.clipA = new Animation();
		rig.clipA.id = "A";
		rig.clipA.duration = 1;
		final NodeAnimation aSpine = new NodeAnimation();
		aSpine.node = rig.spine;
		aSpine.translation = vecKeys(0, new Vector3(0, 0, 0), 1, new Vector3(2, 0, 0));
		aSpine.rotation = quatKeys(0, new Quaternion(), 1, new Quaternion().setFromAxis(0, 0, 1, 90f));
		aSpine.scaling = vecKeys(0, new Vector3(1, 1, 1), 1, new Vector3(1.5f, 1.2f, 0.8f));
		rig.clipA.nodeAnimations.add(aSpine);
		final NodeAnimation aHand = new NodeAnimation();
		aHand.node = rig.hand;
		aHand.translation = vecKeys(0, new Vector3(0, 0, 0.5f));
		rig.clipA.nodeAnimations.add(aHand);

		rig.clipB = new Animation();
		rig.clipB.id = "B";
		rig.clipB.duration = 2;
		final NodeAnimation bHand = new NodeAnimation();
		bHand.node = rig.hand;
		bHand.translation = vecKeys(0, new Vector3(0, 0, 0), 2, new Vector3(0, 3, 0));
		rig.clipB.nodeAnimations.add(bHand);
		final NodeAnimation bRoot = new NodeAnimation();
		bRoot.node = rig.root;
		bRoot.scaling = vecKeys(0, new Vector3(1, 1, 1), 1, new Vector3(1.3f, 1.3f, 1.3f));
		rig.clipB.nodeAnimations.add(bRoot);

		rig.clipC = new Animation();
		rig.clipC.id = "C";
		rig.clipC.duration = 1;
		final NodeAnimation cRoot = new NodeAnimation();
		cRoot.node = rig.root;
		cRoot.rotation = quatKeys(0, new Quaternion().setFromAxis(0, 0, 1, 30f), 1,
			new Quaternion().setFromAxis(0, 0, 1, 120f));
		rig.clipC.nodeAnimations.add(cRoot);
		final NodeAnimation cFoot = new NodeAnimation();
		cFoot.node = rig.foot;
		cFoot.translation = vecKeys(0, new Vector3(0, 0, 0), 1, new Vector3(0, 0, 2));
		rig.clipC.nodeAnimations.add(cFoot);
		final NodeAnimation cSpine = new NodeAnimation();
		cSpine.node = rig.spine;
		cSpine.scaling = vecKeys(0, new Vector3(1.1f, 0.9f, 1.2f));
		rig.clipC.nodeAnimations.add(cSpine);

		rig.clips = new Animation[] {rig.clipA, rig.clipB, rig.clipC};
		rig.model.animations.addAll(rig.clipA, rig.clipB, rig.clipC);
		// The instance was created before the clips existed; copy and remap them onto the instance's own nodes.
		rig.instance.copyAnimations(rig.model.animations);
		rig.times = new float[] {0.5f, 3.0f, 0.5f};
		rig.starWeights = new double[] {0.5, 0.2, 0.3};

		final RefPose star = new RefPose(rig, rig.clips, rig.times, rig.starWeights);
		rig.observations = new Observation[3];
		rig.observations[0] = observation(rig, 0, new double[] {0.3, -0.2, 0.5}, new int[] {0, 1, 2, 3},
			new float[] {0.4f, 0.3f, 0.2f, 0.1f}, star, 1.5f);
		rig.observations[1] = observation(rig, 1, new double[] {-0.4, 0.25, 0.8}, new int[] {3, 0, 1, 2},
			new float[] {0.1f, 0.4f, 0.3f, 0.2f}, star, 0.7f,
			new double[] {0.1, -0.05, 0.02});
		rig.observations[2] = observation(rig, 2, new double[] {0.1, 0.1, -0.3}, new int[] {0, 1, 2, 3},
			new float[] {0.5f, 0.5f, 0f, 0f}, star, 1f);
		return rig;
	}

	private static Observation observation (final Rig rig, final int id, final double[] bind, final int[] slots,
		final float[] skins, final RefPose star, final float weight) {
		return observation(rig, id, bind, slots, skins, star, weight, null);
	}

	private static Observation observation (final Rig rig, final int id, final double[] bind, final int[] slots,
		final float[] skins, final RefPose star, final float weight, final double[] targetOffset) {
		final Observation template = new Observation();
		template.part = rig.part;
		template.bindPoint.set((float)bind[0], (float)bind[1], (float)bind[2]);
		template.boneSlots = slots.clone();
		template.skinWeights = skins.clone();
		final double[] point = star.point(template);
		if (targetOffset != null)
			for (int a = 0; a < 3; a++)
				point[a] += targetOffset[a];
		final Observation obs = new Observation();
		obs.part = rig.part;
		obs.bindPoint.set(template.bindPoint);
		obs.boneSlots = slots.clone();
		obs.skinWeights = skins.clone();
		obs.target.set((float)point[0], (float)point[1], (float)point[2]);
		obs.weight = weight;
		return obs;
	}

	private static Observation copyObs (final Observation src) {
		final Observation copy = new Observation();
		copy.part = src.part;
		copy.bindPoint.set(src.bindPoint);
		copy.boneSlots = src.boneSlots.clone();
		copy.skinWeights = src.skinWeights.clone();
		copy.target.set(src.target);
		copy.weight = src.weight;
		return copy;
	}

	@SuppressWarnings("unchecked")
	private static Array<NodeKeyframe<Vector3>> vecKeys (final Object... kv) {
		final Array<NodeKeyframe<Vector3>> out = new Array<>();
		for (int i = 0; i < kv.length; i += 2)
			out.add(new NodeKeyframe<Vector3>(((Number)kv[i]).floatValue(), (Vector3)kv[i + 1]));
		return out;
	}

	@SuppressWarnings("unchecked")
	private static Array<NodeKeyframe<Quaternion>> quatKeys (final Object... kv) {
		final Array<NodeKeyframe<Quaternion>> out = new Array<>();
		for (int i = 0; i < kv.length; i += 2)
			out.add(new NodeKeyframe<Quaternion>(((Number)kv[i]).floatValue(), (Quaternion)kv[i + 1]));
		return out;
	}

	/** Rebuild a skinned world point from the real palette committed to the instance. */
	private static double[] pointFromPalette (final Rig rig, final Observation obs) {
		final double[] model = new double[3];
		for (int j = 0; j < SLOTS; j++) {
			final int slot = obs.boneSlots[j];
			final float sw = obs.skinWeights[j];
			final Vector3 v = new Vector3(obs.bindPoint).mul(rig.part.bones[slot]);
			model[0] += sw * v.x;
			model[1] += sw * v.y;
			model[2] += sw * v.z;
		}
		final Vector3 world = new Vector3((float)model[0], (float)model[1], (float)model[2]).mul(rig.instance.transform);
		return new double[] {world.x, world.y, world.z};
	}

	/** Flip {@code inheritTransform} on one node and verify the candidate pose matches a scalar reference that also drops
	 * the parent transform, then restore the flag. */
	private static boolean checkDetachedInheritance (final Rig rig) {
		final Node detached = rig.hand;
		final boolean previous = detached.inheritTransform;
		try {
			detached.inheritTransform = false;
			final Preparation detachedPrep = BlendWeightSolver.prepare(rig.instance, rig.clips, rig.times,
				rig.observations);
			final double[] w = {0.3, 0.5, 0.2};
			final Evaluation evaluation = detachedPrep.evaluate(w);
			final RefPose reference = new RefPose(rig, rig.clips, rig.times, w);
			for (int o = 0; o < rig.observations.length; o++) {
				final double[] expected = reference.point(rig.observations[o]);
				for (int a = 0; a < 3; a++)
					if (Math.abs(evaluation.worldPoints[3 * o + a] - expected[a]) > 1e-9) return false;
			}
			return true;
		} finally {
			detached.inheritTransform = previous;
		}
	}

	private static double objective (final Preparation prep, final Evaluation evaluation, final double[] w,
		final double[] prior, final double lambda, final Observation[] observations) {
		double data = 0;
		for (int o = 0; o < observations.length; o++) {
			for (int a = 0; a < 3; a++) {
				final double d = evaluation.worldPoints[3 * o + a] - targetComponent(observations[o], a);
				data += observations[o].weight * d * d;
			}
		}
		double reg = 0;
		for (int k = 0; k < w.length; k++) {
			final double d = w[k] - prior[k];
			reg += d * d;
		}
		return 0.5 * data + 0.5 * lambda * reg;
	}

	private static boolean isSimplex (final double[] w) {
		double sum = 0;
		for (double v : w) {
			if (!(v >= -1e-12)) return false;
			sum += v;
		}
		return Math.abs(sum - 1) < 1e-9;
	}

	private static double sum (final double[] v) {
		double s = 0;
		for (double d : v)
			s += d;
		return s;
	}

	private static void assertArrayClose (final Checks checks, final double[] actual, final double[] expected,
		final double tol, final String message) {
		checks.check(actual.length == expected.length, message + " (length)");
		for (int i = 0; i < actual.length; i++)
			checks.check(Math.abs(actual[i] - expected[i]) <= tol, message + " (index " + i + ")");
	}

	private static void assertFloatArrayClose (final Checks checks, final float[] actual, final float[] expected,
		final float tol, final String message) {
		checks.check(actual.length == expected.length, message + " (length)");
		for (int i = 0; i < actual.length; i++)
			checks.check(Math.abs(actual[i] - expected[i]) <= tol, message + " (index " + i + ")");
	}

	private static void expectIae (final Checks checks, final String message, final ThrowingRunnable body) {
		boolean threw = false;
		try {
			body.run();
		} catch (final IllegalArgumentException expected) {
			threw = true;
		} catch (final Throwable other) {
			throw new AssertionError(message + " threw " + other.getClass().getSimpleName(), other);
		}
		checks.check(threw, message);
	}

	private interface ThrowingRunnable {
		void run () throws Exception;
	}

	private static final class Player extends BaseAnimationController {
		Player (final ModelInstance instance) {
			super(instance);
		}

		void sample (final Animation clip, final float time) {
			applyAnimation(clip, time);
		}
	}

	// ------------------------------------------------------------------
	// Independent scalar forward reference (own double quaternion/matrix math)
	// ------------------------------------------------------------------

	static final class RefPose {
		final Node[] nodes;
		final int[] parent;
		final double[][] global;
		final double[] instanceMatrix = new double[16];

		RefPose (final Rig rig, final Animation[] clips, final float[] times, final double[] weights) {
			final Array<Node> flat = new Array<>();
			collect(rig.root, flat);
			nodes = flat.toArray(Node.class);
			parent = new int[nodes.length];
			final double[][][] t = new double[nodes.length][][];
			final double[][][] r = new double[nodes.length][][];
			final double[][][] s = new double[nodes.length][][];			for (int i = 0; i < nodes.length; i++) {
				final Node p = nodes[i].getParent();
				parent[i] = p == null ? -1 : indexOf(p);
				t[i] = new double[clips.length][3];
				r[i] = new double[clips.length][4];
				s[i] = new double[clips.length][3];
				final double[] restR = quat(nodes[i].rotation);
				for (int k = 0; k < clips.length; k++) {
					NodeAnimation track = null;
					for (NodeAnimation candidate : clips[k].nodeAnimations)
						if (candidate.node == nodes[i]) track = candidate;
					if (track == null || track.translation == null)
						copy(t[i][k], nodes[i].translation.x, nodes[i].translation.y, nodes[i].translation.z);
					else
						sampleVec(track.translation, times[k], t[i][k]);
					if (track == null || track.rotation == null)
						System.arraycopy(restR, 0, r[i][k], 0, 4);
					else {
						sampleQuat(track.rotation, times[k], r[i][k]);
						if (dot(restR, r[i][k]) < 0)
							for (int a = 0; a < 4; a++)
								r[i][k][a] = -r[i][k][a];
					}
					if (track == null || track.scaling == null)
						copy(s[i][k], nodes[i].scale.x, nodes[i].scale.y, nodes[i].scale.z);
					else
						sampleVec(track.scaling, times[k], s[i][k]);
				}
			}
			global = new double[nodes.length][16];
			for (int i = 0; i < nodes.length; i++) {
				final double[] bt = {0, 0, 0};
				final double[] br = {0, 0, 0, 0};
				final double[] bs = {0, 0, 0};
				final double[] restR = quat(nodes[i].rotation);
				for (int k = 0; k < clips.length; k++) {
					final double w = weights[k];
					for (int a = 0; a < 3; a++) {
						bt[a] += w * t[i][k][a];
						bs[a] += w * s[i][k][a];
					}
					final double sign = dot(restR, r[i][k]) < 0 ? -w : w;
					for (int a = 0; a < 4; a++)
						br[a] += sign * r[i][k][a];
				}
				normalize(br);
				final double[] local = trs(bt, br, bs);
				if (parent[i] >= 0 && nodes[i].inheritTransform)
					mul(global[i], global[parent[i]], local);
				else
					System.arraycopy(local, 0, global[i], 0, 16);
			}
			for (int a = 0; a < 16; a++)
				instanceMatrix[a] = rig.instance.transform.val[a];
		}

		double[] point (final Observation obs) {
			final double[] model = {0, 0, 0};
			final double[] bind = {obs.bindPoint.x, obs.bindPoint.y, obs.bindPoint.z};
			for (int j = 0; j < SLOTS; j++) {
				final Node bone = obs.part.invBoneBindTransforms.keys[obs.boneSlots[j]];
				final Matrix4 invBind = obs.part.invBoneBindTransforms.values[obs.boneSlots[j]];
				final double[] inv = new double[16];
				for (int a = 0; a < 16; a++)
					inv[a] = invBind.val[a];
				final double[] bound = xform(inv, bind);
				final double[] skinned = xform(global[indexOf(bone)], bound);
				final double sw = obs.skinWeights[j];
				for (int a = 0; a < 3; a++)
					model[a] += sw * skinned[a];
			}
			return xform(instanceMatrix, model);
		}

		private int indexOf (final Node node) {
			for (int i = 0; i < nodes.length; i++)
				if (nodes[i] == node) return i;
			throw new IllegalStateException("unknown node");
		}

		private static void collect (final Node node, final Array<Node> out) {
			out.add(node);
			for (Node child : node.getChildren())
				collect(child, out);
		}
	}

	private static void sampleVec (final Array<NodeKeyframe<Vector3>> channel, final float time, final double[] out) {
		final int last = channel.size - 1;
		if (last == 0) {
			copy(out, channel.get(0).value.x, channel.get(0).value.y, channel.get(0).value.z);
			return;
		}
		final float first = channel.get(0).keytime;
		final float end = channel.get(last).keytime;
		final float clamped = Math.max(first, Math.min(end, time));
		int idx = 0;
		while (idx < last && channel.get(idx + 1).keytime <= clamped)
			idx++;
		final NodeKeyframe<Vector3> a = channel.get(idx);
		if (idx == last) {
			copy(out, a.value.x, a.value.y, a.value.z);
			return;
		}
		final NodeKeyframe<Vector3> b = channel.get(idx + 1);
		final double alpha = (clamped - a.keytime) / (b.keytime - a.keytime);
		for (int c = 0; c < 3; c++)
			out[c] = component(a.value, c) + (component(b.value, c) - component(a.value, c)) * alpha;
	}

	private static void sampleQuat (final Array<NodeKeyframe<Quaternion>> channel, final float time, final double[] out) {
		final int last = channel.size - 1;
		if (last == 0) {
			System.arraycopy(quat(channel.get(0).value), 0, out, 0, 4);
			normalize(out);
			return;
		}
		final float first = channel.get(0).keytime;
		final float end = channel.get(last).keytime;
		final float clamped = Math.max(first, Math.min(end, time));
		int idx = 0;
		while (idx < last && channel.get(idx + 1).keytime <= clamped)
			idx++;
		final double[] a = quat(channel.get(idx).value);
		if (idx == last) {
			System.arraycopy(a, 0, out, 0, 4);
			return;
		}
		final double[] b = quat(channel.get(idx + 1).value);
		slerp(out, a, b, (clamped - channel.get(idx).keytime)
			/ (channel.get(idx + 1).keytime - channel.get(idx).keytime));
	}

	private static double component (final Vector3 v, final int c) {
		return c == 0 ? v.x : c == 1 ? v.y : v.z;
	}

	private static double targetComponent (final Observation obs, final int c) {
		return component(obs.target, c);
	}

	private static void copy (final double[] out, final double x, final double y, final double z) {
		out[0] = x;
		out[1] = y;
		out[2] = z;
	}

	private static double[] quat (final Quaternion q) {
		final double[] out = {q.x, q.y, q.z, q.w};
		normalize(out);
		return out;
	}

	private static double dot (final double[] a, final double[] b) {
		return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
	}

	private static void normalize (final double[] v) {
		double len = 0;
		for (double x : v)
			len += x * x;
		len = Math.sqrt(len);
		for (int i = 0; i < v.length; i++)
			v[i] /= len;
	}

	private static void slerp (final double[] out, final double[] aIn, final double[] bIn, final double t) {
		final double[] a = aIn.clone();
		final double[] b = bIn.clone();
		double d = dot(a, b);
		if (d < 0) {
			d = -d;
			for (int i = 0; i < 4; i++)
				b[i] = -b[i];
		}
		double s0 = 1 - t;
		double s1 = t;
		if (1 - d > 0.1) {
			final double theta = Math.acos(Math.min(1, Math.max(-1, d)));
			final double sin = Math.sin(theta);
			s0 = Math.sin((1 - t) * theta) / sin;
			s1 = Math.sin(t * theta) / sin;
		}
		for (int i = 0; i < 4; i++)
			out[i] = s0 * a[i] + s1 * b[i];
		normalize(out);
	}

	private static int mi (final int col, final int row) {
		return col * 4 + row;
	}

	private static double[] trs (final double[] t, final double[] r, final double[] s) {
		final double x = r[0], y = r[1], z = r[2], w = r[3];
		final double xs = x * 2, ys = y * 2, zs = z * 2;
		final double wx = w * xs, wy = w * ys, wz = w * zs;
		final double xx = x * xs, xy = x * ys, xz = x * zs;
		final double yy = y * ys, yz = y * zs, zz = z * zs;
		final double[] m = new double[16];
		m[mi(0, 0)] = s[0] * (1 - (yy + zz));
		m[mi(0, 1)] = s[0] * (xy + wz);
		m[mi(0, 2)] = s[0] * (xz - wy);
		m[mi(1, 0)] = s[1] * (xy - wz);
		m[mi(1, 1)] = s[1] * (1 - (xx + zz));
		m[mi(1, 2)] = s[1] * (yz + wx);
		m[mi(2, 0)] = s[2] * (xz + wy);
		m[mi(2, 1)] = s[2] * (yz - wx);
		m[mi(2, 2)] = s[2] * (1 - (xx + yy));
		m[mi(3, 0)] = t[0];
		m[mi(3, 1)] = t[1];
		m[mi(3, 2)] = t[2];
		m[mi(3, 3)] = 1;
		return m;
	}

	private static void mul (final double[] out, final double[] a, final double[] b) {
		for (int col = 0; col < 4; col++)
			for (int row = 0; row < 4; row++) {
				double sum = 0;
				for (int k = 0; k < 4; k++)
					sum += a[mi(k, row)] * b[mi(col, k)];
				out[mi(col, row)] = sum;
			}
	}

	private static double[] xform (final double[] m, final double[] v) {
		final double[] out = new double[3];
		for (int row = 0; row < 3; row++)
			out[row] = m[mi(0, row)] * v[0] + m[mi(1, row)] * v[1] + m[mi(2, row)] * v[2] + m[mi(3, row)];
		return out;
	}
}
