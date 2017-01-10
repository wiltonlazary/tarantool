--
-- gh-1233: JOIN/SUBSCRIBE must fail if master has wal_mode = "none"
--

env = require('test_run')
test_run = env.new()
test_run:cmd('switch default')
box.schema.user.grant('guest', 'replication')
test_run:cmd("create server wal_off with rpl_master=default, script='replication/wal_off.lua'")
test_run:cmd("start server wal_off")
test_run:cmd('switch default')
wal_off_uri = test_run:eval('wal_off', 'return box.cfg.listen')[1]
wal_off_uri ~= nil
wal_off_id = test_run:eval('wal_off', 'return box.info.server.id')[1]

box.cfg { replication_source = wal_off_uri }
-- Replication does not support wal_mode = 'none'
box.info.replication[wal_off_id].message
box.info.replication[wal_off_id].status
box.cfg { replication_source = "" }

test_run:cmd('switch wal_off')
box.schema.user.revoke('guest', 'replication')
test_run:cmd('switch default')

box.cfg { replication_source = wal_off_uri }
-- Read access is denied
box.info.replication[wal_off_id].message
box.info.replication[wal_off_id].status
box.cfg { replication_source = "" }

test_run:cmd("stop server wal_off")
test_run:cmd("cleanup server wal_off")
