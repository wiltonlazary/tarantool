-- tuple.lua (internal file)

local ffi = require('ffi')
local yaml = require('yaml')
local msgpackffi = require('msgpackffi')
local fun = require('fun')
local buffer = require('buffer')
local internal = require('box.internal')

ffi.cdef[[
/** \cond public */
typedef struct tuple_format box_tuple_format_t;

box_tuple_format_t *
box_tuple_format_default(void);

typedef struct tuple *tuple_id;
typedef tuple_id box_tuple_t;

box_tuple_t
box_tuple_new(box_tuple_format_t *format, const char *data, const char *end);

int
box_tuple_ref(box_tuple_t tuple);

void
box_tuple_unref(box_tuple_t tuple);

uint32_t
box_tuple_field_count(box_tuple_t tuple);

size_t
box_tuple_bsize(box_tuple_t tuple);

ssize_t
box_tuple_to_buf(box_tuple_t tuple, char *buf, size_t size);

box_tuple_format_t *
box_tuple_format(box_tuple_t tuple);

const char *
box_tuple_field(box_tuple_t tuple, uint32_t i);

typedef struct tuple_iterator box_tuple_iterator_t;

box_tuple_iterator_t *
box_tuple_iterator(box_tuple_t tuple);

void
box_tuple_iterator_free(box_tuple_iterator_t *it);

uint32_t
box_tuple_position(box_tuple_iterator_t *it);

void
box_tuple_rewind(box_tuple_iterator_t *it);

const char *
box_tuple_seek(box_tuple_iterator_t *it, uint32_t field_no);

const char *
box_tuple_next(box_tuple_iterator_t *it);

/** \endcond public */

box_tuple_t
box_tuple_update(box_tuple_t tuple, const char *expr, const char *expr_end);

box_tuple_t
box_tuple_upsert(box_tuple_t tuple, const char *expr, const char *expr_end);
]]

local builtin = ffi.C

local tuple_t = ffi.typeof('struct tuple_cdata')

local TUPLE_ID_NIL = nil

local is_tuple = function(tuple)
    return tuple ~= nil and type(tuple) == 'cdata' and ffi.istype(tuple_t, tuple)
end

local encode_fix = msgpackffi.internal.encode_fix
local encode_array = msgpackffi.internal.encode_array
local encode_r = msgpackffi.internal.encode_r

tuple_encode = function(obj)
    local tmpbuf = buffer.IBUF_SHARED
    tmpbuf:reset()
    if obj == nil then
        encode_fix(tmpbuf, 0x90, 0)  -- empty array
    elseif is_tuple(obj) then
        encode_r(tmpbuf, obj, 1)
    elseif type(obj) == "table" then
        encode_array(tmpbuf, #obj)
        local i
        for i = 1, #obj, 1 do
            encode_r(tmpbuf, obj[i], 1)
        end
    else
        encode_fix(tmpbuf, 0x90, 1)  -- array of one element
        encode_r(tmpbuf, obj, 1)
    end
    return tmpbuf.rpos, tmpbuf.wpos
end

local tuple_gc = function(tuple)
    builtin.box_tuple_unref(tuple.id)
end

local tuple_bless = function(tuple_id)
    -- overflow checked by tuple_bless() in C
    builtin.box_tuple_ref(tuple_id)
    local res = ffi.new(tuple_t)
    res.id = tuple_id
    return res;
end

local tuple_check = function(tuple, usage)
    if not is_tuple(tuple) then
        error('Usage: ' .. usage)
    end
end

local tuple_iterator_t = ffi.typeof('box_tuple_iterator_t')
local tuple_iterator_ref_t = ffi.typeof('box_tuple_iterator_t &')

local function tuple_iterator(tuple)
    local it = builtin.box_tuple_iterator(tuple.id)
    if it == nil then
        box.error()
    end
    return ffi.gc(ffi.cast(tuple_iterator_ref_t, it),
        builtin.box_tuple_iterator_free)
end

local function tuple_iterator_next(it, tuple, pos)
    if pos == nil then
        pos = 0
    elseif type(pos) ~= "number" then
         error("error: invalid key to 'next'")
    end
    local curpos = builtin.box_tuple_position(it)
    local field
    if curpos == pos then
        -- Sequential iteration
        field = builtin.box_tuple_next(it)
    else
        -- Seek
        builtin.box_tuple_rewind(it)
        field = builtin.box_tuple_seek(it, pos);
    end
    if field == nil then
        if #tuple == pos then
            -- No more fields, stop iteration
            return nil
        else
            -- Invalid pos
            error("error: invalid key to 'next'")
        end
    end
    -- () used to shrink the return stack to one value
    return pos + 1, (msgpackffi.decode_unchecked(field))
end;

-- See http://www.lua.org/manual/5.2/manual.html#pdf-next
local function tuple_next(tuple, pos)
    tuple_check(tuple, "tuple:next(tuple[, pos])")
    if pos == nil then
        pos = 0
    end
    local field = builtin.box_tuple_field(tuple.id, pos)
    if field == nil then
        return nil
    end
    return pos + 1, (msgpackffi.decode_unchecked(field))
end

-- See http://www.lua.org/manual/5.2/manual.html#pdf-ipairs
local function tuple_ipairs(tuple, pos)
    tuple_check(tuple, "tuple:pairs(tuple[, pos])")
    local it = tuple_iterator(tuple)
    return fun.wrap(it, tuple, pos)
end

local function tuple_totable(tuple, i, j)
    tuple_check(tuple, "tuple:totable([from[, to]])");
    local it = tuple_iterator(tuple)
    builtin.box_tuple_rewind(it)
    local field
    if i ~= nil then
        if i < 1 then
            error('tuple.totable: invalid second argument')
        end
        field = builtin.box_tuple_seek(it, i - 1)
    else
        i = 1
        field = builtin.box_tuple_next(it)
    end
    if j ~= nil then
        if j <= 0 then
            error('tuple.totable: invalid third argument')
        end
    else
        j = 4294967295
    end
    local ret = {}
    while field ~= nil and i <= j do
        local val = msgpackffi.decode_unchecked(field)
        table.insert(ret, val)
        i = i + 1
        field = builtin.box_tuple_next(it)
    end
    return setmetatable(ret, msgpackffi.array_mt)
end

local function tuple_unpack(tuple, i, j)
    return unpack(tuple_totable(tuple, i, j))
end

local function tuple_find(tuple, offset, val)
    tuple_check(tuple, "tuple:find([offset, ]val)");
    if val == nil then
        val = offset
        offset = 0
    end
    local r = tuple:pairs(offset):index(val)
    return r ~= nil and offset + r or nil
end

local function tuple_findall(tuple, offset, val)
    tuple_check(tuple, "tuple:findall([offset, ]val)");
    if val == nil then
        val = offset
        offset = 0
    end
    return tuple:pairs(offset):indexes(val)
        :map(function(i) return offset + i end)
        :totable()
end

local function tuple_update(tuple, expr)
    tuple_check(tuple, "tuple:update({ { op, field, arg}+ })");
    if type(expr) ~= 'table' then
        error("Usage: tuple:update({ { op, field, arg}+ })")
    end
    local pexpr, pexpr_end = tuple_encode(expr)
    local tuple_id = builtin.box_tuple_update(tuple.id, pexpr, pexpr_end)
    if tuple_id == TUPLE_ID_NIL then
        return box.error()
    end
    return tuple_bless(tuple_id)
end

local function tuple_upsert(tuple, expr)
    tuple_check(tuple, "tuple:upsert({ { op, field, arg}+ })");
    if type(expr) ~= 'table' then
        error("Usage: tuple:upsert({ { op, field, arg}+ })")
    end
    local pexpr, pexpr_end = tuple_encode(expr)
    local tuple_id = builtin.box_tuple_upsert(tuple.id, pexpr, pexpr_end)
    if tuple_id == TUPLE_ID_NIL then
        return box.error()
    end
    return tuple_bless(tuple_id)
end

-- Set encode hooks for msgpackffi
local function tuple_to_msgpack(buf, tuple)
    assert(ffi.istype(tuple_t, tuple))
    local bsize = builtin.box_tuple_bsize(tuple.id)
    buf:reserve(bsize)
    builtin.box_tuple_to_buf(tuple.id, buf.wpos, bsize)
    buf.wpos = buf.wpos + bsize
end

local function tuple_bsize(tuple)
    tuple_check(tuple, "tuple:bsize()");
    return tonumber(builtin.box_tuple_bsize(tuple.id))
end

msgpackffi.on_encode(tuple_t, tuple_to_msgpack)


-- cfuncs table is set by C part

local methods = {
    ["next"]        = tuple_next;
    ["ipairs"]      = tuple_ipairs;
    ["pairs"]       = tuple_ipairs; -- just alias for ipairs()
    ["slice"]       = cfuncs.slice;
    ["transform"]   = cfuncs.transform;
    ["find"]        = tuple_find;
    ["findall"]     = tuple_findall;
    ["unpack"]      = tuple_unpack;
    ["totable"]     = tuple_totable;
    ["update"]      = tuple_update;
    ["upsert"]      = tuple_upsert;
    ["bsize"]       = tuple_bsize;
    ["__serialize"] = tuple_totable; -- encode hook for msgpack/yaml/json
}

local tuple_field = function(tuple, field_n)
    local field = builtin.box_tuple_field(tuple.id, field_n - 1)
    if field == nil then
        return nil
    end
    -- Use () to shrink stack to the first return value
    return (msgpackffi.decode_unchecked(field))
end


ffi.metatype(tuple_t, {
    __len = function(tuple)
        return builtin.box_tuple_field_count(tuple.id)
    end;
    __tostring = function(tuple)
        -- Unpack tuple, call yaml.encode, remove yaml header and footer
        -- 5 = '---\n\n' (header), -6 = '\n...\n' (footer)
        return yaml.encode(methods.totable(tuple)):sub(5, -6)
    end;
    __index = function(tuple, key)
        if type(key) == "number" then
            return tuple_field(tuple, key)
        end
        return methods[key]
    end;
    __eq = function(tuple_a, tuple_b)
        -- Two tuple are considered equal if they have same memory address
        if not tuple_b then return tuple_a.id == TUPLE_ID_NIL end --comparing with nil
        return tuple_a.id == tuple_b.id;
    end;
    __pairs = tuple_ipairs;  -- Lua 5.2 compatibility
    __ipairs = tuple_ipairs; -- Lua 5.2 compatibility
    __gc = tuple_gc;
})

ffi.metatype(tuple_iterator_t, {
    __call = tuple_iterator_next;
    __tostring = function(it) return "<tuple iterator>" end;
})

-- Remove the global variable
cfuncs = nil

-- internal api for box.select and iterators
box.tuple.bless = tuple_bless
box.tuple.encode = tuple_encode
box.tuple.is = is_tuple
