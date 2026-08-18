/* libFuzzer harness for ghostcon_term_feed() -- hardening plan #5.
 *
 * Feeds arbitrary/malformed byte streams straight into the VT stream
 * processor to catch crashes that a hand-written conformance test
 * wouldn't think to construct. A fresh ghostcon_term_t per input (not
 * persisted across LLVMFuzzerTestOneInput calls) keeps each run's
 * blast radius to exactly the bytes libFuzzer is currently minimizing,
 * and matches this project's "does this single blob of bytes crash a
 * clean terminal" threat model rather than modeling a long-lived
 * session with accumulated state.
 *
 * Requires clang (libFuzzer isn't a GCC feature) -- not wired into the
 * regular meson build for that reason. Build with:
 *
 *   clang -g -O1 -fsanitize=fuzzer,address,undefined \
 *       -I include -I build-san/subprojects/tomlc99 \
 *       tools/fuzz_term.c build-san/src/libghostcon-term.a \
 *       $(pkg-config --cflags --libs libghostty-vt) \
 *       -o /tmp/fuzz_term
 *
 * Run:  /tmp/fuzz_term -max_len=4096 -max_total_time=120 corpus/
 * (corpus/ is optional -- libFuzzer will start from an empty corpus
 * and mutate outward; the .seq files in tools/corpus (from the
 * existing Ghostty-comparison harness) make a decent seed corpus if
 * copied in first).
 */

#include "ghostcon/term/term.h"
#include <stdint.h>
#include <stddef.h>

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    ghostcon_term_t term;
    if (!ghostcon_term_init(&term, 80, 24, 500))
        return 0;

    ghostcon_term_feed(&term, data, size);

    ghostcon_term_deinit(&term);
    return 0;
}
