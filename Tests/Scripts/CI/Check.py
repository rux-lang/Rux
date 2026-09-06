"""Offline regression tests for environment integrity and analysis coverage."""
import copy
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / '.github/CI'))
import Environment
import ValidateNative

spec = importlib.util.spec_from_file_location('TidyShard', ROOT / '.github/CI/Verify/TidyShard.py')
TidyShard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(TidyShard)


class EnvironmentTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.body = b'prepared tool bytes'
        self.asset = {'name': 'tools.tar.gz', 'url': 'https://example.invalid/tools.tar.gz',
                      'sha256': hashlib.sha256(self.body).hexdigest()}
        self.value = {'key': 'linux-build-x86_64', 'kind': 'bundle', 'revision': 'ci-tools-test-1',
                      'archive': 'tools.tar.gz', 'files': [self.asset]}

    def test_download_verifies_and_reuses_bytes(self):
        with patch('Environment.urllib.request.urlopen', return_value=io.BytesIO(self.body)) as request:
            path = Environment.download(self.asset, self.directory)
            self.assertEqual(path.read_bytes(), self.body)
            self.assertEqual(Environment.download(self.asset, self.directory), path)
            request.assert_called_once()

    def test_corrupt_cache_is_replaced(self):
        (self.directory / self.asset['sha256']).write_bytes(b'corrupt')
        with patch('Environment.urllib.request.urlopen', return_value=io.BytesIO(self.body)):
            self.assertEqual(Environment.download(self.asset, self.directory).read_bytes(), self.body)

    def test_corrupt_download_is_not_installed(self):
        with patch('Environment.urllib.request.urlopen', return_value=io.BytesIO(b'corrupt')):
            with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
                Environment.download(self.asset, self.directory)
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_manifest_rejects_unsafe_or_missing_payloads(self):
        Environment.validate(self.value)
        for name in ('../escape', '/absolute', '..', 'tools\\escape'):
            value = copy.deepcopy(self.value)
            value['files'][0]['name'] = name
            with self.subTest(name=name), self.assertRaises(ValueError):
                Environment.validate(value)
        value = copy.deepcopy(self.value)
        value['archive'] = 'missing.tar.gz'
        with self.assertRaises(ValueError):
            Environment.validate(value)

    def test_missing_lock_entry_is_explicit(self):
        lock = self.directory / 'Lock.json'
        lock.write_text(json.dumps({'schema': 1, 'environments': {}}))
        self.assertIsNone(Environment.entry('freebsd-build-aarch64', lock, required=False))
        with self.assertRaisesRegex(ValueError, 'No published'):
            Environment.entry('freebsd-build-aarch64', lock)

    def test_workflow_output_rejects_injection(self):
        with self.assertRaises(ValueError):
            Environment.emit('revision', 'valid\nkind=qcow2')


class AnalysisTests(unittest.TestCase):
    def test_shards_cover_each_translation_unit_once(self):
        rows = [{'directory': str(ROOT), 'file': f'Compiler/File{n}.cpp'} for n in range(17)]
        rows += [rows[0], dict(rows[1], file='Compiler/../Compiler/File1.cpp')]
        shards = [TidyShard.select_files(rows, n, 3) for n in range(3)]
        flattened = [file for shard in shards for file in shard]
        self.assertEqual(len(flattened), 17)
        self.assertEqual(len(set(flattened)), 17)
        self.assertEqual(set(flattened), {str((ROOT / row['file']).resolve()) for row in rows})

    def test_invalid_shards_are_rejected(self):
        for index, count in ((0, 0), (-1, 3), (3, 3)):
            with self.subTest(index=index, count=count), self.assertRaises(ValueError):
                TidyShard.select_files([], index, count)


class LLVMTests(unittest.TestCase):
    def test_package_patch_updates_and_rejected_compilers(self):
        shell = 'C:/Program Files/Git/bin/bash.exe' if os.name == 'nt' else shutil.which('sh')
        if not shell or not Path(shell).is_file():
            self.skipTest('A POSIX shell is unavailable')
        with tempfile.TemporaryDirectory() as directory:
            stub = Path(directory) / 'Compiler.sh'
            stub.write_text('printf "%s\\n" "$TEST_LLVM_VERSION"\n', newline='\n')
            for output, accepted in (
                ('clang version 23.1.0', True),
                ('Ubuntu clang version 23.1.1 (++20260903063840+47bafb752202)', True),
                ('clang version 23.2.0', True),
                ('clang version 23.0.9', False),
                ('clang version 22.1.9', False),
                ('clang version 24.1.0', False),
                ('Apple clang version 23.1.0', False),
                ('unrecognized compiler', False),
            ):
                with self.subTest(output=output):
                    result = subprocess.run(
                        [shell, (ROOT / '.github/CI/Verify/LLVM.sh').as_posix(), shell, stub.as_posix()],
                        env=dict(os.environ, TEST_LLVM_VERSION=output), capture_output=True, text=True)
                    self.assertEqual(result.returncode == 0, accepted, result.stdout + result.stderr)


class MacOSTests(unittest.TestCase):
    def test_bootstrap_includes_dependencies_unique_to_each_tool(self):
        shell = 'C:/Program Files/Git/bin/bash.exe' if os.name == 'nt' else shutil.which('sh')
        if not shell or not Path(shell).is_file():
            self.skipTest('A POSIX shell is unavailable')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'cache').mkdir()
            python_arguments = '"$@"' if os.name != 'nt' else '"$1" "$(cygpath -m "$2")" "$(cygpath -m "$3")"'
            scripts = {
                'bin/brew': '''case "$1" in
update|install) exit 0 ;;
--prefix)
  case "${2:-}" in
    '') printf '%s\\n' "$TEST_BREW_PREFIX" ;;
    *) printf '%s/Cellar/%s/1\\n' "$TEST_BREW_PREFIX" "$2" ;;
  esac ;;
deps)
  case " $* " in *' --union '*) printf '%s\\n' z3 fmt ;; esac ;;
esac''',
                'bin/tar': 'if [ "$1" = -czf ]; then : > "$2"; fi',
                'bin/python3': 'exec "' + Path(sys.executable).as_posix() + '" ' + python_arguments,
                'homebrew/Cellar/llvm@23/1/bin/clang++': 'echo "clang version 23.1.1"',
                'homebrew/Cellar/z3/1/lib/libz3.dylib': '',
                'homebrew/Cellar/fmt/1/lib/libfmt.dylib': '',
                'homebrew/Cellar/ccache/1/bin/ccache': 'echo ccache',
                'homebrew/opt/ccache/bin/ccache': 'echo ccache',
            }
            for name, body in scripts.items():
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('#!/bin/sh\n' + body + '\n', newline='\n')
                path.chmod(0o755)
            command = '''root=$(cd -- "$1" && pwd)
export PATH="$root/bin:/usr/bin:/bin:$PATH" TEST_BREW_PREFIX="$TEST_NATIVE_PREFIX"
sh "$2" "$root/cache"'''
            result = subprocess.run([shell, '-c', command, 'brew-test', root.as_posix(),
                                     (ROOT / '.github/CI/Install/MacOS.sh').as_posix()],
                                    env=dict(os.environ, TEST_NATIVE_PREFIX=(root / 'homebrew').as_posix(),
                                             GITHUB_OUTPUT='', GITHUB_PATH=''),
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            paths = [Path(line).as_posix() for line in (root / 'cache/paths').read_text().splitlines()]
            self.assertIn('Cellar/z3/1', paths)
            self.assertIn('Cellar/fmt/1', paths)

    def test_cached_c_driver_metadata_is_repaired(self):
        shell = 'C:/Program Files/Git/bin/bash.exe' if os.name == 'nt' else shutil.which('sh')
        if not shell or not Path(shell).is_file():
            self.skipTest('A POSIX shell is unavailable')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            scripts = {
                'bin/brew': 'printf "%s\\n" "$TEST_BREW_PREFIX"',
                'bin/tar': 'exit 0',
                'homebrew/Cellar/llvm/23.1.1/bin/clang-23': 'exit 99',
                'homebrew/Cellar/llvm/23.1.1/bin/clang++': 'echo "clang version 23.1.1"',
                'homebrew/opt/ccache/bin/ccache': 'echo "ccache version 4.14"',
            }
            for name, body in scripts.items():
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('#!/bin/sh\n' + body + '\n', newline='\n')
                path.chmod(0o755)
            (root / 'cache').mkdir()
            (root / 'cache/homebrew-tools.tar.gz').touch()
            # Use the shell's native paths on Windows as well as POSIX.
            command = '''set -eu
root=$(cd -- "$1" && pwd)
export PATH="$root/bin:$PATH:/usr/bin:/bin" TEST_BREW_PREFIX="$root/homebrew"
export GITHUB_OUTPUT="$root/output" GITHUB_PATH="$root/paths"
printf '%s\\n' "$TEST_BREW_PREFIX" > "$root/cache/prefix"
printf '%s\\n' "$TEST_BREW_PREFIX/Cellar/llvm/23.1.1/bin/clang-23" > "$root/cache/compiler"
sh "$2" "$root/cache"
'''
            result = subprocess.run([shell, '-c', command, 'macos-test', root.as_posix(),
                                     (ROOT / '.github/CI/Install/MacOS.sh').as_posix()],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertTrue((root / 'cache/compiler').read_text().strip().endswith('/bin/clang++'))
            self.assertTrue((root / 'output').read_text().strip().endswith('/bin/clang++'))


class NativeValidationTests(unittest.TestCase):
    def test_linux_uses_shell_entrypoint_and_does_not_publish_after_failure(self):
        if not shutil.which('pwsh'):
            self.skipTest('PowerShell is unavailable')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            candidate = root / 'candidate-validation/linux-build-x86_64.json'
            candidate.parent.mkdir()
            candidate.write_text(json.dumps({'validated': False, 'environments': {
                'linux-build-x86_64': {'revision': 'ci-tools-native-test', 'files': []}}}))
            installer = root / '.github/Scripts/Install.ps1'
            installer.parent.mkdir(parents=True)
            installer.write_text('param([string]$Tool)\n', newline='\n')
            (root / 'Run.ps1').write_text("throw 'Wrong platform entry point'\n", newline='\n')
            (root / 'Run.sh').write_text('printf "%s\\n" "$@" > arguments\nexit 23\n', newline='\n')
            real_run = subprocess.run
            def execute(command, **kwargs):
                if command[0] == 'pwsh':
                    return real_run(command, cwd=root, capture_output=True, text=True, **kwargs)
                return subprocess.CompletedProcess(command, 0)
            env = dict(RUNNER_TEMP=str(root), GITHUB_REPOSITORY='example/repo')
            if os.name == 'nt':
                env['PATH'] = 'C:/Program Files/Git/bin;' + os.environ['PATH']
            with patch.dict(os.environ, env), patch.object(sys, 'argv', ['ValidateNative', '--revision', 'ci-tools-native-test']), \
                 patch('ValidateNative.host_key', return_value='linux-build-x86_64'), \
                 patch('ValidateNative.platform.system', return_value='Linux'), \
                 patch('ValidateNative.subprocess.run', side_effect=execute) as calls:
                with self.assertRaises(subprocess.CalledProcessError) as failure:
                    ValidateNative.main()
            self.assertEqual(failure.exception.returncode, 23, failure.exception.stderr)
            self.assertEqual((root / 'arguments').read_text().splitlines(),
                             ['test', '--compiler', 'clang++-23', '--jobs', '4'])
            self.assertFalse(json.loads(candidate.read_text())['validated'])
            self.assertFalse(any(call.args[0][:3] == ['gh', 'release', 'upload'] for call in calls.call_args_list))


if __name__ == '__main__':
    unittest.main()
