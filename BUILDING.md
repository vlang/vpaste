# Building and releasing Keypour

Keypour has a V application and a native input adapter for each operating system.
The adapters compile into `build/native.o`; V links that object into the final
executable. No Qt, browser extension, network service, or V package is required.

## Release targets

| Target | Build host | Release file |
| --- | --- | --- |
| Windows 10 version 1607+ / Windows 11, amd64 | Windows x64, MinGW-w64 GCC | `keypour-VERSION-windows-amd64.zip` |
| Linux x86_64, X11 desktop | Ubuntu 22.04 x86_64 | `keypour-VERSION-linux-amd64.AppImage` |
| macOS 12+, Apple Silicon | macOS arm64, Apple Clang | `keypour-VERSION-macos-arm64.tar.gz` |
| Source | Git checkout | `keypour-VERSION-source.tar.gz` |

The Linux release is built on Ubuntu 22.04, establishing a glibc 2.35 baseline.
A binary built locally on a newer distribution can require a newer glibc.
AppImage packages the X11/input libraries and their notices; it does not bundle
glibc or provide native Wayland support. Use an X11 session.

Windows CI runs on Windows Server 2022. Compilation targets Windows 10; actual
interactive input still needs testing on the intended Windows 10 desktop and VNC
viewer. macOS CI can compile and package without Accessibility permission, but
interactive input must be tested on a logged-in Mac after granting permission.

## Pinned tools

`scripts/bootstrap-v.sh` and `scripts/bootstrap-v.ps1` download **V 0.5.2** and
verify release-asset SHA-256 hashes before extraction. `scripts/package-linux.sh`
downloads **linuxdeploy 1-alpha-20251107-1**, also checksum-verified. It reuses the
verified tool's own AppImage runtime rather than downloading a moving runtime.
Checksums are from each upstream GitHub release's published asset metadata.

Native compilers and OS development libraries come from the build host. These
scripts specify consistent tool versions and build targets; they do not claim
bit-for-bit reproducibility across changing GitHub runner images.

## Linux

On Debian/Ubuntu, install development prerequisites explicitly:

```sh
sudo apt-get update
sudo apt-get install -y build-essential unzip curl python3 \
  libx11-dev libxtst-dev libxi-dev libxkbcommon-dev
./scripts/bootstrap-v.sh
./scripts/build.sh
./dist/keypour --help
```

To build the AppImage, use:

```sh
./scripts/build.sh --package
```

For the X11 integration tests, also install `xvfb xauth xdotool xclip` and run:

```sh
./scripts/integration-linux.sh
```

The package script expects Debian/Ubuntu package metadata so it can retain
copyright notices for every bundled native library. The executable alone can be
built on other Linux distributions with the equivalent development packages.

Running an AppImage normally can require FUSE. On systems without usable FUSE:

```sh
chmod +x dist/keypour-0.1.0-linux-amd64.AppImage
./dist/keypour-0.1.0-linux-amd64.AppImage --appimage-extract-and-run --help
```

The desktop entry has `Terminal=true`; Keypour's status is shown in a terminal.
To see the included documentation without running the application, extract with
`--appimage-extract` and open `squashfs-root/usr/share/doc/keypour/`.

## Windows

Install a 64-bit MinGW-w64 GCC compiler, Python 3, and Git. One supported option
is [MSYS2 UCRT64](https://www.msys2.org/). From its UCRT64 shell:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-gcc
```

Then open PowerShell in the project folder, ensuring the GCC `bin` directory is
on `PATH` (for a default installation: `C:\msys64\ucrt64\bin`):

```powershell
$env:Path = "C:\msys64\ucrt64\bin;" + $env:Path
./scripts/bootstrap-v.ps1
./scripts/build.ps1 -Package
./dist/keypour.exe --help
```

The packaging step verifies the executable imports only Windows system DLLs.
If a different compiler introduces a runtime DLL dependency, packaging fails
with its name instead of creating an incomplete ZIP. Builds use the console
subsystem so the resident process has visible status and can be stopped with
Ctrl+C. No Windows installer or Authenticode signature is generated.

On a disposable, unlocked interactive Windows desktop, the native adapter
integration suite can be run separately:

```powershell
./scripts/test-windows.ps1 -IsolatedDesktop
```

This test creates its own receiver windows but uses that desktop's keyboard and
clipboard. The explicit switch acknowledges that this is a disposable test
desktop. CI runs it on its temporary Windows VM. A service/session-0 runner fails
the desktop precondition; the test is not silently skipped.

## macOS arm64

Install Apple's command line tools with `xcode-select --install`, and ensure
Python 3 is available. From an Apple Silicon terminal:

```sh
./scripts/bootstrap-v.sh
./scripts/build.sh --package
```

The archive contains the arm64 `keypour` executable, documentation (including
`VALIDATION.md`), runtime
notices, and `Keypour.command`. Opening the command file in Finder starts the
application in Terminal. Run `keypour` directly for CLI flags.

The executable receives an ad-hoc code signature. This is not an Apple Developer
ID signature and it is not notarized. Distribution using a Developer ID requires
your own certificate, signing, and notarization step. macOS can require an
explicit first-open approval for downloaded unsigned software. Install the
binary at a stable path before granting Accessibility and Input Monitoring
permissions; a replacement build can require those permissions to be refreshed.

## Existing V installation and compiler selection

The scripts prefer `KEYPOUR_V`, then `.tools/v/v`, then `v` on `PATH`. For example:

```sh
KEYPOUR_V=/path/to/v CC=gcc ./scripts/build.sh
```

In PowerShell:

```powershell
$env:KEYPOUR_V = 'C:\tools\v\v.exe'
$env:CC = 'gcc'
./scripts/build.ps1
```

Use V 0.5.2 for the supported build. Build scripts run the V core tests and
`--version` / `--help` smoke tests. The GitHub workflow additionally runs the X11
integration suite and the native Win32 receiver tests. Native adapters are compiled with warnings
enabled. Each release package retains the V runtime and native-library notices.

## GitHub Actions and Releases

`.github/workflows/build-release.yml` builds all three platforms on pull requests,
pushes to `main` or `master`, and manual dispatches. Successful jobs upload their
packages as workflow artifacts. Manual dispatch builds artifacts without
publishing a release.

To publish a release from your own GitHub repository:

1. Set the same version in `v.mod` and `core/config.v`.
2. Commit all source changes. Review the build results and perform the native
   interactive checks described in the README.
3. Push a matching tag, for example:

```sh
git tag v0.1.0
git push origin v0.1.0
```

The workflow validates the tag against the source version, builds every target,
archives committed sources, and only publishes after all jobs succeed. The
release contains the three platform packages, the source archive, and
`SHA256SUMS`. It uses the repository's GitHub Actions token; no external signing
keys are required. Only the final publishing job requests `contents: write`.

`SHA256SUMS` verifies downloaded bytes, but is not a publisher code signature.
The publish job does not overwrite an existing release. Resolve an existing-tag
conflict deliberately, or increment the version and use a new tag.

A source archive can also be created from a clean local Git checkout:

```sh
./scripts/package-source.sh 0.1.0
```

The source archive includes tracked files at `HEAD`, excludes ignored build
artifacts and downloaded tools, and refuses a dirty checkout to avoid silently
omitting source changes.

## Tool references

- [V 0.5.2 release](https://github.com/vlang/v/releases/tag/0.5.2)
- [linuxdeploy pinned release](https://github.com/linuxdeploy/linuxdeploy/releases/tag/1-alpha-20251107-1)
- [AppImage runtime source used by that release](https://github.com/AppImage/type2-runtime/tree/61e6688)
- [GitHub runner labels and architectures](https://docs.github.com/en/actions/how-tos/write-workflows/choose-where-workflows-run/choose-the-runner-for-a-job)
- [MSYS2 setup action](https://github.com/msys2/setup-msys2)
- [AppDir format](https://docs.appimage.org/reference/appdir.html)
