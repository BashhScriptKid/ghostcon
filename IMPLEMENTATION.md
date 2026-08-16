# ghostcon — Implementation Guide

## Overview

`libghostty-vt` (installed on this system at `/usr/lib/libghostty-vt.so`,
version 0.1.0) is **not** a complete terminal emulator. It exports:

- **OSC parser**: `ghostty_osc_new/next/end/command_type/command_data` —
  parses OSC escape sequences into typed commands.
- **SGR parser**: `ghostty_sgr_new/set_params/next` — parses SGR sequences
  into text attributes (bold, italic, colors, etc.).
- **Key encoder**: `ghostty_key_encoder_encode` — encodes key events into
  Kitty keyboard protocol sequences.
- **Paste check**: `ghostty_paste_is_safe` — validates paste data.
- **Color utilities**: `ghostty_color_rgb_get`, palette constants.

> **Note (verified against the installed v0.1.0)**: `/usr/include/ghostty/vt/`
> exports only `allocator.h`, `color.h`, `key/`, `key.h`, `osc.h`, `paste.h`,
> `result.h`, `sgr.h`, `wasm.h`. The SIMD helpers listed below are **not**
> exported — ghostcon ports UTF-8 decoding and codepoint width itself
> (see "UTF-8 decoding" below).
>
> ~~SIMD helpers: `ghostty_simd_codepoint_width`, `ghostty_simd_index_of`,
> `ghostty_simd_decode_utf8_until_control_seq`.~~

What it does NOT provide: terminal state machine (screen buffer, grid,
cursor tracking, scrollback, scrollback search, selection, alt screen),
resize/reflow, or any GPU rendering.

The full Ghostty terminal emulator lives in Zig source at
`github.com/ghostty-org/ghostty`. Its terminal core (`src/terminal/`)
processes byte streams into screen state; its renderer (`src/renderer/`)
draws quads from a font atlas via OpenGL/Metal/Vulkan.

**Strategy**: port the essential terminal state machine from Ghostty's Zig
to C, reusing `libghostty-vt` for the parsers it already exposes. Write a
GLES 2.0 renderer (using Ghostty's rendering architecture as reference).
This gives us identical behavioral correctness to Ghostty without requiring
a Zig build chain or depending on libtsm's protocol ceiling.

---

## Important: Phase 0 may reshape this entire plan

Everything below this line (data structures, C types, function signatures,
renderer design, phase ordering) is a **best guess** before reading the
actual Ghostty Zig source. Phase 0 — the direct port of `stream.zig`,
`Screen.zig`, and their dependencies — is the first time we see the real
constraints:

- Ghostty's Zig code may use features with no direct C equivalent
  (comptime generics, error unions, slices with runtime length, packed
  structs with Zig-specific layout rules). The C port may require
  different data structures than assumed here.
- The screen/cell/row data model in the actual source may not match
  the simple grid assumed below. Ghostty may use sparse rows,
  copy-on-write cells, or event-driven screen updates that require
  a different damage tracking approach.
- The stream processor's callback model may assume an allocator or
  async pattern that forces a different event loop structure.
- Ghostty's actual GL renderer may reveal requirements (e.g., the
  atlas format, shader complexity, or buffer management strategy)
  that change the GLES 2.0 renderer design.

**Rules of the road**:

1. Update this file as Phase 0 progresses. Don't let the plan diverge
   from what the actual port requires.
2. If a ported Zig function forces a C design decision not anticipated
   here, document the departure and the reasoning.
3. The API surface (`ghostcon_term_*` functions) is provisional until
   the port is complete. Lock it down only after Phase 0 is done.
4. The renderer phase (Phase 0.5) starts AFTER the screen/stream types
   are settled. Don't parallelize these — the renderer depends on the
   exact screen data model.

With that caveat, here is the provisional implementation guide.

## 1. Ghostty Source Map (reference)

```
src/
├── main.zig              ← entry point (app shell, irrelevant)
├── Terminal.zig          ← top-level Terminal struct (the API surface)
├── terminal/
│   ├── Screen.zig        ← screen buffer, grid of cells, scrollback
│   ├── Row.zig           ← a single row of cells
│   ├── Cell.zig          ← a single cell (char, style, hyperlink)
│   ├── color.zig         ← color handling (palette, truecolor, indexed)
│   ├── selection.zig     ← selection state and management
│   ├── stream.zig        ← byte stream processor (the main parser loop)
│   ├── kitty.zig         ← Kitty keyboard protocol state
│   ├── modifier.zig      ← modifier key handling
│   ├── mouse.zig         ← mouse protocol handling
│   ├── scrollback.zig    ← scrollback buffer
│   ├── search.zig        ← scrollback search
│   ├── size.zig          ← terminal size/cell dimensions
│   ├── SynchronizedOutput.zig  ← mode 2026 synchronization
│   └── osc/              ← OSC handlers (already in libghostty-vt)
├── renderer/
│   ├── Machine.zig       ← render state machine (damage → commands)
│   ├── Metal.zig         ← Metal backend (macOS)
│   ├── vulkan/
│   │   ├── VkMachine.zig ← Vulkan backend (Linux/Windows)
│   │   └── shaders/      ← GLSL/Vulkan shaders
│   └── atlas.zig         ← glyph atlas management
├── font/
│   ├── face.zig          ← font face loading (fontconfig + freetype)
│   └── shape.zig         ← text shaping (harfbuzz)
└── vt/
    ├── osc.zig           ← OSC parser (already in libghostty-vt)
    ├── sgr.zig           ← SGR parser (already in libghostty-vt)
    └── key.zig           ← key encoder (already in libghostty-vt)
```

The port target is `terminal/` (Screen, Row, Cell, stream, scrollback,
selection, kitty, SynchronizedOutput) plus the font atlas and render
command generation from `renderer/`. The renderer backends (Metal,
Vulkan) are replaced by a KMS/EGL/GLES 2.0 renderer written fresh.

---

## 2. Porting Strategy

### What we keep from libghostty-vt (C API, already compiled)

| Header | Symbols | Use |
|--------|---------|-----|
| `ghostty/vt/osc.h` | `osc_new`, `osc_next`, `osc_end`, etc. | Parse OSC sequences from byte stream |
| `ghostty/vt/sgr.h` | `sgr_new`, `sgr_set_params`, `sgr_next`, etc. | Parse SGR parameters into text attributes |
| `ghostty/vt/key.h` | `key_encoder_encode`, `key_event_*` | Encode keyboard input to terminal sequences |
| `ghostty/vt/paste.h` | `paste_is_safe` | Validate clipboard paste data |
| `ghostty/vt/color.h` | `color_rgb_get`, palette constants | Color handling utilities |

### What we port from Ghostty Zig → C

| Zig Source | C Destination | What It Does |
|------------|---------------|--------------|
| `src/terminal/Cell.zig` | `src/term/cell.h` + `cell.c` | Cell struct: codepoint, style ID, hyperlink flag, width |
| `src/terminal/style.zig` | `src/term/style.h` + `style.c` | Style attributes (bold/italic/underline/colors) as an interned table addressed by 15-bit `ghostcon_style_id_t`; cells store an ID, not inline style bits |
| `src/terminal/Row.zig` | `src/term/row.h` + `row.c` | Row of cells, wrapped-char handling |
| `src/terminal/Screen.zig` | `src/term/screen.h` + `screen.c` | Grid of rows, scrollback ring buffer, cursor, alt screen |
| `src/terminal/stream.zig` | `src/term/stream.h` + `stream.c` | Byte stream parser (the main ESC[/OSC/DCS/etc. dispatch loop) |
| `src/terminal/selection.zig` | `src/term/selection.h` + `selection.c` | Selection tracking |
| `src/terminal/scrollback.zig` | `src/term/screen.c` (inline) | Scrollback is part of Screen |
| `src/terminal/SynchronizedOutput.zig` | `src/term/screen.h` + `screen.c` | Mode 2026 defer/commit flag |
| `src/terminal/kitty.zig` | `src/term/kitty.h` + `kitty.c` | Kitty keyboard protocol state tracking |
| `src/terminal/size.zig` | `src/term/size.h` (struct only) | Cell dimensions |
| `src/terminal/color.zig` | `src/term/color.h` + `color.c` | Color palette, indexed→RGB conversion |
| `src/renderer/atlas.zig` | `src/render/atlas.h` + `atlas.c` | Glyph atlas (freetype → GPU texture) |
| `src/renderer/Machine.zig` | `src/render/machine.h` + `machine.c` | Damage → render command generation |

### What we write fresh

| File | Purpose |
|------|---------|
| `src/render/gles.c` + `gles.h` | GLES 2.0 renderer: EGL/GBM setup, shaders, draw calls |
| `src/render/shaders/gles2.vert` | Vertex shader for glyph quads |
| `src/render/shaders/gles2.frag` | Fragment shader for glyph quads |
| `src/term/term.c` + `term.h` | Top-level terminal handle orchestrating screen, stream, render |
| `src/core/kms.c` | DRM/KMS mode setting, GBM buffer allocation, page flip |
| `src/core/egl.c` | EGL initialization on GBM surface |
| `src/core/input.c` | libinput event loop → key/mouse event translation |
| `src/core/vtctl.c` | VT_PROCESS ioctl handshake (SIGUSR1/SIGUSR2) |
| `src/core/transport.c` | pty child socket (connect, feed bytes, receive bytes) |

---

## 3. Core Data Structures (C port)

### Cell (`src/term/cell.h`)

> **Departure (Phase 0, as built):** the pre-port guess below assumed each
> cell carries its style inline as packed `uint32_t` bits. The actual port
> follows Ghostty's `style.zig` instead: styles live in an **interned
> table** (`src/term/style.h` + `style.c`) and each cell stores a 15-bit
> `ghostcon_style_id_t` referencing it, with ID 0 reserved for the default
> style. This matters because runs of identically-styled cells — the
> common case — collapse to a repeated ID rather than duplicating full
> attribute state per cell, and because truecolor fg/bg/underline need
> more bits than an inline packed field affords. The flag set
> (`GC_STYLE_BOLD`, `DIM`, `ITALIC`, `UNDERLINE`, `BLINK`, `INVERSE`,
> `HIDDEN`, `STRIKETHROUGH`, `OVERLINE`, the three `*_TRUECOLOR` bits and
> the two `*_DEFAULT` bits) maps 1:1 to Ghostty's. Treat the struct below
> as historical; `cell.h` and `style.h` are the truth.

```c
typedef struct {
    uint32_t codepoint;        // Unicode codepoint
    uint32_t style;            // packed style bits (see below)
    uint16_t hyperlink_id;     // 0 = no hyperlink
    uint8_t width;             // 1 or 2 (wide chars)
    uint8_t pad;
} ghostcon_cell_t;

// Style bits:
//   bits 0-7:   foreground color index (0-255) or special
//   bits 8-15:  background color index (0-255) or special
//   bit 16:     bold
//   bit 17:     italic
//   bit 18:     underline
//   bit 19:     strikethrough
//   bit 20:     blink
//   bit 21:     inverse
//   bit 22:     foreground is truecolor (next 24 bits in separate field)
//   bit 23:     background is truecolor
//   bits 24-31: underline style
```

### Row (`src/term/row.h`)

```c
typedef struct {
    ghostcon_cell_t *cells;    // array of cells (dynamically sized)
    uint16_t *wraps;           // bitmask: which positions have soft-wrapped
    uint16_t col_count;
    bool dirty;
} ghostcon_row_t;
```

### Screen (`src/term/screen.h`)

```c
typedef struct {
    ghostcon_row_t *rows;              // visible rows (ring buffer view)
    ghostcon_row_t *scrollback;        // scrollback rows (ring buffer)
    int cursor_x, cursor_y;
    int cols, rows_visible;
    int scrollback_capacity;
    int scrollback_count;
    int scroll_pos;                    // scrollback offset (0 = active)
    bool alt_screen_active;
    bool synchronized_output;          // mode 2026
    // Damage tracking
    bool *dirty_lines;
    int min_dirty, max_dirty;
} ghostcon_screen_t;
```

### Damage callback model

The render loop polls the screen for dirty lines each frame:

```c
// After feeding bytes via stream_process(), the render loop:
for (int y = 0; y < screen->rows_visible; y++) {
    if (screen->dirty_lines[y]) {
        for (int x = 0; x < screen->cols; x++) {
            ghostcon_cell_t *cell = &screen->rows[y].cells[x];
            // queue glyph quad for this cell
        }
    }
}
```

This is intentionally simpler than Ghostty's damage → command model.
For a VT terminal with predictable frame pacing (vsync-locked KMS flip),
redrawing all dirty cells each frame is sufficient and keeps the port
surface small. Ghostty's more sophisticated damage optimization
(render command coalescing, incremental GPU updates) can be added later.

---

## 4. Renderer Architecture

### Pipeline

```
1. Frame start (triggered by KMS vsync or input event)
2. Poll libinput (non-blocking)
3. Process any new PTY bytes via stream_process()
4. If dirty lines:
   a. Build GLES quad buffer from dirty cells
   b. Bind glyph atlas texture
   c. Draw quads (background rects + glyph rects + cursor)
   d. eglSwapBuffers → GBM buffer → KMS page flip
5. If no dirty lines: skip render (idle, no page flip)
6. Write canary heartbeat byte
7. Go to step 1 (event loop iteration)
```

### Font atlas (`src/render/atlas.c`)

```
- Fontconfig: select font by family/size/style
- Freetype: render each glyph to bitmap
- Pack bitmaps into a GPU texture atlas (power-of-two, e.g. 2048x2048)
- Cache: LRU, regenerate atlas when full
- Track: codepoint → UV rect in atlas
```

### GLES shaders

Vertex shader: pass-through (glyph quad positions from CPU buffer).

Fragment shader: sample texture atlas, apply foreground/background
colors, handle bold/italic/underline via offset/scale adjustments.

---

## 5. Byte Stream Processing

The stream processor (`src/term/stream.c`) is the heart of the terminal
state machine. It takes PTY bytes and mutates the screen:

```c
typedef enum {
    GHOSTCON_STREAM_GROUND,
    GHOSTCON_STREAM_ESC,
    GHOSTCON_STREAM_CSI,
    GHOSTCON_STREAM_OSC,
    GHOSTCON_STREAM_DCS,
    GHOSTCON_STREAM_SOS,
    GHOSTCON_STREAM_STRING_TERM,
    // ... (full DEC VT state machine)
} ghostcon_stream_state_t;

void ghostcon_stream_process(
    ghostcon_stream_t *st,
    const uint8_t *data,
    size_t len,
    ghostcon_screen_t *screen
);
```

This is a direct port of Ghostty's `stream.zig` — the same state
transitions, the same dispatch logic. Every escape sequence, control
character, and CSI/OSC/DCS handler is ported identically. `libghostty-vt`
provides the OSC and SGR sub-parsers that get called from within this
state machine.

### libghostty-vt integration points

```
In the OSC handler (called by stream processor when OSC sequence is complete):
  ghostty_osc_new(NULL, &parser);
  for each byte in osc_content: ghostty_osc_next(parser, byte);
  cmd = ghostty_osc_end(parser, terminator);
  switch (ghostty_osc_command_type(cmd)) {
      case GHOSTTY_OSC_COMMAND_CHANGE_WINDOW_TITLE:
          ghostty_osc_command_data(cmd, GHOSTTY_OSC_DATA_CHANGE_WINDOW_TITLE_STR, &title);
          screen_set_title(screen, title);
          break;
      case GHOSTTY_OSC_COMMAND_CLIPBOARD_CONTENTS:
          // handle clipboard OSC 52
          break;
      // ...
  }
  ghostty_osc_free(parser);

In the SGR handler (called by CSI handler when SGR sequence is received):
  ghostty_sgr_new(NULL, &parser);
  ghostty_sgr_set_params(parser, params, separators, param_count);
  while (ghostty_sgr_next(parser, &attr)) {
      switch (ghostty_sgr_attribute_tag(attr)) {
          case GHOSTTY_SGR_ATTR_BOLD:
              screen->cursor.bold = true; break;
          case GHOSTTY_SGR_ATTR_FG_8:
              screen->cursor.fg = attr.value.fg_8; break;
          // ...
      }
  }
  ghostty_sgr_free(parser);
```

### OSC sequences not handled by libghostty-vt

The installed `libghostty-vt` version 0.1.0 recognizes ~22 OSC command
types (see `GhosttyOscCommandType` in `osc.h`). For any OSC sequence
it doesn't recognize, `ghostty_osc_command_type()` returns
`GHOSTTY_OSC_COMMAND_INVALID`. The stream processor should:

- Log unrecognized OSC sequences to journald (structured, for debugging)
- Discard the payload
- Continue parsing normally — never hang or crash

### UTF-8 decoding (ported, not reused)

The installed `libghostty-vt` exports **no** UTF-8 or width helpers
(verified — see Overview note). ghostcon ports both itself:

- **`ghostcon_unicode_width()`** (`src/term/cell.c`): Markus Kuhn-style
  `wcwidth` (public domain), the same family of tables Ghostty's UCD data
  replaces. Returns 0 (combining/zero-width), 1, or 2. Fast path `cp <=
  0xFF → 1` mirrors Ghostty's `print`.

- **`ghostcon_stream_t.utf8_state` / `utf8_acc`** (`src/term/stream.c`):
  Bjoern Hoehrmann's DFA decoder, ported byte-for-byte from Ghostty's
  `UTF8Decoder.zig` (`char_classes`/`transitions` tables verified equal by
  script). Decoder state lives on the stream struct so a codepoint split
  across two `feed()` calls decodes correctly.

- **GROUND handling**: every byte in ground flows through the decoder
  (`stream_handle_codepoint` mirrors Ghostty's `handleCodepoint`): C0 → 
  `execute`, ESC → escape state, DEL ignored, everything else → `print`.
  Invalid bytes emit U+FFFD; after a mid-sequence reject the offending
  byte is retried (Ghostty `nextUtf8` semantics). 8-bit C1 controls
  (0x80–0x9F) now only dispatch in **non-ground** states — a lone C1 byte
  in ground is invalid UTF-8 and yields U+FFFD.

- **Width-aware `ghostcon_screen_put_char`** (`src/term/screen.c`):
  mirrors Ghostty's `print`/`printCell`/`printWrap`. Zero-width codepoints
  attach to the previous cell (tagged `CODEPOINT_GRAPHEME`; full combining
  storage is a later phase). Wide codepoints write `WIDE` + `SPACER_TAIL`
  cells, or a `SPACER_HEAD` + wrap when at the right edge. Right-limit,
  pending-wrap deferral, and soft-wrap row marking (`wrap` /
  `wrap_continuation`) follow Ghostty. Also fixed a latent insert-mode bug
  (cleared `cells[margin_region.left]` instead of `cells[cursor.x]`).

### CSI intermediate discrimination (ported, not reused)

Ghostty's parser collects **all** of `0x20–0x2F` *and* the private
markers `< = > ?` into `intermediates[]` when they lead a sequence
(`parse_table.zig` csi_entry `.collect` → csi_param); a private marker
seen mid-params routes the whole sequence to `csi_ignore`. ghostcon's
`csi_dispatch` (`src/term/stream.c`) mirrors Ghostty's
`switch (input.intermediates.len)` so these sequences no longer
misroute:

- `q`: `SP q` = DECSCUSR, `" q` = DECSCA, else ignored. Plain `CSI q`
  / `CSI 1 q` (no SP) are **ignored** like Ghostty — previously they
  were wrongly treated as cursor-style.
- `s`: `CSI s` = SCOSC (0 params) / DECSLRM (params, gated on mode 69);
  `? s` = save mode (XTSAVE); `> s` = XTSHIFTESCAPE.
- `r`: `CSI r` = DECSTBM; `? r` = restore mode (XTRESTORE).
- `h`/`l`: plain = SM/RM; `? h`/`? l` = DECSET/DECRST.
- `J`/`K`: plain = ED/EL; `? J`/`? K` = DECSED/DECSEL (selective erase).
- `c`: `CSI c` = DA1, `> c` = DA2, `= c` = DA3 (responses still need the
  output channel).
- `p`: `$ p` / `? $ p` = DECRQM (no-op until output channel); `! p`
  (DECSTR) and `" p` (DECSCL) are unhandled/ignored like Ghostty.

DECSCA feeds the `protected_mode` state (`GC_PROTECTED_OFF/DEC/ISO`) on the
screen. **Important (confirmed against Ghostty 1.3.1+):** `CSI 1 " q` = DEC
(only DECSED/DECSEL respect protected cells), while `CSI 0 " q` **and** `CSI
2 " q` both turn protection off — Ghostty maps Ps 2 to `.off`, never to ISO.
The ISO screen mode is internal-only (set by non-DECSCA callers); the erase
handlers' `protected_mode == ISO` branch exists to match that. `off` clears
`cursor.protected` but leaves `protected_mode` untouched (Ghostty: "NEVER
reset to .off because logic such as eraseChars depends on knowing what the
most recent mode was"). Cells written with `cursor.protected` set get the
cell protected bit; selective erase (`ghostcon_row_clear_range_unprotected`)
skips them. Save/restore mode (`? s`/`? r`) stores per-mode booleans in
`ghostcon_saved_modes_t`.

### Missing CSI finals (ported, not reused)

All of these were previously unhandled (returned `NULL` in dispatch) and
are now functional, matching Ghostty's `stream.zig` behavior:

- **REP** (`b`): repeat the preceding graphic character. `s->last_codepoint`
  is set in `ghostcon_screen_put_char` on every width≥1 print; `CSI Ps b`
  re-prints it Ps times (with `@max(Ps, 1)` — Ghostty semantics).
  `last_codepoint` is initialized to 0 (no previous char → no-op).
- **CHT** (`I`): forward tab Ps times — loops `ghostcon_screen_tab(s)`.
- **CBT** (`Z`): backward tab Ps times — loops `ghostcon_screen_tab_back(s)`.
- **HPA** (`` ` ``): cursor horizontal absolute — same as `G` (reuses
  `handle_csi_cursor_horizontal_abs`).
- **VPA** (`d`): cursor vertical absolute — new
  `ghostcon_screen_cursor_vertical_abs` respects origin mode (row offset
  by `scroll_region.top`, clamped to `scroll_region.bottom`).
- **HPR** (`a`): cursor horizontal relative — reuses `cursor_right`.
- **VPR** (`e`): cursor vertical relative — reuses `cursor_down`.

### Output channel (ported, not reused)

Terminal responses (DSR, DA, DECRPM) were TODO no-ops. Now they emit
bytes through a callback registered via
`ghostcon_term_set_output(term, fn, userdata)`. The callback receives
raw response bytes to write back to the PTY.

Implemented responses (matching Ghostty defaults):

- **DA1** (`CSI c`): `\x1b[?62;22c` (VT220 level-2 + ANSI color)
- **DA2** (`CSI > c`): `\x1b[>1;0;0c` (VT220, firmware 0)
- **DA3** (`CSI = c`): `\x1bP!|00000000\x1b\\` (DECRPTUI 0)
- **DSR 5** (`CSI 5 n`): `\x1b[0n` (operating status OK)
- **DSR 6** (`CSI 6 n`): `\x1b[row;colR` (cursor position report,
  1-based; with origin mode, coordinates are relative to scroll region)
- **DECRQM** (`CSI Ps $ p` / `CSI ? Ps $ p`): `\x1b[Ps;Pn$y` where Pn
  is 0 (not recognized), 1 (set), or 2 (reset). Covers modes 1/4/5/6/7
  (DEC) and 4/20 (ANSI) with real state checks; unknown modes → state 0.

### Ghostty comparison harness (Phase 0 validation)

`tools/` contains the Phase 0 validation harness that feeds the same byte
stream into both ghostcon and a real Ghostty terminal core and diffs the
resulting screen state:

- **`ghostty_dump.c`** — links a locally built `libghostty-vt` (master, which
  exposes the full `ghostty_terminal_*` C API; the distro 1.3.1 package only
  exports the parser subset). Feeds bytes in 256-byte chunks (exercising
  split-sequence handling) and dumps a canonical state.
- **`ghostcon_dump.c`** — same canonical dump for ghostcon.
- **`run_compare.sh`** — runs both dumpers over every file in `tools/corpus/`
  and diffs. Exit 0 = byte-identical screen state.
- **`gen_corpus.sh`** — generates the hand-written conformance `.seq` files.
  `script`-recorded real shell sessions (`session1-3.seq`) are checked in too.

Canonical dump format: `cols/rows/screen/cursor_x/cursor_y/pending_wrap`,
one `row<y>wrap=` line per row, and one `cell x y cp=0x%06X wide= prot= styled=`
line per non-empty cell.

The harness drove several correctness fixes in ghostcon (all now in
`tools/corpus/`):

- **Param-count strictness** — Ghostty drops a CSI sequence whose parameter
  count doesn't match the handler's `switch (input.params.len)` (count-based
  finals take 0/1, CUP/DECSTBM/DECSLRM take 0/1/2, DSR/DECRQM exactly 1) and
  drops *any* sequence once `params_idx >= 24`. ghostcon now mirrors both
  (`csi_param_count`, the `params_idx >= GC_STREAM_MAX_PARAMS` early return,
  and per-handler guards). Previously `CSI 1;2;3;…;30H` wrongly moved the
  cursor.
- **DECOM homes the cursor** — setting *and* resetting origin mode moves the
  cursor to home (Ghostty `.origin => setCursorPos(1,1)`).
- **Multi-mode DECSET/DECRST/SM/RM** — mode sequences iterate all params
  (`CSI ? 6;7 h` sets both DECOM and DECAWM), not just the first.
- **DECSCA `Ps=2` is OFF, not ISO** — Ghostty maps `CSI 2 " q` to `.off`; the
  ISO screen mode is internal-only. `off` clears `cursor.protected` but
  leaves `protected_mode` untouched.
- **Soft-wrap flag placement on scroll** — `ghostcon_row_clear` now resets
  `wrap`/`wrap_continuation` (matching Ghostty's `clearRows`/scroll blanks);
  EL-right and ED-below reset the cursor row's wrap (`cursorResetWrap`
  semantics); ED-complete clears all wraps; EL-complete/left and ECH preserve
  wrap. Previously stale `wrap` bits appeared on rows that Ghostty reports
  as unwrapped.
- **Emoji width** — `ghostcon_unicode_width` now uses the full Markus Kuhn
  wcwidth wide-range table (binary-searched, like the combining table), so
  U+1F680 🚀, U+2B1B ⬛, U+1F201 🈁, etc. are width 2.

Build/run the harness:

**Use a persistent location, not `/tmp`** — a prior session built this under
`/tmp/opencode`/`/tmp/ghostty-src` and lost it on reboot, silently disabling
the differential check until rediscovered and rebuilt. Use
`~/.cache/ghostcon/` instead:

```
# ~/.cache/ghostcon/ghostty-src is a persistent master clone of
# github.com/ghostty-org/ghostty (not checked into this repo):
cd ~/.cache/ghostcon/ghostty-src
zig build -Demit-lib-vt=true -Doptimize=ReleaseSafe

cc -O2 -o ~/.cache/ghostcon/bin/ghostty_dump tools/ghostty_dump.c \
    -I~/.cache/ghostcon/ghostty-src/zig-out/include \
    -L~/.cache/ghostcon/ghostty-src/zig-out/lib \
    -lghostty-vt -Wl,-rpath,~/.cache/ghostcon/ghostty-src/zig-out/lib

GHOSTTY_DUMP=~/.cache/ghostcon/bin/ghostty_dump sh tools/run_compare.sh
# 44/44 corpus files match (last verified 2026-08-14)
```

---

## 6. Color Handling

Ghostty's color model (ported from `terminal/color.zig`):

- 16 named colors (ANSI black/bright-black through white/bright-white)
- 256-color palette (xterm-compatible)
- Truecolor (24-bit RGB via `\e[38;2;R;G;Bm` and `\e[48;2;R;G;Bm`)
- Color index transparency: `GHOSTTY_COLOR_INDEX_TRANSPARENT = 256`
  used for "use default foreground/background"
- OSC 4/10/11/12/104 color query/change protocol

The renderer queries the resolved color (palette or truecolor) for each
cell and passes it to the fragment shader as a uniform.

---

## 7. Key Event Flow

```
libinput event
  → ghostcon-core/input.c: translate to ghostty key event
  → ghostty_key_event_set_key/action/mods/composing/utf8
  → ghostty_encoder_encode() → encoded byte sequence
  → transport_write(pty_socket, encoded_bytes)
  → pty child writes to PTY master → shell reads it
```

The Kitty keyboard protocol is handled entirely by libghostty-vt's
key encoder. If the shell/application queries for Kitty protocol
support (via `\e[?u` CSI sequence), the terminal responds with the
flags the encoder was configured with — this is a property of the
encoder, not the state machine.

---

## 8. Build System

Meson (single build system for all C code):

```
project/
├── meson.build              ← top-level
├── src/
│   ├── meson.build
│   ├── core/                ← KMS, EGL, VT, libinput, transport
│   ├── term/                ← ported Ghostty state machine
│   ├── render/              ← GLES renderer, font atlas, shaders
│   └── main.c               ← entry point (per-VT binary)
├── subprojects/
│   └── libtsm.wrap          ← NOT USED (archived only)
```

Dependencies:
- `libghostty-vt` (via pkg-config)
- `libdrm` + `libgbm` (KMS/GBM)
- `libEGL` + `libGLESv2` (EGL/GLES)
- `libinput` (input)
- `libudev` (input device discovery)
- `libfontconfig` + `libfreetype` (font rendering)
- `libsystemd` (journald logging, sd_notify)

No Zig in Phase 0/1 (can be added later for ghostcon-ipc).

---

## 9. Phases

> **Numbering authority: PLAN.md.** PLAN.md defines three phases (0, 1, 2).
> This file previously used its own five-stage scheme (0, 0.5, 1, 2, 3)
> whose numbers *collided* with PLAN.md's while meaning different things —
> most dangerously, "Phase 2" meant "Supervision + Multi-VT" here but
> "IPC and overlay" there. The stages below are therefore expressed as
> **sub-stages of PLAN.md's phases**, so the finer implementation
> granularity is preserved without any number meaning two things:
>
> | This file | PLAN.md phase | Content |
> |---|---|---|
> | Phase 0 | Phase 0 | Port core state machine |
> | Phase 1a | Phase 1 (step 2) | Minimal renderer (KMS/EGL/GLES) |
> | Phase 1b | Phase 1 (steps 1, 3-6) | Integration: vtctl, input, transport, ptyserv |
> | Phase 1c | Phase 1 (step 7) | Supervision + multi-VT |
> | Phase 2 | Phase 2 | IPC + overlay |
>
> PLAN.md's per-phase completion criteria remain authoritative and are not
> restated here.

### Phase 0 — Port Core State Machine (C files only)

Goal: a standalone `ghostcon_screen_t` that can be fed bytes and queried
for screen state. No rendering, no KMS, no input. Tested with a
regression corpus of escape sequences.

Files to write:
- `src/term/cell.h` + `cell.c`
- `src/term/style.h` + `style.c` (interned style table; see §3 departure note)
- `src/term/row.h` + `row.c`
- `src/term/screen.h` + `screen.c` (includes scrollback, alt screen, mode 2026)
- `src/term/stream.h` + `stream.c` (byte stream parser with full DEC state machine)
- `src/term/color.h` + `color.c` (palette, truecolor conversion)
- `src/term/kitty.h` + `kitty.c` (Kitty keyboard protocol state)
- `src/term/selection.h` + `selection.c`
- `src/term/term.h` + `term.c` (orchestrator: screen + stream + callbacks)

Test: feed PTY capture files through `ghostcon_term_feed()`, verify
screen state (cursor, cell contents, colors, scrollback) matches
Ghostty's output for the same input.

### Phase 1a — Minimal Renderer (software + GLES)

Goal: render the screen state to a visible KMS output.

Files to write:
- `src/core/kms.c` — DRM mode setting, GBM buffer alloc, page flip
- `src/core/egl.c` — EGL init on GBM surface
- `src/render/atlas.h` + `atlas.c` — font glyph atlas (freetype → GPU tex)
- `src/render/gles.h` + `gles.c` — GLES 2.0 renderer
- `src/render/shaders/gles2.vert` + `gles2.frag`
- `src/render/machine.h` + `machine.c` — damage → quad list generation

Test: render a known screen state (e.g., text loaded from a file) to a
KMS buffer and verify the output visually.

### Phase 1b — Integration

Goal: a single-VT ghostcon that can spawn a shell, display output, and
accept keyboard input.

Files to write:
- `src/core/vtctl.c` — VT_PROCESS handshake (SIGUSR1/SIGUSR2)
- `src/core/input.c` — libinput event loop
- `src/core/transport.c` — pty child socket connection
- `src/core/main.c` — main loop orchestration

Also: ghost-ptyserv + pty-ttyN (per existing plan).

### Phase 1c — Supervision + Multi-VT

Per existing plan: undead-head, supervisor[ttyN], canary, recovery,
three-tier fallback.

### Phase 2 — IPC + Overlay

Per existing plan: ghostcon-ipc, ghostcon-overlay, clipboard, notifications.

---

## 10. Testing Strategy

### Unit tests (Phase 0)
- Feed byte sequences through `stream_process()`, assert screen state
- Test every CSI command, OSC type, control character individually
- Edge cases: wide chars, combining chars, buffer overflow, malformed
  sequences, half-received sequences (streaming)

### Regression tests (Phase 0)
- Record PTY byte streams from real shell sessions
- Feed through both Ghostty (via libghostty-vt build) and our C port
- Assert identical screen state (cell grid dump, cursor, scrollback)

### Integration tests (Phase 1)
- Spawn ghost-ptyserv + pty child, connect a renderer
- Induce hang (SIGSTOP on renderer), confirm supervisor detects and
  replaces it
- Induce crash (SIGKILL on renderer), confirm pty child preserves
  shell session and replacement renderer reconnects
- Rapid VT switching stress test

### Protocol conformance (Phase 1)
- OSC matrix from PLAN.md: every entry tested for correct parsing and
  correct stub/direct behavior
- Kitty keyboard protocol: confirm encoder produces correct sequences
  for all modifier + key combinations
- Synchronized output (mode 2026): confirm defer/commit produces
  atomic screen updates

---

## 11. Dependencies Not Yet Available

None that aren't already on the system or readily available via the
package manager. Key dependencies confirmed present:

- `libghostty-vt` (v0.1.0) — installed at `/usr/lib/libghostty-vt.so`
- `libdrm` — DRM/KMS (kernel graphics)
- `libgbm` — GBM buffer management
- `libEGL` + `libGLESv2` — EGL/GLES (via Mesa)
- `libinput` — input device handling
- `libudev` — udev device discovery
- `libfontconfig` — font selection
- `libfreetype` — glyph rasterization
- `libsystemd` — journald logging, sd_notify

Missing (will be added by package manager when building):
- None of note. These are all standard distribution packages.
