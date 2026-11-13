// Tile-based POSIX worker backend for filled triangles.
//
// The calling thread keeps running the unchanged vertex stage, guard-band
// clipping and primitive assembly. Generated triangles are bucketed into
// 16x16 pixel tiles and published in batches; persistent worker threads run
// the existing fill rasterizer on one tile at a time. At most two batches are
// in flight, so geometry production for the next batch overlaps shading of
// the previous one without ever accumulating geometry proportional to the
// whole draw.
//
// This file is only compiled when PGL_TILE_THREADS is defined. Without it the
// public entry point lives in gl_impl.c and positive worker counts fail with
// GL_INVALID_OPERATION, and the library never references pthread.

#ifdef PGL_TILE_THREADS

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

// Statics provided by gl_internal.c (one translation unit after amalgamation).
static glVertex* pgl_vert(u32 id);
static int pgl_tri_prim_count(GLenum mode, GLsizei count);
static void pgl_tri_slots(GLenum mode, GLsizei k, GLsizei* i0, GLsizei* i1,
                          GLsizei* i2, unsigned* provoke);
static int pgl_tri_class(glVertex* v0, glVertex* v1, glVertex* v2);
static int pgl_emit_tri_fast(glVertex* v0, glVertex* v1, glVertex* v2,
                             GLsizei i0, GLsizei i1, GLsizei i2,
                             unsigned provoke, pgl_tri* rec);
static void pgl_clip_tri(glVertex* v0, glVertex* v1, glVertex* v2,
                         unsigned provoke, pgl_tri* recs, int* n_out);
static void pgl_screen_space_range(GLenum mode, GLsizei count, GLsizei prim, GLsizei chunk);
static int pgl_viewport_raster_rect(int* left, int* bottom, int* right, int* top);
static inline int pgl_snap_xy(float v);
static void draw_triangle_fill(glVertex* v0, glVertex* v1, glVertex* v2, unsigned int provoke);
static void draw_triangle_fill_clipped(glVertex* v0, glVertex* v1, glVertex* v2,
                                       unsigned int provoke, const pgl_fill_clip* tile);

// TLS accessors implemented in gl_internal.c.
static void pgl_tls_enter(GLboolean front);
static void pgl_tls_set_front(GLboolean front);
static void pgl_tls_reset_mip(void);
static void pgl_tls_exit(void);

typedef struct pgl_tile_pool {
	pthread_t* threads;
	int nworkers;
	int shutdown;

	pthread_mutex_t lock;
	pthread_cond_t work_cv; // workers: a batch was published
	pthread_cond_t done_cv; // wake reapers / publishers: tile work finished

	// Publication ring, at most PGL_TILE_MAX_FLIGHT entries, oldest first.
	pgl_tile_batch* queue[2];
	int n_queued;

	// Per-tile publication/completion order, grown with the tile grid.
	unsigned* last_pub;
	unsigned* last_done;
	int tile_seq_cap;
} pgl_tile_pool;

#define PGL_TILE_MAX_FLIGHT 2

// Only filled triangle modes run in parallel; points, lines and non-fill
// polygon modes keep the serial pipeline.
static GLboolean pgl_tile_fill_active(void)
{
	return c->tile_workers > 0 && c->tile_pool != NULL &&
	       c->draw_triangle_front == draw_triangle_fill &&
	       c->draw_triangle_back == draw_triangle_fill;
}

static glVertex* pgl_tile_vert(pgl_tile_batch* b, u32 id)
{
	u32 idx = id & PGL_INDEX_MASK;
	if (id & PGL_VERT_ARENA)
		return &b->arena_verts[idx];
	return &c->glverts.a[idx];
}

// Pixel bbox of one record in final write-pixel coordinates, using the same
// snapping and viewport/scissor intersection as the fill rasterizer. Returns
// 0 when the record covers no writable pixel. Record ids still resolve in the
// caller's clip_arena/glverts at publication time.
static int pgl_tile_pixel_bounds(const pgl_tri* t,
                                 int* px0, int* py0, int* px1, int* py1)
{
	glVertex* v0 = pgl_vert(t->v[0]);
	glVertex* v1 = pgl_vert(t->v[1]);
	glVertex* v2 = pgl_vert(t->v[2]);

	int vx0 = pgl_snap_xy(v0->screen_space.x);
	int vy0 = pgl_snap_xy(v0->screen_space.y);
	int vx1 = pgl_snap_xy(v1->screen_space.x);
	int vy1 = pgl_snap_xy(v1->screen_space.y);
	int vx2 = pgl_snap_xy(v2->screen_space.x);
	int vy2 = pgl_snap_xy(v2->screen_space.y);

	float inv_scale = 1.0f / (float)PGL_SUBPIXEL_SCALE;
	float x_min = MIN((float)vx0, MIN((float)vx1, (float)vx2)) * inv_scale;
	float x_max = MAX((float)vx0, MAX((float)vx1, (float)vx2)) * inv_scale;
	float y_min = MIN((float)vy0, MIN((float)vy1, (float)vy2)) * inv_scale;
	float y_max = MAX((float)vy0, MAX((float)vy1, (float)vy2)) * inv_scale;

	int r_left, r_bottom, r_right, r_top;
	if (!pgl_viewport_raster_rect(&r_left, &r_bottom, &r_right, &r_top))
		return 0;

	x_min = MAX(x_min, (float)r_left);
	x_max = MIN(x_max, (float)r_right);
	y_min = MAX(y_min, (float)r_bottom);
	y_max = MIN(y_max, (float)r_top);
	if (!(x_min < x_max) || !(y_min < y_max))
		return 0;

	*px0 = (int)x_min;
	*py0 = (int)y_min;
	*px1 = (int)(x_max + 0.5f) - 1;
	*py1 = (int)(y_max + 0.5f) - 1;
	return 1;
}

static void pgl_tile_render(pgl_tile_batch* b, int tile)
{
	int tx = tile % b->tiles_x;
	int ty = tile / b->tiles_x;

	// Tiles cover the raster clip rect [lx,ux) x [ly,uy), which may start
	// off-origin when the scissor test is enabled.
	int x0 = c->lx + tx * PGL_TILE_SIZE;
	int y0 = c->ly + ty * PGL_TILE_SIZE;
	int x1 = x0 + PGL_TILE_SIZE;
	int y1 = y0 + PGL_TILE_SIZE;
	if (x1 > c->ux)
		x1 = c->ux;
	if (y1 > c->uy)
		y1 = c->uy;

	int lo = b->tile_offset[tile];
	int hi = b->tile_offset[tile + 1];
	if (lo == hi)
		return;

	GLboolean first_front = (b->entries[lo].meta & PGL_FRONT_BIT) ? GL_TRUE : GL_FALSE;
	pgl_tls_enter(first_front);

	for (int i = lo; i < hi; ++i) {
		pgl_tri* t = &b->entries[i];
		glVertex* v0 = pgl_tile_vert(b, t->v[0]);
		glVertex* v1 = pgl_tile_vert(b, t->v[1]);
		glVertex* v2 = pgl_tile_vert(b, t->v[2]);
		unsigned provoke = t->meta & PGL_PROVOKE_MASK;
		GLboolean front = (t->meta & PGL_FRONT_BIT) ? GL_TRUE : GL_FALSE;

		pgl_fill_clip fc;
		fc.x0 = x0;
		fc.y0 = y0;
		fc.x1 = x1;
		fc.y1 = y1;
		fc.instance_id = b->instance_id;

		pgl_tls_set_front(front);
		pgl_tls_reset_mip();
		draw_triangle_fill_clipped(v0, v1, v2, provoke, &fc);
	}

	pgl_tls_exit();
}

static void* pgl_tile_worker(void* arg)
{
	pgl_tile_pool* pool = (pgl_tile_pool*)arg;

	pthread_mutex_lock(&pool->lock);
	for (;;) {
		if (pool->shutdown)
			break;

		// Claim the next unclaimed tile across the in-flight batches, in
		// publication order. Same-tile ordering is enforced by wait_seq.
		pgl_tile_batch* chosen = NULL;
		int job_idx = -1;
		for (int qi = 0; qi < pool->n_queued; ++qi) {
			pgl_tile_batch* b = pool->queue[qi];
			if (b->next_tile < b->n_jobs) {
				chosen = b;
				job_idx = b->next_tile++;
				break;
			}
		}

		if (!chosen) {
			pthread_cond_wait(&pool->work_cv, &pool->lock);
			continue;
		}

		pgl_tile_job job = chosen->jobs[job_idx];
		unsigned key = chosen->seq_key;

		// A later batch never overtakes an earlier one on the same tile.
		// Keys are 1-based; 0 means no earlier batch exists for that tile.
		while (!pool->shutdown && pool->last_done[job.tile] < (unsigned)job.wait_seq)
			pthread_cond_wait(&pool->done_cv, &pool->lock);
		if (pool->shutdown)
			break;

		pthread_mutex_unlock(&pool->lock);
		pgl_tile_render(chosen, job.tile);
		pthread_mutex_lock(&pool->lock);

		pool->last_done[job.tile] = key;
		chosen->tiles_remaining--;
		pthread_cond_broadcast(&pool->done_cv);
	}
	pthread_mutex_unlock(&pool->lock);
	return NULL;
}

// Grow the per-tile ordering arrays when the draw surface gets more tiles.
static GLboolean pgl_tile_ensure_tiles(pgl_tile_pool* pool, int n_tiles)
{
	if (n_tiles <= pool->tile_seq_cap)
		return GL_TRUE;
	int ncap = pool->tile_seq_cap ? pool->tile_seq_cap : 64;
	while (ncap < n_tiles)
		ncap *= 2;
	unsigned* pub = (unsigned*)PGL_MALLOC((size_t)ncap * sizeof(unsigned));
	unsigned* done = (unsigned*)PGL_MALLOC((size_t)ncap * sizeof(unsigned));
	if (!pub || !done) {
		PGL_FREE(pub);
		PGL_FREE(done);
		return GL_FALSE;
	}
	for (int i = 0; i < ncap; ++i) {
		pub[i] = i < pool->tile_seq_cap ? pool->last_pub[i] : 0;
		done[i] = i < pool->tile_seq_cap ? pool->last_done[i] : 0;
	}
	PGL_FREE(pool->last_pub);
	PGL_FREE(pool->last_done);
	pool->last_pub = pub;
	pool->last_done = done;
	pool->tile_seq_cap = ncap;
	return GL_TRUE;
}

static GLboolean pgl_batch_ensure(pgl_tile_batch* b, int entries, int arena_verts,
                                  int tiles, int jobs)
{
	if (entries > b->entries_cap) {
		int ncap = b->entries_cap ? b->entries_cap : PGL_CHUNK_PRIMS;
		while (ncap < entries)
			ncap *= 2;
		pgl_tri* p = (pgl_tri*)PGL_REALLOC(b->entries, (size_t)ncap * sizeof(pgl_tri));
		if (!p)
			return GL_FALSE;
		b->entries = p;
		b->entries_cap = ncap;
	}
	if (tiles + 1 > b->offset_cap) {
		int ncap = b->offset_cap ? b->offset_cap : 64;
		while (ncap < tiles + 1)
			ncap *= 2;
		int* p = (int*)PGL_REALLOC(b->tile_offset, (size_t)ncap * sizeof(int));
		if (!p)
			return GL_FALSE;
		b->tile_offset = p;
		b->offset_cap = ncap;
	}
	if (jobs > b->jobs_cap) {
		int ncap = b->jobs_cap ? b->jobs_cap : 64;
		while (ncap < jobs)
			ncap *= 2;
		pgl_tile_job* p = (pgl_tile_job*)PGL_REALLOC(b->jobs,
		                    (size_t)ncap * sizeof(pgl_tile_job));
		if (!p)
			return GL_FALSE;
		b->jobs = p;
		b->jobs_cap = ncap;
	}
	if (arena_verts > b->arena_cap) {
		int ncap = b->arena_cap ? b->arena_cap : PGL_CLIP_ARENA_INIT;
		while (ncap < arena_verts)
			ncap *= 2;
		glVertex* v = (glVertex*)PGL_REALLOC(b->arena_verts, (size_t)ncap * sizeof(glVertex));
		float* f = (float*)PGL_REALLOC(b->arena_varyings,
		                               (size_t)ncap * GL_MAX_VERTEX_OUTPUT_COMPONENTS * sizeof(float));
		if (!v || !f) {
			// realloc preserves the old allocations; remember whichever moved.
			if (v) b->arena_verts = v;
			if (f) b->arena_varyings = f;
			return GL_FALSE;
		}
		b->arena_verts = v;
		b->arena_varyings = f;
		b->arena_cap = ncap;
	}
	return GL_TRUE;
}

// Reap the oldest batch once all of its tiles are done, then fire RETIRED.
static pgl_tile_batch* pgl_tile_reap_head(pgl_tile_pool* pool)
{
	pthread_mutex_lock(&pool->lock);
	while (pool->n_queued == 0 || pool->queue[0]->tiles_remaining > 0)
		pthread_cond_wait(&pool->done_cv, &pool->lock);

	pgl_tile_batch* b = pool->queue[0];
	for (int i = 1; i < pool->n_queued; ++i)
		pool->queue[i - 1] = pool->queue[i];
	pool->n_queued--;
	unsigned seq = b->seq_key - 1;
	b->seq_key = 0;
	pthread_cond_broadcast(&pool->done_cv);
	pthread_mutex_unlock(&pool->lock);

	PGL_TILE_BATCH_RETIRED(seq);
	return b;
}
// Publish one flushed chunk. Blocks while two batches are already in flight.
// Empty chunks never become batches and do not consume a sequence number.
static void pgl_tile_publish(pgl_tile_pool* pool, pgl_tri* recs, int n_out,
                             int arena_count)
{
	if (n_out == 0)
		return;

	// Hard cap of two in-flight batches: reclaim the oldest slot first.
	if (pool->n_queued == PGL_TILE_MAX_FLIGHT)
		pgl_tile_reap_head(pool);

	pgl_tile_batch* b = NULL;
	for (int i = 0; i < PGL_TILE_MAX_FLIGHT; ++i) {
		if (c->tile_slots[i].seq_key == 0) {
			b = &c->tile_slots[i];
			break;
		}
	}
	PGL_ASSERT(b);

	int w = c->ux - c->lx;
	int h = c->uy - c->ly;
	int tiles_x = (w + PGL_TILE_SIZE - 1) / PGL_TILE_SIZE;
	int tiles_y = (h + PGL_TILE_SIZE - 1) / PGL_TILE_SIZE;
	int n_tiles = tiles_x * tiles_y;

	int* counts = (int*)PGL_MALLOC(((size_t)n_tiles + 1) * sizeof(int));
	PGL_ASSERT(counts);
	memset(counts, 0, ((size_t)n_tiles + 1) * sizeof(int));

	int nonempty = 0;
	int total_refs = 0;
	for (int i = 0; i < n_out; ++i) {
		int px0, py0, px1, py1;
		if (!pgl_tile_pixel_bounds(&recs[i], &px0, &py0, &px1, &py1))
			continue;
		int tx0 = (px0 - c->lx) / PGL_TILE_SIZE;
		int tx1 = (px1 - c->lx) / PGL_TILE_SIZE;
		int ty0 = (py0 - c->ly) / PGL_TILE_SIZE;
		int ty1 = (py1 - c->ly) / PGL_TILE_SIZE;
		for (int ty = ty0; ty <= ty1; ++ty)
			for (int tx = tx0; tx <= tx1; ++tx)
				counts[ty * tiles_x + tx]++;
	}
	for (int t = 0; t < n_tiles; ++t) {
		if (counts[t])
			nonempty++;
		total_refs += counts[t];
	}
	// Every record landed outside the writable rect: no batch, no sequence
	// number and no hook events for work that has no fragment references.
	if (total_refs == 0) {
		PGL_FREE(counts);
		return;
	}
	// A primitive spanning several tiles is referenced once per tile, so the
	// entry table can legitimately be larger than the chunk's record count.
	if (!pgl_batch_ensure(b, total_refs, arena_count, n_tiles, nonempty)) {
		PGL_FREE(counts);
		PGL_ASSERT(0 && "tile batch allocation failed");
		return;
	}

	b->tiles_x = tiles_x;
	b->tiles_y = tiles_y;
	b->instance_id = c->builtins.gl_InstanceID;
	b->tile_offset[0] = 0;
	for (int t = 0; t < n_tiles; ++t)
		b->tile_offset[t + 1] = b->tile_offset[t] + counts[t];

	// Refill via per-tile cursors; serial emission order is preserved inside
	// every tile because records are scanned in order.
	int* cursor = (int*)PGL_MALLOC((size_t)n_tiles * sizeof(int));
	PGL_ASSERT(cursor);
	for (int t = 0; t < n_tiles; ++t)
		cursor[t] = b->tile_offset[t];

	for (int i = 0; i < n_out; ++i) {
		int px0, py0, px1, py1;
		if (!pgl_tile_pixel_bounds(&recs[i], &px0, &py0, &px1, &py1))
			continue;
		int tx0 = (px0 - c->lx) / PGL_TILE_SIZE;
		int tx1 = (px1 - c->lx) / PGL_TILE_SIZE;
		int ty0 = (py0 - c->ly) / PGL_TILE_SIZE;
		int ty1 = (py1 - c->ly) / PGL_TILE_SIZE;
		for (int ty = ty0; ty <= ty1; ++ty) {
			for (int tx = tx0; tx <= tx1; ++tx) {
				int t = ty * tiles_x + tx;
				b->entries[cursor[t]++] = recs[i];
			}
		}
	}
	PGL_FREE(cursor);
	PGL_FREE(counts);

	// Private copies of the clip arena, with vs_out repointed at the copy.
	b->arena_count = arena_count;
	if (arena_count) {
		memcpy(b->arena_verts, c->clip_arena.verts, (size_t)arena_count * sizeof(glVertex));
		memcpy(b->arena_varyings, c->clip_arena.varyings,
		       (size_t)arena_count * GL_MAX_VERTEX_OUTPUT_COMPONENTS * sizeof(float));
		for (int i = 0; i < arena_count; ++i)
			b->arena_verts[i].vs_out = b->arena_varyings +
			                           (size_t)i * GL_MAX_VERTEX_OUTPUT_COMPONENTS;
	}

	pthread_mutex_lock(&pool->lock);
	if (!pgl_tile_ensure_tiles(pool, n_tiles)) {
		pthread_mutex_unlock(&pool->lock);
		PGL_ASSERT(0 && "tile ordering array allocation failed");
		return;
	}

	unsigned seq = c->tile_seq++;
	unsigned key = seq + 1;
	b->seq_key = key;
	b->n_jobs = nonempty;
	b->next_tile = 0;
	b->tiles_remaining = nonempty;

	int ji = 0;
	for (int t = 0; t < n_tiles; ++t) {
		if (b->tile_offset[t] == b->tile_offset[t + 1])
			continue;
		b->jobs[ji].tile = t;
		b->jobs[ji].wait_seq = pool->last_pub[t];
		pool->last_pub[t] = key;
		++ji;
	}
	pool->queue[pool->n_queued++] = b;
	pthread_cond_broadcast(&pool->work_cv);
	pthread_mutex_unlock(&pool->lock);

	PGL_TILE_BATCH_READY(seq);
}

// Mirror of pgl_assemble_tris() with tile publication in place of immediate
// rasterization. Chunking, clipping and arena rewinds happen at exactly the
// same points, so vertex shader invocations and clip fragments match serial.
static void pgl_tile_assemble_tris(GLenum mode, GLsizei count)
{
	pgl_tile_pool* pool = (pgl_tile_pool*)c->tile_pool;
	int nprims = pgl_tri_prim_count(mode, count);
	int prim = 0;
	pgl_tri* recs = (pgl_tri*)c->prim_buf;

	while (prim < nprims) {
		int chunk = prim + PGL_CHUNK_PRIMS;
		int n_out = 0;
		if (chunk > nprims)
			chunk = nprims;

		pgl_screen_space_range(mode, count, prim, chunk);
		c->clip_arena.count = 0;

		for (; prim < chunk; ++prim) {
			GLsizei i0, i1, i2;
			unsigned provoke;
			glVertex* v0;
			glVertex* v1;
			glVertex* v2;
			int kind;

			pgl_tri_slots(mode, (GLsizei)prim, &i0, &i1, &i2, &provoke);
			v0 = &c->glverts.a[i0];
			v1 = &c->glverts.a[i1];
			v2 = &c->glverts.a[i2];
			kind = pgl_tri_class(v0, v1, v2);
			if (kind < 0)
				continue;
			if (kind == 0) {
				if (PGL_CHUNK_PRIMS - n_out < PGL_MAX_CLIP_TRIS) {
					pgl_tile_publish(pool, recs, n_out, c->clip_arena.count);
					n_out = 0;
					c->clip_arena.count = 0;
				}
				pgl_clip_tri(v0, v1, v2, provoke, recs, &n_out);
				continue;
			}
			if (n_out == PGL_CHUNK_PRIMS) {
				pgl_tile_publish(pool, recs, n_out, c->clip_arena.count);
				n_out = 0;
				c->clip_arena.count = 0;
			}
			n_out += pgl_emit_tri_fast(v0, v1, v2, i0, i1, i2, provoke, &recs[n_out]);
		}
		if (n_out > 0)
			pgl_tile_publish(pool, recs, n_out, c->clip_arena.count);
		c->clip_arena.count = 0;
	}

	// Drain in publication order so draw returns with every fragment done.
	while (pool->n_queued > 0)
		pgl_tile_reap_head(pool);
}

static void pgl_tile_pool_destroy(pgl_tile_pool* pool)
{
	if (!pool)
		return;
	pthread_mutex_lock(&pool->lock);
	pool->shutdown = 1;
	pthread_cond_broadcast(&pool->work_cv);
	pthread_cond_broadcast(&pool->done_cv);
	pthread_mutex_unlock(&pool->lock);

	for (int i = 0; i < pool->nworkers; ++i)
		pthread_join(pool->threads[i], NULL);

	pthread_mutex_destroy(&pool->lock);
	pthread_cond_destroy(&pool->work_cv);
	pthread_cond_destroy(&pool->done_cv);
	PGL_FREE(pool->threads);
	PGL_FREE(pool->last_pub);
	PGL_FREE(pool->last_done);
	PGL_FREE(pool);
}

static void pgl_tile_batch_free(pgl_tile_batch* b)
{
	PGL_FREE(b->tile_offset);
	PGL_FREE(b->entries);
	PGL_FREE(b->jobs);
	PGL_FREE(b->arena_verts);
	PGL_FREE(b->arena_varyings);
	memset(b, 0, sizeof(*b));
}

// Build a fresh pool; on failure every created thread is joined and no
// half-built pool remains. The caller keeps the previous configuration.
static pgl_tile_pool* pgl_tile_pool_create(int nworkers)
{
	pgl_tile_pool* pool = (pgl_tile_pool*)PGL_MALLOC(sizeof(pgl_tile_pool));
	if (!pool)
		return NULL;
	memset(pool, 0, sizeof(*pool));

	pool->threads = (pthread_t*)PGL_MALLOC((size_t)nworkers * sizeof(pthread_t));
	if (!pool->threads) {
		PGL_FREE(pool);
		return NULL;
	}

	if (pthread_mutex_init(&pool->lock, NULL) != 0 ||
	    pthread_cond_init(&pool->work_cv, NULL) != 0 ||
	    pthread_cond_init(&pool->done_cv, NULL) != 0) {
		pthread_mutex_destroy(&pool->lock);
		pthread_cond_destroy(&pool->work_cv);
		pthread_cond_destroy(&pool->done_cv);
		PGL_FREE(pool->threads);
		PGL_FREE(pool);
		return NULL;
	}

	int created = 0;
	for (int i = 0; i < nworkers; ++i) {
		if (pthread_create(&pool->threads[i], NULL, pgl_tile_worker, pool) != 0)
			break;
		created++;
	}
	if (created != nworkers) {
		pool->nworkers = created;
		pgl_tile_pool_destroy(pool);
		return NULL;
	}
	pool->nworkers = nworkers;
	return pool;
}

PGLDEF GLboolean pglSetTileWorkers(GLsizei workers)
{
	PGL_ERR_RET_VAL(workers < 0, GL_INVALID_VALUE, GL_FALSE);

	if (workers == c->tile_workers)
		return GL_TRUE;

	if (workers == 0) {
		pgl_tile_pool_destroy((pgl_tile_pool*)c->tile_pool);
		c->tile_pool = NULL;
		c->tile_workers = 0;
		for (int i = 0; i < PGL_TILE_MAX_FLIGHT; ++i)
			pgl_tile_batch_free(&c->tile_slots[i]);
		// tile_seq intentionally keeps increasing for the life of the context.
		return GL_TRUE;
	}

	pgl_tile_pool* pool = pgl_tile_pool_create(workers);
	if (!pool) {
		PGL_SET_ERR(GL_OUT_OF_MEMORY);
		return GL_FALSE;
	}

	// Swap only after the new pool is fully built, so a failure leaves the
	// existing configuration untouched.
	pgl_tile_pool* old = (pgl_tile_pool*)c->tile_pool;
	c->tile_pool = pool;
	c->tile_workers = workers;
	pgl_tile_pool_destroy(old);
	return GL_TRUE;
}

// Called from free_glContext().
static void pgl_tile_shutdown(glContext* ctx)
{
	if (!ctx->tile_pool) {
		for (int i = 0; i < PGL_TILE_MAX_FLIGHT; ++i)
			pgl_tile_batch_free(&ctx->tile_slots[i]);
		return;
	}
	pgl_tile_pool_destroy((pgl_tile_pool*)ctx->tile_pool);
	ctx->tile_pool = NULL;
	ctx->tile_workers = 0;
	for (int i = 0; i < PGL_TILE_MAX_FLIGHT; ++i)
		pgl_tile_batch_free(&ctx->tile_slots[i]);
}

#endif // PGL_TILE_THREADS
