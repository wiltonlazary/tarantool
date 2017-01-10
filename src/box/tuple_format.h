#ifndef TARANTOOL_BOX_TUPLE_FORMAT_H_INCLUDED
#define TARANTOOL_BOX_TUPLE_FORMAT_H_INCLUDED
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

#include "key_def.h" /* for enum field_type */
#include "errinj.h"

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

enum { FORMAT_ID_MAX = UINT16_MAX - 1, FORMAT_ID_NIL = UINT16_MAX };
enum { FORMAT_REF_MAX = INT32_MAX};

/*
 * We don't pass TUPLE_INDEX_BASE around dynamically all the time,
 * at least hard code it so that in most cases it's a nice error
 * message
 */
enum { TUPLE_INDEX_BASE = 1 };

/**
 * @brief Tuple field format
 * Support structure for struct tuple_format.
 * Contains information of one field.
 */
struct tuple_field_format {
	/**
	 * Field type of an indexed field.
	 * If a field participates in at least one of space indexes
	 * then its type is stored in this member.
	 * If a field does not participate in an index
	 * then UNKNOWN is stored for it.
	 */
	enum field_type type;
	/**
	 * Offset slot in field map in tuple.
	 * Normally tuple stores field map - offsets of all fields
	 * participating in indexes. This allows quick access to most
	 * used fields without parsing entire mspack.
	 * This member stores position in the field map of tuple
	 * for current field.
	 * If the field does not participate in indexes then it has
	 * no offset in field map and INT_MAX is stored in this member.
	 * Due to specific field map in tuple (it is stored before tuple),
	 * the positions in field map is negative.
	 * Thus if this member is negative, smth like
	 * tuple->data[((uint32_t *)tuple)[format->offset_slot[fieldno]]]
	 * gives the start of the field
	 */
	int32_t offset_slot;
};

struct tuple;

/** Engine-specific tuple format methods. */
struct tuple_format_vtab {
	/**
	 * Allocate memory for a new tuple. Reserves memory for
	 * engine-specific fields, uses engine-specific allocator.
	 * Initializes the tuple.
	 */
	struct tuple *
	(*create)(struct tuple_format *format, const char *data,
		     const char *end);
	/** Free allocated tuple using engine-specific memory allocator. */
	void
	(*destroy)(struct tuple_format *format, struct tuple *tuple);
};

/**
 * Hack: tuple_format_default and sysview engine use this vtab,
 * but should use the runtime arena and runtime specific vtab.
 */
extern struct tuple_format_vtab memtx_tuple_format_vtab;

/**
 * @brief Tuple format
 * Tuple format describes how tuple is stored and information about its fields
 */
struct tuple_format {
	struct tuple_format_vtab vtab;

	uint16_t id;
	/* Format objects are reference counted. */
	int refs;
	/**
	 * If not set (== 0), any tuple in the space can have any number of
	 * fields. If set, each tuple must have exactly this number of fields.
	 */
	uint32_t exact_field_count;
	/* Length of 'fields' array. */
	uint32_t field_count;
	/**
	 * Size of field map of tuple in bytes.
	 * See tuple_field_format::ofset for details//
	 */
	uint16_t field_map_size;

	/* Formats of the fields */
	struct tuple_field_format fields[];
};

/**
 * Default format for a tuple which does not belong
 * to any space and is stored in memory.
 */
extern struct tuple_format *tuple_format_default;

extern struct tuple_format **tuple_formats;

inline uint32_t
tuple_format_id(struct tuple_format *format)
{
	assert(tuple_formats[format->id] == format);
	return format->id;
}

inline struct tuple_format *
tuple_format_by_id(uint32_t tuple_format_id)
{
	return tuple_formats[tuple_format_id];
}

/** Delete a format with zero ref count. */
void
tuple_format_delete(struct tuple_format *format);

static inline void
tuple_format_ref(struct tuple_format *format, int count)
{
	assert(format->refs + count >= 0);
	assert((uint64_t)format->refs + count <= FORMAT_REF_MAX);

	format->refs += count;
	if (format->refs == 0)
		tuple_format_delete(format);

};

/**
 * Allocate, construct and register a new in-memory tuple format.
 * @param key_list List of key_defs of a space.
 * @param name  name of the tuple format
 *
 * @retval not NULL Tuple format.
 * @retval     NULL Memory error.
 */
struct tuple_format *
tuple_format_new(struct rlist *key_list, struct tuple_format_vtab *vtab);

/**
 * Fill the field map of tuple with field offsets.
 * @param format    Tuple format.
 * @param field_map A pointer behind the last element of the field
 *                  map.
 * @param tuple     MessagePack array.
 *
 * @retval  0 Success.
 * @retval -1 Format error.
 *            +-------------------+
 * Result:    | offN | ... | off1 |
 *            +-------------------+
 *                                ^
 *                             field_map
 * tuple + off_i = indexed_field_i;
 */
int
tuple_init_field_map(const struct tuple_format *format, uint32_t *field_map,
		     const char *tuple);

/**
 * Get a field at the specific position in this MessagePack array.
 * Returns a pointer to MessagePack data.
 * @param format tuple format
 * @param tuple a pointer to MessagePack array
 * @param field_map a pointer to the LAST element of field map
 * @param field_no the index of field to return
 *
 * @returns field data if field exists or NULL
 * @sa tuple_init_field_map()
 */
inline const char *
tuple_field_raw(const struct tuple_format *format, const char *tuple,
		const uint32_t *field_map, uint32_t field_no)
{
	if (likely(field_no < format->field_count)) {
		/* Indexed field */

		if (field_no == 0) {
			mp_decode_array(&tuple);
			return tuple;
		}

		if (format->fields[field_no].offset_slot != INT32_MAX) {
			int32_t slot = format->fields[field_no].offset_slot;
			return tuple + field_map[slot];
		}
	}
	ERROR_INJECT(ERRINJ_TUPLE_FIELD, return NULL);
	uint32_t field_count = mp_decode_array(&tuple);
	if (unlikely(field_no >= field_count))
		return NULL;
	for (uint32_t k = 0; k < field_no; k++)
		mp_next(&tuple);
	return tuple;
}

#if defined(__cplusplus)
} /* extern "C" */
#endif /* defined(__cplusplus) */

void
tuple_format_init();

/** Destroy tuple format subsystem and free resourses */
void
tuple_format_free();

#endif /* #ifndef TARANTOOL_BOX_TUPLE_FORMAT_H_INCLUDED */
