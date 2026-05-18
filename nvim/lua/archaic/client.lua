local M = {}

local ARCHAIC_SOCKET = "/tmp/archaic-daemon.sock"

local function get_socket_path()
    return vim.g.archaic_socket_path or ARCHAIC_SOCKET
end

local function send_ipc_message(msg_type, payload, callback)
    local pipe = vim.uv.new_pipe()
    if not pipe then
        callback(nil)
        return
    end

    local socket_path = get_socket_path()
    local timer = vim.uv.new_timer()
    local response_buf = ""

    timer:start(200, 0, function()
        pipe:close()
        timer:close()
        vim.schedule(function() callback(nil) end)
    end)

    pipe:connect(socket_path, function(err)
        if err then
            timer:stop()
            timer:close()
            pipe:close()
            vim.schedule(function() callback(nil) end)
            return
        end

        local msg = msg_type .. " " .. (payload or "") .. "\n"
        pipe:write(msg)

        vim.uv.read_start(pipe, function(read_err, chunk)
            if read_err then
                timer:stop()
                timer:close()
                pipe:close()
                vim.schedule(function() callback(nil) end)
                return
            end

            if chunk then
                response_buf = response_buf .. chunk
            else
                timer:stop()
                timer:close()
                pipe:close()
                vim.schedule(function()
                    local completions = {}
                    for line in response_buf:gmatch("[^\n]+") do
                        local type, full_path = line:match("^(.) (.+)$")
                        if type and full_path then
                            table.insert(completions, {
                                type = type,
                                path = full_path,
                                is_dir = (type == "D"),
                            })
                        end
                    end
                    callback(completions)
                end)
            end
        end)
    end)
end

function M.complete(prefix, cwd, limit, callback)
    limit = limit or 50
    cwd = cwd or vim.fn.getcwd()
    prefix = prefix or ""

    local payload = string.format("%s %s %d", prefix, cwd, limit)
    send_ipc_message("complete", payload, callback)
end

function M.fuzzy(query, cwd, limit, callback)
    limit = limit or 50
    cwd = cwd or vim.fn.getcwd()
    query = query or ""

    local payload = string.format("%s %s %d", query, cwd, limit)
    send_ipc_message("fuzzy", payload, callback)
end

function M.ping(callback)
    send_ipc_message("ping", "", function(completions)
        callback(completions ~= nil)
    end)
end

return M