#!/usr/bin/env python3
"""Check the native comparison tool against matching, corrupt and missing data."""
from pathlib import Path
import subprocess
import sys
import tempfile

executable = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='kyty-pkg-compare-') as temporary:
    root = Path(temporary)
    package = root / 'fixture.pkg'
    folder = root / 'folder'
    folder.mkdir()
    subprocess.run([sys.executable, str(Path(__file__).with_name('make_sparse_fixture.py')),
                    str(package)], check=True)
    boot = folder / 'eboot.bin'
    for content, expected in [(b'\x7fELF' + bytes(252), 0),
                              (b'\x7fBAD' + bytes(252), 1), (None, 1)]:
        if content is None:
            boot.unlink()
        else:
            boot.write_bytes(content)
        result = subprocess.run([str(executable), str(package), str(folder), 'eboot.bin'],
                                timeout=90)
        if result.returncode != expected:
            raise SystemExit(f'Expected exit {expected}, received {result.returncode}')
