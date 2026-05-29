class Rux < Formula
  desc "Fast, compiled, strongly typed, multi-paradigm programming language"
  homepage "https://rux-lang.dev"
  # Point this at a release tag that INCLUDES the macOS Mach-O backend.
  # The backend landed after v0.2.2, so a newer tag (e.g. v0.3.0) is required.
  url "https://github.com/rux-lang/Rux/archive/refs/tags/v0.3.0.tar.gz"
  # Fill in after the release exists:
  #   curl -L <url> | shasum -a 256
  sha256 "REPLACE_WITH_TARBALL_SHA256"
  license "MIT"
  head "https://github.com/rux-lang/Rux.git", branch: "main"

  # Rux is written in C++26; Apple's bundled clang is too old, so build with
  # Homebrew's LLVM. CMake 3.31+ is required by the build.
  depends_on "cmake" => :build
  depends_on "llvm" => :build

  def install
    ENV["CXX"] = Formula["llvm"].opt_bin/"clang++"
    system "cmake", "-S", ".", "-B", "build",
           "-DCMAKE_BUILD_TYPE=Release",
           *std_cmake_args
    system "cmake", "--build", "build"
    system "cmake", "--install", "build"
  end

  test do
    # rux exists and reports a version. (Note: `rux --version` reflects the
    # CMake project() version, so keep CMakeLists.txt's VERSION in sync with
    # the release tag if you want this to match `version` exactly.)
    assert_match "Rux", shell_output("#{bin}/rux --version")

    # End-to-end: compile a trivial package, confirming the Mach-O backend
    # produces an executable. `rux build` operates on the current directory,
    # so the package must be the cwd. (Produced binaries are x86-64 and run
    # via Rosetta 2 on Apple Silicon.)
    (testpath/"Rux.toml").write <<~TOML
      [Package]
      Name    = "hello"
      Version = "0.1.0"
      Type    = "bin"

      [Build]
      Output = "Bin"
    TOML
    (testpath/"Src/Main.rux").write <<~RUX
      func Main() -> int {
          return 0;
      }
    RUX
    system bin/"rux", "build"
    assert_predicate testpath/"Bin/Debug/hello", :exist?
  end
end
