
function join(inspector, n)
    local path = os.getenv('TARANTOOL_SRC_DIR')
    for i=1,n do
        local rid = tostring(i)
        os.execute('mkdir -p tmp')
        os.execute('cp '..path..'/test/replication/replica.lua ./tmp/replica'..rid..'.lua')
        os.execute('chmod +x ./tmp/replica'..rid..'.lua')
        local out_dir = box.cfg.wal_dir
        inspector:cmd("create server replica"..rid.." with rpl_master=default, script='"..out_dir.."/../tmp/replica"..rid..".lua'")
        inspector:cmd("start server replica"..rid)
    end
end


function drop_all(inspector)
    local all = box.space._cluster:select{}
    for _, tuple in pairs(all) do
        local id = tuple[1]
        if id ~= box.info.server.id then
            box.space._cluster:delete{id}
            inspector:cmd('stop server replica'..tostring(id - 1))
            inspector:cmd('delete server replica'..tostring(id - 1))
        end
    end
end

function vclock_diff(left, right)
    local diff = 0
    for id, lsn in ipairs(left) do
        diff = diff + (right[id] or 0) - left[id]
    end
    for id, lsn in ipairs(right) do
        if left[id] == nil then
            diff = diff + right[id]
        end
    end
    return diff
end

return {
    join = join;
    drop_all = drop_all;
    vclock_diff = vclock_diff;
}
