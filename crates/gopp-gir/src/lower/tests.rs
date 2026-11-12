use std::sync::Arc;

use gopp_diag::{Diagnostic, Sink};
use gopp_sema::types::Types;
use gopp_sema::{Package, PackageLoader, Unit, check};
use gopp_span::{Dialect, SourceFile, SourceMap};
use gopp_syntax::ast::File;
use gopp_syntax::parser::parse;
use gopp_target::TargetLayout;
use lasso::ThreadedRodeo;

use super::{Checked, lower_counted};
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
	type Local struct{{ v int{bits} }}
	l := Local{{{at} % 100}}
	both := struct {{
		l Local
		p *Pair
	}}{{l, &p}}
	if n > {at} {{
		m, _ := f{next}(n-1, Pair{{n, int(l.v)}})
		total += m
	}}
	for i := 0; i < n; i++ {{
		total += both.p.a * i
	}}
	println(total)
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
    assert!(alone.0.contains("main.f59.Local.1"));
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
        text.contains("let _1: struct{t main.f.T.1} \"s\""),
        "{text}"
    );
    assert!(
        text.contains("let _1: struct{t main.g.T.1} \"s\""),
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
        steps_of(&many_functions(n).replace("\tprintln(total)\n", ""))
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
