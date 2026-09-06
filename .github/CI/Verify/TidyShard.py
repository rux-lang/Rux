"""Analyze one disjoint part of the complete compilation database."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess


def select_files(database, index, count):
    if count < 1 or not 0 <= index < count:
        raise ValueError('Invalid clang-tidy shard')
    files = sorted({str((Path(row['directory']) / row['file']).resolve()) for row in database})
    return files[index::count]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--index', type=int, required=True)
    parser.add_argument('--count', type=int, default=3)
    parser.add_argument('--database', default='Build')
    args = parser.parse_args()
    database = json.loads((Path(args.database) / 'compile_commands.json').read_text())
    files = select_files(database, args.index, args.count)
    if not files:
        raise ValueError('Empty clang-tidy shard')
    print(f'Analyzing {len(files)} translation units (shard {args.index + 1}/{args.count})', flush=True)
    expression = '^(?:' + '|'.join(re.escape(name) for name in files) + ')$'
    subprocess.run(['run-clang-tidy-23', '-quiet', '-j', str(min(4, os.cpu_count() or 1)), '-clang-tidy-binary', 'clang-tidy-23',
                    '-config-file', '.clang-tidy', '-p', args.database, expression], check=True)


if __name__ == '__main__':
    main()
