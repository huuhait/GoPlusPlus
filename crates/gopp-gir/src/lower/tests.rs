use std::sync::Arc;

use gopp_diag::{Diagnostic, Sink};
use gopp_sema::types::Types;
use gopp_sema::{Package, PackageLoader, Unit, check};
use gopp_span::{Dialect, SourceFile, SourceMap};
use gopp_syntax::ast::File;
use gopp_syntax::parser::parse;
use gopp_target::TargetLayout;
use lasso::ThreadedRodeo;

use super::{Checked, Shared, lower_counted};
use crate::Module;
use gopp_testutil::linear::assert_near_linear;

use crate::print_module;

/// Every import is a package about which nothing is known.
struct NoPackages;

impl PackageLoader for NoPackages {
    fn load(&self, path: &str) -> Result<Arc<Package>, String> {
        Ok(Arc::new(Package::fake(path)))
    }
}

/// Checks the package `main` made of the files, each a name and its
/// text, and gives it to `then` with what the checker reported.
fn checked<T>(texts: &[(&str, &str)], then: impl FnOnce(&Checked<'_>, Vec<String>) -> T) -> T {
    let map = SourceMap::new();
    let sink = Sink::new(1000);
    let names = ThreadedRodeo::new();
    let mut sources = Vec::new();
    let mut files: Vec<File> = Vec::new();
    for &(name, text) in texts {
        let dialect = Dialect::from_path(name.as_ref()).unwrap();
        let id = map.add(SourceFile::new(name.into(), text.to_owned(), dialect).unwrap());
        let source = map.file(id);
        files.push(parse(id, &source, &names, &sink));
        sources.push(source);
    }
    let units: Vec<Unit> = files
        .iter()
        .zip(&sources)
        .map(|(file, source)| Unit { file, source })
        .collect();
    let types = Types::new(&names);
    let package = check("main", &units, &NoPackages, &names, &types, &sink);
    let pairs: Vec<(&File, &SourceFile)> = files
        .iter()
        .zip(&sources)
        .map(|(file, source)| (file, &**source))
        .collect();
    let checked = Checked {
        package: &package,
        files: &pairs,
        types: &types,
        names: &names,
    };
    let said = |diagnostic: &Diagnostic| {
        format!("{}: {}", map.location(diagnostic.span), diagnostic.message)
    };
    then(&checked, sink.finish().iter().map(said).collect())
}

/// A module with the steps that the lowering took.
struct Lowered {
    module: Module,
    steps: u64,
}

/// The module of the package `main` made of the files, with what the
/// checker and the lowering report. A run of the lowering takes
/// `source_each` bytes.
fn lowered_with(texts: &[(&str, &str)], source_each: usize) -> (Lowered, Vec<String>) {
    checked(texts, |checked, mut messages| {
        let (module, reported, steps) = lower_counted(checked, TargetLayout::new(64), source_each);
        // The places of what the lowering reports are in the files of
        // the package, which have their names.
        for diagnostic in reported {
            let (_, source) = checked.files[file_of(checked, &diagnostic)];
            let at = source.line_col(diagnostic.span.lo);
            messages.push(format!(
                "{}:{}:{}: {}",
                source.path().display(),
                at.line,
                at.column,
                diagnostic.message
            ));
        }
        (Lowered { module, steps }, messages)
    })
}

/// Which file of the package the diagnostic is in.
fn file_of(checked: &Checked<'_>, diagnostic: &Diagnostic) -> usize {
    let files = checked.files.iter();
    let mut found = files
        .enumerate()
        .filter(|(_, (file, _))| file.id == diagnostic.span.file);
    found.next().map_or(0, |(at, _)| at)
}

/// The text of the module of one file of Go++, which must lower without
/// a report.
fn gir(text: &str) -> String {
    let (lowered, messages) = lowered_with(&[("a.gopp", text)], usize::MAX);
    assert_eq!(messages, [] as [&str; 0]);
    print_module(&lowered.module)
}

#[test]
fn arithmetic_has_the_results_and_the_checks_of_go() {
    insta::assert_snapshot!(gir("package main

func ints(a, b int, u uint8, n uint, k int) int {
	c := a + b*3 - a/b
	c += a % 4
	c = c&b | a ^ b
	c &^= 7
	c = -c + ^a
	x := u << n
	y := a >> k
	z := a << 70
	_, _, _ = x, y, z
	return c / 8
}

func floats(x, y float64, f float32) float64 {
	q := x / y
	q -= float64(f) * 0.5
	return -q
}

func compares(a, b int, x float64, p, q *int) bool {
	ok := a < b
	ok = a >= b
	ok = x != 1.5
	ok = p == q
	return !ok
}

func converts(a int, u uint8, x float64) (int8, float32, uint64, int) {
	type Celsius float64
	c := Celsius(x)
	return int8(a), float32(c), uint64(u), int(x)
}
"));
}

#[test]
fn conditions_branch_and_loops_have_their_ways_out() {
    insta::assert_snapshot!(gir("package main

func sign(a int) int {
	if a < 0 {
		return -1
	} else if a == 0 {
		return 0
	}
	return 1
}

func both(a, b int) bool {
	inside := a > 0 && b > 0 || a == b
	if !(a < b) || inside && b != 3 {
		return inside
	}
	return false
}

func loops(n int) int {
	sum := 0
	for i := 0; i < n; i++ {
		if i%2 == 0 {
			continue
		}
		if i > 100 {
			break
		}
		sum += i
	}
	for sum > 10 {
		sum /= 2
	}
outer:
	for {
		for j := 0; ; j++ {
			if j > n {
				break outer
			}
			if j == 3 {
				continue outer
			}
		}
	}
	return sum
}
"));
}

#[test]
fn a_switch_is_a_table_or_a_row_of_comparisons() {
    insta::assert_snapshot!(gir("package main

func table(a int) int {
	switch a {
	case 1, 2:
		return 10
	case -1:
		a++
		fallthrough
	default:
		a *= 2
	case 7:
		break
	}
	return a
}

func row(a, b int, x float64) int {
	switch c := a + b; c {
	case a:
		return 1
	case b, 3:
		return 2
	}
	switch x {
	case 0.5:
		return 3
	}
	switch {
	case a > b:
		return 4
	case a < b && b > 0:
		return 5
	}
	return 0
}
"));
}

#[test]
fn a_goto_leaves_the_scopes_that_it_jumps_out_of() {
    insta::assert_snapshot!(gir("package main

func jumps(n int) int {
	i := 0
again:
	j := i * 2
	if j < n {
		k := j
		i += k + 1
		goto again
	}
	{
		m := i
		if m > 5 {
			goto done
		}
		i = m
	}
done:
	return i
}
"));
}

#[test]
fn calls_pass_their_arguments_and_take_their_results() {
    insta::assert_snapshot!(gir("package main

type Point struct {
	x, y int
}

type Line struct {
	from Point
	end  *Point
}

func (p Point) sum() int { return p.x + p.y }

func (p *Point) move(dx int) { p.x += dx }

func pair(a int) (int, bool) { return a, a > 0 }

func named(a int) (n int, ok bool) {
	if a > 0 {
		n = a
		return
	}
	return a * 2, true
}

func swapped(a int) (n, m int) {
	n, m = a, a+1
	return m, n
}

func take(a int, ok bool) int { return a }

func calls(l Line, q *Point, n *int) int {
	a, ok := pair(1)
	var b, c = named(a)
	_ = c
	d := take(pair(b))
	p := Point{a, d}
	p.move(1)
	q.move(p.sum())
	e := q.sum() + l.from.sum() + l.end.sum()
	l.from.move(l.end.x)
	e = take(n, ok) + n
	if ok {
		return take(named(e))
	}
	return e
}
"));
}

#[test]
fn structs_arrays_and_pointers_are_places() {
    insta::assert_snapshot!(gir("package main

type Point struct {
	x, y int
}

type Box struct {
	at    Point
	sizes [3]int
	next  *Box
}

func literals(a int, next *Box) Box {
	p := Point{y: a}
	sizes := [3]int{1, 2: a}
	grid := [2][2]int{{1, a}, {}}
	_ = grid
	var many [100]int8
	many = [100]int8{5: 1, 2}
	_ = many
	return Box{p, sizes, next}
}

func places(b *Box, i int, grid *[4][2]float64) float64 {
	b.at.x = b.sizes[i]
	b.next.sizes[2] = i
	p := &b.at
	p = Point{1, 2}
	p = Point{p.y, p.x}
	p.y++
	q := &b.sizes[i]
	q += len(b.sizes) + len(grid)
	q = *q * 2
	grid[i][1] = grid[0][i]
	b = b.next
	return (*grid)[3][0]
}
"));
}

#[test]
fn an_assignment_evaluates_all_before_it_stores_any() {
    insta::assert_snapshot!(gir("package main

type Node struct {
	value int
	next  *Node
}

func next() int { return 1 }

func assigns(a, b, i int, arr [4]int, n, m *Node) int {
	a, b = b, a
	arr[i], i = 7, 2
	n.value, n = 3, m
	arr[next()] = next()
	arr[i] += next()
	a, _ = b, next()
	x, i := arr[i], 0
	return x + i + a
}
"));
}

#[test]
fn operands_are_evaluated_from_left_to_right() {
    insta::assert_snapshot!(gir("package main

type Counter struct {
	n int
}

var total int

func bump() int {
	total++
	return total
}

func (c Counter) add(a, b int) int { return c.n + a + b }

func three(a, b, c int) int { return a + b + c }

func order(c Counter, x int, p *int) int {
	a := x + bump()
	b := bump() + x
	d := total + bump() + total
	e := three(x, bump(), *p)
	f := c.add(bump(), c.n)
	g := Counter{x}.add(total, bump())
	h := [3]int{total, bump(), total}
	return a + b + d + e + f + g + h[0]
}
"));
}

#[test]
fn variables_live_from_their_declaration_to_the_end_of_their_block() {
    insta::assert_snapshot!(gir("package main

func scopes(n int) int {
	a := 1
	{
		a := a + 1
		b := a
		if b > n {
			c := b
			return c
		}
		for d := 0; d < n; d++ {
			e := d
			if e == 3 {
				break
			}
			var f int
			f = e
			_ = f
		}
	}
	var g, h int
	return a + g + h
}
"));
}

#[test]
fn the_variables_of_a_package_are_globals_and_its_init_functions_are_numbered() {
    let (lowered, messages) = lowered_with(
        &[
            (
                "b.gopp",
                "package main

var second float64

func init() { second = 2 }

func init() { first++ }
",
            ),
            (
                "a.gopp",
                "package main

var first, _ int

func init() { first = int(second) }

func main() {}
",
            ),
        ],
        usize::MAX,
    );
    assert_eq!(messages, [] as [&str; 0]);
    insta::assert_snapshot!(print_module(&lowered.module));
}

#[test]
fn a_type_of_a_function_has_its_number_in_the_function() {
    insta::assert_snapshot!(gir("package main

type T struct{ a int }

func (T) f() int {
	type T struct{ b bool }
	{
		type T struct{ c float64 }
		x := T{1.5}
		_ = x
	}
	y := T{true}
	_ = y
	return 0
}

func g() {
	type T struct{ d int8 }
	var z T
	_ = z
	var anonymous struct {
		t T
		n int
	}
	_ = anonymous
}
"));
}

#[test]
fn a_go_file_has_nil_pointers_and_embedded_structs() {
    let text = "package main

type Point struct{ x int }

func (p Point) get() int { return p.x }

func (p *Point) set(x int) { p.x = x }

type Line struct {
	Point
	end *Point
}

func read(p *Point, q *int, l Line, m *Line) int {
	*q = p.x
	l.set(l.x)
	m.set(m.get() + l.end.get())
	return *q
}
";
    let (lowered, messages) = lowered_with(&[("a.go", text)], usize::MAX);
    assert_eq!(messages, [] as [&str; 0]);
    insta::assert_snapshot!(print_module(&lowered.module));
}

#[test]
fn strings_are_joined_compared_and_cut_by_the_runtime_and_by_their_parts() {
    insta::assert_snapshot!(gir("package main

func ops(a, b string, i, j int) (string, bool, byte) {
	joined := a + b
	joined += a
	same := a == b
	same = a != b
	same = a < b
	same = a >= b
	return joined[i:j], same, a[i]
}

func ends(s string, r rune, u uint64) (string, string, string) {
	return s[:2], s[1:], string(r) + string(u)
}
"));
}

#[test]
fn a_range_loop_counts_walks_an_array_or_decodes_a_string() {
    insta::assert_snapshot!(gir("package main

func count(n int8) int8 {
	var total int8
	for i := range n {
		total += i
	}
	return total
}

func walk(cells [3]int, names [2]string, p *[3]int) int {
	total := 0
	for i, cell := range cells {
		total += i * cell
	}
	for i := range cells {
		total += i
	}
	for _, name := range names {
		total += len(name)
	}
	for i, cell := range p {
		total += i - cell
	}
	return total
}

func runes(s string) int {
	var at int
	var last rune
	for at, last = range s {
		if last == 'x' {
			continue
		}
	}
	for range s {
		at++
	}
	return at + int(last)
}
"));
}

#[test]
fn a_slice_is_made_grown_and_copied_by_the_runtime() {
    insta::assert_snapshot!(gir("package main

func build(n int) []int {
	xs := make([]int, n, n+2)
	xs[0] = 7
	xs = append(xs, 1, 2)
	ys := []int{3, 1: 4}
	xs = append(xs, ys...)
	return xs
}

func owner(s []int, p *[]int) int {
	t := s
	t[0] = len(s) + cap(t)
	copy(p, s)
	clear(t)
	if s == nil {
		return 0
	}
	return t[0]
}
"));
}

#[test]
fn part_of_a_slice_is_a_copy_the_slice_itself_or_a_view() {
    insta::assert_snapshot!(gir("package main

func parts(s []int, a [4]int, i, j, k int) int {
	copied := s[i:j]
	s = s[i:j]
	view := &s[1:]
	three := s[i:j:k]
	array := a[:j]
	return len(copied) + len(view) + len(three) + len(array)
}

func bytes(text string, raw []byte) (string, []byte) {
	return string(raw), []byte(text)
}

func walk(names []string) int {
	total := 0
	for i, name := range names {
		total += i + len(name)
	}
	return total
}
"));
}

#[test]
fn a_value_that_holds_slices_is_copied_by_the_glue_of_its_type() {
    insta::assert_snapshot!(gir("package main

type Table struct {
	name string
	rows []int
	seen map[string][]int
}

type Tree struct {
	kids []Tree
	last Option[[2][]int]
}

func copies(t Table, grid [][]int, tree Tree) int {
	u := t
	inner := grid
	part := grid[1:]
	grown := append(inner, part...)
	twig := tree
	for _, row := range grid {
		grown = append(grown, row)
	}
	return len(u.rows) + len(grown) + len(twig.kids) + copy(&inner, grown)
}
"));
}

#[test]
fn a_map_is_kept_by_the_runtime_and_copied_for_a_new_owner() {
    insta::assert_snapshot!(gir("package main

type point struct{ x, y int32 }

func made(n int) map[string]int {
	ages := make(map[string]int, n)
	ages[\"ann\"] = n
	ages[\"ann\"] += 2
	ages[\"bob\"]++
	age, ok := ages[\"cat\"]
	if ok {
		delete(ages, \"cat\")
	}
	return map[string]int{\"one\": age, \"all\": len(ages)}
}

func owner(m map[point]float64, p *map[point]float64) float64 {
	n := m
	n[point{1, 2}] = p[point{3, 4}]
	p[point{5, 6}] = 1.5
	clear(n)
	if m == nil {
		return 0
	}
	return n[point{}]
}
"));
}

#[test]
fn a_range_over_a_map_ends_its_walk_on_every_way_out() {
    insta::assert_snapshot!(gir("package main

func find(m map[int]string, wanted string) int {
	for key, value := range m {
		if value == wanted {
			return key
		}
		if key < 0 {
			break
		}
	}
	return -1
}

func pairs(a *map[int]bool, b *map[int]bool) int {
	both := 0
outer:
	for x := range a {
		for y := range b {
			if x == y {
				both++
				continue outer
			}
		}
	}
	return both
}
"));
}

#[test]
fn a_range_variable_with_an_address_names_the_element() {
    insta::assert_snapshot!(gir("package main

func twice(nums []int, grid [2]int, rows [][]int, hits map[string]int) int {
	for _, &n := range nums {
		n *= 2
	}
	for i, &cell := range grid {
		cell = i
	}
	for _, &row := range rows {
		row = append(row, len(row))
	}
	for key, &hit := range hits {
		hit += len(key)
	}
	return grid[1]
}

func runes(text string, points []rune) ([]rune, string) {
	return []rune(text), string(points)
}
"));
}

#[test]
fn a_result_that_points_to_a_slice_has_a_place_in_the_caller() {
    insta::assert_snapshot!(gir("package main

func tail(xs *[]int) *[]int {
	return &xs[1:]
}

func twice(xs *[]int) *[]int {
	return tail(tail(xs))
}

func both(xs *[]int, n int) (*[]int, int, *[]int) {
	if n < 0 {
		return xs, 0, tail(xs)
	}
	return &xs[:n], n, &xs[n:]
}

func all(xs *[]int) (*[]int, int, *[]int) {
	return both(xs, len(xs))
}
"));
}

#[test]
fn a_value_ends_with_its_owner_on_every_way_out() {
    insta::assert_snapshot!(gir("package main

type box struct{ items []int }

func build(n int) []int { return make([]int, n) }

func two(n int) ([]int, box) { return build(n), box{build(n)} }

func owners(b box, n int) []int {
	kept := b.items
	b.items = build(n)
	build(n)
	_, other := two(n)
	if len(build(n)) > n {
		return kept
	}
	for _, v := range build(n) {
		if v > n {
			break
		}
	}
	kept, other.items = other.items, kept
	return nil
}
"));
}

#[test]
fn structs_and_arrays_are_compared_and_hashed_by_the_glue_of_their_types() {
    insta::assert_snapshot!(gir("package main

type point struct{ x, y int32 }

type named struct {
	name string
	at   point
	_    int
	tags [2]string
	rate float32
}

func same(a, b named, grid [2][3]int16) bool {
	if a == b {
		return grid != [2][3]int16{}
	}
	switch a.at {
	case point{1, 2}, b.at:
		return true
	}
	return false
}

func keys(a named) int {
	seen := map[named]int{a: 1}
	seen[a]++
	loose := map[[2]float64]bool{}
	return len(seen) + len(loose)
}
"));
}

/// For each function of the first file, in the order of the source:
/// its name and which of its parameters it only reads.
fn lent(text: &str) -> Vec<String> {
    checked(&[("a.gopp", text)], |checked, messages| {
        assert_eq!(messages, [] as [&str; 0]);
        let shared = Shared::new(checked, TargetLayout::new(64));
        let named = shared.jobs.iter().map(|job| {
            let flags: Vec<&str> = (job.facts.lent.iter())
                .map(|&lent| if lent { "lent" } else { "own" })
                .collect();
            let name = gopp_mangle::demangle(job.symbol.as_bytes(), 64).unwrap();
            format!("{name}: {}", flags.join(" "))
        });
        named.collect()
    })
}

#[test]
fn a_parameter_that_is_only_read_is_lent() {
    let facts = lent(
        "package main

type box struct{ items []int }

func (b box) size() int   { return len(b.items) }
func (b *box) add(n int)  { b.items = append(b.items, n) }
func (b box) grown() box  { b.add(1); return b }

func reads(xs []int, m map[string]int, s string, b box, _ []int) int {
	kept := xs
	for _, x := range xs {
		kept = append(kept, x+m[s])
	}
	return len(kept) + b.size() + reads(b.items, m, s[1:], b, kept)
}

func writes(xs []int, m map[string]int, b box, s string) {
	xs[0] = 1
	m[\"k\"]++
	b.items = nil
	s += \"x\"
	println(len(xs), len(m))
}

func more(a, b, c, d, e []int, f map[int]int, g box) []int {
	p := &a
	_ = p
	for _, &x := range b {
		x++
	}
	clear(c)
	copy(&d, e)
	delete(f, 1)
	g.add(2)
	println(len(c), len(f))
	return e
}

func later(xs []int, ys []int) int {
	defer println(len(xs))
	return len(ys)
}

func swaps(a, b []int) int {
	a, b = b, a
	return len(a)
}
",
    );
    assert_eq!(
        facts,
        [
            "main.box.size: lent",
            "main.box.add: own lent",
            "main.box.grown: own",
            "main.reads: lent lent lent lent lent",
            "main.writes: own own own own",
            "main.more: own own own own own own own",
            "main.later: own own",
            "main.swaps: own own",
        ]
    );
}

#[test]
fn an_argument_is_lent_or_given_as_its_parameter_asks() {
    insta::assert_snapshot!(gir("package main

var shared []int

func total(xs []int, name string) int {
	return len(xs) + len(name)
}

func grown(xs []int) []int {
	return append(xs, 1)
}

func calls(xs []int, name string) int {
	local := []int{1}
	n := total(xs, name) + total(local, name[1:]) + total(shared, \"lit\")
	return n + len(grown(xs)) + total(grown(local), name)
}
"));
}

#[test]
fn the_last_use_of_a_variable_gives_its_value_and_makes_no_copy() {
    let text = "package main

var kept [][]int

func keep(xs []int) {
	xs[0] = 1
	kept = append(kept, xs)
}

func uses(c bool) int {
	a := []int{1}
	keep(a)
	keep(a)
	b := []int{2}
	if c {
		keep(b)
	}
	return len(b)
}

func taken(c bool) int {
	a := []int{1}
	keep(move(a))
	a = []int{2}
	p := &a
	keep(a)
	return len(p)
}
";
    // A copy where the variable is read again: the first `keep(a)` and
    // `keep(b)` of `uses`. And where its address is taken: in `taken`.
    let copies = |text: &str, name: &str| {
        let (lowered, messages) = lowered_with(&[(name, text)], usize::MAX);
        assert_eq!(messages, [] as [&str; 0]);
        let module = print_module(&lowered.module);
        module.matches("call const fn @_gopp_slice_copy").count()
    };
    assert_eq!(copies(text, "a.gopp"), 3);
    // A Go file has no `move` and takes addresses as Go does.
    let plain = text.replace("move(a)", "a").replace("len(p)", "len(*p)");
    assert_eq!(copies(&plain, "a.go"), 4);
    insta::assert_snapshot!(gir(text));
}

#[test]
fn a_type_with_a_drop_method_ends_by_it_and_is_copied_by_its_clone_method() {
    let text = "package main

type File struct {
	fd   int
	name string
}

func (f *File) drop() {}

type Handle struct{ id int }

func (h *Handle) drop() {}

func (h Handle) Clone() Handle { return Handle{id: h.id} }

func use(h Handle) int {
	f := File{fd: 1, name: \"n\"}
	g := h
	return f.fd + g.id + h.id
}
";
    let calls = |module: &str| {
        let count = |method: &str| {
            module
                .matches(&format!("call const fn @_GPM{method}"))
                .count()
        };
        [count("4drop"), count("5Clone")]
    };
    let module = gir(text);
    // The drop glue of each type calls its `drop`, and the clone glue
    // of `Handle` its `Clone`.
    assert_eq!(calls(&module), [2, 1]);
    insta::assert_snapshot!(module);
    // In a Go file the two are methods like any other: nothing calls
    // them.
    let (lowered, messages) = lowered_with(&[("a.go", text)], usize::MAX);
    assert_eq!(messages, [] as [&str; 0]);
    assert_eq!(calls(&print_module(&lowered.module)), [0, 0]);
}

#[test]
fn drop_and_clone_methods_that_are_not_supported_yet_say_so() {
    let (_, messages) = lowered_with(
        &[(
            "a.gopp",
            "package main

type Fd int

func (f *Fd) drop() {}

type Buf struct{ data []int }

func (b *Buf) drop() {}

func (b Buf) Clone() Buf {
	b.data = nil
	return Buf{}
}

type File struct{ fd int }

func (f *File) drop() {}

func main() {
	var f Fd
	_ = f
	a := Buf{}
	b := a
	_, _ = a, b
	m := map[string]File{}
	_ = m
}
",
        )],
        usize::MAX,
    );
    assert_eq!(
        messages,
        [
            "a.gopp:5:14: not supported yet: a `drop` method of a type that is no struct",
            "a.gopp:21:6: not supported yet: a `drop` method of a type that is no struct",
            "a.gopp:22:6: not supported yet: a `drop` method of a type that is no struct",
            "a.gopp:24:7: not supported yet: a `Clone` method that does more than read its \
             receiver",
            "a.gopp:26:7: not supported yet: a map whose values cannot be copied",
            "a.gopp:26:2: not supported yet: a map whose values cannot be copied",
        ]
    );
}

#[test]
fn places_that_exchange_their_values_copy_and_end_nothing() {
    let text = "package main

func two(xs [][]int, i, j int) [][]int {
	xs[i], xs[j] = xs[j], xs[i]
	return xs
}

func locals(a, b, c []int) int {
	a, b, c = b, c, a
	return len(a) + len(b) + len(c)
}

func three(xs [][]int, i, j, k int) [][]int {
	xs[i], xs[j], xs[k] = xs[j], xs[k], xs[i]
	return xs
}
";
    // How often the statement of the line copies a slice and ends one.
    let counts = |module: &str, line: u32| {
        let at = format!(":{line}:2");
        let count = |call: &str| {
            let called = format!("call const fn @{call}");
            let of_line = |text: &&str| text.contains(&called) && text.contains(&at);
            module.lines().filter(of_line).count()
        };
        [count("_gopp_slice_copy"), count("_GPTd")]
    };
    let module = gir(text);
    // Two places, and variables that are not one another: the bytes
    // change places. Three elements of which two may be one: each is
    // copied, and what the places had ends.
    assert_eq!(counts(&module, 4), [0, 0]);
    assert_eq!(counts(&module, 9), [0, 0]);
    assert_eq!(counts(&module, 14), [3, 3]);
    // A Go file has no exchange: every value is copied.
    let (lowered, messages) = lowered_with(&[("a.go", text)], usize::MAX);
    assert_eq!(messages, [] as [&str; 0]);
    let module = print_module(&lowered.module);
    assert_eq!(counts(&module, 4), [2, 2]);
    assert_eq!(counts(&module, 14), [3, 3]);
}

#[test]
fn a_first_value_of_a_package_variable_is_reported_until_it_is_run() {
    let (_, messages) = lowered_with(
        &[(
            "a.gopp",
            "package main\n\nvar count = 5\n\nvar name string\n\nfunc main() {}\n",
        )],
        usize::MAX,
    );
    assert_eq!(
        messages,
        ["a.gopp:3:13: not supported yet: a first value of a variable of the package"]
    );
}

#[test]
fn what_is_appended_from_a_value_of_no_known_type_is_no_fault() {
    let (_, messages) = lowered_with(
        &[(
            "a.go",
            "package main

import \"other\"

func f(names []string) []string {
	return append(names, other.More()...)
}
",
        )],
        usize::MAX,
    );
    assert_eq!(
        messages,
        [
            "a.go:6:23: not supported yet: a call of what is not known",
            "a.go:6:9: not supported yet: the end of a value whose type is not known",
        ]
    );
}

#[test]
fn values_that_cannot_be_compared_yet_say_so() {
    let (_, messages) = lowered_with(
        &[(
            "a.go",
            "package main

type boxed struct {
	n int
	v any
}

func f(a, b boxed) int {
	m := map[boxed]int{}
	if a == b {
		return len(m)
	}
	return 0
}
",
        )],
        usize::MAX,
    );
    assert_eq!(
        messages,
        [
            "a.go:9:7: not supported yet: a comparison of interface values",
            "a.go:9:2: not supported yet: a comparison of interface values",
            "a.go:10:5: not supported yet: a comparison of interface values",
        ]
    );
}

#[test]
fn print_gives_each_value_to_the_runtime_after_all_are_worked_out() {
    insta::assert_snapshot!(gir("package main

func next(n int) int { return n + 1 }

func show(small int8, big uint64, ratio float32, ok bool, name string) {
	print(small, big, ratio)
	println(name, ok, next(1), 2.5, 'x', next(2))
	println()
}
"));
}

#[test]
fn a_pointer_of_a_go_file_is_printed_as_its_address() {
    let (lowered, messages) = lowered_with(
        &[(
            "a.go",
            "package main\n\nfunc show(p *int, q *[4]bool) { println(p, q) }\n",
        )],
        usize::MAX,
    );
    assert_eq!(messages, [] as [&str; 0]);
    let text = print_module(&lowered.module);
    assert_eq!(
        text.matches("call const fn @_gopp_print_pointer(").count(),
        2,
        "{text}"
    );
    // What has no kind that `print` writes is said, and the rest is
    // printed.
    let (lowered, messages) = lowered_with(
        &[(
            "a.go",
            "package main\n\nfunc show(pair [2]int) { println(1, pair, 2) }\n",
        )],
        usize::MAX,
    );
    assert_eq!(
        messages,
        ["a.go:3:37: not supported yet: `print` of a value of this type"]
    );
    let text = print_module(&lowered.module);
    assert_eq!(text.matches("@_gopp_print_int(").count(), 2, "{text}");
}

#[test]
fn what_is_not_supported_is_reported_once_and_has_a_zero() {
    let text = "package main

type Shape interface{ Area() float64 }

func f(xs []int, s Shape, name string) (int, string) {
	sum := 0
	for _, x := range xs {
		sum += x
	}
	g := func() int { return sum }
	println(sum)
	var any Shape = s
	_ = any
	area := s.Area() + 1
	_ = area
	defer g()
	return g() + len(xs) + xs[0], name + \"!\"
}
";
    let (lowered, messages) = lowered_with(&[("a.gopp", text)], usize::MAX);
    insta::assert_snapshot!(format!(
        "{}\n{}",
        messages.join("\n"),
        print_module(&lowered.module)
    ));
}

/// A package of `count` functions that call one another, with types of
/// their own, strings, a global, and one thing that is not supported.
fn many_functions(count: usize) -> String {
    let mut text = "package main\n\nvar total int\n\ntype Pair struct{ a, b int }\n".to_owned();
    for at in 0..count {
        let next = (at + 7) % count;
        text += &format!(
            "
func f{at}(n int, p Pair) (int, string) {{
	type Local struct {{
		v    int{bits}
		rows []int
	}}
	l := Local{{v: {at} % 100}}
	both := struct {{
		l Local
		p *Pair
	}}{{l, &p}}
	if n > {at} && p != (Pair{{n, {at}}}) {{
		m, _ := f{next}(n-1, Pair{{n, int(l.v)}})
		total += m
	}}
	for i := 0; i < n; i++ {{
		total += both.p.a * i
	}}
	println(total, \"f{at}\")
	defer println(total)
	return total + p.b, \"f{at}\"
}}
",
            bits = [8, 16, 32, 64][at % 4],
        );
    }
    text
}

#[test]
fn the_module_does_not_depend_on_the_number_of_threads() {
    let text = many_functions(60);
    let made_by = |threads: usize, source_each: usize| {
        let pool = rayon::ThreadPoolBuilder::new()
            .num_threads(threads)
            .build()
            .unwrap();
        pool.install(|| {
            let (lowered, messages) = lowered_with(&[("a.gopp", &text)], source_each);
            (print_module(&lowered.module), messages)
        })
    };
    let alone = made_by(1, usize::MAX);
    assert_eq!(alone.1.len(), 60, "{:#?}", alone.1);
    assert!(alone.0.contains("_GPTig0_5LocalF4mainE3f59"));
    for threads in [2, 5, 16] {
        let together = made_by(threads, 256);
        assert!(together == alone, "{threads} threads");
    }
}

#[test]
fn two_types_of_one_name_in_two_functions_are_two_types() {
    let text = gir("package main

func f() int8 {
	type T struct{ v int8 }
	var s struct{ t T }
	return s.t.v
}

func g() float64 {
	type T struct{ v float64 }
	var s struct{ t T }
	return s.t.v
}
");
    assert!(
        text.contains("let _1: struct{t _GPTig0_1TF4mainE1f} \"s\""),
        "{text}"
    );
    assert!(
        text.contains("let _1: struct{t _GPTig0_1TF4mainE1g} \"s\""),
        "{text}"
    );
}

#[test]
fn every_sample_of_the_specification_is_lowered_or_says_what_is_not() {
    let mut faults = Vec::new();
    for path in gopp_testutil::samples::files() {
        let text = std::fs::read_to_string(&path).unwrap();
        let name = path.to_str().unwrap();
        let (_, messages) = lowered_with(&[(name, &text)], usize::MAX);
        let unexpected = |message: &String| !message.contains("not supported yet: ");
        faults.extend(messages.into_iter().filter(unexpected));
    }
    assert_eq!(faults, [] as [&str; 0]);
}

fn steps_of(text: &str) -> u64 {
    let (lowered, messages) = lowered_with(&[("a.gopp", text)], usize::MAX);
    assert_eq!(messages, [] as [&str; 0]);
    lowered.steps
}

/// One function whose body is `body`.
fn function(body: &str) -> String {
    format!(
        "package main\n\nfunc g() int {{ return 1 }}\n\nfunc f(x int, ok bool) int {{\n{body}\treturn x\n}}\n"
    )
}

#[test]
fn the_steps_grow_with_the_source() {
    use gopp_testutil::stress;
    let sizes = [100, 200, 400, 800];
    assert_near_linear(&sizes, |n| steps_of(&stress::deep_expr(n)));
    assert_near_linear(&sizes, |n| steps_of(&stress::wide_block(n)));
    assert_near_linear(&sizes, |n| steps_of(&stress::long_chain(n)));
    assert_near_linear(&sizes, |n| steps_of(&stress::diamonds(n)));
    // Calls on the right, each of which asks whether what is left of
    // it must be held.
    assert_near_linear(&sizes, |n| {
        let right = format!("\tx = {}g(){}\n", "x + (".repeat(n), ")".repeat(n));
        steps_of(&function(&right))
    });
    assert_near_linear(&sizes, |n| {
        steps_of(&function(&format!("\tx = g(){}\n", " + x + g()".repeat(n))))
    });
    // Blocks in blocks, each with a variable that a jump out leaves.
    assert_near_linear(&sizes, |n| {
        let open: String = (0..n)
            .map(|at| format!("\tfor i{at} := 0; i{at} < x; i{at}++ {{\n\t\tif ok && i{at} > 3 {{\n\t\t\tbreak\n\t\t}}\n"))
            .collect();
        steps_of(&function(&format!("{open}{}", "\t}\n".repeat(n))))
    });
    assert_near_linear(&sizes, |n| {
        let cases: String = (0..n)
            .map(|at| format!("\tcase {at}:\n\t\tx += {at}\n"))
            .collect();
        steps_of(&function(&format!("\tswitch x {{\n{cases}\t}}\n")))
    });
    assert_near_linear(&sizes, |n| {
        let conditions = "ok && x > 1 || ".repeat(n);
        steps_of(&function(&format!(
            "\tif {conditions}ok {{\n\t\tx++\n\t}}\n"
        )))
    });
    assert_near_linear(&sizes, |n| {
        let labels: String = (0..n)
            .map(|at| format!("l{at}:\n\tif x > {at} {{\n\t\tgoto l{at}\n\t}}\n"))
            .collect();
        steps_of(&function(&labels))
    });
    assert_near_linear(&[50, 100, 200, 400], |n| {
        steps_of(&many_functions(n).replace("\tdefer println(total)\n", ""))
    });
}

#[test]
fn the_steps_grow_with_ranges_slices_maps_and_strings() {
    let sizes = [100, 200, 400, 800];
    // Range loops in range loops, over a number, an array and a string.
    assert_near_linear(&sizes, |n| {
        let open: String = (0..n)
            .map(|at| match at % 3 {
                0 => format!("\tfor a{at} := range x {{\n\t\tx += a{at}\n"),
                1 => format!(
                    "\tfor a{at}, b{at} := range [3]int{{1, 2, 3}} {{\n\t\tx += a{at} * b{at}\n"
                ),
                _ => format!(
                    "\tfor a{at}, b{at} := range \"text\" {{\n\t\tx += a{at} + int(b{at})\n"
                ),
            })
            .collect();
        steps_of(&function(&format!("{open}{}", "\t}\n".repeat(n))))
    });
    // Slices that are grown, cut and copied one after the other.
    assert_near_linear(&sizes, |n| {
        let grown: String = (0..n)
            .map(|at| format!("\txs = append(xs, x, {at})\n\tys := xs[1:]\n\txs = xs[:len(ys)]\n\tx += ys[0]\n")
                .replace("ys", &format!("y{at}")))
            .collect();
        steps_of(&function(&format!("\tvar xs []int\n{grown}")))
    });
    // Functions that pass the place of a view on to the next, and make
    // views in it.
    assert_near_linear(&sizes, |n| {
        let chain: String = (1..n)
            .map(|at| {
                format!(
                    "func v{at}(xs *[]int, n int) (*[]int, *[]int) {{\n\tif n > {at} {{\n\t\t\
                     return v{}(xs, n)\n\t}}\n\treturn &xs[:n], &xs[n:]\n}}\n\n",
                    at - 1
                )
            })
            .collect();
        steps_of(&format!(
            "package main\n\nfunc v0(xs *[]int, n int) (*[]int, *[]int) {{\n\t\
             return xs, &xs[n:]\n}}\n\n{chain}"
        ))
    });
    // Loops whose variable is the element itself, over a slice and a
    // map.
    assert_near_linear(&sizes, |n| {
        let loops: String = (0..n)
            .map(|at| match at % 2 {
                0 => format!("\tfor _, &a{at} := range xs {{\n\t\ta{at} += x\n\t}}\n"),
                _ => format!("\tfor _, &a{at} := range m {{\n\t\ta{at}++\n\t}}\n"),
            })
            .collect();
        steps_of(&function(&format!(
            "\txs := []int{{x}}\n\tm := map[int]int{{1: x}}\n{loops}"
        )))
    });
    // Range loops over maps in range loops over maps, each of which a
    // `break` leaves.
    assert_near_linear(&sizes, |n| {
        let open: String = (0..n)
            .map(|at| format!("\tfor a{at}, b{at} := range m {{\n\t\tif a{at} > b{at} {{\n\t\t\tbreak\n\t\t}}\n"))
            .collect();
        steps_of(&function(&format!(
            "\tm := map[int]int{{1: x}}\n{open}{}",
            "\t}\n".repeat(n)
        )))
    });
    // Entries of a map that are written, read and deleted one after the
    // other.
    assert_near_linear(&sizes, |n| {
        let used: String = (0..n)
            .map(|at| format!("\tm[x] = {at}\n\tm[{at}] += x\n\tv{at}, ok{at} := m[x]\n\tif ok{at} {{\n\t\tdelete(m, v{at})\n\t}}\n"))
            .collect();
        steps_of(&function(&format!(
            "\tm := map[int]int{{1: x}}\n{used}\tx = len(m)\n"
        )))
    });
    // Strings that are joined, cut and compared in one expression.
    assert_near_linear(&sizes, |n| {
        let joined = " + s[1:2] + \"a\"".repeat(n);
        steps_of(&function(&format!(
            "\ts := \"text\"\n\tif s{joined} < s {{\n\t\tx++\n\t}}\n"
        )))
    });
}

#[test]
fn the_steps_grow_with_the_glue_of_types() {
    let sizes = [100, 200, 400, 800];
    // Values in values: slices of slices, a struct of many slices, and
    // many types that each hold the one before, all copied by glue.
    assert_near_linear(&sizes, |n| {
        let deep = format!("{}int", "[]".repeat(n));
        steps_of(&format!(
            "package main\n\nfunc f(xs {deep}) int {{\n\tys := xs\n\treturn len(ys)\n}}\n"
        ))
    });
    assert_near_linear(&sizes, |n| {
        let fields: String = (0..n).map(|at| format!("\tf{at} []int\n")).collect();
        steps_of(&format!(
            "package main\n\ntype Wide struct {{\n{fields}}}\n\n\
             func f(w Wide) int {{\n\tv := w\n\treturn len(v.f0)\n}}\n"
        ))
    });
    assert_near_linear(&sizes, |n| {
        let chain: String = (1..n)
            .map(|at| {
                format!(
                    "type T{at} struct {{\n\tinner []T{}\n\tm map[int]T{}\n}}\n\n",
                    at - 1,
                    at - 1
                )
            })
            .collect();
        steps_of(&format!(
            "package main\n\ntype T0 struct{{ rows []int }}\n\n{chain}\
             func f(t T{}) int {{\n\tu := t\n\treturn len(u.inner)\n}}\n",
            n - 1
        ))
    });
    // Many types with a `drop` and a `Clone` method that each hold the
    // one before.
    assert_near_linear(&sizes, |n| {
        let chain: String = (1..n)
            .map(|at| {
                format!(
                    "type T{at} struct {{\n\tinner T{}\n\trows []int\n}}\n\n\
                     func (t *T{at}) drop() {{}}\n\nfunc (t T{at}) Clone() T{at} {{ return \
                     T{at}{{}} }}\n\n",
                    at - 1
                )
            })
            .collect();
        steps_of(&format!(
            "package main\n\ntype T0 struct{{ rows []int }}\n\n{chain}\
             func f(t T{}) int {{\n\tu := t\n\treturn len(u.rows) + len(t.rows)\n}}\n",
            n - 1
        ))
    });
    // Values that end: a slice in each of many blocks in blocks that a
    // `break` leaves, a literal of many slices, and a call that is given
    // many of them.
    assert_near_linear(&sizes, |n| {
        let open: String = (0..n)
            .map(|at| format!("\tfor {{\n\t\ts{at} := make([]int, x)\n\t\tif len(s{at}) > {at} {{\n\t\t\tbreak\n\t\t}}\n"))
            .collect();
        steps_of(&function(&format!("{open}{}", "\t}\n".repeat(n))))
    });
    assert_near_linear(&sizes, |n| {
        let rows = "{1}, ".repeat(n);
        steps_of(&function(&format!(
            "\tgrid := [][]int{{{rows}}}\n\tx += len(grid) + len([][]int{{{rows}}})\n"
        )))
    });
    assert_near_linear(&sizes, |n| {
        let params: String = (0..n).map(|at| format!("p{at} []int, ")).collect();
        let args = "make([]int, 1), ".repeat(n);
        steps_of(&format!(
            "package main\n\nfunc many({params}) int {{ return len(p0) }}\n\n\
             func f() int {{\n\treturn many({args})\n}}\n"
        ))
    });
}

#[test]
fn the_steps_grow_with_values_compared_by_glue() {
    let sizes = [100, 200, 400, 800];
    // Values that are compared and hashed by glue: a chain of types that
    // each hold the one before, a struct of many strings, and arrays in
    // arrays.
    assert_near_linear(&sizes, |n| {
        let chain: String = (1..n)
            .map(|at| {
                format!(
                    "type T{at} struct {{\n\tname string\n\tinner T{}\n}}\n\n",
                    at - 1
                )
            })
            .collect();
        steps_of(&format!(
            "package main\n\ntype T0 struct{{ f float64 }}\n\n{chain}\
             func f(a, b T{last}) int {{\n\tm := map[T{last}]int{{a: 1}}\n\t\
             if a == b {{\n\t\treturn 1\n\t}}\n\treturn len(m)\n}}\n",
            last = n - 1
        ))
    });
    assert_near_linear(&sizes, |n| {
        let fields: String = (0..n).map(|at| format!("\tf{at} string\n")).collect();
        steps_of(&format!(
            "package main\n\ntype Wide struct {{\n{fields}}}\n\n\
             func f(a, b Wide) bool {{\n\tm := map[Wide]bool{{}}\n\treturn a == b || m[a]\n}}\n"
        ))
    });
    assert_near_linear(&sizes, |n| {
        let deep = format!("{}string", "[1]".repeat(n));
        steps_of(&format!(
            "package main\n\nfunc f(a, b *{deep}) bool {{\n\treturn *a == *b\n}}\n"
        ))
    });
}

/// Every directory of Go's standard library whose files check as one
/// package, with nothing known of what they import, is lowered: much is
/// not supported and many types are not known, and still no function
/// fails to lower and no module breaks a rule. Needs `go` on `PATH`.
#[test]
#[ignore = "long: lowers Go's standard library"]
fn the_standard_library_of_go_is_lowered_without_a_fault() {
    use rayon::prelude::*;
    let goroot = std::process::Command::new("go")
        .args(["env", "GOROOT"])
        .output()
        .unwrap();
    let root =
        std::path::PathBuf::from(String::from_utf8(goroot.stdout).unwrap().trim()).join("src");
    let mut dirs = vec![root];
    let mut packages: Vec<Vec<std::path::PathBuf>> = Vec::new();
    while let Some(dir) = dirs.pop() {
        let mut files = Vec::new();
        for entry in std::fs::read_dir(&dir).unwrap() {
            let path = entry.unwrap().path();
            let name = path.file_name().unwrap().to_str().unwrap().to_owned();
            if path.is_dir() && name != "testdata" && name != "vendor" {
                dirs.push(path);
            } else if name.ends_with(".go") && !name.ends_with("_test.go") {
                files.push(path);
            }
        }
        files.sort();
        if !files.is_empty() {
            packages.push(files);
        }
    }
    packages.sort();
    if let Ok(only) = std::env::var("ONLY") {
        packages.retain(|files| files[0].to_str().unwrap().contains(&only));
    }
    let lowered: Vec<(usize, Vec<String>)> = packages
        .par_iter()
        .map(|files| {
            let texts: Vec<(String, String)> = files
                .iter()
                .filter_map(|path| {
                    Some((
                        path.to_str()?.to_owned(),
                        std::fs::read_to_string(path).ok()?,
                    ))
                })
                .collect();
            let texts: Vec<(&str, &str)> = texts
                .iter()
                .map(|(name, text)| (&**name, &**text))
                .collect();
            checked(&texts, |checked, messages| {
                if !messages.is_empty() {
                    return (0, Vec::new());
                }
                let (module, reported, _) =
                    lower_counted(checked, TargetLayout::new(64), usize::MAX);
                let faults = reported.iter();
                let faults =
                    faults.filter(|diagnostic| diagnostic.code != gopp_diag::Code::GirUnsupported);
                let faults =
                    faults.map(|diagnostic| format!("{}: {}", texts[0].0, diagnostic.message));
                (module.funcs.len(), faults.collect())
            })
        })
        .collect();
    let functions: usize = lowered.iter().map(|(functions, _)| functions).sum();
    let packages = lowered
        .iter()
        .filter(|(functions, _)| *functions > 0)
        .count();
    let faults: Vec<&String> = lowered.iter().flat_map(|(_, faults)| faults).collect();
    println!("{packages} packages, {functions} functions");
    assert!(
        faults.is_empty(),
        "{} faults:\n{:#?}",
        faults.len(),
        &faults[..faults.len().min(40)]
    );
    assert!(packages > 50, "{packages} packages");
}
