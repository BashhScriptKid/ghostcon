# ghostcon

A modern terminal system for UNIX TTY virtual terminals, matching modern
terminal emulator functionality without additional complexity. Not a
multiplexer; just TTYs that don't fall behind.

ghostcon replaces `fbcon`/`agetty` on the Linux virtual consoles: it owns
DRM/KMS directly, renders through EGL/GLES, and runs a Ghostty-derived
terminal engine, with a multiprocess supervisor so a crashed renderer never
takes the console down with it.

## Components

| Binary | Role |
|---|---|
| `undead-head` | Single supervisor process; owns the configured VT list |
| `supervisor` | Per-VT canary watchdog; falls back to kmscon/agetty |
| `ghost-ptyserv` / `pty-ttyN` | Long-lived PTY pair, survives renderer restarts |
| `ghostcon-core` | Per-VT renderer event loop (KMS/EGL/GLES + terminal) |

## Requirements

- Linux with DRM/KMS (`/dev/dri/cardN`) and a free virtual console
- A C11 toolchain, plus `meson`, `ninja`, and `pkgconf`
- Python 3 (for the `tools/format_*.py` style checks only)

Libraries (Arch package names in parentheses):

- `libghostty-vt` >= 0.1.0 (`ghostty`) — vendored terminal parser API
- `libdrm` (`libdrm`)
- `libgbm` (`mesa`)
- `libEGL`, `libGLESv2` (`libglvnd` + a Mesa driver)
- `libinput` (`libinput`), `libudev` (`systemd-libs`)
- `libxkbcommon` (`libxkbcommon`)
- `libfontconfig` (`fontconfig`), `libfreetype` (`freetype2`), `libharfbuzz` (`harfbuzz`)
- `libpng` (`libpng`)
- `libsystemd` (`systemd-libs`, optional — journald crash reporting)

`libghostty-vt` is the only non-optional runtime dependency that is not part
of a base system; everything else can be absent at configure time and the
corresponding feature is compiled out. `tomlc99` is a meson subproject and
needs no system package.

## Build

```sh
meson setup build
ninja -C build
```

## Test

The default suite is fully headless — no display, VT, or DRM master needed
(`test_render` renders through a DRM render node, and SKIPs if none is present):

```sh
meson test -C build
```

For the sanitizer build:

```sh
meson setup build-san -Db_sanitize=address,undefined
ninja -C build-san
meson test -C build-san
```

### Coverage

Meson's built-in gcov support works with the plain GCC build; run the tests
first, then generate a report (needs `gcovr` or `lcov`):

```sh
meson setup build-cov -Db_coverage=true
ninja -C build-cov && meson test -C build-cov
ninja -C build-cov coverage
```

### Fuzzing

`tools/fuzz_term.c` feeds arbitrary byte streams into the VT parser. libFuzzer
is a clang feature, so it is opt-in and needs clang:

```sh
CC=clang meson setup build-fuzz -Dbuild_fuzzers=true \
    -Db_sanitize=address,undefined -Db_lundef=false
ninja -C build-fuzz src/fuzz_term
./build-fuzz/src/fuzz_term -max_len=4096 -max_total_time=120
```

With `-Dbuild_fuzzers=true` the terminal core is compiled with libFuzzer
coverage instrumentation, so the harness explores the parser rather than just
its own file. Append a corpus directory to persist interesting inputs across
runs; `tools/corpus/*.seq` are reasonable seeds.

## Style checks

Formatting is enforced by three idempotent tools (not clang-format); see
`docs/RUNBOOK.md` section 0 for the rules:

```sh
python3 tools/format_comments.py   --check
python3 tools/format_braces.py     --check
python3 tools/format_signatures.py --check
```

Add `--apply` to rewrite in place.

## Install

```sh
sudo meson install -C build
sudo systemctl enable --now ghostcon.service
```

The systemd unit is not a per-VT template: `undead-head` takes the VT list on
its command line. Before handing ghostcon a VT, mask the services that would
otherwise claim it, e.g. `systemctl mask getty@tty4.service`. The unit install
is skippable with `-D install_systemd_unit=false`.

Deploying to a live VT **takes over that console's output**; see
`docs/RUNBOOK.md` sections 5 and 6 for safe hardware testing and the offline
inspectors (`gc-inspect`, `headless_probe`) to use instead when possible.

## Documentation

- `docs/RUNBOOK.md` — where things live, plus recipes for config options,
  keybindings, escape sequences, and offline debugging
- `docs/REFACTOR_PLAN.md` — architecture rationale and the module boundaries
- `IMPLEMENTATION.md` — Ghostty porting strategy, data structures, build deps
- `PLAN.md` — the original project plan and supervision design
- `share/ghostcon.toml.example` — every config key with defaults
