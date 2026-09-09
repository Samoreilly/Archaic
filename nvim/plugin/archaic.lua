local has_cmp, cmp = pcall(require, "cmp")
if has_cmp then
    local archaic_source = require("archaic.cmp_source")
    cmp.register_source("archaic", archaic_source)
end

local has_telescope, telescope = pcall(require, "telescope")
if has_telescope then
    telescope.load_extension("archaic")
end
