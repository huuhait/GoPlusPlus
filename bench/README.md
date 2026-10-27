# bench/

`gopp-bench-parse` (`ParseThroughput.cpp`) measures lex + parse throughput on
the Go standard library (`$(go env GOROOT)/src`, read at run time; nothing is
copied into the repository). It is not built by default:

    nix develop -c cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
    nix develop -c ninja -C build-release gopp-bench-parse
    nix develop -c build-release/bin/gopp-bench-parse -runs=10 -threads=16

(`ninja bench-parse` builds and runs it with the defaults.) Only a Release
build counts: the Dev build has -O1 and assertions.

| Mode | What |
|---|---|
| serial | one thread, every file one after the other, CPU time: the number of the D2.9 budget |
| pkg-par | `gopp::parseFiles` on a shared pool, one package after the other, its files in parallel (D2.9), wall clock |
| all-par | `gopp::parseFiles` on every file at once (packages in parallel too, as the build driver can do), wall clock |

Each mode is the best of `-runs`. `testdata` directories are left out (many
files have syntax errors on purpose) unless `-with-testdata` is given.

## Result (P02.11, D2.9 budget: lex + parse >= 1 M lines/s per core)

Go 1.25.14 `src` without testdata: 6240 files, 738 packages, 2.53 M lines,
76 MiB. Release build, best of 10, x86-64 build host (192 cores, other jobs
running), 2026-10-27:

| Mode | Result |
|---|---|
| serial, 1 core | **4.57 M lines/s** (138 MiB/s) |
| pkg-par, 16 threads | 12.0 M lines/s (speedup 2.6x) |
| all-par, 16 / 64 threads | 46.8 / 88.3 M lines/s (speedup 10.2x / 19.6x) |

The budget is met about 4.5 times over, so no ADR records a gap. pkg-par is
capped by the corpus, not the parser: with one package after the other, the
biggest file of each package adds up to 27.7% of all lines, so no thread count
can do better than 3.6x. Parsing packages in parallel (all-par) removes that
barrier.

The comment groups of the comment map (D2.3) are built while parsing; they
look a line number up only when a comment follows a token. A line lookup for
every token (as in a first version) made the serial parse about 1.5 times
slower (2.9 M lines/s).
