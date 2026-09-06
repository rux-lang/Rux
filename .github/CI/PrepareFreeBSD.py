"""Manual image factory. Checkpoints live in a draft release, not paid cache storage."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

from Environment import ROOT, digest, download
from FreeBSDVM import FreeBSDVM


def call(*args, **kwargs):
    return subprocess.run(list(args), check=True, **kwargs)


def pack(image, output, name):
    # Stream compression into bounded release assets without a second full disk copy.
    parts = []
    process = subprocess.Popen(['zstd', '-T0', '-3', '-c', str(image)], stdout=subprocess.PIPE)
    try:
        eof = False
        index = 0
        while not eof:
            target = output / f'{name}.qcow2.zst.{index:03}'
            total = 0
            with target.open('wb') as out:
                while total < 1024 ** 3:
                    chunk = process.stdout.read(min(1024 ** 2, 1024 ** 3 - total))
                    if not chunk:
                        eof = True
                        break
                    out.write(chunk)
                    total += len(chunk)
            if total:
                parts.append(target)
            else:
                target.unlink()
            index += 1
        if process.wait() != 0:
            raise RuntimeError('Image compression failed')
    finally:
        process.stdout.close()
        if process.poll() is None:
            process.kill()
            process.wait()
    return parts


def unpack(parts, image):
    with image.open('wb') as out:
        process = subprocess.Popen(['zstd', '-d', '-c'], stdin=subprocess.PIPE, stdout=out)
        try:
            for part in parts:
                with part.open('rb') as source:
                    shutil.copyfileobj(source, process.stdin)
            process.stdin.close()
            if process.wait() != 0:
                raise RuntimeError('Checkpoint decompression failed')
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--arch', choices=['x86_64', 'aarch64'], required=True)
    p.add_argument('--revision', required=True)
    p.add_argument('--phase', choices=['base', 'cmake', 'finalize'], required=True)
    args = p.parse_args()
    if not re.fullmatch(r'ci-tools-freebsd-[A-Za-z0-9._-]+', args.revision):
        raise ValueError('Revision must start ci-tools-freebsd- and contain only letters, digits, dots, dashes, underscores')
    work = Path(os.environ['RUNNER_TEMP']) / 'image-factory'
    work.mkdir(parents=True, exist_ok=True)
    output = work / 'publish'
    output.mkdir(exist_ok=True)
    image = work / 'guest.qcow2'
    repo = os.environ['GITHUB_REPOSITORY']
    base = json.loads((ROOT / '.github/CI/FreeBSDBases.json').read_text())['architectures'][args.arch]
    key = download(base['key'], work / 'downloads')
    identity = hashlib.sha256(b''.join((ROOT / file).read_bytes() for file in (
        '.github/CI/FreeBSDBases.json', '.github/CI/Toolchains.json', '.github/CI/PrepareFreeBSD.py',
        '.github/CI/FreeBSDVM.py', '.github/CI/Install/FreeBSDCMake.sh',
        '.github/CI/Verify/LLVM.sh'))).hexdigest()
    checkpoint_name = f'checkpoint-{args.arch}'
    if args.phase == 'base':
        source = download(base['image'], work / 'downloads')
        unpack([source], image)
    else:
        call('gh', 'release', 'download', args.revision, '--repo', repo,
             '--pattern', checkpoint_name + '*', '--dir', str(work))
        checkpoint = json.loads((work / (checkpoint_name + '.json')).read_text())
        if checkpoint['identity'] != identity:
            raise ValueError('Incompatible checkpoint: use a new revision after changing the recipe')
        parts = [work / part['name'] for part in checkpoint['parts']]
        for part, pin in zip(parts, checkpoint['parts']):
            if digest(part) != pin['sha256']:
                raise ValueError('Checkpoint checksum mismatch')
        unpack(parts, image)
        if args.phase == 'cmake' and checkpoint['complete']:
            print('CMake already complete; retaining checkpoint')
            return
    complete = False
    with FreeBSDVM(image, key, args.arch, work / 'vm', writable=True) as vm:
        if args.phase == 'base':
            vm.run('pkg install -y llvm23 cmake-core ninja ccache git rsync ca_root_nss')
        vm.push(ROOT, '/work/Rux')
        if args.phase == 'base':
            vm.run("cd /work/Rux && sh .github/CI/Verify/LLVM.sh clang++23 && ninja --version | grep '^1.13.2$'")
        if args.phase == 'cmake':
            result = vm.run('cd /work/Rux && timeout -s INT -k 60 7200 sh '
                            '.github/CI/Install/FreeBSDCMake.sh /opt/rux-tools/cmake', check=False)
            if result.returncode not in (0, 124):
                raise RuntimeError(f'CMake failed, rather than reaching its checkpoint budget: {result.returncode}')
            complete = result.returncode == 0
        elif args.phase == 'finalize':
            vm.run("/opt/rux-tools/cmake/bin/cmake --version | grep '^cmake version 4.4.3$'")
            vm.run(f'printf "%s\\n" {args.revision} > /etc/rux-ci-revision')
            vm.run('mkdir -p /opt/rux-tools/metadata; pkg query "%n %v" > /opt/rux-tools/metadata/packages.txt; '
                   'freebsd-version > /opt/rux-tools/metadata/freebsd.txt')
            # This invocation is intentionally preparation-only. Consumer workflows never reach it.
            vm.run('rm -rf /opt/rux-tools/cmake/build /opt/rux-tools/cmake/cmake-4.4.3; '
                   'rm -f /opt/rux-tools/cmake/cmake-4.4.3.tar.gz; pkg clean -ay; '
                   'rm -rf /work/Rux; rm -f /root/.history /root/.sh_history')
        vm.shutdown()
    if args.phase != 'finalize':
        parts = pack(image, output, checkpoint_name)
        checkpoint = {'identity': identity, 'complete': complete,
                      'parts': [{'name': x.name, 'sha256': digest(x)} for x in parts]}
        metadata = output / (checkpoint_name + '.json')
        metadata.write_text(json.dumps(checkpoint, indent=2) + '\n')
        call('gh', 'release', 'upload', args.revision, '--repo', repo, '--clobber',
             *(str(x) for x in [*parts, metadata]))
        return
    # Fresh boot is independent of the writable preparation boot.
    with FreeBSDVM(image, key, args.arch, work / 'validate') as vm:
        vm.push(ROOT, '/work/Rux')
        vm.run('cd /work/Rux && export PATH=/opt/rux-tools/cmake/bin:$PATH && '
               'sh Run.sh test --compiler clang++23 --jobs 4')
        if args.arch == 'aarch64':
            vm.run('cd /work/Rux && sh Tests/Native/FreeBSDAArch64/Verify.sh ./Bin/rux')
        vm.pull('/work/Rux/Bin', work / 'rux')
        # Preserve actual runtime library paths rather than assuming a package split.
        vm.run("set -e; mkdir -p /tmp/runtime; ldd /work/Rux/Bin/rux > /tmp/runtime/ldd.txt; "
               "! grep -q 'not found' /tmp/runtime/ldd.txt; "
               "awk '$2 == \"=>\" && $3 ~ /^\\/usr\\/local\\// { sub(/^\\//, \"\", $3); print $3 }' "
               "/tmp/runtime/ldd.txt > /tmp/runtime/libraries; "
               "tar -czhf /tmp/runtime/libraries.tar.gz -C / -T /tmp/runtime/libraries; "
               "tar -czf /tmp/runtime/cmake.tar.gz -C /opt/rux-tools/cmake .")
        vm.pull('/tmp/runtime', work / 'runtime')
        if args.arch == 'x86_64':
            vm.run('cd /work/Rux && sh Tests/Native/FreeBSDAArch64/BuildTransfer.sh '
                   './Bin/rux /tmp/transfer && tar -czf /tmp/runtime/transfer.tar.gz -C /tmp/transfer .')
            vm.pull('/tmp/runtime', work / 'runtime')
            call('gh', 'release', 'upload', args.revision, '--repo', repo,
                 str(work / 'runtime/transfer.tar.gz'), '--clobber')
    candidates = {}
    def publish(role, disk):
        name = f'freebsd-{role}-{args.arch}'
        parts = pack(disk, output, name)
        key_out = output / f'{name}.key'
        shutil.copyfile(key, key_out)
        files = [*parts, key_out]
        value = {'key': name, 'kind': 'qcow2', 'revision': args.revision,
                 'parts': [x.name for x in parts], 'ssh_key': key_out.name,
                 'files': [{'name': x.name, 'sha256': digest(x),
                            'url': f'https://github.com/{repo}/releases/download/{args.revision}/{x.name}'} for x in files]}
        call('gh', 'release', 'upload', args.revision, '--repo', repo, *(str(x) for x in files))
        candidates[name] = value
        for part in parts:
            part.unlink()

    publish('build', image)
    source = download(base['image'], work / 'downloads')
    for role in (['runtime', 'minimal'] if args.arch == 'aarch64' else ['runtime']):
        unpack([source], image)
        with FreeBSDVM(image, key, args.arch, work / role, writable=True) as vm:
            vm.run(f'printf "%s\\n" {args.revision} > /etc/rux-ci-revision')
            if role == 'runtime':
                vm.push(work / 'runtime', '/tmp/runtime')
                vm.run('tar -xzf /tmp/runtime/libraries.tar.gz -C /; rm -rf /tmp/runtime')
            vm.shutdown()
        with FreeBSDVM(image, key, args.arch, work / (role + '-validate')) as vm:
            vm.push(ROOT, '/work/Rux')
            if role == 'runtime':
                vm.push(work / 'rux', '/work/Rux/Bin')
                vm.run('cd /work/Rux && ./Bin/rux check && ./Bin/rux lint && '
                       './Bin/rux test --release --jobs 4')
                if args.arch == 'aarch64':
                    vm.run('cd /work/Rux && sh Tests/Native/FreeBSDAArch64/Verify.sh ./Bin/rux')
            else:
                call('gh', 'release', 'download', args.revision, '--repo', repo,
                     '--pattern', 'transfer.tar.gz', '--dir', str(work / 'transfer'))
                vm.push(work / 'transfer', '/tmp/transfer-input')
                vm.run('mkdir -p /tmp/transfer; tar -xzf /tmp/transfer-input/transfer.tar.gz -C /tmp/transfer; '
                       'cd /work/Rux && sh Tests/Native/FreeBSDAArch64/VerifyTransfer.sh /tmp/transfer')
        publish(role, image)
    cmake = output / f'cmake-4.4.3-freebsd-15.1-{args.arch}.tar.gz'
    shutil.copyfile(work / 'runtime/cmake.tar.gz', cmake)
    candidate = output / f'freebsd-{args.arch}.json'
    candidate.write_text(json.dumps({'validated': True, 'environments': candidates}, indent=2) + '\n')
    call('gh', 'release', 'upload', args.revision, '--repo', repo, str(cmake), str(candidate))


if __name__ == '__main__':
    main()
