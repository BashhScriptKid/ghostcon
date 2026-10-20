# ghostcon Maintenance Runbook

Concrete recipes for extending and debugging ghostcon without an LLM.
See `docs/REFACTOR_PLAN.md` for the architecture rationale; this file is
the "what file do I touch" companion.

After **every** change, the minimum verification is:

```sh
ninja -C build-release
meson test -C build-release          # currently 22 tests
```

---

## 0. Code style

Five rules, each enforced by a script in `tools/`. The scripts are the
executable specification; update both if a rule changes. One command
runs them all, from the repository root:

```sh
python3 tools/format.py            # rewrite in place
python3 tools/format.py --check    # report only (what CI runs)
```

Run a single rule with `--only <rule>`, e.g. `--only extern`. The
rules are also runnable directly, e.g. `python3
tools/format_signatures.py --check`.

Each accepts `--apply` to rewrite in place and is idempotent. All four
are tokenizer-based, not full C parsers: they ignore comments and
strings while tokenizing, skip preprocessor logical lines (including
macro bodies), and only inspect top-level function definitions. Review
the diff after `--apply`, and always run the tests.

### Braces — Allman

Block braces go on their own line for functions,
if/for/while/switch, else, do, and struct/enum/union:

```c
if (condition)
{
    statement;
}
```

Indentation is 4 spaces. Initializers, designated initializers, and
compound literals keep their K&R `= { ... }` / `(T){ ... }` form.
`tools/format_braces.py` moves same-line block braces, expands
single-line blocks, and rewrites `} else {` as `}`, `else`, and `{`
on separate lines. It does
not touch initializer/compound-literal braces or macro definitions.

### Function-definition return types

Only definitions are governed by the return-type rules below; a
prototype's return type is left as written. (The parameter-list rule
applies to both.)

One line when the return type's base is:

- a C builtin scalar keyword: `void`, `char`, `short`, `int`, `long`,
  `float`, `double`, `signed`, `unsigned`, `_Bool` (`bool` via
  `<stdbool.h>`); or
- a standard/POSIX scalar typedef: `int8_t`..`int64_t`,
  `uint8_t`..`uint64_t`, `int_least*`/`uint_least*`, `int_fast*`/
  `uint_fast*`, `intptr_t`, `uintptr_t`, `intmax_t`, `uintmax_t`,
  `size_t`, `ssize_t`, `ptrdiff_t`, `wchar_t`, `wint_t`, `clock_t`,
  `time_t`, `off_t`, `pid_t`, `uid_t`, `gid_t`, `mode_t`, `dev_t`,
  `ino_t`, `nlink_t`, `blksize_t`, `blkcnt_t`, `FILE`, `va_list`.

```c
static bool load_config(const char *path)
char *xstrdup(const char *s)
size_t ghostcon_input_encode_key(...)
```

Qualifiers (`const`, `volatile`, `restrict`, `_Atomic`) and storage
specifiers (`static`, `inline`, `extern`, `register`, `auto`,
`_Noreturn`) are ignored when finding the base. Pointers and arrays do
not change the base: `char *xstrdup(...)` stays one line.

Split when the base is:

- a type declared in this repository (`ghostcon_*`); or
- a third-party type (`Ghostty*` from libghostty, `drmMode*` from
  libdrm, `libinput_*`, `xkb_*`, ...); or
- a struct/union/enum tag, including standard/POSIX tags (deliberately
  conservative); or
- a function-pointer declarator return — the tool skips these rather
  than attempting a split.

```c
ghostcon_style_id_t
ghostcon_style_add(...)
```

A pointer return splits between the base type and the declarator,
so the `*` goes down with the name — the `*` is part of the
declarator, not of the base type:

```c
ghostcon_style_set_t
*ghostcon_style_set_create(uint16_t capacity)
```

Parameter lists are always collapsed onto the name line, however long;
only a non-pointer return type may remain split. This applies to
prototypes as well as definitions:

```c
static bool handle_scroll_shortcut(ghostcon_screen_t *screen,
                                   uint32_t evdev_code, GhosttyMods mods,
                                   GhosttyKeyAction action)
```

becomes:

```c
static bool handle_scroll_shortcut(ghostcon_screen_t *screen, uint32_t evdev_code, GhosttyMods mods, GhosttyKeyAction action)
```

Lists containing line comments, multi-line comments, or preprocessor
lines are left as written. The exact classification sets are
`PRIMITIVE` and `STD_TYPEDEFS` in `tools/format_signatures.py`; update
both that script and this list together.

There is no code line-length limit. The ~82-column guidance below
applies to prose inside comments, not to code.

Splitting is a guideline, not a rule: a call's argument list may be
split across lines wherever that reads better, and no formatter
enforces either way. Break at a meaningful boundary -- an operator, or
the open paren -- rather than in the middle of a list:

```c
if (!ghostcon_kms_init(&disp->kms, disp->drm_fd) ||
    !ghostcon_kms_create_scanout_surface(&disp->kms))

sd_journal_send(
    "MESSAGE=%s crashed on signal %d", g_component, signum);
```

The one hard rule is on *declarations*: a function's parameter list is
always on one line, however long. See the return-type section above.

### Pointer declarators — `T *name`

The `*` binds to the declared name, not the base type:

```c
uint32_t *pixels;   /* not uint32_t* pixels */
char   **fields;    /* not char * * fields    */
```

At least one space (or a newline) before the `*` run, and none between
the `*`s and the name. Multiple spaces before the `*` are allowed so
struct fields can stay column-aligned:

```c
ghostcon_row_t  *rows;
char            *buf;
```

The name must be on the `*`'s line: a `*` left at the end of a line is
joined to the name too, since the `*` belongs to the declarator. This
is why a pointer return splits above the `*` rather than below it
(see above). Spacing around pointer qualifiers (`char *const *p`) and
function-pointer declarators (`bool (*fn)(int)`) is left alone;
expressions such as `width * height` are not touched because the token
before the `*` is not a type.

### Explicit `extern` on prototypes

A function declaration with no body -- a prototype -- that is neither
`static` nor a `typedef` says `extern` explicitly:

```c
extern size_t ghostcon_mouse_encode(const ghostcon_screen_t *screen,
                                     ghostcon_mouse_button_t button,
                                     ...);
```

C11 6.7.2p4 gives such a declaration external linkage "exactly as if"
it were declared with the storage-class specifier `extern`, so this
changes no semantics and no generated code. It makes the linkage
visible where the declaration is written, which is how header-declared
*variables* must already be spelled:

```c
extern const ghostcon_style_t GC_STYLE_DEFAULT;
```

Left alone: definitions (a body follows the parameter list), `static`
prototypes (internal linkage, where `extern` would contradict it),
function-pointer typedefs (`typedef void (*fn)(int)`), and
declarations that span a preprocessor line.

Wrapped parameter lists keep their alignment. When the parameters
start on the same line as the open paren, the continuation lines are
aligned under that paren and move right with it; when the open paren
ends its line, the parameters start on the next line with a fixed
hanging indent, which the keyword does not affect.

### Comments — wrapped block style

```c
/* Formats "CLEAR\n" into buf. Returns written length, or 0 if it
 * wouldn't fit.
 *
 * Sent over the control socket by pty_child.c when its session dies
 *
 * Tells ghostcon-core the previous owner is gone, so it can wipe the
 * display and scrollback instead of leaving the old screenful under the
 * new login prompt.
 */
```

Docs are meant to be brief. Keep prose paragraphs to at most ~3
wrapped lines at ~82 columns, one idea each, with a blank ` *` between
paragraphs. Lowercase detail paragraphs (`pass 0/0 ...`, `see ...`) are
fine; capitalization and terminal periods are not enforced. Prefer
separate paragraphs over semicolon or em-dash chains.

Enumerations should be separate `- item` paragraphs, or label lines with
indented descriptions. The formatter splits long paragraphs at sentence
boundaries and starts a new lowercase paragraph at semicolons before
common connectors. It does not mechanically split em dashes: those need
hand editing, because blind splitting breaks parentheticals.

Long asides: if a parenthetical would run more than a line, or contains
several clauses, move it to a numbered footnote paragraph instead of
breaking the sentence:

    ... does not consult the fallback chain[1].
    *
    * [1] Ligature shaping only runs against the primary face's own
    *     hb_font_t.

Leave just the marker inline. Indent footnote continuation lines so they
group with the marker.

Cross-reference instructions follow the same rule. If a `see X` aside
would make a sentence long, end the sentence with `[n]` and put `See X`
in the footnote block. A standalone `See X.` paragraph is already brief
and can stay as one.

The same applies to struct-field comments: keep the trailing comment to
one line (`float is_glyph; // 1.0 for glyph quads, else 0.0 [2]`) and put
the long explanation in a `[n]` block after the field list, rather than
letting the comment wrap into a column-block.

Inline/trailing comments are untouched. License headers, ASCII banners,
comments inside `#define` continuations, and verbatim code/table blocks
are skipped. Boxed runs of adjacent one-line comments collapse into one
block. `tools/format_comments.py` enforces this (and is idempotent).

### Editor / clangd

`.clangd` pins clangd to the repository-root `compile_commands.json`
(a git-ignored symlink to `build-release/compile_commands.json`), so
include paths work regardless of the editor's workspace root.
`.clang-tidy` defines the curated check set (clang-analyzer, bugprone,
performance, portability, minus the noisy checks documented there).

- clangd and `clang-tidy -p build-release <file>` use the same config.
- Formatting is deliberately NOT delegated to clang-format: the house
  rules above are enforced by `tools/format_*.py`, which clang-format
  cannot express.
- If the compile database goes stale (new build dir, new dependency),
  regenerate it with `meson setup`/`ninja` and re-point the symlink.

### Tests

`meson test -C build-release` must stay green (currently 25 tests).

## 1. Where things live now (post-refactor map)

| Concern | Module |
|---|---|
| Event loop, VT lifecycle orchestration | `src/core/main.c` (~1.6k lines) |
| DRM/KMS/GBM/EGL/GLES, master retry, cursor plane, frame present | `src/core/display.c` (`ghostcon_display_*`) |
| Hover hit-testing, selection transitions, zoom clamp, comm-name | `src/core/ui.c` (`ghostcon_ui_*`) |
| Raw input dispatch (libinput/xkb/key encoding) | `src/core/input.c` |
| Terminal emulation state machine (ECMA-48/DEC) | `src/term/stream/stream.c` |
| CSI sequences | `src/term/stream/stream_csi.c` |
| OSC sequences | `src/term/stream/stream_osc.c` |
| APC (Kitty graphics), DCS (sixel) | `src/term/stream/stream_apc.c` |
| Grid/cursor/scrollback/selection model | `src/term/screen.c` (+ `screen_cursor.c`, `screen_edit.c`, `screen_scroll.c`), `cell.c`, `row.c` |
| Frame rendering (glyphs, images, cursor, selection) | `src/render/machine.c`, `gles.c` |
| Config load/hot-reload | `src/config/config.c`, `include/ghostcon/config/config.h` |
| Crash reporting (journald) | `src/core/diag.c` |
| Domain debug logging | `include/ghostcon/core/log.h` |
| Offline state inspection | `tools/gc_inspect.c` (`gc-inspect`) |

---

## 2. Recipe: add a TOML config option

Example goal: `[general] my_option = 42`.

1. **Field** — add to `ghostcon_config_t` in
   `include/ghostcon/config/config.h` (doc-comment what it means and
   whether it applies live or only on respawn).
2. **Default** — set it in `ghostcon_config_defaults()` in
   `src/config/config.c`.
3. **Parse** — in `ghostcon_config_load()`, inside the right table
   (`general`, `keybindings`, `cursor`, `mouse`, `touchpad`, ...), add:
   ```c
   load_int(general, "my_option", &cfg->my_option);      /* or */
   load_string(section, "my_option", cfg->my_option, sizeof(cfg->my_option));
   load_bool(section, "my_option", &cfg->my_option);
   load_double(section, "my_option", &cfg->my_option);
   ```
4. **Env export** (only if supervisor/undead-head/ghost-ptyserv need it
   at spawn time) — add it to `ghostcon_config_export_env()` in
   `src/config/config.c`.
5. **Test** — extend `tests/test_config.c` (parse a TOML string,
   assert the field; also assert the default for an empty file).
6. **Consume it live** (only if `ghostcon-core` must react without a
   restart):
   - Add a field to `app_t` in `src/core/main.c` and copy it in the
     initial config-load block in `main()`.
   - In `apply_config_reload()`, diff old vs new (`strcmp`/`==`) and
     only then apply side effects, exactly like `theme_changed`,
     `gamma_correct`, and the `[cursor]` block do.
   - If it must survive VT release/reacquire, keep it in `app_t` (not
     just in the input/display object) and re-push it in
     `acquire_display()`, the same way `apply_keybindings()` and
     `apply_mouse_touchpad_config()` are re-run after every
     `ghostcon_input_open()`.
7. **Document** — add it to `share/ghostcon.toml.example`.

Run `meson test -C build-release test_config`.

---

## 3. Recipe: add a keyboard shortcut

Current architecture (there is no generic action enum yet): each feature
owns its binding. Clipboard is the model to copy.

1. **Handler** — add a `static bool handle_my_action(ghostcon_input_t *,
   ...)` in `src/core/input.c`, and call it from
   `ghostcon_input_dispatch()` when its key/mods match.
   - To compare against a configurable spec, parse it once with
     `ghostcon_parse_keybinding(spec, &mods, &evdev)`
     (`include/ghostcon/core/input.h`) and compare with
     `input->mods`/`evdev_code` in the dispatch switch.
   - For focus-stealing safety, terminals route VT-switch combos
     (`is_vt_switch_combo()`) before app input; follow that pattern if
     the shortcut must never reach the PTY.
2. **Expose it (if configurable)** —
   - `char my_binding[64]` in `ghostcon_config_t`; default in
     `ghostcon_config_defaults()`; `load_string(keybindings, ...)`.
   - Persist it in `app_t` (`src/core/main.c`) and add a
     `ghostcon_input_set_my_binding()` in `input.h`/`input.c` whose
     defaults are set in `ghostcon_input_open()`.
   - Re-push it from `apply_keybindings()` (called after every
     `ghostcon_input_open()` and on config reload) — a fresh libinput
     context otherwise silently reverts to hardcoded defaults.
3. **Test** — extend `tests/test_input.c` for the parser/dispatch part.
4. **Document** — `share/ghostcon.toml.example` under `[keybindings]`.

Actions that need display/font state (zoom, screenshots) are reported
back to `main.c` through `ghostcon_input_dispatch()`'s out-params
(`zoom_delta`, `pointer`, `dump_requested`); add an out-param or extend
`ghostcon_input_pointer_t` rather than reaching into `app_t` from
`input.c`.

---

## 4. Recipe: add an escape sequence

### CSI (e.g. a new `CSI Ps xyz`)

1. Add a handler in `src/term/stream/stream_csi.c`:
   ```c
   static void
   handle_csi_my_thing(ghostcon_stream_t *st, ghostcon_screen_t *s) {
       if (!csi_param_count(st, 0, 1)) return;
       int n = PARAM1(st, 1);
       ghostcon_screen_my_thing(s, n);
   }
   ```
   Shared helpers (`PARAM*`, `csi_param_count()`, `clamp_loop_count()`,
   `stream_output()`) come from `src/term/stream/stream_csi.h` -- the
   module's private header, which only this module includes.
2. Register it in `csi_table[]` in the same file — one row:
   ```c
   { 'x', 0, 0, 0, handle_csi_my_thing },
   ```
   A sequence that takes an intermediate (a private marker: `?`, `>`,
   `<`, `=`, `$`, space, `"`) adds a row per variant, giving the number
   of intermediates and then the intermediates themselves:
   ```c
   { 'x', 1, '?', 0, handle_csi_my_thing },
   ```
   The table is sorted by final byte, because `csi_lookup()` searches it
   with a binary search. `test_csi_dispatch` checks both that it is
   sorted and that each variant still dispatches, so put a new row in the
   right place rather than at the end.
3. If it needs new screen state, add accessors in
   `src/term/screen.c`/`screen.h` (not in the parser).
4. Tests: add a case to `tests/test_conformance.c` (escape-level
   behavior) or `tests/test_term.c`; compare against real Ghostty when
   in doubt (see `tools/symbol_probe.c` for a manual cross-check
   pattern).

### OSC (e.g. a new `OSC 1337;...`)

1. In `src/term/stream/stream_osc.c`, add a handler and one row to
   `osc_dispatch_table[]`, keyed on the leading number (the number tier
   is deliberately manual because libghostty-vt's C API doesn't expose
   payloads):
   ```c
   static bool
   osc_handle_my_thing(ghostcon_stream_t *st, ghostcon_screen_t *s,
                       long num, char **fields, int nfields)
   {
       ...
       return true;   /* recognized */
   }
   ```
   ```c
   { 1234, osc_handle_my_thing },
   ```
   Return `true` if you recognized the number, `false` to fall through
   to the libghostty-vt path.
2. If the payload is structured, split it with
   `osc_split_fields(copy, len, fields, n)` and remember it mutates in
   place — the existing code copies `st->buf` first for exactly that
   reason.
3. Reply through `osc_write_response(st, body)`.
4. Test: `tests/test_osc.c`.

### DCS / APC

Add a branch keyed on `st->dcs_final` in `gc_stream_dcs_dispatch()`
(`src/term/stream/stream_apc.c`) or on `st->apc_introducer` in
`gc_stream_apc_dispatch()`. DCS payload collection/growth is already
handled by the state machine.

Note: unknown CSI finals and discarded OSCs log only under
`GHOSTCON_LOG=stream` (see below), which is exactly what you want while
implementing a new sequence.

---

## 5. Recipe: debug a rendering/state problem offline

Record the raw PTY bytes, then inspect the model without a display:

```sh
# 1a. From a headless probe run (see CLAUDE.md):
HEADLESS_PROBE_RAW_DUMP=/tmp/session.raw \
  build-release/src/headless_probe 1920 1080 16 500 -- nmtui

# 1b. Or from a live session: Ctrl+Alt+D writes /tmp/ghostcon-dump.txt
#     (screen dump) and asks the transport for its raw ring buffer.

# 2. Inspect every cell, including content-tag backgrounds:
build-release/src/gc-inspect --cols 192 --rows 51 /tmp/session.raw
build-release/src/gc-inspect --offset 20 /tmp/session.raw   # scrollback view
```

Each cell line reports `tag=`, `style=`, `wide=`, `link=`, and — for
`bg_palette`/`bg_rgb` cells — the actual `content=` payload. That is the
one thing the older dump paths never showed (see `CLAUDE.md`).

Useful greps:

```sh
gc-inspect ... | grep "tag=bg_"            # erased/colored blank cells
gc-inspect ... | grep "wide=spacer"        # wide-char bookkeeping
gc-inspect ... | head -6                   # modes/cursor/selection header
```

Debug logging channels (stderr; journald captures service stderr):

```sh
sudo env GHOSTCON_LOG=vt,drm systemctl restart ghostcon.service   # boot only
# or run a component directly while testing:
GHOSTCON_LOG=input,selection /usr/libexec/ghostcon/ghostcon-core 4
```

Domains: `vt`, `drm`, `cursor`, `input`, `selection`, `config`, `pty`,
`stream`, or `all`. Unset = silent.

---

## 6. Recipe: safe VT/KMS hardware testing

```sh
# Unit/integration tests, no display or VT needed:
meson test -C build-release

# One real frame through a DRM render node (no modeset, no VT takeover):
build-release/src/headless_probe out.ppm 1920 1080 16 500 -- htop

# Parser/rendering parity cross-checks:
build-release/src/ghostcon_dump 80 24 saved.raw      # style-oriented dump
build-release/src/gc-inspect saved.raw               # tag-aware cell dump
```

Every headless GLES path (`test_render`, `headless_probe`, `bench_render`)
takes its DRM node from `GHOSTCON_DRM_RENDER_NODE` (default
`/dev/dri/renderD128`; see `include/ghostcon/core/render_node.h`). When no
render node is present, `test_render` SKIPs (exit 77) instead of failing, so a
GPU-less CI runner can still run the rest of the suite.

To exercise the render path with no GPU at all, back the node with a virtual
DRM device (load `vkms`) and force Mesa's software rasterizer:

```sh
sudo modprobe vkms
GALLIUM_DRIVER=llvmpipe meson test -C build-release test_render
```

`test_vtctl` (`tests/test_vtctl.c`) covers `vtctl.c`'s pure handshake
transition -- the release-before-ack ordering -- with no `/dev/ttyN`. The
ioctl/signal side of `vtctl.c` still needs a real VT (a QEMU guest works).

Only the acquire/release path touches real DRM master and VT signals. To
exercise that, deploy and switch VTs on the physical console:

```sh
ninja -C build-release
sudo meson install -C build-release
sudo systemctl restart ghostcon.service   # drops the ttyN session(s)
```

Restarting the service **drops live VT sessions** (it is the console
renderer, not a background daemon) — confirm with anyone using those
VTs first. After switching away and back a few times, check:

```sh
journalctl -u ghostcon.service --since "-5 min" | grep -i "display"
```

You should see matched `display released` / `display acquired` pairs and
no `drmSetMaster failed`.

---

## 7. Refactor invariants to preserve

- **DRM details stay in `display.c`.** `main.c` must not call KMS/EGL/
  GLES/drmSetMaster directly; add a method to `display.h` instead.
- **UI logic stays DRM-free.** `ui.c` may read the screen and mutate
  selection, but must not know KMS exists — that keeps it unit-testable.
- **Selection/zoom/hover changes go through `ui.c`**, pointer moves and
  cursor-state commits through `display.c`. Do not reintroduce the
  inline copies in `main.c`.
- **A blank colored cell uses a content tag** (`GC_CELL_BG_COLOR_*`),
  never an interned style. Any new code that reads cell backgrounds must
  check `ghostcon_cell_get_tag()` in addition to `style_id`.
- **Release order matters:** `ghostcon_display_release()` (drops DRM
  master) runs in `vtctl_pre_ack` *before* the `VT_RELDISP` ack. Do not
  move it later; VT switching has hung on this before.
- **One handler line beats one switch case:** new CSI/OSC/DCS support
  belongs in the stream dispatch modules, not in `stream.c`'s state
  machine.
