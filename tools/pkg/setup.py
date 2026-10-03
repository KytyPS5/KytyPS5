#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Build the separate read-only PKG helper from a pinned upstream source tree."""
from pathlib import Path
import argparse
import shutil
import subprocess

ROOT = Path(__file__).resolve().parent
PIN = '748eabf1b7d17819528cabf367d8e27109d8fce3'

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, help='Existing LibProsperoPKG source (offline builds)')
    parser.add_argument('--output', type=Path, default=ROOT / 'helper/out')
    args = parser.parse_args()
    if shutil.which('dotnet') is None:
        raise SystemExit('Install the .NET 10 SDK and reopen the terminal, then retry.')
    source = args.source.resolve() if args.source else ROOT / 'vendor/LibProsperoPKG'
    if not source.exists():
        if args.source:
            raise SystemExit(f'Source directory not found: {source}')
        if shutil.which('git') is None:
            raise SystemExit('Install Git, or pass --source with an existing source checkout.')
        source.mkdir(parents=True)
        subprocess.run(['git', 'init', str(source)], check=True)
        subprocess.run(['git', '-C', str(source), 'remote', 'add', 'origin',
                        'https://github.com/SvenGDK/LibProsperoPKG.git'], check=True)
    if args.source is None:
        head = subprocess.run(['git', '-C', str(source), 'rev-parse', 'HEAD'],
                              text=True, capture_output=True)
        if head.returncode != 0 or head.stdout.strip() != PIN:
            if subprocess.check_output(['git', '-C', str(source), 'status', '--porcelain'], text=True).strip():
                raise SystemExit('Reader checkout has local changes; preserve them before rebuilding.')
            subprocess.run(['git', '-C', str(source), 'fetch', '--depth=1', 'origin', PIN], check=True)
            subprocess.run(['git', '-C', str(source), 'checkout', '--detach', PIN], check=True)
    subprocess.run(['dotnet', 'publish', str(ROOT / 'helper/KytyPkgMount.csproj'),
                    '-c', 'Release', '-p:UseAppHost=false', f'-p:ProsperoSource={source}',
                    '-o', str(args.output.resolve())], check=True)
    print(f'PKG helper published to {args.output.resolve()}')

if __name__ == '__main__':
    main()
