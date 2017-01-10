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
#include "box/lua/call.h"
#include "box/error.h"
#include "fiber.h"

#include "lua/utils.h"
#include "lua/msgpack.h"

#include "box/txn.h"
#include "box/xrow.h"
#include "box/iproto_constants.h"
#include "box/iproto_port.h"
#include "box/lua/tuple.h"
#include "small/obuf.h"

/**
 * A helper to find a Lua function by name and put it
 * on top of the stack.
 */
static int
box_lua_find(lua_State *L, const char *name, const char *name_end)
{
	int index = LUA_GLOBALSINDEX;
	int objstack = 0;
	const char *start = name, *end;

	while ((end = (const char *) memchr(start, '.', name_end - start))) {
		lua_checkstack(L, 3);
		lua_pushlstring(L, start, end - start);
		lua_gettable(L, index);
		if (! lua_istable(L, -1)) {
			diag_set(ClientError, ER_NO_SUCH_PROC,
				 name_end - name, name);
			luaT_error(L);
		}
		start = end + 1; /* next piece of a.b.c */
		index = lua_gettop(L); /* top of the stack */
	}

	/* box.something:method */
	if ((end = (const char *) memchr(start, ':', name_end - start))) {
		lua_checkstack(L, 3);
		lua_pushlstring(L, start, end - start);
		lua_gettable(L, index);
		if (! (lua_istable(L, -1) ||
			lua_islightuserdata(L, -1) || lua_isuserdata(L, -1) )) {
				diag_set(ClientError, ER_NO_SUCH_PROC,
					  name_end - name, name);
				luaT_error(L);
		}
		start = end + 1; /* next piece of a.b.c */
		index = lua_gettop(L); /* top of the stack */
		objstack = index;
	}


	lua_pushlstring(L, start, name_end - start);
	lua_gettable(L, index);
	if (!lua_isfunction(L, -1) && !lua_istable(L, -1)) {
		/* lua_call or lua_gettable would raise a type error
		 * for us, but our own message is more verbose. */
		diag_set(ClientError, ER_NO_SUCH_PROC,
			  name_end - name, name);
		luaT_error(L);
	}
	/* setting stack that it would contain only
	 * the function pointer. */
	if (index != LUA_GLOBALSINDEX) {
		if (objstack == 0) {        /* no object, only a function */
			lua_replace(L, 1);
		} else if (objstack == 1) { /* just two values, swap them */
			lua_insert(L, -2);
		} else {		    /* long path */
			lua_insert(L, 1);
			lua_insert(L, 2);
			objstack = 1;
		}
		lua_settop(L, 1 + objstack);
	}
	return 1 + objstack;
}

/**
 * A helper to find lua stored procedures for box.call.
 * box.call iteslf is pure Lua, to avoid issues
 * with infinite call recursion smashing C
 * thread stack.
 */

static int
lbox_call_loadproc(struct lua_State *L)
{
	const char *name;
	size_t name_len;
	name = lua_tolstring(L, 1, &name_len);
	return box_lua_find(L, name, name + name_len);
}

/*
 * Encode CALL result.
 * Please read gh-291 carefully before "fixing" this code.
 */
static inline uint32_t
luamp_encode_call(lua_State *L, struct luaL_serializer *cfg,
		  struct mpstream *stream)
{
	int nrets = lua_gettop(L);
	if (nrets == 0) {
		return 0;
	} else if (nrets > 1) {
		/*
		 * Multireturn:
		 * `return 1, box.tuple.new(...), array, 3, ...`
		 */
		for (int i = 1; i <= nrets; ++i) {
			struct luaL_field field;
			luaL_tofield(L, cfg, i, &field);
			struct tuple *tuple;
			if (field.type == MP_EXT &&
			    (tuple = luaT_istuple(L, i)) != NULL) {
				/* `return ..., box.tuple.new(...), ...` */
				tuple_to_mpstream(tuple, stream);
			} else if (field.type != MP_ARRAY) {
				/*
				 * `return ..., scalar, ... =>
				 *         ..., { scalar }, ...`
				 */
				lua_pushvalue(L, i);
				luamp_encode_array(cfg, stream, 1);
				luamp_encode_r(L, cfg, stream, &field, 0);
				lua_pop(L, 1);
			} else {
				/* `return ..., array, ...` */
				luamp_encode(L, cfg, stream, i);
			}
		}
		return nrets;
	}
	assert(nrets == 1);

	/*
	 * Inspect the first result
	 */
	struct luaL_field root;
	luaL_tofield(L, cfg, 1, &root);
	struct tuple *tuple;
	if (root.type == MP_EXT && (tuple = luaT_istuple(L, 1)) != NULL) {
		/* `return box.tuple()` */
		tuple_to_mpstream(tuple, stream);
		return 1;
	} else if (root.type != MP_ARRAY) {
		/*
		 * `return scalar`
		 * `return map`
		 */
		luamp_encode_array(cfg, stream, 1);
		assert(lua_gettop(L) == 1);
		luamp_encode_r(L, cfg, stream, &root, 0);
		return 1;
	}

	assert(root.type == MP_ARRAY);
	if (root.size == 0) {
		/* `return {}` => `{ box.tuple() }` */
		luamp_encode_array(cfg, stream, 0);
		return 1;
	}

	/* `return { tuple, scalar, tuple }` */
	assert(root.type == MP_ARRAY && root.size > 0);
	for (uint32_t t = 1; t <= root.size; t++) {
		lua_rawgeti(L, 1, t);
		struct luaL_field field;
		luaL_tofield(L, cfg, -1, &field);
		if (field.type == MP_EXT && (tuple = luaT_istuple(L, -1))) {
			tuple_to_mpstream(tuple, stream);
		} else if (field.type != MP_ARRAY) {
			/* The first member of root table is not tuple/array */
			if (t == 1) {
				/*
				 * `return { scalar, ... } =>
				 *        box.tuple.new(scalar, ...)`
				 */
				luamp_encode_array(cfg, stream, root.size);
				/*
				 * Encode the first field of tuple using
				 * existing information from luaL_tofield
				 */
				luamp_encode_r(L, cfg, stream, &field, 0);
				lua_pop(L, 1);
				assert(lua_gettop(L) == 1);
				/* Encode remaining fields as usual */
				for (uint32_t f = 2; f <= root.size; f++) {
					lua_rawgeti(L, 1, f);
					luamp_encode(L, cfg, stream, -1);
					lua_pop(L, 1);
				}
				return 1;
			}
			/*
			 * `return { tuple/array, ..., scalar, ... } =>
			 *         { tuple/array, ..., { scalar }, ... }`
			 */
			luamp_encode_array(cfg, stream, 1);
			luamp_encode_r(L, cfg, stream, &field, 0);
		} else {
			/* `return { tuple/array, ..., tuple/array, ... }` */
			luamp_encode_r(L, cfg, stream, &field, 0);
		}
		lua_pop(L, 1);
		assert(lua_gettop(L) == 1);
	}
	return root.size;
}

struct lua_function_ctx {
	struct request *request;
	struct obuf *out;
	struct obuf_svp svp;
	/* true if `out' was changed and `svp' can be used for rollback  */
	bool out_is_dirty;
};

/**
 * Invoke a Lua stored procedure from the binary protocol
 * (implementation of 'CALL' command code).
 */
static inline int
execute_lua_call(lua_State *L)
{
	struct lua_function_ctx *ctx = (struct lua_function_ctx *)
		lua_topointer(L, 1);
	struct request *request = ctx->request;
	struct obuf *out = ctx->out;
	struct obuf_svp *svp = &ctx->svp;
	lua_settop(L, 0); /* clear the stack to simplify the logic below */

	const char *name = request->key;
	uint32_t name_len = mp_decode_strl(&name);

	int oc = 0; /* how many objects are on stack after box_lua_find */
	/* Try to find a function by name in Lua */
	oc = box_lua_find(L, name, name + name_len);

	/* Push the rest of args (a tuple). */
	const char *args = request->tuple;

	uint32_t arg_count = mp_decode_array(&args);
	luaL_checkstack(L, arg_count, "call: out of stack");

	for (uint32_t i = 0; i < arg_count; i++)
		luamp_decode(L, luaL_msgpack_default, &args);
	lua_call(L, arg_count + oc - 1, LUA_MULTRET);

	/**
	 * Add all elements from Lua stack to iproto.
	 *
	 * To allow clients to understand a complex return from
	 * a procedure, we are compatible with SELECT protocol,
	 * and return the number of return values first, and
	 * then each return value as a tuple.
	 *
	 * If a Lua stack contains at least one scalar, each
	 * value on the stack is converted to a tuple. A single
	 * Lua with scalars is converted to a tuple with multiple
	 * fields.
	 *
	 * If the stack is a Lua table, each member of which is
	 * not scalar, each member of the table is converted to
	 * a tuple. This way very large lists of return values can
	 * be used, since Lua stack size is limited by 8000 elements,
	 * while Lua table size is pretty much unlimited.
	 */
	/* TODO: forbid explicit yield from __serialize or __index here */
	if (iproto_prepare_select(out, svp) != 0)
		luaT_error(L);
	ctx->out_is_dirty = true;
	struct luaL_serializer *cfg = luaL_msgpack_default;
	struct mpstream stream;
	mpstream_init(&stream, out, obuf_reserve_cb, obuf_alloc_cb,
		      luamp_error, L);

	int count;
	if (request->type == IPROTO_CALL_16) {
		/* Tarantool < 1.7.1 compatibility */
		count = luamp_encode_call(L, cfg, &stream);
	} else {
		assert(request->type == IPROTO_CALL);
		count = lua_gettop(L);
		for (int k = 1; k <= count; ++k) {
			luamp_encode(L, cfg, &stream, k);
		}
	}

	mpstream_flush(&stream);
	iproto_reply_select(out, svp, request->header->sync, count);
	return 0; /* truncate Lua stack */
}

static int
execute_lua_eval(lua_State *L)
{
	struct lua_function_ctx *ctx = (struct lua_function_ctx *)
		lua_topointer(L, 1);
	struct request *request = ctx->request;
	struct obuf *out = ctx->out;
	struct obuf_svp *svp = &ctx->svp;
	lua_settop(L, 0); /* clear the stack to simplify the logic below */

	/* Compile expression */
	const char *expr = request->key;
	uint32_t expr_len = mp_decode_strl(&expr);
	if (luaL_loadbuffer(L, expr, expr_len, "=eval")) {
		diag_set(LuajitError, lua_tostring(L, -1));
		luaT_error(L);
	}

	/* Unpack arguments */
	const char *args = request->tuple;
	uint32_t arg_count = mp_decode_array(&args);
	luaL_checkstack(L, arg_count, "eval: out of stack");
	for (uint32_t i = 0; i < arg_count; i++) {
		luamp_decode(L, luaL_msgpack_default, &args);
	}

	/* Call compiled code */
	lua_call(L, arg_count, LUA_MULTRET);

	/* Send results of the called procedure to the client. */
	if (iproto_prepare_select(out, svp) != 0)
		diag_raise();
	ctx->out_is_dirty = true;
	struct mpstream stream;
	mpstream_init(&stream, out, obuf_reserve_cb, obuf_alloc_cb,
		      luamp_error, L);
	int nrets = lua_gettop(L);
	for (int k = 1; k <= nrets; ++k) {
		luamp_encode(L, luaL_msgpack_default, &stream, k);
	}
	mpstream_flush(&stream);
	iproto_reply_select(out, svp, request->header->sync, nrets);

	return 0;
}

static inline int
box_process_lua(struct request *request, struct obuf *out, lua_CFunction handler)
{
	struct lua_function_ctx ctx = { request, out, {0, 0, 0}, false };

	lua_State *L = lua_newthread(tarantool_L);
	int coro_ref = luaL_ref(tarantool_L, LUA_REGISTRYINDEX);
	int rc = luaT_cpcall(L, handler, &ctx);
	luaL_unref(tarantool_L, LUA_REGISTRYINDEX, coro_ref);
	if (rc != 0) {
		if (ctx.out_is_dirty) {
			/*
			 * Output buffer has been altered, rollback to svp.
			 * (!) Please note that a save point for output buffer
			 * must be taken only after finishing executing of Lua
			 * function because Lua can yield and leave the
			 * buffer in inconsistent state (a parallel request
			 * from the same connection will break the protocol).
			 */
			obuf_rollback_to_svp(out, &ctx.svp);
		}

		return -1;
	}
	return 0;
}

int
box_lua_call(struct request *request, struct obuf *out)
{
	return box_process_lua(request, out, execute_lua_call);
}

int
box_lua_eval(struct request *request, struct obuf *out)
{
	return box_process_lua(request, out, execute_lua_eval);
}

static const struct luaL_reg boxlib_internal[] = {
	{"call_loadproc",  lbox_call_loadproc},
	{NULL, NULL}
};

void
box_lua_call_init(struct lua_State *L)
{
	luaL_register(L, "box.internal", boxlib_internal);
	lua_pop(L, 1);

#if 0
	/* Get CTypeID for `struct port *' */
	int rc = luaL_cdef(L, "struct port;");
	assert(rc == 0);
	(void) rc;
	CTID_STRUCT_PORT_PTR = luaL_ctypeid(L, "struct port *");
	assert(CTID_CONST_STRUCT_TUPLE_REF != 0);
#endif
}
