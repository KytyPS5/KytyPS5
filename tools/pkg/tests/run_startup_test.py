#!/usr/bin/env python3
"""Run transport failure tests with a native mock helper, without .NET or game files."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

executable = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='kyty-pkg-startup-') as directory:
    root = Path(directory)
    shutil.copy2(executable, root / ('dotnet.exe' if os.name == 'nt' else 'dotnet'))
    helper = root / 'mock.dll'
    helper.touch()
    env = os.environ.copy()
    env['PATH'] = str(root) + os.pathsep + env.get('PATH', '')
    env['KYTY_PKG_HELPER'] = str(helper)
    subprocess.run([str(executable)], cwd=root, env=env, check=True, timeout=30)
