#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdbool.h>

#include "trivia/util.h"
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
test_create()
{
	header();
	struct test_type value;
	value.val1 = 0;
	value.val2 = 0;
	value.c = 0;

	struct heap_test_node *test_node = &value.node;
	if (test_node->parent != NULL) {
		fail("parent is not null", "test_node->parent != NULL");
 	}
	if (test_node->left != NULL) {
		fail("check that left is null", "test_node->left != NULL");
 	}
	if (test_node->right != NULL) {
		fail("check that right is null", "test_node->right != NULL");
 	}
	if (test_node->right != NULL) {
		fail("check that right is null", "test_node->right != NULL");
 	}

	footer();
}

static void
test_insert_1_to_3()
{
	header();
	struct test_type *value, *root_value;
	struct heap_test_core heap;
	heap_test_init(&heap, 0);

	for (uint32_t i = 0; i < 4; ++i) {
		value = (struct test_type *)malloc(sizeof(struct test_type));
		value->val1 = i;
		heap_test_insert(&heap, &value->node);

		root_value = (struct test_type *)((char *)heap.root -
						offsetof(struct test_type, node));
		if (root_value->val1 != 0) {
			fail("check that min.val1 is incorrect",
				"root_value->val1 != 0");
		}
		if (!heap_test_check_invariants(&heap, 0, heap.root)) {
			fail("check heap invariants failed",
			"!heap_test_check_invariants(&heap, 0, heap.root)");
	 	}
	}

	free_all_nodes(&heap);

	footer();
}

static void
test_insert_3_to_1()
{
	header();
	struct test_type *value, *root_value;
	struct heap_test_core heap;
	heap_test_init(&heap, 0);

	for (uint32_t i = 3; i > 0; --i) {
		value = (struct test_type *)malloc(sizeof(struct test_type));
		value->val1 = i;
		heap_test_insert(&heap, &value->node);

		root_value = (struct test_type *)((char *)heap.root -
						offsetof(struct test_type, node));
		if (root_value->val1 != i) {
			fail("check that min.val1 is incorrect",
				"root_value->val1 != i");
		}
		if (!heap_test_check_invariants(&heap, 0, heap.root)) {
			fail("check heap invariants failed",
			"!heap_test_check_invariants(&heap, 0, heap.root");
	 	}
	}

	free_all_nodes(&heap);

	footer();
}

static void
test_insert_50_to_150_mod_100()
{
	header();
	struct test_type *value, *root_value;
	struct heap_test_core heap;
	heap_test_init(&heap, 0);

	for (uint32_t i = 50; i < 150; ++i) {
		value = (struct test_type *)malloc(sizeof(struct test_type));
		value->val1 = i % 100;
		heap_test_insert(&heap, &value->node);

		root_value = (struct test_type *)((char *)heap.root -
						offsetof(struct test_type, node));

		if (i < 100 && root_value->val1 != 50) {
			fail("min.val1 is incorrect",
			"i < 100 && root_value->val1 != 50");
		}
		if (i >= 100 && root_value->val1 != 0) {
			fail("min.val1 is incorrect",
			"i >= 100 && root_value->val1 != 0");
		}


		if (!heap_test_check_invariants(&heap, 0, heap.root)) {
			fail("check heap invariants failed",
			"!heap_test_check_invariants(&heap, 0, heap.root)");
		}
	}

	for (int i = 0; i < 100; ++i) {
		root_value = (struct test_type *) ((char *)heap.root -
				offsetof(struct test_type, node));
		heap_test_pop(&heap);
		free(root_value);
	}

	footer();
}

static void
test_insert_1000_random()
{
	header();
	const uint32_t TEST_CASE_SIZE = 1000;
	uint32_t ans = UINT_MAX;
	struct test_type *value, *root_value;
	struct heap_test_core heap;
	heap_test_init(&heap, 0);

	for (uint32_t i = 0; i < TEST_CASE_SIZE; ++i) {
		value = (struct test_type *)malloc(sizeof(struct test_type));
		value->val1 = rand();

		ans = (value->val1 < ans ? value->val1 : ans);
		heap_test_insert(&heap, &value->node);

		root_value = (struct test_type *)((char *)heap.root -
						offsetof(struct test_type, node));
		if (root_value->val1 != ans) {
			fail("min.val1 is incorrect", "root_value->val1 != ans");
		}
		if (heap.root->size != i + 1) {
			fail("check that size is correct failed", "root->size != i + 2");
		}

		if (!heap_test_check_invariants(&heap, 0, heap.root)) {
			fail("check heap invariants failed",
				"test_heap_check_invariants");
		}
	}

	free_all_nodes(&heap);
	footer();
}

static void
test_insert_10_to_1_pop()
{
	header();
	struct test_type *value, *root_value;
	struct heap_test_core heap;
	heap_test_init(&heap, 0);

	for (uint32_t i = 10; i > 0; --i) {
		value = (struct test_type *)malloc(sizeof(struct test_type));
		value->val1 = i;

		heap_test_insert(&heap, &value->node);
		root_value = (struct test_type *)((char *)heap.root -
					offsetof(struct test_type, node));
		if (root_value->val1 != i) {
		 	fail("check that min.val1 is correct failed",
	 			"root_value->val1 != i");
		}
		if (!heap_test_check_invariants(&heap, 0, heap.root)) {
			fail("check heap invariants failed",
				"heap_test_check_invariants(0, root)");
		}
	}

	for (uint32_t i = 1; i <= 10; ++i) {
		root_value = (struct test_type *)((char *)heap.root -
					offsetof(struct test_type, node));

		heap_test_pop(&heap);
		if (root_value->val1 != i) {
		 	fail("check that min.val1 is correct failed",
	 			"root_value->val1 != i");
		}
		if (!heap_test_check_invariants(&heap, 0, heap.root)) {
			fail("check heap invariants failed",
				"heap_test_check_invariants(0, root)");
		}
		free(root_value);
	}

	footer();
}

int uint32_compare(const void *a, const void *b) {
	const uint32_t *ua = (const uint32_t *)a;
	const uint32_t *ub = (const uint32_t *)b;
	if (*ua < *ub) {
		return -1;
	}
	else if (*ua > *ub) {
		return 1;
	}

	return 0;
}

static void
test_insert_10000_pop_10000_random() {
	header();
	const uint32_t TEST_CASE_SIZE = 10000;
	uint32_t ans = UINT_MAX;

	struct test_type *value, *root_value;
	struct heap_test_core heap;
	heap_test_init(&heap, 0);

	uint32_t keys_it = 0;
	uint32_t *keys = (uint32_t *)malloc(sizeof(uint32_t) * TEST_CASE_SIZE);
	if (keys == NULL) {
		fail("keys == NULL", "fail to alloc memory for keys array");
	}

	for (uint32_t i = 0; i < TEST_CASE_SIZE; ++i) {
		value = (struct test_type *)malloc(sizeof(struct test_type));
		keys[keys_it++] = value->val1 = rand();
		ans = (value->val1 < ans ? value->val1 : ans);

		heap_test_insert(&heap, &value->node);

		root_value = (struct test_type *)((char *)heap.root -
					offsetof(struct test_type, node));
		if (root_value->val1 != ans) {
		 	fail("check that min.val1 is correct failed",
	 			"root_value->val1 != ans");
		}
		if (!heap_test_check_invariants(&heap, 0, heap.root)) {
			fail("check heap invariants failed",
				"heap_test_check_invariants(0, root)");
		}
		if (heap_test_size(&heap) != i + 1) {
			fail("check that size is correct",
				"heap_test_size(root) != i + 1");
		}
	}

	qsort(keys, TEST_CASE_SIZE, sizeof(uint32_t), uint32_compare);
	bool f = true;
	for (uint32_t i = 0; i + 1 < TEST_CASE_SIZE; ++i) {
		f = f && (keys[i] < keys[i + 1]);
	}
	if(!f)  {
		fail("check that keys is sorted failed", "!f");
	}

	uint32_t full_size = heap_test_size(&heap);
	for (uint32_t i = 0; i < TEST_CASE_SIZE; ++i) {
		root_value = (struct test_type *)((char *)heap.root -
					offsetof(struct test_type, node));

		heap_test_pop(&heap);

		if (root_value->val1 != keys[i]) {
		 	fail("check that min.val1 is correct failed",
	 			"root_value->val1 != keys[i]");
		}
		if (!heap_test_check_invariants(&heap, 0, heap.root)) {
			fail("check heap invariants failed",
				"heap_test_check_invariants(0, root)");
		}
		if (heap_test_size(&heap) != full_size - 1 - i) {
			fail("check that size is correct",
				"heap_test_size(root) != full_size - 1 - i");
		}
		free(root_value);
	}

	free(keys);
	footer();
}

static void
test_insert_with_null() {
	header();

	struct test_type *value;
	struct heap_test_core heap;
	heap_test_init(&heap, 0);

	value = (struct test_type *)malloc(sizeof(struct test_type));

	heap_test_insert(&heap, &value->node);
	if (heap.root != &value->node) {
		fail("test insert into null failed", "result != value");
	 }

 	heap_test_insert(&heap, NULL);
 	if (heap.root != &value->node) {
 		fail("test insert null failed", "result != value");
  	}

	free(value);
	footer();
}

static void
test_insert_pop_workload() {
	header();
	const uint32_t TEST_CASE_SIZE = 10000;
	uint32_t ans = UINT_MAX;

	struct test_type *value, *root_value;
	struct heap_test_core heap;
	heap_test_init(&heap, 0);

	uint32_t current_size = 0;

	for(uint32_t i = 0; i < TEST_CASE_SIZE; ++i) {
		if (heap_test_size(&heap) == 0 || rand() % 5) {
			current_size++;
			value = (struct test_type *)
				malloc(sizeof(struct test_type));
			value->val1 = rand();
			heap_test_insert(&heap, &value->node);
		}
		else {
			current_size--;
			root_value = (struct test_type *)((char *)heap.root -
						offsetof(struct test_type, node));

			heap_test_pop(&heap);
			free(root_value);
		}

		if (!heap_test_check_invariants(&heap, 0, heap.root)) {
			fail("check heap invariants failed",
				"heap_test_check_invariants(0, root)");
		}
		if (heap_test_size(&heap) != current_size) {
			fail("check that size is correct",
				"heap_test_size(root) != current_size");
		}
	}

	free_all_nodes(&heap);
	footer();
}

static void
test_pop_last() {
	header();
	const uint32_t TEST_CASE_SIZE = 10000;
	uint32_t ans = UINT_MAX;

	struct test_type *value, *root_value;
	struct heap_test_core heap;
	heap_test_init(&heap, 0);

	value = (struct test_type *)malloc(sizeof(struct test_type));
	heap_test_insert(&heap, &value->node);

	heap_test_pop(&heap);
	if (heap.root != NULL) {
		fail("test delete last node failed", "heap.root != NULL");
	}

	free(value);
	footer();
}

static void
test_insert_update_workload() {
	header();
	uint32_t nodes_it = 0;
	uint64_t current_size = 0;
	const uint32_t TEST_CASE_SIZE = 10000;
	uint32_t ans = UINT_MAX;

	struct test_type *value, *root_value;
	struct heap_test_core heap;
	heap_test_init(&heap, 0);

	struct test_type **nodes = (struct test_type **)
		malloc(sizeof(struct test_type *) * TEST_CASE_SIZE);

	struct heap_test_node *test_node = NULL, *root = NULL;
	for(uint32_t i = 0; i < TEST_CASE_SIZE; ++i) {
		if (nodes_it == current_size ||
			heap_test_size(&heap) == 0 ||
			rand() % 5) {

			value = (struct test_type *)
				malloc(sizeof(struct test_type));
			value->val1 = rand();

			nodes[current_size++] = value;
			heap_test_insert(&heap, &value->node);
		}
		else {
			nodes[nodes_it]->val1 = rand() % 5;
			heap_test_update(&heap, &(nodes[nodes_it]->node));
			nodes_it++;
		}

		if (!heap_test_check_invariants(&heap, 0, heap.root)) {
			fail("check heap invariants failed",
				"heap_test_check_invariants(0, root)");
		}
		if (heap_test_size(&heap) != current_size) {
			fail("check that size is correct",
				"heap_test_size(root) != current_size");
		}
	}

	free_all_nodes(&heap);
	free(nodes);
	footer();
}


int
main(int argc, const char** argv)
{
	srand(179);
	test_create();
	test_insert_1_to_3();
	test_insert_3_to_1();
	test_insert_50_to_150_mod_100();
	test_insert_1000_random();
	test_insert_10_to_1_pop();
	test_insert_10000_pop_10000_random();
	test_insert_with_null();
	test_insert_pop_workload();
	test_pop_last();
	test_insert_update_workload();
}
