-- snapshot_daemon.lua (internal file)

local log = require 'log'
local fiber = require 'fiber'
local fio = require 'fio'
local yaml = require 'yaml'
local errno = require 'errno'
local digest = require 'digest'
local pickle = require 'pickle'

local PREFIX = 'snapshot_daemon'

local daemon = {
    snapshot_period = 0;
    snapshot_count = 6;
    fiber = nil;
    control = nil;
}

local function sprintf(fmt, ...) return string.format(fmt, ...) end

-- create snapshot, return true if no errors
local function snapshot()
    log.info("making snapshot...")
    local s, e = pcall(function() box.snapshot() end)
    if s then
        return true
    end
    -- don't complain in the log if the snapshot already exists
    if errno() == errno.EEXIST then
        return false
    end
    log.error("error while creating snapshot: %s", e)
    return false
end

-- create snapshot
local function make_snapshot(last_snap)

    if daemon.snapshot_period == nil then
        return false
    end

    if not(daemon.snapshot_period > 0) then
        return false
    end


    if last_snap == nil then
        return snapshot()
    end

    local vclock = box.info.vclock
    local lsn = 0
    for i, vlsn in pairs(vclock) do
        lsn = lsn + vlsn
    end

    local snap_name = sprintf('%020d.snap', tonumber(lsn))
    if fio.basename(last_snap) == snap_name then
        if daemon.last_snap_name ~= snap_name then
            daemon.last_snap_name = snap_name
            log.debug('snapshot file %s already exists', last_snap)
        end
        return false
    end
    daemon.last_snap_name = snap_name

    local snstat = fio.stat(last_snap)
    if snstat == nil then
        log.error("can't stat %s: %s", last_snap, errno.strerror())
        return false
    end
    if snstat.mtime <= fiber.time() + daemon.snapshot_period then
        return snapshot()
    end
end

-- check filesystem and current time
local function process(self)
    local snaps = fio.glob(fio.pathjoin(box.cfg.snap_dir, '*.snap'))

    if snaps == nil then
        log.error("can't read snap_dir %s: %s", box.cfg.snap_dir,
                  errno.strerror())
        return
    end

    if not make_snapshot(snaps[#snaps]) then
        return
    end

    -- cleanup code
    if daemon.snapshot_count == nil then
        return
    end

    if not (self.snapshot_count > 0) then
        return
    end


    -- reload snap list after snapshot
    snaps = fio.glob(fio.pathjoin(box.cfg.snap_dir, '*.snap'))
    local xlogs = fio.glob(fio.pathjoin(box.cfg.wal_dir, '*.xlog'))
    if xlogs == nil then
        log.error("can't read wal_dir %s: %s", box.cfg.wal_dir,
                  errno.strerror())
        return
    end

    while #snaps > self.snapshot_count do
        local rm = snaps[1]
        table.remove(snaps, 1)

        log.info("removing old snapshot %s", rm)
        if not fio.unlink(rm) then
            log.error("error while removing %s: %s",
                      rm, errno.strerror())
            return
        end
    end


    local snapno = fio.basename(snaps[1], '.snap')

    while #xlogs > 0 do
        if #xlogs < 2 then
            break
        end

        if fio.basename(xlogs[1], '.xlog') > snapno then
            break
        end

        if fio.basename(xlogs[2], '.xlog') > snapno then
            break
        end


        local rm = xlogs[1]
        table.remove(xlogs, 1)
        log.info("removing old xlog %s", rm)

        if not fio.unlink(rm) then
            log.error("error while removing %s: %s",
                      rm, errno.strerror())
            return
        end
    end
end

local function daemon_fiber(self)
    fiber.name(PREFIX)
    log.info("started")

    --
    -- Add random offset to the initial period to avoid simultaneous
    -- snapshotting when multiple instances of tarantool are running
    -- on the same host.
    -- See https://github.com/tarantool/tarantool/issues/732
    --
    local random = pickle.unpack('i', digest.urandom(4))
    local offset = random % self.snapshot_period
    while true do
        local period = self.snapshot_period + offset
        -- maintain next_snapshot_time as a self member for testing purposes
        self.next_snapshot_time = fiber.time() + period
        log.info("scheduled the next snapshot at %s",
                os.date("%c", self.next_snapshot_time))
        local msg = self.control:get(period)
        if msg == 'shutdown' then
            break
        elseif msg == 'reload' then
            log.info("reloaded") -- continue
        elseif msg == nil and box.info.status == 'running' then
            local s, e = pcall(process, self)
            if not s then
                log.error(e)
            end
            offset = 0
        end
    end
    self.next_snapshot_time = nil
    log.info("stopped")
end

local function reload(self)
    if self.snapshot_period > 0 then
        if self.control == nil then
            -- Start daemon
            self.control = fiber.channel()
            self.fiber = fiber.create(daemon_fiber, self)
            fiber.sleep(0)
        else
            -- Reload daemon
            self.control:put("reload")
            --
            -- channel:put() doesn't block the writer if there
            -- is a ready reader. Give daemon fiber way so that
            -- it can execute before reload() returns to the caller.
            --
            fiber.sleep(0)
        end
    elseif self.control ~= nil then
        -- Shutdown daemon
        self.control:put("shutdown")
        self.fiber = nil
        self.control = nil
        fiber.sleep(0) -- see comment above
    end
end

setmetatable(daemon, {
    __index = {
        set_snapshot_period = function()
            daemon.snapshot_period = box.cfg.snapshot_period
            reload(daemon)
            return
        end,

        set_snapshot_count = function()
            if math.floor(box.cfg.snapshot_count) ~= box.cfg.snapshot_count then
                box.error(box.error.CFG, "snapshot_count",
                         "must be an integer")
            end
            daemon.snapshot_count = box.cfg.snapshot_count
            reload(daemon)
        end
    }
})

if box.internal == nil then
    box.internal = { [PREFIX] = daemon }
else
    box.internal[PREFIX] = daemon
end
