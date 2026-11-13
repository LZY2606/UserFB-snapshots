/* Target tests for the in-process cpSpace checkpoint API.
 *
 * The cases build small deterministic scenes, run an unbranched reference
 * simulation, then replay the same scenes through cpSpaceCheckpointNew()/
 * cpSpaceCheckpointRestore() branches and compare body kinematics, joint and
 * contact impulses, sleeping state and collision event sequences.
 */

#include <math.h>
#include <string.h>

#include "chipmunk/chipmunk.h"
#include "chipmunk/chipmunk_private.h"
#include "chipmunk/cpHastySpace.h"
#include "check.h"

#define MAX_STEPS 256
#define MAX_BODIES 12
#define MAX_JOINTS 12

enum {
	COLL_GROUND = 1,
	COLL_BALL = 2,
	COLL_BOX = 3,
};

//MARK: Numeric comparison (absolute 1e-9 plus relative 1e-9)

static int
closef(cpFloat a, cpFloat b)
{
	cpFloat d = fabs(a - b);
	return d <= 1e-9 + 1e-9*fmax(fabs(a), fabs(b));
}

static int
closev(cpVect a, cpVect b)
{
	return closef(a.x, b.x) && closef(a.y, b.y);
}

//MARK: Collision event recording

typedef struct StepRecord {
	int begin;
	int separate;
	int contacts;
	cpVect impulse;
} StepRecord;

typedef struct Recorder {
	StepRecord steps[MAX_STEPS];
	int n;
} Recorder;

static cpBool
recBegin(cpArbiter *arb, cpSpace *space, cpDataPointer data)
{
	Recorder *r = (Recorder *)data;
	r->steps[r->n - 1].begin++;
	return cpTrue;
}

// Distinct callback used to exercise handler-field validation.
static cpBool
otherBegin(cpArbiter *arb, cpSpace *space, cpDataPointer data)
{
	return cpTrue;
}

static void
recSeparate(cpArbiter *arb, cpSpace *space, cpDataPointer data)
{
	Recorder *r = (Recorder *)data;
	r->steps[r->n - 1].separate++;
}

static void
recPostSolve(cpArbiter *arb, cpSpace *space, cpDataPointer data)
{
	Recorder *r = (Recorder *)data;
	StepRecord *s = &r->steps[r->n - 1];
	s->contacts += cpArbiterGetCount(arb);
	s->impulse = cpvadd(s->impulse, cpArbiterTotalImpulse(arb));
}

static void
recInstall(cpSpace *space, Recorder *r)
{
	memset(r, 0, sizeof(*r));
	cpCollisionHandler *h = cpSpaceAddCollisionHandler(space, COLL_GROUND, COLL_BALL);
	h->beginFunc = recBegin; h->separateFunc = recSeparate; h->postSolveFunc = recPostSolve;
	h->userData = r;

	cpCollisionHandler *h2 = cpSpaceAddCollisionHandler(space, COLL_GROUND, COLL_BOX);
	h2->beginFunc = recBegin; h2->separateFunc = recSeparate; h2->postSolveFunc = recPostSolve;
	h2->userData = r;

	cpCollisionHandler *h3 = cpSpaceAddCollisionHandler(space, COLL_BOX, COLL_BOX);
	h3->beginFunc = recBegin; h3->separateFunc = recSeparate; h3->postSolveFunc = recPostSolve;
	h3->userData = r;
}

//MARK: Worlds

typedef struct World {
	cpSpace *space;
	Recorder rec;
	cpBody *body[MAX_BODIES];
	int nb;
	cpShape *shape[MAX_BODIES];
	int ns;
	cpConstraint *joint[MAX_JOINTS];
	int nj;

	// Per-frame sampled state.
	cpVect bp[MAX_BODIES][MAX_STEPS];
	cpVect bv[MAX_BODIES][MAX_STEPS];
	cpFloat ba[MAX_BODIES][MAX_STEPS];
	cpFloat bw[MAX_BODIES][MAX_STEPS];
	int bsleep[MAX_BODIES][MAX_STEPS];
	cpFloat jimp[MAX_JOINTS][MAX_STEPS];
} World;

static cpBody *
worldAddBody(World *w, cpFloat mass, cpFloat moment, cpVect pos)
{
	cpBody *b = cpSpaceAddBody(w->space, cpBodyNew(mass, moment));
	cpBodySetPosition(b, pos);
	w->body[w->nb++] = b;
	return b;
}

static void
worldAddGround(World *w)
{
	cpShape *floor = cpSpaceAddShape(w->space,
		cpSegmentShapeNew(cpSpaceGetStaticBody(w->space), cpv(-40, 0), cpv(40, 0), 0.1));
	cpShapeSetFriction(floor, 0.8);
	cpShapeSetElasticity(floor, 0.0);
	cpShapeSetCollisionType(floor, COLL_GROUND);
	w->shape[w->ns++] = floor;
}

static cpShape *
worldAddBall(World *w, cpBody *b, cpFloat r)
{
	cpShape *s = cpSpaceAddShape(w->space, cpCircleShapeNew(b, r, cpvzero));
	cpShapeSetFriction(s, 0.7);
	cpShapeSetCollisionType(s, COLL_BALL);
	w->shape[w->ns++] = s;
	return s;
}

static cpShape *
worldAddBox(World *w, cpBody *b, cpFloat side)
{
	cpShape *s = cpSpaceAddShape(w->space, cpBoxShapeNew(b, side, side, 0.02));
	cpShapeSetFriction(s, 0.7);
	cpShapeSetCollisionType(s, COLL_BOX);
	w->shape[w->ns++] = s;
	return s;
}

static void
worldStep(World *w, cpFloat dt)
{
	int f = w->rec.n;
	CHECK(f < MAX_STEPS);
	memset(&w->rec.steps[f], 0, sizeof(StepRecord));
	// Advance the frame counter before stepping: collision callbacks fire
	// inside cpSpaceStep() and record into the current frame.
	w->rec.n = f + 1;
	cpSpaceStep(w->space, dt);
	for(int i=0; i<w->nb; i++){
		cpBody *b = w->body[i];
		w->bp[i][f] = cpBodyGetPosition(b);
		w->bv[i][f] = cpBodyGetVelocity(b);
		w->ba[i][f] = cpBodyGetAngle(b);
		w->bw[i][f] = cpBodyGetAngularVelocity(b);
		w->bsleep[i][f] = cpBodyIsSleeping(b);
	}
	for(int j=0; j<w->nj; j++) w->jimp[j][f] = cpConstraintGetImpulse(w->joint[j]);
}

static void
worldFree(World *w)
{
	for(int j=0; j<w->nj; j++){
		cpSpaceRemoveConstraint(w->space, w->joint[j]);
		cpConstraintFree(w->joint[j]);
	}
	for(int s=0; s<w->ns; s++){
		cpSpaceRemoveShape(w->space, w->shape[s]);
		cpShapeFree(w->shape[s]);
	}
	for(int i=0; i<w->nb; i++){
		cpSpaceRemoveBody(w->space, w->body[i]);
		cpBodyFree(w->body[i]);
	}
	cpSpaceFree(w->space);
	cpfree(w);
}

// Variable timestep sequence; never a constant 1/60.
static cpFloat
dtFor(int frame)
{
	return (frame % 3 == 0) ? (1.0/60.0) : (frame % 3 == 1 ? 1.0/90.0 : 1.0/120.0);
}

static void
checkWorldsMatch(const World *ref, const World *br, int fromFrame, int toFrame, int checkJoints)
{
	for(int f=fromFrame; f<toFrame; f++){
		// Collision event sequence.
		const StepRecord *a = &ref->rec.steps[f];
		const StepRecord *b = &br->rec.steps[f];
		CHECK(a->begin == b->begin);
		CHECK(a->separate == b->separate);
		CHECK(a->contacts == b->contacts);
		CHECK(closev(a->impulse, b->impulse));

		for(int i=0; i<br->nb; i++){
			CHECK(closev(ref->bp[i][f], br->bp[i][f]));
			CHECK(closev(ref->bv[i][f], br->bv[i][f]));
			CHECK(closef(ref->ba[i][f], br->ba[i][f]));
			CHECK(closef(ref->bw[i][f], br->bw[i][f]));
			CHECK(ref->bsleep[i][f] == br->bsleep[i][f]);
		}
		if(checkJoints){
			for(int j=0; j<br->nj; j++) CHECK(closef(ref->jimp[j][f], br->jimp[j][f]));
		}
	}
}

// Run the shared force/input script for a frame range.
static void
runScript(World *w, int from, int to, int variant)
{
	for(int f=from; f<to; f++){
		if(variant == 1 && f >= from + 4 && f < from + 8){
			// Branch variant: sideways kick on the first body.
			cpBodyApplyImpulseAtWorldPoint(w->body[0], cpv(1.5, 0), cpBodyGetPosition(w->body[0]));
		}
		worldStep(w, dtFor(f));
	}
}

//MARK: contacts group

static World *
buildBallOnGround(void)
{
	World *w = (World *)cpcalloc(1, sizeof(World));
	w->space = cpSpaceNew();
	cpSpaceSetGravity(w->space, cpv(0, -200));
	recInstall(w->space, &w->rec);
	worldAddGround(w);
	cpBody *ball = worldAddBody(w, 1, cpMomentForCircle(1, 0, 0.5, cpvzero), cpv(0, 2));
	worldAddBall(w, ball, 0.5);
	return w;
}

static void
case_contact_lifecycle(void)
{
	World *ref = buildBallOnGround();
	World *br = buildBallOnGround();

	// Settle into a persistent resting contact, then checkpoint the branch.
	for(int f=0; f<25; f++){ worldStep(ref, dtFor(f)); worldStep(br, dtFor(f)); }
	cpSpaceCheckpoint *cp = cpSpaceCheckpointNew(br->space);
	CHECK(cp != NULL);

	// First branch run, same script as the reference.
	runScript(ref, 25, 70, 0);
	runScript(br, 25, 70, 0);
	checkWorldsMatch(ref, br, 25, 70, 0);

	// Exactly one begin over the whole settle, no separate while resting.
	int begins = 0, separates = 0;
	for(int f=0; f<ref->rec.n; f++){
		begins += ref->rec.steps[f].begin;
		separates += ref->rec.steps[f].separate;
	}
	CHECK(begins == 1);
	CHECK(separates == 0);

	// Rewind to the warm contact and replay: the persistent contact must not
	// fire begin again and no phantom separate may appear.
	CHECK(cpSpaceCheckpointRestore(br->space, cp) == cpTrue);
	br->rec.n = 25;
	runScript(br, 25, 70, 0);
	checkWorldsMatch(ref, br, 25, 70, 0);

	int begins2 = 0, separates2 = 0;
	for(int f=25; f<br->rec.n; f++){
		begins2 += br->rec.steps[f].begin;
		separates2 += br->rec.steps[f].separate;
	}
	CHECK(begins2 == 0);
	CHECK(separates2 == 0);

	cpSpaceCheckpointFree(cp);
	worldFree(ref);
	worldFree(br);
}

static World *
buildSlidingBox(void)
{
	World *w = (World *)cpcalloc(1, sizeof(World));
	w->space = cpSpaceNew();
	cpSpaceSetGravity(w->space, cpv(0, -200));
	recInstall(w->space, &w->rec);
	worldAddGround(w);
	cpBody *box = worldAddBody(w, 2, cpMomentForBox(2, 1, 1), cpv(0, 0.6));
	cpBodySetVelocity(box, cpv(4, 0));
	worldAddBox(w, box, 1);
	return w;
}

static void
case_contact_sliding(void)
{
	World *ref = buildSlidingBox();
	World *br = buildSlidingBox();

	cpSpaceCheckpoint *cp = cpSpaceCheckpointNew(br->space);
	CHECK(cp != NULL);

	for(int f=0; f<70; f++){ worldStep(ref, dtFor(f)); worldStep(br, dtFor(f)); }
	checkWorldsMatch(ref, br, 0, 70, 0);

	// Friction is actually doing work: tangent contact impulses are nonzero.
	cpFloat totalTangent = 0;
	for(int f=0; f<ref->rec.n; f++) totalTangent += fabs(ref->rec.steps[f].impulse.x);
	CHECK(totalTangent > 1.0);
	// And the box slows down.
	CHECK(ref->bv[0][69].x < ref->bv[0][0].x);

	// Rewind and replay the sliding contact.
	CHECK(cpSpaceCheckpointRestore(br->space, cp) == cpTrue);
	br->rec.n = 0;
	for(int f=0; f<70; f++) worldStep(br, dtFor(f));
	checkWorldsMatch(ref, br, 0, 70, 0);

	cpSpaceCheckpointFree(cp);
	worldFree(ref);
	worldFree(br);
}

static World *
buildGapBall(void)
{
	World *w = (World *)cpcalloc(1, sizeof(World));
	w->space = cpSpaceNew();
	cpSpaceSetGravity(w->space, cpv(0, -200));
	cpSpaceSetCollisionPersistence(w->space, 4);
	recInstall(w->space, &w->rec);
	worldAddGround(w);
	cpBody *ball = worldAddBody(w, 1, cpMomentForCircle(1, 0, 0.5, cpvzero), cpv(0, 0.6));
	worldAddBall(w, ball, 0.5);
	return w;
}

static void
case_contact_gap_cache(void)
{
	World *ref = buildGapBall();
	World *br = buildGapBall();

	// Settle the resting contact first.
	for(int f=0; f<40; f++){ worldStep(ref, dtFor(f)); worldStep(br, dtFor(f)); }
	cpSpaceCheckpoint *cp = cpSpaceCheckpointNew(br->space);
	CHECK(cp != NULL);

	// Shared script: short hop (gap shorter than persistence), long hop later.
	for(int f=40; f<110; f++){
		if(f == 40){
			cpBodySetVelocity(ref->body[0], cpv(0, 2.5));
			cpBodySetVelocity(br->body[0], cpv(0, 2.5));
		}
		if(f == 44){
			cpBodySetVelocity(ref->body[0], cpv(0, 0));
			cpBodySetVelocity(br->body[0], cpv(0, 0));
		}
		if(f == 70){
			cpBodySetVelocity(ref->body[0], cpv(0, 9));
			cpBodySetVelocity(br->body[0], cpv(0, 9));
		}
		worldStep(ref, dtFor(f));
		worldStep(br, dtFor(f));
	}
	checkWorldsMatch(ref, br, 40, 110, 0);

	// The short cached gap keeps the same arbiter warm; whatever events the
	// reference produces, the branch replay must produce the same sequence
	// (verified per frame by checkWorldsMatch). The long hop must expire the
	// cache, yielding one separate and one fresh begin.
	int longGapBegins = 0, longGapSeparates = 0;
	for(int f=70; f<110; f++){
		longGapBegins += ref->rec.steps[f].begin;
		longGapSeparates += ref->rec.steps[f].separate;
	}
	CHECK(longGapSeparates == 1);
	CHECK(longGapBegins == 1);

	// Replay from the checkpoint and compare the whole event stream.
	CHECK(cpSpaceCheckpointRestore(br->space, cp) == cpTrue);
	br->rec.n = 40;
	for(int f=40; f<110; f++){
		if(f == 40) cpBodySetVelocity(br->body[0], cpv(0, 2.5));
		if(f == 44) cpBodySetVelocity(br->body[0], cpv(0, 0));
		if(f == 70) cpBodySetVelocity(br->body[0], cpv(0, 9));
		worldStep(br, dtFor(f));
	}
	checkWorldsMatch(ref, br, 40, 110, 0);

	cpSpaceCheckpointFree(cp);
	worldFree(ref);
	worldFree(br);
}

static void
case_interleaved_restores(void)
{
	World *ref = buildBallOnGround();
	World *br = buildBallOnGround();
	for(int f=0; f<5; f++){ worldStep(ref, dtFor(f)); worldStep(br, dtFor(f)); }

	cpSpaceCheckpoint *cp1 = cpSpaceCheckpointNew(br->space);
	CHECK(cp1 != NULL);
	for(int f=5; f<15; f++){ worldStep(ref, dtFor(f)); worldStep(br, dtFor(f)); }
	cpSpaceCheckpoint *cp2 = cpSpaceCheckpointNew(br->space);
	CHECK(cp2 != NULL);

	// Diverge with a different input script.
	runScript(br, 15, 25, 1);
	runScript(ref, 15, 25, 0);

	// Restore cp1: branch must return to the reference state at frame 5.
	CHECK(cpSpaceCheckpointRestore(br->space, cp1) == cpTrue);
	CHECK(closev(cpBodyGetPosition(br->body[0]), ref->bp[0][4]));
	CHECK(closev(cpBodyGetVelocity(br->body[0]), ref->bv[0][4]));

	// Re-run frames 5..15; cp2 must remain usable afterwards.
	br->rec.n = 5;
	for(int f=5; f<15; f++) worldStep(br, dtFor(f));
	checkWorldsMatch(ref, br, 5, 15, 0);

	CHECK(cpSpaceCheckpointRestore(br->space, cp2) == cpTrue);
	CHECK(closev(cpBodyGetPosition(br->body[0]), ref->bp[0][14]));
	br->rec.n = 15;
	runScript(br, 15, 25, 0);
	checkWorldsMatch(ref, br, 15, 25, 0);

	// Freeing one checkpoint leaves the other and the space untouched.
	cpSpaceCheckpointFree(cp1);
	CHECK(cpSpaceCheckpointRestore(br->space, cp2) == cpTrue);
	CHECK(closev(cpBodyGetPosition(br->body[0]), ref->bp[0][14]));
	cpSpaceCheckpointFree(cp2);

	worldFree(ref);
	worldFree(br);
}

//MARK: Rejection handling

static void
case_rejections(void)
{
	World *w = buildBallOnGround();
	for(int f=0; f<10; f++) worldStep(w, dtFor(f));

	cpSpaceCheckpoint *cp = cpSpaceCheckpointNew(w->space);
	CHECK(cp != NULL);
	cpVect pBefore = cpBodyGetPosition(w->body[0]);

	// Structural change: add a body. Restore must be refused even if the
	// object is removed again afterwards.
	cpBody *extra = cpSpaceAddBody(w->space, cpBodyNew(1, 1));
	CHECK(cpSpaceCheckpointRestore(w->space, cp) == cpFalse);
	cpSpaceRemoveBody(w->space, extra);
	cpBodyFree(extra);
	CHECK(cpSpaceCheckpointRestore(w->space, cp) == cpFalse);
	CHECK(closev(cpBodyGetPosition(w->body[0]), pBefore));

	// A checkpoint taken after the change is valid again.
	cpSpaceCheckpoint *cp2 = cpSpaceCheckpointNew(w->space);
	CHECK(cp2 != NULL);
	cpBodySetMass(w->body[0], 2.0);
	CHECK(cpSpaceCheckpointRestore(w->space, cp2) == cpFalse);
	cpSpaceCheckpointFree(cp);
	cpSpaceCheckpointFree(cp2);

	// Body type change invalidates. A fresh checkpoint first keeps this
	// independent of the generation already consumed above.
	cpSpaceCheckpoint *cp3 = cpSpaceCheckpointNew(w->space);
	CHECK(cp3 != NULL);
	cpBodySetType(w->body[0], CP_BODY_TYPE_KINEMATIC);
	CHECK(cpSpaceCheckpointRestore(w->space, cp3) == cpFalse);
	cpBodySetType(w->body[0], CP_BODY_TYPE_DYNAMIC);
	CHECK(cpSpaceCheckpointRestore(w->space, cp3) == cpFalse);
	cpSpaceCheckpointFree(cp3);

	// Shape material and geometry changes invalidate.
	cpSpaceCheckpoint *cp4 = cpSpaceCheckpointNew(w->space);
	CHECK(cp4 != NULL);
	cpShapeSetFriction(w->shape[1], 0.31415);
	CHECK(cpSpaceCheckpointRestore(w->space, cp4) == cpFalse);
	cpSpaceCheckpointFree(cp4);

	cpSpaceCheckpoint *cp5 = cpSpaceCheckpointNew(w->space);
	CHECK(cp5 != NULL);
	cpSpaceSetIterations(w->space, 25);
	CHECK(cpSpaceCheckpointRestore(w->space, cp5) == cpFalse);
	cpSpaceCheckpointFree(cp5);

	// Cross-space restore is refused.
	World *other = buildBallOnGround();
	cpSpaceCheckpoint *cpOther = cpSpaceCheckpointNew(other->space);
	CHECK(cpOther != NULL);
	CHECK(cpSpaceCheckpointRestore(w->space, cpOther) == cpFalse);

	// NULL handling.
	CHECK(cpSpaceCheckpointNew(NULL) == NULL);
	CHECK(cpSpaceCheckpointRestore(w->space, NULL) == cpFalse);
	cpSpaceCheckpointFree(NULL);

	cpSpaceCheckpointFree(cpOther);
	worldFree(other);
	worldFree(w);
}

// Restore attempt from inside a collision callback: space is locked.
struct LockCtx { cpSpaceCheckpoint *cp; cpBool result; };
// Tests run sequentially, so a file-scope context keeps the collision
// handler's userData free for the event recorder.
static struct LockCtx *g_lockCtx;

static cpBool
lockedPreSolve(cpArbiter *arb, cpSpace *space, cpDataPointer data)
{
	g_lockCtx->result = cpSpaceCheckpointRestore(space, g_lockCtx->cp);
	return cpTrue;
}

static void
noopPostStep(cpSpace *space, void *key, void *data)
{
}

static void
case_locked_and_queue_rejection(void)
{
	World *w = buildBallOnGround();

	// The in-callback rejection uses a preSolve handler that must be in place
	// before the checkpoint, since handler fields are fixed structure.
	struct LockCtx ctx = {NULL, cpTrue};
	g_lockCtx = &ctx;
	cpCollisionHandler *h = cpSpaceAddCollisionHandler(w->space, COLL_GROUND, COLL_BALL);
	h->preSolveFunc = lockedPreSolve;

	cpSpaceCheckpoint *cp = cpSpaceCheckpointNew(w->space);
	CHECK(cp != NULL);
	ctx.cp = cp;
	CHECK(h->userData == &w->rec);

	for(int f=0; f<30 && ctx.result == cpTrue; f++) worldStep(w, dtFor(f));
	CHECK(ctx.result == cpFalse);
	// The space kept simulating after the refused restore.
	CHECK(isfinite(cpBodyGetPosition(w->body[0]).y));
	g_lockCtx = NULL;

	// A non-empty post-step queue rejects a restore. Lock the space and queue
	// a callback exactly as a collision handler would.
	cpSpaceLock(w->space);
	cpBool added = cpSpaceAddPostStepCallback(w->space, noopPostStep, (void *)0x2, NULL);
	CHECK(added == cpTrue);
	CHECK(cpSpaceCheckpointRestore(w->space, cp) == cpFalse);
	// Unlocking drains the queue; afterwards the restore is accepted again.
	cpSpaceUnlock(w->space, cpTrue);
	CHECK(cpSpaceCheckpointRestore(w->space, cp) == cpTrue);

	cpSpaceCheckpointFree(cp);
	worldFree(w);
}

static void
case_handler_field_rejection(void)
{
	World *w = buildBallOnGround();
	for(int f=0; f<10; f++) worldStep(w, dtFor(f));
	cpSpaceCheckpoint *cp = cpSpaceCheckpointNew(w->space);
	CHECK(cp != NULL);

	// Mutating a handler field after the checkpoint is rejected while the
	// field still differs from the captured value.
	cpCollisionHandler *h = cpSpaceAddCollisionHandler(w->space, COLL_GROUND, COLL_BALL);
	cpCollisionBeginFunc original = h->beginFunc;
	h->beginFunc = otherBegin;
	CHECK(cpSpaceCheckpointRestore(w->space, cp) == cpFalse);
	// A direct write-back between engine calls is an invisible transient
	// change the contract does not require detecting.
	h->beginFunc = original;
	CHECK(cpSpaceCheckpointRestore(w->space, cp) == cpTrue);

	cpSpaceCheckpointFree(cp);
	worldFree(w);
}

static void
case_unsupported_spaces(void)
{
	// cpHastySpace is explicitly unsupported.
	cpSpace *hasty = cpHastySpaceNew();
	CHECK(cpSpaceCheckpointNew(hasty) == NULL);
	cpHastySpaceFree(hasty);

	// Switching a regular space to a custom spatial index disables checkpoints.
	cpSpace *s = cpSpaceNew();
	cpSpaceCheckpoint *before = cpSpaceCheckpointNew(s);
	CHECK(before != NULL);
	cpSpaceCheckpointFree(before);
	cpSpaceUseSpatialHash(s, 10, 100);
	CHECK(cpSpaceCheckpointNew(s) == NULL);
	cpSpaceFree(s);
}

static void
case_free_after_space(void)
{
	cpSpace *s = cpSpaceNew();
	cpBody *b = cpSpaceAddBody(s, cpBodyNew(1, cpMomentForCircle(1, 0, 0.5, cpvzero)));
	cpBodySetPosition(b, cpv(0, 2));
	cpSpaceAddShape(s, cpCircleShapeNew(b, 0.5, cpvzero));
	cpSpaceCheckpoint *cp1 = cpSpaceCheckpointNew(s);
	cpSpaceCheckpoint *cp2 = cpSpaceCheckpointNew(s);
	CHECK(cp1 != NULL && cp2 != NULL);
	cpSpaceFree(s);
	// Checkpoint destruction must not require the space to remain alive.
	cpSpaceCheckpointFree(cp1);
	cpSpaceCheckpointFree(cp2);
}

//MARK: joints group

static World *
buildJointZoo(void)
{
	World *w = (World *)cpcalloc(1, sizeof(World));
	w->space = cpSpaceNew();
	cpSpaceSetGravity(w->space, cpv(0, -500));
	recInstall(w->space, &w->rec);
	worldAddGround(w);
	cpBody *st = cpSpaceGetStaticBody(w->space);

	// One dynamic body per built-in joint type, each tied to the static body.
	cpBody *pin = worldAddBody(w, 1, cpMomentForBox(1, 1, 1), cpv(-4, 4));
	w->joint[w->nj++] = cpSpaceAddConstraint(w->space, cpPinJointNew(st, pin, cpv(-4, 5), cpvzero));

	cpBody *slide = worldAddBody(w, 1, cpMomentForBox(1, 1, 1), cpv(-2, 4));
	w->joint[w->nj++] = cpSpaceAddConstraint(w->space, cpSlideJointNew(st, slide, cpv(-2, 6), cpvzero, 1, 3));

	cpBody *pivot = worldAddBody(w, 1, cpMomentForBox(1, 1, 1), cpv(0, 4));
	w->joint[w->nj++] = cpSpaceAddConstraint(w->space, cpPivotJointNew2(st, pivot, cpv(0, 5), cpvzero));

	cpBody *groove = worldAddBody(w, 1, cpMomentForBox(1, 1, 1), cpv(2, 4));
	w->joint[w->nj++] = cpSpaceAddConstraint(w->space, cpGrooveJointNew(st, groove, cpv(1.5, 6), cpv(2.5, 6), cpvzero));

	cpBody *spring = worldAddBody(w, 1, cpMomentForBox(1, 1, 1), cpv(4, 4));
	w->joint[w->nj++] = cpSpaceAddConstraint(w->space, cpDampedSpringNew(st, spring, cpv(4, 6), cpvzero, 2, 30, 4));

	cpBody *rspring = worldAddBody(w, 1, cpMomentForBox(1, 1, 1), cpv(6, 4));
	cpBodySetAngle(rspring, 0.3);
	w->joint[w->nj++] = cpSpaceAddConstraint(w->space, cpDampedRotarySpringNew(st, rspring, 0.0, 40, 2));

	cpBody *rlimit = worldAddBody(w, 1, cpMomentForBox(1, 1, 1), cpv(8, 4));
	cpBodySetAngle(rlimit, 0.8);
	w->joint[w->nj++] = cpSpaceAddConstraint(w->space, cpRotaryLimitJointNew(st, rlimit, -0.5, 0.5));

	cpBody *ratchet = worldAddBody(w, 1, cpMomentForBox(1, 1, 1), cpv(-4, 8));
	w->joint[w->nj++] = cpSpaceAddConstraint(w->space, cpRatchetJointNew(st, ratchet, 0.0, 0.4));

	cpBody *gear = worldAddBody(w, 1, cpMomentForBox(1, 1, 1), cpv(-2, 8));
	w->joint[w->nj++] = cpSpaceAddConstraint(w->space, cpGearJointNew(st, gear, 0.0, 2.0));

	cpBody *motor = worldAddBody(w, 1, cpMomentForBox(1, 1, 1), cpv(0, 8));
	w->joint[w->nj++] = cpSpaceAddConstraint(w->space, cpSimpleMotorNew(st, motor, 1.5));

	(void)worldAddBox; (void)worldAddBall;
	return w;
}

static void
case_all_joint_types(void)
{
	World *ref = buildJointZoo();
	World *br = buildJointZoo();
	CHECK(ref->nj == 10);

	cpSpaceCheckpoint *cp = cpSpaceCheckpointNew(br->space);
	CHECK(cp != NULL);

	for(int f=0; f<60; f++){
		if(f == 20){
			cpBodyApplyImpulseAtWorldPoint(ref->body[0], cpv(2, 1), cpBodyGetPosition(ref->body[0]));
			cpBodyApplyImpulseAtWorldPoint(br->body[0], cpv(2, 1), cpBodyGetPosition(br->body[0]));
		}
		worldStep(ref, dtFor(f));
		worldStep(br, dtFor(f));
	}
	checkWorldsMatch(ref, br, 0, 60, 1);

	// Restore and replay; every joint impulse must reproduce.
	CHECK(cpSpaceCheckpointRestore(br->space, cp) == cpTrue);
	br->rec.n = 0;
	for(int f=0; f<60; f++){
		if(f == 20) cpBodyApplyImpulseAtWorldPoint(br->body[0], cpv(2, 1), cpBodyGetPosition(br->body[0]));
		worldStep(br, dtFor(f));
	}
	checkWorldsMatch(ref, br, 0, 60, 1);

	cpSpaceCheckpointFree(cp);
	worldFree(ref);
	worldFree(br);
}

static World *
buildMotorChain(void)
{
	World *w = (World *)cpcalloc(1, sizeof(World));
	w->space = cpSpaceNew();
	cpSpaceSetGravity(w->space, cpv(0, -700));
	recInstall(w->space, &w->rec);
	worldAddGround(w);
	cpBody *st = cpSpaceGetStaticBody(w->space);

	// Three-link articulated chain driven by a rotary motor, falling onto
	// the ground so that motor torque, pivots and contacts act together.
	cpBody *b0 = worldAddBody(w, 1, cpMomentForBox(1, 0.4, 1.2), cpv(0, 3));
	cpBody *b1 = worldAddBody(w, 1, cpMomentForBox(1, 0.4, 1.2), cpv(0, 1.8));
	cpBody *b2 = worldAddBody(w, 1, cpMomentForBox(1, 0.4, 1.2), cpv(0, 0.9));
	worldAddBox(w, b0, 1.0);
	worldAddBox(w, b1, 1.0);
	worldAddBox(w, b2, 1.0);

	w->joint[w->nj++] = cpSpaceAddConstraint(w->space, cpSimpleMotorNew(st, b0, 2.0));
	w->joint[w->nj++] = cpSpaceAddConstraint(w->space, cpPivotJointNew2(st, b0, cpv(0, 3.6), cpv(0, 0.6)));
	w->joint[w->nj++] = cpSpaceAddConstraint(w->space, cpPivotJointNew2(b0, b1, cpv(0, -0.6), cpv(0, 0.6)));
	w->joint[w->nj++] = cpSpaceAddConstraint(w->space, cpPivotJointNew2(b1, b2, cpv(0, -0.6), cpv(0, 0.6)));
	return w;
}

static void
case_motor_articulated_ground(void)
{
	World *ref = buildMotorChain();
	World *br = buildMotorChain();

	for(int f=0; f<20; f++){ worldStep(ref, dtFor(f)); worldStep(br, dtFor(f)); }
	cpSpaceCheckpoint *cp = cpSpaceCheckpointNew(br->space);
	CHECK(cp != NULL);

	for(int f=20; f<120; f++){
		worldStep(ref, dtFor(f));
		worldStep(br, dtFor(f));
	}
	// The chain must actually be in contact with the ground over the window.
	int contactFrames = 0;
	for(int f=20; f<120; f++) if(ref->rec.steps[f].contacts > 0) contactFrames++;
	CHECK(contactFrames > 40);
	checkWorldsMatch(ref, br, 20, 120, 1);

	// Repeated restores must each reproduce poses, joint and contact impulses.
	for(int round=0; round<3; round++){
		CHECK(cpSpaceCheckpointRestore(br->space, cp) == cpTrue);
		br->rec.n = 20;
		for(int f=20; f<120; f++) worldStep(br, dtFor(f));
		checkWorldsMatch(ref, br, 20, 120, 1);
	}

	cpSpaceCheckpointFree(cp);
	worldFree(ref);
	worldFree(br);
}

//MARK: sleep group

static World *
buildSleepingStack(void)
{
	World *w = (World *)cpcalloc(1, sizeof(World));
	w->space = cpSpaceNew();
	cpSpaceSetGravity(w->space, cpv(0, -200));
	cpSpaceSetSleepTimeThreshold(w->space, 0.4);
	recInstall(w->space, &w->rec);
	worldAddGround(w);

	for(int i=0; i<3; i++){
		cpBody *b = worldAddBody(w, 1, cpMomentForBox(1, 0.8, 0.8), cpv(0, 0.45 + 0.82*i));
		worldAddBox(w, b, 0.8);
	}
	return w;
}

static int
allSleeping(World *w, int frame)
{
	for(int i=0; i<w->nb; i++) if(!w->bsleep[i][frame]) return 0;
	return 1;
}

static void
case_sleeping_island(void)
{
	World *ref = buildSleepingStack();
	World *br = buildSleepingStack();

	int sleepFrame = -1;
	for(int f=0; f<160; f++){
		worldStep(ref, dtFor(f));
		worldStep(br, dtFor(f));
		if(sleepFrame < 0 && allSleeping(ref, f)) sleepFrame = f;
	}
	CHECK(sleepFrame > 0);
	checkWorldsMatch(ref, br, 0, 160, 0);

	// Checkpoint a fully sleeping contact island.
	cpSpaceCheckpoint *cp = cpSpaceCheckpointNew(br->space);
	CHECK(cp != NULL);
	for(int i=0; i<br->nb; i++) CHECK(cpBodyIsSleeping(br->body[i]));

	// Wake the branch, run, then restore: the island must be asleep again.
	for(int i=0; i<br->nb; i++)
		cpBodyApplyImpulseAtWorldPoint(br->body[i], cpv(3, 0), cpBodyGetPosition(br->body[i]));
	for(int f=160; f<180; f++) worldStep(br, dtFor(f));
	CHECK(!cpBodyIsSleeping(br->body[0]));

	CHECK(cpSpaceCheckpointRestore(br->space, cp) == cpTrue);
	for(int i=0; i<br->nb; i++) CHECK(cpBodyIsSleeping(br->body[i]));

	// Wake both worlds identically and compare the wake-up and resettle.
	for(int i=0; i<ref->nb; i++)
		cpBodyApplyImpulseAtWorldPoint(ref->body[i], cpv(3, 0), cpBodyGetPosition(ref->body[i]));
	for(int i=0; i<br->nb; i++)
		cpBodyApplyImpulseAtWorldPoint(br->body[i], cpv(3, 0), cpBodyGetPosition(br->body[i]));
	br->rec.n = 160;
	for(int f=160; f<230; f++){
		worldStep(ref, dtFor(f));
		worldStep(br, dtFor(f));
	}
	checkWorldsMatch(ref, br, 160, 230, 0);

	cpSpaceCheckpointFree(cp);
	worldFree(ref);
	worldFree(br);
}

static void
case_sleep_events(void)
{
	World *ref = buildSleepingStack();
	World *br = buildSleepingStack();

	cpSpaceCheckpoint *cp = cpSpaceCheckpointNew(br->space);
	CHECK(cp != NULL);

	for(int f=0; f<170; f++){
		worldStep(ref, dtFor(f));
		worldStep(br, dtFor(f));
	}

	// Sleeping must not manufacture separate events: every contact pair
	// generated a begin, and no separate fires while the island rests asleep.
	int begins = 0, separates = 0;
	for(int f=0; f<ref->rec.n; f++){
		begins += ref->rec.steps[f].begin;
		separates += ref->rec.steps[f].separate;
	}
	CHECK(begins >= 3);
	CHECK(separates == 0);

	// Replay through the sleep transition; the event stream must be identical.
	CHECK(cpSpaceCheckpointRestore(br->space, cp) == cpTrue);
	br->rec.n = 0;
	for(int f=0; f<170; f++) worldStep(br, dtFor(f));
	int begins2 = 0, separates2 = 0;
	for(int f=0; f<br->rec.n; f++){
		begins2 += br->rec.steps[f].begin;
		separates2 += br->rec.steps[f].separate;
	}
	CHECK(begins2 == begins);
	CHECK(separates2 == separates);
	checkWorldsMatch(ref, br, 0, 170, 0);

	cpSpaceCheckpointFree(cp);
	worldFree(ref);
	worldFree(br);
}

//MARK: Registration

// Repeated restore churn across many checkpoints must stay deterministic and
// leak-free. Exercises the broadphase pair clone on every restore.
static void
case_repeated_restore_stress(void)
{
	World *w = buildBallOnGround();
	for(int f=0; f<20; f++) worldStep(w, dtFor(f));

	cpSpaceCheckpoint *base = cpSpaceCheckpointNew(w->space);
	CHECK(base != NULL);

	for(int round=0; round<12; round++){
		cpSpaceCheckpoint *cp = cpSpaceCheckpointNew(w->space);
		CHECK(cp != NULL);
		// Mutate velocities, forces, angle and run a short branch.
		for(int f=0; f<10; f++){
			if(f == 3) cpBodyApplyImpulseAtWorldPoint(w->body[0], cpv(0.5*round, 0), cpBodyGetPosition(w->body[0]));
			cpBodySetForce(w->body[0], cpv(0, -10*(round+1)));
			worldStep(w, dtFor(f));
		}
		// Back to the base, then a known fixed script.
		CHECK(cpSpaceCheckpointRestore(w->space, base) == cpTrue);
		for(int i=0; i<w->nb; i++) CHECK(isfinite(cpBodyGetPosition(w->body[i]).x));
		cpSpaceCheckpointFree(cp);
	}
	// Final replay from the base matches a fresh reference.
	World *ref = buildBallOnGround();
	for(int f=0; f<20; f++) worldStep(ref, dtFor(f));
	CHECK(cpSpaceCheckpointRestore(w->space, base) == cpTrue);
	// Realign the branch recorder to the same frame window as the reference.
	w->rec.n = 20;
	for(int f=20; f<50; f++){
		worldStep(ref, dtFor(f));
		worldStep(w, dtFor(f));
	}
	checkWorldsMatch(ref, w, 20, 50, 0);

	cpSpaceCheckpointFree(base);
	worldFree(ref);
	worldFree(w);
}

// A restored checkpoint must reproduce exact values, not just within tolerance.
static void
case_bit_exact_body_state(void)
{
	World *ref = buildBallOnGround();
	World *br = buildBallOnGround();
	for(int f=0; f<25; f++){ worldStep(ref, dtFor(f)); worldStep(br, dtFor(f)); }

	// Mutate the full set of dynamic fields, then confirm the restore returns
	// every one of them to the captured value bit-for-bit.
	cpSpaceCheckpoint *cp = cpSpaceCheckpointNew(br->space);
	CHECK(cp != NULL);
	cpBody *b = br->body[0];
	cpVect p0 = cpBodyGetPosition(b), v0 = cpBodyGetVelocity(b), f0 = b->f;
	cpFloat a0 = cpBodyGetAngle(b), w0 = cpBodyGetAngularVelocity(b), t0 = b->t;

	cpBodySetPosition(b, cpv(99, 99));
	cpBodySetVelocity(b, cpv(11, 12));
	b->f = cpv(13, 14); b->t = 15;
	cpBodySetAngle(b, 1.23456);
	cpBodySetAngularVelocity(b, 2.5);
	worldStep(br, dtFor(0));

	CHECK(cpSpaceCheckpointRestore(br->space, cp) == cpTrue);
	CHECK(cpBodyGetPosition(b).x == p0.x && cpBodyGetPosition(b).y == p0.y);
	CHECK(cpBodyGetVelocity(b).x == v0.x && cpBodyGetVelocity(b).y == v0.y);
	CHECK(b->f.x == f0.x && b->f.y == f0.y);
	CHECK(b->t == t0);
	CHECK(cpBodyGetAngle(b) == a0);
	CHECK(cpBodyGetAngularVelocity(b) == w0);

	cpSpaceCheckpointFree(cp);
	worldFree(ref);
	worldFree(br);
}

void
register_cases(void)
{
	add_case("contacts", "lifecycle", case_contact_lifecycle);
	add_case("contacts", "sliding", case_contact_sliding);
	add_case("contacts", "gap_cache", case_contact_gap_cache);
	add_case("contacts", "interleaved_restores", case_interleaved_restores);
	add_case("contacts", "rejections", case_rejections);
	add_case("contacts", "locked_and_queue", case_locked_and_queue_rejection);
	add_case("contacts", "handler_field", case_handler_field_rejection);
	add_case("contacts", "unsupported_spaces", case_unsupported_spaces);
	add_case("contacts", "free_after_space", case_free_after_space);
	add_case("contacts", "restore_stress", case_repeated_restore_stress);
	add_case("contacts", "bit_exact_state", case_bit_exact_body_state);

	add_case("joints", "all_joint_types", case_all_joint_types);
	add_case("joints", "motor_articulated_ground", case_motor_articulated_ground);

	add_case("sleep", "sleeping_island", case_sleeping_island);
	add_case("sleep", "sleep_events", case_sleep_events);
}
