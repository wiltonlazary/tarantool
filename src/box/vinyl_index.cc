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
#include "vinyl_index.h"

#include <stdio.h>
#include <inttypes.h>
#include <bit/bit.h> /* load/store */

#include "trivia/util.h"
#include "cfg.h"
#include "say.h"
#include "scoped_guard.h"

#include "vinyl_engine.h"
#include "vinyl_space.h"
#include "tuple.h"
#include "tuple_update.h"
#include "schema.h"
#include "space.h"
#include "txn.h"
#include "vinyl.h"

VinylIndex::VinylIndex(struct key_def *key_def_arg)
	: Index(key_def_arg)
{
	struct space *space = space_cache_find(key_def->space_id);
	VinylEngine *engine =
		(VinylEngine *)space->handler->engine;
	env = engine->env;
	int rc;

	/* if index is not unique then add primary key to end of parts */
	if (!key_def->opts.is_unique) {
		Index *primary = index_find(space, 0);
		struct key_def *primary_def = primary->key_def;
		int new_parts_count = key_def->part_count +
			primary_def->part_count;

		/* create new key_def with unique part */
		struct key_def *new_def
			= key_def_new(key_def->space_id, key_def->iid,
				key_def->name, key_def->type, &key_def->opts,
				new_parts_count);

		/* append original parts to new key_def */
		memcpy(new_def->parts,
			key_def->parts,
			key_def->part_count * sizeof(struct key_part));
		/* 
		 * Append primary parts to new key_def.
		 * Do this by key_def_set_part because this function
		 * sets comparators for key_def
		*/
		uint32_t i = 0, offset = key_def->part_count;
		uint32_t limit = primary_def->part_count;
		struct key_part *primary_parts = primary_def->parts;
		for (; i < limit; ++i, ++offset)
		{
			key_def_set_part(new_def, offset,
				primary_parts[i].fieldno,
				primary_parts[i].type);
		}
		/* all parts is set */
		key_def_delete(key_def);
		key_def = new_def;
	}

	char name[128];
	snprintf(name, sizeof(name), "%d:%d", key_def->space_id, key_def->iid);
	db = vinyl_index_by_name(env, name);
	if (db != NULL) {
		if (key_def_cmp(key_def_arg, vy_index_key_def(db)))
			diag_raise();
		db = NULL;
		goto index_exists;
	}

	/* create database */
	db = vinyl_index_new(env, key_def, space->format);
	if (db == NULL)
		diag_raise();
	/* start two-phase recovery for a space:
	 * a. created after snapshot recovery
	 * b. created during log recovery
	*/
	rc = vinyl_index_open(db);
	if (rc == -1)
		diag_raise();
index_exists:
	format = space->format;
	tuple_format_ref(format, 1);
}

VinylIndex::~VinylIndex()
{
	if (db == NULL)
		return;
	/* schedule database shutdown */
	int rc = vinyl_index_close(db);
	if (rc == -1)
		goto error;
	return;
error:;
	say_info("vinyl space %" PRIu32 " close error: %s",
			 key_def->space_id, diag_last_error(diag_get())->errmsg);
}

size_t
VinylIndex::bsize() const
{
	return vinyl_index_bsize(db);
}

struct tuple *
VinylIndex::min(const char *key, uint32_t part_count) const
{
	struct iterator *it = allocIterator();
	auto guard = make_scoped_guard([=]{it->free(it);});
	initIterator(it, ITER_GE, key, part_count);
	return it->next(it);
}

struct tuple *
VinylIndex::max(const char *key, uint32_t part_count) const
{
	struct iterator *it = allocIterator();
	auto guard = make_scoped_guard([=]{it->free(it);});
	initIterator(it, ITER_LE, key, part_count);
	return it->next(it);
}

size_t
VinylIndex::count(enum iterator_type type, const char *key,
		  uint32_t part_count) const
{
	struct iterator *it = allocIterator();
	auto guard = make_scoped_guard([=]{it->free(it);});
	initIterator(it, type, key, part_count);
	size_t count = 0;
	struct tuple *tuple = NULL;
	while ((tuple = it->next(it)) != NULL)
		++count;
	return count;
}

struct tuple *
VinylIndex::findByKey(const char *key, uint32_t part_count) const
{
	assert(part_count == key_def->part_count);
	/*
	 * engine_tx might be empty, even if we are in txn context.
	 * This can happen on a first-read statement.
	 */
	struct vinyl_tx *transaction = in_txn() ?
		(struct vinyl_tx *) in_txn()->engine_tx : NULL;
	struct tuple *tuple = NULL;
	if (vinyl_coget(transaction, db, key, part_count, &tuple) != 0)
		diag_raise();
	return tuple;
}

struct tuple *
VinylIndex::replace(struct tuple*, struct tuple*, enum dup_replace_mode)
{
	/* This method is unused by vinyl index.
	 *
	 * see: vinyl_space.cc
	*/
	unreachable();
	return NULL;
}

struct vinyl_iterator {
	struct iterator base;
	/* key and part_count used only for EQ */
	const char *key;
	int part_count;
	const VinylIndex *index;
	struct key_def *key_def;
	struct vinyl_env *env;
	struct vinyl_cursor *cursor;
};

void
vinyl_iterator_free(struct iterator *ptr)
{
	assert(ptr->free == vinyl_iterator_free);
	struct vinyl_iterator *it = (struct vinyl_iterator *) ptr;
	if (it->cursor) {
		vinyl_cursor_delete(it->cursor);
		it->cursor = NULL;
	}
	free(ptr);
}

struct tuple *
vinyl_iterator_last(struct iterator *ptr __attribute__((unused)))
{
	return NULL;
}

struct tuple *
vinyl_iterator_next(struct iterator *ptr)
{
	struct vinyl_iterator *it = (struct vinyl_iterator *) ptr;
	assert(it->cursor != NULL);

	uint32_t it_sc_version = ::sc_version;

	struct tuple *tuple;
	if (vinyl_cursor_conext(it->cursor, &tuple) != 0)
		diag_raise();
	if (tuple == NULL) { /* not found */
		/* immediately close the cursor */
		vinyl_cursor_delete(it->cursor);
		it->cursor = NULL;
		ptr->next = NULL;
		return NULL;
	}

	/* found */
	if (it_sc_version != ::sc_version)
		return NULL;
	return tuple;
}

static struct tuple *
vinyl_iterator_eq(struct iterator *ptr)
{
	struct vinyl_iterator *it = (struct vinyl_iterator *) ptr;
	struct tuple *tuple = vinyl_iterator_next(ptr);
	if (tuple == NULL)
		return NULL; /* not found */

	/* check equality */
	if (tuple_compare_with_key(tuple, it->key, it->part_count,
				it->key_def) != 0) {
		/*
		 * tuple is destroyed on the next call to
		 * box_tuple_XXX() API. See box_tuple_ref()
		 * comments.
		 */
		vinyl_cursor_delete(it->cursor);
		it->cursor = NULL;
		ptr->next = NULL;
		return NULL;
	}
	return tuple;
}

static struct tuple *
vinyl_iterator_exact(struct iterator *ptr)
{
	struct vinyl_iterator *it = (struct vinyl_iterator *) ptr;
	ptr->next = vinyl_iterator_last;
	const VinylIndex *index = it->index;
	return index->findByKey(it->key, it->part_count);
}

struct iterator *
VinylIndex::allocIterator() const
{
	struct vinyl_iterator *it =
		(struct vinyl_iterator *) calloc(1, sizeof(*it));
	if (it == NULL) {
		tnt_raise(OutOfMemory, sizeof(struct vinyl_iterator),
			  "Vinyl Index", "iterator");
	}
	it->base.next = vinyl_iterator_last;
	it->base.free = vinyl_iterator_free;
	return (struct iterator *) it;
}

void
VinylIndex::initIterator(struct iterator *ptr,
                          enum iterator_type type,
                          const char *key, uint32_t part_count) const
{
	assert(part_count == 0 || key != NULL);
	struct vinyl_iterator *it = (struct vinyl_iterator *) ptr;
	assert(it->cursor == NULL);
	it->index = this;
	it->key_def = key_def;
	it->env = env;
	it->key = key;
	it->part_count = part_count;

	enum vinyl_order order;
	switch (type) {
	case ITER_ALL:
	case ITER_GE:
		order = VINYL_GE;
		ptr->next = vinyl_iterator_next;
		break;
	case ITER_GT:
		order = part_count > 0 ? VINYL_GT : VINYL_GE;
		ptr->next = vinyl_iterator_next;
		break;
	case ITER_LE:
		order = VINYL_LE;
		ptr->next = vinyl_iterator_next;
		break;
	case ITER_LT:
		order = part_count > 0 ? VINYL_LT : VINYL_LE;
		ptr->next = vinyl_iterator_next;
		break;
	case ITER_EQ:
		/* point-lookup iterator (optimization) */
		if (part_count == key_def->part_count) {
			ptr->next = vinyl_iterator_exact;
			return;
		}
		order = VINYL_GE;
		ptr->next = vinyl_iterator_eq;
		break;
	case ITER_REQ:
		/* point-lookup iterator (optimization) */
		if (part_count == key_def->part_count) {
			ptr->next = vinyl_iterator_exact;
			return;
		}
		order = VINYL_LE;
		ptr->next = vinyl_iterator_eq;
		break;
	default:
		return Index::initIterator(ptr, type, key, part_count);
	}
	it->cursor = vinyl_cursor_new(db, key, part_count, order);
	if (it->cursor == NULL)
		diag_raise();
}
