# Generates the input of strict-stress.test: N nested @Strict structs (each
# holds the one before), N sibling functions that build or declare one, one
# @Strict struct with N fields (half of them @Option or Option[int]), N
# instances BoxOpt[S<k>] of a generic `V T @Option` (each an error: S<k> has
# no zero value), and N sibling pairs of a @Strict K<k> and a plain L<k> that
# convert both ways (their pointer, func and S0 fields have no zero value, so
# they are required on both sides).
import sys

n = int(sys.argv[1])
mode = sys.argv[2] if len(sys.argv) > 2 else ""
if mode == "clone":
    # Clone checks: a chain of N cloneable structs Q<k> (one Clone() call per
    # level), a chain C<k> of N structs whose leaf holds a drop type without
    # Clone (one call at the top: an error), and two DAGs of 16 levels with 3
    # fields of the previous level each (3^16 paths unless every named type
    # caches its answer): D<k> cloneable, B<k> blocked at the leaf.
    out = ["package main", "", "type Mu struct{ b bool }",
           "func (m *Mu) drop() {}", "type Q0 struct{ x []int }",
           "type C0 struct{ mu Mu }", "type D0 struct{ x []int }",
           "type B0 struct{ mu Mu }"]
    for k in range(1, n + 1):
        out.append("type Q%d struct{ in Q%d }" % (k, k - 1))
        out.append("type C%d struct{ in C%d }" % (k, k - 1))
    for k in range(1, 17):
        out.append("type D%d struct{ a D%d; b D%d; c D%d }" % (k, k - 1, k - 1, k - 1))
        out.append("type B%d struct{ a B%d; b B%d; c B%d }" % (k, k - 1, k - 1, k - 1))
    # During declarations (a `[T Clone]` constraint is checked while the
    # struct types are still being declared): F<k> holds two clean DAGs D<k-1>
    # and then F<k-1>, blocked at the leaf.
    out.append("type F0 struct{ mu Mu }")
    for k in range(1, 17):
        out.append("type F%d struct{ a D%d; b D%d; c F%d }" % (k, k - 1, k - 1, k - 1))
    out.append("type Box[T Clone] struct{ v T }")
    out.append("type UseD struct{ v Box[D16] }")
    out.append("type UseF struct{ v Box[F16] }")
    for k in range(n + 1):
        out.append("func okq%d(q Q%d) { _ = q.Clone() }" % (k, k))
    out.append("func okd(d D16) { _ = d.Clone() }")
    out.append("func badc(c C%d) { _ = c.Clone() }" % n)
    out.append("func badb(b B16) { _ = b.Clone() }")
    out.append("func main() {}")
    print("\n".join(out))
    sys.exit(0)
if mode == "walk":
    # Inputs of the clone-walk-*.test files
    # (satisfiesClone walks the type graph with an explicit stack: no
    # recursion, no depth cut). argv[3] picks one:
    #   nest K W body|decl: N<k> holds N<k-1> under W levels of slices, K
    #     levels, cloneable; B<k> the same over a drop type (blocked). Asked
    #     by x.Clone() in a body, or by a [T Clone] instance while the types
    #     are declared.
    #   chain ab|ba: N cloneable named structs Q<k> (a Clone() call per
    #     level, or per 10th level at 30000), N blocked ones C<k> (calls at
    #     three levels), and the 16x3 DAGs of the "clone" mode; ab and ba
    #     give the functions in opposite orders.
    #   perlevel: a [T Clone] and a [T Copy] instance per level of N levels.
    #   generic: a chain of N generic types used by 2N generic functions.
    #   iface: 1000 queries on a chain instantiated with an interface that
    #     mentions a type parameter.
    #   runaway: a generic type whose instances grow without end.
    #   slices: 1980 levels of slices in one type, of int and of a drop type.
    #   cycle W: a ring of N types L<k> holding L<k+1> under W levels of
    #     slices (cloneable, a call per type) and a ring K<k> whose last type
    #     also holds a drop type (calls at two places). The ring is short (the
    #     declaration of a ring of about 1000 types overflows the stack, also
    #     on master); the slices make the walk long.
    case = sys.argv[3]
    out = ["package main", "", "type Mu struct{ b bool }",
           "func (m *Mu) drop() {}"]
    if case == "nest":
        w, where = int(sys.argv[4]), sys.argv[5]
        out += ["type N0 struct{ x []int }", "type B0 struct{ mu Mu }"]
        for k in range(1, n + 1):
            out.append("type N%d struct{ x %sN%d }" % (k, "[]" * w, k - 1))
            out.append("type B%d struct{ x %sB%d }" % (k, "[]" * w, k - 1))
        if where == "body":
            out.append("func ok(x N%d) { _ = x.Clone() }" % n)
            out.append("func bad(x B%d) { _ = x.Clone() }" % n)
        else:
            out.append("type Need[T Clone] struct{ v []T }")
            out.append("type UseN struct{ n Need[N%d] }" % n)
            out.append("type UseB struct{ n Need[B%d] }" % n)
    elif case == "chain":
        out += ["type Q0 struct{ x []int }", "type C0 struct{ mu Mu }",
                "type D0 struct{ x []int }", "type E0 struct{ mu Mu }"]
        for k in range(1, n + 1):
            out.append("type Q%d struct{ in Q%d }" % (k, k - 1))
            out.append("type C%d struct{ in C%d }" % (k, k - 1))
        for k in range(1, 17):
            out.append("type D%d struct{ a D%d; b D%d; c D%d }" % (k, k - 1, k - 1, k - 1))
            out.append("type E%d struct{ a D%d; b D%d; c E%d }" % (k, k - 1, k - 1, k - 1))
        step = max(1, n // 3000)  # at most about 3000 calls (bodies cost)
        fs = ["func okq%d(q Q%d) { _ = q.Clone() }" % (k, k) for k in range(0, n + 1, step)]
        fs += ["func badc%d(c C%d) { _ = c.Clone() }" % (k, k) for k in (1, n // 2, n)]
        fs += ["func okd(d D16) { _ = d.Clone() }", "func bade(e E16) { _ = e.Clone() }"]
        out += fs if sys.argv[4] == "ab" else fs[::-1]
    elif case == "perlevel":
        # A constraint per level, checked while the types are declared: N<k>
        # holds Need[N<k-1>] ([T Clone]) and P<k> holds NeedC[P<k-1>] ([T
        # Copy]); quadratic unless the answers are stored once the types are
        # declared. S<k> holds a string n levels down: not Copy (an error at
        # UseS and at the copy in cp); P<n> copies fine.
        out += ["type Need[T Clone] struct{ v []T }",
                "type NeedC[T Copy] struct{ v T }",
                "type N0 struct{ x []int }", "type P0 struct{ x int }",
                "type S0 struct{ s string }"]
        for k in range(1, n + 1):
            out.append("type N%d struct{ x []N%d; u Need[N%d] }" % (k, k - 1, k - 1))
            out.append("type P%d struct{ in P%d; c NeedC[P%d] }" % (k, k - 1, k - 1))
            out.append("type S%d struct{ in S%d }" % (k, k - 1))
        out += ["type UseS struct{ c NeedC[S%d] }" % n,
                "func cp(s S%d) S%d { t := s; return t }" % (n, n),
                "func cq(p P%d) P%d { t := p; return t }" % (n, n)]
    elif case == "generic":
        # N generic types G<k>[T] (each holds G<k-1>[T]) and per level a
        # generic function with its own T: c<k> clones a G<k>[T] for a
        # [T Clone] (fine) and m<k> copies one out of a borrowed [T Copy]
        # parameter (fine); b at three levels clones it for a [T any] (an
        # error naming the path down to T). Each function has its own T, so
        # the instances are new in every body: linear only because the
        # answers are asked with shared stand-ins for T.
        out.append("type G0[T any] struct{ V T }")
        for k in range(1, n + 1):
            out.append("type G%d[T any] struct{ P G%d[T] }" % (k, k - 1))
        for k in range(n + 1):
            out.append("func c%d[T Clone](g G%d[T]) { _ = g.Clone() }" % (k, k))
            out.append("func m%d[T Copy](g G%d[T]) G%d[T] { h := g; return h }" % (k, k, k))
        for k in (1, n // 2, n):
            out.append("func b%d[T any](g G%d[T]) { _ = g.Clone() }" % (k, k))
    elif case == "iface":
        # A type parameter inside an interface literal (not uniqued): the
        # queries must not build a new copy of the N-level chain each time.
        out.append("func needCopy[T Copy](x T) {}")
        out.append("type G0[T any] struct{ v int }")
        for k in range(1, n + 1):
            out.append("type G%d[T any] struct{ next G%d[T] }" % (k, k - 1))
        calls = "; ".join(["needCopy(x); _ = x.Clone()"] * 1000)
        out.append("func f[T any](x G%d[interface{ Get() T }]) { %s }" % (n, calls))
    elif case == "runaway":
        # T[int] reaches T[Option[int]], T[Option[Option[int]]], ... without
        # end (Go: an instantiation cycle; not reported here yet): the walk
        # stops at its instance bound with "not cloneable".
        # The capped answer is stored for the root, so 100 queries cost one
        # walk.
        out += ["type T[P any] struct {", "\ts []T[Option[P]]", "\tn int", "}"]
        out += ["func f%d(t T[int]) { _ = t.Clone() }" % k for k in range(100)]
    elif case == "slices":
        out += ["type A1 = " + "[]" * 990 + "int", "type A2 = " + "[]" * 990 + "A1",
                "type M1 = " + "[]" * 990 + "Mu", "type M2 = " + "[]" * 990 + "M1",
                "type N0 struct{ x A2 }", "type B0 struct{ x M2 }",
                "func ok(x N0) { _ = x.Clone() }", "func bad(x B0) { _ = x.Clone() }"]
    elif case == "cycle":
        sl = "[]" * int(sys.argv[4])
        for k in range(n):
            nx = (k + 1) % n
            out.append("type L%d struct{ next %sL%d; v int }" % (k, sl, nx))
            last = "; mu Mu" if k == n - 1 else ""
            out.append("type K%d struct{ next %sK%d%s }" % (k, sl, nx, last))
        out += ["func okl%d(x L%d) { _ = x.Clone() }" % (k, k) for k in range(n)]
        out += ["func badk%d(x K%d) { _ = x.Clone() }" % (k, k) for k in (0, n // 2)]
    out.append("func main() {}")
    print("\n".join(out))
    sys.exit(0)
if mode == "order":
    # N0 holds 1980 levels of slices of int (two aliases of 990 levels), N<n>
    # is n named structs above it. Whether Clone() is accepted does not
    # depend on which of the two functions is checked first (argv[3] is "ab"
    # or "ba").
    out = ["package main", "", "type A1 = " + "[]" * 990 + "int",
           "type A2 = " + "[]" * 990 + "A1", "type N0 struct{ x A2 }"]
    for k in range(1, n + 1):
        out.append("type N%d struct{ n N%d }" % (k, k - 1))
    fa = "func a(x N0) { _ = x.Clone() }"
    fb = "func b(x N%d) { _ = x.Clone() }" % n
    out += [fa, fb] if sys.argv[3] == "ab" else [fb, fa]
    out.append("func main() {}")
    print("\n".join(out))
    sys.exit(0)
if mode == "generic":
    # Generic chains over a type parameter: G<k>[T] holds G<k-1>[T]; per level
    # a conversion from H<k>[T] to R<k>[T] (R<k> marks the field @Required: an
    # error, since an instance of T may have a zero value) and a `var z
    # G<k>[T]` (an error: T may have no zero value). Each function has its own
    # T, so no instance is shared between levels.
    out = ["package main", "", "type G0[T any] struct{ V T }"]
    for k in range(1, n):
        out.append("type G%d[T any] struct{ P G%d[T] }" % (k, k - 1))
    for k in range(n):
        out.append("type H%d[T any] struct{ F G%d[T] }" % (k, k))
        out.append("type R%d[T any] struct{ F G%d[T] @Required }" % (k, k))
        out.append("func c%d[T any](h H%d[T]) R%d[T] { return R%d[T](h) }" % (k, k, k, k))
        out.append("func z%d[T any]() { var z G%d[T]; _ = z }" % (k, k))
    out.append("func main() {}")
    print("\n".join(out))
    sys.exit(0)
if mode == "chain":
    # N nested named structs A<k> (each holds the one before) and, per level,
    # a defined type B<k> of the same underlying type and a conversion
    # B<k>(a). Every conversion asks "is A<k> pinned?", which walks the whole
    # chain unless the answer is cached: quadratic without the cache.
    out = ["package main", "", "type A0 struct{ N int }"]
    for k in range(1, n):
        out.append("type A%d struct{ P A%d }" % (k, k - 1))
    for k in range(n):
        out.append("type B%d A%d" % (k, k))
        out.append("func conv%d(a A%d) B%d { return B%d(a) }" % (k, k, k, k))
    out.append("func main() {}")
    print("\n".join(out))
    sys.exit(0)
out = ["package main", ""]
out.append("@Strict\ntype S0 struct {\n\tA int\n\tB Option[int]\n\tC int @Option\n\t_ int\n}\n")
for k in range(1, n):
    out.append("@Strict\ntype S%d struct {\n\tP S%d\n\tN int\n\tX Option[int]\n}\n" % (k, k - 1))
out.append("@Strict\ntype Wide struct {")
for k in range(n):
    kind = k % 3
    out.append("\tF%d %s" % (k, ["int", "Option[int]", "int @Option"][kind]))
out.append("}\n")
for k in range(n):
    prev = "S%d" % (k - 1) if k else None
    if k == 0:
        out.append("func ok0() S0 { return S0{A: 1} }")
        out.append("func bad0() S0 { return S0{} }")
    else:
        out.append("func ok%d(p S%d) S%d { return S%d{P: p, N: 1} }" % (k, k - 1, k, k))
        out.append("func bad%d() S%d { return S%d{N: 1} }" % (k, k, k))
        out.append("func zero%d() { var z S%d; _ = z }" % (k, k))
out.append("@Strict\ntype BoxOpt[T any] struct {\n\tN int\n\tV T @Option\n}\n")
for k in range(n):
    out.append("func box%d(b BoxOpt[S%d]) {}" % (k, k))
for k in range(n):
    out.append("@Strict\ntype K%d struct {\n\tID int\n\tP *S0\n\tF func(int) int\n\tQ S0\n}" % k)
    out.append("type L%d struct {\n\tID int @Required\n\tP *S0\n\tF func(int) int\n\tQ S0\n}" % k)
    out.append("func conv%d(a K%d, b L%d) (L%d, K%d) { return L%d(a), K%d(b) }" % (k, k, k, k, k, k, k))
out.append("func wide() Wide { return Wide{} }")
out.append("func wideOK() Wide { return Wide{%s} }" % ", ".join(
    "F%d: 1" % k for k in range(n) if k % 3 == 0))
out.append("func main() {}")
print("\n".join(out))
