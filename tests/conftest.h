#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

/* Maximum number of test cases */
#define GC_TEST_MAX 512

/* Test function signature */
typedef void (*gc_test_fn)(void);

/* Internal test registry */
typedef struct {
    const char *name;
    gc_test_fn  fn;
} gc_test_entry_t;

static gc_test_entry_t gc_test_registry[GC_TEST_MAX];
static int             gc_test_count = 0;
static int             gc_test_passed = 0;
static int             gc_test_failed = 0;
static jmp_buf         gc_test_jmp;
static char            gc_test_fail_buf[1024];
static const char     *gc_test_fail_msg = NULL;
static int             gc_test_fail_line = 0;

/* Register a test (called via TEST macro) */
static inline void
gc_test_register(const char *name, gc_test_fn fn) {
    if (gc_test_count < GC_TEST_MAX)
        gc_test_registry[gc_test_count++] = (gc_test_entry_t){name, fn};
}

/* Test runner: run all registered tests */
static inline int
gc_test_run_all(void) {
    printf("--- conformance test suite ---\n\n");
    for (int i = 0; i < gc_test_count; i++) {
        gc_test_fail_msg = NULL;
        printf("  %s ... ", gc_test_registry[i].name);
        fflush(stdout);

        if (setjmp(gc_test_jmp) == 0)
            gc_test_registry[i].fn();

        if (gc_test_fail_msg) {
            printf("FAIL\n");
            if (gc_test_fail_line)
                printf("    at %s:%d: %s\n", __FILE__,
                       gc_test_fail_line, gc_test_fail_msg);
            else
                printf("    %s\n", gc_test_fail_msg);
            gc_test_failed++;
        } else {
            printf("ok\n");
            gc_test_passed++;
        }
    }

    printf("\n--- results: %d passed, %d failed, %d total ---\n",
           gc_test_passed, gc_test_failed, gc_test_count);
    return gc_test_failed == 0 ? 0 : 1;
}

/* Register test case */
#define TEST(name) \
    static void gc_test_impl_##name(void); \
    __attribute__((constructor)) static void gc_test_reg_##name(void) { \
        gc_test_register(#name, gc_test_impl_##name); \
    } \
    static void gc_test_impl_##name(void)

/* Fail the current test with a message */
#define FAIL(msg) do { \
    snprintf(gc_test_fail_buf, sizeof(gc_test_fail_buf), "%s", msg); \
    gc_test_fail_msg = gc_test_fail_buf; \
    gc_test_fail_line = __LINE__; \
    longjmp(gc_test_jmp, 1); \
} while(0)

/* Assert a boolean condition */
#define ASSERT(cond, msg) do { \
    if (!(cond)) FAIL(msg); \
} while(0)

/* Assert two integers are equal */
#define ASSERT_EQ(a, b, msg) do { \
    long long _a = (long long)(a); \
    long long _b = (long long)(b); \
    if (_a != _b) { \
        char _buf[256]; \
        snprintf(_buf, sizeof(_buf), "%s: got %lld, expected %lld", msg, _a, _b); \
        FAIL(_buf); \
    } \
} while(0)

/* Assert two unsigned integers are equal */
#define ASSERT_EQ_U(a, b, msg) do { \
    unsigned long long _a = (unsigned long long)(a); \
    unsigned long long _b = (unsigned long long)(b); \
    if (_a != _b) { \
        char _buf[256]; \
        snprintf(_buf, sizeof(_buf), "%s: got %llu, expected %llu", msg, _a, _b); \
        FAIL(_buf); \
    } \
} while(0)

/* Assert string equality */
#define ASSERT_STR(got, expected, msg) do { \
    const char *_g = (got); \
    const char *_e = (expected); \
    if (strcmp(_g, _e) != 0) { \
        char _buf[512]; \
        snprintf(_buf, sizeof(_buf), "%s: got \"%s\", expected \"%s\"", msg, _g, _e); \
        FAIL(_buf); \
    } \
} while(0)

/* Assert cell codepoint at (x,y) */
#define ASSERT_CELL(term, x, y, expected_cp, msg) do { \
    ghostcon_cell_t *_c = ghostcon_screen_cell(&(term).screen, x, y); \
    ASSERT(_c != NULL, msg); \
    uint32_t _cp = ghostcon_cell_get_codepoint(*_c); \
    if (_cp != (uint32_t)(expected_cp)) { \
        char _buf[256]; \
        snprintf(_buf, sizeof(_buf), "%s at (%d,%d): got U+%04X, expected U+%04X", \
                 msg, x, y, _cp, (uint32_t)(expected_cp)); \
        FAIL(_buf); \
    } \
} while(0)

/* Assert cursor position */
#define ASSERT_CURSOR(term, ex, ey, msg) do { \
    if ((term).screen.cursor.x != (ex) || (term).screen.cursor.y != (ey)) { \
        char _buf[256]; \
        snprintf(_buf, sizeof(_buf), \
                 "%s: cursor (%d,%d), expected (%d,%d)", \
                 msg, (term).screen.cursor.x, (term).screen.cursor.y, \
                 (int)(ex), (int)(ey)); \
        FAIL(_buf); \
    } \
} while(0)

/* Assert cell style flag set */
#define ASSERT_STYLE_FLAG(term, x, y, flag, msg) do { \
    ghostcon_cell_t *_c = ghostcon_screen_cell(&(term).screen, x, y); \
    ASSERT(_c != NULL, msg); \
    ghostcon_style_id_t _sid = ghostcon_cell_get_style(*_c); \
    const ghostcon_style_t *_s = ghostcon_style_set_get((term).screen.styles, _sid); \
    if (!(_s->flags & (flag))) { \
        char _buf[256]; \
        snprintf(_buf, sizeof(_buf), "%s at (%d,%d): flag 0x%04X not set", \
                 msg, x, y, (unsigned)(flag)); \
        FAIL(_buf); \
    } \
} while(0)

/* Assert cell wide state */
#define ASSERT_WIDE(term, x, y, expected, msg) do { \
    ghostcon_cell_t *_c = ghostcon_screen_cell(&(term).screen, x, y); \
    ASSERT(_c != NULL, msg); \
    ghostcon_cell_wide_t _w = ghostcon_cell_get_wide(*_c); \
    if (_w != (expected)) { \
        char _buf[256]; \
        snprintf(_buf, sizeof(_buf), "%s at (%d,%d): wide=%d, expected %d", \
                 msg, x, y, (int)_w, (int)(expected)); \
        FAIL(_buf); \
    } \
} while(0)

/* Assert cell content tag */
#define ASSERT_TAG(term, x, y, expected, msg) do { \
    ghostcon_cell_t *_c = ghostcon_screen_cell(&(term).screen, x, y); \
    ASSERT(_c != NULL, msg); \
    ghostcon_cell_content_tag_t _tg = ghostcon_cell_get_tag(*_c); \
    if (_tg != (expected)) { \
        char _buf[256]; \
        snprintf(_buf, sizeof(_buf), "%s at (%d,%d): tag=%d, expected %d", \
                 msg, x, y, (int)_tg, (int)(expected)); \
        FAIL(_buf); \
    } \
} while(0)

/* Assert cell protected bit */
#define ASSERT_PROTECTED(term, x, y, expected, msg) do { \
    ghostcon_cell_t *_c = ghostcon_screen_cell(&(term).screen, x, y); \
    ASSERT(_c != NULL, msg); \
    if (ghostcon_cell_get_protected(*_c) != (bool)(expected)) { \
        char _buf[256]; \
        snprintf(_buf, sizeof(_buf), "%s at (%d,%d): protected=%d, expected %d", \
                 msg, x, y, (int)ghostcon_cell_get_protected(*_c), (int)(expected)); \
        FAIL(_buf); \
    } \
} while(0)

/* ------------------------------------------------------------------ */
/* Output capture helpers (for testing DSR/DA/DECRPM responses)        */
/* ------------------------------------------------------------------ */

#define GC_TEST_OUTPUT_BUF_SIZE 256

typedef struct {
    char buf[GC_TEST_OUTPUT_BUF_SIZE];
    size_t len;
} gc_test_output_t;

static void
gc_test_output_fn(void *userdata, const uint8_t *data, size_t len) {
    gc_test_output_t *out = (gc_test_output_t *)userdata;
    size_t remaining = GC_TEST_OUTPUT_BUF_SIZE - out->len;
    size_t to_copy = (len < remaining) ? len : remaining;
    memcpy(out->buf + out->len, data, to_copy);
    out->len += to_copy;
    if (out->len < GC_TEST_OUTPUT_BUF_SIZE)
        out->buf[out->len] = '\0';
}

/* Reset output buffer to empty */
static inline void gc_test_output_reset(gc_test_output_t *out) {
    out->buf[0] = '\0';
    out->len = 0;
}

/* Assert output buffer equals expected string */
#define ASSERT_OUTPUT(out, expected_str, msg) do { \
    if ((out).len != strlen(expected_str) || \
        memcmp((out).buf, expected_str, (out).len) != 0) { \
        char _buf[512]; \
        snprintf(_buf, sizeof(_buf), "%s: got \"%.*s\" (len %zu), expected \"%s\"", \
                 msg, (int)(out).len, (out).buf, (out).len, expected_str); \
        FAIL(_buf); \
    } \
} while(0)
