/*
** MEMWATCH.C
** Nonintrusive ANSI C memory leak / overwrite detection
** Copyright (C) 1992-2003 Johan Lindh
** All rights reserved.
** Version 2.71

	This file is part of MEMWATCH.

    MEMWATCH is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    MEMWATCH is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with MEMWATCH; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

**
** 920810 JLI   [1.00]
** 920830 JLI   [1.10 double-free detection]
** 920912 JLI   [1.15 mwPuts, mwGrab/Drop, mwLimit]
** 921022 JLI   [1.20 ASSERT and VERIFY]
** 921105 JLI   [1.30 C++ support and TRACE]
** 921116 JLI   [1.40 mwSetOutFunc]
** 930215 JLI   [1.50 modified ASSERT/VERIFY]
** 930327 JLI   [1.51 better auto-init & PC-lint support]
** 930506 JLI   [1.55 MemWatch class, improved C++ support]
** 930507 JLI   [1.60 mwTest & CHECK()]
** 930809 JLI   [1.65 Abort/Retry/Ignore]
** 930820 JLI   [1.70 data dump when unfreed]
** 931016 JLI   [1.72 modified C++ new/delete handling]
** 931108 JLI   [1.77 mwSetAssertAction() & some small changes]
** 940110 JLI   [1.80 no-mans-land alloc/checking]
** 940328 JLI   [2.00 version 2.0 rewrite]
**              Improved NML (no-mans-land) support.
**              Improved performance (especially for free()ing!).
**              Support for 'read-only' buffers (checksums)
**              ^^ NOTE: I never did this... maybe I should?
**              FBI (free'd block info) tagged before freed blocks
**              Exporting of the mwCounter variable
**              mwBreakOut() localizes debugger support
**              Allocation statistics (global, per-module, per-line)
**              Self-repair ability with relinking
** 950913 JLI   [2.10 improved garbage handling]
** 951201 JLI   [2.11 improved auto-free in emergencies]
** 960125 JLI   [X.01 implemented auto-checking using mwAutoCheck()]
** 960514 JLI   [2.12 undefining of existing macros]
** 960515 JLI   [2.13 possibility to use default new() & delete()]
** 960516 JLI   [2.20 suppression of file flushing on unfreed msgs]
** 960516 JLI   [2.21 better support for using MEMWATCH with DLL's]
** 960710 JLI   [X.02 multiple logs and mwFlushNow()]
** 960801 JLI   [2.22 merged X.01 version with current]
** 960805 JLI   [2.30 mwIsXXXXAddr() to avoid unneeded GP's]
** 960805 JLI   [2.31 merged X.02 version with current]
** 961002 JLI   [2.32 support for realloc() + fixed STDERR bug]
** 961222 JLI   [2.40 added mwMark() & mwUnmark()]
** 970101 JLI   [2.41 added over/underflow checking after failed ASSERT/VERIFY]
** 970113 JLI   [2.42 added support for PC-Lint 7.00g]
** 970207 JLI   [2.43 added support for strdup()]
** 970209 JLI   [2.44 changed default filename to lowercase]
** 970405 JLI   [2.45 fixed bug related with atexit() and some C++ compilers]
** 970723 JLI   [2.46 added MW_ARI_NULLREAD flag]
** 970813 JLI   [2.47 stabilized marker handling]
** 980317 JLI   [2.48 ripped out C++ support; wasn't working good anyway]
** 980318 JLI   [2.50 improved self-repair facilities & SIGSEGV support]
** 980417 JLI	[2.51 more checks for invalid addresses]
** 980512 JLI	[2.52 moved MW_ARI_NULLREAD to occur before aborting]
** 990112 JLI	[2.53 added check for empty heap to mwIsOwned]
** 990217 JLI	[2.55 improved the emergency repairs diagnostics and NML]
** 990224 JLI	[2.56 changed ordering of members in structures]
** 990303 JLI	[2.57 first maybe-fixit-for-hpux test]
** 990516 JLI	[2.58 added 'static' to the definition of mwAutoInit]
** 990517 JLI	[2.59 fixed some high-sensitivity warnings]
** 990610 JLI	[2.60 fixed some more high-sensitivity warnings]
** 990715 JLI	[2.61 changed TRACE/ASSERT/VERIFY macro names]
** 991001 JLI	[2.62 added CHECK_BUFFER() and mwTestBuffer()]
** 991007 JLI	[2.63 first shot at a 64-bit compatible version]
** 991009 JLI	[2.64 undef's strdup() if defined, mwStrdup made const]
** 000704 JLI	[2.65 added some more detection for 64-bits]
** 010502 JLI   [2.66 incorporated some user fixes]
**              [mwRelink() could print out garbage pointer (thanks mac@phobos.ca)]
**				[added array destructor for C++ (thanks rdasilva@connecttel.com)]
**				[added mutex support (thanks rdasilva@connecttel.com)]
** 010531 JLI	[2.67 fix: mwMutexXXX() was declared even if MW_HAVE_MUTEX was not defined]
** 010619 JLI	[2.68 fix: mwRealloc() could leave the mutex locked]
** 020918 JLI	[2.69 changed to GPL, added C++ array allocation by Howard Cohen]
** 030212 JLI	[2.70 mwMalloc() bug for very large allocations (4GB on 32bits)]
** 030520 JLI	[2.71 added ULONG_LONG_MAX as a 64-bit detector (thanks Sami Salonen)]
*/

#define __MEMWATCH_C 1

#ifdef MW_NOCPP
#define MEMWATCH_NOCPP
#endif
#ifdef MW_STDIO
#define MEMWATCH_STDIO
#endif

/***********************************************************************
** Include files
***********************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include <time.h>
#include <limits.h>
#include <stdint.h>
#include "memwatch.h"
#include "mw_recovery.h"

#ifndef toupper
#include <ctype.h>
#endif

#if defined(WIN32) || defined(__WIN32__)
#define MW_HAVE_MUTEX 1
#include <windows.h>
#endif

#if defined(MW_PTHREADS) || defined(HAVE_PTHREAD_H)
#define MW_HAVE_MUTEX 1
#include <pthread.h>
#endif

/***********************************************************************
** Defines & other weird stuff
***********************************************************************/

/*lint -save -e767 */
#define VERSION     "2.71"         /* the current version number */
#define CHKVAL(mw)  (0xFE0180L^(long)mw->count^(long)mw->size^(long)mw->line)
#define FLUSH()     mwFlush()
#define TESTS(f,l)  if(mwTestAlways) (void)mwTestNow(f,l,1)
#define PRECHK      0x01234567L
#define POSTCHK     0x76543210L
#define mwBUFFER_TO_MW(p) ( (mwData*) (void*) ( ((char*)p)-mwDataSize-mwOverflowZoneSize ) )
/*lint -restore */

#define MW_NML      0x0001

#ifdef _MSC_VER
#define COMMIT "c"  /* Microsoft C requires the 'c' to perform as desired */
#else
#define COMMIT ""   /* Normal ANSI */
#endif /* _MSC_VER */

#ifdef __cplusplus
#define CPPTEXT "++"
#else
#define CPPTEXT ""
#endif /* __cplusplus */

#ifdef MEMWATCH_STDIO
#define mwSTDERR stderr
#else
#define mwSTDERR mwLog
#endif

#ifdef MW_HAVE_MUTEX
#define MW_MUTEX_INIT()		mwMutexInit()
#define MW_MUTEX_TERM()		mwMutexTerm()
#define MW_MUTEX_LOCK()		mwMutexLock()
#define MW_MUTEX_UNLOCK()	mwMutexUnlock()
#else
#define MW_MUTEX_INIT()
#define MW_MUTEX_TERM()
#define MW_MUTEX_LOCK()
#define MW_MUTEX_UNLOCK()
#endif

/***********************************************************************
** If you really, really know what you're doing,
** you can predefine these things yourself.
***********************************************************************/

#ifndef mwBYTE_DEFINED
# if CHAR_BIT != 8
#  error need CHAR_BIT to be 8!
# else
typedef unsigned char mwBYTE;
#  define mwBYTE_DEFINED 1
# endif
#endif

#if defined(ULONGLONG_MAX) || defined(ULLONG_MAX) || defined(_UI64_MAX) || defined(ULONG_LONG_MAX)
# define mw64BIT 1
# define mwROUNDALLOC_DEFAULT 8
#else
# if UINT_MAX <= 0xFFFFUL
#  define mw16BIT 1
#  define mwROUNDALLOC_DEFAULT	2
# else
#  if ULONG_MAX > 0xFFFFFFFFUL
#   define mw64BIT 1
#   define mwROUNDALLOC_DEFAULT	8
#  else
#   define mw32BIT 1
#   define mwROUNDALLOC_DEFAULT	4
#  endif
# endif
#endif

/* mwROUNDALLOC is the number of bytes to */
/* round up to, to ensure that the end of */
/* the buffer is suitable for storage of */
/* any kind of object */
#ifndef mwROUNDALLOC
# define mwROUNDALLOC mwROUNDALLOC_DEFAULT
#endif

#ifndef mwDWORD_DEFINED
#if ULONG_MAX == 0xFFFFFFFFUL
typedef unsigned long mwDWORD;
#define mwDWORD_DEFINED "unsigned long"
#endif
#endif

#ifndef mwDWORD_DEFINED
#if UINT_MAX == 0xFFFFFFFFUL
typedef unsigned int mwDWORD;
#define mwDWORD_DEFINED "unsigned int"
#endif
#endif

#ifndef mwDWORD_DEFINED
#if USHRT_MAX == 0xFFFFFFFFUL
typedef unsigned short mwDWORD;
#define mwDWORD_DEFINED "unsigned short"
#endif
#endif

#ifndef mwBYTE_DEFINED
#error "can't find out the correct type for a 8 bit scalar"
#endif

#ifndef mwDWORD_DEFINED
#error "can't find out the correct type for a 32 bit scalar"
#endif

/***********************************************************************
** Typedefs & structures
***********************************************************************/

/* main data holding area, precedes actual allocation */
typedef struct mwData_ mwData;
struct mwData_ {
    mwData*     prev;   /* previous allocation in chain */
    mwData*     next;   /* next allocation in chain */
    const char* file;   /* file name where allocated */
    long        count;  /* action count */
    long        check;  /* integrity check value */
#if 0
    long        crc;    /* data crc value */
#endif
    size_t      size;   /* size of allocation */
    int         line;   /* line number where allocated */
    unsigned    flag;   /* flag word */
    };

/* statistics structure */
typedef struct mwStat_ mwStat;
struct mwStat_ {
    mwStat*     next;   /* next statistic buffer */
    const char* file;
    long        total;  /* total bytes allocated */
    long        num;    /* total number of allocations */
    long        max;    /* max allocated at one time */
    long        curr;   /* current allocations */
    int         line;
    };

/* grabbing structure, 1K in size */
typedef struct mwGrabData_ mwGrabData;
struct mwGrabData_ {
    mwGrabData* next;
    int         type;
    char        blob[ 1024 - sizeof(mwGrabData*) - sizeof(int) ];
    };

typedef struct mwMarker_ mwMarker;
struct mwMarker_ {
    void *host;
    char *text;
    mwMarker *next;
    int level;
    };

#if defined(WIN32) || defined(__WIN32__)
typedef HANDLE          mwMutex;
#endif

#if defined(MW_PTHREADS) || defined(HAVE_PTHREAD_H)
typedef pthread_mutex_t mwMutex;
#endif

/***********************************************************************
** Static variables
***********************************************************************/

static int      mwInited =      0;
static int      mwInfoWritten = 0;
static int      mwUseAtexit =   0;
static FILE*    mwLog =         NULL;
static int      mwFlushing =    0;
static int      mwStatLevel =   MW_STAT_DEFAULT;
static int      mwNML =         MW_NML_DEFAULT;
static int      mwFBI =         0;
static long     mwAllocLimit =  0L;
static int      mwUseLimit =    0;

static long     mwNumCurAlloc = 0L;
static mwData*  mwHead = 		NULL;
static mwData*  mwTail = 		NULL;
static int		mwDataSize =	0;
static unsigned char mwOverflowZoneTemplate[] = "mEmwAtch";
static int		mwOverflowZoneSize = mwROUNDALLOC;

static void     (*mwOutFunction)(int) = NULL;
static int      (*mwAriFunction)(const char*) = NULL;
static int      mwAriAction = MW_ARI_ABORT;

static char     mwPrintBuf[MW_TRACE_BUFFER+8];

static unsigned long mwCounter = 0L;
static long     mwErrors =      0L;

static int      mwTestFlags =   0;
static int      mwTestAlways =  0;

static FILE*    mwLogB1 =       NULL;
static int      mwFlushingB1 =  0;

static mwStat*  mwStatList = NULL;
static long     mwStatTotAlloc = 0L;
static long     mwStatMaxAlloc = 0L;
static long     mwStatNumAlloc = 0L;
static long     mwStatCurAlloc = 0L;
static long     mwNmlNumAlloc = 0L;
static long     mwNmlCurAlloc = 0L;

static mwGrabData* mwGrabList = NULL;
static long     mwGrabSize = 0L;

static void *   mwLastFree[MW_FREE_LIST];
static const char *mwLFfile[MW_FREE_LIST];
static int      mwLFline[MW_FREE_LIST];
static int      mwLFcur = 0;

static mwMarker* mwFirstMark = NULL;

static FILE*    mwLogB2 =       NULL;
static int      mwFlushingB2 =  0;

#ifdef MW_HAVE_MUTEX
static mwMutex	mwGlobalMutex;
#endif

/***********************************************************************
** Static function declarations
***********************************************************************/

static void     mwAutoInit( void );
static FILE*    mwLogR( void );
static void     mwLogW( FILE* );
static int      mwFlushR( void );
static void     mwFlushW( int );
static void     mwFlush( void );
static void     mwIncErr( void );
static void     mwUnlink( mwData*, const char* file, int line );
static int      mwTestBuf( mwData* mw, const char* file, int line );
static void     mwDefaultOutFunc( int );
static void     mwWrite( const char* format, ... );
static void     mwLogFile( const char* name );
static size_t   mwFreeUp( size_t, int );
static const void *mwTestMem( const void *, unsigned, int );
static int      mwStrCmpI( const char *s1, const char *s2 );
static int      mwTestNow( const char *file, int line, int always_invoked );
static void     mwDropAll( void );
static const char *mwGrabType( int type );
static unsigned mwGrab_( unsigned kb, int type, int silent );
static unsigned mwDrop_( unsigned kb, int type, int silent );
static int      mwARI( const char* text );
static void     mwStatReport( void );
static mwStat*  mwStatGet( const char*, int, int );
static void     mwStatAlloc( size_t, const char*, int );
static void     mwStatFree( size_t, const char*, int );
static int		mwCheckOF( const void * p );
static void		mwWriteOF( void * p );
static char		mwDummy( char c );
#ifdef MW_HAVE_MUTEX
static void		mwMutexInit( void );
static void		mwMutexTerm( void );
static void		mwMutexLock( void );
static void		mwMutexUnlock( void );
#endif

/***********************************************************************
** Trusted ledger
**
** A side table, allocated with the system allocator and never passed
** through mwMalloc(), that records the canonical identity of every
** delivered system allocation.  The management headers (mwData) stay
** diagnostic; the ledger is the only source of truth used by the
** metadata rebuild and the final teardown.
***********************************************************************/

typedef struct mwLedgerEntry_ {
    void          *sys_ptr;   /* raw system allocation address */
    size_t         sys_len;   /* raw system allocation length */
    void          *user_ptr;  /* exact delivered user pointer */
    size_t         req_size;  /* requested user area size */
    unsigned long  count;     /* allocation action count */
    const char    *file;      /* owning source file */
    int            line;      /* owning source line */
    int            nml;       /* nonzero once converted to NML */
} mwLedgerEntry;

static mwLedgerEntry *mwLedger = NULL;
static size_t         mwLedgerCount = 0;
static size_t         mwLedgerCap = 0;
static unsigned       mwLedgerVersion = 0;  /* bumped on any fact change */
static unsigned       mwHeapGeneration = 0; /* same timeline as version */
static unsigned       mwHeapEpoch = 0;      /* bumped at each (re)init */
static int            mwHeapDegraded = 0;   /* header damage detected */

#ifdef MW_HEAP_TESTING
static int            mwTestPrepFailure = 0;
static unsigned long  mwTestReclaimedNML = 0;
#endif

static int  mwLedgerEnsure( size_t need );
static int  mwLedgerAdd( void *sys, size_t syslen, void *user,
                         size_t req, unsigned long count,
                         const char *file, int line );
static int  mwLedgerFindUser( const void *user );
static int  mwLedgerFindSys( const void *sys );
static void mwLedgerRemoveAt( size_t idx );
static void mwLedgerMarkNML( size_t idx );
static void mwLedgerBump( void );
static void mwMarkDegraded( const char *what, const void *id );
static int  mwHeaderAgrees( const mwLedgerEntry *e );
static void mwTrustedGuardCheck( const mwLedgerEntry *e );
static int  mwHeapAdapterRead( void *ctx, mwToken id, mwObsHead *out );
static int  mwHeapAdapterEnds( void *ctx, mwObsEnds *out );
static long mwHeapAdapterCheck( void *ctx, const mwFact *fact,
                                const mwObsHead *wanted );
static int  mwHeapAdapterValid( void *ctx, mwToken token );
static int  mwHeapAdapterApply( void *ctx, const mwObsHead *writes,
                                size_t write_count,
                                const mwObsEnds *ends );

static void mwLedgerBump( void ) {
    mwLedgerVersion ++;
    mwHeapGeneration ++;
}

/* Grow the side table.  Failure is reported, never papered over by
** triggering NML reclamation: metadata storage must not recurse into
** the pressure-reclaim path. */
static int mwLedgerEnsure( size_t need ) {
    mwLedgerEntry *grown;
    size_t newcap;
#ifdef MW_HEAP_TESTING
    if( mwTestPrepFailure ) return 0;
#endif
    if( need <= mwLedgerCap ) return 1;
    newcap = mwLedgerCap ? mwLedgerCap : 16;
    while( newcap < need ) {
        if( newcap > ( (size_t)-1 ) / 2 ) return 0;
        newcap *= 2;
    }
    grown = (mwLedgerEntry*) realloc( mwLedger,
                                      newcap * sizeof(mwLedgerEntry) );
    if( grown == NULL ) return 0;
    mwLedger = grown;
    mwLedgerCap = newcap;
    return 1;
}

static int mwLedgerAdd( void *sys, size_t syslen, void *user,
                        size_t req, unsigned long count,
                        const char *file, int line ) {
    if( !mwLedgerEnsure( mwLedgerCount + 1 ) ) return 0;
    mwLedger[mwLedgerCount].sys_ptr = sys;
    mwLedger[mwLedgerCount].sys_len = syslen;
    mwLedger[mwLedgerCount].user_ptr = user;
    mwLedger[mwLedgerCount].req_size = req;
    mwLedger[mwLedgerCount].count = count;
    mwLedger[mwLedgerCount].file = file;
    mwLedger[mwLedgerCount].line = line;
    mwLedger[mwLedgerCount].nml = 0;
    mwLedgerCount ++;
    mwLedgerBump();
    return 1;
}

/* Ownership is decided by the exact user start address only; an
** unknown address never causes memory in front of it to be read. */
static int mwLedgerFindUser( const void *user ) {
    size_t i;
    for( i = 0; i < mwLedgerCount; ++i )
        if( mwLedger[i].user_ptr == user ) return (int) i;
    return -1;
}

static int mwLedgerFindSys( const void *sys ) {
    size_t i;
    for( i = 0; i < mwLedgerCount; ++i )
        if( mwLedger[i].sys_ptr == sys ) return (int) i;
    return -1;
}

static void mwLedgerRemoveAt( size_t idx ) {
    if( idx >= mwLedgerCount ) return;
    mwLedger[idx] = mwLedger[mwLedgerCount - 1];
    mwLedgerCount --;
    mwLedgerBump();
}

static void mwLedgerMarkNML( size_t idx ) {
    if( idx >= mwLedgerCount ) return;
    if( !mwLedger[idx].nml ) {
        mwLedger[idx].nml = 1;
        mwLedgerBump();
    }
}

static void mwMarkDegraded( const char *what, const void *id ) {
    if( !mwHeapDegraded ) {
        mwHeapDegraded = 1;
        mwWrite( "recovery: heap metadata inconsistent (%s at %p); "
                 "alloc/realloc/free/check refused until mwHeapCommit()\n",
                 what ? what : "header", id );
        mwIncErr();
        FLUSH();
    }
}

/* Read-only comparison of a live header against the ledger truth. */
static int mwHeaderAgrees( const mwLedgerEntry *e ) {
    mwData *mw = (mwData*) e->sys_ptr;
    if( !mwIsSafeAddr( mw, mwDataSize ) ) return 0;
    if( mw->check != CHKVAL(mw) ) return 0;
    if( mw->count != (long) e->count ) return 0;
    if( mw->size != e->req_size ) return 0;
    if( mw->line != e->line ) return 0;
    if( mw->file != e->file ) return 0;
    if( ( mw->flag & MW_NML ) != ( e->nml ? MW_NML : 0 ) ) return 0;
    if( mw->prev ) {
        if( !mwIsSafeAddr( mw->prev, mwDataSize ) ) return 0;
        if( mw->prev->next != mw ) return 0;
    }
    else if( mwHead != mw ) return 0;
    if( mw->next ) {
        if( !mwIsSafeAddr( mw->next, mwDataSize ) ) return 0;
        if( mw->next->prev != mw ) return 0;
    }
    else if( mwTail != mw ) return 0;
    return 1;
}

/* Guard and NML-fill checks positioned entirely from trusted sizes. */
static void mwTrustedGuardCheck( const mwLedgerEntry *e ) {
    char *base = (char*) e->sys_ptr;
    char *pre = base + mwDataSize;
    char *post;
    if( mwCheckOF( pre ) ) {
        mwIncErr();
        mwWrite( "underflow: <%lu> %s(%d), %lu bytes alloc'd at <%lu>\n",
                 e->count, e->file, e->line, (unsigned long)e->req_size,
                 e->count );
    }
    post = pre + mwOverflowZoneSize + e->req_size;
    if( mwIsReadAddr( post, mwOverflowZoneSize ) && mwCheckOF( post ) ) {
        mwIncErr();
        mwWrite( "overflow: <%lu> %s(%d), %lu bytes alloc'd at <%lu>\n",
                 e->count, e->file, e->line, (unsigned long)e->req_size,
                 e->count );
    }
}

/* ---- real-heap adapter: safe field access + write-set application ---- */

static int mwHeapAdapterRead( void *ctx, mwToken id, mwObsHead *out ) {
    mwData *mw;
    (void) ctx;
    mw = (mwData*)(void*) id;
    if( !mwIsSafeAddr( mw, mwDataSize ) ) return 0;
    out->identity = id;
    out->prev = (mwToken)(uintptr_t) mw->prev;
    out->next = (mwToken)(uintptr_t) mw->next;
    out->count = mw->count;
    out->line = mw->line;
    out->flag = mw->flag;
    out->size = mw->size;
    out->check = mw->check;
    /* copied as a token; the file pointer is never probed */
    out->source_token = (mwToken)(uintptr_t) mw->file;
    return 1;
}

static int mwHeapAdapterEnds( void *ctx, mwObsEnds *out ) {
    (void) ctx;
    out->head = (mwToken)(uintptr_t) mwHead;
    out->tail = (mwToken)(uintptr_t) mwTail;
    return 1;
}

static long mwHeapAdapterCheck( void *ctx, const mwFact *fact,
                                const mwObsHead *wanted ) {
    (void) ctx;
    (void) fact;
    return (long)( 0xFE0180UL
                   ^ (unsigned long) wanted->count
                   ^ (unsigned long) wanted->size
                   ^ (unsigned long) wanted->line );
}

static int mwHeapAdapterValid( void *ctx, mwToken token ) {
    (void) ctx;
    return mwLedgerFindSys( (void*)(uintptr_t) token ) >= 0;
}

static int mwHeapAdapterApply( void *ctx, const mwObsHead *writes,
                               size_t write_count,
                               const mwObsEnds *ends ) {
    size_t i;
    (void) ctx;
    /* only mwData fields are written; user areas and guard bytes are
    ** never touched by the rebuild. */
    for( i = 0; i < write_count; ++i ) {
        mwData *mw = (mwData*)(void*) writes[i].identity;
        mw->prev = (mwData*)(void*) writes[i].prev;
        mw->next = (mwData*)(void*) writes[i].next;
        mw->file = (const char*)(void*) writes[i].source_token;
        mw->count = writes[i].count;
        mw->check = writes[i].check;
        mw->size = writes[i].size;
        mw->line = writes[i].line;
        mw->flag = writes[i].flag;
    }
    mwHead = (mwData*)(void*) ends->head;
    mwTail = (mwData*)(void*) ends->tail;
    return 1;
}

static const mwPlanAdapter mwHeapPlanAdapter = {
    mwHeapAdapterRead,
    mwHeapAdapterEnds,
    mwHeapAdapterCheck,
    mwHeapAdapterValid,
    NULL
};

static int mwMapPlanRC( int rc ) {
    switch( rc ) {
        case MW_RC_OK:       return MW_HEAP_OK;
        case MW_RC_STALE:    return MW_HEAP_STALE;
        case MW_RC_NOMEM:    return MW_HEAP_NOMEM;
        case MW_RC_NOTREADY: return MW_HEAP_NOTREADY;
        default:             return MW_HEAP_INVALID;
    }
}

static int mwFactCountDesc( const void *a, const void *b ) {
    const mwFact *fa = (const mwFact*) a;
    const mwFact *fb = (const mwFact*) b;
    if( fa->count > fb->count ) return -1;
    if( fa->count < fb->count ) return 1;
    return 0;
}

int mwHeapPrepare( struct mwPlan **out_plan ) {
    mwFact *facts;
    mwPlanAdapter ad;
    size_t i;
    int rc;

    if( out_plan == NULL ) return MW_HEAP_INVALID;
    *out_plan = NULL;
    mwAutoInit();
    MW_MUTEX_LOCK();

    if( !mwInited ) {
        MW_MUTEX_UNLOCK();
        return MW_HEAP_NOTREADY;
    }

    facts = NULL;
    if( mwLedgerCount ) {
        facts = (mwFact*) malloc( mwLedgerCount * sizeof(mwFact) );
        if( facts == NULL ) {
            MW_MUTEX_UNLOCK();
            return MW_HEAP_NOMEM;
        }
    }

    /* ledger order is newest-first; the canonical chain is rebuilt in
    ** descending allocation-count order. */
    for( i = 0; i < mwLedgerCount; ++i ) {
        const mwLedgerEntry *e = &mwLedger[i];
        facts[i].system_base = (mwToken)(uintptr_t) e->sys_ptr;
        facts[i].system_len = e->sys_len;
        facts[i].user_base = (mwToken)(uintptr_t) e->user_ptr;
        facts[i].user_len = e->req_size;
        facts[i].count = (long) e->count;
        facts[i].source_token = (mwToken)(uintptr_t) e->file;
        facts[i].line = e->line;
        facts[i].state = e->nml ? MW_ENTRY_NML : MW_ENTRY_LIVE;
        facts[i].identity = (mwToken)(uintptr_t) e->sys_ptr;
    }

    /* the canonical chain is newest-first, by descending allocation
    ** count; the ledger's physical array order is irrelevant */
    qsort( facts, mwLedgerCount, sizeof(mwFact), mwFactCountDesc );

    ad = mwHeapPlanAdapter;
    rc = mwPlanBuild( facts, mwLedgerCount, mwLedgerVersion,
                      mwHeapGeneration, &ad, out_plan );
    free( facts );

    if( rc == MW_RC_OK ) {
        mwWrite( "recovery: plan prepared, %lu registered block(s), "
                 "%lu field discrepancy(ies)\n",
                 (unsigned long) mwPlanItemCount( *out_plan ),
                 (unsigned long) mwPlanDiscrepancyCount( *out_plan ) );
        FLUSH();
    }

    MW_MUTEX_UNLOCK();
    return mwMapPlanRC( rc );
}

int mwHeapCommit( struct mwPlan *plan ) {
    mwPlanAdapter ad;
    int rc;

    if( plan == NULL ) return MW_HEAP_INVALID;
    mwAutoInit();
    MW_MUTEX_LOCK();

    if( !mwInited ) {
        MW_MUTEX_UNLOCK();
        return MW_HEAP_NOTREADY;
    }

    ad = mwHeapPlanAdapter;
    rc = mwPlanCommit( plan, &ad, mwHeapGeneration, mwLedgerVersion,
                       mwHeapAdapterApply );
    if( rc == MW_RC_OK ) {
        mwHeapDegraded = 0;
        /* a published rebuild is itself a heap change: all other,
        ** older plans become stale */
        mwLedgerBump();
        mwWrite( "recovery: commit successful, canonical chain restored\n" );
        FLUSH();
    }
    else if( rc == MW_RC_STALE ) {
        mwWrite( "recovery: commit refused, plan is stale\n" );
        FLUSH();
    }

    MW_MUTEX_UNLOCK();
    return mwMapPlanRC( rc );
}

int mwHeapPlanFree( struct mwPlan *plan ) {
    /* releasing a plan touches no observed storage and is allowed even
    ** after the heap that produced it has been terminated */
    mwPlanRelease( plan );
    return MW_HEAP_OK;
}

#ifdef MW_HEAP_TESTING
size_t mwTestLedgerCount( void ) { return mwLedgerCount; }
int mwTestInited( void ) { return mwInited; }
void mwTestLayout( size_t *data_size, size_t *guard_size ) {
    if( data_size ) *data_size = (size_t) mwDataSize;
    if( guard_size ) *guard_size = (size_t) mwOverflowZoneSize;
}
int mwTestIsDegraded( void ) { return mwHeapDegraded; }
unsigned mwTestHeapEpoch( void ) { return mwHeapEpoch; }
unsigned mwTestLedgerVersion( void ) { return mwLedgerVersion; }
void mwTestSetPrepFailure( int onoff ) { mwTestPrepFailure = onoff ? 1 : 0; }
/* raises the degraded state deterministically without touching any
** real management header, guard byte or freed region */
void mwTestMarkDegraded( void ) {
    MW_MUTEX_LOCK();
    mwMarkDegraded( "test seam", NULL );
    MW_MUTEX_UNLOCK();
}
unsigned long mwTestReclaimedNMLCount( void ) { return mwTestReclaimedNML; }
void mwTestResetReclaimedNML( void ) { mwTestReclaimedNML = 0; }
/* deterministic NML pressure-reclaim seam: invokes the production
** reclamation routine directly so tests can drive real evictions */
unsigned long mwTestForceReclaim( void ) {
    size_t before = mwLedgerCount;
    MW_MUTEX_LOCK();
    (void) mwFreeUp( (size_t) -1, 0 );
    MW_MUTEX_UNLOCK();
    return (unsigned long)( before - mwLedgerCount );
}
int mwTestLedgerInfo( size_t ord, void **sys, size_t *syslen,
                      void **user, size_t *req, int *nml,
                      unsigned long *count ) {
    const mwLedgerEntry *e;
    if( ord >= mwLedgerCount ) return 0;
    e = &mwLedger[ord];
    if( sys ) *sys = e->sys_ptr;
    if( syslen ) *syslen = e->sys_len;
    if( user ) *user = e->user_ptr;
    if( req ) *req = e->req_size;
    if( nml ) *nml = e->nml;
    if( count ) *count = e->count;
    return 1;
}
#endif

/***********************************************************************
** System functions
***********************************************************************/

void mwInit( void ) {
    time_t tid;

    if( mwInited++ > 0 ) return;

	MW_MUTEX_INIT();

    /* a fresh observation epoch starts; the ledger (if any) was fully
    ** drained by the matching mwAbort(), and monotonic generation
    ** numbers ensure plans from an older epoch can never recommit */
    mwLedger = NULL;
    mwLedgerCount = mwLedgerCap = 0;
    mwHeapDegraded = 0;
    mwHeapEpoch ++;
    mwLedgerBump();
#ifdef MW_HEAP_TESTING
    mwTestPrepFailure = 0;
    mwTestReclaimedNML = 0;
#endif

    /* start a log if none is running */
    if( mwLogR() == NULL ) mwLogFile( "memwatch.log" );
    if( mwLogR() == NULL ) {
        int i;
        char buf[32];
        /* oops, could not open it! */
        /* probably because it's already open */
        /* so we try some other names */
        for( i=1; i<100; i++ ) {
            sprintf( buf, "memwat%02d.log", i );
            mwLogFile( buf );
            if( mwLogR() != NULL ) break;
            }
        }

    /* initialize the statistics */
    mwStatList = NULL;
    mwStatTotAlloc = 0L;
    mwStatCurAlloc = 0L;
    mwStatMaxAlloc = 0L;
    mwStatNumAlloc = 0L;
	mwNmlCurAlloc = 0L;
	mwNmlNumAlloc = 0L;

	/* calculate the buffer size to use for a mwData */
	mwDataSize = sizeof(mwData);
	while( mwDataSize % mwROUNDALLOC ) mwDataSize ++;

    /* write informational header if needed */
    if( !mwInfoWritten ) {
        mwInfoWritten = 1;
        (void) time( &tid );
        mwWrite(
            "\n============="
            " MEMWATCH " VERSION " Copyright (C) 1992-1999 Johan Lindh "
            "=============\n");
        mwWrite( "\nStarted at %s\n", ctime( &tid ) );

/**************************************************************** Generic */
		mwWrite( "Modes: " );
#ifdef mwNew
        mwWrite( "C++ " );
#endif /* mwNew */
#ifdef __STDC__
        mwWrite( "__STDC__ " );
#endif /* __STDC__ */
#ifdef mw16BIT
		mwWrite( "16-bit " );
#endif
#ifdef mw32BIT
		mwWrite( "32-bit " );
#endif
#ifdef mw64BIT
		mwWrite( "64-bit " );
#endif
		mwWrite( "mwDWORD==(" mwDWORD_DEFINED ")\n" );
		mwWrite( "mwROUNDALLOC==%d sizeof(mwData)==%d mwDataSize==%d\n",
			mwROUNDALLOC, sizeof(mwData), mwDataSize );
/**************************************************************** Generic */

/************************************************************ Microsoft C */
#ifdef _MSC_VER
        mwWrite( "Compiled using Microsoft C" CPPTEXT
            " %d.%02d\n", _MSC_VER / 100, _MSC_VER % 100 );
#endif /* _MSC_VER */
/************************************************************ Microsoft C */

/************************************************************** Borland C */
#ifdef __BORLANDC__
        mwWrite( "Compiled using Borland C"
#ifdef __cplusplus
            "++ %d.%01d\n", __BCPLUSPLUS__/0x100, (__BCPLUSPLUS__%0x100)/0x10 );
#else
            " %d.%01d\n", __BORLANDC__/0x100, (__BORLANDC__%0x100)/0x10 );
#endif /* __cplusplus */
#endif /* __BORLANDC__ */
/************************************************************** Borland C */

/************************************************************** Watcom C */
#ifdef __WATCOMC__
        mwWrite( "Compiled using Watcom C %d.%02d ",
            __WATCOMC__/100, __WATCOMC__%100 );
#ifdef __FLAT__
        mwWrite( "(32-bit flat model)" );
#endif /* __FLAT__ */
        mwWrite( "\n" );
#endif /* __WATCOMC__ */
/************************************************************** Watcom C */

        mwWrite( "\n" );
        FLUSH();
        }

    if( mwUseAtexit ) (void) atexit( mwAbort );
    return;
    }

void mwAbort( void ) {
    mwMarker *mrk;
    char *data;
    time_t tid;
    int c, i, j;
    size_t li;

    tid = time( NULL );
    mwWrite( "\nStopped at %s\n", ctime( &tid) );

    if( !mwInited )
        mwWrite( "internal: mwAbort(): MEMWATCH not initialized!\n" );

    /* release the grab list */
    mwDropAll();

    /* report mwMarked items */
    while( mwFirstMark ) {
        mrk = mwFirstMark->next;
        mwWrite( "mark: %p: %s\n", mwFirstMark->host, mwFirstMark->text );
        free( mwFirstMark->text );
        free( mwFirstMark );
        mwFirstMark = mrk;
        mwErrors ++;
        }

    /*
    ** Release every delivered system allocation straight from the
    ** trusted ledger.  The management headers and the prev/next chain
    ** are never followed, so a trashed header cannot lose a block,
    ** truncate the scan or cause a double free.  Each system pointer is
    ** visited exactly once.
    */
    mwFlushW(0);
    for( li = 0; li < mwLedgerCount; ++li ) {
        mwLedgerEntry *e = &mwLedger[li];
        char *base = (char*) e->sys_ptr;
        char *user = base + mwDataSize + mwOverflowZoneSize;

        if( !e->nml ) {
            mwErrors++;
            mwTrustedGuardCheck( e );
            mwWrite( "unfreed: <%lu> %s(%d), %lu bytes at %p ",
                e->count, e->file, e->line,
                (unsigned long)e->req_size, user );
            mwWrite( " \t{" );
            j = 16; if( e->req_size < 16 ) j = (int) e->req_size;
            for( i=0;i<16;i++ ) {
                if( i<j ) mwWrite( "%02X ",
                    (unsigned char) user[i] );
                else mwWrite( ".. " );
                }
            for( i=0;i<j;i++ ) {
                c = (unsigned char) user[i];
                if( c < 32 || c > 126 ) c = '.';
                mwWrite( "%c", c );
                }
            mwWrite( "}\n" );
        }
        else {
            data = base + mwDataSize + mwOverflowZoneSize;
            if( mwTestMem( data, (unsigned) e->req_size, MW_VAL_NML ) ) {
                mwErrors++;
                mwWrite( "wild pointer: <%lu> NoMansLand %p alloc'd at %s(%d)\n",
                    e->count, user, e->file, e->line );
                FLUSH();
                }
            mwNmlNumAlloc --;
            mwNmlCurAlloc -= (long) e->req_size;
        }
        free( e->sys_ptr );
    }

    /* the side table itself was never part of the observed heap */
    free( mwLedger );
    mwLedger = NULL;
    mwLedgerCount = mwLedgerCap = 0;

	if( mwNmlNumAlloc ) mwWrite("internal: NoMansLand block counter %ld, not zero\n", mwNmlNumAlloc );
	if( mwNmlCurAlloc ) mwWrite("internal: NoMansLand byte counter %ld, not zero\n", mwNmlCurAlloc );

    /* report statistics */
    mwStatReport();
    FLUSH();

    mwInited = 0;
    mwHead = mwTail = NULL;
    mwHeapDegraded = 0;
    /* the ledger version changes, so plans prepared against the old
    ** heap cannot recommit; the epoch itself advances at the next
    ** mwInit() */
    mwLedgerBump();
    if( mwErrors )
        fprintf(mwSTDERR,"MEMWATCH detected %ld anomalies\n",mwErrors);
    mwLogFile( NULL );
    mwErrors = 0;

    MW_MUTEX_TERM();

    }

void mwTerm( void ) {
    if( mwInited == 1 )
    {
        mwAbort();
        return;
    }
    if( !mwInited )
        mwWrite("internal: mwTerm(): MEMWATCH has not been started!\n");
    else
        mwInited --;
    }

void mwStatistics( int level )
{
    mwAutoInit();
    if( level<0 ) level=0;
    if( mwStatLevel != level )
    {
		mwWrite( "statistics: now collecting on a %s basis\n",
			level<1?"global":(level<2?"module":"line") );
	    mwStatLevel = level;
	}
}

void mwAutoCheck( int onoff ) {
    mwAutoInit();
    mwTestAlways = onoff;
    if( onoff ) mwTestFlags = MW_TEST_ALL;
    }

void mwSetOutFunc( void (*func)(int) ) {
    mwAutoInit();
    mwOutFunction = func;
    }

static void mwWriteOF( void *p )
{
	int i;
	unsigned char *ptr;
	ptr = (unsigned char*) p;
	for( i=0; i<mwOverflowZoneSize; i++ )
	{
		*(ptr+i) = mwOverflowZoneTemplate[i%8];
	}
	return;
}

static int mwCheckOF( const void *p )
{
	int i;
	const unsigned char *ptr;
	ptr = (const unsigned char *) p;
	for( i=0; i<mwOverflowZoneSize; i++ )
	{
		if( *(ptr+i) != mwOverflowZoneTemplate[i%8] )
			return 1; /* errors found */
	}
	return 0; /* no errors */
}

int mwTest( const char *file, int line, int items ) {
    mwAutoInit();
    mwTestFlags = items;
    return mwTestNow( file, line, 0 );
    }

/*
** Returns zero if there are no errors.
** Returns nonzero if there are errors.
*/
int mwTestBuffer( const char *file, int line, void *p ) {
    mwData* mw;
    int lidx;
    int retv;

    mwAutoInit();

    /* resolve ownership through the trusted ledger only; for an
    ** unknown pointer nothing in front of it is read */
    lidx = mwLedgerFindUser( p );
    if( lidx < 0 )
        return 1;

    mw = (mwData*) mwLedger[lidx].sys_ptr;
    retv = mwTestBuf( mw, file, line );
    mwTrustedGuardCheck( &mwLedger[lidx] );
    if( mwLedger[lidx].nml ) {
        char *data = ((char*)mw)+mwDataSize+mwOverflowZoneSize;
        if( mwTestMem( data, (unsigned) mwLedger[lidx].req_size,
                       MW_VAL_NML ) ) {
            mwIncErr();
            mwWrite( "wild pointer: <%lu> NoMansLand %p alloc'd at %s(%d)\n",
                mwLedger[lidx].count,
                data + mwOverflowZoneSize,
                mwLedger[lidx].file, mwLedger[lidx].line );
            retv ++;
        }
    }
    return retv;
}

void mwBreakOut( const char* cause ) {
    fprintf(mwSTDERR, "breakout: %s\n", cause);
    mwWrite("breakout: %s\n", cause );
    return;
    }

/*
** 981217 JLI: is it possible that ->next is not always set?
*/
void * mwMark( void *p, const char *desc, const char *file, unsigned line ) {
    mwMarker *mrk;
    unsigned n, isnew;
    char *buf;
    int tot, oflow = 0;
    char wherebuf[128];

    mwAutoInit();
    TESTS(NULL,0);

    if( desc == NULL ) desc = "unknown";
    if( file == NULL ) file = "unknown";

    tot = sprintf( wherebuf, "%.48s called from %s(%d)", desc, file, line );
    if( tot >= (int)sizeof(wherebuf) ) { wherebuf[sizeof(wherebuf)-1] = 0; oflow = 1; }

    if( p == NULL ) {
        mwWrite("mark: %s(%d), no mark for NULL:'%s' may be set\n", file, line, desc );
        return p;
        }

	if( mwFirstMark != NULL && !mwIsReadAddr( mwFirstMark, sizeof( mwMarker ) ) )
	{
		mwWrite("mark: %s(%d), mwFirstMark (%p) is trashed, can't mark for %s\n",
			file, line, mwFirstMark, desc );
		return p;
	}

    for( mrk=mwFirstMark; mrk; mrk=mrk->next )
	{
		if( mrk->next != NULL && !mwIsReadAddr( mrk->next, sizeof( mwMarker ) ) )
		{
			mwWrite("mark: %s(%d), mark(%p)->next(%p) is trashed, can't mark for %s\n",
				file, line, mrk, mrk->next, desc );
			return p;
		}
		if( mrk->host == p ) break;
	}

    if( mrk == NULL ) {
        isnew = 1;
        mrk = (mwMarker*) malloc( sizeof( mwMarker ) );
        if( mrk == NULL ) {
            mwWrite("mark: %s(%d), no mark for %p:'%s', out of memory\n", file, line, p, desc );
            return p;
            }
		mrk->next = NULL;
        n = 0;
        }
    else {
        isnew = 0;
        n = strlen( mrk->text );
        }

    n += strlen( wherebuf );
    buf = (char*) malloc( n+3 );
    if( buf == NULL ) {
        if( isnew ) free( mrk );
        mwWrite("mark: %s(%d), no mark for %p:'%s', out of memory\n", file, line, p, desc );
        return p;
        }

    if( isnew ) {
        memcpy( buf, wherebuf, n+1 );
        mrk->next = mwFirstMark;
        mrk->host = p;
        mrk->text = buf;
        mrk->level = 1;
        mwFirstMark = mrk;
        }
    else {
        strcpy( buf, mrk->text );
        strcat( buf, ", " );
        strcat( buf, wherebuf );
        free( mrk->text );
        mrk->text = buf;
        mrk->level ++;
        }

    if( oflow ) {
        mwIncErr();
        mwTrace( " [WARNING: OUTPUT BUFFER OVERFLOW - SYSTEM UNSTABLE]\n" );
        }
    return p;
    }

void* mwUnmark( void *p, const char *file, unsigned line ) {
    mwMarker *mrk, *prv;
    mrk = mwFirstMark;
    prv = NULL;
    while( mrk ) {
        if( mrk->host == p ) {
            if( mrk->level < 2 ) {
                if( prv ) prv->next = mrk->next;
                else mwFirstMark = mrk->next;
                free( mrk->text );
                free( mrk );
                return p;
                }
            mrk->level --;
            return p;
            }
        prv = mrk;
        mrk = mrk->next;
        }
    mwWrite("mark: %s(%d), no mark found for %p\n", file, line, p );
    return p;
    }


/***********************************************************************
** Abort/Retry/Ignore handlers
***********************************************************************/

static int mwARI( const char *estr ) {
    char inbuf[81];
    int c;
    fprintf(mwSTDERR, "\n%s\nMEMWATCH: Abort, Retry or Ignore? ", estr);
    (void) fgets(inbuf,sizeof(inbuf),stdin);
	for( c=0; inbuf[c] && inbuf[c] <= ' '; c++ ) ;
    c = inbuf[c];
    if( c == 'R' || c == 'r' ) {
        mwBreakOut( estr );
        return MW_ARI_RETRY;
        }
    if( c == 'I' || c == 'i' ) return MW_ARI_IGNORE;
    return MW_ARI_ABORT;
    }

/* standard ARI handler (exported) */
int mwAriHandler( const char *estr ) {
    mwAutoInit();
    return mwARI( estr );
    }

/* used to set the ARI function */
void mwSetAriFunc( int (*func)(const char *) ) {
    mwAutoInit();
    mwAriFunction = func;
    }

/***********************************************************************
** Allocation handlers
***********************************************************************/

void* mwMalloc( size_t size, const char* file, int line) {
    size_t needed;
    mwData *mw;
    char *ptr;
    void *p;

    mwAutoInit();

	MW_MUTEX_LOCK();

    /* while metadata is known inconsistent, refuse rather than copy a
    ** damaged size/file/chain into new bookkeeping */
    if( mwHeapDegraded ) {
        mwWrite( "recovery: <%lu> %s(%d), allocation refused; heap "
                 "metadata awaits mwHeapCommit()\n",
                 (unsigned long) (mwCounter + 1), file, line );
        FLUSH();
        MW_MUTEX_UNLOCK();
        return NULL;
    }

    TESTS(file,line);

    mwCounter ++;
    needed = mwDataSize + mwOverflowZoneSize*2 + size;
    if( needed < size )
    {
    	/* theoretical case: req size + mw overhead exceeded size_t limits */
    	MW_MUTEX_UNLOCK();
    	return NULL;
    }

    /* if this allocation would violate the limit, fail it */
    if( mwUseLimit && ((long)size + mwStatCurAlloc > mwAllocLimit) ) {
        mwWrite( "limit fail: <%ld> %s(%d), %ld wanted %ld available\n",
            mwCounter, file, line, (long)size, mwAllocLimit - mwStatCurAlloc );
        mwIncErr();
        FLUSH();
		MW_MUTEX_UNLOCK();
        return NULL;
        }

    /* reserve the ledger slot before touching the system heap, so a
    ** metadata failure can never deliver a half-registered object, and
    ** never recurses into NML pressure reclamation */
    if( !mwLedgerEnsure( mwLedgerCount + 1 ) ) {
        mwWrite( "fail: <%ld> %s(%d), trusted ledger unavailable\n",
            mwCounter, file, line );
        mwIncErr();
        FLUSH();
        MW_MUTEX_UNLOCK();
        return NULL;
    }

    mw = (mwData*) malloc( needed );
    if( mw == NULL ) {
        if( mwFreeUp(needed,0) >= needed ) {
            mw = (mwData*) malloc(needed);
            if( mw == NULL ) {
                mwWrite( "internal: mwFreeUp(%u) reported success, but malloc() fails\n", needed );
                mwIncErr();
                FLUSH();
                }
            }
        if( mw == NULL ) {
            mwWrite( "fail: <%ld> %s(%d), %ld wanted %ld allocated\n",
                mwCounter, file, line, (long)size, mwStatCurAlloc );
            mwIncErr();
            FLUSH();
			MW_MUTEX_UNLOCK();
            return NULL;
            }
        }

    mw->count = mwCounter;
    mw->prev = NULL;
    mw->next = mwHead;
    mw->file = file;
    mw->size = size;
    mw->line = line;
    mw->flag = 0;
    mw->check = CHKVAL(mw);

    if( mwHead ) mwHead->prev = mw;
    mwHead = mw;
    if( mwTail == NULL ) mwTail = mw;

    ptr = ((char*)mw) + mwDataSize;
	mwWriteOF( ptr ); /* '*(long*)ptr = PRECHK;' */
    ptr += mwOverflowZoneSize;
    p = ptr;
    memset( ptr, MW_VAL_NEW, size );
    ptr += size;
    mwWriteOF( ptr ); /* '*(long*)ptr = POSTCHK;' */

    /* publish the canonical identity only after the block is fully
    ** prepared; if this last step fails, withdraw the system block */
    if( !mwLedgerAdd( mw, needed, p, size, mwCounter, file, line ) ) {
        mwUnlink( mw, file, line );
        free( mw );
        if( mwHead == NULL ) mwTail = NULL;
        mwWrite( "fail: <%ld> %s(%d), trusted ledger registration failed\n",
            mwCounter, file, line );
        mwIncErr();
        FLUSH();
        MW_MUTEX_UNLOCK();
        return NULL;
    }

    mwNumCurAlloc ++;
    mwStatCurAlloc += (long) size;
    mwStatTotAlloc += (long) size;
    if( mwStatCurAlloc > mwStatMaxAlloc )
        mwStatMaxAlloc = mwStatCurAlloc;
    mwStatNumAlloc ++;

    if( mwStatLevel ) mwStatAlloc( size, file, line );

	MW_MUTEX_UNLOCK();
    return p;
    }

void* mwRealloc( void *p, size_t size, const char* file, int line) {
    int oldUseLimit;
    mwData *mw;
    char *ptr;
    int lidx;
    mwLedgerEntry *le;

    mwAutoInit();

    if( p == NULL ) return mwMalloc( size, file, line );
    if( size == 0 ) { mwFree( p, file, line ); return NULL; }

	MW_MUTEX_LOCK();

    if( mwHeapDegraded ) {
        mwWrite( "recovery: <%lu> %s(%d), realloc refused; heap "
                 "metadata awaits mwHeapCommit()\n",
                 (unsigned long) (mwCounter + 1), file, line );
        FLUSH();
        MW_MUTEX_UNLOCK();
        return NULL;
    }

    /* ownership comes from the exact user start address; for an
    ** unknown pointer the memory in front of it is never read */
    lidx = mwLedgerFindUser( p );
    if( lidx >= 0 ) {
        le = &mwLedger[lidx];
        mw = (mwData*) le->sys_ptr;

		/* if the buffer is an NML, treat this as a double-free */
		if( le->nml )
		{
            mwIncErr();
            if( !mwTestMem( (char*)mw + mwDataSize + mwOverflowZoneSize,
                            1, MW_VAL_NML ) )
            {
				mwWrite( "internal: <%ld> %s(%d), no-mans-land MW-%p is corrupted\n",
					mwCounter, file, line, mw );
			}
			goto check_dbl_free;
		}

        /* refuse if the header has drifted from the canonical fact */
        if( !mwHeaderAgrees( le ) ) {
            mwMarkDegraded( "realloc ownership check", mw );
            MW_MUTEX_UNLOCK();
            return NULL;
        }

        /* if this allocation would violate the limit, fail it; the
        ** old live block is retained */
        if( mwUseLimit && ((long)size + mwStatCurAlloc - (long)mw->size > mwAllocLimit) ) {
            TESTS(file,line);
            mwCounter ++;
            mwWrite( "limit fail: <%ld> %s(%d), %ld wanted %ld available\n",
                mwCounter, file, line, (unsigned long)size - mw->size, mwAllocLimit - mwStatCurAlloc );
            mwIncErr();
            FLUSH();
			MW_MUTEX_UNLOCK();
            return NULL;
            }

        /* fake realloc operation: mwMalloc publishes the new identity,
        ** mwFree retires the old one through the same ledger */
        oldUseLimit = mwUseLimit;
        mwUseLimit = 0;
        ptr = (char*) mwMalloc( size, file, line );
        if( ptr != NULL ) {
            if( size < mw->size )
                memcpy( ptr, p, size );
            else
                memcpy( ptr, p, mw->size );
            mwFree( p, file, line );
            }
        mwUseLimit = oldUseLimit;
		MW_MUTEX_UNLOCK();
        return (void*) ptr;
        }

    /* Unknown pointer! */

    /* using free'd pointer? */
check_dbl_free:
    for(int i=0;i<MW_FREE_LIST;i++) {
        if( mwLastFree[i] == p ) {
            mwIncErr();
            mwWrite( "realloc: <%ld> %s(%d), %p was"
                " freed from %s(%d)\n",
                mwCounter, file, line, p,
                mwLFfile[i], mwLFline[i] );
            FLUSH();
			MW_MUTEX_UNLOCK();
            return NULL;
            }
        }

    /* some weird pointer */
    mwIncErr();
    mwWrite( "realloc: <%ld> %s(%d), unknown pointer %p\n",
        mwCounter, file, line, p );
    FLUSH();
	MW_MUTEX_UNLOCK();
    return NULL;
    }

char *mwStrdup( const char* str, const char* file, int line ) {
    size_t len;
    char *newstring;

	MW_MUTEX_LOCK();

    if( str == NULL ) {
        mwIncErr();
        mwWrite( "strdup: <%ld> %s(%d), strdup(NULL) called\n",
            mwCounter, file, line );
        FLUSH();
		MW_MUTEX_UNLOCK();
        return NULL;
        }

    len = strlen( str ) + 1;
    newstring = (char*) mwMalloc( len, file, line );
    if( newstring != NULL ) memcpy( newstring, str, len );
	MW_MUTEX_UNLOCK();
    return newstring;
    }

void mwFree( void* p, const char* file, int line ) {
    int i;
    mwData* mw;
    char buffer[ sizeof(mwData) + (mwROUNDALLOC*3) + 64 ];
    int lidx;
    mwLedgerEntry *le;

    /* this code is in support of C++ delete */
    if( file == NULL ) {
        mwFree_( p );
		MW_MUTEX_UNLOCK();
        return;
        }

    mwAutoInit();

	MW_MUTEX_LOCK();
    TESTS(file,line);
    mwCounter ++;

    /* on NULL free, write a warning and return */
    if( p == NULL ) {
        mwWrite( "NULL free: <%ld> %s(%d), NULL pointer free'd\n",
            mwCounter, file, line );
        FLUSH();
		MW_MUTEX_UNLOCK();
        return;
        }

    /* ownership is established solely from the ledger keyed by the
    ** exact user start address; no memory in front of an unknown
    ** pointer is dereferenced */
    lidx = mwLedgerFindUser( p );

    if( lidx >= 0 ) {
        le = &mwLedger[lidx];
        mw = (mwData*) le->sys_ptr;

        if( mwHeapDegraded ) {
            mwWrite( "recovery: <%ld> %s(%d), free of %p refused; "
                     "heap metadata awaits mwHeapCommit()\n",
                     mwCounter, file, line, p );
            FLUSH();
            MW_MUTEX_UNLOCK();
            return;
        }

        /* a damaged header blocks the free rather than being copied,
        ** reported through a trashed size or silently repaired */
        if( !mwHeaderAgrees( le ) ) {
            mwMarkDegraded( "free ownership check", mw );
            MW_MUTEX_UNLOCK();
            return;
        }

        (void) mwTestBuf( mw, file, line );
        /* guard zones are located from the trusted ledger size */
        mwTrustedGuardCheck( le );

		/* if the buffer is an NML, treat this as a double-free */
		if( le->nml )
		{
			if( *(((unsigned char*)mw)+mwDataSize+mwOverflowZoneSize) != MW_VAL_NML )
			{
				mwWrite( "internal: <%ld> %s(%d), no-mans-land MW-%p is corrupted\n",
					mwCounter, file, line, mw );
			}
			goto check_dbl_free;
		}

        /* update the statistics */
        mwNumCurAlloc --;
        mwStatCurAlloc -= (long) mw->size;
        if( mwStatLevel ) mwStatFree( mw->size, mw->file, mw->line );

        /* we should either free the allocation or keep it as NML.
        ** NML blocks stay in the ledger until the raw block is really
        ** returned; normal frees leave the ledger immediately. */
        if( mwNML ) {
            mw->flag |= MW_NML;
			mwNmlNumAlloc ++;
			mwNmlCurAlloc += (long) mw->size;
            memset( ((char*)mw)+mwDataSize+mwOverflowZoneSize, MW_VAL_NML, mw->size );
            mwLedgerMarkNML( (size_t) lidx );
            }
        else {
            /* unlink the allocation, and enter the post-free data */
            mwUnlink( mw, file, line );
            memset( mw, MW_VAL_DEL,
                mw->size + mwDataSize+mwOverflowZoneSize+mwOverflowZoneSize );
            if( mwFBI ) {
                memset( mw, '.', mwDataSize + mwOverflowZoneSize );
                sprintf( buffer, "FBI<%ld>%s(%d)", mwCounter, file, line );
                strncpy( (char*)(void*)mw, buffer, mwDataSize + mwOverflowZoneSize );
                }
            free( mw );
            mwLedgerRemoveAt( (size_t) lidx );
            }

        /* add the pointer to the last-free track */
        mwLFfile[ mwLFcur ] = file;
        mwLFline[ mwLFcur ] = line;
        mwLastFree[ mwLFcur++ ] = p;
        if( mwLFcur == MW_FREE_LIST ) mwLFcur = 0;

		MW_MUTEX_UNLOCK();
        return;
        }

    /* check for double-freeing */
check_dbl_free:
    for(i=0;i<MW_FREE_LIST;i++) {
        if( mwLastFree[i] == p ) {
            mwIncErr();
            mwWrite( "double-free: <%ld> %s(%d), %p was"
                " freed from %s(%d)\n",
                mwCounter, file, line, p,
                mwLFfile[i], mwLFline[i] );
            FLUSH();
			MW_MUTEX_UNLOCK();
            return;
            }
        }

    /* some weird pointer... block the free */
    mwIncErr();
    mwWrite( "WILD free: <%ld> %s(%d), unknown pointer %p\n",
        mwCounter, file, line, p );
    FLUSH();
	MW_MUTEX_UNLOCK();
    return;
    }

void* mwCalloc( size_t a, size_t b, const char *file, int line ) {
    void *p;
    size_t size = a * b;
    p = mwMalloc( size, file, line );
    if( p == NULL ) return NULL;
    memset( p, 0, size );
    return p;
    }

void mwFree_( void *p ) {
	MW_MUTEX_LOCK();
    TESTS(NULL,0);
	MW_MUTEX_UNLOCK();
    free(p);
    }

void* mwMalloc_( size_t size ) {
	MW_MUTEX_LOCK();
    TESTS(NULL,0);
	MW_MUTEX_UNLOCK();
    return malloc( size );
    }

void* mwRealloc_( void *p, size_t size ) {
	MW_MUTEX_LOCK();
    TESTS(NULL,0);
	MW_MUTEX_UNLOCK();
    return realloc( p, size );
    }

void* mwCalloc_( size_t a, size_t b ) {
	MW_MUTEX_LOCK();
    TESTS(NULL,0);
	MW_MUTEX_UNLOCK();
    return calloc( a, b );
    }

void mwFlushNow( void ) {
    if( mwLogR() ) fflush( mwLogR() );
    return;
    }

void mwDoFlush( int onoff ) {
    mwFlushW( onoff<1?0:onoff );
    if( onoff ) if( mwLogR() ) fflush( mwLogR() );
    return;
    }

void mwLimit( long lim ) {
    TESTS(NULL,0);
    mwWrite("limit: old limit = ");
    if( !mwAllocLimit ) mwWrite( "none" );
    else mwWrite( "%ld bytes", mwAllocLimit );
    mwWrite( ", new limit = ");
    if( !lim ) {
        mwWrite( "none\n" );
        mwUseLimit = 0;
        }
    else {
        mwWrite( "%ld bytes\n", lim );
        mwUseLimit = 1;
        }
    mwAllocLimit = lim;
    FLUSH();
    }

void mwSetAriAction( int action ) {
	MW_MUTEX_LOCK();
    TESTS(NULL,0);
    mwAriAction = action;
	MW_MUTEX_UNLOCK();
    return;
    }

int mwAssert( int exp, const char *exps, const char *fn, int ln ) {
    int i;
    char buffer[MW_TRACE_BUFFER+8];
    if( exp ) {
    	return 0;
    	}
    mwAutoInit();
	MW_MUTEX_LOCK();
    TESTS(fn,ln);
    mwIncErr();
    mwCounter++;
    mwWrite( "assert trap: <%ld> %s(%d), %s\n", mwCounter, fn, ln, exps );
    if( mwAriFunction != NULL ) {
        sprintf( buffer, "MEMWATCH: assert trap: %s(%d), %s", fn, ln, exps );
        i = (*mwAriFunction)(buffer);
		switch( i ) {
			case MW_ARI_IGNORE:
	           	mwWrite( "assert trap: <%ld> IGNORED - execution continues\n", mwCounter );
				MW_MUTEX_UNLOCK();
    	        return 0;
			case MW_ARI_RETRY:
            	mwWrite( "assert trap: <%ld> RETRY - executing again\n", mwCounter );
				MW_MUTEX_UNLOCK();
            	return 1;
			}
        }
    else {
        if( mwAriAction & MW_ARI_IGNORE ) {
            mwWrite( "assert trap: <%ld> AUTO IGNORED - execution continues\n", mwCounter );
			MW_MUTEX_UNLOCK();
            return 0;
            }
        fprintf(mwSTDERR,"\nMEMWATCH: assert trap: %s(%d), %s\n", fn, ln, exps );
        }

    FLUSH();
    (void) mwTestNow( fn, ln, 1 );
    FLUSH();

	if( mwAriAction & MW_ARI_NULLREAD ) {
		/* This is made in an attempt to kick in */
		/* any debuggers or OS stack traces */
	    FLUSH();
		/*lint -save -e413 */
		i = *((int*)NULL);
		mwDummy( (char)i );
		/*lint -restore */
		}

	MW_MUTEX_UNLOCK();
    exit(255);
    /* NOT REACHED - the return statement is in to keep */
    /* stupid compilers from squeaking about differing return modes. */
    /* Smart compilers instead say 'code unreachable...' */
    /*lint -save -e527 */
    return 0;
    /*lint -restore */
    }

int mwVerify( int exp, const char *exps, const char *fn, int ln ) {
    int i;
    char buffer[MW_TRACE_BUFFER+8];
    if( exp ) {
    	return 0;
    	}
    mwAutoInit();
	MW_MUTEX_LOCK();
    TESTS(fn,ln);
    mwIncErr();
    mwCounter++;
    mwWrite( "verify trap: <%ld> %s(%d), %s\n", mwCounter, fn, ln, exps );
    if( mwAriFunction != NULL ) {
        sprintf( buffer, "MEMWATCH: verify trap: %s(%d), %s", fn, ln, exps );
        i = (*mwAriFunction)(buffer);
        if( i == 0 ) {
            mwWrite( "verify trap: <%ld> IGNORED - execution continues\n", mwCounter );
			MW_MUTEX_UNLOCK();
            return 0;
            }
        if( i == 1 ) {
            mwWrite( "verify trap: <%ld> RETRY - executing again\n", mwCounter );
			MW_MUTEX_UNLOCK();
            return 1;
            }
        }
    else {
        if( mwAriAction & MW_ARI_NULLREAD ) {
            /* This is made in an attempt to kick in */
            /* any debuggers or OS stack traces */
		    FLUSH();
            /*lint -save -e413 */
            i = *((int*)NULL);
			mwDummy( (char)i );
            /*lint -restore */
            }
        if( mwAriAction & MW_ARI_IGNORE ) {
            mwWrite( "verify trap: <%ld> AUTO IGNORED - execution continues\n", mwCounter );
			MW_MUTEX_UNLOCK();
            return 0;
            }
        fprintf(mwSTDERR,"\nMEMWATCH: verify trap: %s(%d), %s\n", fn, ln, exps );
        }
    FLUSH();
    (void) mwTestNow( fn, ln, 1 );
    FLUSH();
	MW_MUTEX_UNLOCK();
	exit(255);
    /* NOT REACHED - the return statement is in to keep */
    /* stupid compilers from squeaking about differing return modes. */
    /* Smart compilers instead say 'code unreachable...' */
    /*lint -save -e527 */
    return 0;
    /*lint -restore */
    }

void mwTrace( const char *format, ... ) {
    int tot, oflow = 0;
    va_list mark;

    mwAutoInit();
	MW_MUTEX_LOCK();
    TESTS(NULL,0);
    if( mwOutFunction == NULL ) mwOutFunction = mwDefaultOutFunc;

    va_start( mark, format );
    tot = vsprintf( mwPrintBuf, format, mark );
    va_end( mark );
    if( tot >= MW_TRACE_BUFFER ) { mwPrintBuf[MW_TRACE_BUFFER] = 0; oflow = 1; }
    for(tot=0;mwPrintBuf[tot];tot++)
        (*mwOutFunction)( mwPrintBuf[tot] );
    if( oflow ) {
        mwIncErr();
        mwTrace( " [WARNING: OUTPUT BUFFER OVERFLOW - SYSTEM UNSTABLE]\n" );
        }

    FLUSH();
	MW_MUTEX_UNLOCK();
    }


/***********************************************************************
** Grab & Drop
***********************************************************************/

unsigned mwGrab( unsigned kb ) {
    TESTS(NULL,0);
    return mwGrab_( kb, MW_VAL_GRB, 0 );
    }

unsigned mwDrop( unsigned kb ) {
    TESTS(NULL,0);
    return mwDrop_( kb, MW_VAL_GRB, 0 );
    }

static void mwDropAll() {
    TESTS(NULL,0);
    (void) mwDrop_( 0, MW_VAL_GRB, 0 );
    (void) mwDrop_( 0, MW_VAL_NML, 0 );
    if( mwGrabList != NULL )
        mwWrite( "internal: the grab list is not empty after mwDropAll()\n");
    }

static const char *mwGrabType( int type ) {
    switch( type ) {
        case MW_VAL_GRB:
            return "grabbed";
        case MW_VAL_NML:
            return "no-mans-land";
        default:
            /* do nothing */
            ;
        }
    return "<unknown type>";
    }

static unsigned mwGrab_( unsigned kb, int type, int silent ) {
    unsigned i = kb;
    mwGrabData *gd;
    if( !kb ) i = kb = 65000U;

    for(;kb;kb--) {
        if( mwUseLimit &&
            (mwStatCurAlloc + mwGrabSize + (long)sizeof(mwGrabData) > mwAllocLimit) ) {
            if( !silent ) {
                mwWrite("grabbed: all allowed memory to %s (%u kb)\n",
                    mwGrabType(type), i-kb);
                FLUSH();
                }
            return i-kb;
            }
        gd = (mwGrabData*) malloc( sizeof(mwGrabData) );
        if( gd == NULL ) {
            if( !silent ) {
                mwWrite("grabbed: all available memory to %s (%u kb)\n",
                    mwGrabType(type), i-kb);
                FLUSH();
                }
            return i-kb;
            }
        mwGrabSize += (long) sizeof(mwGrabData);
        gd->next = mwGrabList;
        memset( gd->blob, type, sizeof(gd->blob) );
        gd->type = type;
        mwGrabList = gd;
        }
    if( !silent ) {
        mwWrite("grabbed: %u kilobytes of %s memory\n", i, mwGrabType(type) );
        FLUSH();
        }
    return i;
    }

static unsigned mwDrop_( unsigned kb, int type, int silent ) {
    unsigned i = kb;
    mwGrabData *gd,*tmp,*pr;
    const void *p;

    if( mwGrabList == NULL && kb == 0 ) return 0;
    if( !kb ) i = kb = 60000U;

    pr = NULL;
    gd = mwGrabList;
    for(;kb;) {
        if( gd == NULL ) {
            if( i-kb > 0 && !silent ) {
                mwWrite("dropped: all %s memory (%u kb)\n", mwGrabType(type), i-kb);
                FLUSH();
                }
            return i-kb;
            }
        if( gd->type == type ) {
            if( pr ) pr->next = gd->next;
            kb --;
            tmp = gd;
            if( mwGrabList == gd ) mwGrabList = gd->next;
            gd = gd->next;
            p = mwTestMem( tmp->blob, sizeof( tmp->blob ), type );
            if( p != NULL ) {
                mwWrite( "wild pointer: <%ld> %s memory hit at %p\n",
                    mwCounter, mwGrabType(type), p );
                FLUSH();
                }
            mwGrabSize -= (long) sizeof(mwGrabData);
            free( tmp );
            }
        else {
            pr = gd;
            gd = gd->next;
            }
        }
    if( !silent ) {
        mwWrite("dropped: %u kilobytes of %s memory\n", i, mwGrabType(type) );
        FLUSH();
        }
    return i;
    }

/***********************************************************************
** No-Mans-Land
***********************************************************************/

void mwNoMansLand( int level ) {
    mwAutoInit();
    TESTS(NULL,0);
    switch( level ) {
        case MW_NML_NONE:
            (void) mwDrop_( 0, MW_VAL_NML, 0 );
            break;
        case MW_NML_FREE:
            break;
        case MW_NML_ALL:
            (void) mwGrab_( 0, MW_VAL_NML, 0 );
            break;
        default:
            return;
        }
    mwNML = level;
    }

/***********************************************************************
** Static functions
***********************************************************************/

static void mwAutoInit( void )
{
    if( mwInited ) return;
    mwUseAtexit = 1;
    mwInit();
    return;
}

static FILE *mwLogR() {
    if( (mwLog == mwLogB1) && (mwLog == mwLogB2) ) return mwLog;
    if( mwLog == mwLogB1 ) mwLogB2 = mwLog;
    if( mwLog == mwLogB2 ) mwLogB1 = mwLog;
    if( mwLogB1 == mwLogB2 ) mwLog = mwLogB1;
    if( (mwLog == mwLogB1) && (mwLog == mwLogB2) ) {
        mwWrite("internal: log file handle damaged and recovered\n");
        FLUSH();
        return mwLog;
        }
    fprintf(mwSTDERR,"\nMEMWATCH: log file handle destroyed, using mwSTDERR\n" );
    mwLog = mwLogB1 = mwLogB2 = mwSTDERR;
    return mwSTDERR;
    }

static void mwLogW( FILE *p ) {
    mwLog = mwLogB1 = mwLogB2 = p;
    }

static int mwFlushR() {
    if( (mwFlushing == mwFlushingB1) && (mwFlushing == mwFlushingB2) ) return mwFlushing;
    if( mwFlushing == mwFlushingB1 ) mwFlushingB2 = mwFlushing;
    if( mwFlushing == mwFlushingB2 ) mwFlushingB1 = mwFlushing;
    if( mwFlushingB1 == mwFlushingB2 ) mwFlushing = mwFlushingB1;
    if( (mwFlushing == mwFlushingB1) && (mwFlushing == mwFlushingB2) ) {
        mwWrite("internal: flushing flag damaged and recovered\n");
        FLUSH();
        return mwFlushing;
        }
    mwWrite("internal: flushing flag destroyed, so set to true\n");
    mwFlushing = mwFlushingB1 = mwFlushingB2 = 1;
    return 1;
    }

static void mwFlushW( int n ) {
    mwFlushing = mwFlushingB1 = mwFlushingB2 = n;
    }

static void mwIncErr() {
    mwErrors++;
    mwFlushW( mwFlushR()+1 );
    FLUSH();
    }

static void mwFlush() {
    if( mwLogR() == NULL ) return;
#ifdef MW_FLUSH
    fflush( mwLogR() );
#else
    if( mwFlushR() ) fflush( mwLogR() );
#endif
    return;
    }

static void mwUnlink( mwData* mw, const char* file, int line ) {
    if( mw->prev == NULL ) {
        if( mwHead != mw )
            mwWrite( "internal: <%ld> %s(%d), MW-%p: link1 NULL, but not head\n",
                mwCounter, file, line, mw );
        mwHead = mw->next;
        }
    else {
        if( mw->prev->next != mw )
            mwWrite( "internal: <%ld> %s(%d), MW-%p: link1 failure\n",
                mwCounter, file, line, mw );
        else mw->prev->next = mw->next;
        }
    if( mw->next == NULL ) {
        if( mwTail != mw )
            mwWrite( "internal: <%ld> %s(%d), MW-%p: link2 NULL, but not tail\n",
                mwCounter, file, line, mw );
        mwTail = mw->prev;
        }
    else {
        if( mw->next->prev != mw )
            mwWrite( "internal: <%ld> %s(%d), MW-%p: link2 failure\n",
                mwCounter, file, line, mw );
        else mw->next->prev = mw->prev;
        }
    }


/*
** mwTestBuf:
**  Read-only check of one buffer's header links and checksum.
**  Guard zones are deliberately not located from mw->size here; the
**  caller pairs this with mwTrustedGuardCheck(), which positions the
**  pre/post zones from the ledger's trusted request size.  This
**  function never rewrites a link or invokes the old heuristic relink.
*/
static int mwTestBuf( mwData* mw, const char* file, int line ) {
    int retv = 0;

    if( file == NULL ) file = "unknown";

    if( !mwIsSafeAddr( mw, mwDataSize + mwOverflowZoneSize ) ) {
        mwWrite( "internal: <%ld> %s(%d): pointer MW-%p is invalid\n",
            mwCounter, file, line, mw );
        mwIncErr();
        mwMarkDegraded( "invalid header pointer", mw );
        return 2;
        }

    if( mw->check != CHKVAL(mw) ) {
        mwWrite( "internal: <%ld> %s(%d), info trashed; explicit "
                 "mwHeapCommit() required\n",
            mwCounter, file, line );
        mwIncErr();
        mwMarkDegraded( "bad checksum", mw );
        retv = 2;
        }

    if( mw->prev && mw->prev->next != mw ) {
        mwWrite( "internal: <%ld> %s(%d), buffer <%ld> %s(%d) link1 broken\n",
            mwCounter,file,line, (long)mw->size, mw->count, mw->file, mw->line );
        mwIncErr();
        mwMarkDegraded( "prev link", mw );
        retv = 2;
        }
    if( mw->next && mw->next->prev != mw ) {
        mwWrite( "internal: <%ld> %s(%d), buffer <%ld> %s(%d) link2 broken\n",
            mwCounter,file,line, (long)mw->size, mw->count, mw->file, mw->line );
        mwIncErr();
        mwMarkDegraded( "next link", mw );
        retv = 2;
        }

    return retv;
    }

static void mwDefaultOutFunc( int c ) {
    if( mwLogR() ) fputc( c, mwLogR() );
    }

static void mwWrite( const char *format, ... ) {
    int tot, oflow = 0;
    va_list mark;
    mwAutoInit();
    if( mwOutFunction == NULL ) mwOutFunction = mwDefaultOutFunc;
    va_start( mark, format );
    tot = vsprintf( mwPrintBuf, format, mark );
    va_end( mark );
    if( tot >= MW_TRACE_BUFFER ) { mwPrintBuf[MW_TRACE_BUFFER] = 0; oflow = 1; }
    for(tot=0;mwPrintBuf[tot];tot++)
        (*mwOutFunction)( mwPrintBuf[tot] );
    if( oflow ) {
        mwWrite( "\ninternal: mwWrite(): WARNING! OUTPUT EXCEEDED %u CHARS: SYSTEM UNSTABLE\n", MW_TRACE_BUFFER-1 );
        FLUSH();
        }
    return;
    }

static void mwLogFile( const char *name ) {
    time_t tid;
    (void) time( &tid );
    if( mwLogR() != NULL ) {
        fclose( mwLogR() );
        mwLogW( NULL );
        }
    if( name == NULL ) return;
    mwLogW( fopen( name, "a" COMMIT ) );
    if( mwLogR() == NULL )
        mwWrite( "logfile: failed to open/create file '%s'\n", name );
    }

/*
** Try to free NML memory until a contiguous allocation of
** 'needed' bytes can be satisfied. If this is not enough
** and the 'urgent' parameter is nonzero, grabbed memory is
** also freed.
*/
static size_t mwFreeUp( size_t needed, int urgent ) {
    void *p;
    mwData *mw, *mw2;
    char *data;

    /* free grabbed NML memory */
    for(;;) {
        if( mwDrop_( 1, MW_VAL_NML, 1 ) == 0 ) break;
        p = malloc( needed );
        if( p == NULL ) continue;
        free( p );
        return needed;
        }

    /* free normal NML memory */
    mw = mwHead;
    while( mw != NULL ) {
        if( !(mw->flag & MW_NML) ) mw = mw->next;
        else {
            data = ((char*)mw)+mwDataSize+mwOverflowZoneSize;
            if( mwTestMem( data, mw->size, MW_VAL_NML ) ) {
                mwIncErr();
                mwWrite( "wild pointer: <%ld> NoMansLand %p alloc'd at %s(%d)\n",
                    mw->count, data + mwOverflowZoneSize, mw->file, mw->line );
                }
            mw2 = mw->next;
            mwUnlink( mw, "mwFreeUp", 0 );
            /* the block really leaves the heap now, so its canonical
            ** fact is removed as well; an NML that was actually
            ** evicted is always honestly accounted.  The cumulative
            ** NML statistics keep their original meaning and are
            ** reconciled at mwAbort() against what remains. */
            {
                int evicted = mwLedgerFindSys( mw );
                if( evicted >= 0 )
                    mwLedgerRemoveAt( (size_t) evicted );
            }
            free( mw );
#ifdef MW_HEAP_TESTING
            mwTestReclaimedNML ++;
#endif
            mw = mw2;
            p = malloc( needed );
            if( p == NULL ) continue;
            free( p );
            return needed;
            }
        }

    /* if not urgent (for internal purposes), fail */
    if( !urgent ) return 0;

    /* free grabbed memory */
    for(;;) {
        if( mwDrop_( 1, MW_VAL_GRB, 1 ) == 0 ) break;
        p = malloc( needed );
        if( p == NULL ) continue;
        free( p );
        return needed;
        }

    return 0;
    }

static const void * mwTestMem( const void *p, unsigned len, int c ) {
    const unsigned char *ptr;
    ptr = (const unsigned char *) p;
    while( len-- ) {
        if( *ptr != (unsigned char)c ) return (const void*)ptr;
        ptr ++;
        }
    return NULL;
    }

static int mwStrCmpI( const char *s1, const char *s2 ) {
    if( s1 == NULL || s2 == NULL ) return 0;
    while( *s1 ) {
        if( toupper(*s2) == toupper(*s1) ) { s1++; s2++; continue; }
        return 1;
        }
    return 0;
    }

#define AIPH() if( always_invoked ) { mwWrite("autocheck: <%ld> %s(%d) ", mwCounter, file, line ); always_invoked = 0; }

static int mwTestNow( const char *file, int line, int always_invoked ) {
    int retv = 0;
    size_t li;
    char *data;

    if( file && !always_invoked )
        mwWrite("check: <%ld> %s(%d), checking %s%s%s\n",
            mwCounter, file, line,
			(mwTestFlags & MW_TEST_CHAIN) ? "chain ": "",
		    (mwTestFlags & MW_TEST_ALLOC) ? "alloc ": "",
		    (mwTestFlags & MW_TEST_NML) ? "nomansland ": ""
			);

    /* the scan is driven by the trusted ledger; the prev/next chain
    ** is only observed, never repaired while checking */
    if( mwTestFlags & MW_TEST_CHAIN ) {
        for( li = 0; li < mwLedgerCount; ++li ) {
            const mwLedgerEntry *e = &mwLedger[li];
            mwData *mw = (mwData*) e->sys_ptr;
            if( !mwIsSafeAddr(mw, mwDataSize) ) {
                AIPH();
                mwWrite("check: heap corruption detected\n");
                mwIncErr();
                mwMarkDegraded( "unreadable header", mw );
                return retv + 1;
            }
            if( mw->prev ) {
                if( !mwIsSafeAddr(mw->prev, mwDataSize) ) {
                    AIPH();
                    mwWrite("check: heap corruption detected\n");
                    mwIncErr();
                    mwMarkDegraded( "unreadable prev link", mw );
                    return retv + 1;
                }
                if( mw->prev->next != mw ) {
                    AIPH();
                    mwWrite("check: heap chain broken, prev link incorrect\n");
                    mwIncErr();
                    mwMarkDegraded( "prev link", mw );
                    retv ++;
                }
            }
            else if( mwHead != mw ) {
                AIPH();
                mwWrite("check: heap chain broken, head incorrect\n");
                mwIncErr();
                mwMarkDegraded( "head link", mw );
                retv ++;
            }
            if( mw->next ) {
                if( !mwIsSafeAddr(mw->next, mwDataSize) ) {
                    AIPH();
                    mwWrite("check: heap corruption detected\n");
                    mwIncErr();
                    mwMarkDegraded( "unreadable next link", mw );
                    return retv + 1;
                }
                if( mw->next->prev != mw ) {
                    AIPH();
                    mwWrite("check: heap chain broken, next link incorrect\n");
                    mwIncErr();
                    mwMarkDegraded( "next link", mw );
                    retv ++;
                }
            }
            else if( mwTail != mw ) {
                AIPH();
                mwWrite("check: heap chain broken, tail incorrect\n");
                mwIncErr();
                mwMarkDegraded( "tail link", mw );
                retv ++;
            }
        }
    }
    if( mwTestFlags & MW_TEST_ALLOC ) {
        for( li = 0; li < mwLedgerCount; ++li ) {
            if( mwTestBuf( (mwData*) mwLedger[li].sys_ptr,
                           file, line ) ) retv ++;
            /* guard positions come from the trusted request size */
            mwTrustedGuardCheck( &mwLedger[li] );
        }
    }
    if( mwTestFlags & MW_TEST_NML ) {
        for( li = 0; li < mwLedgerCount; ++li ) {
            const mwLedgerEntry *e = &mwLedger[li];
            if( e->nml ) {
                mwData *mw = (mwData*) e->sys_ptr;
                data = ((char*)mw)+mwDataSize+mwOverflowZoneSize;
                if( mwTestMem( data, (unsigned) e->req_size,
                               MW_VAL_NML ) ) {
                    mwIncErr();
                    mwWrite( "wild pointer: <%lu> NoMansLand %p alloc'd at %s(%d)\n",
                        e->count, data + mwOverflowZoneSize,
                        e->file, e->line );
                }
            }
        }
    }

	if( file && !always_invoked && !retv )
        mwWrite("check: <%ld> %s(%d), complete; no errors\n",
            mwCounter, file, line );
    return retv;
    }

/**********************************************************************
** Statistics
**********************************************************************/

static void mwStatReport()
{
    mwStat* ms, *ms2;
    const char *modname;
    int modnamelen;

    /* global statistics report */
    mwWrite( "\nMemory usage statistics (global):\n" );
    mwWrite( " N)umber of allocations made: %ld\n", mwStatNumAlloc );
    mwWrite( " L)argest memory usage      : %ld\n", mwStatMaxAlloc );
    mwWrite( " T)otal of all alloc() calls: %ld\n", mwStatTotAlloc );
    mwWrite( " U)nfreed bytes totals      : %ld\n", mwStatCurAlloc );
    FLUSH();

    if( mwStatLevel < 1 ) return;

    /* on a per-module basis */
    mwWrite( "\nMemory usage statistics (detailed):\n");
    mwWrite( " Module/Line                                Number   Largest  Total    Unfreed \n");
    for( ms=mwStatList; ms; ms=ms->next )
    {
        if( ms->line == -1 )
        {
			if( ms->file == NULL || !mwIsReadAddr(ms->file,22) ) modname = "<unknown>";
			else modname = ms->file;
			modnamelen = strlen(modname);
			if( modnamelen > 42 )
			{
				modname = modname + modnamelen - 42;
			}

            mwWrite(" %-42s %-8ld %-8ld %-8ld %-8ld\n",
            	modname, ms->num, ms->max, ms->total, ms->curr );
            if( ms->file && mwStatLevel > 1 )
            {
                for( ms2=mwStatList; ms2; ms2=ms2->next )
                {
                    if( ms2->line!=-1 && ms2->file!=NULL && !mwStrCmpI( ms2->file, ms->file ) )
					{
					mwWrite( "  %-8d                                  %-8ld %-8ld %-8ld %-8ld\n",
						ms2->line, ms2->num, ms2->max, ms2->total, ms2->curr );
					}
				}
			}
		}
	}
}

static mwStat* mwStatGet( const char *file, int line, int makenew ) {
    mwStat* ms;

    if( mwStatLevel < 2 ) line = -1;

    for( ms=mwStatList; ms!=NULL; ms=ms->next ) {
        if( line != ms->line ) continue;
        if( file==NULL ) {
            if( ms->file == NULL ) break;
            continue;
            }
        if( ms->file == NULL ) continue;
        if( !strcmp( ms->file, file ) ) break;
        }

    if( ms != NULL ) return ms;

    if( !makenew ) return NULL;

    ms = (mwStat*) malloc( sizeof(mwStat) );
    if( ms == NULL ) {
        if( mwFreeUp( sizeof(mwStat), 0 ) < sizeof(mwStat) ||
            (ms=(mwStat*)malloc(sizeof(mwStat))) == NULL ) {
            mwWrite("internal: memory low, statistics incomplete for '%s'\n", file );
            return NULL;
            }
        }
    ms->file = file;
    ms->line = line;
    ms->total = 0L;
    ms->max = 0L;
    ms->num = 0L;
    ms->curr = 0L;
    ms->next = mwStatList;
    mwStatList = ms;
    return ms;
    }

static void mwStatAlloc( size_t size, const char* file, int line ) {
    mwStat* ms;

    /* update the module statistics */
    ms = mwStatGet( file, -1, 1 );
    if( ms != NULL ) {
        ms->total += (long) size;
        ms->curr += (long) size;
        ms->num ++;
        if( ms->curr > ms->max ) ms->max = ms->curr;
        }

    /* update the line statistics */
    if( mwStatLevel > 1 && line != -1 && file ) {
        ms = mwStatGet( file, line, 1 );
        if( ms != NULL ) {
            ms->total += (long) size;
            ms->curr += (long) size;
            ms->num ++;
            if( ms->curr > ms->max ) ms->max = ms->curr;
            }
        }

    }

static void mwStatFree( size_t size, const char* file, int line ) {
    mwStat* ms;

    /* update the module statistics */
    ms = mwStatGet( file, -1, 1 );
    if( ms != NULL ) ms->curr -= (long) size;

    /* update the line statistics */
    if( mwStatLevel > 1 && line != -1 && file ) {
        ms = mwStatGet( file, line, 1 );
        if( ms != NULL ) ms->curr -= (long) size;
        }
    }

/***********************************************************************
** Safe memory checkers
**
** Using ifdefs, implement the operating-system specific mechanism
** of identifying a piece of memory as legal to access with read
** and write priviliges. Default: return nonzero for non-NULL pointers.
***********************************************************************/

static char mwDummy( char c )
{
	return c;
}

#ifndef MW_SAFEADDR
#ifdef WIN32
#define MW_SAFEADDR
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
int mwIsReadAddr( const void *p, unsigned len )
{
    if( p == NULL ) return 0;
    if( IsBadReadPtr(p,len) ) return 0;
    return 1;
}
int mwIsSafeAddr( void *p, unsigned len )
{
    /* NOTE: For some reason, under Win95 the IsBad... */
    /* can return false for invalid pointers. */
    if( p == NULL ) return 0;
    if( IsBadReadPtr(p,len) || IsBadWritePtr(p,len) ) return 0;
    return 1;
}
#endif /* WIN32 */
#endif /* MW_SAFEADDR */

#ifndef MW_SAFEADDR
#ifdef SIGSEGV
#define MW_SAFEADDR

typedef void (*mwSignalHandlerPtr)( int );
mwSignalHandlerPtr mwOldSIGSEGV = (mwSignalHandlerPtr) 0;
jmp_buf mwSIGSEGVjump;
static void mwSIGSEGV( int n );

static void mwSIGSEGV( int n )
{
	n = n;
	longjmp( mwSIGSEGVjump, 1 );
}

int mwIsReadAddr( const void *p, unsigned len )
{
	const char *ptr;

    if( p == NULL ) return 0;
	if( !len ) return 1;

	/* set up to catch the SIGSEGV signal */
	mwOldSIGSEGV = signal( SIGSEGV, mwSIGSEGV );

	if( setjmp( mwSIGSEGVjump ) )
	{
		signal( SIGSEGV, mwOldSIGSEGV );
		return 0;
	}

	/* read all the bytes in the range */
	ptr = (const char *)p;
	ptr += len;

	/* the reason for this rather strange construct is that */
	/* we want to keep the number of used parameters and locals */
	/* to a minimum. if we use len for a counter gcc will complain */
	/* it may get clobbered by longjmp() at high warning levels. */
	/* it's a harmless warning, but this way we don't have to see it. */
	do
	{
		ptr --;
		if( *ptr == 0x7C ) (void) mwDummy( (char)0 );
	} while( (const void*) ptr != p );

	/* remove the handler */
	signal( SIGSEGV, mwOldSIGSEGV );

    return 1;
}
int mwIsSafeAddr( void *p, unsigned len )
{
	char *ptr;

	if( p == NULL ) return 0;
	if( !len ) return 1;

	/* set up to catch the SIGSEGV signal */
	mwOldSIGSEGV = signal( SIGSEGV, mwSIGSEGV );

	if( setjmp( mwSIGSEGVjump ) )
	{
		signal( SIGSEGV, mwOldSIGSEGV );
		return 0;
	}

	/* read and write-back all the bytes in the range */
	ptr = (char *)p;
	ptr += len;

	/* the reason for this rather strange construct is that */
	/* we want to keep the number of used parameters and locals */
	/* to a minimum. if we use len for a counter gcc will complain */
	/* it may get clobbered by longjmp() at high warning levels. */
	/* it's a harmless warning, but this way we don't have to see it. */
	do
	{
		ptr --;
		*ptr = mwDummy( *ptr );
	} while( (void*) ptr != p );

	/* remove the handler */
	signal( SIGSEGV, mwOldSIGSEGV );

    return 1;
}
#endif /* SIGSEGV */
#endif /* MW_SAFEADDR */

#ifndef MW_SAFEADDR
int mwIsReadAddr( const void *p, unsigned len )
{
    if( p == NULL ) return 0;
    if( len == 0 ) return 1;
    return 1;
}
int mwIsSafeAddr( void *p, unsigned len )
{
    if( p == NULL ) return 0;
    if( len == 0 ) return 1;
    return 1;
}
#endif

/**********************************************************************
** Mutex handling
**********************************************************************/

#if defined(WIN32) || defined(__WIN32__)

static void	mwMutexInit( void )
{
	mwGlobalMutex = CreateMutex( NULL, FALSE, NULL);
	return;
}

static void	mwMutexTerm( void )
{
	CloseHandle( mwGlobalMutex );
	return;
}

static void	mwMutexLock( void )
{
	if( WaitForSingleObject(mwGlobalMutex, 1000 ) == WAIT_TIMEOUT )
	{
		mwWrite( "mwMutexLock: timed out, possible deadlock\n" );
	}
	return;
}

static void	mwMutexUnlock( void )
{
	ReleaseMutex( mwGlobalMutex );
	return;
}

#endif

#if defined(MW_PTHREADS) || defined(HAVE_PTHREAD_H)

static void	mwMutexInit( void )
{
	pthread_mutex_init( &mwGlobalMutex, NULL );
	return;
}

static void	mwMutexTerm( void )
{
	pthread_mutex_destroy( &mwGlobalMutex );
	return;
}

static void	mwMutexLock( void )
{
	pthread_mutex_lock(&mwGlobalMutex);
	return;
}

static void	mwMutexUnlock( void )
{
	pthread_mutex_unlock(&mwGlobalMutex);
	return;
}

#endif

/**********************************************************************
** C++ new & delete
**********************************************************************/

#if 0 /* 980317: disabled C++ */

#ifdef __cplusplus
#ifndef MEMWATCH_NOCPP

int mwNCur = 0;
const char *mwNFile = NULL;
int mwNLine = 0;

class MemWatch {
public:
    MemWatch();
    ~MemWatch();
    };

MemWatch::MemWatch() {
    if( mwInited ) return;
    mwUseAtexit = 0;
    mwInit();
    }

MemWatch::~MemWatch() {
    if( mwUseAtexit ) return;
    mwTerm();
    }

/*
** This global new will catch all 'new' calls where MEMWATCH is
** not active.
*/
void* operator new( size_t size ) {
    mwNCur = 0;
    return mwMalloc( size, "<unknown>", 0 );
    }

/*
** This is the new operator that's called when a module uses mwNew.
*/
void* operator new( size_t size, const char *file, int line ) {
    mwNCur = 0;
    return mwMalloc( size, file, line );
    }

/*
** This is the new operator that's called when a module uses mwNew[].
** -- hjc 07/16/02
*/
void* operator new[] ( size_t size, const char *file, int line ) {
    mwNCur = 0;
    return mwMalloc( size, file, line );
    }

/*
** Since this delete operator will recieve ALL delete's
** even those from within libraries, we must accept
** delete's before we've been initialized. Nor can we
** reliably check for wild free's if the mwNCur variable
** is not set.
*/
void operator delete( void *p ) {
    if( p == NULL ) return;
    if( !mwInited ) {
        free( p );
        return;
        }
    if( mwNCur ) {
        mwFree( p, mwNFile, mwNLine );
        mwNCur = 0;
        return;
        }
    mwFree_( p );
    }

void operator delete[]( void *p ) {
    if( p == NULL ) return;
    if( !mwInited ) {
        free( p );
        return;
        }
    if( mwNCur ) {
        mwFree( p, mwNFile, mwNLine );
        mwNCur = 0;
        return;
        }
    mwFree_( p );
    }

#endif /* MEMWATCH_NOCPP */
#endif /* __cplusplus */

#endif /* 980317: disabled C++ */

/* MEMWATCH.C */
