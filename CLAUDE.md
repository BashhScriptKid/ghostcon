# Working in ghostcon

## Debugging rendering-parity bugs (background color, erase, etc.)

When ghostcon's output looks wrong compared to a real terminal (Ghostty),
don't stop at "does ghostcon's model match a captured byte stream" — the
comparison tooling itself can have blind spots that make a real bug look
like a non-issue. What actually resolved the last such investigation:

1. **Reproduce headlessly first.** `tools/headless_probe.c` runs a real
   child program (e.g. `nmtui`) under a pty and renders one frame via a
   DRM render node — no display, no VT, no physical console needed.
   Build it with `ninja -C <builddir> src/headless_probe` (note: the
   ninja target is `src/headless_probe`, not `headless_probe`). Pass
   `HEADLESS_PROBE_RAW_DUMP=<path>` to also capture the raw pty bytes for
   later replay/diffing.

2. **Match the real environment's grid, not an arbitrary size.** Derive
   cols/rows from the real viewport (e.g. 1920x1080) and the real
   configured font — `headless_probe`'s `<viewport_w> <viewport_h>
   <font_size> [settle_ms] [font_family] [font_variant]` args do this the
   same way `core/main.c` does (cell size first, then `cols =
   width/cell_w`). A TUI's own layout math can behave very differently at
   a mismatched size.

3. **Don't trust an offline dump tool without checking what it actually
   reports.** `tools/ghostcon_dump.c` (via `term/dump.c`) and the
   external reference `ghostty_dump` binary both only reported
   *style*-based background info for a long time — meaning a cell whose
   background came from the newer content-tag mechanism (see below)
   showed up as "default" in both dumps even though it wasn't. Two
   independent tools agreeing is not proof of correctness if they share
   the same blind spot. If a comparison looks suspiciously clean, check
   what the dump format actually captures before trusting it.

4. **When in doubt, diff against a *live* Ghostty session, not just its
   library.** Ghostty ships a real-time terminal inspector
   (`ctrl+shift+i` by default, or `keybind = inspector:toggle`) with a
   Cell tab that shows the *actual* live internal state of any cell —
   including its `Content` type, which is a different axis from `Style
   ID`/`Style`. This is what actually found the real bug: a cell can have
   `Style ID: 0 (default)` and still carry a real background via
   `Content: bg_color_palette`. An offline library replay (`ghostty_dump`)
   never surfaced this distinction.

## Cell background representation

`include/ghostcon/term/cell.h` has two content tags,
`GHOSTCON_CELL_BG_COLOR_PALETTE` and `GHOSTCON_CELL_BG_COLOR_RGB`, for a
blank cell that carries *only* a background color. This matches Ghostty's
own representation and is what erase operations (ED/EL/ECH, and the blank
rows scrolling produces) should use — see `erase_fill_cell()` in
`src/term/screen.c`. Do **not** represent an erase-only background by
interning a style (a near-duplicate-of-default style per erase color) —
that was the first attempt, and while it "worked" in that the color did
show up, it didn't match how a real terminal or the renderer's own
optimizations expect the color to be found, and it caused a real coverage
gap for large erased regions than the content-tag path doesn't have.

Any code that reads a cell's background/rendering info (renderer,
dump/debug tools, selection/copy logic) needs to check
`ghostcon_cell_get_tag()` for `BG_COLOR_PALETTE`/`BG_COLOR_RGB` in
addition to (not instead of) the cell's `style_id` — style_id alone is
not the full story for a blank cell.

## Live service deploy/redeploy

The real target for verifying against tty4 is the `build-release` meson
build directory (configured for `prefix=/usr`), not `build-san`
(sanitizer build — useful for tests, not for the live service). Deploy
with:

```
ninja -C build-release
sudo meson install -C build-release   # passwordless sudo is configured
sudo systemctl restart ghostcon.service
```

Restarting the service **will drop the tty4 session** if you're
interacting with it through that VT (it's a real virtual-console
renderer, not a background daemon) — always confirm before restarting,
even mid-investigation.
