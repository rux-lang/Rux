{
  description = "Rux compiler, checks, and LLVM 22 development environment";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs =
    { self, nixpkgs }:
    let
      inherit (nixpkgs) lib;

      supportedSystems = [
        "x86_64-linux"
        "aarch64-linux"
        "x86_64-darwin"
        "aarch64-darwin"
      ];
      forAllSystems = lib.genAttrs supportedSystems;
      source = lib.cleanSource ./.;

      # Keep the Nix package version in sync with CMake, the release pipeline's
      # source of truth, without duplicating it in this file.
      cmakeProject = builtins.replaceStrings [ "\n" ] [ " " ] (builtins.readFile ./CMakeLists.txt);
      versionMatch =
        builtins.match ".*project\\(Rux VERSION ([0-9]+\\.[0-9]+\\.[0-9]+) LANGUAGES CXX\\).*" cmakeProject;
      version =
        if versionMatch == null then
          throw "flake.nix: could not read the Rux version from CMakeLists.txt"
        else
          builtins.head versionMatch;

      toolchainFor =
        pkgs:
        let
          llvm = pkgs.llvmPackages_22;
        in
        {
          inherit llvm;
          stdenv = llvm.libcxxStdenv;
        };

      packageFor =
        system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
          toolchain = toolchainFor pkgs;
        in
        toolchain.stdenv.mkDerivation {
          pname = "rux";
          inherit version;
          src = source;

          strictDeps = true;
          nativeBuildInputs = with pkgs; [
            cmake
            ninja
          ];

          cmakeBuildType = "Release";
          cmakeFlags = [
            "-DRUX_BUILD_TESTS=ON"
            "-DRUX_WERROR=ON"
          ];

          # CTest covers the hermetic C++ unit suite. The Rux-language suite
          # intentionally stays out of the sandbox because `rux install` must
          # access the package registry before those tests can be compiled.
          doCheck = true;
          checkPhase = ''
            runHook preCheck
            ctest --test-dir build --output-on-failure
            runHook postCheck
          '';

          enableParallelBuilding = true;

          meta = {
            description = "Compiler and package manager for the Rux programming language";
            homepage = "https://rux-lang.dev";
            license = lib.licenses.mit;
            mainProgram = "rux";
            platforms = lib.platforms.linux ++ lib.platforms.darwin;
          };
        };
    in
    {
      packages = forAllSystems (
        system:
        let
          rux = packageFor system;
        in
        {
          inherit rux;
          default = rux;
        }
      );

      apps = forAllSystems (system: {
        default = {
          type = "app";
          program = "${self.packages.${system}.default}/bin/rux";
        };
      });

      checks = forAllSystems (
        system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
          toolchain = toolchainFor pkgs;
        in
        {
          build-and-unit-tests = self.packages.${system}.default;

          formatting = pkgs.runCommand "rux-formatting-check" {
            nativeBuildInputs = [ toolchain.llvm.clang-tools ];
          } ''
            find ${source}/Compiler ${source}/Tests/Unit \
              \( -name '*.cpp' -o -name '*.h' \) \
              -not -path '*/ThirdParty/*' \
              -exec clang-format --dry-run --Werror {} +
            touch "$out"
          '';

          platform-isolation = pkgs.runCommand "rux-platform-isolation-check" {
            nativeBuildInputs = [ pkgs.gnugrep ];
          } ''
            sh ${source}/Tools/PlatformIsolation/Check.sh
            touch "$out"
          '';
        }
      );

      formatter = forAllSystems (
        system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
        in
        pkgs.nixfmt-rfc-style or pkgs.nixfmt
      );

      devShells = forAllSystems (
        system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
          toolchain = toolchainFor pkgs;
        in
        {
          default = pkgs.mkShell.override { stdenv = toolchain.stdenv; } {
            inputsFrom = [ self.packages.${system}.default ];
            packages = [
              pkgs.git
              toolchain.llvm.clang-tools
            ];

            CMAKE_GENERATOR = "Ninja";

            shellHook = ''
              echo "Rux development shell (${system})"
              echo "  compiler:  $(clang++ --version | head -n 1)"
              echo "  configure: cmake -S . -B build -DRUX_BUILD_TESTS=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
              echo "  build:     cmake --build build --config Release"
              echo "  unit test: ctest --test-dir build --output-on-failure -C Release"
              echo "  language:  ./Bin/Release/rux install && ./Bin/Release/rux test --release"
            '';
          };
        }
      );
    };
}
