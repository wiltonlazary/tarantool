#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdbool.h>
#include <memory.h>

#include "unit.h"

#define HEAP_NAME _test
#define HEAP_LESS(h, a, b) test_type_less(h, a, b)
#define HEAP_CMP_ARG_TYPE int

struct heap_test_core;
struct heap_test_node;

int test_type_less(const struct heap_test_core *heap,
			const struct heap_test_node *a,
			const struct heap_test_node *b);


#include "salad/heap.h"

struct test_type {
		uint32_t val1;
		uint32_t val2;
		char c;
		struct heap_test_node node;
};

int test_type_less(const struct heap_test_core *heap,
			const struct heap_test_node *a,
			const struct heap_test_node *b) {

	const struct test_type *left = (struct test_type *)((char *)a -
					offsetof(struct test_type, node));
	const struct test_type *right = (struct test_type *)((char *)b -
					offsetof(struct test_type, node));
	return left->val1 < right->val1;
}

void free_all_nodes(struct heap_test_core *p_heap) {
	struct test_type *root_value;
	while (p_heap->root) {
		root_value = (struct test_type *) ((char *)p_heap->root -
				offsetof(struct test_type, node));
		heap_test_pop(p_heap);
		free(root_value);
	}
}
static void
test_iterator_create() {
	header();
	struct test_type *value, *root_value;
	struct heap_test_core heap;
	heap_test_init_core(&heap, 0);

	value = (struct test_type *)malloc(sizeof(struct test_type));
	heap_test_init_node(&value->node);
	value->val1 = 0;
	heap_test_insert(&heap, &value->node);

	struct heap_test_iterator it;
	heap_test_iterator_init(&heap, &it);

	if (it.mask != 0) {
		fail("incorrect mask after create", "it.mask != 0");
	}
	if (it.current_node != &value->node) {
		fail("incorrect current_node", "it.current_node != root");
	}
	if (it.depth != 0) {
		fail("incorrect depth", "it.depth != 0");
	}

	free_all_nodes(&heap);

	footer();
}

static void
test_iterator_small() {
	header();
	struct test_type *value, *root_value;
	struct heap_test_node *test_node;
	struct heap_test_core heap;
	heap_test_init_core(&heap, 0);

	for (uint32_t i = 4; i > 0; --i) {
		value = (struct test_type *)malloc(sizeof(struct test_type));
		heap_test_init_node(&value->node);
		value->val1 = i;
		heap_test_insert(&heap, &value->node);
	}

	struct heap_test_iterator it;
	bool used_key[5];
	memset((void *)used_key, 0, sizeof(used_key));

	heap_test_iterator_init(&heap, &it);
	test_node = NULL;
	for (uint32_t i = 0; i < 4; ++i) {
		test_node = heap_test_iterator_next(&it);

		if (test_node == NULL) {
			fail("null returned from iterator",
				"test_node == NULL");
		}

		value = (struct test_type *)((char *)test_node -
					offsetof(struct test_type, node));
		uint32_t val = value->val1;
		if (val < 1 || val > 5) {
			fail("from iterator returned incorrect value",
				"val < 1 || val > 5");
		}
		if (used_key[val]) {
			fail("from iterator some value returned twice",
				"used[val]");
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

	test_node = heap_test_iterator_next(&it);
	if (test_node) {
		fail("after all iterator returns not null", "test_node");
	}

	free_all_nodes(&heap);
	footer();
}

static void
test_iterator_large() {
	header();
	uint32_t const TEST_CASE_SIZE = 1000;
	struct test_type *value, *root_value;
	struct heap_test_node *test_node;
	struct heap_test_core heap;
	heap_test_init_core(&heap, 0);

	for (uint32_t i = TEST_CASE_SIZE; i > 0; --i) {
		value = (struct test_type *)malloc(sizeof(struct test_type));
		heap_test_init_node(&value->node);
		value->val1 = i;
		heap_test_insert(&heap, &value->node);
	}

	struct heap_test_iterator it;
	bool used_key[TEST_CASE_SIZE + 1];
	memset((void *)used_key, 0, sizeof(used_key));

	heap_test_iterator_init(&heap, &it);
	test_node = NULL;
	for (uint32_t i = 0; i < TEST_CASE_SIZE; ++i) {
		test_node = heap_test_iterator_next(&it);

		if (test_node == NULL) {
			fail("null returned from iterator", "test_node == NULL");
		}

		value = (struct test_type *)((char *)test_node -
						offsetof(struct test_type, node));
		uint32_t val = value->val1;
		if (val == 0 || val > TEST_CASE_SIZE) {
			fail("from iterator returned incorrect value",
				"val < 0 || val > TEST_CASE_SIZE");
		}
		if (used_key[val]) {
			fail("from iterator some value returned twice",
				"used[val]");
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

	test_node = heap_test_iterator_next(&it);
	if (test_node) {
		fail("after all iterator returns not null", "test_node");
	}

	free_all_nodes(&heap);
	footer();
}


int
main(int argc, const char** argv)
{
	srand(179);
	test_iterator_create();
	test_iterator_small();
	test_iterator_large();
}
