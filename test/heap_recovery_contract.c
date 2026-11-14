/*
** heap_recovery_contract.c
**
** Contract tests for the trusted-ledger metadata rebuild.
**
** Three groups:
**   ledger    - the side ledger follows real, healthy heap operations
**   repair    - all corruption is constructed in plain integer-token
**               mirror arrays; the same engine plans, re-validates and
**               atomically applies the rebuild write set
**   lifecycle - stale plans, generation isolation, nested termination,
**               degraded-state refusal and ledger-only teardown
**
** Real allocations only ever use the healthy public API.  No test
** writes a real management header, guard byte or freed region; damaged
** fields, broken links and stale observations exist only inside the
** mirror arrays.
*/
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "memwatch.h"
#include "mw_recovery.h"
#include "../task_support/check.h"

/* -------------------------------------------------------------------- */
/* mirror helpers                                                       */
/* -------------------------------------------------------------------- */

#define MIRROR_N 3

static long mirror_check( long count, size_t size, int line )
{
    return (long)( 0xFE0180UL ^ (unsigned long)count
                   ^ (unsigned long)size ^ (unsigned long)line );
}

static void mirror_fact( mwFact *f, size_t i )
{
    mwToken base = (mwToken)( 0x1000UL * ( i + 1 ) );
    f->system_base = base;
    f->system_len = 0x800;
    f->user_base = base + 0x40;
    f->user_len = (size_t)( 100 + 8 * i );
    f->count = 100 + (long) i;
    f->source_token = (mwToken)( 10 + i );
    f->line = (int) i + 1;
    f->state = MW_ENTRY_LIVE;
    f->identity = (mwToken)( i + 1 );
}

/* build a healthy, canonical mirror */
static void mirror_build( mwMirror *m, mwFact *facts, mwObsHead *heads,
                          size_t n )
{
    size_t i;
    memset( m, 0, sizeof( *m ) );
    memset( facts, 0, n * sizeof( *facts ) );
    memset( heads, 0, n * sizeof( *heads ) );
    for( i = 0; i < n; ++i ) {
        mirror_fact( &facts[i], i );
        heads[i].identity = facts[i].identity;
        heads[i].prev = i ? facts[i - 1].identity : MW_TOKEN_NULL;
        heads[i].next = i + 1 < n ? facts[i + 1].identity
                                  : MW_TOKEN_NULL;
        heads[i].count = facts[i].count;
        heads[i].line = facts[i].line;
        heads[i].source_token = facts[i].source_token;
        heads[i].size = facts[i].user_len;
        heads[i].flag = 0;
        heads[i].check = mirror_check( heads[i].count, heads[i].size,
                                       heads[i].line );
    }
    m->facts = facts;
    m->fact_count = n;
    m->heads = heads;
    m->head_count = n;
    m->ends.head = facts[0].identity;
    m->ends.tail = facts[n - 1].identity;
    m->ledger_version = 1;
    m->generation = 1;
}

/* walk the applied mirror chain and require exactly n unique members */
static int mirror_chain_canonical( const mwMirror *m )
{
    size_t seen = 0;
    mwToken cur = m->ends.head;
    mwToken prev = MW_TOKEN_NULL;
    while( cur != MW_TOKEN_NULL ) {
        size_t idx;
        if( cur < 1 || cur > (mwToken)m->head_count ) return 0;
        idx = (size_t)( cur - 1 );
        if( m->heads[idx].prev != prev ) return 0;
        prev = cur;
        cur = m->heads[idx].next;
        if( ++seen > m->head_count ) return 0; /* cycle */
    }
    if( seen != m->head_count ) return 0;
    if( m->ends.tail != (mwToken)m->head_count ) return 0;
    return 1;
}

/* -------------------------------------------------------------------- */
/* ledger group                                                         */
/* -------------------------------------------------------------------- */

static int case_ledger_publish( void )
{
    void *a, *b, *c;
    size_t data_size, guard, i;
    size_t n;

    mwInit();
    mwNoMansLand( MW_NML_NONE );
    a = mwMalloc( 16, "ledger.c", 11 );
    b = mwMalloc( 32, "ledger.c", 12 );
    c = mwMalloc( 48, "ledger.c", 13 );
    TASK_CHECK( a && b && c );
    TASK_CHECK( mwTestLedgerCount() == 3 );

    mwTestLayout( &data_size, &guard );
    TASK_CHECK( data_size > 0 && guard > 0 );
    for( i = 0; i < mwTestLedgerCount(); ++i ) {
        void *sys, *user;
        size_t syslen, req;
        int nml;
        unsigned long count;
        TASK_CHECK( mwTestLedgerInfo( i, &sys, &syslen, &user, &req,
                                      &nml, &count ) );
        TASK_CHECK( (char*)user ==
                    (char*)sys + (long)data_size + (long)guard );
        TASK_CHECK( !nml && count > 0 );
        TASK_CHECK( ( req == 16 || req == 32 || req == 48 ) );
        TASK_CHECK( syslen == data_size + 2 * guard + req );
    }

    mwFree( b, "ledger.c", 14 );
    n = mwTestLedgerCount();
    TASK_CHECK( n == 2 );
    TASK_CHECK( mwTestBuffer( "ledger.c", 15, b ) != 0 );
    TASK_CHECK( mwTestBuffer( "ledger.c", 16, a ) == 0 );

    mwFree( a, "ledger.c", 17 );
    mwFree( c, "ledger.c", 18 );
    TASK_CHECK( mwTestLedgerCount() == 0 );
    mwTerm();
    return 0;
}

static int case_ledger_realloc_identity( void )
{
    unsigned char *p, *q;
    size_t i;

    mwInit();
    p = (unsigned char*) mwMalloc( 24, "ledger.c", 21 );
    TASK_CHECK( p != NULL );
    for( i = 0; i < 24; ++i ) p[i] = (unsigned char)( i + 1 );

    q = (unsigned char*) mwRealloc( p, 80, "ledger.c", 22 );
    TASK_CHECK( q != NULL );
    /* the old identity is retired, the new one published */
    TASK_CHECK( mwTestLedgerCount() == 1 );
    TASK_CHECK( mwTestBuffer( "ledger.c", 23, p ) != 0 );
    TASK_CHECK( mwTestBuffer( "ledger.c", 24, q ) == 0 );
    for( i = 0; i < 24; ++i )
        TASK_CHECK( q[i] == (unsigned char)( i + 1 ) );

    /* a shrinking realloc keeps the same prefix and a single record */
    p = (unsigned char*) mwRealloc( q, 9, "ledger.c", 25 );
    TASK_CHECK( p != NULL );
    TASK_CHECK( mwTestLedgerCount() == 1 );
    for( i = 0; i < 9; ++i )
        TASK_CHECK( p[i] == (unsigned char)( i + 1 ) );

    /* a limit-driven realloc failure retains the old live block */
    mwLimit( 1 );
    TASK_CHECK( mwRealloc( p, 4096, "ledger.c", 27 ) == NULL );
    mwLimit( 0 );
    TASK_CHECK( mwTestLedgerCount() == 1 );
    TASK_CHECK( mwTestBuffer( "ledger.c", 28, p ) == 0 );
    for( i = 0; i < 9; ++i )
        TASK_CHECK( p[i] == (unsigned char)( i + 1 ) );

    mwFree( p, "ledger.c", 26 );
    TASK_CHECK( mwTestLedgerCount() == 0 );
    mwTerm();
    return 0;
}

static int case_ledger_nml_lifecycle( void )
{
    void *p;
    size_t i;
    int found_nml = 0;

    mwInit();
    mwNoMansLand( MW_NML_NONE );
    p = mwMalloc( 40, "ledger.c", 31 );
    TASK_CHECK( p != NULL );

    mwNoMansLand( MW_NML_FREE );
    mwTestResetReclaimedNML();
    mwFree( p, "ledger.c", 32 );

    /* NML blocks remain registered until really returned */
    TASK_CHECK( mwTestLedgerCount() == 1 );
    for( i = 0; i < mwTestLedgerCount(); ++i ) {
        int nml;
        TASK_CHECK( mwTestLedgerInfo( i, NULL, NULL, NULL, NULL,
                                      &nml, NULL ) );
        if( nml ) found_nml = 1;
    }
    TASK_CHECK( found_nml );
    TASK_CHECK( mwTestBuffer( "ledger.c", 33, p ) == 0 );

    /* the production pressure-reclaim seam performs the real eviction */
    TASK_CHECK( mwTestForceReclaim() >= 1 );
    TASK_CHECK( mwTestReclaimedNMLCount() >= 1 );
    TASK_CHECK( mwTestLedgerCount() == 0 );
    TASK_CHECK( mwTestBuffer( "ledger.c", 34, p ) != 0 );

    mwNoMansLand( MW_NML_NONE );
    mwTerm();
    return 0;
}

static int case_ledger_prep_failure( void )
{
    void *p;

    mwInit();
    p = mwMalloc( 12, "ledger.c", 41 );
    TASK_CHECK( p != NULL );
    TASK_CHECK( mwTestLedgerCount() == 1 );

    /* deterministic ledger reservation failure: no half-registered
    ** object may be delivered, and no NML reclaim is triggered */
    mwTestResetReclaimedNML();
    mwTestSetPrepFailure( 1 );
    TASK_CHECK( mwMalloc( 12, "ledger.c", 42 ) == NULL );
    TASK_CHECK( mwTestLedgerCount() == 1 );
    TASK_CHECK( mwTestReclaimedNMLCount() == 0 );
    mwTestSetPrepFailure( 0 );

    TASK_CHECK( mwMalloc( 12, "ledger.c", 43 ) != NULL );
    TASK_CHECK( mwTestLedgerCount() == 2 );

    mwFree( p, "ledger.c", 44 );
    mwTerm();
    return 0;
}

static int case_ledger_unknown_address( void )
{
    void *wild = (void*) (uintptr_t) 0x12340000;

    mwInit();
    /* unknown addresses never cause reads in front of the pointer */
    TASK_CHECK( mwTestBuffer( "ledger.c", 51, wild ) != 0 );
    mwFree( wild, "ledger.c", 52 );
    TASK_CHECK( mwRealloc( wild, 8, "ledger.c", 53 ) == NULL );
    TASK_CHECK( mwTestLedgerCount() == 0 );
    mwTerm();
    return 0;
}

/* -------------------------------------------------------------------- */
/* repair group (mirror only)                                           */
/* -------------------------------------------------------------------- */

static int case_repair_preview( void )
{
    mwFact facts[MIRROR_N];
    mwObsHead heads[MIRROR_N];
    mwMirror m;
    mwPlan *plan = NULL;
    size_t i;
    unsigned masks = 0;

    mirror_build( &m, facts, heads, MIRROR_N );
    /* damage several observed fields, without touching any fact */
    heads[0].next = MW_TOKEN_NULL;          /* broken head link */
    heads[1].size = 999;                    /* trashed size */
    heads[2].line = 77;                     /* trashed line */
    heads[2].check = 0xdeadbeefL;           /* trashed checksum */
    m.ends.tail = MW_TOKEN_NULL;            /* bad tail */

    TASK_CHECK( mwMirrorPrepare( &m, &plan ) == MW_RC_OK );
    TASK_CHECK( plan != NULL );
    TASK_CHECK( mwPlanItemCount( plan ) == MIRROR_N );
    TASK_CHECK( mwPlanDiscrepancyCount( plan ) >= 3 );
    TASK_CHECK( mwPlanChainFlags( plan ) != 0 );
    TASK_CHECK( mwPlanObservedEnds( plan )->tail == MW_TOKEN_NULL );
    TASK_CHECK( mwPlanWantedEnds( plan )->head == 1 );
    TASK_CHECK( mwPlanWantedEnds( plan )->tail == MIRROR_N );

    for( i = 0; i < mwPlanItemCount( plan ); ++i ) {
        const mwPlanItem *it = mwPlanItemAt( plan, i );
        TASK_CHECK( it != NULL );
        TASK_CHECK( it->identity == (mwToken)( i + 1 ) );
        TASK_CHECK( it->expected_check ==
                    mirror_check( it->count, it->size, it->line ) );
        masks |= it->diffmask;
    }
    TASK_CHECK( masks & MW_DIFF_SIZE );
    TASK_CHECK( masks & MW_DIFF_LINE );
    TASK_CHECK( masks & MW_DIFF_CHECK );
    TASK_CHECK( masks & MW_DIFF_NEXT );

    /* prepare is side-effect free */
    TASK_CHECK( heads[1].size == 999 );
    mwPlanRelease( plan );
    return 0;
}

static int case_repair_commit( void )
{
    mwFact facts[MIRROR_N];
    mwObsHead heads[MIRROR_N];
    mwMirror m;
    mwPlan *plan = NULL, *again = NULL;
    size_t i;

    mirror_build( &m, facts, heads, MIRROR_N );
    heads[0].next = MW_TOKEN_NULL;
    heads[1].size = 999;
    heads[1].check = 0x42;
    heads[2].prev = MW_TOKEN_NULL;
    m.ends.head = 3;
    m.ends.tail = 1;

    TASK_CHECK( mwMirrorPrepare( &m, &plan ) == MW_RC_OK );
    TASK_CHECK( mwMirrorCommit( &m, plan ) == MW_RC_OK );
    TASK_CHECK( mirror_chain_canonical( &m ) );

    for( i = 0; i < MIRROR_N; ++i ) {
        TASK_CHECK( heads[i].size == facts[i].user_len );
        TASK_CHECK( heads[i].count == facts[i].count );
        TASK_CHECK( heads[i].line == facts[i].line );
        TASK_CHECK( heads[i].source_token == facts[i].source_token );
        TASK_CHECK( heads[i].check ==
                    mirror_check( facts[i].count, facts[i].user_len,
                                  facts[i].line ) );
    }

    /* a fresh plan over the repaired mirror is discrepancy-free, and
    ** the already applied plan is now stale */
    TASK_CHECK( mwMirrorCommit( &m, plan ) == MW_RC_STALE );
    TASK_CHECK( mwMirrorPrepare( &m, &again ) == MW_RC_OK );
    TASK_CHECK( mwPlanDiscrepancyCount( again ) == 0 );
    TASK_CHECK( mwPlanChainFlags( again ) == 0 );
    mwPlanRelease( plan );
    mwPlanRelease( again );
    return 0;
}

static int case_repair_stale_observation( void )
{
    mwFact facts[MIRROR_N];
    mwObsHead heads[MIRROR_N];
    mwObsHead snapshot[MIRROR_N];
    mwMirror m;
    mwPlan *plan = NULL;

    mirror_build( &m, facts, heads, MIRROR_N );
    TASK_CHECK( mwMirrorPrepare( &m, &plan ) == MW_RC_OK );

    /* an observed field moves after the snapshot was taken */
    heads[1].line = 222;
    memcpy( snapshot, heads, sizeof( snapshot ) );
    TASK_CHECK( mwMirrorCommit( &m, plan ) == MW_RC_STALE );

    /* second-stage failure applies no writes at all */
    TASK_CHECK( memcmp( snapshot, heads, sizeof( heads ) ) == 0 );
    TASK_CHECK( m.ends.head == 1 && m.ends.tail == MIRROR_N );
    mwPlanRelease( plan );
    return 0;
}

static int case_repair_stale_generation( void )
{
    mwFact facts[MIRROR_N];
    mwObsHead heads[MIRROR_N];
    mwObsHead snapshot[MIRROR_N];
    mwMirror m;
    mwPlan *plan = NULL;

    mirror_build( &m, facts, heads, MIRROR_N );
    TASK_CHECK( mwMirrorPrepare( &m, &plan ) == MW_RC_OK );
    memcpy( snapshot, heads, sizeof( snapshot ) );

    /* a simulated structural heap change bumps ledger and generation */
    mwMirrorTouch( &m );
    TASK_CHECK( mwMirrorCommit( &m, plan ) == MW_RC_STALE );
    TASK_CHECK( memcmp( snapshot, heads, sizeof( heads ) ) == 0 );
    mwPlanRelease( plan );
    return 0;
}

static int case_repair_invalid_descriptors( void )
{
    mwFact facts[MIRROR_N];
    mwObsHead heads[MIRROR_N];
    mwMirror m;

    mirror_build( &m, facts, heads, MIRROR_N );
    facts[1].identity = facts[0].identity; /* duplicate identity */
    TASK_CHECK( mwMirrorValidate( &m ) == MW_RC_INVALID );
    {
        mwPlan *bad = NULL;
        TASK_CHECK( mwMirrorPrepare( &m, &bad ) == MW_RC_INVALID );
        TASK_CHECK( bad == NULL ); /* no half-published plan */
    }

    mirror_build( &m, facts, heads, MIRROR_N );
    facts[1].system_base = facts[0].system_base + 0x10; /* overlap */
    TASK_CHECK( mwMirrorValidate( &m ) == MW_RC_INVALID );

    mirror_build( &m, facts, heads, MIRROR_N );
    facts[2].system_base = (mwToken) UINTPTR_MAX - 1;
    facts[2].system_len = 64; /* extent wraps */
    TASK_CHECK( mwMirrorValidate( &m ) == MW_RC_INVALID );
    return 0;
}

static int case_repair_cycle_and_duplicates( void )
{
    mwFact facts[MIRROR_N];
    mwObsHead heads[MIRROR_N];
    mwMirror m;
    mwPlan *plan = NULL;

    mirror_build( &m, facts, heads, MIRROR_N );
    heads[1].next = 2;            /* member 2 has two predecessors */
    heads[2].next = 2;            /* self cycle at the tail */
    m.ends.tail = 2;

    TASK_CHECK( mwMirrorPrepare( &m, &plan ) == MW_RC_OK );
    TASK_CHECK( mwPlanChainFlags( plan ) &
                ( MW_CHAIN_CYCLE | MW_CHAIN_DUPLICATE_LINK |
                  MW_CHAIN_UNREACHABLE ) );
    TASK_CHECK( mwPlanItemCount( plan ) == MIRROR_N ); /* none dropped */

    TASK_CHECK( mwMirrorCommit( &m, plan ) == MW_RC_OK );
    TASK_CHECK( mirror_chain_canonical( &m ) );
    mwPlanRelease( plan );
    return 0;
}

static int case_repair_missing_header( void )
{
    mwFact facts[MIRROR_N];
    mwObsHead heads[MIRROR_N];
    mwMirror m;
    mwPlan *plan = NULL, *again = NULL;
    size_t i;
    unsigned missing = 0;

    mirror_build( &m, facts, heads, MIRROR_N );
    m.blocked = MIRROR_N; /* last observed header is unreadable */

    TASK_CHECK( mwMirrorPrepare( &m, &plan ) == MW_RC_OK );
    TASK_CHECK( mwPlanItemCount( plan ) == MIRROR_N );
    for( i = 0; i < mwPlanItemCount( plan ); ++i )
        if( mwPlanItemAt( plan, i )->diffmask & MW_DIFF_MISSING )
            missing ++;
    TASK_CHECK( missing == 1 );

    /* while the header stays unreadable, the second stage refuses and
    ** applies nothing */
    TASK_CHECK( mwMirrorCommit( &m, plan ) == MW_RC_STALE );
    mwPlanRelease( plan );

    /* once readable again, a fresh plan repairs the whole set */
    m.blocked = 0;
    heads[MIRROR_N - 1].next = MW_TOKEN_NULL; /* still logically broken */
    m.ends.tail = MIRROR_N - 1;
    TASK_CHECK( mwMirrorPrepare( &m, &again ) == MW_RC_OK );
    TASK_CHECK( mwMirrorCommit( &m, again ) == MW_RC_OK );
    TASK_CHECK( mirror_chain_canonical( &m ) );
    mwPlanRelease( again );
    return 0;
}

/* -------------------------------------------------------------------- */
/* lifecycle group                                                      */
/* -------------------------------------------------------------------- */

static int case_lifecycle_generation_isolation( void )
{
    void *p;
    mwPlan *plan = NULL;

    mwInit();
    p = mwMalloc( 20, "life.c", 61 );
    TASK_CHECK( p != NULL );
    {
        unsigned epoch = mwTestHeapEpoch();
        unsigned ver = mwTestLedgerVersion();
        TASK_CHECK( mwHeapPrepare( &plan ) == MW_HEAP_OK );
        TASK_CHECK( plan != NULL );
        TASK_CHECK( mwPlanItemCount( plan ) == 1 );
        TASK_CHECK( mwPlanGeneration( plan ) == ver );
        TASK_CHECK( mwPlanLedgerVersion( plan ) == ver );

        /* terminate and start a new observation epoch; the new
        ** mwInit() advances the epoch and invalidates the old plan */
        mwTerm();
        mwInit();
        TASK_CHECK( mwTestHeapEpoch() == epoch + 1 );
        TASK_CHECK( mwTestLedgerCount() == 0 );
    }

    /* the old plan cannot touch the old addresses anymore */
    TASK_CHECK( mwHeapCommit( plan ) == MW_HEAP_STALE );
    /* it remains readable for preview and can always be released */
    TASK_CHECK( mwPlanItemCount( plan ) == 1 );
    TASK_CHECK( mwHeapPlanFree( plan ) == MW_HEAP_OK );
    mwTerm();
    return 0;
}

static int case_lifecycle_nested_term( void )
{
    void *p, *q;

    mwInit();
    mwInit(); /* nested observation layer */
    TASK_CHECK( mwTestInited() == 2 );
    p = mwMalloc( 10, "life.c", 71 );
    TASK_CHECK( p != NULL );
    TASK_CHECK( mwTestLedgerCount() == 1 );

    mwTerm(); /* middle layer: the ledger must survive */
    TASK_CHECK( mwTestInited() == 1 );
    TASK_CHECK( mwTestLedgerCount() == 1 );
    q = mwMalloc( 12, "life.c", 72 );
    TASK_CHECK( q != NULL );
    TASK_CHECK( mwTestLedgerCount() == 2 );

    mwFree( q, "life.c", 73 );
    mwTerm(); /* topmost layer drains everything from the ledger */
    return 0;
}

static int case_lifecycle_degraded_gate( void )
{
    void *p, *q;

    mwInit();
    p = mwMalloc( 16, "life.c", 81 );
    TASK_CHECK( p != NULL );

    /* enter the degraded state through the test seam: no real header
    ** is modified */
    mwTestMarkDegraded();
    TASK_CHECK( mwTestIsDegraded() );
    TASK_CHECK( mwMalloc( 8, "life.c", 82 ) == NULL );
    TASK_CHECK( mwRealloc( p, 32, "life.c", 83 ) == NULL );
    mwFree( p, "life.c", 84 );
    TASK_CHECK( mwTestLedgerCount() == 1 ); /* free was refused */

    /* explicit rebuild restores the entry points */
    {
        mwPlan *plan = NULL;
        TASK_CHECK( mwHeapPrepare( &plan ) == MW_HEAP_OK );
        TASK_CHECK( mwPlanDiscrepancyCount( plan ) == 0 );
        TASK_CHECK( mwHeapCommit( plan ) == MW_HEAP_OK );
        mwHeapPlanFree( plan );
    }
    TASK_CHECK( !mwTestIsDegraded() );
    q = mwMalloc( 8, "life.c", 85 );
    TASK_CHECK( q != NULL );
    mwFree( p, "life.c", 86 );
    mwFree( q, "life.c", 87 );
    TASK_CHECK( mwTestLedgerCount() == 0 );
    mwTerm();
    return 0;
}

static int case_lifecycle_teardown_ledger_only( void )
{
    void *a, *b;

    mwInit();
    mwNoMansLand( MW_NML_NONE );
    a = mwMalloc( 16, "life.c", 91 );
    b = mwMalloc( 24, "life.c", 92 );
    TASK_CHECK( a && b );
    mwNoMansLand( MW_NML_FREE );
    mwFree( b, "life.c", 93 ); /* one LIVE, one NML at teardown */
    TASK_CHECK( mwTestLedgerCount() == 2 );

    /* even while degraded, teardown frees every raw allocation once */
    mwTestMarkDegraded();
    mwNoMansLand( MW_NML_NONE );
    mwTerm();

    mwInit();
    TASK_CHECK( mwTestLedgerCount() == 0 );
    TASK_CHECK( !mwTestIsDegraded() );
    mwTerm();
    return 0;
}

static int case_lifecycle_healthy_commit_inert( void )
{
    unsigned char *p;
    size_t data_size, guard;
    unsigned char before_pre[64], after_pre[64];
    unsigned char before_post[64], after_post[64];
    unsigned char payload[32];
    mwPlan *plan = NULL;

    mwInit();
    p = (unsigned char*) mwMalloc( 32, "life.c", 101 );
    TASK_CHECK( p != NULL );
    memset( p, 0xA5, 32 );
    memcpy( payload, p, 32 );
    mwTestLayout( &data_size, &guard );
    TASK_CHECK( guard <= sizeof( before_pre ) );
    memcpy( before_pre, (char*)p - guard, guard );
    memcpy( before_post, p + 32, guard );

    TASK_CHECK( mwHeapPrepare( &plan ) == MW_HEAP_OK );
    TASK_CHECK( mwPlanDiscrepancyCount( plan ) == 0 );
    TASK_CHECK( mwPlanChainFlags( plan ) == 0 );

    /* pure queries, checks and heap-neutral failures do not age a
    ** prepared plan */
    TASK_CHECK( mwTest( "life.c", 102, MW_TEST_ALL ) == 0 );
    TASK_CHECK( mwTestBuffer( "life.c", 102, p ) == 0 );
    mwLimit( 1 );
    TASK_CHECK( mwMalloc( 4096, "life.c", 102 ) == NULL );
    mwLimit( 0 );

    TASK_CHECK( mwHeapCommit( plan ) == MW_HEAP_OK );

    /* user area and both guard zones are byte-for-byte unchanged */
    TASK_CHECK( memcmp( payload, p, 32 ) == 0 );
    memcpy( after_pre, (char*)p - guard, guard );
    memcpy( after_post, p + 32, guard );
    TASK_CHECK( memcmp( before_pre, after_pre, guard ) == 0 );
    TASK_CHECK( memcmp( before_post, after_post, guard ) == 0 );
    TASK_CHECK( mwTest( "life.c", 102, MW_TEST_ALL ) == 0 );

    mwHeapPlanFree( plan );
    mwFree( p, "life.c", 103 );
    mwTerm();
    return 0;
}

/* -------------------------------------------------------------------- */
/* registration                                                         */
/* -------------------------------------------------------------------- */

const struct task_case task_cases[] = {
    { "ledger", "publish",            case_ledger_publish },
    { "ledger", "realloc_identity",   case_ledger_realloc_identity },
    { "ledger", "nml_lifecycle",      case_ledger_nml_lifecycle },
    { "ledger", "prep_failure",       case_ledger_prep_failure },
    { "ledger", "unknown_address",    case_ledger_unknown_address },

    { "repair", "preview",            case_repair_preview },
    { "repair", "commit",             case_repair_commit },
    { "repair", "stale_observation",  case_repair_stale_observation },
    { "repair", "stale_generation",   case_repair_stale_generation },
    { "repair", "invalid_descriptors",case_repair_invalid_descriptors },
    { "repair", "cycle_duplicates",   case_repair_cycle_and_duplicates },
    { "repair", "missing_header",     case_repair_missing_header },

    { "lifecycle", "generation_isolation", case_lifecycle_generation_isolation },
    { "lifecycle", "nested_term",          case_lifecycle_nested_term },
    { "lifecycle", "degraded_gate",        case_lifecycle_degraded_gate },
    { "lifecycle", "teardown_ledger_only", case_lifecycle_teardown_ledger_only },
    { "lifecycle", "healthy_commit_inert", case_lifecycle_healthy_commit_inert },
};

const size_t task_case_count =
    sizeof( task_cases ) / sizeof( task_cases[0] );
