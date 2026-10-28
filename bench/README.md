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

## Check throughput and parallel bodies (P03.13, P03.10, D3.10: >= 250k lines/s per core)

`gopp-bench-check` (`TypeCheckThroughput.cpp`) type-checks a generated
package of `-funcs` blocks (default 3000: 123,001 lines, one struct type, its
methods and a function with loops, calls, closures and generics per block)
`-runs` times. `-threads=1,2,4` adds a wall-clock run per thread count for the
parallel body phase (`CheckOptions::BodyThreads`). Release build, best of 10,
three rounds, the same 192-core host with other jobs running, 2026-10-27:

    nix develop -c cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
    nix develop -c ninja -C build-release gopp-bench-check
    nix develop -c build-release/bin/gopp-bench-check -runs 10 -threads=1,2,4,8,16

| Measurement | Result |
|---|---|
| before P03.10 (one thread, one arena and table per body, merged per body) | 865 to 870 k lines/s/core |
| after P03.10, 1 thread | **927 to 943 k lines/s/core** (about 3.7 times the budget) |
| 2 threads | 1.26 to 1.31x (1.19 to 1.23 M lines/s wall) |
| 4 threads | 1.66 to 1.78x |
| 8 threads | 1.95 to 2.03x (1.84 to 1.91 M lines/s wall) |
| 16 threads | 2.08 to 2.16x |

The budget is met with a wide margin, so no ADR records a gap. One thread is
faster than before P03.10: the bodies of one thread share one arena and write
straight into the package's side table, where every body used to get its own
arena and table that were then merged.

How the time splits (one thread): phase 1 (package-level declarations, serial
by design) takes about 0.025 s, the bodies 0.10 s. With 8 threads the bodies
take 0.020 s (5 times faster), and merging the workers' side tables 0.008 s;
phase 1 is then the larger part, so more threads gain little. `gopp check`
uses one thread per core but at most 8 (`CheckOptions::MaxAutoBodyThreads`,
`--check-j=N` overrides it), and a thread only for every 16 bodies.

What made the bodies scale (P03.10 merge):
- Every uniquing table of the TypeContext is split in 16 shards by the hash
  of the type, each with its own lock and allocator. This corpus creates
  about 66,000 new types per run; with one table lock (shared for lookups,
  exclusive for inserts) 8 threads were slower than one (0.8x). A per-thread
  cache in front of the tables gave nothing once the tables were sharded and
  was removed.
- The side tables of the workers are merged into a map reserved once for all
  of them: copying a big DenseMap into a smaller one that grows on the way
  clusters the entries and took 0.1 s.
What is left: phase 1 is serial, and the lazy completion of generic instances
(expansion, instantiated methods) takes one context-wide lock. Real packages,
which use fewer distinct types per function, should scale at least as well;
that is not measured yet (there is no large Go++ corpus before P13).

Correctness of the parallel phase does not depend on the numbers: the
diagnostics and the side table are the same for every thread count
(`ParallelCheck.*` in `tests/unit/Sema/ParallelTest.cpp`: 1, 2, 3, 4, 8, 12
and 16 threads, a shared pool, the error limit, the whole conformance corpus),
and the unit tests pass under ASan+UBSan and under TSan with no report.
