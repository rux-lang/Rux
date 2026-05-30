{
  description = "Rux Compiler";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = {
    self,
    nixpkgs,
    flake-utils,
  }:
    flake-utils.lib.eachDefaultSystem (system: let
      pkgs = nixpkgs.legacyPackages.${system};
    in {
      packages.default = pkgs.stdenv.mkDerivation rec {
        pname = "rux";
        version = "v0.2.2";

        src = pkgs.fetchFromGitHub {
          owner = "rux-lang";
          repo = "Rux";
          rev = "${version}";
          hash = "sha256-mOPksefI4u3xZUKNkiC9aXoYUyjNpnSP7/aqTZaBff0=";
        };

        nativeBuildInputs = with pkgs; [cmake ninja];

        configurePhase = ''
          runHook preConfigure
          cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
          runHook postConfigure
        '';

        buildPhase = ''
          runHook preBuild
          cmake --build build/release
          runHook postBuild
        '';

        installPhase = ''
          runHook preInstall
          install -Dm755 build/release/rux $out/bin/rux
          runHook postInstall
        '';

        meta = with pkgs.lib; {
          description = "Rux Compiler (GCC, Ninja, Out‑of‑Source)";
          homepage = "https://rux-lang.dev";
          license = licenses.mit;
          platforms = platforms.linux;
        };
      };
    });
}
