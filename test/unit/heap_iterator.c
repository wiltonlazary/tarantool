#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdbool.h>
#include <memory.h>

#include "unit.h"

struct test_type {
		uint32_t val1;
		uint32_t val2;
		char c;
};

int test_type_less(const struct test_type *a, const struct test_type *b) {
	return a->val1 < b->val1;
}


#define HEAP_NAME _test
#define HEAP_DATA_TYPE struct test_type
#define HEAP_LESS(a, b, arg) test_type_less(a, b)
#define HEAP_CMP_ARG_TYPE int

#include "salad/heap.h"

static void
test_iterator_create() {
	header();
	struct HEAP(node) *test_node, *root;
	struct test_type value = {4, 0, 0};

	root = HEAP(alloc)();
	HEAP(create)(root, value, 0);

	struct HEAP(iterator) it;
	HEAP(iterator_init)(&it, root);

	if (it.mask != 0) {
		fail("incorrect mask after create", "it.mask != 0");
	}
	if (it.current_node != root) {
		fail("incorrect current_node", "it.current_node != root");
	}
	if (it.depth != 0) {
		fail("incorrect depth", "it.depth != 0");
	}

	HEAP(free)(root);
	footer();
}

static void
test_iterator_small() {
	header();
	struct HEAP(node) *test_node, *root;
	struct test_type value = {4, 0, 0};

	for (uint32_t i = 3; i > 0; --i) {
		test_node = HEAP(alloc)();
		value.val1 = i;
		HEAP(create)(test_node, value, 0);
		root = HEAP(insert)(root, test_node);
	}

	struct HEAP(iterator) it;
	bool used_key[5];
	memset((void *)used_key, 0, sizeof(used_key));

	HEAP(iterator_init)(&it, root);
	test_node = NULL;
	for (uint32_t i = 0; i < 4; ++i) {
		test_node = HEAP(iterator_next)(&it);
		if (test_node == NULL) {
			fail("null returned from iterator", "test_node == NULL");
		}
		uint32_t val = test_node->value.val1;
		if (val < 1 || val > 5) {
			fail("from iterator returned incorrect value", "val < 1 || val > 5");
		}
		if (used_key[val]) {
			fail("from iterator some value returned twice", "used[val]");
		}
		used_key[val] = 1;
	}

	bool f = true;
	for (uint32_t i = 1; i < 5; ++i) {
		f = used_key[i] && f;
	}
	if (!f) {
		fail("some node was skipped", "!f");
	}

	test_node = HEAP(iterator_next)(&it);
	if (test_node) {
		fail("after all iterator returns not null", "test_node");
	}

	for (uint32_t i = 0; i < 4; ++i) {
		test_node = root;
		root = HEAP(pop)(root);
		HEAP(free)(test_node);
	}
}

static void
test_iterator_large() {
	header();
	uint32_t const TEST_CASE_SIZE = 1000;
	struct HEAP(node) *test_node = NULL, *root = NULL;
	struct test_type value = {0, 0, 0};

	for (uint32_t i = TEST_CASE_SIZE - 1; i > 0; --i) {
		test_node = HEAP(alloc)();
		value.val1 = i;
		HEAP(create)(test_node, value, 0);
		root = HEAP(insert)(root, test_node);
	}

	struct HEAP(iterator) it;
	bool used_key[TEST_CASE_SIZE];
	memset((void *)used_key, 0, sizeof(used_key));

	HEAP(iterator_init)(&it, root);
	test_node = NULL;
	for (uint32_t i = 1; i < TEST_CASE_SIZE; ++i) {
		test_node = HEAP(iterator_next)(&it);
		if (test_node == NULL) {
			fail("null returned from iterator", "test_node == NULL");
		}
		uint32_t val = test_node->value.val1;
		if (val == 0 || val >= TEST_CASE_SIZE) {
			fail("from iterator returned incorrect value", "val < 0 || val >= TEST_CASE_SIZE");
		}
		if (used_key[val]) {
			fail("from iterator some value returned twice", "used[val]");
		}
		used_key[val] = 1;
	}

	bool f = true;
	for (uint32_t i = 1; i < TEST_CASE_SIZE; ++i) {
		f = used_key[i] && f;
	}
	if (!f) {
		fail("some node was skipped", "!f");
	}

	test_node = HEAP(iterator_next)(&it);
	if (test_node) {
		fail("after all iterator returns not null", "test_node");
	}

	for (uint32_t i = 1; i < TEST_CASE_SIZE; ++i) {
		test_node = root;
		root = HEAP(pop)(root);
		HEAP(free)(test_node);
	}
	footer();
}


int
main(int argc, const char** argv)
{
	srand(179);
	test_iterator_create();
	test_iterator_large();
}
