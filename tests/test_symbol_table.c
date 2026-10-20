#define _DEFAULT_SOURCE /* for strsep() under c_std=c11 */

#include "ghostcon/term/cell.h"
#include "conftest.h"

/* Full-range sweep of ghostcon_cell_is_symbol() against the vendored UCD
 * (third_party/UnicodeData.txt, the same input tools/gen_symbol_table.py reads)
 *
 * not a spot-check of known-problem codepoints (that's
 * cell_is_symbol_classification in test_conformance.c), but a cross-check that
 * the generated table plus its two carve-outs (Box Drawing/Block Elements
 * excluded, PUA included) agrees with real UCD General Category data everywhere,
 * catching codegen bugs and carve-out boundary mistakes that a handful of spot
 * checks would miss.
 */

#ifndef GHOSTCON_UCD_PATH
#error "GHOSTCON_UCD_PATH must be defined (path to vendored UnicodeData.txt)"
#endif

#ifndef GHOSTCON_SYMBOL_TABLE_PATH
#error "GHOSTCON_SYMBOL_TABLE_PATH must be defined (path to the generated symbol table)"
#endif

/* The Unicode version the fetched input is, as recorded by
 * tools/fetch_ucd.sh next to it.
 */
static const char *ucd_input_version(void)
{
    static char version[32];
    char path[512];
    snprintf(path, sizeof path, "%s", GHOSTCON_UCD_PATH);
    char *slash = strrchr(path, '/');
    if (!slash)
        return NULL;
    snprintf(slash + 1, sizeof path - (size_t)(slash + 1 - path), "UCD_VERSION");

    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;
    if (!fgets(version, sizeof version, f))
    {
        fclose(f);
        return NULL;
    }
    fclose(f);
    version[strcspn(version, "\r\n")] = '\0';
    return version;
}

/* The Unicode version the generated table was built from, as recorded in the
 * table's own header.
 */
static const char *table_ucd_version(void)
{
    static char version[32];
    FILE *f = fopen(GHOSTCON_SYMBOL_TABLE_PATH, "r");
    if (!f)
        return NULL;

    const char *found = NULL;
    char line[256];
    while (fgets(line, sizeof line, f))
    {
        const char *p = strstr(line, "Unicode version: ");
        if (!p)
            continue;
        p += strlen("Unicode version: ");
        size_t i = 0;
        while (p[i] && p[i] != '\n' && p[i] != '\r' && i + 1 < sizeof version)
        {
            version[i] = p[i];
            i++;
        }
        version[i] = '\0';
        /* The line ends with the sentence's period. */
        while (i > 0 && (version[i - 1] == '.' || version[i - 1] == ' '))
            version[--i] = '\0';
        found = version;
        break;
    }
    fclose(f);
    return found;
}

static bool is_symbol_category(const char *cat)
{
    return strcmp(cat, "Sm") == 0 || strcmp(cat, "Sc") == 0 ||
           strcmp(cat, "Sk") == 0 || strcmp(cat, "So") == 0;
}

static bool expected_is_symbol(uint32_t cp, bool ucd_symbol)
{
    if (cp >= 0x2500 && cp <= 0x259F) return false; /* box draw/block elements */
    if (cp >= 0xE000 && cp <= 0xF8FF) return true;  /* PUA (Nerd Font icons) */
    return ucd_symbol;
}

TEST(symbol_table_matches_ucd)
{
    FILE *f = fopen(GHOSTCON_UCD_PATH, "r");
    ASSERT(f != NULL, "failed to open vendored UnicodeData.txt");

    char line[512];
    long pending_first_cp = -1;
    char pending_first_cat[8] = {0};

    long checked = 0;
    long mismatches = 0;
    uint32_t first_bad_cp = 0;
    bool first_bad_got = false, first_bad_expected = false;

    while (fgets(line, sizeof(line), f))
    {
        char *fields[16];
        int nf = 0;
        char *p = line;
        char *tok;
        while (nf < 16 && (tok = strsep(&p, ";")) != NULL)
        {
            fields[nf++] = tok;
        }
        if (nf < 3) continue;

        uint32_t cp = (uint32_t)strtoul(fields[0], NULL, 16);
        const char *name = fields[1];
        const char *cat = fields[2];
        size_t name_len = strlen(name);

        bool is_first = name_len >= 8 && strcmp(name + name_len - 8, ", First>") == 0;
        bool is_last = name_len >= 7 && strcmp(name + name_len - 7, ", Last>") == 0;

        if (is_first)
        {
            pending_first_cp = (long)cp;
            snprintf(pending_first_cat, sizeof(pending_first_cat), "%s", cat);
            continue;
        }

        uint32_t range_lo = cp, range_hi = cp;
        const char *use_cat = cat;
        if (is_last)
        {
            ASSERT(pending_first_cp >= 0, "Last> entry with no preceding First>");
            range_lo = (uint32_t)pending_first_cp;
            range_hi = cp;
            use_cat = pending_first_cat;
            pending_first_cp = -1;
        }

        bool ucd_symbol = is_symbol_category(use_cat);
        for (uint32_t c = range_lo; c <= range_hi; c++)
        {
            bool expected = expected_is_symbol(c, ucd_symbol);
            bool got = ghostcon_cell_is_symbol(c);
            checked++;
            if (got != expected)
            {
                if (mismatches == 0)
                {
                    first_bad_cp = c;
                    first_bad_got = got;
                    first_bad_expected = expected;
                }
                mismatches++;
            }
            if (c == 0xFFFFFFFFu) break; /* guard against wraparound */
        }
    }
    fclose(f);

    ASSERT(checked > 250000, "sanity: expected to sweep a substantial fraction of the UCD");

    if (mismatches > 0)
    {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "%ld/%ld codepoints mismatched; first at U+%04X: got %s, expected %s",
                 mismatches, checked, first_bad_cp,
                 first_bad_got ? "symbol" : "not-symbol",
                 first_bad_expected ? "symbol" : "not-symbol");
        FAIL(buf);
    }
}

int main(void)
{
    /* The UCD input is fetched on demand (tools/fetch_ucd.sh) and is not
     * committed -- see .gitignore.
     *
     * Without it there is nothing to compare the generated table against, so the
     * test has nothing to check rather than something to fail about.
     */
    FILE *probe = fopen(GHOSTCON_UCD_PATH, "r");
    if (!probe)
    {
        printf("SKIP: %s is not present (fetch it with tools/fetch_ucd.sh)\n",
               GHOSTCON_UCD_PATH);
        return 77; /* meson's skip code */
    }
    fclose(probe);

    /* The table is a snapshot of one UCD version, and this test compares it
     * against the UCD.
     *
     * If the input is a different version, every codepoint Unicode has changed
     * since would be reported as a mismatch -- which says nothing about the
     * table.
     *
     * (Unicode 16.0 gave U+20C2..U+20CF to new currency signs, so a 15.x table
     * and a 16.0 UCD disagree about 143 codepoints.)
     *
     * So the versions are compared first, and a mismatch is reported as what it
     * is: the wrong input, not a broken table.
     */
    const char *input_version = ucd_input_version();
    const char *table_version = table_ucd_version();
    if (input_version && table_version && strcmp(input_version, table_version) != 0)
    {
        printf("SKIP: the fetched UCD is Unicode %s, but the generated table was "
               "built from Unicode %s -- regenerate the table (see "
               "tools/fetch_ucd.sh) or fetch the matching version\n",
               input_version, table_version);
        return 77; /* meson's skip code */
    }

    return gc_test_run_all();
}
