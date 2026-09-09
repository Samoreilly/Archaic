local M = {}

M._config = {
    socket_path = "/tmp/archaic-daemon.sock",
    timeout_ms = 100,
    enable_cmp = true,
    enable_telescope = true,
}

M._connected = false
M._pipe = nil

-- Health API helpers: use vim.health (Neovim 0.7+) or fall back to vim.notify
local function _health_api()
    if vim.health then
        return {
            start  = vim.health.start or vim.health.report_start,
            ok     = vim.health.ok or vim.health.report_ok,
            warn   = vim.health.warn or vim.health.report_warn,
            error  = vim.health.error or vim.health.report_error,
            info   = vim.health.info or vim.health.report_info,
        }
    end
    return {
        start = function(s) vim.notify(s, vim.log.levels.INFO) end,
        ok    = function(s) vim.notify("OK: " .. s, vim.log.levels.INFO) end,
        warn  = function(s) vim.notify("WARN: " .. s, vim.log.levels.WARN) end,
        error = function(s) vim.notify("ERROR: " .. s, vim.log.levels.ERROR) end,
        info  = function(s) vim.notify(s, vim.log.levels.INFO) end,
    }
end

function M.setup(opts)
    M._config = vim.tbl_deep_extend("force", M._config, opts or {})

    vim.api.nvim_create_user_command("ArchaicStatus", function()
        M.check_health()
    end, { desc = "Check archaic daemon status" })

    vim.api.nvim_create_user_command("ArchaicReindex", function(cmd_opts)
        M.reindex(cmd_opts.args)
    end, { nargs = "?", desc = "Trigger daemon rescan" })

    vim.api.nvim_create_user_command("ArchaicToggle", function()
        M.toggle()
    end, { desc = "Toggle archaic completions" })

    vim.api.nvim_create_user_command("ArchaicCheckHealth", function()
        M.check_health()
    end, { desc = "Check archaic daemon health" })

    vim.api.nvim_create_user_command("ArchaicInfo", function()
        M.quick_info()
    end, { desc = "Show quick archaic status (one line)" })
end

function M.toggle()
    vim.g.archaic_disabled = not vim.g.archaic_disabled
    local state = vim.g.archaic_disabled and "disabled" or "enabled"
    vim.notify("archaic: " .. state, vim.log.levels.INFO)
end

function M.reindex(path)
    local scan_path = path or M._config.scan_path or vim.fn.getcwd()
    M._send_request("SCAN", { path = scan_path }, function(resp)
        if resp and resp.status then
            vim.notify("archaic: reindex started for " .. scan_path, vim.log.levels.INFO)
        else
            vim.notify("archaic: reindex failed - daemon not running", vim.log.levels.WARN)
        end
    end)
end

function M.check_health()
    local h = _health_api()
    local socket_path = vim.g.archaic_socket_path or M._config.socket_path

    h.start("archaic.nvim")

    -- Configuration
    h.info(string.format("socket_path: %s", socket_path))
    h.info(string.format("timeout_ms: %d", M._config.timeout_ms))
    h.info(string.format("enable_cmp: %s", tostring(M._config.enable_cmp)))
    h.info(string.format("enable_telescope: %s", tostring(M._config.enable_telescope)))

    -- Socket file existence
    local socket_exists = vim.uv.fs_stat(socket_path) ~= nil
    if socket_exists then
        h.ok("Socket file exists")
    else
        h.error(string.format("Socket not found at %s", socket_path))
        h.error("Daemon not running — start with: ./run.sh start ~/projects")
    end

    -- Daemon ping with response time
    local ping_start = vim.uv.hrtime()
    M._send_request("PING", {}, function(resp)
        local elapsed = math.floor((vim.uv.hrtime() - ping_start) / 1e6)
        if resp then
            local uptime = resp.uptime_ms and (tostring(resp.uptime_ms) .. "ms") or "unknown"
            h.ok(string.format("Daemon responded in %dms (uptime: %s)", elapsed, uptime))
        else
            if socket_exists then
                h.warn("Socket exists but daemon did not respond")
                h.warn("Daemon may be stuck — try: ./run.sh restart ~/projects")
            end
        end
    end)

    -- nvim-cmp source registration
    if M._config.enable_cmp then
        if package.loaded["cmp"] then
            local found = false
            local cmp_core = package.loaded["cmp"].core
            if cmp_core and cmp_core.sources then
                for _, src in ipairs(cmp_core.sources) do
                    local name = src.name
                    if not name and src.source and src.source.get_debug_name then
                        name = src.source:get_debug_name()
                    end
                    if name == "archaic" then
                        found = true
                        break
                    end
                end
            end
            if found then
                h.ok("nvim-cmp source registered")
            else
                h.warn("nvim-cmp loaded but archaic source not registered")
                h.warn("Fix: require('cmp').register_source('archaic', require('archaic.cmp_source'))")
            end
        else
            h.warn("nvim-cmp not loaded — install nvim-cmp for autocomplete")
        end
    else
        h.warn("cmp integration disabled (enable_cmp = false)")
    end

    -- Telescope extension
    if M._config.enable_telescope then
        if package.loaded["telescope"] then
            local ext = package.loaded["telescope"]._extensions
            if ext and ext.archaic then
                h.ok("Telescope extension loaded")
            else
                h.warn("Telescope loaded but archaic extension not registered")
                h.warn("Fix: require('telescope').load_extension('archaic')")
            end
        else
            h.warn("Telescope not loaded — install telescope for :ArchaicFind")
        end
    else
        h.warn("telescope integration disabled (enable_telescope = false)")
    end

    -- Disabled state
    if vim.g.archaic_disabled then
        h.warn("Archaic is currently disabled (toggle with :ArchaicToggle)")
    end
end

function M.quick_info()
    local socket_path = vim.g.archaic_socket_path or M._config.socket_path
    local socket_exists = vim.uv.fs_stat(socket_path) ~= nil
    local disabled = vim.g.archaic_disabled and "DISABLED" or "enabled"

    if not socket_exists then
        vim.notify(string.format("archaic: %s | socket missing at %s", disabled, socket_path), vim.log.levels.WARN)
        return
    end

    local ping_start = vim.uv.hrtime()
    M._send_request("PING", {}, function(resp)
        local elapsed = math.floor((vim.uv.hrtime() - ping_start) / 1e6)
        if resp then
            vim.notify(string.format("archaic: %s | daemon OK (%dms) | socket: %s", disabled, elapsed, socket_path), vim.log.levels.INFO)
        else
            vim.notify(string.format("archaic: %s | socket exists but no response | %s", disabled, socket_path), vim.log.levels.WARN)
        end
    end)
end

function M._send_request(method, params, callback)
    local pipe = vim.uv.new_pipe()
    if not pipe then
        callback(nil)
        return
    end

    local socket_path = vim.g.archaic_socket_path or M._config.socket_path

    pipe:connect(socket_path, function(err)
        if err then
            pipe:close()
            vim.schedule(function() callback(nil) end)
            return
        end

        local request = vim.json.encode({
            method = method,
            params = params or {},
        })
        pipe:write(request .. "\n")

        local response_data = {}
        vim.uv.read_start(pipe, function(read_err, chunk)
            if read_err or not chunk then
                pipe:close()
                local response = vim.json.decode(table.concat(response_data))
                vim.schedule(function() callback(response) end)
                return
            end
            table.insert(response_data, chunk)
        end)
    end)
end

function M.get_completions(prefix, cwd, limit, callback)
    limit = limit or 50
    cwd = cwd or vim.fn.getcwd()
    prefix = prefix or ""

    M._send_request("COMPLETE", {
        prefix = prefix,
        cwd = cwd,
        limit = limit,
    }, function(resp)
        if resp and resp.completions then
            vim.schedule(function() callback(resp.completions) end)
        else
            vim.schedule(function() callback({}) end)
        end
    end)
end

return M