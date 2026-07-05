{
  description = "Rux compiler development environment";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" "x86_64-darwin" "aarch64-darwin" ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
    in {
      devShells = forAllSystems (system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
          stdenv = pkgs.llvmPackages_latest.libcxxStdenv;
        in {
          default = pkgs.mkShell.override { inherit stdenv; } {
            packages = with pkgs; [
              cmake
              ninja
              git
            ];

            shellHook = ''
              echo "--- Rux Development Environment Loaded ---"
              echo "Compiler: $(clang++ --version | head -n 1)"
              echo "To build: cmake -S . -B build -G Ninja && cmake --build build"
            '';
          };
        }
      );
    };
}