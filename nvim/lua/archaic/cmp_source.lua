local client = require("archaic.client")

local source = {}

function source:is_available()
    return not vim.g.archaic_disabled
end

function source:get_debug_name()
    return "archaic"
end

function source:get_trigger_characters()
    return { "/", ".", "-", "_" }
end

function source:complete(params, callback)
    if vim.g.archaic_disabled then
        callback({ items = {}, isIncomplete = false })
        return
    end

    local line = params.context.cursor_before_line
    local prefix = line:match("[%w%d._/-]*$") or ""
    if prefix == "" or #prefix < 1 then
        callback({ items = {}, isIncomplete = false })
        return
    end

    local cwd = vim.fn.getcwd()
    local offset = params.offset
    local cursor_col = params.context.cursor.col

    client.complete(prefix, cwd, 50, function(completions)
        if not completions or #completions == 0 then
            callback({ items = {}, isIncomplete = false })
            return
        end

        local items = {}
        for _, comp in ipairs(completions) do
            local basename = comp.path:match("([^/]+)$") or comp.path
            local display = basename
            if comp.is_dir then
                display = basename .. "/"
            end

            local kind = comp.is_dir and 19 or 17 -- 19=Folder, 17=File

            table.insert(items, {
                label = display,
                insertText = display,
                detail = comp.path,
                kind = kind,
                sortText = string.format("%03d", #items),
                filterText = display,
            })
        end

        callback({ items = items, isIncomplete = false })
    end)
end

function source:resolve(completion_item, callback)
    callback(completion_item)
end

return source