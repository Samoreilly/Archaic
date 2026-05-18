local archaic = require("archaic")

vim.api.nvim_create_user_command("ArchaicStatus", function()
    archaic.check_health()
end, { desc = "Check archaic daemon status" })

vim.api.nvim_create_user_command("ArchaicReindex", function(cmd_opts)
    archaic.reindex(cmd_opts.args)
end, { nargs = "?", desc = "Trigger daemon rescan" })

vim.api.nvim_create_user_command("ArchaicToggle", function()
    archaic.toggle()
end, { desc = "Toggle archaic completions" })

vim.api.nvim_create_user_command("ArchaicCheckHealth", function()
    archaic.check_health()
end, { desc = "Check archaic daemon health" })

local has_cmp, cmp = pcall(require, "cmp")
if has_cmp then
    local archaic_source = require("archaic.cmp_source")
    cmp.register_source("archaic", archaic_source)
end

local has_telescope, telescope = pcall(require, "telescope")
if has_telescope then
    telescope.load_extension("archaic")
end