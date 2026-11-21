/*******************************************************************************
 * Copyright 2011 See AUTHORS file.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 ******************************************************************************/

package com.badlogic.gdx.graphics.g3d.utils;

import com.badlogic.gdx.graphics.g3d.ModelInstance;
import com.badlogic.gdx.graphics.g3d.model.Animation;
import com.badlogic.gdx.graphics.g3d.model.Node;
import com.badlogic.gdx.graphics.g3d.model.NodePart;
import com.badlogic.gdx.math.Matrix4;
import com.badlogic.gdx.math.Vector3;

import java.util.Arrays;

/** Solves a single set of animation blend weights shared by the whole skeleton, from skinned point observations in world
 * space.
 * <p>
 * The unknowns are one weight per clip; the same weights are applied to every node of the {@link ModelInstance}. There are
 * no per-joint weights and no joint angle unknowns. All candidate poses come from the shared multi-clip forward pose
 * implemented in {@link WeightedAnimationPose}, which is also the implementation backing
 * {@link BaseAnimationController#applyWeightedAnimations}; this solver never perturbs, resamples or finite-differences the
 * instance and never writes to its nodes, palettes or playback state while evaluating.
 * <p>
 * For an observation with skin weights {@code s_j}, bone slots {@code j}, bind space point {@code b} and instance world
 * transform {@code T}, the modeled world point is
 *
 * <pre>
 * p(w) = T * sum_j s_j * (G_j(w) * inverseBind_j * b)
 * </pre>
 *
 * where {@code G_j(w)} is the bone node's global transform under the blended pose. The objective minimized on the
 * probability simplex is
 *
 * <pre>
 * F(w) = 0.5 * sum_o observationWeight_o * ||p_o(w) - target_o||^2  +  0.5 * lambda * ||w - prior||^2
 * </pre>
 *
 * Iterations are feasible projected-gradient steps with backtracking, so every accepted point stays on the simplex and F
 * never increases. The problem is not assumed convex; no global optimum is guaranteed.
 * <p>
 * The whole implementation is pure JDK plus the bundled libGDX sources: no JNI, no OpenGL.
 */
public final class BlendWeightSolver {
	/** Number of bone slots every observation carries. */
	public static final int BONE_SLOTS = 4;
	/** Accepted tolerance for the unit sum of caller supplied simplex vectors. */
	public static final double SIMPLEX_SUM_TOLERANCE = 1e-8;
	private static final double SKIN_SUM_TOLERANCE = 1e-5;
	private static final double ARMIJO_C1 = 1e-4;
	private static final double MIN_STEP = 1e-16;
	private static final double MAX_STEP = 1e6;

	private BlendWeightSolver () {
	}

	/** One skinned point observation. All arrays are copied numerically by {@link #prepare}; after preparation the solver no
	 * longer reads the caller's arrays. */
	public static final class Observation {
		/** The skinned part of the target {@link ModelInstance}; its {@code invBoneBindTransforms} defines the slot order. */
		public NodePart part;
		/** The point in bind (skin) space, i.e. the vector fed through {@code inverseBind} matrices. */
		public final Vector3 bindPoint = new Vector3();
		/** Four bone slot indices into {@code part.invBoneBindTransforms}; slot order may differ between observations. */
		public int[] boneSlots = new int[BONE_SLOTS];
		/** Four non-negative skin weights summing to one, aligned with {@link #boneSlots}. */
		public float[] skinWeights = new float[BONE_SLOTS];
		/** The desired world space position of the skinned point. */
		public final Vector3 target = new Vector3();
		/** Positive observation weight. */
		public float weight = 1f;

		/** Creates an empty observation; fill its public fields before calling {@link #prepare}. */
		public Observation () {
		}

		/** Convenience constructor. The arrays are copied.
		 * @param part the skinned part owned by the target {@link ModelInstance}
		 * @param bindPoint bind space point
		 * @param boneSlots four valid slot indices into the part's inverse bind table
		 * @param skinWeights four non-negative skin weights summing to one
		 * @param target desired world point
		 * @param observationWeight strictly positive observation weight */
		public Observation (final NodePart part, final Vector3 bindPoint, final int[] boneSlots, final float[] skinWeights,
			final Vector3 target, final float observationWeight) {
			this.part = part;
			this.bindPoint.set(bindPoint);
			this.target.set(target);
			this.weight = observationWeight;
			setBoneSlots(boneSlots);
			setSkinWeights(skinWeights);
		}

		/** Copies the four bone slot indices. */
		public void setBoneSlots (final int[] slots) {
			if (slots == null || slots.length != BONE_SLOTS)
				throw new IllegalArgumentException("boneSlots must have length " + BONE_SLOTS);
			boneSlots = slots.clone();
		}

		/** Copies the four skin weights. */
		public void setSkinWeights (final float[] weights) {
			if (weights == null || weights.length != BONE_SLOTS)
				throw new IllegalArgumentException("skinWeights must have length " + BONE_SLOTS);
			skinWeights = weights.clone();
		}
	}

	/** Immutable, self contained result of {@link #prepare}. Holds its own copies of every input number and its own scratch
	 * space, so preparations for different instances can be evaluated and solved interleaved. */
	public static final class Preparation {
		final WeightedAnimationPose pose;
		final int observationCount;
		/** One entry per observation: four bone node indices into {@link WeightedAnimationPose#nodes}. */
		final int[][] boneNodeIndex;
		/** One entry per observation: four bind-space points {@code inverseBind_j * bindPoint}, each length 3. */
		final double[][][] slotBindPoint;
		final double[][] skinWeight;
		final double[][] target;
		final double[] observationWeight;
		/** The parts referenced by the observations, in observation order, for inspection by callers. */
		public final NodePart[] parts;

		Preparation (final WeightedAnimationPose pose, final int observationCount, final int[][] boneNodeIndex,
			final double[][][] slotBindPoint, final double[][] skinWeight, final double[][] target,
			final double[] observationWeight, final NodePart[] parts) {
			this.pose = pose;
			this.observationCount = observationCount;
			this.boneNodeIndex = boneNodeIndex;
			this.slotBindPoint = slotBindPoint;
			this.skinWeight = skinWeight;
			this.target = target;
			this.observationWeight = observationWeight;
			this.parts = parts;
		}

		/** @return the number of clips (blend weights) fixed at preparation. */
		public int clipCount () {
			return pose.clipCount();
		}

		/** @return the number of observations fixed at preparation. */
		public int observationCount () {
			return observationCount;
		}

		/** @return the instance this preparation evaluates. */
		public ModelInstance instance () {
			return pose.instance;
		}

		/** Evaluate world points and the analytic weight Jacobian without touching the instance.
		 * @param weights blend weights, length {@link #clipCount()}, all finite
		 * @return a freshly allocated, independently owned {@link Evaluation} */
		public Evaluation evaluate (final double[] weights) {
			return BlendWeightSolver.evaluate(this, weights);
		}
	}

	/** Output of one forward evaluation. The arrays are owned by this object and never shared with later evaluations. */
	public static final class Evaluation {
		/** World points: {@code worldPoints[3*o + axis]} for observation {@code o} and axis 0=x, 1=y, 2=z. */
		public final double[] worldPoints;
		/** Analytic Jacobian of the world points with respect to the clip weights. {@code jacobian[o][3*k + axis]} is
		 * {@code d p_o(axis) / d w_k}; rows belong to observations in preparation order, the three columns of one clip are
		 * contiguous. */
		public final double[][] jacobian;
		/** The weights that produced this evaluation, copied. */
		public final double[] weights;

		Evaluation (final int observationCount, final int clipCount, final double[] weights) {
			this.worldPoints = new double[observationCount * 3];
			this.jacobian = new double[observationCount][clipCount * 3];
			this.weights = weights.clone();
		}
	}

	/** Result of {@link #solveAndApply}. Independently owned; keeping it does not alias any solver workspace. */
	public static final class Result {
		/** Final feasible blend weights on the simplex, length equal to the number of clips. */
		public final double[] weights;
		/** Final objective value {@code F(w)}. */
		public final double objective;
		/** Number of iterations whose candidate point was accepted (each strictly kept F from increasing). */
		public final int acceptedIterations;
		/** Projected gradient residual {@code ||w - ProjectSimplex(w - gradientF)||_2}. */
		public final double projectedGradientResidual;
		/** True only if {@link #projectedGradientResidual} is at most the caller supplied tolerance. */
		public final boolean converged;

		Result (final double[] weights, final double objective, final int acceptedIterations,
			final double projectedGradientResidual, final boolean converged) {
			this.weights = weights;
			this.objective = objective;
			this.acceptedIterations = acceptedIterations;
			this.projectedGradientResidual = projectedGradientResidual;
			this.converged = converged;
		}
	}

	/** Validate and snapshot everything needed to evaluate the weighted pose and the observations.
	 * @param instance the single instance every clip, node part and bone slot must belong to
	 * @param clips the animations to blend (repeated clips are legal); fixed for the life of the preparation
	 * @param times sample time in seconds per clip, fixed for the life of the preparation
	 * @param observations skinned point observations; all numeric inputs are copied
	 * @return an independent preparation
	 * @throws IllegalArgumentException for any malformed input; the instance, existing reports and caller arrays stay
	 *            untouched when this is thrown */
	public static Preparation prepare (final ModelInstance instance, final Animation[] clips, final float[] times,
		final Observation[] observations) {
		if (observations == null || observations.length == 0)
			throw new IllegalArgumentException("observations must contain at least one entry");
		// Build the pose first: it performs all clip/channel/node/instance-transform validation without writing the instance.
		final WeightedAnimationPose pose = new WeightedAnimationPose(instance, clips, times);
		final int clipCount = clips.length;
		final int observationCount = observations.length;

		final int[][] boneNodeIndex = new int[observationCount][BONE_SLOTS];
		final double[][][] inverseBind = new double[observationCount][BONE_SLOTS][16];
		final double[][][] slotBindPoint = new double[observationCount][BONE_SLOTS][3];
		final double[][] skinWeight = new double[observationCount][BONE_SLOTS];
		final double[][] target = new double[observationCount][3];
		final double[] observationWeight = new double[observationCount];
		final NodePart[] parts = new NodePart[observationCount];

		try {
			for (int o = 0; o < observationCount; o++) {
				final Observation obs = observations[o];
				if (obs == null) throw new IllegalArgumentException("observations[" + o + "] is null");
				final NodePart part = obs.part;
				if (part == null) throw new IllegalArgumentException("observations[" + o + "].part is null");
				if (part.invBoneBindTransforms == null)
					throw new IllegalArgumentException("observations[" + o + "].part has no inverse bind transforms");
				if (part.bones == null || part.bones.length != part.invBoneBindTransforms.size)
					throw new IllegalArgumentException("observations[" + o
						+ "].part has no bone palette matching its inverse bind transforms");
				if (!partBelongsToInstance(pose, part))
					throw new IllegalArgumentException("observations[" + o + "].part does not belong to the target instance");
				final int slotCount = part.invBoneBindTransforms.size;
				if (obs.boneSlots == null || obs.boneSlots.length != BONE_SLOTS)
					throw new IllegalArgumentException("observations[" + o + "] must provide " + BONE_SLOTS + " bone slots");
				if (obs.skinWeights == null || obs.skinWeights.length != BONE_SLOTS)
					throw new IllegalArgumentException("observations[" + o + "] must provide " + BONE_SLOTS + " skin weights");

				double skinSum = 0.0;
				for (int j = 0; j < BONE_SLOTS; j++) {
					final int slot = obs.boneSlots[j];
					if (slot < 0 || slot >= slotCount)
						throw new IllegalArgumentException("observations[" + o + "] bone slot " + j + " is out of range");
					final float sw = obs.skinWeights[j];
					if (!Float.isFinite(sw) || sw < 0f)
						throw new IllegalArgumentException("observations[" + o + "] skin weight " + j + " must be finite and >= 0");
					skinSum += sw;
					final Node bone = part.invBoneBindTransforms.keys[slot];
					final Matrix4 invBind = part.invBoneBindTransforms.values[slot];
					if (bone == null || invBind == null)
						throw new IllegalArgumentException("observations[" + o + "] bone slot " + slot + " is null");
					final int nodeIdx = pose.indexOf(bone);
					if (nodeIdx < 0)
						throw new IllegalArgumentException("observations[" + o + "] bone does not belong to the target instance");
					boneNodeIndex[o][j] = nodeIdx;
					for (int a = 0; a < 16; a++) {
						final float v = invBind.val[a];
						if (!Float.isFinite(v))
							throw new IllegalArgumentException("non-finite inverse bind matrix in observations[" + o + "]");
						inverseBind[o][j][a] = v;
					}
					skinWeight[o][j] = sw;
				}
				if (Math.abs(skinSum - 1.0) > SKIN_SUM_TOLERANCE)
					throw new IllegalArgumentException("observations[" + o + "] skin weights must sum to one (sum=" + skinSum
						+ ")");

				if (!finite(obs.bindPoint))
					throw new IllegalArgumentException("observations[" + o + "].bindPoint must be finite");
				if (!finite(obs.target))
					throw new IllegalArgumentException("observations[" + o + "].target must be finite");
				if (!Float.isFinite(obs.weight) || obs.weight <= 0f)
					throw new IllegalArgumentException("observations[" + o + "].weight must be strictly positive");
				observationWeight[o] = obs.weight;
				target[o][0] = obs.target.x;
				target[o][1] = obs.target.y;
				target[o][2] = obs.target.z;
				parts[o] = part;

				final double[] bind = {obs.bindPoint.x, obs.bindPoint.y, obs.bindPoint.z};
				for (int j = 0; j < BONE_SLOTS; j++) {
					// bind point of slot j: inverseBind_j * bindPoint as a point (w = 1)
					WeightedAnimationPose.mulVec(slotBindPoint[o][j], inverseBind[o][j], bind);
				}
			}
		} catch (final RuntimeException ex) {
			// The preparation is discarded; nothing was written to the instance or caller arrays.
			throw ex;
		}
		return new Preparation(pose, observationCount, boneNodeIndex, slotBindPoint, skinWeight, target,
			observationWeight, parts);
	}

	private static boolean partBelongsToInstance (final WeightedAnimationPose pose, final NodePart part) {
		for (final Node node : pose.nodes) {
			for (final NodePart candidate : node.parts) {
				if (candidate == part) return true;
			}
		}
		return false;
	}

	/** Run the no-write forward evaluation and analytic Jacobian computation.
	 * @param preparation a preparation returned by {@link #prepare}
	 * @param weights blend weights, length {@link Preparation#clipCount()}, all finite
	 * @return independently owned world points and Jacobian */
	public static Evaluation evaluate (final Preparation preparation, final double[] weights) {
		if (preparation == null) throw new IllegalArgumentException("preparation must not be null");
		if (weights == null || weights.length != preparation.clipCount())
			throw new IllegalArgumentException("weights length must match the number of clips");
		for (double w : weights)
			if (!Double.isFinite(w)) throw new IllegalArgumentException("weights must be finite");
		final WeightedAnimationPose pose = preparation.pose;
		pose.evaluate(weights);
		final int n = preparation.observationCount;
		final int clipCount = preparation.clipCount();
		final Evaluation evaluation = new Evaluation(n, clipCount, weights);
		final double[] instanceMatrix = pose.instanceMatrix();
		final double[][][] dGlobal = pose.derivativeGlobals();

		for (int o = 0; o < n; o++) {
			final double[] modelPoint = new double[3];
			final double[][] dModel = new double[clipCount][3];
			for (int j = 0; j < BONE_SLOTS; j++) {
				final double sw = preparation.skinWeight[o][j];
				if (sw == 0.0) continue;
				final int nodeIdx = preparation.boneNodeIndex[o][j];
				final double[] global = pose.globalMatrix(nodeIdx);
				final double[] bp = preparation.slotBindPoint[o][j];
				final double[] transformed = new double[3];
				WeightedAnimationPose.mulVec(transformed, global, bp);
				for (int a = 0; a < 3; a++)
					modelPoint[a] += sw * transformed[a];
				for (int k = 0; k < clipCount; k++) {
					final double[] dTransformed = new double[3];
					WeightedAnimationPose.mulVec(dTransformed, dGlobal[nodeIdx][k], bp);
					for (int a = 0; a < 3; a++)
						dModel[k][a] += sw * dTransformed[a];
				}
			}
			final double[] world = new double[3];
			WeightedAnimationPose.mulVec(world, instanceMatrix, modelPoint);
			evaluation.worldPoints[3 * o] = world[0];
			evaluation.worldPoints[3 * o + 1] = world[1];
			evaluation.worldPoints[3 * o + 2] = world[2];
			for (int k = 0; k < clipCount; k++) {
				final double[] dWorld = new double[3];
				// Derivatives are direction vectors: the instance translation must not enter the Jacobian.
				WeightedAnimationPose.mulDirection(dWorld, instanceMatrix, dModel[k]);
				evaluation.jacobian[o][3 * k] = dWorld[0];
				evaluation.jacobian[o][3 * k + 1] = dWorld[1];
				evaluation.jacobian[o][3 * k + 2] = dWorld[2];
			}
		}
		return evaluation;
	}

	/** Minimize {@code F} on the simplex starting from {@code initial}, then commit the final feasible pose to the instance
	 * through the regular {@link ModelInstance#calculateTransforms()} pipeline (real local matrices, hierarchy and bone
	 * palette). If the iteration budget is exhausted the last feasible point is still applied, but
	 * {@link Result#converged} is {@code false}. After this method returns normal animation playback can take over again.
	 * @param preparation a preparation returned by {@link #prepare}
	 * @param initial starting point; finite, non-negative, summing to one
	 * @param prior Tikhonov prior; finite, non-negative, summing to one
	 * @param lambda regularization strength, strictly positive
	 * @param tolerance convergence tolerance for the projected gradient residual, strictly positive
	 * @param maxIterations maximum number of projected-gradient iterations, zero or positive
	 * @return an independently owned report */
	public static Result solveAndApply (final Preparation preparation, final double[] initial, final double[] prior,
		final double lambda, final double tolerance, final int maxIterations) {
		if (preparation == null) throw new IllegalArgumentException("preparation must not be null");
		final int clipCount = preparation.clipCount();
		if (initial == null || initial.length != clipCount)
			throw new IllegalArgumentException("initial length must match the number of clips");
		if (prior == null || prior.length != clipCount)
			throw new IllegalArgumentException("prior length must match the number of clips");
		if (!(lambda > 0.0) || !Double.isFinite(lambda))
			throw new IllegalArgumentException("lambda must be strictly positive and finite");
		if (!(tolerance > 0.0) || !Double.isFinite(tolerance))
			throw new IllegalArgumentException("tolerance must be strictly positive and finite");
		if (maxIterations < 0) throw new IllegalArgumentException("maxIterations must be zero or positive");

		final double[] w = copySimplex(initial, "initial");
		final double[] priorCopy = copySimplex(prior, "prior");

		Evaluation evaluation = evaluate(preparation, w);
		double f = objective(preparation, evaluation, w, priorCopy, lambda);
		double[] gradient = gradient(preparation, evaluation, w, priorCopy, lambda);
		int accepted = 0;
		double residual = projectedGradientResidual(w, gradient);
		boolean converged = residual <= tolerance;

		double step = 1.0;
		for (int iteration = 0; !converged && iteration < maxIterations; iteration++) {
			boolean tookStep = false;
			double alpha = step;
			double candidateF = f;
			double[] candidate = w;
			while (alpha >= MIN_STEP) {
				candidate = projectSimplex(move(w, gradient, -alpha));
				if (equals(candidate, w)) break;
				final Evaluation candidateEvaluation = evaluate(preparation, candidate);
				candidateF = objective(preparation, candidateEvaluation, candidate, priorCopy, lambda);
				final double directional = dot(gradient, subtract(candidate, w));
				if (candidateF <= f + ARMIJO_C1 * directional) {
					evaluation = candidateEvaluation;
					tookStep = true;
					break;
				}
				alpha *= 0.5;
			}
			if (!tookStep) break;
			// Armijo acceptance above guarantees the objective does not increase.
			System.arraycopy(candidate, 0, w, 0, clipCount);
			f = candidateF;
			gradient = gradient(preparation, evaluation, w, priorCopy, lambda);
			residual = projectedGradientResidual(w, gradient);
			accepted++;
			step = Math.min(MAX_STEP, Math.max(step, alpha * 1.5));
			if (residual <= tolerance) converged = true;
		}

		// Commit through the shared forward pose; the instance builds its own real palette afterwards.
		preparation.pose.applyToInstance(w);
		return new Result(w.clone(), f, accepted, residual, converged);
	}

	private static double objective (final Preparation preparation, final Evaluation evaluation, final double[] w,
		final double[] prior, final double lambda) {
		double sum = 0.0;
		for (int o = 0; o < preparation.observationCount; o++) {
			final double dx = evaluation.worldPoints[3 * o] - preparation.target[o][0];
			final double dy = evaluation.worldPoints[3 * o + 1] - preparation.target[o][1];
			final double dz = evaluation.worldPoints[3 * o + 2] - preparation.target[o][2];
			sum += preparation.observationWeight[o] * (dx * dx + dy * dy + dz * dz);
		}
		double reg = 0.0;
		for (int k = 0; k < w.length; k++) {
			final double d = w[k] - prior[k];
			reg += d * d;
		}
		return 0.5 * sum + 0.5 * lambda * reg;
	}

	private static double[] gradient (final Preparation preparation, final Evaluation evaluation, final double[] w,
		final double[] prior, final double lambda) {
		final int clipCount = w.length;
		final double[] g = new double[clipCount];
		for (int o = 0; o < preparation.observationCount; o++) {
			final double rx = evaluation.worldPoints[3 * o] - preparation.target[o][0];
			final double ry = evaluation.worldPoints[3 * o + 1] - preparation.target[o][1];
			final double rz = evaluation.worldPoints[3 * o + 2] - preparation.target[o][2];
			final double ow = preparation.observationWeight[o];
			for (int k = 0; k < clipCount; k++) {
				g[k] += ow * (rx * evaluation.jacobian[o][3 * k] + ry * evaluation.jacobian[o][3 * k + 1]
					+ rz * evaluation.jacobian[o][3 * k + 2]);
			}
		}
		for (int k = 0; k < clipCount; k++)
			g[k] += lambda * (w[k] - prior[k]);
		return g;
	}

	/** Project onto the canonical probability simplex {@code {v >= 0, sum v = 1}} (Duchi et al. 2008). */
	public static double[] projectSimplex (final double[] v) {
		final int n = v.length;
		final double[] sorted = v.clone();
		Arrays.sort(sorted);
		double cumulative = 0.0;
		double theta = 0.0;
		for (int i = 0; i < n; i++) {
			final double value = sorted[n - 1 - i];
			cumulative += value;
			final double candidate = (cumulative - 1.0) / (i + 1);
			if (value - candidate > 0.0) theta = candidate;
		}
		final double[] out = new double[n];
		for (int i = 0; i < n; i++)
			out[i] = Math.max(0.0, v[i] - theta);
		return out;
	}

	private static double projectedGradientResidual (final double[] w, final double[] gradient) {
		final double[] projected = projectSimplex(subtract(w, gradient));
		return norm2(subtract(w, projected));
	}

	private static double[] copySimplex (final double[] v, final String name) {
		final double[] out = new double[v.length];
		double sum = 0.0;
		for (int i = 0; i < v.length; i++) {
			if (!Double.isFinite(v[i])) throw new IllegalArgumentException(name + " must contain finite values");
			if (v[i] < -SIMPLEX_SUM_TOLERANCE)
				throw new IllegalArgumentException(name + " must be non-negative");
			out[i] = Math.max(0.0, v[i]);
			sum += out[i];
		}
		if (Math.abs(sum - 1.0) > SIMPLEX_SUM_TOLERANCE)
			throw new IllegalArgumentException(name + " must sum to one (sum=" + sum + ")");
		for (int i = 0; i < out.length; i++)
			out[i] /= sum;
		return out;
	}

	private static double[] move (final double[] v, final double[] direction, final double scale) {
		final double[] out = new double[v.length];
		for (int i = 0; i < v.length; i++)
			out[i] = v[i] + scale * direction[i];
		return out;
	}

	private static double[] subtract (final double[] a, final double[] b) {
		final double[] out = new double[a.length];
		for (int i = 0; i < a.length; i++)
			out[i] = a[i] - b[i];
		return out;
	}

	private static double dot (final double[] a, final double[] b) {
		double sum = 0.0;
		for (int i = 0; i < a.length; i++)
			sum += a[i] * b[i];
		return sum;
	}

	private static double norm2 (final double[] v) {
		return Math.sqrt(dot(v, v));
	}

	private static boolean equals (final double[] a, final double[] b) {
		for (int i = 0; i < a.length; i++)
			if (a[i] != b[i]) return false;
		return true;
	}

	private static boolean finite (final Vector3 v) {
		return Float.isFinite(v.x) && Float.isFinite(v.y) && Float.isFinite(v.z);
	}
}
