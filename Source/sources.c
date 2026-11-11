/*
** SGI FREE SOFTWARE LICENSE B (Version 2.0, Sept. 18, 2008)
** Copyright (C) [dates of first publication] Silicon Graphics, Inc.
** All Rights Reserved.
**
** Permission is hereby granted, free of charge, to any person obtaining a copy
** of this software and associated documentation files (the "Software"), to deal
** in the Software without restriction, including without limitation the rights
** to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
** of the Software, and to permit persons to whom the Software is furnished to do so,
** subject to the following conditions:
**
** The above copyright including the dates of first publication and either this
** permission notice or a reference to http://oss.sgi.com/projects/FreeB/ shall be
** included in all copies or substantial portions of the Software.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
** INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
** PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL SILICON GRAPHICS, INC.
** BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
** TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
** OR OTHER DEALINGS IN THE SOFTWARE.
*/

/*
** Per-half-edge source bookkeeping.  Only source edge *identities* are
** propagated through the mesh operations; the parameter intervals t0/t1
** are evaluated from the final boundary endpoints when the output is
** built.  Every record is owned by the pool, so resetting or deleting the
** pool reclaims all records without walking the mesh.
*/

#include <stddef.h>

#include "sources.h"

TESSsourcePool *tessSourceNewPool( TESSalloc *alloc )
{
	TESSsourcePool *pool = (TESSsourcePool *)alloc->memalloc( alloc->userData, sizeof( TESSsourcePool ) );
	if( pool == NULL ) return NULL;
	pool->bucket = createBucketAlloc( alloc, "Boundary Sources",
									  sizeof( TESSsourceRec ), alloc->meshEdgeBucketSize );
	if( pool->bucket == NULL ) {
		alloc->memfree( alloc->userData, pool );
		return NULL;
	}
	return pool;
}

void tessSourceDeletePool( TESSalloc *alloc, TESSsourcePool *pool )
{
	if( pool == NULL ) return;
	deleteBucketAlloc( pool->bucket );
	alloc->memfree( alloc->userData, pool );
}

static TESSsourceRec *AllocRec( TESSsourcePool *pool )
{
	TESSsourceRec *rec = (TESSsourceRec *)bucketAlloc( pool->bucket );
	if( rec == NULL ) return NULL;
	rec->next = NULL;
	rec->t0 = 0;
	rec->t1 = 0;
	return rec;
}

static void FreeList( TESSsourcePool *pool, TESSsourceRec *head )
{
	while( head != NULL ) {
		TESSsourceRec *next = head->next;
		bucketFree( pool->bucket, head );
		head = next;
	}
}

/* Inserts rec into the sorted (contour, edge) list *head, merging with an
** existing record for the same source edge.  Takes ownership of rec. */
static int ListInsert( TESSsourcePool *pool, TESSsourceRec **head, TESSsourceRec *rec )
{
	TESSsourceRec *cur, *prev = NULL;

	for( cur = *head; cur != NULL; prev = cur, cur = cur->next ) {
		if( cur->contour == rec->contour && cur->edge == rec->edge ) {
			bucketFree( pool->bucket, rec );
			return 1;
		}
		if( cur->contour > rec->contour ||
			( cur->contour == rec->contour && cur->edge > rec->edge ) ) {
			break;
		}
	}
	rec->next = cur;
	if( prev == NULL ) {
		*head = rec;
	} else {
		prev->next = rec;
	}
	return 1;
}

static int ListCopy( TESSsourcePool *pool, TESSsourceRec **dst, const TESSsourceRec *src )
{
	*dst = NULL;
	while( src != NULL ) {
		TESSsourceRec *rec = AllocRec( pool );
		if( rec == NULL ) return 0;
		rec->contour = src->contour;
		rec->edge = src->edge;
		if( !ListInsert( pool, dst, rec ) ) return 0;
		src = src->next;
	}
	return 1;
}

int tessSourceSetContourEdge( TESSsourcePool *pool, TESShalfEdge *e,
							  TESSindex contour, TESSindex edge )
{
	TESSsourceRec *r0, *r1;
	if( pool == NULL ) return 1;

	/* tessMeshSplitEdge() may have pre-propagated the previous edge's
	** identity onto this fresh half-edge; replace it with the real one. */
	FreeList( pool, e->srcHead );
	FreeList( pool, e->Sym->srcHead );

	r0 = AllocRec( pool );
	if( r0 == NULL ) goto fail;
	r0->contour = contour;
	r0->edge = edge;
	e->srcHead = r0;

	r1 = AllocRec( pool );
	if( r1 == NULL ) goto fail;
	r1->contour = contour;
	r1->edge = edge;
	e->Sym->srcHead = r1;
	return 1;

fail:
	FreeList( pool, e->srcHead );
	FreeList( pool, e->Sym->srcHead );
	e->srcHead = NULL;
	e->Sym->srcHead = NULL;
	return 0;
}

int tessSourceSplit( TESSsourcePool *pool, TESShalfEdge *eOrg )
{
	TESShalfEdge *eNew;
	if( pool == NULL ) return 1;

	/* tessMeshSplitEdge() made eNew = eOrg->Lnext the second piece of the
	** split edge.  Both pieces are covered by the same source edges, so the
	** identity sets are duplicated (parameters are derived at output time). */
	eNew = eOrg->Lnext;
	if( !ListCopy( pool, &eNew->srcHead, eOrg->srcHead ) ) return 0;
	if( !ListCopy( pool, &eNew->Sym->srcHead, eOrg->Sym->srcHead ) ) return 0;
	return 1;
}

int tessSourceMerge( TESSsourcePool *pool, TESShalfEdge *eKeep, TESShalfEdge *eDrop )
{
	TESSsourceRec *cur, *next;
	if( pool == NULL ) return 1;

	/* Both directions are merged; the call sites delete geometrically
	** coincident half-edge pairs, so the surviving pair covers the union. */
	for( cur = eDrop->srcHead; cur != NULL; cur = next ) {
		TESSsourceRec *rec;
		next = cur->next;
		rec = AllocRec( pool );
		if( rec == NULL ) return 0;
		rec->contour = cur->contour;
		rec->edge = cur->edge;
		if( !ListInsert( pool, &eKeep->srcHead, rec ) ) return 0;
		bucketFree( pool->bucket, cur );
	}
	eDrop->srcHead = NULL;

	for( cur = eDrop->Sym->srcHead; cur != NULL; cur = next ) {
		TESSsourceRec *rec;
		next = cur->next;
		rec = AllocRec( pool );
		if( rec == NULL ) return 0;
		rec->contour = cur->contour;
		rec->edge = cur->edge;
		if( !ListInsert( pool, &eKeep->Sym->srcHead, rec ) ) return 0;
		bucketFree( pool->bucket, cur );
	}
	eDrop->Sym->srcHead = NULL;
	return 1;
}
