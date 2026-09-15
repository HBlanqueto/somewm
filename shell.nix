{ pkgs ? import <nixpkgs> {} }:

let
  # Resolve a package (falling back to the main output when there is no dev
  # output) and list the directories that may hold its .pc files.
  pkgConfigDirs = p: let q = if p ? dev then p.dev else p; in [
    "${q}/lib/pkgconfig"
    "${q}/share/pkgconfig"
  ];
  maybeDevOutput = p: if p ? dev then p.dev else p;
in

pkgs.mkShell {
  name = "somewm-lua52-dev";

  # Native + lib deps mirroring the repo's package.nix / CI matrix for
  # Lua 5.2 + wlroots 0.20 (the configurations that failed CI).
  buildInputs = [
    pkgs.meson
    pkgs.ninja
    pkgs.pkg-config
    pkgs.wayland-scanner
    pkgs.gobject-introspection
    pkgs.gnumake
    pkgs.git
    pkgs.bc
    pkgs.procps
    pkgs.findutils
    pkgs.cairo
    pkgs.dbus
    pkgs.gdk-pixbuf
    pkgs.glib
    pkgs.harfbuzz
    pkgs.libdrm
    pkgs.libgbm
    pkgs.libinput
    pkgs.librsvg
    pkgs.libxcb
    pkgs.libxcb-util
    pkgs.libxcb-wm
    pkgs.libxkbcommon
    pkgs.pam
    pkgs.wayland
    pkgs.wayland-protocols
    pkgs.wlroots_0_20
    pkgs.xwayland

    pkgs.lua5_2
    pkgs.lua52Packages.lgi
    pkgs.lua52Packages.busted
    pkgs.lua52Packages.luacheck
  ];

  # The classic nix pkg-config wrapper ignores the dev-shell inputs (it only
  # picks up whatever is already on PKG_CONFIG_PATH), so pin the .pc search
  # paths here. Using `dev` outputs keeps this independent of nixpkgs store
  # hashes; missing dirs are simply ignored by pkg-config.
  PKG_CONFIG_PATH = pkgs.lib.concatStringsSep ":" (
    (pkgs.lib.concatLists (map pkgConfigDirs [
      pkgs.cairo pkgs.dbus pkgs.gdk-pixbuf pkgs.glib
      pkgs.gobject-introspection pkgs.harfbuzz pkgs.libdrm pkgs.libgbm
      pkgs.libinput pkgs.librsvg pkgs.libxcb pkgs.libxcb-util
      pkgs.libxcb-wm pkgs.libxkbcommon pkgs.pango pkgs.wayland
      pkgs.wlroots_0_20
    ])) ++ [
      "${pkgs.pam}/lib/pkgconfig"
      "${pkgs.pixman}/lib/pkgconfig"
      "${pkgs.lua5_2}/lib/pkgconfig"
      "${pkgs.wayland-protocols}/share/pkgconfig"
    ]
  );

  # The embedded interpreter seeds package.path/cpath from LUA_PATH/LUA_CPATH.
  # On NixOS the lgi C module lives in the store, so point cpath at it.
  LUA_PATH = "${pkgs.lua52Packages.lgi}/share/lua/5.2/?.lua;${pkgs.lua52Packages.lgi}/share/lua/5.2/?/init.lua;;";
  LUA_CPATH = "${pkgs.lua52Packages.lgi}/lib/lua/5.2/?.so;;";

  # lgi finds GI typelibs via GI_TYPELIB_PATH; on NixOS they live in the store.
  GI_TYPELIB_PATH = pkgs.lib.concatStringsSep ":" [
    "${pkgs.cairo}/lib/girepository-1.0"
    "${pkgs.gdk-pixbuf}/lib/girepository-1.0"
    "${pkgs.glib.out}/lib/girepository-1.0"
    "${pkgs.gobject-introspection}/lib/girepository-1.0"
    "${pkgs.harfbuzz}/lib/girepository-1.0"
    "${pkgs.librsvg.out}/lib/girepository-1.0"
    "${pkgs.pango.out}/lib/girepository-1.0"
  ];

  shellHook = ''
    # The pkg-config wrapper only consults the role-suffixed variable, keeping
    # the setup-hook closure, so fold our pinned search paths into it too.
    export PKG_CONFIG_PATH_FOR_TARGET="$PKG_CONFIG_PATH''${PKG_CONFIG_PATH_FOR_TARGET:+:''$PKG_CONFIG_PATH_FOR_TARGET}"
    # The cc-wrapper injects -D_FORTIFY_SOURCE even at -O0; glibc >= 2.42
    # #warns about that combo and -Werror turns it into a hard error.
    NIX_HARDENING_ENABLE="$([[ -n "''${NIX_HARDENING_ENABLE:-}" ]] && printf '%s\n' ''${NIX_HARDENING_ENABLE} | grep -v fortify | paste -sd' ' -)"
    export NIX_HARDENING_ENABLE
    export LIBSEAT_BACKEND=noop
    export WLR_BACKENDS=headless
    export WLR_RENDERER=pixman
    export WLR_WL_OUTPUTS=1
    echo "somewm dev shell: Lua 5.2 + wlroots 0.20"
    echo "  make check-qa"
    echo "  make build-test LUA_PKG=lua5.2 MESON_OPTS='-Dwlroots_version=0.20 -Dxwayland=enabled'"
    echo "  make test-one TEST=tests/test-innerline-follow.lua"
  '';
}