# settings.json — schema v1 (P2.1)

The single source of truth for the personal SomeWM session. Owned by the config
repo (`~/.config/somewm`), which is the ONLY writer; Quickshell only reads it;
the fork reads two keys through `core.settings` from its Lua modules. The file
is **not** versioned in git (ignored by `~/.config/somewm/.gitignore`).

## Rules

- **Lua is the only writer.** `core/settings.lua` owns the contract: `settings.set()`
  is the single write path, atomic tmp+rename, and emits `settings::changed::<section>`
  (`core/settings.lua:52-70`, `:137-146`). Never edit the file by hand as a rule.
- **Quickshell only reads.** It watches the file via `FileView`/`JsonAdapter`
  (e.g. `core/Theme.qml:212-235`) and never writes it.
- **Unknown keys are preserved.** A key not present in a section's registered
  defaults survives `settings.set()` (defaults merge *below* user values) and a
  reload (`merge(defaults, data)`, `core/settings.lua:26-33`, `:158-175`). This
  is how Quickshell- and fork-only keys (e.g. `focus_space.leave_animated`,
  `focus_space.placement`) coexist with the config's defaults.

## version

| Clave | Tipo | Default | Valores válidos | Quién lo lee | ¿Quickshell? |
|---|---|---|---|---|---|
| `version` | integer | `1` | `>= 1` | nobody (schema marker; default in `core/settings.lua:34`) | No |

## theme

Defaults: `core/theme.lua:59-66`.

| Clave | Tipo | Default | Valores válidos | Quién lo lee | ¿Quickshell? |
|---|---|---|---|---|---|
| `mode` | string | `"dark"` | `"dark"`, `"light"` (theme dir `themes/<mode>`) | `rc.lua:79-95`; `core/theme.lua:94` | Sí — `Theme.qml:220` |
| `accent` | string | `"blue"` | accent ids `themes/accents.lua:8-15` (graphite, red, orange, yellow, green, blue, purple, pink) | `core/theme.lua` (menu + accents) | Sí — `Theme.qml:221` (hand-duplicated palette) |
| `accent_border` | boolean | `false` | `true`, `false` | `core/theme.lua` | No |
| `wallpaper_tint` | boolean | `true` | `true`, `false` | `core/theme.lua:398-403` | No |
| `titlebar_tint` | boolean | `false` | `true`, `false` | `core/theme.lua:387-392` | No |
| `sync_apps` | boolean | `true` | `true`, `false` | `core/theme.lua` | No |
| `bg_tint_strength` | number | *(none)* | `0..1` | `core/theme.lua:202-203` | No |
| `accent_tint_strength` | number | *(none)* | `0..1` | `core/theme.lua:194-195` | No |

`bg_tint_strength` / `accent_tint_strength` are not in the registered defaults;
they are preserved unknown keys written via `settings.set`.

## wallpaper

Defaults: `ui/wallpaper.lua:39` (`{ preset="graphite", color=nil, image=nil, fit="cover" }`).
Preset ids: `ui/wallpaper.lua:23-35`.

| Clave | Tipo | Default | Valores válidos | Quién lo lee | ¿Quickshell? |
|---|---|---|---|---|---|
| `preset` | string | `"graphite"` | preset ids (`ui/wallpaper.lua:23-35`: graphite, black, purple, cyan, salmon, blue, peach, beige, ochre, fuchsia, red, light_pink, light_gray, pale_pink, medium_gray, silver, dark_gray, teal, turquoise, yellow) | `ui/wallpaper.lua:41-53` (`resolve`) | Sí — `PanelStyle.qml:255`, `NotchContrast.qml:49` |
| `color` | string / null | `nil` | `"#rrggbb"` | `ui/wallpaper.lua:44` | Sí — `PanelStyle.qml:256`, `NotchContrast.qml:50` |
| `image` | string / null | `nil` | readable path: `file://`, `~`, or relative to the config dir (`ui/wallpaper.lua:60-84`) | `ui/wallpaper.lua:60-84` (`resolve_image`) | No (bar samples the state file published by `libs/panel_state`) |
| `fit` | string | `"cover"` | `"cover"`, `"fit"`, `"center"`, `"tile"` (`ui/wallpaper.lua:21`, `:112`) | `ui/wallpaper.lua:95-112` | No (only via the wallpaper state file) |

## slide

New section (P2.1/P2.2). Applied to the C `slide.*` global by the config loader
(`core/settings.lua`, on boot and on `settings::changed::slide`). Defaults come
from `slide.c`, quoted in the "Default" column.

| Clave | Tipo | Default | Valores válidos | Quién lo lee | ¿Quickshell? |
|---|---|---|---|---|---|
| `duration` | number | `0.300` (`slide.c:63`) | `> 0` (`slide.c:76-79`) | `core/settings.lua` → `slide.set_duration` | No |
| `easing` | string | `"ease-out-cubic"` (`slide.c:64`, enum in `:1492-1508`) | `"linear"`, `"ease-out-cubic"`, `"ease-in-out-cubic"`, `"spring"` (aliases `"spring-critical"`, `"macos"`; `slide.c:1473-1487`) | `core/settings.lua` → `slide.set_easing` | No |
| `gap` | number | `80` (`slide.c:65`) | `>= 0` (`slide.c:87-92`) | `core/settings.lua` → `slide.set_gap` | No |
| `gap_color` | string | `"#000000"` (`slide.c:66`, read back in `:1540-1548`) | `"#rrggbb"` (`slide.c:1525-1537`) | `core/settings.lua` → `slide.set_gap_color` | No |
| `sliding_layers` | array of strings | `[]` (none; `slide.c:110-112`, `:114-125`) | layer-shell namespace strings, max 8 (`SLIDE_MAX_LAYER_NS`, `slide.c:110`) | `core/settings.lua` → `slide.set_sliding_layers` | No (it *is* the `"neptune-bar"` namespace, `ui/panel/Panel.qml`) |

Current effective values of the live session (kept until P2.2 moves them from
`rc.lua` into settings.json): `duration: 0.5`, `easing: "ease-in-out-cubic"`,
`gap: 80`, `sliding_layers: ["neptune-bar"]`, `gap_color` default black.

## focus_space

Defaults: `libs/focus_space.lua:826`.

| Clave | Tipo | Default | Valores válidos | Quién lo lee | ¿Quickshell? |
|---|---|---|---|---|---|
| `enabled` | boolean | `true` | `true`, `false` | `libs/focus_space.lua:37` | No |
| `reveal_mode` | string | `"push"` | `"push"` only (overlay removed P1.1) | `libs/focus_space.lua:826` | Sí — `FocusMode.qml:340,349` |
| `placement` | string | *(none)* | `"after_origin"`, `"end"` (fork default `"after_origin"`) | fork `lua/somewm/workspaces.lua:504-509` | No |
| `leave_animated` | boolean | *(none)* | `true`, `false` (default `false`) | Quickshell `FocusMode.qml:344,350` | Sí — `FocusMode.qml:344,350` |

`placement` and `leave_animated` are unknown keys for the config (preserved by
the merge rule); only the fork and Quickshell consume them.

## workspaces

Defaults: none registered in the config (preserved unknown section).

| Clave | Tipo | Default | Valores válidos | Quién lo lee | ¿Quickshell? |
|---|---|---|---|---|---|
| `max` | number | `16` (fork fallback) | `>= 1` integer | fork `lua/somewm/workspaces.lua:519-527` (`workspaces_max`) | Indirecto: via the fork's `workspaces` IPC snapshot (`Workspaces.activeMax`, `ui/bar/WorkspaceWidget.qml`); no lee el JSON directamente |

## keys

Defaults: `core/keys.lua:313-385` (`KEYS_DEFAULTS`), registered at `:391`.
Modifier names: `core/keys.lua:25`.

| Clave | Tipo | Default | Valores válidos | Quién lo lee | ¿Quickshell? |
|---|---|---|---|---|---|
| `modkey` | string | `"Mod4"` | key name | `core/keys.lua` (`parse_combo`) | No |
| `mod` | string | `"Mod4"` | named modifier | `core/keys.lua:25` | No |
| `alt` | string | `"Mod1"` | named modifier | `core/keys.lua:25` | No |
| `altgr` | string | `"Mod5"` | named modifier | `core/keys.lua:25` | No |
| `ctrl` | string | `"Control"` | named modifier | `core/keys.lua:25` | No |
| `shift` | string | `"Shift"` | named modifier | `core/keys.lua:25` | No |
| `global` | object | id→combo map (`core/keys.lua:320-385`) | combos `"Mod+X"` … | `core/keys.lua:533,552,573` | No |
| `client` | object | id→combo map (`core/keys.lua:367-377`) | combos `"Mod+X"` … | `core/keys.lua:533,552,573` | No |

## autostart

Defaults: `core/autostart.lua:13-15`.

| Clave | Tipo | Default | Valores válidos | Quién lo lee | ¿Quickshell? |
|---|---|---|---|---|---|
| `apps` | array | `[]` | array of command strings or argv arrays (`core/autostart.lua:72-79`) | `core/autostart.lua:85` | No (it is the spawned app: `["env", ..., "quickshell", "-n"]`) |

## ¿Está versionado settings.json?

No. `~/.config/somewm/.gitignore` ignores `settings.json` and `git ls-files`
does not list it; the live file is runtime state written by `core/settings.lua`.
The P0.11 smoke harness copies it read-only into the isolated config dir.