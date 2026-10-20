#!/usr/bin/env python3
"""Run every house-style rule in one go.

The rules themselves live in one script each, under tools/format_<rule>.py.
Each takes --check (report and fail) or --apply (rewrite in place), and is
idempotent.

This wrapper just runs them in a fixed order and aggregates the result:

    python3 tools/format.py           # apply every rule in place
    python3 tools/format.py --check   # report only; exit non-zero on drift

Pass --only <rule> to run a single rule, e.g. --only extern.

The order matters only in that it is fixed: a rule that rewrites another
rule's output is still idempotent either way, so a partially-applied run
converges on the next one.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# The rule scripts, in the order they are applied.
RULES = [
    'comments',
    'braces',
    'signatures',
    'pointers',
    'extern',
]


def main():
    argv = sys.argv[1:]
    apply = '--apply' in argv
    check = '--check' in argv
    only = None
    if '--only' in argv:
        i = argv.index('--only')
        if i + 1 >= len(argv):
            print("--only needs a rule name", file=sys.stderr)
            return 2
        only = argv[i + 1]
        argv = argv[:i] + argv[i + 2:]
    unknown = [a for a in argv if a not in ('--apply', '--check')]
    if unknown:
        print(f"unknown argument: {' '.join(unknown)}", file=sys.stderr)
        return 2
    if not apply and not check:
        # default: apply
        apply = True

    rules = RULES
    if only is not None:
        if only not in RULES:
            print(f"unknown rule '{only}'; known rules: {', '.join(RULES)}",
                  file=sys.stderr)
            return 2
        rules = [only]

    mode = '--apply' if apply else '--check'
    failed = []
    for rule in rules:
        script = os.path.join(HERE, f'format_{rule}.py')
        if not os.path.exists(script):
            print(f"{rule}: MISSING {script}", file=sys.stderr)
            failed.append(rule)
            continue
        r = subprocess.run([sys.executable, script, mode], cwd=ROOT)
        if r.returncode != 0:
            failed.append(rule)

    if failed:
        print(f"\nFAILED: {', '.join(failed)}", file=sys.stderr)
        return 1
    print(f"\nOK: {len(rules)} rule(s) {'applied' if apply else 'checked'}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
