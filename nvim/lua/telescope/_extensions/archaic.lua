local telescope = require("telescope")
local archaic_telescope = require("archaic.telescope")

return telescope.register_extension({
    exports = {
        find_files = archaic_telescope.find_files,
    },
    setup = archaic_telescope.setup,
})
