# Agent instructions

somewm fork (C, wlroots 0.20, SceneFX 0.5, LuaJIT). The live session runs the Nix
build of this repo; ~/nixdots pins it as a flake input.

## Git
- Default: work and commit directly on release/1.4. Use a separate branch only when
  I explicitly ask for one in the task.
- Commit in small logical steps (English messages, one logical change per commit).
- Push release/1.4 to origin when the task's tests pass. Report the pushed hash.
- Never rebase, force-push or rewrite history on release/1.4.
- If release/1.4 moved on origin, pull with a merge (no rebase) before pushing. If there are
  conflicts, stop and report.

## The live session is off-limits
- These rules apply unless the task explicitly grants live-session access; then follow
  exactly the permissions written in that task, and nothing more.
- Your shell inherits the live SOMEWM_SOCKET and WAYLAND_DISPLAY. Talk to test
  instances only through `somewm-client test <cmd> --name <instance>`; never run
  bare `somewm-client eval|reload|screenshot`, and run `grim` only with the test
  instance's XDG_RUNTIME_DIR/WAYLAND_DISPLAY.
- No `pkill -f`, `killall` or `pgrep | xargs kill` (this includes
  `test-nested.sh stop|restart`). Stop instances with `somewm-client test stop`
  or `kill` on a PID you started. Leave no instance running.
- Never run nixos-rebuild, `nix flake update` or flake-update-sw, and never edit ~/nixdots.
- Edit ~/.config/somewm or ~/.config/quickshell only when the task asks for it: back up each
  file as <name>.bak-<task>, show the diff, and save only after my OK.

## Build and test
- Work inside `nix develop`. build-test must match the Nix package:
  `build-test/somewm --version` shows `SceneFX: yes`.
- Test like the real session: GLES2 and hot-reloads. Use the orchestrator with
  `--host wayland`, `WLR_RENDERER=gles2` and `SOMEWM_BINARY=<build dir>/somewm`
  (without it, it runs the deployed somewm from PATH). `--host headless` forces
  pixman.
- Validate visuals with `grim` captures only. `screenshot`, `root.content` and
  `screen.content` composite in software and are wrong under GLES2.
- Take a baseline of `make test-restart` and `make check-qa` before changing C,
  and compare after.
- Nested tests must ALWAYS use an isolated XDG_STATE_HOME (and XDG_CACHE_HOME,
  XDG_DATA_HOME) inside the instance runtime: sharing the live ones lets a nested
  persist overwrite the live ~/.local/state/somewm/workspaces.json or the
  autostart.started marker. Nested configs must also never run the real
  autostart (the live config spawns `quickshell -n`); run the instance with a
  test config that pre-sets package.loaded["core.autostart"] before loading the
  personal rc.lua (a LUA_PATH shadow does NOT work: the C loader puts the
  config dir first). The orchestrator sets the XDG state dirs itself.
- Test artifacts go to /tmp, never into the repo.

## Code
- Smallest diff that works; no new abstractions or files unless asked.
- Comments only for a non-obvious why, in English, one line.
- `lua/awful`, `lua/gears`, `lua/wibox`, `lua/naughty` stay identical to
  AwesomeWM: fix bugs in C (see CONTRIBUTING.md).
- Commit messages in English, one logical change per commit.
