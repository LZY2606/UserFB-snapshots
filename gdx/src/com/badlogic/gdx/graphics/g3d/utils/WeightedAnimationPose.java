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
import com.badlogic.gdx.graphics.g3d.model.NodeAnimation;
import com.badlogic.gdx.graphics.g3d.model.NodeKeyframe;
import com.badlogic.gdx.math.Quaternion;
import com.badlogic.gdx.math.Vector3;
import com.badlogic.gdx.utils.Array;
import com.badlogic.gdx.utils.ObjectMap;

/** Multi-clip forward pose used by both {@link BaseAnimationController#applyWeightedAnimations} and
 * {@link BlendWeightSolver}. Instances are fully self contained: every sampled channel, every rest pose value and every
 * scratch array is owned by this object, so two poses (including poses built for different {@link ModelInstance}s) can be
 * evaluated interleaved without sharing writable state.
 * <p>
 * The pose is evaluated in {@code double} precision and never writes to the {@link ModelInstance}, its nodes, its bone
 * palettes or any animation playback state. {@link #applyToInstance(double[])} is the only method that commits a result,
 * converting the final TRS values to {@code float} and letting the regular {@link ModelInstance#calculateTransforms()}
 * pipeline build the local matrices, hierarchy and palette.
 * <p>
 * Blending rules, per node:
 * <ul>
 * <li>each clip's channels are sampled like the existing controller (linear translation/scaling, shortest arc slerp for
 * rotations), but the sample time is clamped to the channel's own first/last key time;</li>
 * <li>a missing channel or a missing node track contributes the node's rest TRS;</li>
 * <li>translation and scale are the direct weighted sums of the samples;</li>
 * <li>each sampled rotation is first aligned to the hemisphere of the node's rest rotation, then
 * {@code q = normalize(sum_k w_k * q_k)};</li>
 * <li>the full affine hierarchy is rebuilt from the real parent/child relations and {@link Node#inheritTransform}.</li>
 * </ul>
 * All rest and sampled scales must be strictly positive and all quaternions must be finite and non zero; the absolute
 * dot product of every sampled rotation with the node's rest rotation must be greater than {@code 1e-6}.
 */
final class WeightedAnimationPose {
	/** Hemisphere alignment tolerance between a sampled rotation and the node's rest rotation. */
	static final double HEMISPHERE_EPSILON = 1e-6;

	/** Column-major index into a 4x4 matrix, matching {@link com.badlogic.gdx.math.Matrix4} layout. */
	private static int mi (int col, int row) {
		return col * 4 + row;
	}

	final ModelInstance instance;
	final int clipCount;

	/** All nodes of the instance in pre-order, so every parent precedes its children. */
	final Node[] nodes;
	private final ObjectMap<Node, Integer> nodeIndex = new ObjectMap<Node, Integer>();
	private final int[] parentIndex;
	private final boolean[] inherit;

	/** Rest TRS copied from the nodes (rotations normalized). */
	private final double[][] restT;
	private final double[][] restR;
	private final double[][] restS;

	/** Per-node, per-clip sampled channels: [node][clip][component]. */
	private final double[][][] sampleT;
	private final double[][][] sampleR;
	private final double[][][] sampleS;

	/** Current blended local TRS and local/global affine matrices (column major, length 16). */
	private final double[][] localT;
	private final double[][] localR;
	private final double[][] localS;
	private final double[][] localMatrix;
	private final double[][] globalMatrix;
	/** Analytic derivative of each global matrix with respect to each weight: [node][clip][16]. */
	private final double[][][] dGlobal;
	/** Norm of the quaternion accumulator {@code sum_k w_k * q_k} before normalization, per node. */
	private final double[] blendNorm;

	/** Instance world transform, copied once at construction; its invertibility is validated here. */
	private final double[] instanceMatrix = new double[16];

	/** @throws IllegalArgumentException if the clips, channels, keyframes, node rest values or instance transform violate the
	 *            documented constraints. The instance is never modified when this throws. */
	WeightedAnimationPose (final ModelInstance instance, final Animation[] clips, final float[] times) {
		if (instance == null) throw new IllegalArgumentException("instance must not be null");
		if (clips == null || clips.length == 0) throw new IllegalArgumentException("clips must contain at least one animation");
		this.instance = instance;
		this.clipCount = clips.length;
		if (times == null || times.length != clipCount)
			throw new IllegalArgumentException("times length must match clips length");
		for (int k = 0; k < clipCount; k++) {
			if (clips[k] == null) throw new IllegalArgumentException("clips[" + k + "] is null");
			if (!Float.isFinite(times[k])) throw new IllegalArgumentException("times[" + k + "] must be finite");
		}

		// Flatten the instance hierarchy.
		final Array<Node> flat = new Array<Node>();
		for (Node root : instance.nodes)
			collect(root, flat);
		nodes = flat.toArray(Node.class);
		final int nodeCount = nodes.length;
		if (nodeCount == 0) throw new IllegalArgumentException("instance has no nodes");
		parentIndex = new int[nodeCount];
		inherit = new boolean[nodeCount];
		restT = new double[nodeCount][3];
		restR = new double[nodeCount][4];
		restS = new double[nodeCount][3];
		for (int i = 0; i < nodeCount; i++) {
			final Node node = nodes[i];
			nodeIndex.put(node, i);
			final Node parent = node.getParent();
			parentIndex[i] = parent == null ? -1 : requireInstanceNode(parent, "animation track node");
			inherit[i] = node.inheritTransform;
			copyVector(node.translation, restT[i]);
			copyAndNormalizeQuaternion(node.rotation, restR[i], "rest rotation of node " + node.id);
			copyPositiveScale(node.scale, restS[i], "rest scale of node " + node.id);
		}

		sampleT = new double[nodeCount][clipCount][];
		sampleR = new double[nodeCount][clipCount][];
		sampleS = new double[nodeCount][clipCount][];

		for (int k = 0; k < clipCount; k++) {
			final ObjectMap<Node, NodeAnimation> tracks = new ObjectMap<Node, NodeAnimation>();
			for (NodeAnimation track : clips[k].nodeAnimations) {
				if (track == null || track.node == null)
					throw new IllegalArgumentException("clip " + k + " contains a node animation without a node");
				final Node node = track.node;
				if (nodeIndex.get(node, null) == null)
					throw new IllegalArgumentException("clip " + k + " animates a node that does not belong to the target instance");
				if (tracks.containsKey(node))
					throw new IllegalArgumentException("clip " + k + " contains duplicate node tracks for node " + node.id);
				tracks.put(node, track);
				validateKeyTimes(track, k, node.id);
			}
			final float time = times[k];
			for (int i = 0; i < nodeCount; i++) {
				final Node node = nodes[i];
				final NodeAnimation track = tracks.get(node, null);
				sampleT[i][k] = new double[3];
				sampleR[i][k] = new double[4];
				sampleS[i][k] = new double[3];
				if (track == null || track.translation == null) {
					System.arraycopy(restT[i], 0, sampleT[i][k], 0, 3);
				} else {
					sampleVectorChannel(track.translation, time, sampleT[i][k], "translation", k, node.id);
				}
				if (track == null || track.rotation == null) {
					System.arraycopy(restR[i], 0, sampleR[i][k], 0, 4);
				} else {
					sampleQuaternionChannel(track.rotation, time, sampleR[i][k], k, node.id);
					final double dot = dot4(restR[i], sampleR[i][k]);
					if (!Double.isFinite(dot) || Math.abs(dot) <= HEMISPHERE_EPSILON)
						throw new IllegalArgumentException("rotation sample for node " + node.id + " in clip " + k
							+ " is too close to the rest rotation hemisphere boundary (|dot|=" + Math.abs(dot) + ")");
				}
				if (track == null || track.scaling == null) {
					System.arraycopy(restS[i], 0, sampleS[i][k], 0, 3);
				} else {
					sampleVectorChannel(track.scaling, time, sampleS[i][k], "scale", k, node.id);
					for (int a = 0; a < 3; a++)
						if (!(sampleS[i][k][a] > 0.0))
							throw new IllegalArgumentException("sampled scale component for node " + node.id + " in clip " + k
								+ " must be strictly positive");
				}
			}
		}

		localT = new double[nodeCount][3];
		localR = new double[nodeCount][4];
		localS = new double[nodeCount][3];
		localMatrix = new double[nodeCount][16];
		globalMatrix = new double[nodeCount][16];
		dGlobal = new double[nodeCount][clipCount][16];
		blendNorm = new double[nodeCount];

		copyMatrix(instance.transform.val, instanceMatrix);
		for (double v : instanceMatrix)
			if (!Double.isFinite(v)) throw new IllegalArgumentException("instance.transform contains non-finite values");
		final double[] inverseCheck = new double[16];
		if (!invert4(instanceMatrix, inverseCheck))
			throw new IllegalArgumentException("instance.transform is not invertible");
	}

	int nodeCount () {
		return nodes.length;
	}

	int clipCount () {
		return clipCount;
	}

	int indexOf (final Node node) {
		final Integer idx = nodeIndex.get(node, null);
		return idx == null ? -1 : idx;
	}

	int requireInstanceNode (final Node node, final String what) {
		final int idx = indexOf(node);
		if (idx < 0) throw new IllegalArgumentException(what + " does not belong to the target instance");
		return idx;
	}

	double[] instanceMatrix () {
		return instanceMatrix;
	}

	double[] globalMatrix (final int nodeIdx) {
		return globalMatrix[nodeIdx];
	}

	double[][][] derivativeGlobals () {
		return dGlobal;
	}

	/** Evaluate the blended local TRS, the global hierarchy and the analytic global-matrix Jacobian. Only this pose's own
	 * {@code double} arrays are touched. */
	void evaluate (final double[] weights) {
		if (weights == null || weights.length != clipCount)
			throw new IllegalArgumentException("weights length must match the number of clips");
		final int nodeCount = nodes.length;
		for (int i = 0; i < nodeCount; i++) {
			final double[] t = localT[i];
			final double[] s = localS[i];
			final double[] r = localR[i];
			t[0] = t[1] = t[2] = 0.0;
			s[0] = s[1] = s[2] = 0.0;
			r[0] = r[1] = r[2] = r[3] = 0.0;
			for (int k = 0; k < clipCount; k++) {
				final double w = weights[k];
				final double[] st = sampleT[i][k];
				final double[] ss = sampleS[i][k];
				final double[] sr = sampleR[i][k];
				t[0] += w * st[0];
				t[1] += w * st[1];
				t[2] += w * st[2];
				s[0] += w * ss[0];
				s[1] += w * ss[1];
				s[2] += w * ss[2];
				// Align the sample to the rest hemisphere; the boundary was validated at preparation time.
				final double sign = dot4(restR[i], sr) < 0.0 ? -w : w;
				r[0] += sign * sr[0];
				r[1] += sign * sr[1];
				r[2] += sign * sr[2];
				r[3] += sign * sr[3];
			}
			blendNorm[i] = Math.sqrt(dot4(r, r));
			normalize4(r);
			trs(localMatrix[i], t, r, s);

			// Local matrix derivatives and global propagation.
			final int p = parentIndex[i];
			for (int k = 0; k < clipCount; k++) {
				final double[] dL = dGlobal[i][k];
				composeLocalDerivative(dL, i, k);
				if (inherit[i] && p >= 0) {
					// dG = dG_parent * L + G_parent * dL (temporaries keep mul4 free of output aliasing).
					final double[] gParent = globalMatrix[p];
					final double[] dgParent = dGlobal[p][k];
					final double[] parentTerm = new double[16];
					mul4(parentTerm, gParent, dL);
					mul4(dL, dgParent, localMatrix[i]);
					add4(dL, parentTerm);
				}
			}

			if (inherit[i] && p >= 0)
				mul4(globalMatrix[i], globalMatrix[p], localMatrix[i]);
			else
				System.arraycopy(localMatrix[i], 0, globalMatrix[i], 0, 16);
		}
	}

	/** dL_k = linR(r, dq_k) * diag(s) + R(r) * diag(ds_k) with the translation derivative in the translation column.
	 * {@code dq_k} is a quaternion tangent, so it cannot be fed through the quadratic TRS rotation formula; the rotation
	 * matrix is linearized explicitly. */
	private void composeLocalDerivative (final double[] out, final int nodeIdx, final int clipIdx) {
		final double[] r = localR[nodeIdx];
		final double[] s = localS[nodeIdx];
		final double[] dq = new double[4];
		// d(acc)/d w_k, with the same hemisphere sign used in the blend.
		final double[] sr = sampleR[nodeIdx][clipIdx];
		final double sign = dot4(restR[nodeIdx], sr) < 0.0 ? -1.0 : 1.0;
		dq[0] = sign * sr[0];
		dq[1] = sign * sr[1];
		dq[2] = sign * sr[2];
		dq[3] = sign * sr[3];
		// Derivative of normalize(acc): (dq - (r . dq) * r) / |acc|, where r = acc / |acc|.
		final double qDotDq = dot4(r, dq);
		final double invNorm = 1.0 / blendNorm[nodeIdx];
		final double dx = (dq[0] - qDotDq * r[0]) * invNorm;
		final double dy = (dq[1] - qDotDq * r[1]) * invNorm;
		final double dz = (dq[2] - qDotDq * r[2]) * invNorm;
		final double dw = (dq[3] - qDotDq * r[3]) * invNorm;
		final double x = r[0], y = r[1], z = r[2], w = r[3];

		// Linearized unit quaternion rotation matrix dR/dq * dq (row, col indexing).
		final double[][] dRot = new double[3][3];
		dRot[0][0] = -4.0 * (y * dy + z * dz);
		dRot[0][1] = 2.0 * (dx * y + x * dy - dw * z - w * dz);
		dRot[0][2] = 2.0 * (dx * z + x * dz + dw * y + w * dy);
		dRot[1][0] = 2.0 * (dx * y + x * dy + dw * z + w * dz);
		dRot[1][1] = -4.0 * (x * dx + z * dz);
		dRot[1][2] = 2.0 * (dy * z + y * dz - dw * x - w * dx);
		dRot[2][0] = 2.0 * (dx * z + x * dz - dw * y - w * dy);
		dRot[2][1] = 2.0 * (dy * z + y * dz + dw * x + w * dx);
		dRot[2][2] = -4.0 * (x * dx + y * dy);

		final double[] rot = new double[16];
		trs(rot, new double[] {0, 0, 0}, r, new double[] {1, 1, 1});
		final double[] ds = sampleS[nodeIdx][clipIdx];
		for (int col = 0; col < 3; col++) {
			for (int row = 0; row < 3; row++) {
				// linR * diag(s): column col of the linearized rotation times s[col];
				// R * diag(ds): column col of the rotation times ds[col].
				out[mi(col, row)] = dRot[row][col] * s[col] + rot[mi(col, row)] * ds[col];
			}
		}
		// Translation derivative: direct weighted sum, so derivative is the sampled translation.
		out[mi(3, 0)] = sampleT[nodeIdx][clipIdx][0];
		out[mi(3, 1)] = sampleT[nodeIdx][clipIdx][1];
		out[mi(3, 2)] = sampleT[nodeIdx][clipIdx][2];
		out[mi(0, 3)] = out[mi(1, 3)] = out[mi(2, 3)] = 0.0;
		out[mi(3, 3)] = 0.0;
	}

	/** Commit the pose for the given weights to the instance's nodes and run the regular transform/palette pipeline. */
	void applyToInstance (final double[] weights) {
		evaluate(weights);
		for (int i = 0; i < nodes.length; i++) {
			final Node node = nodes[i];
			node.translation.set((float)localT[i][0], (float)localT[i][1], (float)localT[i][2]);
			node.rotation.set((float)localR[i][0], (float)localR[i][1], (float)localR[i][2], (float)localR[i][3]).nor();
			node.scale.set((float)localS[i][0], (float)localS[i][1], (float)localS[i][2]);
			node.isAnimated = false;
		}
		// Rebuilds local matrices, the hierarchy and the real palette via Node#calculateBoneTransforms.
		instance.calculateTransforms();
	}

	// ------------------------------------------------------------------
	// Channel sampling (time clamped to the channel's own first/last key)
	// ------------------------------------------------------------------

	private static void validateKeyTimes (final NodeAnimation track, final int clipIdx, final String nodeId) {
		validateChannel(track.translation, clipIdx, nodeId, "translation", true);
		validateChannel(track.rotation, clipIdx, nodeId, "rotation", false);
		validateChannel(track.scaling, clipIdx, nodeId, "scale", true);
	}

	private static <T> void validateChannel (final Array<NodeKeyframe<T>> channel, final int clipIdx, final String nodeId,
		final String kind, final boolean vectorChannel) {
		if (channel == null || channel.size == 0) return;
		float previous = Float.NEGATIVE_INFINITY;
		for (int f = 0; f < channel.size; f++) {
			final NodeKeyframe<T> key = channel.get(f);
			if (key == null || key.value == null)
				throw new IllegalArgumentException("null keyframe in " + kind + " channel of node " + nodeId + " (clip " + clipIdx
					+ ")");
			if (!(key.keytime > previous))
				throw new IllegalArgumentException("key times must be strictly ascending in " + kind + " channel of node " + nodeId
					+ " (clip " + clipIdx + ")");
			previous = key.keytime;
			if (vectorChannel) {
				final Vector3 v = (Vector3)key.value;
				if (!Float.isFinite(v.x) || !Float.isFinite(v.y) || !Float.isFinite(v.z))
					throw new IllegalArgumentException("non-finite " + kind + " keyframe on node " + nodeId);
			} else {
				final Quaternion q = (Quaternion)key.value;
				if (!Float.isFinite(q.x) || !Float.isFinite(q.y) || !Float.isFinite(q.z) || !Float.isFinite(q.w)
					|| q.len2() <= 0f)
					throw new IllegalArgumentException("invalid rotation keyframe on node " + nodeId);
			}
		}
	}

	private static void sampleVectorChannel (final Array<NodeKeyframe<Vector3>> channel, final float time, final double[] out,
		final String kind, final int clipIdx, final String nodeId) {
		final int last = channel.size - 1;
		if (last == 0) {
			copyVector(channel.get(0).value, out);
			return;
		}
		final float firstTime = channel.get(0).keytime;
		final float lastTime = channel.get(last).keytime;
		final float clamped = Math.max(firstTime, Math.min(lastTime, time));
		int idx = firstKeyframeIndex(channel, clamped);
		final NodeKeyframe<Vector3> a = channel.get(idx);
		if (idx == last) {
			copyVector(a.value, out);
			return;
		}
		final NodeKeyframe<Vector3> b = channel.get(idx + 1);
		final float span = b.keytime - a.keytime;
		final float alpha = span == 0f ? 0f : (clamped - a.keytime) / span;
		for (int c = 0; c < 3; c++) {
			final double av = component(a.value, c);
			final double bv = component(b.value, c);
			out[c] = av + (bv - av) * alpha;
			if (!Double.isFinite(out[c]))
				throw new IllegalArgumentException("non-finite sampled " + kind + " on node " + nodeId + " clip " + clipIdx);
		}
	}

	private static void sampleQuaternionChannel (final Array<NodeKeyframe<Quaternion>> channel, final float time,
		final double[] out, final int clipIdx, final String nodeId) {
		final int last = channel.size - 1;
		if (last == 0) {
			copyAndNormalizeQuaternion(channel.get(0).value, out, "rotation keyframe on node " + nodeId);
			return;
		}
		final float firstTime = channel.get(0).keytime;
		final float lastTime = channel.get(last).keytime;
		final float clamped = Math.max(firstTime, Math.min(lastTime, time));
		final int idx = firstKeyframeIndex(channel, clamped);
		final NodeKeyframe<Quaternion> a = channel.get(idx);
		if (idx == last) {
			copyAndNormalizeQuaternion(a.value, out, "rotation keyframe on node " + nodeId);
			return;
		}
		final NodeKeyframe<Quaternion> b = channel.get(idx + 1);
		final float span = b.keytime - a.keytime;
		final float alpha = span == 0f ? 0f : (clamped - a.keytime) / span;
		final double[] qa = new double[4];
		final double[] qb = new double[4];
		copyAndNormalizeQuaternion(a.value, qa, "rotation keyframe on node " + nodeId);
		copyAndNormalizeQuaternion(b.value, qb, "rotation keyframe on node " + nodeId);
		slerp(out, qa, qb, alpha);
		if (!Double.isFinite(out[0]) || !Double.isFinite(out[1]) || !Double.isFinite(out[2]) || !Double.isFinite(out[3]))
			throw new IllegalArgumentException("non-finite sampled rotation on node " + nodeId + " clip " + clipIdx);
	}

	/** Same binary search semantics as BaseAnimationController#getFirstKeyframeIndexAtTime, called with a time already clamped
	 * to the channel bounds. */
	private static <T> int firstKeyframeIndex (final Array<NodeKeyframe<T>> channel, final float time) {
		int lo = 0;
		int hi = channel.size - 1;
		while (lo < hi) {
			final int mid = (lo + hi) >>> 1;
			if (time > channel.get(mid + 1).keytime)
				lo = mid + 1;
			else if (time < channel.get(mid).keytime)
				hi = mid - 1;
			else
				return mid;
		}
		return lo;
	}

	/** Shortest arc slerp between unit quaternions, mirroring {@link Quaternion#slerp(Quaternion, float)}. */
	private static void slerp (final double[] out, final double[] a, final double[] bIn, final double alpha) {
		final double[] b = {bIn[0], bIn[1], bIn[2], bIn[3]};
		double dot = dot4(a, b);
		if (dot < 0.0) {
			dot = -dot;
			for (int c = 0; c < 4; c++)
				b[c] = -b[c];
		}
		double s0 = 1.0 - alpha;
		double s1 = alpha;
		if (1.0 - dot > 0.1) {
			final double theta = Math.acos(Math.min(1.0, Math.max(-1.0, dot)));
			final double sinTheta = Math.sin(theta);
			s0 = Math.sin((1.0 - alpha) * theta) / sinTheta;
			s1 = Math.sin(alpha * theta) / sinTheta;
		}
		for (int c = 0; c < 4; c++)
			out[c] = s0 * a[c] + s1 * b[c];
		normalize4(out);
	}

	// ------------------------------------------------------------------
	// Small double precision matrix/vector helpers
	// ------------------------------------------------------------------

	/** Compose a TRS matrix using the exact same formula as {@link com.badlogic.gdx.math.Matrix4#set(Vector3, Quaternion,
	 * Vector3)}. */
	static void trs (final double[] out, final double[] t, final double[] r, final double[] s) {
		final double x = r[0], y = r[1], z = r[2], w = r[3];
		final double xs = x * 2.0, ys = y * 2.0, zs = z * 2.0;
		final double wx = w * xs, wy = w * ys, wz = w * zs;
		final double xx = x * xs, xy = x * ys, xz = x * zs;
		final double yy = y * ys, yz = y * zs, zz = z * zs;
		out[mi(0, 0)] = s[0] * (1.0 - (yy + zz));
		out[mi(0, 1)] = s[0] * (xy + wz);
		out[mi(0, 2)] = s[0] * (xz - wy);
		out[mi(1, 0)] = s[1] * (xy - wz);
		out[mi(1, 1)] = s[1] * (1.0 - (xx + zz));
		out[mi(1, 2)] = s[1] * (yz + wx);
		out[mi(2, 0)] = s[2] * (xz + wy);
		out[mi(2, 1)] = s[2] * (yz - wx);
		out[mi(2, 2)] = s[2] * (1.0 - (xx + yy));
		out[mi(3, 0)] = t[0];
		out[mi(3, 1)] = t[1];
		out[mi(3, 2)] = t[2];
		out[mi(0, 3)] = out[mi(1, 3)] = out[mi(2, 3)] = 0.0;
		out[mi(3, 3)] = 1.0;
	}

	/** out = a * b for column-major 4x4 matrices. */
	static void mul4 (final double[] out, final double[] a, final double[] b) {
		for (int col = 0; col < 4; col++) {
			for (int row = 0; row < 4; row++) {
				double sum = 0.0;
				for (int k = 0; k < 4; k++)
					sum += a[mi(k, row)] * b[mi(col, k)];
				out[mi(col, row)] = sum;
			}
		}
	}

	/** Transform a point (w = 1) by a column-major 4x4 matrix. */
	static void mulVec (final double[] out, final double[] m, final double[] v) {
		for (int row = 0; row < 3; row++) {
			out[row] = m[mi(0, row)] * v[0] + m[mi(1, row)] * v[1] + m[mi(2, row)] * v[2] + m[mi(3, row)];
		}
	}

	/** Transform a direction (w = 0): only the linear 3x3 part, so no translation is added. Required for Jacobian vectors,
	 * where the instance translation would otherwise appear as a constant spurious derivative. */
	static void mulDirection (final double[] out, final double[] m, final double[] v) {
		for (int row = 0; row < 3; row++) {
			out[row] = m[mi(0, row)] * v[0] + m[mi(1, row)] * v[1] + m[mi(2, row)] * v[2];
		}
	}

	private static void add4 (final double[] a, final double[] b) {
		for (int i = 0; i < 16; i++)
			a[i] += b[i];
	}

	private static void copyMatrix (final float[] src, final double[] dst) {
		for (int i = 0; i < 16; i++)
			dst[i] = src[i];
	}

	/** Gauss-Jordan inversion with partial pivoting. Returns false for a singular/non-finite matrix. */
	static boolean invert4 (final double[] in, final double[] out) {
		final double[][] a = new double[4][8];
		for (int row = 0; row < 4; row++) {
			for (int col = 0; col < 4; col++) {
				final double v = in[mi(col, row)];
				if (!Double.isFinite(v)) return false;
				a[row][col] = v;
			}
			a[row][row + 4] = 1.0;
		}
		for (int col = 0; col < 4; col++) {
			int pivot = col;
			for (int row = col + 1; row < 4; row++)
				if (Math.abs(a[row][col]) > Math.abs(a[pivot][col])) pivot = row;
			final double pivotValue = a[pivot][col];
			if (!Double.isFinite(pivotValue) || Math.abs(pivotValue) < 1e-18) return false;
			final double[] swap = a[pivot];
			a[pivot] = a[col];
			a[col] = swap;
			for (int j = 0; j < 8; j++)
				a[col][j] /= pivotValue;
			for (int row = 0; row < 4; row++) {
				if (row == col) continue;
				final double factor = a[row][col];
				if (factor == 0.0) continue;
				for (int j = 0; j < 8; j++)
					a[row][j] -= factor * a[col][j];
			}
		}
		for (int row = 0; row < 4; row++)
			for (int col = 0; col < 4; col++)
				out[mi(col, row)] = a[row][col + 4];
		return true;
	}

	private static void collect (final Node node, final Array<Node> out) {
		out.add(node);
		for (Node child : node.getChildren())
			collect(child, out);
	}

	private static double component (final Vector3 v, final int c) {
		switch (c) {
		case 0:
			return v.x;
		case 1:
			return v.y;
		default:
			return v.z;
		}
	}

	private static void copyVector (final Vector3 src, final double[] dst) {
		if (!Float.isFinite(src.x) || !Float.isFinite(src.y) || !Float.isFinite(src.z))
			throw new IllegalArgumentException("non-finite vector value");
		dst[0] = src.x;
		dst[1] = src.y;
		dst[2] = src.z;
	}

	private static void copyPositiveScale (final Vector3 src, final double[] dst, final String what) {
		copyVector(src, dst);
		for (int c = 0; c < 3; c++)
			if (!(dst[c] > 0.0)) throw new IllegalArgumentException(what + " must be strictly positive");
	}

	private static void copyAndNormalizeQuaternion (final Quaternion src, final double[] dst, final String what) {
		if (!Float.isFinite(src.x) || !Float.isFinite(src.y) || !Float.isFinite(src.z) || !Float.isFinite(src.w)
			|| src.len2() <= 0f)
			throw new IllegalArgumentException(what + " must be a finite, non-zero quaternion");
		dst[0] = src.x;
		dst[1] = src.y;
		dst[2] = src.z;
		dst[3] = src.w;
		normalize4(dst);
	}

	private static double dot4 (final double[] a, final double[] b) {
		return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
	}

	private static void normalize4 (final double[] v) {
		final double len = Math.sqrt(dot4(v, v));
		if (!Double.isFinite(len) || len <= 0.0)
			throw new IllegalArgumentException("cannot normalize a zero quaternion blend");
		for (int i = 0; i < 4; i++)
			v[i] /= len;
	}
}
