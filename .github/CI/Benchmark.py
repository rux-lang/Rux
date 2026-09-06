"""Measure worker counts against one existing build; retain every sample."""
import argparse
import json
import os
from pathlib import Path
import platform
import subprocess
import time


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--samples', type=int, default=3)
    p.add_argument('--output', default='BuildCache/worker-benchmark.json')
    args = p.parse_args()
    if args.samples < 1:
        raise ValueError('Samples must be positive')
    rux = './Bin/rux.exe' if platform.system() == 'Windows' else './Bin/rux'
    records = []
    # Alternate order so thermal/cache effects do not always favor the same count.
    for iteration in range(args.samples):
        workers = sorted({1, min(2, os.cpu_count() or 1), min(4, os.cpu_count() or 1)})
        if iteration % 2:
            workers.reverse()
        for count in workers:
            for suite, command in [
                ('ctest', ['ctest', '--test-dir', 'Build', '-C', 'Release', '--output-on-failure',
                           '--no-tests=error', '--parallel', str(count)]),
                ('rux', [rux, 'test', '--release', '--jobs', str(count)])]:
                start = time.monotonic()
                subprocess.run(command, check=True)
                records.append({'suite': suite, 'jobs': count, 'iteration': iteration,
                                'seconds': time.monotonic() - start})
                output = Path(args.output)
                output.parent.mkdir(parents=True, exist_ok=True)
                output.write_text(json.dumps({'platform': platform.platform(), 'cpu_count': os.cpu_count(),
                                              'samples': records}, indent=2) + '\n')
    print(json.dumps(records, indent=2))


if __name__ == '__main__':
    main()
