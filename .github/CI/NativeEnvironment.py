"""Package or restore complete native tool installations, independently of build caches."""
import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tarfile

from Environment import LOCK, ROOT, digest, emit, entry, materialize


def host_key():
    system = {'Darwin': 'macos', 'Windows': 'windows', 'Linux': 'linux'}[platform.system()]
    arch = 'aarch64' if platform.machine().lower() in ('arm64', 'aarch64') else 'x86_64'
    # RUNNER_ARCH describes the OS, even if a tool is translated on Windows ARM.
    if os.environ.get('RUNNER_ARCH') == 'ARM64':
        arch = 'aarch64'
    return f'{system}-build-{arch}'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('command', choices=['restore', 'package'])
    parser.add_argument('--revision')
    parser.add_argument('--lock', type=Path, default=LOCK)
    args = parser.parse_args()
    key = host_key()
    temporary = Path(os.environ['RUNNER_TEMP'])
    if args.command == 'restore':
        value = entry(key, args.lock, required=False)
        if value is None:
            emit('prepared', 'false')
            return
        if value['kind'] != 'bundle':
            raise ValueError('Expected a native bundle, not ' + value['kind'])
        assets = materialize(value, temporary / 'rux-assets', temporary / 'native-assets')
        directory = temporary / 'native-payload'
        with tarfile.open(assets / value['archive']) as archive:
            archive.extractall(directory, filter='data')
        for name in ('rux-build-tools', 'rux-macos-tools', 'rux-linux-tools'):
            if (directory / name).exists():
                shutil.copytree(directory / name, temporary / name, dirs_exist_ok=True, symlinks=True)
        if (directory / 'LLVM').exists():
            if platform.system() != 'Windows':
                raise ValueError('Windows payload on non-Windows host')
            shutil.copytree(directory / 'LLVM', Path('C:/LLVM'), dirs_exist_ok=True)
        emit('prepared', 'true')
        return
    output = ROOT / 'BuildCache/environment-output'
    output.mkdir(parents=True, exist_ok=True)
    path = output / f'{key}.tar.gz'
    with tarfile.open(path, 'w:gz', compresslevel=1) as archive:
        for name in ('rux-build-tools', 'rux-macos-tools', 'rux-linux-tools'):
            if (temporary / name).exists():
                archive.add(temporary / name, arcname=name)
        if platform.system() == 'Windows':
            archive.add('C:/LLVM', arcname='LLVM')
    value = {'key': key, 'kind': 'bundle', 'revision': args.revision, 'archive': path.name,
             'files': [{'name': path.name, 'sha256': digest(path),
                        'url': f'https://github.com/{os.environ["GITHUB_REPOSITORY"]}/releases/download/{args.revision}/{path.name}'}]}
    # The candidate remains unvalidated until a different hosted job restores it.
    (output / f'{key}.json').write_text(json.dumps({'validated': False, 'environments': {key: value}}, indent=2) + '\n')


if __name__ == '__main__':
    main()
