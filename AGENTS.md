# Agent instructions

somewm fork (C, wlroots 0.20, SceneFX 0.5, LuaJIT). The live session runs the Nix
build of this repo; ~/nixdots pins it as a flake input.

## Git
- One branch per task, from release/1.4. Never commit to release/1.4 directly.
- Never push, merge, rebase or force-push. Stop when tests pass and report; the
  user merges and deploys.

## The live session is off-limits
- Your shell inherits the live SOMEWM_SOCKET and WAYLAND_DISPLAY. Talk to test
  instances only through `somewm-client test <cmd> --name <instance>`; never run
  bare `somewm-client eval|reload|screenshot`, and run `grim` only with the test
  instance's XDG_RUNTIME_DIR/WAYLAND_DISPLAY.
- No `pkill -f`, `killall` or `pgrep | xargs kill` (this includes
  `test-nested.sh stop|restart`). Stop instances with `somewm-client test stop`
  or `kill` on a PID you started. Leave no instance running.
- Never run nixos-rebuild, `nix flake update` or flake-update-sw, and never edit
  ~/.config/somewm, ~/.config/quickshell or ~/nixdots from this repo.

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
- Test artifacts go to /tmp, never into the repo.

## Code
- Smallest diff that works; no new abstractions or files unless asked.
- Comments only for a non-obvious why, in English, one line.
- `lua/awful`, `lua/gears`, `lua/wibox`, `lua/naughty` stay identical to
  AwesomeWM: fix bugs in C (see CONTRIBUTING.md).
- Commit messages in English, one logical change per commit.
