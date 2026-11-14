# Kitty Litter — Linux C port

Preserves the review workflow from the supplied Swift app:

- Tray menu with Open Review Window, Scan Now and Quit.
- Candidates and Quarantine tabs, per-file checkboxes and selected-byte totals.
- Old/large-file scans at launch, on request and every six hours while running.
- Quarantine, Restore, and Release Permanently with a default-Cancel confirmation.
- Free disk space display and the 20 GB low-space notice.

The Linux version shows its review window on launch. Closing that window hides
it; use Quit to exit. Scans and file operations run on a worker thread. No file
is automatically moved or deleted. The original Swift source is preserved at
`macos/KittyLitter.swift`; `make macos` builds it on macOS.

## Build and run on a 64-bit Raspberry Pi desktop

```sh
sudo apt install build-essential pkg-config libgtk-3-dev libayatana-appindicator3-dev
make
./build/native/kitty-litter
```

Requires GTK3, GLib and Ayatana AppIndicator3. The review window works without a
tray host; the tray icon requires an indicator-capable panel. Some desktops need
a panel plugin/extension. `--dark-outline` uses a black tray symbol on light
panels; the default is white for dark panels. The tray uses a vector port of the
supplied litter-tray-and-scoop drawing, not the cat/folder icon made for catz.

```sh
make install PREFIX="$HOME/.local"
```

Installs an executable and application launcher, without enabling autostart.

## ARM64 ELF64

```sh
make elf64
```

On AArch64 Linux, native tools produce `build/arm64/kitty-litter`. Cross builds
require an AArch64 **Linux** compiler and pkg-config wrapper configured with a
matching Linux sysroot and all target GUI development dependencies:

```sh
make elf64 ELF64_CC=/path/to/aarch64-linux-gnu-gcc ELF64_PKG_CONFIG=/path/to/target-pkg-config
```

Native compilation on the Pi is simpler. A macOS ARM64 compiler creates Mach-O,
not Linux ELF. The application is dynamically linked.

## Linux paths and scan rules

| Category | Default location | Minimum age | Minimum file size |
| --- | --- | --- | --- |
| Cache | `$XDG_CACHE_HOME`, normally `~/.cache` | 30 days | 1 MiB |
| Log | `$XDG_STATE_HOME/log`, normally `~/.local/state/log` | 30 days | 1 MiB |
| Old download | Configured XDG Downloads directory, otherwise `~/Downloads` | 60 days | 50 MiB |

Download candidates are regular `.zip`, `.deb`, `.rpm`, `.AppImage`, `.tar`,
`.gz`, `.xz`, `.bz2`, `.zst`, `.7z`, `.dmg` or `.pkg` files (case insensitive).
Only regular files are candidates. Directory packages are not treated as files.
Hidden descendants and symbolic links are skipped; scans do not traverse symlink
components even in the configured roots. Missing/unreadable roots are counted as
skipped. Scan results are capped at 5,000 and initially sorted by descending size.
Overlapping root paths are deduplicated. File sizes are logical bytes, not
allocated-block usage; hard links can make those totals overstate reclaimable space.

Override paths with absolute environment values as needed:

```sh
KITTY_LITTER_LOG_DIR="$HOME/my-logs" ./build/native/kitty-litter
```

Available variables: `KITTY_LITTER_CACHE_DIR`, `KITTY_LITTER_LOG_DIR`,
`KITTY_LITTER_DOWNLOAD_DIR`, and `KITTY_LITTER_QUARANTINE`.
The app does not scan all of `~/.local/state`, which may contain important
application state rather than logs.

## Quarantine storage and recovery

Default: `$XDG_DATA_HOME/kitty-litter/quarantine`, normally
`~/.local/share/kitty-litter/quarantine`. This port does not import or modify the
macOS JSON index. Each entry gets a random 128-bit hexadecimal directory name,
a binary `record` manifest, and the original regular file as `payload`.
Manifests contain the original path, category, size, inode/device and modification
time. They are synced before moving the source, so a moved file retains recovery
information even if the app exits before updating the UI.

Storage must be private to the current user. All path traversal uses no-follow
directory descriptors. Source identity, size and modification time are checked
against the reviewed item; changed/replaced files are refused. Moves use Linux
`renameat2(RENAME_NOREPLACE)`. Existing restore destinations, including dangling
symlinks, are never overwritten. Missing restore parents are recreated with
private permissions; symlink parents are refused. Restores retain the original
inode and its metadata because files are renamed rather than copied.

**Quarantine moves require the source and quarantine to be on the same filesystem.**
A cross-filesystem move is refused without removing the source. If Downloads is
on a separate disk, point `KITTY_LITTER_QUARANTINE` at a private location on that
disk for those items. A quarantine location selects that store's history; use the
same value when reopening it. Systems/filesystems without no-replace rename
support report an error; no unsafe overwrite/copy fallback is used.

Invalid manifests, missing payloads and interrupted records are reported as
skipped entries rather than deleted automatically. If a payload was altered after
quarantine, Restore and Release refuse it. Recover such files manually from the
entry's `payload`; the `record` stores the original path. If a rare concurrent
source replacement prevents rollback, the moved object remains under its entry
for manual recovery. The app does not promise transactional recovery across every
filesystem/power-loss failure; keep backups of important data.

Quarantine on the same filesystem does not free space until Release Permanently.
There is no automatic quarantine retention/deletion timer.

## Tests and validation

```sh
make test
ASAN_OPTIONS=detect_leaks=0 make test CFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer'
```

Tests use an isolated temporary tree with sparse fixture files, never personal
files. Passed: scan ages/sizes/extensions; hidden-file and symlink exclusion;
quarantine/restore/release; no-overwrite restore (including dangling symlinks);
changed-source rejection; duplicate roots; private storage; incomplete records;
recreated restore parents; symlink parent replacement; and changed-payload refusal.
The core compiles with `-Werror`. These checks also pass under AddressSanitizer
and UBSan with leak detection disabled because this environment blocks it.

The full GTK/Ayatana program has **not been compiled or visually tested** because
those development headers were unavailable. ARM64 execution is also unverified.
Before using real files, build on the target desktop and exercise scan → selected
quarantine → restore, conflict handling, and confirmed permanent deletion against
a disposable test tree. Closing the window should leave the tray active; Quit
should be blocked while a file operation is running.
