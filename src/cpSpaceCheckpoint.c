/* Copyright (c) 2013 Scott Lembcke and Howling Moon Software
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

// In-process cpSpace checkpoints. See cpSpaceCheckpoint.h for the public
// contract. A checkpoint is a deep, value-based snapshot of every piece of
// mutable simulation state: body dynamics, the contact graph (including
// cached and sleeping arbiters and their warm-started contact impulses),
// constraint solver accumulators, the sleeping component graph and the
// spatial indexes. Structural identity is retained: objects keep their
// addresses, only their contents are rewound.

#include <string.h>

#include "chipmunk/chipmunk_private.h"

// Full layout of the contact buffer ring headers, kept private in
// cpSpaceStep.c. The definition must stay in sync with that file.
struct cpContactBufferHeader {
	cpTimestamp stamp;
	cpContactBufferHeader *next;
	unsigned int numContacts;
};

//MARK: Snapshot types

typedef struct cpCPBody {
	cpBody *body;
	cpBool sleeping;

	// Integration callbacks (shallow function pointers).
	cpBodyVelocityFunc velocity_func;
	cpBodyPositionFunc position_func;

	// Dynamic state.
	cpVect p, v, f;
	cpFloat a, w, t;
	cpTransform transform;
	cpVect v_bias;
	cpFloat w_bias;

	// Sleeping bookkeeping.
	cpBody *root, *next;
	cpFloat idleTime;

	// Structural fingerprint. The arbiter count is deliberately absent from
	// the fixed-parameter validation; the ordered list below is simulation
	// state used to rebuild the contact graph exactly.
	cpFloat m, m_inv, i, i_inv;
	cpVect cog;
	cpBodyType type;
	int shapeCount;
	int constraintCount;
	int arbiterCount;
	cpConstraint **constraintOrder;
	cpArbiter **arbiterOrder;

	// Shallow user pointer, restored but never validated.
	cpDataPointer userData;
} cpCPBody;

typedef struct cpCPGeometry {
	int type; // cpShapeType
	cpFloat r;
	int count;
	// Circle: x holds c. Segment: x/y hold a,b, z/w the tangent endpoints.
	cpVect x, y, z, w;
	struct cpSplittingPlane *planes; // poly local planes, count entries
} cpCPGeometry;

typedef struct cpCPShape {
	cpShape *shape;
	cpBody *body;

	// Structural fingerprint.
	const cpShapeClass *klass;
	struct cpShapeMassInfo massInfo;
	cpBB bb;
	cpBool sensor;
	cpFloat e, u;
	cpVect surfaceV;
	cpCollisionType type;
	cpShapeFilter filter;
	cpHashValue hashid;

	cpCPGeometry geom;

	// Shallow user pointer, restored but never validated.
	cpDataPointer userData;
} cpCPShape;

enum {
	CP_CP_PIN_JOINT,
	CP_CP_SLIDE_JOINT,
	CP_CP_PIVOT_JOINT,
	CP_CP_GROOVE_JOINT,
	CP_CP_DAMPED_SPRING,
	CP_CP_DAMPED_ROTARY_SPRING,
	CP_CP_ROTARY_LIMIT_JOINT,
	CP_CP_RATCHET_JOINT,
	CP_CP_GEAR_JOINT,
	CP_CP_SIMPLE_MOTOR,
	CP_CP_NUM_JOINTS
};

typedef struct cpCPConstraint {
	cpConstraint *constraint;
	int kind;

	// Identity fingerprint.
	cpBody *a, *b;

	// Base parameters, restored verbatim. preSolve/postSolve are also
	// validated because they are handler fields.
	cpFloat maxForce, errorBias, maxBias;
	cpBool collideBodies;
	cpConstraintPreSolveFunc preSolve;
	cpConstraintPostSolveFunc postSolve;

	// Full type-specific struct tail (configuration + solver accumulators).
	size_t tailSize;
	void *tail;

	// True when the constraint is in space->constraints and is iterated by
	// the solver. Sleeping constraints live only on the body constraint lists.
	cpBool awake;

	// Shallow user pointer, restored but never validated.
	cpDataPointer userData;
} cpCPConstraint;

typedef struct cpCPContact {
	cpVect r1, r2;
	cpFloat nMass, tMass, bounce;
	cpFloat jnAcc, jtAcc, jBias;
	cpFloat bias;
	cpHashValue hash;
} cpCPContact;

typedef struct cpCPArbiter {
	cpArbiter *arbiter;

	cpFloat e, u;
	cpVect surface_vr;
	cpDataPointer data;

	const cpShape *a, *b;
	cpBody *body_a, *body_b;

	cpCollisionHandler *handler, *handlerA, *handlerB;

	int count;
	cpVect n;
	cpBool swapped;
	cpTimestamp stamp;
	enum cpArbiterState state;

	// True when the arbiter was part of space->arbiters (an active contact of
	// awake bodies) and threaded onto its bodies' contact graphs.
	cpBool active;
	// True when both bodies were sleeping and the contacts lived in a private
	// allocation instead of the space's contact buffers.
	cpBool sleeping;

	// Rebuilt arbiter address, filled in during restore.
	cpArbiter *rebuilt;

	cpCPContact contacts[CP_MAX_CONTACTS_PER_ARBITER];
} cpCPArbiter;

typedef struct cpCPHandler {
	cpCollisionHandler handler;
} cpCPHandler;

struct cpSpaceCheckpoint {
	cpSpace *space;
	cpTimestamp generation;

	// Space parameters.
	int iterations;
	cpVect gravity;
	cpFloat damping;
	cpFloat idleSpeedThreshold;
	cpFloat sleepTimeThreshold;
	cpFloat collisionSlop;
	cpFloat collisionBias;
	cpTimestamp collisionPersistence;
	cpTimestamp stamp;
	cpFloat curr_dt;
	cpHashValue shapeIDCounter;
	cpBool usesWildcards;
	cpCollisionHandler defaultHandler;

	int numBodies, numShapes, numConstraints, numArbiters, numHandlers;
	int numComponents;
	int numActiveArbiters;
	cpCPBody *bodies;
	cpCPShape *shapes;
	cpCPConstraint *constraints;
	cpCPArbiter *arbiters;
	// space->arbiters ordering at capture time, as arbiter snapshot pointers.
	cpArbiter **activeArbiters;
	cpCPHandler *handlers;
	cpBody **components;

	// Pristine deep copies of the broadphase trees at capture time. Restore
	// re-clones these so repeated restores always see the same topology.
	cpSpatialIndex *staticShapes;
	cpSpatialIndex *dynamicShapes;
};

//MARK: Small helpers

void
cpSpaceCheckpointBump(cpSpace *space)
{
	if(space) space->cpCheckpointGeneration++;
}

void
cpSpaceCheckpointSetUnsupported(cpSpace *space)
{
	if(space) space->cpCheckpointFlags &= ~CP_CHECKPOINT_SUPPORTED;
}

static cpCPBody *
FindBody(cpSpaceCheckpoint *cp, cpBody *body)
{
	for(int i=0; i<cp->numBodies; i++){
		if(cp->bodies[i].body == body) return &cp->bodies[i];
	}
	return NULL;
}

// Classify a built-in constraint. Returns cpFalse for user extended classes.
static cpBool
ConstraintKind(const cpConstraint *constraint, int *kind)
{
	if(cpConstraintIsPinJoint(constraint)){ *kind = CP_CP_PIN_JOINT; return cpTrue; }
	if(cpConstraintIsSlideJoint(constraint)){ *kind = CP_CP_SLIDE_JOINT; return cpTrue; }
	if(cpConstraintIsPivotJoint(constraint)){ *kind = CP_CP_PIVOT_JOINT; return cpTrue; }
	if(cpConstraintIsGrooveJoint(constraint)){ *kind = CP_CP_GROOVE_JOINT; return cpTrue; }
	if(cpConstraintIsDampedSpring(constraint)){ *kind = CP_CP_DAMPED_SPRING; return cpTrue; }
	if(cpConstraintIsDampedRotarySpring(constraint)){ *kind = CP_CP_DAMPED_ROTARY_SPRING; return cpTrue; }
	if(cpConstraintIsRotaryLimitJoint(constraint)){ *kind = CP_CP_ROTARY_LIMIT_JOINT; return cpTrue; }
	if(cpConstraintIsRatchetJoint(constraint)){ *kind = CP_CP_RATCHET_JOINT; return cpTrue; }
	if(cpConstraintIsGearJoint(constraint)){ *kind = CP_CP_GEAR_JOINT; return cpTrue; }
	if(cpConstraintIsSimpleMotor(constraint)){ *kind = CP_CP_SIMPLE_MOTOR; return cpTrue; }
	return cpFalse;
}

static size_t
JointStructSize(int kind)
{	switch(kind){
		case CP_CP_PIN_JOINT: return sizeof(cpPinJoint);
		case CP_CP_SLIDE_JOINT: return sizeof(cpSlideJoint);
		case CP_CP_PIVOT_JOINT: return sizeof(cpPivotJoint);
		case CP_CP_GROOVE_JOINT: return sizeof(cpGrooveJoint);
		case CP_CP_DAMPED_SPRING: return sizeof(cpDampedSpring);
		case CP_CP_DAMPED_ROTARY_SPRING: return sizeof(cpDampedRotarySpring);
		case CP_CP_ROTARY_LIMIT_JOINT: return sizeof(cpRotaryLimitJoint);
		case CP_CP_RATCHET_JOINT: return sizeof(cpRatchetJoint);
		case CP_CP_GEAR_JOINT: return sizeof(cpGearJoint);
		case CP_CP_SIMPLE_MOTOR: return sizeof(cpSimpleMotor);
		default: return 0;
	}
}

// Tail-only mirrors of the built-in joint structs: every field following the
// cpConstraint base, in the same order. The checkpoint stores exactly these
// bytes, so validation must read them through the tail layouts rather than
// casting back to the full struct (whose base would precede the buffer).
typedef struct { cpVect anchorA, anchorB; cpFloat dist; cpVect r1, r2, n; cpFloat nMass, jnAcc, bias; } CPTailPin;
typedef struct { cpVect anchorA, anchorB; cpFloat min, max; cpVect r1, r2, n; cpFloat nMass, jnAcc, bias; } CPTailSlide;
typedef struct { cpVect anchorA, anchorB; cpVect r1, r2; cpMat2x2 k; cpVect jAcc, bias; } CPTailPivot;
typedef struct { cpVect grv_n, grv_a, grv_b; cpVect anchorB; cpVect grv_tn; cpFloat clamp; cpVect r1, r2; cpMat2x2 k; cpVect jAcc, bias; } CPTailGroove;
typedef struct { cpVect anchorA, anchorB; cpFloat restLength, stiffness, damping; cpDampedSpringForceFunc springForceFunc; cpFloat target_vrn, v_coef; cpVect r1, r2; cpFloat nMass; cpVect n; cpFloat jAcc; } CPTailDampedSpring;
typedef struct { cpFloat restAngle, stiffness, damping; cpDampedRotarySpringTorqueFunc springTorqueFunc; cpFloat target_wrn, w_coef, iSum, jAcc; } CPTailDampedRotarySpring;
typedef struct { cpFloat min, max, iSum, bias, jAcc; } CPTailRotaryLimit;
typedef struct { cpFloat angle, phase, ratchet, iSum, bias, jAcc; } CPTailRatchet;
typedef struct { cpFloat phase, ratio, ratio_inv, iSum, bias, jAcc; } CPTailGear;
typedef struct { cpFloat rate, iSum, jAcc; } CPTailSimpleMotor;

// Fail the build if a tail mirror ever drifts from its engine struct.
typedef char CP_TAIL_CHECK_1[sizeof(CPTailPin) == sizeof(cpPinJoint) - sizeof(cpConstraint) ? 1 : -1];
typedef char CP_TAIL_CHECK_2[sizeof(CPTailSlide) == sizeof(cpSlideJoint) - sizeof(cpConstraint) ? 1 : -1];
typedef char CP_TAIL_CHECK_3[sizeof(CPTailPivot) == sizeof(cpPivotJoint) - sizeof(cpConstraint) ? 1 : -1];
typedef char CP_TAIL_CHECK_4[sizeof(CPTailGroove) == sizeof(cpGrooveJoint) - sizeof(cpConstraint) ? 1 : -1];
typedef char CP_TAIL_CHECK_5[sizeof(CPTailDampedSpring) == sizeof(cpDampedSpring) - sizeof(cpConstraint) ? 1 : -1];
typedef char CP_TAIL_CHECK_6[sizeof(CPTailDampedRotarySpring) == sizeof(cpDampedRotarySpring) - sizeof(cpConstraint) ? 1 : -1];
typedef char CP_TAIL_CHECK_7[sizeof(CPTailRotaryLimit) == sizeof(cpRotaryLimitJoint) - sizeof(cpConstraint) ? 1 : -1];
typedef char CP_TAIL_CHECK_8[sizeof(CPTailRatchet) == sizeof(cpRatchetJoint) - sizeof(cpConstraint) ? 1 : -1];
typedef char CP_TAIL_CHECK_9[sizeof(CPTailGear) == sizeof(cpGearJoint) - sizeof(cpConstraint) ? 1 : -1];
typedef char CP_TAIL_CHECK_10[sizeof(CPTailSimpleMotor) == sizeof(cpSimpleMotor) - sizeof(cpConstraint) ? 1 : -1];

// Validate the fixed configuration fields of a constraint against a snapshot.
// Solver accumulators in the same struct tail are restored rather than checked.
static cpBool
ConstraintConfigMatches(const cpCPConstraint *sc, const cpConstraint *c)
{
	if(
		c->a != sc->a || c->b != sc->b ||
		c->maxForce != sc->maxForce || c->errorBias != sc->errorBias ||
		c->maxBias != sc->maxBias || c->collideBodies != sc->collideBodies ||
		c->preSolve != sc->preSolve || c->postSolve != sc->postSolve
	) return cpFalse;

	switch(sc->kind){
		case CP_CP_PIN_JOINT: {
			const cpPinJoint *j = (const cpPinJoint *)c;
			const CPTailPin *s = (const CPTailPin *)sc->tail;
			return cpveql(j->anchorA, s->anchorA) && cpveql(j->anchorB, s->anchorB) && j->dist == s->dist;
		}
		case CP_CP_SLIDE_JOINT: {
			const cpSlideJoint *j = (const cpSlideJoint *)c;
			const CPTailSlide *s = (const CPTailSlide *)sc->tail;
			return cpveql(j->anchorA, s->anchorA) && cpveql(j->anchorB, s->anchorB) && j->min == s->min && j->max == s->max;
		}
		case CP_CP_PIVOT_JOINT: {
			const cpPivotJoint *j = (const cpPivotJoint *)c;
			const CPTailPivot *s = (const CPTailPivot *)sc->tail;
			return cpveql(j->anchorA, s->anchorA) && cpveql(j->anchorB, s->anchorB);
		}
		case CP_CP_GROOVE_JOINT: {
			const cpGrooveJoint *j = (const cpGrooveJoint *)c;
			const CPTailGroove *s = (const CPTailGroove *)sc->tail;
			return cpveql(j->grv_a, s->grv_a) && cpveql(j->grv_b, s->grv_b) && cpveql(j->anchorB, s->anchorB);
		}
		case CP_CP_DAMPED_SPRING: {
			const cpDampedSpring *j = (const cpDampedSpring *)c;
			const CPTailDampedSpring *s = (const CPTailDampedSpring *)sc->tail;
			return (
				cpveql(j->anchorA, s->anchorA) && cpveql(j->anchorB, s->anchorB) &&
				j->restLength == s->restLength && j->stiffness == s->stiffness &&
				j->damping == s->damping && j->springForceFunc == s->springForceFunc
			);
		}
		case CP_CP_DAMPED_ROTARY_SPRING: {
			const cpDampedRotarySpring *j = (const cpDampedRotarySpring *)c;
			const CPTailDampedRotarySpring *s = (const CPTailDampedRotarySpring *)sc->tail;
			return (
				j->restAngle == s->restAngle && j->stiffness == s->stiffness &&
				j->damping == s->damping && j->springTorqueFunc == s->springTorqueFunc
			);
		}
		case CP_CP_ROTARY_LIMIT_JOINT: {
			const cpRotaryLimitJoint *j = (const cpRotaryLimitJoint *)c;
			const CPTailRotaryLimit *s = (const CPTailRotaryLimit *)sc->tail;
			return j->min == s->min && j->max == s->max;
		}
		case CP_CP_RATCHET_JOINT: {
			const cpRatchetJoint *j = (const cpRatchetJoint *)c;
			const CPTailRatchet *s = (const CPTailRatchet *)sc->tail;
			return j->phase == s->phase && j->ratchet == s->ratchet;
		}
		case CP_CP_GEAR_JOINT: {
			const cpGearJoint *j = (const cpGearJoint *)c;
			const CPTailGear *s = (const CPTailGear *)sc->tail;
			return j->phase == s->phase && j->ratio == s->ratio;
		}
		case CP_CP_SIMPLE_MOTOR: {
			const cpSimpleMotor *j = (const cpSimpleMotor *)c;
			const CPTailSimpleMotor *s = (const CPTailSimpleMotor *)sc->tail;
			return j->rate == s->rate;
		}
	}
	return cpFalse;
}

//MARK: Geometry snapshots

static void
SnapshotGeometry(cpCPShape *ss, const cpShape *shape)
{
	cpCPGeometry *g = &ss->geom;
	memset(g, 0, sizeof(*g));
	g->type = (int)shape->klass->type;

	if(shape->klass->type == CP_CIRCLE_SHAPE){
		const cpCircleShape *circle = (const cpCircleShape *)shape;
		g->r = circle->r;
		g->x = circle->c;
	} else if(shape->klass->type == CP_SEGMENT_SHAPE){
		const cpSegmentShape *seg = (const cpSegmentShape *)shape;
		g->r = seg->r;
		g->x = seg->a;
		g->y = seg->b;
		g->z = seg->a_tangent;
		g->w = seg->b_tangent;
	} else if(shape->klass->type == CP_POLY_SHAPE){
		const cpPolyShape *poly = (const cpPolyShape *)shape;
		g->r = poly->r;
		g->count = poly->count;
		// The persistent local planes live in the second half of the array.
		struct cpSplittingPlane *planes = (struct cpSplittingPlane *)cpcalloc((size_t)poly->count, sizeof(struct cpSplittingPlane));
		if(planes) memcpy(planes, poly->planes + poly->count, (size_t)poly->count*sizeof(struct cpSplittingPlane));
		g->planes = planes;
	}
}

static cpBool
GeometryMatches(const cpCPGeometry *g, const cpShape *shape)
{
	if(g->type != (int)shape->klass->type) return cpFalse;

	if(shape->klass->type == CP_CIRCLE_SHAPE){
		const cpCircleShape *circle = (const cpCircleShape *)shape;
		return (circle->r == g->r && cpveql(circle->c, g->x));
	} else if(shape->klass->type == CP_SEGMENT_SHAPE){
		const cpSegmentShape *seg = (const cpSegmentShape *)shape;
		return (
			seg->r == g->r &&
			cpveql(seg->a, g->x) && cpveql(seg->b, g->y) &&
			cpveql(seg->a_tangent, g->z) && cpveql(seg->b_tangent, g->w)
		);
	} else if(shape->klass->type == CP_POLY_SHAPE){
		const cpPolyShape *poly = (const cpPolyShape *)shape;
		if(poly->count != g->count || poly->r != g->r || !g->planes) return cpFalse;
		const struct cpSplittingPlane *planes = poly->planes + poly->count;
		for(int i=0; i<poly->count; i++){
			if(!cpveql(planes[i].v0, g->planes[i].v0) || !cpveql(planes[i].n, g->planes[i].n)) return cpFalse;
		}
		return cpTrue;
	}
	return cpFalse;
}

static void
FreeGeometry(cpCPGeometry *g)
{
	if(g->planes) cpfree(g->planes);
	g->planes = NULL;
}

//MARK: Body enumeration

// Append every body the space knows about, in a stable order:
// awake dynamic bodies, static bodies (plus the designated static body),
// then the sleeping component chains.
static int
CollectBodies(cpSpace *space, cpBody **out, int capacity)
{
	int n = 0;

	for(int i=0; i<space->dynamicBodies->num; i++){
		if(n < capacity) out[n] = (cpBody *)space->dynamicBodies->arr[i];
		n++;
	}
	for(int i=0; i<space->staticBodies->num; i++){
		if(n < capacity) out[n] = (cpBody *)space->staticBodies->arr[i];
		n++;
	}
	// The designated static body is not kept in staticBodies.
	if(!cpArrayContains(space->staticBodies, space->staticBody)){
		if(n < capacity) out[n] = space->staticBody;
		n++;
	}
	for(int i=0; i<space->sleepingComponents->num; i++){
		CP_BODY_FOREACH_COMPONENT((cpBody *)space->sleepingComponents->arr[i], body){
			if(n < capacity) out[n] = body;
			n++;
		}
	}
	return n;
}

//MARK: Arbiter enumeration

typedef struct ArbiterEnumContext {
	cpCPArbiter *out;
	int count;
	int capacity;
} ArbiterEnumContext;

static void
CopyContact(cpCPContact *dst, const struct cpContact *src)
{
	dst->r1 = src->r1; dst->r2 = src->r2;
	dst->nMass = src->nMass; dst->tMass = src->tMass;
	dst->bounce = src->bounce;
	dst->jnAcc = src->jnAcc; dst->jtAcc = src->jtAcc; dst->jBias = src->jBias;
	dst->bias = src->bias;
	dst->hash = src->hash;
}

static cpCPArbiter *
PushArbiter(ArbiterEnumContext *ctx)
{
	if(ctx->count >= ctx->capacity) return NULL;
	cpCPArbiter *sa = &ctx->out[ctx->count++];
	memset(sa, 0, sizeof(*sa));
	return sa;
}

static void
SnapshotArbiter(cpCPArbiter *sa, cpArbiter *arb, cpBool active, cpBool sleeping)
{
	sa->arbiter = arb;
	sa->e = arb->e; sa->u = arb->u; sa->surface_vr = arb->surface_vr;
	sa->data = arb->data;
	sa->a = arb->a; sa->b = arb->b;
	sa->body_a = arb->body_a; sa->body_b = arb->body_b;
	sa->handler = arb->handler; sa->handlerA = arb->handlerA; sa->handlerB = arb->handlerB;
	sa->count = arb->count;
	sa->n = arb->n;
	sa->swapped = arb->swapped;
	sa->stamp = arb->stamp;
	sa->state = arb->state;
	sa->active = active;
	sa->sleeping = sleeping;

	for(int i=0; i<arb->count && i<CP_MAX_CONTACTS_PER_ARBITER; i++){
		CopyContact(&sa->contacts[i], &arb->contacts[i]);
	}
}

static void
arbiterHashEnum(void *elt, void *data)
{
	ArbiterEnumContext *ctx = (ArbiterEnumContext *)data;
	cpCPArbiter *sa = PushArbiter(ctx);
	if(sa) SnapshotArbiter(sa, (cpArbiter *)elt, cpFalse, cpFalse);
}

typedef struct HandlerEnumContext {
	cpCPHandler *out;
	int count;
} HandlerEnumContext;

static void
handlerHashEnum(void *elt, void *data)
{
	HandlerEnumContext *ctx = (HandlerEnumContext *)data;
	// cpCollisionHandler has const members, so it cannot be assigned directly.
	memcpy(&ctx->out[ctx->count].handler, elt, sizeof(cpCollisionHandler));
	ctx->count++;
}

//MARK: cpSpaceCheckpointNew

static cpBool
SnapshotBody(cpCPBody *sb, cpBody *body, cpBool sleeping)
{
	sb->body = body;
	sb->sleeping = sleeping;
	sb->velocity_func = body->velocity_func;
	sb->position_func = body->position_func;
	sb->p = body->p; sb->v = body->v; sb->f = body->f;
	sb->a = body->a; sb->w = body->w; sb->t = body->t;
	sb->transform = body->transform;
	sb->v_bias = body->v_bias; sb->w_bias = body->w_bias;
	sb->root = body->sleeping.root; sb->next = body->sleeping.next;
	sb->idleTime = body->sleeping.idleTime;
	sb->m = body->m; sb->m_inv = body->m_inv; sb->i = body->i; sb->i_inv = body->i_inv;
	sb->cog = body->cog;
	sb->type = cpBodyGetType(body);
	sb->userData = body->userData;
	sb->shapeCount = sb->constraintCount = sb->arbiterCount = 0;
	CP_BODY_FOREACH_SHAPE(body, shape) sb->shapeCount++;
	CP_BODY_FOREACH_CONSTRAINT(body, constraint) sb->constraintCount++;
	CP_BODY_FOREACH_ARBITER(body, arb) sb->arbiterCount++;

	// Capture the exact head-first order of the per-body solver lists so the
	// rebuilt contact and constraint graphs iterate identically.
	sb->constraintOrder = (cpConstraint **)cpcalloc((size_t)(sb->constraintCount ? sb->constraintCount : 1), sizeof(cpConstraint *));
	sb->arbiterOrder = (cpArbiter **)cpcalloc((size_t)(sb->arbiterCount ? sb->arbiterCount : 1), sizeof(cpArbiter *));
	if(sb->constraintOrder && sb->arbiterOrder){
		int k = 0;
		CP_BODY_FOREACH_CONSTRAINT(body, constraint) sb->constraintOrder[k++] = constraint;
		k = 0;
		CP_BODY_FOREACH_ARBITER(body, arb) sb->arbiterOrder[k++] = arb;
	}
	return (sb->constraintOrder != NULL && sb->arbiterOrder != NULL);
}

cpSpaceCheckpoint *
cpSpaceCheckpointNew(cpSpace *space)
{
	if(!space) return NULL;
	if(space->locked) return NULL;
	if(space->postStepCallbacks->num > 0) return NULL;
	if(space->rousedBodies->num > 0) return NULL;
	if(!(space->cpCheckpointFlags & CP_CHECKPOINT_SUPPORTED)) return NULL;

	int numComponents = space->sleepingComponents->num;

	int numBodies = CollectBodies(space, NULL, 0);

	// Every constraint, awake or sleeping, must use a built-in class.
	for(int i=0; i<space->constraints->num; i++){
		int kind;
		if(!ConstraintKind((cpConstraint *)space->constraints->arr[i], &kind)) return NULL;
	}
	for(int i=0; i<numComponents; i++){
		CP_BODY_FOREACH_COMPONENT((cpBody *)space->sleepingComponents->arr[i], body){
			CP_BODY_FOREACH_CONSTRAINT(body, constraint){
				int kind;
				if(constraint->a == body && !ConstraintKind(constraint, &kind)) return NULL;
			}
		}
	}

	// Constraints: awake ones first, in exact solver-array order, then the
	// constraints of sleeping components (which are kept off the space array).
	int numAwakeConstraints = space->constraints->num;
	int numConstraints = numAwakeConstraints;
	{
		// Count the distinct sleeping constraints via their body A node.
		for(int i=0; i<numComponents; i++){
			CP_BODY_FOREACH_COMPONENT((cpBody *)space->sleepingComponents->arr[i], body){
				CP_BODY_FOREACH_CONSTRAINT(body, constraint){
					if(constraint->a == body && constraint->space == space) numConstraints++;
				}
			}
		}
	}

	int numShapes = 0;

	// Enumerate every body with a temporary list.
	cpBody **bodyList = (cpBody **)cpcalloc((size_t)(numBodies ? numBodies : 1), sizeof(cpBody *));
	if(!bodyList) return NULL;
	CollectBodies(space, bodyList, numBodies);

	for(int i=0; i<numBodies; i++){
		cpBody *body = bodyList[i];
		CP_BODY_FOREACH_SHAPE(body, shape) numShapes++;
	}

	int numHashArbiters = cpHashSetCount(space->cachedArbiters);
	int numSleepingArbiters = 0;
	for(int i=0; i<numComponents; i++){
		CP_BODY_FOREACH_COMPONENT((cpBody *)space->sleepingComponents->arr[i], body){
			CP_BODY_FOREACH_ARBITER(body, arb){
				if(body == arb->body_a || cpBodyGetType(arb->body_a) == CP_BODY_TYPE_STATIC) numSleepingArbiters++;
			}
		}
	}
	int numArbiters = numHashArbiters + numSleepingArbiters;
	int numHandlers = cpHashSetCount(space->collisionHandlers);

	cpSpaceCheckpoint *cp = (cpSpaceCheckpoint *)cpcalloc(1, sizeof(cpSpaceCheckpoint));
	if(!cp){ cpfree(bodyList); return NULL; }

	cp->space = space;
	cp->generation = space->cpCheckpointGeneration;

	cp->iterations = space->iterations;
	cp->gravity = space->gravity;
	cp->damping = space->damping;
	cp->idleSpeedThreshold = space->idleSpeedThreshold;
	cp->sleepTimeThreshold = space->sleepTimeThreshold;
	cp->collisionSlop = space->collisionSlop;
	cp->collisionBias = space->collisionBias;
	cp->collisionPersistence = space->collisionPersistence;
	cp->stamp = space->stamp;
	cp->curr_dt = space->curr_dt;
	cp->shapeIDCounter = space->shapeIDCounter;
	cp->usesWildcards = space->usesWildcards;
	memcpy(&cp->defaultHandler, &space->defaultHandler, sizeof(cpCollisionHandler));

	cp->numBodies = numBodies;
	cp->numShapes = numShapes;
	cp->numConstraints = numConstraints;
	cp->numArbiters = numArbiters;
	cp->numHandlers = numHandlers;
	cp->numComponents = numComponents;

	cp->bodies = (cpCPBody *)cpcalloc((size_t)(numBodies ? numBodies : 1), sizeof(cpCPBody));
	cp->shapes = (cpCPShape *)cpcalloc((size_t)(numShapes ? numShapes : 1), sizeof(cpCPShape));
	cp->constraints = (cpCPConstraint *)cpcalloc((size_t)(numConstraints ? numConstraints : 1), sizeof(cpCPConstraint));
	cp->arbiters = (cpCPArbiter *)cpcalloc((size_t)(numArbiters ? numArbiters : 1), sizeof(cpCPArbiter));
	cp->activeArbiters = (cpArbiter **)cpcalloc((size_t)(space->arbiters->num ? space->arbiters->num : 1), sizeof(cpArbiter *));
	cp->handlers = (cpCPHandler *)cpcalloc((size_t)(numHandlers ? numHandlers : 1), sizeof(cpCPHandler));
	cp->components = (cpBody **)cpcalloc((size_t)(numComponents ? numComponents : 1), sizeof(cpBody *));

	if(!cp->bodies || !cp->shapes || !cp->constraints || !cp->arbiters || !cp->activeArbiters || !cp->handlers || !cp->components){
		cpfree(bodyList);
		cpSpaceCheckpointFree(cp);
		return NULL;
	}

	// Bodies.
	for(int i=0; i<numBodies; i++){
		cpBody *body = bodyList[i];
		cpBool sleeping = (body->sleeping.root != NULL);
		if(!SnapshotBody(&cp->bodies[i], body, sleeping)){
			cpfree(bodyList);
			cpSpaceCheckpointFree(cp);
			return NULL;
		}
	}
	cpfree(bodyList);

	// Active arbiter order (space->arbiters), stored as original pointers.
	cp->numActiveArbiters = space->arbiters->num;
	for(int i=0; i<space->arbiters->num; i++){
		cp->activeArbiters[i] = (cpArbiter *)space->arbiters->arr[i];
	}

	// Sleeping component roots, in the space's array order.
	for(int i=0; i<numComponents; i++){
		cp->components[i] = (cpBody *)space->sleepingComponents->arr[i];
	}

	// Shapes.
	int si = 0;
	for(int i=0; i<numBodies; i++){
		cpBody *body = cp->bodies[i].body;
		CP_BODY_FOREACH_SHAPE(body, shape){
			cpCPShape *ss = &cp->shapes[si++];
			ss->shape = shape;
			ss->body = body;
			ss->klass = shape->klass;
			ss->massInfo = shape->massInfo;
			ss->bb = shape->bb;
			ss->sensor = shape->sensor;
			ss->e = shape->e; ss->u = shape->u;
			ss->surfaceV = shape->surfaceV;
			ss->type = shape->type;
			ss->filter = shape->filter;
			ss->hashid = shape->hashid;
			ss->userData = shape->userData;
			SnapshotGeometry(ss, shape);
			if(shape->klass->type == CP_POLY_SHAPE && !ss->geom.planes){
				cpSpaceCheckpointFree(cp);
				return NULL;
			}
		}
	}

	// Constraints: awake ones first in exact solver-array order, then the
	// constraints of sleeping components. Each sleeping constraint is owned by
	// body A, or by its sleeping body B when body A is the static body.
	int ci = 0;
	for(int i=0; i<numAwakeConstraints; i++){
		cpConstraint *constraint = (cpConstraint *)space->constraints->arr[i];
		cpCPConstraint *sc = &cp->constraints[ci++];
		sc->constraint = constraint;
		sc->awake = cpTrue;
		if(!ConstraintKind(constraint, &sc->kind)){
			cpSpaceCheckpointFree(cp);
			return NULL;
		}
		sc->a = constraint->a; sc->b = constraint->b;
		sc->maxForce = constraint->maxForce;
		sc->errorBias = constraint->errorBias;
		sc->maxBias = constraint->maxBias;
		sc->collideBodies = constraint->collideBodies;
		sc->preSolve = constraint->preSolve;
		sc->postSolve = constraint->postSolve;
		sc->userData = constraint->userData;
		sc->tailSize = JointStructSize(sc->kind) - sizeof(cpConstraint);
		sc->tail = cpcalloc(1, sc->tailSize ? sc->tailSize : 1);
		if(!sc->tail){
			cpSpaceCheckpointFree(cp);
			return NULL;
		}
		memcpy(sc->tail, (char *)constraint + sizeof(cpConstraint), sc->tailSize);
	}
	for(int i=0; i<numComponents; i++){
		CP_BODY_FOREACH_COMPONENT(cp->components[i], body){
			CP_BODY_FOREACH_CONSTRAINT(body, constraint){
				if(!(constraint->a == body || cpBodyGetType(constraint->a) == CP_BODY_TYPE_STATIC)) continue;
				cpCPConstraint *sc = &cp->constraints[ci++];
				sc->constraint = constraint;
				sc->awake = cpFalse;
				if(!ConstraintKind(constraint, &sc->kind)){
					cpSpaceCheckpointFree(cp);
					return NULL;
				}
				sc->a = constraint->a; sc->b = constraint->b;
				sc->maxForce = constraint->maxForce;
				sc->errorBias = constraint->errorBias;
				sc->maxBias = constraint->maxBias;
				sc->collideBodies = constraint->collideBodies;
				sc->preSolve = constraint->preSolve;
				sc->postSolve = constraint->postSolve;
				sc->userData = constraint->userData;
				sc->tailSize = JointStructSize(sc->kind) - sizeof(cpConstraint);
				sc->tail = cpcalloc(1, sc->tailSize ? sc->tailSize : 1);
				if(!sc->tail){
					cpSpaceCheckpointFree(cp);
					return NULL;
				}
				memcpy(sc->tail, (char *)constraint + sizeof(cpConstraint), sc->tailSize);
			}
		}
	}
	if(ci != numConstraints){
		cpSpaceCheckpointFree(cp);
		return NULL;
	}

	// Arbiters: every cached/active arbiter in the hash set, then the private
	// contact graph of sleeping components.
	ArbiterEnumContext ctx = {cp->arbiters, 0, numArbiters ? numArbiters : 1};
	cpHashSetEach(space->cachedArbiters, arbiterHashEnum, &ctx);

	// Flag the arbiters that were actively solving this step.
	for(int i=0; i<space->arbiters->num; i++){
		cpArbiter *arb = (cpArbiter *)space->arbiters->arr[i];
		for(int j=0; j<ctx.count; j++){
			if(cp->arbiters[j].arbiter == arb){
				cp->arbiters[j].active = cpTrue;
				break;
			}
		}
	}

	for(int i=0; i<numComponents; i++){
		CP_BODY_FOREACH_COMPONENT(cp->components[i], body){
			CP_BODY_FOREACH_ARBITER(body, arb){
				if(!(body == arb->body_a || cpBodyGetType(arb->body_a) == CP_BODY_TYPE_STATIC)) continue;
				cpCPArbiter *sa = PushArbiter(&ctx);
				if(!sa){
					cpSpaceCheckpointFree(cp);
					return NULL;
				}
				// Sleeping arbiters are threaded on the body graphs only; they
				// are never in space->arbiters or the arbiter hash.
				SnapshotArbiter(sa, arb, cpFalse, cpTrue);
			}
		}
	}
	cp->numArbiters = ctx.count;

	// Collision handlers.
	HandlerEnumContext hctx = {cp->handlers, 0};
	cpHashSetEach(space->collisionHandlers, handlerHashEnum, &hctx);
	cp->numHandlers = hctx.count;

	// Deep-copy the broadphase trees while the space is still at the captured
	// instant. Subsequent branching mutates the live trees, never these copies.
	cp->staticShapes = cpBBTreeClone(space->staticShapes, NULL, NULL);
	cp->dynamicShapes = cpBBTreeClone(space->dynamicShapes, cp->staticShapes, space->staticShapes);
	if(!cp->staticShapes || !cp->dynamicShapes){
		cpSpaceCheckpointFree(cp);
		return NULL;
	}

	return cp;
}

//MARK: Free

void
cpSpaceCheckpointFree(cpSpaceCheckpoint *cp)
{
	if(!cp) return;

	for(int i=0; i<cp->numShapes; i++) FreeGeometry(&cp->shapes[i].geom);
	for(int i=0; i<cp->numConstraints; i++) cpfree(cp->constraints[i].tail);
	for(int i=0; i<cp->numBodies; i++){
		cpfree(cp->bodies[i].constraintOrder);
		cpfree(cp->bodies[i].arbiterOrder);
	}

	cpfree(cp->bodies);
	cpfree(cp->shapes);
	cpfree(cp->constraints);
	cpfree(cp->arbiters);
	cpfree(cp->activeArbiters);
	cpfree(cp->handlers);
	cpfree(cp->components);
	if(cp->staticShapes) cpSpatialIndexFree(cp->staticShapes);
	if(cp->dynamicShapes) cpSpatialIndexFree(cp->dynamicShapes);
	cpfree(cp);
}

//MARK: Validation

static cpBool
BodiesMatch(cpSpaceCheckpoint *cp, cpSpace *space)
{
	// Sleeping membership is simulation state: a body may move between the
	// awake array and the sleeping components between capture and restore.
	// Only the total body count and per-body fixed parameters are structural.
	if(CollectBodies(space, NULL, 0) != cp->numBodies) return cpFalse;

	cpBody **bodyList = (cpBody **)cpcalloc((size_t)(cp->numBodies ? cp->numBodies : 1), sizeof(cpBody *));
	if(!bodyList) return cpFalse;
	CollectBodies(space, bodyList, cp->numBodies);

	cpBool ok = cpTrue;
	for(int i=0; i<cp->numBodies; i++){
		cpCPBody *sb = FindBody(cp, bodyList[i]);
		if(!sb || sb->type != cpBodyGetType(bodyList[i])){ ok = cpFalse; break; }
	}

	if(ok){
		for(int i=0; i<cp->numBodies; i++){
			cpCPBody *sb = &cp->bodies[i];
			cpBody *body = sb->body;
			if(
				body->space != space ||
				body->m != sb->m || body->m_inv != sb->m_inv ||
				body->i != sb->i || body->i_inv != sb->i_inv ||
				!cpveql(body->cog, sb->cog)
			){ ok = cpFalse; break; }

			int shapes = 0, constraints = 0;
			CP_BODY_FOREACH_SHAPE(body, shape) shapes++;
			CP_BODY_FOREACH_CONSTRAINT(body, constraint) constraints++;
			if(shapes != sb->shapeCount || constraints != sb->constraintCount){
				ok = cpFalse; break;
			}
		}
	}

	cpfree(bodyList);
	return ok;
}

static cpBool
ShapesMatch(cpSpaceCheckpoint *cp)
{
	for(int i=0; i<cp->numShapes; i++){
		cpCPShape *ss = &cp->shapes[i];
		cpShape *shape = ss->shape;
		if(
			shape->space != cp->space || shape->body != ss->body ||
			shape->klass != ss->klass ||
			shape->sensor != ss->sensor ||
			shape->e != ss->e || shape->u != ss->u ||
			!cpveql(shape->surfaceV, ss->surfaceV) ||
			shape->type != ss->type ||
			shape->filter.group != ss->filter.group ||
			shape->filter.categories != ss->filter.categories ||
			shape->filter.mask != ss->filter.mask ||
			shape->hashid != ss->hashid
		) return cpFalse;
		if(
			shape->massInfo.m != ss->massInfo.m ||
			shape->massInfo.i != ss->massInfo.i ||
			!cpveql(shape->massInfo.cog, ss->massInfo.cog) ||
			shape->massInfo.area != ss->massInfo.area
		) return cpFalse;
		if(!GeometryMatches(&ss->geom, shape)) return cpFalse;
	}
	return cpTrue;
}

static cpBool
ConstraintsMatch(cpSpaceCheckpoint *cp)
{
	for(int i=0; i<cp->numConstraints; i++){
		cpCPConstraint *sc = &cp->constraints[i];
		cpConstraint *constraint = sc->constraint;
		int kind;
		if(
			constraint->space != cp->space ||
			!ConstraintKind(constraint, &kind) || kind != sc->kind
		) return cpFalse;
		if(!ConstraintConfigMatches(sc, constraint)) return cpFalse;
	}
	return cpTrue;
}

static cpBool
handlerSetEql(cpCollisionHandler *check, cpCollisionHandler *pair)
{
	return (
		(check->typeA == pair->typeA && check->typeB == pair->typeB) ||
		(check->typeB == pair->typeA && check->typeA == pair->typeB)
	);
}

static cpBool
HandlersMatch(cpSpaceCheckpoint *cp, cpSpace *space)
{
	if((int)cpHashSetCount(space->collisionHandlers) != cp->numHandlers) return cpFalse;
	if(space->usesWildcards != cp->usesWildcards) return cpFalse;
	if(memcmp(&space->defaultHandler, &cp->defaultHandler, sizeof(cpCollisionHandler)) != 0) return cpFalse;

	for(int i=0; i<cp->numHandlers; i++){
		cpCollisionHandler *want = &cp->handlers[i].handler;
		cpCollisionHandler *have = (cpCollisionHandler *)cpHashSetFind(
			space->collisionHandlers, CP_HASH_PAIR(want->typeA, want->typeB), want
		);
		if(!have || !handlerSetEql(have, want)) return cpFalse;
		if(memcmp(have, want, sizeof(cpCollisionHandler)) != 0) return cpFalse;
	}
	return cpTrue;
}

static cpBool
SpaceParamsMatch(cpSpaceCheckpoint *cp, cpSpace *space)
{
	return (
		space->iterations == cp->iterations &&
		cpveql(space->gravity, cp->gravity) &&
		space->damping == cp->damping &&
		space->idleSpeedThreshold == cp->idleSpeedThreshold &&
		space->sleepTimeThreshold == cp->sleepTimeThreshold &&
		space->collisionSlop == cp->collisionSlop &&
		space->collisionBias == cp->collisionBias &&
		space->collisionPersistence == cp->collisionPersistence &&
		space->shapeIDCounter == cp->shapeIDCounter
	);
}

//MARK: Restore helpers

static cpArbiter *
PooledArbiterAlloc(cpSpace *space)
{
	if(space->pooledArbiters->num == 0){
		int count = CP_BUFFER_BYTES/(int)sizeof(cpArbiter);
		if(count <= 0) return NULL;
		cpArbiter *buffer = (cpArbiter *)cpcalloc(1, CP_BUFFER_BYTES);
		if(!buffer) return NULL;
		cpArrayPush(space->allocatedBuffers, buffer);
		for(int i=0; i<count; i++) cpArrayPush(space->pooledArbiters, buffer + i);
	}
	return (cpArbiter *)cpArrayPop(space->pooledArbiters);
}

static void
WriteContact(struct cpContact *dst, const cpCPContact *src)
{
	dst->r1 = src->r1; dst->r2 = src->r2;
	dst->nMass = src->nMass; dst->tMass = src->tMass;
	dst->bounce = src->bounce;
	dst->jnAcc = src->jnAcc; dst->jtAcc = src->jtAcc; dst->jBias = src->jBias;
	dst->bias = src->bias;
	dst->hash = src->hash;
}

// Mirror of the static cpBodyPushArbiter() in cpSpaceComponent.c.
static void
PushArbiterToBody(cpBody *body, cpArbiter *arb)
{
	struct cpArbiterThread *thread = cpArbiterThreadForBody(arb, body);
	cpArbiter *next = body->arbiterList;
	thread->next = next;
	thread->prev = NULL;
	if(next) cpArbiterThreadForBody(next, body)->prev = arb;
	body->arbiterList = arb;
}

// cpHashSetFilter callback: move every cached arbiter back to the pool.
static cpBool
teardownCachedArbiter(cpArbiter *arb, cpSpace *space)
{
	cpArrayPush(space->pooledArbiters, arb);
	return cpFalse;
}

//MARK: cpSpaceCheckpointRestore

cpBool
cpSpaceCheckpointRestore(cpSpace *space, const cpSpaceCheckpoint *checkpoint)
{
	cpSpaceCheckpoint *cp = (cpSpaceCheckpoint *)(void *)checkpoint;
	if(!space || !cp) return cpFalse;
	if(cp->space != space) return cpFalse;
	if(space->locked) return cpFalse;
	if(space->postStepCallbacks->num > 0) return cpFalse;
	if(space->rousedBodies->num > 0) return cpFalse;
	if(!(space->cpCheckpointFlags & CP_CHECKPOINT_SUPPORTED)) return cpFalse;
	if(cp->generation != space->cpCheckpointGeneration) return cpFalse;

	// Full structural validation before touching anything.
	if(!SpaceParamsMatch(cp, space)) return cpFalse;
	if(!BodiesMatch(cp, space)) return cpFalse;
	if(!ShapesMatch(cp)) return cpFalse;
	if(!ConstraintsMatch(cp)) return cpFalse;
	if(!HandlersMatch(cp, space)) return cpFalse;

	// Pre-allocate the private contact blocks of sleeping arbiters so that an
	// allocation failure cannot leave a half-restored space behind.
	struct cpContact **sleepingContacts = (struct cpContact **)cpcalloc(
		(size_t)(cp->numArbiters ? cp->numArbiters : 1), sizeof(struct cpContact *)
	);
	if(!sleepingContacts) return cpFalse;
	for(int i=0; i<cp->numArbiters; i++){
		sleepingContacts[i] = NULL;
		if(cp->arbiters[i].sleeping && cp->arbiters[i].count > 0){
			sleepingContacts[i] = (struct cpContact *)cpcalloc((size_t)cp->arbiters[i].count, sizeof(struct cpContact));
			if(!sleepingContacts[i]){
				for(int j=0; j<i; j++) cpfree(sleepingContacts[j]);
				cpfree(sleepingContacts);
				return cpFalse;
			}
		}
	}

	// Make sure the arbiter pool is large enough for the rebuild before any
	// simulation state is touched.
	while(space->pooledArbiters->num < cp->numArbiters){
		int count = CP_BUFFER_BYTES/(int)sizeof(cpArbiter);
		if(count <= 0){
			for(int j=0; j<cp->numArbiters; j++) cpfree(sleepingContacts[j]);
			cpfree(sleepingContacts);
			return cpFalse;
		}
		cpArbiter *buffer = (cpArbiter *)cpcalloc(1, CP_BUFFER_BYTES);
		if(!buffer){
			for(int j=0; j<cp->numArbiters; j++) cpfree(sleepingContacts[j]);
			cpfree(sleepingContacts);
			return cpFalse;
		}
		cpArrayPush(space->allocatedBuffers, buffer);
		for(int j=0; j<count; j++) cpArrayPush(space->pooledArbiters, buffer + j);
	}

	// --- Teardown of all derived state -----------------------------------

	// Pool the cached arbiters (their contacts live in the contact buffers).
	cpHashSetFilter(space->cachedArbiters, (cpHashSetFilterFunc)teardownCachedArbiter, space);

	// Pool the sleeping arbiters and free their private contact blocks.
	for(int i=0; i<space->sleepingComponents->num; i++){
		CP_BODY_FOREACH_COMPONENT((cpBody *)space->sleepingComponents->arr[i], body){
			CP_BODY_FOREACH_ARBITER(body, arb){
				if(!(body == arb->body_a || cpBodyGetType(arb->body_a) == CP_BODY_TYPE_STATIC)) continue;
				if(arb->contacts) cpfree(arb->contacts);
				cpArrayPush(space->pooledArbiters, arb);
			}
		}
	}

	// Reset the per-body contact and constraint graphs.
	for(int i=0; i<cp->numBodies; i++){
		cpBody *body = cp->bodies[i].body;
		body->arbiterList = NULL;
		body->constraintList = NULL;
	}

	space->arbiters->num = 0;
	space->constraints->num = 0;

	// Free the contact buffer ring and start a fresh one. The header must
	// carry the captured timestamp: on the next step cpSpaceStep() decides
	// whether to rotate the ring from (newStamp - headStamp), and a stamp
	// from the branched future would make it recycle the warm contacts before
	// they can be re-used.
	if(space->contactBuffersHead){
		cpContactBufferHeader *head = space->contactBuffersHead;
		cpContactBufferHeader *cur = head;
		do {
			cpContactBufferHeader *next = cur->next;
			cpArrayDeleteObj(space->allocatedBuffers, cur);
			cpfree(cur);
			cur = next;
		} while(cur != head);
		space->contactBuffersHead = NULL;
	}
	space->stamp = cp->stamp;
	cpSpacePushFreshContactBuffer(space);

	// Re-clone the pristine captured trees. Re-cloning (rather than reusing the
	// copies directly) keeps them available for further restores and branches.
	cpSpatialIndexFree(space->staticShapes);
	cpSpatialIndexFree(space->dynamicShapes);
	space->staticShapes = cpBBTreeClone(cp->staticShapes, NULL, NULL);
	space->dynamicShapes = cpBBTreeClone(cp->dynamicShapes, space->staticShapes, cp->staticShapes);
	if(!space->staticShapes || !space->dynamicShapes){
		// cpBBTreeClone only fails on allocation failure; nothing partial is
		// observable since the old indexes were already released.
		return cpFalse;
	}

	// --- Restore body state ----------------------------------------------

	for(int i=0; i<cp->numBodies; i++){
		cpCPBody *sb = &cp->bodies[i];
		cpBody *body = sb->body;

		body->velocity_func = sb->velocity_func;
		body->position_func = sb->position_func;
		body->p = sb->p; body->v = sb->v; body->f = sb->f;
		body->a = sb->a; body->w = sb->w; body->t = sb->t;
		body->transform = sb->transform;
		body->v_bias = sb->v_bias; body->w_bias = sb->w_bias;
		body->sleeping.root = sb->root;
		body->sleeping.next = sb->next;
		body->sleeping.idleTime = sb->idleTime;
		body->userData = sb->userData;
	}

	// --- Restore shape state ------------------------------------------------
	// The broadphase trees were structurally cloned with every leaf already in
	// place, so shapes must not be re-inserted here; only their stored BB and
	// user pointer need refreshing.

	for(int i=0; i<cp->numShapes; i++){
		cpCPShape *ss = &cp->shapes[i];
		cpShape *shape = ss->shape;
		shape->bb = ss->bb;
		shape->userData = ss->userData;
	}

	// --- Rebuild body array membership and sleeping components ------------

	space->dynamicBodies->num = 0;
	space->sleepingComponents->num = 0;

	for(int i=0; i<cp->numBodies; i++){
		cpCPBody *sb = &cp->bodies[i];
		if(sb->type == CP_BODY_TYPE_DYNAMIC){
			if(sb->sleeping){
				// Component roots are recorded in snapshot order.
			} else {
				cpArrayPush(space->dynamicBodies, sb->body);
			}
		}
	}
	for(int i=0; i<cp->numComponents; i++){
		cpArrayPush(space->sleepingComponents, cp->components[i]);
	}

	// --- Restore constraints and rebuild the constraint graph -------------

	for(int i=0; i<cp->numConstraints; i++){
		cpCPConstraint *sc = &cp->constraints[i];
		cpConstraint *constraint = sc->constraint;

		constraint->maxForce = sc->maxForce;
		constraint->errorBias = sc->errorBias;
		constraint->maxBias = sc->maxBias;
		constraint->collideBodies = sc->collideBodies;
		constraint->userData = sc->userData;
		memcpy((char *)constraint + sizeof(cpConstraint), sc->tail, sc->tailSize);

		// Awake constraints go back onto the solver array in captured order,
		// which already matches space->constraints ordering.
		if(sc->awake) cpArrayPush(space->constraints, constraint);
	}

	// Rebuild each body's constraint list in its captured order.
	for(int i=0; i<cp->numBodies; i++){
		cpCPBody *sb = &cp->bodies[i];
		cpBody *body = sb->body;
		for(int k=sb->constraintCount - 1; k>=0; k--){
			cpConstraint *constraint = sb->constraintOrder[k];
			if(constraint->a == body) constraint->next_a = body->constraintList, body->constraintList = constraint;
			if(constraint->b == body) constraint->next_b = body->constraintList, body->constraintList = constraint;
		}
	}

	// --- Rebuild the arbiters --------------------------------------------

	// Construct every arbiter and its contacts, but defer the list threading
	// so the captured ordering can be reproduced exactly.
	for(int i=0; i<cp->numArbiters; i++){
		cpCPArbiter *sa = &cp->arbiters[i];
		cpArbiter *arb = PooledArbiterAlloc(space);
		if(!arb){ cpfree(sleepingContacts); return cpFalse; }
		sa->rebuilt = arb;

		cpArbiterInit(arb, (cpShape *)sa->a, (cpShape *)sa->b);
		arb->e = sa->e; arb->u = sa->u;
		arb->surface_vr = sa->surface_vr;
		arb->data = sa->data;
		arb->body_a = sa->body_a; arb->body_b = sa->body_b;
		arb->handler = sa->handler; arb->handlerA = sa->handlerA; arb->handlerB = sa->handlerB;
		arb->count = sa->count;
		arb->n = sa->n;
		arb->swapped = sa->swapped;
		arb->stamp = sa->stamp;
		arb->state = sa->state;

		if(sa->count > 0){
			if(sa->sleeping){
				struct cpContact *contacts = sleepingContacts[i];
				for(int j=0; j<sa->count; j++) WriteContact(&contacts[j], &sa->contacts[j]);
				arb->contacts = contacts;
			} else {
				struct cpContact *contacts = cpContactBufferGetArray(space);
				for(int j=0; j<sa->count; j++) WriteContact(&contacts[j], &sa->contacts[j]);
				cpSpacePushContacts(space, sa->count);
				arb->contacts = contacts;
			}
		} else {
			arb->contacts = NULL;
		}

		// Only awake arbiters live in the cache hash set.
		if(!sa->sleeping){
			const cpShape *shape_pair[] = {sa->a, sa->b};
			cpHashValue arbHashID = CP_HASH_PAIR((cpHashValue)sa->a, (cpHashValue)sa->b);
			cpHashSetInsert(space->cachedArbiters, arbHashID, shape_pair, NULL, arb);
		}
	}

	// Rebuild space->arbiters in the exact captured solver order.
	for(int i=0; i<cp->numActiveArbiters; i++){
		cpArbiter *original = cp->activeArbiters[i];
		for(int j=0; j<cp->numArbiters; j++){
			if(cp->arbiters[j].arbiter == original){
				cpArrayPush(space->arbiters, cp->arbiters[j].rebuilt);
				break;
			}
		}
	}

	// Rebuild each body's contact graph in its captured order.
	for(int i=0; i<cp->numBodies; i++){
		cpCPBody *sb = &cp->bodies[i];
		cpBody *body = sb->body;
		for(int k=sb->arbiterCount - 1; k>=0; k--){
			cpArbiter *original = sb->arbiterOrder[k];
			for(int j=0; j<cp->numArbiters; j++){
				if(cp->arbiters[j].arbiter == original){
					PushArbiterToBody(body, cp->arbiters[j].rebuilt);
					break;
				}
			}
		}
	}
	// The inner contact blocks are now owned by the rebuilt arbiters.
	cpfree(sleepingContacts);

	// --- Restore space-level parameters -----------------------------------

	space->iterations = cp->iterations;
	space->gravity = cp->gravity;
	space->damping = cp->damping;
	space->idleSpeedThreshold = cp->idleSpeedThreshold;
	space->sleepTimeThreshold = cp->sleepTimeThreshold;
	space->collisionSlop = cp->collisionSlop;
	space->collisionBias = cp->collisionBias;
	space->collisionPersistence = cp->collisionPersistence;
	space->curr_dt = cp->curr_dt;
	space->shapeIDCounter = cp->shapeIDCounter;
	space->usesWildcards = cp->usesWildcards;
	memcpy(&space->defaultHandler, &cp->defaultHandler, sizeof(cpCollisionHandler));

	return cpTrue;
}
