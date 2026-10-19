/* test_log
 *
 * pure-function checks for the GHOSTCON_LOG domain matcher used by GC_LOG()
 * (include/ghostcon/core/log.h). No env state touched: gc_log_domain_matches() is
 * deliberately side-effect free.
 */

#include "ghostcon/core/log.h"

#include <stdio.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
    else { printf("PASS: %s\n", msg); } \
} while (0)

int main(void)
{
    /* Empty / NULL spec: nothing matches. */
    CHECK(gc_log_domain_matches(NULL, "drm") == 0, "NULL spec matches nothing");
    CHECK(gc_log_domain_matches("", "drm") == 0, "empty spec matches nothing");

    /* Single-domain spec. */
    CHECK(gc_log_domain_matches("drm", "drm") == 1, "exact single-token match");
    CHECK(gc_log_domain_matches("drm", "vt") == 0, "non-listed domain rejected");
    CHECK(gc_log_domain_matches("drm", "dr") == 0, "prefix-only match rejected");
    CHECK(gc_log_domain_matches("drm", "drmx") == 0, "extension match rejected");

    /* Comma list. */
    CHECK(gc_log_domain_matches("input,selection", "selection") == 1,
          "second listed domain matches");
    CHECK(gc_log_domain_matches("input,selection", "input") == 1,
          "first listed domain matches");
    CHECK(gc_log_domain_matches("input,selection", "config") == 0,
          "unlisted domain rejected");
    CHECK(gc_log_domain_matches("input,", "input") == 1,
          "trailing comma tolerated");
    CHECK(gc_log_domain_matches("input,", "selection") == 0,
          "trailing-comma spec does not over-match");
    CHECK(gc_log_domain_matches("input,,selection", "") == 0,
          "empty token can never match a domain");
    CHECK(gc_log_domain_matches("input,  ,selection", "input") == 1,
          "whitespace-only tokens are separate tokens (no trimming, exact token still matches)");

    /* Wildcard. */
    CHECK(gc_log_domain_matches("all", "anything") == 1, "'all' wildcards");
    CHECK(gc_log_domain_matches("input,all", "anything") == 1,
          "'all' in list wildcards too");

    if (failures == 0)
        printf("ALL TESTS PASSED\n");
    return failures > 0 ? 1 : 0;
}
