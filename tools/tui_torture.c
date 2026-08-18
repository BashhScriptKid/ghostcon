/* tui_torture -- a plain terminal client (no libghostcon dependency at
 * all, unlike tools/bench_*.c) that emits real VT escape sequences to
 * stdout, choreographed to reproduce the specific redraw idioms real
 * full-screen TUIs use (fixed status/input areas, synchronized-output
 * batches, alt-screen dips, scrollback churn, ...). Meant to be run
 * from an interactive shell INSIDE ghostcon itself (tty4), so each
 * page exercises the real parser -> screen -> renderer pipeline
 * end-to-end -- turning "does this real complex TUI glitch" into
 * "does this one specific, isolated, repeatable pattern glitch."
 *
 * Usage:
 *   tui_torture --list            list all pages
 *   tui_torture <page>            run just that one page
 *   tui_torture [--all]           run every page in sequence
 *
 * In --all / no-args mode, each page waits for Enter before advancing
 * so you have time to look and call out which one (if any) glitches.
 */

#define _DEFAULT_SOURCE /* usleep() under -std=c11 without this */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define ESC "\x1b"

/* Several pages (mouse reporting, Kitty keyboard query, OSC 10/11
   color query) make the terminal write a reply back into this
   program's stdin, exactly like a real keystroke -- the kernel tty
   line discipline's ECHO flag then reflects those bytes straight back
   out to the display as literal text before this program ever reads
   them, regardless of whether a human typed them or ghostcon injected
   them. Found live: exactly this made those pages' output unreadable.
   Disabling ECHO for the whole run (ICANON stays on, so wait_enter()'s
   line-buffered getchar() loop is unaffected) suppresses it. */
static struct termios g_orig_termios;
static bool g_termios_saved = false;

static void
restore_termios(void)
{
    if (g_termios_saved)
        tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
}

static void
disable_echo(void)
{
    if (!isatty(STDIN_FILENO))
        return;
    if (tcgetattr(STDIN_FILENO, &g_orig_termios) != 0)
        return;
    g_termios_saved = true;
    atexit(restore_termios);
    struct termios raw = g_orig_termios;
    raw.c_lflag &= (tcflag_t)~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

static void
banner(const char *title, const char *desc)
{
    printf(ESC "[2J" ESC "[H");
    printf(ESC "[1;97;44m %s " ESC "[0m\r\n", title);
    printf(ESC "[36m%s" ESC "[0m\r\n\r\n", desc);
    fflush(stdout);
    usleep(400000);
    /* Clear again before returning -- pages assume a guaranteed-blank
       screen to draw on top of, but they don't all fully overwrite
       every cell they touch (e.g. a shorter line printed where the
       inverse-video title bar above was longer), which would
       otherwise leave leftover styled characters from this banner
       behind. Found live: reproduced identically on real Ghostty used
       as a control, confirming it's a bug in this tool's own output
       sequencing, not a terminal rendering bug. */
    printf(ESC "[2J" ESC "[H");
    fflush(stdout);
}

static void
wait_enter(void)
{
    printf("\r\n" ESC "[90m-- done, press Enter to continue --" ESC "[0m");
    fflush(stdout);
    int c;
    while ((c = getchar()) != '\n' && c != EOF) { }
}

/* ------------------------------------------------------------------ */
/* Pages                                                               */
/* ------------------------------------------------------------------ */

/* Bare \r progress-line redraw, no cursor positioning or scroll
   region at all -- the simplest and most common "spinner"/progress
   idiom. Interleaved with plain log lines that scroll normally, so
   the spinner line keeps moving down the screen as it's rewritten. */
static void
page_cr_redraw(void)
{
    banner("cr_redraw", "Bare \\r progress-line redraw interleaved with scrolling log lines.");
    static const char spin[] = "|/-\\";
    for (int i = 0; i < 40; i++) {
        if (i % 5 == 0) {
            /* End the previous spinner line (which deliberately never
               emitted its own \n, so it could be overwritten in
               place) before starting this new log line -- otherwise
               it gets appended mid-row onto whatever the spinner left
               behind instead of starting fresh. */
            if (i != 0)
                printf("\r\n");
            printf("log line %d: doing some work...\r\n", i / 5);
        }
        printf("\r" ESC "[K" "  %c working... %d%%", spin[i % 4], (i * 100) / 40);
        fflush(stdout);
        usleep(60000);
    }
    printf("\r\n\r" ESC "[K" "  done.\r\n");
}

/* The bug found live this session: a scroll region excluding the
   bottom 2 rows (a fixed status/input area), redrawn via CNL (CSI E)
   on every tick while unrelated content scrolls in the region above. */
static void
page_scroll_status_bar(void)
{
    banner("scroll_status_bar", "Scroll region excludes bottom 2 rows; status area redrawn via CNL each tick.");
    printf(ESC "[1;22r");   /* reserve the last 2 of 24 rows */
    printf(ESC "[H");
    for (int i = 0; i < 30; i++) {
        printf("scrolling content line %d\r\n", i);
        /* Save the in-region cursor position before jumping down to
           redraw the reserved status/input area, and restore it
           right after -- otherwise the NEXT iteration's content line
           gets printed wherever the status/input redraw left the
           cursor (the reserved area itself), never actually reaching
           the visible scrollable region. Found live: without this,
           only the very first content line ever showed. */
        printf(ESC "7");     /* save cursor */
        printf(ESC "[23;1H" ESC "[K" "status: tick %d", i);
        printf(ESC "[E");   /* CNL -- the exact sequence that triggered the bug */
        printf(ESC "[K" "input: |");
        printf(ESC "8");     /* restore cursor -- back inside the region */
        fflush(stdout);
        usleep(50000);
    }
    printf(ESC "[r");       /* reset scroll region */
    printf(ESC "[24;1H");
}

/* A large (>4KB, so it spans multiple pty reads) diff wrapped in mode
   2026 -- should present atomically, no visible tearing. */
static void
page_sync_output(void)
{
    banner("sync_output", "Large (>4KB) screen diff wrapped in CSI ?2026h ... ?2026l.");
    for (int rep = 0; rep < 3; rep++) {
        printf(ESC "[?2026h");
        printf(ESC "[2J" ESC "[H");
        for (int y = 0; y < 20; y++) {
            printf(ESC "[%d;1H", y + 1);
            for (int x = 0; x < 70; x++)
                putchar('A' + ((x + y + rep) % 26));
        }
        printf(ESC "[?2026l");
        fflush(stdout);
        usleep(300000);
    }
}

/* Repeated ?1049h/l (vim/htop/less convention) -- cursor position on
   the main screen must survive the round-trip. */
static void
page_alt_screen(void)
{
    banner("alt_screen", "Repeated ?1049h/l alt-screen enter/exit; cursor position must be preserved.");
    printf("main screen text before\r\n");
    printf("cursor should return to right after this line\r\n> ");
    fflush(stdout);
    usleep(300000);
    for (int i = 0; i < 4; i++) {
        printf(ESC "[?1049h");
        printf(ESC "[2J" ESC "[H");
        printf("-- inside alt screen, iteration %d --\r\n", i);
        fflush(stdout);
        usleep(200000);
        printf(ESC "[?1049l");
        fflush(stdout);
        usleep(200000);
    }
    printf(" <- cursor should be right here on the main screen\r\n");
}

static void
page_insert_delete_lines(void)
{
    banner("insert_delete_lines", "IL (CSI L) / DL (CSI M) exercised directly.");
    printf(ESC "[H");
    for (int i = 0; i < 10; i++)
        printf("line %d\r\n", i);
    printf(ESC "[3;1H" ESC "[2L");   /* insert 2 blank lines at row 3 */
    printf("INSERTED-A\r\nINSERTED-B");
    fflush(stdout);
    usleep(500000);
    printf(ESC "[3;1H" ESC "[2M");   /* delete them again */
    /* Move well clear of the tested rows before returning -- cursor
       is left at row 3 (col 1) by the DL above, and wait_enter()'s
       own leading "\r\n" would otherwise print the "done" prompt
       exactly on row 4, overwriting the very content (the restored
       "line 3") this page exists to let you verify. */
    printf(ESC "[12;1H");
}

static void
page_insert_delete_chars(void)
{
    banner("insert_delete_chars", "ICH (CSI @) / DCH (CSI P) exercised directly.");
    printf(ESC "[H" "0123456789ABCDEFGHIJ\r\n");
    printf(ESC "[1;5H" ESC "[5@" "XXXXX");   /* insert 5 chars at col 5 */
    fflush(stdout);
    usleep(500000);
    printf(ESC "[2;1H" "0123456789ABCDEFGHIJ\r\n");
    printf(ESC "[2;5H" ESC "[5P");           /* delete 5 chars at col 5 */
}

static void
page_erase_modes(void)
{
    banner("erase_modes", "EL (CSI K) and ED (CSI J) -- all param modes, including scrollback erase.");
    printf(ESC "[H");
    for (int i = 0; i < 5; i++)
        printf("XXXXXXXXXXXXXXXXXXXX line %d\r\n", i);
    fflush(stdout);
    usleep(300000);
    printf(ESC "[3;10H" ESC "[K");   /* EL 0: erase to end of line */
    printf(ESC "[4;10H" ESC "[1K");  /* EL 1: erase to start of line */
    printf(ESC "[5;1H" ESC "[2K");   /* EL 2: erase whole line */
    fflush(stdout);
    usleep(500000);
    printf(ESC "[2J");               /* ED 2: erase whole screen */
    printf(ESC "[H" "screen erased (ED 2)\r\n");
    fflush(stdout);
    usleep(300000);
    for (int i = 0; i < 5; i++)
        printf("scrollback line %d\r\n", i);
    printf(ESC "[3J");               /* ED 3: erase scrollback */
    printf(ESC "[H" ESC "[2J" "scrollback erased (ED 3) -- scroll up should show nothing above\r\n");
}

static void
page_cursor_save_restore(void)
{
    banner("cursor_save_restore", "Bare ESC 7 / ESC 8 (DECSC/DECRC), distinct from the ?1048/?1049 variants.");
    printf(ESC "[H" "line one\r\nline two: ");
    printf(ESC "7");  /* save */
    printf(ESC "[10;40H" "(elsewhere)");
    fflush(stdout);
    usleep(400000);
    printf(ESC "8");  /* restore */
    printf("<- cursor restored here");
}

static void
page_dectcem_toggle(void)
{
    banner("dectcem_toggle", "Rapid cursor show/hide (?25h/l), common while a spinner animates.");
    printf(ESC "[H" "watch the cursor: ");
    fflush(stdout);
    for (int i = 0; i < 20; i++) {
        printf(ESC "[?25l");
        fflush(stdout);
        usleep(80000);
        printf(ESC "[?25h");
        fflush(stdout);
        usleep(80000);
    }
    printf("\r\ndone\r\n");
}

static void
page_wide_chars(void)
{
    banner("wide_chars", "CJK/emoji double-width cells mixed with normal text.");
    printf(ESC "[H");
    printf("normal text \xe4\xbd\xa0\xe5\xa5\xbd wide chars \xf0\x9f\x9a\x80 emoji then more text\r\n");
    printf("second row: \xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e mixed with ASCII abc123\r\n");
}

static void
page_scrollback(void)
{
    banner("scrollback", "Push enough lines to exercise history push/scroll-up repeatedly.");
    for (int i = 0; i < 200; i++)
        printf("scrollback stress line %d\r\n", i);
    fflush(stdout);
}

static void
page_origin_mode(void)
{
    banner("origin_mode", "DECOM (?6h/l) on/off cursor-positioning boundary checks.");
    printf(ESC "[5;20r");   /* scroll region rows 5-20 */
    printf(ESC "[?6h");     /* origin mode on */
    printf(ESC "[H" "origin-mode home (should be row 5 absolute)\r\n");
    fflush(stdout);
    usleep(400000);
    printf(ESC "[?6l");     /* origin mode off */
    printf(ESC "[r");       /* reset region */
    printf(ESC "[H" "origin-mode off, home is real row 1 now\r\n");
}

static void
page_hyperlinks(void)
{
    banner("hyperlinks", "OSC 8 hyperlinks.");
    printf(ESC "[H");
    printf(ESC "]8;;https://example.com" ESC "\\" "click me (example.com)" ESC "]8;;" ESC "\\" "\r\n");
    printf("plain text after the link\r\n");
}

static void
page_mouse_reporting(void)
{
    banner("mouse_reporting", "Enable/disable each mouse mode (no synthetic events -- move the mouse if you want to check reports).");
    const int modes[] = { 1000, 1002, 1003 };
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        printf(ESC "[?%dh" ESC "[?1006h", modes[i]);
        printf("mode %d + SGR (1006) enabled -- move the mouse now\r\n", modes[i]);
        fflush(stdout);
        usleep(1500000);
        printf(ESC "[?%dl" ESC "[?1006l", modes[i]);
    }
}

static void
page_osc_title_color(void)
{
    banner("osc_title_color", "OSC 0/2 window title, OSC 4/10/11 palette set/query.");
    printf(ESC "]0;tui_torture demo title" ESC "\\");
    printf("window title set (OSC 0)\r\n");
    printf(ESC "]4;1;#ff8800" ESC "\\");
    printf("palette color 1 set to #ff8800 (OSC 4)\r\n");
    printf(ESC "]10;?" ESC "\\" ESC "]11;?" ESC "\\");
    printf("queried fg/bg color (OSC 10/11 -- reply goes back to this pty, not visible here)\r\n");
}

static void
page_kitty_keyboard(void)
{
    banner("kitty_keyboard", "CSI > u push / = u set / ? u query / < u pop.");
    printf(ESC "[>1u");
    printf("pushed kitty flags (disambiguate)\r\n");
    printf(ESC "[?u");
    printf("queried current flags (reply not visible here)\r\n");
    printf(ESC "[=5;1u");
    printf("set flags to 5\r\n");
    printf(ESC "[<1u");
    printf("popped kitty flags\r\n");
}

static void
page_sgr_styles(void)
{
    banner("sgr_styles", "Bold/italic/underline/strikethrough/dim, 256-color, truecolor.");
    printf(ESC "[1mbold" ESC "[0m ");
    printf(ESC "[3mitalic" ESC "[0m ");
    printf(ESC "[4munderline" ESC "[0m ");
    printf(ESC "[9mstrikethrough" ESC "[0m ");
    printf(ESC "[2mdim" ESC "[0m\r\n");
    for (int i = 0; i < 16; i++)
        printf(ESC "[48;5;%dm  " ESC "[0m", i);
    printf("\r\n");
    printf(ESC "[38;2;255;100;0mtruecolor fg" ESC "[0m ");
    printf(ESC "[48;2;0;100;255mtruecolor bg" ESC "[0m\r\n");
}

static void
page_tabs(void)
{
    banner("tabs", "HT (\\t), HTS (ESC H), TBC (CSI g).");
    printf(ESC "[H" "a\tb\tc\td\r\n");
    /* Second demo goes on row 2, not back to row 1 -- homing there
       would silently overwrite the first demo before it's ever seen. */
    printf(ESC "[2;1H" ESC "[3G" ESC "H");  /* row 2, set a custom tab stop at column 3 */
    printf(ESC "[2;1H" "X\tY (custom stop at col 3 should apply after clearing defaults)\r\n");
    printf(ESC "[3g");          /* TBC 3: clear all tab stops */
}

static void
page_margins_declrm(void)
{
    banner("margins_declrm", "DECSLRM left/right margins (mode 69).");
    printf(ESC "[?69h");        /* enable left/right margin mode */
    printf(ESC "[10;30s");      /* margins at columns 10-30 */
    printf(ESC "[H");
    for (int i = 0; i < 5; i++)
        printf("row %d: text that should wrap/clip within cols 10-30 only if margins apply xxxxxxxxxxxxxxxxxxxxxxxxxxxx\r\n", i);
    fflush(stdout);
    usleep(400000);
    printf(ESC "[?69l");        /* disable */
}

/* Fires several mechanisms nested/back-to-back -- real bugs often only
   surface from combinations, not any single feature in isolation. */
static void
page_combined_stress(void)
{
    banner("combined_stress", "Sync-output wrapping a scroll-region status-bar redraw wrapping alt-screen dips.");
    printf(ESC "[1;20r");
    printf(ESC "[H");
    for (int i = 0; i < 15; i++) {
        printf(ESC "[?2026h");
        printf("content line %d\r\n", i);
        printf(ESC "7");     /* save cursor -- see page_scroll_status_bar's own comment */
        printf(ESC "[21;1H" ESC "[K" "status tick %d", i);
        printf(ESC "[E");
        printf(ESC "[K" "input: |");
        printf(ESC "8");     /* restore cursor -- back inside the region */
        printf(ESC "[?2026l");
        fflush(stdout);
        usleep(70000);

        if (i == 7) {
            printf(ESC "[?1049h");
            printf(ESC "[2J" ESC "[H" "-- brief alt-screen dip mid-stress --\r\n");
            fflush(stdout);
            usleep(200000);
            printf(ESC "[?1049l");
        }
    }
    printf(ESC "[r");
    printf(ESC "[21;1H");
}

/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    const char *desc;
    void (*fn)(void);
} page_t;

static const page_t pages[] = {
    { "cr_redraw",           "bare \\r progress-line redraw",                    page_cr_redraw },
    { "combined_stress",     "several mechanisms nested/back-to-back",           page_combined_stress },
    { "scroll_status_bar",   "scroll region + CNL-redrawn fixed status area",    page_scroll_status_bar },
    { "sync_output",         "large diff wrapped in mode 2026",                  page_sync_output },
    { "alt_screen",          "repeated ?1049h/l enter/exit",                     page_alt_screen },
    { "insert_delete_lines", "IL/DL",                                            page_insert_delete_lines },
    { "insert_delete_chars", "ICH/DCH",                                          page_insert_delete_chars },
    { "erase_modes",         "EL/ED all param modes",                            page_erase_modes },
    { "cursor_save_restore", "bare ESC 7 / ESC 8",                               page_cursor_save_restore },
    { "dectcem_toggle",      "rapid cursor show/hide",                           page_dectcem_toggle },
    { "wide_chars",          "CJK/emoji double-width cells",                     page_wide_chars },
    { "scrollback",          "history push/scroll-up churn",                     page_scrollback },
    { "origin_mode",         "DECOM on/off boundary checks",                     page_origin_mode },
    { "hyperlinks",          "OSC 8",                                            page_hyperlinks },
    { "mouse_reporting",     "enable/disable each mouse mode",                   page_mouse_reporting },
    { "osc_title_color",     "OSC 0/2 title, OSC 4/10/11 palette",               page_osc_title_color },
    { "kitty_keyboard",      "CSI u push/set/query/pop",                         page_kitty_keyboard },
    { "sgr_styles",          "bold/italic/underline/dim/256/truecolor",          page_sgr_styles },
    { "tabs",                "HT/HTS/TBC",                                       page_tabs },
    { "margins_declrm",      "DECSLRM left/right margins",                       page_margins_declrm },
};
#define NUM_PAGES (sizeof(pages) / sizeof(pages[0]))

static void
list_pages(void)
{
    for (size_t i = 0; i < NUM_PAGES; i++)
        printf("%-20s %s\n", pages[i].name, pages[i].desc);
}

static void
run_page(const page_t *p, bool interactive)
{
    p->fn();
    if (interactive)
        wait_enter();
    else
        fflush(stdout);
}

int
main(int argc, char **argv)
{
    if (argc > 1 && (strcmp(argv[1], "--list") == 0 || strcmp(argv[1], "-l") == 0)) {
        list_pages();
        return 0;
    }

    disable_echo();

    if (argc > 1 && strcmp(argv[1], "--all") != 0) {
        for (size_t i = 0; i < NUM_PAGES; i++) {
            if (strcmp(argv[1], pages[i].name) == 0) {
                run_page(&pages[i], false);
                printf("\r\n");
                return 0;
            }
        }
        fprintf(stderr, "tui_torture: unknown page '%s' (try --list)\n", argv[1]);
        return 1;
    }

    /* --all, or no args at all: run every page in sequence, pausing
       for Enter between each so the pages can be watched one at a
       time. */
    for (size_t i = 0; i < NUM_PAGES; i++)
        run_page(&pages[i], true);

    printf(ESC "[2J" ESC "[H" "all pages done.\r\n");
    return 0;
}
