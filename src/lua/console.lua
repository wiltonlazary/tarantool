-- console.lua -- internal file

local internal = require('console')
local fiber = require('fiber')
local socket = require('socket')
local log = require('log')
local errno = require('errno')
local urilib = require('uri')
local yaml = require('yaml')

-- admin formatter must be able to encode any Lua variable
local formatter = yaml.new()
formatter.cfg{
    encode_invalid_numbers = true;
    encode_load_metatables = true;
    encode_use_tostring    = true;
    encode_invalid_as_nil  = true;
}

local function format(status, ...)
    -- When storing a nil in a Lua table, there is no way to
    -- distinguish nil value from no value. This is a trick to
    -- make sure yaml converter correctly
    local function wrapnull(v)
        return v == nil and formatter.NULL or v
    end
    local err
    if status then
        local count = select('#', ...)
        if count == 0 then
            return "---\n...\n"
        end
        local res = {}
        for i=1,count,1 do
            table.insert(res, wrapnull(select(i, ...)))
        end
        -- serializer can raise an exception
        status, err = pcall(formatter.encode, res)
        if status then
            return err
        else
            err = 'console: an exception occurred when formatting the output: '..
                tostring(err)
        end
    else
        err = wrapnull(...)
    end
    return formatter.encode({{error = err }})
end

--
-- Evaluate command on local server
--
local function local_eval(self, line)
    if not line then
        return nil
    end
    --
    -- Attempt to append 'return ' before the chunk: if the chunk is
    -- an expression, this pushes results of the expression onto the
    -- stack. If the chunk is a statement, it won't compile. In that
    -- case try to run the original string.
    --
    local fun, errmsg = loadstring("return "..line)
    if not fun then
        fun, errmsg = loadstring(line)
    end
    if not fun then
        return format(false, errmsg)
    end
    return format(pcall(fun))
end

local function eval(line)
    return local_eval(nil, line)
end

--
-- Evaluate command on remote server
--
local function remote_eval(self, line)
    if not line or self.remote.state ~= 'active' then
        local err = self.remote.error
        self.remote:close()
        self.remote = nil
        -- restore local REPL mode
        self.eval = nil
        self.prompt = nil
        self.completion = nil
        pcall(self.on_client_disconnect, self)
        return (err and format(false, err)) or ''
    end
    --
    -- execute line
    --
    local ok, res = pcall(self.remote.eval, self.remote, line)
    return ok and res or format(false, res)
end

--
-- Read command from stdin
--
local function local_read(self)
    local buf = ""
    local prompt = self.prompt
    while true do
        local delim = self.delimiter
        local line = internal.readline({
            prompt = prompt.. "> ",
            completion = self.ac and self.completion or nil
        })
        if not line then
            return nil
        end
        buf = buf..line
        if delim == "" then
            -- stop once a complete Lua statement is entered
            local fn, err = loadstring(buf)
            if fn ~= nil or not string.find(err, " near '<eof>'$") then
                -- valid Lua code or a syntax error not due to
                -- an incomplete input
                break
            end
            if loadstring('return '..buf) ~= nil then
                -- certain obscure inputs like '(42\n)' yield the
                -- same error as incomplete statement
                break
            end
        elseif #buf >= #delim and buf:sub(#buf - #delim + 1) == delim then
            buf = buf:sub(0, #buf - #delim)
            break
        end
        buf = buf.."\n"
        prompt = string.rep(' ', #self.prompt)
    end
    internal.add_history(buf)
    if self.history_file then
        internal.save_history(self.history_file)
    end
    return buf
end

--
-- Print result to stdout
--
local function local_print(self, output)
    if output == nil then
        self.running = nil
        return
    end
    print(output)
end

--
-- Read command from connected client console.listen()
--
local function client_read(self)
    local delim = self.delimiter.."\n"
    local buf = self.client:read(delim)
    if buf == nil then
        return nil
    elseif buf == "" then
        return nil -- EOF
    elseif buf == "~.\n" then
        -- Escape sequence to close current connection (like SSH)
        return nil
    end
    -- remove trailing delimiter
    return buf:sub(1, -#delim-1)
end

--
-- Print result to connected client from console.listen()
--
local function client_print(self, output)
    if not self.client then
        return
    elseif not output then
        -- disconnect peer
        self.client = nil -- socket will be closed by tcp_server() function
        self.running = nil
        return
    end
    self.client:write(output)
end

--
-- REPL state
--
local repl_mt = {
    __index = {
        running = false;
        delimiter = "";
        prompt = "tarantool";
        read = local_read;
        eval = local_eval;
        print = local_print;
        completion = internal.completion_handler;
        ac = true;
    };
}

--
-- REPL = read-eval-print-loop
--
local function repl(self)
    
    fiber.self().storage.console = self
    if type(self.on_start) == 'function' then
        self:on_start()
    end

    while self.running do
        local command = self:read()
        local output = self:eval(command)
        self:print(output)
    end
    fiber.self().storage.console = nil
end

local function on_start(foo)
    if foo == nil or type(foo) == 'function' then
        repl_mt.__index.on_start = foo 
        return
    end
    error('Wrong type of on_start hook: ' .. type(foo))
end

local function on_client_disconnect(foo)
    if foo == nil or type(foo) == 'function' then
        repl_mt.__index.on_client_disconnect = foo 
        return
    end
    error('Wrong type of on_client_disconnect hook: ' .. type(foo))
end

--
-- Set delimiter
--
local function delimiter(delim)
    local self = fiber.self().storage.console
    if self == nil then
        error("console.delimiter(): need existing console")
    end
    if delim == nil then
        return self.delimiter
    elseif type(delim) == 'string' then
        self.delimiter = delim
    else
        error('invalid delimiter')
    end
end

--
--
--
local function ac(yes_no)
    local self = fiber.self().storage.console
    if self == nil then
        error("console.ac(): need existing console")
    end
    self.ac = not not yes_no
end

--
-- Start REPL on stdin
--
local started = false
local function start()
    if started then
        error("console is already started")
    end
    started = true
    local self = setmetatable({ running = true }, repl_mt)
    local home_dir = os.getenv('HOME')
    if home_dir then
        self.history_file = home_dir .. '/.tarantool_history'
        internal.load_history(self.history_file)
    end
    repl(self)
    started = false
end

--
-- Connect to remove server
--
local netbox_connect
local function connect(uri)
    if not netbox_connect then -- workaround the broken loader
        netbox_connect = require('net.box').connect
    end

    local self = fiber.self().storage.console
    if self == nil then
        error("console.connect() need existing console")
    end

    local u
    if uri then
        u = urilib.parse(tostring(uri))
    end
    if u == nil or u.service == nil then
        error('Usage: console.connect("[login:password@][host:]port")')
    end

    -- connect to remote host
    local remote
    remote = netbox_connect(u.host, u.service, {
        user = u.login, password = u.password, console = true
    })
    remote.host, remote.port = u.host or 'localhost', u.service

    -- run disconnect trigger if connection failed
    if not remote:is_connected() then
        pcall(self.on_client_disconnect, self)
        error('Connection is not established: '..remote.error)
    end

    -- check connection && permissions
    local ok, res = pcall(remote.eval, remote, 'return true')
    if not ok then
        remote:close()
        pcall(self.on_client_disconnect, self)
        error(res)
    end

    -- override methods
    self.remote = remote
    self.eval = remote_eval
    self.prompt = string.format("%s:%s", self.remote.host, self.remote.port)
    self.completion = function (str, pos1, pos2)
        local c = string.format(
            'return require("console").completion_handler(%q, %d, %d)',
            str, pos1, pos2)
        return yaml.decode(remote:eval(c))[1]
    end
    log.info("connected to %s:%s", self.remote.host, self.remote.port)
    return true
end

local function client_handler(client, peer)
    log.info("client %s:%s connected", peer.host, peer.port)
    local state = setmetatable({
        running = true;
        read = client_read;
        print = client_print;
        client = client;
    }, repl_mt)
    local version = _TARANTOOL
    state:print(string.format("%-63s\n%-63s\n",
        "Tarantool ".. version.." (Lua console)",
        "type 'help' for interactive help"))
    repl(state)
    log.info("client %s:%s disconnected", peer.host, peer.port)
end

--
-- Start admin server
--
local function listen(uri)
    local host, port
    if uri == nil then
        host = 'unix/'
        port = '/tmp/tarantool-console.sock'
    else
        local u = urilib.parse(tostring(uri))
        if u == nil or u.service == nil then
            error('Usage: console.listen("[host:]port")')
        end
        host = u.host
        port = u.service or 3313
    end
    local s, addr = socket.tcp_server(host, port, { handler = client_handler,
        name = 'console'})
    if not s then
        error(string.format('failed to create server %s:%s: %s',
            host, port, errno.strerror()))
    end
    return s
end

return {
    start = start;
    eval = eval;
    delimiter = delimiter;
    ac = ac;
    connect = connect;
    listen = listen;
    on_start = on_start;
    on_client_disconnect = on_client_disconnect;
    completion_handler = internal.completion_handler;
}
