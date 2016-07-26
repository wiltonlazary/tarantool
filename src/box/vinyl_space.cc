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
#include "vinyl_engine.h"
#include "vinyl_index.h"
#include "vinyl_space.h"
#include "xrow.h"
#include "tuple.h"
#include "scoped_guard.h"
#include "txn.h"
#include "index.h"
#include "space.h"
#include "schema.h"
#include "port.h"
#include "request.h"
#include "iproto_constants.h"
#include "vinyl.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

VinylSpace::VinylSpace(Engine *e)
	:Handler(e)
{ }

void
VinylSpace::applySnapshotRow(struct space *space, struct request *request)
{
	assert(request->type == IPROTO_INSERT);
	assert(request->header != NULL);
	struct vinyl_env *env = ((VinylEngine *)space->handler->engine)->env;
	VinylIndex *index;

	/* Check field count in tuple */
	space_validate_tuple_raw(space, request->tuple);

	/* Check tuple fields */
	tuple_validate_raw(space->format, request->tuple);

	struct vinyl_tx *tx = vinyl_begin(env);
	if (tx == NULL)
		diag_raise();

	int64_t signature = request->header->lsn;
	for (uint32_t i = 0; i < space->index_count; ++i) {
		index = (VinylIndex *)space->index[i];
		if (vinyl_replace(tx, index->db, request->tuple, request->tuple_end))
			diag_raise();
	}


	int rc = vinyl_prepare(env, tx);
	switch (rc) {
	case 0:
		if (vinyl_commit(env, tx, signature))
			panic("failed to commit vinyl transaction");
		return;
	case 1: /* rollback */
	case 2: /* lock */
		vinyl_rollback(env, tx);
		/* must never happen during JOIN */
		tnt_raise(ClientError, ER_TRANSACTION_CONFLICT);
		return;
	case -1:
		vinyl_rollback(env, tx);
		diag_raise();
		return;
	default:
		unreachable();
	}
}

static void
vinyl_execute_replace_impl(VinylIndex *index, struct space *space,
			  struct request *request)
{
	/* unique constraint */
	VinylEngine *engine = (VinylEngine *)space->handler->engine;
	if (request->type == IPROTO_INSERT && engine->recovery_complete) {
		uint32_t key_len;
		const char *key = tuple_extract_key_raw(request->tuple,
			request->tuple_end, index->key_def, &key_len);
		mp_decode_array(&key); /* skip array header */
		struct tuple *found = index->findByKey(key,
			index->key_def->part_count);
		if (found) {
			/*
			 * tuple is destroyed on the next call to
			 * box_tuple_XXX() API. See box_tuple_ref()
			 * comments.
			 */
			tnt_raise(ClientError, ER_TUPLE_FOUND,
					  index_name(index), space_name(space));
		}
	}

	/* replace */
	struct vinyl_tx *tx = (struct vinyl_tx *)(in_txn()->engine_tx);
	int rc = vinyl_replace(tx, index->db, request->tuple,
			        request->tuple_end);
	if (rc == -1)
		diag_raise();
}

struct tuple *
VinylSpace::executeReplace(struct txn*,
			  struct space *space,
			  struct request *request)
{
	/* Check field count in tuple */
	space_validate_tuple_raw(space, request->tuple);

	/* Check tuple fields */
	tuple_validate_raw(space->format, request->tuple);

	VinylIndex *index;
	for (uint32_t i = 0; i < space->index_count; ++i) {
		index = (VinylIndex *)space->index[i];
		vinyl_execute_replace_impl(index, space, request);
	}

	struct tuple *new_tuple = tuple_new(space->format, request->tuple,
		request->tuple_end);
	/* GC the new tuple if there is an exception below. */
	TupleRef ref(new_tuple);

	return tuple_bless(new_tuple);
}

struct tuple *
VinylSpace::executeDelete(struct txn*, struct space *space,
                           struct request *request)
{
	int space_id = request->space_id;
	int index_id = request->index_id;
	/* find full tuple in index */
	box_tuple_t *full_tuple = NULL;
	box_index_get(space_id, index_id,
		request->key, request->key_end, &full_tuple);
	if (full_tuple == NULL) {
		diag_raise();
		return NULL;
	}

	struct vinyl_tx *tx = (struct vinyl_tx *)(in_txn()->engine_tx);
	uint32_t part_count; /* count of index key parts */
	uint32_t key_size; /* size of key in bytes */
	VinylIndex *index; /* current index */
	char *key; /* key from deleting from current index */
	for (uint32_t iid = 0; iid < space->index_count; ++iid) {
		index = (VinylIndex *)space->index[iid];
		key = box_tuple_extract_key(full_tuple, space_id, iid, &key_size);
		part_count = mp_decode_array((const char **)&key);
		if (vinyl_delete(tx, index->db, key, part_count) < 0) {
			diag_raise();
			return NULL;
		}
	}
	return NULL;
}

struct tuple *
VinylSpace::executeUpdate(struct txn*, struct space *space,
                           struct request *request)
{
	int space_id = request->space_id;
	int index_id = request->index_id;
	/* find full tuple in index */
	box_tuple_t *old_full_tuple = NULL;
	box_index_get(space_id, index_id, request->key,
		request->key_end, &old_full_tuple);
	if (old_full_tuple == NULL) {
		diag_raise();
		return NULL;
	}
	TupleRef old_ref(old_full_tuple);
	box_tuple_t *new_tuple =
		tuple_update(space->format,
		             region_aligned_alloc_xc_cb,
		             &fiber()->gc,
		             old_full_tuple, request->tuple,
		             request->tuple_end,
		             request->index_base);
	TupleRef result_ref(new_tuple);
	space_validate_tuple(space, new_tuple);
	space_check_update(space, old_full_tuple, new_tuple);

	struct vinyl_tx *tx = (struct vinyl_tx *)(in_txn()->engine_tx);
	VinylIndex *index;
	char *key;
	uint32_t part_count;
	uint32_t key_size;
	for (uint32_t iid = 0; iid < space->index_count; ++iid) {
		index = (VinylIndex *)space->index[iid];
		key = box_tuple_extract_key(old_full_tuple, space_id, iid, &key_size);
		part_count = mp_decode_array((const char **)&key);
		if (vinyl_delete(tx, index->db, key, part_count) < 0) {
			diag_raise();
			return NULL;
		}
		if (vinyl_replace(tx, index->db, new_tuple->data,
			new_tuple->data + new_tuple->bsize) < 0)
		{
			diag_raise();
			return NULL;
		}
	}
	return tuple_bless(new_tuple);
}

void
VinylSpace::executeUpsert(struct txn*, struct space *space,
                           struct request *request)
{
	VinylIndex *index = (VinylIndex *)index_find(space, request->index_id);

	/* Check field count in tuple */
	space_validate_tuple_raw(space, request->tuple);

	/* Check tuple fields */
	tuple_validate_raw(space->format, request->tuple);

	struct vinyl_tx *tx = (struct vinyl_tx *)(in_txn()->engine_tx);
	int rc = vinyl_upsert(tx, index->db, request->tuple, request->tuple_end,
			     request->ops, request->ops_end,
			     request->index_base);
	if (rc == -1)
		diag_raise();
}
