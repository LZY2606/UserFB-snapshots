/* SPDX-FileCopyrightText: 2026 Mattias Jansson
 * SPDX-License-Identifier: Unlicense OR MIT
 */

/*
 * Contract tests for the debug free quarantine (rpmalloc_config_t.quarantine_max_bytes).
 *
 * The quarantine keeps freed blocks in a global FIFO, fills their whole usable user area with a
 * poison byte and validates the area before a block becomes reusable again. These tests drive the
 * allocator through a custom, controlled memory mapping interface and observe it through the
 * quarantine_error callback.
 *
 * Threading rules exercised here: every worker thread calls rpmalloc_thread_initialize before any
 * allocation and rpmalloc_thread_finalize before it exits, and thread phases are ordered with
 * pthread barriers or pthread_join, never with timed sleeps.
 *
 * The quarantine_error callback only records diagnostics: it must never call back into the
 * allocator (no rpmalloc/rpfree and, under the malloc override, no libc malloc/free), and must not
 * block on a thread that allocates. All callbacks here just touch plain counters and arrays.
 */

#include "contract.h"
#include "rpmalloc.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define TEST_POISON 0xD3
#define TEST_DUPLICATE_OFFSET ((size_t)-1)

/* ------------------------------------------------------------------ */
/* Portable two-party phase barrier                                   */
/* ------------------------------------------------------------------ */

/*
 * macOS does not provide pthread_barrier_t, so the remote-thread phase ordering uses this small
 * barrier built on a mutex and condition variable. It is a real rendezvous: every wait returns only
 * once both parties have arrived, and phases are separated by a generation counter. No timed sleeps
 * are used anywhere in these tests.
 */
typedef struct test_barrier_t {
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	unsigned int generation;
	unsigned int arrived;
} test_barrier_t;

static void
test_barrier_init(test_barrier_t* barrier) {
	assert(pthread_mutex_init(&barrier->mutex, 0) == 0);
	assert(pthread_cond_init(&barrier->cond, 0) == 0);
	barrier->generation = 0;
	barrier->arrived = 0;
}

static void
test_barrier_wait(test_barrier_t* barrier) {
	pthread_mutex_lock(&barrier->mutex);
	unsigned int generation = barrier->generation;
	if (++barrier->arrived == 2) {
		barrier->arrived = 0;
		++barrier->generation;
		pthread_cond_broadcast(&barrier->cond);
	} else {
		while (barrier->generation == generation)
			pthread_cond_wait(&barrier->cond, &barrier->mutex);
	}
	pthread_mutex_unlock(&barrier->mutex);
}

static void
test_barrier_destroy(test_barrier_t* barrier) {
	pthread_cond_destroy(&barrier->cond);
	pthread_mutex_destroy(&barrier->mutex);
}

/* ------------------------------------------------------------------ */
/* Controlled memory mapping                                          */
/* ------------------------------------------------------------------ */

/*
 * Every region handed out by the custom map function is tracked here so the test can prove that a
 * mapping backing a quarantined block stays alive (never unmapped and handed to a new object) while
 * the block is still in the queue.
 */
typedef struct test_mapping_t {
	void* base;
	size_t size;
	int live;
	struct test_mapping_t* next;
} test_mapping_t;

static test_mapping_t* test_mappings;
static pthread_mutex_t test_mapping_lock = PTHREAD_MUTEX_INITIALIZER;

static test_mapping_t*
test_mapping_find(void* address) {
	uintptr_t addr = (uintptr_t)address;
	for (test_mapping_t* mapping = test_mappings; mapping; mapping = mapping->next) {
		uintptr_t base = (uintptr_t)mapping->base;
		if ((addr >= base) && (addr < (base + mapping->size)))
			return mapping;
	}
	return 0;
}

static int
test_mapping_live(void* address) {
	pthread_mutex_lock(&test_mapping_lock);
	test_mapping_t* mapping = test_mapping_find(address);
	int live = mapping && mapping->live;
	pthread_mutex_unlock(&test_mapping_lock);
	return live;
}

static void*
test_memory_map(size_t size, size_t alignment, size_t* offset, size_t* mapped_size) {
	size_t map_size = size + alignment;
	void* ptr = mmap(0, map_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (ptr == MAP_FAILED)
		return 0;
	*offset = 0;
	if (alignment) {
		uintptr_t unalign = (uintptr_t)ptr & (uintptr_t)(alignment - 1);
		size_t padding = unalign ? (alignment - unalign) : 0;
		if (padding)
			munmap(ptr, padding);
		ptr = (void*)((char*)ptr + padding);
		if (alignment - padding)
			munmap((void*)((char*)ptr + size), alignment - padding);
		map_size = size;
	}
	*mapped_size = map_size;

	test_mapping_t* mapping = (test_mapping_t*)mmap(0, sizeof(test_mapping_t), PROT_READ | PROT_WRITE,
	                                               MAP_PRIVATE | MAP_ANON, -1, 0);
	assert(mapping != MAP_FAILED);
	mapping->base = ptr;
	mapping->size = map_size;
	mapping->live = 1;
	pthread_mutex_lock(&test_mapping_lock);
	mapping->next = test_mappings;
	test_mappings = mapping;
	pthread_mutex_unlock(&test_mapping_lock);
	return ptr;
}

static void
test_memory_unmap(void* address, size_t offset, size_t mapped_size) {
	void* base = (void*)((char*)address - (ptrdiff_t)offset);
	pthread_mutex_lock(&test_mapping_lock);
	test_mapping_t* mapping = test_mapping_find(base);
	/* Quarantined blocks keep their span and pages occupied: nothing still queued may be unmapped */
	if (mapping) {
		assert(mapping->live);
		mapping->live = 0;
	}
	pthread_mutex_unlock(&test_mapping_lock);
	munmap(base, mapped_size);
}

static int
test_memory_commit(void* address, size_t size) {
	(void)address;
	(void)size;
	return 0;
}

static int
test_memory_decommit(void* address, size_t size) {
	(void)address;
	(void)size;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Error callback and shared diagnostics                              */
/* ------------------------------------------------------------------ */

#define TEST_EVENTS_MAX 256

typedef struct test_event_t {
	void* ptr;
	size_t usable;
	size_t offset;
	unsigned char expected;
	unsigned char actual;
	int duplicate;
} test_event_t;

typedef struct test_state_t {
	test_event_t event[TEST_EVENTS_MAX];
	size_t event_count;
	size_t damage_count;
	size_t duplicate_count;
} test_state_t;

static test_state_t test_state;

static void
test_reset_events(void) {
	memset(&test_state, 0, sizeof(test_state));
}

static void
test_quarantine_error(void* ptr, size_t usable_size, size_t offset, unsigned char expected,
                      unsigned char actual, void* userdata) {
	assert(userdata == (void*)&test_state);
	test_state_t* state = (test_state_t*)userdata;
	if (offset == TEST_DUPLICATE_OFFSET) {
		++state->duplicate_count;
		if (state->event_count < TEST_EVENTS_MAX)
			state->event[state->event_count++] =
			    (test_event_t){ptr, usable_size, offset, expected, actual, 1};
		return;
	}
	++state->damage_count;
	if (state->event_count < TEST_EVENTS_MAX)
		state->event[state->event_count++] =
		    (test_event_t){ptr, usable_size, offset, expected, actual, 0};
}

static void
test_initialize(size_t quarantine_max_bytes) {
	test_reset_events();
	test_mappings = 0;

	/*
	 * rpmalloc_initialize keeps the interface pointer (it does not copy the struct), so both the
	 * interface and config must outlive this helper. Static storage duration keeps them valid until
	 * rpmalloc_finalize.
	 */
	static rpmalloc_interface_t iface;
	static rpmalloc_config_t config;
	memset(&iface, 0, sizeof(iface));
	iface.memory_map = test_memory_map;
	iface.memory_unmap = test_memory_unmap;
	iface.memory_commit = test_memory_commit;
	iface.memory_decommit = test_memory_decommit;
	iface.quarantine_error = test_quarantine_error;
	iface.quarantine_userdata = &test_state;

	memset(&config, 0, sizeof(config));
	config.unmap_on_finalize = 1;
	config.quarantine_max_bytes = quarantine_max_bytes;

	assert(rpmalloc_initialize_config(&iface, &config) == 0);
	assert(rpmalloc_config()->quarantine_max_bytes == quarantine_max_bytes);
	rpmalloc_thread_initialize();
}

static void
test_finalize(void) {
	rpmalloc_thread_finalize();
	rpmalloc_finalize();
	test_mappings = 0;
}

/* A fail() helper for assertions inside the contract functions, which return void */
static void
test_fail(const char* message) {
	fprintf(stderr, "quarantine contract failed: %s\n", message);
	abort();
}

static void
test_expect(int condition, const char* message) {
	if (!condition)
		test_fail(message);
}

/* ------------------------------------------------------------------ */
/* retention group                                                    */
/* ------------------------------------------------------------------ */

static void
test_retention_disabled_by_default(void) {
	/* Explicit zero limit: frees take the ordinary path, nothing is queued or reported */
	test_initialize(0);
	rpmalloc_quarantine_stats_t stats = {99, 99};
	rpmalloc_quarantine_statistics(&stats);
	test_expect(stats.block_count == 0 && stats.byte_count == 0, "disabled quarantine reported occupancy");
	test_expect(rpmalloc_quarantine_flush() == 0, "flush with quarantine disabled reported damage");

	void* p = rpmalloc(128);
	test_expect(p != 0, "allocation failed");
	rpfree(p);
	rpmalloc_quarantine_statistics(&stats);
	test_expect(stats.block_count == 0 && stats.byte_count == 0, "free entered a disabled quarantine");
	test_expect(test_state.event_count == 0, "callback fired with quarantine disabled");
	test_finalize();
}

static void
test_retention_holds_block_and_address(void) {
	/* Limit large enough that the freed blocks stay queued for the whole test */
	test_initialize(1 << 20);
	size_t size = 200;
	void* p = rpmalloc(size);
	test_expect(p != 0, "allocation failed");
	size_t usable = rpmalloc_usable_size(p);
	test_expect(usable >= size, "usable size smaller than request");

	rpfree(p);

	rpmalloc_quarantine_stats_t stats;
	rpmalloc_quarantine_statistics(&stats);
	test_expect(stats.block_count == 1, "freed block not counted in quarantine");
	test_expect(stats.byte_count == usable, "quarantine byte accounting does not match usable size");
	test_expect(test_mapping_live(p), "mapping of quarantined block was released");

	/* Repeated same-size allocations must not hand back the quarantined address */
	void* others[32];
	for (size_t i = 0; i < 32; ++i) {
		others[i] = rpmalloc(size);
		test_expect(others[i] != 0, "allocation failed");
		test_expect(others[i] != p, "quarantined address was reused while still queued");
	}
	for (size_t i = 0; i < 32; ++i)
		rpfree(others[i]);
	rpmalloc_quarantine_statistics(&stats);
	test_expect(stats.block_count >= 33, "quarantine lost blocks during reuse pressure");
	test_expect(test_state.damage_count == 0 && test_state.duplicate_count == 0,
	            "clean frees reported errors");

	test_expect(rpmalloc_quarantine_flush() == 0, "undamaged quarantined blocks reported damage");
	rpmalloc_quarantine_statistics(&stats);
	test_expect(stats.block_count == 0 && stats.byte_count == 0, "flush did not empty the quarantine");
	test_finalize();
}

static void
test_retention_fifo_eviction(void) {
	/* A small limit forces head eviction: a block larger than the limit is released at once */
	test_initialize(256);
	void* big = rpmalloc(4096);
	test_expect(big != 0, "allocation failed");
	size_t big_usable = rpmalloc_usable_size(big);
	test_expect(big_usable > 256, "test block did not exceed the quarantine limit");
	rpfree(big);
	rpmalloc_quarantine_stats_t stats;
	rpmalloc_quarantine_statistics(&stats);
	test_expect(stats.block_count == 0, "oversized block was retained instead of released at once");
	test_expect(test_state.damage_count == 0, "immediately released clean block reported damage");

	/* FIFO: fill past the limit, the oldest blocks leave the queue first */
	void* blocks[16];
	size_t usables[16];
	for (size_t i = 0; i < 16; ++i) {
		blocks[i] = rpmalloc(64);
		test_expect(blocks[i] != 0, "allocation failed");
		usables[i] = rpmalloc_usable_size(blocks[i]);
		rpfree(blocks[i]);
	}
	rpmalloc_quarantine_statistics(&stats);
	test_expect(stats.byte_count <= 256, "quarantine exceeded its byte limit");
	test_expect(stats.block_count < 16, "FIFO eviction did not run");
	test_expect(test_state.damage_count == 0, "clean evicted blocks reported damage");
	test_finalize();
}

static void
test_retention_statistics_exclude_live_blocks(void) {
	test_initialize(1 << 20);
	void* live[8];
	for (size_t i = 0; i < 8; ++i) {
		live[i] = rpmalloc(128);
		test_expect(live[i] != 0, "allocation failed");
	}
	void* dead[4];
	for (size_t i = 0; i < 4; ++i) {
		dead[i] = rpmalloc(128);
		test_expect(dead[i] != 0, "allocation failed");
	}
	size_t dead_bytes = 0;
	for (size_t i = 0; i < 4; ++i) {
		dead_bytes += rpmalloc_usable_size(dead[i]);
		rpfree(dead[i]);
	}
	rpmalloc_quarantine_stats_t stats;
	rpmalloc_quarantine_statistics(&stats);
	test_expect(stats.block_count == 4, "statistics count does not match queued blocks only");
	test_expect(stats.byte_count == dead_bytes, "statistics must count usable bytes of queued blocks");
	for (size_t i = 0; i < 8; ++i)
		rpfree(live[i]);
	rpmalloc_quarantine_statistics(&stats);
	test_expect(stats.block_count == 12, "live blocks were counted before being freed");
	test_expect(test_state.damage_count == 0, "clean frees reported damage");
	test_finalize();
}

/* ------------------------------------------------------------------ */
/* poison group                                                       */
/* ------------------------------------------------------------------ */

static void
test_poison_detects_single_changed_byte(void) {
	test_initialize(1 << 20);
	void* p = rpmalloc(128);
	test_expect(p != 0, "allocation failed");
	size_t usable = rpmalloc_usable_size(p);
	const size_t offset = 37;
	test_expect(offset < usable, "test offset outside usable area");
	rpfree(p);

	/* The whole usable area is poisoned while the block is queued */
	test_expect(((unsigned char*)p)[offset] == TEST_POISON, "block was not poisoned on quarantine entry");
	test_expect(((unsigned char*)p)[0] == TEST_POISON, "first user byte was not poisoned");
	test_expect(((unsigned char*)p)[usable - 1] == TEST_POISON, "last usable byte was not poisoned");

	/* One real write after free, on a mapping that is still alive */
	((unsigned char*)p)[offset] = 0x7A;

	size_t damaged = rpmalloc_quarantine_flush();
	test_expect(damaged == 1, "flush did not report the single damaged block");
	test_expect(test_state.damage_count == 1, "damage callback count mismatch");
	test_expect(test_state.event_count == 1, "unexpected number of callback events");
	test_event_t* event = &test_state.event[0];
	test_expect(event->ptr == p, "callback reported wrong pointer");
	test_expect(event->usable == usable, "callback reported wrong usable size");
	test_expect(event->offset == offset, "callback reported wrong offset");
	test_expect(event->expected == TEST_POISON, "callback reported wrong expected byte");
	test_expect(event->actual == 0x7A, "callback reported wrong actual byte");
	test_finalize();
}

static void
test_poison_reports_only_first_byte(void) {
	test_initialize(1 << 20);
	void* p = rpmalloc(256);
	test_expect(p != 0, "allocation failed");
	size_t usable = rpmalloc_usable_size(p);
	rpfree(p);

	/* Several corrupted bytes: exactly one event for the block, at the lowest offset */
	const size_t first = 11;
	((unsigned char*)p)[first] = 0x01;
	((unsigned char*)p)[first + 5] = 0x02;
	((unsigned char*)p)[usable - 2] = 0x03;

	size_t damaged = rpmalloc_quarantine_flush();
	test_expect(damaged == 1, "a damaged block must count once however many bytes changed");
	test_expect(test_state.event_count == 1, "more than one event for a single damaged block");
	test_expect(test_state.event[0].offset == first, "reported offset is not the first differing byte");
	test_expect(test_state.event[0].actual == 0x01, "reported actual byte mismatch");
	test_finalize();
}

static void
test_poison_aligned_block_internal_bytes_untouched(void) {
	test_initialize(1 << 20);
	const size_t alignment = 256;
	void* p = rpaligned_alloc(alignment, 400);
	test_expect(p != 0 && (((uintptr_t)p % alignment) == 0), "aligned allocation failed");
	size_t usable = rpmalloc_usable_size(p);
	rpfree(p);

	/* Poisoning covers the user pointer area, and the callback reports only user-relative offsets */
	((unsigned char*)p)[usable - 1] = 0x55;
	size_t damaged = rpmalloc_quarantine_flush();
	test_expect(damaged == 1, "aligned block damage not detected");
	test_expect(test_state.event[0].ptr == p, "callback pointer must be the user pointer");
	test_expect(test_state.event[0].offset == usable - 1, "offset must be relative to user pointer");
	test_expect(test_state.event[0].usable == usable, "usable size mismatch on aligned block");

	/* A clean aligned block reports nothing and its internal header bytes are not user bytes */
	void* q = rpaligned_alloc(alignment, 1000);
	test_expect(q != 0, "aligned allocation failed");
	rpfree(q);
	test_expect(rpmalloc_quarantine_flush() == 0, "clean aligned block reported damage");
	test_finalize();
}

static void
test_poison_duplicate_free(void) {
	test_initialize(1 << 20);
	void* p = rpmalloc(128);
	test_expect(p != 0, "allocation failed");
	rpfree(p);

	/* A second free while the pointer is still queued: no requeue, no counter pollution */
	rpmalloc_quarantine_stats_t before;
	rpmalloc_quarantine_statistics(&before);
	rpfree(p);
	rpmalloc_quarantine_stats_t after;
	rpmalloc_quarantine_statistics(&after);
	test_expect(after.block_count == before.block_count, "duplicate free changed block count");
	test_expect(after.byte_count == before.byte_count, "duplicate free changed byte count");
	test_expect(test_state.duplicate_count == 1, "duplicate free not reported");
	test_expect(test_state.damage_count == 0, "duplicate free reported as poison damage");
	test_expect(test_state.event[0].ptr == p, "duplicate event has wrong pointer");
	test_expect(test_state.event[0].duplicate == 1, "event not marked as duplicate");
	test_expect(test_state.event[0].offset == TEST_DUPLICATE_OFFSET, "duplicate event offset wrong");

	/* A third free is still a duplicate, and still does not requeue */
	rpfree(p);
	test_expect(test_state.duplicate_count == 2, "second duplicate free not reported");
	rpmalloc_quarantine_statistics(&after);
	test_expect(after.block_count == before.block_count, "repeated duplicate free changed block count");
	test_finalize();
}

static void
test_poison_huge_block(void) {
	/* Limit above the block size so the huge block stays queued instead of being released at once */
	test_initialize(16 << 20);
	/* Above the largest size class, this allocates a huge block with its own span */
	const size_t size = 3 * 1024 * 1024;
	void* p = rpmalloc(size);
	test_expect(p != 0, "huge allocation failed");
	size_t usable = rpmalloc_usable_size(p);
	test_expect(usable >= size, "huge usable size smaller than request");
	rpfree(p);

	rpmalloc_quarantine_stats_t stats;
	rpmalloc_quarantine_statistics(&stats);
	test_expect(stats.block_count == 1, "huge block not quarantined");
	test_expect(stats.byte_count == usable, "huge block byte accounting mismatch");
	test_expect(test_mapping_live(p), "huge block mapping released while quarantined");

	const size_t offset = 1024 * 1024 + 7;
	((unsigned char*)p)[offset] = 0x9C;
	size_t damaged = rpmalloc_quarantine_flush();
	test_expect(damaged == 1, "huge block damage not detected");
	test_expect(test_state.event[0].offset == offset, "huge block wrong damage offset");
	test_finalize();
}

/* ------------------------------------------------------------------ */
/* remote group                                                       */
/* ------------------------------------------------------------------ */

typedef struct remote_free_arg_t {
	test_barrier_t* barrier;
	void* block;
} remote_free_arg_t;

static void*
remote_free_worker(void* arg) {
	remote_free_arg_t* rarg = (remote_free_arg_t*)arg;
	rpmalloc_thread_initialize();
	/* Tell the main thread the worker is initialized and parked on the barrier */
	test_barrier_wait(rarg->barrier);
	/* Main thread releases the block after this wait returns */
	test_barrier_wait(rarg->barrier);
	rpfree(rarg->block);
	test_barrier_wait(rarg->barrier);
	rpmalloc_thread_finalize();
	return 0;
}

static void
test_remote_free_quarantine(void) {
	test_initialize(1 << 20);
	void* p = rpmalloc(256);
	test_expect(p != 0, "allocation failed");
	size_t usable = rpmalloc_usable_size(p);

	test_barrier_t barrier;
	test_barrier_init(&barrier);
	pthread_t thread;
	remote_free_arg_t arg = {&barrier, p};
	test_expect(pthread_create(&thread, 0, remote_free_worker, &arg) == 0, "thread create failed");

	test_barrier_wait(&barrier);  /* worker initialized */
	test_barrier_wait(&barrier);  /* release the cross-thread free */
	test_barrier_wait(&barrier);  /* worker has called rpfree */
	test_expect(pthread_join(thread, 0) == 0, "thread join failed");
	test_barrier_destroy(&barrier);

	/* The block freed by another thread is quarantined exactly once, on the global FIFO */
	rpmalloc_quarantine_stats_t stats;
	rpmalloc_quarantine_statistics(&stats);
	test_expect(stats.block_count == 1, "remote free did not enter the quarantine");
	test_expect(stats.byte_count == usable, "remote free byte accounting mismatch");
	test_expect(test_state.event_count == 0, "remote free reported an error");

	/* A local duplicate free of the remotely quarantined pointer is still a duplicate */
	rpfree(p);
	test_expect(test_state.duplicate_count == 1, "duplicate after remote free not reported");
	rpmalloc_quarantine_statistics(&stats);
	test_expect(stats.block_count == 1, "duplicate remote free requeued the block");

	((unsigned char*)p)[5] = 0x31;
	test_expect(rpmalloc_quarantine_flush() == 1, "remote block damage not detected");
	/* event[0] is the duplicate free above, the damage event is the one marked non-duplicate */
	test_expect(test_state.damage_count == 1, "remote damage callback count mismatch");
	for (size_t i = 0; i < test_state.event_count; ++i) {
		if (!test_state.event[i].duplicate) {
			test_expect(test_state.event[i].ptr == p && test_state.event[i].offset == 5,
			            "remote block damage reported with wrong details");
			test_expect(test_state.event[i].actual == 0x31, "remote block wrong actual byte");
		}
	}
	test_finalize();
}

typedef struct remote_damage_arg_t {
	test_barrier_t* barrier;
	void* block;
	size_t offset;
} remote_damage_arg_t;

static void*
remote_damage_worker(void* arg) {
	remote_damage_arg_t* rarg = (remote_damage_arg_t*)arg;
	rpmalloc_thread_initialize();
	test_barrier_wait(rarg->barrier);  /* parked until the main thread queued the block */
	((unsigned char*)rarg->block)[rarg->offset] = 0x6B;
	test_barrier_wait(rarg->barrier);  /* damage done */
	rpmalloc_thread_finalize();
	return 0;
}

static void
test_remote_damage_detection(void) {
	test_initialize(1 << 20);
	void* p = rpmalloc(512);
	test_expect(p != 0, "allocation failed");
	const size_t offset = 63;
	rpfree(p);

	test_barrier_t barrier;
	test_barrier_init(&barrier);
	pthread_t thread;
	remote_damage_arg_t arg = {&barrier, p, offset};
	test_expect(pthread_create(&thread, 0, remote_damage_worker, &arg) == 0, "thread create failed");
	test_barrier_wait(&barrier);
	test_barrier_wait(&barrier);
	test_expect(pthread_join(thread, 0) == 0, "thread join failed");
	test_barrier_destroy(&barrier);

	test_expect(rpmalloc_quarantine_flush() == 1, "cross-thread write after free not detected");
	test_expect(test_state.event[0].ptr == p && test_state.event[0].offset == offset,
	            "cross-thread damage reported with wrong details");
	test_finalize();
}

static void*
remote_owner_worker(void* arg) {
	(void)arg;
	rpmalloc_thread_initialize();
	/* Allocate and free entirely on this thread, then exit while the block is still queued */
	void* p = rpmalloc(256);
	assert(p);
	*(volatile unsigned char*)p = 0x42;
	rpfree(p);
	rpmalloc_thread_finalize();
	return 0;
}

static void
test_remote_thread_exit_keeps_quarantine(void) {
	test_initialize(1 << 20);
	pthread_t thread;
	test_expect(pthread_create(&thread, 0, remote_owner_worker, 0) == 0, "thread create failed");
	test_expect(pthread_join(thread, 0) == 0, "thread join failed");

	/* The owning thread is gone: its finalized heap and collect must not have reused the block */
	rpmalloc_quarantine_stats_t stats;
	rpmalloc_quarantine_statistics(&stats);
	test_expect(stats.block_count == 1, "quarantined block lost on owning thread exit");
	test_expect(test_state.damage_count == 0 && test_state.duplicate_count == 0,
	            "thread exit reported a spurious error");
	test_expect(rpmalloc_quarantine_flush() == 0, "block damaged by thread finalization");
	test_finalize();
}

/* ------------------------------------------------------------------ */
/* heaps group                                                        */
/* ------------------------------------------------------------------ */

static void
test_heap_free_and_realloc_rules(void) {
#if !RPMALLOC_FIRST_CLASS_HEAPS
	test_expect(0, "first class heaps not compiled in");
#else
	test_initialize(1 << 20);
	rpmalloc_heap_t* heap = rpmalloc_heap_acquire();
	test_expect(heap != 0, "heap acquire failed");

	/* heap_free follows the same quarantine rules */
	void* p = rpmalloc_heap_alloc(heap, 300);
	test_expect(p != 0, "heap allocation failed");
	size_t usable = rpmalloc_usable_size(p);
	rpmalloc_heap_free(heap, p);
	rpmalloc_quarantine_stats_t stats;
	rpmalloc_quarantine_statistics(&stats);
	test_expect(stats.block_count == 1 && stats.byte_count == usable, "heap_free did not quarantine");
	((unsigned char*)p)[9] = 0x27;
	test_expect(rpmalloc_quarantine_flush() == 1, "heap block damage not detected");

	/* In-place realloc is not quarantined */
	void* q = rpmalloc_heap_alloc(heap, 64);
	test_expect(q != 0, "heap allocation failed");
	memset(q, 0x5A, 64);
	void* q2 = rpmalloc_heap_realloc(heap, q, 128, 0);
	test_expect(q2 != 0, "heap realloc failed");
	if (q2 == q) {
		rpmalloc_quarantine_statistics(&stats);
		test_expect(stats.block_count == 0, "in-place realloc quarantined the block");
		for (int i = 0; i < 64; ++i)
			test_expect(((unsigned char*)q2)[i] == 0x5A, "in-place realloc lost content");
	}
	void* old = q2;

	/* Moving realloc: content is copied first, then the old block is quarantined */
	void* moved = rpmalloc_heap_realloc(heap, old, 4096, 0);
	test_expect(moved != 0, "heap moving realloc failed");
	for (int i = 0; i < 64; ++i)
		test_expect(((unsigned char*)moved)[i] == 0x5A, "moving realloc did not preserve content");
	rpmalloc_quarantine_statistics(&stats);
	test_expect(stats.block_count >= 1, "old block of moving realloc not quarantined");
	/* The old block is poisoned, its copied content is gone from the old address */
	test_expect(((unsigned char*)old)[0] == TEST_POISON, "old realloc block was not poisoned");
	rpmalloc_quarantine_flush();
	rpfree(moved);
	rpmalloc_quarantine_flush();

	rpmalloc_heap_release(heap);
	test_finalize();
#endif
}

static void
test_heap_free_all_drains_own_records_only(void) {
#if !RPMALLOC_FIRST_CLASS_HEAPS
	test_expect(0, "first class heaps not compiled in");
#else
	test_initialize(1 << 22);
	rpmalloc_heap_t* heap_a = rpmalloc_heap_acquire();
	rpmalloc_heap_t* heap_b = rpmalloc_heap_acquire();
	test_expect(heap_a && heap_b && heap_a != heap_b, "heap acquire failed");

	void* a_blocks[8];
	void* b_blocks[8];
	size_t a_bytes = 0;
	size_t b_bytes = 0;
	for (size_t i = 0; i < 8; ++i) {
		a_blocks[i] = rpmalloc_heap_alloc(heap_a, 200);
		b_blocks[i] = rpmalloc_heap_alloc(heap_b, 200);
		test_expect(a_blocks[i] && b_blocks[i], "heap allocation failed");
	}
	for (size_t i = 0; i < 8; ++i) {
		a_bytes += rpmalloc_usable_size(a_blocks[i]);
		b_bytes += rpmalloc_usable_size(b_blocks[i]);
		rpmalloc_heap_free(heap_a, a_blocks[i]);
		rpmalloc_heap_free(heap_b, b_blocks[i]);
	}
	rpmalloc_quarantine_stats_t stats;
	rpmalloc_quarantine_statistics(&stats);
	test_expect(stats.block_count == 16 && stats.byte_count == (a_bytes + b_bytes),
	            "heap blocks not all quarantined");

	/* Damage one block of each heap, both must still be reported despite the drain below */
	((unsigned char*)a_blocks[2])[4] = 0x41;
	((unsigned char*)b_blocks[5])[6] = 0x42;

	/* Destroying heap A validates and removes only its own records */
	rpmalloc_heap_free_all(heap_a);
	rpmalloc_quarantine_statistics(&stats);
	test_expect(stats.block_count == 8, "heap_free_all removed records of another heap");
	test_expect(stats.byte_count == b_bytes, "heap_free_all byte accounting wrong");
	test_expect(test_state.damage_count == 1, "heap A damage not validated on free_all");
	test_expect(test_state.event[0].ptr == a_blocks[2], "heap A free_all validated wrong block");

	/* heap B blocks survive untouched until their own heap is destroyed */
	test_expect(((unsigned char*)b_blocks[5])[6] == 0x42, "heap B block touched by heap A teardown");
	rpmalloc_heap_free_all(heap_b);
	rpmalloc_quarantine_statistics(&stats);
	test_expect(stats.block_count == 0 && stats.byte_count == 0, "heap B records survived free_all");
	test_expect(test_state.damage_count == 2, "heap B damage not validated on free_all");

	rpmalloc_heap_release(heap_a);
	rpmalloc_heap_release(heap_b);
	test_finalize();
#endif
}

static void
test_heap_separate_lifecycles(void) {
#if !RPMALLOC_FIRST_CLASS_HEAPS
	test_expect(0, "first class heaps not compiled in");
#else
	test_initialize(1 << 22);
	rpmalloc_heap_t* heap_a = rpmalloc_heap_acquire();
	rpmalloc_heap_t* heap_b = rpmalloc_heap_acquire();
	test_expect(heap_a && heap_b, "heap acquire failed");

	/* Interleave the lifetimes: tear heap A down while heap B keeps queued blocks */
	void* a = rpmalloc_heap_alloc(heap_a, 512);
	void* b1 = rpmalloc_heap_alloc(heap_b, 512);
	test_expect(a && b1, "heap allocation failed");
	rpmalloc_heap_free(heap_a, a);
	rpmalloc_heap_free(heap_b, b1);
	rpmalloc_heap_free_all(heap_a);
	rpmalloc_heap_release(heap_a);

	rpmalloc_quarantine_stats_t stats;
	rpmalloc_quarantine_statistics(&stats);
	test_expect(stats.block_count == 1, "second heap quarantine disturbed by first heap teardown");

	/* A fresh heap can be acquired and used while heap B records remain queued */
	rpmalloc_heap_t* heap_c = rpmalloc_heap_acquire();
	test_expect(heap_c != 0, "heap acquire after teardown failed");
	void* c = rpmalloc_heap_alloc(heap_c, 512);
	test_expect(c != 0, "allocation on fresh heap failed");
	test_expect(c != b1, "reused heap handed out a quarantined address");
	rpfree(c);
	rpmalloc_quarantine_statistics(&stats);
	test_expect(stats.block_count == 2, "new heap free did not quarantine");

	void* b2 = rpmalloc_heap_alloc(heap_b, 512);
	test_expect(b2 != 0 && b2 != b1, "heap B reused its own quarantined block");
	rpmalloc_heap_free(heap_b, b2);
	rpmalloc_heap_free_all(heap_b);
	rpmalloc_heap_release(heap_b);
	rpmalloc_heap_release(heap_c);
	test_expect(test_state.damage_count == 0 && test_state.duplicate_count == 0,
	            "heap lifecycle test reported spurious errors");
	test_finalize();
#endif
}

/* ------------------------------------------------------------------ */
/* Registration                                                       */
/* ------------------------------------------------------------------ */

void
register_contract(void) {
	contract_add("retention", "disabled_by_default", test_retention_disabled_by_default);
	contract_add("retention", "holds_block_and_address", test_retention_holds_block_and_address);
	contract_add("retention", "fifo_eviction", test_retention_fifo_eviction);
	contract_add("retention", "statistics_exclude_live", test_retention_statistics_exclude_live_blocks);

	contract_add("poison", "detects_single_byte", test_poison_detects_single_changed_byte);
	contract_add("poison", "reports_only_first_byte", test_poison_reports_only_first_byte);
	contract_add("poison", "aligned_internal_bytes", test_poison_aligned_block_internal_bytes_untouched);
	contract_add("poison", "duplicate_free", test_poison_duplicate_free);
	contract_add("poison", "huge_block", test_poison_huge_block);

	contract_add("remote", "remote_free_quarantine", test_remote_free_quarantine);
	contract_add("remote", "remote_damage_detection", test_remote_damage_detection);
	contract_add("remote", "thread_exit_keeps_quarantine", test_remote_thread_exit_keeps_quarantine);

	contract_add("heaps", "free_and_realloc_rules", test_heap_free_and_realloc_rules);
	contract_add("heaps", "free_all_own_records_only", test_heap_free_all_drains_own_records_only);
	contract_add("heaps", "separate_lifecycles", test_heap_separate_lifecycles);
}
