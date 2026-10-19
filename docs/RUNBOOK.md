# ghostcon Maintenance Runbook

Concrete recipes for extending and debugging ghostcon without an LLM.
See `docs/REFACTOR_PLAN.md` for the architecture rationale; this file is
the "what file do I touch" companion.

After **every** change, the minimum verification is:

```sh
ninja -C build-release
meson test -C build-release          # currently 21 tests
```

---

## 1. Where things live now (post-refactor map)

| Concern | Module |
|---|---|
| Event loop, VT lifecycle orchestration | `src/core/main.c` (~1.2k lines) |
| DRM/KMS/GBM/EGL/GLES, master retry, cursor plane, frame present | `src/core/display.c` (`ghostcon_display_*`) |
| Hover hit-testing, selection transitions, zoom clamp, comm-name | `src/core/ui.c` (`ghostcon_ui_*`) |
| Raw input dispatch (libinput/xkb/key encoding) | `src/core/input.c` |
| Terminal emulation state machine (ECMA-48/DEC) | `src/term/stream.c` |
| CSI sequences | `src/term/stream_csi.c` |
| OSC sequences | `src/term/stream_osc.c` |
| APC (Kitty graphics), DCS (sixel) | `src/term/stream_apc.c` |
| Grid/cursor/scrollback/selection model | `src/term/screen.c`, `cell.c`, `row.c` |
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
3. **Parse** — in `ghostcon_config_load()`, add one row to the
   relevant field table (`GENERAL_FIELDS`, `CURSOR_FIELDS`,
   `KEYBINDINGS_FIELDS`, `MOUSE_FIELDS`, `TOUCHPAD_FIELDS`):
   ```c
   CFG_INT   ("my_option", my_option),
   CFG_STRING("my_option", my_option),
   CFG_BOOL  ("my_option", my_option),
   CFG_FLOAT ("my_option", my_option),
   ```
   The table row is type, TOML key, and struct member; `load_fields()`
   derives the offset and capacity automatically. Adding a new table
   means defining its `cfg_field_t[]` and calling `load_fields()` once
   with `CFG_FIELD_COUNT(...)`.
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

1. Add a handler in `src/term/stream_csi.c`:
   ```c
   static void
   handle_csi_my_thing(ghostcon_stream_t *st, ghostcon_screen_t *s) {
       if (!csi_param_count(st, 0, 1)) return;
       int n = PARAM1(st, 1);
       ghostcon_screen_my_thing(s, n);
   }
   ```
   Shared helpers (`PARAM*`, `csi_param_count()`, `clamp_loop_count()`,
   `stream_output()`) come from `src/term/stream_internal.h`.
2. Register it in `csi_lookup()` in the same file — one line in the
   intermediate-less switch:
   ```c
   case 'x': return handle_csi_my_thing;
   ```
   Sequences that take intermediates/private markers (`?`, `>`, `$`,
   space, `"`) go in the first (intermediate-discriminating) switch.
3. If it needs new screen state, add accessors in
   `src/term/screen.c`/`screen.h` (not in the parser).
4. Tests: add a case to `tests/test_conformance.c` (escape-level
   behavior) or `tests/test_term.c`; compare against real Ghostty when
   in doubt (see `tools/symbol_probe.c` for a manual cross-check
   pattern).

### OSC (e.g. a new `OSC 1337;...`)

1. In `src/term/stream_osc.c`, add a `case` to `osc_dispatch_manual()`
   keyed on the leading number (the number tier is deliberately manual
   because libghostty-vt's C API doesn't expose payloads). Return
   `true` if you recognized the number.
2. If the payload is structured, split it with
   `osc_split_fields(copy, len, fields, n)` and remember it mutates in
   place — the existing code copies `st->buf` first for exactly that
   reason.
3. Reply through `osc_write_response(st, body)`.
4. Test: `tests/test_osc.c`.

### DCS / APC

Add a branch keyed on `st->dcs_final` in `gc_stream_dcs_dispatch()`
(`src/term/stream_apc.c`) or on `st->apc_introducer` in
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
- **A blank colored cell uses a content tag** (`GHOSTCON_CELL_BG_COLOR_*`),
  never an interned style. Any new code that reads cell backgrounds must
  check `ghostcon_cell_get_tag()` in addition to `style_id`.
- **Release order matters:** `ghostcon_display_release()` (drops DRM
  master) runs in `vtctl_pre_ack` *before* the `VT_RELDISP` ack. Do not
  move it later; VT switching has hung on this before.
- **One handler line beats one switch case:** new CSI/OSC/DCS support
  belongs in the stream dispatch modules, not in `stream.c`'s state
  machine.
