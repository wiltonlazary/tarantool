/*
 * *No header guard*: the header is allowed to be included twice
 * with different sets of defines.
 */
/*
 * Copyright 2010-2016, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *		copyright notice, this list of conditions and the
 *		following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *		copyright notice, this list of conditions and the following
 *		disclaimer in the documentation and/or other materials
 *		provided with the distribution.
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

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>


/**
 * Additional user defined name that appended to prefix 'heap'
 *	for all names of structs and functions in this header file.
 * All names use pattern: heap<HEAP_NAME>_<name of func/struct>
 * May be empty, but still have to be defined (just #define HEAP_NAME)
 * Example:
 * #define HEAP_NAME _test
 * ...
 * struct heap_test_core some_heap;
 * heap_test_create(&some_heap, ...);
 */
#ifndef HEAP_NAME
#error "HEAP_NAME must be defined"
#endif

/**
 * Data type that heap holds. Must be less tant 8 bytes.
 */
#ifndef HEAP_DATA_TYPE
#error "HEAP_DATA_TYPE must be defined"
#endif

/**
 * Type of optional third parameter of comparing function.
 * If not needed, simply use #define HEAP_CMP_ARG_TYPE int
 */
#ifndef HEAP_CMP_ARG_TYPE
#error "HEAP_CMP_ARG_TYPE must be defined"
#endif

/**
 * Data comparing function. Takes 3 parameters - value1, value2 and
 * optional value that stored in hash table struct.
 * Third parameter may be simply ignored like that:
 * #define HEAP_LESS(a, b, garb) a < b
 * HEAP_LESS is less function that is important!
 */
#ifndef HEAP_LESS
#error "HEAP_LESS must be defined"
#endif

/**
 * Tools for name substitution:
 */
#ifndef CONCAT4
#define CONCAT4_R(a, b, c, d) a##b##c##d
#define CONCAT4(a, b, c, d) CONCAT4_R(a, b, c, d)
#endif

#ifdef _
#error '_' must be undefinded!
#endif
#ifndef HEAP
#define HEAP(name) CONCAT4(heap, HEAP_NAME, _, name)
#endif

/**
 * Heap entry structure.
 */
struct HEAP(node){
 uint64_t size; /*size of subtree*/
 struct HEAP(node) *left;
 struct HEAP(node) *right;
 struct HEAP(node) *parent;
 HEAP_CMP_ARG_TYPE arg;

 HEAP_DATA_TYPE value;
};


/**
 * Allocate node for heap.
 */
static struct HEAP(node) *
HEAP(alloc)();

/**
 * Free heap node.
 */
static void
HEAP(free)(struct HEAP(node) *nd);

/**
 * Init node as one element heap.
 */
static void
HEAP(create)(struct HEAP(node) *node,
						 HEAP_DATA_TYPE value,
						 HEAP_CMP_ARG_TYPE arg);

/**
 * Check that current tree is full binary tree.
 */
static bool
HEAP(is_full)(const struct HEAP(node) *root);

/**
 * Returns min value of heap.
 */
static HEAP_DATA_TYPE *
HEAP(get_min)(struct HEAP(node) *root);

/**
 * Returns size of according to root.
 */
inline static uint64_t
HEAP(size)(struct HEAP(node) *root);

/**
 * Returns root node of heap.
 */
static struct HEAP(node) *
HEAP(get_root)(struct HEAP(node) *node);

/**
 * Swap two parent and son.
 */
static void
HEAP(swap_parent_and_son)(struct HEAP(node) *parent, struct HEAP(node) *son);

/**
 * Update parent field of children.
 */
static void
HEAP(push_info_to_children)(struct HEAP(node) *node);

/**
 * Update left or right field of parent.
 */
inline static void
HEAP(push_info_to_parent)(struct HEAP(node) *parent, struct HEAP(node) *son);

/**
 * Cut leaf. Node is a pointer to leaf.
 */
static void
HEAP(cut_leaf)(struct HEAP(node) *node);

/**
 * Get first not full, i.e. first node with less that 2 sons.
 */
struct HEAP(node)*
HEAP(get_first_not_full)(struct HEAP(node) *root);


/**
 * Get last node, i.e. the most right in bottom layer.
 */
struct HEAP(node) *
HEAP(get_last)(struct HEAP(node) *root);

/**
 * Sift up current node.
 */
void
HEAP(sift_up)(struct HEAP(node) *node);

/**
 * Sift down current node.
 */
void
HEAP(sift_down)(struct HEAP(node) *node);

/**
 * Increment size in every node on path to root.
 */
void
HEAP(inc_size)(struct HEAP(node) *node);

/**
 * Decrement size in every node on path to root.
 */
void
HEAP(dec_size)(struct HEAP(node) *node);


/**
 * Insert value.
 */
struct HEAP(node) *
HEAP(insert)(struct HEAP(node) *root, struct HEAP(node) *nd);

/**
 * Erase min value.
 */
static struct HEAP(node) *
HEAP(pop)(struct HEAP(node) *root);

/**
 * Delete node from heap.
 */
struct HEAP(node) *
HEAP(delete)(struct HEAP(node) *root, struct HEAP(node) *value_node);

/**
 * Heapify tree after update of value under value_node pointer.
 */
inline static struct HEAP(node) *
HEAP(update)(struct HEAP(node) *value_node);

/**
 * Debug function. Check heap invariants for pair node, parent.
 */
static bool
HEAP(check_local_invariants) (struct HEAP(node) *node,
															struct HEAP(node) *parent);

/*
 * Debug function. Check heap invariants for all nodes.
 */
static bool
HEAP(check_invariants)(struct HEAP(node) *node, struct HEAP(node) *parent);

/* Function defenitions */

/**
 * Allocate node for heap.
 */
inline static struct HEAP(node) *
HEAP(alloc)() {
	return (struct HEAP(node) *) malloc(sizeof(struct HEAP(node)));
}

/**
 * Free heap node.
 */
inline static void
HEAP(free)(struct HEAP(node) *nd) {
	free(nd);
}


/**
 * Init node as one element heap.
 */
inline static void
HEAP(create)(struct HEAP(node) *node,
						 HEAP_DATA_TYPE value,
						 HEAP_CMP_ARG_TYPE arg) {
	node->size = 1;
	node->left = NULL;
	node->right = NULL;
	node->parent = NULL;

	node->arg = arg;
	node->value = value;
}

/**
 * Returns min value of heap.
 */
inline static HEAP_DATA_TYPE *
HEAP(get_min)(struct HEAP(node) *node) {
	assert(node);
	return &(HEAP(get_root)(node)->value);
}

/**
 * Returns size of according to root.
 */
inline static uint64_t
HEAP(size)(struct HEAP(node) *root) {
	return (root ? root->size : 0);
}

/**
 * Returns size of subtree according to choldren sizes.
 */
inline static uint64_t
HEAP(get_size_from_children)(struct HEAP(node) *node) {
	if (node == NULL) {
		return 0;
	}
	uint32_t size = 1;
	if (node->left) {
		size += node->left->size;
	}
	if (node->right) {
		size += node->right->size;
	}

	return size;
}

/**
 * Returns root node of heap.
 */
inline static struct HEAP(node) *
HEAP(get_root)(struct HEAP(node) *node) {
	assert(node);
	while (node->parent) {
		node = node->parent;
	}

	return node;
}

/**
 * Check that current tree is full binary tree.
 */
inline static bool
HEAP(is_full)(const struct HEAP(node) *root) {
	assert(root);
	uint64_t size = root->size + 1;
	/*check that size + 1 is 2^n for some n*/
	return ((size&(-size)) == size);
}

/**
 * Update parent field of children.
 */
inline static void
HEAP(push_info_to_children)(struct HEAP(node) *node) {
 assert(node);
 if (node->left) {
	 node->left->parent = node;
 }
 if (node->right) {
	 node->right->parent = node;
 }
}

/**
 * Update left or right field of parent.
 */
inline static void
HEAP(push_info_to_parent)(struct HEAP(node) *parent, struct HEAP(node) *son) {
	assert(parent);
	assert(son);
	if (!parent->parent) {
		return;
	}
	struct HEAP(node) *pparent = parent->parent;
	if (pparent->left == son) {
		pparent->left = parent;
	}
	else {
		pparent->right = parent;
	}
}

/**
 * Cut leaf. Node is a pointer to leaf.
 */
inline void
HEAP(cut_leaf)(struct HEAP(node) *node) {
	assert(node);
	assert(node->left == NULL);
	assert(node->right == NULL);

	if (node->parent == NULL) {
		return;
	}

	struct HEAP(node) *parent = node->parent;
	if (parent->left == node) {
		parent->left = NULL;
		return;
	}

	if (parent->right == node) {
		parent->right = NULL;
		return;
	}

	/* unreachable */
	assert(false);
}

/**
 * Swap two connected(i.e parent and son) nodes.
 */
inline static void
HEAP(swap_parent_and_son)(struct HEAP(node) *parent, struct HEAP(node) *son) {
	assert(parent);
	assert(son);
	struct HEAP(node) *tmp;
	uint32_t tmp_size;
	tmp_size = parent->size;
	parent->size = son->size;
	son->size = tmp_size;

	if (parent->left == son) {
		tmp = son->left;
		son->left = parent;
		parent->left = tmp;
		son->parent = parent->parent;
		HEAP(push_info_to_parent)(son, parent);
	}
	else {
		tmp = parent->left;
		parent->left = son->left;
		son->left = tmp;
	}

	if (parent->right == son) {
		tmp = son->right;
		son->right = parent;
		parent->right = tmp;
		son->parent = parent->parent;
		HEAP(push_info_to_parent)(son, parent);
	}
	else {
		tmp = parent->right;
		parent->right	= son->right;
		son->right = tmp;
	}

	HEAP(push_info_to_children)(parent);
	HEAP(push_info_to_children)(son);
}

/**
 * Get first not full, i.e. first node with less that 2 sons.
 */
struct HEAP(node) *
HEAP(get_first_not_full)(struct HEAP(node) *root) {
 assert(root);

 bool is_full_left, is_full_right;
 while (root->right) {
		is_full_left = HEAP(is_full)(root->left);
		is_full_right = HEAP(is_full)(root->right);

		assert(is_full_left || is_full_right); /* heap is always complete tree */

	if (is_full_left && is_full_right) {
		if (root->left->size == root->right->size) {
			root = root->left;
		}
		else {
			root = root->right;
		}
	}
	else {
		if (is_full_left) {
			root = root->right;
		}
		else {
			root = root->left;
		}
	}
 }

 return root;
}

/**
 * Get last node, i.e. the most right in bottom layer.
 */
struct HEAP(node) *
HEAP(get_last)(struct HEAP(node) *root) {
	assert(root);

	bool is_full_left, is_full_right;
	while (root->right) {
		is_full_left = HEAP(is_full)(root->left);
		is_full_right = HEAP(is_full)(root->right);

		assert(is_full_left || is_full_right); /* heap is always complete tree */

		if (is_full_left && is_full_right) {
			if (root->left->size == root->right->size) {
				root = root->right;
			}
		else {
			root = root->left;
		}
		}
		else {
			if (is_full_left) {
				root = root->right;
			}
			else {
				root = root->left;
			}
		}
	}

	return (root->left ? root->left : root);
}


/**
 * Sift up current node.
 */
void
HEAP(sift_up)(struct HEAP(node) *node) {
	assert(node);
	struct HEAP(node) *parent = node->parent;
	while (parent && HEAP_LESS(&node->value, &parent->value, node->arg)) {
		HEAP(swap_parent_and_son)(parent, node);
		parent = node->parent;
	}
}

/**
 * Sift down current node.
 */
void
HEAP(sift_down)(struct HEAP(node) *node) {
	assert(node);
	struct HEAP(node) *left = node->left;
	struct HEAP(node) *right = node->right;
	struct HEAP(node) *min_son;
	if (left && right) {
		min_son = (HEAP_LESS(&left->value, &right->value, node->arg) ? left : right);
	}

	while (left && right && HEAP_LESS(&min_son->value, &node->value, node->arg)) {
		HEAP(swap_parent_and_son)(node, min_son);
		left = node->left;
		right = node->right;

		if (left && right) {
			min_son = (HEAP_LESS(&left->value, &right->value, node->arg) ? left : right);
		}
	}

	if ((left || right) && HEAP_LESS(&left->value, &node->value, node->arg)) {
		assert(left); /*left is not null because heap is complete tree*/
		assert(right == NULL);
		if (HEAP_LESS(&left->value, &node->value, node->arg)) {
			HEAP(swap_parent_and_son)(node, left);
		}
	}
}

/**
 * Increment size in every node on path to root.
 */
void
HEAP(inc_size)(struct HEAP(node) *node) {
	while (node->parent) {
		node = node->parent;
		node->size++;
	}
}

/**
 * Decrement size in every node on path to root.
 */
void
HEAP(dec_size)(struct HEAP(node) *node) {
	while (node->parent) {
		node = node->parent;
		node->size--;
	}
}


/**
 * Insert value.
 */
struct HEAP(node) *
HEAP(insert)(struct HEAP(node) *root, struct HEAP(node) *node) {
	if (root == NULL) {
		return node;
	}
	if (node == NULL) {
		return root;
	}

	struct HEAP(node) *first_not_full = HEAP(get_first_not_full)(root);
	node->parent = first_not_full;
	if (first_not_full->left) {
		first_not_full->right = node;
	}
	else {
		first_not_full->left = node;
	}
	HEAP(inc_size)(node); /* update sizes */

	HEAP(sift_up)(node); /* heapify */

	/* return new root */
	return HEAP(get_root)(node);
}

/**
 * Erase min value.
 */
inline static struct HEAP(node) *
HEAP(pop)(struct HEAP(node) *root) {
	return HEAP(delete)(root, root);
}

/*
 * Delete node from heap.
 */
struct HEAP(node) *
HEAP(delete)(struct HEAP(node) *root, struct HEAP(node) *value_node) {
	struct HEAP(node) *last_node = HEAP(get_last)(root);

	/* check that we try to delete last node */
	if (last_node == root) {
		assert(last_node == value_node);
		return NULL;
	}

	assert(last_node->left == NULL);
	assert(last_node->right == NULL);

	HEAP(dec_size)(last_node); /* update sizes */

	/* cut leaf */
	HEAP(cut_leaf)(last_node);

	/* insert last_node as root */
	last_node->parent = value_node->parent;
	last_node->left = value_node->left;
	last_node->right = value_node->right;
	last_node->size = HEAP(get_size_from_children)(last_node);
	HEAP(push_info_to_parent)(last_node, value_node);
	HEAP(push_info_to_children)(last_node);

	/* delte root from tree */
	value_node->left =  NULL;
	value_node->right =  NULL;
	value_node->parent = NULL;
	value_node->size = 1;

	/*heapify */
	HEAP(update)(last_node);

	/* return new root */
	return HEAP(get_root)(last_node);
}

/**
 * Heapify tree after update of value under value_node pointer.
 */
inline static struct HEAP(node) *
HEAP(update)(struct HEAP(node) *value_node) {
	/* heapify */
	HEAP(sift_down)(value_node);
	HEAP(sift_up)(value_node);

	/* return new root */
	return HEAP(get_root)(value_node);
}


/**
 * Debug function. Check heap invariants for pair node, parent.
 */
inline static bool
HEAP(check_local_invariants) (struct HEAP(node) *parent, struct HEAP(node) *node) {
	assert(node);

	if (parent != node->parent) {
		return false;
	}
	if (parent && parent->left != node && parent->right != node) {
		return false;
	}

	if (node->size != HEAP(get_size_from_children)(node)) {
		return false;
	}
	if (node->right && node->left &&
			!HEAP(is_full)(node->right) && !HEAP(is_full)(node->left)) {
		return false;
	}

	if (node->left && HEAP_LESS(&node->left->value, &node->value, node->arg)) {
		return false;
	}
	if (node->right && HEAP_LESS(&node->right->value, &node->value, node->arg)) {
		return false;
	}

	return true;
}

/**
 * Debug function. Check heap invariants for all nodes.
 */
static bool
HEAP(check_invariants)(struct HEAP(node) *parent, struct HEAP(node) *node) {
	if (!node) {
		return true;
	}

	if (!HEAP(check_local_invariants)(parent, node)) {
		return false;
	}

	bool check_left = HEAP(check_invariants)(node, node->left);
	bool check_right = HEAP(check_invariants)(node, node->right);

	return (check_right && check_left);
}
