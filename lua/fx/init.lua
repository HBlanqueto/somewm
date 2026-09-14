---------------------------------------------------------------------------
--- fx extensions namespace.
--
-- Modules under fx.* are compositor-specific additions that do not exist
-- in upstream AwesomeWM.  They must never modify the sacred awful/gears/wibox
-- libraries.
--
-- Submodules are loaded lazily so that `require("fx")` does not install
-- signal handlers or other side effects until a submodule is actually used.
--
-- @module fx
---------------------------------------------------------------------------

local submodules = {
    autocolor = "fx.autocolor",
}

return setmetatable({}, {
    __index = function(self, key)
        local mod_path = submodules[key]
        if mod_path then
            local mod = require(mod_path)
            rawset(self, key, mod)
            return mod
        end
    end,
})
