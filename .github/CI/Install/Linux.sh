#!/bin/sh
# Bootstrap once; warm consumers restore files without apt update/install.
set -eu
installer=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
destination=${1:?archive directory required}
mkdir -p "$destination"
archive="$destination/llvm.tar.gz"
if [ ! -f "$archive" ]; then
    dpkg-query -W -f='${binary:Package} ${db:Status-Status}\n' > "$destination/baseline"
    curl --proto '=https' --tlsv1.2 -fsSLo "$destination/llvm.sh" \
        https://raw.githubusercontent.com/opencollab/llvm-jenkins.debian.net/6dc0d1ad7de83d0782731687fd555a7859b4da58/llvm.sh
    printf '%s  %s\n' 03878e08f47b66cc95bc4b544b0db3c6d9ce8d60e6cf2492ae357984330a9eae "$destination/llvm.sh" | sha256sum -c -
    sudo bash "$destination/llvm.sh" 23
    sudo apt-get install -y clang-format-23 clang-tidy-23 clang-tools-23 ccache
    sh "$installer/../Verify/LLVM.sh" clang++-23
    # Preserve tool packages and newly introduced dependencies. Never overwrite
    # the runner's libc, loader, or other existing base-runtime packages.
    dpkg-query -W -f='${binary:Package} ${db:Status-Status}\n' > "$destination/packages"
    python3 - "$destination" <<'PY'
from pathlib import Path
import os
import subprocess
import sys
out = Path(sys.argv[1])
paths = set()
baseline = {line.split()[0] for line in (out / 'baseline').read_text().splitlines()
            if line.endswith(' installed')}
for line in (out / 'packages').read_text().splitlines():
    package, status = line.split()
    llvm = '23' in package and package.startswith(('clang', 'llvm', 'libclang', 'libllvm', 'lld', 'libpolly'))
    if status != 'installed' or (package in baseline and not llvm and package.split(':')[0] != 'ccache'):
        continue
    query = subprocess.run(['dpkg-query', '-L', package], capture_output=True, text=True)
    if query.returncode:
        continue
    for name in query.stdout.splitlines():
        path = Path(name)
        if path.is_file() or path.is_symlink():
            paths.add(name)
paths = {str(Path(x).parent.resolve() / Path(x).name).lstrip('/') for x in paths}
(out / 'paths').write_text('\n'.join(sorted(paths)) + '\n')
PY
    sudo tar -czf "$archive.partial" -C / -T "$destination/paths"
    sudo chown "$(id -u):$(id -g)" "$archive.partial"
    mv "$archive.partial" "$archive"
fi
sudo tar -xzf "$archive" -C /
sudo ldconfig
sh "$installer/../Verify/LLVM.sh" clang++-23
clang-format-23 --version
clang-tidy-23 --version
ccache --version
