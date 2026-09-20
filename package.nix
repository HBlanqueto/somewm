{
  lib,
  stdenv,
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
}:

assert gtk3Support -> gtk3 != null;

let
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
stdenv.mkDerivation {
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

  mesonFlags = [ "-Dsystemd=disabled" ];

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
