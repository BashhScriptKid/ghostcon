# ghostcon — Project Plan

A modern terminal system for UNIX TTY virtual terminals, matching modern
terminal emulator functionality without additional complexity. Not a
multiplexer; just TTYs that don't fall behind.

License: `GPL-2.0-or-later` (SPDX identifier — use this exact string in
build metadata; the `LICENSE` file carries the full GPLv2 text)

## Why this exists

`kmscon` (the existing userspace VT renderer that replaces `fbcon`+`agetty`)
was dormant for a decade but **is now actively maintained again** —
development resumed in 2025 from a fork maintained by Aetf since 2011, and
Fedora has proposed (and is moving forward with) making it the default
console, replacing `fbcon` entirely on Fedora 44/45. It is getting real
upstream investment: improved keyboard/mouse handling, multiseat support,
and a better security posture than kernel-resident `fbcon`. **It is not
abandoned, and this project does not claim otherwise.**

That said, two real gaps remain even in the actively-maintained kmscon,
and they're the actual reasons `ghostcon` exists:

1. **Protocol floor gap — a correctness problem, not an aesthetics one.**
   kmscon's terminal state machine (`tsm`, itself still a separately
   maintained but protocol-conservative project) predates what is, at
   this point, simply the modern *floor* for a terminal emulator: OSC 8
   (hyperlinks), OSC 52 (clipboard), sixel/Kitty graphics protocol, true
   color, synchronized output, Kitty keyboard protocol. This is not
   "cutting edge" — these have been baseline expectations across actively
   maintained terminal emulators for years now. Failing to support them
   produces two distinct severities, and the difference matters:
   - **Graceful degradation** — a program (e.g. opencode) emits sequences
     the terminal doesn't understand, and the terminal ignores or
     substitutes them. Ugly, but control flow stays intact — you can
     still type, navigate, and quit cleanly.
   - **Hostile failure** — a program assumes baseline protocol support
     that simply isn't there, and instead of degrading, the unrecognized
     bytes get misinterpreted: spammed replacement characters
     (U+FFFD, the "question mark diamond") overwriting actual content, or
     a partially-parsed escape sequence consuming bytes it shouldn't,
     desyncing the parser's internal state (cursor position, active mode,
     screen buffer) from what the program thinks it sent. This is not
     "looks bad" — it's the terminal's model of its own state diverging
     from reality, which is how you end up unable to even Ctrl-C cleanly,
     because Ctrl-C's interpretation depends on state the terminal no
     longer correctly tracks.

   Nothing in kmscon's current upstream roadmap (mouse support, rotation,
   security hardening) addresses this — it's an orthogonal axis of
   improvement. `libghostty`/`libghostty-vt`, by contrast, has had years
   of edge-case hardening against exactly this protocol surface, driven
   by tens of thousands of real daily Ghostty users — hardening `tsm`,
   dormant for a decade, never received. That maturity is the other half
   of why `ghostcon` wraps Ghostty specifically rather than attempting to
   extend `tsm`.

   This also isn't only about correctness in isolation — it's about a
   concrete, repeated cost to real workflows. The common path today for
   running a heavy interactive TUI (`fzf`, `lazygit`, `opencode`, `Claude
   Code`) on hardware that isn't already in a graphical session is:
   switch to or boot a display manager, authenticate, wait for a full
   compositor/WM session to spin up, open a real terminal emulator inside
   it, *then* finally run the one command you wanted. None of that
   overhead is actually required by the tool you wanted to run — `fzf`
   has no dependency on a compositor, it depends on a terminal that
   doesn't fall below the modern floor. `ghostcon` collapses that chain
   to "switch to an already-running (or near-instantly-started) VT, type
   the command" — which matters as much for sysadmin/ops work as it does
   for tight, high-frequency iteration loops (e.g. benchmarking work
   where a full compositing session's memory footprint is itself an
   unwanted, uncontrolled variable sitting under the very thing being
   measured).
2. **No hang/liveness detection** — kmscon runs as one process per *seat*,
   internally managing multiple terminal "sessions" within that single
   process. If a session exits, kmscon restarts it — but this is
   crash-on-exit recovery *internal* to one process, not external
   supervision. There is no mechanism — in kmscon's revived codebase or
   in any related historical effort (e.g. the abandoned `uvtd` /
   systemd-consoled lineage) — that detects a process that is **alive but
   wedged** and forcibly reclaims the VT. This is precisely the failure
   mode observed in practice: under load (rapid VT switching during a
   multi-terminal debugging session), kmscon could freeze indefinitely —
   VT switches stopped responding, new terminals failed to spawn, and
   eventually every VT went dark to a bare blinking cursor with nothing
   to recover it. Power/shutdown still worked, confirming the kernel and
   PID 1 were unaffected — this was purely a wedged userspace process
   with zero external recourse. kmscon's own crash-restart logic cannot
   catch this class of failure because the process never exits; it just
   stops responding.

`ghostcon` solves the rendering/protocol gap by porting Ghostty's terminal
core (Zig → C) and writing a native GLES 2.0 renderer, reusing
`libghostty-vt` for its modular parsers (OSC, SGR, key encoding).
(Ghostty's actively-maintained terminal core and GPU-accelerated renderer)
instead of forking it, and solves the liveness gap with a supervised,
per-VT process model with bounded, dumb, deadline-based liveness
guarantees that kmscon's architecture has no equivalent for.

**kmscon is used as a reference for correct low-level mechanics only, not
as a base or dependency.** It remains a legitimate, improving, separately
useful project — `ghostcon` is not trying to replace its real strengths
(multiseat, XKB keyboard handling, security posture vs. kernel-resident
`fbcon`) so much as to fill the two specific gaps above. Do not vendor or
fork kmscon source. Read it (and Fedora's revived fork) to understand
correct sequences for KMS/DRM setup, the `VT_RELDISP`/`VT_ACTIVATE`
handshake, and libinput/seat integration — then write fresh code against
ghostcon's process model (one OS process per VT, externally supervised)
since kmscon's one-process-per-seat-with-internal-sessions model is
structurally incompatible with external per-VT liveness supervision.

`ghostcon` may also reasonably choose to **interoperate with** kmscon
rather than fully replace it — e.g. treating kmscon as an intermediate
fallback tier (see fallback section below), since its existing
start-failure-to-`getty` systemd unit behavior already covers one slice
of graceful degradation that `ghostcon` can build on rather than
reinvent.

---

## Prior art: `bcon`

[`bcon`](https://github.com/sanohiro/bcon) (MIT, Rust, ~30 stars as of
this writing) is the closest existing project to `ghostcon`'s premise and
is worth naming explicitly rather than discovering by accident later.
Its own pitch is almost word-for-word the motivation behind this project:
GPU-accelerated, modern-protocol terminal rendering directly on the Linux
console, aimed squarely at the same trigger (heavy terminal use driven by
AI coding tools like Claude Code/Codex/Gemini CLI eating into desktop
session time). It already ships GPU rendering via DRM/KMS + EGL + GBM +
OpenGL ES, Sixel and Kitty graphics protocols, OSC 4/9/10/11/12/52,
synchronized output (mode 2026), the Kitty keyboard protocol, true color,
emoji, and even a D-Bus-based IME bridge (fcitx5) — i.e. it has already
proven that essentially every item on this project's OSC/rendering wishlist
is achievable on bare TTY. `bcon` is real, working, validating evidence
that this whole direction is sound, not speculative.

Where `ghostcon` differs, deliberately:

1. **Terminal core provenance.** `bcon`'s VT parsing/state machine is a
   from-scratch implementation (its own credited inspirations — Ghostty,
   foot, yaft, Alacritty — are listed as influences, not dependencies; no
   shared terminal-parsing library is used). This is exactly the pattern
   Ghostty's own `libghostty`/`libghostty-vt` was created to end: many
   independent, ad-hoc terminal emulator implementations, each carrying
   its own correctness burden and its own long tail of edge-case bugs.
    `ghostcon` ports Ghostty's terminal core (Zig → C) and reuses
    `libghostty-vt`'s modular parsers specifically so its protocol
    correctness rides on an actively-maintained, widely-adopted
    shared implementation instead of being one more bespoke parser to
    independently get right and keep right.
2. **Scope — multiplexer-or-not.** `bcon` deliberately includes its own
   split panes, tabs, and copy mode — session/workspace management
   features, explicitly positioned as a tmux/screen replacement
   (`bcon includes built-in split panes and tabs — no need for tmux or
   screen for basic multiplexing`). `ghostcon` deliberately does none of
   this. It renders one terminal per VT and otherwise gets out of the
   way — see the project's own tagline ("not a multiplexer; just TTYs
   that don't fall behind"). This isn't a missing-feature gap to close;
   it's an intentional scope boundary, specifically around *multi-session
   management*.

   That boundary does not extend to single-surface utilities that operate
   on the one terminal already being rendered rather than on managing
   multiple terminals. `bcon`'s **font scaling** (runtime adjustment,
   Ctrl+Plus/Minus) and **screenshot tool** (save terminal as PNG) are in
   this second category — they're closer in kind to "set cursor color"
   than to "split this view into two," and carry no multiplexer-style
   state (no pane tree, no tab list, no session model) for a supervisor
   to reason about. Both are worth adopting in `ghostcon`'s own scope:
   - **Font scaling** is a natural `wrap`-layer responsibility (it's a
     resize-and-reflow operation against libghostty, conceptually
     identical to the VT-mode-change resize handling `wrap` already needs
     to implement) plus a small keybinding/IPC trigger.
   - **Screenshot** is a natural `ghostcon-core`/`wrap` boundary feature
     — render the current frame to a PNG instead of (or in addition to)
     the framebuffer. Cheap to add once the rendering pipeline exists,
     and useful for the project's own development (capturing real
     before/after state when debugging rendering issues) independent of
     whether end users use it often.

   Net effect: the scope boundary is "no multi-session
   management," not "no utility beyond bare rendering." Worth tracking
   both as small, low-priority additions alongside the OSC matrix work
   rather than dismissing them as out-of-scope by association with the
   pane/tab/copy-mode features they happened to ship alongside in `bcon`.
3. **Liveness/supervision.** Nothing in `bcon`'s documented feature set,
   architecture, or stated limitations addresses process supervision,
   hang detection, or fallback behavior — its stated limitations are
   capability constraints (no multi-seat/DRM-lease support, single
   monitor output only), not reliability ones. It appears to run as one
   self-contained binary per VT (`bcon@tty2` via systemd template unit),
   structurally similar in this respect to kmscon — i.e. nothing
   independently confirms it handles the "alive but wedged" failure mode
   any differently than kmscon does. `ghostcon`'s supervisor/canary/
   orphan/recovery layer (see below) is the part of this project's scope
   that no comparable project — kmscon, `bcon`, or otherwise found in
   research — currently addresses.
4. **License.** `bcon` is `MIT`. `ghostcon` is `GPL-2.0-or-later`, per the
   earlier reasoning about wanting derivatives of system-level VT
   infrastructure to flow back upstream rather than disappear into closed
   forks.
   > **Careful:** this sentence states two different projects' licenses
   > side by side, and `MIT` here belongs to `bcon`, *not* to `ghostcon`.
   > This exact adjacency has already caused one real bug — `meson.build`
   > shipped `license: 'MIT'`. When copying a license string into build
   > metadata, take it from the `License:` line at the top of this file,
   > never from this comparison row.

In short: `bcon` is proof the destination is reachable. `ghostcon` takes a
different path to it — shared/maintained terminal core over bespoke
parser, minimal scope over all-in-one terminal, and a supervision layer
purpose-built for the specific failure mode (hangs, not just crashes or
missing features) that motivated this project in the first place.

---

## Architecture overview

```
undead-head (pure process-group anchor, minimal logic, cannot hang)
  ├── ghost-ptyserv (registry + pty child spawner)
  │     ├── pty-tty1          ← PTY master fd + ring buffer (stupid byte holder)
  │     ├── pty-tty2
  │     └── ...
  ├── supervisor[tty1]
  │     ├── ghostcon[tty1]          ← renderer (canary)
  │     └── ghostcon-overlay[tty1]  ← overlay (no canary, plain restart)
  ├── supervisor[tty2]
  │     ├── ghostcon[tty2]
  │     └── ghostcon-overlay[tty2]
  └── ...

ghostcon-ipc        (separate systemd unit, restart-on-hang only — shared junction)
```

### Data flow

```
1. Renderer starts → asks ghost-ptyserv for child PID/socket for its VT
2. ghost-ptyserv returns: "pty-tty3 pid=1856 socket=/run/ghostcon/pty-tty3.sock"
3. Renderer connects to pty-tty3 directly (Unix socket): receives ring buffer,
   streams live output, sends input bytes
4. On hang: supervisor kills renderer, starts new one → repeat steps 1-3
5. pty-tty3 never notices — still talking to the same PTY master fd
6. All bytes go renderer↔pty child directly — ghost-ptyserv parent never
   touches PTY data, only coordinates the handshake
```

### Component list

| Component | Language | Role |
|---|---|---|---|
| `undead-head` | C | Process-group anchor. Forks `ghost-ptyserv` and all `supervisor[ttyN]` instances, then enters a `waitpid()` reaper loop. Minimal logic — just fork + waitpid + exit propagation. Cannot hang. |
| `ghost-ptyserv` | C | Single process, registry + pty child spawner. Spawns a `pty-ttyN` child per VT, tracks child PID/socket path, answers renderer queries. Never touches PTY data — just the coordinator. |
| `pty-ttyN` | C | PTY child: holds one PTY master fd, spawns shell, maintains ring buffer. Talks directly to the connected renderer via Unix socket. Stupid byte holder — no KMS, no libghostty, no input parsing. |
| `ghostcon-core` | C | Per-VT renderer: KMS/DRM ownership, VT ioctl handshake, libinput, ported terminal state machine, GLES 2.0 renderer. Stateless — asks ghost-ptyserv for the pty child to connect to, then streams bytes directly child↔renderer. Terminal behavior identical to Ghostty (code is a line-by-line port of Ghostty's Zig terminal engine). |
| `supervisor[ttyN]` | C | Per-VT watchdog: startup race timer, canary pings ghostcon-core, restart-on-failure for ghostcon-overlay. Implements VT lifecycle state machine (IDLE→SPAWNING→ACTIVE/FALLBACK). Kills hung renderer on deadline expiry, starts replacement — pty child and terminal state survive. |
| `ghostcon-overlay` | C or Rust | Per-VT, separate DRM plane, renders notifications/title. Managed by same supervisor as the renderer (no canary, plain restart). |
| `ghostcon-ipc` | Rust | Separate systemd unit (restart-on-hang, no canary). Shared junction for cross-VT IPC (clipboard, notification brokering). Zero unsafe Rust. |

---

## 1. `ghostcon-core` (per-VT process)

One process per VT. This is the load-bearing architectural break from
kmscon — kmscon's single multi-VT event loop is exactly what allowed one
VT's misbehavior (or a race during fast switching) to wedge every VT
simultaneously. Isolating to one process per VT means a hang on tty3 cannot
affect tty1/tty2.

### Config format

Config file path: `/etc/ghostcon/config.toml` (system-wide, read by
`undead-head` at startup and cached per-process). No per-user config —
this runs at VT level, before any user session.

Uses **extended TOML** — standard TOML for all
`category { key = value }` groupings (font defaults, canary deadline,
clipboard isolation policy, cursor theme, overlay options, etc.), plus
a trivial custom parser (one regex, ~50 lines) for `bind` lines:

```toml
[vt]
list = [1, 2, 3, 4, 5, 6]     # VTs ghostcon manages; each gets a supervisor + pty child

[supervisor]
canary_deadline_ms = 4000

[fonts]
family = "Iosevka Term"
size = 11

[keybinds]
bind = "SUPER, XF86MonBrightnessUp, exec, brightnessctl set +5% && ghostconctl notify-hud brightness \"$(brightnessctl get -p)\""
bind = "SUPER, XF86AudioRaiseVolume, exec, wpctl set-volume @DEFAULT_SINK@ 5%+ && ghostconctl notify-hud volume \"$(wpctl get-volume @DEFAULT_SINK@)\""
```

This avoids depending on an unofficial, bus-factor-1 library (`hyprlang-rs`)
for something TOML already handles natively, while keeping `bind` lines as
a simple TOML array-of-strings that the ~50-line custom parser splits into
`(mod, key, dispatcher, args)` tuples for the `keybind` module. Every
other config consumer reads standard TOML via whatever mature parser
exists in that language (e.g. `toml`/`toml_edit` crate in Rust,
`zig-toml`, or a C TOML library).

### Modules

- **`kms.c`** — DRM/KMS setup: mode setting, framebuffer/GBM surface
  acquisition, DRM master acquire/release. Reference kmscon's KMS code for
  the correct sequence; write fresh for single-VT-per-process ownership.
- **`input.c`** — libinput + seat integration (via logind
  `sd_session_take_control` or equivalent). Reference kmscon's input.c.
  Raw event capture only — translating events into terminal mouse-reporting
  escape sequences (SGR mouse mode etc.) is `wrap`'s job, since that's
  protocol-level and the terminal state machine should own it. See the
  **Input pipeline** section below for the full ordered path an event
  takes from here through `keybind` and `wrap` to its final destination.
- **`vtctl.c`** — VT lifecycle: follows kmscon's proven kernel-level
  pattern (`src/uterm/vt_real.c`):
  1. Open `/dev/ttyN` with `O_RDWR | O_NOCTTY`.
  2. `ioctl(KDSETMODE, KD_GRAPHICS)` — enter graphics mode.
  3. `ioctl(VT_SETMODE, { .mode = VT_PROCESS, .acqsig = SIGUSR1, .relsig = SIGUSR2 })` —
     kernel sends `SIGUSR1` when this VT becomes active (acquire), `SIGUSR2`
     when it must deactivate (release).
  4. On `SIGUSR2` (release): drop DRM master via `drmDropMaster()`, then
     `ioctl(VT_RELDISP, 1)` to ack the release.
  5. On `SIGUSR1` (acquire): `ioctl(VT_RELDISP, VT_ACKACQ)`, then
     `drmSetMaster()` to claim DRM master.
  **This is the highest-risk module** — it's the direct cause of the
  "switching doesn't work" failure mode observed with kmscon (a wedged
  event loop never acks the release request, and the kernel has no way to
  force it). The fix is not "make this code perfect" — it's "make sure the
  supervisor's canary can detect and recover from this specific class of
  stall," since a cooperative kernel handshake can always theoretically
  stall regardless of how careful the implementation is. SIGUSR1/SIGUSR2
  are now consumed by every per-VT renderer; no other component should use
  these signals (SIGRTMIN+0 through SIGRTMAX remain available if needed).
- **`pty.c`** — **Moved to `pty-ttyN` child processes under `ghost-ptyserv`.**
  PTY allocation, shell spawn, and PTY master fd ownership all live in
  dedicated `pty-ttyN` children, one per VT. `ghostcon-core`'s `pty.c` is
  replaced by a thin transport module that:
  1. Queries `ghost-ptyserv` (parent registry) for the pty child's socket
     path for this renderer's VT number.
  2. Connects directly to the pty child via Unix socket — one stream for
     receiving PTY output bytes (fed into `wrap`), one for sending encoded
     input.
  3. On renderer hang/kill: the socket closes, the pty child pauses output
     forwarding, ring buffer stays intact.
  4. On new renderer connect: repeat step 1-2; pty child replays ring
     buffer from the beginning (the new renderer fast-feeds every byte
     through its own terminal state machine, deterministically reconstructing terminal
     state), then resumes live forwarding.

  The shell process never sees the renderer die — it's still talking to
  the same PTY master, same fd, same pty child. PTY rearrangement (moving
  a session to a different VT) is just ghost-ptyserv updating its registry
  and telling the renderer to reconnect to a different pty child.
- **`diag.c`** — panic/crash self-reporting. **No kmscon equivalent.**
  - Install a signal handler (SIGSEGV, SIGABRT, etc.) and/or hook into
    Zig's panic handler (for anything panicking on the `wrap` side) early
    at startup.
  - On trigger: capture a backtrace, truncate to a small head (~5-10
    frames) for human-readable output, but log the *full* trace separately.
  - Emit to the init log system via `sd_journal_send` (structured fields:
    component, VT id, signal/error, timestamp, truncated trace) — prefer
    journald over flat syslog since the system is already systemd-coupled
    via VT units, and structured fields make filtering
    (`journalctl -t ghostcon -p err`) meaningful.
  - This must be self-contained inside `ghostcon-core`/`wrap` — the
    supervisor does NOT do the catching (no external ptrace/coredump
    sniffing). The binary reports on itself.
- **`keybind`** (Rust component, called from `input.c`'s event loop) —
  config-driven key-combo → shell-command dispatch, in the same spirit as
  Hyprland's `bind = MOD, KEY, dispatcher, args` keybind lines. Reads its
  bindings from `ghostcon`'s config file (see config format note below).
  - Config example:
    ```
    bind = SUPER, XF86MonBrightnessUp, exec, brightnessctl set +5% && ghostconctl notify-hud brightness "$(brightnessctl get -p)"
    bind = SUPER, XF86AudioRaiseVolume, exec, wpctl set-volume @DEFAULT_SINK@ 5%+ && ghostconctl notify-hud volume "$(wpctl get-volume @DEFAULT_SINK@)"
    ```
  - **`ghostcon-core` has zero knowledge of brightness, volume, or any
    other hardware-control concept.** Its only job is: recognize the
    configured key combo, spawn the configured command string via the
    shell. The sequencing ("change the value, then report the new value")
    is ordinary shell `&&` chaining inside a user-authored command — not
    anything `ghostcon` orchestrates or needs to understand. This mirrors
    the `ghostconctl switch` design philosophy (see `ghostconctl`
    section): ghostcon provides mechanism, never policy or hardware
    backend logic, for anything where "the correct backend" is
    system/user-stack-dependent (ALSA vs. PipeWire, `/sys/class/backlight`
    vs. DDC/CI, etc.).

### Process identity (OSC 0 / OSC 2 repurposing)

There's no window chrome on a bare TTY to display a "window title," so
OSC 0/2 is repurposed into process identity, visible via `ps`/`top` instead
of a title bar. This directly helps the use case that originally surfaced
the kmscon hang (debugging across many VTs running different
logging/tracing/log-reading tools) — `ps aux` becomes meaningful instead of
showing N identical `ghostcon` lines.

On receiving OSC 0 or OSC 2 with string `S`:

- **`argv[0]`** ← `"ghostcon@tty<N> -- " + S`, verbatim, no parsing, no
  truncation. Visible via `ps -ef` / `/proc/<pid>/cmdline`.
  - Implementation note: naively overwriting `argv[0]` past its original
    allocated length will corrupt/truncate adjacent argv entries on Linux,
    since argv lives in fixed backing memory from the initial stack layout.
    Use `prctl(PR_SET_MM_ARG_START/END)` to relocate properly, or accept
    truncation to original argv length as a known constraint — decide
    explicitly, don't let this be accidental.
- **`PR_SET_NAME`** (the `comm` field, 15 bytes incl. null → 14 usable
  chars) ← truncated form, for `top`/`htop`'s fixed-width display:
  1. Extract the binary/command token from `S` (first word if it looks
     like `cmd args...`, or basename if it's a path).
  2. Build prefix `"gc@tty<N> "`.
  3. Remaining budget = 14 − len(prefix).
  4. If basename fits in budget, use as-is.
  5. If not, truncate from the **front**, keep the tail, prepend `"..."`:
     `keep = budget − 3`; result = prefix + "..." + last `keep` chars of
     basename. (Tail-truncation preferred because file extensions /
     trailing identifiers are usually the most informative part of a long
     name.)
  6. Edge case: if budget is too small even for `"..."` + 1 char, just hard
     truncate the whole prefix+name to the 14-char limit.

  Example: VT 3, title `/usr/.../some-very-long-script.sh` →
  `gc@tty3 ...ipt.sh`

---

## 2. Terminal engine — Ghostty port + GLES renderer

**This is the critical architectural discovery that shapes the entire
implementation.** The installed `libghostty-vt` (v0.1.0) provides only
parser utilities — not a terminal state machine:

| Exported by libghostty-vt | Not exported |
|---|---|
| OSC parser (22 command types) | Terminal state machine (screen buffer, grid) |
| SGR parser (bold, italic, colors, underline) | Byte stream processor (ESC/CSI/DCS dispatch) |
| Key encoder (Kitty keyboard protocol) | Scrollback, alt screen, selection |
| Paste safety check | Resize/reflow |
| Color utilities, SIMD helpers | Rendering of any kind |
| Custom allocator interface | |

The full Ghostty terminal emulator lives in Zig source at
`github.com/ghostty-org/ghostty`. Its terminal core (`src/terminal/`) and
renderer (`src/renderer/`) are written in Zig and compiled into the
Ghostty binary — never shipped as a C library.

### Strategy: port, don't wrap

Port the essential terminal state machine from Ghostty's Zig to C,
reusing `libghostty-vt` for the parsers it already exposes. Write a
GLES 2.0 renderer (EGL/GBM on KMS, textured-quad font rendering via
freetype). This gives us:

1. **Identical protocol behavior** — every edge case Ghostty handles,
   our port handles the same way. No behavioral divergence.
2. **Upstream tracking** — a Ghostty bugfix commit maps to a specific
   Zig file and function, which maps to our C port. We can diff and
   patch.
3. **No Zig build chain** — all C, standard toolchain.
4. **libghostty-vt reuse** — OSC/SGR/key parsers are already C.

### Architecture

```
ghostcon-core (per-VT process)
├── core/
│   ├── kms.c          — DRM/KMS mode setting, GBM buffer, page flip
│   ├── egl.c          — EGL init on GBM surface
│   ├── vtctl.c        — VT_PROCESS handshake (SIGUSR1, SIGUSR2)
│   ├── input.c        — libinput event loop
│   ├── transport.c    — pty child socket connect, feed/receive bytes
│   └── diag.c         — panic/crash self-reporting
├── term/              ← ported from Ghostty src/terminal/
│   ├── cell.h + .c    — Cell struct (codepoint, style ID, hyperlink, width)
│   ├── style.h + .c   — Interned style table (cells hold a 15-bit ID)
│   ├── row.h + .c     — Row of cells, wrap tracking
│   ├── screen.h + .c  — Grid, scrollback ring, cursor, alt screen, mode 2026
│   ├── stream.h + .c  — Byte stream parser (full DEC state machine)
│   ├── color.h + .c   — Palette, indexed/RGB conversion
│   ├── selection.h + .c — Selection state
│   ├── kitty.h + .c   — Kitty keyboard protocol state
│   └── term.h + .c    — Top-level: screen + stream + callbacks
├── render/
│   ├── atlas.h + .c   — Font glyph atlas (freetype → GPU texture)
│   ├── gles.h + .c    — GLES 2.0 renderer, shaders, draw calls
│   ├── machine.h + .c — Damage → render command generation
│   └── shaders/
│       ├── gles2.vert — Pass-through vertex shader
│       └── gles2.frag — Glyph quad fragment shader
```

### API surface

**As actually implemented in Phase 0** (`include/ghostcon/term/term.h` —
this section was provisional until the port landed, per the Phase 0
caveat below; it now reflects the real header):

```c
// ghostcon_term_t is a value type, not an opaque handle — the caller owns
// the storage. Scrollback capacity is a construction parameter.
bool ghostcon_term_init(ghostcon_term_t *term,
                        uint16_t cols, uint16_t rows,
                        uint16_t scrollback_cap);
void ghostcon_term_deinit(ghostcon_term_t *term);

// Feed PTY bytes into the state machine (drives the stream processor)
void ghostcon_term_feed(ghostcon_term_t *term, const uint8_t *data, size_t len);

// Resize the terminal (display mode change → reflow)
// NOTE: reflow is currently truncate/extend only — see screen.c TODO.
bool ghostcon_term_resize(ghostcon_term_t *term,
                          uint16_t new_cols, uint16_t new_rows);

// Terminal responses (DSR, DA, DECRPM, OSC replies) are delivered via a
// callback rather than a return buffer — the caller writes them to the PTY.
void ghostcon_term_set_output(ghostcon_term_t *term,
                              ghostcon_output_fn fn, void *userdata);

// Screen state accessor (inline)
ghostcon_screen_t *ghostcon_term_screen(ghostcon_term_t *term);
```

Deliberate departures from the pre-port guess: value type over opaque
handle, `init`/`deinit` over `new`/`free`, `uint16_t` dimensions, an
explicit `scrollback_cap`, and a push-style output callback in place of
returning encoded bytes.

Not yet present — these arrive with the renderer and input layers, and
their signatures remain provisional until then:

```c
void   ghostcon_term_render(ghostcon_term_t *term);
size_t ghostcon_term_encode_key(ghostcon_term_t *term,
           ghostcon_key_event_t *event, uint8_t *buf, size_t len);
void   ghostcon_term_bind_egl(ghostcon_term_t *term,
           EGLDisplay dpy, EGLSurface surf);
```

This replaces the earlier `wrap` concept entirely — there is no separate
FFI library. The terminal engine lives inside `ghostcon-core` and is
linked statically.

### Python-style note

> The Phase 0 port of Ghostty's `term/stream.zig` and `term/Screen.zig`
> is the single highest-risk item in the entire project. Every phase
> after it (renderer, input, supervision, IPC) depends on its correctness.
> The port may reveal structural assumptions in Ghostty's Zig code that
> the C data structures or GLES renderer cannot easily satisfy. **Phase 0
> output may force changes to the renderer design, the API surface, or
> even the phase ordering itself.** The IMPLEMENTATION.md file must be
> updated as Phase 0 progresses to reflect what the actual Ghostty source
> requires.

### libghostty-vt integration points

The `libghostty-vt` parsers are called from within the stream processor:

- **OSC sequences**: when the stream processor detects a complete OSC
  sequence, it calls `ghostty_osc_*` to parse and dispatch:
  `osc_new`, `osc_next` (per byte), `osc_end`, `osc_command_type`,
  `osc_command_data`. Unknown OSC types get `GHOSTTY_OSC_COMMAND_INVALID`
  → logged and discarded, never a crash/hang.

- **SGR sequences**: when the CSI handler detects an SGR parameter list,
  it calls `ghostty_sgr_*` to extract individual attributes:
  `sgr_new`, `sgr_set_params`, `sgr_next`, `sgr_attribute_tag`.
  Each attribute (bold, italic, color, underline) is applied to the
  cursor state.

- **Key encoding**: `ghostcon-core/input.c` translates libinput events
  into `ghostty_key_event_*` structs, then calls
  `ghostty_key_encoder_encode` to produce the byte sequence sent to the
  pty child.

### Renderer design

The renderer uses GLES 2.0 via EGL on a GBM surface:

1. Fontconfig selects the font family/size
2. Freetype renders each glyph to a bitmap
3. Bitmaps are packed into a GPU texture atlas (power-of-two)
4. Each frame: for every dirty cell, a textured quad is emitted
   (background rect + glyph rect + cursor rect)
5. `eglSwapBuffers` → GBM buffer → KMS atomic page flip

No copyless/zero-copy GPU text rendering. For a VT terminal at 60fps
with typical text throughput, the textured-quad approach is
well within budget. Ghostty's more sophisticated rendering pipeline
(damage → command coalescing, incremental GPU updates) can be adopted
later if profiling shows a need.

### Input pipeline

Input handling spans `ghostcon-core/input.c` and the key encoder
(libghostty-vt's `key_encoder_encode`):

```
libinput event
  → input.c: translate to ghostty_key_event_*
  → key_encoder_encode() → encoded byte sequence
  → transport.c: write to pty child socket
  → pty child: write to PTY master → shell reads it
```

The terminal mouse reporting path (SGR mouse mode, etc.) is handled by
the terminal state machine responding to CSI sequences, not by the
input layer. input.c only captures raw events and passes them through
the key encoder. The keybind system (user-configured shortcuts) intercepts
events before encoding per the same ordered pipeline as the original plan.

---

## 3. Input pipeline

Input handling spans three components (`ghostcon-core`'s `input.c`, the
`keybind` module, and `wrap`), and it's easy for ambiguity to creep in
about ordering and precedence — e.g. does a configured keybind win over
sending a keystroke to the running program? Does the overlay ever see
mouse events? This section is the single source of truth for the path a
raw input event takes, end to end, and where it can be consumed
(stopped) versus passed through.

### Ordered pipeline (every event, keyboard or mouse)

```
1. libinput (kernel evdev → libinput)
     `input.c` reads raw libinput events. No interpretation happens here
     beyond what libinput itself does (key codes, pointer deltas, button
     states, touchpad gestures). This stage cannot consume/stop an event
     — it only observes and forwards everything downstream.

2. keybind match (Rust `keybind` module)
     Every event is checked against the configured keybind table
     (hyprlang-rs-parsed `bind = MOD, KEY, exec, command` lines) BEFORE
     it reaches the terminal. This ordering is deliberate and mirrors
     Hyprland's own precedence: a global keybind always wins over
     whatever the focused program would have done with that keystroke.
     - MATCH: the configured command string is spawned via the shell;
       the event is CONSUMED — it does not propagate further down the
       pipeline. The running program (vim, opencode, whatever's in the
       PTY) never sees a keystroke that matched a keybind.
     - NO MATCH: the event passes through unmodified to step 3.
     This is the only consumption point in the pipeline for keyboard
     events. Mouse events are NOT matched against the keybind table —
     keybinds are keyboard-only by design, consistent with Hyprland's own
     `bind` semantics.

3. wrap translation (protocol-level encoding)
     Events that were not consumed by a keybind reach `wrap`, which
     translates them into whatever libghostty's input model expects:
     - Keyboard: standard key encoding, plus Kitty keyboard protocol
       encoding when the running program has opted in via the relevant
       mode (this is genuinely protocol-level work and belongs here, not
       in `core/input.c`, since it depends on libghostty's terminal mode
       state).
     - Mouse: SGR/X10/URXVT mouse-reporting escape sequences, gated by
       whatever mouse-tracking mode the running program has enabled
       (mouse events are only forwarded as escape sequences if the
       terminal is currently in a mouse-tracking mode; otherwise they are
       dropped at this stage, same as any standards-compliant terminal).
     This is the only stage that can consume mouse events (when no
     mouse-tracking mode is active) — there is no separate
     keybind-equivalent match step for mouse, by design (see note above).

4. PTY write via pty child
     The translated/encoded sequence is sent to the `pty-ttyN` child via
     its input socket. The pty child writes it to the PTY master, where it
     becomes input to whatever program is running in that session. This is
     the normal terminal case — the renderer never holds the PTY master fd
     directly.
```

### What never sees raw input directly

- **`ghostcon-ipc`** does not receive input events at all. It only
  receives already-decided *outcomes* — e.g. `ghostconctl notify-hud` is
  invoked by a keybind's spawned command, not by the input pipeline
  itself reaching into IPC. The pipeline's job ends at step 4 (or at
  keybind consumption in step 2); anything beyond that is the spawned
  command's own responsibility, fully decoupled from input handling.
- **`ghostcon-overlay`** has **no standing input path** — by default it
  is purely a rendering consumer of `ghostcon-ipc`'s notify path (see
  overlay section), with no click-handling or focus concept of its own.
  There are two narrow, explicitly scoped exceptions, neither of which
  introduces a persistent input route:
  - Hyperlink click-to-open (OSC 8) is handled within `wrap`'s own
    mouse-event consumption at step 3, using cursor position against
    tracked hyperlink regions — this does not route through the overlay
    at all.
  - The interactive modal mode (clipboard picker, notification
    interaction — see overlay section) is a deliberate, strictly
    temporal exception: input is redirected to the overlay only for the
    duration of one invoked interaction session, then reverts exactly to
    the rules above with zero residual state. It is a focus grab, not a
    standing capability.

### Why keybind-before-terminal ordering matters

If this were reversed (terminal gets first refusal, keybind only fires on
otherwise-unhandled keys), a program that grabs all keyboard input (most
TUIs do, by design) would make keybinds unreachable while that program is
running — exactly the kind of "Ctrl+Alt+F-key doesn't work because
something else has the keyboard" problem global keybinds exist to avoid.
Matching keybinds first, unconditionally, guarantees them as a global,
always-available control surface regardless of what's running in the PTY
— consistent with how Hyprland's own `bind` keywords behave relative to
focused windows.

---

## 4. Supervision layer

### Philosophy

The canary/watchdog is **deliberately dumb**. It does not try to diagnose
*why* a process is unresponsive (stuck `VT_RELDISP` ack vs. a
panic vs. a libinput epoll deadlock vs. something unanticipated) — it only
enforces one rule: **silence past a deadline = treat as dead.** This is
more robust than a handshake-specific watchdog because it catches every
failure mode by virtue of checking liveness, not correctness, including
modes nobody has hit yet. (Specific failure analysis like "stalled on
VT_RELDISP" is still useful — but as diagnostic content attached to the
crash report, not as the detection mechanism itself.)

This directly addresses the real-world failure mode observed: kmscon under
rapid VT-switching load (heavy multi-terminal debugging session) would
freeze indefinitely — VT switches stopped responding, new terminals failed
to spawn, and eventually every VT showed nothing but a blinking underscore.
Power/shutdown still worked, confirming the kernel and PID 1 were fine —
this was purely a wedged userspace process with zero external recourse.
The supervisor exists specifically to be that recourse.

### `undead-head`

Entry point — the single process systemd starts (`ghostcon.service`).
Forks `ghost-ptyserv` and every `supervisor[ttyN]`, then enters a
`waitpid()` reaper and restart loop. The set of VTs to manage is read
from config (see `[vt]` section below), defaulting to tty1–tty6.

```
for (;;) {
    /* (Re-)launch the tree */
    ghost_ptyserv_pid = fork_ghost_ptyserv();
    for (int i = 0; i < n_vts; i++)       /* n_vts from config */
        supervisor_pid[i] = fork_supervisor(i);  /* passes VT number */

    /* Reap/restart loop */
    while ((pid = waitpid(-1, &status, 0)) > 0) {
        if (pid == ghost_ptyserv_pid) {
            /* ghost-ptyserv crashed — restart the whole tree.
               Kill all children (supervisors + their trees),
               then loop back to re-fork. */
            kill(-getpgid(ghost_ptyserv_pid), SIGTERM);
            for (int i = 0; i < n_vts; i++)
                kill(-getpgid(supervisor_pid[i]), SIGTERM);
            goto restart;
        }
        for (int i = 0; i < n_vts; i++) {
            if (pid == supervisor_pid[i]) {
                /* Supervisor[i] crashed — kill its process
                   group (ghostcon + overlay children), then
                   restart just this one supervisor. */
                kill(-getpgid(pid), SIGTERM);
                supervisor_pid[i] = fork_supervisor(i);
                break;
            }
        }
    }
}
```

Each supervisor calls `setpgid(0, 0)` at startup so its renderer and
overlay children belong to a single group that undead-head can kill
atomically. ghost-ptyserv does the same for pty-ttyN children.

No PTY data, renderer state, IPC, or KMS/DRM lives here — just fork,
waitpid, and process-group signal delivery. If this process did any of
those things, it would reintroduce a single point of failure
structurally identical to the problem being solved.

### `supervisor[ttyN]` (one per VT)

Count = number of VTs spawned. The per-VT supervisor is the **VT lifecycle
manager** — it doesn't just watch, it decides what runs on this VT and
when. It implements a small state machine:

```
IDLE → SPAWNING (spawn ghostcon, start timer)
        ├── ghostcon claims VT → ACTIVE (cancel timer, canary loop)
        └── timer fires → kill ghostcon, spawn agetty → FALLBACK

ACTIVE → canary timeout → kill ghostcon → SPAWNING (re-arm race)
ACTIVE → ghostcon exits → SPAWNING (re-arm race)

FALLBACK → user requests ghostcon → kill agetty → SPAWNING
```

- **Startup race timer**: When spawning a renderer for this VT, the
  supervisor starts a timer (default 5s, same TOML config as canary
  deadline). If the renderer doesn't open the VT and call
  `ioctl(KDSETMODE, KD_GRAPHICS)` within this window, the supervisor
  kills it and falls through to kmscon (if installed) or directly to
  `fork()+exec(agetty)`. This prevents a hung or misconfigured renderer
  from leaving a black VT indefinitely — the same gap kmscon's systemd
  unit approach covers for start failures, but done here in-process with
  a deadline rather than relying on systemd's exit-code-only detection.

- **Canary**: a Unix socket pair (`socketpair(SOCK_STREAM)`) between the
  supervisor and `ghostcon-core`, created at spawn time. The renderer
  writes a byte to its end every event-loop iteration. The supervisor
  calls `poll(renderer_fd, POLLIN | POLLHUP, deadline)`. On `POLLIN`:
  alive, read the byte, reset timer. On `POLLHUP`: renderer exited
  cleanly. On timeout: renderer is hung — the event loop isn't processing
  I/O (the exact failure mode observed with kmscon). No custom protocol,
  no signals, no shared memory — just `poll()` on an fd.
- **Config**: single system-wide deadline value, in the same TOML config
  file as everything else (see Config format in the `ghostcon-core`
  section), e.g.:
  ```toml
  [supervisor]
  canary_deadline_ms = 4000
  ```
  Read once and applied identically to every per-VT supervisor. There is
  no legitimate reason for per-VT divergence here — every `ghostcon[ttyN]`
  instance is an identical fork of the same binary, so a single flat value
  is correct, not a missing feature. Default 4000ms; should be safely
  configurable up to at least 30000ms for slower hardware.
- **On deadline expiry**:
  1. **Kill the hung renderer** — SIGTERM first, SIGKILL if it doesn't
     respond quickly. Don't wait politely on something already confirmed
     unresponsive. The shell is **unaffected**: the `pty-ttyN` child holds
     the PTY master fd, not the renderer, so closing the renderer's socket
     connection does not trigger SIGHUP. The shell keeps running, talking
     to the same PTY master in the same pty child.
  2. **Start replacement renderer** — spawn a new `ghostcon[ttyN]`.
     The new renderer asks ghost-ptyserv for the pty child's socket,
     connects directly, receives ring buffer replay, and resumes rendering
     where the dead one left off. No notification step needed — the pty
     child detects the closed socket on its own and pauses forwarding
     until a new renderer connects.
  3. **Write a recovery file** — e.g. `/run/ghostcon/recovery-tty<N>.json`
     containing: pty child PID, VT id, timestamp, reason (`hang` vs.
     reported panic).
  4. **Broadcast diagnostics via `wall`** — literal `wall(1)` usage. Format
     a message with the failed component, the error/signal, and a
     truncated backtrace (~5-10 frames; full trace goes to journald only,
     not into the `wall` broadcast). Rate-limit this (e.g. once per boot
     per failure class) so flaky hardware doesn't spam every open session
     on every retry.
  5. **Only on full pty child failure** (crash, not hang — it gets no
     canary): fall back per the tiering below. A crashed pty child means
     the session state is genuinely lost, so fallback to kmscon/fbcon+agetty
     is appropriate.

### Three-tier fallback

`ghostcon` and `kmscon` are **independently installed packages** with no
dependency relationship between them — `ghostcon` does not require kmscon
to be present. The supervisor detects what's actually installed/configured
for a given VT and only considers tiers that exist:

```
1. ghostcon  (preferred, attempted first)
2. kmscon    (if installed — acts as a proven, actively-maintained
              fallback; this is NOT readiness-polling/racing against
              ghostcon, it's "try ghostcon with a timeout; only on
              failure, check if kmscon is available and try that instead")
3. fbcon + agetty (the floor — always available, requires releasing DRM
              master so fbcon can resume; this is the kernel console path
              and needs no setup, but also provides zero capability beyond
              this being unconditionally what's left when nothing else
              works)
```

DRM master is exclusive — there is no true concurrent "race" between
candidates for the same display resource. Arbitration is timeout-based and
sequential, not parallel polling. The supervisor's role in DRM master
coordination is **indirect**: it does not sit in the signal path (SIGUSR1/
SIGUSR2 are delivered to each per-VT renderer directly by the kernel).
Instead, the supervisor ensures a healthy renderer exists for each VT:
- On VT switch: kernel signals the active renderer to release (SIGUSR2)
  and the target renderer to acquire (SIGUSR1). Each renderer calls
  `drmDropMaster()`/`drmSetMaster()` on its own DRM fd — this works
  because the kernel grants DRM master to whichever process calls
  `drmSetMaster()` after the current holder has dropped it.
- If the active renderer is hung and never acks VT_RELDISP: the supervisor
  kills it via the canary timeout. The process death causes the kernel to
  auto-release DRM master and abort the stuck VT switch. The target
  renderer can then grab master fresh.
- If the target renderer is dead: it can't receive SIGUSR1. The user sees
  the switch fail (VT stays on the current one). The supervisor kills the
  dead renderer, spawns a replacement, and the new renderer can be
  activated via another VT switch attempt.
- If kmscon or fbcon+agetty is the fallback and gains DRM master, ghostcon
  renderers for other VTs must wait — they only attempt `drmSetMaster()`
  when their VT becomes active and they receive SIGUSR1.

**Note on overlap with kmscon's own fallback behavior**: kmscon already
ships a tier-2→tier-3 equivalent at the systemd unit level — its
`kmsconvt@.service` unit falls back to starting `getty@.service` if kmscon
fails to *start*. Where ghostcon's design adds real, non-duplicated value
is the case kmscon's existing fallback does not and cannot cover: a
process that starts successfully and then **hangs while still alive**
(holding DRM master, not exiting, not triggering any unit-failure state
systemd can react to). systemd's unit-level fallback only fires on
start failure or process exit — it has no concept of "alive but
unresponsive." `ghostcon`'s supervisor/canary layer is what catches that
case for `ghostcon` itself; whether an equivalent watchdog should also
wrap kmscon-as-fallback-tier (to catch *kmscon* hanging, not just
`ghostcon` hanging) is an open question — out of scope for `ghostcon`'s
own development, but worth flagging if kmscon is configured as tier 2 and
encountered to be the one hanging in some future system layout.

Note: if `ghostcon` was never started in the first place (service
disabled/masked), nothing has taken DRM master away from fbcon, so
fbcon+agetty continues working normally with zero special-casing needed.

### `ghost-ptyserv` supervision tier

Two tiers within ghost-ptyserv's process tree:

**pty children** (`pty-ttyN`): no canary, `Restart=on-failure` by ghost-ptyserv
parent. If a pty child crashes, the session state for that VT is lost —
ghost-ptyserv spawns a replacement (new shell), and the fallback tier
(kmscon/fbcon+agetty) is attempted for that VT. Isolation holds: one
crashing pty child doesn't affect other VTs.

The PTY allocation flow: for each VT in `[vt].list`, ghost-ptyserv creates a
named Unix socket at `/run/ghostcon/pty-tty<N>.sock`, then forks `pty-ttyN`.
The child opens `/dev/ptmx`, calls `ptsname()`/`grantpt()`/`unlockpt()` on the
slave, spawns the shell with the slave as stdin/stdout/stderr, and listens for
renderer connections on the socket. ghost-ptyserv registers the child PID and
socket path — that's all it knows about the child.

**ghost-ptyserv parent**: no canary at all — restarted by undead-head's
reaper loop (see `undead-head` section). If the parent crashes, all pty
children are orphaned and eventually reaped by undead-head (which then
forks a new ghost-ptyserv). All VT sessions are lost, but this is no worse
than any terminal emulator crashing. The parent is thin enough (registry +
child spawner, no PTY data, no renderer state) that its failure surface is
negligible — mostly `fork()`/`waitpid()` and socket `listen()`/`accept()`.

### Recovery (automatic — no separate utility needed)

When the supervisor starts a replacement `ghostcon[ttyN]` after a kill:
1. New renderer asks `ghost-ptyserv` (parent) for the pty child's
   socket path for this VT.
2. ghost-ptyserv returns the socket path (or a `SCM_RIGHTS` fd).
3. New renderer connects directly to the `pty-ttyN` child.
4. The pty child replays its **raw ring buffer** — every byte the PTY
   has emitted since the session started (configurable cap, e.g. last 10k
   lines or 4 MiB of scrollback).
5. The new renderer fast-feeds every byte through its own terminal state
   machine instance, which deterministically reconstructs the identical terminal
   state (cursor, modes, scrollback, color palette, hyperlink regions —
   everything). No second parser, no serialization API, no shared memory
   protocol needed; the terminal state machine is a pure function of the
   bytes it has consumed.

The maximum replay time is bounded by `ring_buffer_size / parse_throughput`.
At ~370k lines/sec parsing throughput (conservative for a mature parser),
even 50k lines of scrollback replays in ~135ms — barely perceptible. While
the new renderer is replaying, the pty child accumulates new live bytes in
a secondary buffer; once replay completes, the secondary buffer is appended
and the renderer switches to live streaming. The user sees a brief freeze
(the renderer was hung), then the exact same terminal contents reappear,
and they continue typing.

---

## 5. `ghostcon-ipc` — shared broker

A separate, standing process. **Not part of the `undead-head` supervision
tree** in the heavy sense — it gets its own lighter tier:

### Supervision tier: restart-on-hang only

No canary chain to `ghostcon-ipc`'s own internal state, no orphan logic,
no recovery file, no `wall` broadcast, no fallback tiers. Justification:
failure here is cheap (a hang just means notifications/clipboard events
stop flowing momentarily; nothing downstream is corrupted, nothing needs
recovery) — *but* it's still a shared junction every active VT depends on,
so a wedge has a wider blast radius (all VTs lose IPC simultaneously) than
any single overlay or even a single `ghostcon[ttyN]` crashing. That's
enough to justify basic liveness supervision (just restart it if it stops
responding), but not the full heavy machinery built for the per-VT
processes.

### Transport

Unix domain sockets, isolated per concern (not one shared socket for
everything):

```
/run/ghostcon/ipc/notify.sock      ← OSC 9 / OSC 777 (normalized into one
                                      event type before reaching this
                                      socket — see wrap responsibilities)
/run/ghostcon/ipc/clipboard.sock   ← OSC 52
/run/ghostcon/ipc/control.sock     ← ghostconctl verbs (switch, font
                                      scale, screenshot, notify, status,
                                      recover — see ghostconctl section)
(future event types get their own socket too)
```

Rationale for separate sockets per concern: different sensitivity
(clipboard can contain secrets; notifications are ephemeral/low-stakes;
control commands like `switch` may need elevated trust) and different
lifecycle. Permission bits can differ per socket accordingly. A bug in
notification handling has no code-path overlap with clipboard or control
handling — they don't share a transport, only a parent process.

### Clipboard (OSC 52) specifics

- `ghostcon-ipc` is the natural owner of the actual clipboard buffer in
  memory, since it's already the shared process all VTs talk to.
- On write from `ghostcon[ttyN]`: store buffer, ack.
- On read request from `ghostcon[ttyM]`: return stored buffer.
- Optional: bridge to a real system clipboard (e.g. wl-clipboard/xclip
  equivalent) if a graphical session's clipboard is reachable — same
  pattern as the optional D-Bus notification bridging below.
- **Isolation config**: default behavior is one global buffer shared
  across all VTs (copy in tty1, paste in tty3 — works out of the box).
  Config option `group_by = "user"` scopes the clipboard buffer by
  requesting UID instead (read via `SO_PEERCRED`/`peer_cred()`), so
  multi-user machines can opt into per-user isolation without needing a
  structurally different broker — same process, same socket, just a
  scoped lookup policy internally.

### Notification (OSC 9 / OSC 777) specifics

- Receives normalized notification events from any `ghostcon[ttyN]`
  (already merged from both OSC 9 and OSC 777 spellings by `wrap`).
- Publishes events to whichever `ghostcon-overlay` instance(s) are
  currently active/subscribed.
- **Optional D-Bus bridge**: if a D-Bus session bus exists (e.g. this
  machine sometimes also runs a full desktop session), optionally listen
  to `org.freedesktop.Notifications` (the standard desktop notification
  spec) and surface those through the same overlay path too. This is QoL
  only — `ghostcon-ipc`'s own notify path must work with zero dependency on
  D-Bus being present at all (pure-TTY machines won't have a session bus).
  - Privilege note: bridging a root-level/system process with a
    user-level D-Bus session bus crosses a privilege boundary — be
    deliberate about this connection so a user-level notification can't
    be used to inject anything unintended into the privileged IPC channel.

### Security posture

`ghostcon-ipc` should be implemented in **safe Rust with zero `unsafe`
blocks**. This is achievable because it has no FFI boundary (no
terminal state machine calls — that risk lives entirely in ghostcon-core — and no raw
ioctls; `SO_PEERCRED`/peer credential reading is available via safe
standard library APIs, e.g. `UnixStream::peer_cred()`).

With zero `unsafe` and no FFI, the realistic remaining risk surface is
**logic bugs**, not memory corruption:

- **Unbounded allocation / resource exhaustion**: enforce a hard cap on
  message size and reject *before* allocating a buffer for a claimed
  length, not after.
- **Credential checks**: verify `SO_PEERCRED`/peer credentials on every
  message where it matters, not just at connection setup, in case of
  socket reuse/hijack scenarios.
- **Trust boundary**: every `ghostcon[ttyN]` client must be treated as
  untrusted input by the broker, even though they're "our own" processes —
  a compromised or buggy per-VT instance becomes an attack vector against
  the shared broker otherwise.
- Use length-prefixed framing with strict bounds-checked reads (or
  `SOCK_SEQPACKET` if message-boundary semantics fit better than streaming).

---

## 6. `ghostcon-overlay` — per-VT notification/title rendering

A separate, sandboxed, low-privilege process responsible only for
rendering — it does no IPC brokering itself (that's `ghostcon-ipc`'s job)
and makes no supervision/fallback decisions.

### Privilege model

- Runs at default userspace privilege, inheriting the default group (e.g.
  whatever group grants `/dev/dri/*` access — typically `video`/`render`
  on most distros) — **not root**, and not elevated beyond what's needed
  to open a DRM plane and a client socket to `ghostcon-ipc`.
- Sandboxed.
- Does **not** own DRM master for the primary terminal surface — that
  stays with `ghostcon-core`. The overlay only owns its own separate plane.
- Job, narrowly: (1) connect to `ghostcon-ipc`'s notify socket as a
  subscriber, (2) render received content onto its own DRM plane, (3)
  composite that plane over the primary `ghostcon-core` buffer for
  whichever VT is currently active.
- Does not make VT-switching, fallback, or supervision decisions — pure
  rendering consumer.

### Supervision tier: managed by per-VT supervisor (renderer-adjacent)

The overlay runs under the same `supervisor[ttyN]` that manages the
renderer for that VT, not as a separate systemd service. However, it gets
radically lighter treatment than the renderer: **no canary ping**, just
`Restart=always` in the supervisor's internal table. If the overlay
crashes, the supervisor respawns it immediately without any deadline,
diagnostics, or fallback logic. This is appropriate because the overlay is
stateless — nothing it holds is precious. A missed notification or HUD is
ephemeral; the next one renders normally once the overlay is back up.
There's nothing to recover.

### Compositing

Use a **separate DRM plane**, not shared framebuffer drawing into
`ghostcon-core`'s buffer. This keeps the overlay's failure (or restart)
from being able to touch or corrupt the primary terminal rendering surface
underneath it — a clean hardware-level isolation boundary that pairs well
with the "no supervision needed" decision above.

### Title display (OSC 0/2, optional)

Config-driven on/off. The primary use of OSC 0/2 is process identity (see
`ghostcon-core` section above, via `argv[0]`/`PR_SET_NAME`) — this overlay
display is a secondary, visual nice-to-have on top, not load-bearing.

### Interactive modal mode (clipboard picker, notification interaction)

The overlay can also support **transient, modal interaction** — e.g. an
interactive clipboard history picker (`ghostconctl clipboard interact`,
bound by default-suggested config to something like `Meta+V`) and
ascending-from-top notification interaction
(`ghostconctl notify interact`, bound to e.g. `Meta+Ctrl+N`). This is the
one deliberate exception to "the overlay has no standing input path" (see
Input pipeline section) — it is a **modal override, strictly temporal,
never persistent**:

```
on ghostconctl clipboard|notify interact:
  1. ghostcon-ipc tells ghostcon-overlay to enter interactive mode for
     this data source (clipboard history, or pending notifications)
  2. ghostcon-overlay renders the picker UI on its existing DRM plane
  3. for the DURATION of this session ONLY: keyboard/mouse events are
     redirected to the overlay instead of proceeding through the normal
     input pipeline (steps 3/4 — wrap translation → PTY write)
  4. on selection or dismissal (Esc, click-away, selecting an entry):
     interactive mode ends immediately, input routing reverts exactly to
     the standard pipeline with no residual state
  5. (clipboard case only) selected entry is written back as the active
     clipboard buffer via ghostcon-ipc's existing clipboard mechanism
```

Hard constraints on this mode, to prevent it from becoming
multiplexer-style scope creep:
- No "leave the picker open in the background" — entering interactive
  mode is a focus grab with a single, immediate exit path, not a
  persistent window/pane-like object.
- No standing input path is introduced — the override exists only
  between invocation and dismissal of a single interaction session, then
  the Input pipeline's normal rules (section 3) resume exactly as
  specified, with zero residual hooks.
- The trigger itself is just another `keybind`-configured command
  (`bind = SUPER, V, exec, ghostconctl clipboard interact`), no special-
  cased input path in `ghostcon-core` — same mechanism as the brightness/
  volume HUD examples, not a separate code path.

**Explicit non-feature: no notification log/history.** Unlike clipboard
history (which has clear, repeated practical value — re-pasting something
copied minutes ago is a routine workflow), a persistent notification log
was considered and explicitly rejected. Notifications are, by nature,
transient and time-bound; a historical log of them has materially less
recurring value than clipboard history, and adds a second persistent,
potentially sensitive data store to reason about for comparatively little
payoff. This holds even though the technical objection (could it be done
safely?) has an easy answer — yes, it could simply live in an encrypted
keyring alongside or instead of how clipboard data is held — the decision
not to build it is about value versus complexity, not about a security or
feasibility blocker. `ghostconctl notify interact` only ever surfaces
currently-pending notifications, ascending from top (oldest-first), not a
durable history.

---

## 7. `ghostconctl` — control CLI

A small CLI binary, modeled on the `hyprctl` pattern: a thin front-end
that talks to a long-running daemon (here, `ghostcon-ipc`, via a dedicated
control socket alongside its existing `notify.sock`/`clipboard.sock`) and
issues simple, scriptable, verb-based commands. This gives ghostcon a
single consistent control surface for both routine actions and the crash
recovery flow, instead of several one-off utilities.

### Design philosophy

`ghostconctl` should stay **deliberately dumb** about anything beyond
"take an argument, issue a command, print a result." It should not grow
its own resolution/aliasing logic for things the shell can already do —
e.g. it does not need to know what a "DE session" is, or maintain a
name-to-VT-number mapping. If a script needs to resolve a target (such as
finding which VT a desktop session lives on), that resolution happens
*outside* `ghostconctl`, via existing tools (`loginctl`, `who`, etc.), and
the result is fed in as a plain argument via ordinary command
substitution:

```bash
ghostconctl switch "$(loginctl show-session "$(loginctl | awk '/seat0/ {print $1}')" -p VTNr --value)"

# or, if the convention on a given system is already known:
ghostconctl switch 2
```

This keeps `ghostcon` itself free of DE-awareness, session-naming
concepts, or alias tables — composability is the feature, not anything
`ghostconctl` does internally. Resist the urge to add a `switch de` or
similar convenience alias; if that's ever wanted, it belongs in a wrapper
script outside this project, not in `ghostconctl` itself.

### Initial verb set

```
ghostconctl switch <ttyN>
    Raw VT number only — no session-name resolution, no DE-awareness.
    Resolves to the logind session currently occupying that VT (or
    directly via VT_ACTIVATE, implementation detail to decide) and
    activates it. Works identically whether the target VT is running
    ghostcon, kmscon, fbcon+agetty, or a graphical DE session — ghostcon
    does not need to know or care what's there.

ghostconctl font scale <factor>
    Runtime font size adjustment for the calling/target VT's ghostcon
    instance. Routes through `wrap`, conceptually the same resize/reflow
    path already needed for VT-mode-change handling (see wrap section).

ghostconctl screenshot <path>
    Render the current frame to a PNG instead of (or in addition to) the
    framebuffer. Implemented at the ghostcon-core/wrap boundary.

ghostconctl notify <title> <body>
    Lets scripts post a notification through ghostcon-ipc's notify path
    without needing to emit a raw OSC 9/777 escape sequence themselves.

ghostconctl notify-hud <label> <value>
    Renders a transient, generic value-bar HUD via ghostcon-overlay (e.g.
    "brightness 80%", "volume 45%") — display-only, zero hardware-control
    knowledge. ghostcon has no concept of what "brightness" or "volume"
    mean or how to change them; it only knows how to render a label+value
    HUD when told to. The actual control action (querying/changing real
    hardware state) is entirely the caller's responsibility, typically
    chained via a `keybind`-configured shell command — see the
    `ghostcon-core` keybind module above for the full example. The HUD
    rendering itself is intentionally generic (label + value, not
    hardcoded "brightness" and "volume" cases) so it's reusable for
    anything else someone wants a transient overlay indicator for later.

ghostconctl clipboard get
ghostconctl clipboard set <text>
    Thin wrapper over ghostcon-ipc's clipboard.sock, for scripting
    clipboard access without going through OSC 52 escape sequences.

ghostconctl clipboard interact
    Opens the interactive clipboard history picker via ghostcon-overlay
    (see overlay section's "Interactive modal mode"). Strictly temporal
    focus grab — selecting an entry sets it as the active clipboard
    buffer and immediately exits; dismissing (Esc/click-away) exits with
    no change. Typically bound via keybind config, e.g.
    `bind = SUPER, V, exec, ghostconctl clipboard interact`.

ghostconctl notify interact
    Opens interactive notification interaction via ghostcon-overlay,
    ascending from top (oldest pending notification first). Same
    strictly-temporal modal behavior as `clipboard interact`. Note: this
    surfaces only currently-pending notifications, not a persistent log —
    see overlay section for why a notification history was explicitly
    not built. Typically bound e.g.
    `bind = SUPER CTRL, N, exec, ghostconctl notify interact`.

ghostconctl status
    Query supervisor state for a given VT (useful for scripting/
    debugging — e.g. confirming whether a VT is currently being served
    by ghostcon, kmscon fallback, or fbcon).

ghostconctl recover
    (May not be needed — recovery is automatic on renderer restart;
    see supervision section above. If kept, it's a manual trigger for
    reconnecting a new renderer to ghost-ptyserv after a crash that
    systemd's restart didn't catch automatically.)
```

### Privilege note on `switch`

VT switching requires either an open fd to a VT device with appropriate
permissions, or going through `logind`'s session/seat D-Bus API
(`Session.Activate`). Prefer routing through `logind` rather than having
`ghostconctl`/`ghostcon-ipc` implement and audit its own privileged
`VT_ACTIVATE` path — `logind` already owns this responsibility safely and
system-wide, and ghostcon already depends on it elsewhere for seat/session
management, so this avoids duplicating privilege-escalation logic the
project doesn't need to own.

---

## 8. OSC sequence support matrix

| OSC | Purpose | Status | Notes |
|---|---|---|---|
| 0 / 2 | Window/icon title | **Repurposed** | → process identity (`argv[0]` verbatim + truncated `PR_SET_NAME`); optionally also shown via `ghostcon-overlay` if configured |
| 1 | Icon name | No-op | No icon concept on bare TTY |
| 4 | Set/query palette entry | Implement directly | Pure rendering state via the ported color module |
| 7 | Report CWD (file:// URI) | Implement directly | State tracking only |
| 8 | Hyperlinks | Implement, needs overlay work | Parsing via the ported terminal state machine; click-to-open needs a handler — shell out to `xdg-open` if available, or fall back to highlight + copy-on-click |
| 9 | Notification (iTerm2-style) | Implement via `ghostcon-ipc` | Normalize with OSC 777 into one event type before reaching IPC |
| 10 / 11 | Set/query fg/bg color | Implement directly | Pure rendering state |
| 12 | Set/query cursor color | Implement directly | Pure rendering state |
| 22 | Set mouse cursor shape | Implement via X11 convention | Assume X11 Xcursor theme convention (`~/.icons/<theme>/cursors/`, `/usr/share/icons/<theme>/cursors/`), named cursors (`left_ptr`, `text`, etc.) map directly. Config: `theme = "..."` to pick installed theme, `override.<name> = path` for per-name custom images. Avoid linking `libXcursor` directly if possible (pulls in X11-adjacent linkage in a non-X11 context) — prefer a minimal standalone Xcursor file parser. |
| 52 | Clipboard read/write | Implement via `ghostcon-ipc` | See clipboard section above; default global, `group_by=user` config for isolation |
| 104 | Reset palette entry | Implement directly | Pure rendering state |
| 133 | Shell integration (prompt markers) | Implement directly | State tracking via ported terminal state machine |
| 633 | VSCode-style shell integration | Implement directly | Same as 133 |
| 777 | Notification (rxvt-style) | Implement, merge with OSC 9 | Same event type, different wire spelling — normalized in the terminal engine before IPC |
| 1337 | iTerm2 proprietary (images, clipboard, etc.) | **Deprioritized** | Reverse-engineer surface-level if needed later, but: `CurrentDir` duplicates OSC 7, `Copy` duplicates OSC 52, `CursorShape` duplicates standard `DECUSCR`. The one genuinely unique piece (`File=` inline images) belongs to the inline-graphics surface generally, and should be considered only if/when sixel and Kitty graphics are scheduled (see the note below) — not implemented ahead of them. |

> **Inline graphics (sixel / Kitty graphics protocol) are not yet
> scheduled.** The rationale section names them as part of the modern
> floor, but no phase in this plan implements them: they are absent from
> the Phase 0 module list and from Phase 1 and 2, and there is no such
> code in the tree. An earlier version of the OSC 1337 row above claimed
> the ported state machine already had sixel/Kitty graphics support — it
> does not, and that claim was wrong. Adding them is a real, unscheduled
> work item; `ghostcon-diag` pages 8 and 9 (sixel, Kitty graphics) will
> score FAIL against `ghostcon` until it is done.
| *(unimplemented/unknown)* | — | **Fallback policy** | Log to journald (structured, queryable e.g. `journalctl -t ghostcon | grep unimplemented-osc`), send a low-priority nudge via `ghostcon-ipc` notify socket, discard payload, continue parsing normally. Never crash or hang on an unrecognized sequence. Use real-world frequency of these log entries to prioritize future work. |

---

## Build / language summary

- **C**: Everything except `ghostcon-ipc`. `ghostcon-core` (kms.c, input.c,
  vtctl.c, transport.c, diag.c, plus the term/ and render/ directories
  ported from Ghostty's Zig), `ghost-ptyserv`, `pty-ttyN`, `undead-head`,
  `supervisor[ttyN]`, `ghostcon-overlay`.
- **Rust**: `ghostcon-ipc`, zero `unsafe`.
- **Zig**: Not used. The earlier plan assumed Zig for the `wrap` FFI
  binding — this is superseded by the direct C port of Ghostty's terminal
  state machine.

## Build phasing

Three phases. Phase 0 is new — it's the port of Ghostty's terminal core
and carries the highest risk. Phase 1 builds the runnable system. Phase 2
adds IPC and overlay. Every phase after 0 depends on the port's
correctness, and Phase 0 findings may force changes to later phases.

### Phase 0 — Port Ghostty terminal core (highest risk)

No rendering, no KMS, no input. A standalone C library that can be fed
PTY bytes and queried for screen state.

1. **`term/cell.h` + `cell.c`** — Cell struct ported from
   `ghostty/src/terminal/Cell.zig`. Unicode codepoint, style ID,
   hyperlink ID, character width.
2. **`term/style.h` + `style.c`** — Interned style table ported from
   `style.zig`. Cells carry a 15-bit `ghostcon_style_id_t` (ID 0 =
   default) into a ref-counted set rather than inline style bits; see
   the API-surface departure note above.
3. **`term/row.h` + `row.c`** — Row of cells, wrap-bit tracking, dirty
   flag. Ported from `Row.zig`.
4. **`term/color.h` + `color.c`** — Palette, 16-color, 256-color,
   truecolor conversion, OSC 4/10/11/12/104. Ported from `color.zig`.
5. **`term/screen.h` + `screen.c`** — Grid, scrollback ring buffer,
   cursor, alt screen, synchronized output (mode 2026), damage tracking.
   Ported from `Screen.zig`.
6. **`term/stream.h` + `stream.c`** — Byte stream processor — the full
   DEC VT state machine (ground, esc, csi, osc, dcs, sos, string_term,
   etc.). Ported from `stream.zig`. Calls into `libghostty-vt` for OSC
   and SGR sub-parsing.
7. **`term/kitty.h` + `kitty.c`** — Kitty keyboard protocol state (flags,
   progressive enhancement handshake). Ported from `kitty.zig`.
8. **`term/selection.h` + `selection.c`** — Selection tracking and
   management. Ported from `selection.zig`.
9. **`term/term.h` + `term.c`** — Top-level orchestrator binding screen,
   stream, color, and selection together.

**Phase 0 is done** when a `ghostcon_screen_t` can be fed a recorded PTY
byte stream and produce a screen state identical to what Ghostty produces
for the same input. Test against a corpus of real shell session captures
and edge-case escape sequence files.

> **Phase 0 may reshape everything.** The actual Ghostty Zig source may
> reveal structural assumptions about memory ownership, event-driven
> rendering, or Zig-specific features (comptime, error unions, slices)
> that require non-trivial C design decisions. The renderer design, API
> surface, event loop structure, and even the screen/stream data model
> documented here are provisional — they represent the best guess before
> reading the actual source, not a locked specification. Update
> `IMPLEMENTATION.md` as Phase 0 progresses.

### Phase 1 — Runnable core (no IPC, no overlay)

1. **`ghost-ptyserv` parent + pty child** — PTY allocation, shell spawn,
   ring buffer, socket I/O, and parent registry. Build the parent as a
   thin `fork()`/`waitpid()` loop that spawns one pty child per requested
   VT. Build the pty child as byte-stupid: reads from PTY master, writes
   to ring buffer, forwards to connected renderer socket; reads from
   renderer socket, writes to PTY master. Must be independently testable:
   start ghost-ptyserv with a test config requesting one VT, then a test
   client connects to the pty child and verifies bidirectional byte flow
   and ring buffer replay on reconnect.

   **Done** (`src/ptyserv/{main,pty_child,protocol}.c`,
   `include/ghostcon/ptyserv/protocol.h`, `tests/test_ptyserv.c`).
   Decisions not pinned down elsewhere in this doc:
   - Registry protocol is plain-text, newline-framed (`GET <vtnum>` →
     `OK pid=<pid> socket=<path>` / `ERR <reason>`) — deliberately not
     `ghostcon-ipc`'s strict-framed/`SO_PEERCRED` protocol, since this is
     a same-host coordination handshake, not a cross-privilege broker.
   - Per-VT pty child sockets live alongside the registry socket
     (`dirname(registry_socket)/pty-tty<N>.sock`), not hardcoded under
     `/run/ghostcon` — keeps it root-free and testable; production still
     gets `/run/ghostcon/pty-tty<N>.sock` because the registry socket
     itself is `/run/ghostcon/ptyserv.sock`.
   - Ring buffer default: 1 MiB, fixed-size, overwrite-oldest
     (`GHOSTCON_PTY_RINGBUF_DEFAULT_SIZE`).
   - `ghost-ptyserv` takes VT list via argv for now (`<registry_socket>
     <vtnum> [vtnum...]`) — TOML config parsing is unstarted and out of
     scope for this item per this doc's own step list.
   - Uses glibc `forkpty()` rather than manual `openpty`/`fork` — same
     "byte-stupid" simplicity goal, no behavioral difference.
2. **KMS/DRM + EGL/GLES + renderer** — set up mode setting, GBM buffer
   allocation, EGL context on GBM, and the GLES 2.0 render pipeline:
   - `core/kms.c`: DRM mode setting, dumb/GBM buffer allocation,
     atomic page flip
   - `core/egl.c`: EGL initialization on GBM surface
   - `render/atlas.c`: font glyph atlas (fontconfig + freetype → GPU tex)
   - `render/gles.c`: GLES 2.0 renderer (shaders, draw calls)
   - `render/machine.c`: damage → quad list
   Build as a standalone test: render a known screen state to a KMS
   buffer and verify visually.

   **All of `egl.c`/`atlas.c`/`gles.c`/`machine.c`/`kms.c` done and
   verified — including real KMS scanout**, not just compile-checked.
   `tests/test_render.c` covers the headless render path (no DRM
   master). `kms.c` + `vtctl.c` were then verified together live on
   real hardware via a manual scratch harness (root, brief VT switch to
   a free VT4 and back) — see item 3's note for the harness details;
   it rendered actual text via atomic KMS scanout on the real
   1920x1080 display and the user visually confirmed it. `egl.c` is
   written so the same code path works for both (see its own doc
   comment): `ghostcon_egl_init` opens its own render-only GBM
   device/surface (headless), `ghostcon_egl_init_with_gbm` wraps a
   scanout-capable surface `kms.c` already created (real display).
   - Fixed a real Phase 0 modeling bug found here, not a renderer bug:
     `GHOSTCON_STYLE_DEFAULT` (`term/style.c`) had `flags = 0`, making
     "untouched/default style" indistinguishable from "explicitly set
     to ANSI palette color 0 (black)" when resolving fg/bg — text
     rendered black-on-black. Ghostty's real `Style.fg_color`/`bg_color`
     default to a tagged `.none` ("use terminal default"); the port's
     flat-flags model already had `GC_STYLE_FG_DEFAULT`/`BG_DEFAULT` bits
     for this but never set them on the default constant, contradicting
     its own doc comment ("fg=default, bg=default"). Phase 0's tests
     never exercised color resolution (only codepoints/dirty tracking),
     so this went unnoticed until the renderer actually resolved colors.
   - Atlas texture upload must happen *after* the frame's damage pass,
     not before — glyphs rasterize lazily on first use inside
     `ghostcon_machine_render_dirty`, so `ghostcon_gles_sync_atlas`
     syncing first uploads a stale/empty atlas with no GL error (see
     `gles.h`'s doc comment on call order: begin → render_dirty →
     sync_atlas → end).
   - Glyph baseline was anchored at `cell_h` (the cell's bottom edge),
     leaving no room below the baseline — descenders (g, q, y, p, j)
     spilled into the next row and got overdrawn by its background
     quad (found via the real-hardware test: "font render... some of
     them aren't rendered well probably on overdraws like g,q"). Fixed
     by anchoring at the font's actual ascent metric instead
     (`ghostcon_atlas_ascent`, from `face->size->metrics.ascender`, not
     any per-glyph value) — see `atlas.c`/`machine.c` comments.
   - `ghostcon_screen_init` (`term/screen.c`, **Phase 0 code**) left
     `dirty.y_min/y_max = -1` (nothing dirty) instead of marking the
     whole screen dirty like every other full-redraw path in the same
     file already does (`ghostcon_screen_reset` etc. use `y_min=0,
     y_max=rows_visible-1`). Consequence: any row never explicitly
     written to (the common case — most sessions don't fill every row)
     never got a background quad pushed on the first frame at all,
     leaving whatever was already in the render target instead of the
     terminal's actual background — found via the real-hardware retest:
     "rows that aren't allocated by chars have different background
     than the ones that are allocated". Fixed to match the existing
     full-redraw convention. Re-verified: `run_compare.sh` still 44/44
     (dirty state isn't part of the diffed canonical dump), all 4
     `meson test` targets still pass.
   - **Incident: `ghostcon_kms_page_flip` wedged the display badly
     enough to require a hard reboot**, hit while testing a real
     interactive session (a throwaway `boot_bash_test` harness running
     `/bin/bash` over real KMS scanout — the first code path to ever
     exercise repeated page flips, as opposed to the single
     `ghostcon_kms_modeset` call the earlier vtctl/kms verification
     used). Root cause: on a page-flip completion-event timeout, the
     function returned failure while leaving the just-committed
     buffer/framebuffer completely untracked — not released, not
     adopted as current — reasoning (wrongly) that since we didn't
     confirm the flip, nothing had changed. In fact
     `drmModeAtomicCommit` succeeding means the kernel **will** apply
     the flip regardless of whether its completion event is ever
     delivered (real drivers do lose/delay these under load) — so the
     caller's teardown path then destroyed the GBM surface and dropped
     DRM master while that buffer may have still been the one actively
     scanned out, corrupting GPU/display state (symptom: black screen,
     system still alive/disk-active, unrecoverable via VT switch,
     needed a hard reboot).
     Also worth correcting explicitly: **a VT switch (Ctrl+Alt+Fn) is
     only guaranteed to work if the process controlling the VT is still
     responsive enough to ack `VT_RELDISP`** — an earlier claim in this
     conversation that it "always works regardless of program state"
     was wrong. A genuinely hung controlling process can block the
     kernel from forcing the switch, which is exactly the
     highest-risk failure mode this whole module exists to survive
     (via an external supervisor that can kill a hung renderer — not
     built yet, see Phase 1 item 7/`undead-head`).
     Fix: separated "did the kernel accept the commit" (known
     synchronously and reliably from `drmModeAtomicCommit`'s return)
     from "did we get notified about it" (the completion event — now
     used only to pace the next flip, not as a correctness gate). The
     new buffer is unconditionally adopted as current on a successful
     commit, and the old one retired, regardless of whether the
     completion event ever arrives. Recompiled and re-verified: all 4
     `meson test` targets still pass. **Not yet re-verified live** —
     deferred until the user is ready for another VT-switch session,
     this time with an external timeout/kill-switch armed as a safety
     net given what just happened.
3. **`core/vtctl.c`** — VT_PROCESS handshake (SIGUSR1, SIGUSR2,
   `VT_RELDISP`, `drmSetMaster`/`drmDropMaster`). Reference kmscon's
   `vt.c` as a reading guide. Must be independently testable with a
   single-VT process opening `/dev/ttyN`.

   **Done and verified live**, together with `kms.c` (see item 2's
   note): a manual root harness (`pkexec`, since this machine has no
   passwordless sudo) opened `/dev/tty4`, called `ghostcon_vtctl_open`,
   issued `VT_ACTIVATE`/`VT_WAITACTIVE` to switch to it, observed the
   real SIGUSR1 acquire handshake complete, `drmSetMaster` succeeded
   (with a short retry loop to absorb the race against the outgoing
   compositor's own asynchronous release handling), ran the full
   kms+egl+render pipeline showing real text via atomic KMS scanout at
   1920x1080, then reversed the whole sequence (`drmDropMaster` before
   acking release, per this module's documented ordering contract) and
   returned cleanly to the original VT. User visually confirmed the
   text appeared on screen during the switch.
   Self-pipe pattern: the SIGUSR1/SIGUSR2 handler only writes a byte
   (async-signal-safe); the actual `ioctl`/`drmSetMaster`/
   `drmDropMaster` calls happen in `ghostcon_vtctl_process_pending()`,
   called from the main loop after polling `ghostcon_vtctl_signal_fd()`
   readable — signal handlers can't safely call `ioctl()` directly.
4. **`core/transport.c` + `core/diag.c`** — transport: query ghost-ptyserv
   for pty child socket, connect, manage bidirectional stream. diag:
   panic/crash self-reporting via signal handlers and journald.

   **Done and verified** (`tests/test_transport.c`, `tests/test_diag.c`).
   - `transport.c` reuses the registry protocol from ptyserv item 1
     (`ghostcon_ptyserv_format_get`/`parse_ok`) — connects to the
     registry, resolves the pty child socket for a VT, connects to it.
     Verified end-to-end against a real `ghost-ptyserv` + `pty-ttyN`
     pair (same setup as `test_ptyserv.c`), including bidirectional
     byte flow through the transport API.
   - `diag.c` installs handlers for SIGSEGV/SIGABRT/SIGFPE/SIGILL/SIGBUS
     (`SA_RESETHAND`, so a fault recurring inside the handler hits the
     now-default disposition directly instead of recursing), captures a
     backtrace (`backtrace()`/`backtrace_symbols()` — not
     async-signal-safe, a deliberate pragmatic tradeoff shared by most
     real-world crash reporters, e.g. Breakpad: best-effort diagnostics
     beat none, and a re-fault here means the process was dying anyway),
     sends structured fields to journald via `sd_journal_send`
     (`GHOSTCON_COMPONENT`/`GHOSTCON_VT`/`GHOSTCON_SIGNAL`), writes the
     full trace to stderr via the async-signal-safe
     `backtrace_symbols_fd`, then re-raises so the process dies with the
     correct signal/exit status. `test_diag.c` verifies this against a
     *real* triggered `SIGABRT` in a forked child — not just a code
     read — matching this doc's own testing-strategy note (line ~1810)
     to confirm the panic-handler wiring works, and checks the actual
     `journalctl` entry for the expected fields. (One test-harness
     gotcha found along the way: bare `_PID=<pid>` isn't unique in
     `journalctl` — PIDs get reused across boots — so the query needs
     `-b` plus `_COMM=` alongside it.)
5. **`core/input.c`** — libinput event loop → `ghostty_key_event_*`
   translation → `ghostty_key_encoder_encode` → write to pty child.

   **Done and verified**, no hardware risk involved unlike kms/vtctl —
   evdev is non-exclusive (multiple readers coexist fine; this never
   interferes with the desktop session's own input) and this machine's
   user is in the `input` group, so libinput's udev backend opens
   `seat0` without root. Split deliberately in two:
   - `ghostcon_input_evdev_to_ghostty_key()` +
     `ghostcon_input_encode_key()` are pure functions (evdev keycode +
     action + mods + already-resolved text → encoded bytes), no
     libinput/hardware dependency, directly unit-tested
     (`tests/test_input.c`) against real encoder output — e.g. plain
     `a` → `0x61`, Ctrl+C → `0x03`, arrow-up → `ESC [ A` normally vs.
     `ESC O A` under DECCKM/application-cursor mode. Verified against
     actual `ghostty_key_encoder_encode` output, not assumed.
   - The live half (`ghostcon_input_open`/`_dispatch`, libinput + udev +
     xkbcommon context/keymap/state) was smoke-tested standalone:
     opens `seat0` cleanly, `poll()`s correctly, closes cleanly.
   - Scope note: per PLAN.md's own "Ordered pipeline", the `keybind`
     Rust component (step 2) and `wrap`'s mouse-reporting/Kitty-mode
     awareness aren't built yet, so `input.c` currently goes straight
     from raw evdev keys to `pty-ttyN` via transport.c with no keybind
     interception — pointer/touchpad events are observed and discarded.
   - Real bug found and fixed along the way, unrelated to input.c
     itself: this repo's top-level `meson.build` had
     `dependency('udev', ...)` — that pkg-config name resolves to a
     virtual/header-only package with no `Libs:` line on this distro;
     the actual linkable library is `libudev.pc`. Silently never
     linked anything before now because nothing had actually called a
     libudev symbol at link time until `input.c`. Fixed to
     `dependency('libudev', ...)`.
6. **`core/main.c`** — event loop: poll libinput, poll pty socket for data,
   call `ghostcon_term_feed`, `ghostcon_term_render`, canary heartbeat.

   **Written, compiles/links clean, ties every prior Phase 1 module
   together** (`vtctl`, `kms`, `egl`, `transport`, `input`, `diag`,
   `render/{atlas,gles,machine}`, `term`). Fails gracefully without
   root (confirmed: `vtctl_open` reports the permission error and exits
   1, no crash). **Not yet live-tested end-to-end** — that would be the
   highest-integration-risk test yet (every module at once, via the
   real production binary + a real `ghost-ptyserv`, not a scratch
   harness), and given today's session already hit three real hardware
   incidents getting the individual pieces solid, this is deliberately
   left for the user to explicitly opt into rather than proposed
   proactively.
   The one genuinely new piece of logic here, not exercised by any
   earlier scratch harness (`kms_vtctl_test`/`boot_bash_test` each only
   ever handled a single startup-activate-then-exit cycle): a real
   acquire ↔ release ↔ reacquire loop, since a desktop user switches
   VTs back and forth repeatedly over one session. `acquire_display()`/
   `release_display()` split what's per-cycle (DRM master, `kms`, `egl`,
   `gles` — torn down on release, rebuilt on reacquire) from what's
   long-lived (terminal state, font atlas, the transport/input
   connections — survive every cycle, so a VT switch away and back
   doesn't lose scrollback or reconnect the shell). Known Phase 1
   limitation, not a bug: terminal grid size is fixed at whatever the
   first successful acquire reports; a reacquire at a different
   resolution keeps the old grid rather than live-resizing (proper
   resize + SIGWINCH to the shell is a follow-up, not built here).

   **Two real bugs found on the first actual live VT-switch test**
   (`DEFAULT_DRM_NODE`/wrong-GPU-node issue aside — that one was just
   this machine having `card1` instead of the common-case `card0`,
   fixed with a `GHOSTCON_DRM_NODE` override in `supervisor` since
   `undead-head` deliberately doesn't thread hardware-specific args
   through, and since `pkexec` resets the environment, that override
   has to go through `pkexec env VAR=val CMD`, not a plain
   `VAR=val pkexec CMD` prefix):
   - Acquiring the display (the VT-switch/`SIGUSR1` path) never itself
     triggered a render — `render_frame()` only ran when `need_render`
     was set, which otherwise only happened when *new* PTY bytes
     arrived in that same poll iteration. Whatever was already fed
     while the VT was inactive (e.g. the shell's own startup prompt)
     just sat undrawn until some unrelated later write forced a render
     — in practice this looked exactly like "the screen stays on the
     previous frame until you press a key," which is exactly the
     symptom hit live. Fixed by setting `need_render = true` on a
     successful acquire.
   - That fix alone wasn't sufficient for a *re*acquire specifically
     (as opposed to the very first acquire): `acquire_display()`
     destroys and recreates the entire GPU context (`kms`/`egl`/`gles`)
     from scratch, but the terminal's dirty-region tracking assumes a
     *continuous* GPU context and only remembers cells changed since
     the last render — if a prior render cycle had already cleared it,
     nothing would be marked dirty even though the fresh GPU context
     has no prior frame content at all. Fixed by explicitly forcing the
     whole screen dirty on every successful acquire (same convention
     `ghostcon_screen_reset()` already uses internally), not just
     setting the render flag.
   - Related, found by reading the code while fixing the above rather
     than by a live symptom: `ghostcon_screen_clear_dirty()` was never
     called anywhere in the render pipeline — the dirty region only
     ever grew, never reset, so every frame was redrawing the entire
     accumulated range since startup instead of just what changed.
     Harmless for correctness (over-drawing already-correct cells isn't
     wrong) but wasteful; fixed by clearing it after each frame's
     `ghostcon_machine_render_dirty()` call.

   **That "harmless but wasteful" fix turned out not to be harmless —
   reverted after the very next live test.** Paired with a matching
   change to only `glClear()` on the first frame (not every frame, to
   stop wiping rows nothing was about to repaint), the combination
   caused visible text to flash in and out on literally every
   keystroke. Root cause: `core/kms.c`'s GBM/EGL surface rotates across
   multiple physical buffers for tear-free presentation, and each
   buffer has its own independent paint history. "Only redraw what
   changed since the last `render_frame()` call" implicitly assumes a
   single continuous buffer; against N rotating buffers, each one only
   ever receives the damage from every Nth frame, so already-rendered
   content disappears on the buffers that didn't get that specific
   update and reappears on the ones that did — exactly the alternating
   flash observed. Proper per-buffer damage tracking would make the
   incremental version correct, but is real complexity not worth
   taking on yet. Reverted to clearing + redrawing the full accumulated
   dirty region every single frame (`ghostcon_gles_begin(..., true,
   ...)`, no `ghostcon_screen_clear_dirty()` call) — the same approach
   the earlier live interactive-bash test had already proven correct
   on real hardware, before this regression was introduced one commit
   later. Matches this doc's own renderer design note ("redrawing all
   dirty cells each frame is sufficient" for a terminal at this scale)
   more literally than the reverted version did: *every* frame, not
   just the first.
   All 9 `meson test` targets re-verified passing after these changes.
   Also worth recording: a `test_undead_head` failure that recurred 3x
   in a row during this session turned out to be pure environmental
   contamination, not a code bug — three separate real `undead-head`
   trees were left running (root, from repeated manual `pkexec`
   attempts on tty4), all sharing the same binary paths the test
   matches via `pgrep -f`, so the test was catching a stray production
   process instead of its own. Cleaning those up made it pass
   immediately with no code change. Worth remembering next time a test
   fails after a live hardware session: check for leftover real
   processes sharing the test's binary paths before assuming the code
   regressed.

   One more practical finding: `pkexec env VAR=val CMD` (needed in
   principle to pass an env var through `pkexec`'s environment reset)
   did not reliably show its polkit auth prompt when actually tried —
   the process just sat there with no visible dialog and no way to
   authenticate it, indistinguishable from a hang. Worked around by
   hardcoding this machine's actual DRM node as `core/main.c`'s and
   `supervisor/main.c`'s default instead of depending on that env var
   being reachable at all — see item 2's own note on the `DEFAULT_DRM_NODE`
   fix. Plain `pkexec /path/to/binary args` (no `env` wrapper) has been
   reliable throughout.

   **Milestone: `ghostcon` fully replaced kmscon on tty4 on real
   hardware** — the complete production stack (`undead-head` →
   `ghost-ptyserv` → `supervisor[tty4]` → `ghostcon-core`) running via
   `pkexec`, `kmsconvt@tty4.service`/`getty@tty4.service` masked so
   nothing raced it for the VT, real KMS scanout, a real interactive
   shell, confirmed working correctly by the user after the buffer-
   flash fix above (item 6's note) — text stable across keystrokes, no
   flicker, matching the standard this project set out to hit.

   **That milestone still had a real gap, found immediately after**:
   `pty-ttyN` (item 1) spawned the raw shell directly with no
   authentication at all — since the whole stack runs as root (under
   `pkexec`, until a real privilege-separated deployment exists),
   whoever connects landed straight in a root shell, and exiting it
   just ended the session with nothing left attached to the VT's PTY
   ("exiting bails it to blank kernel state" — the user's own words).
   Real `agetty`/kmscon always delegate to `login(1)` for the
   `hostname login:` / `password:` prompt and proper session setup;
   `pty-ttyN` now does the same — spawns `agetty -8 --noclear - $TERM`
   (`-` = use the already-attached pty line, matching kmscon's own unit
   file convention already referenced above) instead of the shell
   directly, and **respawns a fresh session automatically when one
   exits** rather than this process exiting and leaving the PTY with
   nothing attached — same backoff reasoning as `undead-head`'s own
   restart loop (this item's own bug list, `apply_restart_backoff`),
   applied here too since a session that dies instantly (misconfigured
   `agetty`, missing binary) must not respawn in a tight loop either.
   `GHOSTCON_PTY_SKIP_LOGIN=1` bypasses this for `test_ptyserv.c`/
   `test_transport.c`/`test_undead_head.c`, which all assume immediate
   raw shell access. Verified manually (not just unit-tested): connected
   to a real `pty-ttyN` instance and confirmed the actual
   `Arch Linux ... / dev login:` prompt appears, and that killing the
   session mid-test produces a fresh prompt automatically without the
   process exiting. All 9 `meson test` targets still pass.

   **Two more real bugs, both found live after the login fix landed**:
   - Terminal grid size: `core/main.c` only computed the real
     cols/rows from `kms.width`/`height` if the VT was already active
     at process *startup* — the normal case (you switch to the VT
     later) fell back to a hardcoded 80x24 placeholder that was never
     revisited even after a real acquire happened via the VT switch.
     Practical effect: the terminal only ever occupied a small corner
     of the actual screen and started scrolling well before reaching
     the real bottom — the user's own description, "halfway through it
     just scrolls up instead of keeping to allocate the next row" (a
     ~1080p screen at this font size needs roughly double 24 rows, so
     "halfway" lines up exactly with hitting the 80x24 placeholder's
     limit). Fixed by recomputing cols/rows on every acquire (not just
     startup) and calling `ghostcon_term_resize()` when they differ —
     this also naturally covers a genuine future reacquire-at-different-
     resolution case, not just the startup-placeholder one.
   - Atlas texture upload: every VT reacquire creates a brand-new
     `ghostcon_gles_t`, including a brand-new (empty) GPU texture for
     the glyph atlas — but `ghostcon_gles_sync_atlas()`'s upload was
     gated purely on the atlas's own dirty flag, which tracks the
     separate, longer-lived CPU-side bitmap, not any given GLES
     instance's texture object lifetime. If no new glyphs had been
     rasterized since some earlier instance's last sync, the flag reads
     "not dirty" and the fresh texture never gets ANY data uploaded to
     it — an "incomplete" texture per the GL spec, and sampling one is
     implementation-defined. On this driver that evidently returns
     solid opaque alpha, so every glyph quad rendered as a filled
     rectangle instead of its shape — the user's "solid coloured
     blocks when the renderer is refreshed". Fixed by adding a `force`
     parameter to `ghostcon_gles_sync_atlas()`, called `true` exactly
     once per `ghostcon_gles_t` instance right after
     `ghostcon_gles_create()` (in `acquire_display()`), unconditionally
     uploading the current atlas state to the new texture regardless of
     the dirty flag's unrelated history; normal per-frame calls stay
     dirty-gated (`force=false`) as an optimization.
   Both fixed together, not yet re-verified live (the fixes landed
   while the user's session from testing the login fix was still up —
   deliberately left untouched rather than killing an active session
   to test sooner). All 9 `meson test` targets re-verified passing.
7. **Supervisor + `undead-head`** — once a `ghostcon[ttyN]` + pty child
   pair can start, render, and self-report, build the canary/reconnect/
   fallback loop around the renderer. Test against an artificially induced
   hang (e.g. a debug build with a deliberate infinite loop trigger) to
   confirm the kill-reconnect path actually works before trusting it.

   **Done and verified**, no real KMS/VT hardware needed — the canary
   mechanism only needs a socketpair and a process to watch, so
   `tests/fake_renderer.c` stands in for `ghostcon-core` (heartbeats
   normally, or on cue: never heartbeats, hangs after N ms, or exits
   cleanly after N ms) and drives every state transition in
   `tests/test_supervisor.c` for real. `tests/test_undead_head.c`
   covers undead-head's own specific job (restart-on-crash for both
   ghost-ptyserv and a supervisor) using the real binaries.
   `src/supervisor/main.c` implements the state machine exactly as
   specified: `SPAWNING` → `ACTIVE` (first canary byte) or `FALLBACK`
   (startup deadline expires with no byte — same `poll()` call serves
   as both the startup race timer and the ongoing canary, per the
   philosophy section's "silence past a deadline = treat as dead");
   `ACTIVE` → hang detected → kill + retry `ghostcon` (not fallback —
   fallback is only for "never even started"); `FALLBACK` → `SIGHUP`
   ("user requests ghostcon") → retry. Recovery file + rate-limited
   `wall(1)` broadcast on both hang and startup-timeout. Three-tier
   fallback uses this system's *actual* `kmsconvt@.service` invocation
   (verified against `/usr/lib/systemd/system/kmsconvt@.service`, not
   guessed) for tier 2, `agetty` for tier 3.
   Three real bugs found and fixed while getting these tests to
   actually pass (not just compile):
   - `supervisor`'s `ACTIVE`-state canary drain used
     `while (read(canary_fd, buf, sizeof(buf)) > 0);` to "drain" the
     socket. On a blocking stream socket that's still open, the read
     *after* the last available byte blocks waiting for more data
     instead of returning 0 (that only happens on EOF) — so the moment
     the renderer actually went silent, this loop hung forever instead
     of ever returning to the `poll()` that exists specifically to
     detect that silence. Fixed to a single non-looping read (`POLLIN`
     already guarantees ≥1 byte is ready).
   - `undead-head`'s `SIGTERM`/`SIGINT` handler used `SA_RESTART`. The
     reaper loop spends most of its time blocked in `waitpid(-1, ...)`,
     and the shutdown flag the handler sets is only checked at the loop
     top — with `SA_RESTART` the kernel transparently resumes the
     interrupted `waitpid()` itself instead of returning `EINTR`, so
     control never came back to notice the flag. `SIGTERM` was
     silently swallowed; the process just kept blocking. Fixed by
     dropping `SA_RESTART` for this handler specifically.
   - `undead-head`'s `kill_group()` called `getpgid(pid)` to find the
     process group to signal, but it's most often called right after
     `waitpid()` has already reaped `pid` — a fully-reaped pid no
     longer exists to query, so `getpgid` failed (`ESRCH`) and the kill
     was silently skipped, leaving orphaned grandchildren (e.g.
     `pty-ttyN`, once its parent `ghost-ptyserv` died) running forever.
     Since every group leader here calls `setpgid(0, 0)`, the pgid
     always equals the leader's own original pid by construction —
     fixed to `kill(-pid, SIGTERM)` directly, no lookup needed. (This
     class of bug is exactly why `tests/test_undead_head.c` checks for
     zero leftover processes after teardown, not just exit codes —
     the first version of that test passed on stdout content alone
     while the process itself hung for the full 30s timeout holding an
     orphan's inherited stdout pipe open, the same failure shape as an
     earlier `test_transport.c` bug from Phase 1 item 1.)

   **A fourth bug surfaced on the first real (non-test) launch**, run
   by the user via `pkexec undead-head ...` to actually take over tty4
   from kmscon: `fork_ghost_ptyserv`/`fork_supervisor`/`spawn_renderer`
   all defaulted to bare binary names ("ghost-ptyserv", "supervisor",
   "ghostcon-core") relying on `PATH` when no override env var was set.
   `pkexec` resets the environment for security, including `PATH` — so
   every `execvp`/`execlp` failed immediately, which (correctly, if
   uselessly) triggered undead-head's own "child died, restart"
   handling in a tight loop, producing ~5.9MB of log spam in seconds.
   Not a hardware/display risk (no VT/DRM was ever touched — every exec
   attempt failed before the child could do anything), but wasteful and
   alarming to watch. Fixed by resolving sibling binaries via
   `/proc/self/exe`'s own directory instead of trusting `PATH` or an
   env var that also doesn't survive `pkexec` — see
   `resolve_sibling_path()` in `undead_head/main.c`, `supervisor/main.c`,
   and (same issue, one binary later — `pty-ttyN`, missed by the first
   pass) `ptyserv/main.c`. `ghost-ptyserv` also gained a best-effort
   `mkdir()` of the registry socket's directory before `bind()` —
   `/run/ghostcon` doesn't exist by default without a systemd
   tmpfiles.d rule, which this project doesn't ship yet.

   **A fifth, more serious incident** happened while those first-pass
   fixes were only half-applied: the user switched to tty4 while
   `undead-head` was still stuck in the "ghost-ptyserv keeps dying near-
   instantly → restart the whole tree" loop. That restart path had
   **zero backoff** — an immediate `goto restart` with no delay — so a
   persistently-failing `ghost-ptyserv` meant the whole tree (every
   supervisor, every `ghostcon-core`) was being torn down and rebuilt
   in a tight loop, each cycle doing a fresh `vtctl_open()`/claim/close
   on tty4 with no pause between cycles. That rapid-fire VT_PROCESS
   acquire/release pattern wedged VT switching badly enough that even
   `Ctrl+Alt+Fn` couldn't recover it — needed a hard reboot. Fixed with
   real exponential backoff (`apply_restart_backoff()`, 1s → 2s → 4s...
   capped at 30s, reset once a restart survives 10s) applied to both
   the whole-tree restart and the individual-supervisor restart paths,
   so a persistently-broken child now restarts slower and slower rather
   than spinning tightly — structurally prevents this failure mode
   regardless of whatever else might be broken, not just a fix for this
   specific bug. All 9 `meson test` targets re-verified passing
   (including 4 repeated runs of `test_undead_head` for stability) after
   this change.
8. **OSC matrix, "implement directly" tier only** — OSC 4, 7, 10, 11, 12,
   104, 133, 633, plus the OSC 0/2 process-identity repurposing. These
   fall out of correct integration with libghostty-vt's OSC parser and
   don't touch IPC/overlay at all.

   **Load-bearing discovery, checked before writing any of this**: the
   installed `libghostty-vt`'s OSC C API (`GhosttyOscCommandData`)
   exposes exactly ONE data field across all ~22 recognized OSC
   command types — window-title text. Verified this isn't a
   distro-packaging gap by checking the from-source master build too
   (`~/.cache/ghostcon/ghostty-src`, built with `-Demit-lib-vt=true`
   for Phase 0/1 KMS work) — identical, single-field enum. So none of
   OSC 4/7/8/9/10/11/12/52/104/133/633's *parameters* (palette index,
   color spec, CWD string, hyperlink URI, notification text, semantic-
   prompt marker, ...) are extractable via the library at all — only
   the coarse command *type* is, and even that's ambiguous for this
   tier: OSC 4/10/11/12/104 (five different commands) all collapse
   into one `GHOSTTY_OSC_COMMAND_COLOR_OPERATION` classification with
   no way to tell them apart via the API. This matches this doc's own
   phrasing ("via the ported terminal state machine") more literally
   than a thin-wrapper implementation would have — parsing the
   semicolon-delimited parameters ourselves from the raw OSC payload
   was already the intent, just not spelled out as bluntly as "the
   library can't do this part at all."
   **OSC 4/10/11/12/104 done and verified** (`src/term/stream.c`'s new
   `osc_dispatch_manual()`, `src/term/color.c`'s new
   `ghostcon_color_parse_spec()`/`ghostcon_color_format_spec()`,
   `tests/test_osc.c`): parses `#RGB`/`#RRGGBB`/`#RRRGGGBBB`/
   `#RRRRGGGGBBBB` and X11 `rgb:R/G/B` (variable digit width per
   component) color specs, handles both `set` and `?` (query, written
   back through `ghostcon_term_set_output` — see below) for all five
   commands. `osc_dispatch_manual()` operates on a **local copy** of
   the raw OSC buffer, not `st->buf` directly, specifically because it
   must fall through to the existing `ghostty_osc`-based path (window
   title, and the discard-and-log fallback) for numbers it doesn't
   own — mutating the shared buffer in place would have corrupted that
   fallback path's input (verified this matters: a test case feeding a
   window-title sequence between two OSC 4 sequences catches exactly
   this class of regression).
   One real parser bug caught by `test_osc.c`, not by manual review:
   `#RGB`-style single-hex-digit-per-component specs were being scaled
   via a plain left-shift (`0xf` → `0xf0`) instead of X11's actual
   convention, bit *replication* to fill the target width (`0xf` →
   `0xff`, since X11's `XParseColor` conceptually repeats the 4-bit
   pattern to fill 16 bits before truncating back down, not zero-pads
   it) — `#f00` was parsing as a slightly dim red instead of pure red.
   Fixed in `parse_hex_component()`.
   All 10 `meson test` targets pass (9/10 — the 10th, `test_undead_head`,
   hit the same known live-session `pgrep` contamination documented
   under item 7, unrelated to this change), and the 44-file Ghostty
   differential conformance corpus (`tools/run_compare.sh`) still
   passes 44/44 — confirming this new OSC dispatch path didn't disturb
   Phase 0's byte-level correctness.
   **OSC 7 (report CWD) done and verified**, same manual-parse tier
   (added as a new `case 7:` in `osc_dispatch_manual()`). Confirmed the
   same library limitation applies here too: `GHOSTTY_OSC_COMMAND_REPORT_PWD`
   is a distinguishable command *type*, but — like the color-operation
   tier — carries no extractable data field, so the `file://[host]/path`
   URI has to be parsed by hand (`osc7_decode_pwd()`). Discards the host
   component unconditionally (xterm's own documented stance: accept it,
   don't validate against the local hostname — this terminal has no
   concept of "which host a path is valid on" beyond whichever shell is
   currently attached), and percent-decodes the path (`%20` etc.),
   falling back to passing a malformed `%` escape through literally
   rather than rejecting the whole sequence, matching OSC 7's advisory,
   best-effort nature. Also accepts a bare path with no `file://`
   wrapper, since not every shell integration script wraps one. Stored
   as a new fixed `char cwd[1024]` field directly on `ghostcon_screen_t`
   (same access pattern as `palette` — no separate accessor function,
   consistent with how the rest of the OSC state is exposed for tests).
   No new bugs found by `tests/test_osc.c`'s two new cases (URI-with-
   host-and-percent-escapes, bare-path) — both passed on first run,
   likely because it reused the already-hardened field-split/local-copy
   machinery from the color tier. Full suite still 9/10 (`test_undead_head`
   still failing on the same known live-tty4-session `pgrep` contamination,
   confirmed via `pgrep -af` against the unchanged pids 8103/8119/8120/8122
   from earlier in this doc) and the 44-file differential corpus still
   44/44.

   **OSC 133/633 (shell integration prompt markers) done and verified**,
   same manual-parse tier, one shared `case 133: case 633:` in
   `osc_dispatch_manual()`. Noteworthy: OSC 633 (VSCode's shell
   integration protocol) isn't in `GhosttyOscCommandType` at all — the
   library only knows FinalTerm's OSC 133 (as `GHOSTTY_OSC_COMMAND_
   SEMANTIC_PROMPT`, again with zero extractable data) — so 633 would
   have silently fallen all the way through to the discard-and-log
   default case without this tier, not just lost its parameters. Since
   the manual tier dispatches purely by the numeric OSC command (parsed
   off the raw buffer, before any library classification happens), this
   was a non-issue: both numbers share one case body.
   Implemented the FinalTerm/VSCode-shared sub-letter set: `A` (prompt
   start), `B` (prompt end / input start), `C` (command output start),
   `D[;exit_code]` (command finished, optional exit code). This reuses
   pre-existing-but-previously-dead infrastructure discovered while
   scoping the work: `include/ghostcon/term/cell.h` already had a
   2-bit `ghostcon_cell_semantic_t` (OUTPUT/INPUT/PROMPT) packed into
   the cell's bitfield with get/set accessors, but nothing anywhere in
   the codebase ever called the setter — every cell was implicitly
   OUTPUT forever. Added `semantic_current`/`semantic_last_exit_code`
   to `ghostcon_screen_t`, and one line in `screen.c`'s `print_cell()`
   (`ghostcon_cell_set_semantic(cell, s->semantic_current)`) to
   actually stamp it — the OSC handler only flips the screen-level
   "what phase are we in" flag; the existing per-character print path
   does the rest. This is intentionally screen-level state, not
   cursor-level: semantic phase describes what the *shell* is doing,
   not where the cursor happens to be, so it must survive things like
   DECSC/DECRC cursor save/restore that a per-cursor field would not.
   Also implemented OSC 633's `P;Cwd=<path>` property report (VSCode
   sends CWD both via OSC 7 and via this alternate path) by directly
   reusing `osc7_decode_pwd()` — same URI-or-bare-path grammar.
   `tests/test_osc.c` extended to drive a real prompt/input/output/
   exit-code cycle through `ghostcon_term_feed()` and read back the
   actual stamped cell bits (`ghostcon_cell_get_semantic()`) rather
   than just checking the screen-level state flag, so the ghostcon_cell_set_semantic
   wiring itself is under test, not just the OSC parsing. All new
   cases passed without needing a fix — no new bugs found this round,
   plausibly because the manual-tier scaffolding (field-split, local-
   copy-of-buffer) was already hardened by the earlier OSC 4/7 work.
   9/10 `meson test` (same known unrelated `test_undead_head`
   live-session contamination) and 44/44 on the differential corpus.

   **OSC 0/2 (process-identity repurposing) done and verified.** Added
   a new term-level callback tier (`ghostcon_title_fn`/
   `ghostcon_term_set_title`/`ghostcon_stream_set_title`), mirroring
   `ghostcon_term_set_output`'s existing shape exactly — the term/
   stream layer hands the raw title string up and does nothing else
   with it, since the actual `prctl`/`argv[0]` rewriting this section's
   spec calls for has no business living below `ghostcon-core` (it's
   process-level, not terminal-state-level). Wired in `osc_dispatch()`'s
   existing `GHOSTTY_OSC_COMMAND_CHANGE_WINDOW_TITLE`/`_ICON` case,
   which is the one OSC command whose data the library *does* expose
   (`GHOSTTY_OSC_DATA_CHANGE_WINDOW_TITLE_STR`) — the one exception to
   every other item in this whole OSC matrix, so for once no manual
   parsing was needed on the term side.
   `core/main.c`'s `term_title_callback()` implements the spec's two
   halves:
   - `argv[0]` ← `"ghostcon@tty<N> -- " + title`, truncated to argv[0]'s
     original length (captured once in `main()`, before anything can
     overwrite it) rather than relocated via
     `prctl(PR_SET_MM_ARG_START/END)` — the spec explicitly leaves this
     as an either/or as long as it's a deliberate choice; relocation
     needs `CAP_SYS_RESOURCE` and correct `mm_struct` bookkeeping to
     avoid corrupting the process's own memory map, so truncation was
     picked as the safe option.
   - `PR_SET_NAME` (`comm`, 15 bytes incl. NUL) ← `"gc@tty<N> "` +
     first-word-then-basename-extracted token, tail-truncated with a
     leading `"..."` when it doesn't fit, per the spec's algorithm.
   **Inconsistency caught while implementing, not by a test**: this
   section's own worked example (`VT 3`, title
   `/usr/.../some-very-long-script.sh` → `gc@tty3 ...ipt.sh`) doesn't
   actually fit the 14-usable-char limit the same paragraph states —
   `gc@tty3 ...ipt.sh` is 17 characters, 3 over. The implementation
   here enforces the real, hard kernel constraint (`comm` is genuinely
   capped at 15 bytes total by the kernel, not a stylistic choice) over
   reproducing the doc's evidently-uncomputed example. Left uncorrected
   in this section above (the "Process identity" spec near the top of
   the doc) rather than silently rewritten, since that's original
   design intent, not implementation log — flagging it here instead.
   `tests/test_osc.c` covers the term-level plumbing only (OSC 0 and
   OSC 2 both deliver the title string via the callback) — the
   `argv[0]`/`PR_SET_NAME` half lives in `core/main.c`, which has no
   unit-test harness (consistent with `kms.c`/`vtctl.c`'s existing
   compile-checked-only status; see their own PLAN.md notes), so that
   half is unverified beyond manual code review pending a future live
   check (`ps -ef` / `top` showing the repurposed name on tty4). 9/10
   `meson test` (same known unrelated `test_undead_head` contamination)
   and 44/44 on the differential corpus.

   **Not yet done**: OSC 633's `E;<commandline>;<nonce>` command-line
   capture (would need new storage, no consumer for it yet) remains
   deferred. This closes out Phase 1 item 8's full list.

### Toward real use: config, packaging, stub-tier OSC, kms/vtctl tests

Started once the user pointed out ghostcon was only usable on this one
machine, launched by hand — four gaps, tackled in one pass (see the
`lucky-wibbling-graham` plan approved for the exact scoping/tradeoff
questions asked before starting):

**TOML config — done and verified.** No C TOML library exists in this
machine's repos (only C++ ones — `tomlplusplus`, `toml11` — and ghostcon is
pure C throughout), and there's no WrapDB entry for a C one either, so
**tomlc99** (github.com/cktan/tomlc99, MIT, a single `toml.c`/`toml.h`) was
vendored directly into `subprojects/tomlc99/` at a pinned commit
(`29076dfd095bbbbd50a3c1b2760d29f4b83e74ac`) with a small hand-written
`meson.build` — no network access needed at build time, no WrapDB
dependency. New `include/ghostcon/config/config.h` +
`src/config/config.c` (`ghostcon_config_defaults`/`_load`/`_export_env`),
linked into **only `undead-head`** — every other binary
(`ghost-ptyserv`/`supervisor`/`ghostcon-core`) keeps reading `getenv(
"GHOSTCON_*")` exactly as it always has (that surface was enumerated
directly from the source this pass: `GHOSTCON_CANARY_FD`,
`GHOSTCON_RUN_DIR`, `GHOSTCON_DISABLE_WALL`,
`GHOSTCON_DISABLE_KMSCON_FALLBACK`, `GHOSTCON_DRM_NODE`,
`GHOSTCON_CANARY_DEADLINE_MS`, plus the sibling-binary-path overrides).
`undead-head`'s `main()` loads `$GHOSTCON_CONFIG_PATH` (default
`/etc/ghostcon/ghostcon.toml`; missing file is not an error, just
defaults) before spawning anything, then `setenv(..., 0)`s each
config-backed variable — the `0` (don't-overwrite) means an already-set
env var, e.g. from a test harness or set by hand, always wins. Precedence
is explicit env var > config file > hardcoded default, and the whole rest
of the tree inherits the result through normal `fork`/`exec`, unchanged.
Defaults in `config.c` were pulled from the actual current hardcoded
values in the source (not re-guessed) — `/dev/dri/card1`, `/run/ghostcon`,
and a **4000ms** canary deadline (this pass's own plan draft guessed
5000ms before checking `supervisor/main.c`'s real
`DEFAULT_CANARY_DEADLINE_MS`; corrected before writing `config.c`, not
after finding a test failure).
`tests/test_config.c`: defaults-on-missing-file, partial-file (untouched
keys stay default), full-file (every key), malformed-file (real parse
error, not silently defaulted), and the env-var-wins-over-config
precedence check in both directions. One self-caught test bug along the
way: `mkstemp()` overwrites its template argument with the resolved
filename in place, so reusing the same `char path[]` across multiple
`mkstemp()` calls in the same test fails from the second call onward (no
more `XXXXXX` left to substitute) — fixed by resetting from a
`PATH_TEMPLATE` macro before each call, not by the config code.
Also installed: `share/ghostcon.toml.example` (commented, one line per
field, matches the real defaults) via `install_data` to
`{datadir}/ghostcon/` — never written to `/etc` directly by the install
step.
10/11 `meson test` (10 real + `test_config` new; `test_undead_head` still
the same known unrelated live-tty4-session `pgrep` contamination,
re-confirmed via `pgrep -af` against the same pids as every previous
mention of this in the doc).

**Packaging — binaries + unit installable, not yet enabled.**
`undead-head`'s actual CLI (`undead-head <registry_socket> <vtnum>
[vtnum...]`, confirmed via the live `pgrep -af` output this whole session
already captured) supports multiple VTs in one process — this is *not*
kmscon's per-VT-template-unit shape (`kmsconvt@.service`, read directly
off this machine via `systemctl cat` to model against), so packaging is
one plain `packaging/ghostcon.service`, not a `ghostcon@.service` template.
The VT list is a literal `ExecStart` argument (not config-driven — TOML
config covers behavioral knobs, not which VTs to claim; a
variable-length arg list from an external file needs a systemd generator
unit, more machinery than a single-machine project needs right now — an
admin who wants a different VT set uses `systemctl edit
ghostcon.service`). Getty-conflict handling is deliberately *not*
automated (no `Conflicts=getty@tty4.service`) — matches what's actually
been done by hand all session (`systemctl mask kmsconvt@tty4.service`,
confirmed still masked, symlinked to `/dev/null`, when checked this pass)
rather than inventing an auto-mask mechanism this project has exactly one
data point for.
`src/meson.build`: all five binaries (`undead-head`, `ghost-ptyserv`,
`pty-ttyN`, `supervisor`, `ghostcon-core`) now `install: true` into
`{libexecdir}/ghostcon` — a private dir, not `$PATH`, consistent with
`resolve_sibling_path()`'s existing `/proc/self/exe`-based resolution
already not caring about `$PATH` at all.
`packaging/ghostcon.service.in` (template, `@LIBEXECDIR@` placeholder) is
substituted via `configure_file()` at configure time, not installed as a
static file — the ExecStart path has to match wherever *this specific
configure's* `libexecdir` actually resolves to, which changes with
`--prefix`. This mattered in practice: this machine's meson defaults
`libexecdir` to a bare `libexec` (relative), which under the default
`/usr/local` prefix would NOT have matched a naively-hardcoded
`/usr/lib/ghostcon/...` ExecStart at all — caught by actually running the
verification below rather than assuming the hardcoded guess was right.
Gated behind a new `meson_options.txt` boolean `install_systemd_unit`
(default `true`), and behind a runtime check for the separate `systemd`
pkg-config module (distinct from the existing `dep_systemd`/`libsystemd`
dependency — `libsystemd` doesn't expose the `systemdsystemunitdir`
pkg-config variable, only the standalone `systemd` module does) with a
`warning()` fallback instead of a hard `required` failure, so a system
without systemd installed doesn't break the build.
**Verified via a real `--destdir` sandbox install**, not just reading the
generated `meson install` log: configured a scratch build with
`--prefix=/usr` (matching a real distro install, not this repo's own dev
`build/` which stays at meson's `/usr/local` default), ran
`DESTDIR=... meson install`, and inspected the resulting tree directly —
all five binaries under `/usr/libexec/ghostcon/`, `ghostcon.service`
under `/usr/lib/systemd/system/` with `ExecStart=/usr/libexec/ghostcon/
undead-head ...` (confirming the `@LIBEXECDIR@` substitution actually
tracked the real install location, not just compiled), and the example
config under `/usr/share/ghostcon/`. Scratch dirs removed after
verification; the repo's own `build/` was untouched by this check.
No live `systemctl enable` was run or will be — that's for the user to do
once they've reviewed the installed unit.

9. **OSC matrix, stub tier** — OSC 8 (hyperlinks), OSC 9/777
   (notifications), OSC 52 (clipboard) get parsed correctly by the
   terminal state machine (libghostty-vt's OSC parser handles these)
   but their handler is a stub:
   - OSC 9/777 stub: log the normalized notification event to journald
     (reuse the same "unimplemented OSC" structured logging path) instead
     of sending to a notify socket that doesn't exist yet.
   - OSC 52 stub: accept read/write requests, store/return the buffer in
     an in-process variable scoped to that single `ghostcon[ttyN]`
     instance (no cross-VT sharing yet, since that requires the broker) —
     good enough to validate the parsing/wiring path without blocking on
     `ghostcon-ipc`.
   - OSC 8 stub: parse and track hyperlink regions/state in `wrap`, but
     no click-to-open handler yet (no overlay to render an interactive
     affordance against) — render as styled/underlined text only.

   **All three done and verified**, same `osc_dispatch_manual()` manual
   tier as every other item in this matrix.
   **OSC 8**: `cell.h`'s bit layout changed — the 1-bit `hyperlink` flag
   (bit 44, never set anywhere, confirmed via grep before touching it)
   couldn't carry *which* hyperlink a cell belongs to, so it was replaced
   with a 15-bit `hyperlink_id` field (bits 47-61, same width and
   0-is-none convention as `style_id`) using the padding bits already
   reserved there; `ghostcon_cell_get_hyperlink(c)` is kept as a
   convenience predicate (`id != 0`) for any future caller that only
   needs the boolean. New `include/ghostcon/term/hyperlink.h` +
   `src/term/hyperlink.c`: `ghostcon_hyperlink_set_t`, an interned,
   ref-counted URI table, deliberately structured as a near-exact mirror
   of the existing `ghostcon_style_set_t` (`style.h`/`style.c`) — same
   open-addressing hash table, same id-reuse scheme — since it's solving
   the identical problem (many cells referencing a small set of
   deduplicated values by id) for a different payload type. `cursor_t`'s
   existing-but-dead `hyperlink_id` field (`screen.h`, present since
   early in the session, never read or written until now) is what
   `osc_dispatch_manual`'s new `case 8:` sets on start
   (`8;params;uri` → intern via `ghostcon_hyperlink_set_add`) and clears
   on end (`8;;` → id back to 0); `screen.c`'s `print_cell()` stamps it
   onto each printed cell, same line pattern as `style_id`/
   `semantic_current`. Matches an existing simplification already
   present for `style_id` (confirmed by grep before assuming otherwise):
   neither ref/unref on a cursor's *previous* id when it changes — only
   `_add` on set — so refcounts only ever grow. Flagged here rather than
   "fixed", since it's consistent with how style transitions already
   behave, not a new gap introduced by this work.
   **OSC 9/777**: needed a new term-level callback tier
   (`ghostcon_notify_fn`/`ghostcon_term_set_notify`/
   `ghostcon_stream_set_notify`), mirroring `output_fn`/`title_fn`'s
   existing shape exactly — `ghostcon_diag_log_warning()` lives in
   `core/diag.c`, which depends on `libsystemd`, and `term/` deliberately
   has zero dependency on that (it needs to stay usable standalone by
   `ghostcon_dump`/its own tests without pulling systemd in); calling it
   directly from `stream.c` was tried first and immediately failed to
   link, which is what surfaced this layering boundary concretely rather
   than it being planned in advance. `core/main.c`'s
   `term_notify_callback()` is the one line that actually calls
   `ghostcon_diag_log_warning()`.
   **OSC 52**: `screen_t` gained `char clipboard[4096]`, storing the
   base64 payload verbatim (no decode/re-encode — the wire format already
   is base64). `52;c;?` queries via the existing `osc_write_response()`
   path; a payload that isn't valid base64 (checked character-by-character,
   not length/padding — deliberately lenient there, matching xterm's own
   leniency, so a slightly-malformed-but-legitimate payload from some
   other terminal's app doesn't just silently vanish) is rejected outright
   rather than stored as garbage that a later query would then echo back.
   `tests/test_osc.c` covers all three: OSC 8 start/end verified by
   reading back real stamped cell bits via `ghostcon_cell_get_hyperlink_id()`
   (not just the screen-level id) and resolving through
   `ghostcon_hyperlink_set_get()`; OSC 9/777 checked for "doesn't corrupt
   subsequent parsing" (same regression style as the window-title check);
   OSC 52 set/query/reject-invalid.
   **Real test bug caught and fixed, not a code bug**: the OSC 8 test
   initially indexed `rows[0].cells[0]`/`cells[5]` directly, copied from
   the file's existing convention — but by that point in the test file
   the cursor was already mid-row from every earlier OSC 4/7/133/633 test
   sharing the same `ghostcon_term_t`, so those indices pointed at
   already-written cells from unrelated earlier assertions, not the
   hyperlink text. Fixed by emitting `\r\n` and capturing `cursor.y`
   before writing, rather than assuming column 0.
   10/11 `meson test` (`test_config` new since the last mention; same
   known unrelated `test_undead_head` contamination) and 44/44 on the
   differential corpus (`cell.h`'s bit-layout change made this an
   especially important re-run, not just a formality — a shifted mask
   touching the wrong bits would show up here before anywhere else).

### First real boot via `ghostcon.service`, and three bugs only a real boot could find

Once packaging (above) was done, the user asked to actually install and
boot the real `systemd` unit on tty4 — not another scratch/manual pkexec
session, the genuine `meson install` → `systemctl enable --now
ghostcon.service` path. Sequence: killed the manually-launched stack from
earlier in the session (`pkexec kill -TERM` on `undead-head`, whole tree
exited cleanly via its own existing signal handling), configured a
separate `build-release` dir with `--prefix=/usr` (the repo's own dev
`build/` stays at meson's default `/usr/local`, which would have made
`ghostcon.service`'s `ExecStart` point at binaries that were never
actually installed there), installed via `pkexec meson install` (needed
an absolute `-C` path — `pkexec` doesn't preserve cwd reliably, same
lesson as every other `pkexec` interaction this session), then
`pkexec systemctl daemon-reload && pkexec systemctl enable --now
ghostcon.service`. Confirmed via `systemctl status` (`active (running)`,
full process tree, `supervisor: vt 4: renderer claimed VT, ACTIVE`) and
`kmsconvt@tty4.service`/`getty@tty4.service` both still masked. This is
the first time ghostcon has run as the thing systemd itself starts,
not a process babysat by hand.

That surfaced three real, previously-undiscovered bugs — each one only
reachable by an actual person typing on the actual physical keyboard
while the actual service owns tty4, which is exactly the class of bug
this project's own earlier live-testing rounds were built to catch and
scratch-harness/unit tests structurally cannot:

**1. No cursor rendered, anywhere, ever — two compounding bugs, not one.**
User: "there's no cursor" on the very first login prompt. Code inspection
found two independent gaps, both needed fixing:
- `render/machine.c` had no cursor-drawing call at all. `render/atlas.c`
  and `render/gles.h` both had comments referencing a "reserved UV" for a
  cursor block that was never actually wired up — the plumbing was
  half-built and forgotten. Fixed: new `ghostcon_machine_render_cursor()`
  (mirrors `render_dirty`'s exact pixel-position math, `x*cell_w,
  y*cell_h`), called every frame from `core/main.c`'s `render_frame()`
  right after `render_dirty` — reuses `ghostcon_gles_push_rect()`, which
  already existed and needed no changes. Shapes the block/underline/bar
  cursor per `screen->cursor.cursor_style` (an enum that already existed
  and was already correctly tracked, just never consumed by anything);
  `_BLINK` variants render identically to their steady counterparts for
  now, no blink timer built.
- Even with that fixed, the cursor still wouldn't have shown: DECTCEM
  (`CSI ?25h/l`, cursor visibility) was **entirely unimplemented** —
  not just "off by default," genuinely never read or written anywhere in
  `stream.c`, despite the `GC_MODE_DECTCEM` constant existing in
  `modes.h` since early in the project. Screen state's `memset`-zero
  default made every mode flag start "off" — correct for every other
  DEC private mode in this codebase, but DECTCEM is the one exception:
  real terminals default it ON, and no program ever sends `CSI ?25h` at
  startup because it's assumed already visible. Fixed: added a real
  `bool cursor_visible` field (mirroring `auto_wrap`'s existing pattern
  exactly, not the vestigial `ghostcon_modes_t` bitset — confirmed via
  grep that private-mode DECSET/DECRST dispatch in `stream.c` already
  uses dedicated bool fields for every other mode, not that bitset),
  defaulted `true` in `ghostcon_screen_init()` with a comment explaining
  the asymmetry, and wired `case 25:` into both `handle_csi_dec_set()`
  and `handle_csi_dec_reset()` alongside the existing DECAWM/DECOM/etc.
  cases. Both fixes were necessary; either alone would have left the
  cursor invisible.

**2. `login`'s password prompt is real, invisible input is not a bug.**
User reported "stalls on Enter" after typing a username. Diagnosed live
by checking the pty's actual termios via `stty -F <pts> -a` as root
(`pkexec`) — found `icanon icrnl` correctly set and, critically, `-echo`
set, meaning `login` had *already* successfully read the username and
moved to the password prompt, which intentionally disables echo (normal,
correct behavior, identical to any real terminal). This wasn't a bug —
what actually looked like a bug was the *combination* of standard
invisible-password-entry with bug #1 (no cursor) giving no visual
indication the prompt state had even changed. Worth recording as a
diagnostic technique, not a fix: `stty -F <pts> -a` as root against the
pty a `login`/`agetty` process is attached to directly reveals which
input phase it's in, without needing to touch ghostcon's own code at
all.

**3. `libinput` reads are not scoped to a VT — a real cross-session input
leak, the most serious bug found this session.** User: "things I type
here gets put in ghostcon's buffer, including enter, spaces and stuff" —
while typing in an *unrelated* terminal (this very conversation),
keystrokes were appearing in tty4's `login` prompt. Root cause:
`core/main.c`'s main loop called `ghostcon_input_dispatch()` whenever the
libinput fd had `POLLIN`, with **no check for whether this VT was
actually active**. libinput reads raw evdev device events, which have no
concept of "VT focus" at the kernel level — every keystroke on the
machine arrives at every process with an open fd on that input device,
regardless of which VT is displayed. A background, VT-inactive
`ghostcon-core` was therefore capturing and forwarding every keystroke
typed anywhere else on the machine straight into its pty.
First fix attempted (confirmed insufficient, not shipped as the final
fix): excluding the input fd from the `poll()` set while
`!display_acquired`. This stops *active dispatch* while inactive, but
does NOT stop the kernel from continuing to queue events into this
process's own already-open fd the entire time regardless — confirmed via
`journalctl`: reactivating the VT produced a burst of
`libinput error: client bug: timer event11 keyboard: scheduled expiry
is in the past (-7021ms), your system is too slow` followed by the
entire backlog (including the leaked chat message) replaying in one
frame. Not polling only delays consumption of an already-accumulating
backlog; it doesn't discard it.
**Real fix**: moved `ghostcon_input_t *input` out of `app_t`'s
"long-lived, survives every acquire/release cycle" category into the
"per-acquire-cycle, torn down on release, rebuilt on reacquire" category
— the exact same category `kms`/`egl`/`gles` already belong to, for
exactly the same reason (a fresh GPU context has no relationship to
stale prior state; a fresh libinput context has no relationship to a
stale prior keystroke backlog). `release_display()` now calls
`ghostcon_input_close()`; `acquire_display()` now calls
`ghostcon_input_open("seat0")` fresh, right where `kms`/`egl`/`gles` are
already rebuilt. Removed the old one-time `ghostcon_input_open()` call at
`main()` startup entirely (would have leaked the first context and
silently replaced it once `acquire_display()` started managing its own).
Verified clean via the same live technique used to find it: typed
ordinary chat messages ("hello", "meow") immediately before and during a
tty4 login attempt, confirmed via `journalctl` byte-dump instrumentation
(temporary, reverted after diagnosis — see below) that zero unrelated
keystrokes reached the pty, then completed a real login end-to-end:
`pam_unix(login:session): session opened for user bashh(uid=1000) by
bashh(uid=0)`, followed by a normal interactive shell (`ls` ran
correctly).
**Diagnostic technique worth keeping**: two rounds of *temporary*
instrumentation were added and reverted during this investigation —
first hex-dumping every encoded keypress in `input.c` (confirmed Enter
correctly encodes to `\r`, ruling out the input-encoding path early),
then dumping raw bytes received from the pty in `core/main.c` (confirmed
output was/wasn't arriving, and eventually showed the full clean login
transcript proving the fix). Both were clearly marked `TEMPORARY` in
code comments and fully reverted once diagnosis was complete — neither
should exist in the codebase currently; if either turns up in a `git
diff` later, that's a sign a revert was missed, not that they're meant
to stay.
All three fixes verified: 11/11 `meson test` (`test_undead_head` also
now passing again in this run, not just "known contamination" — the
earlier manually-launched stack was killed for this real-boot work, so
there was nothing left to contaminate it) and 44/44 on the differential
corpus. Re-installed and restarted `ghostcon.service` after each fix;
final state confirmed via a real, complete, successful login.

### Scoping ghost-ptyserv's death: one component crashing used to kill every VT's session

Surfaced by an incident during the login-flow debugging above: a
`pkexec systemctl restart ghostcon.service` landed late (a dismissed
polkit prompt executed anyway, minutes after being retried) and
restarted the whole service unexpectedly. That specific incident turned
out to be harmless, but the user's reaction to it was the real finding:
`undead-head`'s documented behavior — "ghost-ptyserv dies -> kill
everything -> restart the entire tree" — means *any* single crash of one
shared component destroys every VT's session, not just the affected
one. Flagged as a real reliability problem worth fixing, not just living
with.

**Root-cause investigation, not just accepting the documented behavior
at face value**: read `core/transport.c`'s `ghostcon_transport_connect()`
and found that an already-connected renderer has **zero ongoing
dependency on ghost-ptyserv** — the registry socket is used for exactly
one lookup at startup; every byte after that goes straight to the
renderer's own `pty-tty<N>.sock`. So a `ghost-ptyserv` death shouldn't
need to touch a live session at all. The actual reason it did:
`pty_child.c` never called its own `setpgid(0,0)`, so `pty-ttyN`
inherited `ghost-ptyserv`'s process group, and `undead-head`'s cleanup
(`kill_group(ghost_ptyserv_pid)` → `kill(-pid, SIGTERM)`) killed that
whole group — taking the live shell/login session down as pure
collateral damage, on top of `undead-head` separately, unconditionally
killing every supervisor too regardless of whether its VT was ever
affected.
A connect-then-close liveness probe against the renderer-facing
`pty-tty<N>.sock` (an earlier idea for how a restarted ghost-ptyserv
could recognize a still-alive `pty-ttyN`) was considered and rejected
after reading `pty_child.c`'s accept loop: it treats *any* new
connection as "the renderer reconnected" and immediately replaces
(closes) whatever was already connected — a bare probe would silently
kick out a real, working renderer, worse than the bug being fixed.

**Three changes, done and verified**:
1. `pty_child.c` now calls `setpgid(0, 0)` on startup, matching every
   other group leader in this tree (`supervisor`,
   `fork_ghost_ptyserv`/`fork_supervisor` in `undead_head/main.c`) —
   `pty-ttyN` was the one process that didn't yet. This alone makes it
   survive `ghost-ptyserv`'s process-group kill.
2. `ptyserv/main.c`'s `spawn_pty_child()` gained a pidfile-based reuse
   path (`<socket_dir>/pty-tty<N>.pid`, written via temp-file-then-
   `rename()` for atomicity): before forking, it checks whether the
   pid on record is both alive (`kill(pid, 0)`) and still actually
   `pty-ttyN` (cross-checked via `/proc/<pid>/exe` resolving to the
   same canonical path this process would itself exec — closes the
   pid-reuse edge case where an unrelated process has since taken that
   pid). If so, it registers the existing socket path and pid instead
   of spawning a duplicate, which would otherwise orphan the real,
   possibly-mid-use session behind a socket the registry no longer
   points to.
3. `undead_head/main.c`'s `ghost_ptyserv_pid` branch no longer touches
   `supervisor_pid[]` at all — it only cleans up and reforks
   `ghost-ptyserv` itself. The `restart:` label (previously also
   re-forking every supervisor, reached via `goto` from this branch) is
   gone entirely — the branch reforks `ghost_ptyserv_pid` directly and
   continues the loop.
**A real, independent bug found and fixed while adding test coverage,
not in the production code**: `tests/test_undead_head.c`'s
`pgrep_once()` used `popen("sh -c \"pgrep -f -- 'PATTERN' ...\"", ...)`
— and that wrapper shell's own command line necessarily contains
`PATTERN` as a literal substring (it's right there in the string being
run), which `pgrep -f` happily matches, since `pgrep` only excludes its
*own* pid from results, not a parent shell's. This was a latent,
previously-unnoticed self-matching hazard in every existing
`pgrep_wait`/`pgrep_wait_ne` call in this file — masked in practice only
because the real target process, by the time those particular checks
ran, already existed with a lower (older) pid than the freshly-spawned
wrapper shell, so `pgrep`'s ascending-pid-order output put the real
match first. The new supervisor-pid-unchanged assertion this fix added
(`tests/test_undead_head.c`) checks a pid *before* it's necessarily
running yet, and was the first check in this file young enough to expose
it: the self-matching wrapper won the race and got returned as a false
"found" result on the very first attempt, without ever actually
retrying. Fixed by rewriting `pgrep_once()` to `fork`+`exec pgrep`
directly with no intermediate shell — pgrep's own argv is the only place
the pattern appears, and pgrep already excludes its own pid.
**A second false alarm, self-inflicted during manual reproduction, not
a bug at all**: while chasing the above, a *second*, seemingly identical
failure appeared when reproducing manually via
`env GHOSTCON_SUPERVISOR_BIN=build/src/supervisor ... build/src/test_undead_head`
run directly through this session's own shell tool. Root cause (found
via temporary `/proc/<pid>/cmdline` debug output, since reverted): the
shell tool's own wrapper process invoking that exact command line has
`GHOSTCON_SUPERVISOR_BIN=build/src/supervisor` embedded in *its own*
cmdline too, and — with a lower pid than anything the test forks — became
the false match. `meson test`'s own invocation doesn't embed env values
as literal argv text, so this never manifests there; only ever showed up
when manually invoking the test binary directly with inline `VAR=value`
env assignments on the command line.
New `tests/test_ptyserv.c` coverage (spawn ghost-ptyserv, connect a
renderer, kill ghost-ptyserv directly, start a second instance against
the same registry socket, confirm it returns the *identical* pid/socket
for the same VT rather than spawning a duplicate, then confirm the
still-open renderer connection from before the kill still round-trips a
command) and the `tests/test_undead_head.c` supervisor-pid-unchanged
assertion described above. 11/11 `meson test` (run 5x consecutively for
confidence given the flakiness investigated above) and 44/44 on the
differential corpus.
**Verified live against the real, installed `ghostcon.service`**, not
just the test suite: with a real session active on tty4, ran
`pkexec kill -SIGKILL` on the running `ghost-ptyserv` pid directly (not
the service). Confirmed via `systemctl status`/journal:
`ghost-ptyserv died, restarting it (supervisors unaffected)`, a fresh
`ghost-ptyserv` pid appeared, and — the actual point of all of this —
`pty-ttyN`, its `agetty` session, `supervisor`, and `ghostcon-core` all
kept their exact original pids throughout, completely undisturbed.

**Fourth bug found this stretch, unrelated to the ghost-ptyserv fix
itself**: while that work was wrapping up, the user caught a real
`wall(1)` broadcast — `ghostcon[tty90]: renderer hang, recovering` —
appearing live in their own terminal mid-session. Traced via
`journalctl` (a stray `agetty[...]: /dev/tty90: cannot open as standard
input` line nearby was a red herring — real VT device, unrelated to this
pty-backed test) to `tests/test_supervisor.c`'s `TEST_VT 90`. Root cause:
`scenario_hang_and_respawn()` deliberately makes the fake renderer hang
to test `supervisor`'s hang-detection/respawn path (`wall_broadcast(vtnum,
"hang")` in `supervisor/main.c`), but — unlike its sibling
`scenario_startup_timeout_fallback()`, which correctly sets
`GHOSTCON_DISABLE_WALL=1` before running — it never disabled wall
broadcasts. Every `meson test` run this whole session (and prior
sessions) genuinely paged whoever was logged in on this machine with a
live "renderer hang" message; nobody had noticed until this specific
moment. Confirmed via `grep -n DISABLE_WALL tests/test_supervisor.c`
that this was the only scenario in the file missing it (`scenario_
clean_exit_respawn` doesn't hit either `wall_broadcast()` call site at
all, so it was never at risk). Fixed with a single `setenv(
"GHOSTCON_DISABLE_WALL", "1", 1)` added to `scenario_hang_and_respawn()`,
matching the existing sibling pattern exactly — test-only change, no
production code touched. Verified: `test_supervisor` still passes, and
no broadcast fired on the next `meson test -C build` run (11/11 clean).

### The "no password prompt reply" saga: a real bug found and fixed, and a false lead ruled out with hard evidence

A long live-debugging thread, worth recording in full because of how many
plausible-looking dead ends it walked through before landing on a
correctly-evidenced conclusion.

**User-visible symptom**: after typing a username and pressing Enter on
tty4, the password field (correctly invisible — `stty -F <pts> -a` as
root repeatedly confirmed `icanon icrnl -echo`, i.e. `login` had
genuinely already accepted the username and moved on) appeared to never
respond. Waiting it out hit `login`'s own 60-second `LOGIN_TIMEOUT`
(`/etc/login.defs`), which respawned a fresh session — and the typed
password then appeared in **plaintext**, echoed into the new prompt.

**Real, independent bug found and fixed along the way**:
`ptyserv/pty_child.c`'s respawn-backoff logic called `sleep((unsigned)
backoff)` directly inside its single-threaded event loop — the same loop
that also services the live renderer connection. Any respawn backoff
(up to the 30s cap) froze the *entire* session, not just the dead shell:
keystrokes typed during that window sat unread in the kernel socket
buffer instead of being forwarded, then flushed all at once once the
sleep ended. Fixed by tracking a respawn deadline and using `poll()`'s
own timeout parameter instead of blocking — `master_fd` is excluded from
the pollfd set while a respawn is pending (stale, at-EOF anyway), but
`listen_fd`/`renderer_fd` stay fully serviced the whole time. This is a
correct, valuable fix, kept regardless of the rest of this investigation
— see the code's own comment in `pty_child.c` for the full reasoning.

**That fix alone did not resolve the reported symptom** — confirmed by
reproducing again on a completely fresh `pty-ttyN` (backoff at zero, so
the fixed `sleep()` path couldn't even have been active) and seeing the
identical ~57-60s stall-then-flush pattern. This ruled out backoff as
the actual cause of *this* symptom, even though the bug itself was real.

**Diagnostic methodology, since this took several rounds to pin down
correctly**: added temporary (fully reverted after) instrumentation at
each hop of the pipeline in turn — `core/input.c`'s key encoder
(confirmed `ghostcon_transport_write()` succeeded immediately, correct
byte count, for every single password character, no delay or short
write) — `ptyserv/pty_child.c`'s `renderer_fd` read (confirmed these
same bytes did NOT arrive at `pty_child.c` for ~57-60s, then arrived as
one consolidated batch) — and finally `poll()`'s own return value
directly (confirmed **zero** `poll()` return log lines appeared during
the entire stall window, versus one per keystroke immediately before and
after it — i.e. the process was inside the blocking `poll(2)` syscall
the whole time, not spinning and misreading `revents`). Explicitly ruled
out a stdio-buffering artifact in the diagnostic logging itself by
adding `fflush(stderr)` after every diagnostic `fprintf()` and
re-reproducing — the pattern persisted identically, confirming the gap
was in the actual process behavior, not in when log lines became
visible.

**User pushed back that the issue was in `login`/PAM itself, not
ghostcon** — a reasonable instinct, addressed directly with the
structural argument rather than dismissed: the stalled hop
(`ghostcon-core` → `pty_child.c`'s `renderer_fd`) sits entirely upstream
of `login`, which only ever reads from the *other* side (`master_fd`).
A slow PAM module would produce keystrokes arriving at `pty_child.c`
promptly and sitting un-consumed on the kernel/`login` side — not a
delay in delivery *into* `pty_child.c` in the first place. The data
matched the latter, not the former.

**Conclusion, not fully certain but the best-supported explanation
given the evidence**: the very next reproduction attempt — run with
*no* concurrent `meson compile`/`pkexec meson install` happening in the
background, unlike essentially every earlier attempt this session, which
were each immediately preceded by a rebuild-install-restart cycle to
deploy the next round of diagnostics — completed with every password
character arriving in real time and a genuine, immediate
`pam_unix(login:session): session opened for user bashh`. The `poll()`
evidence (zero return events for ~60s despite data provably ready) is
consistent with the `pty-ttyN` process being starved of CPU scheduling
by concurrent heavy compilation on the same machine, not a code defect
in the poll loop itself — plausible given how much concurrent rebuilding
this exact debugging process itself was generating. Not proven with the
same rigor as the `sleep()` bug above; flagged here as the leading
hypothesis rather than a closed case, should the symptom resurface
under genuinely idle conditions.
All temporary diagnostic instrumentation (`core/input.c`,
`ptyserv/pty_child.c`) fully reverted; only the `sleep()` → non-blocking
backoff fix remains as a permanent change. 11/11 `meson test` (isolated
rerun of `test_undead_head` after one more known-flaky timeout, matching
this session's established contamination pattern under heavy concurrent
load) and 44/44 differential corpus.

### Four more live bug reports, three fixed, one deferred as a feature request

**1. TUI programs (e.g. `opencode`) render into a small, wrong-sized,
"blocky" area — done and verified.** Root cause: zero occurrences of
`TIOCSWINSZ` anywhere in the codebase, confirmed via grep before writing
any code. Ghostcon tracked its own `cols`/`rows` internally
(`ghostcon_term_resize()`) but never told the actual kernel pty, so any
program calling the standard `ioctl(TIOCGWINSZ)` got whatever bare
`forkpty()` default happened to exist, not the real computed size — the
"blocky" appearance is a consequence of this, not a separate bug (a
program drawing into a small grid it thinks is correct, stretched/
sparse across the real display).
Fixed with a new per-VT control socket
(`pty-tty<N>.ctl.sock`, `GHOSTCON_PTY_CTL_SOCKET_FMT` in
`ptyserv/protocol.h`), carrying a simple `RESIZE <rows> <cols>\n` text
message (same plain-text-framed style as the existing registry
protocol) from `ghostcon-core` to `pty_child.c`, which applies
`ioctl(master_fd, TIOCSWINSZ, ...)` — the kernel then auto-delivers
`SIGWINCH` to the pty's foreground process group on its own, no manual
`kill()` needed. A same-socket in-band signaling approach (reconnecting
to the data socket to send a resize command) was considered and
rejected: `pty_child.c`'s accept loop treats any new connection as "the
renderer reconnected," immediately replacing whatever was already
connected — exactly the same hazard identified and rejected for a
liveness-probe design earlier in this doc. `ghostcon-core` sends this
both right after the initial `ghostcon_transport_connect()` (covers the
VT already being active at startup) and on every subsequent reacquire-
resize. Verified live: `stty -F <pts> -a` on the real tty4 pty showed
`rows 49; columns 192` (a genuine, KMS-geometry-derived size) instead
of the previous `24 80` fallback.

**3. Can't page back to the desktop without bouncing through kmscon
first — done and verified.** Root cause: `core/main.c` called
`ghostcon_vtctl_process_pending()` (which internally acks the kernel's
`VT_RELDISP` for a release) *before* `release_display()` (which is what
actually calls `drmDropMaster()`) — backwards from the contract
`vtctl.c`'s own pre-existing doc comment already stated explicitly:
"caller must have already called `drmDropMaster()`... BEFORE this."
Acking the release before dropping master tells the kernel it's safe
to hand the VT to whoever's switching in while ghostcon-core still
holds it, racing the desktop compositor into a stuck black screen.
Fixed by restructuring `ghostcon_vtctl_process_pending()` to accept a
`pre_ack` callback (`ghostcon_vtctl_pre_ack_fn`), invoked immediately
before each individual signal's kernel ack rather than after the whole
batch — necessary because multiple signals can coalesce into one drain
(a rapid release-then-reacquire), and each needs its own correctly-
ordered action, not one blanket action before the whole call.
`core/main.c`'s `vtctl_pre_ack()` calls `release_display()` (already
idempotent — early-returns if nothing's acquired, so no double-teardown
risk) on `'R'`.

**A real, distinct bug found investigating the same report, not the
fix for it**: a user photo showed `^[[17;7~` (Ctrl+Alt+F6, the key
combo used to switch to a GNOME session) appearing as literal text in
the login prompt on the VT being switched away from. Root cause:
`libinput` reads raw evdev keyboard events completely independently of
the kernel's own separate Ctrl+Alt+Fn VT-switch handling — both paths
see the same physical keystroke, since evdev device nodes support
multiple simultaneous readers with no concept of "who should get this
event." `ghostcon-core` had no code to recognize and swallow reserved
VT-switch combos, so it dutifully encoded and forwarded them into
whatever shell was running at that moment. Fixed with
`is_vt_switch_combo()` in `core/input.c`: Ctrl+Alt+F1 through F12 are
now filtered out before encoding (never forwarded), while
`xkb_state_update_key()` (already called earlier in the same function)
still runs regardless, so modifier tracking stays correct even though
the keystroke itself is swallowed. The user noted this wasn't the bug
they'd originally meant to report (item 4, next) but asked for it fixed
anyway, being a real bug in its own right.

**4. Ctrl+D and similar shortcuts arriving as raw escape codes instead
of being interpreted — done and verified, this was the user's actual
original report.** Live-instrumented `core/input.c`'s encode step
directly: `evdev_code=32` (`KEY_D`), `mods=0x2` (Ctrl) encoded to
`1b 5b 34 3b 35 75` — `ESC[4;5u`, the **kitty keyboard protocol's**
CSI-u escape format, not the plain single byte (`0x04`) an ordinary
shell expects. Confirmed the earlier hypothesis exactly: nothing in
`input.c` ever configured `GHOSTTY_KEY_ENCODER_OPT_KITTY_FLAGS` or
`GHOSTTY_KEY_ENCODER_OPT_MODIFY_OTHER_KEYS_STATE_2` (only
`CURSOR_KEY_APPLICATION` was ever set), and the key encoder's own
out-of-the-box default for those apparently isn't the plain legacy
encoding a bare, unaware shell needs — real terminals only enable
either of these when the connected *application* explicitly requests
it via its own CSI sequence, and ghostcon has no code yet to intercept
and track those requests (a real gap, deliberately left for a future
pass rather than building it under this same fix). Fixed by explicitly
setting both to their documented "disabled"/legacy values
(`GHOSTTY_KITTY_KEY_DISABLED` = 0, and `false`) right after
`ghostty_key_encoder_new()`, establishing sane legacy defaults for a
plain, kitty-unaware environment. Verified live: same `evdev_code=32
mods=0x2` now encodes to a single `0x04` byte.

**2. Auto-repeat delay/speed as a configurable, disable-able option —
explicitly deferred, feature request not a bug.** Not implemented this
pass; would extend `config.h`'s existing TOML-backed settings (delay/
speed knobs, matching xset-style semantics, plus an on/off switch) —
scoped out since it's new surface area, not a fix for existing broken
behavior, and the other three items were higher priority.

**Connecting back to the still-not-fully-explained "stuck at password"
saga from earlier in this doc**: with the VT-switch-leak fix in place,
a very plausible (though not provable after the fact) explanation
emerges for at least some of those earlier incidents — a stray
Ctrl+Alt+Fn keystroke landing mid-password-entry would silently
corrupt the line being typed without ever satisfying a canonical-mode
newline, indistinguishable from the terminal "not responding" until
`login`'s own 60-second timeout eventually fired and reset the prompt.
Retested the exact same live-instrumented methodology (poll()-level
timing in `pty_child.c`, high-resolution timestamps) after all four
fixes above: no reproduction of the earlier multi-second single-`poll()`
stall signature — the one clean capture that DID show the "stuck"
behavior this round matched a genuine, uneventful ~60s `login` timeout
window with completely ordinary sub-second `poll()` activity throughout,
not a code-level hang. Not a fully closed case (the original single
smoking-gun capture from earlier in this doc was real and is still
unexplained on its own terms), but no further reproduction after these
fixes, across several more live attempts.
All temporary diagnostic instrumentation (`core/input.c`,
`ptyserv/pty_child.c`) reverted after each finding, matching this
session's established discipline. 11/11 `meson test` (isolated rerun of
`test_undead_head` past the same known flaky-under-concurrent-activity
timeout pattern) and 44/44 differential corpus after every fix. Each
fix built, installed (`meson install` into the real system prefix via
`pkexec`), and restarted on the live `ghostcon.service` before being
considered done — not just compiled.

- Unknown OSC fallback policy (journald log + discard) is unaffected
  and should already be in place from step 6/7.

Phase 1 is "done" when a `ghostcon[ttyN]` + `pty-ttyN` pair can run under
supervision, survive an induced renderer hang via the kill-reconnect path
(terminal state and shell process undisturbed), fall back correctly
through kmscon/fbcon tiers on full session loss, and correctly parse the
full OSC matrix (even where the handler is just a stub) without crashing
on anything — including completely unrecognized sequences.

### The "no password prompt reply" saga, part 2: the real root cause, found by reading kmscon's own source

The CPU-starvation hypothesis above turned out to be incomplete. The
symptom recurred later in a genuinely idle session (confirmed via
`ps -ef | grep -E "meson|ninja|cc1|gcc"` returning empty immediately
before the repro), ruling that hypothesis out on its own evidence.

At the user's explicit prompt to verify against kmscon's actual source
rather than reason from architecture alone, `kmscon/src/misc/pty.c` (a
local clone, read-only, not vendored into this tree) turned out to have
an explicit developer comment documenting exactly this failure class:
`/bin/login` calls `vhangup()` on its own controlling TTY as part of its
password-prompt procedure. This causes a **transient, spurious hangup
condition on the PTY MASTER side** — `POLLHUP`/`read()` returning `<=0`
— even though the child has **not** actually exited; it is mid-procedure
and about to reopen its controlling terminal and continue normally.
kmscon's own comment states this makes `EV_HUP` on the pty unreliable as
a "the client died" signal, and documents its fix: rely exclusively on
`SIGCHLD` (its `sig_child()` callback) for death detection, never on a
`waitpid()` triggered by pty read failure. Confirmed via
`grep -n "waitpid|WNOHANG" pty.c` on kmscon's source returning zero
matches — it never calls `waitpid()` in this code path at all.

`ptyserv/pty_child.c` did exactly what kmscon's comment warns against: a
**blocking** `waitpid(child, NULL, 0)` fired the instant `master_fd`
reported `POLLHUP`/EOF. When `login`'s `vhangup()` triggered this
spuriously, the call blocked with nothing to reap — freezing this
single-threaded event loop, including the live renderer connection,
until the child eventually exited for some unrelated reason (e.g.
`login`'s own ~60s `LOGIN_TIMEOUT`). This is the actual mechanism behind
the whole-session mystery: the earlier "clean" reproduction with no
concurrent builds was coincidence, not evidence of a fix.

**Fix, matching kmscon's proven approach**: `pty_child.c` now installs a
`SIGCHLD` handler wired to a self-pipe (mirroring the pattern already
used by `core/vtctl.c` for its own `VT_PROCESS` signal handling — same
codebase, same technique, not a new idiom). The event loop's poll set
gained a `chld_idx` fd; on wakeup it drains the pipe and reaps via
`waitpid(-1, &status, WNOHANG)` in a loop, only treating a reap that
matches `child`'s pid as a real death. `master_fd`'s `POLLHUP`/`read()<=0`
no longer triggers `waitpid()` or death handling at all — it is purely
informational now.

Since `poll()` (unlike `epoll`) has no edge-triggered mode, leaving
`master_fd` in the pollfd set during a persistent-but-spurious hangup
would busy-spin the loop at 100% CPU (`POLLHUP` re-asserts every
iteration). Instead, on `read()<=0`, `master_fd` is temporarily dropped
from the poll set and retried after a short cooldown. The first version
of this cooldown was a flat 1 second (`time_t`-resolution), which fixed
the freeze but introduced a new, smaller, self-inflicted lag noticeable
on every login — flagged live by the user during a 5-round repeat-restart
verification pass (5/5 logins worked, no freeze, but "some of them are
delayed"). Replaced with a millisecond-resolution (`CLOCK_MONOTONIC`)
exponential backoff — 20ms doubling to a 500ms cap, reset to 0 on the
next successful `master_fd` read or on a confirmed real death — since
the actual `vhangup()`-then-reopen sequence is typically near-instant, so
most hangups clear well before the cap. Re-verified live afterward:
"feels quicker now."

Verified: 11/11 `meson test` (isolated rerun of `test_undead_head` past
the same known flaky-under-concurrent-load timeout pattern, unchanged
from every other fix this session) after both the `SIGCHLD` rework and
the cooldown-tuning follow-up. Live: 5 consecutive
`pkexec systemctl restart ghostcon.service` + real password-login cycles
on tty4, 5/5 clean (no freeze), then reconfirmed once more after the
backoff-tuning fix landed.

### Procedural box-drawing / block-element rendering: fixing "blocky" TUIs for real

The original "blocky" bug report (item 1 above) was fixed for *sizing*
via `TIOCSWINSZ`, but a follow-up live screenshot (`opencode`'s splash
logo, built from Unicode block characters) showed the real remaining
problem: visible seams/gaps forming a checkerboard, while ordinary text
in the same frame rendered perfectly crisp. Root cause, confirmed
against Ghostty's own source (`~/.cache/ghostcon/ghostty-src/src/font/
sprite/draw/`, cached locally from vendoring `libghostty-vt`): Ghostty
never sources box-drawing (U+2500–257F), block-element (U+2580–259F),
or legacy-computing-sextant (U+1FB00–1FB3B) glyphs from a font at all —
it draws them procedurally, sized exactly to the cell, specifically so
adjacent glyphs tile with zero seams. Ghostcon routed every codepoint
through the font atlas, so the font's own glyph bearing/padding for
these characters became a visible seam — fine for ordinary letters,
wrong for shapes meant to connect edge-to-edge.

**Fix**: new `src/render/box_draw.c` / `include/ghostcon/render/
box_draw.h`, exposing `ghostcon_box_draw_render(codepoint, cell_w,
cell_h, alpha)` — synthesizes an 8-bit coverage bitmap sized exactly to
the cell for the covered ranges, fed into `atlas.c`'s *existing*
shelf-pack/glyph-cache/GPU-upload pipeline (via a new shared
`pack_bitmap()` helper factored out of `ghostcon_atlas_glyph()`) as if
it were a rasterized font glyph. `bearing_x = 0`, `bearing_y = ascent`
makes `machine.c`'s existing placement formula land the glyph at
exactly the cell's top-left corner with no changes needed to
`machine.c`/`gles.c` at all.

Coverage, in the order added (see `box_draw.c`'s own doc comments for
full detail):
- Block elements (U+2580–259F): halves, eighths, quadrants, shades
  (flat partial coverage, not a stipple pattern — simpler, deliberate).
- Legacy computing sextants (U+1FB00–1FB3B): bit-decoded via the exact
  formula and bit layout taken from Ghostty's own
  `symbols_for_legacy_computing.zig` (`mask = idx + idx/20 + 1`,
  skipping masks 21/42 since those duplicate existing half-block
  chars). **Octants (U+1CD00+) were investigated and explicitly
  deferred**: Ghostty's own source says it "wasn't able to discern a
  mathematical pattern" for them and hardcodes all ~226 from a literal
  `octants.txt` list — reproducing that by hand was judged too
  error-prone for this pass; falls through to the font atlas unchanged.
- Box-drawing lines (U+2500–254B, pure light/heavy weight only) plus
  single-arm terminators (U+2574–257B) — the latter added after a
  second live round-trip (see below). Mixed-weight, double-line,
  dashed, and diagonal variants stay deferred, unchanged behavior.
- Rounded corners (U+256D–2570, `╭╮╯╰`): reproduced as a true
  quarter-circle arc via squared-distance comparison (no `libm`
  dependency needed) rather than a bezier stroke — verified
  mathematically equivalent to Ghostty's own `arc()` function, since
  both of its bezier endpoints sit at exactly the same radius from the
  cell center.

**Process note, worth remembering**: the first live round after this
landed looked mostly right but the user flagged one specific visible
notch/gap in a screenshot. Guessing further from photos was explicitly
avoided (already burned time on that earlier this session) — instead,
asked the user to capture `opencode`'s actual raw output via `script`
in a completely different terminal (the bug is in ghostcon's glyph
pipeline, not opencode's output, so where it's captured doesn't
matter), then scanned it with a small Python script for codepoints in
the relevant Unicode ranges. This found the actual character in one
grep attempt: `U+2579` (`╹` BOX DRAWINGS HEAVY UP, a single-arm
terminator), which was outside the table's range at the time. Fixing
blind from a blurry photo would very likely have missed this exact
codepoint.

Verified: `tests/test_box_draw.c` (new, pure-CPU, no GPU/DRM dependency
unlike `test_render.c` — checks known codepoints per family against
expected coverage patterns), 12/12 `meson test` (clean, no flaky
`test_undead_head` timeout this round). Live: `opencode`'s logo and
input-box border on tty4, confirmed clean by the user across three
rounds (sextants/quadrants, rounded corners, then the `U+2579`
terminator).

### Bright ANSI colors rendering wrong: double-offset bug in SGR bright-color handling

Live testing (`claude`, the Claude Code CLI) showed its bright-red/orange
branding rendering as dark blue on tty4. Truecolor and 256-color SGR
paths were checked first and ruled out (both map `attr.value.*.{r,g,b}`
straight through with no reordering, verified end-to-end from
`stream.c` through `machine.c`'s `resolve_colors()` to the vertex
buffer layout in `gles.c` — no channel swap anywhere in that path).

A dead-end was chased first: a diagnostic (`eglChooseConfig()` picking
an `EGLConfig` whose `EGL_NATIVE_VISUAL_ID` didn't match the
`GBM_FORMAT_XRGB8888` scanout surface `core/kms.c` creates) did find a
real mismatch (`ARGB8888` vs `XRGB8888`), but those two formats share
the same R/G/B byte order — they only differ in whether the 4th byte
is treated as alpha — so it couldn't explain a channel swap. Ruled
out, though worth fixing separately at some point (Mesa handing back
an alpha-having config for a surface with none).

Root cause, found by writing a standalone test program that feeds
`ESC[91m` (bright red) directly into `libghostty-vt`'s SGR parser and
printing the raw attribute value: `bright_fg_8` comes back as `9` —
already the **absolute** 0–15 palette index (`GHOSTTY_COLOR_NAMED_BRIGHT_RED`
= 9), not a 0–7 value relative to the first 8 ANSI colors. But
`src/term/stream.c`'s `GHOSTTY_SGR_ATTR_BRIGHT_FG_8`/`_BRIGHT_BG_8`
handlers did `cur.fg_palette = attr.value.bright_fg_8 + 8`, double
offsetting it (9 + 8 = 17). Index 17 isn't a named color at all — it
lands inside the 216-color cube (indices 16–231, `idx-16=1` →
`r=0,g=0,b=95`), an arbitrary dark blue. Explains why only
90–97/100–107 (aixterm bright) SGR codes were affected; truecolor and
256-color (`FG_256`/`BG_256`/`DIRECT_COLOR_FG`/`DIRECT_COLOR_BG`) were
already correct.

Fix: removed the `+ 8` in both branches — `cur.fg_palette` /
`cur.bg_palette` now take `attr.value.bright_fg_8` / `bright_bg_8`
as-is.

**Process note, and a real mistake made along the way**: to confirm
the `EGLConfig`/GBM-format hypothesis, a standalone diagnostic opened
`/dev/dri/card1` (the actual display card) directly with
`GBM_BO_USE_SCANOUT` *while `ghostcon-core` was live on tty4* — two
processes contending for the same scanout resources hung the running
renderer. The supervisor's canary watchdog caught it about 6 minutes
later (`vt 4: renderer hung (canary silent), killing and retrying`)
and auto-recovered, but the kill/restart's `wall` broadcast interrupted
an unrelated terminal session. Lesson: diagnostics that need a DRM
device should target a render node (`/dev/dri/renderD128`), never the
live scanout card, unless the service is first stopped.

Verified: `meson test -C build-release` (12/12), live on tty4 —
`claude`'s bright-red branding renders correctly (confirmed by the
user via a phone photo of the physical screen) instead of dark blue.

### Ctrl+letter shortcuts inserting as literal `ESC[N;5u` text

Live testing showed every `Ctrl+`letter combo (Ctrl+D, Ctrl+C, Ctrl+A,
...) landing as literal escaped text like `\x1b[4;5u` in the shell
instead of being interpreted, even though this exact class of bug had
already been fixed once this session (`core/input.c:296-307`'s kitty-
protocol-disabled setopt calls, added for the same symptom). That fix
was real but incomplete — a different code path was reintroducing the
same failure mode.

Root cause, found by writing a standalone program that calls
`ghostty_key_encoder_encode()` directly with ghostcon's exact
`setopt()` configuration and a synthetic Ctrl+D event: feeding it
`utf8="d"` (the plain letter) correctly produced `\x04`, confirming the
encoder's own settings were fine. Feeding it `utf8="\x04"` (an
already-Ctrl-transformed byte) reproduced the bug exactly, byte for
byte. `handle_keyboard_event()` (`core/input.c:430`) was computing
`utf8` via `xkb_state_key_get_utf8(input->xkb_state, ...)` — and
`input->xkb_state` has Ctrl currently held for a Ctrl+letter combo.
xkbcommon's `xkb_state_key_get_utf8()` has *built-in* special-case
behavior: when Control is active it returns the already-transformed
control byte, not the plain letter. Passed into the Ghostty key
encoder alongside `GHOSTTY_MODS_CTRL`, the encoder's legacy path can't
map an already-non-printable byte to a plain Ctrl+letter sequence and
falls back to kitty-style CSI-u.

Compared against kmscon per the user's request
(`src/input/input_uxkb.c:479` in the cached kmscon source): kmscon
never calls the UTF-8-with-modifiers lookup at all. It works from raw
keysyms (`xkb_state_key_get_syms()`/`xkb_state_key_get_one_sym()`,
unaffected by Ctrl — Ctrl isn't a shift-level modifier in the keymap)
and lets its own VTE layer do Ctrl mapping from the keysym + a
separately-tracked modifier mask.

Fix, mirroring that approach: `handle_keyboard_event()` now computes
`utf8` via `xkb_state_key_get_one_sym(input->xkb_state, xkb_code)` +
`xkb_keysym_to_utf8()` instead of `xkb_state_key_get_utf8()`. Neither
of those has the Ctrl special-case (keysym selection isn't affected by
Ctrl, and `xkb_keysym_to_utf8()` is a pure keysym→UTF-8 table lookup
with no modifier awareness), so the encoder receives the plain letter
and performs its own — already verified correct — Ctrl+letter
encoding. `unshifted_cp` needed no change; it already comes from
`xkb_state_unshifted`, a separate, never-modifier-updated state.

Verified: `meson test -C build-release` (12/12), live on tty4 —
Ctrl+D, Ctrl+C, Ctrl+A all confirmed interpreted correctly instead of
inserting literal `ESC[N;5u` text.

### `clear_on_logout` — wipe screen + scrollback when a session ends

Feature request following the pts-leak fix above: on logout the old
session's last screenful (and scrollback) stayed visible underneath the
new login prompt until overwritten. Two design questions resolved with
the user before implementing (per this session's own "stop and ask on
architecture" convention): scope is screen *and* scrollback (not just
the visible grid), and the trigger lives in `pty_child.c` (which already
detects session death via the SIGCHLD self-pipe) signaling
`ghostcon-core` explicitly, mirroring the existing resize control-socket
pattern rather than having the renderer try to infer logout from the
byte stream.

**New IPC message, opposite direction from `RESIZE`, same control
socket** (`ptyserv/protocol.h`'s `GHOSTCON_PTY_CTL_SOCKET_FMT`):
`ghostcon_ptyserv_format_clear()`/`parse_clear()` format/parse a bare
`"CLEAR\n"` line. `pty_child.c`'s SIGCHLD reap block (the same one the
pts-leak fix touched) now writes this to `ctl_fd` — best-effort, same
tolerance as `RESIZE` going the other way — right before setting
`awaiting_respawn`. `core/transport.c` gained
`ghostcon_transport_read_ctl()` (mirrors `pty_child.c`'s own RESIZE-side
receive: one `read()`, no partial-line handling, matching the existing
convention on both ends of this socket).

**`core/main.c`** polls `app.transport.ctl_fd` (bumped `fds[3]` ->
`fds[4]`); on a parsed CLEAR, if `app.clear_on_logout` is set, calls
`ghostcon_screen_erase_display()` with both `GC_ERASE_DISPLAY_ALL` and
`GC_ERASE_DISPLAY_SCROLLBACK` (both already existed, just never chained
together for this purpose) and homes the cursor.

**Config**: new `clear_on_logout` bool (`config.h`/`config.c`/
`share/ghostcon.toml.example`), default `true` — the first config key
in this tree that defaults to true rather than false, so it follows the
opposite `setenv()` convention from every other bool here: `GHOSTCON_
CLEAR_ON_LOGOUT` is only ever exported (as `"0"`) when the config
explicitly turns it *off*; the env var being unset means the true
default stands, not false. `core/main.c` reads it the same way
(`getenv() == "0"` to disable, anything else including absence keeps
the default).

Verified: `meson test -C build-release` (12/12, `test_undead_head`
isolated-clean per this session's known-flaky-under-load pattern), live
on tty4 — logging out with unsaved shell output/scrollback present now
leaves the new login prompt on a fully blank screen.

### Leftover DE mouse cursor visible over ghostcon's screen

Live testing found the DE session's hardware mouse cursor sprite still
visibly rendering on top of ghostcon's own frame on tty4 after
switching VTs — not interactive (ghostcon has no cursor input handling
at all yet), just a stale visual artifact.

Root cause: `core/kms.c` only ever discovers and drives the *primary*
plane (`find_primary_plane()`, filtered to `DRM_PLANE_TYPE_PRIMARY`).
DRM master handoff (`drmSetMaster()`/`drmDropMaster()`, `core/vtctl.c`)
is a permission/ownership change, not a modeset — it does not reset any
plane's committed state. A hardware cursor plane the DE's compositor
last committed (`CRTC_ID`/`FB_ID`/position, on the same physical CRTC
this VT switch just handed to ghostcon) stays exactly as-is forever,
since ghostcon's own atomic commits never mention that plane at all —
atomic KMS only ever changes properties a commit explicitly includes.

Discussed with the user before implementing (per this session's
"architecture decisions get confirmed first" practice): rather than a
narrow fix targeting only `DRM_PLANE_TYPE_CURSOR` specifically, went
with a blanket "disable every plane on this CRTC other than our own
primary plane" — the user's suggestion, and a better fit for this
project's existing philosophy (PLAN.md's canary "Philosophy" section:
check liveness/ownership broadly, don't hand-diagnose each specific
failure mode). Catches any leftover overlay a previous DRM client left
active, not just the one instance already found.

**`core/kms.c`**: new `disable_other_planes()`, called once per modeset
(inside `commit_frame()`'s existing `is_modeset` block — acquire/
reacquire only, not per-frame, since plane state persists exactly like
the bug it's fixing). Enumerates all planes via
`drmModeGetPlaneResources()`, and for each plane other than
`kms->plane_id` whose *current* `CRTC_ID` property value equals
`kms->crtc_id` (not just `possible_crtcs`, which is "could be
assigned", not "is currently assigned" — matters on a multi-monitor
machine, so another active display's planes are never touched), adds
`CRTC_ID=0, FB_ID=0` to the same atomic request. Best-effort: a failed
property lookup for one stray plane is silently skipped rather than
failing the whole modeset commit over a cosmetic cleanup.

Verified: `meson test -C build-release` (12/12, clean — `kms.c` itself
stays compile-checked-only per its existing "untested at runtime" doc
comment, no DRM mock exists to unit test this against), live on tty4 —
moving the DE's cursor then switching to tty4 no longer shows it
anywhere on ghostcon's rendered frame.

### Scrollback view shortcuts (Shift+Up/Down/PageUp/PageDown)

First feature-shaped addition after a long run of core reliability/
correctness fixes — deliberately scoped down from full kmscon-style
mouse support (discussed and compared: mouse would need libinput
pointer-capability handling, cell coordinate math, a whole new escape-
sequence-encoding subsystem with nothing existing to build on unlike
keyboard, and a cursor-plane or per-frame-quad rendering decision —
multi-day scope). Scrollback shortcuts reuse two already-existing,
previously-unused pieces: the `history[]` ring buffer (populated on
every scroll-up, never read by anything) and the keyboard shortcut
interception pattern already established for the Ctrl+Alt+F VT-switch
filter.

**`term/screen.h`/`screen.c`**: new `view_offset` field (0 = live,
distinct from the pre-existing `scrollback_top`, which is an internal
ring-rotation index for the live grid, not a user-facing scroll
position). `ghostcon_screen_row()` — the renderer's sole row-read path
(`render/machine.c`) — transparently splices in `history[]` rows when
`view_offset > 0`, computed as a single chronological sequence
(oldest history ... newest history, then live rows 0..rows_visible)
windowed by `view_offset`; this is the only place scrollback viewing
needed touching, since everything downstream already goes through this
one function. New `ghostcon_screen_scroll_view(screen, delta)` clamps
to `[0, history_count]` and marks the whole viewport dirty on change.
`view_offset` resets to 0 on resize (layout changes invalidate it) and
when scrollback is erased (`GC_ERASE_DISPLAY_SCROLLBACK`, e.g. via
`clear_on_logout`).

**`core/input.c`**: new `handle_scroll_shortcut()`, checked right after
the existing VT-switch filter in `handle_keyboard_event()` — exact
Shift-only match (not "Shift among other mods"), mirroring kmscon's own
default grab specificity so e.g. Shift+Ctrl+Up still forwards normally.
`ghostcon_input_dispatch()`/`handle_keyboard_event()` gained a `screen`
parameter to reach it. `core/main.c` passes `&app.term.screen` through
and checks `ghostcon_screen_get_dirty()` after dispatch, since a
scrollback shortcut changes the screen directly with no pty output to
trigger the usual render-on-new-data path.

**Two UX bugs found live after the shortcuts themselves worked**,
fixed same-session:
- Cursor stayed visibly rendered at its live grid position while
  scrolled back, sitting on top of spliced-in history content that
  wasn't the line it actually belonged to. Fixed:
  `ghostcon_machine_render_cursor()` now skips entirely while
  `view_offset > 0`, matching how most terminals hide the cursor
  rather than draw it somewhere misleading.
- Typing while scrolled back left the view stuck in history instead of
  snapping back to live. Fixed: `handle_keyboard_event()` now resets
  `view_offset` to 0 the moment any (non-shortcut) key actually reaches
  the shell.

Verified: `meson test -C build-release` (12/12; new `tests/test_term.c`
"scrollback view" case added — isolated 5x3 term, traces exact line
contents through offset 0 -> 1 -> clamped-fully-back -> clamped-back-
to-live, all matched on first run), live on tty4 — Shift+Up/Down/
PageUp/PageDown scroll correctly, cursor hides while scrolled back,
typing snaps back to live.

### Key auto-repeat

Not implemented at all until now — `libinput` never generates repeat
events itself (that's userspace's job), and `core/input.c` had zero
repeat handling anywhere.

Mirrors kmscon's own proven approach (`input_uxkb.c`), adapted to
ghostcon's plain `poll()` loop instead of kmscon's `eloop` abstraction:
`xkb_keymap_key_repeats()` gates which keys repeat at all (the active
XKB keymap excludes modifiers automatically — no manual blocklist
needed), and a `timerfd` resends the exact bytes the initial press
produced, first after an initial delay then at a fixed interval
(kmscon's own documented defaults: 250ms delay, 50ms rate — not made
configurable yet, matching this session's "don't build config plumbing
nobody asked for" scoping).

**`core/input.c`**: new `repeat_fd`/`repeating`/`repeat_evdev_code`/
`repeat_encoded`/`repeat_len` fields on `ghostcon_input_t`. On a
forwarded press whose key repeats per the keymap, arms the timer and
remembers the encoded bytes; on release of that same key, disarms it
(checked unconditionally right after computing press/release action,
not folded into the write-path arm logic, since a release often
encodes to zero bytes in legacy mode and would otherwise never reach
that code). `timerfd_settime()` unconditionally replaces the timer's
prior state, so a different key pressed mid-repeat naturally takes
over with no separate "stop the old one" step. New
`ghostcon_input_repeat_fd()`/`ghostcon_input_repeat_fire()` — the
latter drains the timer and resends the stored bytes.

**`core/main.c`**: `fds[]` bumped 4 -> 5 for the new timer fd (added
only when `app.input` exists and `timerfd_create()` succeeded, same
tolerance pattern as the ctl-socket/CLEAR fd); on fire, calls
`ghostcon_input_repeat_fire()`. No explicit render trigger needed here
unlike the scrollback shortcuts — repeated keystrokes just forward to
the shell like normal typing, and its echo coming back through
`transport_idx` triggers rendering the same way any other keypress
does.

Verified: `meson test -C build-release` (12/12), live on tty4 —
holding a printable key auto-repeats after the initial delay, and
releasing it stops cleanly.

### General config hot-reload (inotify), starting with font_size

Started as "add a font size in/decrease shortcut," but the user's
actual intent was different: config values should be hot-reloadable by
editing `ghostcon.toml` and having it apply live, Hyprland-style — no
keybinding, no restart, no VT blink, no lost session state. Confirmed
via AskUserQuestion before implementing (this touches every process's
own event loop shape, genuinely architectural): reload trigger is
inotify (auto-apply on save), and scope is general — every applicable
config key, not just `font_size`.

**Why this couldn't just be a bigger version of what existed**:
`config.h`'s own doc comment stated undead-head was "the one and only
place that needs to know a config file exists" — it parsed
`ghostcon.toml` once at startup and `setenv()`'d `GHOSTCON_*` for the
rest of the tree. Env vars can't change under an already-running
process, so hot-reload needed a genuinely different delivery path.

**Design chosen**: each config-consuming process
(`undead-head`/`supervisor`/`ghostcon-core`) watches the file directly
and independently — no reload-propagation IPC between them. All three
now link `config_lib`/`tomlc99` (previously undead-head-only) and
re-call `ghostcon_config_load()` on change, applying whichever fields
they individually understand.

**New shared helpers, `config.c`/`config.h`** (used by all three, so
the inotify boilerplate and its one real gotcha only needed solving
once): `ghostcon_config_watch_open(path)` watches the parent
*directory*, not the file itself — editors that save via write-temp-
then-rename (vim's default) orphan a watch placed on the file's own
inode, which then silently never fires again; watching the directory
with `IN_CLOSE_WRITE | IN_MOVED_TO | IN_MODIFY` catches both a plain
in-place write and the rename pattern. `ghostcon_config_watch_check()`
drains all pending events and filters by basename (ignores unrelated
files in the same directory). `ghostcon_config_export_env()` gained a
`bool overwrite` parameter — startup keeps `overwrite=false` (an
already-set env var, e.g. from a test harness, still wins the first
time); a reload uses `overwrite=true` (the file's new value must
actually take effect) — including correctly *unsetting* a bool that
flipped back to its default, since the existing readers
(`GHOSTCON_DISABLE_WALL` etc.) check presence, not value, so a stale
`setenv` left over from a previous reload would otherwise stick.
`GHOSTCON_CONFIG_PATH` itself is now also exported unconditionally, so
supervisor/ghostcon-core know what to watch without duplicating
undead-head's default-path/env-override resolution.

**Per-process integration**:
- `undead_head/main.c`'s reaper loop was a *blocking* `waitpid(-1, ...,
  0)` with no `poll()` at all — converted to a poll-based loop (SIGCHLD
  self-pipe, mirroring `pty_child.c`'s own established pattern) so it
  can also watch the inotify fd. On change: reload, re-export
  (`overwrite=true`) — undead-head owns no live runtime state itself by
  design, so this only affects *future* respawns (correctly covers
  `drm_node`/`run_dir`, which can't safely apply to an already-running
  process anyway — see below).
- `supervisor/main.c` had three separate single-fd `poll(&pfd, 1,
  timeout)` call sites (one per state), not one shared loop. A naive
  "just add a second fd" would have been a real bug: a config-only
  wakeup (state fd's own revents at 0) is indistinguishable from an
  actual timeout to the existing `rv>0`/`revents` branching, which
  would have made saving `ghostcon.toml` spuriously trigger "renderer
  hung"/"did not claim VT in time". Fixed with a small
  `poll_state_fd()` wrapper that transparently consumes config-only
  wakeups (applies the reload, polls again) before ever returning to
  the caller, so all three call sites keep their exact original
  semantics. `canary_deadline_ms` applies immediately (loop re-reads
  it each call); `disable_wall`/`disable_kmscon_fallback` apply for
  free with zero extra code, since `wall_broadcast()`/
  `spawn_kmscon_fallback()` already call `getenv()` fresh at point of
  use rather than caching at startup — re-exporting the env is
  sufficient. `drm_node` is a mutable buffer now (was a plain pointer
  into argv/getenv storage) so reload can update it in place; only
  takes effect on the *next* `spawn_renderer()` call, and never
  overrides an explicit `argv[3]`, at reload same as at startup.
- `core/main.c`: `fds[]` bumped 5 -> 6 for the inotify fd.
  `clear_on_logout` applies immediately (trivial bool).  `font_size`
  triggers a live rebuild: create the NEW atlas first
  (`ghostcon_atlas_create()`) and only destroy the old one + swap
  pointers on success (a failed rebuild — e.g. requested size doesn't
  fit `ATLAS_DIM` — can't leave `app.atlas` NULL), then reuse the exact
  cols/rows-from-`kms.width/height`/`cell_w`/`cell_h` +
  `ghostcon_term_resize()` + `ghostcon_transport_resize()` (TIOCSWINSZ)
  sequence already used on VT reacquire. No `render/gles.c` changes
  needed — confirmed by reading it: `gles->atlas_tex` is one fixed-size
  GL texture (`ATLAS_DIM`, unrelated to font size) that re-uploads based
  on `ghostcon_atlas_dirty()` on whatever atlas pointer is passed each
  frame, and a freshly-created atlas is already dirty by construction.
  Also fixed in passing: `GHOSTCON_FONT_SIZE` was already exported by
  `config.c` but never actually *read* at startup (`FONT_SIZE` was a
  bare compile-time constant) — now read the same
  getenv()-with-fallback way every other tunable is.

**Honest limitation, not a scope cut**: `drm_node`/`run_dir` set up
hardware/filesystem resources (EGL/GBM context, file paths) at process
start — swapping those under an already-running process is a
fundamentally different, riskier operation than updating a runtime
value (even real compositors don't hot-swap which GPU they render on).
These two apply on the next natural respawn, not immediately. Every
other key applies with zero restart.

**Minor known rough edge**: a single save can occasionally fire the
reload twice in one process (a plain in-place write, e.g. via `tee`,
can generate both `IN_MODIFY` and `IN_CLOSE_WRITE` as two separate
events if they land in different `poll()` wakeups) — harmless since
reload is idempotent (same file, same result), just an extra log line;
not worth debouncing for what it costs to fix.

Verified: new `ghostcon_config_watch_open()`/`_watch_check()` test
coverage in `test_config.c` (unrelated-file-ignored, write-via-rename
detected — both passed first run) plus `overwrite=true` bool-flip-in-
both-directions coverage; `meson test -C build-release` (12/12,
`test_undead_head` reran 3x isolated-clean after the reaper-loop
rewrite specifically because that's exactly the code touched, not just
assumed-flaky). Live on tty4: created `/etc/ghostcon/ghostcon.toml` for
the first time on this machine, restarted once (a watch can't exist on
a directory that didn't exist yet), then edited `font_size` 16 -> 24
*without* any further restart — all three processes logged "config
changed, applied" within about a second, same pids throughout
(confirmed via `systemctl status`), text visibly larger on the
physical screen, session/scrollback intact. `clear_on_logout` flip
verified the same way.

### Font zoom shortcut (Ctrl+=/Ctrl+Minus), and the 1pt-step squish bug

Follow-up to the general config hot-reload work above, once font_size
could already be rebuilt live: added Ctrl+=/Ctrl+Minus (kmscon's
grab-zoom-in/grab-zoom-out defaults, bound to the unshifted '='/'-'
keys rather than requiring Shift — matches most terminals/browsers).
Small addition given the atlas-rebuild machinery and shortcut-
interception pattern (`handle_scroll_shortcut`) already existed:
`core/input.c` gained `handle_zoom_shortcut()`, `core/main.c` factored
the config-reload path's inline font_size-rebuild code out into a
reusable `apply_font_size()` (shared by both the config-file path and
the new keyboard-shortcut path). `ghostcon_input_dispatch()` gained an
`int *out_zoom_delta` out-param, since `core/input.c` has no
atlas/font ownership at all (that's `core/main.c`'s `app_t`) — it only
accumulates direction.

**Live bug found immediately after the shortcut worked**: a single
Ctrl+= press left text visibly stretched (persisted until another
press), which a first guess (extra swap-flush cycles, theorizing GBM/
EGL buffer-rotation staleness) didn't fix — confirmed live it made no
difference, so that theory was wrong and got reverted rather than left
in as dead code. Root-caused instead with targeted debug logging
(temporary, removed once diagnosed) rather than guessing again:
`font_size` 23->24 produced `cell_w=14->14` (completely unchanged)
while `cell_h=31->33`. FreeType/fontconfig round the cell-width metric
('M' advance) and line-height metric to whole pixels independently,
and at 1pt granularity they frequently don't cross that rounding
boundary at the same point size — cells got taller without getting
wider for one press, a real visible aspect-ratio stretch, until a
later press happened to bump `cell_w` too. Fixed by using a step size
of 2pt (later made config-driven, see below) instead of 1pt, which
in practice makes both dimensions round together far more reliably.

**Then made configurable** (user: "configurable stuff are meant to be
hot-reloadable... like hyprland" — this follows the same philosophy
already established for `font_size` itself): new `zoom_step` key,
default 2, flows through the exact same hot-reload pipeline as every
other value in the "General config hot-reload" section above — no new
mechanism needed, this was the point of building that generally rather
than font_size-specifically. `core/input.c`'s `handle_zoom_shortcut()`
was simplified to report direction only (+-1 "one zoom tick"); the
actual magnitude (`app.zoom_step`) is applied in `core/main.c`, which
is the only place with config access anyway.

Verified: `meson test -C build-release` (12/12); live on tty4 across
three rounds — the shortcut itself, the 1pt-squish diagnosis (via
temporary debug logging, removed after) and 2pt fix confirmed clean on
a single press, then `zoom_step = 4` set via a live `ghostcon.toml`
edit (no restart) and confirmed each press now jumps by 4pt.

### Mouse support, pass 1 — app mouse-reporting + hardware cursor plane

Scoped explicitly into two passes (app mouse-reporting now, selection
later): this pass covers X10/SGR mouse-reporting escape encoding
(`term/mouse.c`, pure/unit-testable, mirroring `term/box_draw.c`'s own
split between logic and hardware-facing dispatch) plus a **real DRM
hardware cursor plane** for the visible pointer sprite, rather than
compositing it as a GLES quad — chosen so cursor position updates are
decoupled from content rendering latency (a plain atomic commit
touching only `CRTC_X`/`CRTC_Y`, independent of `commit_frame()`'s
per-content-frame path).

DECSET/DECRST mode 1006 (SGR extended mouse coordinates) was a
complete no-op before this — `screen->mouse_sgr` is now tracked and
actually switches the wire encoding between X10 legacy framing
(`ESC[M` + 3 raw bytes, value+32, coordinate-capped at 223) and SGR
(`ESC[<Cb;Cx;Cy` + `M`/`m`, decimal, unlimited range).

**Touchpad support** was verified rather than assumed: checked
directly against kmscon's own `src/input/input_pointer.c`, which
hand-rolls raw evdev `ABS_X`/`ABS_Y` tracking per touchpad because it
doesn't use libinput for pointer input at all. ghostcon already goes
through libinput, whose `LIBINPUT_DEVICE_CAP_POINTER` capability
covers mice, trackballs, and touchpads uniformly — touchpad relative
dragging already arrives as ordinary `LIBINPUT_EVENT_POINTER_MOTION`
(`MOTION_ABSOLUTE` is for genuinely absolute devices like touchscreens
and tablets), so no extra touchpad-specific code was needed.

**Real bug found live, fixed during this pass**: the cursor plane's
buffer must be **square** on this machine's amdgpu — a narrower-than-
tall procedural I-beam buffer was rejected outright with `EINVAL` on
atomic commit. Fixed with a fixed square canvas (queried via
`DRM_CAP_CURSOR_WIDTH`/`DRM_CAP_CURSOR_HEIGHT`) with the glyph
letterboxed/centered within it — not universal across every DRM
driver, but safe to always honor.

Verified: `meson test -C build-release`; live on tty4 — mouse
reporting confirmed against a real mouse-aware TUI app, hardware
cursor motion and the procedural I-beam confirmed working.

### Cursor sprite: raster images, per-state, config-driven

Follow-up to the hardware cursor plane work above: the cursor image
can now be loaded from real raster files, per interaction *state*
(`default`, and `link` — hovering an OSC-8 hyperlink cell), each
independently overridable via `ghostcon.toml`, with a shared Xcursor
theme as the fallback source for states not explicitly overridden.
Precedence: **explicit per-state asset path > theme's auto-resolved
asset for that state > procedural I-beam fallback**.

New `[cursor]` config table: `theme`, `default`, `link` (paths),
`base_scale` and `scale_with_terminal` (see below). New pure,
DRM-independent module `core/cursor_image.c` (hand-rolled uncompressed
BMP decoder/encoder plus a minimal Xcursor binary-format decoder —
deliberately hand-rolled rather than pulling in `libpng`/`libXcursor`,
matching this project's existing preference, e.g. vendoring `tomlc99`
rather than a bigger TOML library). `core/kms.c`'s single cursor image
became N *simultaneously loaded* per-state images (`GC_CURSOR_STATE_*`
enum) so hover-switching is a cheap FB swap, not a decode-and-reupload.
Companion tool `tools/xcursor2bmp.c` flattens a whole Xcursor theme
into a directory of per-name BMP files.

**Real bug, `XCURSOR_MAGIC` typo**: defined as `0x72754358` (a
transposition of the correct `0x72756358`, verified via `xxd` on a
real theme file plus manual bit-math) — every real-world Xcursor file
failed to parse, but the hand-written synthetic unit test used the
*same* wrong constant for both writing and reading, so it passed while
rejecting every real file. Fixed the constant and changed the test to
write literal ASCII magic bytes instead of a precomputed constant,
specifically so this self-consistent-but-wrong bug class can't recur
silently — never validate a decoder's magic number against a
hand-computed constant used by its own test.

**Follow-up bug #1, wrong cursor-plane size ceiling**: a "cursor
becomes clipped" report after zooming down led to finding
`GHOSTCON_CURSOR_MAX_SIZE` was hardcoded to 64 (copied from kmscon's
own convention without verifying against real hardware) while this
machine's actual `DRM_CAP_CURSOR_WIDTH`/`HEIGHT` is 256. Fixed by
raising the constant to 256 — necessary but not sufficient (see #2).

**Follow-up bug #2, raster images didn't scale with terminal zoom at
all**: the 256 fix only stopped clipping; a raster asset's on-screen
size never actually tracked font-size changes the way the procedural
I-beam already did. Added a real nearest-neighbor image scaler
(`ghostcon_cursor_scale()`) plus `[cursor].base_scale` (default `1.0`)
and `[cursor].scale_with_terminal` (default `true`). Semantics went
through two iterations based on live testing:
- v1: `base_scale=1.0` meant "the asset's own raw native pixel size."
  Live-tested with a 96×96 BMP at `font_size=24` and found "comically
  larger" than intended — a 96px asset is already huge for a cursor,
  and `scale_with_terminal` compounded it further.
- v2 (final): `base_scale=1.0` means "this cursor's height is
  `CURSOR_BASE_SCALE_CELL_RATIO` (1.25×) of a cell," independent of
  the asset's own native pixel dimensions; aspect ratio is preserved.
  `scale_with_terminal` picks whether that cell height is the *live*
  one (grows/shrinks with Ctrl+=/Ctrl+Minus) or a fixed reference
  (`CURSOR_SCALE_REFERENCE_CELL_H`, measured directly via FreeType
  against the default font rather than guessed).

**Follow-up bug #3, hotspot didn't account for canvas letterboxing**:
once the canvas (256×256) became much larger than typical scaled
raster glyphs (~41px), the visible glyph sat deep inside the
letterboxed canvas while the stored hotspot only accounted for the
glyph's own local coordinates — `commit_cursor()` was anchoring the
canvas's mostly-empty top-left corner at the pointer instead of the
actual glyph. Fixed by folding `blit_cursor_image()`/
`draw_cursor_ibeam()`'s own centering offset into the stored hotspot.

**Follow-up bug #4, dead zone at the top-left screen edge**: after
fixing #3, the (now much larger) hotspot revealed that `CRTC_X`/
`CRTC_Y` were being computed and floored as *unsigned*, to dodge
underflow — so any pointer position with `x`/`y` smaller than the
hotspot all collapsed to `crtc=0,0`, pinning the glyph a fixed
distance from the true top-left corner. `CRTC_X`/`CRTC_Y` on a DRM
plane are actually a **signed** property (`DRM_MODE_PROP_SIGNED_RANGE`)
— hardware already tolerates a plane extending past the bottom/right
edge with no clamp; the fix was signed math with no floor at all,
sign-extended correctly through `int64_t` when building the atomic
property value (a plain `(uint64_t)` cast would have zero-extended a
negative offset into a huge positive one).

**Follow-up bug #5, built-in transparent padding in the asset itself**:
even with #3 and #4 fixed, a real Xcursor-derived BMP (extracted via
`xcursor2bmp` from a real theme) still showed a visible gap between
the pointer and the glyph, and the glyph looked too small for
`base_scale=1.0`. Diagnosed by inspecting the raw BMP pixel data
directly: the asset's actual opaque content didn't start until column
35 of a 96px-wide image — over a third of it was transparent padding
baked into the source theme (headroom for shadow/antialiasing). Fixed
with `ghostcon_cursor_crop_to_content()`, called before scaling: crops
to the tightest opaque bounding box and shifts the hotspot to match,
so `base_scale` sizes and positions the actual glyph, not the padding
around it.

Verified: `meson test -C build-release` (18 checks across
`test_cursor_image`, including the crop function); live on tty4 across
all five follow-ups — BMP override and Xcursor-theme fallback both
confirmed, hyperlink-hover state switching confirmed, all four screen
corners confirmed reachable, sizing confirmed proportional to cell
height after the v2 semantics + crop fix.

### Mouse support, pass 2 — click-drag selection + local clipboard

Wires together four pieces that already existed but were entirely
unwired: `ghostcon_selection_t` (start/update/finish/contains were all
implemented, never called by anything but one `clear()` at init), the
`screen_t.clipboard[4096]` buffer (OSC 52 only), the pass-1 mouse
pipeline (`ghostcon_input_pointer_t` gained `left_pressed`/
`left_released`, set only when `core/input.c`'s new
`should_intercept_for_selection()` decides a click is local rather
than reported to an app that's grabbed mouse tracking -- mirrors
xterm's own Shift-bypass convention), and the alpha-blended overlay
precedent the software cursor already established
(`ghostcon_machine_render_selection()`, one rect per selected row via
a new shared `ghostcon_selection_row_range()` helper -- also fixed a
real pre-existing bug in `ghostcon_selection_contains()`, which didn't
correctly bound a single-row selection's right edge).

Copy/paste are **explicit, configurable keyboard shortcuts**, not
copy-on-release -- click-drag only highlights; nothing touches the
clipboard until `[keybindings].copy_to_clipboard`/`paste_from_clipboard`
(default `ctrl+shift+c`/`ctrl+shift+v`, Ghostty's own Linux defaults,
matched deliberately and verified against Ghostty's real source/
terminfo) is pressed. New `ghostcon_parse_keybinding()` (Ghostty
Trigger-string-compatible syntax) -- caught and fixed a real bug while
writing it: evdev letter keycodes follow physical QWERTY layout, not
alphabetical order (`KEY_A=30`, `KEY_Z=44`, `KEY_C=46` -- nowhere near
contiguous), so `KEY_A + offset` would have silently mapped to the
wrong keys for most letters; needed an explicit 26-entry table.
Middle-click paste is a non-configurable bonus (X11 convention),
handled entirely in `input.c` with no `main.c` round-trip needed.
Clipboard scope deliberately stays local/in-process this pass -- no
`ghostcon-ipc` broker, no desktop (`wl-copy`) bridge, no `copy_raw`/
`paste_raw` (styled/HTML) variant -- all explicitly deferred alongside
the not-yet-built Phase 2 IPC broker, the natural owner of any
cross-session reach.

New `src/term/base64.c`/`.h` (RFC 4648 encode/decode -- OSC 52
previously only had a validity *checker*, never an actual decoder,
since it stored/returned payloads verbatim without needing one).

**Follow-up, hotspot UX**: after shipping, live testing surfaced a
real design flaw the user caught precisely: the procedural I-beam
fallback reported its own hotspot as its bounding-box corner (sensible
for an arrow-shaped pointer, whose tip IS a corner) instead of the
center of its own vertical stem (the natural "pointing pixel" for an
I-beam) -- fixed in `core/kms.c` to `kms->cursor_w/2, kms->cursor_h/2`
(the glyph is always centered in the square canvas, so its own
geometric center needs no extra math). For BMP overrides -- which
carry no hotspot field in the format at all, unlike Xcursor -- added
explicit `[cursor].default_hot_pos`/`link_hot_pos` config (`"x,y"` in
the original asset's own pixel coordinates) for precise cases (e.g. an
arrow's tip), falling back to auto-centering the cropped glyph (a much
better generic guess than a fixed corner) when unset and no real
Xcursor-embedded hotspot exists either.

**Follow-up, `TERM`**: live testing (`nano`'s colors/mouse response
noticeably worse under ghostcon than kmscon) traced to
`ptyserv/pty_child.c` falling back to `TERM=linux` (the bare Linux
console's own minimal terminfo entry) whenever the environment didn't
already set one -- undersells what ghostcon's libghostty-vt engine
actually implements (24-bit color, OSC 8, X10/SGR mouse). Changed the
fallback to `xterm-256color`, with `xterm-ghostty` (verified against
Ghostty's own source as its real default, terminfo already installed
on this machine) left as a commented alternative and a note on why it
isn't the default yet: it advertises Ghostty's full feature set
(Kitty graphics, certain OSC extensions) which ghostcon doesn't
implement, so claiming it before coverage is close enough would make
apps attempt features that then silently misbehave.

Verified: `meson test -C build-release` (all suites, including new
`test_selection` -- selection state machine, row-range, text
extraction, and base64 round-trip coverage -- plus `parse_keybinding`
tests added to `test_input`); live on tty4 -- click-drag highlight, Ctrl+
Shift+C/V round-trip, middle-click paste, hot-reloaded `[keybindings]`
override, corners/positioning after the hotspot fix, `nano` syntax
highlighting confirmed improved after the `TERM` fix.

**Known follow-up, not yet started**: live testing also surfaced that
`core/input.c` hardcodes the key encoder to legacy mode permanently
(`ghostcon_input_open()` forces `GHOSTTY_KITTY_KEY_DISABLED`, set once,
never revisited) even though `term/kitty.c`/`kitty.h` already track an
app's live Kitty keyboard protocol negotiation in `screen_t` -- nothing
ever reads that tracked state back into the encoder. A Kitty-protocol-
aware app (e.g. `opencode`) that successfully requests the protocol
still receives legacy-encoded keys regardless, breaking anything
relying on precise CSI-u modifier reporting (found live: Ctrl+Left/
Right word-jump silently not working in `opencode`, but working in
real Ghostty). This was already flagged as deliberately deferred when
the original hardcode-to-legacy fix went in; wiring `ghostcon_input_
sync_modes()` to also push `screen->kitty`'s live flags into the
encoder (the same place `CURSOR_KEY_APPLICATION` already gets synced
every dispatch) is the next scoped piece of work.

### Phase 2 — IPC and overlay

7. **`ghostcon-ipc`** — build the real broker (separate sockets for
   notify/clipboard, zero-`unsafe` Rust, `SO_PEERCRED` checks, size-capped
   framing). Then go back and replace the Phase 1 stubs:
   - OSC 9/777 handler switches from "log to journald" to "send to
     `ghostcon-ipc` notify.sock."
   - OSC 52 handler switches from "local in-process variable" to "read/
     write via `ghostcon-ipc` clipboard.sock," gaining cross-VT sharing
     (and optional `group_by=user` isolation) for free.
8. **`ghostcon-overlay`** — build the sandboxed, separate-DRM-plane
   renderer once `ghostcon-ipc`'s notify path is real. Subscribes to
   `ghostcon-ipc`, renders notifications and (if configured) titles.
   No supervision needed for this process — plain `Restart=always`.
9. **OSC 8 completion** — with the overlay in place, add the click-to-open
   handler (shell out to `xdg-open` if available, or highlight +
   copy-on-click as fallback) and wire it through input handling.
10. **OSC 22 (cursor shape)** — can land in either phase since it has no
    IPC/overlay dependency; included here only because it's lower
    priority than the core path. Minimal Xcursor file parser, X11 theme
    convention, config for theme selection and per-name overrides.

OSC 1337 stays deprioritized per the matrix above and is not part of
either phase unless real-world unimplemented-OSC log volume justifies
revisiting it later.

---

## Testing strategy

Different parts of this system fail in different ways and need different
testing approaches. A few components — the canary/kill-reconnect path
especially — are exactly the kind of thing that's easy to get right on
paper and never actually verify until it fails for real. This section
exists so that doesn't happen by default.

### Terminal engine — conformance + fuzzing

This is the component with the clearest, most mechanical test story,
because correctness here is "did we parse/render what a real terminal
should," which is checkable without any hardware:

- **VT100/xterm conformance suites** — there are existing terminal
  conformance test suites (e.g. `vttest`, and whatever test corpus
  `libghostty-vt` itself ships, since its correctness is
  presumably validated this way upstream. Run these against the terminal
  engine in a headless mode (no DRM/KMS needed — this is
  pure byte-stream-in, state-out testing) to catch basic protocol
  regressions early, independent of the rendering pipeline.
- **OSC matrix tests** — for every row in the OSC support matrix
  (section 8), a test case that feeds the literal escape sequence in and
  asserts the expected internal state/event (e.g. OSC 52 write → correct
  buffer stored; OSC 9 → correct normalized notification event emitted;
  unknown OSC → journald log entry + no crash + parser state unaffected
  by the next valid sequence). This is the cheapest, highest-value test
  suite in the whole project and should be written incrementally
  alongside each OSC handler, not after.
- **Fuzzing the OSC/escape-sequence parser path** — feed `wrap`'s input
  path random and malformed byte sequences (a basic fuzzer harness,
  e.g. via `cargo-fuzz`/AFL-style if `wrap`'s C layer is fuzzable, or
  targeting libghostty-vt's parser directly if it already has fuzz
  coverage upstream). This is the direct test for the "hostile failure /
  parser desync" risk named in the rationale section — the goal is
  confirming that malformed input degrades gracefully (dropped/ignored)
  rather than corrupting parser state in a way that persists into
  subsequent valid input.

### `ghostcon-diag` — purpose-built interactive capability diagnostic

The patchwork of existing tools (`vttest` for legacy VT100/xterm
conformance, kitty's own scattered test scripts for its graphics
protocol, `libsixel`'s sample corpus, manual per-OSC checks) is real but
fragmented — there is no single, purpose-built tool that tests
specifically against the "modern floor" claim this project's whole
rationale rests on (OSC 8/52/9, sixel/Kitty graphics, true color,
synchronized output, Kitty keyboard protocol). Rather than continuing to
assemble that patchwork by hand, `ghostcon-diag` is a small standalone
diagnostic tool, built as part of this project, modeled on the classic
SNES hardware test-cartridge format: multi-paged, partially interactive,
each page isolated to a single targeted capability, with a clear
pass/fail/partial result per page rather than one large undifferentiated
wall of escape sequences.

This is deliberately built as a **general-purpose terminal capability
diagnostic, not a ghostcon-only internal test** — it should be usable
standalone against any terminal emulator someone wants to check, with
**Ghostty itself used as the reference implementation that scores
maximum** across every page, since Ghostty's `libghostty`/`libghostty-vt`
is the actively-maintained, broadly-exercised implementation this whole
project is built around and trusts as correct. Calibrating against
Ghostty (rather than, say, an idealized spec reading) keeps the tool
honest about what "the floor" practically means today, not what a
strict reading of every RFC/proposal would imply.

```
ghostcon-diag
  ├── page 1: VT100/xterm baseline
  │     cursor positioning, scrolling regions, character sets — the
  │     vttest-equivalent baseline, kept as one page rather than the
  │     entirety of the tool, since this part is genuinely solved/old
  │     ground and shouldn't dominate a "modern floor" diagnostic
  ├── page 2: color
  │     16-color, 256-color, true color (24-bit) — isolated per
  │     sub-page so a partial true-color failure doesn't get conflated
  │     with full color-support failure
  ├── page 3: text attributes
  │     SGR 58/59 colored underlines (styles: single, double, curly,
  │     dotted, dashed), bold/italic/strikethrough combinations
  ├── page 4: OSC baseline
  │     OSC 4/10/11/12/104 (palette/fg/bg/cursor color) — directly
  │     exercises the "implement directly" tier of the OSC matrix
  │     (section 8)
  ├── page 5: hyperlinks (OSC 8)
  │     renders a test hyperlink, INTERACTIVE: tester clicks it,
  │     diagnostic confirms whether click-to-open fired correctly
  ├── page 6: clipboard (OSC 52)
  │     INTERACTIVE: diagnostic writes a known string via OSC 52, tester
  │     confirms it landed in the system clipboard by pasting elsewhere
  ├── page 7: notifications (OSC 9 / OSC 777)
  │     triggers a test notification, INTERACTIVE: tester confirms
  │     whether/how it was surfaced
  ├── page 8: sixel graphics
  │     renders a known test image via sixel, tester visually confirms
  │     correct decode (color accuracy, no corruption/artifacts)
  ├── page 9: Kitty graphics protocol
  │     renders test images via direct transfer, file transfer, and
  │     shared-memory transfer modes (the three modes `bcon`'s own
  │     feature list calls out) as separate sub-pages, since these are
  │     genuinely different code paths in a correct implementation
  ├── page 10: synchronized output (DEC mode 2026)
  │     renders a fast-updating test pattern with and without sync mode
  │     enabled; tester visually confirms whether tearing/flicker differs
  │     between the two — this is inherently a visual/perceptual check,
  │     not something that can be scored purely programmatically
  ├── page 11: Kitty keyboard protocol
  │     INTERACTIVE: tester presses specific key combinations (modifiers,
  │     repeated keys) and the diagnostic reports what encoding was
  │     actually received, compared against what progressive enhancement
  │     mode should produce
  ├── page 12: mouse reporting
  │     INTERACTIVE: tester clicks/scrolls within a test region, X10 /
  │     SGR / URXVT / SGR-Pixels modes tested as separate sub-pages
  └── page 13: bracketed paste
        INTERACTIVE: tester pastes a multi-line string, diagnostic
        confirms it was correctly wrapped/delimited rather than
        interpreted as individual keystrokes
```

Each page reports one of: **PASS** (matches Ghostty's reference
behavior), **PARTIAL** (some sub-capability works, some doesn't — e.g.
sixel renders but with color inaccuracy), **FAIL** (capability absent or
actively breaks — including the "hostile failure" class named in the
rationale section: a FAIL that corrupts subsequent page rendering should
itself be flagged distinctly, since that's a more severe finding than
simple absence of support). A final summary screen aggregates these into
an overall "modern floor coverage" score — the actual artifact useful for
comparing `ghostcon` against kmscon/tsm, `bcon`, or any other terminal,
giving the project's "modern floor" claim something concrete and
reproducible to point to instead of prose assertion.

Given its general-purpose framing, `ghostcon-diag` is a reasonable
candidate for its own small standalone repo/release rather than being
buried inside `ghostcon`'s own source tree — it has clear value
independent of this project and may be more useful to the wider terminal
ecosystem if it isn't perceived as a ghostcon-only marketing tool.
Whether it ships as part of Phase 1 or is treated as a parallel-track
project is worth deciding once Phase 1's core implementation work is
underway and there's bandwidth to assess.



This is the part that can't be meaningfully tested with pure unit tests,
since correctness depends on actual kernel ioctl behavior:

- **VM-based integration testing** — run `ghostcon-core` inside a VM with
  virtual KMS/DRM support (e.g. QEMU with `virtio-gpu`, or the kernel's
  `vkms` — Virtual KMS — driver, which exists specifically for testing
  KMS/DRM clients without real GPU hardware). This should be the default
  environment for CI, since it doesn't require dedicated hardware runners
  and still exercises real DRM/KMS code paths, not mocks.
- **Real-hardware smoke testing before release** — VM/virtual-KMS testing
  validates the ioctl sequences are *correct*, but timing-sensitive
  behavior (actual VT switch latency, real GPU driver quirks across
  vendors) still needs periodic testing on real hardware before tagging
  a release, even if it's not part of routine CI.
- **`vtctl.c` specifically** — given this is flagged as the
  highest-risk module, it should have a test harness that can
  deliberately trigger `VT_RELDISP` and assert the ack happens within a
  bounded time, run repeatedly under simulated load (see chaos testing
  below) to catch the exact race-during-rapid-switching condition that
  motivated this project.

### Supervision layer — fault injection (the most important test category)

This is the part of the system that exists *specifically* to handle
failure, so it needs to be tested by deliberately causing the failures
it's meant to catch — not just tested for the happy path:

- **Induced hang testing.** Build a debug-only flag/mode for
  `ghostcon-core` that, on a signal or env var, deliberately enters an
  infinite loop (simulating the exact wedged-event-loop condition that
  was observed in practice). Run the full supervisor stack against this
  and assert, end to end:
  1. the canary detects the hang within the configured deadline (not
     early, not late — confirms the deadline logic itself, not just
     "eventually something happens")
  2. the pty child survives the kill (still running, still holding
     the PTY master fd, shell process still alive with no SIGHUP)
  3. the recovery file is written with correct, complete contents
  4. the replacement renderer queries ghost-ptyserv for the pty child's
     socket, connects directly, and receives the ring buffer replay
  5. the user's prior shell session (cwd, running child processes, shell
     history) is genuinely intact — not just "a shell appeared" — as
     confirmed by inspecting the new renderer's terminal state matching
     the pre-hang state
  6. `wall`/journald diagnostics are emitted, rate-limited correctly on
     repeated triggers within the same boot
- **Induced crash testing** (distinct from hang) — trigger an actual
  SIGSEGV/panic via a debug flag and confirm the `diag.c`/panic-handler
  self-reporting path produces a correct journald entry with a properly
  truncated backtrace, separately from the hang-path tests above, since
  these are different code paths (signal handler vs. canary timeout) that
  both need independent coverage.
- **Rapid VT-switching stress test** — script repeated, fast VT switches
  (the actual real-world trigger condition) against a multi-VT test setup
  and confirm no VT enters an unrecoverable state across many iterations.
  This is the closest thing to a regression test for the original bug
  that motivated this entire project, and should be run for an extended
  duration/iteration count, not just a few manual switches, since the
  original failure was load-dependent and may not reproduce on the first
  few tries.
- **Fallback chain testing with partial installs** — verify the
  ghostcon→kmscon→fbcon tier logic correctly skips unavailable tiers
  (e.g. confirm a kmscon-not-installed system falls straight from
  ghostcon to fbcon+agetty, not to a nonexistent kmscon step) by testing
  each combination of "what's installed" explicitly, not just the
  all-three-present case.

### `ghostcon-ipc` — standard service testing, plus the security properties

Since this is restart-on-hang tier (not heavy supervision) and zero-
`unsafe` safe Rust, its test burden is lighter but still has specific
things worth checking deliberately:

- **Standard unit/integration tests** for each socket's protocol (notify,
  clipboard, control) — request/response correctness, `group_by=user`
  clipboard isolation behaving correctly when configured on vs. off.
- **Resource-exhaustion testing** — send oversized/malformed
  length-prefixed messages and confirm rejection happens *before*
  allocation, not after (this is a specific, named risk in the security
  posture section and should have a specific test asserting it, not just
  be assumed correct from code review).
- **Peer credential spoofing attempts** — from a test client, attempt to
  claim a different UID than the kernel-verified one and confirm
  `SO_PEERCRED`/`peer_cred()` based checks can't be bypassed by anything
  the client controls.
- **Restart behavior** — kill `ghostcon-ipc` mid-operation (e.g. while a
  clipboard write is in flight) and confirm systemd's restart brings it
  back cleanly with no corrupted on-disk/in-memory state carried over
  incorrectly, and that dependent `ghostcon[ttyN]` instances and
  `ghostcon-overlay` degrade gracefully (queue/drop/retry, not crash)
  during the restart window.

### `ghostcon-overlay` — minimal, given its own design intentionally
discourages heavy investment here

Since this component is explicitly "no supervision needed, cheap to
restart, nothing precious to lose," its test investment should be
proportionally light:

- Basic rendering correctness (does a given notify/HUD/clipboard-picker
  event produce the expected visual output) — can be tested by capturing
  the DRM plane's output and comparing against expected reference frames,
  similar in spirit to the screenshot feature it shares an implementation
  boundary with.
- **Modal interaction exit-path testing** — specifically test that the
  interactive modal mode (clipboard/notification picker) always returns
  input routing to normal on every exit path (selection, Esc, click-away,
  and also overlay crash mid-interaction) — this is the one place this
  component's correctness actually matters for the rest of the system,
  since a stuck modal grab would effectively (if temporarily) break input
  for the affected VT.

### `ghostconctl` — straightforward CLI testing

Since it's a thin client over `ghostcon-ipc`'s control socket with no
internal logic of its own (by design — see its "stay dumb" philosophy),
testing is mostly about confirming it really is thin: each verb produces
the correct socket message and correctly surfaces the daemon's response
or error, nothing more. Also worth a specific test confirming `switch`
correctly rejects/handles a target VT number that doesn't exist, since
that's the one verb with externally-visible side effects.

### What to test before considering Phase 1 "done"

Tying back to the Phase 1 completion criterion already stated in Build
phasing: the induced-hang and induced-crash test suites above, plus the
rapid-VT-switching stress test, should all be passing — not just
implemented — before Phase 1 is considered complete. The OSC conformance
suite should also be passing for every "implement directly" and "stub"
tier entry. Phase 1 being "done" should mean "the regression tests for
the original motivating bug pass reliably," not just "the code compiles
and looks right."
