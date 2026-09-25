# Agent instructions

somewm fork (C, wlroots 0.20, SceneFX 0.5, LuaJIT). Owns: geometry, movement,
decoration (macOS frame), IPC, compositor defaults. The live session runs the
Nix build of this repo, pinned by `~/nixdots` as a flake input.

## Ownership
- This repo is the single owner of geometry, movement, decoration, IPC and
  defaults. Config (`~/.config/somewm`) owns policy and writes `settings.json`;
  Quickshell (`~/.config/quickshell`) owns UI/intent and only reads
  `settings.json`. Do not move these responsibilities here or into the config.
- `lua/awful`, `lua/gears`, `lua/wibox`, `lua/naughty` stay identical to
  AwesomeWM: fix bugs in C (see CONTRIBUTING.md).

## Git
- Work on `docs/agents-md` (or a branch named for the task); merge into
  `release/1.4` only after tests pass. No rebase, no force-push, no rewrite of
  `release/1.4`. If `release/1.4` moved on origin, pull with a merge first.
- One logical change per commit, English messages.
- Push only when the task asks; report the pushed hash.

## The live session is off-limits
- Unless a task explicitly grants live-session access (and then only what it
  lists), never touch the live session: no `pkill -f`, `killall`,
  `pgrep | xargs kill`, no `kill` on PIDs you did not start, no
  `somewm-client reload`, no `grim`/screenshot of the live session, no
  `nixos-rebuild`, `nix flake update` or `flake-update-sw`.
- Talk to test instances only via `somewm-client test <cmd> --name <name>` or
  `./test-nested.sh`; stop them with `somewm-client test stop` or `kill` on a
  PID you started. Leave no instance running.

## Build and test
- Work inside `nix develop`. `make build-test` then `./build-test/somewm
  --version` must show `SceneFX: yes` (matches the Nix package).
- One rebuild per batch of C changes: edit, `make build-test`, run the tests
  for that batch, then move to the next batch.
- Nested harness: `./.run-test [args]` execs `build-test/somewm`; it does NOT
  accept `--renderer` — select the renderer via env, e.g. `WLR_RENDERER=gles2
  ./.run-test`. `./test-nested.sh` (start|user|stop|restart|run) auto-picks
  `gles2` for SceneFX builds and pixman otherwise. Orchestrator:
  `SOMEWM_BINARY=./build-test/somewm WLR_RENDERER=gles2 somewm-client test
  start --host wayland` (headless forces pixman).
- Test like the real session: GLES2 and hot-reloads. Validate visuals with
  `grim` only; `screenshot`/`root.content` composite in software and are wrong
  under GLES2.
- Take a baseline of `make test-restart` and `make check-qa` before changing
  C; compare after. `make test-unit test-check test-signal test-orchestrator
  test-integration` are the other suites.
- Nested tests must use an isolated XDG_STATE_HOME/CACHE_HOME/DATA_HOME (the
  orchestrator sets these itself) and must not run the real autostart (it
  spawns `quickshell -n`): pre-set `package.loaded["core.autostart"]` in the
  test config before loading the personal rc.lua.
- P0.11 isolated harness: `tests/p0.3-gles2-smoke.sh [--keep] [--config
  <ref|ruta>] [--shell <ref|ruta>]` runs every nested instance against a
  throwaway copy of the personal config + quickshell under /tmp/p0.3/xdg/ with
  XDG_CONFIG_HOME/XDG_STATE_HOME/XDG_CACHE_HOME pinned there. A ref becomes an
  ephemeral `git worktree` (removed with `git worktree remove --force` in the
  trap), an absolute path becomes a symlink (origin kept), and the default is a
  detached worktree at each repo's main-branch HEAD. `settings.json` is not
  versioned in the config repo, so the live file is copied read-only into the
  isolated config dir. The suite asserts isolation (live wallpaper-state mtime
  and both live repos' git status unchanged, `require()` anchored under
  /tmp/p0.3/xdg/config). Never checkout/switch in ~/.config/somewm or
  ~/.config/quickshell (live session; Quickshell reloads on change); develop in
  a worktree and never edit ~/.config/somewm/settings.json.
- Test artifacts go to /tmp, never into the repo.

## Adoption / rollback (requires explicit task permission)
- Adopt: `nix flake update somewm` in `~/nixdots` -> `flake-update-sw` (defined
  in `~/nixdots/home/humbe/apps.nix` as `sudo nixos-rebuild switch --flake
  .#<host> --impure`) -> commit `flake.lock` -> relogin.
- Rollback: restore the old `flake.lock` in `~/nixdots`, `flake-update-sw`,
  commit, relogin.

## Reports and style
- Every claim in a report carries VERIFIED / INFERRED / UNVERIFIED with the
  command that proves it or a `file:line` citation. Never invent APIs or
  commands; a command that does not exist is `TODO: verificar`.
- KISS; comments only for a non-obvious why, in English, one line. Do not
  create files unless the task asks.
