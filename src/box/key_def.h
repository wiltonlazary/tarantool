#ifndef TARANTOOL_BOX_KEY_DEF_H_INCLUDED
#define TARANTOOL_BOX_KEY_DEF_H_INCLUDED
/*
 * Copyright 2010-2016, Tarantool AUTHORS, please see AUTHORS file.
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
#include "trivia/util.h"
#include "small/rlist.h"
#include "error.h"
#include <msgpuck.h>
#define RB_COMPACT 1
#include "small/rb.h"
#include <limits.h>
#include <wchar.h>
#include <wctype.h>
#include "tuple_compare.h"

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

enum {
	BOX_ENGINE_MAX = 3, /* + 1 to the actual number of engines */
	BOX_SPACE_MAX = INT32_MAX,
	BOX_FUNCTION_MAX = 32000,
	BOX_INDEX_MAX = 128,
	BOX_NAME_MAX = 64,
	BOX_FIELD_MAX = INT32_MAX,
	BOX_USER_MAX = 32,
	/**
	 * A fairly arbitrary limit which is still necessary
	 * to keep tuple_format object small.
	 */
	BOX_INDEX_FIELD_MAX = INT16_MAX,
	/** Yet another arbitrary limit which simply needs to
	 * exist.
	 */
	BOX_INDEX_PART_MAX = UINT8_MAX
};

/*
 * Different objects which can be subject to access
 * control.
 *
 * Use 0 for unknown to use the same index consistently
 * even when there are more object types in the future.
 */
enum schema_object_type {
	SC_UNKNOWN = 0, SC_UNIVERSE = 1, SC_SPACE = 2, SC_FUNCTION = 3,
	SC_USER = 4, SC_ROLE = 5
};

enum schema_object_type
schema_object_type(const char *name);

const char *
schema_object_name(enum schema_object_type type);

/*
 * Possible field data types. Can't use STRS/ENUM macros for them,
 * since there is a mismatch between enum name (STRING) and type
 * name literal ("STR"). STR is already used as Objective C type.
 */
enum field_type {
	FIELD_TYPE_ANY = 0,
	FIELD_TYPE_UNSIGNED,
	FIELD_TYPE_STRING,
	FIELD_TYPE_ARRAY,
	FIELD_TYPE_NUMBER,
	FIELD_TYPE_INTEGER,
	FIELD_TYPE_SCALAR,
	field_type_MAX
};
extern const char *field_type_strs[];

/* MsgPack type names */
extern const char *mp_type_strs[];

/**
 * The supported language of the stored function.
 */
enum func_language {
	FUNC_LANGUAGE_LUA,
	FUNC_LANGUAGE_C,
	func_language_MAX,
};
extern const char *func_language_strs[];

static inline uint32_t
field_type_maxlen(enum field_type type)
{
	static const uint32_t maxlen[] =
		{ UINT32_MAX, 8, UINT32_MAX, UINT32_MAX, UINT32_MAX };
	return maxlen[type];
}

enum field_type
field_type_by_name(const char *name);

enum index_type {
	HASH = 0, /* HASH Index */
	TREE,     /* TREE Index */
	BITSET,   /* BITSET Index */
	RTREE,    /* R-Tree Index */
	index_type_MAX,
};

extern const char *index_type_strs[];

struct opt_def {
	const char *name;
	enum mp_type type;
	ptrdiff_t offset;
	uint32_t len;
};

#define OPT_DEF(key, type, opts, field) \
	{ key, type, offsetof(opts, field), sizeof(((opts *)0)->field) }

enum rtree_index_distance_type {
	 /* Euclid distance, sqrt(dx*dx + dy*dy) */
	RTREE_INDEX_DISTANCE_TYPE_EUCLID,
	/* Manhattan distance, fabs(dx) + fabs(dy) */
	RTREE_INDEX_DISTANCE_TYPE_MANHATTAN,
	rtree_index_distance_type_MAX
};
extern const char *rtree_index_distance_type_strs[];

/** Descriptor of a single part in a multipart key. */
struct key_part {
	uint32_t fieldno;
	enum field_type type;
};

/** Index options */
struct key_opts {
	/**
	 * Is this index unique or not - relevant to HASH/TREE
	 * index
	 */
	bool is_unique;
	/**
	 * RTREE index dimension.
	 */
	uint32_t dimension;
	/**
	 * RTREE distance type.
	 */
	char distancebuf[16];
	enum rtree_index_distance_type distance;
	/**
	 * Vinyl index options.
	 */
	char path[PATH_MAX];
	uint64_t range_size;
	uint32_t page_size;
	/**
	 * Begin compaction when there are more than compact_wm
	 * runs in a range.
	 */
	uint32_t compact_wm;
	/**
	 * LSN from the time of index creation.
	 */
	int64_t lsn;
};

extern const struct key_opts key_opts_default;
extern const struct opt_def key_opts_reg[];

static inline int
key_opts_cmp(const struct key_opts *o1, const struct key_opts *o2)
{
	if (o1->is_unique != o2->is_unique)
		return o1->is_unique < o2->is_unique ? -1 : 1;
	if (o1->dimension != o2->dimension)
		return o1->dimension < o2->dimension ? -1 : 1;
	if (o1->distance != o2->distance)
		return o1->distance < o2->distance ? -1 : 1;
	return 0;
}

/* Descriptor of a multipart key. */
struct key_def {
	/* A link in key list. */
	struct rlist link;
	/** Ordinal index number in the index array. */
	uint32_t iid;
	/* Space id. */
	uint32_t space_id;
	/** Index name. */
	char name[BOX_NAME_MAX + 1];
	/** Index type. */
	enum index_type type;
	struct key_opts opts;
	/** comparators */
	tuple_compare_t tuple_compare;
	tuple_compare_with_key_t tuple_compare_with_key;
	/** The size of the 'parts' array. */
	uint32_t part_count;
	/** Description of parts of a multipart index. */
	struct key_part parts[];
};

struct key_def *
key_def_dup(const struct key_def *def);

/* Destroy and free a key_def. */
void
key_def_delete(struct key_def *def);

/**
 * Encapsulates privileges of a user on an object.
 * I.e. "space" object has an instance of this
 * structure for each user.
 */
struct access {
	/**
	 * Granted access has been given to a user explicitly
	 * via some form of a grant.
	 */
	uint8_t granted;
	/**
	 * Effective access is a sum of granted access and
	 * all privileges inherited by a user on this object
	 * via some role. Since roles may be granted to other
	 * roles, this may include indirect grants.
	 */
	uint8_t effective;
};

/**
 * Effective session user. A cache of user data
 * and access stored in session and fiber local storage.
 * Differs from the authenticated user when executing
 * setuid functions.
 */
struct credentials {
	/** A look up key to quickly find session user. */
	uint8_t auth_token;
	/**
	 * Cached global grants, to avoid an extra look up
	 * when checking global grants.
	 */
	uint8_t universal_access;
	/** User id of the authenticated user. */
	uint32_t uid;
};

/**
 * Definition of a function. Function body is not stored
 * or replicated (yet).
 */
struct func_def {
	/** Function id. */
	uint32_t fid;
	/** Owner of the function. */
	uint32_t uid;
	/**
	 * True if the function requires change of user id before
	 * invocation.
	 */
	bool setuid;
	/**
	 * The language of the stored function.
	 */
	enum func_language language;
	/** Function name. */
	char name[BOX_NAME_MAX + 1];
};

/**
 * Definition of a privilege
 */
struct priv_def {
	/** Who grants the privilege. */
	uint32_t grantor_id;
	/** Whom the privilege is granted. */
	uint32_t grantee_id;
	/* Object id - is only defined for object type */
	uint32_t object_id;
	/* Object type - function, space, universe */
	enum schema_object_type object_type;
	/**
	 * What is being granted, has been granted, or is being
	 * revoked.
	 */
	uint8_t access;
	/** To maintain a set of effective privileges. */
	rb_node(struct priv_def) link;
};

/** Space options */
struct space_opts {
        /**
	 * The space is a temporary:
	 * - it is empty at server start
	 * - changes are not written to WAL
	 * - changes are not part of a snapshot
	 */
	bool temporary;
};

extern const struct space_opts space_opts_default;
extern const struct opt_def space_opts_reg[];

/** Space metadata. */
struct space_def {
	/** Space id. */
	uint32_t id;
	/** User id of the creator of the space */
	uint32_t uid;
	/**
	 * If not set (is 0), any tuple in the
	 * space can have any number of fields.
	 * If set, each tuple
	 * must have exactly this many fields.
	 */
	uint32_t exact_field_count;
	char name[BOX_NAME_MAX + 1];
	char engine_name[BOX_NAME_MAX + 1];
	struct space_opts opts;
};

/**
 * API of C stored function.
 */
typedef struct box_function_ctx box_function_ctx_t;
typedef int (*box_function_f)(box_function_ctx_t *ctx,
	     const char *args, const char *args_end);

static inline size_t
key_def_sizeof(uint32_t part_count)
{
	return sizeof(struct key_def) + sizeof(struct key_part) * (part_count + 1);
}

/**
 * Allocate a new key definition.
 * @retval not NULL Success.
 * @retval NULL     Memory error.
 */
struct key_def *
key_def_new(uint32_t space_id, uint32_t iid, const char *name,
	    enum index_type type, const struct key_opts *opts,
	    uint32_t part_count);

/**
 * Copy one key def into another, preserving the membership
 * in rlist. This only works for key defs with equal number of
 * parts.
 */
static inline void
key_def_copy(struct key_def *to, const struct key_def *from)
{
	struct rlist save_link = to->link;
	int part_count = (to->part_count < from->part_count ?
			  to->part_count : from->part_count);
	size_t size  = (sizeof(struct key_def) +
			sizeof(struct key_part) * part_count);
	memcpy(to, from, size);
	to->link = save_link;
}

/**
 * Set a single key part in a key def.
 * @pre part_no < part_count
 */
void
key_def_set_part(struct key_def *def, uint32_t part_no,
		 uint32_t fieldno, enum field_type type);

/**
 * Returns the part in key_def->parts for the specified fieldno.
 * If fieldno is not in key_def->parts returns NULL.
 */
const struct key_part *
key_def_find(const struct key_def *key_def, uint32_t fieldno);

/**
 * Allocate a new key_def with a set union of key parts from
 * first and second key defs. Parts of the new key_def consist
 * of the first key_def's parts and those parts of the second
 * key_def that were not among the first parts.
 * @retval not NULL Ok.
 * @retval NULL     Memory error.
 */
struct key_def *
key_def_merge(const struct key_def *first, const struct key_def *second);

/**
 * Create a key_def to fetch primary key parts from the tuple
 * stored in a non-covering secondary index.
 *
 * A non-covering secondary index stores a tuple with a union
 * of fields from the primary and secondary key. If a field is
 * present in both indexes, it's only stored once.
 *
 * For example, if there's a primary key defined over fields
 * (1, 5) and a secondary key defined over fields (7, 5), then
 * the tuple, stored in the secondary key will contain fields (7,
 * 5, 1) from the original tuple, in this order.
 *
 * The key def returned by this function for the above example
 * will contain fields (3, 2), since it's built to operate on
 * the secondary index tuple, and extract key parts for a look
 * up in the primary key.
 *
 * All key parts in the new key_def will keep their original
 * types.
 *
 * @param primary the definition of the primary key
 * @param secondary the definition of the secondary key
 *
 * @retval not NULL Ok.
 * @retval NULL     Memory error.
 *
 * @sa usage in vinyl_index.cc
 */
struct key_def *
key_def_build_secondary_to_primary(const struct key_def *primary,
				   const struct key_def *secondary);

/**
 * Create a key def with a set union of primary and secondary
 * keys, used to compare such keys between each other. This
 * key_def describes how the index is stored in the engine.
 *
 * @retval not NULL Ok.
 * @retval NULL     Memory error.
 */
struct key_def *
key_def_build_secondary(const struct key_def *primary,
			const struct key_def *secondary);

/*
 * Check that parts of the key match with the key definition.
 * @param key_def Key definition.
 * @param key MessagePack'ed data for matching.
 * @param part_count Field count in the key.
 *
 * @retval 0  The key is valid.
 * @retval -1 The key is invalid.
 */
int
key_validate_parts(struct key_def *key_def, const char *key,
		   uint32_t part_count);

#if defined(__cplusplus)
} /* extern "C" */

/** Compare two key part arrays.
 *
 * This function is used to find out whether alteration
 * of an index has changed it substantially enough to warrant
 * a rebuild or not. For example, change of index id is
 * not a substantial change, whereas change of index type
 * or key parts requires a rebuild.
 *
 * One key part is considered to be greater than the other if:
 * - its fieldno is greater
 * - given the same fieldno, NUM < STRING
 *   (coarsely speaking, based on field_type_maxlen()).
 *
 * A key part array is considered greater than the other if all
 * its key parts are greater, or, all common key parts are equal
 * but there are additional parts in the bigger array.
 */
int
key_part_cmp(const struct key_part *parts1, uint32_t part_count1,
	     const struct key_part *parts2, uint32_t part_count2);

/**
 * One key definition is greater than the other if it's id is
 * greater, it's name is greater,  it's index type is greater
 * (HASH < TREE < BITSET) or its key part array is greater.
 */
int
key_def_cmp(const struct key_def *key1, const struct key_def *key2);

/** Add a key to the list of keys. */
static inline  void
key_list_add_key(struct rlist *key_list, struct key_def *key)
{
	rlist_add_entry(key_list, key, link);
}

/** Remove a key from the list of keys. */
void
key_list_del_key(struct rlist *key_list, uint32_t id);

/**
 * Check a key definition for violation of various limits.
 *
 * @param key_def   key_def
 */
void
key_def_check(struct key_def *key_def);

/** Check space definition structure for errors. */
void
space_def_check(struct space_def *def, uint32_t namelen,
                uint32_t engine_namelen,
                int32_t errcode);

/** A helper table for key_mp_type_validate */
extern const uint32_t key_mp_type[];

/**
 * @brief Checks if \a field_type (MsgPack) is compatible \a type (KeyDef).
 * @param type KeyDef type
 * @param field_type MsgPack type
 * @param field_no - a field number (is used to store an error message)
 *
 * @retval 0  mp_type is valid.
 * @retval -1 mp_type is invalid.
 */
static inline int
key_mp_type_validate(enum field_type key_type, enum mp_type mp_type,
	       int err, uint32_t field_no)
{
	assert(key_type < field_type_MAX);
	assert((size_t) mp_type < CHAR_BIT * sizeof(*key_mp_type));
	if (unlikely((key_mp_type[key_type] & (1U << mp_type)) == 0)) {
		diag_set(ClientError, err, field_no, field_type_strs[key_type]);
		return -1;
	}
	return 0;
}

/**
 * Check object identifier for invalid symbols.
 * The function checks \a str for matching [a-zA-Z_][a-zA-Z0-9_]* expression.
 * Result is locale-dependent.
 */
bool
identifier_is_valid(const char *str);

/**
 * Throw an error if identifier is not valid.
 */
void
identifier_check(const char *str);

#endif /* defined(__cplusplus) */

#endif /* TARANTOOL_BOX_KEY_DEF_H_INCLUDED */
