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
#include "vinyl.h"

#include <dirent.h>

#include <bit/bit.h>
#include <small/rlist.h>
#define RB_COMPACT 1
#include <small/rb.h>
#include <small/mempool.h>
#include <small/region.h>
#include <small/lsregion.h>
#include <msgpuck/msgpuck.h>
#include <coeio_file.h>

#include "trivia/util.h"
#include "crc32.h"
#include "clock.h"
#include "trivia/config.h"
#include "tt_pthread.h"
#include "cfg.h"
#include "diag.h"
#include "fiber.h" /* cord_slab_cache() */
#include "ipc.h"
#include "coeio.h"
#include "histogram.h"
#include "rmean.h"
#include "assoc.h"
#include "errinj.h"

#include "errcode.h"
#include "key_def.h"
#include "tuple.h"
#include "tuple_update.h"
#include "txn.h" /* box_txn_alloc() */
#include "iproto_constants.h"
#include "cluster.h" /* SERVER_UUID */
#include "vclock.h"
#include "schema.h"
#include "xrow.h"
#include "xlog.h"
#include "fio.h"
#include "space.h"
#include "index.h"

#include "vy_stmt.h"
#include "vy_quota.h"

#define HEAP_FORWARD_DECLARATION
#include "salad/heap.h"

#define vy_cmp(a, b) \
	((a) == (b) ? 0 : (((a) > (b)) ? 1 : -1))

enum vinyl_status {
	VINYL_OFFLINE,
	VINYL_INITIAL_RECOVERY_LOCAL,
	VINYL_INITIAL_RECOVERY_REMOTE,
	VINYL_FINAL_RECOVERY_LOCAL,
	VINYL_FINAL_RECOVERY_REMOTE,
	VINYL_ONLINE,
};

struct tx_manager;
struct vy_scheduler;
struct vy_task;
struct vy_stat;
struct vy_squash_queue;

/**
 * Global configuration of an entire vinyl instance (env object).
 */
struct vy_conf {
	/* path to vinyl_dir */
	char *path;
	/* memory */
	uint64_t memory_limit;
};

struct vy_env {
	/** Recovery status */
	enum vinyl_status status;
	/** The list of indexes for vinyl_info(). */
	struct rlist indexes;
	/** Configuration */
	struct vy_conf      *conf;
	/** TX manager */
	struct tx_manager   *xm;
	/** Scheduler */
	struct vy_scheduler *scheduler;
	/** Statistics */
	struct vy_stat      *stat;
	/** Upsert squash queue */
	struct vy_squash_queue *squash_queue;
	/** Mempool for struct vy_cursor */
	struct mempool      cursor_pool;
	/** Mempool for struct vy_page_read_task */
	struct mempool      read_task_pool;
	/** Allocator for tuples */
	struct lsregion     allocator;
	/** Key for thread-local ZSTD context */
	pthread_key_t       zdctx_key;
	/** Memory quota */
	struct vy_quota     quota;
	/** Timer for updating quota watermark. */
	ev_timer            quota_timer;
};

#define vy_crcs(p, size, crc) \
	crc32_calc(crc, (char*)p + sizeof(uint32_t), size - sizeof(uint32_t))

struct vy_latency {
	uint64_t count;
	double total;
	double max;
};

static void
vy_latency_update(struct vy_latency *lat, double v)
{
	lat->count++;
	lat->total += v;
	if (v > lat->max)
		lat->max = v;
}

static int
path_exists(const char *path)
{
	struct stat st;
	int rc = lstat(path, &st);
	return rc == 0;
}

enum vy_stat_name {
	VY_STAT_GET,
	VY_STAT_TX,
	VY_STAT_TX_OPS,
	VY_STAT_TX_WRITE,
	VY_STAT_CURSOR,
	VY_STAT_CURSOR_OPS,
	VY_STAT_LAST,
};

static const char *vy_stat_strings[] = {
	"get",
	"tx",
	"tx_ops",
	"tx_write",
	"cursor",
	"cursor_ops",
};

struct vy_stat {
	struct rmean *rmean;
	uint64_t write_count;
	uint64_t tx_rlb;
	uint64_t tx_conflict;
	struct vy_latency get_latency;
	struct vy_latency tx_latency;
	struct vy_latency cursor_latency;
	/**
	 * Dump bandwidth is needed for calculating the quota watermark.
	 * The higher the bandwidth, the later we can start dumping w/o
	 * suffering from transaction throttling. So we want to be very
	 * conservative about estimating the bandwidth.
	 *
	 * To make sure we don't overestimate it, we maintain a
	 * histogram of all observed measurements and assume the
	 * bandwidth to be equal to the 10th percentile, i.e. the
	 * best result among 10% worst measurements.
	 */
	struct histogram *dump_bw;
	int64_t dump_total;
};

static struct vy_stat *
vy_stat_new()
{
	enum { KB = 1000, MB = 1000 * 1000 };
	static int64_t bandwidth_buckets[] = {
		100 * KB, 200 * KB, 300 * KB, 400 * KB, 500 * KB,
		  1 * MB,   2 * MB,   3 * MB,   4 * MB,   5 * MB,
		 10 * MB,  20 * MB,  30 * MB,  40 * MB,  50 * MB,
		 60 * MB,  70 * MB,  80 * MB,  90 * MB, 100 * MB,
		110 * MB, 120 * MB, 130 * MB, 140 * MB, 150 * MB,
		160 * MB, 170 * MB, 180 * MB, 190 * MB, 200 * MB,
		220 * MB, 240 * MB, 260 * MB, 280 * MB, 300 * MB,
		320 * MB, 340 * MB, 360 * MB, 380 * MB, 400 * MB,
		450 * MB, 500 * MB, 550 * MB, 600 * MB, 650 * MB,
		700 * MB, 750 * MB, 800 * MB, 850 * MB, 900 * MB,
		950 * MB, 1000 * MB,
	};

	struct vy_stat *s = calloc(1, sizeof(*s));
	if (s == NULL) {
		diag_set(OutOfMemory, sizeof(*s), "stat", "struct");
		return NULL;
	}
	s->dump_bw = histogram_new(bandwidth_buckets,
				   lengthof(bandwidth_buckets));
	if (s->dump_bw == NULL) {
		free(s);
		return NULL;
	}
	/*
	 * Until we dump anything, assume bandwidth to be 10 MB/s,
	 * which should be fine for initial guess.
	 */
	histogram_collect(s->dump_bw, 10 * MB);

	s->rmean = rmean_new(vy_stat_strings, VY_STAT_LAST);
	if (s->rmean == NULL) {
		histogram_delete(s->dump_bw);
		free(s);
		return NULL;
	}
	return s;
}

static void
vy_stat_delete(struct vy_stat *s)
{
	histogram_delete(s->dump_bw);
	rmean_delete(s->rmean);
	free(s);
}

static void
vy_stat_get(struct vy_stat *s, ev_tstamp start)
{
	ev_tstamp diff = ev_now(loop()) - start;
	rmean_collect(s->rmean, VY_STAT_GET, 1);
	vy_latency_update(&s->get_latency, diff);
}

static void
vy_stat_tx(struct vy_stat *s, ev_tstamp start,
	   int ops, int write_count, size_t write_size)
{
	ev_tstamp diff = ev_now(loop()) - start;
	rmean_collect(s->rmean, VY_STAT_TX, 1);
	rmean_collect(s->rmean, VY_STAT_TX_OPS, ops);
	rmean_collect(s->rmean, VY_STAT_TX_WRITE, write_size);
	s->write_count += write_count;
	vy_latency_update(&s->tx_latency, diff);
}

static void
vy_stat_cursor(struct vy_stat *s, ev_tstamp start, int ops)
{
	ev_tstamp diff = ev_now(loop()) - start;
	rmean_collect(s->rmean, VY_STAT_CURSOR, 1);
	rmean_collect(s->rmean, VY_STAT_CURSOR_OPS, ops);
	vy_latency_update(&s->cursor_latency, diff);
}

static void
vy_stat_dump(struct vy_stat *s, ev_tstamp time, size_t written)
{
	histogram_collect(s->dump_bw, written / time);
	s->dump_total += written;
}

static int64_t
vy_stat_dump_bandwidth(struct vy_stat *s)
{
	/* See comment to vy_stat->dump_bw. */
	return histogram_percentile(s->dump_bw, 10);
}

static int64_t
vy_stat_tx_write_rate(struct vy_stat *s)
{
	return rmean_mean(s->rmean, VY_STAT_TX_WRITE);
}

/**
 * Apply the UPSERT statement to the REPLACE, UPSERT or DELETE statement.
 * If the second statement is
 * - REPLACE then update operations of the first one will be applied to the
 *   second and a REPLACE statement will be returned;
 *
 * - UPSERT then the new UPSERT will be created with combined operations of both
 *   arguments;
 *
 * - DELETE or NULL then the first one will be turned into REPLACE and returned
 *   as the result;
 *
 * @param upsert An UPSERT statement.
 * @param object An REPLACE/DELETE/UPSERT statement or NULL.
 * @param index Index that will be used for formatting result statement.
 * @param suppress_error True if ClientErrors must not be written to log.
 *
 * @retval NULL     Memory allocation error.
 * @retval not NULL Success.
 */
static struct vy_stmt *
vy_apply_upsert(const struct vy_stmt *upsert, const struct vy_stmt *object,
		const struct key_def *key_def,
		const struct tuple_format *format, bool suppress_error);

struct tree_mem_key {
	const struct vy_stmt *stmt;
	int64_t lsn;
};

struct vy_mem;

static int
vy_mem_tree_cmp(const struct vy_stmt *a, const struct vy_stmt *b,
		struct vy_mem *index);

static int
vy_mem_tree_cmp_key(const struct vy_stmt *a, struct tree_mem_key *key,
		    struct vy_mem *index);

#define VY_MEM_TREE_EXTENT_SIZE (16 * 1024)

#define BPS_TREE_NAME vy_mem_tree
#define BPS_TREE_BLOCK_SIZE 512
#define BPS_TREE_EXTENT_SIZE VY_MEM_TREE_EXTENT_SIZE
#define BPS_TREE_COMPARE(a, b, index) vy_mem_tree_cmp(a, b, index)
#define BPS_TREE_COMPARE_KEY(a, b, index) vy_mem_tree_cmp_key(a, b, index)
#define bps_tree_elem_t const struct vy_stmt *
#define bps_tree_key_t struct tree_mem_key *
#define bps_tree_arg_t struct vy_mem *
#define BPS_TREE_NO_DEBUG

#include "salad/bps_tree.h"

/*
 * vy_mem is an in-memory container for vy_stmt objects in
 * a single vinyl range.
 * Internally it uses bps_tree to stores struct vy_stmt *objects.
 * which are ordered by statement key and, for the same key,
 * by lsn, in descending order.
 *
 * For example, assume there are two statements with the same key,
 * but different LSN. These are duplicates of the same key,
 * maintained for the purpose of MVCC/consistent read view.
 * In Vinyl terms, they form a duplicate chain.
 *
 * vy_mem distinguishes between the first duplicate in the chain
 * and other keys in that chain.
 *
 * During insertion, the reference counter of vy_stmt is
 * incremented, during destruction all vy_stmt' reference
 * counters are decremented.
 */
struct vy_mem {
	/** Link in range->frozen list. */
	struct rlist in_frozen;
	/** Link in scheduler->dirty_mems list. */
	struct rlist in_dirty;
	struct vy_mem_tree tree;
	size_t used;
	int64_t min_lsn;
	/* A key definition for this index. */
	struct key_def *key_def;
	/* A tuple format for key_def. */
	struct tuple_format *format;
	/** version is initially 0 and is incremented on every write */
	uint32_t version;
};

static int
vy_mem_tree_cmp(const struct vy_stmt *a, const struct vy_stmt *b,
		struct vy_mem *index)
{
	int res = vy_stmt_compare(a, b, index->format, index->key_def);
	res = res ? res : a->lsn > b->lsn ? -1 : a->lsn < b->lsn;
	return res;
}

static int
vy_mem_tree_cmp_key(const struct vy_stmt *a, struct tree_mem_key *key,
		    struct vy_mem *index)
{
	int res = vy_stmt_compare(a, key->stmt, index->format, index->key_def);
	if (res == 0) {
		if (key->lsn == INT64_MAX - 1)
			return 0;
		res = a->lsn > key->lsn ? -1 : a->lsn < key->lsn;
	}
	return res;
}

static void *
vy_mem_tree_extent_alloc(void *ctx);

static void
vy_mem_tree_extent_free(void *ctx, void *p)
{
	/* Can't free part of region allocated memory. */
	(void)ctx;
	(void)p;
}

static struct vy_mem *
vy_mem_new(struct vy_env *env, struct key_def *key_def,
	   struct tuple_format *format)
{
	struct vy_mem *index = malloc(sizeof(*index));
	if (!index) {
		diag_set(OutOfMemory, sizeof(*index),
			 "malloc", "struct vy_mem");
		return NULL;
	}
	index->min_lsn = INT64_MAX;
	index->used = 0;
	index->key_def = key_def;
	index->version = 0;
	index->format = format;
	vy_mem_tree_create(&index->tree, index, vy_mem_tree_extent_alloc,
			   vy_mem_tree_extent_free, env);
	rlist_create(&index->in_frozen);
	rlist_create(&index->in_dirty);
	return index;
}

static void
vy_mem_delete(struct vy_mem *index)
{
	assert(index == index->tree.arg);
	TRASH(index);
	free(index);
}

/*
 * Return the older statement for the given one.
 */
static const struct vy_stmt *
vy_mem_older_lsn(struct vy_mem *mem, const struct vy_stmt *stmt,
		 const struct key_def *key_def)
{
	struct tree_mem_key tree_key;
	tree_key.stmt = stmt;
	tree_key.lsn = stmt->lsn - 1;
	bool exact = false;
	struct vy_mem_tree_iterator itr =
		vy_mem_tree_lower_bound(&mem->tree, &tree_key, &exact);

	if (vy_mem_tree_iterator_is_invalid(&itr))
		return NULL;

	const struct vy_stmt *result;
	result = *vy_mem_tree_iterator_get_elem(&mem->tree, &itr);
	if (vy_stmt_compare(result, stmt, mem->format, key_def) != 0)
		return NULL;
	return result;
}

/**
 * Run metadata. A run is a written to a file as a single
 * chunk.
 */
struct vy_run_info {
	/** Run page count. */
	uint32_t  count;
	/** Number of keys. */
	uint32_t  keys;
	/* Min and max lsn over all statements in the run. */
	int64_t  min_lsn;
	int64_t  max_lsn;
	/** Total sizeof run */
	uint64_t  total;
	/** Pages meta. */
	struct vy_page_info *page_infos;
};

struct vy_page_info {
	/* count of statements in the page */
	uint32_t count;
	/* offset of page data in run */
	uint64_t offset;
	/* size of page data in file */
	uint32_t size;
	/* size of page data in memory, i.e. unpacked */
	uint32_t unpacked_size;
	/* Offset of the min key in the parent run->pages_min. */
	uint32_t min_key_offset;
	/* minimal lsn of all records in page */
	int64_t min_lsn;
	/* maximal lsn of all records in page */
	int64_t max_lsn;
	/* minimal key */
	struct vy_stmt *min_key;
	/* row index offset in page */
	uint32_t row_index_offset;
};

static int
vy_page_info_create(struct vy_page_info *page_info, uint64_t offset,
		    const struct key_def *key_def, struct vy_stmt *min_key);

static void
vy_page_info_destroy(struct vy_page_info *page_info);

struct vy_run {
	struct vy_run_info info;
	/** Run data file. */
	int fd;
	/**
	 * Reference counter. The run file is closed and the run
	 * in-memory structure is freed only when it reaches 0.
	 * Needed to prevent coeio thread from using a closed
	 * (worse, reopened) file descriptor.
	 */
	int refs;
	/** Link in range->runs list. */
	struct rlist in_range;
};

struct vy_range {
	int64_t   id;
	/**
	 * Range lower bound. NULL if range is leftmost.
	 * Both 'begin' and 'end' statements have SELECT type with the full
	 * idexed key.
	 */
	struct vy_stmt *begin;
	/** Range upper bound. NULL if range is rightmost. */
	struct vy_stmt *end;
	struct vy_index *index;
	/** Total amount of memory used by this range (sum of mem->used). */
	size_t used;
	/** Minimal in-memory lsn (min over mem->min_lsn). */
	int64_t min_lsn;
	/** New run created for dump/compaction. */
	struct vy_run *new_run;
	/**
	 * List of all on-disk runs, linked by vy_run->in_range.
	 * The newer a run, the closer it to the list head.
	 */
	struct rlist runs;
	/** Number of entries in the ->runs list. */
	int run_count;
	/** Active in-memory index, i.e. the one used for insertions. */
	struct vy_mem *mem;
	/**
	 * List of frozen in-memory indexes, i.e. indexes that can't
	 * be inserted into, only read from, linked by vy_mem->in_frozen.
	 * The newer an index, the closer it to the list head.
	 */
	struct rlist frozen;
	/** Number of times the range was compacted. */
	int n_compactions;
	/** Points to the range being compacted to this range. */
	struct vy_range *shadow;
	/** List of ranges this range is being compacted to. */
	struct rlist compact_list;
	rb_node(struct vy_range) tree_node;
	struct heap_node   in_compact;
	struct heap_node   in_dump;
	/**
	 * Incremented whenever an in-memory index or on disk
	 * run is added to or deleted from this range. Used to
	 * invalidate iterators.
	 */
	uint32_t version;
};

typedef rb_tree(struct vy_range) vy_range_tree_t;

/**
 * A single operation made by a transaction:
 * a single read or write in a vy_index.
 */
struct txv {
	/** Transaction start logical time - unique ID of the transaction. */
	int64_t tsn;
	struct vy_index *index;
	struct vy_stmt *stmt;
	struct vy_tx *tx;
	/** Next in the transaction log. */
	struct stailq_entry next_in_log;
	/** Member of either read or write set. */
	rb_node(struct txv) in_set;
	/** true for read tx, false for write tx */
	bool is_read;
	/** true if that is a read statement,
	 * and there was no value found for that key */
	bool is_gap;
};

typedef rb_tree(struct txv) read_set_t;

/**
 * A struct for primary and secondary Vinyl indexes.
 *
 * Vinyl primary and secondary indexes work differently:
 *
 * - the primary index is fully covering (also known as
 *   "clustered" in MS SQL circles).
 *   It stores all tuple fields of the tuple coming from
 *   INSERT/REPLACE/UPDATE/DELETE operations. This index is
 *   the only place where the full tuple is stored.
 *
 * - a secondary index only stores parts participating in the
 *   secondary key, coalesced with parts of the primary key.
 *   Duplicate parts, i.e. identical parts of the primary and
 *   secondary key are only stored once. (@sa key_def_merge
 *   function). This reduces the disk and RAM space necessary to
 *   maintain a secondary index, but adds an extra look-up in the
 *   primary key for every fetched tuple.
 *
 * When a search in a secondary index is made, we first look up
 * the secondary index tuple, containing the primary key, and then
 * use this key to find the original tuple in the primary index.

 * While the primary index has only one key_def that is
 * used for validating tuples, secondary index needs four:
 *
 * - the first one is defined by the user. It contains the key
 *   parts of the secondary key, as present in the original tuple.
 *   This is user_key_def.
 *
 * - the second one is used to fetch key parts of the secondary
 *   key, *augmented* with the parts of the primary key from the
 *   original tuple. These parts concatenated together construe the
 *   tuple of the secondary key, i.e. the tuple stored. This is
 *   key_def_tuple_to_key.
 *
 * - the third one is used to compare tuples of the secondary key
 *   between each other. This is key_def.
 *   @sa key_def_build_secondary()
 *
 * - the last one is used to build a key for lookup in the primary
 *   index, based on the tuple fetched from the secondary index.
 *   This is key_def_secondary_to_primary.
 *   @sa key_def_build_secondary_to_primary()
 */
struct vy_index {
	struct vy_env *env;
	/**
	 * Conflict manager index. Contains all changes
	 * made by transaction before they commit. Is used
	 * to implement read committed isolation level, i.e.
	 * the changes made by a transaction are only present
	 * in this tree, and thus not seen by other transactions.
	 */
	read_set_t read_set;
	vy_range_tree_t tree;
	/** Number of ranges in this index. */
	int range_count;
	/** Number of runs in all ranges. */
	int run_count;
	/** Number of pages in all runs. */
	int page_count;
	/**
	 * Total number of statements in this index,
	 * stored both in memory and on disk.
	 */
	uint64_t stmt_count;
	/** Size of data stored on disk. */
	uint64_t size;
	/** Amount of memory used by in-memory indexes. */
	uint64_t used;
	/** Histogram of number of runs in range. */
	struct histogram *run_hist;
	/**
	 * Reference counter. Used to postpone index drop
	 * until all pending operations have completed.
	 */
	uint32_t refs;
	/** A schematic name for profiler output. */
	char *name;
	/** The path with index files. */
	char *path;

	/**
	 * A key definition for this index, used to
	 * compare tuples.
	 */
	struct key_def *key_def;
	/**
	 * A key definition that was declared by an user with
	 * space:create_index().
	 */
	struct key_def *user_key_def;
	/**
	 * A key definition for the key extraction from a tuple.
	 * NULL for primary index.
	 */
	struct key_def *key_def_tuple_to_key;
	/**
	 * A key definition to fetch the primary key from a
	 * secondary index tuple.
	 */
	struct key_def *key_def_secondary_to_primary;
	/* A tuple format for key_def. */
	struct tuple_format *format;

	/** Member of env->indexes. */
	struct rlist link;
	/**
	 * For each index range list modification,
	 * get a new range id and increment this variable.
	 * For new ranges, use this id as a sequence.
	 */
	int64_t range_id_max;
	/**
	 * Incremented for each change of the range list,
	 * to invalidate iterators.
	 */
	uint32_t version;
	/** Space to which the index belongs. */
	struct space *space;
	/**
	 * column_mask is the bitmask in that bit 'n' is set if
	 * user_key_def parts contains a part with fieldno equal
	 * to 'n'. This mask is used for update optimization
	 * (@sa vy_update).
	 */
	uint64_t column_mask;
};

/** @sa implementation for details. */
extern struct vy_index *
vy_index(struct Index *index);

/**
 * Get struct vy_index by a space index with the specified
 * identifier. If the index is not found then set the
 * corresponding error in the diagnostics area.
 * @param space Vinyl space.
 * @param iid   Identifier of the index for search.
 *
 * @retval not NULL Pointer to index->db
 * @retval NULL     The index is not found.
 */
static inline struct vy_index *
vy_index_find(struct space *space, uint32_t iid)
{
	struct Index *index = index_find(space, iid);
	if (index == NULL)
		return NULL;
	return vy_index(index);
}

/**
 * Get unique struct vy_index by a space index with the specified
 * identifier. If the index is not found or found not unique then
 * set the corresponding error in the diagnostics area.
 * @param space Vinyl space.
 * @param iid   Identifier of the index for search.
 *
 * @retval not NULL Pointer to index->db
 * @retval NULL     The index is not found, or found not unique.
 */
static inline struct vy_index *
vy_index_find_unique(struct space *space, uint32_t index_id)
{
	struct vy_index *index = vy_index_find(space, index_id);
	if (index != NULL && !index->user_key_def->opts.is_unique) {
		diag_set(ClientError, ER_MORE_THAN_ONE_TUPLE);
		return NULL;
	}
	return index;
}

/** Transaction state. */
enum tx_state {
	/** Initial state. */
	VINYL_TX_READY,
	/**
	 * A transaction is finished and validated in the engine.
	 * It may still be rolled back if there is an error
	 * writing the WAL.
	 */
	VINYL_TX_COMMIT,
	/** A transaction is aborted or rolled back. */
	VINYL_TX_ROLLBACK
};

/** Transaction type. */
enum tx_type {
	VINYL_TX_RO,
	VINYL_TX_RW
};

struct read_set_key {
	struct vy_stmt *stmt;
	int64_t tsn;
};

typedef rb_tree(struct txv) write_set_t;

struct vy_tx {
	/**
	 * In memory transaction log. Contains both reads
	 * and writes.
	 */
	struct stailq log;
	/**
	 * Writes of the transaction segregated by the changed
	 * vy_index object.
	 */
	write_set_t write_set;
	/**
	 * Version of write_set state; if the state changes (insert/remove),
	 * the version increments.
	 */
	uint32_t write_set_version;
	ev_tstamp start;
	enum tx_type type;
	enum tx_state state;
	/**
	 * The transaction is forbidden to commit unless it's read-only.
	 */
	bool is_aborted;
	/** Transaction logical start time. */
	int64_t tsn;
	/**
	 * Consistent read view LSN. Originally read-only transactions
	 * receive a read view lsn upon creation and do not see further
	 * changes.
	 * Other transactions are expected to be read-write and
	 * have vlsn == INT64_MAX to read newest data. Once a value read
	 * by such a transaction (T) is overwritten by another
	 * commiting transaction, T permanently goes to read view that does
	 * not see this change.
	 * If T does not have any write statements by the commit time it will
	 * be committed successfully, or aborted as conflicted otherwise.
	 */
	int64_t vlsn;
	rb_node(struct vy_tx) tree_node;
	/*
	 * For non-autocommit transactions, the list of open
	 * cursors. When a transaction ends, all open cursors are
	 * forcibly closed.
	 */
	struct rlist cursors;
	struct tx_manager *manager;
};

/**
 * Merge iterator takes several iterators as sources and sorts
 * output from them by the given order and LSN DESC. It has no filter,
 * it just sorts output from its sources.
 *
 * All statements from all sources can be traversed via
 * next_key()/next_lsn() like in a simple iterator (run, mem etc).
 * next_key() switches to the youngest statement of
 * the next key (according to the order), and next_lsn()
 * switches to an older statement of the same key.
 *
 * There are several merge optimizations, which expect that:
 *
 * 1) All sources are sorted by age, i.e. the most fresh
 * sources are added first.
 * 2) Mutable sources are added before read-blocking sources.
 *
 * The iterator can merge the write set of the current
 * transaction, that does not belong to any range but to entire
 * index, and mems and runs of some range. For this purpose the
 * iterator has a special flag (range_ended) that signals to the
 * read iterator that it must switch to the next range.
 */
struct vy_merge_iterator {
	/** Array of sources */
	struct vy_merge_src *src;
	/** Number of elements in the src array */
	uint32_t src_count;
	/** Number of elements allocated in the src array */
	uint32_t src_capacity;
	/** Current source offset that merge iterator is positioned on */
	uint32_t curr_src;
	/** Offset of the first source with is_mutable == true */
	uint32_t mutable_start;
	/** Next offset after the last source with is_mutable == true */
	uint32_t mutable_end;
	/* Index for key_def and ondex->version */
	struct vy_index *index;

	/* {{{ Range version checking */
	/* copy of index->version to track range tree changes */
	uint32_t index_version;
	/* current range */
	struct vy_range *curr_range;
	/* copy of curr_range->version to track mem/run lists changes */
	uint32_t range_version;
	/* Range version checking }}} */

	const struct vy_stmt *key;
	/** Iteration type. */
	enum iterator_type iterator_type;
	/** Current stmt that merge iterator is positioned on */
	struct vy_stmt *curr_stmt;
	/**
	 * All sources with this front_id are on the same key of
	 * current iteration (optimization)
	 */
	uint32_t front_id;
	/**
	 * If index is unique and full key is given we can
	 * optimize first search in order to avoid unnecessary
	 * reading from disk.  That flag is set to true during
	 * initialization if index is unique and  full key is
	 * given. After first _get or _next_key call is set to
	 * false
	 */
	bool unique_optimization;
	/**
	 * After first search with unique_optimization we must do some extra
	 * moves and optimizations for _next_lsn call. So that flag is set to
	 * true after first search and will set to false after consequent
	 * _next_key call */
	bool is_in_uniq_opt;
	/**
	 * This flag is set to false during initialization and
	 * means that we must do lazy search for first _get or
	 * _next call. After that is set to false
	 */
	bool search_started;
	/**
	 * If all sources marked with belong_range = true comes to
	 * the end of data this flag is automatically set to true;
	 * is false otherwise.  For read iterator range_ended = true
	 * means that it must switch to next range
	 */
	bool range_ended;
};

struct vy_range_iterator {
	struct vy_index *index;
	enum iterator_type iterator_type;
	const struct vy_stmt *key;
	struct vy_range *curr_range;
};

/**
 * Complex read iterator over vinyl index and write_set of current tx
 * Iterates over ranges, creates merge iterator for every range and outputs
 * the result.
 * Can also wor without transaction, just set tx = NULL in _open
 * Applyes upserts and skips deletes, so only one replace stmt for every key
 * is output
 */
struct vy_read_iterator {
	/* index to iterate over */
	struct vy_index *index;
	/* transaction to iterate over */
	struct vy_tx *tx;
	bool only_disk;

	/* search options */
	enum iterator_type iterator_type;
	const struct vy_stmt *key;
	const int64_t *vlsn;

	/* iterator over ranges */
	struct vy_range_iterator range_iterator;
	/* current range */
	struct vy_range *curr_range;
	/* merge iterator over current range */
	struct vy_merge_iterator merge_iterator;

	struct vy_stmt *curr_stmt;
	/* is lazy search started */
	bool search_started;
};

/**
 * Open the read iterator.
 * @param itr           Read iterator.
 * @param index         Vinyl index to iterate.
 * @param tx            Current transaction, if exists.
 * @param iterator_type Type of the iterator that determines order
 *                      of the iteration.
 * @param key           Key for the iteration.
 * @param vlsn          Maximal visible LSN of transactions.
 * @param only_disk     True, if no need to open vy_mems and tx.
 */
static void
vy_read_iterator_open(struct vy_read_iterator *itr,
		      struct vy_index *index, struct vy_tx *tx,
		      enum iterator_type iterator_type,
		      const struct vy_stmt *key, const int64_t *vlsn,
		      bool only_disk);

/**
 * Get the next statement with another key, or start the iterator,
 * if it wasn't started.
 * @param itr         Read iterator.
 * @param[out] result Found statement is stored here.
 *
 * @retval  0 Success.
 * @retval -1 Read error.
 */
static NODISCARD int
vy_read_iterator_next(struct vy_read_iterator *itr, struct vy_stmt **result);

/** Close the iterator and free resources. */
static void
vy_read_iterator_close(struct vy_read_iterator *itr);

/** Cursor. */
struct vy_cursor {
	/**
	 * A built-in transaction created when a cursor is open
	 * in autocommit mode.
	 */
	struct vy_tx tx_autocommit;
	struct vy_index *index;
	struct vy_stmt *key;
	/**
	 * Points either to tx_autocommit for autocommit mode or
	 * to a multi-statement transaction active when the cursor
	 * was created.
	 */
	struct vy_tx *tx;
	enum iterator_type iterator_type;
	/** The number of vy_cursor_next() invocations. */
	int n_reads;
	/** Cursor creation time, used for statistics. */
	ev_tstamp start;
	/**
	 * All open cursors are registered in a transaction
	 * they belong to. When the transaction ends, the cursor
	 * is closed.
	 */
	struct rlist next_in_tx;
	/** Iterator over index */
	struct vy_read_iterator iterator;
	/** Set to true, if need to check statements to match the cursor key. */
	bool need_check_eq;
};

/**
 * coio task for vinyl page read
 */
struct vy_page_read_task {
	/** parent */
	struct coio_task base;
	/** vinyl page metadata */
	struct vy_page_info page_info;
	/** vy_run with fd - ref. counted */
	struct vy_run *run;
	/** vy_env - contains environment with task mempool */
	struct vy_env *env;
	/** [out] resulting vinyl page */
	struct vy_page *page;
	/** [out] result code */
	int rc;
};

static struct txv *
txv_new(struct vy_index *index, struct vy_stmt *stmt, struct vy_tx *tx)
{
	struct txv *v = malloc(sizeof(struct txv));
	if (unlikely(v == NULL)) {
		diag_set(OutOfMemory, sizeof(struct txv), "malloc",
			 "struct txv");
		return NULL;
	}
	v->index = index;
	v->tsn = tx->tsn;
	v->stmt = stmt;
	vy_stmt_ref(stmt);
	v->tx = tx;
	return v;
}

static void
txv_delete(struct txv *v)
{
	vy_stmt_unref(v->stmt);
	free(v);
}

static int
read_set_cmp(struct txv *a, struct txv *b);

static int
read_set_key_cmp(struct read_set_key *a, struct txv *b);

rb_gen_ext_key(MAYBE_UNUSED static inline, read_set_, read_set_t, struct txv,
	       in_set, read_set_cmp, struct read_set_key *, read_set_key_cmp);

static struct txv *
read_set_search_key(read_set_t *rbtree, struct vy_stmt *stmt, int64_t tsn)
{
	struct read_set_key key;
	key.stmt = stmt;
	key.tsn = tsn;
	return read_set_search(rbtree, &key);
}

static int
read_set_cmp(struct txv *a, struct txv *b)
{
	assert(a->index == b->index);
	struct vy_index *idx = a->index;
	int rc = vy_stmt_compare(a->stmt, b->stmt, idx->format, idx->key_def);
	/**
	 * While in svindex older value are "bigger" than newer
	 * ones, i.e. the newest value comes first, in
	 * transactional index (read_set), we want to look
	 * at data in chronological order.
	 * @sa vy_mem_tree_cmp
	 */
	if (rc == 0)
		rc = a->tsn < b->tsn ? -1 : a->tsn > b->tsn;
	return rc;
}

static int
read_set_key_cmp(struct read_set_key *a, struct txv *b)
{
	struct vy_index *idx = b->index;
	int rc = vy_stmt_compare(a->stmt, b->stmt, idx->format, idx->key_def);
	if (rc == 0)
		rc = a->tsn < b->tsn ? -1 : a->tsn > b->tsn;
	return rc;
}

typedef rb_tree(struct vy_tx) tx_tree_t;

static int
tx_tree_cmp(struct vy_tx *a, struct vy_tx *b)
{
	int rc = vy_cmp(a->vlsn, b->vlsn);
	return rc ? rc : vy_cmp(a->tsn, b->tsn);
}

rb_gen(MAYBE_UNUSED static inline, tx_tree_, tx_tree_t, struct vy_tx,
       tree_node, tx_tree_cmp);


struct tx_manager {
	tx_tree_t tree;
	uint32_t count_rd;
	uint32_t count_rw;
	/** Transaction logical time. */
	int64_t tsn;
	/**
	 * The last committed log sequence number known to
	 * vinyl. Updated in vy_commit().
	 */
	int64_t lsn;
	/**
	 * View sequence number: the oldest read view maintained
	 * by the front end.
	 */
	int64_t vlsn;
	struct vy_env *env;
};

static void *
vy_mem_tree_extent_alloc(void *ctx)
{
	struct vy_env *env = (struct vy_env *) ctx;
	void *ret = lsregion_alloc(&env->allocator, VY_MEM_TREE_EXTENT_SIZE,
				   env->xm->lsn);
	if (ret == NULL)
		diag_set(OutOfMemory, VY_MEM_TREE_EXTENT_SIZE, "lsregion_alloc",
			 "ret");
	return ret;
}

/**
 * Abort all transaction which are reading the stmt v written by
 * tx.
 */
static void
txv_abort_all(struct vy_env *env, struct vy_tx *tx, struct txv *v)
{
	read_set_t *tree = &v->index->read_set;
	struct key_def *key_def = v->index->key_def;
	struct tuple_format *format = v->index->format;
	struct read_set_key key;
	key.stmt = v->stmt;
	key.tsn = 0;
	/** Find the first value equal to or greater than key */
	for (struct txv *abort = read_set_nsearch(tree, &key);
	     abort != NULL; abort = read_set_next(tree, abort)) {
		/* Check if we're still looking at the matching key. */
		if (vy_stmt_compare(key.stmt, abort->stmt, format, key_def))
			break;
		/* Don't abort self. */
		if (abort->tx == tx)
			continue;
		/* Delete of nothing does not cause a conflict */
		if (abort->is_gap && v->stmt->type == IPROTO_DELETE)
			continue;

		/* the found tx can only be commited as read-only */
		abort->tx->is_aborted = true;
		/* Set the read view of the found (now read-only) tx */
		if (abort->tx->vlsn == INT64_MAX) {
			abort->tx->vlsn = env->xm->lsn;
			tx_tree_insert(&env->xm->tree, abort->tx);
			if (env->xm->vlsn == INT64_MAX)
				env->xm->vlsn = abort->tx->vlsn;
			else
				assert(env->xm->vlsn <= env->xm->lsn);
		} else {
			assert(abort->tx->vlsn <= env->xm->lsn);
			assert(abort->tx->vlsn >= env->xm->vlsn);
		}
	}
}

static int
write_set_cmp(struct txv *a, struct txv *b)
{
	/* Order by index first, by key in the index second. */
	int rc = a->index < b->index ? -1 : a->index > b->index;
	if (rc == 0) {
		struct key_def *key_def = a->index->key_def;
		struct tuple_format *format = a->index->format;
		return vy_stmt_compare(a->stmt, b->stmt, format, key_def);
	}
	return rc;
}

struct write_set_key {
	struct vy_index *index;
	const struct vy_stmt *stmt;
};

static int
write_set_key_cmp(struct write_set_key *a, struct txv *b)
{
	/* Order by index first, by key in the index second. */
	int rc = a->index < b->index ? -1 : a->index > b->index;
	if (rc == 0) {
		if (a->stmt == NULL) {
			/*
			 * A special key to position search at the
			 * beginning of the index.
			 */
			return -1;
		}
		struct key_def *key_def = a->index->key_def;
		struct tuple_format *format = a->index->format;
		return vy_stmt_compare(a->stmt, b->stmt, format, key_def);
	}
	return rc;
}

rb_gen_ext_key(MAYBE_UNUSED static inline, write_set_, write_set_t, struct txv,
		in_set, write_set_cmp, struct write_set_key *,
		write_set_key_cmp);

static struct txv *
write_set_search_key(write_set_t *tree, struct vy_index *index,
		     const struct vy_stmt *data)
{
	struct write_set_key key = { .index = index, .stmt = data};
	return write_set_search(tree, &key);
}

static bool
vy_tx_is_ro(struct vy_tx *tx)
{
	return tx->type == VINYL_TX_RO ||
		tx->write_set.rbt_root == &tx->write_set.rbt_nil;
}

static struct tx_manager *
tx_manager_new(struct vy_env *env)
{
	struct tx_manager *m = malloc(sizeof(*m));
	if (m == NULL) {
		diag_set(OutOfMemory, sizeof(*m), "tx_manager", "struct");
		return NULL;
	}
	tx_tree_new(&m->tree);
	m->count_rd = 0;
	m->count_rw = 0;
	m->tsn = 0;
	m->lsn = 0;
	m->vlsn = INT64_MAX;
	m->env = env;
	return m;
}

static int
tx_manager_delete(struct tx_manager *m)
{
	free(m);
	return 0;
}

/*
 * Determine a lowest possible vlsn - the level below which the
 * history could be compacted.
 * If there are active read views, it is the m->vlsn. If there is
 * no active read view (m->vlsn == INT64_MAX), a read view could
 * be created at any moment with vlsn = m->lsn. Therefore, the
 * minimum of m->vlsn and m->lsn must be chosen.
 */
static int64_t
tx_manager_vlsn(struct tx_manager *m)
{
	assert(m->vlsn == INT64_MAX || m->vlsn <= m->lsn);
	int64_t vlsn = m->vlsn;
	if (vlsn == INT64_MAX)
		vlsn = m->lsn;
	return vlsn;
}

static struct txv *
read_set_delete_cb(read_set_t *t, struct txv *v, void *arg)
{
	(void) t;
	(void) arg;
	txv_delete(v);
	return NULL;
}

static void
vy_tx_begin(struct tx_manager *m, struct vy_tx *tx, enum tx_type type)
{
	stailq_create(&tx->log);
	write_set_new(&tx->write_set);
	tx->write_set_version = 0;
	tx->start = ev_now(loop());
	tx->manager = m;
	tx->state = VINYL_TX_READY;
	tx->type = type;
	tx->is_aborted = false;
	rlist_create(&tx->cursors);

	tx->tsn = ++m->tsn;

	if (type == VINYL_TX_RO) {
		/* read-only tx obtains read view at once */
		tx->vlsn = m->lsn;
		tx_tree_insert(&m->tree, tx);
		if (m->vlsn == INT64_MAX)
			m->vlsn = tx->vlsn;
		m->count_rd++;
	} else {
		/* possible read-write tx reads latest changes */
		tx->vlsn = INT64_MAX;
		m->count_rw++;
	}
}

/**
 * Remember the read in the conflict manager index.
 */
static int
vy_tx_track(struct vy_tx *tx, struct vy_index *index,
	    struct vy_stmt *key, bool is_gap)
{
	if (tx->type == VINYL_TX_RO || tx->is_aborted)
		return 0; /* no reason to track reads */
	uint32_t part_count = vy_stmt_part_count(key);
	if (part_count >= index->key_def->part_count) {
		struct txv *v =
			write_set_search_key(&tx->write_set, index, key);
		if (v != NULL && (v->stmt->type == IPROTO_REPLACE ||
				  v->stmt->type == IPROTO_DELETE)) {
			/** reading from own write set is serializable */
			return 0;
		}
	}
	struct txv *v = read_set_search_key(&index->read_set, key, tx->tsn);
	if (v == NULL) {
		if ((v = txv_new(index, key, tx)) == NULL)
			return -1;
		v->is_read = true;
		v->is_gap = is_gap;
		stailq_add_tail_entry(&tx->log, v, next_in_log);
		read_set_insert(&index->read_set, v);
	}
	return 0;
}

static void
tx_manager_end(struct tx_manager *m, struct vy_tx *tx)
{
	if (tx->vlsn != INT64_MAX) {
		tx_tree_remove(&m->tree, tx);
		if (tx->vlsn == m->vlsn) {
			struct vy_tx * oldest = tx_tree_first(&m->tree);
			m->vlsn = oldest ? oldest->vlsn : INT64_MAX;
		}
	}

	/** Abort all open cursors. */
	struct vy_cursor *c;
	rlist_foreach_entry(c, &tx->cursors, next_in_tx)
		c->tx = NULL;

	/* Remove from the conflict manager index */
	struct txv *v;
	stailq_foreach_entry(v, &tx->log, next_in_log)
		if (v->is_read)
			read_set_remove(&v->index->read_set, v);

	if (tx->type == VINYL_TX_RO)
		m->count_rd--;
	else
		m->count_rw--;
}

static void
vy_tx_rollback(struct vy_env *e, struct vy_tx *tx)
{
	if (tx->state == VINYL_TX_READY) {
		/** freewill rollback, vy_prepare have not been called yet */
		tx_manager_end(tx->manager, tx);
	}
	struct txv *v, *tmp;
	stailq_foreach_entry_safe(v, tmp, &tx->log, next_in_log)
		txv_delete(v);
	e->stat->tx_rlb++;
}

static struct vy_page_info *
vy_run_page_info(struct vy_run *run, uint32_t pos)
{
	assert(pos < run->info.count);
	return &run->info.page_infos[pos];
}

static uint64_t
vy_run_total(struct vy_run *run)
{
	if (unlikely(run->info.page_infos == NULL))
		return 0;
	return run->info.total;
}

static uint64_t
vy_run_size(struct vy_run *run)
{
	return sizeof(run->info) +
	       run->info.count * sizeof(struct vy_page_info);
}

static struct vy_run *
vy_run_new()
{
	struct vy_run *run = (struct vy_run *)malloc(sizeof(struct vy_run));
	if (unlikely(run == NULL)) {
		diag_set(OutOfMemory, sizeof(struct vy_run), "malloc",
			 "struct vy_run");
		return NULL;
	}
	memset(&run->info, 0, sizeof(run->info));
	run->fd = -1;
	run->refs = 1;
	rlist_create(&run->in_range);
	return run;
}

static void
vy_run_delete(struct vy_run *run)
{
	if (run->fd >= 0 && close(run->fd) < 0)
		say_syserror("close failed");
	if (run->info.page_infos != NULL) {
		uint32_t page_no;
		for (page_no = 0; page_no < run->info.count; ++page_no)
			vy_page_info_destroy(run->info.page_infos + page_no);
		free(run->info.page_infos);
	}
	TRASH(run);
	free(run);
}

static void
vy_run_ref(struct vy_run *run)
{
	assert(run->refs > 0);
	run->refs++;
}

static void
vy_run_unref(struct vy_run *run)
{
	assert(run->refs > 0);
	if (--run->refs == 0)
		vy_run_delete(run);
}

enum vy_file_type {
	VY_FILE_INDEX,
	VY_FILE_RUN,
	vy_file_MAX,
};

static const char *vy_file_suffix[] = {
	"index",	/* VY_FILE_INDEX */
	"run",		/* VY_FILE_RUN */
};

/** xlog meta type for .run files */
#define XLOG_META_TYPE_RUN "RUN"

/** xlog meta type for .index files */
#define XLOG_META_TYPE_INDEX "INDEX"

static int
vy_run_parse_name(const char *name, int64_t *index_lsn, int64_t *range_id,
		  int *run_id, enum vy_file_type *type)
{
	int n = 0;

	sscanf(name, "%"SCNx64".%"SCNx64".%d.%n",
	       index_lsn, range_id, run_id, &n);
	if (*run_id < 0)
		return -1;

	int i;
	const char *suffix = name + n;
	for (i = 0; i < vy_file_MAX; i++) {
		if (strcmp(suffix, vy_file_suffix[i]) == 0)
			break;
	}
	if (i >= vy_file_MAX)
		return -1;

	*type = i;
	return 0;
}

static int
vy_run_snprint_path(char *buf, size_t size, const char *dir,
		    int64_t index_lsn, int64_t range_id, int run_id,
		    enum vy_file_type type)
{
	return snprintf(buf, size, "%s/%016"PRIx64".%016"PRIx64".%d.%s",
			dir, index_lsn, range_id, run_id,
			vy_file_suffix[type]);
}

static void
vy_index_acct_run(struct vy_index *index, struct vy_run *run)
{
	index->run_count++;
	index->page_count += run->info.count;
	index->stmt_count += run->info.keys;
	index->size += vy_run_size(run) + vy_run_total(run);
}

static void
vy_index_unacct_run(struct vy_index *index, struct vy_run *run)
{
	index->run_count--;
	index->page_count -= run->info.count;
	index->stmt_count -= run->info.keys;
	index->size -= vy_run_size(run) + vy_run_total(run);
}

static void
vy_index_acct_range(struct vy_index *index, struct vy_range *range)
{
	struct vy_run *run;
	rlist_foreach_entry(run, &range->runs, in_range)
		vy_index_acct_run(index, run);
	histogram_collect(index->run_hist, range->run_count);
}

static void
vy_index_unacct_range(struct vy_index *index, struct vy_range *range)
{
	struct vy_run *run;
	rlist_foreach_entry(run, &range->runs, in_range)
		vy_index_unacct_run(index, run);
	histogram_discard(index->run_hist, range->run_count);
}

static void
vy_index_acct_range_dump(struct vy_index *index,
			 struct vy_range *range, struct vy_run *run)
{
	vy_index_acct_run(index, run);
	histogram_discard(index->run_hist, range->run_count - 1);
	histogram_collect(index->run_hist, range->run_count);
}

static int
vy_range_snprint(char *buf, int size, const struct vy_range *range)
{
	int total = 0;
	struct key_def *key_def = range->index->key_def;

	SNPRINT(total, snprintf, buf, size,
		"%"PRIu32"/%"PRIu32"/%016"PRIx64".%016"PRIx64"(",
		 key_def->space_id, key_def->iid, key_def->opts.lsn, range->id);
	SNPRINT(total, vy_key_snprint, buf, size,
		range->begin != NULL ? vy_stmt_data(range->begin) : NULL);
	SNPRINT(total, snprintf, buf, size, "..");
	SNPRINT(total, vy_key_snprint, buf, size,
		range->end != NULL ? vy_stmt_data(range->end) : NULL);
	SNPRINT(total, snprintf, buf, size, ")");
	return total;
}

static const char *
vy_range_str(struct vy_range *range)
{
	char *buf = tt_static_buf();
	vy_range_snprint(buf, TT_STATIC_BUF_LEN, range);
	return buf;
}

static void
vy_scheduler_add_range(struct vy_scheduler *, struct vy_range *range);
static void
vy_scheduler_update_range(struct vy_scheduler *, struct vy_range *range);
static void
vy_scheduler_remove_range(struct vy_scheduler *, struct vy_range*);
static void
vy_scheduler_mem_dirtied(struct vy_scheduler *scheduler, struct vy_mem *mem);
static void
vy_scheduler_mem_dumped(struct vy_scheduler *scheduler, struct vy_mem *mem);

static int
vy_range_tree_cmp(struct vy_range *a, struct vy_range *b);

static int
vy_range_tree_key_cmp(const struct vy_stmt *a, struct vy_range *b);

rb_gen_ext_key(MAYBE_UNUSED static inline, vy_range_tree_, vy_range_tree_t,
	       struct vy_range, tree_node, vy_range_tree_cmp,
	       const struct vy_stmt *, vy_range_tree_key_cmp);

static void
vy_range_delete(struct vy_range *);

static struct vy_range *
vy_range_tree_free_cb(vy_range_tree_t *t, struct vy_range *range, void *arg)
{
	(void)t;
	struct vy_index *index = (struct vy_index *) arg;
	vy_scheduler_remove_range(index->env->scheduler, range);
	vy_range_delete(range);
	return NULL;
}

static void
vy_index_ref(struct vy_index *index);

static void
vy_index_unref(struct vy_index *index);

static int
vy_range_tree_cmp(struct vy_range *range_a, struct vy_range *range_b)
{
	if (range_a == range_b)
		return 0;

	/* Any key > -inf. */
	if (range_a->begin == NULL)
		return -1;
	if (range_b->begin == NULL)
		return 1;

	assert(range_a->index == range_b->index);
	struct key_def *key_def = range_a->index->key_def;
	return vy_key_compare(range_a->begin, range_b->begin, key_def);
}

static int
vy_range_tree_key_cmp(const struct vy_stmt *stmt, struct vy_range *range)
{
	/* Any key > -inf. */
	if (range->begin == NULL)
		return 1;

	struct key_def *key_def = range->index->key_def;
	struct tuple_format *format = range->index->format;
	return vy_stmt_compare_with_key(stmt, range->begin, format, key_def);
}

static void
vy_index_delete(struct vy_index *index);

static void
vy_range_iterator_open(struct vy_range_iterator *itr, struct vy_index *index,
		       enum iterator_type iterator_type,
		       const struct vy_stmt *key)
{
	itr->index = index;
	itr->iterator_type = iterator_type;
	itr->key = key;
	itr->curr_range = NULL;
}

/*
 * Find the first range in which a given key should be looked up.
 * This function only takes into account the actual range tree layout
 * and does not handle the compaction case, when a range being compacted
 * is replaced by new range(s) back-pointing to it via range->shadow.
 * Therefore, as is, this function is only suitable for handling
 * insertions, which always go to in-memory indexes of ranges found in
 * the range tree. Select operations have to check range->shadow to
 * guarantee that no keys are skipped no matter if there is a
 * compaction operation in progress (see vy_range_iterator_next()).
 */
static struct vy_range *
vy_range_tree_find_by_key(vy_range_tree_t *tree,
			  enum iterator_type iterator_type,
			  struct tuple_format *format, struct key_def *key_def,
			  const struct vy_stmt *key)
{
	if (vy_stmt_part_count(key) == 0) {
		switch (iterator_type) {
		case ITER_LT:
		case ITER_LE:
			return vy_range_tree_last(tree);
		case ITER_GT:
		case ITER_GE:
		case ITER_EQ:
			return vy_range_tree_first(tree);
		default:
			unreachable();
			return NULL;
		}
	}
	/* route */
	struct vy_range *range;
	if (iterator_type == ITER_GE || iterator_type == ITER_GT ||
	    iterator_type == ITER_EQ) {
		/**
		 * Case 1. part_count == 1, looking for [10]. ranges:
		 * {1, 3, 5} {7, 8, 9} {10, 15 20} {22, 32, 42}
		 *                      ^looking for this
		 * Case 2. part_count == 1, looking for [10]. ranges:
		 * {1, 2, 4} {5, 6, 7, 8} {50, 100, 200}
		 *            ^looking for this
		 * Case 3. part_count == 2, looking for [10]. ranges:
		 * {[1, 2], [2, 3]} {[9, 1], [10, 1], [10 2], [11 3]} {[12,..}
		 *                   ^looking for this
		 * Case 4. part_count == 2, looking for [10]. ranges:
		 * {[1, 2], [10, 1]} {[10, 2] [10 3] [11 3]} {[12, 1]..}
		 *  ^looking for this
		 * Case 5. part_count does not matter, looking for [10].
		 * ranges:
		 * {100, 200}, {300, 400}
		 * ^looking for this
		 */
		/**
		 * vy_range_tree_psearch finds least range with begin == key
		 * or previous if equal was not found
		 */
		range = vy_range_tree_psearch(tree, key);
		/* switch to previous for case (4) */
		if (range != NULL && range->begin != NULL &&
		    vy_stmt_part_count(key) < key_def->part_count &&
		    vy_stmt_compare_with_key(key, range->begin, format,
					     key_def) == 0)
			range = vy_range_tree_prev(tree, range);
		/* for case 5 or subcase of case 4 */
		if (range == NULL)
			range = vy_range_tree_first(tree);
	} else {
		assert(iterator_type == ITER_LT || iterator_type == ITER_LE);
		/**
		 * Case 1. part_count == 1, looking for [10]. ranges:
		 * {1, 3, 5} {7, 8, 9} {10, 15 20} {22, 32, 42}
		 *                      ^looking for this
		 * Case 2. part_count == 1, looking for [10]. ranges:
		 * {1, 2, 4} {5, 6, 7, 8} {50, 100, 200}
		 *            ^looking for this
		 * Case 3. part_count == 2, looking for [10]. ranges:
		 * {[1, 2], [2, 3]} {[9, 1], [10, 1], [10 2], [11 3]} {[12,..}
		 *                   ^looking for this
		 * Case 4. part_count == 2, looking for [10]. ranges:
		 * {[1, 2], [10, 1]} {[10, 2] [10 3] [11 3]} {[12, 1]..}
		 *                    ^looking for this
		 * Case 5. part_count does not matter, looking for [10].
		 * ranges:
		 * {1, 2}, {3, 4, ..}
		 *          ^looking for this
		 */
		/**
		 * vy_range_tree_nsearch finds most range with begin == key
		 * or next if equal was not found
		 */
		range = vy_range_tree_nsearch(tree, key);
		if (range != NULL) {
			/* fix curr_range for cases 2 and 3 */
			if (range->begin != NULL &&
			    vy_stmt_compare_with_key(key, range->begin, format,
						     key_def) != 0) {
				struct vy_range *prev;
				prev = vy_range_tree_prev(tree,
							  range);
				if (prev != NULL)
					range = prev;
			}
		} else {
			/* Case 5 */
			range = vy_range_tree_last(tree);
		}
	}
	/* Range tree must span all possible keys. */
	assert(range != NULL);
	return range;
}

/**
 * Iterate to the next range. The next range is returned in @result.
 * This function is supposed to be used for iterating over a subset of
 * keys in an index. Therefore it should handle the compaction case,
 * i.e. check if the range returned by vy_range_tree_find_by_key()
 * is a replacement range and return a pointer to the range being
 * compacted if it is.
 */
static void
vy_range_iterator_next(struct vy_range_iterator *itr, struct vy_range **result)
{
	struct vy_range *curr = itr->curr_range;
	struct vy_range *next;
	struct vy_index *index = itr->index;
	struct key_def *def = index->key_def;
	struct tuple_format *format = index->format;

	if (curr == NULL) {
		/* First iteration */
		if (unlikely(index->range_count == 1))
			next = vy_range_tree_first(&index->tree);
		else
			next = vy_range_tree_find_by_key(&index->tree,
							 itr->iterator_type,
							 format, def, itr->key);
		goto check;
	}
next:
	switch (itr->iterator_type) {
	case ITER_LT:
	case ITER_LE:
		next = vy_range_tree_prev(&index->tree, curr);
		break;
	case ITER_GT:
	case ITER_GE:
		next = vy_range_tree_next(&index->tree, curr);
		break;
	case ITER_EQ:
		if (curr->end != NULL &&
		    vy_stmt_compare_with_key(itr->key, curr->end, format,
					     def) >= 0) {
			/* A partial key can be found in more than one range. */
			next = vy_range_tree_next(&index->tree, curr);
		} else {
			next = NULL;
		}
		break;
	default:
		unreachable();
	}
check:
	/*
	 * When compaction starts, the selected range is replaced with
	 * one or more new ranges, each of which has ->shadow pointing
	 * to the original range (see vy_task_compact_new()). New
	 * ranges must not be read from until compaction has finished,
	 * because (1) their in-memory indexes are linked to the
	 * original range and (2) they don't have on-disk runs yet. So
	 * whenever we encounter such a range we return ->shadow
	 * instead. We also have to be careful not to return the same
	 * range twice in case of split taking place.
	 */
	if (next != NULL && next->shadow != NULL) {
		if (curr != NULL && curr->shadow == next->shadow) {
			curr = next;
			goto next;
		}
		*result = next->shadow;
	} else {
		*result = next;
	}
	itr->curr_range = next;
}

/**
 * Position iterator @itr to the range that contains @last_stmt and
 * return the current range in @result. If @last_stmt is NULL, restart
 * the iterator.
 */
static void
vy_range_iterator_restore(struct vy_range_iterator *itr,
			  const struct vy_stmt *last_stmt,
			  struct vy_range **result)
{
	struct vy_index *index = itr->index;
	struct vy_range *curr = vy_range_tree_find_by_key(&index->tree,
				itr->iterator_type, index->format,
				index->key_def,
				last_stmt != NULL ? last_stmt : itr->key);
	itr->curr_range = curr;
	*result = curr->shadow != NULL ? curr->shadow : curr;
}

static void
vy_index_add_range(struct vy_index *index, struct vy_range *range)
{
	vy_range_tree_insert(&index->tree, range);
	index->range_count++;
}

static void
vy_index_remove_range(struct vy_index *index, struct vy_range *range)
{
	vy_range_tree_remove(&index->tree, range);
	index->range_count--;
}

/*
 * Check if a is left-adjacent to b, i.e. a->end == b->begin.
 */
static bool
vy_range_is_adjacent(struct vy_range *a, struct vy_range *b,
		     struct key_def *key_def)
{
	if (a->end == NULL || b->begin == NULL)
		return false;
	assert(a->index == b->index);
	return vy_key_compare(a->end, b->begin, key_def) == 0;
}

/*
 * Check if a precedes b, i.e. a->end <= b->begin.
 */
static bool
vy_range_precedes(struct vy_range *a, struct vy_range *b,
		  struct key_def *key_def)
{
	if (a->end == NULL || b->begin == NULL)
		return false;
	assert(a->index == b->index);
	return vy_key_compare(a->end, b->begin, key_def) <= 0;
}

/*
 * Check if a ends before b, i.e. a->end < b->end.
 */
static bool
vy_range_ends_before(struct vy_range *a, struct vy_range *b,
		     struct key_def *key_def)
{
	if (b->end == NULL)
		return a->end != NULL;
	if (a->end == NULL)
		return false;
	assert(a->index == b->index);
	return vy_key_compare(a->end, b->end, key_def) < 0;
}

/*
 * Check if ranges present in an index span a range w/o holes. If they
 * do, delete the range, otherwise remove all ranges of the index
 * intersecting the range (if any) and insert the range instead.
 */
static void
vy_index_recover_range(struct vy_index *index, struct vy_range *range)
{
	/*
	 * The algorithm can be briefly outlined by the following steps:
	 *
	 * 1. Find the first range in the index having an intersection
	 *    with the given one. If there is no such range, go to step
	 *    4, otherwise check if the found range can be the first
	 *    spanning range, i.e. if it starts before or at the same
	 *    point as the given range. If it does, proceed to step 2,
	 *    otherwise go to step 3.
	 *
	 * 2. Check if there are holes in the span. To do it, iterate
	 *    over all intersecting ranges and check that for each two
	 *    neighbouring ranges the end of the first one equals the
	 *    beginning of the second one. If there is a hole, proceed
	 *    to step 3, otherwise delete the given range and return.
	 *
	 * 3. Remove all ranges intersecting the given range from the
	 *    index.
	 *
	 * 4. Insert the given range to the index.
	 */
	struct vy_range *first, *prev, *n;

	first = vy_range_tree_first(&index->tree);
	if (first == NULL) {
		/* Trivial case - the index tree is empty. */
		goto insert;
	}

	/*
	 * 1. Find the first intersecting range.
	 */
	if (range->begin == NULL) {
		/*
		 * The given range starts with -inf.
		 * Check the leftmost.
		 */
		if (vy_range_precedes(range, first, index->key_def)) {
			/*
			 * The given range precedes the leftmost,
			 * hence no intersection is possible.
			 *
			 *   ----range----|   |----first----|
			 */
			goto insert;
		}
		if (first->begin != NULL) {
			/*
			 * The leftmost range does not span -inf,
			 * so there cannot be a span, but there is
			 * an intersection.
			 *
			 *   -----range-----|
			 *              |------first------|
			 */
			goto replace;
		}
		/*
		 * Possible span. Check for holes.
		 *
		 *   --------range--------|
		 *   ----first----|
		 */
		goto check;
	}
	/*
	 * The given range starts with a finite key (> -inf).
	 * Check the predecessor.
	 */
	prev = vy_range_tree_psearch(&index->tree, range->begin);
	if (prev == NULL) {
		/*
		 * There is no predecessor, i.e. no range with
		 * begin <= range->begin. Check if the first range
		 * in the index intersects the given range.
		 */
		if (vy_range_precedes(range, first, index->key_def)) {
			/*
			 * No intersections. The given range is
			 * going to be leftmost in the index.
			 *
			 *   |----range----|   |---first---|
			 */
			goto insert;
		}
		/*
		 * Neither strict succession nor strict precedence:
		 * the first range intersects the given one.
		 *
		 *   |------range------|
		 *                |------first------|
		 */
		goto replace;
	}
	/*
	 * There is a predecessor. Check whether it intersects
	 * the given range.
	 */
	if (vy_range_precedes(prev, range, index->key_def)) {
		/*
		 * The predecessor does not intersect the given
		 * range, hence there is no span. Check if there
		 * is an intersection with the successor (if any).
		 */
		n = vy_range_tree_next(&index->tree, prev);
		if (n == NULL || vy_range_precedes(range, n, index->key_def)) {
			/*
			 * No successor or the given range
			 * precedes it, hence no intersections.
			 *
			 *   |--prev--| |--range--| |--next--|
			 */
			goto insert;
		} else {
			/*
			 * There is an overlap with the successor.
			 *
			 *   |--prev--| |--range--|
			 *                    |-----next-----|
			 */
			first = n;
			goto replace;
		}
	} else {
		/*
		 * There is an intersection between the given range
		 * and the predecessor, so there might be a span.
		 * Check for holes.
		 *
		 *   |-------prev-------|
		 *                |-------range-------|
		 */
		first = prev;
	}
check:
	/*
	 * 2. Check for holes in the spanning range.
	 */
	n = first;
	prev = NULL;
	do {
		if (prev != NULL &&
		    !vy_range_is_adjacent(prev, n, index->key_def)) {
			/*
			 * There is a hole in the spanning range.
			 *
			 *   |---prev---|   |---next---|
			 *        |----------range----------|
			 */
			break;
		}
		if (!vy_range_ends_before(n, range, index->key_def)) {
			/*
			 * Spanned the whole range w/o holes.
			 *
			 *                       |---next---|
			 *   |----------range----------|
			 */
			say_warn("found stale range %s", vy_range_str(range));
			vy_range_delete(range);
			return;
		}
		prev = n;
		n = vy_range_tree_next(&index->tree, n);
	} while (n != NULL);
	/* Fall through. */
replace:
	/*
	 * 3. Remove intersecting ranges.
	 */
	n = first;
	do {
		struct vy_range *next = vy_range_tree_next(&index->tree, n);
		say_warn("found partial range %s", vy_range_str(n));
		vy_index_remove_range(index, n);
		vy_range_delete(n);
		n = next;
	} while (n != NULL && !vy_range_precedes(range, n, index->key_def));
	/* Fall through. */
insert:
	/*
	 * 4. Insert the given range to the index.
	 */
	vy_index_add_range(index, range);
	say_debug("range recover insert: %s", vy_range_str(range));
}

/* dump statement to the run page buffers (stmt header and data) */
static int
vy_run_dump_stmt(struct vy_stmt *value, struct xlog *data_xlog,
		 struct vy_page_info *info, const struct key_def *key_def)
{
	struct xrow_header xrow;
	if (vy_stmt_encode(value, key_def, &xrow) < 0)
		return -1;
	ssize_t row_size;
	if ((row_size = xlog_write_row(data_xlog, &xrow)) < 0)
		return -1;

	info->unpacked_size += row_size;

	++info->count;
	if (value->lsn > info->max_lsn)
		info->max_lsn = value->lsn;
	if (value->lsn < info->min_lsn)
		info->min_lsn = value->lsn;
	return 0;
}

struct vy_write_iterator;

static struct vy_write_iterator *
vy_write_iterator_new(struct vy_index *index, bool is_last_level,
		      int64_t oldest_vlsn);
static NODISCARD int
vy_write_iterator_add_run(struct vy_write_iterator *wi,
			  struct vy_range *range, struct vy_run *run);
static NODISCARD int
vy_write_iterator_add_mem(struct vy_write_iterator *wi, struct vy_mem *mem);
static NODISCARD int
vy_write_iterator_next(struct vy_write_iterator *wi, struct vy_stmt **ret);

static void
vy_write_iterator_delete(struct vy_write_iterator *wi);

/**
 * Initialize page info struct
 *
 * @retval 0 for Success
 * @retval -1 for error
 */
static int
vy_page_info_create(struct vy_page_info *page_info, uint64_t offset,
		    const struct key_def *key_def, struct vy_stmt *min_key)
{
	memset(page_info, 0, sizeof(*page_info));
	page_info->min_lsn = INT64_MAX;
	page_info->offset = offset;
	page_info->unpacked_size = 0;
	page_info->min_key = vy_stmt_extract_key(min_key, key_def);
	return page_info->min_key != NULL ? 0: -1;
}

/**
 * Destroy page info struct
 */
static void
vy_page_info_destroy(struct vy_page_info *page_info)
{
	if (page_info->min_key != NULL)
		vy_stmt_unref(page_info->min_key);
}

/**
 * Encode uint32_t array of row offsets (row index) as xrow
 *
 * @param row_index row index
 * @param count size of row index
 * @param[out] xrow xrow to fill.
 * @retval 0 for success
 * @retval -1 for error
 */
static int
vy_row_index_encode(const uint32_t *row_index, uint32_t count,
		    struct xrow_header *xrow)
{
	memset(xrow, 0, sizeof(*xrow));
	xrow->type = IPROTO_REPLACE;

	struct request request;
	request_create(&request, IPROTO_REPLACE);
	request.space_id = BOX_VINYL_ROW_INDEX_ID;
	size_t tuple_size = mp_sizeof_array(1) +
			    mp_sizeof_bin(sizeof(uint32_t) * count);
	char *tuple = region_alloc(&fiber()->gc, tuple_size);
	if (tuple == NULL) {
		diag_set(OutOfMemory, tuple_size, "region", "row index");
		return -1;
	}
	request.tuple = tuple;
	tuple = mp_encode_array(tuple, 1);
	tuple = mp_encode_binl(tuple, sizeof(uint32_t) * count);
	for (uint32_t i = 0; i < count; ++i)
		tuple = mp_store_u32(tuple, row_index[i]);
	request.tuple_end = tuple;
	assert(request.tuple_end == request.tuple + tuple_size);
	xrow->bodycnt = request_encode(&request, xrow->body);
	return xrow->bodycnt >= 0 ? 0 : -1;
}

/**
 * Write statements from the iterator to a new page in the run,
 * update page and run statistics.
 *
 *  @retval  1 all is ok, the iterator is finished
 *  @retval  0 all is ok, the iterator isn't finished
 *  @retval -1 error occurred
 */
static int
vy_run_write_page(struct vy_run_info *run_info, struct xlog *data_xlog,
		  struct vy_write_iterator *wi,
		  const struct vy_stmt *split_key,
		  uint32_t *page_info_capacity, struct vy_stmt **curr_stmt,
		  const struct key_def *key_def,
		  const struct tuple_format *format)
{
	assert(curr_stmt);
	if (*curr_stmt == NULL)
		return 1;

	/* row offsets accumulator */
	struct ibuf row_index_buf;
	ibuf_create(&row_index_buf, &cord()->slabc, sizeof(uint32_t) * 4096);

	if (run_info->count >= *page_info_capacity) {
		uint32_t cap = *page_info_capacity > 0 ?
			*page_info_capacity * 2 : 16;
		struct vy_page_info *new_infos =
			realloc(run_info->page_infos, cap * sizeof(*new_infos));
		if (new_infos == NULL) {
			diag_set(OutOfMemory, cap, "realloc",
				 "struct vy_page_info");
			goto error_row_index;
		}
		run_info->page_infos = new_infos;
		*page_info_capacity = cap;
	}
	assert(*page_info_capacity >= run_info->count);

	struct vy_page_info *page = run_info->page_infos + run_info->count;
	vy_page_info_create(page, data_xlog->offset, key_def, *curr_stmt);
	bool end_of_run = false;
	xlog_tx_begin(data_xlog);

	do {
		uint32_t *offset = (uint32_t *) ibuf_alloc(&row_index_buf,
							   sizeof(uint32_t));
		if (offset == NULL) {
			diag_set(OutOfMemory, sizeof(uint32_t),
				 "ibuf", "row index");
			goto error_rollback;
		}
		*offset = page->unpacked_size;

		struct vy_stmt *stmt = *curr_stmt;
		if (vy_run_dump_stmt(stmt, data_xlog, page, key_def) != 0)
			goto error_rollback;

		if (vy_write_iterator_next(wi, curr_stmt))
			goto error_rollback;

		end_of_run = *curr_stmt == NULL ||
			/* Split key reached, proceed to the next run. */
			     (split_key != NULL &&
		             vy_stmt_compare_with_key(*curr_stmt, split_key,
						      format, key_def) >= 0);

	} while (end_of_run == false &&
		 obuf_size(&data_xlog->obuf) < key_def->opts.page_size);

	/* Save offset to row index  */
	page->row_index_offset = page->unpacked_size;

	/* Write row index */
	struct xrow_header xrow;
	const uint32_t *row_index = (const uint32_t *) row_index_buf.rpos;
	assert(ibuf_used(&row_index_buf) == sizeof(uint32_t) * page->count);
	if (vy_row_index_encode(row_index, page->count, &xrow) < 0)
		goto error_rollback;

	ssize_t written = xlog_write_row(data_xlog, &xrow);
	if (written < 0)
		goto error_rollback;

	page->unpacked_size += written;

	written = xlog_tx_commit(data_xlog);
	if (written == 0)
		written = xlog_flush(data_xlog);
	if (written < 0)
		goto error_row_index;

	page->size = written;

	assert(page->count > 0);

	++run_info->count;
	if (page->min_lsn < run_info->min_lsn)
		run_info->min_lsn = page->min_lsn;
	if (page->max_lsn > run_info->max_lsn)
		run_info->max_lsn = page->max_lsn;
	run_info->total += page->size;
	run_info->keys += page->count;

	ibuf_destroy(&row_index_buf);
	return !end_of_run ? 0: 1;

error_rollback:
	xlog_tx_rollback(data_xlog);
error_row_index:
	ibuf_destroy(&row_index_buf);
	return -1;
}

/**
 * Write statements from the iterator to a new run file.
 *
 *  @retval 0, curr_stmt != NULL: all is ok, the iterator is not finished
 *  @retval 0, curr_stmt == NULL: all is ok, the iterator finished
 *  @retval -1 error occurred
 */
static int
vy_run_write_data(struct vy_run *run, const char *dirpath,
		  int64_t range_id, int run_id,
		  struct vy_write_iterator *wi, struct vy_stmt **curr_stmt,
		  const struct vy_stmt *end_key,
		  const struct key_def *key_def,
		  const struct tuple_format *format)
{
	assert(curr_stmt);
	struct vy_run_info *run_info = &run->info;

	char path[PATH_MAX];
	vy_run_snprint_path(path, sizeof(path), dirpath,
			    key_def->opts.lsn, range_id, run_id,
			    VY_FILE_RUN);
	struct xlog data_xlog;
	struct xlog_meta meta = {
		.filetype = XLOG_META_TYPE_RUN,
		.server_uuid = SERVER_UUID,
	};
	if (xlog_create(&data_xlog, path, &meta) < 0)
		return -1;

	/*
	 * Read from the iterator until it's exhausted or
	 * the split key is reached.
	 */
	run_info->min_lsn = INT64_MAX;
	assert(run_info->page_infos == NULL);
	uint32_t page_infos_capacity = 0;
	int rc;
	do {
		rc = vy_run_write_page(run_info, &data_xlog, wi,
				       end_key, &page_infos_capacity,
				       curr_stmt, key_def, format);
		if (rc < 0)
			goto err;
		fiber_gc();
	} while (rc == 0);

	/* Sync data and link the file to the final name. */
	if (xlog_sync(&data_xlog) < 0 ||
	    xlog_rename(&data_xlog) < 0)
		goto err;

	run->fd = data_xlog.fd;
	xlog_close(&data_xlog, true);
	fiber_gc();

	return 0;
err:
	xlog_close(&data_xlog, false);
	fiber_gc();
	return -1;
}

/** {{{ vy_page_info */

enum vy_request_page_key {
	VY_PAGE_REQUEST_COUNT = 1,
	VY_PAGE_MIN_KEY = 2,
	VY_PAGE_DATA_SIZE = 3,
	VY_PAGE_ROW_INDEX_OFFSET = 4
};

const char *vy_page_info_key_strs[] = {
	"count",
	"min",
	"data size",
	"row index"
};

const uint64_t vy_page_info_key_map = (1 << VY_PAGE_REQUEST_COUNT) |
				      (1 << VY_PAGE_MIN_KEY) |
				      (1 << VY_PAGE_DATA_SIZE) |
				      (1 << VY_PAGE_ROW_INDEX_OFFSET);

/**
 * Encode vy_page_info as xrow.
 * Allocates using region_alloc.
 *
 * @param page_info page information to encode
 * @param run_id run identifier
 * @param[out] xrow xrow to fill
 *
 * @retval  0 success
 * @retval -1 error, check diag
 */
static int
vy_page_info_encode(const struct vy_page_info *page_info,
		    int run_id, struct xrow_header *xrow)
{
	struct region *region = &fiber()->gc;

	struct request request;
	request_create(&request, IPROTO_REPLACE);
	request.space_id = BOX_VINYL_PAGE_ID;
	request.index_id = 0;

	uint32_t min_key_size;
	const char *min_key = vy_key_data_range(page_info->min_key,
						&min_key_size);

	/* calc tuple size */
	uint32_t size;
	/* 3 items run_id, page_id and map */
	size = mp_sizeof_array(4) +
	       mp_sizeof_uint(run_id) +
	       mp_sizeof_uint(page_info->offset) +
	       mp_sizeof_uint(page_info->size) +
	       /* page map contains 4 items */
	       mp_sizeof_map(4) +
	       mp_sizeof_uint(VY_PAGE_REQUEST_COUNT) +
	       mp_sizeof_uint(page_info->count) +
	       mp_sizeof_uint(VY_PAGE_MIN_KEY) +
	       min_key_size +
	       mp_sizeof_uint(VY_PAGE_DATA_SIZE) +
	       mp_sizeof_uint(page_info->unpacked_size) +
	       mp_sizeof_uint(VY_PAGE_ROW_INDEX_OFFSET) +
	       mp_sizeof_uint(page_info->row_index_offset);

	char *pos = region_alloc(region, size);
	if (pos == NULL) {
		diag_set(OutOfMemory, size, "region", "page encode");
		return -1;
	}
	/* encode tuple */
	request.tuple = pos;
	pos = mp_encode_array(pos, 4);
	pos = mp_encode_uint(pos, run_id);
	pos = mp_encode_uint(pos, page_info->offset);
	pos = mp_encode_uint(pos, page_info->size);
	pos = mp_encode_map(pos, 4);
	pos = mp_encode_uint(pos, VY_PAGE_REQUEST_COUNT);
	pos = mp_encode_uint(pos, page_info->count);
	pos = mp_encode_uint(pos, VY_PAGE_MIN_KEY);
	memcpy(pos, min_key, min_key_size);
	pos += min_key_size;
	pos = mp_encode_uint(pos, VY_PAGE_DATA_SIZE);
	pos = mp_encode_uint(pos, page_info->unpacked_size);
	pos = mp_encode_uint(pos, VY_PAGE_ROW_INDEX_OFFSET);
	pos = mp_encode_uint(pos, page_info->row_index_offset);
	request.tuple_end = pos;

	memset(xrow, 0, sizeof(*xrow));
	xrow->type = IPROTO_REPLACE;
	if ((xrow->bodycnt = request_encode(&request, xrow->body)) < 0)
		return -1;
	return 0;
}

/**
 * Decode page information from xrow.
 *
 * @param xrow xrow to decode
 * @param run_id the run identifier to check
 * @param[out] page page information
 * @param[out] run_info the run information
 *
 * @retval  0 success
 * @retval -1 error, check diag
 */
static int
vy_page_info_decode(struct vy_page_info *page, int run_id,
		    const struct xrow_header *xrow)
{
	struct request request;
	/* extract all pages info */
	request_create(&request, xrow->type);
	request_decode(&request, xrow->body->iov_base, xrow->body->iov_len);
	if (request.space_id != BOX_VINYL_PAGE_ID) {
		diag_set(ClientError, ER_VINYL, "Can't decode page meta: "
			 "incorrect space id");
		return -1;
	}
	const char *pos = request.tuple;
	if (mp_decode_array(&pos) < 4) {
		diag_set(ClientError, ER_VINYL, "Can't decode page meta "
			 "tuple is too small");
		return -1;
	}
	if (run_id != (int) mp_decode_uint(&pos)) {
		diag_set(ClientError, ER_VINYL, "Can't decode page meta "
			 "incorrect run id");
		return -1;
	}

	memset(page, 0, sizeof(*page));
	page->offset = mp_decode_uint(&pos);
	page->size = mp_decode_uint(&pos);

	uint64_t key_map = vy_page_info_key_map;
	uint32_t map_size = mp_decode_map(&pos);
	uint32_t map_item;
	const char *key_beg;
	for (map_item = 0; map_item < map_size; ++map_item) {
		uint32_t key = mp_decode_uint(&pos);
		key_map &= ~(1 << key);
		switch (key) {
		case VY_PAGE_REQUEST_COUNT:
			page->count = mp_decode_uint(&pos);
			break;
		case VY_PAGE_MIN_KEY:
			key_beg = pos;
			mp_next(&pos);
			uint32_t part_count;
			part_count = mp_decode_array(&key_beg);
			page->min_key = vy_stmt_new_select(key_beg, part_count);
			break;
		case VY_PAGE_DATA_SIZE:
			page->unpacked_size = mp_decode_uint(&pos);
			break;
		case VY_PAGE_ROW_INDEX_OFFSET:
			page->row_index_offset = mp_decode_uint(&pos);
			break;
		default:
			diag_set(ClientError, ER_VINYL, "Can't decode page meta "
				 "unknown page meta key %d", key);
			return -1;
		}
	}
	if (key_map) {
		diag_set(ClientError, ER_MISSING_REQUEST_FIELD,
			 vy_page_info_key_strs[__builtin_ffsll(key_map) - 1]);
		return -1;
	}

	return 0;
}

/** vy_page_info }}} */

/** {{{ vy_run_info */

/**
 * Keys for msgpuck run encoding
 */
enum vy_request_run_key {
	VY_RUN_MIN_LSN = 1,
	VY_RUN_MAX_LSN = 2,
	VY_RUN_PAGE_COUNT = 3,
	VY_RANGE_MIN_KEY = 4,
	VY_RANGE_MAX_KEY = 5
};

const char *vy_run_info_key_strs[] = {
	"min lsn",
	"max lsn",
	"page count",
	"range min key",
	"range max key"
};

const uint64_t vy_run_info_key_map = (1 << VY_RUN_MIN_LSN) |
				     (1 << VY_RUN_MAX_LSN) |
				     (1 << VY_RUN_PAGE_COUNT);

/**
 * Encode vy_run_info as xrow
 * Allocates using region alloc
 *
 * @param run_info the run information
 * @param run_id the run identifier
 * @param begin left bound of the run
 * @param end right bound of the run
 * @param xrow xrow to fill.
 *
 * @retval  0 success
 * @retval -1 on error, check diag
 */
static int
vy_run_info_encode(const struct vy_run_info *run_info, int run_id,
		   const struct vy_stmt *begin,
		   const struct vy_stmt *end,
		   struct xrow_header *xrow)
{
	/* size for run tuple of 2 items: id + map */
	size_t size = mp_sizeof_array(2) + mp_sizeof_uint(run_id);
	/*
	 * run map size: min lsn, max lsn, page count, begin and end keys if defined
	 */
	size_t map_size = 3;
	size += mp_sizeof_uint(VY_RUN_MIN_LSN) +
		mp_sizeof_uint(run_info->min_lsn);
	size += mp_sizeof_uint(VY_RUN_MAX_LSN) +
		mp_sizeof_uint(run_info->max_lsn);
	size += mp_sizeof_uint(VY_RUN_PAGE_COUNT) +
		mp_sizeof_uint(run_info->count);
	if (begin != NULL) {
		/* range begin is set */
		++map_size;
		uint32_t bsize;
		vy_key_data_range(begin, &bsize);
		size += mp_sizeof_uint(VY_RANGE_MIN_KEY) + bsize;
	}
	if (end != NULL) {
		/* range end is set */
		++map_size;
		uint32_t bsize;
		vy_key_data_range(end, &bsize);
		size += mp_sizeof_uint(VY_RANGE_MAX_KEY) + bsize;
	}
	size += mp_sizeof_map(map_size);

	char *tuple = region_alloc(&fiber()->gc, size);
	if (tuple == NULL) {
		diag_set(OutOfMemory, size, "region", "run encode");
		return -1;
	}
	char *pos = tuple;
	/* encode values */
	pos = mp_encode_array(pos, 2);
	pos = mp_encode_uint(pos, run_id);
	pos = mp_encode_map(pos, map_size);
	pos = mp_encode_uint(pos, VY_RUN_MIN_LSN);
	pos = mp_encode_uint(pos, run_info->min_lsn);
	pos = mp_encode_uint(pos, VY_RUN_MAX_LSN);
	pos = mp_encode_uint(pos, run_info->max_lsn);
	pos = mp_encode_uint(pos, VY_RUN_PAGE_COUNT);
	pos = mp_encode_uint(pos, run_info->count);
	if (begin != NULL) {
		pos = mp_encode_uint(pos, VY_RANGE_MIN_KEY);
		uint32_t bsize;
		const char *data = vy_key_data_range(begin, &bsize);
		memcpy(pos, data, bsize);
		pos += bsize;
	}
	if (end != NULL) {
		pos = mp_encode_uint(pos, VY_RANGE_MAX_KEY);
		uint32_t bsize;
		const char *data = vy_key_data_range(end, &bsize);
		memcpy(pos, data, bsize);
		pos += bsize;
	}

	/* put tuple in a replace request to run's space */
	struct request request;
	request_create(&request, IPROTO_REPLACE);
	request.space_id = BOX_VINYL_RUN_ID;;
	request.index_id = 0;
	request.tuple = tuple;
	request.tuple_end = pos;
	memset(xrow, 0, sizeof(*xrow));
	xrow->type = IPROTO_REPLACE;
	xrow->lsn = run_info->min_lsn;
	if ((xrow->bodycnt = request_encode(&request, xrow->body)) < 0)
		return -1;

	return 0;
}

/**
 * Decode the run metadata from xrow.
 *
 * @param xrow xrow to decode
 * @param key_def key definition
 * @param[out] run_info the run information
 * @param[out] p_run_id the run identifier
 * @param[out] p_begin the begin of the run
 * @param[out] p_end the end of the run
 *
 * @retval  0 success
 * @retval -1 error (check diag)
 */
static int
vy_run_info_decode(const struct xrow_header *xrow,
		   const struct key_def *key_def,
		   struct vy_run_info *run_info, int *p_run_id,
		   struct vy_stmt **p_begin, struct vy_stmt **p_end)
{
	struct vy_stmt *begin = NULL;
	struct vy_stmt *end = NULL;

	struct request request;
	request_create(&request, xrow->type);
	if (request_decode(&request, xrow->body->iov_base,
			   xrow->body->iov_len)) {
		return -1;
	}

	/* decode run */
	if (request.space_id != BOX_VINYL_RUN_ID) {
		diag_set(ClientError, ER_VINYL, "Can't decode run meta: "
			 "incorrect space id");
		return -1;
	}
	const char *pos = request.tuple;
	if (mp_decode_array(&pos) < 2) {
		diag_set(ClientError, ER_VINYL, "Can't decode run meta: "
			 "not enough values");
		return -1;
	}
	int run_id = mp_decode_uint(&pos);
	memset(run_info, 0, sizeof(*run_info));
	uint64_t key_map = vy_run_info_key_map;
	uint32_t map_size = mp_decode_map(&pos);
	uint32_t map_item;
	/* decode run values */
	for (map_item = 0; map_item < map_size; ++map_item) {
		uint32_t key = mp_decode_uint(&pos);
		key_map &= ~(1 << key);
		switch (key) {
		case VY_RUN_MIN_LSN:
			run_info->min_lsn = mp_decode_uint(&pos);
			break;
		case VY_RUN_MAX_LSN:
			run_info->max_lsn = mp_decode_uint(&pos);
			break;
		case VY_RUN_PAGE_COUNT:
			run_info->count = mp_decode_uint(&pos);
			break;
		case VY_RANGE_MIN_KEY:
			if (begin != NULL) {
				/* begin already set, skip */
				mp_next(&pos);
				break;
			}
			begin = vy_key_from_msgpack(pos, key_def);
			mp_next(&pos);
			break;
		case VY_RANGE_MAX_KEY:
			if (end != NULL) {
				/* end already set, skip it */
				mp_next(&pos);
				break;
			}
			end = vy_key_from_msgpack(pos, key_def);
			mp_next(&pos);
			break;
		default:
			diag_set(ClientError, ER_VINYL,
				 "Unknown run meta key %d", key);
			goto fail;
		}
	}
	if (key_map) {
		diag_set(ClientError, ER_MISSING_REQUEST_FIELD,
			 vy_run_info_key_strs[__builtin_ffsll(key_map) - 1]);
		goto fail;
	}

	*p_begin = begin;
	*p_end = end;
	*p_run_id = run_id;
	return 0;

fail:
	if (begin != NULL)
		vy_stmt_unref(begin);
	if (end != NULL)
		vy_stmt_unref(end);
	return -1;
}

/* vy_run_info }}} */

/**
 * Write run to file.
 */
static int
vy_run_write_index(struct vy_run *run, const char *dirpath,
		   int64_t range_id, int run_id,
		   const struct vy_stmt *begin,
		   const struct vy_stmt *end,
		   const struct key_def *key_def)
{
	char path[PATH_MAX];
	vy_run_snprint_path(path, sizeof(path), dirpath,
			    key_def->opts.lsn, range_id, run_id,
			    VY_FILE_INDEX);

	struct xlog index_xlog;
	struct xlog_meta meta = {
		.filetype = XLOG_META_TYPE_INDEX,
		.server_uuid = SERVER_UUID,
	};
	if (xlog_create(&index_xlog, path, &meta) < 0)
		return -1;

	xlog_tx_begin(&index_xlog);

	struct xrow_header xrow;
	if (vy_run_info_encode(&run->info, run_id, begin, end,
			       &xrow) != 0 ||
	    xlog_write_row(&index_xlog, &xrow) < 0)
		goto fail;

	for (uint32_t page_no = 0; page_no < run->info.count; ++page_no) {
		struct vy_page_info *page_info = vy_run_page_info(run, page_no);
		struct xrow_header xrow;
		if (vy_page_info_encode(page_info, run_id, &xrow) < 0) {
			goto fail;
		}
		if (xlog_write_row(&index_xlog, &xrow) < 0)
			goto fail;
	}

	if (xlog_tx_commit(&index_xlog) < 0 ||
	    xlog_flush(&index_xlog) < 0 ||
	    xlog_rename(&index_xlog) < 0)
		goto fail;
	xlog_close(&index_xlog, false);
	fiber_gc();
	return 0;
fail:
	fiber_gc();
	xlog_tx_rollback(&index_xlog);
	xlog_close(&index_xlog, false);
	unlink(path);
	return -1;
}

/**
 * Allocate and initialize a range (either a new one or for
 * restore from disk).
 */
static struct vy_range *
vy_range_new(struct vy_index *index, int64_t id,
	     struct vy_stmt *begin, struct vy_stmt *end)
{
	struct vy_range *range = (struct vy_range*) calloc(1, sizeof(*range));
	if (range == NULL) {
		diag_set(OutOfMemory, sizeof(struct vy_range), "malloc",
			 "struct vy_range");
		return NULL;
	}
	if (id != 0) {
		range->id = id;
		/** Recovering an existing range from disk. Update
		 * range_id_max to not create a new range wit the
		 * same id.
		 */
		index->range_id_max = MAX(index->range_id_max, id);
	} else {
		/**
		 * Creating a new range. Assign a new id.
	         */
		range->id = ++index->range_id_max;
	}
	if (begin != NULL) {
		vy_stmt_ref(begin);
		range->begin = begin;
	}
	if (end != NULL) {
		vy_stmt_ref(end);
		range->end = end;
	}
	rlist_create(&range->runs);
	rlist_create(&range->frozen);
	range->min_lsn = INT64_MAX;
	range->index = index;
	range->in_dump.pos = UINT32_MAX;
	range->in_compact.pos = UINT32_MAX;
	rlist_create(&range->compact_list);
	return range;
}

static int
vy_range_recover_run(struct vy_range *range, int run_id)
{
	const struct vy_index *index = range->index;
	const struct key_def *key_def = index->key_def;

	struct vy_run *run = vy_run_new();
	if (run == NULL)
		return -1;

	char path[PATH_MAX];
	vy_run_snprint_path(path, sizeof(path), index->path,
			    key_def->opts.lsn, range->id, run_id,
			    VY_FILE_INDEX);
	struct xlog_cursor cursor;
	if (xlog_cursor_open(&cursor, path))
		goto fail;

	struct xlog_meta *meta = &cursor.meta;
	if (strcmp(meta->filetype, XLOG_META_TYPE_INDEX) != 0) {
		diag_set(ClientError, ER_INVALID_XLOG_TYPE,
			 XLOG_META_TYPE_INDEX, meta->filetype);
		goto fail_close;
	}

	/* Read run header. */
	struct xrow_header xrow;
	int run_id_check = 0;
	/* all rows should be in one tx */
	if (xlog_cursor_next_tx(&cursor) != 0 ||
	    xlog_cursor_next_row(&cursor, &xrow) != 0 ||
	    vy_run_info_decode(&xrow, key_def, &run->info, &run_id_check,
			       &range->begin, &range->end) != 0) {
		goto fail_close;
	}
	if (run_id_check != run_id) {
		diag_set(ClientError, ER_INVALID_RUN_ID,
			 (long long) run_id_check,
			 (long long) run_id);
		goto fail_close;
	}

	/* Allocate buffer for page info. */
	run->info.page_infos = calloc(run->info.count, sizeof(struct vy_page_info));
	if (run->info.page_infos == NULL) {
		diag_set(OutOfMemory, run->info.count * sizeof(struct vy_page_info),
			 "malloc", "struct vy_page_info");
		goto fail_close;
	}

	int rc;
	uint32_t page_no = 0;
	while ((rc = xlog_cursor_next_row(&cursor, &xrow)) == 0) {
		if (page_no >= run->info.count) {
			/** To many pages in file */
			diag_set(ClientError, ER_VINYL, "To many pages in run meta file");
			goto fail_close;
		}
		struct vy_page_info *page = run->info.page_infos + page_no;
		if (vy_page_info_decode(page, run_id, &xrow) < 0) {
			/** Limit count of pages to successfully created pages */
			run->info.count = page_no;
			goto fail_close;
		}
		++page_no;
	}

	/* We don't need to keep metadata file open any longer. */
	xlog_cursor_close(&cursor, false);

	/* Prepare data file for reading. */
	vy_run_snprint_path(path, sizeof(path), index->path,
			    key_def->opts.lsn, range->id, run_id,
			    VY_FILE_RUN);
	if (xlog_cursor_open(&cursor, path))
		goto fail;
	meta = &cursor.meta;
	if (strcmp(meta->filetype, XLOG_META_TYPE_RUN) != 0) {
		diag_set(ClientError, ER_INVALID_XLOG_TYPE,
			 XLOG_META_TYPE_RUN, meta->filetype);
		goto fail_close;
	}
	run->fd = cursor.fd;
	xlog_cursor_close(&cursor, true);

	/* Finally, link run to the range. */
	rlist_add_entry(&range->runs, run, in_range);
	range->run_count++;
	return 0;

fail_close:
	xlog_cursor_close(&cursor, false);
fail:
	vy_run_unref(run);
	return -1;
}

/* Move the active in-memory index of a range to the frozen list. */
static void
vy_range_freeze_mem(struct vy_range *range)
{
	if (range->mem != NULL) {
		rlist_add_entry(&range->frozen, range->mem, in_frozen);
		range->mem = NULL;
	}
}

static void
vy_range_delete_mem(struct vy_range *range, struct vy_mem *mem)
{
	struct vy_index *index = range->index;
	struct vy_env *env = index->env;

	vy_scheduler_mem_dumped(env->scheduler, mem);
	index->used -= mem->used;
	index->stmt_count -= mem->tree.size;
	vy_mem_delete(mem);
}

static void
vy_range_delete(struct vy_range *range)
{
	/* The range has been deleted from the scheduler queues. */
	assert(range->in_dump.pos == UINT32_MAX);
	assert(range->in_compact.pos == UINT32_MAX);

	if (range->begin)
		vy_stmt_unref(range->begin);
	if (range->end)
		vy_stmt_unref(range->end);

	if (range->new_run != NULL)
		vy_run_delete(range->new_run);

	/* Delete all runs */
	while (!rlist_empty(&range->runs)) {
		struct vy_run *run = rlist_shift_entry(&range->runs,
						       struct vy_run, in_range);
		vy_run_unref(run);
	}
	/* Release all mems */
	if (range->mem != NULL)
		vy_range_delete_mem(range, range->mem);
	while (!rlist_empty(&range->frozen)) {
		struct vy_mem *mem;
		mem = rlist_shift_entry(&range->frozen,
					struct vy_mem, in_frozen);
		vy_range_delete_mem(range, mem);
	}

	TRASH(range);
	free(range);
}

/*
 * Create a new run for a range and write statements returned by a write
 * iterator to the run file until the end of the range is encountered.
 */
static int
vy_range_write_run(struct vy_range *range, struct vy_write_iterator *wi,
		   struct vy_stmt **stmt, size_t *written)
{
	assert(stmt);
	const struct vy_index *index = range->index;
	const struct key_def *key_def = index->key_def;
	const struct tuple_format *format = index->format;

	struct vy_run *run = range->new_run;
	assert(run != NULL);

	ERROR_INJECT(ERRINJ_VY_RANGE_DUMP,
		     {diag_set(ClientError, ER_INJECTION,
			       "vinyl range dump"); return -1;});

	int run_id = range->run_count;
	if (vy_run_write_data(run, index->path, range->id, run_id,
			      wi, stmt, range->end,
			      key_def, format) != 0 ||
	    vy_run_write_index(run, index->path, range->id, run_id,
		               range->begin, range->end, key_def) != 0) {
		return -1;
	}

	*written += vy_run_size(run) + vy_run_total(run);
	return 0;
}

/**
 * Return true and set split_key accordingly if the range needs to be
 * split in two.
 *
 * - We should never split a range until it was merged at least once
 *   (actually, it should be a function of compact_wm/number of runs
 *   used for the merge: with low compact_wm it's more than once, with
 *   high compact_wm it's once).
 * - We should use the last run size as the size of the range.
 * - We should split around the last run middle key.
 * - We should only split if the last run size is greater than
 *   4/3 * range_size.
 */
static bool
vy_range_needs_split(struct vy_range *range, const char **p_split_key)
{
	struct key_def *key_def = range->index->key_def;
	struct vy_run *run = NULL;

	/* The range hasn't been merged yet - too early to split it. */
	if (range->n_compactions < 1)
		return false;

	/* Find the oldest run. */
	run = rlist_last_entry(&range->runs, struct vy_run, in_range);
	assert(run != NULL);

	/* The range is too small to be split. */
	if (run->info.total < key_def->opts.range_size * 4 / 3)
		return false;

	/* Find the median key in the oldest run (approximately). */
	struct vy_page_info *mid_page;
	mid_page = vy_run_page_info(run, run->info.count / 2);
	const char *split_key = vy_stmt_data(mid_page->min_key);

	struct vy_page_info *first_page = vy_run_page_info(run, 0);
	const char *min_key = vy_stmt_data(first_page->min_key);

	/* No point in splitting if a new range is going to be empty. */
	if (vy_key_compare_raw(min_key, split_key, key_def) == 0)
		return false;

	*p_split_key = split_key;
	return true;
}

static void
vy_range_add_compact_part(struct vy_range *range, struct vy_range *part)
{
	struct vy_index *index = range->index;

	if (rlist_empty(&range->compact_list)) {
		/*
		 * Make sure no new statement is inserted into the
		 * active mem after we start compacting the range.
		 * Needed by the write iterator, which requires its
		 * sources to be immutable.
		 */
		vy_range_freeze_mem(range);
		vy_index_remove_range(index, range);
	}

	/*
	 * While compaction is in progress, new statements are inserted
	 * to new ranges while read iterator walks over the old range
	 * (see vy_range_iterator_next()). To make new statements
	 * visible, link new ranges to the old range via ->compact_list.
	 */
	rlist_add_tail(&range->compact_list, &part->compact_list);
	assert(part->shadow == NULL);
	part->shadow = range;

	vy_index_add_range(index, part);
	say_debug("range new: %s", vy_range_str(part));
}

static void
vy_range_commit_compact_parts(struct vy_range *range)
{
	struct vy_range *r, *tmp;

	/*
	 * If compaction completed successfully, all runs and mems of
	 * the original range were dumped and hence we don't need it any
	 * longer. So unlink new ranges from the original one and delete
	 * the latter.
	 */
	rlist_foreach_entry_safe(r, &range->compact_list, compact_list, tmp) {
		rlist_del(&r->compact_list);
		assert(r->shadow == range);
		r->shadow = NULL;
	}
	vy_range_delete(range);
}

static void
vy_range_discard_compact_parts(struct vy_range *range)
{
	struct vy_index *index = range->index;
	struct vy_range *r, *tmp;

	/*
	 * On compaction failure we delete new ranges, but leave their
	 * mems and runs linked to the old range so that statements
	 * inserted during compaction don't get lost.
	 */
	rlist_foreach_entry_safe(r, &range->compact_list, compact_list, tmp) {
		if (range->used == 0)
			range->min_lsn = r->min_lsn;
		assert(range->min_lsn <= r->min_lsn);
		range->used += r->used;

		vy_range_freeze_mem(r);
		rlist_splice(&range->frozen, &r->frozen);

		rlist_splice(&range->runs, &r->runs);
		range->run_count += r->run_count;
		r->run_count = 0;

		rlist_del(&r->compact_list);
		assert(r->shadow == range);
		r->shadow = NULL;

		say_debug("range delete: %s", vy_range_str(r));
		vy_index_remove_range(index, r);
		vy_range_delete(r);
	}
	/* Unfreeze the newest in-memory index. */
	assert(range->mem == NULL);
	assert(!rlist_empty(&range->frozen));
	range->mem = rlist_shift_entry(&range->frozen,
				       struct vy_mem, in_frozen);
	/* Insert the range back into the tree. */
	vy_index_add_range(index, range);
}

/**
 * Create an index directory for a new index.
 * TODO: create index files only after the WAL
 * record is committed.
 */
static int
vy_index_create(struct vy_index *index)
{
	/* create directory */
	int rc;
	char *path_sep = index->path;
	while (*path_sep == '/') {
		/* Don't create root */
		++path_sep;
	}
	while ((path_sep = strchr(path_sep, '/'))) {
		/* Recursively create path hierarchy */
		*path_sep = '\0';
		rc = mkdir(index->path, 0777);
		if (rc == -1 && errno != EEXIST) {
			diag_set(SystemError, "failed to create directory '%s'",
		                 index->path);
			*path_sep = '/';
			return -1;
		}
		*path_sep = '/';
		++path_sep;
	}
	rc = mkdir(index->path, 0777);
	if (rc == -1 && errno != EEXIST) {
		diag_set(SystemError, "failed to create directory '%s'",
			 index->path);
		return -1;
	}

	index->range_id_max = 0;
	/* create initial range */
	struct vy_range *range = vy_range_new(index, 0, NULL, NULL);
	if (unlikely(range == NULL))
		return -1;
	vy_index_add_range(index, range);
	vy_index_acct_range(index, range);
	vy_scheduler_add_range(index->env->scheduler, range);
	/* create initial mem */
	range->mem = vy_mem_new(index->env, index->key_def, index->format);
	if (range->mem == NULL)
		return -1;
	return 0;
}

/*
 * This structure is only needed for sorting runs for recovery.
 * Runs with higher range id go first. Runs that belong to the
 * same range are sorted by serial number in ascending order.
 * This way, we recover newer images of the same range first,
 * while within the same range runs are restored in the order
 * they were dumped.
 */
struct vy_run_desc {
	int64_t range_id;
	int run_id;
};

static int
vy_run_desc_cmp(const void *p1, const void *p2)
{
	const struct vy_run_desc *d1 = p1;
	const struct vy_run_desc *d2 = p2;
	if (d1->range_id > d2->range_id)
		return -1;
	if (d1->range_id < d2->range_id)
		return 1;
	if (d1->run_id > d2->run_id)
		return 1;
	if (d1->run_id < d2->run_id)
		return -1;
	return 0;
}

/*
 * Return list of all run files found in the index directory.
 * A run file is described by range id and run serial number.
 */
static int
vy_index_recover_run_list(struct vy_index *index,
			  struct vy_run_desc **desc, int *count)
{
	DIR *dir = opendir(index->path);
	if (dir == NULL) {
		diag_set(SystemError, "failed to open directory '%s'",
			 index->path);
		return -1;
	}

	*desc = NULL;
	*count = 0;
	int cap = 0;

	while (true) {
		errno = 0;
		struct dirent *dirent = readdir(dir);
		if (dirent == NULL) {
			if (errno == 0)
				break; /* eof */
			diag_set(SystemError, "error reading directory '%s'",
				 index->path);
			goto fail;
		}

		int64_t index_lsn;
		struct vy_run_desc v;
		enum vy_file_type t;

		if (vy_run_parse_name(dirent->d_name, &index_lsn,
				      &v.range_id, &v.run_id, &t) != 0)
			continue; /* unknown file */
		if (index_lsn != index->key_def->opts.lsn)
			continue; /* different incarnation */
		if (t != VY_FILE_INDEX)
			continue; /* the run file */

		if (*count == cap) {
			cap = cap > 0 ? cap * 2 : 16;
			void *p = realloc(*desc, cap * sizeof(v));
			if (p == NULL) {
				diag_set(OutOfMemory, cap * sizeof(v),
					 "realloc", "struct vy_run_desc");
				goto fail;
			}
			*desc = p;
		}
		(*desc)[(*count)++] = v;
	}
	closedir(dir);
	return 0;
fail:
	closedir(dir);
	free(*desc);
	return -1;
}
 
/**
 * A quick intro into Vinyl cosmology and file format
 * --------------------------------------------------
 * A single vinyl index on disk consists of a set of "range"
 * objects. A range contains a sorted set of index keys;
 * keys in different ranges do not overlap, for example:
 * [0..100],[103..252],[304..360]
 *
 * The sorted set of keys in a range is called a run. A single
 * range may contain multiple runs, each run contains changes of
 * keys in the range over a certain period of time. The periods do
 * not overlap, while, of course, two runs of the same range may
 * contain changes of the same key.
 * All keys in a run are sorted and split between pages of
 * approximately equal size. The purpose of putting keys into
 * pages is a quicker key lookup, since (min,max) key of every
 * page is put into the page index, stored at the beginning of each
 * run. The page index of an active run is fully cached in RAM.
 *
 * All files of an index have the following name pattern:
 * <lsn>.<range_id>.<run_id>.{run,index}
 * and are stored together in the index directory.
 *
 * Files that end with '.run' store metadata (see vy_run_info)
 * while '.data' files store vinyl statements.
 *
 * The <lsn> component represents LSN of index creation: it is used
 * to distinguish between different "incarnations" of the same index,
 * e.g. on create/drop events. In a most common case LSN is the
 * same for all files in an index.
 *
 * <range_id> component represents the id of the range in an
 * index. The id is a monotonically growing integer, and is
 * assigned to a range when it's created. Thus newer ranges will
 * have greater ids, and hence by recovering ranges with greater
 * ids first and ignoring ranges which are already fully spanned,
 * we can restore the whole index to its latest state.
 *
 * <run_id> is the serial number of the run in the range,
 * starting from 0.
 */
static int
vy_index_open_ex(struct vy_index *index)
{
	struct vy_run_desc *desc;
	int count;
	int rc = -1;

	if (vy_index_recover_run_list(index, &desc, &count) != 0)
		return -1;

	/*
	 * Always prefer newer ranges (i.e. those that have greater ids)
	 * over older ones. Only fall back on an older range, if it has
	 * not been spanned by the time we get to it. The latter can
	 * only happen if there was an incomplete range split. Within
	 * the same range, start recovery from the oldest run in order
	 * to restore the original order of vy_range->runs list.
	 */
	qsort(desc, count, sizeof(*desc), vy_run_desc_cmp);

	struct vy_range *range = NULL;
	for (int i = 0; i < count; i++) {
		if (range == NULL || range->id != desc[i].range_id) {
			/* Proceed to the next range. */
			if (range != NULL)
				vy_index_recover_range(index, range);
			range = vy_range_new(index, desc[i].range_id,
					     NULL, NULL);
			if (range == NULL)
				goto out;
		}
		if (desc[i].run_id != range->run_count) {
			diag_set(ClientError, ER_VINYL, "run file missing");
			return -1;
		}
		if (vy_range_recover_run(range, desc[i].run_id) != 0)
			goto out;
	}
	if (range != NULL)
		vy_index_recover_range(index, range);

	if (index->range_count == 0) {
		/*
		 * Special case: index hasn't been dumped yet.
		 * Create a range for it.
		 */
		range = vy_range_new(index, 0, NULL, NULL);
		if (range == NULL)
			goto out;
		vy_index_add_range(index, range);
	}

	/*
	 * Update index size and make ranges visible to the scheduler.
	 * Also, make sure that the index does not have holes, i.e.
	 * all data were recovered.
	 */
	struct vy_range *prev = NULL;
	for (range = vy_range_tree_first(&index->tree); range != NULL;
	     prev = range, range = vy_range_tree_next(&index->tree, range)) {
		if ((prev == NULL && range->begin != NULL) ||
		    (prev != NULL && !vy_range_is_adjacent(prev, range,
							   index->key_def)))
			break;
		vy_index_acct_range(index, range);
		vy_scheduler_add_range(index->env->scheduler, range);
		range->mem = vy_mem_new(index->env, index->key_def,
					index->format);
		if (range->mem == NULL)
			goto out;
	}
	if (range != NULL || prev->end != NULL) {
		diag_set(ClientError, ER_VINYL, "range missing");
		goto out;
	}

	rc = 0; /* success */
out:
	free(desc);
	return rc;
}

/*
 * Save a statement in the range's in-memory index.
 */
static int
vy_range_set(struct vy_range *range, const struct vy_stmt *stmt,
	     int64_t alloc_id)
{
	struct vy_index *index = range->index;
	struct vy_scheduler *scheduler = index->env->scheduler;

	const struct vy_stmt *replaced_stmt = NULL;
	struct vy_mem *mem = range->mem;
	size_t size = vy_stmt_size(stmt);
	struct vy_stmt *mem_stmt;
	mem_stmt = lsregion_alloc(&index->env->allocator, size, alloc_id);
	if (mem_stmt == NULL) {
		diag_set(OutOfMemory, size, "lsregion_alloc", "mem_stmt");
		return -1;
	}
	memcpy(mem_stmt, stmt, size);
	/*
	 * Region allocated statements can't be referenced or unreferenced
	 * because they are located in monolithic memory region. Referencing has
	 * sense only for separately allocated memory blocks.
	 * The reference count here is set to 0 for an assertion if somebody
	 * will try to unreference this statement.
	 */
	mem_stmt->refs = 0;

	int rc = vy_mem_tree_insert(&mem->tree, mem_stmt, &replaced_stmt);
	if (rc != 0)
		return -1;

	if (mem->used == 0) {
		mem->min_lsn = mem_stmt->lsn;
		vy_scheduler_mem_dirtied(scheduler, mem);
	}
	if (range->used == 0) {
		range->min_lsn = mem_stmt->lsn;
		vy_scheduler_update_range(scheduler, range);
	}

	assert(mem->min_lsn <= mem_stmt->lsn);
	assert(range->min_lsn <= mem_stmt->lsn);

	mem->used += size;
	range->used += size;
	index->used += size;
	index->stmt_count++;

	mem->version++;

	return 0;
}

static int
vy_range_set_delete(struct vy_range *range, const struct vy_stmt *stmt)
{
	assert(stmt->type == IPROTO_DELETE);

	struct vy_mem *mem = range->mem;
	if (range->shadow == NULL &&
	    rlist_empty(&range->frozen) && range->run_count == 0 &&
	    vy_mem_older_lsn(mem, stmt, range->index->key_def) == NULL) {
		/*
		 * Optimization: the active mem index doesn't have statements
		 * for the key and there are no more mems and runs.
		 *  => discard DELETE statement.
		 */
		return 0;
	}

	return vy_range_set(range, stmt, stmt->lsn);
}

static void
vy_index_squash_upserts(struct vy_index *index, struct vy_stmt *stmt);

static int
vy_range_set_upsert(struct vy_range *range, struct vy_stmt *stmt)
{
	assert(stmt->type == IPROTO_UPSERT);

	struct vy_index *index = range->index;
	struct key_def *key_def = index->key_def;
	struct vy_mem *mem = range->mem;
	const struct vy_stmt *older;
	older = vy_mem_older_lsn(mem, stmt, key_def);
	if ((older != NULL && older->type != IPROTO_UPSERT) ||
	    (older == NULL && range->shadow == NULL &&
	     rlist_empty(&range->frozen) && range->run_count == 0)) {
		/*
		 * Optimization:
		 *
		 *  1. An older non-UPSERT statement for the key has been
		 *     found in the active memory index.
		 *  2. Active memory index doesn't have statements for the
		 *     key, but there are no more mems and runs.
		 *
		 *  => apply UPSERT to the older statement and save
		 *     resulted REPLACE instead of original UPSERT.
		 *
		 */
		assert(older == NULL || older->type != IPROTO_UPSERT);
		struct vy_stmt *upserted =
			vy_apply_upsert(stmt, older, key_def, index->format,
					false);
		if (upserted == NULL)
			return -1; /* OOM */
		if (upserted->lsn != stmt->lsn) {
			/**
			 * This could only happen if the upsert completely
			 * failed and the old tuple was returned.
			 * In this case we shouldn't insert the same replace
			 * again.
			 */
			assert(upserted->lsn == older->lsn);
			vy_stmt_unref(upserted);
			return 0;
		}
		assert(older == NULL || upserted->lsn != older->lsn);
		assert(upserted->type == IPROTO_REPLACE);
		int rc = vy_range_set(range, upserted, upserted->lsn);
		vy_stmt_unref(upserted);
		return rc;
	}

	/*
	 * If there are a lot of successive upserts for the same key,
	 * select might take too long to squash them all. So once the
	 * number of upserts exceeds a certain threshold, we schedule
	 * a fiber to merge them and insert the resulting statement
	 * after the latest upsert.
	 */
	enum {
		VY_UPSERT_THRESHOLD = 128,
		VY_UPSERT_INF = 255,
	};
	if (older != NULL)
		stmt->n_upserts = older->n_upserts;
	if (stmt->n_upserts != VY_UPSERT_INF) {
		stmt->n_upserts++;
		if (stmt->n_upserts > VY_UPSERT_THRESHOLD) {
			vy_index_squash_upserts(index, stmt);
			/*
			 * Prevent further upserts from starting new
			 * workers while this one is in progress.
			 */
			stmt->n_upserts = VY_UPSERT_INF;
		}
	}

	if (vy_range_set(range, stmt, stmt->lsn) != 0)
		return -1;

	return 0;
}

/*
 * Check if a statement was dumped to disk before the last shutdown and
 * therefore can be skipped on WAL replay.
 *
 * Since the minimal unit that can be dumped to disk is a range, a
 * statement is on disk iff its LSN is less than or equal to the max LSN
 * over all statements written to disk in the same range.
 */
static bool
vy_stmt_is_committed(struct vy_index *index, const struct vy_stmt *stmt)
{
	struct vy_range *range;

	range = vy_range_tree_find_by_key(&index->tree, ITER_EQ,
					  index->format, index->key_def, stmt);
	if (rlist_empty(&range->runs))
		return false;

	/*
	 * The newest run, i.e. the run containing a statement with max
	 * LSN, is at the head of the list.
	 */
	struct vy_run *run = rlist_first_entry(&range->runs,
					       struct vy_run, in_range);
	return stmt->lsn <= run->info.max_lsn;
}

/*
 * Commit a single write operation made by a transaction.
 */
static int
vy_tx_write(struct txv *v, enum vinyl_status status, int64_t lsn)
{
	struct vy_index *index = v->index;
	struct vy_stmt *stmt = v->stmt;
	struct vy_range *range = NULL;

	stmt->lsn = lsn;

	/*
	 * If we're recovering the WAL, it may happen so that this
	 * particular run was dumped after the checkpoint, and we're
	 * replaying records already present in the database. In this
	 * case avoid overwriting a newer version with an older one.
	 */
	if (status == VINYL_FINAL_RECOVERY_LOCAL ||
	    status == VINYL_FINAL_RECOVERY_REMOTE) {
		if (vy_stmt_is_committed(index, stmt))
			return 0;
	}
	/* Match range. */
	range = vy_range_tree_find_by_key(&index->tree, ITER_EQ,
					  index->format, index->key_def, stmt);
	int rc;
	switch (stmt->type) {
	case IPROTO_UPSERT:
		rc = vy_range_set_upsert(range, stmt);
		break;
	case IPROTO_DELETE:
		rc = vy_range_set_delete(range, stmt);
		break;
	default:
		rc = vy_range_set(range, stmt, stmt->lsn);
		break;
	}
	return rc;
}

/* {{{ Scheduler Task */

struct vy_task_ops {
	/**
	 * This function is called from a worker. It is supposed to do work
	 * which is too heavy for the tx thread (like IO or compression).
	 * Returns 0 on success. On failure returns -1 and sets diag.
	 */
	int (*execute)(struct vy_task *task);
	/**
	 * This function is called by the scheduler upon task completion.
	 * It may be used to finish the task from the tx thread context.
	 *
	 * If @in_shutdown is set, the callback is invoked from the
	 * engine destructor.
	 *
	 * Returns 0 on success. On failure returns -1 and sets diag.
	 */
	int (*complete)(struct vy_task *task, bool in_shutdown);
	/**
	 * This function is called by the scheduler if either ->execute
	 * or ->complete failed. It may be used to undo changes done to
	 * the index when preparing the task.
	 *
	 * If @in_shutdown is set, the callback is invoked from the
	 * engine destructor.
	 */
	void (*abort)(struct vy_task *task, bool in_shutdown);
};

struct vy_task {
	const struct vy_task_ops *ops;
	/** Return code of ->execute. */
	int status;
	/** If ->execute fails, the error is stored here. */
	struct diag diag;
	/** Index this task is for. */
	struct vy_index *index;
	/** How long ->execute took, in nanoseconds. */
	ev_tstamp exec_time;
	/** Number of bytes written to disk by this task. */
	size_t dump_size;
	/** Range to dump or compact. */
	struct vy_range *range;
	/** Write iterator producing statements for the new run. */
	struct vy_write_iterator *wi;
	/**
	 * A link in the list of all pending tasks, generated by
	 * task scheduler.
	 */
	struct stailq_entry link;
};

static struct vy_task *
vy_task_new(struct mempool *pool, struct vy_index *index,
	    const struct vy_task_ops *ops)
{
	struct vy_task *task = mempool_alloc(pool);
	if (task == NULL) {
		diag_set(OutOfMemory, sizeof(*task), "scheduler", "task");
		return NULL;
	}
	memset(task, 0, sizeof(*task));
	task->ops = ops;
	task->index = index;
	vy_index_ref(index);
	diag_create(&task->diag);
	return task;
}

static void
vy_task_delete(struct mempool *pool, struct vy_task *task)
{
	vy_index_unref(task->index);
	diag_destroy(&task->diag);
	TRASH(task);
	mempool_free(pool, task);
}

static int
vy_task_dump_execute(struct vy_task *task)
{
	struct vy_range *range = task->range;
	struct vy_write_iterator *wi = task->wi;
	struct vy_stmt *stmt;

	/* The range has been deleted from the scheduler queues. */
	assert(range->in_dump.pos == UINT32_MAX);
	assert(range->in_compact.pos == UINT32_MAX);

	/* Start iteration. */
	if (vy_write_iterator_next(wi, &stmt) != 0)
		return -1;
	if (vy_range_write_run(range, wi, &stmt, &task->dump_size) != 0)
		return -1;
	return 0;
}

static int
vy_task_dump_complete(struct vy_task *task, bool in_shutdown)
{
	(void)in_shutdown;

	struct vy_index *index = task->index;
	struct vy_env *env = index->env;
	struct vy_range *range = task->range;
	struct vy_run *run = range->new_run;

	say_info("dump complete: %s", vy_range_str(range));

	vy_write_iterator_delete(task->wi);

	assert(run != NULL);
	range->new_run = NULL;

	rlist_add_entry(&range->runs, run, in_range);
	range->run_count++;
	vy_index_acct_range_dump(index, range, run);

	/*
	 * Release dumped in-memory indexes.
	 */
	while (!rlist_empty(&range->frozen)) {
		struct vy_mem *mem;
		mem = rlist_shift_entry(&range->frozen,
					struct vy_mem, in_frozen);
		vy_range_delete_mem(range, mem);
	}
	range->used = range->mem->used;
	range->min_lsn = range->mem->min_lsn;
	range->version++;

	vy_scheduler_add_range(env->scheduler, range);
	return 0;
}

static void
vy_task_dump_abort(struct vy_task *task, bool in_shutdown)
{
	(void)in_shutdown;

	struct vy_index *index = task->index;
	struct vy_range *range = task->range;

	say_error("dump of range %s failed", vy_range_str(range));

	vy_write_iterator_delete(task->wi);

	/* Delete the run we failed to write. */
	vy_run_delete(range->new_run);
	range->new_run = NULL;

	/*
	 * No need to roll back anything if we failed to write a run.
	 * The range will carry on with a new shadow in-memory index.
	 */
	vy_scheduler_add_range(index->env->scheduler, range);
}

static struct vy_task *
vy_task_dump_new(struct mempool *pool, struct vy_range *range)
{
	static struct vy_task_ops dump_ops = {
		.execute = vy_task_dump_execute,
		.complete = vy_task_dump_complete,
		.abort = vy_task_dump_abort,
	};
	struct vy_index *index = range->index;

	struct vy_task *task = vy_task_new(pool, index, &dump_ops);
	if (task == NULL)
		goto err_task;

	struct vy_write_iterator *wi;
	wi = vy_write_iterator_new(index, range->run_count == 0,
				   tx_manager_vlsn(index->env->xm));
	if (wi == NULL)
		goto err_wi;

	/* We are going to dump all in-memory indexes. */
	struct vy_mem *mem;
	if (range->mem->used > 0 &&
	    vy_write_iterator_add_mem(wi, range->mem) != 0)
		goto err_wi_sub;
	rlist_foreach_entry(mem, &range->frozen, in_frozen) {
		if (vy_write_iterator_add_mem(wi, mem) != 0)
			goto err_wi_sub;
	}

	range->new_run = vy_run_new();
	if (range->new_run == NULL)
		goto err_run;

	/*
	 * If the newest mem is empty, we don't need to dump it
	 * and therefore can omit creating a new mem.
	 */
	if (range->mem->used == 0)
		goto done;

	mem = vy_mem_new(index->env, index->key_def, index->format);
	if (mem == NULL)
		goto err_mem;

	/*
	 * New insertions will go to the new in-memory tree, while we will dump
	 * older trees. This way we don't need to bother about synchronization.
	 * To be consistent, lookups fall back on older trees.
	 */
	vy_range_freeze_mem(range);
	range->mem = mem;
	range->version++;
done:
	task->range = range;
	task->wi = wi;
	say_info("started dump of range %s", vy_range_str(range));
	return task;
err_mem:
	vy_run_delete(range->new_run);
	range->new_run = NULL;
err_run:
	/* Sub iterators are deleted by vy_write_iterator_delete(). */
err_wi_sub:
	vy_write_iterator_delete(wi);
err_wi:
	vy_task_delete(pool, task);
err_task:
	return NULL;
}

static int
vy_task_compact_execute(struct vy_task *task)
{
	struct vy_range *range = task->range;
	struct vy_write_iterator *wi = task->wi;
	struct vy_stmt *stmt;
	struct vy_range *r;

	/* The range has been deleted from the scheduler queues. */
	assert(range->in_dump.pos == UINT32_MAX);
	assert(range->in_compact.pos == UINT32_MAX);

	/* Start iteration. */
	if (vy_write_iterator_next(wi, &stmt) != 0)
		return -1;
	assert(!rlist_empty(&range->compact_list));
	rlist_foreach_entry(r, &range->compact_list, compact_list) {
		assert(r->shadow == range);
		if (&r->compact_list != rlist_first(&range->compact_list)) {
			ERROR_INJECT(ERRINJ_VY_RANGE_SPLIT,
				     {diag_set(ClientError, ER_INJECTION,
					       "vinyl range split");
				      return -1;});
		}
		if (vy_range_write_run(r, wi, &stmt, &task->dump_size) != 0)
			return -1;
	}
	/* Old run files are removed on snapshot. */
	return 0;
}

static int
vy_task_compact_complete(struct vy_task *task, bool in_shutdown)
{
	(void)in_shutdown;

	struct vy_index *index = task->index;
	struct vy_range *range = task->range;
	struct vy_range *r;

	say_info("completed compaction of range %s", vy_range_str(range));

	vy_write_iterator_delete(task->wi);

	vy_index_unacct_range(index, range);
	rlist_foreach_entry(r, &range->compact_list, compact_list) {
		/* Add the new run created by compaction to the list. */
		rlist_add_entry(&r->runs, r->new_run, in_range);
		r->run_count++;
		r->new_run = NULL;
		/*
		 * Account the new range and make it visible to
		 * the scheduler.
		 */
		vy_index_acct_range(index, r);
		vy_scheduler_add_range(index->env->scheduler, r);
	}

	vy_range_commit_compact_parts(range);
	index->version++;
	return 0;
}

static void
vy_task_compact_abort(struct vy_task *task, bool in_shutdown)
{
	(void)in_shutdown;

	struct vy_index *index = task->index;
	struct vy_range *range = task->range;

	say_error("compaction of range %s failed", vy_range_str(range));

	vy_write_iterator_delete(task->wi);

	vy_range_discard_compact_parts(range);
	index->version++;

	vy_scheduler_add_range(index->env->scheduler, range);
}

static struct vy_task *
vy_task_compact_new(struct mempool *pool, struct vy_range *range)
{
	assert(rlist_empty(&range->compact_list));

	static struct vy_task_ops compact_ops = {
		.execute = vy_task_compact_execute,
		.complete = vy_task_compact_complete,
		.abort = vy_task_compact_abort,
	};

	struct vy_index *index = range->index;
	struct vy_stmt *split_key = NULL;
	const char *split_key_raw;
	struct vy_range *parts[2] = {NULL, };
	struct vy_stmt *keys[3];
	int n_parts = 1;

	struct vy_task *task = vy_task_new(pool, index, &compact_ops);
	if (task == NULL)
		goto err_task;

	struct vy_write_iterator *wi;
	wi = vy_write_iterator_new(index, true,
				   tx_manager_vlsn(index->env->xm));
	if (wi == NULL)
		goto err_wi;

	/*
	 * Prepare for merge. Note, merge iterator requires newer
	 * sources to be added first so mems are added before runs.
	 */
	struct vy_mem *mem;
	if (vy_write_iterator_add_mem(wi, range->mem) != 0)
		goto err_wi_sub;
	rlist_foreach_entry(mem, &range->frozen, in_frozen) {
		if (vy_write_iterator_add_mem(wi, mem) != 0)
			goto err_wi_sub;
	}
	struct vy_run *run;
	rlist_foreach_entry(run, &range->runs, in_range) {
		if (vy_write_iterator_add_run(wi, range, run) != 0)
			goto err_wi_sub;
	}

	/* Determine new ranges' boundaries. */
	keys[0] = range->begin;
	if (vy_range_needs_split(range, &split_key_raw)) {
		split_key = vy_key_from_msgpack(split_key_raw,
						     index->key_def);
		if (split_key == NULL)
			goto err_split_key;
		n_parts = 2;
		keys[1] = split_key;
		keys[2] = range->end;
	} else
		keys[1] = range->end;

	/* Allocate new ranges. */
	for (int i = 0; i < n_parts; i++) {
		struct vy_range *r;

		r = parts[i] = vy_range_new(index, 0, keys[i], keys[i + 1]);
		if (r == NULL)
			goto err_parts;
		r->new_run = vy_run_new();
		if (r->new_run == NULL)
			goto err_parts;
		r->mem = vy_mem_new(index->env, index->key_def, index->format);
		if (r->mem == NULL)
			goto err_parts;
		/* Account merge w/o split. */
		if (n_parts == 1)
			r->n_compactions = range->n_compactions + 1;
	}

	say_info("started compaction of range %s", vy_range_str(range));

	/* Replace the old range with the new ones. */
	for (int i = 0; i < n_parts; i++)
		vy_range_add_compact_part(range, parts[i]);

	range->version++;
	index->version++;

	task->range = range;
	task->wi = wi;
	return task;
err_parts:
	for (int i = 0; i < n_parts; i++) {
		if (parts[i] != NULL)
			vy_range_delete(parts[i]);
	}
	if (split_key != NULL)
		vy_stmt_unref(split_key);
err_split_key:
	/* Sub iterators are deleted by vy_write_iterator_delete(). */
err_wi_sub:
	vy_write_iterator_delete(wi);
err_wi:
	vy_task_delete(pool, task);
err_task:
	return NULL;
}

/* Scheduler Task }}} */

/* {{{ Scheduler */

#define HEAP_NAME vy_dump_heap

static bool
heap_dump_less(struct heap_node *a, struct heap_node *b)
{
	struct vy_range *left = container_of(a, struct vy_range, in_dump);
	struct vy_range *right = container_of(b, struct vy_range, in_dump);

	/* Older ranges are dumped first. */
	return left->min_lsn < right->min_lsn;
}

#define HEAP_LESS(h, l, r) heap_dump_less(l, r)

#include "salad/heap.h"

#undef HEAP_LESS
#undef HEAP_NAME

#define HEAP_NAME vy_compact_heap

static int
heap_compact_less(struct heap_node *a, struct heap_node *b)
{
	const struct vy_range *left =
				container_of(a, struct vy_range, in_compact);
	const struct vy_range *right =
				container_of(b, struct vy_range, in_compact);
	return left->run_count > right->run_count;
}

#define HEAP_LESS(h, l, r) heap_compact_less(l, r)

#include "salad/heap.h"

struct vy_scheduler {
	pthread_mutex_t        mutex;
	struct vy_env    *env;
	heap_t dump_heap;
	heap_t compact_heap;

	struct cord *worker_pool;
	struct fiber *scheduler;
	struct ev_loop *loop;
	int worker_pool_size;
	bool is_worker_pool_running;

	/**
	 * There is a pending task for workers in the pool,
	 * or we want to shutdown workers.
	 */
	pthread_cond_t worker_cond;
	/**
	 * There is no pending tasks for workers, so scheduler
	 * needs to create one, or we want to shutdown the
	 * scheduler. Scheduler is a fiber in TX, so ev_async + ipc_channel
	 * are used here instead of pthread_cond_t.
	 */
	struct ev_async scheduler_async;
	struct ipc_cond scheduler_cond;
	/** Used for throttling tx when quota is full. */
	struct ipc_cond quota_cond;
	/**
	 * A queue with all vy_task objects created by the
	 * scheduler and not yet taken by a worker.
	 */
	struct stailq input_queue;
	/**
	 * A queue of processed vy_tasks objects.
	 */
	struct stailq output_queue;
	/**
	 * A memory pool for vy_tasks.
	 */
	struct mempool task_pool;

	/** Last error seen by the scheduler. */
	struct diag diag;
	/**
	 * Schedule timeout. Grows exponentially with each successive
	 * failure. Reset on successful task completion.
	 */
	ev_tstamp timeout;
	/** Set if the scheduler is throttled due to errors. */
	bool is_throttled;

	/**
	 * List of all non-empty in-memory indexes.
	 * Older mems are closer to the tail of the list.
	 */
	struct rlist dirty_mems;
	/** Min LSN over all in-memory indexes. */
	int64_t mem_min_lsn;
	/**
	 * LSN at the time of checkpoint start. All in-memory indexes with
	 * min_lsn <= checkpoint_lsn should be dumped first.
	 */
	int64_t checkpoint_lsn;
	/** Signaled on checkpoint completion or failure. */
	struct ipc_cond checkpoint_cond;
};

/* Min and max values for vy_scheduler->timeout. */
#define VY_SCHEDULER_TIMEOUT_MIN		1
#define VY_SCHEDULER_TIMEOUT_MAX		60

static void
vy_scheduler_start_workers(struct vy_scheduler *scheduler);
static void
vy_scheduler_stop_workers(struct vy_scheduler *scheduler);
static int
vy_scheduler_f(va_list va);

static void
vy_scheduler_quota_cb(enum vy_quota_event event, void *arg)
{
	struct vy_scheduler *scheduler = arg;

	if (scheduler->env->status != VINYL_ONLINE)
		return;

	switch (event) {
	case VY_QUOTA_EXCEEDED:
		ipc_cond_signal(&scheduler->scheduler_cond);
		break;
	case VY_QUOTA_THROTTLED:
		ipc_cond_wait(&scheduler->quota_cond);
		break;
	case VY_QUOTA_RELEASED:
		ipc_cond_broadcast(&scheduler->quota_cond);
		break;
	default:
		unreachable();
	}
}

static void
vy_scheduler_async_cb(ev_loop *loop, struct ev_async *watcher, int events)
{
	(void) loop;
	(void) events;
	struct vy_scheduler *scheduler =
		container_of(watcher, struct vy_scheduler, scheduler_async);
	ipc_cond_signal(&scheduler->scheduler_cond);
}

static struct vy_scheduler *
vy_scheduler_new(struct vy_env *env)
{
	struct vy_scheduler *scheduler = calloc(1, sizeof(*scheduler));
	if (scheduler == NULL) {
		diag_set(OutOfMemory, sizeof(*scheduler), "scheduler",
			 "struct");
		return NULL;
	}
	tt_pthread_mutex_init(&scheduler->mutex, NULL);
	diag_create(&scheduler->diag);
	rlist_create(&scheduler->dirty_mems);
	scheduler->mem_min_lsn = INT64_MAX;
	ipc_cond_create(&scheduler->checkpoint_cond);
	scheduler->env = env;
	vy_compact_heap_create(&scheduler->compact_heap);
	vy_dump_heap_create(&scheduler->dump_heap);
	tt_pthread_cond_init(&scheduler->worker_cond, NULL);
	scheduler->loop = loop();
	ev_async_init(&scheduler->scheduler_async, vy_scheduler_async_cb);
	ipc_cond_create(&scheduler->scheduler_cond);
	ipc_cond_create(&scheduler->quota_cond);
	mempool_create(&scheduler->task_pool, cord_slab_cache(),
			sizeof(struct vy_task));
	/* Start scheduler fiber. */
	scheduler->scheduler = fiber_new("vinyl.scheduler", vy_scheduler_f);
	if (scheduler->scheduler == NULL)
		panic("failed to start vinyl scheduler fiber");
	fiber_start(scheduler->scheduler, scheduler);
	return scheduler;
}

static void
vy_scheduler_delete(struct vy_scheduler *scheduler)
{
	/* Stop scheduler fiber. */
	scheduler->scheduler = NULL;
	/* Sic: fiber_cancel() can't be used here. */
	ipc_cond_signal(&scheduler->scheduler_cond);

	if (scheduler->is_worker_pool_running)
		vy_scheduler_stop_workers(scheduler);

	mempool_destroy(&scheduler->task_pool);
	diag_destroy(&scheduler->diag);
	vy_compact_heap_destroy(&scheduler->compact_heap);
	vy_dump_heap_destroy(&scheduler->dump_heap);
	tt_pthread_cond_destroy(&scheduler->worker_cond);
	TRASH(&scheduler->scheduler_async);
	ipc_cond_destroy(&scheduler->scheduler_cond);
	ipc_cond_destroy(&scheduler->quota_cond);
	tt_pthread_mutex_destroy(&scheduler->mutex);
	free(scheduler);
}

static void
vy_scheduler_add_range(struct vy_scheduler *scheduler,
		       struct vy_range *range)
{
	vy_dump_heap_insert(&scheduler->dump_heap, &range->in_dump);
	vy_compact_heap_insert(&scheduler->compact_heap, &range->in_compact);
	assert(range->in_dump.pos != UINT32_MAX);
	assert(range->in_compact.pos != UINT32_MAX);
}

static void
vy_scheduler_update_range(struct vy_scheduler *scheduler,
			  struct vy_range *range)
{
	if (range->in_dump.pos == UINT32_MAX)
		return; /* range is being processed by a task */

	vy_dump_heap_update(&scheduler->dump_heap, &range->in_dump);
	assert(range->in_dump.pos != UINT32_MAX);
	assert(range->in_compact.pos != UINT32_MAX);
}

static void
vy_scheduler_remove_range(struct vy_scheduler *scheduler,
			  struct vy_range *range)
{
	vy_dump_heap_delete(&scheduler->dump_heap, &range->in_dump);
	vy_compact_heap_delete(&scheduler->compact_heap, &range->in_compact);
	range->in_dump.pos = UINT32_MAX;
	range->in_compact.pos = UINT32_MAX;
}

static int
vy_scheduler_peek_dump(struct vy_scheduler *scheduler, struct vy_task **ptask)
{
	/* try to peek a range with a biggest in-memory index */
	struct vy_range *range;
	struct heap_node *pn = NULL;
	struct heap_iterator it;
	vy_dump_heap_iterator_init(&scheduler->dump_heap, &it);
	while ((pn = vy_dump_heap_iterator_next(&it))) {
		range = container_of(pn, struct vy_range, in_dump);
		if (!vy_quota_is_exceeded(&scheduler->env->quota) &&
		    range->min_lsn > scheduler->checkpoint_lsn)
			return 0; /* nothing to do */
		*ptask = vy_task_dump_new(&scheduler->task_pool, range);
		if (*ptask == NULL)
			return -1; /* oom */
		vy_scheduler_remove_range(scheduler, range);
		return 0; /* new task */
	}
	*ptask = NULL;
	return 0; /* nothing to do */
}

static int
vy_scheduler_peek_compact(struct vy_scheduler *scheduler,
			  struct vy_task **ptask)
{
	/* try to peek a range with a biggest number
	 * of runs */
	struct vy_range *range;
	struct heap_node *pn = NULL;
	struct heap_iterator it;
	vy_compact_heap_iterator_init(&scheduler->compact_heap, &it);
	while ((pn = vy_compact_heap_iterator_next(&it))) {
		range = container_of(pn, struct vy_range, in_compact);
		if ((unsigned)range->run_count <
		    range->index->key_def->opts.compact_wm)
			break; /* TODO: why ? */
		*ptask = vy_task_compact_new(&scheduler->task_pool,
					     range);
		if (*ptask == NULL)
			return -1; /* OOM */
		vy_scheduler_remove_range(scheduler, range);
		return 0; /* new task */
	}
	*ptask = NULL;
	return 0; /* nothing to do */
}

static int
vy_schedule(struct vy_scheduler *scheduler, struct vy_task **ptask)
{
	int rc;

	*ptask = NULL;
	if (rlist_empty(&scheduler->env->indexes))
		return 0;

	rc = vy_scheduler_peek_dump(scheduler, ptask);
	if (rc != 0)
		return rc; /* error */
	if (*ptask != NULL)
		return 0;

	rc = vy_scheduler_peek_compact(scheduler, ptask);
	if (rc != 0)
		return rc; /* error */
	if (*ptask != NULL)
		return 0;

	/* no task to run */
	return 0;

}

static int
vy_scheduler_complete_task(struct vy_scheduler *scheduler,
			   struct vy_task *task, bool in_shutdown)
{
	if (task->status != 0) {
		/* ->execute failed, propagate diag */
		assert(!diag_is_empty(&task->diag));
		diag_move(&task->diag, &scheduler->diag);
		goto fail;
	}
	if (task->ops->complete &&
	    task->ops->complete(task, in_shutdown) != 0) {
		assert(!diag_is_empty(diag_get()));
		diag_move(diag_get(), &scheduler->diag);
		goto fail;
	}
	return 0;
fail:
	if (task->ops->abort)
		task->ops->abort(task, in_shutdown);
	return -1;
}

static int
vy_scheduler_f(va_list va)
{
	struct vy_scheduler *scheduler = va_arg(va, struct vy_scheduler *);
	struct vy_env *env = scheduler->env;
	bool warning_said = false;

	/*
	 * Yield immediately, until the quota watermark is reached
	 * for the first time or a checkpoint is made.
	 * Then start the worker threads: we know they will be
	 * needed. If quota watermark is never reached, workers
	 * are not started and the scheduler is idle until
	 * shutdown or checkpoint.
	 */
	ipc_cond_wait(&scheduler->scheduler_cond);
	if (scheduler->scheduler == NULL)
		return 0; /* destroyed */
	vy_scheduler_start_workers(scheduler);

	int workers_available = scheduler->worker_pool_size;
	while (scheduler->scheduler != NULL) {
		struct stailq output_queue;
		struct vy_task *task, *next;
		int tasks_failed = 0, tasks_done = 0;
		bool was_empty;

		/* Get the list of processed tasks. */
		stailq_create(&output_queue);
		tt_pthread_mutex_lock(&scheduler->mutex);
		stailq_concat(&output_queue, &scheduler->output_queue);
		tt_pthread_mutex_unlock(&scheduler->mutex);

		/* Complete and delete all processed tasks. */
		stailq_foreach_entry_safe(task, next, &output_queue, link) {
			if (vy_scheduler_complete_task(scheduler,
						       task, false) != 0)
				tasks_failed++;
			else
				tasks_done++;
			if (task->dump_size > 0)
				vy_stat_dump(env->stat, task->exec_time,
					     task->dump_size);
			vy_task_delete(&scheduler->task_pool, task);
			workers_available++;
			assert(workers_available <= scheduler->worker_pool_size);
		}
		/*
		 * Reset the timeout if we managed to successfully
		 * complete at least one task.
		 */
		if (tasks_done > 0) {
			scheduler->timeout = 0;
			warning_said = false;
		}
		if (tasks_failed > 0)
			goto error;

		/* All worker threads are busy. */
		if (workers_available == 0)
			goto wait;

		/* Get a task to schedule. */
		if (vy_schedule(scheduler, &task) != 0) {
			struct diag *diag = diag_get();
			assert(!diag_is_empty(diag));
			diag_move(diag, &scheduler->diag);
			/* Can't schedule task right now */
			goto error;
		}

		/* Nothing to do. */
		if (task == NULL)
			goto wait;

		/* Queue the task and notify workers if necessary. */
		tt_pthread_mutex_lock(&scheduler->mutex);
		was_empty = stailq_empty(&scheduler->input_queue);
		stailq_add_tail_entry(&scheduler->input_queue, task, link);
		if (was_empty)
			tt_pthread_cond_signal(&scheduler->worker_cond);
		tt_pthread_mutex_unlock(&scheduler->mutex);

		workers_available--;
		fiber_reschedule();
		continue;
error:
		/* Log error message once. */
		assert(!diag_is_empty(&scheduler->diag));
		if (!warning_said) {
			error_log(diag_last_error(&scheduler->diag));
			warning_said = true;
		}
		/* Abort pending checkpoint. */
		ipc_cond_signal(&scheduler->checkpoint_cond);
		/*
		 * A task can fail either due to lack of memory or IO
		 * error. In either case it is pointless to schedule
		 * another task right away, because it is likely to fail
		 * too. So we throttle the scheduler for a while after
		 * each failure.
		 */
		scheduler->timeout *= 2;
		if (scheduler->timeout < VY_SCHEDULER_TIMEOUT_MIN)
			scheduler->timeout = VY_SCHEDULER_TIMEOUT_MIN;
		if (scheduler->timeout > VY_SCHEDULER_TIMEOUT_MAX)
			scheduler->timeout = VY_SCHEDULER_TIMEOUT_MAX;
		say_info("throttling the scheduler for %.0f seconds",
			  scheduler->timeout);
		scheduler->is_throttled = true;
		fiber_sleep(scheduler->timeout);
		scheduler->is_throttled = false;
		continue;
wait:
		/* Wait for changes */
		ipc_cond_wait(&scheduler->scheduler_cond);
	}

	return 0;
}

static int
vy_worker_f(va_list va)
{
	struct vy_scheduler *scheduler = va_arg(va, struct vy_scheduler *);
	coeio_enable();
	struct vy_task *task = NULL;

	tt_pthread_mutex_lock(&scheduler->mutex);
	while (scheduler->is_worker_pool_running) {
		/* Wait for a task */
		if (stailq_empty(&scheduler->input_queue)) {
			/* Wake scheduler up if there are no more tasks */
			ev_async_send(scheduler->loop,
				      &scheduler->scheduler_async);
			tt_pthread_cond_wait(&scheduler->worker_cond,
					     &scheduler->mutex);
			continue;
		}
		task = stailq_shift_entry(&scheduler->input_queue,
					  struct vy_task, link);
		tt_pthread_mutex_unlock(&scheduler->mutex);
		assert(task != NULL);

		/* Execute task */
		uint64_t start = ev_now(loop());
		task->status = task->ops->execute(task);
		task->exec_time = ev_now(loop()) - start;
		if (task->status != 0) {
			struct diag *diag = diag_get();
			assert(!diag_is_empty(diag));
			diag_move(diag, &task->diag);
		}

		/* Return processed task to scheduler */
		tt_pthread_mutex_lock(&scheduler->mutex);
		stailq_add_tail_entry(&scheduler->output_queue, task, link);
	}
	tt_pthread_mutex_unlock(&scheduler->mutex);
	return 0;
}

static void
vy_scheduler_start_workers(struct vy_scheduler *scheduler)
{
	assert(!scheduler->is_worker_pool_running);
	assert(scheduler->env->status == VINYL_ONLINE);

	/* Start worker threads */
	scheduler->is_worker_pool_running = true;
	scheduler->worker_pool_size = cfg_geti("vinyl.threads");
	if (scheduler->worker_pool_size < 0)
		scheduler->worker_pool_size = 1;
	stailq_create(&scheduler->input_queue);
	stailq_create(&scheduler->output_queue);
	scheduler->worker_pool = (struct cord *)
		calloc(scheduler->worker_pool_size, sizeof(struct cord));
	if (scheduler->worker_pool == NULL)
		panic("failed to allocate vinyl worker pool");
	ev_async_start(scheduler->loop, &scheduler->scheduler_async);
	for (int i = 0; i < scheduler->worker_pool_size; i++) {
		cord_costart(&scheduler->worker_pool[i], "vinyl.worker",
			     vy_worker_f, scheduler);
	}
}

static void
vy_scheduler_stop_workers(struct vy_scheduler *scheduler)
{
	assert(scheduler->is_worker_pool_running);
	scheduler->is_worker_pool_running = false;

	/* Abort all pending tasks and wake up worker threads */
	tt_pthread_mutex_lock(&scheduler->mutex);
	struct vy_task *task, *next;
	stailq_foreach_entry_safe(task, next, &scheduler->input_queue, link) {
		if (task->ops->abort)
			task->ops->abort(task, true);
		vy_task_delete(&scheduler->task_pool, task);
	}
	stailq_create(&scheduler->input_queue);
	pthread_cond_broadcast(&scheduler->worker_cond);
	tt_pthread_mutex_unlock(&scheduler->mutex);

	/* Join worker threads */
	for (int i = 0; i < scheduler->worker_pool_size; i++)
		cord_join(&scheduler->worker_pool[i]);
	ev_async_stop(scheduler->loop, &scheduler->scheduler_async);
	free(scheduler->worker_pool);
	scheduler->worker_pool = NULL;
	scheduler->worker_pool_size = 0;

	/* Complete all processed tasks */
	stailq_foreach_entry_safe(task, next, &scheduler->output_queue, link) {
		vy_scheduler_complete_task(scheduler, task, true);
		vy_task_delete(&scheduler->task_pool, task);
	}
	stailq_create(&scheduler->output_queue);
}

static void
vy_scheduler_mem_dirtied(struct vy_scheduler *scheduler, struct vy_mem *mem)
{
	if (rlist_empty(&scheduler->dirty_mems))
		scheduler->mem_min_lsn = mem->min_lsn;
	assert(scheduler->mem_min_lsn <= mem->min_lsn);
	rlist_add_entry(&scheduler->dirty_mems, mem, in_dirty);
}

static void
vy_scheduler_mem_dumped(struct vy_scheduler *scheduler, struct vy_mem *mem)
{
	struct vy_env *env = scheduler->env;

	if (mem->used == 0)
		return;

	rlist_del_entry(mem, in_dirty);

	if (!rlist_empty(&scheduler->dirty_mems)) {
		struct vy_mem *oldest;
		oldest = rlist_last_entry(&scheduler->dirty_mems,
					  struct vy_mem, in_dirty);
		scheduler->mem_min_lsn = oldest->min_lsn;
	} else {
		scheduler->mem_min_lsn = INT64_MAX;
	}

	/* Free memory and release quota. */
	struct lsregion *allocator = &env->allocator;
	size_t mem_used_before = lsregion_used(allocator);
	lsregion_gc(allocator, scheduler->mem_min_lsn);
	size_t mem_used_after = lsregion_used(allocator);
	assert(mem_used_after <= mem_used_before);
	vy_quota_release(&env->quota, mem_used_before - mem_used_after);

	if (scheduler->mem_min_lsn > scheduler->checkpoint_lsn) {
		/*
		 * All in-memory indexes have been checkpointed. Wake up
		 * the fiber waiting for checkpoint to complete.
		 */
		ipc_cond_signal(&scheduler->checkpoint_cond);
	}
}

/*
 * Schedule checkpoint. Please call vy_wait_checkpoint() after that.
 */
int
vy_checkpoint(struct vy_env *env)
{
	struct vy_scheduler *scheduler = env->scheduler;

	assert(env->status == VINYL_ONLINE);

	scheduler->checkpoint_lsn = env->xm->lsn;
	if (scheduler->mem_min_lsn > scheduler->checkpoint_lsn)
		return 0; /* nothing to do */

	/*
	 * If the scheduler is throttled due to errors, do not wait
	 * until it wakes up as it may take quite a while. Instead
	 * fail checkpoint immediately with the last error seen by
	 * the scheduler.
	 */
	if (scheduler->is_throttled) {
		assert(!diag_is_empty(&scheduler->diag));
		diag_add_error(diag_get(), diag_last_error(&scheduler->diag));
		return -1;
	}

	ipc_cond_signal(&scheduler->scheduler_cond);
	return 0;
}

static void
vy_index_gc(struct vy_index *index);

int
vy_wait_checkpoint(struct vy_env *env, struct vclock *vclock)
{
	(void)vclock;
	struct vy_scheduler *scheduler = env->scheduler;

	while (!scheduler->is_throttled &&
	       scheduler->mem_min_lsn <= scheduler->checkpoint_lsn)
		ipc_cond_wait(&scheduler->checkpoint_cond);

	if (scheduler->mem_min_lsn <= scheduler->checkpoint_lsn) {
		assert(!diag_is_empty(&scheduler->diag));
		diag_add_error(diag_get(), diag_last_error(&scheduler->diag));
		return -1;
	}

	/* Remove old run files. */
	struct vy_index *index;
	rlist_foreach_entry(index, &env->indexes, link)
		vy_index_gc(index);

	return 0;
}

/**
 * Unlink old ranges - i.e. ranges which are not relevant
 * any more because of a passed range split, or create/drop
 * index.
 */
static void
vy_index_gc(struct vy_index *index)
{
	ERROR_INJECT(ERRINJ_VY_GC, return);

	struct mh_i32ptr_t *ranges = NULL;
	DIR *dir = NULL;

	ranges = mh_i32ptr_new();
	if (ranges == NULL)
		goto error;
	/*
	 * Construct a hash map of existing ranges, to quickly
	 * find a valid range by range id.
	 */
	struct vy_range *range = vy_range_tree_first(&index->tree);
	while (range) {
		struct mh_i32ptr_node_t node = {range->id, range};
		if (mh_i32ptr_put(ranges, &node, NULL, NULL) == mh_end(ranges))
			goto error;
		if (range->shadow != NULL) {
			node.key = range->shadow->id;
			node.val = range->shadow;
			if (mh_i32ptr_put(ranges, &node, NULL, NULL) ==
			    mh_end(ranges))
				goto error;
		}
		range = vy_range_tree_next(&index->tree, range);
	}
	/*
	 * Scan the index directory and unlink files not
	 * referenced from any valid range.
	 */
	dir = opendir(index->path);
	if (dir == NULL)
		goto error;
	struct dirent *dirent;
	/**
	 * @todo: only remove files matching the pattern *and*
	 * identified as old, not all files.
	 */
	while ((dirent = readdir(dir))) {
		int64_t index_lsn;
		int64_t range_id;
		int run_id;
		enum vy_file_type t;

		if (!(strcmp(".", dirent->d_name)))
			continue;
		if (!(strcmp("..", dirent->d_name)))
			continue;
		bool is_vinyl_file = false;
		/*
		 * Now we can delete in progress file, this is bad
		if (strstr(dirent->d_name, ".tmp") == dirent->d_name) {
			is_vinyl_file = true;
		}
		*/
		if (vy_run_parse_name(dirent->d_name, &index_lsn,
				      &range_id, &run_id, &t) == 0) {
			is_vinyl_file = true;
			mh_int_t range = mh_i32ptr_find(ranges, range_id, NULL);
			if (index_lsn == index->key_def->opts.lsn &&
			    range != mh_end(ranges))
				continue;
		}
		if (!is_vinyl_file)
			continue;
		char path[PATH_MAX];
		snprintf(path, PATH_MAX, "%s/%s",
			 index->path, dirent->d_name);
		unlink(path);
	}
	goto end;
error:
	say_syserror("failed to cleanup index directory %s", index->path);
end:
	if (dir != NULL)
		closedir(dir);
	if (ranges != NULL)
		mh_i32ptr_delete(ranges);
}

/* Scheduler }}} */

static struct vy_conf *
vy_conf_new()
{
	struct vy_conf *conf = calloc(1, sizeof(*conf));
	if (conf == NULL) {
		diag_set(OutOfMemory, sizeof(*conf), "conf", "struct");
		return NULL;
	}
	conf->memory_limit = cfg_getd("vinyl.memory_limit")*1024*1024*1024;

	conf->path = strdup(cfg_gets("vinyl_dir"));
	if (conf->path == NULL) {
		diag_set(OutOfMemory, sizeof(*conf), "conf", "path");
		goto error_1;
	}
	/* Ensure vinyl data directory exists. */
	if (!path_exists(conf->path)) {
		diag_set(ClientError, ER_CFG, "vinyl_dir",
			 "directory does not exist");
		goto error_2;
	}
	return conf;

error_2:
	free(conf->path);
error_1:
	free(conf);
	return NULL;
}

static void vy_conf_delete(struct vy_conf *c)
{
	free(c->path);
	free(c);
}

/** {{{ Introspection */

static void
vy_info_append_u32(struct vy_info_handler *h, const char *key, uint32_t value)
{
	struct vy_info_node node = {
		.type = VY_INFO_U32,
		.key = key,
		.value.u32 = value,
	};
	h->fn(&node, h->ctx);
}

static void
vy_info_append_u64(struct vy_info_handler *h, const char *key, uint64_t value)
{
	struct vy_info_node node = {
		.type = VY_INFO_U64,
		.key = key,
		.value.u64 = value,
	};
	h->fn(&node, h->ctx);
}

static void
vy_info_append_str(struct vy_info_handler *h, const char *key,
		   const char *value)
{
	struct vy_info_node node = {
		.type = VY_INFO_STRING,
		.key = key,
		.value.str = value,
	};
	h->fn(&node, h->ctx);
}

static void
vy_info_table_begin(struct vy_info_handler *h, const char *key)
{
	struct vy_info_node node = {
		.type = VY_INFO_TABLE_BEGIN,
		.key = key,
	};
	h->fn(&node, h->ctx);
}

static void
vy_info_table_end(struct vy_info_handler *h)
{
	struct vy_info_node node = {
		.type = VY_INFO_TABLE_END,
	};
	h->fn(&node, h->ctx);
}

static void
vy_info_append_global(struct vy_env *env, struct vy_info_handler *h)
{
	vy_info_table_begin(h, "vinyl");
	vy_info_append_str(h, "path", env->conf->path);
	vy_info_append_str(h, "build", PACKAGE_VERSION);
	vy_info_table_end(h);
}

static void
vy_info_append_memory(struct vy_env *env, struct vy_info_handler *h)
{
	char buf[16];
	struct vy_quota *q = &env->quota;
	vy_info_table_begin(h, "memory");
	vy_info_append_u64(h, "used", q->used);
	vy_info_append_u64(h, "limit", q->limit);
	vy_info_append_u64(h, "watermark", q->watermark);
	snprintf(buf, sizeof(buf), "%d%%", (int)(100 * q->used / q->limit));
	vy_info_append_str(h, "ratio", buf);
	vy_info_append_u64(h, "min_lsn", env->scheduler->mem_min_lsn);
	vy_info_table_end(h);
}

static int
vy_info_append_stat_rmean(const char *name, int rps, int64_t total, void *ctx)
{
	struct vy_info_handler *h = ctx;
	vy_info_table_begin(h, name);
	vy_info_append_u32(h, "rps", rps);
	vy_info_append_u64(h, "total", total);
	vy_info_table_end(h);
	return 0;
}

static void
vy_info_append_stat_latency(struct vy_info_handler *h,
			    const char *name, struct vy_latency *lat)
{
	vy_info_table_begin(h, name);
	vy_info_append_u64(h, "max", lat->max * 1000000000);
	vy_info_append_u64(h, "avg", lat->count == 0 ? 0 :
			   lat->total / lat->count * 1000000000);
	vy_info_table_end(h);
}

static void
vy_info_append_performance(struct vy_env *env, struct vy_info_handler *h)
{
	struct vy_stat *stat = env->stat;

	vy_info_table_begin(h, "performance");

	rmean_foreach(stat->rmean, vy_info_append_stat_rmean, h);

	vy_info_append_u64(h, "write_count", stat->write_count);

	vy_info_append_stat_latency(h, "tx_latency", &stat->tx_latency);
	vy_info_append_stat_latency(h, "get_latency", &stat->get_latency);
	vy_info_append_stat_latency(h, "cursor_latency", &stat->cursor_latency);

	vy_info_append_u64(h, "tx_rollback", stat->tx_rlb);
	vy_info_append_u64(h, "tx_conflict", stat->tx_conflict);
	vy_info_append_u32(h, "tx_active_rw", env->xm->count_rw);
	vy_info_append_u32(h, "tx_active_ro", env->xm->count_rd);

	vy_info_append_u64(h, "dump_bandwidth", vy_stat_dump_bandwidth(stat));
	vy_info_append_u64(h, "dump_total", stat->dump_total);

	vy_info_table_end(h);
}

static void
vy_info_append_metric(struct vy_env *env, struct vy_info_handler *h)
{
	vy_info_table_begin(h, "metric");
	vy_info_append_u64(h, "lsn", env->xm->lsn);
	vy_info_table_end(h);
}

static void
vy_info_append_indices(struct vy_env *env, struct vy_info_handler *h)
{
	struct vy_index *i;
	char buf[1024];

	vy_info_table_begin(h, "db");
	rlist_foreach_entry(i, &env->indexes, link) {
		vy_info_table_begin(h, i->name);
		vy_info_append_u64(h, "range_size", i->key_def->opts.range_size);
		vy_info_append_u64(h, "page_size", i->key_def->opts.page_size);
		vy_info_append_u64(h, "memory_used", i->used);
		vy_info_append_u64(h, "size", i->size);
		vy_info_append_u64(h, "count", i->stmt_count);
		vy_info_append_u32(h, "page_count", i->page_count);
		vy_info_append_u32(h, "range_count", i->range_count);
		vy_info_append_u32(h, "run_count", i->run_count);
		vy_info_append_u32(h, "run_avg", i->run_count / i->range_count);
		histogram_snprint(buf, sizeof(buf), i->run_hist);
		vy_info_append_str(h, "run_histogram", buf);
		vy_info_table_end(h);
	}
	vy_info_table_end(h);
}

void
vy_info_gather(struct vy_env *env, struct vy_info_handler *h)
{
	vy_info_append_indices(env, h);
	vy_info_append_global(env, h);
	vy_info_append_memory(env, h);
	vy_info_append_metric(env, h);
	vy_info_append_performance(env, h);
}

/** }}} Introspection */

static int
vy_index_conf_create(struct vy_index *conf, struct key_def *key_def)
{
	char name[128];
	snprintf(name, sizeof(name), "%" PRIu32 "/%" PRIu32,
	         key_def->space_id, key_def->iid);
	conf->name = strdup(name);
	/* path */
	if (key_def->opts.path[0] == '\0') {
		char path[PATH_MAX];
		snprintf(path, sizeof(path), "%s/%" PRIu32 "/%" PRIu32,
			 cfg_gets("vinyl_dir"), key_def->space_id,
			 key_def->iid);
		conf->path = strdup(path);
	} else {
		conf->path = strdup(key_def->opts.path);
	}
	if (conf->name == NULL || conf->path == NULL) {
		if (conf->name)
			free(conf->name);
		if (conf->path)
			free(conf->path);
		conf->name = NULL;
		conf->path = NULL;
		diag_set(OutOfMemory, strlen(key_def->opts.path),
			 "strdup", "char *");
		return -1;
	}
	return 0;
}

/**
 * Detect whether we already have non-garbage index files,
 * and open an existing index if that's the case. Otherwise,
 * create a new index. Take the current recovery status into
 * account.
 */
static int
vy_index_open_or_create(struct vy_index *index)
{
	/*
	 * TODO: don't drop/recreate index in local wal
	 * recovery mode if all operations already done.
	 */
	switch (index->env->status) {
	case VINYL_ONLINE:
		/*
		 * The recovery is complete, simply
		 * create a new index.
		 */
		return vy_index_create(index);
	case VINYL_INITIAL_RECOVERY_REMOTE:
	case VINYL_FINAL_RECOVERY_REMOTE:
		/*
		 * Remote recovery. The index files do not
		 * exist locally, and we should create the
		 * index directory from scratch.
		 */
		return vy_index_create(index);
	case VINYL_INITIAL_RECOVERY_LOCAL:
	case VINYL_FINAL_RECOVERY_LOCAL:
		/*
		 * Local WAL replay or recovery from snapshot.
		 * In either case the index directory should
		 * have already been created, so try to load
		 * the index files from it.
		 */
		return vy_index_open_ex(index);
	default:
		unreachable();
	}
}

int
vy_index_open(struct vy_index *index)
{
	struct vy_env *env = index->env;

	if (vy_index_open_or_create(index) != 0)
		return -1;

	vy_index_ref(index);
	rlist_add(&env->indexes, &index->link);
	return 0;
}

static void
vy_index_ref(struct vy_index *index)
{
	index->refs++;
}

static void
vy_index_unref(struct vy_index *index)
{
	assert(index->refs > 0);
	if (--index->refs == 0)
		vy_index_delete(index);
}

int
vy_index_drop(struct vy_index *index)
{
	/* TODO:
	 * don't drop/recreate index in local wal recovery mode if all
	 * operations are already done.
	 */
	rlist_del(&index->link);
	vy_index_unref(index);
	return 0;
}

extern struct tuple_format_vtab vy_tuple_format_vtab;

struct vy_index *
vy_index_new(struct vy_env *e, struct key_def *user_key_def,
	     struct space *space)
{
	assert(space != NULL);
	static int64_t run_buckets[] = {
		0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 15, 20, 25, 50, 100,
	};

	assert(user_key_def->part_count > 0);
	struct vy_index *pk = NULL;
	if (user_key_def->iid > 0) {
		pk = vy_index_find(space, 0);
		assert(pk != NULL);
	}

	/**
	 * key_def is a merged user defined key_def of this index
	 * and key_def of the primary index, in which parts are
	 * renumbered.
	 *
	 * For instance:
	 * - merged primary and secondary: 3 (str), 6 (uint), 4 (scalar)
	 * - key_def:                      0 (str), 1 (uint), 2 (scalar)
	 *
	 * Condensing is necessary since a partial tuple consists
	 * only from primary and secondary key fields, coalesced.
	 */
	struct key_def *key_def;
	if (user_key_def->iid == 0) {
		key_def = key_def_dup(user_key_def);
	} else {
		key_def = key_def_build_secondary(pk->key_def, user_key_def);
	}
	if (key_def == NULL)
		return NULL;

	/* Original user defined key_def. */
	user_key_def = key_def_dup(user_key_def);
	if (user_key_def == NULL)
		goto fail_user_key_def;

	/*
	 * key_def that is used for extraction the key from a
	 * tuple.
	 */
	struct key_def *key_def_tuple_to_key;
	if (key_def->iid == 0) {
		key_def_tuple_to_key = NULL;
	} else {
		key_def_tuple_to_key =
			key_def_merge(user_key_def, pk->key_def);
		if (key_def_tuple_to_key == NULL)
			goto fail_key_def_tuple_to_key;
	}

	/*
	 * key_def that is used for extraction of the primary key
	 * from  a secondary index tuple.
	 */
	struct key_def *key_def_secondary_to_primary;
	if (key_def->iid == 0) {
		key_def_secondary_to_primary = NULL;
	} else {
		key_def_secondary_to_primary =
			key_def_build_secondary_to_primary(pk->key_def,
							   user_key_def);
		if (key_def_secondary_to_primary == NULL)
			goto fail_key_def_secondary_to_primary;
	}

	struct rlist key_list;
	rlist_create(&key_list);
	rlist_add_entry(&key_list, key_def, link);

	struct tuple_format *format =
		tuple_format_new(&key_list, &vy_tuple_format_vtab);
	assert(format != NULL);
	tuple_format_ref(format, 1);

	struct vy_index *index = calloc(1, sizeof(struct vy_index));
	if (index == NULL) {
		diag_set(OutOfMemory, sizeof(struct vy_index),
			 "calloc", "struct vy_index");
		goto fail_index;
	}
	index->env = e;

	if (vy_index_conf_create(index, key_def))
		goto fail_conf;

	index->key_def = key_def;
	assert(key_def != NULL);

	index->run_hist = histogram_new(run_buckets, lengthof(run_buckets));
	if (index->run_hist == NULL)
		goto fail_run_hist;

	index->format = format;
	if (key_def->iid > 0) {
		/**
		 * Calculate the bitmask of columns used in this
		 * index.
		 */
		for (uint32_t i = 0; i < user_key_def->part_count; ++i) {
			uint32_t fieldno = user_key_def->parts[i].fieldno;
			if (fieldno >= 64) {
				index->column_mask = UINT64_MAX;
				break;
			}
			index->column_mask |= ((uint64_t)1) << (63 - fieldno);
		}
	}

	vy_range_tree_new(&index->tree);
	index->version = 1;
	rlist_create(&index->link);
	read_set_new(&index->read_set);
	index->space = space;
	index->user_key_def = user_key_def;
	index->key_def_tuple_to_key = key_def_tuple_to_key;
	index->key_def_secondary_to_primary = key_def_secondary_to_primary;

	return index;

fail_run_hist:
	free(index->name);
	free(index->path);
fail_conf:
	free(index);
fail_index:
	tuple_format_ref(format, -1);
	if (key_def->iid > 0)
		key_def_delete(key_def_secondary_to_primary);
fail_key_def_secondary_to_primary:
	if (key_def->iid > 0)
		key_def_delete(key_def_tuple_to_key);
fail_key_def_tuple_to_key:
	key_def_delete(user_key_def);
fail_user_key_def:
	key_def_delete(key_def);
	return NULL;
}

void
vy_commit_alter_space(struct space *old_space, struct space *new_space)
{
	(void) old_space;
	struct vy_index *index;
	for (uint32_t i = 0; i < new_space->index_count; ++i) {
		index = vy_index(new_space->index[i]);
		index->space = new_space;
	}
}

static void
vy_index_delete(struct vy_index *index)
{
	read_set_iter(&index->read_set, NULL, read_set_delete_cb, NULL);
	vy_range_tree_iter(&index->tree, NULL, vy_range_tree_free_cb, index);
	free(index->name);
	free(index->path);
	tuple_format_ref(index->format, -1);
	key_def_delete(index->key_def);
	key_def_delete(index->user_key_def);
	if (index->key_def->iid > 0) {
		key_def_delete(index->key_def_tuple_to_key);
		key_def_delete(index->key_def_secondary_to_primary);
	}
	histogram_delete(index->run_hist);
	TRASH(index);
	free(index);
}

size_t
vy_index_bsize(struct vy_index *index)
{
	return index->used;
}

/* {{{ Statements */

/**
 * Create struct tuple from struct vy_stmt in the given space
 * format.
 * @param space   Space from which vinyl statement was taken.
 * @param vy_stmt Vinyl statement.
 *
 * @retval not NULL Success
 * @retval     NULL Memory or format error.
 */
static inline struct tuple *
vy_convert_replace(struct space *space, const struct vy_stmt *vy_stmt)
{
	assert(vy_stmt->type == IPROTO_REPLACE);
	uint32_t bsize;
	assert(space->index_count > 0);
	const char *data = vy_tuple_data_range(vy_stmt, &bsize);
	return box_tuple_new(space->format, data, data + bsize);
}

/* }}} Statements */

/** {{{ Upsert */

static void *
vy_update_alloc(void *arg, size_t size)
{
	/* TODO: rewrite tuple_upsert_execute() without exceptions */
	struct region *region = (struct region *) arg;
	void *data = region_aligned_alloc(region, size, sizeof(uint64_t));
	if (data == NULL)
		diag_set(OutOfMemory, sizeof(struct vy_tx), "region",
			 "upsert");
	return data;
}

/**
 * vinyl wrapper of tuple_upsert_execute.
 * vibyl upsert opts are slightly different from tarantool ops,
 *  so they need some preparation before tuple_upsert_execute call.
 *  The function does this preparation.
 * On successfull upsert the result is placed into stmt and stmt_end args.
 * On fail the stmt and stmt_end args are not changed.
 * Possibly allocates new stmt via fiber region alloc,
 * so call fiber_gc() after usage
 */
static void
vy_apply_upsert_ops(struct region *region, const char **stmt,
		    const char **stmt_end, const char *ops,
		    const char *ops_end, bool suppress_error)
{
	if (ops == ops_end)
		return;

#ifndef NDEBUG
			const char *serie_end_must_be = ops;
			mp_next(&serie_end_must_be);
			assert(ops_end == serie_end_must_be);
#endif
		const char *result;
		uint32_t size;
		result = tuple_upsert_execute(vy_update_alloc, region,
					      ops, ops_end,
					      *stmt, *stmt_end,
					      &size, 0, suppress_error, NULL);
		if (result != NULL) {
			/* if failed, just skip it and leave stmt the same */
			*stmt = result;
			*stmt_end = result + size;
		}
}

const char *
space_name_by_id(uint32_t id);

/**
 * Try to squash two upsert series (msgspacked index_base + ops)
 * Try to create a tuple with squahed operations
 *
 * @retval 0 && *result_stmt != NULL : successful squash
 * @retval 0 && *result_stmt == NULL : unsquashable sources
 * @retval -1 - memory error
 */
static int
vy_upsert_try_to_squash(const struct tuple_format *format,
			uint32_t part_count,
			struct region *region,
			const char *key_mp, const char *key_mp_end,
			const char *old_serie, const char *old_serie_end,
			const char *new_serie, const char *new_serie_end,
			struct vy_stmt **result_stmt)
{
	*result_stmt = NULL;

	size_t squashed_size;
	const char *squashed =
		tuple_upsert_squash(vy_update_alloc, region,
				    old_serie, old_serie_end,
				    new_serie, new_serie_end,
				    &squashed_size, 0);
	if (squashed == NULL)
		return 0;
	/* Successful squash! */
	struct iovec operations[1];
	operations[0].iov_base = (void *)squashed;
	operations[0].iov_len = squashed_size;

	*result_stmt = vy_stmt_new_upsert(key_mp, key_mp_end, format,
					  part_count, operations, 1);
	if (*result_stmt == NULL)
		return -1;
	return 0;
}

static struct vy_stmt *
vy_apply_upsert(const struct vy_stmt *new_stmt, const struct vy_stmt *old_stmt,
		const struct key_def *key_def,
		const struct tuple_format *format, bool suppress_error)
{
	/*
	 * old_stmt - previous (old) version of stmt
	 * new_stmt - next (new) version of stmt
	 * result_stmt - the result of merging new and old
	 */
	assert(new_stmt != NULL);
	assert(new_stmt != old_stmt);
	assert(new_stmt->type == IPROTO_UPSERT);

	if (old_stmt == NULL || old_stmt->type == IPROTO_DELETE) {
		/*
		 * INSERT case: return new stmt.
		 */
		return vy_stmt_replace_from_upsert(new_stmt);
	}

	/*
	 * Unpack UPSERT operation from the new stmt
	 */
	uint32_t mp_size;
	const char *new_ops;
	new_ops = vy_stmt_upsert_ops(new_stmt, &mp_size);
	const char *new_ops_end = new_ops + mp_size;

	/*
	 * Apply new operations to the old stmt
	 */
	const char *result_mp;
	result_mp = vy_tuple_data_range(old_stmt, &mp_size);
	const char *result_mp_end = result_mp + mp_size;
	struct vy_stmt *result_stmt = NULL;
	struct region *region = &fiber()->gc;
	size_t region_svp = region_used(region);
	uint8_t old_type = old_stmt->type;
	vy_apply_upsert_ops(region, &result_mp, &result_mp_end, new_ops,
			    new_ops_end, suppress_error);
	if (old_type != IPROTO_UPSERT) {
		assert(old_type == IPROTO_DELETE || old_type == IPROTO_REPLACE);
		/*
		 * UPDATE case: return the updated old stmt.
		 */
		result_stmt = vy_stmt_new_replace(result_mp, result_mp_end,
						  format, key_def->part_count);
		region_truncate(region, region_svp);
		if (result_stmt == NULL)
			return NULL; /* OOM */
		result_stmt->lsn = new_stmt->lsn;
		goto check_key;
	}

	/*
	 * Unpack UPSERT operation from the old stmt
	 */
	assert(old_stmt != NULL);
	const char *old_ops;
	old_ops = vy_stmt_upsert_ops(old_stmt, &mp_size);
	const char *old_ops_end = old_ops + mp_size;
	assert(old_ops_end > old_ops);

	/*
	 * UPSERT + UPSERT case: combine operations
	 */
	assert(old_ops_end - old_ops > 0);
	if (vy_upsert_try_to_squash(format, key_def->part_count, region,
				    result_mp, result_mp_end,
				    old_ops, old_ops_end,
				    new_ops, new_ops_end,
				    &result_stmt) != 0) {
		region_truncate(region, region_svp);
		return NULL;
	}
	if (result_stmt != NULL) {
		region_truncate(region, region_svp);
		result_stmt->lsn = new_stmt->lsn;
		goto check_key;
	}

	/* Failed to squash, simply add one upsert to another */
	int old_ops_cnt, new_ops_cnt;
	struct iovec operations[3];

	old_ops_cnt = mp_decode_array(&old_ops);
	operations[1].iov_base = (void *)old_ops;
	operations[1].iov_len = old_ops_end - old_ops;

	new_ops_cnt = mp_decode_array(&new_ops);
	operations[2].iov_base = (void *)new_ops;
	operations[2].iov_len = new_ops_end - new_ops;

	char ops_buf[16];
	char *header = mp_encode_array(ops_buf, old_ops_cnt + new_ops_cnt);
	operations[0].iov_base = (void *)ops_buf;
	operations[0].iov_len = header - ops_buf;

	result_stmt = vy_stmt_new_upsert(result_mp, result_mp_end,
					 format, key_def->part_count,
					 operations, 3);
	if (result_stmt == NULL) {
		region_truncate(region, region_svp);
		return NULL;
	}
	region_truncate(region, region_svp);
	result_stmt->lsn = new_stmt->lsn;

check_key:
	/*
	 * Check that key hasn't been changed after applying operations.
	 */
	if (key_def->iid == 0 &&
	    vy_stmt_compare(old_stmt, result_stmt, format, key_def) != 0) {
		/*
		 * Key has been changed: ignore this UPSERT and
		 * @retval the old stmt.
		 */
		diag_set(ClientError, ER_CANT_UPDATE_PRIMARY_KEY,
			 key_def->name, space_name_by_id(key_def->space_id));
		error_log(diag_last_error(diag_get()));
		vy_stmt_unref(result_stmt);
		result_stmt = vy_stmt_dup(old_stmt);
	}
	return result_stmt;
}

/* }}} Upsert */

/**
 * Add the statement to the current transaction.
 * @param tx    Current transaction.
 * @param index Index in whose write_set insert the statement.
 * @param stmt  Statement to set.
 */
static int
vy_tx_set(struct vy_tx *tx, struct vy_index *index, struct vy_stmt *stmt)
{
	assert(stmt->type != 0);

	/* Update concurrent index */
	struct txv *old = write_set_search_key(&tx->write_set, index,
					       stmt);
	/* Found a match of the previous action of this transaction */
	if (old != NULL) {
		if (stmt->type == IPROTO_UPSERT) {
			assert(old->stmt->type == IPROTO_UPSERT ||
			       old->stmt->type == IPROTO_REPLACE ||
			       old->stmt->type == IPROTO_DELETE);

			struct vy_stmt *old_stmt = old->stmt;
			struct vy_stmt *new_stmt = stmt;
			stmt = vy_apply_upsert(new_stmt, old_stmt,
					       index->key_def, index->format,
					       true);
			if (stmt == NULL)
				return -1;
			assert(stmt->type);
		}
		vy_stmt_unref(old->stmt);
		vy_stmt_ref(stmt);
		old->stmt = stmt;
	} else {
		/* Allocate a MVCC container. */
		struct txv *v = txv_new(index, stmt, tx);
		v->is_read = false;
		v->is_gap = false;
		write_set_insert(&tx->write_set, v);
		tx->write_set_version++;
		stailq_add_tail_entry(&tx->log, v, next_in_log);
	}
	return 0;
}

/* {{{ Public API of transaction control: start/end transaction,
 * read, write data in the context of a transaction.
 */

/**
 * Get a vinyl tuple from the index by the key.
 * @param tx          Current transaction.
 * @param index       Index in which search.
 * @param key         MessagePack'ed data, the array without a
 *                    header.
 * @param part_count  Part count of the key.
 * @param[out] result The found tuple is stored here. Must be
 *                    unreferenced after usage.
 *
 * @param  0 Success.
 * @param -1 Memory error or read error.
 */
static inline int
vy_index_get(struct vy_tx *tx, struct vy_index *index, const char *key,
	     uint32_t part_count, struct vy_stmt **result)
{
	/*
	 * tx can be NULL, for example, if an user calls
	 * space.index.get({key}).
	 */
	assert(tx == NULL || tx->state == VINYL_TX_READY);
	struct vy_stmt *vykey;
	assert(part_count <= index->key_def->part_count);
	vykey = vy_stmt_new_select(key, part_count);
	if (vykey == NULL)
		return -1;
	struct vy_env *e = index->env;
	ev_tstamp start  = ev_now(loop());
	int64_t vlsn = INT64_MAX;
	const int64_t *vlsn_ptr = &vlsn;
	if (tx == NULL)
		vlsn = e->xm->lsn;
	else
		vlsn_ptr = &tx->vlsn;

	struct vy_read_iterator itr;
	vy_read_iterator_open(&itr, index, tx, ITER_EQ, vykey, vlsn_ptr, false);
	if (vy_read_iterator_next(&itr, result) != 0)
		goto error;
	if (tx != NULL && vy_tx_track(tx, index, vykey, *result == NULL) != 0) {
		vy_read_iterator_close(&itr);
		goto error;
	}
	vy_stmt_unref(vykey);
	if (*result != NULL)
		vy_stmt_ref(*result);
	vy_read_iterator_close(&itr);
	vy_stat_get(e->stat, start);
	return 0;
error:
	vy_stmt_unref(vykey);
	return -1;
}

/**
 * Check if the index contains the key. If true, then set
 * a duplicate key error in the diagnostics area.
 * @param tx         Current transaction.
 * @param index      Index in which to search.
 * @param key        MessagePack'ed data, the array without a
 *                   header.
 * @param part_count Part count of the key.
 *
 * @retval  0 Success, the key isn't found.
 * @retval -1 Memory error or the key is found.
 */
static inline int
vy_check_dup_key(struct vy_tx *tx, struct vy_index *idx, const char *key,
		 uint32_t part_count)
{
	struct vy_stmt *found;
	(void) part_count;
	/*
	 * Expect a full tuple as input (secondary key || primary key)
	 * but use only  the secondary key fields (partial key look
	 * up) to check for duplicates.
         */
	assert(part_count == idx->key_def->part_count);
	if (vy_index_get(tx, idx, key, idx->user_key_def->part_count, &found))
		return -1;

	if (found) {
		vy_stmt_unref(found);
		diag_set(ClientError, ER_TUPLE_FOUND, idx->user_key_def->name,
			 space_name(idx->space));
		return -1;
	}
	return 0;
}

/**
 * Insert REPLACE into the write set of the transaction.
 * @param tx        Transaction which replaces.
 * @param index     Index in which \p tx replaces.
 * @param tuple     MessagePack array.
 * @param tuple_end End of the tuple.
 *
 * @retval  0 Success.
 * @retval -1 Memory error.
 */
int
vy_index_replace(struct vy_tx *tx, struct vy_index *index,
	   const char *tuple, const char *tuple_end)
{
	assert(tx == NULL || tx->state == VINYL_TX_READY);
	struct vy_stmt *vystmt = vy_stmt_new_replace(tuple, tuple_end,
						     index->format,
						     index->key_def->part_count);
	if (vystmt == NULL)
		return -1;
	assert(vystmt->type == IPROTO_REPLACE);
	int rc = vy_tx_set(tx, index, vystmt);
	vy_stmt_unref(vystmt);
	return rc;
}

/**
 * Insert a tuple in a primary index.
 * @param tx        Current transaction.
 * @param pk        Primary vinyl index.
 * @param tuple     MessagePack array.
 * @param tuple_end End of the tuple.
 *
 * @retval  0 Success.
 * @retval -1 Memory error or duplicate key error.
 */
static inline int
vy_insert_primary(struct vy_tx *tx, struct vy_index *pk,
		     const char *tuple, const char *tuple_end)
{
	const char *key;
	assert(pk->key_def->iid == 0);
	key = tuple_extract_key_raw(tuple, tuple_end, pk->key_def, NULL);
	if (key == NULL)                        /* out of memory */
		return -1;
	/*
	 * A primary index is always unique and the new tuple must not
	 * conflict with existing tuples.
	 */
	uint32_t part_count = mp_decode_array(&key);
	if (vy_check_dup_key(tx, pk, key, part_count))
		return -1;

	return vy_index_replace(tx, pk, tuple, tuple_end);
}

/**
 * Insert a tuple in a secondary index.
 * @param tx        Current transaction.
 * @param index     Secondary index.
 * @param tuple     MessagePack array.
 * @param tuple_end End of the tuple.
 *
 * @retval  0 Success.
 * @retval -1 Memory error or duplicate key error.
 */
int
vy_insert_secondary(struct vy_tx *tx, struct vy_index *index,
		    const char *tuple, const char *tuple_end)
{
	const char *key, *key_end;
	assert(index->key_def->iid > 0);
	uint32_t key_len;
	key = tuple_extract_key_raw(tuple, tuple_end,
				    index->key_def_tuple_to_key, &key_len);
	if (key == NULL)                        /* out of memory */
		return -1;
	key_end = key + key_len;
	/*
	 * If the index is unique then the new tuple must not
	 * conflict with existing tuples. If the index is not
	 * unique a conflict is impossible.
	 */
	if (index->user_key_def->opts.is_unique) {
		const char *check_key = key;
		uint32_t part_count = mp_decode_array(&check_key);
		if (vy_check_dup_key(tx, index, check_key, part_count))
			return -1;
	}
	return vy_index_replace(tx, index, key, key_end);
}

/**
 * Execute REPLACE in a space with a single index, possibly with
 * lookup for an old tuple if the space has at least one
 * on_replace trigger.
 * @param tx      Current transaction.
 * @param space   Space in which replace.
 * @param request Request with the tuple data.
 * @param stmt    Statement for triggers is filled with old
 *                statement.
 *
 * @retval  0 Success.
 * @retval -1 Memory error OR duplicate key error OR the primary
 *            index is not found OR a tuple reference increment
 *            error.
 */
static inline int
vy_replace_one(struct vy_tx *tx, struct space *space,
	       struct request *request, struct txn_stmt *stmt)
{
	assert(space->index_count == 1);
	struct vy_index *pk = vy_index(space->index[0]);
	assert(pk->key_def->iid == 0);
	/**
	 * If the space has triggers, then we need to fetch the
	 * old tuple to pass it to the trigger. Use vy_get to
	 * fetch it.
	 */
	if (stmt != NULL && !rlist_empty(&space->on_replace)) {
		const char *key;
		key = tuple_extract_key_raw(request->tuple, request->tuple_end,
					    pk->key_def, NULL);
		if (key == NULL)                /* out of memory */
			return -1;
		uint32_t part_count = mp_decode_array(&key);
		if (vy_get(tx, pk, key, part_count, &stmt->old_tuple))
			return -1;
		if (stmt->old_tuple)
			box_tuple_ref(stmt->old_tuple);
	}
	return vy_index_replace(tx, pk, request->tuple, request->tuple_end);
}

/**
 * Insert DELETE of the specified key into the write set of the
 * transaction.
 * @param tx         Transaction which deletes.
 * @param index      Index in which \p tx deletes.
 * @param key        MessagePack'ed data, the array without a
 *                   header.
 * @param part_count Count of parts of the key.
 *
 * @retval  0 Success.
 * @retval -1 Memory error.
 */
int
vy_index_delete_key(struct vy_tx *tx, struct vy_index *index,
		    const char *key, uint32_t part_count)
{
	assert(tx == NULL || tx->state == VINYL_TX_READY);
	assert(part_count <= index->key_def->part_count);
	struct vy_stmt *vykey;
	vykey = vy_stmt_new_delete(key, part_count);
	if (vykey == NULL)
		return -1;
	assert(vykey->type == IPROTO_DELETE);
	int rc = vy_tx_set(tx, index, vykey);
	vy_stmt_unref(vykey);
	return rc;
}

/**
 * Execute REPLACE in a space with multiple indexes and lookup for
 * an old tuple, that should has been set in \p stmt->old_tuple if
 * the space has at least one on_replace trigger.
 * @param tx      Current transaction.
 * @param space   Vinyl space.
 * @param request Request with the tuple data.
 * @param stmt    Statement for triggers filled with old
 *                statement.
 *
 * @retval  0 Success
 * @retval -1 Memory error OR duplicate key error OR the primary
 *            index is not found OR a tuple reference increment
 *            error.
 */
static inline int
vy_replace_impl(struct vy_tx *tx, struct space *space, struct request *request,
		struct txn_stmt *stmt)
{
	struct vy_stmt *old_stmt;
	struct vy_index *pk = vy_index_find(space, 0);
	if (pk == NULL) /* space has no primary key */
		return -1;
	assert(pk->key_def->iid == 0);
	const char *key = NULL, *old_tuple = NULL, *old_tuple_end = NULL;
	key = tuple_extract_key_raw(request->tuple, request->tuple_end,
				    pk->key_def, NULL);
	if (key == NULL) /* out of memory */
		return -1;
	uint32_t part_count = mp_decode_array(&key);

	/* Get full tuple from the primary index. */
	if (vy_index_get(tx, pk, key, part_count, &old_stmt))
		return -1;
	if (old_stmt != NULL) {
		uint32_t mp_size;
		old_tuple = vy_tuple_data_range(old_stmt, &mp_size);
		old_tuple_end = old_tuple + mp_size;
	}
	/*
	 * Replace in the primary index without explicit deletion
	 * of the old tuple.
	 */
	if (vy_index_replace(tx, pk, request->tuple, request->tuple_end))
		goto error;

	/* Update secondary keys, avoid duplicates. */
	for (uint32_t iid = 1; iid < space->index_count; ++iid) {
		struct vy_index *index;
		index = vy_index(space->index[iid]);
		/*
		 * Delete goes first, so if old and new keys
		 * fully match, there is no look up beyond the
		 * transaction index.
		 */
		if (old_stmt != NULL) {
			key = tuple_extract_key_raw(old_tuple, old_tuple_end,
						    index->key_def_tuple_to_key,
						    NULL);
			if (key == NULL)
				goto error;
			part_count = mp_decode_array(&key);
			if (vy_index_delete_key(tx, index, key, part_count))
				goto error;
		}
		if (vy_insert_secondary(tx, index, request->tuple,
					   request->tuple_end))
			goto error;
	}
	/*
	 * The old tuple is used if there is an on_replace
	 * trigger.
	 */
	if (stmt != NULL) {
		if (old_stmt == NULL)
			return 0;
		stmt->old_tuple = vy_convert_replace(space, old_stmt);
		box_tuple_ref(stmt->old_tuple);
	}
	if (old_stmt != NULL)
		vy_stmt_unref(old_stmt);
	return 0;
error:
	if (old_stmt != NULL)
		vy_stmt_unref(old_stmt);
	return -1;
}

/**
 * Check that the key can be used for search in a unique index.
 * @param  index      Index for checking.
 * @param  key        MessagePack'ed data, the array without a
 *                    header.
 * @param  part_count Part count of the key.
 *
 * @retval  0 The key is valid.
 * @retval -1 The key is not valid, the appropriate error is set
 *            in the diagnostics area.
 */
static inline int
vy_unique_key_validate(struct vy_index *index, const char *key,
		       uint32_t part_count)
{
	struct key_def *def = index->key_def;
	assert(def->opts.is_unique);
	assert(key != NULL || part_count == 0);
	/*
	 * The index contains tuples with concatenation of
	 * secondary and primary key fields, while the key
	 * supplied by the user only contains the secondary key
	 * fields. Use the correct key def to validate the key.
	 * The key can be used to look up in the index since the
	 * supplied key parts uniquely identify the tuple, as long
	 * as the index is unique.
	 */
	uint32_t original_part_count = index->user_key_def->part_count;
	if (original_part_count != part_count) {
		diag_set(ClientError, ER_EXACT_MATCH,
			 original_part_count, part_count);
		return -1;
	}
	return key_validate_parts(def, key, part_count);
}

/**
 * Get a tuple from the primary index by the partial tuple from
 * the secondary index.
 * @param tx        Current transaction.
 * @param index     Secondary index.
 * @param partial   Partial tuple from the secondary \p index.
 * @param[out] full The full tuple is stored here. Must be
 *                  unreferenced after usage.
 *
 * @retval  0 Success.
 * @retval -1 Memory error.
 */
static inline int
vy_index_full_by_stmt(struct vy_tx *tx, struct vy_index *index,
		      const struct vy_stmt *partial, struct vy_stmt **full)
{
	assert(index->key_def->iid > 0);
	/*
	 * Fetch the primary key from the secondary index tuple.
	 */
	struct key_def *to_pk = index->key_def_secondary_to_primary;
	uint32_t size;
	const char *tuple = vy_tuple_data_range(partial, &size);
	const char *tuple_end = tuple + size;
	const char *pkey = tuple_extract_key_raw(tuple, tuple_end, to_pk, NULL);
	if (pkey == NULL)
		return -1;
	/* Fetch the tuple from the primary index. */
	uint32_t part_count = mp_decode_array(&pkey);
	assert(part_count == to_pk->part_count);
	struct space *space = index->space;
	struct vy_index *pk = vy_index_find(space, 0);
	assert(pk != NULL);
	return vy_index_get(tx, pk, pkey, part_count, full);
}

/**
 * Find a tuple in the primary index by the key of the specified
 * index.
 * @param tx          Current transaction.
 * @param index       Index for which the key is specified. Can be
 *                    both primary and secondary.
 * @param key         MessagePack'ed data, the array without a
 *                    header.
 * @param part_count  Count of parts in the key.
 * @param[out] result The found statement is stored here. Must be
 *                    unreferenced after usage.
 *
 * @retval  0 Success.
 * @retval -1 Memory error.
 */
static inline int
vy_index_full_by_key(struct vy_tx *tx, struct vy_index *index, const char *key,
		     uint32_t part_count, struct vy_stmt **result)
{
	struct vy_stmt *found;
	if (vy_index_get(tx, index, key, part_count, &found))
		return -1;
	if (index->key_def->iid == 0 || found == NULL) {
		*result = found;
		return 0;
	}
	int rc = vy_index_full_by_stmt(tx, index, found, result);
	vy_stmt_unref(found);
	return rc;
}

/**
 * Delete the tuple from all indexes of the vinyl space.
 * @param tx         Current transaction.
 * @param space      Vinyl space.
 * @param tuple      MessagePack array, tuple to delete.
 * @param tuple_end  End of the tuple.
 * @param key        Primary key. This is an optimization, to
 *                   avoid extracting the primary key from the
 *                   \p tuple in the most common case when it's
 *                   already supplied by the user.
 * @param part_count Part count of the key.
 *
 * @retval  0 Success
 * @retval -1 Memory error or the index is not found.
 */
static inline int
vy_delete_impl(struct vy_tx *tx, struct space *space,
	       const char *tuple, const char *tuple_end,
	       const char *key, uint32_t part_count)
{
	struct vy_index *pk = vy_index_find(space, 0);
	if (pk == NULL)
		return -1;
	if (vy_index_delete_key(tx, pk, key, part_count))
		return -1;

	/* At second, delete from seconary indexes. */
	struct vy_index *index;
	for (uint32_t i = 1; i < space->index_count; ++i) {
		index = vy_index(space->index[i]);
		key = tuple_extract_key_raw(tuple, tuple_end,
					    index->key_def_tuple_to_key, NULL);
		if (key == NULL)
			return -1;
		part_count = mp_decode_array(&key);
		if (vy_index_delete_key(tx, index, key, part_count))
			return -1;
	}
	return 0;
}

int
vy_delete(struct vy_tx *tx, struct txn_stmt *stmt, struct space *space,
	  struct request *request)
{
	struct vy_index *pk = vy_index_find(space, 0);
	if (pk == NULL)
		return -1;
	struct vy_index *index = vy_index_find_unique(space, request->index_id);
	if (index == NULL)
		return -1;
	struct vy_stmt *old_stmt = NULL;
	bool has_secondary = space->index_count > 1;
	const char *key = request->key;
	uint32_t part_count = mp_decode_array(&key);
	if (vy_unique_key_validate(index, key, part_count))
		return -1;
	/*
	 * There are two cases when need to get the full tuple
	 * before deletion.
	 * - if the space has on_replace triggers and need to pass
	 *   to them the old tuple.
	 *
	 * - if the space has one or more secondary indexes, then
	 *   we need to extract secondary keys from the old tuple
	 *   and pass them to indexes for deletion.
	 */
	if (has_secondary || !rlist_empty(&space->on_replace)) {
		if (vy_index_full_by_key(tx, index, key, part_count, &old_stmt))
			return -1;
		if (old_stmt == NULL)
			return 0;
	}
	if (has_secondary) {
		assert(old_stmt != NULL);
		/*
		 * If the space has secondary indexes and
		 * the old tuple exists then delete it
		 * from all the indexes.
		 */
		uint32_t size;
		const char *tuple;
		tuple = vy_tuple_data_range(old_stmt, &size);
		const char *tuple_end = tuple + size;
		if (request->index_id != 0) {
			/*
			 * If the specified index is not
			 * primary then extract the
			 * primary key from the tuple,
			 * else the primary key already is
			 * passed as parameter.
			 */
			key = tuple_extract_key_raw(tuple, tuple_end,
						    pk->key_def, NULL);
			if (key == NULL)
				return -1;
			part_count = mp_decode_array(&key);
		}
		if (vy_delete_impl(tx, space, tuple, tuple_end, key,
				   part_count))
			goto error;
	} else { /* Primary is the single index in the space. */
		assert(index->key_def->iid == 0);
		if (vy_index_delete_key(tx, pk, key, part_count))
			goto error;
		if (old_stmt == NULL)
			return 0;               /* nothing else to do */
	}
	stmt->old_tuple = vy_convert_replace(space, old_stmt);
	if (stmt->old_tuple == NULL)
		goto error;
	vy_stmt_unref(old_stmt);
	return box_tuple_ref(stmt->old_tuple);
error:
	if (old_stmt != NULL)
		vy_stmt_unref(old_stmt);
	return -1;
}

/**
 * We do not allow changes of the primary key during update.
 *
 * The syntax of update operation allows the user to update the
 * primary key of a tuple, which is prohibited, to avoid funny
 * effects during replication.
 *
 * @param pk         Primary index.
 * @param index_name Name of the index which was updated - it may
 *                   be not the primary index.
 * @param old_tuple  The tuple before update.
 * @param new_tuple  The tuple after update.
 *
 * @retval  0 Success, the primary key is not modified in the new
 *            tuple.
 * @retval -1 Attempt to modify the primary key.
 */
static inline int
vy_check_update(const struct vy_index *pk, const struct vy_stmt *old_tuple,
		const struct vy_stmt *new_tuple)
{
	if (vy_tuple_compare(old_tuple, new_tuple, pk->format, pk->key_def)) {
		diag_set(ClientError, ER_CANT_UPDATE_PRIMARY_KEY,
			 pk->key_def->name,
			 space_name_by_id(pk->key_def->space_id));
		return -1;
	}
	return 0;
}

/**
 * Don't modify indexes whose fields were not changed by update.
 * If there is at least one bit in the column mask
 * (@sa update_read_ops in tuple_update.cc) set that corresponds
 * to one of the columns from key_def->parts, then the update
 * operation changes at least one indexed field and the
 * optimization is inapplicable. Otherwise, we can skip the
 * update.
 * @param idx         Secondary index which we try to update.
 * @param column_mask Maks of the update operations.
 */
static bool
vy_can_skip_update(const struct vy_index *idx, uint64_t column_mask)
{
	/*
	 * Update of the primary index can't be skipped, since it
	 * stores not indexes tuple fields besides indexed.
	 */
	assert(idx->key_def->iid > 0);
	return (column_mask & idx->column_mask) == 0;
}

/* Callback for tuple_update_execute(). */
static void *
region_aligned_alloc_cb(void *ctx, size_t size)
{
	void *ptr = region_aligned_alloc((struct region *) ctx, size,
					 alignof(uint64_t));
	if (ptr == NULL)
		diag_set(OutOfMemory, size, "region", "new slab");
	return ptr;
}

int
vy_update(struct vy_tx *tx, struct txn_stmt *stmt, struct space *space,
	  struct request *request)
{
	assert(tx != NULL && tx->state == VINYL_TX_READY);
	struct vy_index *index = vy_index_find_unique(space, request->index_id);
	if (index == NULL)
		return -1;
	const char *key = request->key;
	uint32_t part_count = mp_decode_array(&key);
	if (vy_unique_key_validate(index, key, part_count))
		return -1;

	struct vy_stmt *old_stmt;
	if (vy_index_full_by_key(tx, index, key, part_count, &old_stmt))
		return -1;
	/* Nothing to update. */
	if (old_stmt == NULL)
		return 0;

	/* Apply update operations. */
	struct vy_index *pk = vy_index(space->index[0]);
	assert(pk != NULL);
	assert(pk->key_def->iid == 0);
	uint64_t column_mask = 0;
	const char *new_tuple, *new_tuple_end;
	uint32_t new_size, old_size;
	const char *old_tuple = vy_tuple_data_range(old_stmt, &old_size);
	const char *old_tuple_end = old_tuple + old_size;
	new_tuple = tuple_update_execute(region_aligned_alloc_cb, &fiber()->gc,
					 request->tuple, request->tuple_end,
					 old_tuple, old_tuple_end, &new_size,
					 request->index_base, &column_mask);
	if (new_tuple == NULL)
		goto error;
	new_tuple_end = new_tuple + new_size;
	/*
	 * Check that the new tuple matches the space format and
	 * the primary key was not modified.
	 */
	if (tuple_validate_raw(space->format, new_tuple))
		goto error;
	struct vy_stmt *new_stmt;
	new_stmt = vy_stmt_new_replace(new_tuple, new_tuple_end, pk->format,
				       pk->key_def->part_count);
	if (new_stmt == NULL)
		goto error;
	if (vy_check_update(pk, old_stmt, new_stmt)) {
		vy_stmt_unref(new_stmt);
		goto error;
	}

	/*
	 * In the primary index the tuple can be replaced without
	 * the old tuple deletion.
	 */
	int rc = vy_tx_set(tx, pk, new_stmt);
	vy_stmt_unref(new_stmt);
	if (rc != 0)
		goto error;

	for (uint32_t i = 1; i < space->index_count; ++i) {
		index = vy_index(space->index[i]);
		if (vy_can_skip_update(index, column_mask))
			continue;
		key = tuple_extract_key_raw(old_tuple, old_tuple_end,
					    index->key_def_tuple_to_key, NULL);
		if (key == NULL)
			goto error;
		part_count = mp_decode_array(&key);
		if (vy_index_delete_key(tx, index, key, part_count))
			goto error;
		if (vy_insert_secondary(tx, index, new_tuple, new_tuple_end))
			goto error;
	}
	stmt->old_tuple = vy_convert_replace(space, old_stmt);
	vy_stmt_unref(old_stmt);
	if (stmt->old_tuple == NULL)
		return -1;
	if (box_tuple_ref(stmt->old_tuple))
		return -1;
	stmt->new_tuple = box_tuple_new(space->format, new_tuple,
					new_tuple_end);
	if (stmt->new_tuple == NULL)
		return -1;
	return box_tuple_ref(stmt->new_tuple);
error:
	assert(old_stmt != NULL);
	vy_stmt_unref(old_stmt);
	return -1;
}

/**
 * Insert the tuple in the space without checking duplicates in
 * the primary index.
 * @param tx        Current transaction.
 * @param space     Space in which insert.
 * @param tuple     MessagePack array.
 * @param tuple_end End of the tuple.
 *
 * @retval  0 Success.
 * @retval -1 Memory error or a secondary index duplicate error.
 */
static int
vy_insert_first_upsert(struct vy_tx *tx, struct space *space,
		       const char *tuple, const char *tuple_end)
{
	assert(space->index_count > 0);
	struct vy_index *pk = vy_index(space->index[0]);
	assert(pk->key_def->iid == 0);
	if (vy_index_replace(tx, pk, tuple, tuple_end))
		return -1;
	struct vy_index *index;
	for (uint32_t i = 1; i < space->index_count; ++i) {
		index = vy_index(space->index[i]);
		if (vy_insert_secondary(tx, index, tuple, tuple_end))
			return -1;
	}
	return 0;
}

/**
 * Insert UPSERT into the write set of the transaction.
 * @param tx        Transaction which deletes.
 * @param index     Index in which \p tx deletes.
 * @param tuple     MessagePack array.
 * @param tuple_end End of the tuple.
 * @param expr      MessagePack array of update operations.
 * @param expr_end  End of the \p expr.
 *
 * @retval  0 Success.
 * @retval -1 Memory error.
 */
int
vy_index_upsert(struct vy_tx *tx, struct vy_index *index,
	  const char *tuple, const char *tuple_end,
	  const char *expr, const char *expr_end)
{
	assert(tx == NULL || tx->state == VINYL_TX_READY);
	struct vy_stmt *vystmt;
	struct iovec operations[1];
	operations[0].iov_base = (void *)expr;
	operations[0].iov_len = expr_end - expr;
	vystmt = vy_stmt_new_upsert(tuple, tuple_end, index->format,
				    index->key_def->part_count, operations, 1);
	if (vystmt == NULL)
		return -1;
	assert(vystmt->type == IPROTO_UPSERT);
	int rc = vy_tx_set(tx, index, vystmt);
	vy_stmt_unref(vystmt);
	return rc;
}

int
vy_upsert(struct vy_tx *tx, struct txn_stmt *stmt, struct space *space,
	  struct request *request)
{
	assert(tx != NULL && tx->state == VINYL_TX_READY);
	const char *tuple = request->tuple;
	const char *tuple_end = request->tuple_end;
	const char *ops = request->ops;
	const char *ops_end = request->ops_end;
	bool has_secondary = space->index_count > 1;
	bool has_triggers = !rlist_empty(&space->on_replace);
	struct vy_index *pk = vy_index_find(space, 0);
	if (pk == NULL)
		return -1;
	if (tuple_validate_raw(space->format, tuple))
		return -1;

	if (has_secondary == false && has_triggers == false)
		return vy_index_upsert(tx, pk, tuple, tuple_end, ops, ops_end);

	const char *old_tuple, *old_tuple_end;
	const char *new_tuple, *new_tuple_end;
	uint32_t new_size;
	const char *key;
	uint32_t part_count;
	struct vy_stmt *old_stmt = NULL;
	struct key_def *pk_def = pk->key_def;
	uint64_t column_mask;
	/*
	 * There are two cases when need to get the old tuple
	 * before upsert:
	 * - if the space has one or more on_repace triggers;
	 *
	 * - if the space has one or more secondary indexes: then
	 *   we need to extract secondary keys from the old tuple
	 *   to delete old tuples from secondary indexes.
	 */
	/* Find the old tuple using the primary key. */
	key = tuple_extract_key_raw(tuple, tuple_end, pk_def, NULL);
	if (key == NULL)
		return -1;
	part_count = mp_decode_array(&key);
	if (vy_index_get(tx, pk, key, part_count, &old_stmt))
		return -1;
	/*
	 * If the old tuple was not found then UPSERT
	 * turns into INSERT.
	 */
	if (old_stmt == NULL) {
		if (vy_insert_first_upsert(tx, space, tuple, tuple_end))
			return -1;
		if (has_triggers) {
			stmt->new_tuple = box_tuple_new(space->format,
							tuple, tuple_end);
			if (stmt->new_tuple == NULL)
				return -1;
			box_tuple_ref(stmt->new_tuple);
		}
		return 0;
	}
	uint32_t old_size;
	old_tuple = vy_tuple_data_range(old_stmt, &old_size);
	old_tuple_end = old_tuple + old_size;
	new_tuple = tuple_upsert_execute(region_aligned_alloc_cb,
					 &fiber()->gc, ops, ops_end,
					 old_tuple, old_tuple_end,
					 &new_size, 0, false, &column_mask);
	if (new_tuple == NULL)
		goto error;
	new_tuple_end = new_tuple + new_size;
	if (has_triggers) {
		stmt->old_tuple = box_tuple_new(space->format,
						old_tuple, old_tuple_end);
		if (stmt->old_tuple == NULL)
			goto error;
		box_tuple_ref(stmt->old_tuple);
		stmt->new_tuple = box_tuple_new(space->format,
						new_tuple, new_tuple_end);
		if (stmt->new_tuple == NULL)
			goto error;
		box_tuple_ref(stmt->new_tuple);
	}
	/* Apply upsert operations to the old tuple. */
	/*
	 * Check that the new tuple matched the space
	 * format and the primary key was not modified.
	 */
	if (tuple_validate_raw(space->format, new_tuple))
		goto error;

	/* Have to create a vinyl stmt just for vy_check_update() */
	struct vy_stmt *new_stmt;
	new_stmt = vy_stmt_new_replace(new_tuple, new_tuple_end,
				       pk->format, pk_def->part_count);
	if (new_stmt == NULL)
		goto error;

	if (vy_check_update(pk, old_stmt, new_stmt)) {
		error_log(diag_last_error(diag_get()));
		vy_stmt_unref(new_stmt);
		vy_stmt_unref(old_stmt);
		/*
		 * Upsert is skipped, to match the semantics of
		 * vy_index_upsert().
		 */
		return 0;
	}
	vy_stmt_unref(new_stmt);
	/* We know the new tuple, convert upsert to replace. */
	if (vy_index_replace(tx, pk, new_tuple, new_tuple_end))
		goto error;
	/* Replace in secondary indexes works as delete insert. */
	struct vy_index *index;
	for (uint32_t i = 1; i < space->index_count; ++i) {
		index = vy_index(space->index[i]);
		if (vy_can_skip_update(index, column_mask))
			continue;
		key = tuple_extract_key_raw(old_tuple, old_tuple_end,
					    index->key_def_tuple_to_key,
					    NULL);
		if (key == NULL)
			goto error;
		part_count = mp_decode_array(&key);
		if (vy_index_delete_key(tx, index, key, part_count) != 0)
			goto error;
		if (vy_insert_secondary(tx, index, new_tuple,
					new_tuple_end) != 0)
			goto error;
	}
	vy_stmt_unref(old_stmt);
	return 0;
error:
	vy_stmt_unref(old_stmt);
	return -1;
}

int
vy_replace(struct vy_tx *tx, struct txn_stmt *stmt, struct space *space,
	   struct request *request)
{
	/* Check the tuple fields. */
	if (tuple_validate_raw(space->format, request->tuple))
		return -1;
	if (space->index_count == 1) {
		/* Replace in a space with a single index. */
		return vy_replace_one(tx, space, request, stmt);
	} else {
		/* Replace in a space with secondary indexes. */
		return vy_replace_impl(tx, space, request, stmt);
	}
}

int
vy_insert(struct vy_tx *tx, struct space *space, struct request *request)
{
	/* Check the tuple fields. */
	if (tuple_validate_raw(space->format, request->tuple))
		return -1;
	/* First insert into the primary index. */
	struct vy_index *pk = vy_index_find(space, 0);
	if (pk == NULL)                         /* space has no primary key */
		return -1;
	assert(pk->key_def->iid == 0);
	if (vy_insert_primary(tx, pk, request->tuple, request->tuple_end))
		return -1;

	for (uint32_t iid = 1; iid < space->index_count; ++iid) {
		struct vy_index *index = vy_index(space->index[iid]);
		if (vy_insert_secondary(tx, index, request->tuple,
					request->tuple_end))
			return -1;
	}
	return 0;
}

void
vy_rollback(struct vy_env *e, struct vy_tx *tx)
{
	vy_tx_rollback(e, tx);
	TRASH(tx);
	free(tx);
}

int
vy_prepare(struct vy_env *e, struct vy_tx *tx)
{
	/* prepare transaction */
	assert(tx->state == VINYL_TX_READY);
	int rc = 0;

	/* proceed read-only transactions */
	if (!vy_tx_is_ro(tx) && tx->is_aborted) {
		tx->state = VINYL_TX_ROLLBACK;
		e->stat->tx_conflict++;
		diag_set(ClientError, ER_TRANSACTION_CONFLICT);
		rc = -1;
	} else {
		tx->state = VINYL_TX_COMMIT;
		/** Abort read/write intersection */
		struct txv *v = write_set_first(&tx->write_set);
		for (; v != NULL; v = write_set_next(&tx->write_set, v))
			txv_abort_all(e, tx, v);
	}

	tx_manager_end(tx->manager, tx);

	/*
	 * A half committed transaction is no longer
	 * being part of concurrent index, but still can be
	 * committed or rolled back.
	 * Yet, it is important to maintain external
	 * serial commit order.
	 */
	return rc;
}

int
vy_commit(struct vy_env *e, struct vy_tx *tx, int64_t lsn)
{
	assert(tx->state == VINYL_TX_COMMIT);
	if (lsn > e->xm->lsn)
		e->xm->lsn = lsn;

	struct txv *v, *tmp;
	struct vy_quota *quota = &e->quota;
	struct lsregion *allocator = &e->allocator;
	size_t mem_used_before = lsregion_used(allocator);
	/*
	 * Flush transactional changes to the index.
	 * Sic: the loop below must not yield after recovery.
	 */
	uint64_t write_count = 0;
	for (v = write_set_first(&tx->write_set);
	     v != NULL; v = write_set_next(&tx->write_set, v)) {
		int rc = vy_tx_write(v, e->status, lsn);
		write_count++;
		assert(rc == 0); /* TODO: handle BPS tree errors properly */
		(void)rc;
	}

	uint32_t count = 0;
	stailq_foreach_entry_safe(v, tmp, &tx->log, next_in_log) {
		count++;
		txv_delete(v);
	}
	size_t mem_used_after = lsregion_used(allocator);
	assert(mem_used_after >= mem_used_before);
	size_t write_size = mem_used_after - mem_used_before;
	vy_stat_tx(e->stat, tx->start, count, write_count, write_size);

	TRASH(tx);
	free(tx);

	vy_quota_use(quota, write_size);
	return 0;
}

struct vy_tx *
vy_begin(struct vy_env *e)
{
	struct vy_tx *tx;
	tx = malloc(sizeof(struct vy_tx));
	if (unlikely(tx == NULL)) {
		diag_set(OutOfMemory, sizeof(struct vy_tx), "malloc",
			 "struct vy_tx");
		return NULL;
	}
	vy_tx_begin(e->xm, tx, VINYL_TX_RW);
	return tx;
}

void *
vy_savepoint(struct vy_tx *tx)
{
	return stailq_last(&tx->log);
}

void
vy_rollback_to_savepoint(struct vy_tx *tx, void *svp)
{
	struct stailq_entry *last = svp;
	/* Start from the first statement after the savepoint. */
	last = last == NULL ? stailq_first(&tx->log) : stailq_next(last);
	if (last == NULL) {
		/* Empty transaction or no changes after the savepoint. */
		return;
	}
	struct stailq tail;
	stailq_create(&tail);
	stailq_splice(&tx->log, last, &tail);
	struct txv *v, *tmp;
	stailq_foreach_entry_safe(v, tmp, &tail, next_in_log) {
		/* Remove from the conflict manager index */
		if (v->is_read)
			read_set_remove(&v->index->read_set, v);
		/* Remove from the transaction write log. */
		if (!v->is_read) {
			write_set_remove(&tx->write_set, v);
			tx->write_set_version++;
		}
		txv_delete(v);
	}
}

/* }}} Public API of transaction control */

int
vy_get(struct vy_tx *tx, struct vy_index *index, const char *key,
       uint32_t part_count, struct tuple **result)
{
	assert(tx == NULL || tx->state == VINYL_TX_READY);
	assert(result != NULL);
	struct vy_stmt *vyresult = NULL;
	assert(part_count <= index->key_def->part_count);
	if (vy_index_full_by_key(tx, index, key, part_count, &vyresult))
		return -1;
	if (vyresult == NULL)
		return 0;
	MAYBE_UNUSED struct vy_index *pk = vy_index_find(index->space, 0);
	assert(pk != NULL);
	*result = vy_convert_replace(index->space, vyresult);
	vy_stmt_unref(vyresult);
	return *result == NULL ? -1 : 0;
}


/** {{{ Environment */

static void
vy_env_quota_timer_cb(ev_loop *loop, ev_timer *timer, int events)
{
	(void)loop;
	(void)events;
	struct vy_env *e = timer->data;

	int64_t tx_write_rate = vy_stat_tx_write_rate(e->stat);
	int64_t dump_bandwidth = vy_stat_dump_bandwidth(e->stat);

	size_t max_range_size = 0;
	struct heap_iterator it;
	vy_dump_heap_iterator_init(&e->scheduler->dump_heap, &it);
	struct heap_node *pn = vy_dump_heap_iterator_next(&it);
	if (pn != NULL) {
		struct vy_range *range = container_of(pn, struct vy_range,
						      in_dump);
		max_range_size = range->used;
	}

	vy_quota_update_watermark(&e->quota, max_range_size,
				  tx_write_rate, dump_bandwidth);
}

/** Destructor for env->zdctx_key thread-local variable */
static void
vy_free_zdctx(void *arg)
{
	assert(arg != NULL);
	ZSTD_freeDStream(arg);
}

static struct vy_squash_queue *
vy_squash_queue_new(void);
static void
vy_squash_queue_delete(struct vy_squash_queue *q);

struct vy_env *
vy_env_new(void)
{
	struct vy_env *e = malloc(sizeof(*e));
	if (unlikely(e == NULL)) {
		diag_set(OutOfMemory, sizeof(*e), "malloc", "struct vy_env");
		return NULL;
	}
	memset(e, 0, sizeof(*e));
	rlist_create(&e->indexes);
	e->status = VINYL_OFFLINE;
	e->conf = vy_conf_new();
	if (e->conf == NULL)
		goto error_conf;
	e->xm = tx_manager_new(e);
	if (e->xm == NULL)
		goto error_xm;
	e->stat = vy_stat_new();
	if (e->stat == NULL)
		goto error_stat;
	e->scheduler = vy_scheduler_new(e);
	if (e->scheduler == NULL)
		goto error_sched;
	e->squash_queue = vy_squash_queue_new();
	if (e->squash_queue == NULL)
		goto error_squash_queue;

	struct slab_cache *slab_cache = cord_slab_cache();
	mempool_create(&e->cursor_pool, slab_cache,
	               sizeof(struct vy_cursor));
	mempool_create(&e->read_task_pool, slab_cache,
		       sizeof(struct vy_page_read_task));
	lsregion_create(&e->allocator, slab_cache->arena);
	tt_pthread_key_create(&e->zdctx_key, vy_free_zdctx);

	vy_quota_init(&e->quota, e->conf->memory_limit,
		      vy_scheduler_quota_cb, e->scheduler);
	ev_timer_init(&e->quota_timer, vy_env_quota_timer_cb, 0, 1.);
	e->quota_timer.data = e;
	ev_timer_start(loop(), &e->quota_timer);
	return e;
error_squash_queue:
	vy_scheduler_delete(e->scheduler);
error_sched:
	vy_stat_delete(e->stat);
error_stat:
	tx_manager_delete(e->xm);
error_xm:
	vy_conf_delete(e->conf);
error_conf:
	free(e);
	return NULL;
}

void
vy_env_delete(struct vy_env *e)
{
	ev_timer_stop(loop(), &e->quota_timer);
	vy_squash_queue_delete(e->squash_queue);
	vy_scheduler_delete(e->scheduler);
	/* TODO: tarantool doesn't delete indexes during shutdown */
	//assert(rlist_empty(&e->db));
	tx_manager_delete(e->xm);
	vy_conf_delete(e->conf);
	vy_stat_delete(e->stat);
	mempool_destroy(&e->cursor_pool);
	mempool_destroy(&e->read_task_pool);
	lsregion_destroy(&e->allocator);
	tt_pthread_key_delete(e->zdctx_key);
	free(e);
}

/** }}} Environment */

/** {{{ Recovery */

void
vy_bootstrap(struct vy_env *e)
{
	assert(e->status == VINYL_OFFLINE);
	e->status = VINYL_ONLINE;
}

void
vy_begin_initial_recovery(struct vy_env *e, struct vclock *vclock)
{
	assert(e->status == VINYL_OFFLINE);
	if (vclock) {
		e->xm->lsn = vclock_sum(vclock);
		e->status = VINYL_INITIAL_RECOVERY_LOCAL;
	} else {
		e->xm->lsn = 0;
		e->status = VINYL_INITIAL_RECOVERY_REMOTE;
	}
}

void
vy_begin_final_recovery(struct vy_env *e)
{
	switch (e->status) {
	case VINYL_INITIAL_RECOVERY_LOCAL:
		e->status = VINYL_FINAL_RECOVERY_LOCAL;
		break;
	case VINYL_INITIAL_RECOVERY_REMOTE:
		e->status = VINYL_FINAL_RECOVERY_REMOTE;
		break;
	default:
		unreachable();
	}
}

void
vy_end_recovery(struct vy_env *e)
{
	assert(e->status == VINYL_FINAL_RECOVERY_LOCAL ||
	       e->status == VINYL_FINAL_RECOVERY_REMOTE);
	e->status = VINYL_ONLINE;
}

/** }}} Recovery */

/* {{{ vy_stmt_iterator: Common interface for iterator over run, mem, etc */

struct vy_stmt_iterator;

typedef NODISCARD int
(*vy_iterator_next_f)(struct vy_stmt_iterator *virt_iterator,
		      struct vy_stmt *in, struct vy_stmt **ret);
/**
 * The restore() function moves an iterator to the specified
 * statement (@arg last_stmt) and returns the new statement via @arg ret.
 * In addition two cases are possible either the position of the iterator
 * has been changed after the restoration or it hasn't.
 *
 * 1) The position wasn't changed. This case appears if the iterator is moved
 *    to the statement that equals to the old statement by key and less
 *    or equal by LSN.
 *
 *    Example of the unchanged position:
 *    ┃     ...      ┃                      ┃     ...      ┃
 *    ┃ k2, lsn = 10 ┣▶ read_iterator       ┃ k3, lsn = 20 ┃
 *    ┃ k2, lsn = 9  ┃  position            ┃              ┃
 *    ┃ k2, lsn = 8  ┃                      ┃ k2, lsn = 8  ┣▶ read_iterator
 *    ┃              ┃   restoration ▶▶     ┃              ┃  position - the
 *    ┃ k1, lsn = 10 ┃                      ┃ k1, lsn = 10 ┃  same key and the
 *    ┃ k1, lsn = 9  ┃                      ┃ k1, lsn = 9  ┃  older LSN
 *    ┃     ...      ┃                      ┃     ...      ┃
 *
 * 2) Otherwise the position was changed and points on a statement with another
 *    key or with the same key but the bigger LSN.
 *
 *    Example of the changed position:
 *    ┃     ...      ┃                      ┃     ...      ┃
 *    ┃ k2, lsn = 10 ┣▶ read_iterator       ┃ k2, lsn = 11 ┣▶ read_iterator
 *    ┃ k2, lsn = 9  ┃  position            ┃ k2, lsn = 10 ┃  position - found
 *    ┃ k2, lsn = 8  ┃                      ┃ k2, lsn = 9  ┃  the newer LSN
 *    ┃              ┃   restoration ▶▶     ┃ k2, lsn = 8  ┃
 *    ┃ k1, lsn = 10 ┃                      ┃              ┃
 *    ┃ k1, lsn = 9  ┃                      ┃ k1, lsn = 10 ┃
 *    ┃     ...      ┃                      ┃     ...      ┃
 *
 *    Another example:
 *    ┃     ...      ┃                      ┃              ┃
 *    ┃ k3, lsn = 20 ┃                      ┃     ...      ┃
 *    ┃              ┃                      ┃ k3, lsn = 10 ┃
 *    ┃ k2, lsn = 8  ┣▶ read_iterator       ┃ k3, lsn = 9  ┃
 *    ┃              ┃  position            ┃ k3, lsn = 8  ┣▶ read_iterator
 *    ┃ k1, lsn = 10 ┃                      ┃              ┃  position - k2 was
 *    ┃ k1, lsn = 9  ┃   restoration ▶▶     ┃ k1, lsn = 10 ┃  not found, so go
 *    ┃     ...      ┃                      ┃     ...      ┃  to the next key
 */
typedef NODISCARD int
(*vy_iterator_restore_f)(struct vy_stmt_iterator *virt_iterator,
			 const struct vy_stmt *last_stmt, struct vy_stmt **ret);
typedef void
(*vy_iterator_next_close_f)(struct vy_stmt_iterator *virt_iterator);

struct vy_stmt_iterator_iface {
	vy_iterator_next_f next_key;
	vy_iterator_next_f next_lsn;
	vy_iterator_restore_f restore;
	vy_iterator_next_close_f close;
};

struct vy_stmt_iterator {
	struct vy_stmt_iterator_iface *iface;
};

/* }}} vy_stmt_iterator: Common interface for iterator over run, mem, etc */

/* {{{ vy_run_itr API forward declaration */
/* TODO: move to header (with struct vy_run_itr) and remove static keyword */

/** Position of a particular stmt in vy_run. */
struct vy_run_iterator_pos {
	uint32_t page_no;
	uint32_t pos_in_page;
};

/**
 * Return statements from vy_run based on initial search key,
 * iteration order and view lsn.
 *
 * All statements with lsn > vlsn are skipped.
 * The API allows to traverse over resulting statements within two
 * dimensions - key and lsn. next_key() switches to the youngest
 * statement of the next key, according to the iteration order,
 * and next_lsn() switches to an older statement for the same
 * key.
 */
struct vy_run_iterator {
	/** Parent class, must be the first member */
	struct vy_stmt_iterator base;

	/* Members needed for memory allocation and disk access */
	/* index */
	struct vy_index *index;
	/* run */
	struct vy_run *run;
	/* range of the run */
	struct vy_range *range;

	/* Search options */
	/**
	 * Iterator type, that specifies direction, start position and stop
	 * criteria if the key is not specified, GT and EQ are changed to
	 * GE, LT to LE for beauty.
	 */
	enum iterator_type iterator_type;
	/** Key to search. */
	const struct vy_stmt *key;
	/* LSN visibility, iterator shows values with lsn <= vlsn */
	const int64_t *vlsn;

	/* State of the iterator */
	/** Position of the current record */
	struct vy_run_iterator_pos curr_pos;
	/**
	 * Last stmt returned by vy_run_iterator_get.
	 * The iterator holds this stmt until the next call to
	 * vy_run_iterator_get, when it's dereferenced.
	 */
	struct vy_stmt *curr_stmt;
	/** Position of record that spawned curr_stmt */
	struct vy_run_iterator_pos curr_stmt_pos;
	/** LRU cache of two active pages (two pages is enough). */
	struct vy_page *curr_page;
	struct vy_page *prev_page;
	/** Is false until first .._get ot .._next_.. method is called */
	bool search_started;
	/** Search is finished, you will not get more values from iterator */
	bool search_ended;
};

static void
vy_run_iterator_open(struct vy_run_iterator *itr, struct vy_range *range,
		     struct vy_run *run, enum iterator_type iterator_type,
		     const struct vy_stmt *key, const int64_t *vlsn);

/* }}} vy_run_iterator API forward declaration */

/* {{{ vy_run_iterator vy_run_iterator support functions */
/* TODO: move to appropriate c file and remove */

/**
 * Page
 */
struct vy_page {
	/** Page position in the run file (used by run_iterator->page_cache */
	uint32_t page_no;
	/** The number of statements */
	uint32_t count;
	/** Page data size */
	uint32_t unpacked_size;
	/** Array with row offsets in page data */
	uint32_t *row_index;
	/** Page data */
	char *data;
};

static struct vy_page *
vy_page_new(const struct vy_page_info *page_info)
{
	struct vy_page *page = malloc(sizeof(*page));
	if (page == NULL) {
		diag_set(OutOfMemory, sizeof(*page),
			"load_page", "page cache");
		return NULL;
	}
	page->count = page_info->count;
	page->unpacked_size = page_info->unpacked_size;
	page->row_index = calloc(page_info->count, sizeof(uint32_t));
	if (page->row_index == NULL) {
		diag_set(OutOfMemory, page_info->count * sizeof(uint32_t),
			 "malloc", "page->row_index");
		free(page);
		return NULL;
	}

	page->data = (char *)malloc(page_info->unpacked_size);
	if (page->data == NULL) {
		diag_set(OutOfMemory, page_info->unpacked_size,
			 "malloc", "page->data");
		free(page->row_index);
		free(page);
		return NULL;
	}
	return page;
}

static void
vy_page_delete(struct vy_page *page)
{
	uint32_t *row_index = page->row_index;
	char *data = page->data;
#if !defined(NDEBUG)
	memset(page->row_index, '#', sizeof(uint32_t) * page->count);
	memset(page->data, '#', page->unpacked_size);
	memset(page, '#', sizeof(*page));
#endif /* !defined(NDEBUG) */
	free(row_index);
	free(data);
	free(page);
}

/**
 * Read raw stmt data from the page
 * \param page page
 * \param stmt_no stmt position in the page
 * \param[out] pinfo stmt metadata
 * \return stmt data including offsets table
 */
static struct vy_stmt *
vy_page_stmt(struct vy_page *page, uint32_t stmt_no,
	     const struct tuple_format *format, const struct key_def *key_def)
{
	assert(stmt_no < page->count);
	const char *data = page->data + page->row_index[stmt_no];
	const char *data_end = stmt_no + 1 < page->count ?
		page->data + page->row_index[stmt_no + 1] :
		page->data + page->unpacked_size;
	struct xrow_header xrow;
	if (xrow_header_decode(&xrow, &data, data_end) != 0)
		return NULL;
	return vy_stmt_decode(&xrow, format, key_def->part_count);
}

/**
 * Get page from LRU cache
 * @retval page if found
 * @retval NULL otherwise
 */
static struct vy_page *
vy_run_iterator_cache_get(struct vy_run_iterator *itr, uint32_t page_no)
{
	if (itr->curr_page != NULL) {
		if (itr->curr_page->page_no == page_no)
			return itr->curr_page;
		if (itr->prev_page != NULL &&
		    itr->prev_page->page_no == page_no) {
			struct vy_page *result = itr->prev_page;
			itr->prev_page = itr->curr_page;
			itr->curr_page = result;
			return result;
		}
	}
	return NULL;
}

/**
 * Touch page in LRU cache.
 * The cache is at least two pages. Ensure that subsequent read keeps
 * the page_no in the cache by moving it to the start of LRU list.
 * @pre page must be in the cache
 */
static void
vy_run_iterator_cache_touch(struct vy_run_iterator *itr, uint32_t page_no)
{
	struct vy_page *page = vy_run_iterator_cache_get(itr, page_no);
	assert(page != NULL);
	(void) page;
}

/**
 * Put page to LRU cache
 */
static void
vy_run_iterator_cache_put(struct vy_run_iterator *itr, struct vy_page *page,
			  uint32_t page_no)
{
	if (itr->prev_page != NULL)
		vy_page_delete(itr->prev_page);
	itr->prev_page = itr->curr_page;
	itr->curr_page = page;
	page->page_no = page_no;
}

/**
 * Clear LRU cache
 */
static void
vy_run_iterator_cache_clean(struct vy_run_iterator *itr)
{
	if (itr->curr_stmt != NULL) {
		vy_stmt_unref(itr->curr_stmt);
		itr->curr_stmt = NULL;
		itr->curr_stmt_pos.page_no = UINT32_MAX;
	}
	if (itr->curr_page != NULL) {
		vy_page_delete(itr->curr_page);
		if (itr->prev_page != NULL)
			vy_page_delete(itr->prev_page);
		itr->curr_page = itr->prev_page = NULL;
	}
}

static int
vy_row_index_decode(uint32_t *row_index, uint32_t count,
		    struct xrow_header *xrow)
{
	struct request request;
	request_create(&request, xrow->type);
	if (request_decode(&request, xrow->body->iov_base,
			   xrow->body->iov_len) == -1) {
		return -1;
	}
	if (request.tuple == NULL) {
error:
		diag_set(ClientError, ER_VINYL, "Can't decode row index");
		return -1;
	}
	const char *pos = request.tuple;
	if (mp_decode_array(&pos) != 1)
		goto error;
	uint32_t size = mp_decode_binl(&pos);
	if (size != sizeof(uint32_t) * count)
		goto error;
	for (uint32_t i = 0; i < count; ++i)
		row_index[i] = mp_load_u32(&pos);
	assert(pos == request.tuple_end);
	return 0;
}
/**
 * Read a page requests from vinyl xlog data file.
 *
 * @retval 0 on success
 * @retval -1 on error, check diag
 */
static int
vy_page_read(struct vy_page *page, const struct vy_page_info *page_info, int fd,
	     ZSTD_DStream *zdctx)
{
	/* read xlog tx from xlog file */
	size_t region_svp = region_used(&fiber()->gc);
	char *data = (char *)region_alloc(&fiber()->gc, page_info->size);
	if (data == NULL) {
		diag_set(OutOfMemory, page_info->size, "region gc", "page");
		return -1;
	}
	ssize_t readen = fio_pread(fd, data, page_info->size,
				   page_info->offset);
	if (readen < 0) {
		/* TODO: report filename */
		diag_set(SystemError, "failed to read from file");
		goto error;
	}
	if (readen != (ssize_t)page_info->size) {
		/* TODO: replace with XlogError, report filename */
		diag_set(ClientError, ER_VINYL, "Unexpected end of file");
		goto error;
	}
	ERROR_INJECT(ERRINJ_VY_READ_PAGE_TIMEOUT, {usleep(50000);});

	/* decode xlog tx */
	const char *data_pos = data;
	const char *data_end = data + readen;
	char *rows = page->data;
	char *rows_end = rows + page_info->unpacked_size;
	if (xlog_tx_decode(data, data_end, rows, rows_end, zdctx) != 0)
		goto error;

	struct xrow_header xrow;
	data_pos = page->data + page_info->row_index_offset;
	data_end = page->data + page_info->unpacked_size;
	if (xrow_header_decode(&xrow, &data_pos, data_end) == -1)
		goto error;
	if (vy_row_index_decode(page->row_index, page->count, &xrow) != 0)
		goto error;
	region_truncate(&fiber()->gc, region_svp);
	ERROR_INJECT(ERRINJ_VY_READ_PAGE, {
		diag_set(ClientError, ER_VINYL, "page read injection");
		return -1;});
	return 0;
error:
	region_truncate(&fiber()->gc, region_svp);
	return -1;
}

/**
 * Get thread local zstd decompression context
 */
static ZSTD_DStream *
vy_env_get_zdctx(struct vy_env *env)
{
	ZSTD_DStream *zdctx = tt_pthread_getspecific(env->zdctx_key);
	if (zdctx == NULL) {
		zdctx = ZSTD_createDStream();
		if (zdctx == NULL) {
			diag_set(OutOfMemory, sizeof(zdctx), "malloc",
				 "zstd context");
			return NULL;
		}
		tt_pthread_setspecific(env->zdctx_key, zdctx);
	}
	return zdctx;
}

/**
 * vinyl read task callback
 */
static int
vy_page_read_cb(struct coio_task *base)
{
	struct vy_page_read_task *task = (struct vy_page_read_task *)base;
	ZSTD_DStream *zdctx = vy_env_get_zdctx(task->env);
	if (zdctx == NULL)
		return -1;
	task->rc = vy_page_read(task->page, &task->page_info,
				task->run->fd, zdctx);
	return task->rc;
}

/**
 * vinyl read task cleanup callback
 */
static int
vy_page_read_cb_free(struct coio_task *base)
{
	struct vy_page_read_task *task = (struct vy_page_read_task *)base;
	if (task->page)
		vy_page_delete(task->page);
	vy_run_unref(task->run);
	coio_task_destroy(&task->base);
	mempool_free(&task->env->read_task_pool, task);
	return 0;
}

/**
 * Get a page by the given number the cache or load it from the disk.
 *
 * @retval 0 success
 * @retval -1 critical error
 * @retval -2 invalid iterator
 */
static NODISCARD int
vy_run_iterator_load_page(struct vy_run_iterator *itr, uint32_t page_no,
			  struct vy_page **result)
{
	struct vy_index *index = itr->index;
	const struct vy_env *env = index->env;

	/* Check cache */
	*result = vy_run_iterator_cache_get(itr, page_no);
	if (*result != NULL)
		return 0;

	/* Allocate buffers */
	struct vy_page_info *page_info = vy_run_page_info(itr->run, page_no);
	struct vy_page *page = vy_page_new(page_info);
	if (page == NULL)
		return -1;

	/* Read page data from the disk */
	int rc;
	if (cord_is_main() && env->status == VINYL_ONLINE) {
		/*
		 * Use coeio for TX thread **after recovery**.
		 * Please note that vy_run can go away after yield.
		 * In this case vy_run_iterator is no more valid and
		 * rc = -2 is returned to the caller.
		 */

		uint32_t index_version = itr->index->version;
		uint32_t range_version = itr->range->version;

		/* Allocate a coio task */
		struct vy_page_read_task *task =
			(struct vy_page_read_task *)mempool_alloc(&itr->index->env->read_task_pool);
		if (task == NULL) {
			diag_set(OutOfMemory, sizeof(*task), "malloc",
				 "vy_page_read_task");
			return -1;
		}
		coio_task_create(&task->base, vy_page_read_cb,
				  vy_page_read_cb_free);

		/*
		 * Make sure the run file descriptor won't be closed
		 * (even worse, reopened) while a coeio thread is
		 * reading it.
		 */
		task->run = itr->run;
		vy_run_ref(task->run);
		task->page_info = *page_info;
		task->env = index->env;
		task->page = page;

		/* Post task to coeio */
		rc = coio_task_post(&task->base, TIMEOUT_INFINITY);
		if (rc < 0)
			return -1; /* timed out or cancelled */

		if (task->rc != 0) {
			/* posted, but failed */
			diag_move(&task->base.diag, &fiber()->diag);
			vy_page_read_cb_free(&task->base);
			return -1;
		}

		task->page = NULL;
		vy_page_read_cb_free(&task->base);

		/*
		 * Check that vy_index/vy_range/vy_run haven't changed
		 * during coeio_pread().
		 */
		if (index_version != itr->index->version ||
		    range_version != itr->range->version) {
			itr->index = NULL;
			itr->range = NULL;
			itr->run = NULL;
			vy_page_delete(page);
			return -2; /* iterator is no more valid */
		}
	} else {
		/*
		 * Optimization: use blocked I/O for non-TX threads or
		 * during WAL recovery (env->status != VINYL_ONLINE).
		 */
		ZSTD_DStream *zdctx = vy_env_get_zdctx(itr->index->env);
		if (zdctx == NULL)
			return -1;
		if (vy_page_read(page, page_info, itr->run->fd, zdctx) != 0) {
			vy_page_delete(page);
			page = NULL;
		}
	}

	/* Iterator is never used from multiple fibers */
	assert(vy_run_iterator_cache_get(itr, page_no) == NULL);

	/* Update cache */
	vy_run_iterator_cache_put(itr, page, page_no);

	*result = page;
	return 0;
}

/**
 * Read key and lsn by a given wide position.
 * For the first record in a page reads the result from the page
 * index instead of fetching it from disk.
 *
 * @retval 0 success
 * @retval -1 read error or out of memory.
 * @retval -2 invalid iterator
 */
static NODISCARD int
vy_run_iterator_read(struct vy_run_iterator *itr,
		     struct vy_run_iterator_pos pos,
		     struct vy_stmt **stmt)
{
	struct vy_page *page;
	int rc = vy_run_iterator_load_page(itr, pos.page_no, &page);
	if (rc != 0)
		return rc;
	*stmt = vy_page_stmt(page, pos.pos_in_page, itr->index->format,
			     itr->index->key_def);
	if (*stmt == NULL)
		return -1;
	return 0;
}

/**
 * Binary search in page index
 * In terms of STL, makes lower_bound for EQ,GE,LT and upper_bound for GT,LE
 * Additionally *equal_key argument is set to true if the found value is
 * equal to given key (untouched otherwise)
 * @retval page number
 */
static uint32_t
vy_run_iterator_search_page(struct vy_run_iterator *itr,
			    const struct vy_stmt *key, bool *equal_key)
{
	uint32_t beg = 0;
	uint32_t end = itr->run->info.count;
	/* for upper bound we change zero comparison result to -1 */
	int zero_cmp = itr->iterator_type == ITER_GT ||
		       itr->iterator_type == ITER_LE ? -1 : 0;
	struct vy_index *idx = itr->index;
	while (beg != end) {
		uint32_t mid = beg + (end - beg) / 2;
		struct vy_page_info *page_info;
		page_info = vy_run_page_info(itr->run, mid);
		int cmp;
		cmp = -vy_stmt_compare_with_key(key, page_info->min_key, idx->format,
						    idx->key_def);
		cmp = cmp ? cmp : zero_cmp;
		*equal_key = *equal_key || cmp == 0;
		if (cmp < 0)
			beg = mid + 1;
		else
			end = mid;
	}
	return end;
}

/**
 * Binary search in page
 * In terms of STL, makes lower_bound for EQ,GE,LT and upper_bound for GT,LE
 * Additionally *equal_key argument is set to true if the found value is
 * equal to given key (untouched otherwise)
 * @retval position in the page
 */
static uint32_t
vy_run_iterator_search_in_page(struct vy_run_iterator *itr,
			       const struct vy_stmt *key,
			       struct vy_page *page, bool *equal_key)
{
	uint32_t beg = 0;
	uint32_t end = page->count;
	/* for upper bound we change zero comparison result to -1 */
	int zero_cmp = itr->iterator_type == ITER_GT ||
		       itr->iterator_type == ITER_LE ? -1 : 0;
	struct vy_index *idx = itr->index;
	while (beg != end) {
		uint32_t mid = beg + (end - beg) / 2;
		struct vy_stmt *fnd_key = vy_page_stmt(page, mid, idx->format,
						       idx->key_def);
		if (fnd_key == NULL)
			return end;
		int cmp = vy_stmt_compare(fnd_key, key, idx->format,idx->key_def);
		cmp = cmp ? cmp : zero_cmp;
		*equal_key = *equal_key || cmp == 0;
		if (cmp < 0)
			beg = mid + 1;
		else
			end = mid;
		vy_stmt_unref(fnd_key);
	}
	return end;
}

/**
 * Binary search in a run for the given key.
 * In terms of STL, makes lower_bound for EQ,GE,LT and upper_bound for GT,LE
 * Resulting wide position is stored it *pos argument
 * Additionally *equal_key argument is set to true if the found value is
 * equal to given key (untouched otherwise)
 *
 * @retval 0 success
 * @retval -1 read or memory error
 * @retval -2 invalid iterator
 */
static NODISCARD int
vy_run_iterator_search(struct vy_run_iterator *itr, const struct vy_stmt *key,
		       struct vy_run_iterator_pos *pos, bool *equal_key)
{
	pos->page_no = vy_run_iterator_search_page(itr, key, equal_key);
	if (pos->page_no == 0) {
		pos->pos_in_page = 0;
		return 0;
	}
	pos->page_no--;
	struct vy_page *page;
	int rc = vy_run_iterator_load_page(itr, pos->page_no, &page);
	if (rc != 0)
		return rc;
	bool equal_in_page = false;
	pos->pos_in_page = vy_run_iterator_search_in_page(itr, key, page,
							  &equal_in_page);
	if (pos->pos_in_page == page->count) {
		pos->page_no++;
		pos->pos_in_page = 0;
	} else {
		*equal_key = equal_in_page;
	}
	return 0;
}

/**
 * Increment (or decrement, depending on the order) the current
 * wide position.
 * @retval 0 success, set *pos to new value
 * @retval 1 EOF
 * Affects: curr_loaded_page
 */
static NODISCARD int
vy_run_iterator_next_pos(struct vy_run_iterator *itr,
			 enum iterator_type iterator_type,
			 struct vy_run_iterator_pos *pos)
{
	*pos = itr->curr_pos;
	assert(pos->page_no < itr->run->info.count);
	if (iterator_type == ITER_LE || iterator_type == ITER_LT) {
		if (pos->pos_in_page > 0) {
			pos->pos_in_page--;
		} else {
			if (pos->page_no == 0)
				return 1;
			pos->page_no--;
			struct vy_page_info *page_info =
				vy_run_page_info(itr->run, pos->page_no);
			assert(page_info->count > 0);
			pos->pos_in_page = page_info->count - 1;
		}
	} else {
		assert(iterator_type == ITER_GE || iterator_type == ITER_GT ||
		       iterator_type == ITER_EQ);
		struct vy_page_info *page_info =
			vy_run_page_info(itr->run, pos->page_no);
		assert(page_info->count > 0);
		pos->pos_in_page++;
		if (pos->pos_in_page >= page_info->count) {
			pos->page_no++;
			pos->pos_in_page = 0;
			if (pos->page_no == itr->run->info.count)
				return 1;
		}
	}
	return 0;
}

static NODISCARD int
vy_run_iterator_get(struct vy_run_iterator *itr, struct vy_stmt **result);

/**
 * Find the next record with lsn <= itr->lsn record.
 * The current position must be at the beginning of a series of
 * records with the same key it terms of direction of iterator
 * (i.e. left for GE, right for LE).
 * @retval 0 success or EOF (*ret == NULL)
 * @retval -1 read or memory error
 * @retval -2 invalid iterator
 * Affects: curr_loaded_page, curr_pos, search_ended
 */
static NODISCARD int
vy_run_iterator_find_lsn(struct vy_run_iterator *itr, struct vy_stmt **ret)
{
	assert(itr->curr_pos.page_no < itr->run->info.count);
	struct vy_stmt *stmt;
	struct key_def *key_def = itr->index->key_def;
	struct tuple_format *format = itr->index->format;
	const struct vy_stmt *key = itr->key;
	enum iterator_type iterator_type = itr->iterator_type;
	*ret = NULL;
	int rc = vy_run_iterator_read(itr, itr->curr_pos, &stmt);
	if (rc != 0)
		return rc;
	while (stmt->lsn > *itr->vlsn) {
		vy_stmt_unref(stmt);
		stmt = NULL;
		rc = vy_run_iterator_next_pos(itr, iterator_type,
					      &itr->curr_pos);
		if (rc > 0) {
			vy_run_iterator_cache_clean(itr);
			itr->search_ended = true;
			return 0;
		}
		assert(rc == 0);
		rc = vy_run_iterator_read(itr, itr->curr_pos, &stmt);
		if (rc != 0)
			return rc;
		if (iterator_type == ITER_EQ &&
		    vy_stmt_compare(stmt, key, format, key_def)) {
			vy_stmt_unref(stmt);
			stmt = NULL;
			vy_run_iterator_cache_clean(itr);
			itr->search_ended = true;
			return 0;
		}
	}
	if (iterator_type == ITER_LE || iterator_type == ITER_LT) {
		/* Remember the page_no of stmt */
		uint32_t cur_key_page_no = itr->curr_pos.page_no;

		struct vy_run_iterator_pos test_pos;
		rc = vy_run_iterator_next_pos(itr, iterator_type, &test_pos);
		while (rc == 0) {
			/*
			 * The cache is at least two pages. Ensure that
			 * subsequent read keeps the stmt in the cache
			 * by moving its page to the start of LRU list.
			 */
			vy_run_iterator_cache_touch(itr, cur_key_page_no);

			struct vy_stmt *test_stmt;
			rc = vy_run_iterator_read(itr, test_pos, &test_stmt);
			if (rc != 0)
				return rc;
			if (test_stmt->lsn > *itr->vlsn ||
			    vy_stmt_compare(stmt, test_stmt, format, key_def) != 0) {
				vy_stmt_unref(test_stmt);
				test_stmt = NULL;
				break;
			}
			vy_stmt_unref(test_stmt);
			test_stmt = NULL;
			itr->curr_pos = test_pos;

			/* See above */
			vy_run_iterator_cache_touch(itr, cur_key_page_no);

			rc = vy_run_iterator_next_pos(itr, iterator_type,
						      &test_pos);
		}

		rc = rc > 0 ? 0 : rc;
	}
	vy_stmt_unref(stmt);
	if (!rc) /* If next_pos() found something then get it. */
		rc = vy_run_iterator_get(itr, ret);
	return rc;
}

/*
 * FIXME: vy_run_iterator_next_key() calls vy_run_iterator_start() which
 * recursivly calls vy_run_iterator_next_key().
 */
static NODISCARD int
vy_run_iterator_next_key(struct vy_stmt_iterator *vitr, struct vy_stmt *in,
			 struct vy_stmt **ret);
/**
 * Find next (lower, older) record with the same key as current
 * Return true if the record was found
 * Return false if no value was found (or EOF) or there is a read error
 * @retval 0 success or EOF (*ret == NULL)
 * @retval -1 read or memory error
 * @retval -2 invalid iterator
 * Affects: curr_loaded_page, curr_pos, search_ended
 */
static NODISCARD int
vy_run_iterator_start(struct vy_run_iterator *itr, struct vy_stmt **ret)
{
	assert(!itr->search_started);
	itr->search_started = true;
	*ret = NULL;

	if (itr->run->info.count == 1) {
		/* there can be a stupid bootstrap run in which it's EOF */
		struct vy_page_info *page_info = itr->run->info.page_infos;

		if (!page_info->count) {
			vy_run_iterator_cache_clean(itr);
			itr->search_ended = true;
			return 0;
		}
		struct vy_page *page;
		int rc = vy_run_iterator_load_page(itr, 0, &page);
		if (rc != 0)
			return rc;
	} else if (itr->run->info.count == 0) {
		vy_run_iterator_cache_clean(itr);
		itr->search_ended = true;
		return 0;
	}

	struct vy_run_iterator_pos end_pos = {itr->run->info.count, 0};
	bool equal_found = false;
	int rc;
	if (vy_stmt_part_count(itr->key) > 0) {
		rc = vy_run_iterator_search(itr, itr->key, &itr->curr_pos,
					    &equal_found);
		if (rc != 0)
			return rc;
	} else if (itr->iterator_type == ITER_LE) {
		itr->curr_pos = end_pos;
	} else {
		assert(itr->iterator_type == ITER_GE);
		itr->curr_pos.page_no = 0;
		itr->curr_pos.pos_in_page = 0;
	}
	if (itr->iterator_type == ITER_EQ && !equal_found) {
		vy_run_iterator_cache_clean(itr);
		itr->search_ended = true;
		return 0;
	}
	if ((itr->iterator_type == ITER_GE || itr->iterator_type == ITER_GT) &&
	    itr->curr_pos.page_no == end_pos.page_no) {
		vy_run_iterator_cache_clean(itr);
		itr->search_ended = true;
		return 0;
	}
	if (itr->iterator_type == ITER_LT || itr->iterator_type == ITER_LE) {
		/**
		 * 1) in case of ITER_LT we now positioned on the value >= than
		 * given, so we need to make a step on previous key
		 * 2) in case if ITER_LE we now positioned on the value > than
		 * given (special branch of code in vy_run_iterator_search),
		 * so we need to make a step on previous key
		 */
		return vy_run_iterator_next_key(&itr->base, NULL, ret);
	} else {
		assert(itr->iterator_type == ITER_GE ||
		       itr->iterator_type == ITER_GT ||
		       itr->iterator_type == ITER_EQ);
		/**
		 * 1) in case of ITER_GT we now positioned on the value > than
		 * given (special branch of code in vy_run_iterator_search),
		 * so we need just to find proper lsn
		 * 2) in case if ITER_GE or ITER_EQ we now positioned on the
		 * value >= given, so we need just to find proper lsn
		 */
		return vy_run_iterator_find_lsn(itr, ret);
	}
}

/* }}} vy_run_iterator vy_run_iterator support functions */

/* {{{ vy_run_iterator API implementation */
/* TODO: move to c file and remove static keyword */

/** Vtable for vy_stmt_iterator - declared below */
static struct vy_stmt_iterator_iface vy_run_iterator_iface;

/**
 * Open the iterator.
 */
static void
vy_run_iterator_open(struct vy_run_iterator *itr, struct vy_range *range,
		     struct vy_run *run, enum iterator_type iterator_type,
		     const struct vy_stmt *key, const int64_t *vlsn)
{
	itr->base.iface = &vy_run_iterator_iface;

	itr->index = range->index;
	itr->range = range;
	itr->run = run;

	itr->iterator_type = iterator_type;
	itr->key = key;
	itr->vlsn = vlsn;
	if (vy_stmt_part_count(key) == 0) {
		/* NULL key. change itr->iterator_type for simplification */
		itr->iterator_type = iterator_type == ITER_LT ||
				     iterator_type == ITER_LE ?
				     ITER_LE : ITER_GE;
	}

	itr->curr_stmt = NULL;
	itr->curr_pos.page_no = itr->run->info.count;
	itr->curr_stmt_pos.page_no = UINT32_MAX;
	itr->curr_page = NULL;
	itr->prev_page = NULL;

	itr->search_started = false;
	itr->search_ended = false;
}

/**
 * Create a stmt object from a its impression on a run page.
 * Uses the current iterator position in the page.
 *
 * @retval 0 success or EOF (*result == NULL)
 * @retval -1 memory or read error
 * @retval -2 invalid iterator
 */
static NODISCARD int
vy_run_iterator_get(struct vy_run_iterator *itr, struct vy_stmt **result)
{
	assert(itr->search_started);
	*result = NULL;
	if (itr->search_ended)
		return 0;
	if (itr->curr_stmt != NULL) {
		if (itr->curr_stmt_pos.page_no == itr->curr_pos.page_no &&
		    itr->curr_stmt_pos.pos_in_page == itr->curr_pos.pos_in_page) {
			*result = itr->curr_stmt;
			return 0;
		}
		vy_stmt_unref(itr->curr_stmt);
		itr->curr_stmt = NULL;
		itr->curr_stmt_pos.page_no = UINT32_MAX;
	}
	int rc = vy_run_iterator_read(itr, itr->curr_pos, result);
	if (rc == 0) {
		itr->curr_stmt_pos = itr->curr_pos;
		itr->curr_stmt = *result;
	}
	return rc;
}

/**
 * Find the next stmt in a page, i.e. a stmt with a different key
 * and fresh enough LSN (i.e. skipping the keys
 * too old for the current transaction).
 *
 * @retval 0 success or EOF (*ret == NULL)
 * @retval -1 memory or read error
 * @retval -2 invalid iterator
 */
static NODISCARD int
vy_run_iterator_next_key(struct vy_stmt_iterator *vitr, struct vy_stmt *in,
			 struct vy_stmt **ret)
{
	(void)in;
	assert(vitr->iface->next_key == vy_run_iterator_next_key);
	struct vy_run_iterator *itr = (struct vy_run_iterator *) vitr;
	*ret = NULL;
	int rc;

	if (itr->search_ended)
		return 0;
	if (!itr->search_started)
		return vy_run_iterator_start(itr, ret);
	uint32_t end_page = itr->run->info.count;
	assert(itr->curr_pos.page_no <= end_page);
	struct key_def *key_def = itr->index->key_def;
	if (itr->iterator_type == ITER_LE || itr->iterator_type == ITER_LT) {
		if (itr->curr_pos.page_no == 0 &&
		    itr->curr_pos.pos_in_page == 0) {
			vy_run_iterator_cache_clean(itr);
			itr->search_ended = true;
			return 0;
		}
		if (itr->curr_pos.page_no == end_page) {
			/* A special case for reverse iterators */
			uint32_t page_no = end_page - 1;
			struct vy_page *page;
			int rc = vy_run_iterator_load_page(itr, page_no, &page);
			if (rc != 0)
				return rc;
			if (page->count == 0) {
				vy_run_iterator_cache_clean(itr);
				itr->search_ended = true;
				return 0;
			}
			itr->curr_pos.page_no = page_no;
			itr->curr_pos.pos_in_page = page->count - 1;
			return vy_run_iterator_find_lsn(itr, ret);
		}
	}
	assert(itr->curr_pos.page_no < end_page);

	struct vy_stmt *cur_key;
	rc = vy_run_iterator_read(itr, itr->curr_pos, &cur_key);
	if (rc != 0)
		return rc;
	uint32_t cur_key_page_no = itr->curr_pos.page_no;

	struct vy_stmt *next_key = NULL;
	struct tuple_format *format = itr->index->format;
	do {
		if (next_key != NULL)
			vy_stmt_unref(next_key);
		next_key = NULL;
		int rc = vy_run_iterator_next_pos(itr, itr->iterator_type,
						  &itr->curr_pos);
		if (rc > 0) {
			vy_run_iterator_cache_clean(itr);
			itr->search_ended = true;
			vy_stmt_unref(cur_key);
			cur_key = NULL;
			return 0;
		}

		/*
		 * The cache is at least two pages. Ensure that
		 * subsequent read keeps the cur_key in the cache
		 * by moving its page to the start of LRU list.
		 */
		vy_run_iterator_cache_touch(itr, cur_key_page_no);

		rc = vy_run_iterator_read(itr, itr->curr_pos, &next_key);
		if (rc != 0) {
			vy_stmt_unref(cur_key);
			cur_key = NULL;
			return rc;
		}

		/* See above */
		vy_run_iterator_cache_touch(itr, cur_key_page_no);
	} while (vy_stmt_compare(cur_key, next_key, format, key_def) == 0);
	vy_stmt_unref(cur_key);
	cur_key = NULL;
	if (itr->iterator_type == ITER_EQ &&
	    vy_stmt_compare(next_key, itr->key, format,	key_def) != 0) {
		vy_run_iterator_cache_clean(itr);
		itr->search_ended = true;
		return 0;
	}
	return vy_run_iterator_find_lsn(itr, ret);
}

/**
 * Find next (lower, older) record with the same key as current
 * @retval 0 success or EOF (*ret == NULL)
 * @retval -1 memory or read error
 * @retval -2 invalid iterator
 */
static NODISCARD int
vy_run_iterator_next_lsn(struct vy_stmt_iterator *vitr, struct vy_stmt *in,
			 struct vy_stmt **ret)
{
	(void)in;
	assert(vitr->iface->next_lsn == vy_run_iterator_next_lsn);
	struct vy_run_iterator *itr = (struct vy_run_iterator *) vitr;
	*ret = NULL;
	int rc;

	if (itr->search_ended)
		return 0;
	if (!itr->search_started)
		return vy_run_iterator_start(itr, ret);
	assert(itr->curr_pos.page_no < itr->run->info.count);

	struct vy_run_iterator_pos next_pos;
	rc = vy_run_iterator_next_pos(itr, ITER_GE, &next_pos);
	if (rc > 0)
		return 0;

	struct vy_stmt *cur_key;
	rc = vy_run_iterator_read(itr, itr->curr_pos, &cur_key);
	if (rc != 0)
		return rc;

	struct vy_stmt *next_key;
	rc = vy_run_iterator_read(itr, next_pos, &next_key);
	if (rc != 0) {
		vy_stmt_unref(cur_key);
		return rc;
	}

	/**
	 * One can think that we had to lock page of itr->curr_pos,
	 *  to prevent freeing cur_key with entire page and avoid
	 *  segmentation fault in vy_stmt_compare_raw.
	 * But in fact the only case when curr_pos and next_pos
	 *  point to different pages is the case when next_pos points
	 *  to the beginning of the next page, and in this case
	 *  vy_run_iterator_read will read data from page index, not the page.
	 *  So in the case no page will be unloaded and we don't need
	 *  page lock
	 */
	struct key_def *key_def = itr->index->key_def;
	int cmp = vy_stmt_compare(cur_key, next_key, itr->index->format, key_def);
	vy_stmt_unref(cur_key);
	cur_key = NULL;
	vy_stmt_unref(next_key);
	next_key = NULL;
	itr->curr_pos = cmp == 0 ? next_pos : itr->curr_pos;
	rc = cmp != 0;
	if (rc != 0)
		return 0;
	return vy_run_iterator_get(itr, ret);
}

/**
 * Restore the current position (if necessary) after a change in the set of
 * runs or ranges and check if the position was changed.
 * @sa struct vy_stmt_iterator comments.
 *
 * @pre the iterator is not started
 *
 * @param last_stmt the last key on which the iterator was
 *		      positioned
 *
 * @retval 0	if position did not change (iterator started)
 * @retval 1	if position changed
 * @retval -1	a read or memory error
 */
static NODISCARD int
vy_run_iterator_restore(struct vy_stmt_iterator *vitr,
			const struct vy_stmt *last_stmt, struct vy_stmt **ret)
{
	assert(vitr->iface->restore == vy_run_iterator_restore);
	struct vy_run_iterator *itr = (struct vy_run_iterator *) vitr;
	*ret = NULL;
	int rc;

	if (itr->search_started || last_stmt == NULL) {
		if (!itr->search_started) {
			rc = vy_run_iterator_start(itr, ret);
		} else {
			rc = vy_run_iterator_get(itr, ret);
		}
		if (rc < 0)
			return rc;
		return 0;
	}
	/* Restoration is very similar to first search so we'll use that */
	enum iterator_type save_type = itr->iterator_type;
	const struct vy_stmt *save_key = itr->key;
	if (itr->iterator_type == ITER_GT || itr->iterator_type == ITER_EQ)
		itr->iterator_type = ITER_GE;
	else if (itr->iterator_type == ITER_LT)
		itr->iterator_type = ITER_LE;
	itr->key = last_stmt;
	struct vy_stmt *next;
	rc = vy_run_iterator_start(itr, &next);
	itr->iterator_type = save_type;
	itr->key = save_key;
	if (rc != 0)
		return rc;
	else if (next == NULL)
		return 0;
	struct key_def *def = itr->index->key_def;
	struct tuple_format *format = itr->index->format;
	bool position_changed = true;
	if (vy_stmt_compare(next, last_stmt, format, def) == 0) {
		position_changed = false;
		if (next->lsn >= last_stmt->lsn) {
			/* skip the same stmt to next stmt or older version */
			do {
				rc = vy_run_iterator_next_lsn(vitr, next,
							      &next);
				if (rc != 0)
					return rc;
				if (next == NULL) {
					rc = vy_run_iterator_next_key(vitr,
								      next,
								      &next);
					if (rc != 0)
						return rc;
					break;
				}
			} while (next->lsn >= last_stmt->lsn);
			if (next != NULL)
				position_changed = true;
		}
	} else if (itr->iterator_type == ITER_EQ &&
		   vy_stmt_compare(itr->key, next, format, def) != 0) {

		itr->search_ended = true;
		vy_run_iterator_cache_clean(itr);
		return position_changed;
	}
	*ret = next;
	return position_changed;
}

/**
 * Close an iterator and free all resources
 */
static void
vy_run_iterator_close(struct vy_stmt_iterator *vitr)
{
	assert(vitr->iface->close == vy_run_iterator_close);
	struct vy_run_iterator *itr = (struct vy_run_iterator *) vitr;

	vy_run_iterator_cache_clean(itr);
	TRASH(itr);
}

static struct vy_stmt_iterator_iface vy_run_iterator_iface = {
	.next_key = vy_run_iterator_next_key,
	.next_lsn = vy_run_iterator_next_lsn,
	.restore = vy_run_iterator_restore,
	.close = vy_run_iterator_close
};

/* }}} vy_run_iterator API implementation */

/* {{{ vy_mem_iterator API forward declaration */
/* TODO: move to header and remove static keyword */

/**
 * Return statements from vy_mem (in-memory index) based on
 * initial search key, iteration order and view lsn.
 *
 * All statements with lsn > vlsn are skipped.
 * The API allows to traverse over resulting statements within two
 * dimensions - key and lsn. next_key() switches to the youngest
 * statement of the next key, according to the iteration order,
 * and next_lsn() switches to an older statement for the same
 * key.
 */
struct vy_mem_iterator {
	/** Parent class, must be the first member */
	struct vy_stmt_iterator base;

	/* mem */
	struct vy_mem *mem;

	/* Search options */
	/**
	 * Iterator type, that specifies direction, start position and stop
	 * criteria if key == NULL: GT and EQ are changed to GE, LT to LE for
	 * beauty.
	 */
	enum iterator_type iterator_type;
	/** Key to search. */
	const struct vy_stmt *key;
	/* LSN visibility, iterator shows values with lsn <= than that */
	const int64_t *vlsn;

	/* State of iterator */
	/* Current position in tree */
	struct vy_mem_tree_iterator curr_pos;
	/*
	 * The pointer on a region allocated statement from vy_mem BPS tree.
	 * There is no guarantee that curr_pos points on curr_stmt in the tree.
	 * For example, cur_pos can be invalid but curr_stmt can point on a
	 * valid statement.
	 */
	const struct vy_stmt *curr_stmt;
	/*
	 * Copy of the statement returned from one of public methods
	 * (restore/next_lsn/next_key). Need to store the copy, because can't
	 * return region allocated curr_stmt.
	 */
	struct vy_stmt *last_stmt;
	/* data version from vy_mem */
	uint32_t version;

	/* Is false until first .._next_.. method is called */
	bool search_started;
};

/* Vtable for vy_stmt_iterator - declared below */
static struct vy_stmt_iterator_iface vy_mem_iterator_iface;

/**
 * vy_mem_iterator API forward declaration
 */

static void
vy_mem_iterator_open(struct vy_mem_iterator *itr, struct vy_mem *mem,
		     enum iterator_type iterator_type, const struct vy_stmt *key,
		     const int64_t *vlsn);

/* }}} vy_mem_iterator API forward declaration */

/* {{{ vy_mem_iterator support functions */

static int
vy_mem_iterator_copy_to(struct vy_mem_iterator *itr, struct vy_stmt **ret)
{
	assert(itr->curr_stmt != NULL);
	if (itr->last_stmt)
		vy_stmt_unref(itr->last_stmt);
	itr->last_stmt = vy_stmt_dup(itr->curr_stmt);
	*ret = itr->last_stmt;
	if (itr->last_stmt != NULL)
		return 0;
	return -1;
}

/**
 * Get a stmt by current position
 */
static const struct vy_stmt *
vy_mem_iterator_curr_stmt(struct vy_mem_iterator *itr)
{
	return *vy_mem_tree_iterator_get_elem(&itr->mem->tree, &itr->curr_pos);
}

/**
 * Make a step in directions defined by itr->iterator_type
 * @retval 0 success
 * @retval 1 EOF
 */
static int
vy_mem_iterator_step(struct vy_mem_iterator *itr)
{
	if (itr->iterator_type == ITER_LE || itr->iterator_type == ITER_LT)
		vy_mem_tree_iterator_prev(&itr->mem->tree, &itr->curr_pos);
	else
		vy_mem_tree_iterator_next(&itr->mem->tree, &itr->curr_pos);
	if (vy_mem_tree_iterator_is_invalid(&itr->curr_pos))
		return 1;
	itr->curr_stmt = vy_mem_iterator_curr_stmt(itr);
	return 0;
}

/**
 * Find next record with lsn <= itr->lsn record.
 * Current position must be at the beginning of serie of records with the
 * same key it terms of direction of iterator (i.e. left for GE, right for LE)
 *
 * @retval 0 Found
 * @retval 1 Not found
 */
static int
vy_mem_iterator_find_lsn(struct vy_mem_iterator *itr)
{
	assert(!vy_mem_tree_iterator_is_invalid(&itr->curr_pos));
	assert(itr->curr_stmt == vy_mem_iterator_curr_stmt(itr));
	struct key_def *key_def = itr->mem->key_def;
	struct tuple_format *format = itr->mem->format;
	while (itr->curr_stmt->lsn > *itr->vlsn) {
		if (vy_mem_iterator_step(itr) != 0 ||
		    (itr->iterator_type == ITER_EQ &&
		     vy_stmt_compare(itr->key, itr->curr_stmt, format,
				     key_def))) {
			itr->curr_stmt = NULL;
			return 1;
		}
	}
	if (itr->iterator_type == ITER_LE || itr->iterator_type == ITER_LT) {
		struct vy_mem_tree_iterator prev_pos = itr->curr_pos;
		vy_mem_tree_iterator_prev(&itr->mem->tree, &prev_pos);

		while (!vy_mem_tree_iterator_is_invalid(&prev_pos)) {
			const struct vy_stmt *prev_stmt =
				*vy_mem_tree_iterator_get_elem(&itr->mem->tree,
							       &prev_pos);
			if (prev_stmt->lsn > *itr->vlsn ||
			    vy_stmt_compare(itr->curr_stmt, prev_stmt, format,
					    key_def) != 0)
				break;
			itr->curr_pos = prev_pos;
			itr->curr_stmt = prev_stmt;
			vy_mem_tree_iterator_prev(&itr->mem->tree, &prev_pos);
		}
	}
	assert(itr->curr_stmt != NULL);
	return 0;
}

/**
 * Find next (lower, older) record with the same key as current
 *
 * @retval 0 Found
 * @retval 1 Not found
 */
static int
vy_mem_iterator_start(struct vy_mem_iterator *itr)
{
	assert(!itr->search_started);
	itr->search_started = true;
	itr->version = itr->mem->version;

	struct tree_mem_key tree_key;
	tree_key.stmt = itr->key;
	/* (lsn == INT64_MAX - 1) means that lsn is ignored in comparison */
	tree_key.lsn = INT64_MAX - 1;
	if (vy_stmt_part_count(itr->key) > 0) {
		if (itr->iterator_type == ITER_EQ) {
			bool exact;
			itr->curr_pos =
				vy_mem_tree_lower_bound(&itr->mem->tree,
							&tree_key, &exact);
			if (!exact)
				return 1;
		} else if (itr->iterator_type == ITER_LE ||
			   itr->iterator_type == ITER_GT) {
			itr->curr_pos =
				vy_mem_tree_upper_bound(&itr->mem->tree,
							&tree_key, NULL);
		} else {
			assert(itr->iterator_type == ITER_GE ||
			       itr->iterator_type == ITER_LT);
			itr->curr_pos =
				vy_mem_tree_lower_bound(&itr->mem->tree,
							&tree_key, NULL);
		}
	} else if (itr->iterator_type == ITER_LE) {
		itr->curr_pos = vy_mem_tree_invalid_iterator();
	} else {
		assert(itr->iterator_type == ITER_GE);
		itr->curr_pos = vy_mem_tree_iterator_first(&itr->mem->tree);
	}

	if (itr->iterator_type == ITER_LT || itr->iterator_type == ITER_LE)
		vy_mem_tree_iterator_prev(&itr->mem->tree, &itr->curr_pos);
	if (vy_mem_tree_iterator_is_invalid(&itr->curr_pos))
		return 1;
	itr->curr_stmt = vy_mem_iterator_curr_stmt(itr);
	return vy_mem_iterator_find_lsn(itr);
}

/**
 * Restores iterator if the mem have been changed
 */
static void
vy_mem_iterator_check_version(struct vy_mem_iterator *itr)
{
	assert(itr->curr_stmt != NULL);
	if (itr->version == itr->mem->version)
		return;
	itr->version = itr->mem->version;
	const struct vy_stmt * const *record;
	record = vy_mem_tree_iterator_get_elem(&itr->mem->tree, &itr->curr_pos);
	if (record != NULL && *record == itr->curr_stmt)
		return;
	struct tree_mem_key tree_key;
	tree_key.stmt = itr->curr_stmt;
	tree_key.lsn = itr->curr_stmt->lsn;
	bool exact;
	itr->curr_pos = vy_mem_tree_lower_bound(&itr->mem->tree,
						&tree_key, &exact);
	assert(exact);
}

/* }}} vy_mem_iterator support functions */

/* {{{ vy_mem_iterator API implementation */
/* TODO: move to c file and remove static keyword */

/**
 * Open the iterator.
 */
static void
vy_mem_iterator_open(struct vy_mem_iterator *itr, struct vy_mem *mem,
		     enum iterator_type iterator_type,
		     const struct vy_stmt *key, const int64_t *vlsn)
{
	itr->base.iface = &vy_mem_iterator_iface;

	assert(key != NULL);
	itr->mem = mem;

	itr->iterator_type = iterator_type;
	itr->key = key;
	itr->vlsn = vlsn;
	if (vy_stmt_part_count(key) == 0) {
		/* NULL key. change itr->iterator_type for simplification */
		itr->iterator_type = iterator_type == ITER_LT ||
				     iterator_type == ITER_LE ?
				     ITER_LE : ITER_GE;
	}

	itr->curr_pos = vy_mem_tree_invalid_iterator();
	itr->curr_stmt = NULL;
	itr->last_stmt = NULL;

	itr->search_started = false;
}

/*
 * Find the next record with different key as current and visible lsn.
 * @retval 0 Found
 * @retval 1 Not found
 */
static NODISCARD int
vy_mem_iterator_next_key_impl(struct vy_mem_iterator *itr)
{
	if (!itr->search_started)
		return vy_mem_iterator_start(itr);
	if (!itr->curr_stmt) /* End of search. */
		return 1;
	assert(!vy_mem_tree_iterator_is_invalid(&itr->curr_pos));
	vy_mem_iterator_check_version(itr);
	assert(itr->curr_stmt == vy_mem_iterator_curr_stmt(itr));
	struct key_def *key_def = itr->mem->key_def;
	struct tuple_format *format = itr->mem->format;

	const struct vy_stmt *prev_stmt = itr->curr_stmt;
	do {
		if (vy_mem_iterator_step(itr) != 0) {
			itr->curr_stmt = NULL;
			return 1;
		}
	} while (vy_stmt_compare(prev_stmt, itr->curr_stmt, format,
				 key_def) == 0);

	if (itr->iterator_type == ITER_EQ &&
	    vy_stmt_compare(itr->key, itr->curr_stmt, format, key_def) != 0) {
		itr->curr_stmt = NULL;
		return 1;
	}
	return vy_mem_iterator_find_lsn(itr);
}

/**
 * Find the next record with different key as current and visible lsn.
 * @retval 0 success or EOF (*ret == NULL)
 */
static NODISCARD int
vy_mem_iterator_next_key(struct vy_stmt_iterator *vitr, struct vy_stmt *in,
			 struct vy_stmt **ret)
{
	(void)in;
	assert(vitr->iface->next_key == vy_mem_iterator_next_key);
	struct vy_mem_iterator *itr = (struct vy_mem_iterator *) vitr;
	*ret = NULL;

	if (vy_mem_iterator_next_key_impl(itr) == 0)
		return vy_mem_iterator_copy_to(itr, ret);
	return 0;
}

/*
 * Find next (lower, older) record with the same key as current
 * @retval 0 Found
 * @retval 1 Not found
 */
static NODISCARD int
vy_mem_iterator_next_lsn_impl(struct vy_mem_iterator *itr)
{
	if (!itr->search_started)
		return vy_mem_iterator_start(itr);
	if (!itr->curr_stmt) /* End of search. */
		return 1;
	assert(!vy_mem_tree_iterator_is_invalid(&itr->curr_pos));
	vy_mem_iterator_check_version(itr);
	assert(itr->curr_stmt == vy_mem_iterator_curr_stmt(itr));
	struct key_def *key_def = itr->mem->key_def;

	struct vy_mem_tree_iterator next_pos = itr->curr_pos;
	vy_mem_tree_iterator_next(&itr->mem->tree, &next_pos);
	if (vy_mem_tree_iterator_is_invalid(&next_pos))
		return 1; /* EOF */

	const struct vy_stmt *next_stmt;
	next_stmt = *vy_mem_tree_iterator_get_elem(&itr->mem->tree, &next_pos);
	if (vy_stmt_compare(itr->curr_stmt, next_stmt, itr->mem->format,
			    key_def) == 0) {
		itr->curr_pos = next_pos;
		itr->curr_stmt = next_stmt;
		return 0;
	}
	return 1;
}

/**
 * Find next (lower, older) record with the same key as current
 * @retval 0 success or EOF (*ret == NULL)
 */
static NODISCARD int
vy_mem_iterator_next_lsn(struct vy_stmt_iterator *vitr, struct vy_stmt *in,
			 struct vy_stmt **ret)
{
	(void)in;
	assert(vitr->iface->next_lsn == vy_mem_iterator_next_lsn);
	struct vy_mem_iterator *itr = (struct vy_mem_iterator *) vitr;
	*ret = NULL;
	if (vy_mem_iterator_next_lsn_impl(itr) == 0)
		return vy_mem_iterator_copy_to(itr, ret);
	return 0;
}

/**
 * Restore the current position (if necessary).
 * @sa struct vy_stmt_iterator comments.
 *
 * @param last_stmt the key the iterator was positioned on
 *
 * @retval 0 nothing changed
 * @retval 1 iterator position was changed
 */
static NODISCARD int
vy_mem_iterator_restore(struct vy_stmt_iterator *vitr,
			const struct vy_stmt *last_stmt, struct vy_stmt **ret)
{
	struct vy_mem_iterator *itr = (struct vy_mem_iterator *) vitr;
	struct key_def *def = itr->mem->key_def;
	struct tuple_format *format = itr->mem->format;
	int rc;
	*ret = NULL;

	if (!itr->search_started) {
		if (last_stmt == NULL) {
			if (vy_mem_iterator_start(itr) == 0)
				return vy_mem_iterator_copy_to(itr, ret);
			return 0;
		}

		/*
		 * Restoration is very similar to first search so we'll use
		 * that.
		 */
		enum iterator_type save_type = itr->iterator_type;
		const struct vy_stmt *save_key = itr->key;
		if (itr->iterator_type == ITER_GT ||
		    itr->iterator_type == ITER_EQ)
			itr->iterator_type = ITER_GE;
		else if (itr->iterator_type == ITER_LT)
			itr->iterator_type = ITER_LE;
		itr->key = last_stmt;
		rc = vy_mem_iterator_start(itr);
		itr->iterator_type = save_type;
		itr->key = save_key;
		if (rc > 0) /* Search ended. */
			return 0;
		bool position_changed = true;
		if (vy_stmt_compare(itr->curr_stmt, last_stmt, format,
				    def) == 0) {
			position_changed = false;
			if (itr->curr_stmt->lsn >= last_stmt->lsn) {
				/*
				 * Skip the same stmt to next stmt or older
				 * version.
				 */
				do {
					rc = vy_mem_iterator_next_lsn_impl(itr);
					if (rc == 0) /* Move further. */
						continue;
					assert(rc > 0);
					rc = vy_mem_iterator_next_key_impl(itr);
					assert(rc >= 0);
					break;
				} while (itr->curr_stmt->lsn >=
					 last_stmt->lsn);
				if (itr->curr_stmt != NULL)
					position_changed = true;
			}
		} else if (itr->iterator_type == ITER_EQ &&
			   vy_stmt_compare(itr->key, itr->curr_stmt, format,
					   def) != 0) {
			return true;
		}
		if (itr->curr_stmt != NULL &&
		    vy_mem_iterator_copy_to(itr, ret) < 0)
			return -1;
		return position_changed;
	}

	if (itr->version == itr->mem->version) {
		if (itr->curr_stmt)
			return vy_mem_iterator_copy_to(itr, ret);
		return 0;
	}

	if (last_stmt == NULL || itr->curr_stmt == NULL) {
		itr->version = itr->mem->version;
		const struct vy_stmt *was_stmt = itr->curr_stmt;
		itr->search_started = false;
		itr->curr_stmt = NULL;
		vy_mem_iterator_start(itr);
		return was_stmt != itr->curr_stmt;
	}

	vy_mem_iterator_check_version(itr);
	struct vy_mem_tree_iterator pos = itr->curr_pos;
	rc = 0;
	if (itr->iterator_type == ITER_GE || itr->iterator_type == ITER_GT ||
	    itr->iterator_type == ITER_EQ) {
		while (true) {
			vy_mem_tree_iterator_prev(&itr->mem->tree, &pos);
			if (vy_mem_tree_iterator_is_invalid(&pos))
				break;
			const struct vy_stmt *t;
			t = *vy_mem_tree_iterator_get_elem(&itr->mem->tree,
							   &pos);
			int cmp;
			cmp = vy_stmt_compare(t, last_stmt, format, def);
			if (cmp < 0 || (cmp == 0 && t->lsn >= last_stmt->lsn))
				break;
			if (t->lsn <= *itr->vlsn) {
				itr->curr_pos = pos;
				itr->curr_stmt = t;
				rc = 1;
			}
		}
		if (vy_mem_iterator_copy_to(itr, ret) < 0)
			return -1;
		return rc;
	}
	assert(itr->iterator_type == ITER_LE || itr->iterator_type == ITER_LT);
	int cmp;
	cmp = vy_stmt_compare(itr->curr_stmt, last_stmt, format, def);
	int64_t break_lsn = cmp == 0 ? last_stmt->lsn : *itr->vlsn + 1;
	while (true) {
		vy_mem_tree_iterator_prev(&itr->mem->tree, &pos);
		if (vy_mem_tree_iterator_is_invalid(&pos))
			break;
		const struct vy_stmt *t;
		t = *vy_mem_tree_iterator_get_elem(&itr->mem->tree, &pos);
		int cmp;
		cmp = vy_stmt_compare(t, itr->curr_stmt, format, def);
		assert(cmp <= 0);
		if (cmp < 0 || t->lsn >= break_lsn)
			break;
		itr->curr_pos = pos;
		itr->curr_stmt = t;
		rc = 1;
	}
	if (vy_mem_iterator_copy_to(itr, ret) < 0)
		return -1;
	return rc;
}

/**
 * Close an iterator and free all resources
 */
static void
vy_mem_iterator_close(struct vy_stmt_iterator *vitr)
{
	assert(vitr->iface->close == vy_mem_iterator_close);
	struct vy_mem_iterator *itr = (struct vy_mem_iterator *) vitr;
	if (itr->last_stmt != NULL)
		vy_stmt_unref(itr->last_stmt);
	TRASH(itr);
}

static struct vy_stmt_iterator_iface vy_mem_iterator_iface = {
	.next_key = vy_mem_iterator_next_key,
	.next_lsn = vy_mem_iterator_next_lsn,
	.restore = vy_mem_iterator_restore,
	.close = vy_mem_iterator_close
};

/* }}} vy_mem_iterator API implementation */

/* {{{ Iterator over transaction writes : forward declaration */

/**
 * Return statements from the write set of the current
 * transactions.
 *
 * @sa vy_run_iterator, vy_mem_iterator, with which
 * this iterator shares the interface.
 */
struct vy_txw_iterator {
	/** Parent class, must be the first member */
	struct vy_stmt_iterator base;

	struct vy_index *index;
	struct vy_tx *tx;

	/* Search options */
	/**
	 * Iterator type, that specifies direction, start position and stop
	 * criteria if key == NULL: GT and EQ are changed to GE, LT to LE for
	 * beauty.
	 */
	enum iterator_type iterator_type;
	/** Key to search. */
	const struct vy_stmt *key;

	/* Last version of vy_tx */
	uint32_t version;
	/* Current pos in txw tree */
	struct txv *curr_txv;
	/* Is false until first .._get ot .._next_.. method is called */
	bool search_started;
};

static void
vy_txw_iterator_open(struct vy_txw_iterator *itr,
		     struct vy_index *index, struct vy_tx *tx,
		     enum iterator_type iterator_type,
		     const struct vy_stmt *key);

static void
vy_txw_iterator_close(struct vy_stmt_iterator *vitr);

/* }}} Iterator over transaction writes : forward declaration */

/* {{{ Iterator over transaction writes : implementation */

/** Vtable for vy_stmt_iterator - declared below */
static struct vy_stmt_iterator_iface vy_txw_iterator_iface;

/* Open the iterator. */
static void
vy_txw_iterator_open(struct vy_txw_iterator *itr,
		     struct vy_index *index, struct vy_tx *tx,
		     enum iterator_type iterator_type,
		     const struct vy_stmt *key)
{
	itr->base.iface = &vy_txw_iterator_iface;

	itr->index = index;
	itr->tx = tx;

	itr->iterator_type = iterator_type;
	if (vy_stmt_part_count(key) == 0) {
		/* NULL key. change itr->iterator_type for simplification */
		itr->iterator_type = iterator_type == ITER_LT ||
				     iterator_type == ITER_LE ?
				     ITER_LE : ITER_GE;
	}
	itr->key = key;

	itr->version = UINT32_MAX;
	itr->curr_txv = NULL;
	itr->search_started = false;
}

/**
 * Find position in write set of transaction. Used once in first call of
 *  get/next.
 */
static void
vy_txw_iterator_start(struct vy_txw_iterator *itr, struct vy_stmt **ret)
{
	*ret = NULL;
	itr->search_started = true;
	itr->version = itr->tx->write_set_version;
	itr->curr_txv = NULL;
	struct write_set_key key = { itr->index, itr->key };
	struct txv *txv;
	struct key_def *key_def = itr->index->key_def;
	struct tuple_format *format = itr->index->format;
	if (vy_stmt_part_count(itr->key) > 0) {
		if (itr->iterator_type == ITER_EQ)
			txv = write_set_search(&itr->tx->write_set, &key);
		else if (itr->iterator_type == ITER_GE ||
			 itr->iterator_type == ITER_GT)
			txv = write_set_nsearch(&itr->tx->write_set, &key);
		else
			txv = write_set_psearch(&itr->tx->write_set, &key);
		if (txv == NULL || txv->index != itr->index)
			return;
		if (vy_stmt_compare(itr->key, txv->stmt, format,
				    key_def) == 0) {
			while (true) {
				struct txv *next;
				if (itr->iterator_type == ITER_LE ||
				    itr->iterator_type == ITER_GT)
					next = write_set_next(&itr->tx->write_set, txv);
				else
					next = write_set_prev(&itr->tx->write_set, txv);
				if (next == NULL || next->index != itr->index)
					break;
				if (vy_stmt_compare(itr->key, next->stmt,
						    format, key_def) != 0)
					break;
				txv = next;
			}
			if (itr->iterator_type == ITER_GT)
				txv = write_set_next(&itr->tx->write_set, txv);
			else if (itr->iterator_type == ITER_LT)
				txv = write_set_prev(&itr->tx->write_set, txv);
		}
	} else if (itr->iterator_type == ITER_LE) {
		key.index = (struct vy_index *)((uintptr_t)key.index + 1);
		txv = write_set_psearch(&itr->tx->write_set, &key);
	} else {
		assert(itr->iterator_type == ITER_GE);
		txv = write_set_nsearch(&itr->tx->write_set, &key);
	}
	if (txv == NULL || txv->index != itr->index)
		return;
	itr->curr_txv = txv;
	*ret = txv->stmt;
	return;
}

/**
 * Move to next stmt
 * @retval 0 success or EOF (*ret == NULL)
 */
static NODISCARD int
vy_txw_iterator_next_key(struct vy_stmt_iterator *vitr, struct vy_stmt *in,
			 struct vy_stmt **ret)
{
	(void)in;
	assert(vitr->iface->next_key == vy_txw_iterator_next_key);
	struct vy_txw_iterator *itr = (struct vy_txw_iterator *) vitr;
	*ret = NULL;

	if (!itr->search_started) {
		vy_txw_iterator_start(itr, ret);
		return 0;
	}
	itr->version = itr->tx->write_set_version;
	if (itr->curr_txv == NULL)
		return 0;
	if (itr->iterator_type == ITER_LE || itr->iterator_type == ITER_LT)
		itr->curr_txv = write_set_prev(&itr->tx->write_set, itr->curr_txv);
	else
		itr->curr_txv = write_set_next(&itr->tx->write_set, itr->curr_txv);
	if (itr->curr_txv != NULL && itr->curr_txv->index != itr->index)
		itr->curr_txv = NULL;
	if (itr->curr_txv != NULL && itr->iterator_type == ITER_EQ &&
	    vy_stmt_compare(itr->key, itr->curr_txv->stmt, itr->index->format,
	    		    itr->index->key_def) != 0)
		itr->curr_txv = NULL;
	if (itr->curr_txv != NULL)
		*ret = itr->curr_txv->stmt;
	return 0;
}

/**
 * Function for compatibility with run/mem iterators.
 * @retval 0 EOF always
 */
static NODISCARD int
vy_txw_iterator_next_lsn(struct vy_stmt_iterator *vitr, struct vy_stmt *in,
			 struct vy_stmt **ret)
{
	assert(vitr->iface->next_lsn == vy_txw_iterator_next_lsn);
	(void)vitr;
	(void)in;
	*ret = NULL;
	return 0;
}

/**
 * Restore iterator position after some changes in write set. Iterator
 *  position is placed to the next position after last_stmt.
 * @sa struct vy_stmt_iterator comments.
 *
 * Can restore iterator that was out of data previously
 * @retval 0 nothing significant was happend and itr position left the same
 * @retval 1 iterator restored and position changed
 */
static int
vy_txw_iterator_restore(struct vy_stmt_iterator *vitr,
			const struct vy_stmt *last_stmt, struct vy_stmt **ret)
{
	assert(vitr->iface->restore == vy_txw_iterator_restore);
	struct vy_txw_iterator *itr = (struct vy_txw_iterator *) vitr;
	*ret = NULL;

	if (!itr->search_started && last_stmt == NULL) {
		vy_txw_iterator_start(itr, ret);
		return 0;
	}
	if (last_stmt == NULL || itr->version == itr->tx->write_set_version) {
		if (itr->curr_txv)
			*ret = itr->curr_txv->stmt;
		return 0;
	}

	itr->search_started = true;
	itr->version = itr->tx->write_set_version;
	struct write_set_key key = { itr->index, last_stmt };
	struct vy_stmt *was_stmt = itr->curr_txv != NULL ?
				     itr->curr_txv->stmt : NULL;
	itr->curr_txv = NULL;
	struct txv *txv;
	struct key_def *def = itr->index->key_def;
	struct tuple_format *format = itr->index->format;
	if (itr->iterator_type == ITER_LE || itr->iterator_type == ITER_LT)
		txv = write_set_psearch(&itr->tx->write_set, &key);
	else
		txv = write_set_nsearch(&itr->tx->write_set, &key);
	if (txv != NULL && txv->index == itr->index &&
	    vy_stmt_compare(txv->stmt, last_stmt, format, def) == 0) {
		if (itr->iterator_type == ITER_LE ||
		    itr->iterator_type == ITER_LT)
			txv = write_set_prev(&itr->tx->write_set, txv);
		else
			txv = write_set_next(&itr->tx->write_set, txv);
	}
	if (txv != NULL && txv->index == itr->index &&
	    itr->iterator_type == ITER_EQ &&
	    vy_stmt_compare(itr->key, txv->stmt, format, def) != 0)
		txv = NULL;
	if (txv == NULL || txv->index != itr->index) {
		assert(was_stmt == NULL);
		return 0;
	}
	itr->curr_txv = txv;
	*ret = txv->stmt;
	return txv->stmt != was_stmt;
}

/**
 * Close the iterator
 */
static void
vy_txw_iterator_close(struct vy_stmt_iterator *vitr)
{
	assert(vitr->iface->close == vy_txw_iterator_close);
	struct vy_txw_iterator *itr = (struct vy_txw_iterator *) vitr;
	(void)itr; /* suppress warn if NDEBUG */
	TRASH(itr);
}

static struct vy_stmt_iterator_iface vy_txw_iterator_iface = {
	.next_key = vy_txw_iterator_next_key,
	.next_lsn = vy_txw_iterator_next_lsn,
	.restore = vy_txw_iterator_restore,
	.close = vy_txw_iterator_close
};

/* }}} Iterator over transaction writes : implementation */

/* {{{ Merge iterator */

/**
 * Merge source, support structure for vy_merge_iterator
 * Contains source iterator, additional properties and merge state
 */
struct vy_merge_src {
	/** Source iterator */
	union {
		struct vy_run_iterator run_iterator;
		struct vy_mem_iterator mem_iterator;
		struct vy_txw_iterator txw_iterator;
		struct vy_stmt_iterator iterator;
	};
	/** Source can change during merge iteration */
	bool is_mutable;
	/** Source belongs to a range (@sa vy_merge_iterator comments). */
	bool belong_range;
	/**
	 * All sources with the same front_id as in struct
	 * vy_merge_iterator are on the same key of current output
	 * stmt (optimization)
	 */
	uint32_t front_id;
	struct vy_stmt *stmt;
};

/**
 * Open the iterator.
 */
static void
vy_merge_iterator_open(struct vy_merge_iterator *itr, struct vy_index *index,
		       enum iterator_type iterator_type,
		       const struct vy_stmt *key)
{
	assert(key != NULL);
	itr->index = index;
	itr->index_version = 0;
	itr->curr_range = NULL;
	itr->range_version = 0;
	itr->key = key;
	itr->iterator_type = iterator_type;
	itr->src = NULL;
	itr->src_count = 0;
	itr->src_capacity = 0;
	itr->src = NULL;
	itr->curr_src = UINT32_MAX;
	itr->front_id = 1;
	itr->mutable_start = 0;
	itr->mutable_end = 0;
	itr->curr_stmt = NULL;
	itr->unique_optimization =
		(iterator_type == ITER_EQ || iterator_type == ITER_GE ||
		 iterator_type == ITER_LE) &&
		vy_stmt_part_count(key) >= index->key_def->part_count;
	itr->is_in_uniq_opt = false;
	itr->search_started = false;
	itr->range_ended = false;
}

/**
 * Close the iteator and free resources.
 */
static void
vy_merge_iterator_close(struct vy_merge_iterator *itr)
{
	if (itr->curr_stmt != NULL) {
		vy_stmt_unref(itr->curr_stmt);
		itr->curr_stmt = NULL;
	}
	for (size_t i = 0; i < itr->src_count; i++)
		itr->src[i].iterator.iface->close(&itr->src[i].iterator);
	free(itr->src);
	itr->src_count = 0;
	itr->src_capacity = 0;
	itr->src = NULL;
	itr->curr_range = NULL;
	itr->range_version = 0;
	itr->index = NULL;
	itr->index_version = 0;
}

/**
 * Extend internal source array capacity to fit capacity sources.
 * Not necessary to call is but calling it allows to optimize internal memory
 * allocation
 */
static NODISCARD int
vy_merge_iterator_reserve(struct vy_merge_iterator *itr, uint32_t capacity)
{
	if (itr->src_capacity >= capacity)
		return 0;
	struct vy_merge_src *new_src = calloc(capacity, sizeof(*new_src));
	if (new_src == NULL) {
		diag_set(OutOfMemory, capacity * sizeof(*new_src),
			 "calloc", "new_src");
		return -1;
	}
	if (itr->src_count > 0) {
		memcpy(new_src, itr->src, itr->src_count * sizeof(*new_src));
		free(itr->src);
	}
	itr->src = new_src;
	itr->src_capacity = capacity;
	return 0;
}

/**
 * Add another source to merge iterator. Must be called before actual
 * iteration start and must not be called after.
 * @sa necessary order of adding requirements in struct vy_merge_iterator
 * comments.
 * The resulting vy_stmt_iterator must be properly initialized before merge
 * iteration start.
 * param is_mutable - Source can change during merge iteration
 * param belong_range - Source belongs to a range (see vy_merge_iterator comments)
 */
static struct vy_merge_src *
vy_merge_iterator_add(struct vy_merge_iterator *itr,
		      bool is_mutable, bool belong_range)
{
	assert(!itr->search_started);
	if (itr->src_count == itr->src_capacity) {
		if (vy_merge_iterator_reserve(itr, itr->src_count + 1) != 0)
			return NULL;
	}
	if (is_mutable) {
		if (itr->mutable_start == itr->mutable_end)
			itr->mutable_start = itr->src_count;
		itr->mutable_end = itr->src_count + 1;
	}
	itr->src[itr->src_count].front_id = 0;
	struct vy_merge_src *src = &itr->src[itr->src_count++];
	src->is_mutable = is_mutable;
	src->belong_range = belong_range;
	return src;
}

/*
 * Enable version checking.
 */
static void
vy_merge_iterator_set_version(struct vy_merge_iterator *itr,
			      struct vy_range *range)
{
	itr->curr_range = range;
	itr->range_version = range != NULL ? range->version : 0;
	itr->index_version = itr->index->version;
}

/*
 * Try to restore position of merge iterator
 * @retval 0	if position did not change (iterator started)
 * @retval -2	iterator is no more valid
 */
static NODISCARD int
vy_merge_iterator_check_version(struct vy_merge_iterator *itr)
{
	if (!itr->index_version)
		return 0; /* version checking is off */

	assert(itr->curr_range != NULL);
	if (itr->index_version == itr->index->version &&
	    itr->curr_range->version == itr->range_version)
		return 0;

	return -2; /* iterator is not valid anymore */
}

/**
 * Move all source iterating positioned to equal to current stmt (previous
 * result of get) to the next position
 * return 0 : OK
 * return -1 : read error
 * return -2 : iterator is not valid anymore
 */
static NODISCARD int
vy_merge_iterator_propagate(struct vy_merge_iterator *itr)
{
	for (uint32_t i = 0; i < itr->src_count; i++) {
		if (vy_merge_iterator_check_version(itr))
			return -2;
		struct vy_merge_src *src = &itr->src[i];
		if (src->front_id != itr->front_id)
			continue;
		int rc = src->iterator.iface->next_key(&itr->src[i].iterator,
						       itr->curr_stmt,
						       &itr->src[i].stmt);
		if (rc != 0)
			return rc;
	}
	itr->front_id++;
	if (vy_merge_iterator_check_version(itr))
		return -2;
	return 0;
}

/**
 * Same as vy_merge_iterator_locate but optimized for first get in unique
 * index with will key given. See vy_merge_iterator::unique_optimization
 * member comment
 *
 * @retval 0 success or EOF (*ret == NULL)
 * @retval -1 error
 * @retval -2 invalid iterator
 */
static NODISCARD int
vy_merge_iterator_locate_uniq_opt(struct vy_merge_iterator *itr,
				  struct vy_stmt **ret)
{
	assert(itr->src_count);
	*ret = NULL;
	itr->range_ended = false;
	itr->search_started = true;
	itr->unique_optimization = false;
	struct vy_stmt *min_stmt;
	int order = (itr->iterator_type == ITER_LE ||
		     itr->iterator_type == ITER_LT ? -1 : 1);
restart:
	itr->is_in_uniq_opt = false;
	min_stmt = NULL;
	itr->curr_src = UINT32_MAX;
	struct key_def *def = itr->index->key_def;
	struct tuple_format *format = itr->index->format;
	for (uint32_t i = 0; i < itr->src_count; i++) {
		if (vy_merge_iterator_check_version(itr))
			return -2;
		struct vy_merge_src *src = &itr->src[i];
		struct vy_stmt_iterator *sub_itr = &src->iterator;
		struct vy_stmt *t;
		/*
		 * If the tuple of the current source is not set then
		 * either EOF reached or the source iterator is not started.
		 * In the second case start it by the first call of next_key.
		 */
		if (src->stmt == NULL) {
			int rc;
			rc = sub_itr->iface->next_key(sub_itr, itr->curr_stmt,
						      &src->stmt);
			if (rc != 0)
				return rc;
		}
		t = src->stmt;
		if (t == NULL)
			continue;
		if (vy_stmt_compare(itr->key, t, format, def) == 0) {
			src->front_id = ++itr->front_id;
			min_stmt = t;
			vy_stmt_ref(t);
			itr->curr_src = i;
			itr->is_in_uniq_opt = true;
			break;
		}
		int cmp = min_stmt == NULL ? -1 :
			  order * vy_stmt_compare(t, min_stmt, format, def);
		if (cmp == 0) {
			src->front_id = itr->front_id;
		} else if (cmp < 0) {
			src->front_id = ++itr->front_id;
			if (min_stmt != NULL)
				vy_stmt_unref(min_stmt);
			min_stmt = t;
			vy_stmt_ref(t);
			itr->curr_src = i;
		}
	}
	bool must_restart = false;
	for (uint32_t i = itr->mutable_start; i < itr->mutable_end; i++) {
		if (vy_merge_iterator_check_version(itr))
			return -2;
		struct vy_stmt_iterator *sub_itr = &itr->src[i].iterator;
		int rc = sub_itr->iface->restore(sub_itr, NULL, &itr->src[i].stmt);
		if (rc < 0)
			return rc;
		if (rc > 0)
			must_restart = true;
	}
	if (must_restart)
		goto restart;
	if (itr->curr_stmt != NULL)
		vy_stmt_unref(itr->curr_stmt);
	itr->curr_stmt = min_stmt;
	*ret = min_stmt;
	if (vy_merge_iterator_check_version(itr))
		return -2;
	return 0;
}

/**
 * Find minimal stmt from all the sources, mark all sources with stmt equal
 * to the minimum with specific front_id equal to itr->front_id.
 * Guaranteed that all other sources will have different front_id.
 *
 * @retval 0 : success or EOF
 * @retval -1 : read error
 * @retval -2 : iterator is not valid anymore
 */
static NODISCARD int
vy_merge_iterator_locate(struct vy_merge_iterator *itr,
			 struct vy_stmt **ret)
{
	*ret = NULL;
	if (itr->src_count == 0)
		return 0;
	if (itr->unique_optimization)
		return vy_merge_iterator_locate_uniq_opt(itr, ret);
	itr->search_started = true;
	struct vy_stmt *min_stmt = NULL;
	itr->curr_src = UINT32_MAX;
	itr->range_ended = true;
	int order = (itr->iterator_type == ITER_LE ||
		     itr->iterator_type == ITER_LT ? -1 : 1);
	for (uint32_t i = itr->src_count; i--;) {
		if (vy_merge_iterator_check_version(itr))
			return -2;
		struct vy_merge_src *src = &itr->src[i];
		struct vy_stmt_iterator *sub_itr = &src->iterator;
		struct vy_stmt *t;
		int rc = 0;
		if (src->is_mutable) {
			rc = sub_itr->iface->restore(sub_itr, itr->curr_stmt,
						     &src->stmt);
			if (rc < 0)
				return rc;
			rc = 0;
			if (vy_merge_iterator_check_version(itr))
				return -2;
		} else if (src->stmt == NULL) {
			rc = sub_itr->iface->next_key(sub_itr, itr->curr_stmt,
						      &src->stmt);
		}
		if (rc != 0)
			return rc;
		t = src->stmt;
		if (t == NULL)
			continue;
		itr->range_ended = itr->range_ended && !src->belong_range;
		int cmp = min_stmt == NULL ? -1 :
			order * vy_stmt_compare(t, min_stmt, itr->index->format,
						itr->index->key_def);
		if (cmp <= 0) {
			itr->front_id += cmp < 0;
			src->front_id = itr->front_id;
			min_stmt = t;
			itr->curr_src = i;
		}
	}
	if (itr->curr_stmt != NULL)
		vy_stmt_unref(itr->curr_stmt);
	itr->curr_stmt = min_stmt;
	if (itr->curr_stmt != NULL)
		vy_stmt_ref(itr->curr_stmt);
	*ret = min_stmt;
	if (vy_merge_iterator_check_version(itr))
		return -2;
	return 0;
}

/**
 * Iterate to the next key
 * @retval 0 success or EOF (*ret == NULL)
 * @retval -1 read error
 * @retval -2 iterator is not valid anymore
 */
static NODISCARD int
vy_merge_iterator_next_key(struct vy_merge_iterator *itr, struct vy_stmt *in,
			   struct vy_stmt **ret)
{
	(void)in;
	int rc;
	*ret = NULL;
	if (!itr->search_started)
		return vy_merge_iterator_locate(itr, ret);
	if (itr->is_in_uniq_opt) {
		itr->is_in_uniq_opt = false;
		rc = vy_merge_iterator_locate(itr, ret);
		if (rc != 0)
			return rc;
	}
	rc = vy_merge_iterator_propagate(itr);
	if (rc != 0)
		return rc;
	return vy_merge_iterator_locate(itr, ret);
}

/**
 * Iterate to the next (elder) version of the same key
 * @retval 0 success or EOF (*ret == NULL)
 * @retval -1 read error
 * @retval -2 iterator is not valid anymore
 */
static NODISCARD int
vy_merge_iterator_next_lsn(struct vy_merge_iterator *itr, struct vy_stmt *in,
			   struct vy_stmt **ret)
{
	(void)in;
	int rc;
	*ret = NULL;
	if (!itr->search_started)
		return vy_merge_iterator_locate(itr, ret);
	if (itr->curr_src == UINT32_MAX)
		return 0;
	struct vy_stmt_iterator *sub_itr = &itr->src[itr->curr_src].iterator;
	rc = sub_itr->iface->next_lsn(sub_itr, itr->curr_stmt,
				      &itr->src[itr->curr_src].stmt);
	if (rc != 0) {
		return rc;
	} else if (itr->src[itr->curr_src].stmt) {
		if (vy_merge_iterator_check_version(itr))
			return -2;
		if (itr->curr_stmt != NULL)
			vy_stmt_unref(itr->curr_stmt);
		itr->curr_stmt = itr->src[itr->curr_src].stmt;
		vy_stmt_ref(itr->curr_stmt);
		*ret = itr->curr_stmt;
		return 0;
	}
	for (uint32_t i = itr->curr_src + 1; i < itr->src_count; i++) {
		if (vy_merge_iterator_check_version(itr))
			return -2;
		if (itr->is_in_uniq_opt) {
			sub_itr = &itr->src[i].iterator;
			struct vy_stmt *t;
			t = itr->src[i].stmt;
			if (t == NULL) {
				rc = sub_itr->iface->next_lsn(sub_itr, itr->curr_stmt,
							      &itr->src[i].stmt);
				if (rc != 0)
					return rc;
				if (itr->src[i].stmt == NULL)
					continue;
				t = itr->src[i].stmt;
			}
			if (vy_stmt_compare(itr->key, t, itr->index->format,
					    itr->index->key_def) == 0) {
				itr->src[i].front_id = itr->front_id;
				itr->curr_src = i;
				if (itr->curr_stmt != NULL)
					vy_stmt_unref(itr->curr_stmt);
				itr->curr_stmt = t;
				vy_stmt_ref(t);
				*ret = t;
				return 0;
			}

		} else if (itr->src[i].front_id == itr->front_id) {
			sub_itr = &itr->src[i].iterator;
			itr->curr_src = i;
			if (itr->curr_stmt != NULL) {
				vy_stmt_unref(itr->curr_stmt);
				itr->curr_stmt = NULL;
			}
			itr->curr_stmt = itr->src[i].stmt;
			vy_stmt_ref(itr->curr_stmt);
			*ret = itr->curr_stmt;
			return 0;
		}
	}
	itr->is_in_uniq_opt = false;
	return 0;
}

/**
 * Squash in the single statement all rest statements of current key
 * starting from the current statement.
 *
 * @retval 0 success or EOF (*ret == NULL)
 * @retval -1 error
 * @retval -2 invalid iterator
 */
static NODISCARD int
vy_merge_iterator_squash_upsert(struct vy_merge_iterator *itr,
				struct vy_stmt *in,
				struct vy_stmt **ret,
				bool suppress_error)
{
	*ret = NULL;
	struct vy_stmt *t = itr->curr_stmt;
	struct key_def *def = itr->index->key_def;
	struct tuple_format *format = itr->index->format;
	if (t == NULL)
		return 0;
	vy_stmt_ref(t);
	while (t->type == IPROTO_UPSERT) {
		struct vy_stmt *next;
		int rc = vy_merge_iterator_next_lsn(itr, in, &next);
		if (rc != 0) {
			vy_stmt_unref(t);
			return rc;
		}
		if (next == NULL)
			break;
		struct vy_stmt *applied;
		applied = vy_apply_upsert(t, next, def, format, suppress_error);
		vy_stmt_unref(t);
		if (applied == NULL)
			return -1;
		t = applied;
	}
	*ret = t;
	return 0;
}

/**
 * Restore the position of merge iterator after the given key
 * and according to the initial retrieval order.
 */
static NODISCARD int
vy_merge_iterator_restore(struct vy_merge_iterator *itr,
			  const struct vy_stmt *last_stmt)
{
	itr->unique_optimization = false;
	itr->is_in_uniq_opt = false;
	int result = 0;
	for (uint32_t i = 0; i < itr->src_count; i++) {
		struct vy_stmt_iterator *sub_itr = &itr->src[i].iterator;
		int rc = sub_itr->iface->restore(sub_itr, last_stmt,
						 &itr->src[i].stmt);
		if (rc < 0)
			return rc;
		result = result || rc;
	}
	return result;
}

/* }}} Merge iterator */

/* {{{ Write iterator */

/**
 * Iterate over an in-memory index when writing it to disk (dump)
 * or over a series of sorted runs on disk to create a new sorted
 * run (compaction).
 *
 * Use merge iterator to order the output and filter out
 * too old statements (older than the oldest active read view).
 *
 * Squash multiple UPSERT statements over the same key into one,
 * if possible.
 *
 * Background
 * ----------
 * Vinyl provides support for consistent read views. The oldest
 * active read view is maintained in the transaction manager.
 * To support it, when dumping or compacting statements on disk,
 * older versions need to be preserved, and versions outside
 * any active read view garbage collected. This task is handled
 * by the write iterator.
 *
 * Filtering
 * ---------
 * Let's call each transaction consistent read view LSN vlsn.
 *
 *	oldest_vlsn = MIN(vlsn) over all active transactions
 *
 * Thus to preserve relevant data for every key and purge old
 * versions, the iterator works as follows:
 *
 *      If statement lsn is greater than oldest vlsn, the
 *      statement is preserved.
 *
 *      Otherwise, if statement type is REPLACE/DELETE, then
 *      it's returned, and the iterator can proceed to the
 *      next key: the readers do not need the history.
 *
 *      Otherwise, the statement is UPSERT, and in order
 *      to restore the original tuple from UPSERT the reader
 *      does need the history: they need to look for an older
 *      statement to which the UPSERT can be applied to get
 *      a tuple. This older statement can be UPSERT as well,
 *      and so on.
 *	In other words, of statement type is UPSERT, the reader
 *	needs a range of statements from the youngest statement
 *	with lsn <= vlsn to the youngest non-UPSERT statement
 *	with lsn <= vlsn, borders included.
 *
 *	All other versions of this key can be skipped, and hence
 *	garbage collected.
 *
 * Squashing and garbage collection
 * --------------------------------
 * Filtering and garbage collection, performed by write iterator,
 * must have no effect on read views of active transactions:
 * they should read the same data as before.
 *
 * On the other hand, old version should be deleted as soon as possible;
 * multiple UPSERTs could be merged together to take up less
 * space, or substituted with REPLACE.
 *
 * Here's how it's done:
 *
 *
 *	1) Every statement with lsn greater than oldest vlsn is preserved
 *	in the output, since there could be an active transaction
 *	that needs it.
 *
 *	2) For all statements with lsn <= oldest_vlsn, only a single
 *	resultant statement is returned. Here's how.
 *
 *	2.1) If the youngest statement with lsn <= oldest _vlsn is a
 *	REPLACE/DELETE, it becomes the resultant statement.
 *
 *	2.2) Otherwise, it as an UPSERT. Then we must iterate over
 *	all older LSNs for this key until we find a REPLACE/DELETE
 *	or exhaust all input streams for this key.
 *
 *	If the older lsn is a yet another UPSERT, two upserts are
 *	squashed together into one. Otherwise we found an
 *	REPLACE/DELETE, so apply all preceding UPSERTs to it and
 *	get the resultant statement.
 *
 * There is an extra twist to this algorithm, used when performing
 * compaction of the last LSM level (i.e. merging all existing
 * runs into one). The last level does not need to store DELETEs.
 * Thus we can:
 * 1) Completely skip the resultant statement from output if it's
 *    a DELETE.
 *     ┃      ...      ┃       ┃     ...      ┃
 *     ┃               ┃       ┃              ┃    ↑
 *     ┣━ oldest vlsn ━┫   =   ┣━ oldest lsn ━┫    ↑ lsn
 *     ┃               ┃       ┗━━━━━━━━━━━━━━┛    ↑
 *     ┃    DELETE     ┃
 *     ┃      ...      ┃
 * 2) Replace an accumulated resultant UPSERT with an appropriate
 *    REPLACE.
 *     ┃      ...      ┃       ┃     ...      ┃
 *     ┃     UPSERT    ┃       ┃   REPLACE    ┃    ↑
 *     ┃               ┃       ┃              ┃    ↑
 *     ┣━ oldest vlsn ━┫   =   ┣━ oldest lsn ━┫    ↑ lsn
 *     ┃               ┃       ┗━━━━━━━━━━━━━━┛    ↑
 *     ┃    DELETE     ┃
 *     ┃      ...      ┃
 */
struct vy_write_iterator {
	struct vy_index *index;
	/* The minimal VLSN among all active transactions */
	int64_t oldest_vlsn;
	/* There are is no level older than the one we're writing to. */
	bool is_last_level;
	/* On the next iteration we must move to the next key */
	bool goto_next_key;
	struct vy_stmt *key;
	struct vy_stmt *tmp_stmt;
	struct vy_merge_iterator mi;
};

/*
 * Open an empty write iterator. To add sources to the iterator
 * use vy_write_iterator_add_* functions
 */
static void
vy_write_iterator_open(struct vy_write_iterator *wi, struct vy_index *index,
		       bool is_last_level, int64_t oldest_vlsn)
{
	wi->index = index;
	wi->oldest_vlsn = oldest_vlsn;
	wi->is_last_level = is_last_level;
	wi->goto_next_key = false;
	wi->key = vy_stmt_new_select(NULL, 0);
	vy_merge_iterator_open(&wi->mi, index, ITER_GE, wi->key);
}

static struct vy_write_iterator *
vy_write_iterator_new(struct vy_index *index, bool is_last_level,
		      int64_t oldest_vlsn)
{
	struct vy_write_iterator *wi = calloc(1, sizeof(*wi));
	if (wi == NULL) {
		diag_set(OutOfMemory, sizeof(*wi), "calloc", "wi");
		return NULL;
	}
	vy_write_iterator_open(wi, index, is_last_level, oldest_vlsn);
	return wi;
}

static NODISCARD int
vy_write_iterator_add_run(struct vy_write_iterator *wi,
			  struct vy_range *range, struct vy_run *run)
{
	struct vy_merge_src *src;
	src = vy_merge_iterator_add(&wi->mi, false, false);
	if (src == NULL)
		return -1;
	static const int64_t vlsn = INT64_MAX;
	vy_run_iterator_open(&src->run_iterator, range, run,
			     ITER_GE, wi->key, &vlsn);
	return 0;
}

static NODISCARD int
vy_write_iterator_add_mem(struct vy_write_iterator *wi, struct vy_mem *mem)
{
	struct vy_merge_src *src;
	src = vy_merge_iterator_add(&wi->mi, false, false);
	if (src == NULL)
		return -1;
	static const int64_t vlsn = INT64_MAX;
	vy_mem_iterator_open(&src->mem_iterator, mem,
			     ITER_GE, wi->key, &vlsn);
	return 0;
}

/**
 * The write iterator can return multiple LSNs for the same
 * key, thus next() will automatically switch to the next
 * key when it's appropriate.
 *
 * The user of the write iterator simply expects a stream
 * of statements to write to the output.
 */
static NODISCARD int
vy_write_iterator_next(struct vy_write_iterator *wi, struct vy_stmt **ret)
{
	/*
	 * Nullify the result stmt. If the next stmt is not
	 * found, this is a marker of the end of the stream.
	 */
	*ret = NULL;
	/*
	 * The write iterator guarantees that the returned stmt
	 * is alive until the next invocation of next(). If the
	 * returned stmt is obtained from the merge iterator,
	 * this guarantee is fulfilled by the merge iterator
	 * itself. If the write iterator creates the returned
	 * stmt, e.g. by squashing a bunch of upserts, then
	 * it must dereference the created stmt here.
	 */
	if (wi->tmp_stmt)
		vy_stmt_unref(wi->tmp_stmt);
	wi->tmp_stmt = NULL;
	struct vy_merge_iterator *mi = &wi->mi;
	struct vy_stmt *stmt = NULL;
	struct key_def *def = wi->index->key_def;
	struct tuple_format *format = wi->index->format;
	/* @sa vy_write_iterator declaration for the algorithm description. */
	while (true) {
		if (wi->goto_next_key) {
			wi->goto_next_key = false;
			if (vy_merge_iterator_next_key(mi, NULL, &stmt))
				return -1;
		} else {
			if (vy_merge_iterator_next_lsn(mi, NULL, &stmt))
				return -1;
			if (stmt == NULL &&
			    vy_merge_iterator_next_key(mi, NULL, &stmt))
				return -1;
		}
		if (stmt == NULL)
			return 0;
		if (stmt->lsn > wi->oldest_vlsn)
			break; /* Save the current stmt as the result. */
		wi->goto_next_key = true;
		if (stmt->type == IPROTO_DELETE && wi->is_last_level)
			continue; /* Skip unnecessary DELETE */
		if (stmt->type == IPROTO_REPLACE ||
		    stmt->type == IPROTO_DELETE)
			break; /* It's the resulting statement */

		/* Squash upserts */
		assert(stmt->type == IPROTO_UPSERT);
		if (vy_merge_iterator_squash_upsert(mi, NULL, &stmt, false)) {
			vy_stmt_unref(stmt);
			return -1;
		}
		if (stmt->type == IPROTO_UPSERT && wi->is_last_level) {
			/* Turn UPSERT to REPLACE. */
			struct vy_stmt *applied;
			applied = vy_apply_upsert(stmt, NULL, def, format,
						  false);
			vy_stmt_unref(stmt);
			if (applied == NULL)
				return -1;
			stmt = applied;
		}
		wi->tmp_stmt = stmt;
		break;
	}
	*ret = stmt;
	return 0;
}

static void
vy_write_iterator_close(struct vy_write_iterator *wi)
{
	if (wi->tmp_stmt) {
		vy_stmt_unref(wi->tmp_stmt);
	}
	wi->tmp_stmt = NULL;
	vy_merge_iterator_close(&wi->mi);
}

static void
vy_write_iterator_delete(struct vy_write_iterator *wi)
{
	vy_write_iterator_close(wi);
	vy_stmt_unref(wi->key);
	free(wi);
}

/* Write iterator }}} */

/* {{{ Iterator over index */

static void
vy_read_iterator_add_tx(struct vy_read_iterator *itr)
{
	assert(itr->tx != NULL);
	struct vy_merge_src *sub_src =
		vy_merge_iterator_add(&itr->merge_iterator, true, false);
	vy_txw_iterator_open(&sub_src->txw_iterator, itr->index, itr->tx,
			     itr->iterator_type, itr->key);
	vy_txw_iterator_restore(&sub_src->iterator, itr->curr_stmt,
				&sub_src->stmt);
}

static void
vy_read_iterator_add_mem(struct vy_read_iterator *itr)
{
	struct vy_range *range = itr->curr_range;
	struct vy_merge_src *sub_src;

	assert(range != NULL);
	assert(range->shadow == NULL);
	/*
	 * The range may be under compaction, in which case we must
	 * add active in-memory indexes of child ranges first.
	 */
	struct vy_range *r;
	rlist_foreach_entry(r, &range->compact_list, compact_list) {
		assert(rlist_empty(&r->frozen));
		sub_src = vy_merge_iterator_add(&itr->merge_iterator,
						true, true);
		vy_mem_iterator_open(&sub_src->mem_iterator, r->mem,
				     itr->iterator_type, itr->key, itr->vlsn);
	}
	/* Add the active in-memory index of the current range. */
	if (range->mem != NULL) {
		sub_src = vy_merge_iterator_add(&itr->merge_iterator,
						true, true);
		vy_mem_iterator_open(&sub_src->mem_iterator, range->mem,
				     itr->iterator_type, itr->key, itr->vlsn);
	}
	/* Add frozen in-memory indexes. */
	struct vy_mem *mem;
	rlist_foreach_entry(mem, &range->frozen, in_frozen) {
		sub_src = vy_merge_iterator_add(&itr->merge_iterator,
						false, true);
		vy_mem_iterator_open(&sub_src->mem_iterator, mem,
				     itr->iterator_type, itr->key, itr->vlsn);
	}
}

static void
vy_read_iterator_add_disk(struct vy_read_iterator *itr)
{
	assert(itr->curr_range != NULL);
	assert(itr->curr_range->shadow == NULL);
	struct vy_run *run;
	rlist_foreach_entry(run, &itr->curr_range->runs, in_range) {
		struct vy_merge_src *sub_src = vy_merge_iterator_add(
			&itr->merge_iterator, false, true);
		vy_run_iterator_open(&sub_src->run_iterator, itr->curr_range,
				     run, itr->iterator_type, itr->key,
				     itr->vlsn);
	}
}

/**
 * Set up merge iterator for the current range.
 */
static void
vy_read_iterator_use_range(struct vy_read_iterator *itr)
{
	if (!itr->only_disk && itr->tx != NULL)
		vy_read_iterator_add_tx(itr);

	if (itr->curr_range == NULL)
		return;

	if (!itr->only_disk)
		vy_read_iterator_add_mem(itr);

	vy_read_iterator_add_disk(itr);

	/* Enable range and range index version checks */
	vy_merge_iterator_set_version(&itr->merge_iterator, itr->curr_range);
}

/**
 * Open the iterator.
 */
static void
vy_read_iterator_open(struct vy_read_iterator *itr,
		      struct vy_index *index, struct vy_tx *tx,
		      enum iterator_type iterator_type,
		      const struct vy_stmt *key, const int64_t *vlsn,
		      bool only_disk)
{
	itr->index = index;
	itr->tx = tx;
	itr->iterator_type = iterator_type;
	itr->key = key;
	itr->vlsn = vlsn;
	itr->only_disk = only_disk;
	itr->search_started = false;
	itr->curr_stmt = NULL;
	itr->curr_range = NULL;
}

/**
 * Start lazy search
 */
void
vy_read_iterator_start(struct vy_read_iterator *itr)
{
	assert(!itr->search_started);
	assert(itr->curr_stmt == NULL);
	assert(itr->curr_range == NULL);
	itr->search_started = true;

	vy_range_iterator_open(&itr->range_iterator, itr->index,
			       itr->iterator_type, itr->key);
	vy_range_iterator_next(&itr->range_iterator, &itr->curr_range);
	vy_merge_iterator_open(&itr->merge_iterator, itr->index,
			       itr->iterator_type, itr->key);
	vy_read_iterator_use_range(itr);
}

/**
 * Check versions of index and current range and restores position if
 * something was changed
 */
static NODISCARD int
vy_read_iterator_restore(struct vy_read_iterator *itr)
{
	int rc;
restart:
	vy_range_iterator_restore(&itr->range_iterator, itr->curr_stmt,
				  &itr->curr_range);
	/* Re-create merge iterator */
	vy_merge_iterator_close(&itr->merge_iterator);
	vy_merge_iterator_open(&itr->merge_iterator, itr->index,
			       itr->iterator_type, itr->key);
	vy_read_iterator_use_range(itr);
	rc = vy_merge_iterator_restore(&itr->merge_iterator, itr->curr_stmt);
	if (rc == -1)
		return -1;
	if (rc == -2)
		goto restart;
	return rc;
}

/**
 * Conventional wrapper around vy_merge_iterator_next_key() to automatically
 * re-create the merge iterator on vy_index/vy_range/vy_run changes.
 */
static NODISCARD int
vy_read_iterator_merge_next_key(struct vy_read_iterator *itr,
				struct vy_stmt **ret)
{
	int rc;
	*ret = NULL;
	struct vy_merge_iterator *mi = &itr->merge_iterator;
	while ((rc = vy_merge_iterator_next_key(mi,
						itr->curr_stmt, ret)) == -2) {
		if (vy_read_iterator_restore(itr) < 0)
			return -1;
		/* Check if the iterator is restored not on the same key. */
		if (itr->curr_stmt) {
			rc = vy_merge_iterator_locate(mi, ret);
			if (rc == -1)
				return -1;
			if (rc == -2) {
				if (vy_read_iterator_restore(itr) < 0)
					return -1;
				continue;
			}
			/* If the iterator is empty then return. */
			if (*ret == NULL)
				return 0;
			/*
			 * If the iterator after restoration is on the same key
			 * then go to the next.
			 */
			if (vy_stmt_compare(itr->curr_stmt, *ret,
					    itr->index->format,
					    itr->index->key_def) == 0)
				continue;
			/* Else return the new key. */
			break;
		}
	}
	return rc;
}

/**
 * Goto next range according to order
 * return 0 : something was found
 * return 1 : no more data
 * return -1 : read error
 */
static NODISCARD int
vy_read_iterator_next_range(struct vy_read_iterator *itr, struct vy_stmt **ret)
{
	*ret = NULL;
	assert(itr->curr_range != NULL);
	vy_merge_iterator_close(&itr->merge_iterator);
	vy_merge_iterator_open(&itr->merge_iterator, itr->index,
			       itr->iterator_type, itr->key);
	vy_range_iterator_next(&itr->range_iterator, &itr->curr_range);
	vy_read_iterator_use_range(itr);
	struct vy_stmt *stmt = NULL;
	int rc = vy_read_iterator_merge_next_key(itr, &stmt);
	if (rc < 0)
		return -1;
	assert(rc >= 0);
	if (!stmt && itr->merge_iterator.range_ended && itr->curr_range != NULL)
		return vy_read_iterator_next_range(itr, ret);
	*ret = stmt;
	return rc;
}

static NODISCARD int
vy_read_iterator_next(struct vy_read_iterator *itr, struct vy_stmt **result)
{
	if (!itr->search_started)
		vy_read_iterator_start(itr);
	*result = NULL;
	struct vy_stmt *t = NULL;
	struct vy_merge_iterator *mi = &itr->merge_iterator;
	struct key_def *def = itr->index->key_def;
	struct tuple_format *format = itr->index->format;
	while (true) {
		if (vy_read_iterator_merge_next_key(itr, &t))
			return -1;
restart:
		if (mi->range_ended && itr->curr_range != NULL &&
		    vy_read_iterator_next_range(itr, &t))
			return -1;
		if (t == NULL)
			return 0; /* No more data. */
		int rc = vy_merge_iterator_squash_upsert(mi, itr->curr_stmt,
							 &t, true);
		if (rc != 0) {
			if (rc == -1)
				return -1;
			do {
				if (vy_read_iterator_restore(itr) < 0)
					return -1;
				rc = vy_merge_iterator_next_lsn(mi,
								itr->curr_stmt,
								&t);
			} while (rc == -2);
			if (rc != 0)
				return -1;
			goto restart;
		}
		assert(t != NULL);
		if (t->type != IPROTO_DELETE) {
			if (t->type == IPROTO_UPSERT) {
				struct vy_stmt *applied;
				applied = vy_apply_upsert(t, NULL, def, format,
							  true);
				vy_stmt_unref(t);
				t = applied;
				assert(t->type == IPROTO_REPLACE);
			}
			if (itr->curr_stmt != NULL)
				vy_stmt_unref(itr->curr_stmt);
			itr->curr_stmt = t;
			break;
		} else {
			vy_stmt_unref(t);
		}
	}
	*result = itr->curr_stmt;
	assert(*result == NULL || (*result)->type == IPROTO_REPLACE);
	return 0;
}

/**
 * Close the iterator and free resources
 */
static void
vy_read_iterator_close(struct vy_read_iterator *itr)
{
	if (itr->curr_stmt != NULL)
		vy_stmt_unref(itr->curr_stmt);
	itr->curr_stmt = NULL;
	if (itr->search_started)
		vy_merge_iterator_close(&itr->merge_iterator);
}

/* }}} Iterator over index */

/** {{{ Replication */

int
vy_index_send(struct vy_index *index, vy_send_row_f sendrow, void *ctx)
{
	static const int64_t vlsn = INT64_MAX;
	int rc = 0;

	struct vy_read_iterator ri;
	struct vy_stmt *stmt;
	struct vy_stmt *key = vy_stmt_new_select(NULL, 0);
	if (key == NULL)
		return -1;
	vy_read_iterator_open(&ri, index, NULL, ITER_GT, key, &vlsn, true);
	rc = vy_read_iterator_next(&ri, &stmt);
	for (; rc == 0 && stmt; rc = vy_read_iterator_next(&ri, &stmt)) {
		uint32_t mp_size;
		const char *mp_data;
		mp_data = vy_tuple_data_range(stmt, &mp_size);
		int64_t lsn = stmt->lsn;
		rc = sendrow(ctx, mp_data, mp_size, lsn);
		if (rc != 0)
			break;
	}
	vy_read_iterator_close(&ri);
	vy_stmt_unref(key);
	return rc;
}

/* }}} replication */

/**
 * This structure represents a request to squash a sequence of
 * UPSERT statements by inserting the resulting REPLACE statement
 * after them.
 */
struct vy_squash {
	/** Next in vy_squash_queue->queue. */
	struct stailq_entry next;
	/** Index this request is for. */
	struct vy_index *index;
	/** Key to squash upserts for. */
	struct vy_stmt *stmt;
};

struct vy_squash_queue {
	/** Fiber doing background upsert squashing. */
	struct fiber *fiber;
	/** Used to wake up the fiber to process more requests. */
	struct ipc_cond cond;
	/** Queue of vy_squash objects to be processed. */
	struct stailq queue;
	/** Mempool for struct vy_squash. */
	struct mempool pool;
};

static struct vy_squash *
vy_squash_new(struct mempool *pool, struct vy_index *index,
	      struct vy_stmt *stmt)
{
	struct vy_squash *squash;
	squash = mempool_alloc(pool);
	if (squash == NULL)
		return NULL;
	vy_index_ref(index);
	squash->index = index;
	vy_stmt_ref(stmt);
	squash->stmt = stmt;
	return squash;
}

static void
vy_squash_delete(struct mempool *pool, struct vy_squash *squash)
{
	vy_index_unref(squash->index);
	vy_stmt_unref(squash->stmt);
	mempool_free(pool, squash);
}

static int
vy_squash_process(struct vy_squash *squash)
{
	struct vy_index *index = squash->index;
	struct vy_env *env = index->env;
	struct tuple_format *format = index->format;
	struct key_def *key_def = index->key_def;

	struct vy_read_iterator itr;
	const int64_t lsn = INT64_MAX;
	vy_read_iterator_open(&itr, index, NULL, ITER_EQ,
			      squash->stmt, &lsn, false);
	struct vy_stmt *result;
	int rc = vy_read_iterator_next(&itr, &result);
	if (rc != 0) {
		vy_read_iterator_close(&itr);
		return -1;
	} else if (result == NULL) {
		vy_read_iterator_close(&itr);
		return 0;
	}

	vy_stmt_ref(result);
	vy_read_iterator_close(&itr);

	struct vy_range *range;
	range = vy_range_tree_find_by_key(&index->tree, ITER_EQ,
					  format, key_def, result);
	/*
	 * While we were reading on-disk runs, new statements could
	 * have been inserted into the in-memory tree. Apply them to
	 * the result.
	 */
	struct vy_mem *mem = range->mem;
	struct tree_mem_key tree_key = {
		.stmt = result,
		.lsn = result->lsn,
	};
	struct vy_mem_tree_iterator mem_itr =
		vy_mem_tree_lower_bound(&mem->tree, &tree_key, NULL);
	assert(!vy_mem_tree_iterator_is_invalid(&mem_itr));
	vy_mem_tree_iterator_prev(&mem->tree, &mem_itr);
	while (!vy_mem_tree_iterator_is_invalid(&mem_itr)) {
		const struct vy_stmt *mem_stmt =
			*vy_mem_tree_iterator_get_elem(&mem->tree, &mem_itr);
		if (vy_stmt_compare(result, mem_stmt, format, key_def) != 0)
			break;
		struct vy_stmt *applied;
		if (mem_stmt->type == IPROTO_UPSERT)
			applied = vy_apply_upsert(mem_stmt, result,
					    key_def, format, true);
		else
			applied = vy_stmt_dup(mem_stmt);
		vy_stmt_unref(result);
		if (applied == NULL)
			return -1;
		result = applied;
		vy_mem_tree_iterator_prev(&mem->tree, &mem_itr);
	}

	/*
	 * Insert the resulting REPLACE statement to the mem
	 * and adjust the quota.
	 */
	size_t mem_used_before = lsregion_used(&env->allocator);
	rc = vy_range_set(range, result, env->xm->lsn);
	size_t mem_used_after = lsregion_used(&env->allocator);
	assert(mem_used_after >= mem_used_before);
	vy_stmt_unref(result);
	if (rc == 0)
		vy_quota_force_use(&env->quota,
				   mem_used_after - mem_used_before);
	return rc;
}

static struct vy_squash_queue *
vy_squash_queue_new(void)
{
	struct vy_squash_queue *sq = malloc(sizeof(*sq));
	if (sq == NULL)
		return NULL;
	sq->fiber = NULL;
	ipc_cond_create(&sq->cond);
	stailq_create(&sq->queue);
	mempool_create(&sq->pool, cord_slab_cache(),
		       sizeof(struct vy_squash));
	return sq;
}

static void
vy_squash_queue_delete(struct vy_squash_queue *sq)
{
	if (sq->fiber != NULL) {
		sq->fiber = NULL;
		/* Sic: fiber_cancel() can't be used here */
		ipc_cond_signal(&sq->cond);
	}
	struct vy_squash *squash, *next;
	stailq_foreach_entry_safe(squash, next, &sq->queue, next)
		vy_squash_delete(&sq->pool, squash);
	free(sq);
}

static int
vy_squash_queue_f(va_list va)
{
	struct vy_squash_queue *sq = va_arg(va, struct vy_squash_queue *);
	while (sq->fiber != NULL) {
		if (stailq_empty(&sq->queue)) {
			ipc_cond_wait(&sq->cond);
			continue;
		}
		struct vy_squash *squash;
		squash = stailq_shift_entry(&sq->queue, struct vy_squash, next);
		if (vy_squash_process(squash) != 0)
			error_log(diag_last_error(diag_get()));
		vy_squash_delete(&sq->pool, squash);
	}
	return 0;
}

/*
 * For a given UPSERT statement, insert the resulting REPLACE
 * statement after it. Done in a background fiber.
 */
static void
vy_index_squash_upserts(struct vy_index *index, struct vy_stmt *stmt)
{
	struct key_def *key_def = index->key_def;
	struct vy_squash_queue *sq = index->env->squash_queue;

	say_debug("optimize upsert slow: %"PRIu32"/%"PRIu32": %s",
		  key_def->space_id, key_def->iid, vy_stmt_str(stmt));

	/* Start the upsert squashing fiber on demand. */
	if (sq->fiber == NULL) {
		sq->fiber = fiber_new("vinyl.squash_queue", vy_squash_queue_f);
		if (sq->fiber == NULL)
			goto fail;
		fiber_start(sq->fiber, sq);
	}

	struct vy_squash *squash = vy_squash_new(&sq->pool, index, stmt);
	if (squash == NULL)
		goto fail;

	stailq_add_tail_entry(&sq->queue, squash, next);
	ipc_cond_signal(&sq->cond);
	return;
fail:
	error_log(diag_last_error(diag_get()));
	diag_clear(diag_get());
}

/* {{{ Cursor */

struct vy_cursor *
vy_cursor_new(struct vy_tx *tx, struct vy_index *index, const char *key,
	      uint32_t part_count, enum iterator_type type)
{
	struct vy_env *e = index->env;
	struct vy_cursor *c = mempool_alloc(&e->cursor_pool);
	if (c == NULL) {
		diag_set(OutOfMemory, sizeof(*c), "cursor", "cursor pool");
		return NULL;
	}
	assert(part_count <= index->key_def->part_count);
	c->key = vy_stmt_new_select(key, part_count);
	if (c->key == NULL) {
		mempool_free(&e->cursor_pool, c);
		return NULL;
	}
	c->index = index;
	c->n_reads = 0;
	if (tx == NULL) {
		tx = &c->tx_autocommit;
		vy_tx_begin(e->xm, tx, VINYL_TX_RO);
	} else {
		rlist_add(&tx->cursors, &c->next_in_tx);
	}
	c->tx = tx;
	c->start = tx->start;
	/*
	 * Prevent index drop by the backend while the cursor is
	 * still alive.
	 */
	vy_index_ref(c->index);
	c->need_check_eq = false;
	enum iterator_type iterator_type;
	switch (type) {
	case ITER_ALL:
		iterator_type = ITER_GE;
		break;
	case ITER_GE:
	case ITER_GT:
	case ITER_LE:
	case ITER_LT:
	case ITER_EQ:
		iterator_type = type;
		break;
	case ITER_REQ: {
		struct key_def *def = index->key_def;
		/* point-lookup iterator (optimization) */
		if (def->opts.is_unique && part_count == def->part_count) {
			iterator_type = ITER_EQ;
		} else {
			c->need_check_eq = true;
			iterator_type = ITER_LE;
		}
		break;
	}
	default:
		unreachable();
	}
	vy_read_iterator_open(&c->iterator, index, tx, iterator_type, c->key,
			      &tx->vlsn, false);
	c->iterator_type = iterator_type;
	return c;
}

int
vy_cursor_next(struct vy_cursor *c, struct tuple **result)
{
	struct vy_stmt *vyresult = NULL;
	struct vy_index *index = c->index;
	struct key_def *def = index->key_def;
	assert(index->space->index_count > 0);
	*result = NULL;

	if (c->tx == NULL) {
		diag_set(ClientError, ER_NO_ACTIVE_TRANSACTION);
		return -1;
	}

	assert(c->key != NULL);
	int rc = vy_read_iterator_next(&c->iterator, &vyresult);
	if (rc)
		return -1;
	c->n_reads++;
	if (vy_tx_track(c->tx, index, vyresult ? vyresult : c->key,
			vyresult == NULL))
		return -1;
	if (vyresult == NULL)
		return 0;
	if (c->need_check_eq && vy_stmt_compare_with_key(vyresult, c->key,
							 index->format, def))
		return 0;
	if (def->iid > 0 && vy_index_full_by_stmt(c->tx, index, vyresult,
						  &vyresult))
		return -1;
	*result = vy_convert_replace(index->space, vyresult);
	if (def->iid > 0)
		vy_stmt_unref(vyresult);
	return *result != NULL ? 0 : -1;
}

void
vy_cursor_delete(struct vy_cursor *c)
{
	vy_read_iterator_close(&c->iterator);
	struct vy_env *e = c->index->env;
	if (c->tx != NULL) {
		if (c->tx == &c->tx_autocommit) {
			/* Rollback the automatic transaction. */
			vy_tx_rollback(c->index->env, c->tx);
		} else {
			/*
			 * Delete itself from the list of open cursors
			 * in the transaction
			 */
			rlist_del(&c->next_in_tx);
		}
	}
	if (c->key)
		vy_stmt_unref(c->key);
	vy_index_unref(c->index);
	vy_stat_cursor(e->stat, c->start, c->n_reads);
	TRASH(c);
	mempool_free(&e->cursor_pool, c);
}

/*** }}} Cursor */
