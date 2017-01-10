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
#include "tuple_format.h"

/** Global table of tuple formats */
struct tuple_format **tuple_formats;
struct tuple_format *tuple_format_default;
static intptr_t recycled_format_ids = FORMAT_ID_NIL;

static uint32_t formats_size = 0, formats_capacity = 0;


/** Extract all available type info from keys. */
static int
field_type_create(struct tuple_format *format, struct rlist *key_list)
{
	/* There may be fields between indexed fields (gaps). */
	for (uint32_t i = 0; i < format->field_count; i++)
		format->fields[i].type = FIELD_TYPE_ANY;

	struct key_def *key_def;
	/* extract field type info */
	rlist_foreach_entry(key_def, key_list, link) {
		for (uint32_t i = 0; i < key_def->part_count; i++) {
			assert(key_def->parts[i].fieldno < format->field_count);
			enum field_type set_type = key_def->parts[i].type;
			enum field_type *fmt_type =
				&format->fields[key_def->parts[i].fieldno].type;
			if (*fmt_type != FIELD_TYPE_ANY &&
			    *fmt_type != set_type) {
				diag_set(ClientError, ER_FIELD_TYPE_MISMATCH,
					 key_def->name, i + TUPLE_INDEX_BASE,
					 field_type_strs[set_type],
					 field_type_strs[*fmt_type]);
				return -1;
			}
			*fmt_type = set_type;
		}
	}
	return 0;
}

static int
tuple_format_register(struct tuple_format *format)
{
	if (recycled_format_ids != FORMAT_ID_NIL) {

		format->id = (uint16_t) recycled_format_ids;
		recycled_format_ids = (intptr_t) tuple_formats[recycled_format_ids];
	} else {
		if (formats_size == formats_capacity) {
			uint32_t new_capacity = formats_capacity ?
						formats_capacity * 2 : 16;
			struct tuple_format **formats;
			formats = (struct tuple_format **)
				realloc(tuple_formats, new_capacity *
						       sizeof(tuple_formats[0]));
			if (formats == NULL) {
				diag_set(OutOfMemory,
					 sizeof(struct tuple_format), "malloc",
					 "tuple_formats");
				return -1;
			}

			formats_capacity = new_capacity;
			tuple_formats = formats;
		}
		if (formats_size == FORMAT_ID_MAX + 1) {
			diag_set(ClientError, ER_TUPLE_FORMAT_LIMIT,
				 (unsigned) formats_capacity);
			return -1;
		}
		format->id = formats_size++;
	}
	tuple_formats[format->id] = format;
	return 0;
}

static void
tuple_format_deregister(struct tuple_format *format)
{
	if (format->id == FORMAT_ID_NIL)
		return;
	tuple_formats[format->id] = (struct tuple_format *) recycled_format_ids;
	recycled_format_ids = format->id;
	format->id = FORMAT_ID_NIL;
}

static struct tuple_format *
tuple_format_alloc(struct rlist *key_list)
{
	struct key_def *key_def;
	uint32_t max_fieldno = 0;
	uint32_t key_count = 0;

	/* find max max field no */
	rlist_foreach_entry(key_def, key_list, link) {
		struct key_part *part = key_def->parts;
		struct key_part *pend = part + key_def->part_count;
		key_count++;
		for (; part < pend; part++)
			max_fieldno = MAX(max_fieldno, part->fieldno);
	}
	uint32_t field_count = key_count > 0 ? max_fieldno + 1 : 0;

	uint32_t total = sizeof(struct tuple_format) +
			 field_count * sizeof(struct tuple_field_format);

	struct tuple_format *format = (struct tuple_format *) malloc(total);
	if (format == NULL) {
		diag_set(OutOfMemory, sizeof(struct tuple_format), "malloc",
			 "tuple format");
		return NULL;
	}

	format->refs = 0;
	format->id = FORMAT_ID_NIL;
	format->field_count = field_count;
	format->exact_field_count = 0;
	return format;
}

void
tuple_format_delete(struct tuple_format *format)
{
	tuple_format_deregister(format);
	free(format);
}

struct tuple_format *
tuple_format_new(struct rlist *key_list, struct tuple_format_vtab *vtab)
{
	struct tuple_format *format = tuple_format_alloc(key_list);
	if (format == NULL)
		return NULL;
	format->vtab = *vtab;
	if (tuple_format_register(format) < 0) {
		tuple_format_delete(format);
		return NULL;
	}
	if (field_type_create(format, key_list) < 0) {
		tuple_format_delete(format);
		return NULL;
	}
	/* Set up offset slots */
	if (format->field_count == 0) {
		/* Nothing to store */
		format->field_map_size = 0;
		return format;
	}
	/**
	 * First field is always simply accessible,
	 * so we don't store offset for it
	 */
	format->fields[0].offset_slot = INT32_MAX;

	int current_slot = 0;
	for (uint32_t i = 1; i < format->field_count; i++) {
		/*
		 * In the tuple, store only offsets necessary to
		 * quickly access indexed fields.
		 */
		if (format->fields[i].type == FIELD_TYPE_ANY)
			format->fields[i].offset_slot = INT32_MAX;
		else
			format->fields[i].offset_slot = --current_slot;
	}
	assert((uint32_t) (-current_slot * sizeof(uint32_t)) <= UINT16_MAX);
	format->field_map_size = -current_slot * sizeof(uint32_t);
	return format;
}

/** @sa declaration for details. */
int
tuple_init_field_map(const struct tuple_format *format, uint32_t *field_map,
		     const char *tuple)
{
	if (format->field_count == 0)
		return 0; /* Nothing to initialize */

	const char *pos = tuple;

	/* Check to see if the tuple has a sufficient number of fields. */
	uint32_t field_count = mp_decode_array(&pos);
	if (format->exact_field_count > 0 &&
	    format->exact_field_count != field_count) {
		diag_set(ClientError, ER_EXACT_FIELD_COUNT,
			 (unsigned) field_count,
			 (unsigned) format->exact_field_count);
		return -1;
	}
	if (unlikely(field_count < format->field_count)) {
		diag_set(ClientError, ER_INDEX_FIELD_COUNT,
			 (unsigned) field_count,
			 (unsigned) format->field_count);
		return -1;
	}

	/* first field is simply accessible, so we do not store offset to it */
	enum mp_type mp_type = mp_typeof(*pos);
	if (key_mp_type_validate(format->fields[0].type, mp_type, ER_FIELD_TYPE,
				 TUPLE_INDEX_BASE))
		return -1;
	mp_next(&pos);
	/* other fields...*/
	for (uint32_t i = 1; i < format->field_count; i++) {
		mp_type = mp_typeof(*pos);
		if (key_mp_type_validate(format->fields[i].type, mp_type,
					 ER_FIELD_TYPE, i + TUPLE_INDEX_BASE))
			return -1;
		if (format->fields[i].offset_slot < 0)
			field_map[format->fields[i].offset_slot] =
				(uint32_t) (pos - tuple);
		mp_next(&pos);
	}
	return 0;
}

void
tuple_format_init()
{
	RLIST_HEAD(empty_list);
	tuple_format_default = tuple_format_new(&empty_list, &memtx_tuple_format_vtab);
	if (tuple_format_default == NULL)
		diag_raise();
	/* Make sure this one stays around. */
	tuple_format_ref(tuple_format_default, 1);
}

/** Destroy tuple format subsystem and free resourses */
void
tuple_format_free()
{
	/* Clear recycled ids. */
	while (recycled_format_ids != FORMAT_ID_NIL) {
		uint16_t id = (uint16_t) recycled_format_ids;
		recycled_format_ids = (intptr_t) tuple_formats[id];
		tuple_formats[id] = NULL;
	}
	for (struct tuple_format **format = tuple_formats;
	     format < tuple_formats + formats_size;
	     format++)
		free(*format); /* ignore the reference count. */
	free(tuple_formats);
}
