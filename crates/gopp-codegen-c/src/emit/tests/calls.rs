//! The convention of the calls between Go++ functions (ADR-0030,
//! calling-convention.md): what the unit says, what Clang makes of it
//! for each target, and a program that checks what its calls pass.

use std::fmt::Write;
use std::path::Path;

use super::*;

fn options_for(target: &TargetSpec, entry: Entry) -> EmitOptions {
    EmitOptions {
        layout: target.layout(),
        entry,
        swiftcall: target.swiftcall,
        aborts: false,
    }
}

/// The assembly that Clang makes of the unit for the target.
fn assembly(toolchain: &HostToolchain, unit: &str, target: &str, opt: OptLevel) -> String {
    let dir = tempfile::tempdir().unwrap();
    let output = dir.path().join("unit.s");
    strict(toolchain, target, opt)
        .build(unit, dir.path(), &output, Output::Assembly, &[])
        .unwrap_or_else(|error| panic!("{target}: {error}"));
    std::fs::read_to_string(output).unwrap()
}

/// The lines of the function of the symbol.
fn body_of<'t>(assembly: &'t str, symbol: &str) -> &'t str {
    let label = format!("\n{symbol}:");
    let start = assembly
        .find(&label)
        .unwrap_or_else(|| panic!("no function {symbol}"));
    let rest = &assembly[start..];
    let end = rest.find("-- End function").unwrap_or(rest.len());
    &rest[..end]
}

/// A function with an environment, one of C that it calls, and what a
/// unit says of both for a target with `swiftcall` and for one without.
#[test]
fn the_convention_and_the_environment_are_written_for_the_target() {
    use gopp_gir::{BinOp, CallConv, FuncBuilder, FuncType, Linkage, Operand, Place, Rvalue};
    use gopp_gir::{Span, Type, UnwindAction};
    let mut module = Module::default();
    let wide = module.types.int(64, true);
    let byte = module.types.int(8, false);
    let bytes = module.types.ptr(byte);
    let of_c = module.types.intern(Type::Func(FuncType {
        params: [wide].into(),
        ret: wide,
        has_env: false,
        cc: CallConv::C,
    }));
    let with_env = module.types.intern(Type::Func(FuncType {
        params: [bytes, wide].into(),
        ret: wide,
        has_env: true,
        cc: CallConv::Go,
    }));
    let of_c_side = FuncBuilder::new(
        &mut module,
        "counted",
        of_c,
        Linkage::External,
        Span::default(),
    );
    let of_c_side = of_c_side.unwrap().finish().unwrap();
    let mut f = FuncBuilder::new(
        &mut module,
        "p.closure",
        with_env,
        Linkage::External,
        Span::default(),
    )
    .unwrap();
    let [entry, done] = [(); 2].map(|()| f.new_block());
    f.switch_to(entry);
    // What the environment holds, and the number that C gives back.
    let held = f
        .assign_temp(Rvalue::Use(Operand::Copy(Place::local(f.param(0)))))
        .unwrap();
    let first = f.deref(&Place::local(held)).unwrap();
    let first = f.assign_temp(Rvalue::Use(Operand::Copy(first))).unwrap();
    let given = f.temp(wide);
    let callee = f.const_func(of_c_side);
    let argument = vec![Operand::Copy(Place::local(f.param(1)))];
    let unwind = UnwindAction::Continue;
    f.call(callee, argument, Place::local(given), Some(done), unwind)
        .unwrap();
    f.switch_to(done);
    let wider = Rvalue::Cast(
        gopp_gir::CastKind::IntToInt,
        Operand::Copy(Place::local(first)),
        wide,
    );
    let wider = f.assign_temp(wider).unwrap();
    let sum = Rvalue::Binary(
        BinOp::Add,
        Operand::Copy(Place::local(wider)),
        Operand::Copy(Place::local(given)),
    );
    f.assign(Place::local(f.result()), sum).unwrap();
    f.ret().unwrap();
    f.finish().unwrap();

    let with = TargetSpec::named("linux/amd64").unwrap();
    let without = TargetSpec::named("linux/riscv64").unwrap();
    let unit = emit_c23(&module, &options_for(with, Entry::None)).unwrap();
    let said = [
        "#define GOPP_CALL __attribute__((swiftcall))\n",
        "#define GOPP_ENV __attribute__((swift_context))\n",
        "typedef GOPP_CALL int64_t (*T",
        ")(GOPP_ENV uint8_t *, int64_t);",
        "\nint64_t gopp_f0(int64_t _1) __asm__(\"counted\");",
        "\nGOPP_CALL int64_t gopp_f1(GOPP_ENV uint8_t * _1, int64_t _2) __asm__(\"p.closure\");",
    ];
    for text in said {
        assert!(unit.contains(text), "{text}\n{unit}");
    }
    let plain = emit_c23(&module, &options_for(without, Entry::None)).unwrap();
    assert!(plain.contains("#define GOPP_CALL\n#define GOPP_ENV\n"));
    assert_eq!(
        plain.replace("#define GOPP_CALL\n#define GOPP_ENV\n", ""),
        unit.replace(said[0], "").replace(said[1], "")
    );

    // The environment has a register of its own, and C is called as C.
    let toolchain = HostToolchain::discover().unwrap();
    let registers = [("linux/amd64", "(%r13)"), ("linux/arm64", "[x20]")];
    for (target, environment) in registers {
        let made = assembly(&toolchain, &unit, target, OptLevel::O2);
        let closure = body_of(&made, "p.closure");
        assert!(closure.contains(environment), "{target}:\n{closure}");
        assert!(closure.contains("counted"), "{target}:\n{closure}");
    }
}

/// Each program of `tests/codegen` says what the assembly of its
/// functions has for a target: a line `// asm <target> <function>:`
/// with texts between `;` that the function has, and after a `!` texts
/// that it has not.
#[test]
fn the_assembly_of_each_target_is_what_tests_codegen_says() {
    let toolchain = HostToolchain::discover().unwrap();
    let dir = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../tests/codegen");
    let mut files: Vec<_> = std::fs::read_dir(&dir)
        .unwrap()
        .map(|entry| entry.unwrap().path())
        .collect();
    files.sort();
    assert!(!files.is_empty());
    for file in files {
        let text = std::fs::read_to_string(&file).unwrap();
        let name = file.file_name().unwrap().to_string_lossy().into_owned();
        // The target, the function and what is said of it.
        let said: Vec<[&str; 3]> = text
            .lines()
            .filter_map(|line| line.strip_prefix("// asm "))
            .map(|line| {
                let (of, texts) = line.split_once(':').unwrap();
                let (target, function) = of.split_once(' ').unwrap();
                [target, function, texts]
            })
            .collect();
        assert!(!said.is_empty(), "{name} says nothing");
        let mut targets: Vec<&str> = said.iter().map(|&[target, ..]| target).collect();
        targets.sort_unstable();
        targets.dedup();
        let module = module(&name, &text);
        targets.par_iter().for_each(|&target| {
            let spec = TargetSpec::named(target).unwrap();
            let unit = emit_c23(&module, &options_for(spec, Entry::None)).unwrap();
            let made = assembly(&toolchain, &unit, target, OptLevel::O2);
            for &[_, function, texts] in said.iter().filter(|&&[of, ..]| of == target) {
                let body = body_of(&made, &gopp_mangle::func_symbol("main", function));
                // One space wherever the assembly has blanks.
                let body = body.split_whitespace().collect::<Vec<_>>().join(" ");
                for text in texts.split(';').map(str::trim) {
                    let (text, wanted) = match text.strip_prefix('!') {
                        Some(text) => (text.trim(), false),
                        None => (text, true),
                    };
                    let what = if wanted { "has no" } else { "has" };
                    assert!(
                        body.contains(text) == wanted,
                        "{name}: {function} for {target} {what} `{text}`:\n{body}"
                    );
                }
            }
        });
    }
}

/// The kinds of values that the functions of `many_parameters` take.
/// The last four have 3, 5 or 6 bytes: a function that takes one is
/// called as a function of C.
const KINDS: [&str; 18] = [
    "int8", "int16", "int32", "int64", "uint8", "uint16", "uint32", "uint64", "float32", "float64",
    "bool", "string", "pair", "[]int32", "[3]int16", "[3]uint8", "rgb", "[5]bool",
];

/// How many of the kinds have whole words.
const WHOLE: usize = 14;

/// A value of the kind that the number picks, as Go writes it.
fn value(kind: usize, n: u64) -> String {
    match kind {
        0 => (n % 200) as i64 - 100,
        1 => (n % 60_000) as i64 - 30_000,
        2 => (n % 4_000_000_000) as i64 - 2_000_000_000,
        3 => (n >> 1) as i64 - (1 << 62),
        4 => (n % 256) as i64,
        5 => (n % 65_536) as i64,
        6 => (n % (1 << 32)) as i64,
        7 => return n.to_string(),
        8 => return format!("{}.5", n % 1000),
        9 => return format!("{}.25", n % 1_000_000),
        10 => return n.is_multiple_of(2).to_string(),
        11 => return format!("\"s{}\"", n % 1000),
        12 => return format!("pair{{{}, {}.5}}", n % 1000, n % 77),
        13 => return format!("[]int32{{{}, {}}}", n % 50, n % 9),
        14 => return format!("[3]int16{{{}, {}, {}}}", n % 100, n % 7, n % 3),
        15 => return format!("[3]uint8{{{}, {}, {}}}", n % 256, n % 5, n % 11),
        16 => return format!("rgb{{{}, {}, {}}}", n % 256, n % 13, n % 17),
        _ => {
            return format!(
                "[5]bool{{{}, true, false, {}, true}}",
                n.is_multiple_of(2),
                n.is_multiple_of(3)
            );
        }
    }
    .to_string()
}

/// Whether `got` is that value, as a condition of Go.
fn holds(kind: usize, got: &str, n: u64) -> String {
    match kind {
        12 => format!("{got}.a == {} && {got}.b == {}.5", n % 1000, n % 77),
        13 => format!(
            "len({got}) == 2 && {got}[0] == {} && {got}[1] == {}",
            n % 50,
            n % 9
        ),
        14 => format!(
            "{got}[0] == {} && {got}[1] == {} && {got}[2] == {}",
            n % 100,
            n % 7,
            n % 3
        ),
        15 => format!(
            "{got}[0] == {} && {got}[1] == {} && {got}[2] == {}",
            n % 256,
            n % 5,
            n % 11
        ),
        16 => format!(
            "{got}.r == {} && {got}.g == {} && {got}.b == {}",
            n % 256,
            n % 13,
            n % 17
        ),
        17 => format!(
            "{got}[0] == {} && {got}[1] && !{got}[2] && {got}[3] == {} && {got}[4]",
            n.is_multiple_of(2),
            n.is_multiple_of(3)
        ),
        _ => format!("{got} == {}", value(kind, n)),
    }
}

/// How many parameters each function of `many_parameters` has.
const PARAMS: usize = 24;

/// A program of Go that checks itself. Each `f` has 24 parameters of
/// every kind (every other one of the kinds of whole words only) and
/// gives one to eight of them back; each `g` has the
/// parameters of its `f` in another order, passes them on and returns
/// what it gets. `main` calls all of them with values that it knows,
/// says each result that is not the value that was passed, and then
/// how many were not.
fn many_parameters(functions: usize) -> String {
    let mut state = 0x9e37_79b9_7f4a_7c15u64;
    let mut next = || {
        state = state
            .wrapping_mul(6_364_136_223_846_793_005)
            .wrapping_add(1_442_695_040_888_963_407);
        state >> 11
    };
    let mut out = "package main\n\ntype pair struct {\n\ta int32\n\tb float64\n}\n\n\
                   type rgb struct{ r, g, b uint8 }\n"
        .to_owned();
    let mut main = "\nfunc main() {\n\tbad := 0\n".to_owned();
    for at in 0..functions {
        // Every other function has kinds of whole words only.
        let among = if at % 2 == 0 { WHOLE } else { KINDS.len() };
        let kinds: Vec<usize> = (0..PARAMS).map(|_| next() as usize % among).collect();
        let values: Vec<u64> = (0..PARAMS).map(|_| next()).collect();
        let returned: Vec<usize> = (0..at % 8 + 1).map(|_| next() as usize % PARAMS).collect();
        let list = |names: &dyn Fn(usize) -> String| -> String {
            let all: Vec<String> = (0..PARAMS).map(names).collect();
            all.join(", ")
        };
        let results: Vec<&str> = returned.iter().map(|&param| KINDS[kinds[param]]).collect();
        let results = results.join(", ");
        let given: Vec<String> = returned.iter().map(|param| format!("p{param}")).collect();
        let _ = write!(
            out,
            "\nfunc f{at}({}) ({results}) {{\n\treturn {}\n}}\n",
            list(&|param| format!("p{param} {}", KINDS[kinds[param]])),
            given.join(", ")
        );
        // The parameter `q` of `g` is the parameter `(q + 7) % 24` of `f`.
        let turned = |param: usize| (param + 7) % PARAMS;
        let _ = write!(
            out,
            "\nfunc g{at}({}) ({results}) {{\n\treturn f{at}({})\n}}\n",
            list(&|param| format!("q{param} {}", KINDS[kinds[turned(param)]])),
            list(&|param| format!("q{}", (param + PARAMS - 7) % PARAMS)),
        );
        let got: Vec<String> = (0..returned.len()).map(|at| format!("r{at}")).collect();
        let calls = [
            ("f", list(&|param| value(kinds[param], values[param]))),
            (
                "g",
                list(&|param| value(kinds[turned(param)], values[turned(param)])),
            ),
        ];
        for (name, arguments) in calls {
            let _ = writeln!(
                main,
                "\t{{\n\t\t{} := {name}{at}({arguments})",
                got.join(", ")
            );
            for (result, &param) in returned.iter().enumerate() {
                let holds = holds(kinds[param], &format!("r{result}"), values[param]);
                let _ = writeln!(
                    main,
                    "\t\tif !({holds}) {{\n\t\t\tbad++\n\t\t\tprintln(\"{name}{at}\", {result})\n\t\t}}"
                );
            }
            main.push_str("\t}\n");
        }
    }
    let _ = writeln!(
        main,
        "\tprintln(\"checked\", {}, \"bad\", bad)\n}}",
        functions * 2
    );
    out + &main
}

/// Calls with 24 parameters and up to eight results pass what Go says,
/// at every level: in registers, in the frame, and through the pointers
/// that Clang makes for what is too large.
#[test]
#[cfg_attr(
    not(all(target_os = "linux", target_arch = "x86_64")),
    ignore = "links and runs programs for linux/amd64"
)]
fn calls_with_many_parameters_and_results_pass_what_was_given_at_every_level() {
    const FUNCTIONS: usize = 32;
    let toolchain = HostToolchain::discover().unwrap();
    let unit = emit_c23(
        &module("a.go", &many_parameters(FUNCTIONS)),
        &options(Entry::Main),
    );
    let unit = unit.unwrap();
    // Both conventions are in the unit.
    let functions = |go_call: bool| {
        let named = unit
            .lines()
            .filter(|line| line.contains(") __asm__(\"_GPF4mainE"));
        named
            .filter(|line| line.starts_with("GOPP_CALL ") == go_call)
            .count()
    };
    assert!(functions(true) > FUNCTIONS && functions(false) >= FUNCTIONS / 2);
    let cache = tempfile::tempdir().unwrap();
    let levels = [
        OptLevel::O0,
        OptLevel::O1,
        OptLevel::O2,
        OptLevel::O3,
        OptLevel::Oz,
    ];
    levels.par_iter().for_each(|&opt| {
        let dir = tempfile::tempdir().unwrap();
        let program = dir.path().join("program");
        let driver = strict(&toolchain, "linux/amd64", opt);
        let runtime = crate::runtime::build(&driver, Form::Objects, cache.path()).unwrap();
        driver
            .build(&unit, dir.path(), &program, Output::Executable, &runtime)
            .unwrap_or_else(|error| panic!("{error}"));
        let ran = std::process::Command::new(&program).output().unwrap();
        let said = String::from_utf8_lossy(&ran.stderr);
        assert_eq!(
            said,
            format!("checked {} bad 0\n", FUNCTIONS * 2),
            "{opt:?}"
        );
    });
    // The same unit is one of the other system too.
    let windows = TargetSpec::named("windows/amd64").unwrap();
    let unit = emit_c23(
        &module("a.go", &many_parameters(FUNCTIONS)),
        &options_for(windows, Entry::Main),
    );
    assembly(&toolchain, &unit.unwrap(), "windows/amd64", OptLevel::O2);
}
