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
        aborts: false,
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

/// The module of a file of the package `main` where a panic ends the
/// program at once (`-panic=abort`), as text.
fn gir_aborting(file: &str, text: &str) -> String {
    checked(&[(file, text)], |checked, messages| {
        assert_eq!(messages, [] as [&str; 0]);
        let aborting = Checked {
            aborts: true,
            ..*checked
        };
        let (module, reported, _) = lower_counted(&aborting, TargetLayout::new(64), usize::MAX);
        assert!(reported.is_empty(), "{file}");
        print_module(&module)
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
/// The text of a module or of a function without the blocks that
/// unwinding runs: what the function does when no panic leaves it.
fn without_cleanup(text: &str) -> String {
    let mut kept = String::new();
    let mut cleanup = false;
    for line in text.lines() {
        if line.starts_with("  bb") || !line.starts_with("  ") {
            cleanup = line.ends_with(": [cleanup]");
        }
        if !cleanup {
            kept.push_str(line);
            kept.push('\n');
        }
    }
    kept
}

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
/// its name and which of its parameters it only reads, or only calls
/// methods through.
fn lent(text: &str) -> Vec<String> {
    checked(&[("a.gopp", text)], |checked, messages| {
        assert_eq!(messages, [] as [&str; 0]);
        let shared = Shared::new(checked, TargetLayout::new(64));
        let named = shared.jobs.iter().map(|job| {
            let flags = job.facts.lent.iter().zip(&job.facts.worked);
            let flags: Vec<&str> = flags
                .map(|flags| match flags {
                    (true, true) => "worked",
                    (true, false) => "lent",
                    (false, _) => "own",
                })
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
    let module = without_cleanup(&gir(text));
    // Two places, and variables that are not one another: the bytes
    // change places. Three elements of which two may be one: each is
    // copied, and what the places had ends.
    assert_eq!(counts(&module, 4), [0, 0]);
    assert_eq!(counts(&module, 9), [0, 0]);
    assert_eq!(counts(&module, 14), [3, 3]);
    // A Go file has no exchange: every value is copied.
    let (lowered, messages) = lowered_with(&[("a.go", text)], usize::MAX);
    assert_eq!(messages, [] as [&str; 0]);
    let module = without_cleanup(&print_module(&lowered.module));
    assert_eq!(counts(&module, 4), [2, 2]);
    assert_eq!(counts(&module, 14), [3, 3]);
}

#[test]
fn a_string_that_is_added_to_grows_where_it_is_in_both_dialects() {
    let text = "package main

type log struct{ text string }

func add(l *log, s string, more string) string {
	names := []string{\"a\"}
	s += more
	l.text += more
	names[0] += more
	println(names[0])
	return s
}
";
    for name in ["a.gopp", "a.go"] {
        let (lowered, messages) = lowered_with(&[(name, text)], usize::MAX);
        assert_eq!(messages, [] as [&str; 0]);
        let module = print_module(&lowered.module);
        let calls = |called: &str| module.matches(&format!("call const fn @{called}")).count();
        // No new string is made, and none ends, for any of the three.
        assert_eq!(calls("_gopp_string_append"), 3, "{name}");
        assert_eq!(calls("_gopp_string_concat"), 0, "{name}");
    }
}

#[test]
fn a_deferred_call_waits_in_slots_of_the_function_and_runs_where_it_is_left() {
    let text = "package main

func say(s string, n int) {}

func f(n int, s string) int {
	defer say(s, n)
	if n > 0 {
		defer say(\"big\", n)
		return n
	}
	return 0
}
";
    insta::assert_snapshot!(gir(text));
    // Both ways out go through one block that makes the two calls.
    for name in ["a.gopp", "a.go"] {
        let (lowered, messages) = lowered_with(&[(name, text)], usize::MAX);
        assert_eq!(messages, [] as [&str; 0]);
        let module = print_module(&lowered.module);
        assert_eq!(
            module.matches("call const fn @_GPF4mainE3say").count(),
            2,
            "{name}"
        );
    }
}

#[test]
fn a_panic_gives_its_value_to_the_runtime_and_a_function_goes_on_where_it_is_stopped() {
    let text = "package main

type Failure struct{ code int }

func (f Failure) Error() string { return \"failure\" }

func fail(n int) {
	if n > 1 {
		panic(Failure{n})
	}
	panic(\"small\")
}

func guarded(n int) (code int, text string) {
	defer func() {
		switch r := recover().(type) {
		case Failure:
			code = r.code
		case string:
			text = r
		}
	}()
	fail(n)
	return 0, \"none\"
}

func plain(n int) int {
	defer recover()
	defer func() { recover() }()
	return n
}

func main() {
	code, text := guarded(2)
	println(code, text, plain(1))
}
";
    for file in ["a.gopp", "a.go"] {
        let (lowered, messages) = lowered_with(&[(file, text)], usize::MAX);
        assert_eq!(messages, [] as [&str; 0], "{file}");
        let module = print_module(&lowered.module);
        let body = |symbol: &str| {
            let start = module.find(&format!("\nfn {symbol}(")).unwrap();
            &module[start..][..module[start..].find("\n}\n").unwrap()]
        };
        let calls = |body: &str, name: &str| body.matches(&format!("@{name}(")).count();
        // Each panic gives the runtime its value in an interface value
        // and the keys of the two methods that say a text, and nothing
        // follows it.
        let fail = body("_GPF4mainE4fail");
        assert_eq!(calls(fail, "_gopp_panic"), 2, "{file}: {fail}");
        assert_eq!(calls(fail, "_gopp_iface_box"), 1, "{file}: {fail}");
        assert_eq!(fail.matches("unreachable").count(), 2, "{file}: {fail}");
        assert!(module.contains("\"Error||"), "{file}: {module}");
        assert!(module.contains("\"String||"), "{file}: {module}");
        // A function with `defer` statements is known to the runtime
        // from its start to each of its ends, where the function that
        // makes its deferred calls is called.
        for (name, ends) in [("7guarded", 1), ("5plain", 1)] {
            let function = body(&format!("_GPF4mainE{name}"));
            assert_eq!(
                calls(function, "_gopp_defer_enter"),
                1,
                "{file}: {function}"
            );
            assert_eq!(
                calls(function, "_gopp_defer_pop"),
                ends,
                "{file}: {function}"
            );
            let deferred = format!("_GPC0_F4mainE{name}");
            assert_eq!(calls(function, &deferred), ends, "{file}: {function}");
            assert!(
                function.contains(&format!("const fn @{deferred},")),
                "{file}"
            );
            assert!(module.contains(&format!("\nfn {deferred}(_1: *u8) -> () [c, internal]")));
        }
        // After a stopped panic `plain` returns what its result had:
        // the result without a name has a variable too.
        let plain = body("_GPF4mainE5plain");
        assert_eq!(plain.matches("\n    return").count(), 2, "{file}: {plain}");
        // `recover` asks the runtime, in the literal that calls it.
        let literal = body("_GPC1_F4mainE7guarded");
        assert_eq!(calls(literal, "_gopp_recover"), 1, "{file}: {literal}");
        if file == "a.gopp" {
            insta::assert_snapshot!(module);
        }
    }
}

#[test]
fn what_a_function_owns_ends_where_a_panic_leaves_it() {
    let text = "package main

func make(n int) string {
	s := \"a\"
	for i := 0; i < n; i++ {
		s += \"b\"
	}
	return s
}

func join(a, b string) string { return a + b }

func owner(name string, n int) int {
	first := make(n)
	total := len(join(make(1), make(2)))
	if n > 1 {
		second := make(n)
		total += len(join(first, second))
	}
	total += len(make(3))
	return total + len(name)
}

func guarded(n int) (out string) {
	defer func() { recover() }()
	held := make(n)
	out = join(held, make(1))
	return out
}

func main() {
	println(owner(make(1), 2), guarded(2))
}
";
    for file in ["a.gopp", "a.go"] {
        let (lowered, messages) = lowered_with(&[(file, text)], usize::MAX);
        assert_eq!(messages, [] as [&str; 0], "{file}");
        let module = print_module(&lowered.module);
        let body = |symbol: &str| {
            let start = module.find(&format!("\nfn {symbol}(")).unwrap();
            &module[start..][..module[start..].find("\n}\n").unwrap()]
        };
        let owner = body("_GPF4mainE5owner");
        let cleanup: String = owner
            .split("\n  bb")
            .filter(|block| {
                block
                    .lines()
                    .next()
                    .is_some_and(|first| first.ends_with("[cleanup]"))
            })
            .collect();
        // Each place that owns a string has one block that ends it,
        // whatever the number of the calls that a panic may leave:
        // `first`, `second`, and the two results of `make` that wait
        // for `join`. The function only reads `name`, which its caller
        // owns.
        let ends = cleanup.matches("@_gopp_string_release(").count();
        assert_eq!(ends, 4, "{file}: {owner}");
        // A call in such a block ends the program if it panics, and
        // the last of the blocks lets the panic go on.
        assert_eq!(cleanup.matches("unwind: cleanup").count(), 0, "{file}");
        assert_eq!(cleanup.matches("unwind: terminate").count(), ends, "{file}");
        assert_eq!(cleanup.matches("resume").count(), 1, "{file}: {owner}");
        // Every call of the function itself names the block of what
        // is owned where it is made; nothing is where the first is.
        let normal = without_cleanup(owner);
        let calls: Vec<&str> = normal
            .lines()
            .filter(|line| line.contains("@_GPF4mainE"))
            .collect();
        assert_eq!(calls.len(), 7, "{file}: {normal}");
        assert!(calls[0].contains("unwind: continue"), "{file}: {normal}");
        for line in &calls[1..] {
            assert!(line.contains("unwind: cleanup bb"), "{file}: {line}");
        }
        // A function with a frame asks the runtime last whether it
        // goes on, and only then ends its result.
        let guarded = body("_GPF4mainE7guarded");
        let left = guarded.find("@_gopp_defer_left(").unwrap();
        assert_eq!(guarded.matches("@_gopp_defer_left(").count(), 1, "{file}");
        let after = &guarded[left..];
        let block = &after[..after.find("resume").unwrap()];
        assert_eq!(
            block.matches("@_gopp_string_release(").count(),
            1,
            "{file}: {guarded}"
        );
        if file == "a.gopp" {
            insta::assert_snapshot!(module);
        }
    }
}

#[test]
fn a_defer_statement_that_runs_again_makes_a_record_each_time() {
    let text = "package main

func say(s string, n int) {}

func loops(n int, s string) {
	defer say(s, 0)
	for i := 0; i < n; i++ {
		defer say(s, i)
		for j := 0; j < i; j++ {
			defer println(i, j)
		}
	}
	defer say(s, n)
	for range n {
		defer say(\"again\", n)
	}
}

func jumps(n int) {
again:
	defer say(\"jump\", n)
	n--
	if n > 0 {
		goto again
	}
	defer say(\"last\", n)
}
";
    for file in ["a.gopp", "a.go"] {
        let (lowered, messages) = lowered_with(&[(file, text)], usize::MAX);
        assert_eq!(messages, [] as [&str; 0], "{file}");
        let module = print_module(&lowered.module);
        let body = |symbol: &str| {
            let start = module.find(&format!("\nfn {symbol}(")).unwrap();
            without_cleanup(&module[start..][..module[start..].find("\n}\n").unwrap()])
        };
        let calls = |body: &str, name: &str| body.matches(&format!("@{name}(")).count();
        // The three statements in loops make a record in a cell each
        // time they run; the two others have a record in the frame.
        let loops = body("_GPF4mainE5loops");
        assert_eq!(calls(&loops, "_gopp_cell_new"), 3, "{file}: {loops}");
        // The function of the deferred calls goes through the list of
        // each of the two outermost loops, where the loop is among the
        // statements: it has a call for each statement, and lets the
        // cell of a record go after its call.
        let deferred = body("_GPC0_F4mainE5loops");
        assert_eq!(calls(&deferred, "_GPF4mainE3say"), 4, "{file}: {deferred}");
        assert_eq!(
            calls(&deferred, "_gopp_cell_release"),
            3,
            "{file}: {deferred}"
        );
        // The loop that comes last is first, then the statement before
        // it, then the other loop, then the first statement.
        let order: Vec<Option<usize>> = ["15:9", "13:8", "8:9", "6:8"]
            .iter()
            .map(|at| deferred.find(&format!("; {file}:{at}\n")))
            .collect();
        assert!(order.iter().all(Option::is_some), "{file}: {deferred}");
        assert!(order.is_sorted(), "{file}: {deferred}");
        // In a function with a label every statement may run again.
        let jumps = body("_GPF4mainE5jumps");
        assert_eq!(calls(&jumps, "_gopp_cell_new"), 2, "{file}: {jumps}");
        if file == "a.gopp" {
            insta::assert_snapshot!(module);
        }
    }
}

#[test]
fn a_variable_of_a_block_that_a_defer_statement_borrows_lives_as_long_as_the_function() {
    let text = "package main

type File struct{ fd int }

func (f *File) Close() {}

func (f File) Name() int { return f.fd }

func sync(f *File) {}

func read(n int) {
	first := File{fd: 0}
	defer first.Close()
	for i := 0; i < n; i++ {
		f := File{fd: i}
		defer f.Close()
		g := File{fd: -i}
		if i > 1 {
			defer sync(&g)
		}
		h := File{fd: 1}
		defer h.Name()
	}
}
";
    for file in ["a.gopp", "a.go"] {
        let (lowered, messages) = lowered_with(&[(file, text)], usize::MAX);
        assert_eq!(messages, [] as [&str; 0], "{file}");
        let module = print_module(&lowered.module);
        let start = module.find("\nfn _GPF4mainE4read(").unwrap();
        let read = without_cleanup(&module[start..][..module[start..].find("\n}\n").unwrap()]);
        let calls = |name: &str| read.matches(&format!("@{name}(")).count();
        // `f` and `g` each live in a cell, because a deferred call has
        // their address; the three statements of the loop make a
        // record in a cell each. `first` is a variable of the whole
        // function, and `h` is copied for a method that takes a value:
        // they are where they were.
        assert_eq!(calls("_gopp_cell_new"), 5, "{file}: {read}");
        // Each of the two statements that borrow says so when it runs,
        // and the function lets what was borrowed go where it is left,
        // after its deferred calls.
        assert_eq!(calls("_gopp_extend"), 2, "{file}: {read}");
        assert_eq!(calls("_gopp_extended_end"), 1, "{file}: {read}");
        let deferred = read.find("@_GPC0_F4mainE4read(").unwrap();
        let ended = read.find("@_gopp_extended_end(").unwrap();
        assert!(deferred < ended, "{file}: {read}");
        if file == "a.gopp" {
            insta::assert_snapshot!(module);
        }
    }
}

#[test]
fn a_deferred_built_in_function_keeps_what_its_statement_had() {
    // A Go++ function changes the map of its caller through a pointer;
    // a write to its own copy that nothing reads is refused.
    for (file, map) in [("a.gopp", "*map[string]int"), ("a.go", "map[string]int")] {
        let text = format!(
            "package main

func fail(m {map}, key string, reason string) {{
	defer recover()
	defer delete(m, key)
	for range 2 {{
		defer clear(m)
	}}
	defer panic(reason)
}}
"
        );
        let (lowered, messages) = lowered_with(&[(file, &text)], usize::MAX);
        assert_eq!(messages, [] as [&str; 0], "{file}");
        let module = print_module(&lowered.module);
        let body = |symbol: &str| {
            let start = module.find(&format!("\nfn {symbol}(")).unwrap();
            without_cleanup(&module[start..][..module[start..].find("\n}\n").unwrap()])
        };
        let calls = |body: &str, name: &str| body.matches(&format!("@{name}(")).count();
        // The function of the deferred calls panics, clears and
        // deletes; `recover` as the deferred call itself asks the
        // runtime nothing, since it stops no panic.
        let deferred = body("_GPC0_F4mainE4fail");
        assert_eq!(calls(&deferred, "_gopp_panic"), 1, "{file}: {deferred}");
        assert_eq!(calls(&deferred, "_gopp_map_clear"), 1, "{file}: {deferred}");
        assert_eq!(
            calls(&deferred, "_gopp_map_delete"),
            1,
            "{file}: {deferred}"
        );
        assert_eq!(calls(&deferred, "_gopp_recover"), 0, "{file}: {deferred}");
        // The statements keep the value of the panic in an interface
        // value, and the address of the place of the map: the function
        // itself does none of the three.
        let fail = body("_GPF4mainE4fail");
        assert_eq!(calls(&fail, "_gopp_iface_box"), 1, "{file}: {fail}");
        for name in ["_gopp_panic", "_gopp_map_clear", "_gopp_map_delete"] {
            assert_eq!(calls(&fail, name), 0, "{file}: {fail}");
        }
        if file == "a.gopp" {
            insta::assert_snapshot!(module);
        }
    }
}

#[test]
fn a_program_whose_panics_end_it_at_once_has_no_frames_and_no_cleanup_blocks() {
    let text = "package main

func make(n int) string {
	s := \"a\"
	for i := 0; i < n; i++ {
		s += \"b\"
	}
	return s
}

func guarded(n int) (out string) {
	defer func() { recover() }()
	held := make(n)
	if n > 2 {
		panic(held)
	}
	for i := 0; i < n; i++ {
		defer println(i)
	}
	return held + make(1)
}
";
    for file in ["a.gopp", "a.go"] {
        let module = gir_aborting(file, text);
        // No function tells the runtime of itself, and no block is
        // run by unwinding: a panic is the end.
        for name in ["_gopp_defer_enter", "_gopp_defer_pop", "_gopp_defer_left"] {
            assert!(!module.contains(name), "{file}: {name}\n{module}");
        }
        assert!(!module.contains("[cleanup]"), "{file}: {module}");
        assert!(!module.contains("unwind: cleanup"), "{file}: {module}");
        // The deferred calls are made where the function returns, and
        // a panic is given to the runtime as ever.
        let start = module.find("\nfn _GPF4mainE7guarded(").unwrap();
        let guarded = &module[start..][..module[start..].find("\n}\n").unwrap()];
        assert_eq!(
            guarded
                .matches("call const fn @_GPC0_F4mainE7guarded(")
                .count(),
            1
        );
        assert_eq!(
            guarded.matches("@_gopp_panic(").count(),
            1,
            "{file}: {guarded}"
        );
    }
}

#[test]
fn defer_statements_that_are_not_supported_yet_say_so() {
    let (_, messages) = lowered_with(
        &[(
            "a.gopp",
            "package main

type File struct{ fd int }

func (f *File) drop() {}

func (f *File) close() {}

func say(n int) {}

func loops(n int) {
	for i := 0; i < n; i++ {
		defer say(i)
	}
}

func labels(n int) {
again:
	defer say(n)
	if n > 0 {
		n--
		goto again
	}
}

func named() (n int) {
	defer say(n)
	return 1
}

func others(n int, xs []int) int {
	defer copy(&xs, xs)
	defer func() {}()
	if n > 0 {
		f := File{fd: n}
		defer f.close()
		defer say(n)
	}
	return len(xs)
}

func aliases(files []File) {
	for _, &f := range files {
		defer f.close()
	}
}

func main() {}
",
        )],
        usize::MAX,
    );
    // A variable of a block that a deferred call has the address of
    // lives as long as the function; another name of an element does
    // not.
    assert_eq!(
        messages,
        [
            "a.gopp:32:8: not supported yet: a defer statement of this built-in function or of a \
             conversion",
            "a.gopp:44:9: not supported yet: a deferred call that holds a variable of a block",
        ]
    );
}

#[test]
fn a_value_in_an_interface_is_in_its_word_or_in_a_box_of_its_own() {
    let text = "package main

type Point struct{ x, y int }

type Named struct {
	name string
	list []int
}

func box(n int, p Point, d Named) (any, any, any) {
	return n, p, d
}

func look(v any) int {
	if n, ok := v.(int); ok {
		return n
	}
	switch x := v.(type) {
	case Point:
		return x.x
	case Named, nil:
		return 0
	}
	return v.(Named).list[0]
}

func same(a, b any) bool {
	c := a
	return c == b && a != nil
}
";
    let module = gir(text);
    // Each type has one descriptor, with its glue where its bytes do
    // not say all: `Named` is copied, ended, compared and hashed by
    // glue, `Point` and `int` by their bytes.
    let said = |name: &str| module.matches(&format!("global const @{name}:")).count();
    assert_eq!(
        [
            said("_GPTii"),
            said("_GPTiN4mainE5Point"),
            said("_GPTiN4mainE5Named")
        ],
        [1, 1, 1]
    );
    let calls = |name: &str| module.matches(&format!("call const fn @{name}(")).count();
    // A box for the struct of two words and for the one with a slice;
    // the integer is in the word itself.
    assert_eq!(calls("_gopp_iface_box"), 2);
    // A copy for `c`, and one for the variable of the clause with two
    // cases.
    assert_eq!(calls("_gopp_iface_clone"), 2);
    assert_eq!(calls("_gopp_iface_eq"), 1);
    assert_eq!(calls("_gopp_panic_assert"), 1);
    insta::assert_snapshot!(module);
    // A Go file makes the same module but for the pointers that it
    // checks.
    let (lowered, messages) = lowered_with(&[("a.go", text)], usize::MAX);
    assert_eq!(messages, [] as [&str; 0]);
    let module = print_module(&lowered.module);
    assert_eq!(module.matches("call const fn @_gopp_iface_box(").count(), 2);
}

#[test]
fn a_method_of_an_interface_value_is_called_through_its_table() {
    let text = "package main

type Shape interface {
	Area() int
	Scale(by int, note string)
}

type Square struct{ side int }

func (s Square) Area() int { return s.side * s.side }

func (s *Square) Scale(by int, note string) { s.side *= by }

type Tags []string

func (t Tags) Area() int { return len(t) }

func (t *Tags) Scale(by int, note string) {
	t = append(t, note)
}

func use(s Shape, note string) int {
	s.Scale(2, note)
	return s.Area()
}

func main() {
	var s Shape = Square{2}
	t := Tags{\"a\"}
	var a any = s
	_, ok := a.(Square)
	println(use(s, \"n\"), use(t, \"m\"), ok)
}
";
    let module = gir(text);
    // Each type has one table for the interface, with the descriptor
    // of the type and one function for each method, in the order of
    // their names.
    let table = |of: &str| {
        let start = module.find(&format!("global const @_GPTa{of}")).unwrap();
        let lines: Vec<&str> = module[start..].lines().skip(1).take(3).collect();
        lines.join("|").replace("    const ", "")
    };
    assert_eq!(
        table("N4mainE6Square"),
        "global @_GPTiN4mainE6Square|fn @_GPTIM4AreaN4mainE6Square|fn @_GPTIM5ScaleN4mainE6Square"
    );
    assert_eq!(
        table("N4mainE4Tags"),
        "global @_GPTiN4mainE4Tags|fn @_GPTIM4AreaN4mainE4Tags|fn @_GPTIM5ScaleN4mainE4Tags"
    );
    // A call reads the code from the table and passes the address of
    // the second word; the value that is called on is no argument.
    assert_eq!(module.matches(" = call copy _").count(), 2);
    // `Tags.Scale` only reads its string: what is called for it ends
    // the string that the caller gave.
    let thunk = module.find("fn _GPTIM5ScaleN4mainE4Tags(").unwrap();
    let body = &module[thunk..][..module[thunk..].find("\n}\n").unwrap()];
    assert_eq!(body.matches("@_gopp_string_release").count(), 1);
    insta::assert_snapshot!(module);
    // In a Go file a value has the methods of its type only, so the
    // tables are those of the pointers.
    let go = text
        .replace(
            "var s Shape = Square{2}",
            "q := Square{2}\n\tvar s Shape = &q",
        )
        .replace("use(t, ", "use(&t, ")
        .replace("a.(Square)", "a.(*Square)")
        .replace("\tt = append(t, note)", "\t*t = append(*t, note)");
    let (lowered, messages) = lowered_with(&[("a.go", &go)], usize::MAX);
    assert_eq!(messages, [] as [&str; 0]);
    let module = print_module(&lowered.module);
    let thunks = |name: &str| module.matches(&format!("\nfn {name}(")).count();
    assert_eq!(
        [
            thunks("_GPTpM4AreaN4mainE6Square"),
            thunks("_GPTpM5ScaleN4mainE4Tags"),
            thunks("_GPTIM4AreaN4mainE6Square"),
        ],
        [1, 1, 0]
    );
}

#[test]
fn a_function_value_is_its_code_and_its_environment() {
    let text = "package main

type Op func(a int, note string) int

func plain(a int, note string) int { return a + len(note) }

func keeps(a int, note string) int {
	last = note
	return a
}

var last string

func apply(op Op, a int) int { return op(a, \"n\") }

func pick(first bool) Op {
	if first {
		return plain
	}
	return func(a int, note string) int {
		return func(b int) int { return b * 2 }(a)
	}
}

func main() {
	op := pick(true)
	other := op
	ops := []Op{op, keeps, func(a int, note string) int { return a }}
	println(apply(op, 1), apply(other, 2), ops[1](3, \"x\"), len(ops))
}
";
    let module = gir(text);
    let defined = |name: &str| module.matches(&format!("\nfn {name}(")).count();
    // The code of a literal is a function of its own, numbered in the
    // function that it is written in; the code of a function of the
    // package as a value is the function with an environment in front.
    assert_eq!(
        [
            defined("_GPC1_F4mainE4pick"),
            defined("_GPC1_C1_F4mainE4pick"),
            defined("_GPC1_F4mainE4main"),
            defined("_GPTEF4mainE5plain"),
            defined("_GPTEF4mainE5keeps"),
        ],
        [1, 1, 1, 1, 1]
    );
    let body = |name: &str| {
        let start = module.find(&format!("\nfn {name}(")).unwrap();
        &module[start..][..module[start..].find("\n}\n").unwrap()]
    };
    // A call through a value gives every argument away: `plain` only
    // reads its string, so its code as a value ends the string, and
    // that of `keeps`, which stores a copy, ends it too.
    let released = |name: &str| body(name).matches("@_gopp_string_release").count();
    assert_eq!(
        [
            released("_GPTEF4mainE5plain"),
            released("_GPTEF4mainE5keeps")
        ],
        [1, 1]
    );
    // The call in `apply` reads the code and passes the environment
    // first; the function is lent its parameter and ends nothing.
    let apply = body("_GPF4mainE5apply");
    assert_eq!(apply.matches(" = call copy _").count(), 1, "{apply}");
    assert_eq!(apply.matches("@_gopp_func_drop").count(), 0, "{apply}");
    // `other := op` is one more value that shares the environment.
    let main = body("_GPF4mainE4main");
    assert!(main.matches("@_gopp_func_clone").count() >= 1, "{main}");
    insta::assert_snapshot!(module);
    // A Go file checks that the value is not nil before it calls.
    let (lowered, messages) = lowered_with(&[("a.go", text)], usize::MAX);
    assert_eq!(messages, [] as [&str; 0]);
    let module = print_module(&lowered.module);
    let start = module.find("\nfn _GPF4mainE5apply(").unwrap();
    let apply = &module[start..][..module[start..].find("\n}\n").unwrap()];
    assert_eq!(apply.matches("assert(").count(), 1, "{apply}");
}

#[test]
fn a_variable_that_a_function_literal_uses_lives_in_a_cell() {
    let text = "package main

func counter(start int, name string) (func() int, func() string) {
	n := start
	inc := func() int {
		n++
		return n
	}
	label := func() string { return name }
	return inc, label
}

func each(values []int) []func() int {
	var all []func() int
	for i := 0; i < len(values); i++ {
		all = append(all, func() int { return values[i] })
	}
	return all
}

func main() {
	inc, label := counter(1, \"n\")
	println(inc(), label(), len(each([]int{1, 2})))
}
";
    let module = gir(text);
    let body = |name: &str| {
        let start = module.find(&format!("\nfn {name}(")).unwrap();
        &module[start..][..module[start..].find("\n}\n").unwrap()]
    };
    let calls = |body: &str, name: &str| body.matches(&format!("@{name}(")).count();
    // `n` and the parameter `name` each live in a cell; each literal
    // has an environment with the one cell that it uses, which it
    // shares, and the function lets its two cells go where it ends.
    let counter = &without_cleanup(body("_GPF4mainE7counter"));
    assert_eq!(
        [
            calls(counter, "_gopp_cell_new"),
            calls(counter, "_gopp_env_new"),
            calls(counter, "_gopp_cell_retain"),
            calls(counter, "_gopp_cell_release"),
        ],
        [2, 2, 2, 2],
        "{counter}"
    );
    // The code of a literal reads its variable through the cell of its
    // environment and makes no cell.
    let inc = body("_GPC1_F4mainE7counter");
    assert_eq!(calls(inc, "_gopp_cell_new"), 0, "{inc}");
    // `values` has one cell; `i` has one before the loop and a new one
    // for each next pass, and the old one is let go each time.
    let each = &without_cleanup(body("_GPF4mainE4each"));
    assert_eq!(
        [calls(each, "_gopp_cell_new"), calls(each, "_gopp_env_new")],
        [3, 1],
        "{each}"
    );
    insta::assert_snapshot!(module);
    let (lowered, messages) = lowered_with(&[("a.go", text)], usize::MAX);
    assert_eq!(messages, [] as [&str; 0]);
    let module = print_module(&lowered.module);
    assert_eq!(module.matches("@_gopp_cell_new(").count(), 5);
}

#[test]
fn a_method_value_has_its_receiver_in_its_environment() {
    let text = "package main

type List struct{ items []string }

func (l List) Len() int { return len(l.items) }

func (l *List) Add(s string) { l.items = append(l.items, s) }

type Sized interface{ Len() int }

func bound(s Sized) (func() int, func(string), func() int) {
	l := List{}
	return l.Len, l.Add, s.Len
}

func plain() (func(List) int, func(*List, string), func(*List) int) {
	return List.Len, (*List).Add, (*List).Len
}

func main() {
	size, add, other := bound(List{})
	add(\"a\")
	a, b, c := plain()
	l := List{}
	b(&l, \"x\")
	println(size(), other(), a(l), c(&l))
}
";
    let module = gir(text);
    let defined = |name: &str| module.matches(&format!("\nfn {name}(")).count();
    // One code for each method value and each method expression; the
    // expression with an address of a method that takes a value has a
    // name of its own.
    assert_eq!(
        [
            defined("_GPTVM3LenN4mainE4List"),
            defined("_GPTVM3AddN4mainE4List"),
            defined("_GPTVM3LenN4mainE5Sized"),
            defined("_GPTEM3LenN4mainE4List"),
            defined("_GPTEM3AddN4mainE4List"),
            defined("_GPTETpM3LenN4mainE4List"),
        ],
        [1, 1, 1, 1, 1, 1]
    );
    let body = |name: &str| {
        let start = module.find(&format!("\nfn {name}(")).unwrap();
        &module[start..][..module[start..].find("\n}\n").unwrap()]
    };
    let calls = |body: &str, name: &str| body.matches(&format!("@{name}(")).count();
    // `l` lives in a cell, because `l.Add` has its address. Each value
    // has an environment with the cell of its receiver, and that of
    // `l.Add` shares the cell of `l` too.
    let bound = body("_GPF4mainE5bound");
    assert_eq!(
        [
            calls(bound, "_gopp_cell_new"),
            calls(bound, "_gopp_env_new"),
            calls(bound, "_gopp_cell_retain"),
        ],
        [4, 3, 1],
        "{bound}"
    );
    // The code of the value of an interface method calls through the
    // table of the interface value in its cell.
    let dynamic = body("_GPTVM3LenN4mainE5Sized");
    assert_eq!(dynamic.matches(" = call copy _").count(), 1, "{dynamic}");
    insta::assert_snapshot!(module);
    let (lowered, messages) = lowered_with(&[("a.go", text)], usize::MAX);
    assert_eq!(messages, [] as [&str; 0]);
    assert!(print_module(&lowered.module).contains("\nfn _GPTVM3AddN4mainE4List("));
}

#[test]
fn a_deferred_function_value_is_kept_and_called_with_its_environment() {
    let text = "package main

func work(n int) (out int) {
	defer func() { out += n }()
	step := func(by int) { out *= by }
	defer step(2)
	return n
}

func main() {
	println(work(3))
}
";
    for file in ["a.go", "a.gopp"] {
        let (lowered, messages) = lowered_with(&[(file, text)], usize::MAX);
        assert_eq!(messages, [] as [&str; 0], "{file}");
        let module = print_module(&lowered.module);
        let body = |symbol: &str| {
            let start = module.find(&format!("\nfn {symbol}(")).unwrap();
            &module[start..][..module[start..].find("\n}\n").unwrap()]
        };
        let work = &without_cleanup(body("_GPF4mainE4work"));
        let calls = |name: &str| work.matches(&format!("@{name}(")).count();
        // Each statement keeps its value: the literal as it is made,
        // and one more that shares the environment of `step`. Each is
        // called by its code, and ends then, in the function that
        // makes the deferred calls of `work`.
        let deferred = &without_cleanup(body("_GPC0_F4mainE4work"));
        assert_eq!(
            deferred.matches(" = call copy _").count(),
            2,
            "{file}: {deferred}"
        );
        assert_eq!(
            deferred.matches("@_gopp_func_drop(").count(),
            2,
            "{file}: {deferred}"
        );
        assert_eq!(work.matches(" = call copy _").count(), 0, "{file}: {work}");
        assert_eq!(calls("_gopp_func_clone"), 1, "{file}: {work}");
        assert_eq!(calls("_gopp_func_drop"), 1, "{file}: {work}");
        // `out` lives in a cell, because `step` uses it and is kept by
        // a `defer` statement. The literal that the first statement
        // calls uses variables of the whole function, so its
        // environment is in the frame, and `n` stays where it is.
        assert_eq!(calls("_gopp_cell_new"), 1, "{file}: {work}");
        assert_eq!(calls("_gopp_env_new"), 1, "{file}: {work}");
    }
}

#[test]
fn a_literal_that_cannot_outlive_its_variables_has_its_environment_in_the_frame() {
    let text = "package main

func apply(values []int, f func(int)) {
	for _, v := range values {
		f(v)
	}
}

func keep(f func(int)) func(int) { return f }

func direct(n int) int {
	sum := n
	func() { sum *= 2 }()
	return sum
}

func deferred(n int) (out int) {
	defer func() { out += n }()
	return 1
}

func inner(flag bool) (out int) {
	if flag {
		five := 5
		defer func() { out += five }()
	}
	return 1
}

func lent(values []int) int {
	sum := 0
	apply(values, func(v int) { sum += v })
	return sum
}

func helper(values []int) int {
	sum := 0
	add := func(v int) { sum += v }
	for _, v := range values {
		add(v)
	}
	return sum
}

func escapes(values []int) int {
	sum := 0
	add := func(v int) { sum += v }
	keep(add)(1)
	apply(values, add)
	return sum
}

func passed(values []int) int {
	sum := 0
	apply(values, keep(func(v int) { sum += v }))
	return sum
}

func main() {
	xs := []int{1, 2, 3}
	println(direct(2), deferred(3), inner(true), lent(xs), helper(xs), escapes(xs), passed(xs))
}
";
    for file in ["a.go", "a.gopp"] {
        let (lowered, messages) = lowered_with(&[(file, text)], usize::MAX);
        assert_eq!(messages, [] as [&str; 0], "{file}");
        let module = print_module(&lowered.module);
        // The environments and the cells that each function makes on
        // the heap.
        let made = |name: &str| {
            let start = module.find(&format!("\nfn _GPF4mainE{name}(")).unwrap();
            let body = &module[start..][..module[start..].find("\n}\n").unwrap()];
            let calls = |name: &str| body.matches(&format!("@{name}(")).count();
            [calls("_gopp_env_new"), calls("_gopp_cell_new")]
        };
        // Called where it is written, called by a `defer` statement
        // with variables of the whole function, lent to a function
        // that only calls it, held by a variable that is only called:
        // nothing is made.
        for name in ["6direct", "8deferred", "4lent", "6helper"] {
            assert_eq!(made(name), [0, 0], "{file}: {name}");
        }
        // A deferred literal that uses a variable of a block, a
        // variable that is passed on, and a literal that is given to a
        // function which keeps it: an environment, and a cell for each
        // variable.
        assert_eq!(made("5inner"), [1, 2], "{file}");
        assert_eq!(made("7escapes"), [1, 1], "{file}");
        assert_eq!(made("6passed"), [1, 1], "{file}");
        if file == "a.gopp" {
            insta::assert_snapshot!(module);
        }
    }
}

#[test]
fn function_values_that_are_not_supported_yet_say_so() {
    let text = "package main

type T struct{ n int }

func (t T) get() int { return t.n }

func many(more ...int) int { return len(more) }

type Getter interface{ get() int }

func main() {
	xs := []int{1}
	var kept []func(int) int
	for _, &x := range xs {
		kept = append(kept, func(a int) int { return a + x })
	}
	get := Getter.get
	all := many
	go func() {}()
	defer func() {}()
	println(get(T{2}), all(1), len(kept))
}
";
    // Another name of an element is no variable of its own.
    let (_, messages) = lowered_with(&[("a.gopp", text)], usize::MAX);
    let messages: Vec<&str> = messages
        .iter()
        .map(|message| message.split_once(": ").map_or("", |(_, what)| what))
        .collect();
    assert_eq!(
        messages,
        [
            "not supported yet: a function literal that uses a variable that has no cell",
            "not supported yet: a method of an interface or of an embedded type as a function",
            "not supported yet: a call of a function with `...`",
            "not supported yet: a go statement",
            "not supported yet: a call of a function with `...`",
        ]
    );
}

#[test]
fn an_interface_parameter_is_lent_its_argument_without_a_box() {
    let text = "package main

type Shape interface{ Area() int }

type Rect struct{ w, h int }

func (r Rect) Area() int { return r.w * r.h }

func (r *Rect) grow() { r.w++ }

var last any

func read(v any) bool { return v == nil }

func area(s Shape) int { return s.Area() }

func keep(v any) { last = v }

func returned(s Shape) Shape { return s }

func assigned(s Shape, other Shape) int {
	s = other
	return s.Area()
}

func field(h struct{ s Shape }) int { return h.s.Area() }

func passes(s Shape) int { return area(s) }

func twice(s Shape) int { return area(s) + area(s) }

func main() {
	r := Rect{2, 3}
	list := []int{1}
	var s Shape = r
	println(read(r), read(list), area(r), area(Rect{1, 1}), area(s), passes(r), twice(r))
	keep(r)
	keep(list)
	println(r.w, len(list))
}
";
    assert_eq!(
        lent(text),
        [
            "main.Rect.Area: lent",
            "main.Rect.grow: own",
            "main.read: lent",
            "main.area: worked",
            "main.keep: lent",
            "main.returned: own",
            "main.assigned: own lent",
            "main.field: own",
            "main.passes: lent",
            "main.twice: lent",
            "main.main: ",
        ]
    );
    let module = gir(text);
    let main = module.find("fn _GPF4mainE4main(").unwrap();
    let main = &module[main..][..module[main..].find("\n}\n").unwrap()];
    // The one box of `main` is that of `var s Shape = r`: every
    // argument is lent where it is, also to `keep`, which only reads
    // its parameter to store a copy of it.
    let boxes = |text: &str| text.matches("@_gopp_iface_box(").count();
    assert_eq!(boxes(main), 1);
    // The copy that `keep` stores is made where it is stored.
    let keep = module.find("fn _GPF4mainE4keep(").unwrap();
    let keep = &module[keep..][..module[keep..].find("\n}\n").unwrap()];
    assert_eq!(keep.matches("@_gopp_iface_clone(").count(), 1);
    // A function that only passes its parameter on reads it: what it
    // passes on to a function that calls methods is a copy each time.
    let clones = |name: &str| {
        let start = module.find(&format!("fn _GPF4mainE{name}(")).unwrap();
        let body = &module[start..][..module[start..].find("\n}\n").unwrap()];
        body.matches("@_gopp_iface_clone(").count()
    };
    assert_eq!([clones("6passes"), clones("5twice")], [1, 2]);
    insta::assert_snapshot!(module);
    // A Go file lends the same way.
    let (lowered, messages) = lowered_with(&[("a.go", text)], usize::MAX);
    assert_eq!(messages, [] as [&str; 0]);
    let module = print_module(&lowered.module);
    let main = module.find("fn _GPF4mainE4main(").unwrap();
    let main = &module[main..][..module[main..].find("\n}\n").unwrap()];
    assert_eq!(boxes(main), 1);
}

#[test]
fn a_value_is_asked_for_the_methods_of_an_interface_while_the_program_runs() {
    let text = "package main

type Shape interface{ Area() int }

type Named interface{ Name() string }

type NamedShape interface {
	Area() int
	Name() string
}

type Square struct{ side int }

func (s Square) Area() int { return s.side }

func (s Square) Name() string { return \"square\" }

func (s *Square) grow() { s.side++ }

func ask(v any, s Shape, both NamedShape) (Named, Shape, int) {
	n := v.(Named)
	if shape, ok := v.(Shape); ok {
		return n, shape, 1
	}
	switch x := s.(type) {
	case NamedShape:
		return x, x, 2
	case Named, nil:
		return n, s, 3
	}
	return both, both, 4
}

func two() (NamedShape, Square) { return Square{1}, Square{2} }

func main() {
	var n Named = Square{0}
	var s Shape = Square{0}
	n, s = two()
	_, _, k := ask(Square{3}, s, Square{4})
	println(n.Name(), k)
}
";
    let module = gir(text);
    // The methods of the type, by their keys in the order of the keys;
    // the one with a pointer receiver is a method of a value in a Go++
    // file.
    let said = |symbol: &str| {
        let start = module.find(&format!("global const @{symbol}:")).unwrap();
        let lines = module[start..].lines().skip(1);
        let data: Vec<&str> = lines.take_while(|line| line.starts_with("    ")).collect();
        data.join("|").replace("    const ", "")
    };
    let keys = |symbol: &str| {
        let data = said(symbol);
        let keys: Vec<&str> = data
            .split('"')
            .skip(1)
            .step_by(2)
            .map(|key| key.split('|').next().unwrap_or(""))
            .collect();
        keys.join(" ")
    };
    assert_eq!(keys("_GPTmN4mainE6Square"), "Area Name grow");
    assert_eq!(
        keys("_GPTnN4mainE10NamedShape"),
        "main.NamedShape Area Name"
    );
    assert_eq!(keys("_GPTnN4mainE5Named"), "main.Named Name");
    assert!(said("_GPTiN4mainE6Square").contains("global @_GPTmN4mainE6Square|"));
    // Asked of the runtime: `v.(Named)`, `v.(Shape)`, two cases, four
    // results and one of the two values of `two`; the other is put
    // into a box.
    let calls = |name: &str| module.matches(&format!("call const fn @{name}(")).count();
    assert_eq!(calls("_gopp_iface_table"), 9);
    assert_eq!(calls("_gopp_panic_assert_iface"), 1);
    insta::assert_snapshot!(module);
    // In a Go file a value has no method that takes its address.
    let (lowered, messages) = lowered_with(&[("a.go", text)], usize::MAX);
    assert_eq!(messages, [] as [&str; 0]);
    let module = print_module(&lowered.module);
    assert!(module.contains("@_GPTIM4AreaN4mainE6Square"));
    assert!(!module.contains("@_GPTIM4growN4mainE6Square"));
}

#[test]
fn a_method_of_an_embedded_type_is_reached_by_the_fields_on_the_way() {
    let text = "package main

type Shape interface {
	Area() int
	Grow(by int)
}

type Base struct {
	w, h int
	tag  string
}

func (b Base) Area() int { return b.w * b.h }

func (b *Base) Grow(by int) { b.w += by }

type Box struct {
	*Base
	depth int
}

type Crate struct {
	Box
	label string
}

type Wrapped struct {
	Shape
	extra int
}

func main() {
	base := Base{1, 2, \"t\"}
	crate := Crate{Box{&base, 1}, \"c\"}
	var s Shape = crate
	s.Grow(2)
	var w Shape = Wrapped{s, 1}
	w.Grow(3)
	println(s.Area(), w.Area(), base.w, crate.Area(), crate.Box.Area())
}
";
    // The direct calls at the end read the part of `crate` that has
    // the method where it is.
    let (lowered, messages) = lowered_with(&[("a.go", text)], usize::MAX);
    assert_eq!(messages, [] as [&str; 0]);
    let module = print_module(&lowered.module);
    let body = |name: &str| {
        let start = module.find(&format!("\nfn {name}(")).unwrap();
        &module[start..][..module[start..].find("\n}\n").unwrap()]
    };
    // What the table of `Crate` holds for `Grow`: the box, the two
    // fields on the way, and the method of `Base` with the pointer
    // that `Box` embeds, which may be nil as for any call of it.
    let grow = body("_GPTIM4GrowN4mainE5Crate");
    assert!(
        grow.contains("@_GPM4GrowN4mainE4Base(copy (*_4).0.0, "),
        "{grow}"
    );
    assert_eq!(grow.matches("assert(").count(), 0, "{grow}");
    // For `Area` the value of `Base` is read through that pointer,
    // which is checked first.
    let area = body("_GPTIM4AreaN4mainE5Crate");
    assert!(area.contains("call const fn @_GPM4AreaN4mainE4Base("));
    assert_eq!(area.matches("assert(").count(), 1, "{area}");
    // What the table of `Wrapped` holds calls through the table of the
    // interface value in its field.
    let wrapped = body("_GPTIM4GrowN4mainE7Wrapped");
    assert_eq!(wrapped.matches(" = call copy _").count(), 1, "{wrapped}");
    insta::assert_snapshot!(format!("{area}\n}}\n{wrapped}\n}}"));
}

#[test]
fn interface_values_that_are_not_supported_yet_say_so() {
    let text = "package main

type Shape interface{ Area() float64 }

type Named interface {
	Shape
	Name() string
}

type Square struct{ side float64 }

func (s Square) Area() float64 { return s.side * s.side }

func (s Square) Name(more ...string) string { return \"square\" }

type Lister interface{ List(more ...string) int }

func f(v any, s Shape, n Named, l Lister) int {
	if _, ok := v.(Shape); ok {
		return 1
	}
	switch v.(type) {
	case Shape:
		return 2
	case complex128:
		return 3
	}
	var c any = 2i
	_ = c
	s = n
	c = Square{}
	return l.List()
}
";
    for file in ["a.go", "a.gopp"] {
        let (_, messages) = lowered_with(&[(file, text)], usize::MAX);
        let messages: Vec<&str> = messages
            .iter()
            .map(|message| message.split_once(": ").map_or("", |(_, what)| what))
            .collect();
        assert_eq!(
            messages,
            [
                "not supported yet: a comparison of values of this type",
                "not supported yet: a complex number",
                "not supported yet: a value in an interface whose type has a method that is \
                 not supported yet",
                "not supported yet: a call of a function with `...`",
            ],
            "{file}"
        );
    }
    // A struct without a name that has a method from a type that it
    // embeds, and a method that takes any number of arguments.
    let (_, messages) = lowered_with(
        &[(
            "a.go",
            "package main

type Shape interface{ Area() int }

type Base struct{ n int }

func (b Base) Area() int { return b.n }

type Wide struct {
	Base
	w int
}

type Lister interface{ List(more ...string) int }

type Names struct{}

func (n Names) List(more ...string) int { return len(more) }

func main() {
	var s Shape = struct{ Base }{}
	var l Lister = Names{}
	_, _ = s, l
}
",
        )],
        usize::MAX,
    );
    assert_eq!(
        messages,
        [
            "a.go:21:16: not supported yet: a value in an interface whose type has a method that \
             is not supported yet",
            "a.go:22:17: not supported yet: a value in an interface whose type has a method that \
             is not supported yet",
        ]
    );
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
	v complex128
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
            "a.go:9:7: not supported yet: a comparison of values of this type",
            "a.go:9:2: not supported yet: a comparison of values of this type",
            "a.go:10:5: not supported yet: a comparison of values of this type",
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
    let mut text = "package main\n\nvar total int\n\ntype Pair struct{ a, b int }\n\n\
                    type Sized interface{ Size() int }\n\n\
                    func (p Pair) Size() int { return p.a + p.b }\n"
        .to_owned();
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
	var held any = l
	if _, ok := held.(Local); ok {{
		held = p
	}}
	var sized Sized = p
	total += sized.Size()
	next := func(n int) int {{ return func(m int) int {{ return m + int(l.v) + {at} }}(n) }}
	total += next(n)
	go println(total)
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
    steps_of_file("a.gopp", text)
}

/// The steps that lowering the one file of a package takes.
fn steps_of_file(name: &str, text: &str) -> u64 {
    let (lowered, messages) = lowered_with(&[(name, text)], usize::MAX);
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
        steps_of(&many_functions(n).replace("\tgo println(total)\n", ""))
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
    // Many types in interface values, each with a descriptor, and a
    // type switch with a case for each.
    assert_near_linear(&sizes, |n| {
        let types: String = (0..n)
            .map(|at| format!("type T{at} struct {{\n\tn int\n\trows []int\n}}\n\n"))
            .collect();
        let values: String = (0..n)
            .map(|at| format!("\tall = append(all, T{at}{{}})\n"))
            .collect();
        let cases: String = (0..n)
            .map(|at| format!("\tcase T{at}:\n\t\treturn x.n + {at}\n"))
            .collect();
        steps_of(&format!(
            "package main\n\n{types}func f(all []any) []any {{\n{values}\treturn all\n}}\n\n\
             func g(v any) int {{\n\tswitch x := v.(type) {{\n{cases}\t}}\n\treturn 0\n}}\n"
        ))
    });
    // Many types with many methods in one interface, each called
    // through it.
    assert_near_linear(&sizes, |n| {
        let n = n / 10;
        let methods: String = (0..10).map(|at| format!("\tM{at}(x int) int\n")).collect();
        let types: String = (0..n)
            .map(|at| {
                let methods: String = (0..10)
                    .map(|m| format!("func (t T{at}) M{m}(x int) int {{ return x + {m} }}\n"))
                    .collect();
                format!("type T{at} struct{{ n int }}\n\n{methods}\n")
            })
            .collect();
        let values: String = (0..n)
            .map(|at| format!("\tall = append(all, T{at}{{}})\n"))
            .collect();
        let calls: String = (0..10)
            .map(|at| format!("\t\tsum += v.M{at}(sum)\n"))
            .collect();
        steps_of(&format!(
            "package main\n\ntype Wide interface {{\n{methods}}}\n\n{types}\
             func f(all []Wide) int {{\n{values}\tsum := 0\n\tfor _, v := range all {{\n{calls}\t}}\n\
             \treturn sum\n}}\n"
        ))
    });
    // Many interface types that each value is asked for, in a type
    // switch and one by one.
    assert_near_linear(&sizes, |n| {
        let n = n / 4;
        let ifaces: String = (0..n)
            .map(|at| format!("type I{at} interface{{ M{at}() int }}\n\n"))
            .collect();
        let methods: String = (0..n)
            .map(|at| format!("func (t T) M{at}() int {{ return {at} }}\n"))
            .collect();
        let cases: String = (0..n)
            .map(|at| format!("\tcase I{at}:\n\t\treturn x.M{at}()\n"))
            .collect();
        let asked: String = (0..n)
            .map(|at| format!("\tif i, ok := v.(I{at}); ok {{\n\t\tsum += i.M{at}()\n\t}}\n"))
            .collect();
        steps_of(&format!(
            "package main\n\n{ifaces}type T struct{{ n int }}\n\n{methods}\n\
             func f(v any) int {{\n\tswitch x := v.(type) {{\n{cases}\t}}\n\treturn 0\n}}\n\n\
             func g(v any) int {{\n\tsum := 0\n{asked}\treturn sum\n}}\n\n\
             func h() int {{ return f(T{{}}) + g(T{{}}) }}\n"
        ))
    });
    // Many types that embed one type, which embeds another with the
    // methods, in one interface. What a table holds for a method walks
    // one field for each level, so a chain of types that each embed
    // the one before would grow with the square of its length.
    assert_near_linear(&sizes, |n| {
        let n = n / 4;
        let types: String = (2..n)
            .map(|at| format!("type T{at} struct {{\n\tT1\n\tn{at} int\n}}\n\n"))
            .collect();
        let types = format!("type T1 struct {{\n\tT0\n\tn1 int\n}}\n\n{types}");
        let values: String = (0..n)
            .map(|at| format!("\tall = append(all, T{at}{{}})\n"))
            .collect();
        let text = format!(
            "package main\n\ntype Shape interface {{\n\tArea() int\n\tName() string\n}}\n\n\
             type T0 struct{{ n int }}\n\nfunc (t T0) Area() int {{ return t.n }}\n\n\
             func (t T0) Name() string {{ return \"t\" }}\n\n{types}\
             func f(all []Shape) []Shape {{\n{values}\treturn all\n}}\n"
        );
        // Only a Go file embeds a struct.
        steps_of_file("a.go", &text)
    });
    // Many function literals in one function, and literals in
    // literals, each called through its value.
    assert_near_linear(&sizes, |n| {
        let literals: String = (0..n)
            .map(|at| {
                format!("\tf{at} := func(a int) int {{ return a + {at} }}\n\tsum += f{at}(sum)\n")
            })
            .collect();
        let n = n / 4;
        let nested = format!(
            "return {}1{}",
            "func() int { return ".repeat(n),
            " }()".repeat(n)
        );
        steps_of(&format!(
            "package main\n\nfunc f() int {{\n\tsum := 0\n{literals}\treturn sum\n}}\n\n\
             func g() int {{\n\t{nested}\n}}\n"
        ))
    });
    // Many variables that one literal uses, many literals that use
    // one variable, and literals in literals that each use the
    // variable of the one around.
    assert_near_linear(&sizes, |n| {
        let vars: String = (0..n).map(|at| format!("\tv{at} := {at}\n")).collect();
        let sum: String = (0..n).map(|at| format!(" + v{at}")).collect();
        let literals: String = (0..n)
            .map(|at| format!("\tf{at} := func() int {{ return v0 + {at} }}\n\tsum += f{at}()\n"))
            .collect();
        let n = n / 4;
        let nested = format!(
            "return {}v{n}{}",
            (0..n)
                .map(|at| format!("func(v{} int) int {{ return v{at} + ", at + 1))
                .collect::<String>(),
            (0..n)
                .rev()
                .map(|at| format!(" }}({at})"))
                .collect::<String>()
        );
        steps_of(&format!(
            "package main\n\nfunc f() int {{\n{vars}\tall := func() int {{ return 0{sum} }}\n\
             \tsum := all()\n{literals}\treturn sum\n}}\n\n\
             func g(v0 int) int {{\n\t{nested}\n}}\n"
        ))
    });
    // Many literals that are framed, each with many variables, and
    // many that are lent to one function.
    assert_near_linear(&sizes, |n| {
        let n = n / 4;
        let vars: String = (0..n).map(|at| format!("\tv{at} := {at}\n")).collect();
        let sum: String = (0..n).map(|at| format!(" + v{at}")).collect();
        let direct: String = (0..n)
            .map(|at| format!("\tfunc() {{ v{at} += v0 }}()\n"))
            .collect();
        let lent: String = (0..n)
            .map(|at| format!("\teach(func(a int) {{ v{at} += a }})\n"))
            .collect();
        steps_of(&format!(
            "package main\n\nfunc each(f func(int)) {{ f(1) }}\n\n\
             func f() int {{\n{vars}\tfunc() {{ v0 += 0{sum} }}()\n{direct}{lent}\treturn v0\n}}\n"
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
    // Many `defer` statements and as many ways out of the function.
    assert_near_linear(&sizes, |n| {
        let body: String = (0..n)
            .map(|at| {
                format!(
                    "\tif x > {at} {{\n\t\tdefer keep(make([]int, x))\n\t}}\n\tif x == {at} \
                     {{\n\t\treturn\n\t}}\n"
                )
            })
            .collect();
        steps_of(&format!(
            "package main\n\nfunc keep(xs []int) {{}}\n\nfunc f(x int) {{\n{body}}}\n"
        ))
    });
    // Many `defer` statements in loops in loops, and many loops with
    // one each.
    assert_near_linear(&sizes, |n| {
        let open: String = (0..n)
            .map(|at| {
                format!(
                    "\tfor i{at} := 0; i{at} < x; i{at}++ {{\n\t\tdefer keep(make([]int, i{at}))\n"
                )
            })
            .collect();
        steps_of(&format!(
            "package main\n\nfunc keep(xs []int) {{}}\n\nfunc f(x int) {{\n{open}{}}}\n",
            "\t}\n".repeat(n)
        ))
    });
    assert_near_linear(&sizes, |n| {
        let loops: String = (0..n)
            .map(|at| {
                format!(
                    "\tfor i := 0; i < x; i++ {{\n\t\tdefer keep(make([]int, i + {at}))\n\t}}\n"
                )
            })
            .collect();
        steps_of(&format!(
            "package main\n\nfunc keep(xs []int) {{}}\n\nfunc f(x int) {{\n{loops}}}\n"
        ))
    });
    // Variables of blocks that `defer` statements borrow: in many
    // blocks of the function, and in blocks in blocks.
    assert_near_linear(&sizes, |n| {
        let blocks: String = (0..n)
            .map(|at| format!("\t{{\n\t\ts{at} := make([]int, x)\n\t\tdefer keep(&s{at})\n\t}}\n"))
            .collect();
        steps_of(&format!(
            "package main\n\nfunc keep(xs *[]int) {{}}\n\nfunc f(x int) {{\n{blocks}}}\n"
        ))
    });
    assert_near_linear(&sizes, |n| {
        let open: String = (0..n)
            .map(|at| {
                format!("\tif x > {at} {{\n\t\ts{at} := make([]int, x)\n\t\tdefer keep(&s{at})\n")
            })
            .collect();
        steps_of(&format!(
            "package main\n\nfunc keep(xs *[]int) {{}}\n\nfunc f(x int) {{\n{open}{}}}\n",
            "\t}\n".repeat(n)
        ))
    });
    // What unwinding ends: many variables that own, each followed by a
    // call; blocks in blocks, each with one and a call; one call with
    // many arguments that are owned while the next is worked out.
    assert_near_linear(&sizes, |n| {
        let body: String = (0..n)
            .map(|at| format!("\ts{at} := make([]int, x)\n\tx += g() + len(s{at})\n"))
            .collect();
        steps_of(&function(&body))
    });
    assert_near_linear(&sizes, |n| {
        let open: String = (0..n)
            .map(|at| {
                format!(
                    "\tif x > {at} {{\n\t\ts{at} := make([]int, x)\n\t\tx += g() + len(s{at})\n"
                )
            })
            .collect();
        steps_of(&function(&format!("{open}{}", "\t}\n".repeat(n))))
    });
    assert_near_linear(&sizes, |n| {
        let params: String = (0..n).map(|at| format!("p{at} []int, ")).collect();
        let args = "list(), ".repeat(n);
        steps_of(&format!(
            "package main\n\nfunc list() []int {{ return nil }}\n\n\
             func many({params}) int {{ return len(p0) }}\n\n\
             func f() int {{\n\treturn many({args})\n}}\n"
        ))
    });
    // Many panics and as many deferred calls that stop them, in one
    // function and in many.
    assert_near_linear(&sizes, |n| {
        let body: String = (0..n)
            .map(|at| {
                format!(
                    "\tdefer func() {{\n\t\tif recover() != nil {{\n\t\t\tx += {at}\n\t\t}}\n\t}}()\n\
                     \tif x == {at} {{\n\t\tpanic({at})\n\t}}\n"
                )
            })
            .collect();
        steps_of(&format!(
            "package main\n\nfunc f(x int) (n int) {{\n{body}\treturn x\n}}\n"
        ))
    });
    assert_near_linear(&sizes, |n| {
        let functions: String = (0..n)
            .map(|at| {
                format!(
                    "func f{at}(x int) (n int) {{\n\tdefer func() {{\n\t\tif recover() != nil \
                     {{\n\t\t\tn = {at}\n\t\t}}\n\t}}()\n\tif x > {at} {{\n\t\tpanic(\"f{at}\")\n\t}}\n\
                     \treturn x\n}}\n\n"
                )
            })
            .collect();
        steps_of(&format!("package main\n\n{functions}"))
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
