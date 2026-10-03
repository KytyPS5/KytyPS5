# Read-only PKG mounting

Kyty discovers `.pkg` and `.fpkg` alongside `.zar` and extracted games. The
native archive backend reads requested ranges through a separate .NET process;
it does not extract the complete game. The reader includes the SDK NAPS offset,
chunk-length, codec-mode and larger inode-table fixes used in the prior addon.

## Build and install

The native bridge is built normally. To publish and install the helper as part
of the build, install Git and the .NET 10 SDK, then add the helper option to your
normal CMake configuration:

```sh
cmake -S . -B _Build/windows -DKYTY_BUILD_PKG_HELPER=ON
cmake --build _Build/windows --target launcher --parallel 8
cmake --install _Build/windows --prefix _Build/windows/install
```

Running the reader requires the .NET 10 runtime. Its source dependency is fetched
by the enabled helper target, pinned to LibProsperoPKG revision
`748eabf1b7d17819528cabf367d8e27109d8fce3`. For an offline build, supply
`-DKYTY_PKG_SOURCE=/path/to/LibProsperoPKG` using that revision.
The default native build does not download this dependency or require .NET.

Alternatively publish explicitly, then rebuild/install normally:

```sh
python tools/pkg/setup.py --source /path/to/LibProsperoPKG --output _Build/windows/pkg
```

Installed layout: `launcher.exe` and `kyty_emulator.exe`, with
`pkg/KytyPkgMount.dll`, `pkg/KytyPkgMount.deps.json` and
`pkg/KytyPkgMount.runtimeconfig.json` alongside them.
The bridge resolves `pkg` relative to its executable, independently of the working
directory. macOS installs it inside `Contents/MacOS/pkg`.
`KYTY_PKG_HELPER` remains an explicit override; an invalid override reports an
error instead of silently choosing another helper. Standard installations need
no environment variable.

## Refresh

Add the containing folder in launcher settings. After adding, completing,
renaming or removing a package, press Refresh (or the platform Refresh shortcut).
The launcher rescans the filesystem and invalidates cached archive metadata;
existing open readers remain usable. An initially incomplete package is retried
on the next scan. The toolbar displays scan status and unavailable-archive counts.
Hover over that count for troubleshooting. Sorting, search text, selection where
possible, and the identity of running game rows are preserved.

## Scope

The implemented reader handles the plaintext/no-auth SDK debug profile: FIH v3,
64 KiB outer PFS blocks, `PPRPLAIN-NOAUTH!`, and supported NAPS layouts.
Retail/encrypted content and unimplemented layouts or codec modes fail explicitly.
Mounting a package does not establish game compatibility. This contribution does
not include shader workarounds, passcode recovery, signing keys, SDK binaries or
game data.

## Tests

With helper publishing enabled and Qt available:

```sh
cmake --build _Build/windows --target pkg_discovery_tests pkg_bridge_tests
ctest --test-dir _Build/windows -R '^pkg_' --output-on-failure
```

Tests generate sparse data with more than 5 GB logical size. Use a filesystem
supporting sparse files. They cover same-process discovery and incomplete-copy
retry, duplicate roots, metadata invalidation while a reader remains open,
rename/removal, extracted games, Unicode paths, 64-bit offsets, cross-block reads,
EOF and concurrent reads. They do not run a commercial game.

Local validation: managed publish succeeded, both regression executables passed
on Linux, and the changed launcher source passed a Qt Widgets syntax compilation.
Full Windows launcher build and interactive Refresh testing remain necessary.
The originally reported restart-only symptom was not reproduced on Windows here.

## Licenses

Native integration follows Kyty's GPL-2.0-only license. The original managed
helper is AGPL-3.0-or-later. LibProsperoPKG retains its upstream GPL/AGPL notices;
source is fetched separately and compiled without embedded signing resources.
The native process communicates with the reader through stdin/stdout pipes.
See `licenses/AGPL-3.0.txt` and upstream source notices. Distribute corresponding
source alongside helper binaries as required by those licenses.

## Helper startup failures

PKG helper creation is serialized through closing the child-side pipe ends,
preventing concurrent PKG launches from inheriting each other's temporary handles.
Handshake and catalog reads share a 60-second deadline; receiving partial data
does not reset it. A timeout closes the transport and terminates the helper,
and the package is reported as unavailable. Ordinary asset reads retain their
existing behavior. Scanning still runs on the UI thread, so it can remain
unresponsive until a slow package finishes or its startup deadline expires.

The startup regression uses a native mock helper and needs no .NET runtime:

```sh
cmake --build _Build/windows --target pkg_startup_tests
ctest --test-dir _Build/windows -R '^pkg_startup$' --output-on-failure
```

It covers silent startup, partial handshake, stalled catalog, forced cleanup,
a successful mount after failure, and EOF during concurrent helper launches.
