-- Example Hyprland (Lua config layer) snippet: keybr lives in a scratchpad.
--   top    -> keybr-dash (native ImGui dashboard)
--   bottom -> Chromium --app window on keybr.com
-- Toggle with SUPER+SHIFT+T, which runs scripts/keybr-toggle (launches whatever
-- is missing, then toggles the special workspace).

for _, class in ipairs({ "keybr-dash", "keybr-practice" }) do
  local sel = "class:^(" .. class .. ")$"
  hl.windowrule({ rule = "workspace special:keybr", selector = sel })
  hl.windowrule({ rule = "noborder",   selector = sel })
  hl.windowrule({ rule = "norounding", selector = sel })
  hl.windowrule({ rule = "noshadow",   selector = sel })
end

-- Master layout: the dashboard (first window) takes the top share.
-- Only special:keybr should use the master layout, since this is global.
hl.config({ master = { new_status = "slave", mfact = 0.43 } })

-- Keybind: adjust to however your config declares binds.
-- hl.bind("SUPER + SHIFT + T", hl.dsp.exec_cmd(os.getenv("HOME") .. "/.local/bin/keybr-toggle"))

-- The toggle itself, as keybr-toggle runs it:
--   hyprctl dispatch 'hl.dsp.workspace.toggle_special("keybr")'
