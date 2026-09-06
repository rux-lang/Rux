"""Run only FreeBSD's two CI guests, with local disks and loopback-only SSH."""
import os
from pathlib import Path
import shlex
import shutil
import socket
import subprocess
import time


class FreeBSDVM:
    def __init__(self, image, key, arch, directory, writable=False):
        self.image, self.key = Path(image).resolve(), Path(key).resolve()
        self.directory = Path(directory).resolve()
        self.arch, self.writable = arch, writable
        self.directory.mkdir(parents=True, exist_ok=True)
        self.process = None

    def __enter__(self):
        if self.arch not in ('x86_64', 'aarch64'):
            raise ValueError('Unsupported FreeBSD architecture')
        os.chmod(self.key, 0o600)
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            self.port = listener.getsockname()[1]
        args = [f'qemu-system-{self.arch}', '-m', '8192', '-smp', str(min(4, os.cpu_count() or 1)),
                '-display', 'none', '-serial', 'file:' + str(self.directory / 'serial.log'),
                '-monitor', 'none', '-drive', f'file={self.image},format=qcow2,if=virtio',
                '-netdev', f'user,id=net0,hostfwd=tcp:127.0.0.1:{self.port}-:22',
                '-device', 'virtio-net-pci,netdev=net0', '-device', 'virtio-rng-pci']
        if self.arch == 'x86_64':
            if not os.access('/dev/kvm', os.R_OK | os.W_OK):
                raise RuntimeError('FreeBSD x86-64 requires accessible KVM on this host')
            args += ['-enable-kvm', '-cpu', 'host']
        else:
            args += ['-machine', 'virt', '-cpu', 'cortex-a72']
            firmware = Path('/usr/share/AAVMF')
            variables = self.directory / 'AAVMF_VARS.fd'
            shutil.copyfile(firmware / 'AAVMF_VARS.fd', variables)
            args += ['-drive', f'if=pflash,format=raw,readonly=on,file={firmware}/AAVMF_CODE.fd',
                     '-drive', f'if=pflash,format=raw,file={variables}']
        if not self.writable:
            args += ['-snapshot']
        self.log = (self.directory / 'qemu.log').open('w')
        self.process = subprocess.Popen(args, stdout=self.log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 600
            while time.monotonic() < deadline:
                if self.process.poll() is not None:
                    raise RuntimeError('QEMU stopped during boot; inspect qemu.log and serial.log')
                try:
                    if self.run('true', check=False, quiet=True, timeout=15).returncode == 0:
                        self.run('uname -m; freebsd-version')
                        return self
                except subprocess.TimeoutExpired:
                    pass
                time.sleep(3)
            raise TimeoutError('FreeBSD did not become reachable within ten minutes')
        except BaseException:
            self.close()
            raise

    def ssh(self):
        return ['ssh', '-i', str(self.key), '-p', str(self.port), '-o', 'BatchMode=yes',
                '-o', 'IdentitiesOnly=yes', '-o', 'StrictHostKeyChecking=no',
                '-o', 'UserKnownHostsFile=/dev/null', '-o', 'ConnectTimeout=10',
                '-o', 'ServerAliveInterval=20', '-o', 'ServerAliveCountMax=3']

    def run(self, command, check=True, quiet=False, timeout=None):
        return subprocess.run(self.ssh() + ['root@127.0.0.1', command], check=check, timeout=timeout,
                              stdout=subprocess.DEVNULL if quiet else None,
                              stderr=subprocess.DEVNULL if quiet else None)

    def push(self, source, target):
        self.run('mkdir -p ' + shlex.quote(target))
        subprocess.run(['rsync', '-a', '--exclude=Build/', '--exclude=Bin/', '--exclude=.git/',
                        '--exclude=BuildCache/CMake/', '-e', shlex.join(self.ssh()),
                        str(Path(source).resolve()) + '/', f'root@127.0.0.1:{target}/'], check=True)

    def pull(self, source, target):
        Path(target).mkdir(parents=True, exist_ok=True)
        subprocess.run(['rsync', '-a', '-e', shlex.join(self.ssh()),
                        f'root@127.0.0.1:{source}/', str(Path(target).resolve()) + '/'], check=True)

    def shutdown(self):
        self.run('/sbin/shutdown -p now', check=False, quiet=True, timeout=30)
        self.process.wait(timeout=180)

    def close(self):
        if self.process and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        if hasattr(self, 'log'):
            self.log.close()

    def __exit__(self, *exc):
        self.close()
