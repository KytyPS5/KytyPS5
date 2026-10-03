#!/usr/bin/env python3
"""Run a PKG regression using only generated sparse test data."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('executable', type=Path)
parser.add_argument('--bridge', action='store_true')
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='kyty-pkg-test-') as directory:
    root = Path(directory)
    fixture = root / ('fixture.pkg' if args.bridge else 'fixture.staged')
    subprocess.run([sys.executable, str(Path(__file__).with_name('make_sparse_fixture.py')),
                    str(fixture)], check=True)
    subprocess.run([str(args.executable.resolve()), str(fixture if args.bridge else root)], check=True)
