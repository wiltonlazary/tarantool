#!/usr/bin/env tarantool

local math = require('math')
local fiber = require('fiber')
local tap = require('tap')
local ffi = require('ffi')
local fio = require('fio')

box.cfg{ logger="tarantool.log", slab_alloc_arena=0.1, rows_per_wal=5000}

local test = tap.test("snapshot")
test:plan(3)

-------------------------------------------------------------------------------
-- gh-695: Avoid overwriting tuple data with information necessary for smfree()
-------------------------------------------------------------------------------

local continue_snapshoting = true
local snap_chan = fiber.channel()

local function noise()
    fiber.name('noise-'..fiber.id())
    while continue_snapshoting do
        if box.space.test:len() < 300000 then
            local  value = string.rep('a', math.random(255)+1)
            box.space.test:auto_increment{fiber.time64(), value}
        end
        fiber.sleep(0)
    end
end

local function purge()
    fiber.name('purge-'..fiber.id())
    while continue_snapshoting do
        local min = box.space.test.index.primary:min()
        if min ~= nil then
            box.space.test:delete{min[1]}
        end
        fiber.sleep(0)
    end
end

local function snapshot(lsn)
    fiber.name('snapshot')
    while continue_snapshoting do
        local new_lsn = box.info.server.lsn
        if new_lsn ~= lsn then
            lsn = new_lsn;
            pcall(box.snapshot)
        end
        fiber.sleep(0.001)
    end
    snap_chan:put("!")
end

box.once("snapshot.test", function()
    box.schema.space.create('test')
    box.space.test:create_index('primary')
end)

fiber.create(noise)
fiber.create(purge)
fiber.create(noise)
fiber.create(purge)
fiber.create(noise)
fiber.create(purge)
fiber.create(noise)
fiber.create(purge)
fiber.create(snapshot, box.info.server.lsn)

fiber.sleep(0.3)
continue_snapshoting = false
snap_chan:get()

test:ok(true, 'gh-695: avoid overwriting tuple data necessary for smfree()')

-------------------------------------------------------------------------------
-- gh-1185: Crash in matras_touch in snapshot_daemon.test 
-------------------------------------------------------------------------------

local s1 = box.schema.create_space('test1', { engine = 'memtx'})
local i1 = s1:create_index('test', { type = 'tree', parts = {1, 'unsigned'} })

local s2 = box.schema.create_space('test2', { engine = 'memtx'})
local i2 = s2:create_index('test', { type = 'tree', parts = {1, 'unsigned'} })

for i = 1,1000 do s1:insert{i, i, i} end

fiber.create(function () box.snapshot() end)

fiber.sleep(0)

s2:insert{1, 2, 3}
s2:update({1}, {{'+', 2, 2}})

s1:drop()
s2:drop()

test:ok(true, "gh-1185: no crash in matras_touch")

-------------------------------------------------------------------------------
-- gh-1084: box.snapshot() aborts if the server is out of file descriptors
-------------------------------------------------------------------------------

local function gh1094()
    local msg = "gh-1094: box.snapshot() doesn't abort if out of file descriptors"
    local nfile
    local ulimit = io.popen('ulimit -n')
    if ulimit then
        nfile = tonumber(ulimit:read())
        ulimit:close()
    end

    if not nfile or nfile > 1024 then
        -- descriptors limit is to high, just skip test
        test:ok(true, msg)
        return
    end
    local files = {}
    for i = 1,nfile do
        files[i] = fio.open('/dev/null')
        if files[i] == nil then
            break
        end
    end
    local sf, mf = pcall(box.snapshot)
    for i, f in pairs(files) do
        f:close()
    end
    local ss, ms = pcall(box.snapshot)
    test:ok(not sf and ss, msg)
end
gh1094()

test:check()
os.exit(0)
