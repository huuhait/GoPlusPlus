use std::sync::Arc;

use gopp_diag::Sink;
use gopp_gir::{Checked, Module, lower_package};
use gopp_sema::types::Types;
use gopp_sema::{Package, PackageLoader, Unit, check};
use gopp_span::{SourceFile, SourceMap};
use gopp_syntax::ast::File;
use gopp_syntax::parser::parse;
use gopp_target::TargetSpec;
use lasso::ThreadedRodeo;

use rayon::prelude::*;

mod calls;
mod leaks;

use super::{EmitError, EmitOptions, Entry, emit_c23, emit_counted};
use crate::driver::{ClangDriver, DriverError, OptLevel, Output};
use crate::runtime::Form;
use crate::toolchain::HostToolchain;

/// Every import is a package about which nothing is known.
struct NoPackages;

impl PackageLoader for NoPackages {
    fn load(&self, path: &str) -> Result<Arc<Package>, String> {
        Ok(Arc::new(Package::fake(path)))
    }
}

/// The module of the package `main` made of one file, which must check
/// and lower without a report.
fn module(name: &str, text: &str) -> Module {
    let map = SourceMap::new();
    let sink = Sink::new(1000);
    let names = ThreadedRodeo::new();
    let id = map.add(SourceFile::new(name.into(), text.to_owned()).unwrap());
    let source = map.file(id);
    let file: File = parse(id, &source, &names, &sink);
    let units = [Unit {
        file: &file,
        source: &source,
    }];
    let types = Types::new(&names);
    let package = check("main", &units, &NoPackages, &names, &types, &sink);
    let files = [(&file, &*source)];
    let checked = Checked {
        package: &package,
        files: &files,
        types: &types,
        names: &names,
        aborts: false,
    };
    let host = TargetSpec::named("linux/amd64").unwrap();
    let module = lower_package(&checked, host.layout(), &sink);
    let said: Vec<String> = sink
        .finish()
        .iter()
        .map(|diagnostic| format!("{}: {}", map.location(diagnostic.span), diagnostic.message))
        .collect();
    assert_eq!(said, [] as [&str; 0]);
    module
}

fn options(entry: Entry) -> EmitOptions {
    let target = TargetSpec::named("linux/amd64").unwrap();
    EmitOptions {
        layout: target.layout(),
        entry,
        swiftcall: target.swiftcall,
        aborts: false,
    }
}

/// The unit of the program without what every unit starts with.
fn unit(name: &str, text: &str) -> String {
    let unit = emit_c23(&module(name, text), &options(Entry::Main)).unwrap();
    let mut start = crate::runtime::header(options(Entry::Main).layout);
    start.push_str(super::convention(true));
    start.push_str(super::PRELUDE);
    super::number::helpers(&mut start);
    unit.strip_prefix(&start).unwrap().to_owned()
}

const ARITHMETIC: &str = "package main

func ints(a, b int, u uint8, n uint, k int) int {
	c := a + b*3 - a/b
	c = c&b | a ^ b &^ 7
	c = -c + ^a
	x := u << n
	y := a >> k
	u += x * 3 / 2 % 5
	return c/8 + y
}

func floats(x, y float64, f float32) (float64, bool) {
	q := x/y - float64(f)*0.5
	return -q, q < 1e300 || f != 0.1
}

func converts(a int, u uint8, x float64, f float32, p *int) (int8, float32, uint64, int, uint16) {
	return int8(a), float32(x), uint64(u), int(x), uint16(f) + uint16(*p)
}
";

const CONTROL: &str = "package main

func pick(a int, ok bool) int {
	switch a {
	case 1, 2:
		return 10
	case -1:
		a++
		fallthrough
	default:
		a *= 2
	}
	switch ok {
	case true:
		a--
	}
	for i := 0; i < a; i++ {
		if i%2 == 0 && ok || a > 100 {
			continue
		}
		a -= i
	}
	return a
}

func main() {
	pick(3, true)
}
";

const VALUES: &str = "package main

type Point struct {
	x, y int
}

type Nothing struct{}

type Box struct {
	at    Point
	sizes [3]int8
	name  string
	none  Nothing
	next  *Box
}

var total int

var origin Point

var nothing Nothing

func pair(p Point) (int, Point) { return p.x, Point{p.y, p.x} }

func (b *Box) grow(by int8, i int) Nothing {
	b.sizes[i] += by
	b.at = origin
	total++
	return b.none
}

func build(next *Box, grid [2][2]float32) Box {
	n, p := pair(Point{1, 2})
	b := Box{at: p, sizes: [3]int8{1, 2: 3}, name: \"box\\n\", next: next}
	b.none = b.grow(int8(n), len(b.name))
	q := &b.none
	nothing = q
	grid[1][0] = grid[0][1]
	return b
}

func init() { total = 7 }

func main() {}
";

#[test]
fn every_unit_starts_with_the_same_helpers() {
    let whole = emit_c23(&module("a.gopp", "package main\n"), &options(Entry::Main)).unwrap();
    insta::assert_snapshot!(whole);
}

#[test]
fn a_unit_starts_its_program_as_it_is_built_for() {
    let module = module("a.gopp", CONTROL);
    let unit = |entry: Entry| emit_c23(&module, &options(entry)).unwrap();
    let (main, entered) = (
        "\nint main(int argc, char **argv) {",
        "\nint gopp_entry(void) {",
    );
    let (program, loaded, library) = (unit(Entry::Main), unit(Entry::Module), unit(Entry::None));
    assert!(program.contains(main) && !program.contains(entered));
    assert!(loaded.contains(&format!(
        "{entered}\n  _gopp_rt_init(0, 0);\n  _gopp_rt_funcs("
    )));
    assert!(!loaded.contains(main));
    // What is no program has no start and no table of names.
    assert!(!library.contains(main) && !library.contains(entered));
    assert!(!library.contains("gopp_funcs"));
    assert_eq!(
        program
            .replace(main, entered)
            .replace("(argc, argv)", "(0, 0)"),
        loaded
    );
}

#[test]
fn arithmetic_is_written_with_the_helpers_that_keep_the_results_of_go() {
    insta::assert_snapshot!(unit("a.gopp", ARITHMETIC));
}

#[test]
fn blocks_are_labels_and_their_ends_are_jumps() {
    insta::assert_snapshot!(unit("a.gopp", CONTROL));
}

#[test]
fn values_of_several_parts_are_structs_and_what_takes_no_room_is_not_written() {
    insta::assert_snapshot!(unit("a.gopp", VALUES));
}

const INTERFACES: &str = "package main

type Named struct {
	name string
	list []int
}

type Sized interface {
	Size() int
	Grow(by int, note string)
}

func (n Named) Size() int { return len(n.list) }

func (n *Named) Grow(by int, note string) { n.name += note }

func held(v any) int {
	switch x := v.(type) {
	case int:
		return x
	case Named:
		return len(x.name) + len(x.list)
	}
	return 0
}

func grown(s Sized) int {
	s.Grow(1, \"!\")
	return s.Size()
}

type Op func(a int, note string) int

func plain(a int, note string) int { return a + len(note) }

func through(op Op, a int) int {
	ops := []Op{op, plain, func(a int, note string) int { return a }}
	return ops[a%3](a, \"n\")
}

func sized(v any) int {
	if s, ok := v.(Sized); ok {
		return s.Size()
	}
	switch x := v.(type) {
	case interface{ Size() int }:
		return x.Size()
	case interface{}:
		return 1
	}
	return 0
}

func main() {
	n := Named{\"n\", []int{1}}
	println(held(7) + held(n) + held(nil) + grown(&n) + sized(&n) + sized(n) + sized(nil) + through(plain, 1))
}
";

#[test]
fn the_descriptor_of_a_type_is_static_data_that_names_its_glue() {
    let unit = unit("a.gopp", INTERFACES);
    // A descriptor is declared before the functions, which name it,
    // and said after their prototypes, which it names.
    let declared = unit.find("__attribute__((weak)) const T").unwrap();
    let prototype = unit.find("void gopp_f").unwrap();
    let said = unit.find(" = {(uint8_t *)&gopp_g").unwrap();
    assert!(declared < prototype && prototype < said);
    insta::assert_snapshot!(unit);
}

#[test]
fn a_program_whose_panics_end_it_tells_the_runtime_when_it_starts() {
    let module = module("a.gopp", "package main\n\nfunc main() {}\n");
    let told = "  _gopp_rt_funcs(&gopp_table);\n  _gopp_panic_aborts();\n";
    let aborting = EmitOptions {
        aborts: true,
        ..options(Entry::Main)
    };
    assert!(emit_c23(&module, &aborting).unwrap().contains(told));
    let unwinding = emit_c23(&module, &options(Entry::Main)).unwrap();
    assert!(!unwinding.contains("\n  _gopp_panic_aborts();"));
}

#[test]
fn a_cleanup_block_is_a_function_of_its_own_and_a_module_that_breaks_a_rule_is_refused() {
    use gopp_gir::{CallConv, FuncBuilder, FuncType, Linkage, Place, Span, Type, TypeInterner};
    use gopp_gir::{UnwindAction, verify_module};
    let mut module = Module::default();
    let sig = module.types.intern(Type::Func(FuncType {
        params: [].into(),
        ret: TypeInterner::UNIT,
        has_env: false,
        cc: CallConv::Go,
    }));
    fn new<'m>(module: &'m mut Module, name: &str, sig: gopp_gir::TypeId) -> FuncBuilder<'m> {
        FuncBuilder::new(module, name, sig, Linkage::External, Span::default()).unwrap()
    }
    let called = new(&mut module, "p.called", sig).finish().unwrap();
    // A call that unwinds into a block of the function.
    let mut f = new(&mut module, "p.f", sig);
    let [entry, done, cleanup] = [(); 3].map(|()| f.new_block());
    f.set_cleanup(cleanup);
    f.switch_to(entry);
    let nothing = f.temp(TypeInterner::UNIT);
    let callee = f.const_func(called);
    let unwind = UnwindAction::Cleanup(cleanup);
    f.call(
        callee,
        Vec::new(),
        Place::local(nothing),
        Some(done),
        unwind,
    )
    .unwrap();
    f.switch_to(done);
    f.ret().unwrap();
    f.switch_to(cleanup);
    f.resume().unwrap();
    f.finish().unwrap();
    assert_eq!(verify_module(&module), Ok(()));
    // The cleanup block is a function that Clang calls where `p.f` is
    // left; the call says before that its block is the first, and the
    // return that there is none.
    let unit = emit_c23(&module, &options(Entry::None)).unwrap();
    let start = unit.find("struct gopp_f1_own {").unwrap();
    let written = &unit[start..];
    for part in [
        "static inline __attribute__((always_inline)) void gopp_f1_unwind(struct gopp_f1_own \
         *gopp_own) {\n  switch (gopp_own->state) {\n  case 0: return;\n  case 1: goto bb2;\n  \
         default: _gopp_panic_drop();\n  }\nbb2:\n  return;\n}",
        "struct gopp_f1_own gopp_owned __attribute__((cleanup(gopp_f1_unwind)));",
        "  gopp_own->state = 1; gopp_f0();\n",
        "  gopp_own->state = 0; return;\n",
    ] {
        assert!(written.contains(part), "{part}\n{written}");
    }

    // A function whose first block is a cleanup block.
    let mut g = new(&mut module, "p.g", sig);
    let entry = g.new_block();
    g.set_cleanup(entry);
    g.switch_to(entry);
    g.resume().unwrap();
    g.finish().unwrap();
    let refused = emit_c23(&module, &options(Entry::None)).unwrap_err();
    assert!(matches!(refused, EmitError::Verify(_)), "{refused}");
}

/// A directory of its own, and a driver that takes every warning for an
/// error.
fn strict<'t>(toolchain: &'t HostToolchain, target: &str, opt: OptLevel) -> ClangDriver<'t> {
    ClangDriver {
        toolchain,
        target: TargetSpec::named(target).unwrap(),
        opt,
        strict: true,
    }
}

#[test]
fn every_unit_compiles_without_a_warning_for_both_targets_and_twice_the_same() {
    let toolchain = HostToolchain::discover().unwrap();
    let programs = [
        ARITHMETIC,
        CONTROL,
        VALUES,
        CHECKS,
        INTERFACES,
        PANICS,
        "package main\n",
    ];
    let cases: Vec<(&str, &str, OptLevel)> = programs
        .iter()
        .flat_map(|&program| {
            [
                (program, "linux/amd64", OptLevel::O0),
                (program, "linux/amd64", OptLevel::O2),
                (program, "windows/amd64", OptLevel::O0),
            ]
        })
        .collect();
    cases.par_iter().for_each(|&(program, target, opt)| {
        let unit = emit_c23(&module("a.gopp", program), &options(Entry::Main)).unwrap();
        let driver = strict(&toolchain, target, opt);
        let mut objects = Vec::new();
        for _ in 0..2 {
            let dir = tempfile::tempdir().unwrap();
            let object = dir.path().join("unit.o");
            driver
                .build(&unit, dir.path(), &object, Output::Object, &[])
                .unwrap_or_else(|error| panic!("{target}: {error}"));
            objects.push(std::fs::read(object).unwrap());
        }
        assert!(objects[0] == objects[1], "{target}: two builds differ");
        assert!(objects[0].len() > 100);
        // A COFF object has no time in its header: two builds are the
        // same whenever they are made.
        if target == "windows/amd64" {
            assert_eq!(objects[0][4..8], [0; 4], "the time of the build");
        }
    });
}

/// A program that stops its own panics and checks what is left of
/// them, as `CHECKS` does: a function goes on after a panic with what
/// its variables had, at every level.
const PANICS: &str = "package main

func expect(got, want int) {
	if got != want {
		var cell [1]int
		at := 1
		cell[at] = got
	}
}

func fail(n int) int {
	if n > 2 {
		panic(n)
	}
	return n
}

func sum(limit int) (total int) {
	defer func() {
		if r := recover(); r != nil {
			n, _ := r.(int)
			total += 100 * n
		}
	}()
	for i := 0; i < limit; i++ {
		total += fail(i)
	}
	return total
}

func late() int {
	defer func() { recover() }()
	defer func() { panic(\"late\") }()
	return 7
}

func through(n int) int {
	defer func() {}()
	if n == 0 {
		var a [3]int
		return a[n+3]
	}
	return through(n-1) + 1
}

func outer() (n int) {
	defer func() {
		if recover() != nil {
			n = -1
		}
	}()
	return through(3)
}

func main() {
	expect(sum(2), 1)
	expect(sum(9), 303)
	expect(late(), 7)
	for i := 0; i < 3; i++ {
		expect(outer(), -1)
	}
	println(\"stopped\", sum(5))
}
";

/// A program that checks itself: `expect` indexes past an array when a
/// value is not what Go says, which panics.
const CHECKS: &str = "package main

var calls int

func expect(got, want int) {
	if got != want {
		var cell [1]int
		at := 1
		cell[at] = got
	}
}

func least() int { return -9223372036854775808 }

func minus(n int) int { return -n }

func count() int {
	calls++
	return calls
}

type Point struct {
	x, y int
}

func swap(p Point) Point { return Point{p.y, p.x} }

func (p *Point) scale(k int) { p.x *= k; p.y *= k }

func divmod(a, b int) (int, int) { return a / b, a % b }

func fib(n int) int {
	if n < 2 {
		return n
	}
	return fib(n-1) + fib(n-2)
}

func kind(n int) int {
	r := 0
	switch n {
	case 1:
		r += 1
		fallthrough
	case 2:
		r += 2
	case 3, 4:
		r = 30
	default:
		r = -1
	}
	return r
}

func numbers() {
	m, one := least(), minus(-1)
	expect(m/minus(1), m)
	expect(m%minus(1), 0)
	expect(m-one, 9223372036854775807)
	q, r := divmod(-7, 2)
	expect(q, -3)
	expect(r, -1)
	far := uint(70)
	expect(one<<far, 0)
	expect(int(int8(minus(8))>>far), -1)
	expect(int(uint32(one)<<far), 0)
	expect(int(uint8(one)<<7>>7), 1)
	expect(one<<63>>63, -1)
	small := int8(127)
	small++
	expect(int(small), -128)
	wide := uint16(65535)
	expect(int(wide*wide), 1)
	expect(int(uint8(minus(-255))+uint8(one)), 0)
	expect(int(int8(minus(-300))), 44)
	expect(int(int64(int8(minus(1)))), -1)
	expect(int(uint64(uint8(minus(-200)))), 200)
	expect(7&^5|8^1, 11)
}

func floats() {
	zero, big := 0.0, 1e20
	nan := zero / zero
	expect(int(int32(big)), 2147483647)
	expect(int(int32(-big)), -2147483648)
	expect(int(int32(nan)), 0)
	expect(int(uint8(big)), 255)
	expect(int(uint8(-big)), 0)
	expect(int(big*10), 9223372036854775807)
	down := -2.9
	expect(int(down), -2)
	half := float32(0.5)
	expect(int(float64(half)*4+0.25), 2)
	if nan == nan || !(nan != nan) || big <= zero {
		expect(0, 1)
	}
}

func values() {
	p := Point{1, 2}
	q := swap(p)
	q.scale(10)
	expect(p.x*1000+q.x*10+q.y, 1210)
	a := [3]int{1, 2, 3}
	b := a
	b[0] = 9
	i := 2
	a[i], i = b[0], 0
	expect(a[0]*100+a[1]*10+a[2], 129)
	r := &a[1]
	r += 5
	a, b = b, a
	expect(b[1]+a[0], 16)
	x, y := 1, 2
	x, y = y, x+y
	expect(x*10+y, 23)
}

func control() {
	sum := 0
outer:
	for i := 0; i < 10; i++ {
		for j := 0; ; j++ {
			if j > i {
				continue outer
			}
			if i == 7 {
				break outer
			}
			sum += j
		}
	}
	expect(sum, 56)
	expect(kind(1)*1000+kind(2)*100+kind(4)+kind(9), 3229)
	expect(fib(15), 610)
	n := 0
again:
	n += 3
	if n < 10 {
		goto again
	}
	expect(n, 12)
	first := calls + count()*10 + calls*100
	expect(first, 110)
	if count() == 0 && count() == 0 || count() == 0 {
		expect(0, 1)
	}
	expect(calls, 3)
}

func main() {
	numbers()
	floats()
	values()
	control()
}
";

#[test]
#[cfg_attr(
    not(all(target_os = "linux", target_arch = "x86_64")),
    ignore = "links and runs programs for linux/amd64"
)]
fn a_program_computes_what_go_says_at_every_level() {
    let toolchain = HostToolchain::discover().unwrap();
    let fails = |body: &str| {
        format!("package main\n\nfunc zero() int {{ return 0 }}\n\nfunc main() {{\n{body}\n}}\n")
    };
    // A program, the status that it ends with, and what it says first.
    let failed = "panic: runtime error: ";
    let programs = [
        (CHECKS.to_owned(), 0, String::new()),
        (PANICS.to_owned(), 0, "stopped 303".to_owned()),
        (
            fails("\tvar a [3]int\n\ta[zero()+3] = 1"),
            2,
            format!("{failed}index out of range [3] with length 3"),
        ),
        (
            fails("\tvar a [3]int\n\t_ = a[zero()-1]"),
            2,
            format!("{failed}index out of range [-1]"),
        ),
        (
            fails("\tvar a [3]int\n\t_ = a[uint8(zero()+200)]"),
            2,
            format!("{failed}index out of range [200] with length 3"),
        ),
        (
            fails("\tvar a [3]int\n\t_ = a[^uint64(zero())]"),
            2,
            format!("{failed}index out of range [18446744073709551615] with length 3"),
        ),
        (
            fails("\t_ = 1 / zero()"),
            2,
            format!("{failed}integer divide by zero"),
        ),
        (
            fails("\t_ = 1 % zero()"),
            2,
            format!("{failed}integer divide by zero"),
        ),
        (
            fails("\t_ = 1 << (zero() - 1)"),
            2,
            format!("{failed}negative shift amount"),
        ),
        (fails("\t_ = 1.5 / float64(zero())"), 0, String::new()),
        (
            fails("\tprintln(\"x\", zero() - 7, 2.5, zero() == 0)\n\tprint(1, \"\\n\")"),
            0,
            "x -7 +2.500000e+000 true".to_owned(),
        ),
    ];
    let cases: Vec<(&str, i32, &str, OptLevel)> = programs
        .iter()
        .flat_map(|(program, status, said)| {
            [OptLevel::O0, OptLevel::O2, OptLevel::Oz]
                .map(|opt| (&**program, *status, &**said, opt))
        })
        .collect();
    let cache = tempfile::tempdir().unwrap();
    cases.par_iter().for_each(|&(program, status, said, opt)| {
        let unit = emit_c23(&module("a.gopp", program), &options(Entry::Main)).unwrap();
        let dir = tempfile::tempdir().unwrap();
        let program_file = dir.path().join("program");
        let driver = strict(&toolchain, "linux/amd64", opt);
        let runtime = crate::runtime::build(&driver, Form::Objects, cache.path()).unwrap();
        driver
            .build(
                &unit,
                dir.path(),
                &program_file,
                Output::Executable,
                &runtime,
            )
            .unwrap_or_else(|error| panic!("{error}"));
        let ran = std::process::Command::new(&program_file).output().unwrap();
        assert_eq!(ran.status.code(), Some(status), "{opt:?}:\n{program}");
        let written = String::from_utf8_lossy(&ran.stderr);
        let first = written.lines().next().unwrap_or_default();
        assert_eq!(first, said, "{opt:?}:\n{program}");
        // A failure has a trace, which names the function that failed
        // unless its code was put into another.
        let traced = written.contains("\n\ngoroutine 1 [running]:\n");
        assert_eq!(traced, status == 2, "{written}");
        if opt == OptLevel::O0 {
            let named = written.contains("\nmain.main()\n\t+0x");
            assert_eq!(named, status == 2, "{written}");
        }
    });
}

#[test]
fn a_link_for_another_target_says_what_it_lacks() {
    let toolchain = HostToolchain::discover().unwrap();
    let other = match cfg!(target_os = "linux") {
        true => "windows/amd64",
        false => "linux/amd64",
    };
    let dir = tempfile::tempdir().unwrap();
    let unit = emit_c23(
        &module("a.gopp", "package main\n\nfunc main() {}\n"),
        &options(Entry::Main),
    )
    .unwrap();
    let refused = strict(&toolchain, other, OptLevel::O0)
        .build(
            &unit,
            dir.path(),
            &dir.path().join("program"),
            Output::Executable,
            &[],
        )
        .unwrap_err();
    assert!(
        matches!(refused, DriverError::NoSysroot { .. }),
        "{refused}"
    );
}

#[test]
fn the_unit_does_not_depend_on_the_number_of_threads() {
    let functions: String = (0..200)
        .map(|at| format!("func f{at}(a int) int {{\n\tb := [2]int{{a, {at}}}\n\treturn b[a%2] + f{}(a-1)\n}}\n\n", (at + 7) % 200))
        .collect();
    let module = module("a.gopp", &format!("package main\n\n{functions}"));
    let written_by = |threads: usize| {
        let pool = rayon::ThreadPoolBuilder::new()
            .num_threads(threads)
            .build()
            .unwrap();
        pool.install(|| emit_counted(&module, &options(Entry::Main)).unwrap())
    };
    let alone = written_by(1);
    for threads in [2, 5, 16] {
        assert!(written_by(threads) == alone, "{threads} threads");
    }
}

#[test]
fn the_steps_grow_with_the_module() {
    use gopp_testutil::linear::assert_near_linear;
    let steps = |text: &str| {
        emit_counted(&module("a.gopp", text), &options(Entry::Main))
            .unwrap()
            .1
    };
    let sizes = [100, 200, 400, 800];
    assert_near_linear(&sizes, |n| {
        let functions: String = (0..n)
            .map(|at| format!("func f{at}(a int) int {{ return a + {at} }}\n"))
            .collect();
        steps(&format!("package main\n\n{functions}"))
    });
    assert_near_linear(&sizes, |n| {
        let body = "\tx = x*3 + 1\n\tif x > 9 {\n\t\tx -= 9\n\t}\n".repeat(n);
        steps(&format!(
            "package main\n\nfunc f(x int) int {{\n{body}\treturn x\n}}\n"
        ))
    });
    // Structs that hold one another, and pointers to pointers.
    assert_near_linear(&sizes, |n| {
        let types: String = (1..n)
            .map(|at| format!("type S{at} struct {{ in S{}; p *S{at} }}\n", at - 1))
            .collect();
        let last = n - 1;
        steps(&format!(
            "package main\n\ntype S0 struct{{ v int }}\n{types}\nfunc f(s *S{last}) *S{last} {{ return s }}\n"
        ))
    });
    assert_near_linear(&sizes, |n| {
        let stars = "*".repeat(n);
        steps(&format!(
            "package main\n\nfunc f(p {stars}int) {stars}int {{ return p }}\n"
        ))
    });
}
