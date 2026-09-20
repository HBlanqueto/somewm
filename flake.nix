{
  description = "SomeWM - AwesomeWM ported to Wayland";

  inputs.nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs, ... }:
    let
      targetArchs = nixpkgs.lib.systems.flakeExposed;
      forAll =
        f:
        nixpkgs.lib.genAttrs targetArchs (
          system:
          f system (import nixpkgs { inherit system; })
        );
    in
    {
      packages = forAll (
        system: pkgs: rec {
          somewm = pkgs.callPackage ./package.nix {
            # Toolchain knobs are plain package.nix arguments; the default
            # (Clang + lld + ThinLTO) is what ~/nixdots consumes. `march`
            # is left null by default: on Zen 3 it measured within bench
            # noise of the plain baseline, so it is opt-in from ~/nixdots
            # rather than a fork default.
          };
          default = somewm;
        }
      );

      apps = forAll (
        system: pkgs: {
          default = {
            type = "app";
            program = "${self.packages.${system}.somewm}/bin/somewm";
          };
          somewm-client = {
            type = "app";
            program = "${self.packages.${system}.somewm}/bin/somewm-client";
          };
        }
      );

      devShells = forAll (
        system: pkgs: {
          default = pkgs.mkShell {
            inputsFrom = [ self.packages.${system}.somewm ];
            # The release toolchain: Clang + lld + the llvm binutils, so
            # `make all` / `make build-test` link with LLD out of the box.
            # GCC stays callable as the default stdenv.cc is untouched.
            packages = [
              pkgs.llvmPackages.clang
              pkgs.llvmPackages.lld
              pkgs.llvmPackages.llvm
            ];
          };
        }
      );
    };
}
