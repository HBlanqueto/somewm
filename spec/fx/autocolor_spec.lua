-- Unit tests for fx.autocolor foreground derivation + autocolor_white_fg.
-- vim: filetype=lua:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:textwidth=80

local autocolor = require("fx.autocolor")
local beautiful = require("beautiful")

describe("fx.autocolor.foreground", function()
    after_each(function()
        beautiful.autocolor_white_fg = nil
    end)

    it("defaults: white fg on dark bars, dark fg on light bars", function()
        assert.is.same("#f2f2f2", autocolor.foreground("#181818"))
        assert.is.same("#181818", autocolor.foreground("#f2f2f2"))
    end)

    it("honors beautiful.autocolor_white_fg for the white fg only", function()
        beautiful.autocolor_white_fg = "#e8e8e8"
        assert.is.same("#e8e8e8", autocolor.foreground("#181818"))
        -- A light bar keeps the dark fg: the override never replaces it.
        assert.is.same("#181818", autocolor.foreground("#f2f2f2"))
    end)

    it("accepts rgba() and uses only its RGB", function()
        beautiful.autocolor_white_fg = "rgba(232,232,232,0.5)"
        assert.is.same("#e8e8e8", autocolor.foreground("#181818"))
    end)

    it("falls back to #f2f2f2 on an invalid override", function()
        beautiful.autocolor_white_fg = "nonsense"
        assert.is.same("#f2f2f2", autocolor.foreground("#181818"))
    end)
end)