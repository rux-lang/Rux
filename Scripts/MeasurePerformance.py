#!/usr/bin/env python3
"""Repeat matched compiler/build measurements. Run in the configured C++ toolchain environment."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import statistics
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--build", type=Path, default=Path("Build"))
    parser.add_argument("--output", type=Path, default=Path("Build/Bench/performance.json"))
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    parser.add_argument("--configuration", default="Release")
    parser.add_argument("--runtime-only", action="store_true", help="Measure an existing Rux executable without C++ build probes")
    parser.add_argument("--rux-executable", type=Path, help="Existing Rux executable (defaults to the source tree's Bin directory)")
    parser.add_argument("--workspace-check", action="store_true", help="Include complete workspace semantic checking")
    parser.add_argument("--clean", action="store_true", help="Include clean compiler/unit-test builds")
    parser.add_argument("--tests", action="store_true", help="Include the C++ suite")
    parser.add_argument("--rux-suite", action="store_true", help="Include the Rux suite")
    parser.add_argument("--rux-jobs", type=int, default=1, help="Use >1 only with a compiler supporting --jobs")
    parser.add_argument("--manifest", type=Path, default=Path("Tests/Language/Arithmetic/Rux.toml"))
    args = parser.parse_args()
    if args.repetitions < 2 or args.jobs < 1 or args.rux_jobs < 1:
        parser.error("use at least two repetitions and a positive worker count")
    if args.runtime_only and (args.clean or args.tests):
        parser.error("--runtime-only cannot be combined with --clean or --tests")
    source = args.source.resolve()
    build = (source / args.build).resolve()
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    executable = (args.rux_executable or source / "Bin" / ("rux.exe" if os.name == "nt" else "rux")).resolve()
    build_command = ["cmake", "--build", str(build), "--config", args.configuration, "--parallel", str(args.jobs)]
    environment = os.environ.copy()
    environment.setdefault("SOURCE_DATE_EPOCH", "1788566400")
    results = {
        "source": str(source), "build": str(build), "configuration": args.configuration,
        "host": platform.platform(), "architecture": platform.machine(), "processors": os.cpu_count(),
        "jobs": args.jobs, "source_date_epoch": environment["SOURCE_DATE_EPOCH"], "measurements": {},
        "runtime_only": args.runtime_only, "executable": str(executable),
        "cache": (build / "CMakeCache.txt").read_text(encoding="utf-8") if (build / "CMakeCache.txt").exists() else None,
    }

    tool_versions = {}
    for name, command in [("cmake", ["cmake", "--version"]), ("git", ["git", "--version"])]:
        if shutil.which(command[0]):
            tool_versions[name] = subprocess.check_output(command, text=True, encoding="utf-8").strip()
    results["tools"] = tool_versions

    def save():
        output.write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8", newline="\n")

    def run(command, label):
        started = time.perf_counter()
        process = subprocess.run(command, cwd=source, env=environment, capture_output=True, text=True, encoding="utf-8", errors="replace")
        elapsed = time.perf_counter() - started
        log = output.parent / (output.stem + "." + label + ".log")
        with log.open("a", encoding="utf-8", newline="\n") as stream:
            stream.write(json.dumps(command) + "\n" + process.stdout + process.stderr + "\n")
        if process.returncode:
            raise RuntimeError(f"{label} failed ({process.returncode}); see {log}")
        return elapsed

    def measure(label, command, prepare=None):
        samples = []
        for repeat in range(args.repetitions):
            if prepare:
                prepare()
            samples.append(run(command, label))
            print(f"{label} {repeat + 1}/{args.repetitions}: {samples[-1]:.3f} s", flush=True)
        results["measurements"][label] = {"seconds": samples, "median_seconds": statistics.median(samples)}
        save()

    if not args.runtime_only:
        # Warm the configured build before measuring unchanged work. SOURCE_DATE_EPOCH is identical in all subprocesses.
        run(["cmake", "-S", str(source), "-B", str(build)], "configure")
        run(build_command, "warm")
        if args.clean:
            measure("clean_build", build_command, lambda: run(build_command + ["--target", "clean"], "clean"))
        measure("unchanged_build", build_command)
        measure("reconfigure", ["cmake", "-S", str(source), "-B", str(build)])
        measure("after_reconfigure", build_command)

        def implementation_path():
            path = source / "Compiler/Semantic/Analysis/TypeResolution.cpp"
            return path if path.exists() else source / "Compiler/Semantic/SemanticAnalyzer.cpp"

        header = source / "Compiler/Types/Type.h"
        if not header.exists():
            header = source / "Compiler/Semantic/Type.h"
        for label, path in [("implementation_rebuild", implementation_path()), ("shared_header_rebuild", header)]:
            original = path.stat()
            try:
                measure(label, build_command, lambda: os.utime(path, None))
            finally:
                os.utime(path, ns=(original.st_atime_ns, original.st_mtime_ns))

    manifest = (source / args.manifest).resolve()
    compile_command = [str(executable), "--quiet", "--manifest", str(manifest), "build", "--release"]
    run(compile_command, "rux_compile_warm")
    measure("rux_compile", compile_command)
    if args.workspace_check:
        command = [str(executable), "--quiet", "check"]
        run(command, "workspace_check_warm")
        measure("workspace_check", command)
    if args.tests:
        measure("cpp_tests", ["ctest", "--test-dir", str(build), "-C", args.configuration, "--output-on-failure", "-R", "^Unit", "-j", str(min(args.jobs, 4)), "--no-tests=error"])
    if args.rux_suite:
        command = [str(executable), "--quiet", "test", "--release"]
        if args.rux_jobs != 1:
            command += ["--jobs", str(args.rux_jobs)]
        measure("rux_suite", command)
    results["executable_bytes"] = executable.stat().st_size
    results["executable_sha256"] = hashlib.sha256(executable.read_bytes()).hexdigest()
    size_tool = shutil.which("llvm-size")
    if size_tool:
        sections = subprocess.run([size_tool, "-A", "-d", str(executable)], capture_output=True, text=True, check=True)
        results["sections"] = sections.stdout
    save()
    print(f"Saved {output}", flush=True)


if __name__ == "__main__":
    main()
