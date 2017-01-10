#ifndef TARANTOOL_BOX_MEMTX_SPACE_H_INCLUDED
#define TARANTOOL_BOX_MEMTX_SPACE_H_INCLUDED
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
#include "engine.h"

typedef void
(*engine_replace_f)(struct txn_stmt *, struct space *, enum dup_replace_mode);

void
memtx_replace_no_keys(struct txn_stmt *, struct space *space,
		      enum dup_replace_mode /* mode */);

struct MemtxSpace: public Handler {
	MemtxSpace(Engine *e);
	virtual ~MemtxSpace()
	{
		/* do nothing */
		/* engine->close(this); */
	}
	virtual void
	applyInitialJoinRow(struct space *space,
			    struct request *request) override;
	virtual struct tuple *
	executeReplace(struct txn *txn, struct space *space,
		       struct request *request) override;
	virtual struct tuple *
	executeDelete(struct txn *txn, struct space *space,
		      struct request *request) override;
	virtual struct tuple *
	executeUpdate(struct txn *txn, struct space *space,
		      struct request *request) override;
	virtual void
	executeUpsert(struct txn *txn, struct space *space,
		      struct request *request) override;
	virtual void
	executeSelect(struct txn *, struct space *space,
		      uint32_t index_id, uint32_t iterator,
		      uint32_t offset, uint32_t limit,
		      const char *key, const char * /* key_end */,
		      struct port *port) override;

	virtual Index *createIndex(struct space *space,
				   struct key_def *key_def) override;
	virtual void dropIndex(Index *index) override;
	virtual void prepareAlterSpace(struct space *old_space,
				       struct space *new_space) override;
public:
	/**
	 * @brief A single method to handle REPLACE, DELETE and UPDATE.
	 *
	 * @param sp space
	 * @param old_tuple the tuple that should be removed (can be NULL)
	 * @param new_tuple the tuple that should be inserted (can be NULL)
	 * @param mode      dup_replace_mode, used only if new_tuple is not
	 *                  NULL and old_tuple is NULL, and only for the
	 *                  primary key.
	 *
	 * For DELETE, new_tuple must be NULL. old_tuple must be
	 * previously found in the primary key.
	 *
	 * For REPLACE, old_tuple must be NULL. The additional
	 * argument dup_replace_mode further defines how REPLACE
	 * should proceed.
	 *
	 * For UPDATE, both old_tuple and new_tuple must be given,
	 * where old_tuple must be previously found in the primary key.
	 *
	 * Let's consider these three cases in detail:
	 *
	 * 1. DELETE, old_tuple is not NULL, new_tuple is NULL
	 *    The effect is that old_tuple is removed from all
	 *    indexes. dup_replace_mode is ignored.
	 *
	 * 2. REPLACE, old_tuple is NULL, new_tuple is not NULL,
	 *    has one simple sub-case and two with further
	 *    ramifications:
	 *
	 *	A. dup_replace_mode is DUP_INSERT. Attempts to insert the
	 *	new tuple into all indexes. If *any* of the unique indexes
	 *	has a duplicate key, deletion is aborted, all of its
	 *	effects are removed, and an error is thrown.
	 *
	 *	B. dup_replace_mode is DUP_REPLACE. It means an existing
	 *	tuple has to be replaced with the new one. To do it, tries
	 *	to find a tuple with a duplicate key in the primary index.
	 *	If the tuple is not found, throws an error. Otherwise,
	 *	replaces the old tuple with a new one in the primary key.
	 *	Continues on to secondary keys, but if there is any
	 *	secondary key, which has a duplicate tuple, but one which
	 *	is different from the duplicate found in the primary key,
	 *	aborts, puts everything back, throws an exception.
	 *
	 *	For example, if there is a space with 3 unique keys and
	 *	two tuples { 1, 2, 3 } and { 3, 1, 2 }:
	 *
	 *	This REPLACE/DUP_REPLACE is OK: { 1, 5, 5 }
	 *	This REPLACE/DUP_REPLACE is not OK: { 2, 2, 2 } (there
	 *	is no tuple with key '2' in the primary key)
	 *	This REPLACE/DUP_REPLACE is not OK: { 1, 1, 1 } (there
	 *	is a conflicting tuple in the secondary unique key).
	 *
	 *	C. dup_replace_mode is DUP_REPLACE_OR_INSERT. If
	 *	there is a duplicate tuple in the primary key, behaves the
	 *	same way as DUP_REPLACE, otherwise behaves the same way as
	 *	DUP_INSERT.
	 *
	 * 3. UPDATE has to delete the old tuple and insert a new one.
	 *    dup_replace_mode is ignored.
	 *    Note that old_tuple primary key doesn't have to match
	 *    new_tuple primary key, thus a duplicate can be found.
	 *    For this reason, and since there can be duplicates in
	 *    other indexes, UPDATE is the same as DELETE +
	 *    REPLACE/DUP_INSERT.
	 *
	 * @return old_tuple. DELETE, UPDATE and REPLACE/DUP_REPLACE
	 * always produce an old tuple. REPLACE/DUP_INSERT always returns
	 * NULL. REPLACE/DUP_REPLACE_OR_INSERT may or may not find
	 * a duplicate.
	 *
	 * The method is all-or-nothing in all cases. Changes are either
	 * applied to all indexes, or nothing applied at all.
	 *
	 * Note, that even in case of REPLACE, dup_replace_mode only
	 * affects the primary key, for secondary keys it's always
	 * DUP_INSERT.
	 *
	 * The call never removes more than one tuple: if
	 * old_tuple is given, dup_replace_mode is ignored.
	 * Otherwise, it's taken into account only for the
	 * primary key.
	 */
	engine_replace_f replace;
private:
	void
	prepareReplace(struct txn_stmt *stmt, struct space *space,
		       struct request *request);
	void
	prepareDelete(struct txn_stmt *stmt, struct space *space,
		      struct request *request);
	void
	prepareUpdate(struct txn_stmt *stmt, struct space *space,
		      struct request *request);
	void
	prepareUpsert(struct txn_stmt *stmt, struct space *space,
		      struct request *request);
};

#endif /* TARANTOOL_BOX_MEMTX_SPACE_H_INCLUDED */
