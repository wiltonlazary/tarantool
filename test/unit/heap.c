#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdbool.h>

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
test_create()
{
	header();
	struct HEAP(node) *test_node = HEAP(alloc)();
	struct test_type value = {10, 11, 0};
	isnt(test_node, NULL, "check that alloc is not failed");
	HEAP(create)(test_node, value, 0);
	is(test_node->parent, NULL, "check that parent is null");
	is(test_node->left, NULL, "check that left is null");
	is(test_node->right, NULL, "check that right is null");
	is(test_node->right, NULL, "check that right is null");
	is(test_node->value.val1, 10, "check that val.val1 is correct");
	is(test_node->value.val2, 11, "check that val.val2 is correct");
	is(test_node->value.c, 0, "check that val.c is correct");

	footer();
}

static void
test_insert_1_to_3()
{
	header();
	struct HEAP(node) *test_node, *root;
	struct test_type value = {0, 0, 0};

	root = HEAP(alloc)();
	HEAP(create)(root, value, 0);

	for (uint32_t i = 0; i < 3; ++i) {
		test_node = HEAP(alloc)();
		value.val1 = i;
		HEAP(create)(test_node, value, 0);
		root = HEAP(insert)(root, test_node);
		is(HEAP(get_min)(root)->val1, 0, "check that min.val1 is correct");
		ok(HEAP(check_invariants)(0, root), "check heap invariants");
	}
	footer();
}

static void
test_insert_3_to_1()
{
	header();
	struct HEAP(node) *test_node, *root;
	struct test_type value = {4, 0, 0};

	root = HEAP(alloc)();
	HEAP(create)(root, value, 0);

	for (uint32_t i = 3; i > 0; --i) {
		test_node = HEAP(alloc)();
		value.val1 = i;
		HEAP(create)(test_node, value, 0);
		root = HEAP(insert)(root, test_node);
		is(HEAP(get_min)(root)->val1, i, "check that min.val1 is correct");
		ok(HEAP(check_invariants)(0, root), "check_heap_invariants");
	}
	footer();
}

static void
test_insert_50_to_150_mod_100()
{
	header();
	struct HEAP(node) *test_node, *root;
	struct test_type value = {1000, 0, 0};

	root = HEAP(alloc)();
	HEAP(create)(root, value, 0);

	for (uint32_t i = 50; i < 150; ++i) {
		test_node = HEAP(alloc)();
		value.val1 = i % 100;
		HEAP(create)(test_node, value, 0);
		root = HEAP(insert)(root, test_node);
		if (i < 100) {
			is(HEAP(get_min)(root)->val1, 50, "check that min.val1 is correct");
		}
		else {
			is(HEAP(get_min)(root)->val1, 0, "check that min.val1 is correct");
		}
		ok(HEAP(check_invariants)(0, root), "check_heap_invariants");
	}
	footer();
}

static void
test_insert_1000_random()
{
	header();
	const int TEST_CASE_SIZE = 1000;
	struct HEAP(node) *test_node, *root;
	uint32_t ans = 10000;
	struct test_type value = {ans, 0, 0};

	root = HEAP(alloc)();
	HEAP(create)(root, value, 0);

	for (uint32_t i = 0; i < TEST_CASE_SIZE; ++i) {
		test_node = HEAP(alloc)();
		value.val1 = rand();
		ans = (value.val1 < ans ? value.val1 : ans);
		HEAP(create)(test_node, value, 0);
		root = HEAP(insert)(root, test_node);
		is(HEAP(get_min)(root)->val1, ans, "check that min.val1 is correct");
		is(root->size, i + 2, "check that size is correct");
	}
	footer();
}

static void
test_insert_10_to_1_pop()
{
	header();
	struct HEAP(node) *test_node, *root;
	struct test_type value = {11, 0, 0};

	root = HEAP(alloc)();
	HEAP(create)(root, value, 0);

	for (uint32_t i = 10; i > 0; --i) {
		test_node = HEAP(alloc)();
		value.val1 = i;
		HEAP(create)(test_node, value, 0);
		root = HEAP(insert)(root, test_node);
		is(HEAP(get_min)(root)->val1, i, "check that min.val1 is correct");
		ok(HEAP(check_invariants)(0, root), "check_heap_invariants");
	}

	for (uint32_t i = 1; i <= 10; ++i) {
		root = HEAP(pop)(root);
		is(HEAP(get_min)(root)->val1, i + 1, "check that min.val1 is correct");
		ok(HEAP(check_invariants)(0, root), "check_heap_invariants");
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
	const int TEST_CASE_SIZE = 10000;
	struct HEAP(node) *test_node, *root;
	uint32_t ans = UINT_MAX;
	struct test_type value = {ans, 0, 0};


	root = HEAP(alloc)();
	HEAP(create)(root, value, 0);

	uint32_t keys_it = 0;
	uint32_t *keys = (uint32_t *)malloc(sizeof(uint32_t) * (TEST_CASE_SIZE + 1));
	if (keys == NULL) {
		fail("keys == NULL", "fail to alloc memory for keys array");
	}
	keys[keys_it++] = ans;

	for (uint32_t i = 0; i < TEST_CASE_SIZE; ++i) {
		test_node = HEAP(alloc)();
		keys[keys_it++] = value.val1 = rand();
		ans = (value.val1 < ans ? value.val1 : ans);
		HEAP(create)(test_node, value, 0);
		root = HEAP(insert)(root, test_node);
		is(HEAP(get_min)(root)->val1, ans, "check that min.val1 is correct");
		is(root->size, i + 2, "check that size is correct");
		ok(HEAP(check_invariants)(0, root), "check_heap_invariants");
	}

	qsort(keys, TEST_CASE_SIZE + 1, sizeof(uint32_t), uint32_compare);
	bool f = true;
	for (uint32_t i = 0; i < TEST_CASE_SIZE; ++i) {
		f = f && (keys[i] < keys[i + 1]);
	}
	ok(f, "check that keys is sorted");

	uint32_t full_size = root->size;
	for (uint32_t i = 0; i < TEST_CASE_SIZE; ++i) {
		test_node = root;
		HEAP(free)(test_node);
		root = HEAP(pop)(root);
		is(HEAP(get_min)(root)->val1, keys[i + 1], "check that min.val1 is correct");
		is(root->size, full_size - 1 - i, "check that size is correct");
		ok(HEAP(check_invariants)(0, root), "check_heap_invariants");
	}

	free(keys);

	footer();
}

static void
test_insert_with_null() {
	header();

	struct HEAP(node) *result, *root = HEAP(alloc)();
	struct test_type value = {10, 0, 0};
	HEAP(create)(root, value, 0);
	result = HEAP(insert)(root, NULL);
	is(result, root, "test insert null");

	result = HEAP(insert)(NULL, root);
	is(result, root, "test insert into null");

	footer();
}

static void
test_insert_pop_workload() {
	header();
	const int TEST_CASE_SIZE = 10000;
	struct HEAP(node) *test_node, *root = HEAP(alloc)();
	struct test_type value = {UINT_MAX, 0, 0};
	HEAP(create)(root, value, 0);
	uint64_t current_size = 1;

	for(int i = 0; i < TEST_CASE_SIZE; ++i) {
		if (root->size == 1 || rand() % 5) {
			note("insert value");
			current_size++;
			value.val1 = rand();
			test_node = HEAP(alloc)();
			HEAP(create)(test_node, value, 0);
			root = HEAP(insert)(root, test_node);
		}
		else {
			note("pop value");
			current_size--;
			root = HEAP(pop)(root);
		}
		is(root->size, current_size, "check size");
		ok(HEAP(check_invariants)(0, root), "check heap invariants");
	}
	footer();
}

static void
test_pop_last() {
	header();
	struct HEAP(node) *test_root, *root = HEAP(alloc)();
	struct test_type value = {UINT_MAX, 0, 0};
	HEAP(create)(root, value, 0);
	test_root = root;
	test_root = HEAP(pop)(root);
	is(test_root, NULL, "test delete last node");
	footer();
}

static void
test_insert_update_workload() {
	header();
	uint32_t nodes_it = 0;
	uint64_t current_size = 0;
	const int TEST_CASE_SIZE = 10000;
	struct test_type value = {UINT_MAX, 0, 0};
	struct HEAP(node) **nodes;
	nodes = (struct HEAP(node) **)
					malloc(sizeof(struct HEAP(node) *) * (TEST_CASE_SIZE + 1));

	struct HEAP(node) *test_node = NULL, *root = NULL;
	for(int i = 0; i < TEST_CASE_SIZE; ++i) {
		if (nodes_it == current_size || HEAP(size)(root) == 0 || rand() % 5) {
			note("insert value");
			value.val1 = rand();
			test_node = HEAP(alloc)();
			nodes[current_size++] = test_node;
			HEAP(create)(test_node, value, 0);
			root = HEAP(insert)(root, test_node);
		}
		else {
			note("do update");
			value.val1 = rand();
			nodes[nodes_it]->value = value;
			root = HEAP(update)(nodes[nodes_it]);
			nodes_it++;
		}
		is(root->size, current_size, "check size");
		ok(HEAP(check_invariants)(0, root), "check heap invariants");
	}
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
