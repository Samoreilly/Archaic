local pickers = require("telescope.pickers")
local finders = require("telescope.finders")
local conf = require("telescope.config").values
local actions = require("telescope.actions")
local action_state = require("telescope.actions.state")
local client = require("archaic.client")

local M = {}

M.find_files = function(opts)
    opts = opts or {}
    local cwd = opts.cwd or vim.fn.getcwd()
    local prompt = opts.prompt or ""

    client.fuzzy(prompt, cwd, opts.limit or 100, function(completions)
        if not completions or #completions == 0 then
            vim.notify("archaic: no completions found", vim.log.levels.INFO)
            return
        end

        local entries = {}
        for _, comp in ipairs(completions) do
            local basename = comp.path:match("([^/]+)$") or comp.path
            local display = basename
            if comp.is_dir then
                display = basename .. "/"
            end
            local icon = comp.is_dir and "dir" or "file"

            table.insert(entries, {
                value = comp.path,
                display = display,
                ordinal = display .. " " .. comp.path,
                path = comp.path,
                is_dir = comp.is_dir,
                icon = icon,
            })
        end

        pickers.new(opts, {
            prompt_title = "Archaic Files",
            finder = finders.new_table({
                results = entries,
                entry_maker = function(entry)
                    return {
                        value = entry.value,
                        display = entry.display,
                        ordinal = entry.ordinal,
                        path = entry.path,
                        is_dir = entry.is_dir,
                    }
                end,
            }),
            sorter = conf.generic_sorter(opts),
            attach_mappings = function(prompt_bufnr, map)
                actions.select_default:replace(function()
                    local selection = action_state.get_selected_entry()
                    actions.close(prompt_bufnr)
                    if selection then
                        vim.cmd("edit " .. vim.fn.fnameescape(selection.path))
                    end
                end)
                actions.select_vertical:replace(function()
                    local selection = action_state.get_selected_entry()
                    actions.close(prompt_bufnr)
                    if selection then
                        vim.cmd("vsplit " .. vim.fn.fnameescape(selection.path))
                    end
                end)
                actions.select_horizontal:replace(function()
                    local selection = action_state.get_selected_entry()
                    actions.close(prompt_bufnr)
                    if selection then
                        vim.cmd("split " .. vim.fn.fnameescape(selection.path))
                    end
                end)
                return true
            end,
        }):find()
    end)
end

return M