"""Verified, immutable environment assets. Uses only the Python standard library."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
LOCK = ROOT / '.github/CI/Environments.lock.json'


def validate(value):
    if not re.fullmatch(r'(freebsd|linux|macos|windows)-(build|runtime|minimal)-(x86_64|aarch64)', value['key']):
        raise ValueError('Invalid environment key')
    if not re.fullmatch(r'ci-tools-[A-Za-z0-9._-]+', value['revision']):
        raise ValueError('Invalid environment revision')
    if value['kind'] not in ('bundle', 'qcow2'):
        raise ValueError('Unsupported environment kind')
    names = set()
    for item in value['files']:
        name = item['name']
        if not re.fullmatch(r'[A-Za-z0-9_.-]+', name) or name in ('.', '..') or name in names:
            raise ValueError('Unsafe or duplicate environment asset name')
        names.add(name)
        if not re.fullmatch('[a-f0-9]{64}', item['sha256']) or not item['url'].startswith('https://'):
            raise ValueError('Environment assets require SHA-256 and HTTPS')
    references = value['parts'] + [value['ssh_key']] if value['kind'] == 'qcow2' else [value['archive']]
    if not references or len(set(references)) != len(references) or set(references) != names:
        raise ValueError('Environment assets do not match the declared payload')


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def download(asset, directory):
    sha = asset['sha256']
    if not re.fullmatch('[a-f0-9]{64}', sha):
        raise ValueError('Invalid asset SHA-256')
    if not asset['url'].startswith('https://'):
        raise ValueError('Environment assets require HTTPS')
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / sha
    if path.is_file() and digest(path) == sha:
        return path
    partial = path.with_suffix('.partial')
    try:
        for attempt in range(3):
            try:
                with urllib.request.urlopen(asset['url'], timeout=60) as response, partial.open('wb') as out:
                    shutil.copyfileobj(response, out)
                break
            except OSError:
                if attempt == 2:
                    raise
                time.sleep(2 * (attempt + 1))
        if digest(partial) != sha:
            raise ValueError('Environment checksum mismatch: ' + asset['url'])
        partial.replace(path)
        return path
    finally:
        partial.unlink(missing_ok=True)


def entry(key, lock=LOCK, required=True):
    data = json.loads(Path(lock).read_text())
    if data['schema'] != 1:
        raise ValueError('Unsupported environment lock schema')
    value = data['environments'].get(key)
    if value is None and required:
        raise ValueError(f'No published {key} environment. Run Prepare Environments, validate, then promote its manifest.')
    if value is not None and value.get('key') != key:
        raise ValueError('Environment identity mismatch')
    if value is not None:
        validate(value)
    return value


def materialize(value, cache, destination):
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    for item in value['files']:
        name = item['name']
        if not re.fullmatch(r'[A-Za-z0-9_.-]+', name) or name in ('.', '..'):
            raise ValueError('Unsafe environment asset name')
        source = download(item, cache)
        target = destination / name
        if not target.exists() or digest(target) != item['sha256']:
            shutil.copyfile(source, target)
    return destination


def emit(name, value):
    if '\n' in str(value) or '\r' in str(value):
        raise ValueError('Invalid workflow output')
    print(f'{name}={value}')
    if os.environ.get('GITHUB_OUTPUT'):
        with open(os.environ['GITHUB_OUTPUT'], 'a') as out:
            out.write(f'{name}={value}\n')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('command', choices=['resolve', 'fetch', 'promote', 'describe'])
    parser.add_argument('--key')
    parser.add_argument('--lock', type=Path, default=LOCK)
    parser.add_argument('--candidate', type=Path)
    parser.add_argument('--cache', default=os.environ.get('RUNNER_TEMP', '.') + '/rux-assets')
    parser.add_argument('--destination', default=os.environ.get('RUNNER_TEMP', '.') + '/rux-environment')
    args = parser.parse_args()
    if args.command == 'promote':
        candidate = json.loads(args.candidate.read_text())
        if candidate.get('validated') is not True:
            raise ValueError('Candidate has not passed fresh-runner validation')
        data = json.loads(args.lock.read_text())
        # Download and verify every referenced byte before changing the lock.
        for key, value in candidate['environments'].items():
            if key != value['key']:
                raise ValueError('Candidate environment identity mismatch')
            validate(value)
            materialize(value, args.cache, Path(args.destination) / value['key'])
            data['environments'][value['key']] = value
        args.lock.write_text(json.dumps(data, indent=2) + '\n')
        return
    value = entry(args.key, args.lock, required=args.command != 'resolve')
    emit('kind', value['kind'] if value else 'bootstrap')
    if value:
        emit('revision', value['revision'])
        if value['kind'] == 'container':
            emit('image', value['image'])
        if args.command == 'fetch':
            emit('directory', materialize(value, args.cache, args.destination))
        if args.command == 'describe':
            print(json.dumps(value, indent=2))


if __name__ == '__main__':
    main()
