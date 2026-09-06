"""Consume a published FreeBSD image; never install packages or compile tools."""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import time

from Environment import ROOT, entry, materialize
from FreeBSDVM import FreeBSDVM


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--arch', choices=['x86_64', 'aarch64'], required=True)
    parser.add_argument('--role', choices=['build', 'runtime', 'minimal'], required=True)
    parser.add_argument('--script', required=True)
    args = parser.parse_args()
    key = f'freebsd-{args.role}-{args.arch}'
    value = entry(key)
    start = time.monotonic()
    root = Path(os.environ['RUNNER_TEMP']) / key
    assets = materialize(value, Path(os.environ['RUNNER_TEMP']) / 'rux-assets', root / 'assets')
    image = root / 'guest.qcow2'
    # The manifest fixes part order; glob ordering must not define image bytes.
    with image.open('wb') as out:
        decoder = subprocess.Popen(['zstd', '-d', '-c'], stdin=subprocess.PIPE, stdout=out)
        try:
            import shutil
            for part in value['parts']:
                with (assets / part).open('rb') as source:
                    shutil.copyfileobj(source, decoder.stdin)
            decoder.stdin.close()
            if decoder.wait() != 0:
                raise RuntimeError('Image decompression failed')
        finally:
            if decoder.poll() is None:
                decoder.kill()
                decoder.wait()
    with FreeBSDVM(image, assets / value['ssh_key'], args.arch, root / 'vm') as vm:
        expected = shlex.quote(value['revision'])
        vm.run(f'test "$(cat /etc/rux-ci-revision)" = {expected}')
        vm.push(ROOT, '/work/Rux')
        # Runtime jobs consume artifacts, which are intentionally excluded from the source push.
        if (ROOT / 'Bin').exists():
            vm.push(ROOT / 'Bin', '/work/Rux/Bin')
        status = vm.run('cd /work/Rux && export PATH=/opt/rux-tools/cmake/bin:$PATH && sh -eu ' +
                        shlex.quote(args.script), check=False).returncode
        # Preserve completed compiler work even when tests subsequently fail.
        for folder in ('Bin', 'BuildCache/ccache', 'FreeBSDAArch64Payload'):
            if vm.run('test -d /work/Rux/' + folder, check=False, quiet=True).returncode == 0:
                vm.pull('/work/Rux/' + folder, ROOT / folder)
    elapsed = time.monotonic() - start
    if os.environ.get('GITHUB_STEP_SUMMARY'):
        with open(os.environ['GITHUB_STEP_SUMMARY'], 'a') as out:
            out.write(f'FreeBSD {args.arch} {args.role}: {elapsed:.1f}s, environment `{value["revision"]}`\n')
    raise SystemExit(status)


if __name__ == '__main__':
    main()
