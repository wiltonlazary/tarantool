test_run = require('test_run').new()
fiber = require('fiber')

space = box.schema.space.create("vinyl", { engine = 'vinyl' })
_= space:create_index('primary', { parts = { 1, 'unsigned' }, compact_wm = 3 })

function vyinfo() return box.info.vinyl().db[box.space.vinyl.id..'/0'] end

vyinfo().run_count == 0

-- create the frist run
space:insert({1})
space:replace({1, 2})
space:upsert({1},{{'=', 4, 5}}) -- bad upsert
require('log').info(string.rep(" ", 1024))
space:select()
space:select()
-- gh-1571: bad upsert should not log on reads
test_run:grep_log('default', 'UPSERT operation failed', 1024) == nil

box.snapshot()
vyinfo().run_count == 1

-- create the second run
space:replace({2,2})
space:upsert({2},{{'=',4,5}}) -- bad upsert
box.snapshot() -- create the second run
vyinfo().run_count == 2
space:insert({3, 3})
box.snapshot() -- create the third run to trigger compaction

-- wait for compaction
while vyinfo().run_count >= 2 do fiber.sleep(0.1) end
vyinfo().run_count == 1

-- gh-1571: bad upsert should log on compaction
test_run:grep_log('default', 'UPSERT operation failed') ~= nil

space:drop()

fiber = nil
test_run = nil
