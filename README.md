# Kitty Litter

Kitty Litter is a macOS menu-bar cleanup assistant. It puts files into quarantine first; permanent deletion is a separate, explicit action.

## First version

- Scans the signed-in user's `~/Library/Caches` and `~/Library/Logs` for regular files at least 1 MiB old enough to be considered stale (30 days).
- Scans `~/Downloads` for `.dmg`, `.pkg`, and `.zip` files at least 50 MiB and 60 days old.
- Skips hidden files and symbolic links and caps a scan at 5,000 candidates.
- Does not scan other users, external volumes, or system locations.
- Selecting **Pound into Quarantine** moves chosen files to `~/Library/Application Support/Kitty Litter/Quarantine`. The Quarantine tab can restore them to their original paths if those paths are clear.
- **Release Permanently** deletes selected quarantined files only after a confirmation dialog. It does not delete files directly from their original locations.
- Shows a low-space indicator below 20 GB free and scans at launch, on request, and every six hours while running.

## Build and run

```sh
make build
make run
```

The built app is `build/Kitty Litter.app` and requires macOS 13 or later. Apple Silicon (`arm64`) and Intel (`x64`) are supported; 32-bit macOS is unsupported.

Build for Intel from Apple Silicon with `ARCH=x64 make build`, or explicitly build Apple Silicon with `ARCH=arm64 make build`.

## Optional login background app

The app can run at login as a per-user LaunchAgent. This is opt-in:

```sh
make install-agent
```

Remove the login item and copied app with:

```sh
make uninstall-agent
```

Uninstalling preserves the quarantine directory. Review and restore or permanently release its contents from Kitty Litter before removing that data yourself.

## Linux and FreeBSD development builds

The GTK version is an in-progress port. It scans the current user's `~/.cache`, `~/.local/state`, and `~/Downloads`, skips hidden files and symlinks, and caps each scan at 5,000 candidates and 100,000 visited entries. Review items before moving them to the private `kitty-litter/quarantine` data directory; permanent deletion requires a separate confirmation. The GTK port does not scan system directories or other users.

```sh
make -C platforms/linux
# or on FreeBSD with GTK 3 and SQLite development packages installed:
make -C platforms/freebsd
sudo make -C platforms/linux install PREFIX=/usr
# or install on FreeBSD under /usr/local:
sudo make -C platforms/freebsd install
```

Linux and FreeBSD Makefiles accept `ARCH` and `CC` for arm64, x86, and x64 targets. Cross builds need a matching target sysroot and GTK/SQLite libraries; set `PKG_CONFIG_LIBDIR` and `PKG_CONFIG_SYSROOT_DIR` to keep pkg-config from selecting host libraries.

On Debian or Ubuntu, `packaging/debian/build.sh` stages a `.deb` without
installing files on the build host. Set `VERSION` and `MAINTAINER` first.

On Fedora, install `gcc make gtk3-devel sqlite-devel pkgconf-pkg-config rpm-build`, then build an RPM with `VERSION=0.2.0 packaging/fedora/build.sh`. The builder accepts `ARCH=aarch64`, `ARCH=x86_64`, or `ARCH=i686`; cross builds also need a matching compiler and target GTK/SQLite development libraries.

The Debian builder accepts `ARCH=x64`, `ARCH=x86`, or `ARCH=arm64` and forwards `CC` to the Linux Makefile. Cross builds need matching GTK/SQLite libraries and target pkg-config paths.

The GTK ports still need native OS runtime validation.

## Windows development build

The Win32 version uses the native file list and keeps the same review → quarantine → restore/release flow. It scans the current user's LocalAppData Temp/Logs folders and Downloads, skips hidden/system/reparse-point entries, and stores its quarantine and UTF-8 index under LocalAppData. Permanent deletion is limited to explicitly checked quarantine entries and requires a confirmation dialog.

From a Visual Studio Native Tools PowerShell session for each target:

```powershell
./platforms/windows/build.ps1 -Arch x64
./platforms/windows/build.ps1 -Arch x86
./platforms/windows/build.ps1 -Arch arm64
```

The x86 and x64 sources cross-compile on the development Mac; the Windows runtime and ARM64 build still need native validation.
