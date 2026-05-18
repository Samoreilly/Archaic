local M = {}

M._config = {
    socket_path = "/tmp/archaic-daemon.sock",
    timeout_ms = 100,
    enable_cmp = true,
    enable_telescope = true,
}

M._connected = false
M._pipe = nil

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
    M._send_request("PING", {}, function(resp)
        if resp then
            vim.notify("archaic: daemon is running (uptime: " .. (resp.uptime_ms or "?") .. "ms)", vim.log.levels.INFO)
        else
            vim.notify("archaic: daemon is NOT running", vim.log.levels.ERROR)
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