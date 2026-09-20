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

describe("fx.autocolor sampling glue", function()
    local calls

    -- A fake client that implements dominant_color() and records the opts.
    local function fake_client(hex, share, method_ok)
        calls = {}
        return {
            valid = true,
            dominant_color = function(_, opts)
                table.insert(calls, opts)
                if method_ok == false then return nil, nil end
                return hex, share
            end,
        }
    end

    after_each(function()
        autocolor._reset_warning_for_tests()
    end)

    it("maps region=top to a 12-row strip", function()
        local c = fake_client("#00ff00", 0.5)
        local hex, share = autocolor._sample_for_tests(c, { region = "top", min_share = 0.06 })
        assert.is.same("#00ff00", hex)
        assert.is.equal(0.5, share)
        assert.is.same(12, calls[1].rows)
        assert.is_nil(calls[1].thumb)
    end)

    it("maps region=full to a whole-content sample with thumb-derived steps", function()
        local c = fake_client("#ff0000", 0.4)
        local hex = autocolor._sample_for_tests(c, { region = "full", min_share = 0.06, thumb = 48 })
        assert.is.same("#ff0000", hex)
        assert.is.same(0, calls[1].rows)
        assert.is.same(48, calls[1].thumb)
    end)

    it("gates on min_share", function()
        -- share below min_share: no color (fallback kept).
        local c = fake_client("#ff0000", 0.03)
        local hex, share = autocolor._sample_for_tests(c, { region = "top", min_share = 0.06 })
        assert.is_nil(hex)
        assert.is_nil(share)
        -- share at/above min_share: color passes.
        local c2 = fake_client("#ff0000", 0.06)
        local hex2 = autocolor._sample_for_tests(c2, { region = "top", min_share = 0.06 })
        assert.is.same("#ff0000", hex2)
    end)

    it("keeps the fallback when the method is missing (older compositor)", function()
        local c = { valid = true }  -- no dominant_color method
        local hex, share = autocolor._sample_for_tests(c, { region = "top", min_share = 0.06 })
        assert.is_nil(hex)
        assert.is_nil(share)
        -- A method returning nothing (no color) is also a fallback keep.
        local c2 = fake_client(nil, nil, true)
        local hex2 = autocolor._sample_for_tests(c2, { region = "top", min_share = 0.06 })
        assert.is_nil(hex2)
    end)
end)
