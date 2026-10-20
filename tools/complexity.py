#!/usr/bin/env python3
"""The complexity gate (REFACTOR_PLAN.md section 7.6).

Nothing else stops a hotspot from growing back: the style checks catch
formatting, the tests catch behaviour, and neither of them notices that a
function has quietly become a 40-branch tangle. This is the gate that does.

It runs lizard over src/ and fails when any function's cyclomatic
complexity is above the threshold.

    python3 tools/complexity.py            # the gate
    python3 tools/complexity.py --report   # the numbers, without failing

The threshold is 30. The worst function in the tree is at 28, so the gate
is a pure regression net today: it fails on a function that has become
harder to change safely, not on anything that already exists.

lizard is a measurement tool for this gate, not a repo dependency: the
script needs it installed (`pip install lizard`) but the build does not.
"""
import os
import re
import subprocess
import sys

# The cyclomatic complexity a function may reach before the gate fails.
#
# A CCN-10 function can be enumerated by inspection; a CCN-84 one cannot, so
# it has to be reasoned about structurally instead. The threshold is set
# above the current worst function, so the gate only ever fires on
# something that got worse.
THRESHOLD = 30

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# A row of lizard's per-function table:
#   NLOC  CCN  token  PARAM  length  location
# The summary tables at the end of its output have no '@' in them, so
# requiring it keeps them out.
ROW = re.compile(r"^\s*(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\S+@\S+)\s*$")


def lizard_rows():
    """Runs lizard over src/ and yields (ccn, location) for every function."""
    try:
        proc = subprocess.run(
            ["lizard", "src/"],
            cwd=ROOT, capture_output=True, text=True, check=False)
    except FileNotFoundError:
        print("error: lizard is not installed (pip install lizard)", file=sys.stderr)
        sys.exit(2)

    # lizard exits non-zero when it has warnings of its own (anything above
    # CCN 15), which is expected here -- the exit code says nothing about
    # whether the run worked.
    #
    # Its output lists every function twice (once per file, once in the
    # aggregate table), so the locations are deduplicated.
    seen = set()
    for line in proc.stdout.splitlines():
        m = ROW.match(line)
        if not m:
            continue
        location = m.group(6)
        if location in seen:
            continue
        seen.add(location)
        yield int(m.group(2)), location


def main():
    report = "--report" in sys.argv[1:]

    rows = sorted(lizard_rows(), key=lambda r: -r[0])
    if not rows:
        print("error: lizard reported no functions -- did it run?", file=sys.stderr)
        sys.exit(2)

    over = [(ccn, loc) for ccn, loc in rows if ccn > THRESHOLD]
    worst = rows[0]

    print(f"complexity gate: {len(rows)} functions, threshold CCN {THRESHOLD}")
    print(f"worst: {worst[1]} at CCN {worst[0]}")
    if report:
        for ccn, loc in rows[:20]:
            print(f"  {ccn:>4}  {loc}")

    if over and not report:
        print("", file=sys.stderr)
        print(f"error: {len(over)} function(s) above the threshold:", file=sys.stderr)
        for ccn, loc in over:
            print(f"  CCN {ccn:>4}  {loc}", file=sys.stderr)
        print("", file=sys.stderr)
        print("Split the function, or raise the threshold in tools/complexity.py",
              file=sys.stderr)
        print("if the complexity is inherent (REFACTOR_PLAN.md section 7.1).",
              file=sys.stderr)
        sys.exit(1)

    if over:
        print(f"note: {len(over)} function(s) above the threshold (--report does not fail)")
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
