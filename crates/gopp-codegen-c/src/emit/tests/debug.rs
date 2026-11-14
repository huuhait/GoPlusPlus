//! What a unit says of the variables of the source for a debugger
//! (debug-info.md): their names, and the blocks of C of their scopes.

use super::*;
use crate::emit::names::usable;

fn debug() -> EmitOptions<'static> {
    EmitOptions {
        debug: true,
        ..options(Entry::Main)
    }
}

/// The definition of the function of the symbol in a unit for a
/// debugger, without the lines that say where each line is written.
fn defined(unit: &str, symbol: &str) -> String {
    let labeled = format!(" __asm__(\"{symbol}\");");
    let declared = (unit.lines()).find(|line| line.ends_with(&labeled));
    let declared = declared.unwrap_or_else(|| panic!("no function {symbol}\n{unit}"));
    let start = declared.find(" gopp_f").unwrap() + 1;
    let name = &declared[start..start + declared[start..].find('(').unwrap()];
    let head = format!(" {name}(");
    let mut lines = (unit.lines())
        .filter(|line| !line.starts_with("#line "))
        .skip_while(|line| !(line.contains(&head) && line.ends_with(") {")));
    let mut text = String::new();
    for line in &mut lines {
        text.push_str(line);
        text.push('\n');
        if line == "}" {
            break;
        }
    }
    text
}

/// Each variable that the function declares, with how many blocks it
/// is declared inside.
fn declared(function: &str) -> Vec<(String, usize)> {
    let mut depth = 0;
    let mut found = Vec::new();
    for line in function.lines() {
        match line {
            "  {" => depth += 1,
            "  }" => depth -= 1,
            _ => {
                let name = line
                    .strip_prefix("  [[maybe_unused]] ")
                    .and_then(|rest| rest.strip_suffix(';'))
                    .and_then(|rest| rest.rsplit(' ').next());
                found.extend(name.map(|name| (name.to_owned(), depth)));
            }
        }
    }
    found
}

const SCOPES: &str = "package main

func add(a int, b int) int {
	sum := a + b
	if sum > 3 {
		sum := sum * 2
		extra := sum + 1
		println(extra)
	}
	for i := 0; i < 2; i++ {
		twice := i * 2
		println(twice)
	}
	for i := 10; i < 12; i++ {
		println(i)
	}
	return sum
}

func greet(name string, linux int) string {
	text := \"hi \" + name
	bool := linux + 1
	gopp_n := bool + 1
	T7 := gopp_n + 1
	état := T7 + 1
	println(état)
	return text
}

func main() {
	println(add(3, 4))
	println(greet(\"bob\", 5))
}
";

#[test]
fn a_variable_has_its_name_in_the_block_of_its_scope() {
    let unit = emit_c23(&module("a.gopp", SCOPES), &debug()).unwrap();
    let add = defined(&unit, "_GPF4mainE3add");
    assert!(
        add.starts_with("GOPP_CALL int64_t gopp_f0(int64_t a, int64_t b) {\n"),
        "{add}"
    );
    let named: Vec<(String, usize)> = declared(&add)
        .into_iter()
        .filter(|(name, _)| !name.starts_with('_'))
        .collect();
    // The second `sum` is worked out from the first inside its own
    // scope, where C would take the name for the second: it has none.
    let expected = [("sum", 0), ("extra", 1), ("i", 1), ("twice", 2), ("i", 1)];
    let expected: Vec<(String, usize)> = (expected.iter())
        .map(|&(name, depth)| (name.to_owned(), depth))
        .collect();
    assert_eq!(named, expected, "{add}");
    // A variable of a block is declared after the first label of the
    // block: Clang tells a debugger of no variable that is declared
    // where nothing leads.
    let lines: Vec<&str> = add.lines().collect();
    for (at, line) in lines.iter().enumerate() {
        if *line == "  {" {
            assert!(lines[at + 1].ends_with(':'), "{add}");
            assert!(lines[at + 2].starts_with("  [[maybe_unused]] "), "{add}");
        }
    }
    // Without debug information nothing is named and nothing is nested.
    let plain = emit_c23(&module("a.gopp", SCOPES), &options(Entry::Main)).unwrap();
    assert!(!plain.contains("int64_t sum;") && !plain.contains("\n  {\n"));
}

#[test]
fn a_name_that_c_takes_for_something_else_is_not_used() {
    for name in [
        "x",
        "sum",
        "linux",
        "errno",
        "i2",
        "camelCase",
        "with_under",
        "Tx",
        "T",
    ] {
        assert!(usable(name), "{name}");
    }
    let taken = [
        "",
        "_",
        "_x",
        "bool",
        "int",
        "true",
        "nullptr",
        "uint8_t",
        "size_t",
        "NULL",
        "main",
        "state",
        "gopp",
        "gopp_own",
        "GOPP_CALL",
        "T0",
        "T12",
        "état",
        "a-b",
        "9x",
        "typeof",
    ];
    for name in taken {
        assert!(!usable(name), "{name}");
    }
    let unit = emit_c23(&module("a.gopp", SCOPES), &debug()).unwrap();
    let greet = defined(&unit, "_GPF4mainE5greet");
    // A parameter keeps its name, even one that a header has a macro
    // for: the unit takes the macro away before the function.
    assert!(
        greet.contains("(gopp_string name, int64_t linux) {"),
        "{greet}"
    );
    assert!(unit.contains("\n#undef linux\n") && unit.contains("\n#undef name\n"));
    for (name, _) in declared(&greet) {
        assert!(name.starts_with('_'), "{name}\n{greet}");
    }
    // What a cleanup block ends is a field of the record of the
    // function, under its name.
    let record = unit.find("struct gopp_f1_own {\n").unwrap();
    let record = &unit[record..record + unit[record..].find("};").unwrap()];
    assert!(record.contains("  gopp_string text;\n"), "{record}");
    assert!(greet.contains("gopp_own->text = "), "{greet}");
}

/// A function with `count` blocks in one another, each with a variable
/// of its own that the next reads.
fn nested(count: usize) -> String {
    let mut text = "package main\n\nfunc f(x int) int {\n\tv0 := x\n".to_owned();
    for at in 1..=count {
        text.push_str(&format!(
            "\tif v{} > 0 {{\n\tv{at} := v{} + 1\n",
            at - 1,
            at - 1
        ));
    }
    text.push_str(&format!("\tx += v{count}\n"));
    text.push_str(&"\t}\n".repeat(count));
    text.push_str("\treturn x\n}\n\nfunc main() { println(f(1)) }\n");
    text
}

#[test]
fn the_blocks_of_a_function_are_nested_no_deeper_than_c_takes() {
    let unit = emit_c23(&module("a.gopp", &nested(80)), &debug()).unwrap();
    let f = defined(&unit, "_GPF4mainE1f");
    let found = declared(&f);
    let deepest = found.iter().map(|&(_, depth)| depth).max().unwrap();
    assert_eq!(deepest, 64, "{f}");
    // The variables of the scopes below share the last block, each
    // under its name.
    let named = found.iter().filter(|(name, _)| name.starts_with('v'));
    assert_eq!(named.count(), 81, "{f}");
    let shared = found.iter().filter(|&&(_, depth)| depth == 64);
    assert_eq!(shared.count(), 17, "{f}");
}

#[test]
fn a_unit_for_a_debugger_compiles_without_a_warning_and_runs_the_same() {
    let toolchain = HostToolchain::discover().unwrap();
    let deep = nested(80);
    let programs = [SCOPES, &deep, CONTROL, VALUES, CHECKS, INTERFACES, PANICS];
    let cases: Vec<(&str, &str, OptLevel)> = programs
        .iter()
        .flat_map(|&program| {
            [
                (program, "linux/amd64", OptLevel::O0),
                (program, "linux/amd64", OptLevel::O2),
                (program, "windows/amd64", OptLevel::O0),
                (program, "windows/amd64", OptLevel::O2),
            ]
        })
        .collect();
    cases.par_iter().for_each(|&(program, target, opt)| {
        let spec = TargetSpec::named(target).unwrap();
        let options = EmitOptions {
            layout: spec.layout(),
            swiftcall: spec.swiftcall,
            ..debug()
        };
        let unit = emit_c23(&module("a.gopp", program), &options).unwrap();
        let driver = ClangDriver {
            debug: true,
            ..strict(&toolchain, target, opt)
        };
        let dir = tempfile::tempdir().unwrap();
        let object = dir.path().join("unit.o");
        driver
            .build(&unit, dir.path(), &object, Output::Object, &[])
            .unwrap_or_else(|error| panic!("{target}: {error}"));
    });
}

#[test]
fn the_steps_of_naming_grow_with_the_scopes() {
    use gopp_testutil::linear::assert_near_linear;
    let steps = |text: &str| emit_counted(&module("a.gopp", text), &debug()).unwrap().1;
    let sizes = [100, 200, 400, 800];
    // Blocks in one another, far deeper than C takes.
    assert_near_linear(&sizes, |n| steps(&nested(n)));
    // Blocks after one another, each with a variable of one name that
    // is worked out from a variable of the function.
    assert_near_linear(&sizes, |n| {
        let blocks =
            "\tif x > 0 {\n\t\tx := x + 1\n\t\ty := x * 2\n\t\tprintln(y)\n\t}\n".repeat(n);
        steps(&format!(
            "package main\n\nfunc f(x int) int {{\n{blocks}\treturn x\n}}\n\nfunc main() {{ println(f(1)) }}\n"
        ))
    });
}
