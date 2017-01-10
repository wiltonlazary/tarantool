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
#include "key_def.h"

#include <stdlib.h>
#include <stdio.h>

#include "trivia/util.h"
#include "scoped_guard.h"

#include "space.h"
#include "schema.h"

const char *field_type_strs[] = {
	/* [FIELD_TYPE_ANY]      = */ "any",
	/* [FIELD_TYPE_UNSIGNED] = */ "unsigned",
	/* [FIELD_TYPE_STRING]   = */ "string",
	/* [FIELD_TYPE_ARRAY]    = */ "array",
	/* [FIELD_TYPE_NUMBER]   = */ "number",
	/* [FIELD_TYPE_INTEGER]  = */ "integer",
	/* [FIELD_TYPE_SCALAR]   = */ "scalar",
};

enum field_type
field_type_by_name(const char *name)
{
	enum field_type field_type = STR2ENUM(field_type, name);
	/*
	 * FIELD_TYPE_ANY can't be used as type of indexed field,
	 * because it is internal type used only for filling
	 * struct tuple_format.fields array.
	 */
	if (field_type != field_type_MAX && field_type != FIELD_TYPE_ANY)
		return field_type;
	/* 'num' and 'str' in _index are deprecated since Tarantool 1.7 */
	if (strcasecmp(name, "num") == 0)
		return FIELD_TYPE_UNSIGNED;
	else if (strcasecmp(name, "str") == 0)
		return FIELD_TYPE_STRING;
	return field_type_MAX;
}

const char *mp_type_strs[] = {
	/* .MP_NIL    = */ "nil",
	/* .MP_UINT   = */ "unsigned",
	/* .MP_INT    = */ "integer",
	/* .MP_STR    = */ "string",
	/* .MP_BIN    = */ "blob",
	/* .MP_ARRAY  = */ "array",
	/* .MP_MAP    = */ "map",
	/* .MP_BOOL   = */ "boolean",
	/* .MP_FLOAT  = */ "float",
	/* .MP_DOUBLE = */ "double",
	/* .MP_EXT    = */ "extension",
};

const char *index_type_strs[] = { "HASH", "TREE", "BITSET", "RTREE" };

const char *rtree_index_distance_type_strs[] = { "EUCLID", "MANHATTAN" };

const char *func_language_strs[] = {"LUA", "C"};

const uint32_t key_mp_type[] = {
	/* [FIELD_TYPE_ANY]      =  */ UINT32_MAX,
	/* [FIELD_TYPE_UNSIGNED] =  */ 1U << MP_UINT,
	/* [FIELD_TYPE_STRING]   =  */ 1U << MP_STR,
	/* [FIELD_TYPE_ARRAY]    =  */ 1U << MP_ARRAY,
	/* [FIELD_TYPE_NUMBER]   =  */ (1U << MP_UINT) | (1U << MP_INT) |
		(1U << MP_FLOAT) | (1U << MP_DOUBLE),
	/* [FIELD_TYPE_INTEGER]  =  */ (1U << MP_UINT) | (1U << MP_INT),
	/* [FIELD_TYPE_SCALAR]   =  */ (1U << MP_UINT) | (1U << MP_INT) |
		(1U << MP_FLOAT) | (1U << MP_DOUBLE) | (1U << MP_STR) |
		(1U << MP_BIN) | (1U << MP_BOOL),
};

const struct key_opts key_opts_default = {
	/* .unique              = */ true,
	/* .dimension           = */ 2,
	/* .distancebuf         = */ { '\0' },
	/* .distance            = */ RTREE_INDEX_DISTANCE_TYPE_EUCLID,
	/* .path                = */ { 0 },
	/* .range_size          = */ 0,
	/* .page_size           = */ 0,
	/* .compact_wm          = */ 2,
	/* .lsn                 = */ 0,
};

const struct opt_def key_opts_reg[] = {
	OPT_DEF("unique", MP_BOOL, struct key_opts, is_unique),
	OPT_DEF("dimension", MP_UINT, struct key_opts, dimension),
	OPT_DEF("distance", MP_STR, struct key_opts, distancebuf),
	OPT_DEF("path", MP_STR, struct key_opts, path),
	OPT_DEF("range_size", MP_UINT, struct key_opts, range_size),
	OPT_DEF("page_size", MP_UINT, struct key_opts, page_size),
	OPT_DEF("compact_wm", MP_UINT, struct key_opts, compact_wm),
	OPT_DEF("lsn", MP_UINT, struct key_opts, lsn),
	{ NULL, MP_NIL, 0, 0 }
};

static const char *object_type_strs[] = {
	"unknown", "universe", "space", "function", "user", "role" };

enum schema_object_type
schema_object_type(const char *name)
{
	/**
	 * There may be other places in which we look object type by
	 * name, and they are case-sensitive, so be case-sensitive
	 * here too.
	 */
	int n_strs = sizeof(object_type_strs)/sizeof(*object_type_strs);
	int index = strindex(object_type_strs, name, n_strs);
	return (enum schema_object_type) (index == n_strs ? 0 : index);
}

const char *
schema_object_name(enum schema_object_type type)
{
	return object_type_strs[type];
}

static void
key_def_set_cmp(struct key_def *def)
{
	def->tuple_compare = tuple_compare_create(def);
	def->tuple_compare_with_key = tuple_compare_with_key_create(def);
}

struct key_def *
key_def_new(uint32_t space_id, uint32_t iid, const char *name,
	    enum index_type type, const struct key_opts *opts,
	    uint32_t part_count)
{
	size_t sz = key_def_sizeof(part_count);
	/*
	 * Use calloc for nullifying all struct key_def attributes including
	 * comparator pointers.
	 */
	struct key_def *def = (struct key_def *) calloc(1, sz);
	if (def == NULL) {
		diag_set(OutOfMemory, sz, "malloc", "struct key_def");
		return NULL;
	}
	unsigned n = snprintf(def->name, sizeof(def->name), "%s", name);
	if (n >= sizeof(def->name)) {
		free(def);
		struct space *space = space_cache_find(space_id);
		diag_set(ClientError, ER_MODIFY_INDEX, name, space_name(space),
			 "index name is too long");
		error_log(diag_last_error(diag_get()));
		return NULL;
	}
	if (!identifier_is_valid(def->name)) {
		diag_set(ClientError, ER_IDENTIFIER, def->name);
		free(def);
		return NULL;
	}
	def->type = type;
	def->space_id = space_id;
	def->iid = iid;
	def->opts = *opts;
	def->part_count = part_count;
	return def;
}

struct key_def *
key_def_dup(const struct key_def *def)
{
	size_t sz = key_def_sizeof(def->part_count);
	struct key_def *dup = (struct key_def *) malloc(sz);
	if (dup == NULL) {
		diag_set(OutOfMemory, sz, "malloc", "struct key_def");
		return NULL;
	}
	memcpy(dup, def, key_def_sizeof(def->part_count));
	rlist_create(&dup->link);
	return dup;
}

/** Free a key definition. */
void
key_def_delete(struct key_def *key_def)
{
	free(key_def);
}

int
key_part_cmp(const struct key_part *parts1, uint32_t part_count1,
	     const struct key_part *parts2, uint32_t part_count2)
{
	const struct key_part *part1 = parts1;
	const struct key_part *part2 = parts2;
	uint32_t part_count = MIN(part_count1, part_count2);
	const struct key_part *end = parts1 + part_count;
	for (; part1 != end; part1++, part2++) {
		if (part1->fieldno != part2->fieldno)
			return part1->fieldno < part2->fieldno ? -1 : 1;
		if ((int) part1->type != (int) part2->type)
			return (int) part1->type < (int) part2->type ? -1 : 1;
	}
	return part_count1 < part_count2 ? -1 : part_count1 > part_count2;
}

int
key_def_cmp(const struct key_def *key1, const struct key_def *key2)
{
	if (key1->iid != key2->iid)
		return key1->iid < key2->iid ? -1 : 1;
	if (strcmp(key1->name, key2->name))
		return strcmp(key1->name, key2->name);
	if (key1->type != key2->type)
		return (int) key1->type < (int) key2->type ? -1 : 1;
	if (key_opts_cmp(&key1->opts, &key2->opts))
		return key_opts_cmp(&key1->opts, &key2->opts);

	return key_part_cmp(key1->parts, key1->part_count,
			    key2->parts, key2->part_count);
}

void
key_list_del_key(struct rlist *key_list, uint32_t iid)
{
	struct key_def *key;
	rlist_foreach_entry(key, key_list, link) {
		if (key->iid == iid) {
			rlist_del_entry(key, link);
			return;
		}
	}
	unreachable();
}

void
key_def_check(struct key_def *key_def)
{
	struct space *space = space_cache_find(key_def->space_id);

	if (key_def->iid >= BOX_INDEX_MAX) {
		tnt_raise(ClientError, ER_MODIFY_INDEX,
			  key_def->name,
			  space_name(space),
			  "index id too big");
	}
	if (key_def->iid == 0 && key_def->opts.is_unique == false) {
		tnt_raise(ClientError, ER_MODIFY_INDEX,
			  key_def->name,
			  space_name(space),
			  "primary key must be unique");
	}
	if (key_def->part_count == 0) {
		tnt_raise(ClientError, ER_MODIFY_INDEX,
			  key_def->name,
			  space_name(space),
			  "part count must be positive");
	}
	if (key_def->part_count > BOX_INDEX_PART_MAX) {
		tnt_raise(ClientError, ER_MODIFY_INDEX,
			  key_def->name,
			  space_name(space),
			  "too many key parts");
	}
	for (uint32_t i = 0; i < key_def->part_count; i++) {
		assert(key_def->parts[i].type > FIELD_TYPE_ANY &&
		       key_def->parts[i].type < field_type_MAX);
		if (key_def->parts[i].fieldno > BOX_INDEX_FIELD_MAX) {
			tnt_raise(ClientError, ER_MODIFY_INDEX,
				  key_def->name,
				  space_name(space),
				  "field no is too big");
		}
		for (uint32_t j = 0; j < i; j++) {
			/*
			 * Courtesy to a user who could have made
			 * a typo.
			 */
			if (key_def->parts[i].fieldno ==
			    key_def->parts[j].fieldno) {
				tnt_raise(ClientError, ER_MODIFY_INDEX,
					  key_def->name,
					  space_name(space),
					  "same key part is indexed twice");
			}
		}
	}

	/* validate key_def->type */
	space->handler->engine->keydefCheck(space, key_def);
}

void
key_def_set_part(struct key_def *def, uint32_t part_no,
		 uint32_t fieldno, enum field_type type)
{
	assert(part_no < def->part_count);
	assert(type > FIELD_TYPE_ANY && type < field_type_MAX);
	def->parts[part_no].fieldno = fieldno;
	def->parts[part_no].type = type;
	/**
	 * When all parts are set, initialize the tuple
	 * comparator function.
	 */
	/* Last part is set, initialize the comparators. */
	bool all_parts_set = true;
	for (uint32_t i = 0; i < def->part_count; i++) {
		if (def->parts[i].type == FIELD_TYPE_ANY)
			all_parts_set = false;
	}
	if (all_parts_set)
		key_def_set_cmp(def);
}

const struct key_part *
key_def_find(const struct key_def *key_def, uint32_t fieldno)
{
	const struct key_part *part = key_def->parts;
	const struct key_part *end = part + key_def->part_count;
	for (; part != end; part++) {
		if (part->fieldno == fieldno)
			return part;
	}
	return NULL;
}

struct key_def *
key_def_merge(const struct key_def *first, const struct key_def *second)
{
	uint32_t new_part_count = first->part_count + second->part_count;
	/*
	 * Find and remove part duplicates, i.e. parts counted
	 * twice since they are present in both key defs.
	 */
	const struct key_part *part = second->parts;
	const struct key_part *end = part + second->part_count;
	for (; part != end; part++) {
		if (key_def_find(first, part->fieldno))
			--new_part_count;
	}

	struct key_def *new_def;
	new_def =  key_def_new(first->space_id, first->iid, first->name,
			       first->type, &first->opts, new_part_count);
	if (new_def == NULL)
		return NULL;
	/* Write position in the new key def. */
	uint32_t pos = 0;
	/* Append first key def's parts to the new key_def. */
	part = first->parts;
	end = part + first->part_count;
	for (; part != end; part++)
	     key_def_set_part(new_def, pos++, part->fieldno, part->type);

	/* Set-append second key def's part to the new key def. */
	part = second->parts;
	end = part + second->part_count;
	for (; part != end; part++) {
		if (key_def_find(first, part->fieldno))
			continue;
		key_def_set_part(new_def, pos++, part->fieldno, part->type);
	}
	return new_def;
}

struct key_def *
key_def_build_secondary_to_primary(const struct key_def *primary,
				   const struct key_def *secondary)
{
	/*
	 * Find the order in which keys parts from primary and
	 * secondary key_defs are present in the secondary
	 * index tuple.
	 */
	struct key_def *merge = key_def_merge(secondary, primary);
	if (merge == NULL)
		return NULL;

	struct key_def *def;
	def = key_def_new(secondary->space_id, secondary->iid,
			  secondary->name, secondary->type,
			  &secondary->opts, primary->part_count);
	if (def == NULL) {
		key_def_delete(merge);
		return NULL;
	}
	/** Use the order to set parts in the result. */
	for (uint32_t i = 0; i < primary->part_count; ++i) {
		const struct key_part *part;
		part = key_def_find(merge, primary->parts[i].fieldno);
		assert(part);
		key_def_set_part(def, i, part - merge->parts, part->type);
	}
	return def;
}

struct key_def *
key_def_build_secondary(const struct key_def *primary,
			const struct key_def *secondary)
{
	struct key_def *merge = key_def_merge(secondary, primary);
	if (merge == NULL)
		return NULL;
	/*
	 * Renumber key parts, since they are stored consequently
	 * in the secondary index.
	 */
	struct key_part *part = merge->parts;
	struct key_part *end = part + merge->part_count;
	for (; part != end; part++)
		part->fieldno = part - merge->parts;
	return merge;
}

int
key_validate_parts(struct key_def *key_def, const char *key,
		   uint32_t part_count)
{
	for (uint32_t part = 0; part < part_count; part++) {
		enum mp_type mp_type = mp_typeof(*key);
		mp_next(&key);

		if (key_mp_type_validate(key_def->parts[part].type, mp_type,
					 ER_KEY_PART_TYPE, part))
			return -1;
	}
	return 0;
}

const struct space_opts space_opts_default = {
	/* .temporary = */ false,
};

const struct opt_def space_opts_reg[] = {
	OPT_DEF("temporary", MP_BOOL, struct space_opts, temporary),
	{ NULL, MP_NIL, 0, 0 }
};

void
space_def_check(struct space_def *def, uint32_t namelen, uint32_t engine_namelen,
                int32_t errcode)
{
	if (def->id > BOX_SPACE_MAX) {
		tnt_raise(ClientError, errcode,
			  def->name,
			  "space id is too big");
	}
	if (namelen >= sizeof(def->name)) {
		tnt_raise(ClientError, errcode,
			  def->name,
			  "space name is too long");
	}
	identifier_check(def->name);
	if (engine_namelen >= sizeof(def->engine_name)) {
		tnt_raise(ClientError, errcode,
			  def->name,
			  "space engine name is too long");
	}
	identifier_check(def->engine_name);

	if (def->opts.temporary) {
		Engine *engine = engine_find(def->engine_name);
		if (! engine_can_be_temporary(engine->flags))
			tnt_raise(ClientError, ER_ALTER_SPACE,
				  def->name,
			         "space does not support temporary flag");
	}
}

bool
identifier_is_valid(const char *str)
{
	mbstate_t state;
	memset(&state, 0, sizeof(state));
	wchar_t w;
	ssize_t len = mbrtowc(&w, str, MB_CUR_MAX, &state);
	if (len <= 0)
		return false; /* invalid character or zero-length string */
	if (!iswalpha(w) && w != L'_')
		return false; /* fail to match [a-zA-Z_] */

	while ((len = mbrtowc(&w, str, MB_CUR_MAX, &state)) > 0) {
		if (!iswalnum(w) && w != L'_')
			return false; /* fail to match [a-zA-Z0-9_]* */
		str += len;
	}

	if (len < 0)
		return false; /* invalid character  */

	return true;
}

void
identifier_check(const char *str)
{
	if (! identifier_is_valid(str))
		tnt_raise(ClientError, ER_IDENTIFIER, str);
}

