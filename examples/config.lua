-- SPDX-FileCopyrightText: 2026 andrew
-- SPDX-License-Identifier: BSD-3-Clause
-- Load this from the normal configuration body. See README for plugin startup.
local fm = hl.plugin.float_memory
if fm and fm.configure then
    fm.configure({
        default_size = { width = 1200, height = 720 },
        center = true,
        remember_position = true,
        rule = function(w)
            -- Return false to exclude a window.
            -- Return nil to use the generic app ID + transient/main policy.
            -- Or return overrides, for example:
            -- return {
            --     key = w.initial_class,
            --     role = "preview",
            --     default_size = { width = 960, height = 540 },
            --     center = false,
            -- }
            return nil
        end,
    })
end

hl.unbind("SUPER + T")
hl.bind("SUPER + T", function()
    local w = hl.get_active_window()
    if w then
        hl.dispatch(hl.dsp.window.float({ window = w, action = "toggle" }))
    end
end)
