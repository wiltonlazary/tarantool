--
-- Verify that the iterator uses the state of the space before the iterator
-- was created.
--

env = require('test_run')
test_run = env.new()

test_run:cmd("setopt delimiter ';'")

function create_iterator(obj, key, opts)
	local iter, key, state = obj:pairs(key, opts)
	local res = {}
	res['iter'] = iter
	res['key'] = key
	res['state'] = state
	return res
end;

function iterator_next(iter_obj)
	local st, tp = iter_obj.iter.gen(iter_obj.key, iter_obj.state)
	return tp
end;

function iterate_over(iter_obj)
	local tp = nil
	local ret = {}
	local i = 0
	tp = iterator_next(iter_obj)
	while tp do
		ret[i] = tp
		i = i + 1
		tp = iterator_next(iter_obj)
	end
	return ret
end;

test_run:cmd("setopt delimiter ''");

--
-- Following tests verify that combinations
-- of various commands are worked correctly.
-- Combinations mentioned above are explicitly described in
-- write_iterator.test.lua.
--

space = box.schema.space.create('test', { engine = 'vinyl' })
pk = space:create_index('primary')

--
-- DELETE followed by UPSERT
--
--   1) create iterator at first
iter_obj = create_iterator(space)
space:insert({1})
space:insert({2})
space:insert({3})
space:upsert({1}, {{'!', 2, 1}})
space:upsert({2}, {{'!', 2, 2}})
space:upsert({3}, {{'!', 2, 3}})
space:select{}
space:delete{1}
space:delete{2}
space:delete{3}
space:select{}
iterate_over(iter_obj)

--   2) create iterator after initializing
space:insert({1})
space:insert({2})
space:insert({3})
iter_obj = create_iterator(space)
space:upsert({1}, {{'!', 2, 1}})
space:upsert({2}, {{'!', 2, 2}})
space:upsert({3}, {{'!', 2, 3}})
space:select{}
space:delete{1}
space:delete{2}
space:delete{3}
space:select{}
iterate_over(iter_obj)

--   3) create iterator within test case
space:insert({1})
space:insert({2})
space:insert({3})
space:upsert({1}, {{'!', 2, 1}})
space:upsert({2}, {{'!', 2, 2}})
space:upsert({3}, {{'!', 2, 3}})
iter_obj = create_iterator(space)
space:select{}
space:delete{1}
space:delete{2}
space:delete{3}
space:select{}
iterate_over(iter_obj)

--
-- UPSERT followed by DELETE
--
--   1) create iterator at first
iter_obj = create_iterator(space)
space:upsert({1}, {{'!', 2, 1}})
space:upsert({2}, {{'!', 2, 2}})
space:upsert({3}, {{'!', 2, 3}})
space:delete{1}
space:delete{2}
space:delete{3}
space:select{}
iterate_over(iter_obj)

--   2) create iterator after initializing
space:upsert({1}, {{'!', 2, 1}})
space:upsert({2}, {{'!', 2, 2}})
space:upsert({3}, {{'!', 2, 3}})
iter_obj = create_iterator(space)
space:delete{1}
space:delete{2}
space:delete{3}
space:select{}
iterate_over(iter_obj)

--   3) create iterator within test case
space:upsert({1}, {{'!', 2, 1}})
space:upsert({2}, {{'!', 2, 2}})
space:upsert({3}, {{'!', 2, 3}})
space:delete{1}
space:delete{2}
iter_obj = create_iterator(space) -- key 3 must remain
space:delete{3}
space:select{}
iterate_over(iter_obj)

--
-- UPSERT followed by UPSERT
--
--   1) create iterator at first
iter_obj = create_iterator(space)
space:upsert({1}, {{'!', 2, 1}})
space:upsert({2}, {{'!', 2, 2}})
space:upsert({3}, {{'!', 2, 3}})
space:upsert({1}, {{'!', 2, 1}})
space:upsert({2}, {{'!', 2, 2}})
space:upsert({3}, {{'!', 2, 3}})
space:select{}
iterate_over(iter_obj)
space:truncate()

--   2) create iterator after initializing
space:upsert({1}, {{'!', 2, 1}})
space:upsert({2}, {{'!', 2, 2}})
space:upsert({3}, {{'!', 2, 3}})
iter_obj = create_iterator(space)
space:upsert({1}, {{'!', 2, 1}})
space:upsert({2}, {{'!', 2, 2}})
space:upsert({3}, {{'!', 2, 3}})
space:select{}
iterate_over(iter_obj)
space:truncate()

--   3) create iterator within test case
space:upsert({1}, {{'!', 2, 1}})
space:upsert({2}, {{'!', 2, 2}})
space:upsert({3}, {{'!', 2, 3}})
space:upsert({1}, {{'!', 2, 1}})
iter_obj = create_iterator(space) -- keys 2 and 3 must remain in the old state
space:upsert({2}, {{'!', 2, 2}})
space:upsert({3}, {{'!', 2, 3}})
space:select{}
iterate_over(iter_obj)
space:truncate()

--
-- UPSERT followed by REPLACE
--
--   1) create iterator at first
iter_obj = create_iterator(space)
space:upsert({1}, {{'!', 2, 1}})
space:replace({1, 10})
space:upsert({2}, {{'!', 2, 2}})
space:replace({2, 20})
space:upsert({3}, {{'!', 2, 3}})
space:replace({3, 30})
space:select{}
iterate_over(iter_obj)
space:truncate()

--   2) create iterator after initializing
space:upsert({1}, {{'!', 2, 1}})
space:upsert({2}, {{'!', 2, 2}})
space:upsert({3}, {{'!', 2, 3}})
iter_obj = create_iterator(space)
space:replace({1, 10})
space:replace({2, 20})
space:replace({3, 30})
space:select{}
iterate_over(iter_obj)
space:truncate()

--   3) create iterator within test case
space:upsert({1}, {{'!', 2, 1}})
space:replace({1, 10})
space:upsert({2}, {{'!', 2, 2}})
space:replace({2, 20})
space:upsert({3}, {{'!', 2, 3}})
iter_obj = create_iterator(space)
space:replace({3, 30})
space:select{}
iterate_over(iter_obj)
space:truncate()

--
-- REPLACE followed by UPSERT
--
--   1) create iterator at first
iter_obj = create_iterator(space)
space:replace({1, 10})
space:replace({2, 20})
space:replace({3, 30})
space:upsert({1}, {{'!', 2, 1}})
space:upsert({2}, {{'!', 2, 2}})
space:upsert({3}, {{'!', 2, 3}})
space:select{}
iterate_over(iter_obj)
space:truncate()

--   2) create iterator after initializing
space:replace({1, 10})
space:replace({2, 20})
space:replace({3, 30})
iter_obj = create_iterator(space)
space:upsert({1}, {{'!', 2, 1}})
space:upsert({2}, {{'!', 2, 2}})
space:upsert({3}, {{'!', 2, 3}})
space:select{}
iterate_over(iter_obj)
space:truncate()

--   3) create iterator within test case
space:replace({1, 10})
space:replace({2, 20})
space:replace({3, 30})
space:upsert({1}, {{'!', 2, 1}})
iter_obj = create_iterator(space) -- keys 2 and 3 must remain in the old state
space:upsert({2}, {{'!', 2, 2}})
space:upsert({3}, {{'!', 2, 3}})
space:select{}
iterate_over(iter_obj)
space:truncate()

--
-- REPLACE followed by DELETE
--
--   1) create iterator at first
iter_obj = create_iterator(space)
space:replace({1, 10})
space:replace({2, 20})
space:replace({3, 30})
space:delete{1}
space:delete{2}
space:delete{3}
space:select{}
iterate_over(iter_obj)

--   2) create iterator after initializing
space:replace({1, 10})
space:replace({2, 20})
space:replace({3, 30})
iter_obj = create_iterator(space)
space:delete{1}
space:delete{2}
space:delete{3}
space:select{}
iterate_over(iter_obj)

--   3) create iterator within test case
space:replace({1, 10})
space:replace({2, 20})
space:replace({3, 30})
space:delete{1}
space:delete{2}
iter_obj = create_iterator(space) -- key 3 must remain
space:delete{3}
space:select{}
iterate_over(iter_obj)

--
-- DELETE followed by REPLACE
--
--   1) create iterator at first
space:insert({1, 10})
space:insert({2, 20})
space:insert({3, 30})
iter_obj = create_iterator(space)
space:delete({1})
space:delete({2})
space:delete({3})
space:replace({1})
space:replace({2})
space:replace({3})
space:select{}
iterate_over(iter_obj)
space:truncate()

--   2) create iterator after initializing
space:insert({1, 10})
space:insert({2, 20})
space:insert({3, 30})
space:delete({1})
space:delete({2})
space:delete({3})
iter_obj = create_iterator(space)
space:replace({1})
space:replace({2})
space:replace({3})
space:select{}
iterate_over(iter_obj)
space:truncate()

--   3) create iterator within test case
space:insert({1, 10})
space:insert({2, 20})
space:insert({3, 30})
space:delete({1})
space:delete({2})
space:delete({3})
space:replace({1})
space:replace({2})
iter_obj = create_iterator(space)
space:replace({3})
space:select{}
iterate_over(iter_obj)
space:truncate()

--
-- REPLACE followed by REPLACE
--
--   1) create iterator at first
iter_obj = create_iterator(space)
space:replace({1})
space:replace({2})
space:replace({3})
space:replace({1, 10})
space:replace({2, 20})
space:replace({3, 30})
space:select{}
iterate_over(iter_obj)
space:truncate()

--   2) create iterator after initializing
space:replace({1})
space:replace({2})
space:replace({3})
iter_obj = create_iterator(space)
space:replace({1, 10})
space:replace({2, 20})
space:replace({3, 30})
space:select{}
iterate_over(iter_obj)
space:truncate()

--   3) create iterator within test case
space:replace({1})
space:replace({2})
space:replace({3})
space:replace({1, 10})
space:replace({2, 20})
iter_obj = create_iterator(space)
space:replace({3, 30})
space:select{}
iterate_over(iter_obj)
space:truncate()

--
-- single UPSERT (for completeness)
--
--   1) create iterator at first
iter_obj = create_iterator(space)
space:upsert({1}, {{'!', 2, 10}})
space:upsert({2}, {{'!', 2, 20}})
space:upsert({3}, {{'!', 2, 30}})
space:select{}
iterate_over(iter_obj)
space:truncate()

--   2) create iterator after initializing
space:upsert({1}, {{'!', 2, 10}})
iter_obj = create_iterator(space)
space:upsert({2}, {{'!', 2, 20}})
space:upsert({3}, {{'!', 2, 30}})
space:select{}
iterate_over(iter_obj)
space:truncate()

--   3) create iterator within test case
space:upsert({1}, {{'!', 2, 10}})
space:upsert({2}, {{'!', 2, 20}})
space:upsert({3}, {{'!', 2, 30}})
iter_obj = create_iterator(space)
space:select{}
iterate_over(iter_obj)
space:truncate()

--
-- single REPLACE (for completeness)
--
--   1) create iterator at first
iter_obj = create_iterator(space)
space:replace({1})
space:replace({2})
space:replace({3})
space:select{}
iterate_over(iter_obj)
space:truncate()

--   2) create iterator after initializing
space:replace({1})
space:replace({2})
iter_obj = create_iterator(space)
space:replace({3})
space:select{}
iterate_over(iter_obj)
space:truncate()

--   3) create iterator within test case
space:replace({1})
space:replace({2})
space:replace({3})
iter_obj = create_iterator(space)
space:select{}
iterate_over(iter_obj)
space:truncate()

space:drop()
--
-- gh-1797
-- Test another iterator types and move the iterator
-- during the space is modified, try to pass keys in pairs()
-- invocations.
--


-- Test iterator type EQ

space1 = box.schema.space.create('test1', { engine = 'vinyl' })
pk = space1:create_index('primary')
space1:replace({1})
space1:replace({2})
space1:replace({3})
space1:upsert({3}, {{'!', 2, 3}})
space1:upsert({5}, {{'!', 2, 5}})
iter_obj_sp1 = create_iterator(space1, 3, {iterator = box.index.EQ})
space1:replace({6})
iterator_next(iter_obj_sp1)
space1:replace({8})
space1:select{}
iterate_over(iter_obj_sp1)
space1:drop()

-- Test iterator type GT

space2 = box.schema.space.create('test2', { engine = 'vinyl' })
pk = space2:create_index('primary')
space2:replace({1})
space2:replace({2})
space2:replace({3})
space2:replace({4})
space2:replace({5})
iter_obj_sp2 = create_iterator(space2, 3, {iterator = box.index.GT})

-- Test iterator type GE

space3 = box.schema.space.create('test3', { engine = 'vinyl' })
pk = space3:create_index('primary')
space3:replace({1})
space3:replace({2})
space3:replace({3})
space3:replace({4})
space3:replace({5})
iter_obj_sp3 = create_iterator(space3, 3, {iterator = box.index.GE})

-- Test iterator type LT and LE simultaneously

space4 = box.schema.space.create('test4', { engine = 'vinyl' })
pk = space4:create_index('primary')
space4:replace({1})
space4:replace({2})
space4:replace({3})
space4:upsert({3}, {{'!', 2, 3}})
space4:upsert({5}, {{'!', 2, 5}})
iter_obj_sp4 = create_iterator(space4, 3, {iterator = box.index.LE})
iter_obj_sp4_2 = create_iterator(space4, 3, {iterator = box.index.LT})
space4:replace({6})

-- Snapshot for all spaces
box.snapshot()

-- Continue GT
space2:replace({6})
iterator_next(iter_obj_sp2)
space2:replace({8})
space2:select{}

-- Continue GE
space3:replace({6})
iterator_next(iter_obj_sp3)
space3:replace({8})
space3:select{}

-- Continue LT and LE
iterator_next(iter_obj_sp4)
space4:replace({8})
iterator_next(iter_obj_sp4_2)
space4:select{}

-- Snapshot for all spaces
box.snapshot()

-- Continue GT
iterate_over(iter_obj_sp2)
space2:truncate()
space2:drop()

-- Continue GE
iterate_over(iter_obj_sp3)
space3:truncate()
space3:drop()

-- Continue LT and LE
iterate_over(iter_obj_sp4)
iterate_over(iter_obj_sp4_2)
space4:truncate()
space4:drop()

--
-- Test same with multiple indexes.
--

space = box.schema.space.create('test', { engine = 'vinyl' })
pk = space:create_index('primary')
idx2 = space:create_index('idx2', { parts = {2, 'unsigned'} })
idx3 = space:create_index('idx3', { parts = {3, 'integer'}, unique = false })

-- Test iterator type EQ

space:select{}
iter_obj = create_iterator(space, 1, {iterator = 'EQ'}) -- always must return nothing

space:replace({1, 2, 3})
space:delete({1})
space:replace({1, 1, 1})
space:upsert({1, 1, 1}, {{'+', 2, 1}, {'+', 3, 2}})
space:select{}
iterate_over(iter_obj) -- return nothing

iter_obj2 = create_iterator(idx2, 2, {iterator = 'EQ'})
space:delete({1}) -- delete {1, 2, 3}, but iter_obj2 must return it
iterate_over(iter_obj2)

space:truncate()

-- Test iterators inside the transaction, but before create several iterators with
-- various space states.

space:replace({1, 1, 1})
space:replace({2, 4, 1})
space:replace({3, 8, 1})
iter_obj = create_iterator(space, 2, {iterator = 'GT'}) -- must return only 3
space:replace({4, 16, -1})
space:replace({5, 32, -10})
iter_obj2 = create_iterator(idx3, 0, {iterator = 'LE'}) -- must return -1 and -10
space:replace({6, 64, -10})
iter_obj3 = create_iterator(idx3, 0, {iterator = 'GE'}) -- must return {1} * 3
box.begin()
space:replace({7, 128, 20})
iter_obj4 = create_iterator(space) -- must fail after rollback
box.rollback()
space:select{}
iterate_over(iter_obj)
iterate_over(iter_obj2)
iterate_over(iter_obj3)
iterate_over(iter_obj4)
space:truncate()

-- Iterate within transaction
space:replace({1, 1, 1})
box.begin()
space:replace({2, 2, 1})
iter_obj = create_iterator(pk, 1, {iterator = 'GE'})
space:replace({3, 3, 10})
iter_obj2 = create_iterator(idx3, 20, {iterator = 'LT'})
space:replace({4, 4, 15})
space:replace({5, 5, 25})

-- Must print all, include tuples added after the iterator creation
-- because of the opened transaction presense.
iterate_over(iter_obj)
iterator_next(iter_obj2)
space:replace({12, 12, 12})
iterator_next(iter_obj2)
space:replace({9, 9, 9})
iterate_over(iter_obj2)
box.commit()
space:truncate()

-- Create the iterator before the transaction, but iterate inside
space:replace({1, 1, 1})
space:replace({2, 2, 2})
iter_obj = create_iterator(pk)
iter_obj2 = create_iterator(idx2, 2, {iterator = 'GE'})
space:replace({3, 3, 3})
box.begin()
space:replace({4, 4, 4})
iterate_over(iter_obj)
iterate_over(iter_obj2)
box.commit()
space:truncate()

-- Create the iterator inside the transaction, but iterate outside
space:replace({1, 1, 1})
box.begin()
space:replace({2, 2, 2})
iter_obj = create_iterator(pk)
space:replace({3, 3, 3})
box.commit()
iterate_over(iter_obj)
space:truncate()

-- Create the iterator inside the transaction before any other actions
-- and iterate inside
space:replace({1, 1, 1})
box.begin()
iter_obj = create_iterator(pk)
space:replace({2, 2, 2})
iterate_over(iter_obj)
box.commit()

space:drop()
