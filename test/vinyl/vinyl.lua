#!/usr/bin/env tarantool

box.cfg {
    listen            = os.getenv("LISTEN"),
    slab_alloc_arena  = 0.5,
    slab_alloc_maximal = 4 * 1024 * 1024,
    rows_per_wal      = 1000000,
    vinyl = {
        threads = 3;
        memory_limit = 0.5;
        range_size = 1024*64;
        page_size = 1024;
    }
}

function box_info_sort(data)
    if type(data)~='table' then
        return data
    end
    local keys = {}
    for k in pairs(data) do
        table.insert(keys, k)
    end
    table.sort(keys)
    local result = {}
    for _,k in pairs(keys) do
        local v = data[k]
        table.insert(result, {[k] = box_info_sort(v) })
    end
    return result
end

require('console').listen(os.getenv('ADMIN'))
