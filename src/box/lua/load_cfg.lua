-- load_cfg.lua - internal file

local log = require('log')
local json = require('json')
local private = require('box.internal')

-- see default_cfg below
local default_vinyl_cfg = {
    memory_limit      = 1.0, -- 1G
    threads           = 1,
    compact_wm        = 2, -- try to maintain less than 2 runs in a range
    range_size        = 1024 * 1024 * 1024,
    page_size        = 8 * 1024,
}

-- all available options
local default_cfg = {
    listen              = nil,
    slab_alloc_arena    = 1.0,
    slab_alloc_minimal  = 16,
    slab_alloc_maximal  = 1024 * 1024,
    slab_alloc_factor   = 1.1,
    work_dir            = nil,
    snap_dir            = ".",
    wal_dir             = ".",
    vinyl_dir          = '.',
    vinyl              = default_vinyl_cfg,
    logger              = nil,
    logger_nonblock     = true,
    log_level           = 5,
    io_collect_interval = nil,
    readahead           = 16320,
    snap_io_rate_limit  = nil, -- no limit
    too_long_threshold  = 0.5,
    wal_mode            = "write",
    rows_per_wal        = 500000,
    wal_dir_rescan_delay= 2,
    panic_on_snap_error = true,
    panic_on_wal_error  = true,
    replication_source  = nil,
    custom_proc_title   = nil,
    pid_file            = nil,
    background          = false,
    username            = nil,
    coredump            = false,
    read_only           = false,
    hot_standby	        = false,

    -- snapshot_daemon
    snapshot_period     = 0,        -- 0 = disabled
    snapshot_count      = 6,
}

-- see template_cfg below
local vinyl_template_cfg = {
    memory_limit      = 'number',
    threads           = 'number',
    compact_wm        = 'number',
    run_prio          = 'number',
    run_age           = 'number',
    run_age_period    = 'number',
    run_age_wm        = 'number',
    range_size        = 'number',
    page_size        = 'number',
}

-- types of available options
-- could be comma separated lua types or 'any' if any type is allowed
local template_cfg = {
    listen              = 'string, number',
    slab_alloc_arena    = 'number',
    slab_alloc_minimal  = 'number',
    slab_alloc_maximal  = 'number',
    slab_alloc_factor   = 'number',
    work_dir            = 'string',
    snap_dir            = 'string',
    wal_dir             = 'string',
    vinyl_dir          = 'string',
    vinyl              = vinyl_template_cfg,
    logger              = 'string',
    logger_nonblock     = 'boolean',
    log_level           = 'number',
    io_collect_interval = 'number',
    readahead           = 'number',
    snap_io_rate_limit  = 'number',
    too_long_threshold  = 'number',
    wal_mode            = 'string',
    rows_per_wal        = 'number',
    wal_dir_rescan_delay= 'number',
    panic_on_snap_error = 'boolean',
    panic_on_wal_error  = 'boolean',
    replication_source  = 'string, number, table',
    custom_proc_title   = 'string',
    pid_file            = 'string',
    background          = 'boolean',
    username            = 'string',
    coredump            = 'boolean',
    snapshot_period     = 'number',
    snapshot_count      = 'number',
    read_only           = 'boolean',
    hot_standby         = 'boolean'
}

local function normalize_uri(port)
    if port == nil or type(port) == 'table' then
        return port
    end
    return tostring(port);
end

-- options that require special handling
local modify_cfg = {
    listen             = normalize_uri,
    replication_source = normalize_uri,
}

-- dynamically settable options
local dynamic_cfg = {
    listen                  = private.cfg_set_listen,
    replication_source      = private.cfg_set_replication_source,
    log_level               = private.cfg_set_log_level,
    io_collect_interval     = private.cfg_set_io_collect_interval,
    readahead               = private.cfg_set_readahead,
    too_long_threshold      = private.cfg_set_too_long_threshold,
    snap_io_rate_limit      = private.cfg_set_snap_io_rate_limit,
    panic_on_wal_error      = function() end,
    read_only               = private.cfg_set_read_only,
    -- snapshot_daemon
    snapshot_period         = box.internal.snapshot_daemon.set_snapshot_period,
    snapshot_count          = box.internal.snapshot_daemon.set_snapshot_count,
    -- do nothing, affects new replicas, which query this value on start
    wal_dir_rescan_delay    = function() end,
    custom_proc_title       = function()
        require('title').update(box.cfg.custom_proc_title)
    end
}

local dynamic_cfg_skip_at_load = {
    wal_mode                = true,
    listen                  = true,
    replication_source      = true,
    wal_dir_rescan_delay    = true,
    panic_on_wal_error      = true,
    custom_proc_title       = true,
}

local function prepare_cfg(cfg, default_cfg, template_cfg, modify_cfg, prefix)
    if cfg == nil then
        return {}
    end
    if type(cfg) ~= 'table' then
        error("Error: cfg should be a table")
    end
    -- just pass {.. dont_check = true, ..} to disable check below
    if cfg.dont_check then
        return
    end
    local readable_prefix = ''
    if prefix ~= nil and prefix ~= '' then
        readable_prefix = prefix .. '.'
    end
    local new_cfg = {}
    for k,v in pairs(cfg) do
        local readable_name = readable_prefix .. k;
        if template_cfg[k] == nil then
            box.error(box.error.CFG, readable_name , "unexpected option")
        elseif v == "" or v == nil then
            -- "" and NULL = ffi.cast('void *', 0) set option to default value
            v = default_cfg[k]
        elseif template_cfg[k] == 'any' then
            -- any type is ok
        elseif type(template_cfg[k]) == 'table' then
            if type(v) ~= 'table' then
                box.error(box.error.CFG, readable_name, "should be a table")
            end
            v = prepare_cfg(v, default_cfg[k], template_cfg[k], modify_cfg[k], readable_name)
        elseif (string.find(template_cfg[k], ',') == nil) then
            -- one type
            if type(v) ~= template_cfg[k] then
                box.error(box.error.CFG, readable_name, "should be of type "..
                    template_cfg[k])
            end
        else
            local good_types = string.gsub(template_cfg[k], ' ', '');
            if (string.find(',' .. good_types .. ',', ',' .. type(v) .. ',') == nil) then
                good_types = string.gsub(good_types, ',', ', ');
                box.error(box.error.CFG, readable_name, "should be one of types "..
                    template_cfg[k])
            end
        end
        if modify_cfg ~= nil and type(modify_cfg[k]) == 'function' then
            v = modify_cfg[k](v)
        end
        new_cfg[k] = v
    end
    return new_cfg
end

local function apply_default_cfg(cfg, default_cfg)
    for k,v in pairs(default_cfg) do
        if cfg[k] == nil then
            cfg[k] = v
        elseif type(v) == 'table' then
            apply_default_cfg(cfg[k], v)
        end
    end
end

local function reload_cfg(oldcfg, cfg)
    local newcfg = prepare_cfg(cfg, default_cfg, template_cfg, modify_cfg)
    -- iterate over original table because prepare_cfg() may store NILs
    for key, val in pairs(cfg) do
        if dynamic_cfg[key] == nil and oldcfg[key] ~= val then
            box.error(box.error.RELOAD_CFG, key);
        end
    end
    for key in pairs(cfg) do
        local val = newcfg[key]
        local oldval = oldcfg[key]
        if oldval ~= val then
            rawset(oldcfg, key, val)
            if not pcall(dynamic_cfg[key]) then
                rawset(oldcfg, key, oldval) -- revert the old value
                return box.error() -- re-throw
            end
            log.info("set '%s' configuration option to %s", key,
                json.encode(val))
        end
    end
    if type(box.on_reload_configuration) == 'function' then
        box.on_reload_configuration()
    end
end

local box = require('box')
-- Move all box members except 'error' to box_configured
local box_configured = {}
for k, v in pairs(box) do
    box_configured[k] = v
    -- box.net.box uses box.error and box.internal
    if k ~= 'error' and k ~= 'internal' and k ~= 'index' then
        box[k] = nil
    end
end

setmetatable(box, {
    __index = function(table, index)
        error(debug.traceback("Please call box.cfg{} first"))
        error("Please call box.cfg{} first")
     end
})

local function load_cfg(cfg)
    cfg = prepare_cfg(cfg, default_cfg, template_cfg, modify_cfg)
    apply_default_cfg(cfg, default_cfg);
    -- Save new box.cfg
    box.cfg = cfg
    if not pcall(private.cfg_check)  then
        box.cfg = load_cfg -- restore original box.cfg
        return box.error() -- re-throw exception from check_cfg()
    end
    -- Restore box members after initial configuration
    for k, v in pairs(box_configured) do
        box[k] = v
    end
    setmetatable(box, nil)
    box_configured = nil
    box.cfg = setmetatable(cfg,
        {
            __newindex = function(table, index)
                error('Attempt to modify a read-only table')
            end,
            __call = reload_cfg,
        })
    private.cfg_load()
    for key, fun in pairs(dynamic_cfg) do
        local val = cfg[key]
        if val ~= nil and not dynamic_cfg_skip_at_load[key] then
            fun()
            if val ~= default_cfg[key] then
                log.info("set '%s' configuration option to %s", key, json.encode(val))
            end
        end
    end
end
box.cfg = load_cfg

-- gh-810:
-- hack luajit default cpath
-- commented out because we fixed luajit to build properly, see
-- https://github.com/luajit/luajit/issues/76
-- local format = require('tarantool').build.mod_format
-- package.cpath = package.cpath:gsub(
--     '?.so', '?.' .. format
-- ):gsub('loadall.so', 'loadall.' .. format)
