/*
 * Copyright 2010-2015, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "coro.h"

#include "trivia/config.h"
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include "small/slab_cache.h"
#include "third_party/valgrind/memcheck.h"
#include "diag.h"
#include "tt_pthread.h"

static pthread_once_t coro_once = PTHREAD_ONCE_INIT;
static size_t    coro_page_size;

/*
 * coro_stack_size, coro_stack_offset
 * coro_guard_size, coro_guard_offset
 *
 * Stack geometry: relative placement of stack section and guard
 * section, if any. Offsets are relative to the begining of an aligned
 * memory block to host both stack and guard side by side.
 *
 * Note: we assume that the memory comes from a slab allocator and
 * contains a slab header at the beginning we should not touch.
 */
static size_t    coro_stack_size;
static ptrdiff_t coro_stack_offset;
static size_t    coro_guard_size;
static ptrdiff_t coro_guard_offset;

#define STACK_GROWS_UP 0

static void
coro_configure()
{
	/*
	 * __TARANTOOL_FIBER_STACK__=16    the stack of 16 pages (defaults)
	 * __TARANTOOL_FIBER_STACK__=:16   default stack + 16 guard pages
	 * __TARANTOOL_FIBER_STACK__=8:32  8 page stack + 32 guard pages
	 */
	int page_size = sysconf(_SC_PAGESIZE);
	int stack_pages = 16;
	int guard_pages = 0;

	int value, value2;
	const char *param = getenv("__TARANTOOL_FIBER_STACK__");

	if (param == NULL)
		goto init;

	if (sscanf(param, ":%d", &value) == 1) {
		guard_pages = value;
		goto sanitize;
	}

	switch (sscanf(param, "%d:%d", &value, &value2)) {
	case 2:
		guard_pages = value2;
		/* fallthrough */
	case 1:
		stack_pages = value;
	}

sanitize:
	if (stack_pages < 1)
		stack_pages = 1;
	if (stack_pages > 4096)
		stack_pages = 4096;
	if (guard_pages < 0)
		guard_pages = 0;
	if (guard_pages > 4096)
		guard_pages = 4096;

init:
	coro_page_size = page_size;

	if (STACK_GROWS_UP || guard_pages == 0) {
		coro_stack_offset = slab_sizeof();
		coro_stack_size = page_size * stack_pages - slab_sizeof();
	} else {
		coro_stack_offset = page_size * (guard_pages + 1);
		coro_stack_size = page_size * stack_pages;
	}

	if (guard_pages != 0) {
		coro_guard_size = page_size * guard_pages;
#if STACK_GROWS_UP
		coro_guard_offset = page_size * stack_pages;
#else
		coro_guard_offset = page_size;
#endif
	}
}

static void
coro_guard_mprotect(char *stack, int prot)
{
	if (coro_guard_size == 0)
		return;

	char *guard = stack - coro_stack_offset + coro_guard_offset;
	if ((uintptr_t)guard % coro_page_size)
		panic("banana banana banana");
	mprotect(guard, coro_guard_size, prot);
}

int
tarantool_coro_create(struct tarantool_coro *coro,
		      struct slab_cache *slabc,
		      void (*f) (void *), void *data)
{
	tt_pthread_once(&coro_once, &coro_configure);

	memset(coro, 0, sizeof(*coro));

	size_t alloc_size = MAX(
		coro_stack_offset + coro_stack_size,
		coro_guard_offset + coro_guard_size);

	char *mem = (char *) slab_get(slabc, alloc_size);

	if (mem == NULL) {
		diag_set(OutOfMemory, alloc_size,
			 "runtime arena", "coro stack");
		return -1;
	}

	coro->stack = mem + coro_stack_offset;
	coro->stack_size = coro_stack_size;

	(void) VALGRIND_STACK_REGISTER(coro->stack, (char *)
				       coro->stack + coro->stack_size);

	coro_guard_mprotect(coro->stack, PROT_NONE);

	coro_create(&coro->ctx, f, data, coro->stack, coro->stack_size);
	return 0;
}

void
tarantool_coro_destroy(struct tarantool_coro *coro, struct slab_cache *slabc)
{
	if (coro->stack != NULL) {
		coro_guard_mprotect(coro->stack, PROT_READ|PROT_WRITE);
		slab_put(slabc, (struct slab *)
			 ((char *) coro->stack - coro_stack_offset));
	}
}
