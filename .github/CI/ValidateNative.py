"""Restore a draft bundle on a different runner and validate the complete compiler workflow."""
import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys

from Environment import digest
from NativeEnvironment import host_key


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--revision', required=True)
    args = parser.parse_args()
    key = host_key()
    root = Path(os.environ['RUNNER_TEMP']) / 'candidate-validation'
    root.mkdir(parents=True, exist_ok=True)
    repo = os.environ['GITHUB_REPOSITORY']
    subprocess.run(['gh', 'release', 'download', args.revision, '--repo', repo,
                    '--pattern', key + '.*', '--dir', str(root)], check=True)
    candidate_path = root / f'{key}.json'
    candidate = json.loads(candidate_path.read_text())
    value = candidate['environments'][key]
    if value['revision'] != args.revision:
        raise ValueError('Candidate revision mismatch')
    cache = Path(os.environ['RUNNER_TEMP']) / 'rux-assets'
    cache.mkdir(exist_ok=True)
    for file in value['files']:
        source = root / file['name']
        if digest(source) != file['sha256']:
            raise ValueError('Candidate checksum mismatch')
        shutil.copyfile(source, cache / file['sha256'])
    lock = root / 'lock.json'
    lock.write_text(json.dumps({'schema': 1, 'environments': candidate['environments']}))
    subprocess.run([sys.executable, str(Path(__file__).with_name('NativeEnvironment.py')), 'restore',
                    '--lock', str(lock)], check=True)
    # One PowerShell process retains the restored PATH and developer SDK environment.
    setup = './.github/Scripts/Install.ps1 -Tool BuildTools; '
    if platform.system() == 'Windows':
        arch = 'arm64' if key.endswith('aarch64') else 'amd64'
        setup += f'./.github/Scripts/Install.ps1 -Tool VsEnvironment -Arch {arch}; '
        setup += "./.github/Scripts/Install.ps1 -Tool Ccache; ./Run.ps1 test -Compiler 'C:/LLVM/bin/clang++.exe' -Jobs 4"
    else:
        tool = 'MacOSLLVM' if platform.system() == 'Darwin' else 'LinuxLLVM'
        setup += f'./.github/Scripts/Install.ps1 -Tool {tool}; '
        # Shell installer writes paths for following workflow steps; apply them in this process too.
        if tool == 'MacOSLLVM':
            compiler = (Path(os.environ['RUNNER_TEMP']) / 'rux-macos-tools/compiler').read_text().strip()
            setup += f"$env:PATH = '{Path(compiler).parent}:' + $env:PATH; ./Run.ps1 test -Compiler '{compiler}' -Jobs 4"
        else:
            setup += './Run.ps1 test -Compiler clang++-23 -Jobs 4'
    subprocess.run(['pwsh', '-NoProfile', '-Command', "$ErrorActionPreference='Stop'; " + setup], check=True)
    candidate['validated'] = True
    candidate_path.write_text(json.dumps(candidate, indent=2) + '\n')
    subprocess.run(['gh', 'release', 'upload', args.revision, str(candidate_path), '--repo', repo, '--clobber'], check=True)


if __name__ == '__main__':
    main()
