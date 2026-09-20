{
  lib,
  stdenv,
  llvmPackages,
  cairo,
  dbus,
  gdk-pixbuf,
  glib,
  gobject-introspection,
  harfbuzz,
  libdrm,
  libinput,
  librsvg,
  libxcb,
  libxcb-util,
  libxcb-wm,
  libxkbcommon,
  luajit,
  makeWrapper,
  meson,
  ninja,
  pam,
  pango,
  pkg-config,
  scenefx,
  wayland,
  wayland-protocols,
  wayland-scanner,
  wlroots_0_20,
  xwayland,
  gtk3Support ? true,
  gtk3 ? null,
  extraGIPackages ? [ ],
  extraLuaPackages ? (_: [ ]),
  fetchFromGitHub,
  lcms2,
  # Toolchain knobs.
  # `useGcc` switches back to the stock GCC stdenv (for comparison and upstream
  # parity). `march` (e.g. "znver3") tunes for the host CPU; the default null
  # keeps the portable baseline. Measured on this machine's Zen 3, -march and
  # PGO did not beat the Clang + ThinLTO baseline beyond bench noise, so they
  # are left as opt-in knobs rather than defaults.
  useGcc ? false,
  march ? null,
}:

assert gtk3Support -> gtk3 != null;

let
  # Clang + lld + ThinLTO by default; stock GCC when useGcc is set.
  toolchain = if useGcc then stdenv else llvmPackages.stdenv;
  cflags = lib.optional (march != null) "-march=${march}";
  luaEnv = luajit.withPackages (
    ps:
    with ps;
    [
      lgi
      ldbus
    ]
    ++ (extraLuaPackages ps)
  );
  # SceneFX 0.5 is required by the wlroots 0.20 integration. Some nixpkgs
  # pins only ship 0.4.x, so pin the src to the 0.5 tag (and wlroots 0.20)
  # when needed.
  scenefx_0_5 = if lib.versionAtLeast scenefx.version "0.5"
    then scenefx
    else (scenefx.override { wlroots_0_19 = wlroots_0_20; }).overrideAttrs (old: {
      version = "0.5";
      src = fetchFromGitHub {
        owner = "wlrfx";
        repo = "scenefx";
        rev = "refs/tags/0.5";
        hash = "sha256-vUjLG6eubEhJJVa9LPygIcVmNoHwYbSUTJcWEcbxnU4=";
      };
      buildInputs = old.buildInputs ++ [ lcms2 ];
    });
in
toolchain.mkDerivation {
  pname = "somewm";
  version = "dev";

  src = ./.;

  strictDeps = true;

  nativeBuildInputs = [
    gobject-introspection
    makeWrapper
    meson
    ninja
    pkg-config
    wayland-scanner
  ]
  ++ lib.optionals (!useGcc) [
    # lld for the default Clang toolchain (-Dc_link_args=-fuse-ld=lld).
    llvmPackages.lld
  ];

  buildInputs = [
    cairo
    dbus
    gdk-pixbuf
    glib
    harfbuzz
    libdrm
    libinput
    librsvg
    libxkbcommon
    luajit
    luaEnv
    pam
    pango
    wayland
    wayland-protocols
    scenefx_0_5
    wlroots_0_20
    libxcb
    libxcb-wm
    libxcb-util
    xwayland
  ]
  ++ lib.optional gtk3Support gtk3;

  # meson flags and toolchain vary (LTO/LLD/GCC/march), and nixpkgs'
  # meson hook reuses the cached source copy with its already-configured
  # build dir across derivations, which breaks --reconfigure when flags
  # change. Give each toolchain its own build dir so a changed flag never
  # meets a stale meson-private state.
  mesonBuildDir = "build-${if useGcc then "gcc" else "clang"}";

  mesonFlags =
    [ "-Dsystemd=disabled" ]
    ++ lib.optionals (!useGcc) [
      # ThinLTO + lld. The clang stdenv's bintools is GNU binutils, so the
      # linker must be named explicitly; ld.lld is on PATH via llvmPackages.lld.
      "-Db_lto=true"
      "-Db_lto_mode=thin"
      "-Dc_link_args=-fuse-ld=lld"
    ];

  # march reaches the compiler through NIX_CFLAGS_COMPILE (the cc-wrapper
  # applies it to every TU in this derivation).
  env.NIX_CFLAGS_COMPILE = lib.concatStringsSep " " cflags;

  # The clang stdenv does not set LD_LIBRARY_PATH the way the GCC stdenv does,
  # so meson's `cc.run()` (the LGI check, and any try-run) cannot find shared
  # libs at configure time. luajit is the one checked at configure; make it
  # findable so the LGI probe passes regardless of toolchain.
  preConfigure = ''
    export LD_LIBRARY_PATH="${luajit}/lib''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  '';

  postFixup =
    let
      giPackages = [
        gdk-pixbuf
        glib.out
        gobject-introspection
        harfbuzz.out
        librsvg.out
        pango.out
      ]
      ++ lib.optional gtk3Support gtk3.out
      ++ extraGIPackages;
      giTypelibPath = lib.strings.concatMapStringsSep ":" (p: "${p}/lib/girepository-1.0") giPackages;
    in
    ''
      wrapProgram $out/bin/somewm \
        --prefix GI_TYPELIB_PATH : "${giTypelibPath}" \
        --prefix LUA_PATH : "${luaEnv}/share/lua/${luaEnv.luaversion}/?.lua;${luaEnv}/share/lua/${luaEnv.luaversion}/?/init.lua" \
        --prefix LUA_CPATH : "${luaEnv}/lib/lua/${luaEnv.luaversion}/?.so" \
        --set GDK_PIXBUF_MODULE_FILE "${librsvg.out}/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache"
    '';

  passthru.providedSessions = [ "somewm" ];

  meta = with lib; {
    description = "AwesomeWM ported to Wayland - 100% Lua API compatible";
    homepage = "https://github.com/trip-zip/somewm";
    license = licenses.gpl3;
    platforms = platforms.linux;
    mainProgram = "somewm";
  };
}
