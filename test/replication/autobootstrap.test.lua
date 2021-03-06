env = require('test_run')
vclock_diff = require('fast_replica').vclock_diff
test_run = env.new()


SERVERS = { 'autobootstrap1', 'autobootstrap2', 'autobootstrap3' }

--
-- Start servers
--
test_run:create_cluster(SERVERS)

--
-- Wait for full mesh
--
test_run:wait_fullmesh(SERVERS)

--
-- Check vclock
--
vclock1 = test_run:get_vclock('autobootstrap1')
vclock_diff(vclock1, test_run:get_vclock('autobootstrap2'))
vclock_diff(vclock1, test_run:get_vclock('autobootstrap3'))

--
-- Insert rows on each server
--
_ = test_run:cmd("switch autobootstrap1")
_ = box.space.test:insert({box.info.server.id})
_ = test_run:cmd("switch autobootstrap2")
_ = box.space.test:insert({box.info.server.id})
_ = test_run:cmd("switch autobootstrap3")
_ = box.space.test:insert({box.info.server.id})
_ = test_run:cmd("switch default")

--
-- Synchronize
--

vclock = test_run:get_cluster_vclock(SERVERS)
vclock2 = test_run:wait_cluster_vclock(SERVERS, vclock)
vclock_diff(vclock1, vclock2)

--
-- Check result
--
_ = test_run:cmd("switch autobootstrap1")
box.space.test:select()
_ = test_run:cmd("switch autobootstrap2")
box.space.test:select()
_ = test_run:cmd("switch autobootstrap3")
box.space.test:select()
_ = test_run:cmd("switch default")

--
-- Stop servers
--
test_run:drop_cluster(SERVERS)
