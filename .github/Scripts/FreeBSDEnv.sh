# Export the CI build environment inside a FreeBSD guest.
#
# Sourced, not run, at the top of every guest step: the guest has no
# GITHUB_ENV, and the workspace is mirrored at the host's path, so $PWD is the
# repository root. The host toolchain scripts export the same settings for the
# packed prefix; the guest has its packages instead.
#
#   . .github/Scripts/FreeBSDEnv.sh

rux_manifest=.github/Toolchains.env
if grep -qvE '^[[:space:]]*(#.*)?$|^[A-Z0-9_]+=[A-Za-z0-9._:/+-]*$' "$rux_manifest"; then
    printf "error: '%s' contains a line that is not a comment or KEY=VALUE\\n" "$rux_manifest" >&2
    exit 1
fi
. "$rux_manifest"

# The workspace arrives by rsync with the host runner's ownership preserved,
# while the guest builds as root; without this git refuses the checkout as
# dubiously owned, and the CI helper checks that list tracked files fail.
export GIT_CONFIG_COUNT=1
export GIT_CONFIG_KEY_0=safe.directory
export GIT_CONFIG_VALUE_0='*'

# The llvm23 package names the compiler clang++23. Run.sh probes that name,
# but CXX makes the choice explicit where several Clangs are installed.
export CXX=clang++23
export CMAKE_CXX_COMPILER_LAUNCHER=ccache
export CCACHE_DIR=$PWD/BuildCache/ccache
export CCACHE_MAXSIZE
export CCACHE_COMPILERCHECK=content
# Other runs build the same tree at another path; without this they would
# never share cache entries.
export CCACHE_NOHASHDIR=1
