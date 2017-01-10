#ifndef TARANTOOL_XLOG_H_INCLUDED
#define TARANTOOL_XLOG_H_INCLUDED
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
#include <stdio.h>
#include <stdbool.h>
#include <sys/stat.h>
#include "tt_uuid.h"
#include "vclock.h"

#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"

#include "small/ibuf.h"
#include "small/obuf.h"

struct iovec;
struct xrow_header;

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

extern const struct type type_XlogError;

/* {{{ log dir */

/**
 * Type of log directory. A single filesystem directory can be
 * used for logs and snapshots, but an xlog object sees only
 * those files which match its type.
 */
enum xdir_type { SNAP, XLOG };

/**
 * Newly created snapshot files get .inprogress filename suffix.
 * The suffix is removed  when the file is finished
 * and closed.
 */
enum log_suffix { NONE, INPROGRESS };

/**
 * A handle for a data directory with write ahead logs or snapshots.
 * Can be used to find the last log in the directory, scan
 * through all logs, create a new log.
 */
struct xdir {
	/**
	 * Allow partial recovery from a damaged/incorrect
	 * data directory. Suppresses exceptions when scanning
	 * the directory, parsing file headers, or reading
	 * partial or corrupt rows. Incorrect objects
	 * are skipped.
	 */
	bool panic_if_error;

	/**
	 * true if a log file in this directory can by fsync()ed
	 * at close in a separate thread (we use this technique to
	 * speed up sync of write ahead logs, but not snapshots).
	 */
	bool sync_is_async;

	/* Default filename suffix for a new file. */
	enum log_suffix suffix;
	/**
	 * Additional flags to apply at open(2) to write.
	 */
	int open_wflags;
	/**
	 * A pointer to this server uuid. If not assigned
	 * (tt_uuid_is_nil returns true), server id check
	 * for logs in this directory is not performed.
	 * Otherwise, any log in this directory must have
	 * the matching server id.
	 */
	const struct tt_uuid *server_uuid;
	/**
	 * Text of a marker written to the text file header:
	 * XLOG (meaning it's a write ahead log) or SNAP (a
	 * snapshot).
	 */
	const char *filetype;
	/**
	 * File name extension (.xlog or .snap).
	 */
	const char *filename_ext;
	/** File create mode in this directory. */
	mode_t mode;
	/*
	 * Index of files present in the directory. Initially
	 * empty, must be initialized with xdir_scan().
	 */
	vclockset_t index;
	/**
	 * Directory path.
	 */
	char dirname[PATH_MAX+1];
	/** Snapshots or xlogs */
	enum xdir_type type;
	/**
	 * Sync interval in bytes.
	 * xlog file will be synced every sync_interval bytes,
	 * corresponding file cache will be marked as free
	 */
	uint64_t sync_interval;
};

/**
 * Initialize a log dir.
 */
void
xdir_create(struct xdir *dir, const char *dirname, enum xdir_type type,
	    const struct tt_uuid *server_uuid);

/**
 * Destroy a log dir object.
 */
void
xdir_destroy(struct xdir *dir);

/**
 * Scan or re-scan a directory and update directory
 * index with all log files (or snapshots) in the directory.
 * Must be used if it is necessary to find the last log/
 * snapshot or scan through all logs.
 */
int
xdir_scan(struct xdir *dir);

/**
 * Check that a directory exists and is writable.
 */
int
xdir_check(struct xdir *dir);

/**
 * Return a file name based on directory type, vector clock
 * sum, and a suffix (.inprogress or not).
 */
char *
xdir_format_filename(struct xdir *dir, int64_t signature,
		     enum log_suffix suffix);

/* }}} */

/* {{{ xlog meta */

/**
 * A xlog meta info
 */
struct xlog_meta {
	/** Text file header: filetype */
	char filetype[10];
	/**
	 * Text file header: server uuid. We read
	 * only logs with our own uuid, to avoid situations
	 * when a DBA has manually moved a few logs around
	 * and messed the data directory up.
	 */
	struct tt_uuid server_uuid;
	/**
	 * Text file header: vector clock taken at the time
	 * this file was created. For WALs, this is vector
	 * clock *at start of WAL*, for snapshots, this
	 * is vector clock *at the time the snapshot is taken.
	 */
	struct vclock vclock;
};

/* }}} */

/**
 * A single log file - a snapshot or a write ahead log.
 */
struct xlog {
	/** xlog meta header */
	struct xlog_meta meta;
	/** do sync in async mode */
	bool sync_is_async;
	/** File handle. */
	int fd;
	/**
	 * How many xlog rows are in the file last time it
	 * was read or written. Updated in xlog_cursor_close()
	 * and is used to check whether or not we have discovered
	 * a new row in the file since it was last read. This is
	 * used in local hot standby to "follow up" on new rows
	 * appended to the file.
	 */
	int64_t rows; /* should have the same type as lsn */
	/** Log file name. */
	char filename[PATH_MAX + 1];
	/** Whether this file has .inprogress suffix. */
	bool is_inprogress;
	/*
	 * If true, we can flush the data in this buffer whenever
	 * we like, and it's usually when the buffer gets
	 * sufficiently big to get compressed.
	 *
	 * Otherwise, we must observe transactional boundaries
	 * to avoid writing a partial transaction to WAL: a
	 * single transaction always goes to WAL in a single
	 * "chunk" with 1 fixed header and common checksum
	 * for all transactional rows. This prevents miscarriage
	 * or partial delivery of transactional rows to a slave
	 * during replication.
	 */
	bool is_autocommit;
	/** The current offset in the log file, for writing. */
	off_t offset;
	/**
	 * Output buffer, works as row accumulator for
	 * compression.
	 */
	struct obuf obuf;
	/** The context of zstd compression */
	ZSTD_CCtx *zctx;
	/**
	 * Compressed output buffer
	 */
	struct obuf zbuf;
	/**
	 * Sync interval in bytes.
	 * xlog file will be synced every sync_interval bytes,
	 * corresponding file cache will be marked as free
	 */
	uint64_t sync_interval;
	/**
	 * synced file size
	 */
	uint64_t synced_size;
};

/**
 * Create a new file and open it in write (append) mode.
 * Note: an existing file is impossible to open for append,
 * the old files are never appended to.
 *
 * @param xdir xdir
 * @param[out] xlog xlog structure
 * @param server uuid   the server which created the file
 * @param vclock        the global state of replication (vector
 *			clock) at the moment the file is created.
 *
 * @retval 0 if OK
 * @retval -1 if error
 */
int
xdir_create_xlog(struct xdir *dir, struct xlog *xlog,
		 const struct vclock *vclock);

/**
 * Create new xlog writer based on fd.
 * @param fd            file descriptor
 * @param name          the assiciated name
 * @param meta          xlog meta
 *
 * @retval 0 for success
 * @retvl -1 if error
 */

int
xlog_create(struct xlog *xlog, const char *name,
	    const struct xlog_meta *meta);

/**
 * Rename xlog
 *
 * @retval 0 for ok
 * @retval -1 for error
 */
int
xlog_rename(struct xlog *l);

/**
 * Write a row to xlog, 
 *
 * @retval count of writen bytes
 * @retval -1 for error
 */
ssize_t
xlog_write_row(struct xlog *log, const struct xrow_header *packet);

/**
 * Prevent xlog row buffer offloading, should be use
 * at transaction start to write transaction in one xlog tx
 */
void
xlog_tx_begin(struct xlog *log);

/**
 * Enable xlog row buffer offloading
 *
 * @retval count of writen bytes
 * @retval 0 if buffer is not writen
 * @retval -1 if error
 */
ssize_t
xlog_tx_commit(struct xlog *log);

/**
 * Discard xlog row buffer
 */
void
xlog_tx_rollback(struct xlog *log);

/**
 * Flush buffered rows and sync file
 */
ssize_t
xlog_flush(struct xlog *log);


/**
 * Sync a log file. The exact action is defined
 * by xdir flags.
 *
 * @retval 0 success
 * @retval -1 error
 */
int
xlog_sync(struct xlog *l);

/**
 * Close the log file and free xlog object.
 *
 * @retval 0 success
 * @retval -1 error (fclose() failed).
 */
int
xlog_close(struct xlog *l, bool reuse_fd);

/**
 * atfork() handler function to close the log pointed
 * at by xlog in the child.
 */
void
xlog_atfork(struct xlog *xlog);

/* {{{ xlog_tx_cursor - iterate over rows in xlog transaction */

/**
 * xlog tx iterator
 */
struct xlog_tx_cursor
{
	/** rows buffer */
	struct ibuf rows;
};

/**
 * Create xlog tx iterator from memory data.
 * *data will be adjusted to end of tx
 *
 * @retval 0 for Ok
 * @retval -1 for error
 * @retval >0 how many additional bytes should be read to parse tx
 */
ssize_t
xlog_tx_cursor_create(struct xlog_tx_cursor *cursor,
		      const char **data, const char *data_end,
		      ZSTD_DStream *zdctx);

/**
 * Destroy xlog tx cursor and free all associated memory
 * including parsed xrows
 */
int
xlog_tx_cursor_destroy(struct xlog_tx_cursor *tx_cursor);

/**
 * Fetch next xrow from xlog tx cursor
 *
 * @retval 0 for Ok
 * @retval -1 for error
 */
int
xlog_tx_cursor_next_row(struct xlog_tx_cursor *tx_cursor, struct xrow_header *xrow);

/**
 * A conventional helper to decode rows from the raw tx buffer.
 * Decodes fixheader, checks crc32 and length, decompresses rows.
 *
 * @param data a buffer with the raw tx data, including fixheader
 * @param data_end the end of @a data buffer
 * @param[out] rows a buffer to store decoded rows
 * @param[out] rows_end the end of @a rows buffer
 * @retval  0 success
 * @retval -1 error, check diag
 */
int
xlog_tx_decode(const char *data, const char *data_end,
	       char *rows, char *rows_end,
	       ZSTD_DStream *zdctx);

/* }}} */

/* {{{ xlog_cursor - read rows from a log file */

enum xlog_cursor_state {
	/* Cursor is closed */
	XLOG_CURSOR_CLOSED = 0,
	/* The cursor is open but no tx is read */
	XLOG_CURSOR_ACTIVE = 1,
	/* The Cursor is open and a tx is read */
	XLOG_CURSOR_TX = 2,
	/* The cursor is open but is at the end of file. */
	XLOG_CURSOR_EOF = 3
};

/**
 * Xlog cursor, read rows from xlog
 */
struct xlog_cursor
{
	enum xlog_cursor_state state;
	/** xlog meta info */
	struct xlog_meta meta;
	/** file descriptor or -1 for in memory */
	int fd;
	/** associated file name */
	char name[PATH_MAX];
	/** file read buffer */
	struct ibuf rbuf;
	/** file read position */
	off_t read_offset;
	/** cursor for current tx */
	struct xlog_tx_cursor tx_cursor;
	/** ZSTD context for decompression */
	ZSTD_DStream *zdctx;
};

/**
 * Open cursor from file descriptor
 * @param cursor cursor
 * @param fd file descriptor
 * @param name associated file name
 * @retval 0 succes
 * @retval -1 error, check diag
 */
int
xlog_cursor_openfd(struct xlog_cursor *cursor, int fd, const char *name);

/**
 * Open cursor from file
 * @param cursor cursor
 * @param name file name
 * @retval 0 succes
 * @retval -1 error, check diag
 */
int
xlog_cursor_open(struct xlog_cursor *cursor, const char *name);

/**
 * Open cursor from memory
 * @param cursor cursor
 * @param data pointer to memory block
 * @param size memory block size
 * @param name associated file name
 * @retval 0 succes
 * @retval -1 error, check diag
 */
int
xlog_cursor_openmem(struct xlog_cursor *cursor, const char *data, size_t size,
		    const char *name);

/**
 * Close cursor
 * @param cursor cursor
 */
void
xlog_cursor_close(struct xlog_cursor *cursor, bool reuse_fd);

/**
 * Open next tx from xlog
 * @param cursor cursor
 * @retval 0 succes
 * @retval 1 eof
 * retval -1 error, check diag
 */
int
xlog_cursor_next_tx(struct xlog_cursor *cursor);

/**
 * Fetch next xrow from current xlog tx
 *
 * @retval 0 for Ok
 * @retval 1 if current tx is done
 * @retval -1 for error
 */
int
xlog_cursor_next_row(struct xlog_cursor *cursor, struct xrow_header *xrow);

/**
 * Move to the next xlog tx
 *
 * @retval 0 magic found
 * @retval 1 magic not found and eof reached
 * @retval -1 error
 */
int
xlog_cursor_find_tx_magic(struct xlog_cursor *i);

/* }}} */

/** {{{ miscellaneous log io functions. */

/**
 * Open cursor for xdir entry pointed by signature
 * @param xdir xdir
 * @param signature xlog signature
 * @param cursor cursor
 * @retval 0 succes
 * @retval -1 error, check diag
 */
int
xdir_open_cursor(struct xdir *dir, int64_t signature,
		 struct xlog_cursor *cursor);

/** }}} */

#if defined(__cplusplus)
} /* extern C */

#include "exception.h"

/**
 * XlogError is raised when there is an error with contents
 * of the data directory or a log file. A special subclass
 * of exception is introduced to gracefully skip such errors
 * in panic_if_error = false mode.
 */
struct XlogError: public Exception
{
	XlogError(const char *file, unsigned line,
		  const char *format, ...);
	virtual void raise() { throw this; }
protected:
	XlogError(const struct type *type, const char *file, unsigned line,
		  const char *format, ...);
};

struct XlogGapError: public XlogError
{
	XlogGapError(const char *file, unsigned line,
		  const struct vclock *from,
		  const struct vclock *to);
	virtual void raise() { throw this; }
};

static inline void
xdir_scan_xc(struct xdir *dir)
{
	if (xdir_scan(dir) == -1)
		diag_raise();
}

static inline void
xdir_check_xc(struct xdir *dir)
{
	if (xdir_check(dir) == -1)
		diag_raise();
}

/**
 * Fetch next row from cursor, ignores xlog tx boundary,
 * open a next one tx if current is done.
 *
 * @retval 0 for Ok
 * @retval 1 for EOF
 */
static inline int
xlog_cursor_next_xc(struct xlog_cursor *cursor,
			struct xrow_header *xrow, bool panic_if_error)
{
	while (true) {
		int rc;
		rc = xlog_cursor_next_row(cursor, xrow);
		if (rc == 0)
			break;
		if (rc < 0) {
			struct error *e = diag_last_error(diag_get());
			if (panic_if_error ||
			    e->type != &type_XlogError)
				diag_raise();
			say_error("can't decode row: %s", e->errmsg);
		}
		while ((rc = xlog_cursor_next_tx(cursor)) < 0) {
			struct error *e = diag_last_error(diag_get());
			if (panic_if_error ||
			    e->type != &type_XlogError)
				diag_raise();
			say_error("can't open tx: %s", e->errmsg);
			if ((rc = xlog_cursor_find_tx_magic(cursor)) < 0)
				diag_raise();
			if (rc > 0)
				break;
		}
		if (rc == 1)
			return 1;
	}
	return 0;
}

/**
 * @copydoc xdir_open_cursor
 */
static inline int
xdir_open_cursor_xc(struct xdir *dir, int64_t signature,
		    struct xlog_cursor *cursor)
{
	int rc = xdir_open_cursor(dir, signature, cursor);
	if (rc == -1)
		diag_raise();
	return rc;
}

/**
 * @copydoc xlog_cursor_openfd
 */
static inline int
xlog_cursor_openfd_xc(struct xlog_cursor *cursor, int fd, const char *name)
{
	int rc = xlog_cursor_openfd(cursor, fd, name);
	if (rc == -1)
		diag_raise();
	return rc;
}
/**
 * @copydoc xlog_cursor_open
 */
static inline int
xlog_cursor_open_xc(struct xlog_cursor *cursor, const char *name)
{
	int rc = xlog_cursor_open(cursor, name);
	if (rc == -1)
		diag_raise();
	return rc;
}

#endif /* defined(__cplusplus) */

#endif /* TARANTOOL_XLOG_H_INCLUDED */
