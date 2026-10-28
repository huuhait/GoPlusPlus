#!/usr/bin/env python3
"""P03.14: the error codes of the diagnostics never change (D3.11).

Every DIAG(Name, Code, Severity, Text) row of DiagnosticIDs.def has a number.
docs/errors/codes.lock lists "E0042 err_name" for every code ever given. This
check fails when

  * a locked name is gone from the table      (removed),
  * a locked name has another number          (renumbered),
  * a locked number belongs to another name   (reused),
  * the table has a number twice, or a name twice, or a code of 0 on a name
    other than `custom`,
  * the table has a name that the lock file does not list yet (run with
    --update to append it; the file is only ever appended to).

Usage: check-diag-codes.py --def DiagnosticIDs.def --lock codes.lock [--update]
"""
import argparse
import re
import sys

ROW = re.compile(r"DIAG\((\w+), (\d+),")
ANY = re.compile(r"\bDIAG\b")
LOCK = re.compile(r"^E(\d{4,})\s+(\w+)\s*$")


def blank_comments_and_strings(text):
    """Same text with comments and string contents replaced by spaces (newlines
    stay), so that a `DIAG(` inside a message or a comment is not a row."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        elif c == '"':
            j = i + 1
            while j < n and text[j] != '"' and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append('"' + re.sub(r"[^\n]", " ", text[i + 1:j - 1]) + '"')
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def read_table(path):
    """Returns (rows, problems). A row is `DIAG(name, number,` at the start of a
    line. Any other use of DIAG that is not a preprocessor line (a row split
    over lines, extra spaces, a macro) cannot be read, so it is an error: it
    must not escape the lock check."""
    text = blank_comments_and_strings(open(path, encoding="utf-8").read())
    rows = []
    problems = []
    for lineno, line in enumerate(text.split("\n"), 1):
        if line.lstrip().startswith("#") or not ANY.search(line):
            continue
        m = ROW.match(line)
        if m:
            rows.append((m.group(1), int(m.group(2)), lineno))
        else:
            problems.append("%s:%d: error: cannot parse this DIAG row, write it as `DIAG(Name, 123, Severity, \"text\")` with the name and the number on the first line: %s" % (path, lineno, line.strip()))
    return rows, problems


def read_lock(path):
    entries = []
    with open(path, encoding="utf-8") as f:
        for n, raw in enumerate(f, 1):
            s = raw.strip()
            if not s or s.startswith("#"):
                continue
            m = LOCK.match(s)
            if not m:
                sys.exit("%s:%d: error: not a lock line: %s" % (path, n, s))
            entries.append((int(m.group(1)), m.group(2), n))
    return entries


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--def", dest="deffile", required=True)
    ap.add_argument("--lock", required=True)
    ap.add_argument("--update", action="store_true")
    a = ap.parse_args()

    table, errors = read_table(a.deffile)
    lock = read_lock(a.lock)

    by_name = {}
    by_code = {}
    for name, code, line in table:
        if name in by_name:
            errors.append("%s:%d: error: name %s is in the table twice" % (a.deffile, line, name))
        by_name[name] = (code, line)
        if code == 0:
            if name != "custom":
                errors.append("%s:%d: error: %s has no code (only `custom` has code 0)" % (a.deffile, line, name))
            continue
        if code in by_code:
            errors.append("%s:%d: error: E%04d is given to %s and to %s" % (a.deffile, line, code, by_code[code][0], name))
        by_code[code] = (name, line)

    locked_names = {}
    locked_codes = {}
    for code, name, n in lock:
        if name in locked_names or code in locked_codes:
            errors.append("%s:%d: error: lock file lists %s or E%04d twice" % (a.lock, n, name, code))
        locked_names[name] = code
        locked_codes[code] = name

    for code, name, n in lock:
        if name not in by_name:
            errors.append("%s:%d: error: E%04d %s was removed from the table: a code is never deleted, keep the row (or restore it)" % (a.lock, n, code, name))
        elif by_name[name][0] != code:
            errors.append("%s:%d: error: %s was E%04d and is now E%04d: a code never changes" % (a.deffile, by_name[name][1], name, code, by_name[name][0]))
    for name, (code, line) in by_name.items():
        if code == 0:
            continue
        if code in locked_codes and locked_codes[code] != name:
            errors.append("%s:%d: error: E%04d belonged to %s and is reused by %s: a code is never reused" % (a.deffile, line, code, locked_codes[code], name))

    new = [(code, name) for name, (code, line) in by_name.items()
           if code != 0 and name not in locked_names and locked_codes.get(code) in (None, name)]
    if errors:
        print("\n".join(errors), file=sys.stderr)
        print("check-diag-codes: %d problem(s); the lock file %s is the record of the codes" % (len(errors), a.lock), file=sys.stderr)
        return 1
    if new:
        new.sort()
        if a.update:
            with open(a.lock, "a", encoding="utf-8") as f:
                for code, name in new:
                    f.write("E%04d %s\n" % (code, name))
            print("check-diag-codes: added %d code(s) to %s" % (len(new), a.lock))
            return 0
        nxt = max([c for c, _, _ in lock] + [c for c in by_code] + [0]) + 1
        for code, name in new:
            print("%s: error: E%04d %s is not in the lock file" % (a.lock, code, name), file=sys.stderr)
        print("check-diag-codes: run `tools/check-diag-codes.py --def %s --lock %s --update` (the next free code is E%04d)" % (a.deffile, a.lock, nxt), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
