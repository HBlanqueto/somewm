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

describe("fx.autocolor._pump_should_poll", function()
    it("polls every interval until the commit hook has been seen", function()
        -- Legacy builds never fire `surface::commit`: the pump keeps its
        -- old every-interval polling, regardless of client state.
        assert.is_true(autocolor._pump_should_poll({ dirty = false, budget = 0 }, false))
        assert.is_true(autocolor._pump_should_poll({ dirty = false, budget = 2 }, false))
    end)

    it("skips an idle client once the hook is live", function()
        -- No commit since the last sample and no trailing budget left: the
        -- content has not changed, so there is nothing to re-sample.
        assert.is_false(autocolor._pump_should_poll({ dirty = false, budget = 0 }, true))
    end)

    it("samples a dirty client", function()
        -- A commit latched `st.dirty`: content changed, re-sample.
        assert.is_true(autocolor._pump_should_poll({ dirty = true, budget = 0 }, true))
        assert.is_true(autocolor._pump_should_poll({ dirty = true, budget = 2 }, true))
    end)

    it("samples while the post-commit trailing budget remains", function()
        -- Stale-snapshot convergence (XWayland): budget-driven trailing samples.
        assert.is_true(autocolor._pump_should_poll({ dirty = false, budget = 1 }, true))
        assert.is_true(autocolor._pump_should_poll({ dirty = false, budget = 2 }, true))
    end)
end)
