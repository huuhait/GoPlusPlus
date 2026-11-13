use std::collections::{BTreeMap, HashMap};
use std::fs;
use std::sync::Arc;

use gopp_diag::Sink;
use gopp_span::{SourceFile, SourceMap};
use gopp_syntax::ast::{ExprId, ExprKind, File};
use gopp_syntax::parser::parse;
use gopp_testutil::{linear, samples, stress};
use lasso::{Spur, ThreadedRodeo};

use crate::constant::Value;
use crate::resolve::check_counted;
use crate::types::Types;
use crate::{LocalKind, ObjKind, Package, PackageLoader, Ref, Transfer, Unit};

/// Packages by import path. With `fake_rest`, any other path is a package
/// about which nothing is known.
#[derive(Default)]
struct Loader {
    packages: HashMap<String, Arc<Package>>,
    fake_rest: bool,
    /// The types of all packages checked with this loader.
    types: std::sync::OnceLock<Types>,
    /// What runs the `@Const` functions, in the tests that have one.
    evaluator: Option<Box<dyn crate::ConstEvaluator>>,
}

impl PackageLoader for Loader {
    fn load(&self, path: &str) -> Result<Arc<Package>, String> {
        match self.packages.get(path) {
            Some(package) => Ok(Arc::clone(package)),
            None if self.fake_rest => Ok(Arc::new(Package::fake(path))),
            None => Err("package not found".to_owned()),
        }
    }

    fn evaluator(&self) -> Option<&dyn crate::ConstEvaluator> {
        self.evaluator.as_deref()
    }
}

/// A package checked from source texts, with what the checker reported as
/// `file:line:col: message` lines, notes indented under their diagnostic.
struct Checked {
    package: Package,
    messages: Vec<String>,
    steps: u64,
    map: SourceMap,
    files: Vec<File>,
}

fn check_texts(
    path: &str,
    texts: &[(&str, &str)],
    loader: &Loader,
    names: &ThreadedRodeo,
) -> Checked {
    let map = SourceMap::new();
    let sink = Sink::new(1000);
    let mut sources = Vec::new();
    let mut files = Vec::new();
    for &(name, text) in texts {
        let id = map.add(SourceFile::new(name.into(), text.to_owned()).unwrap());
        let source = map.file(id);
        files.push(parse(id, &source, names, &sink));
        sources.push(source);
    }
    let units: Vec<Unit> = files
        .iter()
        .zip(&sources)
        .map(|(file, source)| Unit { file, source })
        .collect();
    let types = loader.types.get_or_init(|| Types::new(names));
    let (package, steps) = check_counted(path, &units, loader, names, types, &sink);
    let mut messages = Vec::new();
    for diagnostic in sink.finish() {
        messages.push(format!(
            "{}: {}",
            map.location(diagnostic.span),
            diagnostic.message
        ));
        for note in diagnostic.notes {
            let place = note
                .span
                .map_or(String::new(), |span| map.location(span) + ": ");
            messages.push(format!("    {place}{}", note.message));
        }
    }
    Checked {
        package,
        messages,
        steps,
        map,
        files,
    }
}

fn messages(name: &str, text: &str) -> Vec<String> {
    let loader = Loader::default();
    check_texts("p", &[(name, text)], &loader, &ThreadedRodeo::new()).messages
}

#[test]
fn every_sample_resolves() {
    let loader = Loader {
        fake_rest: true,
        ..Loader::default()
    };
    for path in samples::files() {
        let text = fs::read_to_string(&path).unwrap();
        let name = path.to_str().unwrap();
        let checked = check_texts("main", &[(name, &text)], &loader, &ThreadedRodeo::new());
        assert_eq!(checked.messages, [] as [&str; 0], "{name}");
        let _ = (
            &checked.package,
            checked.steps,
            &checked.map,
            &checked.files,
        );
    }
}

/// The names of one file: for the offset of a declaring name its kind,
/// and for the offset of every other name what it denotes.
#[derive(Default)]
struct Names {
    defs: BTreeMap<u32, String>,
    uses: BTreeMap<u32, String>,
    /// Names that are keys of composite literals.
    keys: BTreeMap<u32, String>,
}

/// The names of each file of `package`.
fn names_of(package: &Package, files: &[File], names: &ThreadedRodeo) -> Vec<Names> {
    let index: HashMap<_, _> = files
        .iter()
        .enumerate()
        .map(|(i, file)| (file.id, i))
        .collect();
    let mut found: Vec<Names> = files.iter().map(|_| Names::default()).collect();
    for object in &package.objects {
        let kind = match &object.kind {
            // What has no name is in no scope.
            _ if names.resolve(&object.name) == "_" => continue,
            ObjKind::Const => "const",
            ObjKind::Type => "type",
            ObjKind::Var => "var",
            ObjKind::Func => "func",
            // Go's checker has no object in a scope for `import . "p"`,
            // and an import without a name declares no identifier.
            ObjKind::Import { .. } if names.resolve(&object.name) == "." => continue,
            ObjKind::Import { package, .. } if package.name == names.resolve(&object.name) => {
                continue;
            }
            ObjKind::Import { .. } => "import",
            ObjKind::Macro => continue,
        };
        let file = index[&object.span.file];
        found[file].defs.insert(object.span.lo, kind.to_owned());
    }
    for (file, (info, tree)) in package.files.iter().zip(files).enumerate() {
        for body in &info.bodies {
            let denoted = |denotes: &Ref| match *denotes {
                Ref::Universe(name) => format!("universe {}", name.name()),
                Ref::Object(object) => {
                    let span = package.object(object).span;
                    format!("obj {} {}", index[&span.file], span.lo)
                }
                Ref::Member { import, member } => {
                    let ObjKind::Import { package: from, .. } = &package.object(import).kind else {
                        unreachable!("a member of something that is no import");
                    };
                    let name = names.resolve(&from.object(member).name);
                    format!("ext {} {name}", from.path)
                }
                Ref::Local(local) => {
                    format!("obj {file} {}", body.locals[local.0 as usize].span.lo)
                }
            };
            for (&offset, &local) in &body.defs {
                let kind = match body.locals[local.0 as usize].kind {
                    LocalKind::Const => "const",
                    LocalKind::Type | LocalKind::TypeParam => "type",
                    _ => "var",
                };
                found[file].defs.insert(offset, kind.to_owned());
            }
            let offset = |expr: ExprId| match tree.exprs[expr].kind {
                ExprKind::Selector { sel, .. } => sel.span.lo,
                _ => tree.exprs[expr].span.lo,
            };
            for (&expr, denotes) in &body.uses {
                found[file].uses.insert(offset(expr), denoted(denotes));
            }
            for (&expr, denotes) in &body.keys {
                found[file].keys.insert(offset(expr), denoted(denotes));
            }
        }
    }
    found
}

#[test]
fn undefined_and_redeclared_names_are_reported_as_go_reports_them() {
    let cases: &[(&str, &[&str])] = &[
        ("func f() { _ = x }", &["a.gopp:2:16: undefined: x"]),
        // A name is not in scope before its declaration.
        (
            "func f() { _ = x; x := 1; _ = x }",
            &["a.gopp:2:16: undefined: x"],
        ),
        // A variable whose value is in error is not also reported as unused.
        ("func f() { x := x }", &["a.gopp:2:17: undefined: x"]),
        ("func f() { var x = y }", &["a.gopp:2:20: undefined: y"]),
        // The type of a parameter does not see the other parameters.
        ("func f(a int, b a) {}", &["a.gopp:2:17: undefined: a"]),
        (
            "var a int\nfunc a() {}",
            &[
                "a.gopp:3:6: a redeclared in this block",
                "    a.gopp:2:5: other declaration of a",
            ],
        ),
        (
            "func f(a int) (a string) { var a bool }",
            &[
                "a.gopp:2:16: a redeclared in this block",
                "    a.gopp:2:8: other declaration of a",
                "a.gopp:2:32: a redeclared in this block",
                "    a.gopp:2:8: other declaration of a",
                "a.gopp:2:39: missing return",
            ],
        ),
        (
            "func f[T any](T int) {}",
            &[
                "a.gopp:2:15: T redeclared in this block",
                "    a.gopp:2:8: other declaration of T",
            ],
        ),
        (
            "func f() { a, b := 1, 2; a, b := b, a }",
            &["a.gopp:2:31: no new variables on left side of :="],
        ),
        (
            "func f() { a, a := 1, 2; _ = a }",
            &["a.gopp:2:15: a repeated on left side of :="],
        ),
        (
            "func f() { _ = _ }",
            &["a.gopp:2:16: cannot use _ as value or type"],
        ),
        (
            "func f(x any) { switch _ := x.(type) {} }",
            &["a.gopp:2:24: no new variable on left side of :="],
        ),
        // Methods are no names of the package, a package has any number of
        // `init` functions, and `_` declares nothing.
        (
            "type T int\nfunc (T) m() {}\nfunc m() {}\nfunc init() {}\nfunc init() {}\n\
             var _, _ = 1, 2\nfunc _() {}\nfunc _() {}",
            &[],
        ),
        // A predeclared name may be declared again.
        (
            "type int string\nvar true = 1\nfunc len() {}\nfunc f(nil int) int { return nil }",
            &[],
        ),
    ];
    for (body, expected) in cases {
        let text = format!("package p\n{body}\n");
        assert_eq!(messages("a.gopp", &text), *expected, "{body}");
    }
}

#[test]
fn a_variable_that_is_never_read_is_reported() {
    let cases: &[(&str, &[&str])] = &[
        (
            "func f() { x := 1; var y, z int; _ = z }",
            &[
                "a.gopp:2:12: declared and not used: x",
                "a.gopp:2:24: declared and not used: y",
            ],
        ),
        // An assignment may write through a pointer (ADR-0043), so it
        // counts as a use, as `+=` and `++` do.
        ("func f() { x := 1; x = 2; (x) = 3 }", &[]),
        ("func f() { x, y := 1, 2; x += 1; y++ }", &[]),
        ("func f(n int) { p := &n; p = 9 }", &[]),
        // Parameters, results, constants and types need no use.
        (
            "func f(a int) (b int) { const c = 1; type T int; return }",
            &[],
        ),
        ("func f() { x := 1; _ = func() int { return x } }", &[]),
        (
            "func f(v any) { switch x := v.(type) { case int: case string: } }",
            &["a.gopp:2:24: x declared and not used"],
        ),
        (
            "func f(v any) { switch x := v.(type) { case int: case string: _ = x } }",
            &[],
        ),
        (
            "func f(xs []int) { for i, x := range xs { } }",
            &[
                "a.gopp:2:24: declared and not used: i",
                "a.gopp:2:27: declared and not used: x",
            ],
        ),
        // The key of a literal may be a variable.
        ("func f() { k := 1; _ = map[int]int{k: 2} }", &[]),
    ];
    for (body, expected) in cases {
        let text = format!("package p\n{body}\n");
        assert_eq!(messages("a.gopp", &text), *expected, "{body}");
    }
}

/// A package `lib` with an exported and an unexported name.
fn with_lib(names: &ThreadedRodeo) -> Loader {
    let text = "package lib\nfunc Exported() {}\nfunc hidden() {}\nvar Value int\n";
    let mut loader = Loader::default();
    let lib = check_texts("example.com/lib", &[("lib.gopp", text)], &loader, names);
    assert_eq!(lib.messages, [] as [&str; 0]);
    loader
        .packages
        .insert("example.com/lib".to_owned(), Arc::new(lib.package));
    loader
}

#[test]
fn imports_are_checked_as_go_checks_them() {
    let cases: &[(&str, &[&str])] = &[
        ("import \"example.com/lib\"\nvar _ = lib.Value", &[]),
        (
            "import l \"example.com/lib\"\nfunc f() { l.Exported() }",
            &[],
        ),
        ("import . \"example.com/lib\"\nvar _ = Value", &[]),
        ("import _ \"example.com/lib\"", &[]),
        (
            "import \"example.com/lib\"",
            &["a.gopp:2:8: \"example.com/lib\" imported and not used"],
        ),
        (
            "import l \"example.com/lib\"",
            &["a.gopp:2:8: \"example.com/lib\" imported as l and not used"],
        ),
        (
            "import . \"example.com/lib\"",
            &["a.gopp:2:8: \"example.com/lib\" imported and not used"],
        ),
        (
            "import \"example.com/lib\"\nvar _ = lib.Missing",
            &["a.gopp:3:13: undefined: lib.Missing"],
        ),
        (
            "import \"example.com/lib\"\nvar _ = lib.hidden",
            &["a.gopp:3:13: name hidden not exported by package lib"],
        ),
        // One error for a package that is not found, none for its names.
        (
            "import \"nowhere/x\"\nvar _ = x.Anything",
            &["a.gopp:2:8: could not import nowhere/x (package not found)"],
        ),
        (
            "import \"\"",
            &["a.gopp:2:8: invalid import path (empty string)"],
        ),
        (
            "import \"example.com/lib\"\nimport lib \"example.com/lib\"\nvar _ = lib.Value",
            &[
                "a.gopp:3:8: lib redeclared in this block",
                "    a.gopp:2:8: other declaration of lib",
                "a.gopp:3:8: \"example.com/lib\" imported and not used",
            ],
        ),
        (
            "import \"example.com/lib\"\nvar lib = 1",
            &[
                "a.gopp:2:8: \"example.com/lib\" imported and not used",
                "a.gopp:3:5: lib already declared through import of package lib (\"example.com/lib\")",
                "    a.gopp:2:8: other declaration of lib",
            ],
        ),
        (
            "import . \"example.com/lib\"\nvar Value = 1",
            &[
                "a.gopp:2:8: \"example.com/lib\" imported and not used",
                "a.gopp:3:5: Value already declared through dot-import of package lib \
                 (\"example.com/lib\")",
                "    a.gopp:2:8: other declaration of Value",
            ],
        ),
        // A local hides the import.
        (
            "import \"example.com/lib\"\nfunc f(lib struct{ Missing int }) int { return lib.Missing }",
            &["a.gopp:2:8: \"example.com/lib\" imported and not used"],
        ),
    ];
    let names = ThreadedRodeo::new();
    let loader = with_lib(&names);
    for (body, expected) in cases {
        let text = format!("package p\n{body}\n");
        let checked = check_texts("p", &[("a.gopp", &text)], &loader, &names);
        assert_eq!(checked.messages, *expected, "{body}");
    }
}

#[test]
fn an_import_belongs_to_its_file_and_a_declaration_to_the_package() {
    let names = ThreadedRodeo::new();
    let loader = with_lib(&names);
    let files = [
        (
            "a.gopp",
            "package p\nimport \"example.com/lib\"\nvar A = lib.Value + B\n",
        ),
        ("b.gopp", "package p\nvar B = 1\nvar _ = lib.Value\n"),
    ];
    let checked = check_texts("p", &files, &loader, &names);
    assert_eq!(checked.messages, ["b.gopp:3:9: undefined: lib"]);
}

// rule: LEX-7, LEX-8
#[test]
fn the_universe_has_the_names_of_gopp_and_no_new() {
    let body = "var a Option[int]\nvar b = NewUnique(1)\nfunc f[T Send](x Shared[T]) { move(x) }\n";
    let text = format!("package p\n{body}");
    assert_eq!(messages("a.gopp", &text), [] as [&str; 0]);

    let text = "package p\nvar p = new(int)\n";
    assert_eq!(
        messages("a.gopp", text),
        [
            "a.gopp:2:9: undefined: new",
            "    Go++ has no new: write NewUnique(T{}) for one owner or NewShared(T{}) for several",
        ]
    );
    // As in Go, a predeclared name of Go++ may be declared again.
    let text = "package p\ntype Option int\nfunc move() {}\nvar _ Option\n";
    assert_eq!(messages("a.gopp", text), [] as [&str; 0]);
}

#[test]
fn macros_have_a_namespace_of_their_own() {
    let text = "package p\nmacro Twice(e expr) { e; e }\nfunc Twice() {}\n\
                func f() { n := 1; #Twice(n) }\nmacro Twice(e expr) { e }\n";
    assert_eq!(
        messages("a.gopp", text),
        [
            "a.gopp:5:7: Twice redeclared in this block",
            "    a.gopp:2:7: other declaration of Twice",
        ]
    );
}

#[test]
fn the_arguments_of_attributes_and_macros_are_read_leniently() {
    // `linux` is a word of the build condition and `JSON` one of the macro;
    // `limit` is a name of the program, and naming it uses it.
    let text = "@Build(linux)\n\npackage p\n\nconst limit = 3\n\n@Derive(JSON)\n@Retry(limit)\n\
                type T struct {\n\tname string @Json(\"n\", omit)\n}\n\n\
                func f() {\n\tused := 1\n\t#Log(used, anything)\n}\n";
    assert_eq!(messages("a.gopp", text), [] as [&str; 0]);
}

/// The offset of each `/*=name*/` mark, which stands after a declaring
/// identifier, and for the offset of each identifier before a `/*@name*/`
/// mark the name.
fn marks(text: &str) -> (HashMap<&str, u32>, BTreeMap<u32, &str>) {
    let mut declared = HashMap::new();
    let mut used = BTreeMap::new();
    for (at, _) in text.match_indices("/*") {
        let inner = &text[at + 2..];
        let inner = &inner[..inner.find("*/").unwrap()];
        let before = text[..at].trim_end();
        let word = before
            .rfind(|c: char| !c.is_alphanumeric() && c != '_')
            .map_or(0, |i| i + 1);
        if let Some(name) = inner.strip_prefix('=') {
            assert!(declared.insert(name, word as u32).is_none(), "{name} twice");
        } else if let Some(name) = inner.strip_prefix('@') {
            used.insert(word as u32, name);
        }
    }
    (declared, used)
}

#[test]
fn names_denote_the_marked_declarations() {
    let text = "package p

type Node/*=Node*/ struct {
	next Option[*Node/*@Node*/]
}

type List/*=List*/[E/*=E*/ Clone] struct {
	head E/*@E*/
}

func (l/*=l*/ *List/*@List*/[T/*=T*/]) Push(v/*=v*/ T/*@T*/) T/*@T*/ {
	l/*@l*/.head = v/*@v*/
	return v/*@v*/
}

var total/*=total*/ = count/*@count*/ + 1

var count/*=count*/ int

func walk/*=walk*/(n/*=n1*/ *Node/*@Node*/, xs/*=xs*/ []int) (sum/*=sum*/ int) {
	if n/*=n3*/ := n/*@n1*/.next; n/*@n3*/ != nil {
		walk/*@walk*/(n/*@n3*/, xs/*@xs*/)
	}
	for i/*=i*/, &x/*=x*/ := range xs/*@xs*/ {
		x/*@x*/ += i/*@i*/
		sum/*@sum*/ += x/*@x*/
	}
	add/*=add*/ := func(d/*=d*/ int) {
		sum/*@sum*/ += d/*@d*/
		total/*@total*/++
	}
	add/*@add*/(count/*@count*/)
	switch v/*=v2*/ := any(n/*@n1*/).(type) {
	case *Node/*@Node*/:
		_ = v/*@v2*/
	}
	return sum/*@sum*/
}
";
    let (declared, used) = marks(text);
    let names = ThreadedRodeo::new();
    let checked = check_texts("p", &[("a.gopp", text)], &Loader::default(), &names);
    assert_eq!(checked.messages, [] as [&str; 0]);
    let found = &names_of(&checked.package, &checked.files, &names)[0];

    let mut offsets: Vec<u32> = found.defs.keys().copied().collect();
    let mut marked: Vec<u32> = declared.values().copied().collect();
    // The variable of a type switch is declared once per clause.
    offsets.push(declared["v2"]);
    offsets.sort_unstable();
    marked.sort_unstable();
    assert_eq!(offsets, marked);

    // The name of a receiver's type parameter also stands for itself.
    let denoted: BTreeMap<u32, String> = found
        .uses
        .iter()
        .filter(|(offset, denotes)| {
            denotes.starts_with("obj ") && **denotes != format!("obj 0 {offset}")
        })
        .map(|(&offset, denotes)| (offset, denotes.clone()))
        .collect();
    let expected: BTreeMap<u32, String> = used
        .iter()
        .map(|(&offset, name)| (offset, format!("obj 0 {}", declared[name])))
        .collect();
    assert_eq!(denoted, expected);
}

#[test]
fn steps_grow_with_the_input_not_faster() {
    let steps = |text: String| {
        let loader = Loader::default();
        let checked = check_texts("p", &[("a.gopp", &text)], &loader, &ThreadedRodeo::new());
        checked.steps
    };
    let sizes = [500, 1_000, 2_000, 4_000];
    // Each block declares a name that hides the one around it and reads
    // the outermost one.
    let nested = |depth: usize| {
        let open: String = (0..depth).map(|_| "{ x := x + first; ").collect();
        format!(
            "package p\nfunc f(x, first int) {{ {open}_ = x{} }}\n",
            " }".repeat(depth)
        )
    };
    let siblings = |count: usize| {
        let blocks: String = (0..count)
            .map(|i| format!("\t{{ v{i} := x; _ = v{i} + first }}\n"))
            .collect();
        format!("package p\nfunc f(x, first int) {{\n{blocks}}}\n")
    };
    let closures = |depth: usize| {
        let open: String = (0..depth)
            .map(|i| format!("func(a{i} int) {{ _ = a0 + a{i}; "))
            .collect();
        format!("package p\nvar f = {open}{}\n", "}".repeat(depth))
    };
    let declarations = |count: usize| {
        let all: String = (0..count)
            .map(|i| format!("func f{i}() int {{ return f{}() + g }}\n", (i + 1) % count))
            .collect();
        format!("package p\nvar g int\n{all}")
    };
    // Each block jumps out to a label that is declared after all of them.
    let jumps_out = |depth: usize| {
        format!(
            "package p\nfunc f() {{\n{}{}\nEnd:\n}}\n",
            "{ goto End; ".repeat(depth),
            "}".repeat(depth)
        )
    };
    // Jumps that wait for their label while other labels are declared.
    let waiting_jumps = |count: usize| {
        let all: String = (0..count)
            .map(|i| format!("\tgoto End\nL{i}:\n\tgoto L{i}\n"))
            .collect();
        format!("package p\nfunc f() {{\n{all}End:\n}}\n")
    };
    // A function with a result that ends in loops inside loops, each
    // with a `break` that leaves a statement inside it; and one that ends
    // in a chain of `else`.
    let endless_loops = |depth: usize| {
        format!(
            "package p\nfunc f(x bool) int {{\n{}{}\n}}\n",
            "for { switch { case x: break }; ".repeat(depth),
            "}".repeat(depth)
        )
    };
    let else_chain = |count: usize| {
        format!(
            "package p\nfunc f(x bool) int {{\n{}{{ return 0 }}\n}}\n",
            "if x { return 1 } else ".repeat(count)
        )
    };
    // Each loop has a label; its body leaves it and goes on with the
    // outermost one.
    let labeled_loops = |depth: usize| {
        let open: String = (0..depth)
            .map(|i| format!("L{i}: for {{ if x {{ break L{i} }}; if x {{ continue L0 }}; "))
            .collect();
        format!(
            "package p\nfunc f(x bool) {{\n{open}{}\n}}\n",
            "}".repeat(depth)
        )
    };
    linear::assert_near_linear(&[100, 200, 400, 800], |n| steps(nested(n)));
    linear::assert_near_linear(&[100, 200, 400, 800], |n| steps(jumps_out(n)));
    linear::assert_near_linear(&sizes, |n| steps(waiting_jumps(n)));
    linear::assert_near_linear(&[100, 200, 400, 800], |n| steps(labeled_loops(n)));
    linear::assert_near_linear(&[100, 200, 400, 800], |n| steps(endless_loops(n)));
    linear::assert_near_linear(&sizes, |n| steps(else_chain(n)));
    linear::assert_near_linear(&sizes, |n| steps(siblings(n)));
    linear::assert_near_linear(&[100, 200, 400, 800], |n| steps(closures(n)));
    linear::assert_near_linear(&sizes, |n| steps(declarations(n)));
    linear::assert_near_linear(&sizes, |n| steps(stress::wide_block(n)));
    linear::assert_near_linear(&sizes, |n| steps(stress::long_chain(n)));
    linear::assert_near_linear(&sizes, |n| steps(stress::diamonds(n)));
}

#[test]
fn the_result_does_not_depend_on_the_number_of_threads() {
    let functions: String = (0..200)
        .map(|i| {
            format!(
                "func f{i}(a int) {{ unused := a; _ = missing{i}; _ = f{} }}\n",
                (i + 7) % 200
            )
        })
        .collect();
    let text = format!("package p\n{functions}");
    let run = |threads: usize| {
        let pool = rayon::ThreadPoolBuilder::new()
            .num_threads(threads)
            .build()
            .unwrap();
        pool.install(|| {
            let names = ThreadedRodeo::new();
            let checked = check_texts("p", &[("a.gopp", &text)], &Loader::default(), &names);
            let found = names_of(&checked.package, &checked.files, &names).remove(0);
            (checked.messages, found.defs, found.uses)
        })
    };
    let one = run(1);
    assert_eq!(one.0.len(), 400);
    assert_eq!(one, run(8));
}

#[test]
fn a_type_may_not_hold_a_value_of_itself() {
    let cases: &[(&str, &[&str])] = &[
        (
            "type T struct{ t T }",
            &["a.gopp:2:6: invalid recursive type: T refers to itself"],
        ),
        (
            "type T [2]T",
            &["a.gopp:2:6: invalid recursive type: T refers to itself"],
        ),
        (
            "type A struct{ b B }\ntype B struct{ c [3]C }\ntype C struct{ a A }",
            &[
                "a.gopp:2:6: invalid recursive type A",
                "    a.gopp:2:6: A refers to B",
                "    a.gopp:3:6: B refers to C",
                "    a.gopp:4:6: C refers to A",
            ],
        ),
        (
            "type A B\ntype B A",
            &[
                "a.gopp:2:6: invalid recursive type A",
                "    a.gopp:2:6: A refers to B",
                "    a.gopp:3:6: B refers to A",
            ],
        ),
        (
            "type A = B\ntype B = *A",
            &[
                "a.gopp:2:6: invalid recursive type A",
                "    a.gopp:2:6: A refers to B",
                "    a.gopp:3:6: B refers to A",
            ],
        ),
        // An option holds its value in place.
        (
            "type T struct{ next Option[T] }",
            &["a.gopp:2:6: invalid recursive type: T refers to itself"],
        ),
        // Behind a pointer of any kind, in a slice, a map, a channel, a
        // func or an interface, nothing is held.
        (
            "type T struct {\n\ta *T\n\tb []T\n\tc map[string]T\n\td chan T\n\te func(T) T\n\t\
             f interface{ M(T) }\n\tg Option[*T]\n\th Unique[T]\n\ti Shared[T]\n}",
            &[],
        ),
        ("type L[E any] struct{ next *L[E]; v E }\ntype B *B", &[]),
        ("type A struct{ b *B }\ntype B struct{ a A }", &[]),
        (
            "type B struct{ a *A }\ntype A B\nfunc f(x A) *A { return x.a }",
            &[],
        ),
        (
            "type T T",
            &["a.gopp:2:6: invalid recursive type: T refers to itself"],
        ),
        (
            "type I interface{ I }",
            &["a.gopp:2:6: invalid recursive type: I refers to itself"],
        ),
        // A generic type is reported where it is given its arguments.
        (
            "type T[P any] struct{ f T[P] }",
            &["a.gopp:2:25: invalid recursive type: T refers to itself"],
        ),
        (
            "type T[P any] [2]T[*P]",
            &["a.gopp:2:18: invalid recursive type: T refers to itself"],
        ),
        (
            "type N[P any] M[P]\ntype M[P any] N[P]",
            &[
                "a.gopp:2:15: invalid recursive type M",
                "    a.gopp:2:15: M refers to N",
                "    a.gopp:2:15: N refers to M",
            ],
        ),
        (
            "type Pair[P any] struct{ a, b P }\ntype T struct{ p Pair[*T] }",
            &[],
        ),
        (
            "type Pair[P any] struct{ a, b P }\ntype T struct{ p Pair[T] }",
            &[
                "a.gopp:3:6: invalid recursive type T",
                "    a.gopp:3:6: T refers to Pair",
                "    a.gopp:3:18: Pair refers to T",
            ],
        ),
        (
            "func f() {\n\ttype T struct{ t T }\n}",
            &["a.gopp:3:7: invalid recursive type: T refers to itself"],
        ),
        // A type and a value that need each other.
        (
            "type T [len(a)]int\nvar a T",
            &[
                "a.gopp:2:6: invalid recursive type T",
                "    a.gopp:2:6: T refers to a",
                "    a.gopp:3:5: a refers to T",
            ],
        ),
    ];
    for (body, expected) in cases {
        let text = format!("package p\n{body}\n");
        assert_eq!(messages("a.gopp", &text), *expected, "{body}");
    }
}

#[test]
fn a_long_chain_of_generic_types_is_checked() {
    // Wall time is not measured here; this only has to end. As in Go's
    // type checker, a type that is written with a type parameter is looked
    // into again in each place.
    let chain: String = (0..500)
        .map(|i| format!("type G{i}[P any] struct{{ g G{}[P] }}\n", i + 1))
        .collect();
    let text = format!("package p\n{chain}type G500[P any] struct{{ p P }}\n");
    assert_eq!(messages("a.gopp", &text), [] as [&str; 0]);
}

#[test]
fn only_a_function_is_named_init_or_main() {
    let names = ThreadedRodeo::new();
    let text = "package main\nimport init \"example.com/lib\"\ntype main struct{}\n\
                var init int\nfunc main() {}\n";
    let found = check_texts("main", &[("a.gopp", text)], &with_lib(&names), &names).messages;
    assert_eq!(
        found,
        [
            "a.gopp:2:8: cannot import package as init - init must be a func",
            "a.gopp:3:6: cannot declare main - must be func",
            "a.gopp:4:5: cannot declare init - must be func",
        ]
    );
}

#[test]
fn an_import_path_may_name_a_header() {
    let found = messages("a.gopp", "package p\nimport _ \"<vector>\"\n");
    assert_eq!(
        found,
        ["a.gopp:2:10: could not import <vector> (package not found)"]
    );
}

#[test]
fn a_statement_in_error_is_reported_once() {
    let cases: &[(&str, &[&str])] = &[
        // Nothing of a select clause that does not communicate is looked at.
        (
            "func f() {\n\tselect {\n\tcase x:\n\t\ty := 1\n\t}\n}",
            &["a.gopp:4:7: select case must be send or receive (possibly with assignment)"],
        ),
        (
            "func f(c chan int) {\n\tselect {\n\tcase v := <-c:\n\t\t_ = v\n\tcase c <- 1:\n\t}\n}",
            &[],
        ),
        (
            "func f() {\n\tfor i := 0; i < 3; j := 1 {\n\t}\n}",
            &["a.gopp:3:21: syntax error: cannot declare in post statement of for loop"],
        ),
        // What cannot be ranged over gives its variable no type: the
        // variable is not also unused.
        (
            "func f() {\n\tfor x := range 1.5 {\n\t}\n}",
            &["a.gopp:3:17: cannot range over 1.5 (untyped float constant)"],
        ),
        (
            "func f(s []int) {\n\tfor _, x := range s {\n\t}\n}",
            &["a.gopp:3:9: declared and not used: x"],
        ),
    ];
    for (body, expected) in cases {
        let text = format!("package p\n{body}\n");
        assert_eq!(messages("a.gopp", &text), *expected, "{body}");
    }
}

#[test]
fn a_name_as_the_length_of_an_array_is_a_constant() {
    let text = "package p\nvar v int\nconst c = 2\nvar (\n\ta [n]int\n\tb [v]int\n\td [c]int\n)\n";
    assert_eq!(
        messages("a.gopp", text),
        [
            "a.gopp:5:5: undefined array length n or missing type constraint",
            "a.gopp:6:5: invalid array length v",
        ]
    );
}

#[test]
fn a_name_of_a_package_that_differs_in_its_capitals_is_suggested() {
    let names = ThreadedRodeo::new();
    let text = "package p\nimport \"example.com/lib\"\nvar _ = lib.EXPORTED\nvar _ = lib.Other\n";
    let found = check_texts("p", &[("a.gopp", text)], &with_lib(&names), &names).messages;
    assert_eq!(
        found,
        [
            "a.gopp:3:13: undefined: lib.EXPORTED (but have Exported)",
            "a.gopp:4:13: undefined: lib.Other",
        ]
    );
}

#[test]
fn the_signature_of_a_function_is_not_needed_at_the_start() {
    // `r` in the signature is an error of its own; `x` in the length of
    // the result is no need of `g`.
    let text = "package p\nvar r = newReader()\nfunc newReader() r\n\
                func g() [len(x)]int\nvar x = [2]int{}\nvar y = g\n";
    assert_eq!(messages("a.gopp", text), ["a.gopp:3:18: r is not a type"]);
}

#[test]
fn operators_are_checked_as_go_checks_them() {
    let declared = "var (\n\ta int\n\tb string\n\tt bool\n\tu uint\n\tp = &a\n\tf = h\n\t\
                    s struct{ b []int }\n\ti any = 0\n\ty uint8\n)\nconst c int8 = 100\n\
                    func g() (int, int)\nfunc h()\n";
    let cases: &[(&str, &[&str])] = &[
        (
            "a + b",
            &["16:9: invalid operation: a + b (mismatched types int and string)"],
        ),
        (
            "-t",
            &["16:10: invalid operation: operator - not defined on t (variable of type bool)"],
        ),
        (
            "b - b",
            &["16:9: invalid operation: operator - not defined on b (variable of type string)"],
        ),
        (
            "c * 2",
            &["16:9: c * 2 (constant 200 of type int8) overflows int8"],
        ),
        (
            "y + 300",
            &["16:13: 300 (untyped int constant) overflows uint8"],
        ),
        ("a / 0", &["16:13: invalid operation: division by zero"]),
        ("1.5 / 0", &["16:15: invalid operation: division by zero"]),
        (
            "1 << -1",
            &["16:14: invalid operation: negative shift count -1 (untyped int constant)"],
        ),
        (
            "1 << 1.5",
            &["16:14: 1.5 (untyped float constant) truncated to uint"],
        ),
        (
            "1 << 2000",
            &["16:14: invalid operation: invalid shift count 2000 (untyped int constant)"],
        ),
        ("1 << 600", &["16:11: constant shift overflow"]),
        (
            "b << 1",
            &[
                "16:9: invalid operation: shifted operand b (variable of type string) must be integer",
            ],
        ),
        (
            "a << b",
            &["16:14: invalid operation: shift count b (variable of type string) must be integer"],
        ),
        // A pointer is compared as what it points to.
        ("p == 1", &[]),
        (
            "f == f",
            &["16:9: invalid operation: f == f (func values cannot be compared)"],
        ),
        (
            "s == s",
            &["16:9: invalid operation: s == s (struct containing []int cannot be compared)"],
        ),
        (
            "t < t",
            &["16:9: invalid operation: t < t (operator < not defined on bool)"],
        ),
        (
            "a == nil",
            &["16:14: invalid operation: a == nil (mismatched types int and untyped nil)"],
        ),
        (
            "-g()",
            &["16:10: multiple-value g() (value of type (int, int)) in single-value context"],
        ),
        ("1 + h()", &["16:13: h() (no value) used as value"]),
        ("1 + int", &["16:13: int (type) is not an expression"]),
        ("1 + len", &["16:13: len (built-in) must be called"]),
        (
            "&a == &1",
            &["16:16: invalid operation: cannot take address of 1 (untyped int constant)"],
        ),
        // What is right next to each of them.
        ("a + 1 + a<<u + a/2 - a%3&^a", &[]),
        ("b + \"x\" < b", &[]),
        ("1.0<<3 + 1<<u == a", &[]),
        ("i == 1 || p != &a", &[]),
        ("c*1 - c/3 == -c && !t", &[]),
        ("y + 255 + 'a' + ^y", &[]),
        ("s.b == nil", &[]),
        ("float64(a<<u) + 1.5/2 > 0", &[]),
    ];
    for (expr, expected) in cases {
        let text = format!("package p\n{declared}var _ = {expr}\n");
        let found = messages("a.gopp", &text);
        let found: Vec<&str> = found
            .iter()
            .map(|message| message.strip_prefix("a.gopp:").unwrap_or(message))
            .collect();
        assert_eq!(found, *expected, "{expr}");
    }
    // A constant of no type that is shifted gets its type from around it.
    let text = "package p\nfunc f(s uint) {\n\tvar x float64 = 1 << s\n\tvar y int64 = 1 << s\n\t\
                _, _ = x, y\n}\n";
    assert_eq!(
        messages("a.gopp", text),
        ["a.gopp:3:18: invalid operation: shifted operand 1 (type float64) must be integer"]
    );
}

#[test]
fn assignments_are_checked_as_go_checks_them() {
    let declared = "var (\n\ta int\n\tb string\n\tp = &a\n\tc chan int\n\ts []int\n\t\
                    m map[string]struct{ x int }\n\ti any\n)\n\
                    func two() (int, string)\ntype T struct{}\nfunc (*T) ptr() {}\n";
    let cases: &[(&str, &[&str])] = &[
        (
            "a = b",
            &["16:6: cannot use b (variable of type string) as int value in assignment"],
        ),
        (
            "a = 1.5",
            &[
                "16:6: cannot use 1.5 (untyped float constant) as int value in assignment (truncated)",
            ],
        ),
        (
            "var x int8 = 300\n\t_ = x",
            &[
                "16:15: cannot use 300 (untyped int constant) as int8 value in variable declaration \
               (overflows)",
            ],
        ),
        (
            "a = nil",
            &["16:6: cannot use nil as int value in assignment"],
        ),
        (
            "x := nil\n\t_ = x",
            &["16:7: use of untyped nil in assignment"],
        ),
        (
            "a, b = 1",
            &["16:9: assignment mismatch: 2 variables but 1 value"],
        ),
        (
            "a = two()",
            &["16:6: assignment mismatch: 1 variable but two returns 2 values"],
        ),
        (
            "m[\"k\"].x = 1",
            &["16:2: cannot assign to struct field m[\"k\"].x in map"],
        ),
        (
            "len(s) = 1",
            &["16:2: cannot assign to len(s) (neither addressable nor a map index expression)"],
        ),
        (
            "c <- b",
            &["16:7: cannot use b (variable of type string) as int value in send"],
        ),
        ("var x = 1, 2\n\t_ = x", &["16:13: extra init expr 2"]),
        ("const k, l = 1", &["16:11: missing init expr for l"]),
        (
            "const k int = a",
            &["16:16: a (variable of type int) is not constant"],
        ),
        (
            "const k []int = nil",
            &[
                "16:10: invalid constant type []int: the buffer of a []int is made by the running \
                 program",
            ],
        ),
        (
            "const k int8 = 1 << 7",
            &[
                "16:17: cannot use 1 << 7 (untyped int constant 128) as int8 value in constant \
               declaration (overflows)",
            ],
        ),
        (
            "_ = int(b)",
            &["16:10: cannot convert b (variable of type string) to type int"],
        ),
        ("_ = int8(300)", &["16:11: constant 300 overflows int8"]),
        (
            "_ = int(1.5)",
            &["16:10: cannot convert 1.5 (untyped float constant) to type int"],
        ),
        (
            "_ = int()",
            &["16:10: missing argument in conversion to int"],
        ),
        (
            "_ = int(1, 2)",
            &["16:13: too many arguments in conversion to int"],
        ),
        (
            "_ = T.ptr",
            &["16:8: invalid method expression T.ptr (needs pointer receiver (*T).ptr)"],
        ),
        // What is right next to each of them.
        (
            "a, b = two()\n\ta, b = 1, \"x\"\n\tm[\"k\"] = struct{ x int }{1}\n\ts[0], p = 1, 2",
            &[],
        ),
        (
            "c <- 1\n\ti = 1.5\n\ti = nil\n\tvar f float32 = 1\n\t_ = f",
            &[],
        ),
        (
            "const k int8 = 1<<7 - 1\n\tconst l, n = 1, k\n\tconst z = len(\"ab\") + len([2]int{})",
            &[],
        ),
        (
            "_ = int(1.0) + int(a) + int(float64(a)) + len(string(rune(a))+string(s2b(b)))",
            &[],
        ),
        (
            "_ = (*T).ptr\n\tvar v, ok = i.(int)\n\t_, _ = v, ok\n\tx, y := <-c\n\t_, _ = x, y",
            &[],
        ),
    ];
    for (body, expected) in cases {
        let text = format!(
            "package p\n{declared}func s2b(s string) []byte {{ return []byte(s) }}\n\
             func _() {{\n\t{body}\n}}\n"
        );
        let found = messages("a.gopp", &text);
        let found: Vec<&str> = found
            .iter()
            .map(|message| message.strip_prefix("a.gopp:").unwrap_or(message))
            .collect();
        assert_eq!(found, *expected, "{body}");
    }
    // The values of a return are those of the results.
    let cases: &[(&str, &[&str])] = &[
        (
            "return",
            &[
                "3:2: not enough return values",
                "    have ()",
                "    want (int, string)",
            ],
        ),
        (
            "return 1",
            &[
                "3:9: not enough return values",
                "    have (number)",
                "    want (int, string)",
            ],
        ),
        (
            "return 1, \"a\", 2.5",
            &[
                "3:17: too many return values",
                "    have (number, string, number)",
                "    want (int, string)",
            ],
        ),
        (
            "return \"a\", 1",
            &[
                "3:9: cannot use \"a\" (untyped string constant) as int value in return statement",
                "3:14: cannot use 1 (untyped int constant) as string value in return statement",
            ],
        ),
        ("return 1, \"a\"", &[]),
        ("return f()", &[]),
    ];
    for (body, expected) in cases {
        let text = format!("package p\nfunc f() (int, string) {{\n\t{body}\n}}\n");
        let found = messages("a.gopp", &text);
        let found: Vec<&str> = found
            .iter()
            .map(|message| message.strip_prefix("a.gopp:").unwrap_or(message))
            .collect();
        assert_eq!(found, *expected, "{body}");
    }
    // A function with named results returns them with `return` alone.
    let text = "package p\nfunc f() (n int, err error) {\n\treturn\n}\n";
    assert_eq!(messages("a.gopp", text), [] as [&str; 0]);
    // An assignment may write through a pointer.
    let text = "package p\nfunc f(p *int) {\n\tp = 9\n}\n";
    assert_eq!(messages("a.gopp", text), [] as [&str; 0]);
}

#[test]
fn calls_and_indexes_are_checked_as_go_checks_them() {
    let declared = "var (\n\ta int\n\tb string\n\ts []int\n\tarr [4]int\n\tm map[string]int\n\t\
                    p = &arr\n)\nfunc f(x int, y string)\nfunc v(xs ...int)\n\
                    func two() (int, string)\ntype G[P any] struct{ p P }\n";
    let cases: &[(&str, &[&str])] = &[
        (
            "f(1)",
            &[
                "15:5: not enough arguments in call to f",
                "    have (number)",
                "    want (int, string)",
            ],
        ),
        (
            "f(1, \"a\", 2)",
            &[
                "15:12: too many arguments in call to f",
                "    have (number, string, number)",
                "    want (int, string)",
            ],
        ),
        (
            "f(b, a)",
            &[
                "15:4: cannot use b (variable of type string) as int value in argument to f",
                "15:7: cannot use a (variable of type int) as string value in argument to f",
            ],
        ),
        (
            "v(1, s...)",
            &[
                "15:7: too many arguments in call to v",
                "    have (number, []int...)",
                "    want (...int)",
            ],
        ),
        (
            "v(b)",
            &["15:4: cannot use b (variable of type string) as int value in argument to v"],
        ),
        (
            "f(s...)",
            &["15:5: cannot use ... in call to non-variadic f"],
        ),
        ("a", &["15:2: a (variable of type int) is not used"]),
        ("int(a)", &["15:2: int(a) (value of type int) is not used"]),
        ("len", &["15:2: len (built-in) must be called"]),
        (
            "_ = s[1.5]",
            &["15:8: 1.5 (untyped float constant) truncated to int"],
        ),
        (
            "_ = s[b]",
            &["15:8: invalid argument: index b (variable of type string) must be integer"],
        ),
        (
            "_ = s[-1]",
            &["15:8: invalid argument: index -1 (constant of type int) must not be negative"],
        ),
        (
            "_ = arr[4]",
            &["15:10: invalid argument: index 4 out of bounds [0:4]"],
        ),
        ("_ = a[0]", &["15:6: cannot index a (variable of type int)"]),
        (
            "_ = m[1]",
            &["15:8: cannot use 1 (untyped int constant) as string value in map index"],
        ),
        ("_ = arr[3:2]", &["15:12: invalid slice indices: 2 < 3"]),
        (
            "_ = arr[:5]",
            &["15:11: invalid argument: index 5 out of bounds [0:5]"],
        ),
        (
            "_ = b[1:2:3]",
            &["15:12: invalid operation: 3-index slice of string"],
        ),
        (
            "_ = [2]int{}[:]",
            &["15:6: cannot slice unaddressable value [2]int{} (value of type [2]int)"],
        ),
        ("_ = a[:]", &["15:6: cannot slice a (variable of type int)"]),
        (
            "var g G\n\t_ = g",
            &["15:8: cannot use generic type G[P any] without instantiation"],
        ),
        ("var g a[int]\n\t_ = g", &["15:8: a is not a type"]),
        (
            "var g map[int]int[string]\n\t_ = g",
            &["15:16: invalid operation: int[string] (int is not a generic type)"],
        ),
        // What is right next to each of them.
        (
            "f(a, b)\n\tf(two())\n\tv()\n\tv(1, 2)\n\tv(s...)\n\tpanic(0)\n\t<-make(chan int)",
            &[],
        ),
        (
            "_ = s[a] + arr[3] + p[3] + m[\"k\"] + int(b[0]) + s[1<<2]",
            &[],
        ),
        ("_, _, _, _ = b[1:], arr[1:2:3], p[:4], s[a:]", &[]),
        ("var g G[int]\n\t_ = g", &[]),
    ];
    for (body, expected) in cases {
        let text = format!("package p\n{declared}func _() {{\n\t{body}\n}}\n");
        let found = messages("a.gopp", &text);
        let found: Vec<&str> = found
            .iter()
            .map(|message| message.strip_prefix("a.gopp:").unwrap_or(message))
            .collect();
        assert_eq!(found, *expected, "{body}");
    }
}

#[test]
fn built_in_functions_are_checked_as_go_checks_them() {
    let declared = "var (\n\ta int\n\tb string\n\ts []int\n\tbs []byte\n\tarr [4]int\n\t\
                    m map[string]int\n\tc chan int\n\tr <-chan int\n\tf float64\n\tz complex128\n)\n";
    let cases: &[(&str, &[&str])] = &[
        (
            "_ = len()",
            &["15:10: invalid operation: not enough arguments for len() (expected 1, found 0)"],
        ),
        (
            "_ = len(s, s)",
            &["15:14: invalid operation: too many arguments for len(s, s) (expected 1, found 2)"],
        ),
        (
            "_ = len(s...)",
            &["15:11: invalid operation: invalid use of ... with built-in len"],
        ),
        (
            "_ = len(a)",
            &["15:10: invalid argument: a (variable of type int) for built-in len"],
        ),
        (
            "_ = cap(m)",
            &["15:10: invalid argument: m (variable of type map[string]int) for built-in cap"],
        ),
        (
            "_ = append(a, 1)",
            &["15:13: invalid append: argument must be a slice; have a (variable of type int)"],
        ),
        (
            "_ = append(s, b)",
            &["15:16: cannot use b (variable of type string) as int value in argument to append"],
        ),
        (
            "clear(a)",
            &[
                "15:8: invalid argument: cannot clear a (variable of type int): argument must be (or \
               constrained by) map or slice",
            ],
        ),
        (
            "close(a)",
            &["15:8: invalid operation: cannot close non-channel a (variable of type int)"],
        ),
        (
            "close(r)",
            &[
                "15:8: invalid operation: cannot close receive-only channel r (variable of type \
               <-chan int)",
            ],
        ),
        (
            "copy(&s, b)",
            &[
                "15:7: invalid copy: arguments &s (variable of type []int) and b (variable of type \
               string) have different element types int and byte",
            ],
        ),
        (
            "copy(a, s)",
            &["15:7: invalid copy: argument must be a slice; have a (variable of type int)"],
        ),
        (
            "delete(s, 1)",
            &["15:9: invalid argument: s (variable of type []int) is not a map"],
        ),
        (
            "delete(m, 1)",
            &["15:12: cannot use 1 (untyped int constant) as string value in argument to delete"],
        ),
        (
            "_ = make(int)",
            &["15:11: invalid argument: cannot make int: type must be slice, map, or channel"],
        ),
        (
            "_ = make([]int)",
            &["15:6: invalid operation: make([]int) expects 2 or 3 arguments; found 1"],
        ),
        (
            "_ = make([]int, -1)",
            &["15:18: invalid argument: index -1 (constant of type int) must not be negative"],
        ),
        (
            "_ = make([]int, 3, 2)",
            &["15:18: invalid argument: length and capacity swapped"],
        ),
        (
            "_ = min(a, f)",
            &[
                "15:13: invalid argument: mismatched types int (previous argument) and float64 \
               (type of f)",
            ],
        ),
        (
            "_ = max(a, true)",
            &["15:13: invalid argument: true (untyped bool constant) cannot be ordered"],
        ),
        (
            "_ = complex(a, a)",
            &["15:14: invalid argument: arguments have type int, expected floating-point"],
        ),
        (
            "_ = complex(f, float32(1))",
            &[
                "15:14: invalid operation: complex(f, float32(1)) (mismatched types float64 and \
               float32)",
            ],
        ),
        (
            "_ = real(f)",
            &["15:11: invalid argument: argument has type float64, expected complex type"],
        ),
        (
            "x := panic(0)\n\t_ = x",
            &["15:7: panic(0) (no value) used as value"],
        ),
        ("len(s)", &["15:2: len(s) (value of type int) is not used"]),
        (
            "println(nil)",
            &["15:10: use of untyped nil in argument to built-in println"],
        ),
        // What is right next to each of them.
        (
            "_ = len(s) + len(b) + len(arr) + len(m) + len(c) + cap(s) + cap(arr) + cap(c)",
            &[],
        ),
        (
            "s = append(s, 1, 2)\n\ts = append(s, s...)\n\tbs = append(bs, b...)\n\t\
             _ = copy(&s, s) + copy(&bs, b)",
            &[],
        ),
        (
            "clear(m)\n\tclear(s)\n\tclose(c)\n\tdelete(m, b)\n\tpanic(b)\n\tprint(a, b)",
            &[],
        ),
        (
            "_, _, _ = make([]int, 2, 3), make(map[string]int), make(chan int, a)",
            &[],
        ),
        (
            "const k = min(1, 2.5) + max(3, 4)\n\t_ = min(a, 2) + max(a)\n\t_ = min(b, \"x\")",
            &[],
        ),
        (
            "_ = complex(f, 1) + complex(1, 2) + z\n\t_ = real(z) + imag(z) + f\n\t\
             const i = real(2i) + imag(3)",
            &[],
        ),
    ];
    for (body, expected) in cases {
        let text = format!("package p\n{declared}func _() {{\n\t{body}\n}}\n");
        let found = messages("a.gopp", &text);
        let found: Vec<&str> = found
            .iter()
            .map(|message| message.strip_prefix("a.gopp:").unwrap_or(message))
            .collect();
        assert_eq!(found, *expected, "{body}");
    }
}

#[test]
fn composite_literals_are_checked_as_go_checks_them() {
    let declared = "type P struct{ x, y int }\nvar a int\nvar b string\nconst k = 1\n";
    let cases: &[(&str, &[&str])] = &[
        ("P{1}", &["7:9: too few values in struct literal of type P"]),
        (
            "P{1, 2, 3}",
            &["7:14: too many values in struct literal of type P"],
        ),
        (
            "P{x: 1, 2}",
            &["7:14: mixture of field:value and value elements in struct literal"],
        ),
        (
            "P{z: 1}",
            &["7:8: unknown field z in struct literal of type P"],
        ),
        (
            "P{X: 1}",
            &["7:8: unknown field X in struct literal of type P, but does have x"],
        ),
        (
            "P{x: 1, x: 2}",
            &["7:14: duplicate field name x in struct literal"],
        ),
        (
            "P{x: b}",
            &["7:11: cannot use b (variable of type string) as int value in struct literal"],
        ),
        (
            "[]int{0: 1, 0: 2}",
            &["7:18: duplicate index 0 in array or slice literal"],
        ),
        (
            "[2]int{1, 2, 3}",
            &["7:19: index 2 is out of bounds (>= 2)"],
        ),
        ("[]int{a: 1}", &["7:12: index a must be integer constant"]),
        (
            "[]int{b}",
            &[
                "7:12: cannot use b (variable of type string) as int value in array or slice literal",
            ],
        ),
        (
            "map[string]int{\"a\": 1, \"a\": 2}",
            &["7:29: duplicate key \"a\" in map literal"],
        ),
        ("map[string]int{1}", &["7:21: missing key in map literal"]),
        (
            "map[string]int{a: 1}",
            &["7:21: cannot use a (variable of type int) as string value in map literal"],
        ),
        ("map[string]int{nokey: 1}", &["7:21: undefined: nokey"]),
        ("int{}", &["7:6: invalid composite literal type int"]),
        // What is right next to each of them.
        ("[]any{P{}, P{1, 2}, P{y: 2}, []P{{1, 2}, {x: 1}}}", &[]),
        ("map[P]string{{1, 2}: \"a\", {3, 4}: \"b\"}", &[]),
        (
            "[]any{[...]int{5: 1, 2}, map[string]int{b: a, \"k\": 2}, []int{k: 1, 2}}",
            &[],
        ),
    ];
    for (literal, expected) in cases {
        let text = format!("package p\n{declared}func _() {{\n\t_ = {literal}\n}}\n");
        let found = messages("a.gopp", &text);
        let found: Vec<&str> = found
            .iter()
            .map(|message| message.strip_prefix("a.gopp:").unwrap_or(message))
            .collect();
        assert_eq!(found, *expected, "{literal}");
    }
}

#[test]
fn selectors_and_channels_are_checked_as_go_checks_them() {
    let declared = "type S struct {\n\tx  int\n\tUp int\n}\nfunc (S) val()  {}\nfunc (*S) ptr() {}\n\
                    type E1 struct{ f int }\ntype E2 struct{ f int }\ntype B struct {\n\te1 E1\n\te2 E2\n}\n\
                    var (\n\ts    S\n\tb    B\n\tp    = &s\n\ta    int\n\tsend chan<- int\n\t\
                    recv <-chan int\n\tm    map[string]S\n)\n";
    let cases: &[(&str, &[&str])] = &[
        (
            "_ = s.y",
            &["24:8: s.y undefined (type S has no field or method y)"],
        ),
        (
            "_ = s.X",
            &["24:8: s.X undefined (type S has no field or method X, but does have field x)"],
        ),
        (
            "_ = s.up",
            &["24:8: s.up undefined (type S has no field or method up, but does have field Up)"],
        ),
        (
            "_ = s.Val",
            &[
                "24:8: s.Val undefined (type S has no field or method Val, but does have method val)",
            ],
        ),
        (
            "_ = a.x",
            &["24:8: a.x undefined (type int has no field or method x)"],
        ),
        (
            "_ = *a",
            &["24:7: invalid operation: cannot indirect a (variable of type int)"],
        ),
        (
            "_ = <-a",
            &[
                "24:8: invalid operation: cannot receive from non-channel int a (variable of type \
               int)",
            ],
        ),
        (
            "_ = <-send",
            &[
                "24:8: invalid operation: cannot receive from send-only channel chan<- int send \
               (variable of type chan<- int)",
            ],
        ),
        (
            "recv <- 1",
            &[
                "24:7: invalid operation: cannot send to receive-only channel <-chan int recv \
               (variable of type <-chan int)",
            ],
        ),
        // What is right next to each of them.
        (
            "s.val()\n\ts.ptr()\n\tp.val()\n\tp.ptr()\n\t(&s).ptr()\n\tS{}.val()",
            &[],
        ),
        ("_ = s.x + s.Up + b.e1.f + b.e2.f + p.x + *&a", &[]),
        (
            "_ = <-recv\n\tsend <- 1\n\tf, g := S.val, (*S).ptr\n\t_, _ = f, g",
            &[],
        ),
    ];
    for (body, expected) in cases {
        let text = format!("package p\n{declared}func _() {{\n\t{body}\n}}\n");
        let found = messages("a.gopp", &text);
        let found: Vec<&str> = found
            .iter()
            .map(|message| message.strip_prefix("a.gopp:").unwrap_or(message))
            .collect();
        assert_eq!(found, *expected, "{body}");
    }
    // A method may be named in the signature of one that is declared
    // before it, and the receiver may be an alias of a pointer type.
    let text = "package p\nimport \"unsafe\"\ntype T struct{}\ntype P = *T\n\
                func (T) a([unsafe.Sizeof(T.b)]int) {}\nfunc (T) b()                       {}\n\
                func (P) c()                       {}\nfunc _(t T) { (&t).c() }\n";
    let names = ThreadedRodeo::new();
    let mut loader = Loader::default();
    let stub = "package unsafe\nfunc Sizeof(x any) uintptr\n";
    let stub = check_texts("unsafe", &[("unsafe.gopp", stub)], &loader, &names);
    loader
        .packages
        .insert("unsafe".to_owned(), Arc::new(stub.package));
    let found = check_texts("p", &[("a.gopp", text)], &loader, &names).messages;
    assert_eq!(found, [] as [&str; 0]);
}

#[test]
fn what_is_asserted_or_switched_on_is_an_interface_value() {
    let declared = "type I interface{ m() }; type M int; func (M) m() {}\nvar (\n\ta int\n\ti I = M(0)\n\t\
                    e any\n)\n";
    let cases: &[(&str, &[&str])] = &[
        (
            "_ = a.(int)",
            &["9:6: invalid operation: a (variable of type int) is not an interface"],
        ),
        (
            "switch a.(type) {\n\t}",
            &["9:9: a (variable of type int) is not an interface"],
        ),
        (
            "_ = p.(int)",
            &[
                "9:6: invalid operation: cannot use type assertion on type parameter value p \
                 (variable of type P constrained by any)",
            ],
        ),
        (
            "switch p.(type) {\n\t}",
            &[
                "9:9: cannot use type switch on type parameter value p (variable of type P \
                 constrained by any)",
            ],
        ),
        // What is right next to each of them.
        ("_ = i.(I)\n\t_, _ = e.(int)\n\t_ = any(a).(int)", &[]),
        (
            "switch i.(type) {\n\t}\n\tswitch v := e.(type) {\n\tcase int:\n\t\t_ = v\n\t}",
            &[],
        ),
    ];
    for (body, expected) in cases {
        let text = format!("package p\n{declared}func _[P any](p P) {{\n\t{body}\n}}\n");
        let found = messages("a.gopp", &text);
        let found: Vec<&str> = found
            .iter()
            .map(|message| message.strip_prefix("a.gopp:").unwrap_or(message))
            .collect();
        assert_eq!(found, *expected, "{body}");
    }
}

/// A generic function and a plain one may have the same parameters and
/// results; only the first is generic.
#[test]
fn a_function_is_not_generic_for_what_another_one_is() {
    let text = "package p\nfunc g[T any]() {}\nfunc f()        {}\nfunc _() {\n\t_ = f()\n\tf(1)\n\
                \tg[int]()\n\tf()\n}\n";
    assert_eq!(
        messages("a.gopp", text),
        [
            "a.gopp:5:6: f() (no value) used as value",
            "a.gopp:6:4: too many arguments in call to f",
            "    have (number)",
            "    want ()",
        ]
    );
}

#[test]
fn what_stands_for_a_type_or_an_unsafe_argument_is_checked_as_go_checks_it() {
    let names = ThreadedRodeo::new();
    let mut loader = Loader::default();
    let stub = "package unsafe\ntype ArbitraryType int\ntype IntegerType int\n\
                type Pointer *ArbitraryType\nfunc Sizeof(x ArbitraryType) uintptr\n\
                func Offsetof(x ArbitraryType) uintptr\nfunc Alignof(x ArbitraryType) uintptr\n\
                func Add(ptr Pointer, len IntegerType) Pointer\n\
                func Slice(ptr *ArbitraryType, len IntegerType) []ArbitraryType\n\
                func SliceData(slice []ArbitraryType) *ArbitraryType\n\
                func String(ptr *byte, len IntegerType) string\nfunc StringData(str string) *byte\n";
    let stub = check_texts("unsafe", &[("unsafe.gopp", stub)], &loader, &names);
    loader
        .packages
        .insert("unsafe".to_owned(), Arc::new(stub.package));
    let declared = "import \"unsafe\"\ntype S struct {\n\tf int\n\te E\n}\ntype E struct{ g int }\n\
                    func (S) m() {}\nvar (\n\ts  S\n\tx  int\n\tb  byte\n\tsl []int\n\
                    \tp  unsafe.Pointer\n)\n";
    let cases: &[(&str, &[&str])] = &[
        (
            "_ = unsafe.Sizeof(nil)",
            &["17:20: use of untyped nil in argument to unsafe.Sizeof"],
        ),
        (
            "unsafe.Sizeof(x)",
            &["17:2: unsafe.Sizeof(x) (constant 8 of type uintptr) is not used"],
        ),
        (
            "_ = unsafe.Offsetof(x)",
            &["17:22: invalid argument: x is not a selector expression"],
        ),
        (
            "_ = unsafe.Offsetof(s.m)",
            &["17:22: invalid argument: s.m is a method value"],
        ),
        (
            "_ = unsafe.Offsetof(s.h)",
            &["17:22: invalid argument: S has no single field h"],
        ),
        (
            "_ = unsafe.Slice(1, 2)",
            &["17:19: invalid argument: 1 (untyped int constant) is not a pointer"],
        ),
        (
            "_ = unsafe.Slice(&x, -1)",
            &["17:23: invalid argument: length -1 (constant of type int) must not be negative"],
        ),
        (
            "_ = unsafe.SliceData(0)",
            &["17:23: invalid argument: 0 (untyped int constant) is not a slice"],
        ),
        (
            "_ = unsafe.String(1, 2)",
            &[
                "17:20: cannot use 1 (untyped int constant) as *byte value in argument to \
                 unsafe.String",
            ],
        ),
        (
            "_ = unsafe.StringData(0)",
            &[
                "17:24: cannot use 0 (untyped int constant) as string value in argument to \
                 unsafe.StringData",
            ],
        ),
        (
            "_ = unsafe.Add(x, 1)",
            &[
                "17:17: cannot use x (variable of type int) as unsafe.Pointer value in argument \
                 to unsafe.Add",
            ],
        ),
        (
            "_ = iota",
            &["17:6: cannot use iota outside constant declaration"],
        ),
        ("_ = make(x)", &["17:11: x is not a type"]),
        (
            "_ = [...]int(sl)",
            &["17:7: invalid use of [...] array (outside a composite literal)"],
        ),
        (
            "var _ unsafe.Sizeof",
            &["17:8: unsafe.Sizeof (built-in) is not a type"],
        ),
        ("var _ s.f", &["17:10: s.f is not a type"]),
        (
            "switch any(x).(type) {\n\tcase 1:\n\tcase x:\n\t}",
            &["18:7: 1 is not a type", "19:7: x is not a type"],
        ),
        // What is right next to each of them.
        (
            "_ = unsafe.Sizeof(x) + unsafe.Alignof(s) + unsafe.Offsetof(s.f)",
            &[],
        ),
        (
            "_ = unsafe.Slice(&x, 1)\n\t_ = unsafe.SliceData(sl)\n\t_ = unsafe.String(&b, 1)\n\
             \t_ = unsafe.StringData(\"a\")\n\t_ = unsafe.Add(p, -1)",
            &[],
        ),
        ("_ = [...]int{1, 2}\n\tconst c = iota", &[]),
        (
            "switch any(x).(type) {\n\tcase nil, int:\n\tcase (S):\n\t}",
            &[],
        ),
    ];
    for (body, expected) in cases {
        let text = format!("package p\n{declared}func _() {{\n\t{body}\n}}\n");
        let found = check_texts("p", &[("a.gopp", &text)], &loader, &names).messages;
        let found: Vec<&str> = found
            .iter()
            .map(|message| message.strip_prefix("a.gopp:").unwrap_or(message))
            .collect();
        assert_eq!(found, *expected, "{body}");
    }
    assert_eq!(
        messages("a.gopp", "package p\ntype T T.x\n"),
        ["a.gopp:2:6: invalid recursive type: T refers to itself"]
    );
    // A size that an `int64` does not count, next to one that it does.
    let text = "package p\nimport \"unsafe\"\ntype E [1 << 30]complex128\nvar a [1 << 30]E\n\
                var _ = unsafe.Sizeof(a)\nvar b [1 << 20]E\nvar _ = unsafe.Sizeof(b)\n";
    assert_eq!(
        check_texts("p", &[("a.gopp", text)], &loader, &names).messages,
        ["a.gopp:5:23: a (variable of type [1073741824]E) is too large"]
    );
}

#[test]
fn statements_that_compute_and_assign_are_checked_as_go_checks_them() {
    let declared = "func g() int { return 0 }\ntype myint int\nconst (\n\tsmall int8  = 1\n\
                    \tbig   myint = 1 << 100\n)\n";
    let locals = "const c = 3.14\n\tvar (\n\t\tx int\n\t\ts string\n\t\tf float64\n\t\tb bool\n\
                  \t\tu uint8\n\t\tm map[string]int\n\t)\n\t_, _, _, _, _, _ = x, s, f, b, u, m";
    let overflows = "6:16: cannot use 1 << 100 (untyped int constant \
                     1267650600228229401496703205376) as myint value in constant declaration \
                     (overflows)";
    let cases: &[(&str, &[&str])] = &[
        (
            "c++",
            &["19:2: cannot assign to c (neither addressable nor a map index expression)"],
        ),
        (
            "s--",
            &["19:2: invalid operation: s-- (non-numeric type string)"],
        ),
        (
            "g()++",
            &["19:2: cannot assign to g() (neither addressable nor a map index expression)"],
        ),
        (
            "x += \"a\"",
            &["19:2: invalid operation: x += \"a\" (mismatched types int and untyped string)"],
        ),
        (
            "s -= \"a\"",
            &["19:2: invalid operation: operator - not defined on s (variable of type string)"],
        ),
        (
            "f <<= 2",
            &[
                "19:2: invalid operation: shifted operand f (variable of type float64) must be \
                 integer",
            ],
        ),
        ("x /= 0", &["19:7: invalid operation: division by zero"]),
        (
            "c += 1",
            &["19:2: cannot assign to c (neither addressable nor a map index expression)"],
        ),
        (
            "x += f",
            &["19:2: invalid operation: x += f (mismatched types int and float64)"],
        ),
        (
            "u += 300",
            &["19:7: 300 (untyped int constant) overflows uint8"],
        ),
        (
            "type d int\n\tx, c, d := 1, 2, 3",
            &["20:5: cannot assign to c", "20:8: cannot assign to d"],
        ),
        // A constant that is declared with a type has it whatever its
        // value is.
        (
            "_ = big / small",
            &["19:6: invalid operation: big / small (mismatched types myint and int8)"],
        ),
        ("var _ [big]int", &[]),
        (
            "var _ [-1]int",
            &["19:9: invalid array length -1 (untyped int constant)"],
        ),
        (
            "var _ [2.5]int",
            &["19:9: array length 2.5 (untyped float constant) must be integer"],
        ),
        (
            "var _ [g()]int",
            &["19:9: cannot call non-@Const function g in constant expression"],
        ),
        // What is right next to each of them.
        (
            "x++\n\tf--\n\tm[\"a\"]++\n\tx += 1\n\ts += \"a\"\n\tx <<= 2\n\tu += 255\n\tx %= 2",
            &[],
        ),
        (
            "_ = big / big\n\tvar _ [2.0]int\n\tvar _ [len(\"abc\")]int\n\tx, y := 1, 2\n\t_ = y",
            &[],
        ),
    ];
    for (body, expected) in cases {
        let text = format!("package p\n{declared}func _() {{\n\t{locals}\n\t{body}\n}}\n");
        let found = messages("a.gopp", &text);
        let found: Vec<&str> = found
            .iter()
            .map(|message| message.strip_prefix("a.gopp:").unwrap_or(message))
            .filter(|&message| message != overflows)
            .collect();
        assert_eq!(found, *expected, "{body}");
    }
}

#[test]
fn tildes_literals_indexes_and_map_keys_are_checked_as_go_checks_them() {
    let declared = "type F func()\ntype K struct{ f []int }\ntype M map[M]int\nvar (\n\ti  int\n\
                    \tf  float64\n\tm  map[int]int\n\ta  [3]int\n)\n";
    let key = "4:12: invalid map key type M";
    let cases: &[(&str, &[&str])] = &[
        (
            "_ = ~i",
            &[
                "12:6: cannot use ~ outside of interface or type constraint (use ^ for bitwise \
                 complement)",
            ],
        ),
        (
            "_ = ~f",
            &["12:6: cannot use ~ outside of interface or type constraint"],
        ),
        (
            "_ = 6e886451608 / 2",
            &["12:6: malformed constant: 6e886451608"],
        ),
        (
            "_ = m[i, i]",
            &["12:11: invalid operation: more than one index"],
        ),
        (
            "_ = a[i, i]",
            &["12:11: invalid operation: more than one index"],
        ),
        (
            "var _ map[[]int]int",
            &["12:12: invalid map key type []int"],
        ),
        ("var _ map[F]int", &["12:12: invalid map key type F"]),
        ("var _ map[K]int", &["12:12: invalid map key type K"]),
        (
            "_ = make(map[[2]F]int)",
            &["12:15: invalid map key type [2]F"],
        ),
        // What is right next to each of them.
        ("_ = ^i\n\t_ = 6e8 / 2\n\t_ = m[i] + a[i]", &[]),
        (
            "var _ map[*K]int\n\tvar _ map[[2]string]int\n\tvar _ map[any]int\n\
             \tvar _ map[struct{ a, b int }]int",
            &[],
        ),
    ];
    for (body, expected) in cases {
        let text = format!("package p\n{declared}func _() {{\n\t{body}\n}}\n");
        let found = messages("a.gopp", &text);
        let found: Vec<&str> = found
            .iter()
            .map(|message| message.strip_prefix("a.gopp:").unwrap_or(message))
            .filter(|&message| message != key)
            .collect();
        assert_eq!(found, *expected, "{body}");
    }
    assert_eq!(
        messages("a.gopp", "package p\ntype M map[M]int\n"),
        ["a.gopp:2:12: invalid map key type M"]
    );
}

// rule: OWN-R3, OWN-P2, OWN-P3, OWN-P4
#[test]
fn values_are_moved_and_put_on_the_heap_by_the_built_in_functions_of_gopp() {
    let text = "package p

type User struct{ Name string }

var global User

func f(p User, q *User) {
	a := move(p)
	b := move((q))
	u := NewUnique(User{Name: \"b\"})
	s := NewShared(a)
	w := NewWeak(s)
	n := NewUnique(1)
	_, _, _, _ = b, u, w, n
}

func g(p User, u Unique[User]) {
	_ = move(global)
	_ = move(p.Name)
	_ = move(User{})
	_ = NewWeak(u)
	_ = NewUnique(nil)
	_ = &User{Name: \"a\"}
	_ = &p
}
";
    let names = ThreadedRodeo::new();
    let loader = Loader::default();
    let checked = check_texts("p", &[("a.gopp", text)], &loader, &names);
    let expected = [
        "a.gopp:18:11: cannot move global (variable of struct type User): move takes a local \
         variable or a parameter",
        "    use mem.Take(&global)",
        "a.gopp:19:11: cannot move p.Name (variable of type string): move takes a local variable \
         or a parameter",
        "    use mem.Take(&p.Name)",
        "a.gopp:20:11: cannot move User{} (value of struct type User): move takes a local \
         variable or a parameter",
        "a.gopp:21:14: invalid argument: u (variable of type Unique[User]) is not a Shared[T]",
        "a.gopp:22:16: use of untyped nil in argument to NewUnique",
        "a.gopp:23:6: cannot take the address of User{…}",
        "    use NewUnique(User{…}) or NewShared(User{…})",
    ];
    assert_eq!(checked.messages, expected);
    let types = loader.types.get().unwrap();
    let locals: Vec<String> = checked.package.files[0].bodies[2]
        .locals
        .iter()
        .map(|local| {
            format!(
                "{}: {}",
                names.resolve(&local.name),
                types.display(local.ty, &names)
            )
        })
        .collect();
    let expected = [
        "p: p.User",
        "q: *p.User",
        "a: p.User",
        "b: *p.User",
        "u: Unique[p.User]",
        "s: Shared[p.User]",
        "w: Weak[p.User]",
        "n: Unique[int]",
    ];
    assert_eq!(locals, expected);
}

/// What the spec shows as good code: pointers that are read and written
/// through, options, handles that lend what they own.
const GOOD_GOPP: &str = r#"package main

type Node struct {
	Name   string
	Kids   []Shared[Node]
	Parent Option[Weak[Node]]
}

func NewNode(name string, parent Option[Weak[Node]]) Shared[Node] {
	return NewShared(Node{Name: name, Kids: []Shared[Node]{}, Parent: parent})
}

func Add(parent Shared[Node], child Shared[Node]) {
	kids := parent.Kids
	kids = append(kids, child)
}

type Tree struct {
	L, R Option[Unique[Tree]]
	V    int
}

func sum(t Option[*Tree]) int {
	if t == nil {
		return 0
	}
	return t.V + sum(t.L) + sum(t.R)
}

func build() Unique[Tree] {
	return NewUnique(Tree{V: 1, L: NewUnique(Tree{V: 2})})
}

func addOne(xs *[]int) { xs[0]++ }

func swap(a, b *int) { a, b = *b, *a }

type List struct {
	next Option[*List]
	v    int
}

func walk(n *List) int {
	total := 0
	for {
		total += n.v
		next := n.next
		if next == nil {
			return total
		}
		n = next
	}
}

func main() {
	root := NewNode("root", nil)
	Add(root, NewNode("a", NewWeak(root)))
	b := NewNode("b", NewWeak(root))
	Add(root, b)
	if w, ok := b.Parent.Get(); ok {
		if p := w.Lock(); p != nil {
			println(p.Name)
		}
	}
	nums := []int{1, 2}
	addOne(&nums)
	x, y := 1, 2
	swap(&x, &y)
	t := build()
	println(sum(t), sum(nil))
	var f func(int) int = func(i int) int { return i }
	var g Option[func(int) int]
	g = f
	g = nil
	_ = g
}
"#;

// rule: TYP-2, OWN-P5, OWN-P6, OWN-P9
#[test]
fn a_place_takes_what_the_rules_of_gopp_say() {
    assert_eq!(messages("good.gopp", GOOD_GOPP), [] as [&str; 0]);
    let declared = "type User struct{ Name string }\ntype Vec struct{ X, Y int }\n\
                    @Operator func (a Vec) Add(b Vec) Vec { return a }\nfunc take(u User) {}\n\
                    func borrow(u *User) {}\nfunc fail() error    { return nil }\n";
    let params = "p *User, n *int, w Weak[User], s string, xs *[]int, f float64, v Vec, u User, \
                  o Option[int], q *User";
    let never = |ty: &str| format!("    a {ty} is never nil: use Option[{ty}]");
    let cases: &[(&str, Vec<String>)] = &[
        (
            "var _ *User = nil",
            vec![
                "9:16: cannot use nil as *User value in variable declaration".to_owned(),
                never("*User"),
            ],
        ),
        (
            "var _ func() = nil",
            vec![
                "9:17: cannot use nil as func() value in variable declaration".to_owned(),
                never("func()"),
            ],
        ),
        (
            "var _ User = nil",
            vec!["9:15: cannot use nil as User value in variable declaration".to_owned()],
        ),
        (
            "var _ *int = 9",
            vec![
                "9:15: cannot use 9 (untyped int constant) as *int value in variable declaration"
                    .to_owned(),
            ],
        ),
        (
            "var _ Option[int] = \"a\"",
            vec![
                "9:22: cannot use \"a\" (untyped string constant) as Option[int] value in \
                 variable declaration"
                    .to_owned(),
            ],
        ),
        (
            "var _ string = n",
            vec![
                "9:17: cannot use n (variable of type *int) as string value in variable \
                 declaration"
                    .to_owned(),
            ],
        ),
        (
            "var _ *User = w",
            vec![
                "9:16: cannot use w (variable of type Weak[User]) as *User value in variable \
                 declaration"
                    .to_owned(),
            ],
        ),
        (
            "var _ Unique[User] = p",
            vec![
                "9:23: cannot use p (variable of type *User) as Unique[User] value in variable \
                 declaration"
                    .to_owned(),
            ],
        ),
        (
            "take(n)",
            vec![
                "9:7: cannot use n (variable of type *int) as User value in argument to take"
                    .to_owned(),
            ],
        ),
        (
            "borrow(1)",
            vec![
                "9:9: cannot use 1 (untyped int constant) as *User value in argument to borrow"
                    .to_owned(),
            ],
        ),
        (
            "n = \"a\"",
            vec![
                "9:6: cannot use \"a\" (untyped string constant) as *int value in assignment"
                    .to_owned(),
            ],
        ),
        (
            "n += \"a\"",
            vec![
                "9:2: invalid operation: n += \"a\" (mismatched types int and untyped string)"
                    .to_owned(),
            ],
        ),
        (
            "_ = n + s",
            vec!["9:6: invalid operation: n + s (mismatched types int and string)".to_owned()],
        ),
        (
            "_ = len(n)",
            vec!["9:10: invalid argument: n (variable of type int) for built-in len".to_owned()],
        ),
        (
            "_ = u + u",
            vec![
                "9:6: invalid operation: operator + not defined on u (variable of struct type \
                 User)"
                    .to_owned(),
            ],
        ),
        (
            "_ = p.Age",
            vec!["9:8: p.Age undefined (type User has no field or method Age)".to_owned()],
        ),
        (
            "var x, y int\n\tx, y = 1\n\t_, _ = x, y",
            vec!["10:9: assignment mismatch: 2 variables but 1 value".to_owned()],
        ),
        // What is right next to each of them.
        (
            "var _ Option[*User] = nil\n\tvar _ Option[int] = 1\n\tvar _ error = nil\n\
             \tvar _ User = p\n\tvar _ int = n\n\tvar _ *User = q",
            vec![],
        ),
        (
            "take(p)\n\tborrow(q)\n\tn = 9\n\tn += 2\n\tn++\n\txs = append(xs, n)\n\txs[0] = n\n\
             \txs = nil",
            vec![],
        ),
        (
            "_ = v + v\n\t_ = v == v\n\t_ = n + 1\n\t_ = p == q\n\t_ = o == nil\n\t_ = o == 3\n\
             \t_ = *n + 1\n\t_ = len(xs) + len(p.Name)",
            vec![],
        ),
    ];
    for (body, expected) in cases {
        let text = format!("package p\n{declared}func _({params}) {{\n\t{body}\n}}\n");
        let found = messages("a.gopp", &text);
        let found: Vec<&str> = found
            .iter()
            .map(|message| message.strip_prefix("a.gopp:").unwrap_or(message))
            .collect();
        assert_eq!(found, *expected, "{body}");
    }
}

// rule: ERR-3
#[test]
fn a_function_that_can_fail_returns_its_results_or_one_value_more() {
    let text = "package p

type User struct{ Name string }

func fail() error { return nil }

func one!() int               { return 1 }
func two!() (int, string)     { return 1, \"a\" }
func none!()                  { return }
func failing!() int           { return 0, fail() }
func failnone!()              { return fail() }
func forward!() int           { return one() }
func unwrap!() int            { return one!() }
func unset!(p *User) *User {
	if p.Name == \"\" {
		return nil, fail()
	}
	return p
}
func generic[T any]!(x T) T   { return nil, fail() }
func goform(p *User) (*User, error) { return p, nil }

func few!() int               { return }
func fewer!() (int, string)   { return 1 }
func many!() int              { return 1, nil, 2 }
func zero!() int              { return nil, fail() }
func wrong!() int             { return \"a\" }
";
    let expected = [
        "a.gopp:23:33: not enough return values",
        "    have ()",
        "    want (int, error)",
        "a.gopp:24:40: not enough return values",
        "    have (number)",
        "    want (int, string, error)",
        "a.gopp:25:48: too many return values",
        "    have (number, nil, number)",
        "    want (int, error)",
        "a.gopp:26:40: cannot use nil as int value in return statement",
        "a.gopp:27:40: cannot use \"a\" (untyped string constant) as int value in return statement",
    ];
    assert_eq!(messages("a.gopp", text), expected);
}

/// Packages that stand for those of the standard library that the
/// examples of the specification import.
fn with_packages(names: &ThreadedRodeo, packages: &[(&str, &str)]) -> Loader {
    let mut loader = Loader::default();
    for &(path, text) in packages {
        // Package unsafe is declared without bodies, as Go declares it.
        let file = match path {
            "unsafe" => "unsafe.gopp".to_owned(),
            _ => format!("{path}.gopp"),
        };
        let checked = check_texts(path, &[(&file, text)], &loader, names);
        assert_eq!(checked.messages, [] as [&str; 0], "{path}");
        loader
            .packages
            .insert(path.to_owned(), Arc::new(checked.package));
    }
    loader
}

/// What the function literals of the first file capture, and its method
/// values: `line:col: x read, y write`, `line:col x.m: value`.
fn closures(checked: &Checked, names: &ThreadedRodeo) -> Vec<String> {
    let file = &checked.files[0];
    let mut all = Vec::new();
    for body in &checked.package.files[0].bodies {
        let at = |expr: ExprId| {
            let span = file.exprs[expr].span;
            let source = checked.map.file(span.file);
            let at = source.line_col(span.lo);
            let text = source.text()[span.lo as usize..span.hi as usize].to_owned();
            (span.lo, format!("{}:{}", at.line, at.column), text)
        };
        for (&literal, captures) in &body.captures {
            let captured: Vec<String> = captures
                .iter()
                .map(|capture| {
                    let name = names.resolve(&body.locals[capture.local.0 as usize].name);
                    format!("{name} {}", format!("{:?}", capture.how).to_lowercase())
                })
                .collect();
            let (offset, place, _) = at(literal);
            all.push((offset, format!("{place}: {}", captured.join(", "))));
        }
        for (&selector, holds) in &body.method_values {
            let (offset, place, text) = at(selector);
            let holds = format!("{holds:?}").to_lowercase();
            all.push((offset, format!("{place} {text}: {holds}")));
        }
    }
    all.sort();
    all.into_iter().map(|(_, line)| line).collect()
}

/// How the places of the first file give their values, in the order in
/// which they are written: `line:col place: copy`, with `over` for a
/// place that is assigned over and `last` for the last use of a
/// variable; then the places that swaps assign to.
fn transfers(checked: &Checked) -> Vec<String> {
    let file = &checked.files[0];
    let shown = |expr: ExprId| {
        let span = file.exprs[expr].span;
        let source = checked.map.file(span.file);
        let at = source.line_col(span.lo);
        let text = &source.text()[span.lo as usize..span.hi as usize];
        (span.lo, format!("{}:{} {text}", at.line, at.column))
    };
    let mut all = Vec::new();
    for body in &checked.package.files[0].bodies {
        for (&expr, mode) in &body.var_modes {
            let (at, place) = shown(expr);
            let mode = match mode {
                Transfer::Copy => "copy",
                Transfer::Move => "move",
            };
            let over = match body.self_moves.contains(&expr) {
                true => ", over",
                false => "",
            };
            let last = match body.last_uses.contains(&expr) {
                true => ", last",
                false => "",
            };
            all.push((at, format!("{place}: {mode}{over}{last}")));
        }
        for &expr in &body.swaps {
            let (at, place) = shown(expr);
            all.push((at, format!("{place}: swap")));
        }
        // What is sliced is no value that is given.
        for &expr in &body.self_moves {
            if !body.var_modes.contains_key(&expr) {
                let (at, place) = shown(expr);
                all.push((at, format!("{place}: over")));
            }
        }
    }
    all.sort();
    all.into_iter().map(|(_, line)| line).collect()
}

// rule: OWN-R5, EXP-3
#[test]
fn a_variable_is_not_read_after_move_took_its_value() {
    let text = "package p

type T struct {
	name string
	xs   []int
}

func keep(xs []int)         {}
func keep2(xs []int, n int) {}
func keep3(n int, xs []int) {}
func read(xs []int) int     { return len(xs) }
func hold(t T)              {}

func straight(xs []int, t T, n int) {
	keep(move(xs))
	_ = len(xs)
	xs = nil
	_ = len(xs)
	hold(move(t))
	t.name = \"a\"
	t = T{}
	t.name = \"b\"
	_ = move(n)
	_ = n
}

func branches(xs, ys, zs []int, c bool) {
	if c {
		keep(move(xs))
	}
	_ = len(xs)
	if c {
		keep(move(ys))
		ys = nil
	} else {
		keep(move(ys))
		ys = []int{1}
	}
	_ = len(ys)
	if c {
		keep(move(zs))
		return
	}
	_ = len(zs)
}

func loops(xs, ys, zs []int, n int) {
	for i := 0; i < n; i++ {
		keep(move(xs))
	}
	for i := 0; i < n; i++ {
		keep(move(ys))
		ys = nil
	}
	for i := 0; i < n; i++ {
		if i == 3 {
			keep(move(zs))
			break
		}
		_ = len(zs)
	}
	for i := range n {
		w := []int{i}
		keep(move(w))
	}
}

func order(xs, ys, zs []int) {
	keep2(move(xs), len(xs))
	keep3(len(ys), move(ys))
	keep(move(zs))
	keep(move(zs))
}

func jumps(xs, ys []int, n int) {
	i := 0
again:
	_ = len(xs)
	if i < n {
		i++
		keep(move(xs))
		goto again
	}
	switch n {
	case 1:
		keep(move(ys))
		fallthrough
	case 2:
		_ = len(ys)
	case 3:
		_ = len(ys)
	}
}

func named(n int) (xs []int, ys []int) {
	keep(move(xs))
	if n > 0 {
		return
	}
	keep(move(ys))
	return nil, nil
}

func literals(xs, ys []int) {
	f := func() int { return len(ys) }
	keep(move(xs))
	g := func() int { return len(xs) }
	h := func(zs []int) {
		keep(move(zs))
		_ = len(zs)
	}
	_, _, _ = f, g, h
	p := &xs
	_ = p
}
";
    let expected = [
        "a.gopp:16:10: xs is used after it was moved: it has no value until it is assigned again",
        "    a.gopp:15:7: xs is moved here",
        "a.gopp:20:2: t is used after it was moved: it has no value until it is assigned again",
        "    a.gopp:19:7: t is moved here",
        "a.gopp:24:6: n is used after it was moved: it has no value until it is assigned again",
        "    a.gopp:23:6: n is moved here",
        "a.gopp:31:10: xs is used after it was moved: it has no value until it is assigned again",
        "    a.gopp:29:8: xs is moved here",
        "a.gopp:49:8: xs is moved again where it was moved before: it has no value until it is \
         assigned again",
        "a.gopp:69:22: xs is used after it was moved: it has no value until it is assigned again",
        "    a.gopp:69:8: xs is moved here",
        "a.gopp:72:7: zs is used after it was moved: it has no value until it is assigned again",
        "    a.gopp:71:7: zs is moved here",
        "a.gopp:78:10: xs is used after it was moved: it has no value until it is assigned again",
        "    a.gopp:81:8: xs is moved here",
        "a.gopp:81:8: xs is moved again where it was moved before: it has no value until it is \
         assigned again",
        "a.gopp:89:11: ys is used after it was moved: it has no value until it is assigned again",
        "    a.gopp:86:8: ys is moved here",
        "a.gopp:98:3: xs is used after it was moved: it has no value until it is assigned again",
        "    a.gopp:96:7: xs is moved here",
        "a.gopp:107:7: xs is used after it was moved: it has no value until it is assigned again",
        "    a.gopp:106:7: xs is moved here",
        "a.gopp:110:11: zs is used after it was moved: it has no value until it is assigned again",
        "    a.gopp:109:8: zs is moved here",
    ];
    assert_eq!(messages("a.gopp", text), expected);
}

// rule: OWN-R3
#[test]
fn the_last_copy_of_a_variable_is_marked() {
    let text = "package p

type T struct {
	name string
	xs   []int
}

var all [][]int

func keep(xs []int)            {}
func keep2(xs []int, ys []int) {}
func keep3(n int, xs []int)    {}
func hold(t T)                 {}
func make1() []int             { return nil }

func straight(xs []int, s string) []int {
	a := xs
	b := xs
	keep(a)
	keep(a)
	t := s
	u := s
	_, _ = t, u
	return b
}

func branches(xs, ys []int, c bool) {
	if c {
		keep(xs)
	} else {
		keep(xs)
	}
	if c {
		keep(ys)
	}
	keep(ys)
}

func loops(xs, ys []int, n int) {
	for i := 0; i < n; i++ {
		keep(xs)
	}
	for i := 0; i < n; i++ {
		w := make1()
		keep(w)
	}
	for i := 0; i < n; i++ {
		if i == 3 {
			keep(ys)
			break
		}
	}
}

func again(xs []int, n int) {
	keep(xs)
	xs = make1()
	keep(xs)
	xs = make1()
	for i := 0; i < n; i++ {
		keep(xs)
		xs = make1()
	}
}

func units(xs, ys, zs []int, t T) {
	keep2(xs, xs)
	keep3(len(ys), ys)
	zs = append(make1(), zs...)
	all = append(all, zs)
	t.xs = t.xs
	hold(t)
}

func ranged(xs []int, m map[string][]int, t T) {
	for _, x := range xs {
		if x == 3 {
			keep(xs)
			return
		}
	}
	for k, v := range m {
		keep(v)
		_ = k
	}
	switch t.name {
	case \"a\":
		hold(t)
	default:
		hold(t)
	}
}

func named() (xs []int, ys []int) {
	xs = make1()
	keep(xs)
	ys = make1()
	keep(ys)
	xs = nil
	return
}

func literals(xs, ys []int) {
	keep(xs)
	f := func() { keep(ys) }
	f()
	keep(ys)
}
";
    let names = ThreadedRodeo::new();
    let loader = Loader::default();
    let checked = check_texts("p", &[("a.gopp", text)], &loader, &names);
    assert_eq!(checked.messages, [] as [&str; 0]);
    let expected = [
        "17:7 xs: copy",
        "18:7 xs: copy, last",
        "19:7 a: copy",
        "20:7 a: copy, last",
        "21:7 s: copy",
        "22:7 s: copy, last",
        "24:9 b: copy, last",
        "29:8 xs: copy, last",
        "31:8 xs: copy, last",
        "34:8 ys: copy",
        "36:7 ys: copy, last",
        "41:8 xs: copy",
        "45:8 w: copy, last",
        "49:9 ys: copy, last",
        "56:7 xs: copy, last",
        "58:7 xs: copy, last",
        "61:8 xs: copy, last",
        "67:8 xs: copy",
        "67:12 xs: copy",
        "68:17 ys: copy",
        "69:23 zs: copy, over, last",
        "70:15 all: copy, over",
        "70:20 zs: copy, last",
        "71:9 t.xs: copy, over",
        "72:7 t: copy, last",
        "78:9 xs: copy",
        "83:8 v: copy, last",
        "88:8 t: copy, last",
        "90:8 t: copy, last",
        "96:7 xs: copy, last",
        "98:7 ys: copy",
        "104:7 xs: copy, last",
        "105:21 ys: copy",
        "107:7 ys: copy",
    ];
    assert_eq!(transfers(&checked), expected);
}

// rule: PAR-1
#[test]
fn a_write_to_the_function_s_own_slice_or_map_that_nothing_reads_is_lost() {
    let text = "package p

type List []int

type Doc struct{ Title string }

var all [][]int

func keep(xs []int) {}

func zero(xs []int)         { xs[0] = 0 }
func zeroed(xs []int) []int { xs[0] = 0; return xs }
func zeroP(xs *[]int)       { xs[0] = 0 }

func fill(xs []int) {
	for i := range xs {
		xs[i] = 1
	}
}

func filled(xs []int) int {
	for i := range xs {
		xs[i] = 1
	}
	return len(xs)
}

func inc(xs []int, m map[string]int) {
	for i := range xs {
		xs[i]++
	}
	m[\"a\"]++
}

func sums(xs []int) int {
	total := 0
	for i, x := range xs {
		if i > 0 {
			xs[i] += xs[i-1]
		}
		total += x
	}
	return total
}

func entries(m map[string]int, n map[string]int, o map[string][]int) int {
	m[\"a\"] = 1
	delete(m, \"b\")
	clear(n)
	o[\"k\"][0] = 1
	o = nil
	return 0
}

func grow(xs []int, ys []int, zs []int) []int {
	xs = append(xs, 1)
	for i := 0; i < 3; i++ {
		ys = append(ys, i)
	}
	zs = append(zs, 1)
	return zs
}

func passed(xs []int, ys []int, c bool) {
	xs[0] = 1
	keep(xs)
	ys[0] = 2
	if c {
		all = append(all, ys)
	}
}

func (l List) Clear()     { l[0] = 0 }
func (l List) First() int { l[0]++; return l[0] }

func other(d Doc, a [3]int, xs []int, n int) {
	d.Title = \"local\"
	a[0] = 1
	ys := xs
	ys[0] = 1
	n++
}

func generic[S ~[]int](xs S, ys S) S {
	xs[0] = 1
	ys[0] = 2
	return ys
}

func literal() {
	_ = func(xs []int) { xs[0] = 1 }
	_ = func(xs []int) int { xs[0] = 1; return xs[0] }
}

func seen(xs []int, ys []int, zs []int, m map[string]int) int {
	f := func() int { return xs[0] }
	xs[0] = 1
	p := &ys
	ys[0] = 2
	for i, z := range zs {
		zs[i] = 5
		println(z)
	}
	for k, v := range m {
		m[k] = v + 1
	}
	return f() + len(p)
}
";
    let expected = [
        "a.gopp:11:31: this write to xs is lost: xs is the function's own copy, and nothing reads \
         it afterwards",
        "    declare xs *[]int to change the value of the caller, or return xs",
        "a.gopp:17:3: this write to xs is lost: xs is the function's own copy, and nothing reads \
         it afterwards",
        "    declare xs *[]int to change the value of the caller, or return xs",
        "a.gopp:30:3: this write to xs is lost: xs is the function's own copy, and nothing reads \
         it afterwards",
        "    declare xs *[]int to change the value of the caller, or return xs",
        "a.gopp:32:2: this write to m is lost: m is the function's own copy, and nothing reads it \
         afterwards",
        "    declare m *map[string]int to change the value of the caller, or return m",
        "a.gopp:47:2: this write to m is lost: m is the function's own copy, and nothing reads it \
         afterwards",
        "    declare m *map[string]int to change the value of the caller, or return m",
        "a.gopp:49:2: this write to n is lost: n is the function's own copy, and nothing reads it \
         afterwards",
        "    declare n *map[string]int to change the value of the caller, or return n",
        "a.gopp:50:2: this write to o is lost: o is the function's own copy, and nothing reads it \
         afterwards",
        "    declare o *map[string][]int to change the value of the caller, or return o",
        "a.gopp:56:2: this write to xs is lost: xs is the function's own copy, and nothing reads \
         it afterwards",
        "    declare xs *[]int to change the value of the caller, or return xs",
        "a.gopp:58:3: this write to ys is lost: ys is the function's own copy, and nothing reads \
         it afterwards",
        "    declare ys *[]int to change the value of the caller, or return ys",
        "a.gopp:73:29: this write to l is lost: l is the function's own copy, and nothing reads it \
         afterwards",
        "    declare l *List to change the value of the caller, or return l",
        "a.gopp:85:2: this write to xs is lost: xs is the function's own copy, and nothing reads \
         it afterwards",
        "    declare xs *S to change the value of the caller, or return xs",
        "a.gopp:91:23: this write to xs is lost: xs is the function's own copy, and nothing reads \
         it afterwards",
        "    declare xs *[]int to change the value of the caller, or return xs",
    ];
    assert_eq!(messages("a.gopp", text), expected);
}

// rule: OWN-B2
#[test]
fn a_variable_is_not_moved_while_a_statement_around_reads_it() {
    let text = "package p

func keep(xs []int) {}

func holds(xs []int, m map[string]int, v any) {
	for _, x := range xs {
		keep(move(xs))
		_ = x
	}
	keep(move(xs))
	for k := range m {
		if k == \"a\" {
			_ = move(m)
			break
		}
	}
	switch w := v.(type) {
	case int:
		_ = move(v)
		_ = w
	}
	_ = move(v)
}
";
    let expected = [
        "a.gopp:7:8: cannot move xs: the loop that ranges over it, or the switch on it, still \
         reads it",
        "a.gopp:13:8: cannot move m: the loop that ranges over it, or the switch on it, still \
         reads it",
        "a.gopp:19:7: cannot move v: the loop that ranges over it, or the switch on it, still \
         reads it",
    ];
    assert_eq!(messages("a.gopp", text), expected);
}

#[test]
fn the_steps_of_moves_and_last_uses_grow_with_the_input_not_faster() {
    let steps = |text: String| {
        let loader = Loader::default();
        let checked = check_texts("p", &[("a.gopp", &text)], &loader, &ThreadedRodeo::new());
        assert_eq!(checked.messages, [] as [&str; 0]);
        checked.steps
    };
    let head = "package p\nfunc keep(xs []int) {}\nfunc f(c bool, n int) {\n";
    // Many variables, each given away and moved in a block of its own.
    let siblings = |count: usize| {
        let blocks: String = (0..count)
            .map(|i| {
                format!(
                    "if c {{\nv{i} := []int{{{i}}}\nkeep(v{i})\nw{i} := v{i}\nkeep(move(w{i}))\n}}\n"
                )
            })
            .collect();
        format!("{head}{blocks}}}\n")
    };
    // Many variables that live across many branches before each is
    // given away, moved, and set again.
    let long_lived = |count: usize| {
        let declared: String = (0..count)
            .map(|i| format!("v{i} := []int{{{i}}}\n"))
            .collect();
        let between = "if c {\nn++\n}\n".repeat(count);
        let used: String = (0..count)
            .map(|i| format!("keep(v{i})\nkeep(move(v{i}))\nv{i} = nil\n"))
            .collect();
        format!("{head}{declared}{between}{used}}}\n")
    };
    // Loops in loops and branches in branches: every level gives a
    // variable of its own away and moves one, and the innermost gives
    // away a variable of every level.
    let nested = |open: &str, depth: usize| {
        let opened: String = (0..depth)
            .map(|i| {
                format!(
                    "v{i} := []int{{{i}}}\nw{i} := v{i}\nkeep(move(w{i}))\nw{i} = nil\n{open} \
                     {{\nkeep(v{i})\n"
                )
            })
            .collect();
        let inner: String = (0..depth).map(|i| format!("keep(v{i})\n")).collect();
        format!("{head}{opened}{inner}{}}}\n", "}\n".repeat(depth))
    };
    // Jumps back to many labels.
    let jumps = |count: usize| {
        let labels: String = (0..count)
            .map(|i| {
                format!(
                    "l{i}:\nv{i} := []int{{{i}}}\nkeep(v{i})\nif n == {i} {{\nn++\ngoto \
                     l{i}\n}}\nkeep(move(v{i}))\n"
                )
            })
            .collect();
        format!("{head}{labels}}}\n")
    };
    // Many parameters that are written in many loops and read at the
    // end.
    let written = |count: usize| {
        let params: String = (0..count).map(|i| format!(", p{i} []int")).collect();
        let writes: String = (0..count)
            .map(|i| format!("for i := range p{i} {{\np{i}[i]++\n}}\np{i} = append(p{i}, {i})\n"))
            .collect();
        let reads: String = (0..count).map(|i| format!("n += len(p{i})\n")).collect();
        format!("package p\nfunc f(c bool, n int{params}) int {{\n{writes}{reads}return n\n}}\n")
    };
    let sizes = [250, 500, 1_000, 2_000];
    linear::assert_near_linear(&sizes, |n| steps(siblings(n)));
    linear::assert_near_linear(&sizes, |n| steps(long_lived(n)));
    linear::assert_near_linear(&sizes, |n| steps(jumps(n)));
    linear::assert_near_linear(&sizes, |n| steps(written(n)));
    // The parser takes no deeper nesting, and a set of more than 64
    // variables is one level higher, which costs a step more.
    let depths = [100, 200, 400];
    linear::assert_near_linear(&depths, |n| steps(nested("for c", n)));
    linear::assert_near_linear(&depths, |n| steps(nested("if c", n)));
}

// rule: ERR-1, ERR-2, ERR-5, ERR-6, ERR-11a
#[test]
fn a_call_with_a_bang_returns_the_error_or_panics_as_its_function_says() {
    let names = ThreadedRodeo::new();
    let loader = with_packages(
        &names,
        &[
            (
                "os",
                "package os\ntype File struct{}\nfunc Open!(path string) File { return File{} }\nfunc (f File) Close!() {}\n",
            ),
            (
                "io",
                "package io\nimport \"os\"\nfunc ReadAll!(f *os.File) []byte { return nil }\n",
            ),
            (
                "strconv",
                "package strconv\nfunc Itoa(n int) string { return \"\" }\n",
            ),
            ("fmt", "package fmt\nfunc Println(args ...any) {}\n"),
            ("log", "package log\nfunc Fatal(args ...any) {}\n"),
        ],
    );
    let text = "package main

import (
	\"fmt\"
	\"io\"
	\"log\"
	\"os\"
	\"strconv\"
)

func load!(path string) []byte {
	f := os.Open!(path)
	return io.ReadAll!(&f)
}

func count!(path string) int {
	data := load!(path)
	return len(data)
}

func main() {
	a := load!(\"a.txt\")
	fmt.Println(len(a))

	b, err := load(\"b.txt\")
	if err != nil {
		log.Fatal(err)
	}
	fmt.Println(len(b))
}

type Conn struct{}

func (c *Conn) Send!(msg string) int { return len(msg) }

type Loader interface{ Load!(path string) []byte }

func parse[T any]!(s string) T { return nil, fail() }

func fail() error               { return nil }
func cleanup!()                 {}
func step!()                    {}
func worker!()                  {}
func pair!() (int, string)      { return 1, \"\" }
func goform() (int, error)      { return 1, nil }
func plain(n int, s string) int { return n }

func more(p string, l Loader, c *Conn) {
	f := func!(n int) string { return strconv.Itoa(n) }
	var _ func(int) (string, error) = f
	var h func!() int = goform
	g := parse[int]!(\"42\")
	os.Open!(p).Close!()
	n, s := pair!()
	_ = plain(pair!())
	_, _, _, _ = g, n, s, h!() + c.Send!(string(l.Load!(p)))
}

func run!() {
	defer cleanup!()
	step!()
	go worker!()
	func() { step!() }()
	return
}

func opens() (int, error) {
	go worker!()
	defer cleanup!()
	step!()
	f := func!() { step!() }
	return 0, f()
}

func panics() {
	defer cleanup!()
	defer func() { step!() }()
}

var loaded = load!(\"c.txt\")
func constrained[T ~int | ~string](x T) (T, error) { step!(); return x, nil }
";
    let checked = check_texts("main", &[("a.gopp", text)], &loader, &names);
    assert_eq!(checked.messages, [] as [&str; 0]);
    let tree = &checked.files[0];
    let bodies = checked.package.files[0].bodies.iter();
    let mut found: Vec<(u32, String)> = bodies
        .flat_map(|body| &body.bangs)
        .map(|(&expr, bang)| {
            let span = tree.exprs[expr].span;
            let line = text[..span.lo as usize].matches('\n').count() + 1;
            let written = &text[span.lo as usize..span.hi as usize];
            (span.hi, format!("{line}: {written}: {bang:?}"))
        })
        .collect();
    found.sort();
    let found: Vec<String> = found.into_iter().map(|(_, shown)| shown).collect();
    let expected = [
        "12: os.Open!(path): Return",
        "13: io.ReadAll!(&f): Return",
        "17: load!(path): Return",
        "22: load!(\"a.txt\"): Panic",
        "52: parse[int]!(\"42\"): Panic",
        "53: os.Open!(p): Panic",
        "53: os.Open!(p).Close!(): Panic",
        "54: pair!(): Panic",
        "55: pair!(): Panic",
        "56: h!(): Panic",
        "56: l.Load!(p): Panic",
        "56: c.Send!(string(l.Load!(p))): Panic",
        "60: cleanup!(): SetError",
        "61: step!(): Return",
        "62: worker!(): Panic",
        "63: step!(): Panic",
        "68: worker!(): Panic",
        "69: cleanup!(): SetError",
        "70: step!(): Return",
        "71: step!(): Return",
        "76: cleanup!(): Panic",
        "77: step!(): Panic",
        "80: load!(\"c.txt\"): Panic",
        "81: step!(): Return",
    ];
    assert_eq!(found, expected);
}

#[test]
fn the_steps_of_calls_with_a_bang_grow_with_the_input_not_faster() {
    let steps = |text: String| {
        let loader = Loader::default();
        let checked = check_texts("p", &[("a.gopp", &text)], &loader, &ThreadedRodeo::new());
        assert_eq!(checked.messages, [] as [&str; 0]);
        checked.steps
    };
    let header = "package p\nfunc step!() {}\nfunc value!() int { return 1 }\n";
    // Many calls in a function with many results.
    let wide = |count: usize| {
        let results = "int, ".repeat(count);
        let calls = "step!()\n".repeat(count);
        let values = "0, ".repeat(count);
        format!("{header}func f() ({results}error) {{\n{calls}return {values}nil\n}}\n")
    };
    let siblings = |count: usize| {
        let all: String = (0..count)
            .map(|i| format!("func f{i}!() int {{ defer step!(); return value!() + {i} }}\n"))
            .collect();
        format!("{header}{all}")
    };
    // A function literal inside a function literal, each with its own
    // answer.
    let nested = |depth: usize| {
        let open: String = (0..depth)
            .map(|i| match i % 2 {
                0 => "func!() { step!()\n",
                _ => "func() { step!()\n",
            })
            .collect();
        format!("{header}func f() {{\n{open}{}\n}}\n", "}()\n".repeat(depth))
    };
    let sizes = [250, 500, 1_000, 2_000];
    linear::assert_near_linear(&sizes, |n| steps(wide(n)));
    linear::assert_near_linear(&sizes, |n| steps(siblings(n)));
    // The parser takes no deeper nesting.
    linear::assert_near_linear(&[50, 100, 200, 400], |n| steps(nested(n)));
}

// rule: ERR-3, ERR-4, ERR-5, ERR-6, ERR-15
#[test]
fn a_bang_is_refused_where_no_error_is_returned_or_a_result_would_be_unset() {
    let text = "package p

type Node struct{ v int }
type Loader interface{ Load!(path string) error }
type Run func!() error

func fail() error       { return nil }
func step!()            {}
func node!() *Node      { return nil, fail() }
func number() int       { return 1 }

func check!() error                { return nil }
func both!() (int, error)          { return 0, nil }
func named!() (n int, err error)   { return }
func (n *Node) m!() (int, error)   { return 0, nil }
var lit = func!() error { return nil }

func values(n int) {
	number!()
	_ = int!(n)
	_ = len!(\"x\")
	_ = step!()
	step!()!()
}

func goform() (*Node, error) {
	_ = node!()
	defer step!()
	go step!()
	f := func!() *Node { return node!() }
	return f()
}
func generic[T any](x T) (T, error) { step!(); return x, nil }
func zeroed() (int, []int, error)   { step!(); return 0, nil, nil }
func failing() (*Node, error)       { return nil, fail() }
func absent() (*Node, error)        { return nil, nil }
func bare!() *Node                  { return nil }
";
    let expected = [
        "a.gopp:4:43: ! already adds the error; remove it",
        "a.gopp:5:18: ! already adds the error; remove it",
        "a.gopp:12:15: ! already adds the error; remove it",
        "a.gopp:13:20: ! already adds the error; remove it",
        "a.gopp:14:27: ! already adds the error; remove it",
        "a.gopp:15:27: ! already adds the error; remove it",
        "a.gopp:16:19: ! already adds the error; remove it",
        "a.gopp:19:2: invalid operation: number!() (the call returns no error)",
        "a.gopp:20:6: invalid operation: int!(n) (a conversion returns no error)",
        "a.gopp:21:6: invalid operation: len!(\"x\") (the call returns no error)",
        "a.gopp:22:6: step!() (no value) used as value",
        "a.gopp:23:2: step!() (no value) used as value or type",
        "a.gopp:27:6: invalid operation: node!() (on an error the function returns no *Node value)",
        "    declare the function with ! or handle the error and return a value",
        "a.gopp:33:39: invalid operation: step!() (on an error the function returns no T value)",
        "    declare the function with ! or handle the error and return a value",
        "a.gopp:35:46: cannot use nil as *Node value in return statement",
        "    a *Node is never nil: declare the function with ! or return Option[*Node]",
        "a.gopp:36:46: cannot use nil as *Node value in return statement",
        "    a *Node is never nil: use Option[*Node]",
        "a.gopp:37:46: cannot use nil as *Node value in return statement",
        "    a *Node is never nil: use Option[*Node]",
    ];
    assert_eq!(messages("a.gopp", text), expected);
}

#[test]
fn a_selector_records_what_it_reaches_and_the_way_to_it() {
    let text = "package p

type Inner struct{ a, b int }

func (Inner) val()  {}
func (*Inner) ptr() {}

type Outer struct {
	x  int
	in Inner
}

func f(o Outer, p *Outer, u Unique[Outer], opt Option[int]) {
	_ = o.x
	_ = p.x
	_ = u.in.b
	o.in.val()
	p.in.ptr()
	_ = Inner.val
	_ = opt.IsNil()
}
";
    let names = ThreadedRodeo::new();
    let loader = Loader::default();
    let selections = |file: &str, text: &str, body: usize| -> Vec<String> {
        let checked = check_texts("p", &[(file, text)], &loader, &names);
        assert_eq!(checked.messages, [] as [&str; 0]);
        let types = loader.types.get().unwrap();
        let tree = &checked.files[0];
        let mut found: Vec<(u32, String)> = checked.package.files[0].bodies[body]
            .selections
            .iter()
            .map(|(&expr, selection)| {
                let span = tree.exprs[expr].span;
                let shown = format!(
                    "{} {:?} from {} in {} by {:?}{}",
                    &text[span.lo as usize..span.hi as usize],
                    selection.kind,
                    types.display(selection.recv, &names),
                    types.display(selection.holder, &names),
                    selection.path,
                    if selection.indirect {
                        " through a pointer"
                    } else {
                        ""
                    }
                );
                (span.lo, shown)
            })
            .collect();
        found.sort();
        found.into_iter().map(|(_, shown)| shown).collect()
    };
    let expected = [
        "o.x Field from p.Outer in p.Outer by [0]",
        "p.x Field from *p.Outer in p.Outer by [0] through a pointer",
        "u.in Field from Unique[p.Outer] in p.Outer by [1] through a pointer",
        "u.in.b Field from p.Inner in p.Inner by [1]",
        "o.in Field from p.Outer in p.Outer by [1]",
        "o.in.val Method from p.Inner in p.Inner by []",
        "p.in Field from *p.Outer in p.Outer by [1] through a pointer",
        "p.in.ptr Method from p.Inner in p.Inner by []",
        "Inner.val MethodExpr from p.Inner in p.Inner by []",
    ];
    assert_eq!(selections("a.gopp", text, 4), expected);
}

#[test]
fn copy_writes_through_a_pointer_and_a_type_with_parents_is_left_alone() {
    let text = "package p

type Payment struct{ Amount int }

@Virtual
func (p *Payment) Total() int { return p.Amount }

@Extends(Payment)
type Card struct{ Number string }

func pay(p *Payment) {}
func mk() []int      { return nil }

func f(c Card, u Unique[Card]) {
	_ = c.Amount + c.Total()
	pay(&c)
	var up Unique[Payment] = u
	k := Card{Number: \"1\", Amount: 2}
	_, _ = up, k
}

func g(dst, src []int, p *[]int, b []byte, s string) {
	_ = copy(&dst, src)
	_ = copy(p, src)
	_ = copy(&b, s)
	_ = copy(&dst[1:], src)
	_ = copy(dst, src)
	_ = copy(mk(), src)
	_ = copy(&dst, s)
}
";
    let expected = [
        "a.gopp:27:11: invalid copy: the destination dst (variable of type []int) is passed as a \
         copy",
        "    use &dst",
        "a.gopp:28:11: invalid copy: the destination mk() (value of type []int) is passed as a copy",
        "a.gopp:29:11: invalid copy: arguments &dst (variable of type []int) and s (variable of \
         type string) have different element types int and byte",
    ];
    assert_eq!(messages("a.gopp", text), expected);
}

/// The second function is what is right next to each error of the first.
#[test]
fn labels_and_jumps_are_checked_as_go_checks_them() {
    let text = r#"package p

func f(x bool) {
	break
	continue
	fallthrough
	goto Missing
	goto Inner
	{
	Inner:
	}
Unused:
Twice:
Twice:
	goto Twice
	goto Over
	v := 1
	_ = v
Over:
	if x {
		goto Out
	}
Out:
Plain:
	x = !x
	for {
		break Plain
		continue Plain
	}
Sw:
	switch {
	case x:
		break Sw
		continue Sw
		fallthrough
	default:
		fallthrough
	}
	switch any(x).(type) {
	case bool:
		fallthrough
	}
	_ = func() {
		goto Out
	}
}

func g(x bool, c chan int) {
Loop:
	for i := 0; i < 3; i++ {
		for range 3 {
			if x {
				continue Loop
			}
			break Loop
		}
		switch {
		case x:
			break
		}
		continue
	}
Sel:
	select {
	case <-c:
		break Sel
	}
	goto Back
Back:
	goto Back
}
"#;
    let expected = [
        "a.gopp:4:2: break not in for, switch, or select statement",
        "a.gopp:5:2: continue not in for statement",
        "a.gopp:6:2: fallthrough statement out of place",
        "a.gopp:7:7: label Missing not declared",
        "a.gopp:8:7: goto Inner jumps into block",
        "a.gopp:12:1: label Unused declared and not used",
        "a.gopp:14:1: label Twice already declared",
        "    a.gopp:13:1: other declaration of Twice",
        "a.gopp:16:7: goto Over jumps over variable declaration at line 17",
        "a.gopp:24:1: label Plain declared and not used",
        "a.gopp:27:9: invalid break label Plain",
        "a.gopp:28:12: invalid continue label Plain",
        "a.gopp:34:12: invalid continue label Sw",
        "a.gopp:37:3: cannot fallthrough final case in switch",
        "a.gopp:41:3: cannot fallthrough in type switch",
        "a.gopp:44:8: label Out not declared",
    ];
    assert_eq!(messages("a.gopp", text), expected);
}

#[test]
fn a_function_with_results_ends_in_a_statement_that_does_not_go_on() {
    let cases: &[(&str, &[&str])] = &[
        ("func f() int {\n}", &["3:1: missing return"]),
        (
            "func f(x bool) int {\n\tif x {\n\t\treturn 1\n\t}\n}",
            &["6:1: missing return"],
        ),
        (
            "func f(x bool) int {\n\tfor {\n\t\tif x {\n\t\t\tbreak\n\t\t}\n\t}\n}",
            &["8:1: missing return"],
        ),
        (
            "func f(x bool) int {\nL:\n\tfor {\n\t\tswitch {\n\t\tcase x:\n\t\t\tbreak L\n\t\t}\n\t}\n}",
            &["10:1: missing return"],
        ),
        (
            "func f(x int) int {\n\tswitch x {\n\tcase 1:\n\t\treturn 1\n\t}\n}",
            &["7:1: missing return"],
        ),
        (
            "func f(x int) int {\n\tswitch x {\n\tcase 1:\n\t\tbreak\n\tdefault:\n\t\treturn 1\n\t}\n}",
            &["9:1: missing return"],
        ),
        (
            "func f(x int) int {\n\tfor x < 3 {\n\t}\n}",
            &["5:1: missing return"],
        ),
        ("var _ = func() int {\n}", &["3:1: missing return"]),
        // What is right next to each of them.
        ("func f() int {\n\treturn 1\n}\nfunc g() {\n}", &[]),
        (
            "func f(x bool) int {\n\tif x {\n\t\treturn 1\n\t} else {\n\t\tpanic(1)\n\t}\n}",
            &[],
        ),
        (
            "func f(x bool) int {\n\tfor {\n\t\tswitch {\n\t\tcase x:\n\t\t\tbreak\n\t\t}\n\t}\n}",
            &[],
        ),
        (
            "func f(x int) int {\n\tswitch x {\n\tcase 1:\n\t\tfallthrough\n\tdefault:\n\t\treturn 1\n\t}\n}",
            &[],
        ),
        (
            "func f(c chan int) int {\n\tselect {\n\tcase <-c:\n\t\treturn 1\n\t}\n}",
            &[],
        ),
        ("func f() int {\nL:\n\tgoto L\n}", &[]),
    ];
    for (code, expected) in cases {
        let text = format!("package p\n{code}\n");
        let found = messages("a.gopp", &text);
        let found: Vec<&str> = found
            .iter()
            .map(|message| message.strip_prefix("a.gopp:").unwrap_or(message))
            .collect();
        assert_eq!(found, *expected, "{code}");
    }
    // A function that can fail and has no other result needs no return.
    let text = "package p\nfunc f!() {\n}\nfunc g!() int {\n}\n";
    assert_eq!(messages("a.gopp", text), ["a.gopp:5:1: missing return"]);
}

#[test]
fn conditions_go_defer_and_switch_cases_are_checked_as_go_checks_them() {
    let text = r#"package p

type T struct{ f func() }
type B byte

func h() int   { return 0 }
func nop()     {}

func f(x int, s string, b bool, c chan int, i interface{}, fn func(), sl []int, t T, bb B) {
	if x {
	}
	if s {
	}
	for x {
	}
	for ; s; {
	}
	if b {
	}
	go 1
	go len(sl)
	go int(x)
	go h()
	go nop()
	defer 1
	defer len(sl)
	defer int(x)
	defer recover()
	defer panic(1)
	switch x {
	case 1:
	case 1:
	case (1):
	case 5, 1:
	case "a":
	case s:
	case 2.0:
	case 2:
	case x:
	case x:
	default:
	default:
	}
	switch s {
	case "foo":
	case "foo":
	case 1:
	}
	switch i {
	case 1:
	case 1:
	case 1.0:
	case byte(1):
	case B(1):
	case byte(1):
	case nil:
	case sl:
	}
	switch fn {
	case nil:
	}
	switch sl {
	}
	switch t {
	}
	switch {
	case true:
	case true:
	case x > 1:
	case 1:
	}
	switch nil {
	}
	switch nop() {
	}
	switch x := 1.5; x {
	case 1.5:
	case 1.5:
	}
	switch i.(type) {
	case int:
	case int:
	case nil:
	case nil:
	case string, int:
	}
	select {
	default:
	default:
	}
}
"#;
    let expected = [
        "a.gopp:10:5: non-boolean condition in if statement",
        "a.gopp:12:5: non-boolean condition in if statement",
        "a.gopp:14:6: non-boolean condition in for statement",
        "a.gopp:16:8: non-boolean condition in for statement",
        "a.gopp:20:6: expression in go must be function call",
        "a.gopp:21:5: go discards result of len(sl) (value of type int)",
        "a.gopp:22:5: go requires function call, not conversion int(x) (value of type int)",
        "a.gopp:25:9: expression in defer must be function call",
        "a.gopp:26:8: defer discards result of len(sl) (value of type int)",
        "a.gopp:27:8: defer requires function call, not conversion int(x) (value of type int)",
        "a.gopp:32:7: duplicate case 1 (constant of type int) in expression switch",
        "    a.gopp:31:7: previous case",
        "a.gopp:33:7: duplicate case (1) (constant 1 of type int) in expression switch",
        "    a.gopp:31:7: previous case",
        "a.gopp:34:10: duplicate case 1 (constant of type int) in expression switch",
        "    a.gopp:31:7: previous case",
        "a.gopp:35:7: cannot convert \"a\" (untyped string constant) to type int",
        "a.gopp:36:7: invalid case s in switch on x (mismatched types string and int)",
        "a.gopp:38:7: duplicate case 2 (constant of type int) in expression switch",
        "    a.gopp:37:7: previous case",
        "a.gopp:42:2: multiple defaults (first at a.gopp:41:2)",
        "a.gopp:46:7: duplicate case \"foo\" (constant of type string) in expression switch",
        "    a.gopp:45:7: previous case",
        "a.gopp:47:7: cannot convert 1 (untyped int constant) to type string",
        "a.gopp:51:7: duplicate case 1 (constant of type int) in expression switch",
        "    a.gopp:50:7: previous case",
        "a.gopp:55:7: duplicate case byte(1) (constant 1 of type byte) in expression switch",
        "    a.gopp:53:7: previous case",
        "a.gopp:57:7: invalid case sl in switch on i (slice can only be compared to nil)",
        "a.gopp:60:7: invalid case nil in switch on fn (a func() is never nil: use Option[func()])",
        "a.gopp:64:9: cannot switch on t (variable of struct type T) (T is not comparable)",
        "a.gopp:70:7: cannot convert 1 (untyped int constant) to type bool",
        "a.gopp:72:9: use of untyped nil in switch expression",
        "a.gopp:74:9: nop() (no value) used as value",
        "a.gopp:78:7: duplicate case 1.5 (constant of type float64) in expression switch",
        "    a.gopp:77:7: previous case",
        "a.gopp:82:7: duplicate case int in type switch",
        "    a.gopp:81:7: previous case",
        "a.gopp:84:7: duplicate case nil in type switch",
        "    a.gopp:83:7: previous case",
        "a.gopp:85:15: duplicate case int in type switch",
        "    a.gopp:81:7: previous case",
        "a.gopp:89:2: multiple defaults (first at a.gopp:88:2)",
    ];
    assert_eq!(messages("a.gopp", text), expected);
}

#[test]
fn range_clauses_are_checked_as_go_checks_them() {
    let text = r#"package p

type MyBool bool

func seq0(yield func() bool)                {}
func seq1(yield func(int) bool)             {}
func seq2(yield func(int, string) bool)     {}
func seq3(yield func(int, int, int) bool)   {}
func bad1(yield func(int))                  {}
func bad2(yield func(int) MyBool)           {}
func bad3(a, b int)                         {}
func bad4(yield func(int) bool) int         { return 0 }
func bad5(x int)                            {}

func f(a [3]int, p *[3]int, s []string, m map[string]bool, c chan int, send chan<- int, recv <-chan int, str string, x int, fl float64, t struct{}) {
	for range a {
	}
	for i := range a {
		_ = i
	}
	for i, v := range p {
		_, _ = i, v
	}
	for i, v := range s {
		var _ int = i
		var _ string = v
	}
	for k, v := range m {
		var _ string = k
		var _ bool = v
	}
	for v := range c {
		var _ int = v
	}
	for v, w := range c {
		_, _ = v, w
	}
	for range send {
	}
	for v := range recv {
		_ = v
	}
	for i, r := range str {
		var _ int = i
		var _ rune = r
	}
	for i := range x {
		var _ int = i
	}
	for i, j := range x {
		_, _ = i, j
	}
	for i := range 10 {
		var _ int = i
	}
	for range 1.5 {
	}
	for range fl {
	}
	for range t {
	}
	for i, j, k := range s {
		_, _, _ = i, j, k
	}
	for range 1 << 100 {
	}
	for i := range 'a' {
		var _ rune = i
	}
	for x := range seq0 {
		_ = x
	}
	for range seq0 {
	}
	for x := range seq1 {
		var _ int = x
	}
	for x, y := range seq1 {
		_, _ = x, y
	}
	for x, y := range seq2 {
		var _ int = x
		var _ string = y
	}
	for range seq3 {
	}
	for range bad1 {
	}
	for range bad2 {
	}
	for range bad3 {
	}
	for range bad4 {
	}
	for range bad5 {
	}
	var (
		i   int
		str2 string
		f64 float64
		u8  uint8
	)
	for i = range s {
	}
	for str2 = range s {
	}
	for i, i = range s {
	}
	for i, str2 = range s {
	}
	for f64 = range 10 {
	}
	for u8 = range 300 {
	}
	for u8 = range 10 {
	}
	for i = range fl {
	}
	for 1 = range s {
	}
	_, _, _, _ = i, str2, f64, u8
}
"#;
    let expected = [
        "a.gopp:35:9: range over c (variable of type chan int) permits only one iteration variable",
        "a.gopp:38:12: cannot range over send (variable of type chan<- int): receive from send-only channel chan<- int",
        "a.gopp:50:9: range over x (variable of type int) permits only one iteration variable",
        "a.gopp:56:12: cannot range over 1.5 (untyped float constant)",
        "a.gopp:58:12: cannot range over fl (variable of type float64)",
        "a.gopp:60:12: cannot range over t (variable of type struct{})",
        "a.gopp:62:12: expected at most 2 expressions",
        "a.gopp:65:12: cannot use 1 << 100 (untyped int constant 1267650600228229401496703205376) as int value in range clause (overflows)",
        "a.gopp:70:6: range over seq0 (value of type func(func() bool)) permits no iteration variables",
        "a.gopp:78:9: range over seq1 (value of type func(func(int) bool)) permits only one iteration variable",
        "a.gopp:85:12: cannot range over seq3 (value of type func(func(int, int, int) bool)): func must be func(yield func(...) bool): yield func has too many parameters",
        "a.gopp:87:12: cannot range over bad1 (value of type func(func(int))): func must be func(yield func(...) bool): yield func does not return bool",
        "a.gopp:89:12: cannot range over bad2 (value of type func(func(int) MyBool)): func must be func(yield func(...) bool): yield func returns user-defined boolean, not bool",
        "a.gopp:91:12: cannot range over bad3 (value of type func(int, int)): func must be func(yield func(...) bool): wrong argument count",
        "a.gopp:93:12: cannot range over bad4 (value of type func(func(int) bool) int): func must be func(yield func(...) bool): unexpected results",
        "a.gopp:95:12: cannot range over bad5 (value of type func(int)): func must be func(yield func(...) bool): argument is not func",
        "a.gopp:105:6: cannot use str2 (value of type int) as string value in assignment",
        "a.gopp:107:9: cannot use i (value of type string) as int value in assignment",
        "a.gopp:111:6: cannot use iteration variable of type float64",
        "a.gopp:113:17: cannot use 300 (untyped int constant) as uint8 value in range clause (overflows)",
        "a.gopp:117:16: cannot range over fl (variable of type float64)",
        "a.gopp:119:6: cannot assign to 1 (neither addressable nor a map index expression)",
    ];
    assert_eq!(messages("a.gopp", text), expected);
}

#[test]
fn returns_type_switches_and_values_of_interfaces_are_checked_as_go_checks_them() {
    let text = r#"package p

type E1 struct{ X int }
type E2 struct{ X int }
type T struct {
	E1
	E2
}

func f(t T, m map[int]int, x interface{}) (a, b int) {
	_ = t.X
	_ = T{}.X
	_ = x.(type)
	switch (x.(type)) {
	}
	for _ := range "" {
	}
	len.Println
	_ = len.Println
	if a > 0 {
		a := 1
		_ = a
		return
	}
	{
		b := 1
		_ = b
		{
			return
		}
	}
	return m[0]
}

func g(x interface{}) {
	switch t := x.(type) {
	case int:
		var v bool = t
		_ = v
	case float32, complex64:
		var v float32 = t
		_ = v
	default:
		var v float32 = t
		_ = v
	}
}

type I interface{ m() }
type C struct{}

func (C) m() {}

func h(e interface{}, i I, c C, pc *C) {
	var _ int = e
	var _ C = i
	var _ *C = i
	var _ string = i
	_ = int(e)
	_ = C(i)
	c = i
	i = c
	e = i
	e = c
	_ = c == i
	_ = e == 1
	take(e)
	take(i)
}

func take(c C) {}
"#;
    let expected = [
        "a.gopp:6:2: struct embedding is not part of Go++: use `@Extends` to inherit from a struct, or give the field a name",
        "a.gopp:7:2: struct embedding is not part of Go++: use `@Extends` to inherit from a struct, or give the field a name",
        "a.gopp:11:8: ambiguous selector t.X",
        "a.gopp:12:10: ambiguous selector T{}.X",
        "a.gopp:13:6: use of .(type) outside type switch",
        "a.gopp:14:10: use of .(type) outside type switch",
        "a.gopp:16:8: no new variables on left side of :=",
        "a.gopp:18:6: invalid use of len (built-in) in selector expression",
        "a.gopp:19:10: invalid use of len (built-in) in selector expression",
        "a.gopp:23:3: result parameter a not in scope at return",
        "    a.gopp:21:3: inner declaration of var a int",
        "a.gopp:29:4: result parameter b not in scope at return",
        "    a.gopp:26:3: inner declaration of var b int",
        "a.gopp:32:9: not enough return values",
        "    have (int)",
        "    want (int, int)",
        "a.gopp:38:16: cannot use t (variable of type int) as bool value in variable declaration",
        "a.gopp:41:19: cannot use t (variable of type interface{}) as float32 value in variable declaration: need type assertion",
        "a.gopp:44:19: cannot use t (variable of type interface{}) as float32 value in variable declaration: need type assertion",
        "a.gopp:55:14: cannot use e (variable of type interface{}) as int value in variable declaration: need type assertion",
        "a.gopp:56:12: cannot use i (variable of interface type I) as C value in variable declaration: need type assertion",
        "a.gopp:57:13: cannot use i (variable of interface type I) as *C value in variable declaration: need type assertion",
        "a.gopp:58:17: cannot use i (variable of interface type I) as string value in variable declaration",
        "a.gopp:59:10: cannot convert e (variable of type interface{}) to type int: need type assertion",
        "a.gopp:60:8: cannot convert i (variable of interface type I) to type C: need type assertion",
        "a.gopp:61:6: cannot use i (variable of interface type I) as C value in assignment: need type assertion",
        "a.gopp:67:7: cannot use e (variable of type interface{}) as C value in argument to take: need type assertion",
        "a.gopp:68:7: cannot use i (variable of interface type I) as C value in argument to take: need type assertion",
    ];
    assert_eq!(messages("a.gopp", text), expected);
}

#[test]
fn what_implements_an_interface_is_checked_as_go_checks_it() {
    let text = "package p

type I interface{ m() }
type J interface {
	m()
	n(int) string
}
type T struct{}

func (T) m() {}

type P struct{}

func (*P) m()           {}
func (*P) n(int) string { return \"\" }

type W struct{}

func (W) m()     {}
func (W) n() int { return 0 }

type C struct{}

func (C) M() {}

type F struct{ m int }

type E struct{ T }
type A struct {
	T
	W
}

func f(i I, j J, e any) {
	var _ I = T{}
	var _ I = &T{}
	var _ I = &P{}
	var _ J = &P{}
	var _ I = E{}
	var _ I = j
	var _ any = F{}
	var _ J = T{}
	var _ I = P{}
	var _ J = W{}
	var _ I = C{}
	var _ I = F{}
	var _ I = A{}
	var _ I = 1
	var _ J = i
	var _ *I = T{}
	var _ I = &i
	var _ T = i
	var _ P = i
	_ = i.(T)
	_ = i.(*P)
	_ = e.(C)
	_ = i.(J)
	_ = i.(P)
	_ = j.(W)
	_ = i.(C)
	_ = I(P{})
	_ = J(i)
	_ = T(i)
	switch i.(type) {
	case T, P, *P, C:
	case W, F:
	case nil, J:
	}
	i = P{}
	g(P{})
	_ = []I{P{}, T{}}
	_ = i == P{}
	_ = i == T{}
	_ = i == j
}

func g(I) I { return P{} }
";
    let expected = [
        "a.gopp:28:16: struct embedding is not part of Go++: use `@Extends` to inherit from a struct, or give the field a name",
        "a.gopp:30:2: struct embedding is not part of Go++: use `@Extends` to inherit from a struct, or give the field a name",
        "a.gopp:31:2: struct embedding is not part of Go++: use `@Extends` to inherit from a struct, or give the field a name",
        "a.gopp:36:12: cannot take the address of T{}",
        "    use NewUnique(T{}) or NewShared(T{})",
        "a.gopp:36:12: cannot keep &T{} (value of type *T) in a value of type I: an interface value may be given to another goroutine, and a *T borrows what it points to",
        "    keep it in a Local[I], or hold a Shared",
        "a.gopp:37:12: cannot take the address of P{}",
        "    use NewUnique(P{}) or NewShared(P{})",
        "a.gopp:37:12: cannot keep &P{} (value of type *P) in a value of type I: an interface value may be given to another goroutine, and a *P borrows what it points to",
        "    keep it in a Local[I], or hold a Shared",
        "a.gopp:38:12: cannot take the address of P{}",
        "    use NewUnique(P{}) or NewShared(P{})",
        "a.gopp:38:12: cannot keep &P{} (value of type *P) in a value of type J: an interface value may be given to another goroutine, and a *P borrows what it points to",
        "    keep it in a Local[J], or hold a Shared",
        "a.gopp:42:12: cannot use T{} (value of struct type T) as J value in variable declaration: T does not implement J (missing method n)",
        "a.gopp:44:12: cannot use W{} (value of struct type W) as J value in variable declaration: W does not implement J (wrong type for method n)",
        "    have n() int",
        "    want n(int) string",
        "a.gopp:45:12: cannot use C{} (value of struct type C) as I value in variable declaration: C does not implement I (missing method m)",
        "    have M()",
        "    want m()",
        "a.gopp:46:12: cannot use F{} (value of struct type F) as I value in variable declaration: F does not implement I (F.m is a field, not a method)",
        "a.gopp:47:12: cannot use A{} (value of struct type A) as I value in variable declaration: A does not implement I (ambiguous selector A.m)",
        "a.gopp:48:12: cannot use 1 (constant of type int) as I value in variable declaration: int does not implement I (missing method m)",
        "a.gopp:49:12: cannot use i (variable of interface type I) as J value in variable declaration: I does not implement J (missing method n)",
        "a.gopp:50:13: cannot use T{} (value of struct type T) as *I value in variable declaration: T does not implement *I (type *I is pointer to interface, not interface)",
        "a.gopp:52:12: cannot use i (variable of interface type I) as T value in variable declaration: need type assertion",
        "a.gopp:53:12: cannot use i (variable of interface type I) as P value in variable declaration",
        "a.gopp:59:6: impossible type assertion: j.(W)",
        "    W does not implement J (wrong type for method n)",
        "    have n() int",
        "    want n(int) string",
        "a.gopp:60:6: impossible type assertion: i.(C)",
        "    C does not implement I (missing method m)",
        "    have M()",
        "    want m()",
        "a.gopp:61:8: cannot convert P{} (value of struct type P) to type I: P does not implement I (method m has pointer receiver)",
        "a.gopp:62:8: cannot convert i (variable of interface type I) to type J: I does not implement J (missing method n)",
        "a.gopp:63:8: cannot convert i (variable of interface type I) to type T: need type assertion",
        "a.gopp:65:17: impossible type switch case: C",
        "    i (variable of interface type I) cannot have dynamic type C (missing method m)",
        "    have M()",
        "    want m()",
        "a.gopp:66:10: impossible type switch case: F",
        "    i (variable of interface type I) cannot have dynamic type F (F.m is a field, not a method)",
    ];
    assert_eq!(messages("a.gopp", text), expected);
}

// rule: OWN-P5
#[test]
fn a_pointer_and_a_handle_implement_what_their_methods_say() {
    let text = "package p

type I interface{ m() }
type T struct{}

func (T) m() {}

type P struct{}

func (p *P) m() {}

type N struct{}

func use(i I) {}

func f(t T, p P, n N, pt *T, pp *P, pn *N, u Unique[P], s Shared[P], o Option[I]) {
	var a I = t
	var b Local[I] = pt
	var c Local[I] = pp
	var d I = u
	var e I = s
	use(u)
	o = t
	_, _, _, _, _ = a, b, c, d, e
	_ = a.(T)
	_ = a.(*P)
	var g I = p
	var h I = n
	var k I = pn
	use(n)
	o = n
	_, _, _ = g, h, k
	_ = a.(N)
}
";
    let expected = [
        "a.gopp:28:12: cannot use n (variable of struct type N) as I value in variable \
         declaration: N does not implement I (missing method m)",
        "a.gopp:29:12: cannot use pn (variable of type *N) as I value in variable declaration: \
         *N does not implement I (missing method m)",
        "a.gopp:30:6: cannot use n (variable of struct type N) as I value in argument to use: N \
         does not implement I (missing method m)",
        "a.gopp:31:6: cannot use n (variable of struct type N) as Option[I] value in assignment: \
         N does not implement I (missing method m)",
        "a.gopp:33:6: impossible type assertion: a.(N)",
        "    N does not implement I (missing method m)",
    ];
    assert_eq!(messages("a.gopp", text), expected);
}

/// Interfaces whose methods give interfaces that embed them are compared
/// without end unless a pair that is met again counts as identical.
#[test]
fn interfaces_that_reach_themselves_through_their_methods_are_compared() {
    let text = "package p

type A interface {
	a() interface{ AB }
}

type B interface {
	b() interface{ AB }
}

type AB interface {
	a() interface {
		A
		B
	}
	b() interface {
		A
		B
	}
}

var x AB
var y interface {
	A
	B
}

var _ = x == y
var _ A = x
var _ AB = y

type C interface {
	a() interface{ C }
	c()
}

var _ C = x
";
    let expected = [
        "a.gopp:22:5: cannot declare x without a value: AB has no zero value (a AB is never nil)",
        "    give x a value, or declare it as Option[AB]",
        "a.gopp:23:5: cannot declare y without a value: interface{A; B} has no zero value (a interface{A; B} is never nil)",
        "    give y a value, or declare it as Option[interface{A; B}]",
        "a.gopp:37:11: cannot use x (variable of interface type AB) as C value in variable declaration: AB does not implement C (wrong type for method a)",
        "    have a() interface{A; B}",
        "    want a() interface{C}",
    ];
    assert_eq!(messages("a.gopp", text), expected);
}

/// The work of deciding what implements an interface grows with the
/// types, their methods and the interfaces that they embed, not faster.
#[test]
fn the_steps_of_looking_up_methods_grow_with_the_input_not_faster() {
    let steps = |text: String| {
        let loader = Loader::default();
        let checked = check_texts("p", &[("a.gopp", &text)], &loader, &ThreadedRodeo::new());
        assert_eq!(checked.messages, [] as [&str; 0]);
        checked.steps + loader.types.get().unwrap().steps()
    };
    // Types next to each other, each put into an interface of its own.
    let siblings = |count: usize| {
        let all: String = (0..count)
            .map(|i| {
                format!(
                    "type T{i} struct{{}}\nfunc (T{i}) m{i}() {{}}\nfunc (T{i}) n() {{}}\n\
                     var _ interface {{ m{i}(); n() }} = T{i}{{}}\n"
                )
            })
            .collect();
        format!("package p\n{all}")
    };
    // One type with many methods, put into an interface that has them all.
    let many_methods = |count: usize| {
        let methods: String = (0..count)
            .map(|i| format!("func (T) m{i}() {{}}\n"))
            .collect();
        let wanted: String = (0..count).map(|i| format!("\tm{i}()\n")).collect();
        format!(
            "package p\ntype T struct{{}}\n{methods}type I interface {{\n{wanted}}}\nvar _ I = T{{}}\n"
        )
    };
    // An interface that embeds an interface that embeds an interface.
    let embedded_interfaces = |depth: usize| {
        let all: String = (1..depth)
            .map(|i| format!("type I{i} interface {{ I{}; m{i}() }}\n", i - 1))
            .collect();
        format!(
            "package p\ntype I0 interface {{ m0() }}\n{all}func f(x I{}) I0 {{ return x }}\n",
            depth - 1
        )
    };
    // Interfaces next to each other, each with a method and two embedded
    // interfaces.
    let sibling_interfaces = |count: usize| {
        let all: String = (0..count)
            .map(|i| {
                format!(
                    "type I{i} interface {{ A; B; m{i}() }}\nfunc f{i}(x I{i}) A {{ return x }}\n"
                )
            })
            .collect();
        format!("package p\ntype A interface {{ a() }}\ntype B interface {{ b() }}\n{all}")
    };
    let sizes = [500, 1_000, 2_000, 4_000];
    linear::assert_near_linear(&sizes, |n| steps(siblings(n)));
    linear::assert_near_linear(&sizes, |n| steps(many_methods(n)));
    linear::assert_near_linear(&sizes, |n| steps(sibling_interfaces(n)));
    // An interface shares the methods of the interfaces that it embeds:
    // the work for a chain of them grows with its length.
    linear::assert_near_linear(&sizes, |n| steps(embedded_interfaces(n)));
    // So does a chain of constraints, each with the terms of the one
    // before it and a method of its own.
    let embedded_constraints = |depth: usize| {
        let all: String = (1..depth)
            .map(|i| format!("type C{i} interface {{ C{}; m{i}() }}\n", i - 1))
            .collect();
        format!(
            "package p\ntype C0 interface {{ ~int | ~string }}\n{all}func f[P C{}]() {{}}\n",
            depth - 1
        )
    };
    linear::assert_near_linear(&sizes, |n| steps(embedded_constraints(n)));
}

#[test]
fn method_declarations_are_checked_as_go_checks_them() {
    let text = "package p

type T1 struct {
	f int
}

func (T1) m()    {}
func (T1) m()    {}
func (x *T1) f() {}

type T1b struct {
	int
}

func (T1b) int() {}

type T1c struct {
	*T2
}

func (T1c) T2() int { return 0 }

func (*T2) f() {}

type T2 struct {
	f int
}

type T5 interface {
	m() int
}

func (T5) m1() {}
func (T5) m2() {}

type ptr *int

func (ptr) _()  {}
func (*ptr) _() {}

func (int) m()        {}
func ([]int) m()      {}
func (error) m()      {}
func (*error) m()     {}
func (x any) m()      {}

type T0 struct{}
type A0 = T0
type Ai = int
type A3 = *T0
type A10 = **T0

func (Ai) m1()  {}
func (T0) m1()  {}
func (A0) m1()  {}
func (A0) m2()  {}
func (A3) m3()  {}
func (A10) m1() {}

var x0 T0
var _ interface {
	m1()
	m2()
	m3()
} = &x0

type G[P any] struct{}
type GA = G[int]

func (GA) m()         {}
func (*GA) n()        {}
func (G[_, _]) m1()   {}
func (G[[]int]) m2()  {}
func (g G[P]) m3()    {}
func (g *G[P]) m3()   {}
func (**T0) pp()      {}
func (*struct{}) ps() {}

type U6 interface {
	m()
	m()
}
type M32 interface{ m() int32 }
type M64 interface{ m() int64 }
type U7 interface {
	M32
	m()
}
type U8 interface {
	m()
	M32
}
type U9 interface {
	M32
	M64
}
type U10 interface {
	M32
	M32
	m() int32
}
";
    let expected = [
        "a.gopp:8:11: method T1.m already declared",
        "    a.gopp:7:11: other declaration of method m",
        "a.gopp:9:14: field and method with the same name f",
        "    a.gopp:4:2: other declaration of f",
        "a.gopp:12:2: struct embedding is not part of Go++: use `@Extends` to inherit from a struct, or give the field a name",
        "a.gopp:15:12: field and method with the same name int",
        "    a.gopp:12:2: other declaration of int",
        "a.gopp:18:2: struct embedding is not part of Go++: use `@Extends` to inherit from a struct, or give the field a name",
        "a.gopp:21:12: field and method with the same name T2",
        "    a.gopp:18:3: other declaration of T2",
        "a.gopp:23:12: field and method with the same name f",
        "    a.gopp:26:2: other declaration of f",
        "a.gopp:33:7: invalid receiver type T5 (pointer or interface type)",
        "a.gopp:34:7: invalid receiver type T5 (pointer or interface type)",
        "a.gopp:38:7: invalid receiver type ptr (pointer or interface type)",
        "a.gopp:39:8: invalid receiver type ptr (pointer or interface type)",
        "a.gopp:41:7: cannot define new methods on non-local type int",
        "a.gopp:42:7: invalid receiver type []int",
        "a.gopp:43:7: cannot define new methods on non-local type error",
        "a.gopp:44:8: cannot define new methods on non-local type error",
        "a.gopp:45:9: invalid receiver type any",
        "a.gopp:53:7: cannot define new methods on non-local type Ai",
        "a.gopp:55:11: method T0.m1 already declared",
        "    a.gopp:54:11: other declaration of method m1",
        "a.gopp:58:7: invalid receiver type A10",
        "a.gopp:65:5: cannot keep &x0 (value of type *T0) in a value of type interface{m1(); m2(); m3()}: an interface value may be given to another goroutine, and a *T0 borrows what it points to",
        "    keep it in a Local[interface{m1(); m2(); m3()}], or hold a Shared",
        "a.gopp:70:7: cannot define new methods on instantiated type G[int]",
        "a.gopp:71:8: cannot define new methods on instantiated type G[int]",
        "a.gopp:72:7: receiver declares 2 type parameters, but receiver base type declares 1",
        "a.gopp:73:9: receiver type parameter []int must be an identifier",
        "a.gopp:75:16: method G.m3 already declared",
        "    a.gopp:74:15: other declaration of method m3",
        "a.gopp:76:8: invalid receiver type **T0",
        "a.gopp:77:8: invalid receiver type *struct{}",
        "a.gopp:81:2: duplicate method m",
        "    a.gopp:80:2: other declaration of method m",
        "a.gopp:86:2: duplicate method m",
        "    a.gopp:87:2: other declaration of method m",
        "a.gopp:91:2: duplicate method m",
        "    a.gopp:90:2: other declaration of method m",
        "a.gopp:95:2: duplicate method m",
        "    a.gopp:94:2: other declaration of method m",
    ];
    assert_eq!(messages("a.gopp", text), expected);
}

// rule: TYP-25
#[test]
fn the_names_of_fields_are_checked_and_no_field_is_embedded() {
    let text = "package p

type T struct {
	x int
	y func()
}

func (*T) n() {}

func g() {
	_ = (*T).n
	_ = T.a
	_ = T.x
	_ = T.n
	_ = T.y
	_ = (*T).y
}

type S2 struct {
	a, b int
	_    int
	_    int
	a    string
}
";
    let expected = [
        "a.gopp:12:8: T.a undefined (type T has no field or method a)",
        "a.gopp:13:8: T.x undefined (type T has no method x)",
        "a.gopp:14:8: invalid method expression T.n (needs pointer receiver (*T).n)",
        "a.gopp:15:8: T.y undefined (type T has no method y)",
        "a.gopp:16:11: (*T).y undefined (type *T has no method y)",
        "a.gopp:23:2: a redeclared",
        "    a.gopp:20:2: other declaration of a",
    ];
    assert_eq!(messages("a.gopp", text), expected);
    let text = "package p\ntype I interface{}\ntype S struct {\n\t*I\n\ta int\n\ta string\n}\n";
    let expected = [
        "a.gopp:4:2: struct embedding is not part of Go++: use `@Extends` to inherit from a \
         struct, or give the field a name",
        "a.gopp:6:2: a redeclared",
        "    a.gopp:5:2: other declaration of a",
    ];
    assert_eq!(messages("a.gopp", text), expected);
}

#[test]
fn a_constraint_is_no_type_of_a_variable_as_go_checks_it() {
    let text = "package p

type I1 interface{ comparable }
type I2 interface{ ~int | ~string }
type I3 interface {
	I1
	I2
}
type M interface{ m() }
type G[P any] struct{}

func f1[P any]() {}

var (
	_ I1
	_ I2
	_ I3
	_ comparable
	_ []I1
	_ [10]I2
	_ *I3
	_ map[I1]I2
	_ chan I3
	_ func(I1)
	_ func() I2
	_ M
	_ G[I2]
	_ = f1[I2]
	_ = [...]I3{}
	_ = (*interface{ int })(nil)
	_ = I1(1)
	_ = I2(2)
	_ = M(nil)
)

type S struct {
	I2
	f I1
	*I3
}

func f(x any, p I2) I1 {
	_ = x.(I3)
	switch x.(type) {
	case I2:
	}
	var _ I3
	_ = new(I1)
	_ = make([]I2, 1)
	return nil
}

func g[P I2, Q comparable](p P) {}
";
    let expected = [
        "a.gopp:15:4: cannot use type I1 outside a type constraint: interface is (or embeds) comparable",
        "a.gopp:16:4: cannot use type I2 outside a type constraint: interface contains type constraints",
        "a.gopp:17:4: cannot use type I3 outside a type constraint: interface contains type constraints",
        "a.gopp:18:4: cannot use type comparable outside a type constraint: interface is (or embeds) comparable",
        "a.gopp:19:6: cannot use type I1 outside a type constraint: interface is (or embeds) comparable",
        "a.gopp:20:8: cannot use type I2 outside a type constraint: interface contains type constraints",
        "a.gopp:21:5: cannot use type I3 outside a type constraint: interface contains type constraints",
        "a.gopp:22:8: cannot use type I1 outside a type constraint: interface is (or embeds) comparable",
        "a.gopp:22:11: cannot use type I2 outside a type constraint: interface contains type constraints",
        "a.gopp:23:9: cannot use type I3 outside a type constraint: interface contains type constraints",
        "a.gopp:24:9: cannot use type I1 outside a type constraint: interface is (or embeds) comparable",
        "a.gopp:25:11: cannot use type I2 outside a type constraint: interface contains type constraints",
        "a.gopp:27:6: cannot use type I2 outside a type constraint: interface contains type constraints",
        "a.gopp:28:9: cannot use type I2 outside a type constraint: interface contains type constraints",
        "a.gopp:29:11: cannot use type I3 outside a type constraint: interface contains type constraints",
        "a.gopp:30:8: cannot use type interface{int} outside a type constraint: interface contains type constraints",
        "a.gopp:31:6: cannot use interface I1 in conversion (contains specific type constraints or is comparable)",
        "a.gopp:32:6: cannot use interface I2 in conversion (contains specific type constraints or is comparable)",
        "a.gopp:37:2: cannot use type I2 outside a type constraint: interface contains type constraints",
        "a.gopp:37:2: struct embedding is not part of Go++: use `@Extends` to inherit from a struct, or give the field a name",
        "a.gopp:38:4: cannot use type I1 outside a type constraint: interface is (or embeds) comparable",
        "a.gopp:39:2: struct embedding is not part of Go++: use `@Extends` to inherit from a struct, or give the field a name",
        "a.gopp:39:3: cannot use type I3 outside a type constraint: interface contains type constraints",
        "a.gopp:42:17: cannot use type I2 outside a type constraint: interface contains type constraints",
        "a.gopp:42:21: cannot use type I1 outside a type constraint: interface is (or embeds) comparable",
        "a.gopp:43:9: cannot use type I3 outside a type constraint: interface contains type constraints",
        "a.gopp:45:7: cannot use type I2 outside a type constraint: interface contains type constraints",
        "a.gopp:47:8: cannot use type I3 outside a type constraint: interface contains type constraints",
        "a.gopp:48:6: undefined: new",
        "    Go++ has no new: write NewUnique(T{}) for one owner or NewShared(T{}) for several",
        "a.gopp:49:6: cannot make a []I2 with a length: I2 has no zero value (a I2 is never nil)",
        "    use make([]I2, 0, n) and append the elements",
        "a.gopp:49:13: cannot use type I2 outside a type constraint: interface contains type constraints",
        "a.gopp:50:9: cannot use nil as I1 value in return statement",
        "    a I1 is never nil: use Option[I1]",
    ];
    assert_eq!(messages("a.gopp", text), expected);
}

#[test]
fn the_terms_of_a_union_are_checked_as_go_checks_them() {
    let text = "package p

type MyInt int
type M interface{ m() }
type C interface{ comparable }
type E interface{ ~int | ~string }

type (
	_ interface{ ~int | ~string }
	_ interface{ ~MyInt }
	_ interface{ ~M }
	_ interface{ int | M }
	_ interface{ int | comparable }
	_ interface{ int | C }
	_ interface{ int | E }
	_ interface{ int | int }
	_ interface{ int | ~int }
	_ interface{ ~int | MyInt }
	_ interface{ ~int | ~int | string }
	_ interface{ MyInt | int }
	_ interface{ ~comparable }
	_ interface{ ~any }
)

func _[P any, Q P]()                           {}
func _[P any, Q interface{ P }]()              {}
func _[P any, Q interface{ ~P }]()             {}
func _[P any, Q interface{ int | P }]()        {}
func _[P any, Q interface{ int | ~P }]()       {}
func _[P any, Q ~P]()                          {}
func _[P any, Q int | P]()                     {}
func _[P interface{ int | string }, Q ~int]()  {}
func _[P ~MyInt]()                             {}
";
    let expected = [
        "a.gopp:10:15: invalid use of ~ (underlying type of MyInt is int)",
        "a.gopp:11:15: invalid use of ~ (M is an interface)",
        "a.gopp:12:21: cannot use p.M in union (p.M contains methods)",
        "a.gopp:13:21: cannot use comparable in union",
        "a.gopp:14:21: cannot use p.C in union (p.C embeds comparable)",
        "a.gopp:16:21: overlapping terms int and int",
        "a.gopp:17:21: overlapping terms ~int and int",
        "a.gopp:18:22: overlapping terms p.MyInt and ~int",
        "a.gopp:19:22: overlapping terms ~int and ~int",
        "a.gopp:21:15: invalid use of ~ (comparable is an interface)",
        "a.gopp:22:15: invalid use of ~ (any is an interface)",
        "a.gopp:25:17: cannot use a type parameter as constraint",
        "a.gopp:26:28: term cannot be a type parameter",
        "a.gopp:27:29: type in term ~P cannot be a type parameter",
        "a.gopp:28:34: term cannot be a type parameter",
        "a.gopp:29:35: type in term ~P cannot be a type parameter",
        "a.gopp:30:18: type in term ~P cannot be a type parameter",
        "a.gopp:31:23: term cannot be a type parameter",
        "a.gopp:33:10: invalid use of ~ (underlying type of MyInt is int)",
    ];
    assert_eq!(messages("a.gopp", text), expected);
}

// rule: GEN-1, GEN-2, GEN-11
#[test]
fn a_predeclared_constraint_of_gopp_is_embedded_and_is_no_term_of_a_union() {
    let text = "package p

type A interface{ int | Copy }
type B interface{ Clone | Send }
type C interface{ ~Sync }
type D interface {
	Copy
	Send
	m()
}

func f[T Copy, U interface{ Clone; Send }, V D]() {}
";
    let expected = [
        "a.gopp:3:25: cannot use Copy in union",
        "a.gopp:4:19: cannot use Clone in union",
        "a.gopp:4:27: cannot use Send in union",
        "a.gopp:5:19: invalid use of ~ (Sync is an interface)",
    ];
    assert_eq!(messages("a.gopp", text), expected);
}

/// The type set of each constraint of the text: its terms, `comparable`
/// and the names of its methods.
#[test]
fn a_constraint_allows_the_types_that_all_its_elements_allow() {
    let text = "package p

type MyInt int
type Number interface{ ~int | ~float64 }
type Text interface{ ~string | []byte }
type Signed interface {
	Number
	~int | ~int8 | MyInt
}
type Keyed interface {
	comparable
	~int | ~string | []byte | func()
}
type Named interface {
	Number
	String() string
}
type Either interface{ Number | Text }
type Nothing interface {
	int
	string
}
type Cmp interface{ comparable }
type Plain interface{ String() string }
";
    let names = ThreadedRodeo::new();
    let loader = Loader::default();
    let checked = check_texts("p", &[("a.gopp", text)], &loader, &names);
    let types = loader.types.get().unwrap();
    let shown = |name: &str| {
        let object = checked.package.scope[&names.get(name).unwrap()];
        let set = types.type_set(checked.package.object(object).ty).unwrap();
        let terms = set.terms.as_ref().map(|terms| {
            let term = |term: &crate::types::Term| {
                let tilde = if term.tilde { "~" } else { "" };
                format!("{tilde}{}", types.display(term.ty, &names))
            };
            terms.iter().map(term).collect::<Vec<_>>().join(" | ")
        });
        let mut methods = Vec::new();
        set.methods
            .for_each(&mut |method| methods.push(names.resolve(&method.name).to_owned()));
        (terms, set.comparable, methods, set.is_empty())
    };
    let terms = |list: &str| Some(list.to_owned());
    assert_eq!(
        shown("Number"),
        (terms("~int | ~float64"), false, vec![], false)
    );
    // What both elements allow: `MyInt` is in `~int`, `~int8` in neither.
    assert_eq!(shown("Signed"), (terms("~int"), false, vec![], false));
    // Of the terms, those whose values compare.
    assert_eq!(
        shown("Keyed"),
        (terms("~int | ~string"), false, vec![], false)
    );
    let string = vec!["String".to_owned()];
    assert_eq!(
        shown("Named"),
        (terms("~int | ~float64"), false, string.clone(), false)
    );
    assert_eq!(
        shown("Either"),
        (
            terms("~int | ~float64 | ~string | []byte"),
            false,
            vec![],
            false
        )
    );
    assert_eq!(shown("Nothing"), (terms(""), false, vec![], true));
    assert_eq!(shown("Cmp"), (None, true, vec![], false));
    assert_eq!(shown("Plain"), (None, false, string, false));
}

#[test]
fn type_arguments_satisfy_their_constraints_as_go_checks_it() {
    let text = "package p

type MyInt int
type Num interface{ ~int | ~float64 }
type Exact interface{ int | float64 }
type Str interface{ String() string }
type NumStr interface {
	Num
	String() string
}
type Nothing interface {
	int
	string
}
type T struct{}

func (T) String() string { return \"\" }

type S struct{ f func() }

type G[P Num] struct{}
type H[P Exact, Q comparable] struct{}
type K[P Str] struct{}
type L[P NumStr] struct{}
type E[P Nothing] struct{}
type M[K comparable, V any] struct{}
type C[P interface{ m() P }] struct{}

type R struct{}

func (R) m() R { return R{} }

var (
	_ G[int]
	_ G[MyInt]
	_ G[string]
	_ H[MyInt, int]
	_ H[int, []int]
	_ H[int, S]
	_ H[int, any]
	_ K[T]
	_ K[int]
	_ L[MyInt]
	_ E[int]
	_ M[func(), int]
	_ M[int]
	_ M[int, int, int]
	_ G[Num]
	_ C[R]
	_ C[T]
)

func f[P Num](P)          {}
func g[P Num, Q Str]()    {}
func h[P comparable](P)   {}
func k[P Num, Q Exact](p P, q Q) {
	var _ G[P]
	var _ G[Q]
	var _ H[P, P]
	var _ H[Q, Q]
	var _ K[P]
	f(p)
	f(q)
	h(p)
}

func use() {
	f(1)
	f(\"s\")
	f[string](\"s\")
	g[int, T]()
	g[int, int]()
	g[int, int, int]()
	h([]int{})
	h(1)
	_ = f[MyInt]
}
";
    let expected = [
        "a.gopp:36:6: string does not satisfy Num (string missing in ~int | ~float64)",
        "a.gopp:37:6: MyInt does not satisfy Exact (possibly missing ~ for int in Exact)",
        "a.gopp:38:11: []int does not satisfy comparable",
        "a.gopp:39:11: S does not satisfy comparable",
        "a.gopp:42:6: int does not satisfy Str (missing method String)",
        "a.gopp:43:6: MyInt does not satisfy NumStr (missing method String)",
        "a.gopp:44:6: cannot satisfy Nothing (empty type set)",
        "a.gopp:45:6: func() does not satisfy comparable",
        "a.gopp:46:4: not enough type arguments for type M: have 1, want 2",
        "a.gopp:47:4: too many type arguments for type M: have 3, want 2",
        "a.gopp:48:6: cannot use type Num outside a type constraint: interface contains type constraints",
        "a.gopp:50:6: T does not satisfy interface{m() T} (missing method m)",
        "a.gopp:59:10: P does not satisfy Exact",
        "a.gopp:61:10: P does not satisfy Str (missing method String)",
        "a.gopp:69:2: string does not satisfy Num (string missing in ~int | ~float64)",
        "a.gopp:70:4: string does not satisfy Num (string missing in ~int | ~float64)",
        "a.gopp:72:9: int does not satisfy Str (missing method String)",
        "a.gopp:73:14: got 3 type arguments but want 2",
        "a.gopp:74:2: []int does not satisfy comparable",
    ];
    assert_eq!(messages("a.gopp", text), expected);
}

#[test]
fn type_arguments_are_inferred_as_go_infers_them() {
    let text = "package p

type MyInt int
type List []int

func f1[P any](P, P)                         {}
func f2[S ~[]E, E any](S) E                  { var e E; return e }
func f3[K comparable, V any](map[K]V) []K    { return nil }
func f4[P, Q any](P) Q                       { var q Q; return q }
func f5[P any](...P)                         {}
func f6[P any](func(P), P)                   {}
func f7[P interface{ m() Q }, Q any](P) Q    { var q Q; return q }
func f8[P *Q, Q any](P)                      {}
func f9[P interface{ ~int | ~string }](P, P) {}

type T struct{}

func (T) m() string { return \"\" }

func use(i int, s string, l List, m map[string]int, mi MyInt, c chan int, r <-chan int) {
	f1(i, i)
	f1(i, s)
	f1(1, 2.5)
	f1(1, \"a\")
	f1(i, 1)
	f1(1.5, i)
	f1(nil, nil)
	f1(mi, i)
	f1(i, mi)
	_ = f2(l)
	_ = f2([]string{})
	_ = f2(i)
	var _ int = f2(l)
	var _ string = f2(l)
	_ = f3(m)
	_ = f3(l)
	f4(i)
	_ = f4[int, string](i)
	_ = f4[int](i)
	_ = f4[string](i)
	f5(1, 2, 3)
	f5(i, s)
	f5[int]()
	f5()
	f5(l...)
	f6(func(int) {}, i)
	f6(func(int) {}, s)
	var _ string = f7(T{})
	f7(i)
	f8(&i)
	f8(i)
	f9(i, i)
	f9(1, 2)
	f9(1.5, 2)
	f1(c, r)
	f1(r, c)
	f1[int](s, s)
}

func rec[P any](p P, n int) {
	rec(p, n)
	rec(n, n)
	rec[int](n, n)
	rec(&p, n)
}
";
    // Go says where a type parameter is declared inside the message.
    let expected = [
        "a.gopp:7:52: cannot declare e without a value: E may be a type with no zero value",
        "    give e a value, or declare it as Option[E]",
        "a.gopp:9:52: cannot declare q without a value: Q may be a type with no zero value",
        "    give q a value, or declare it as Option[Q]",
        "a.gopp:12:52: cannot declare q without a value: Q may be a type with no zero value",
        "    give q a value, or declare it as Option[Q]",
        "a.gopp:22:8: in call to f1, type string of s does not match inferred type int for P",
        "a.gopp:24:8: in call to f1, mismatched types untyped int and untyped string (cannot infer P)",
        "a.gopp:26:5: cannot use 1.5 (untyped float constant) as int value in argument to f1 (truncated)",
        "a.gopp:27:2: in call to f1, cannot infer P",
        "    a.gopp:6:9: P declared here",
        "a.gopp:28:9: cannot use i (variable of type int) as MyInt value in argument to f1",
        "a.gopp:29:5: cannot use i (variable of type int) as MyInt value in argument to f1",
        "a.gopp:32:6: in call to f2, S (type int) does not satisfy ~[]E",
        "a.gopp:34:17: cannot use f2(l) (value of type int) as string value in variable declaration",
        "a.gopp:36:9: in call to f3, type List of l does not match map[K]V (cannot infer K and V)",
        "a.gopp:37:2: in call to f4, cannot infer Q",
        "    a.gopp:9:12: Q declared here",
        "a.gopp:39:6: in call to f4[int], cannot infer Q",
        "    a.gopp:9:12: Q declared here",
        "a.gopp:40:6: in call to f4[string], cannot infer Q",
        "    a.gopp:9:12: Q declared here",
        "a.gopp:42:8: in call to f5, type string of s does not match inferred type int for P",
        "a.gopp:44:2: in call to f5, cannot infer P",
        "    a.gopp:10:9: P declared here",
        "a.gopp:47:19: in call to f6, type string of s does not match inferred type int for P",
        "a.gopp:49:2: in call to f7, P (type int) does not satisfy interface{m() Q} (missing method m)",
        "a.gopp:51:2: in call to f8, P (type int) does not satisfy *Q",
        "a.gopp:54:2: float64 does not satisfy interface{~int | ~string} (float64 missing in ~int | ~string)",
        "a.gopp:57:10: cannot use s (variable of type string) as int value in argument to f1[int]",
        "a.gopp:57:13: cannot use s (variable of type string) as int value in argument to f1[int]",
        "a.gopp:61:6: cannot copy p (variable of type P constrained by any): P is not constrained by Clone",
        "    use move(p) to give the value, or &p to lend it",
    ];
    assert_eq!(messages("a.gopp", text), expected);
}

// rule: OWN-P5
#[test]
fn a_pointer_argument_is_read_as_what_it_points_to_where_its_type_does_not_fit() {
    let text = "package p

type List []int

func first[S ~[]E, E Clone](s S) E { return s[0] }
func same[P any](a, b P)           {}
func keep[P any](p P) P          { return p }

func use(l List, pl *List, ps *[]string, n int, pn *int) {
	var a int = first(l)
	var b int = first(pl)
	var c string = first(ps)
	var d *int = keep(pn)
	same(n, pn)
	same(pn, pn)
	_, _, _, _ = a, b, c, d
	first(n)
	first(pn)
}
";
    let expected = [
        "a.gopp:17:2: in call to first, S (type int) does not satisfy ~[]E",
        "a.gopp:18:2: in call to first, S (type *int) does not satisfy ~[]E",
    ];
    assert_eq!(messages("a.gopp", text), expected);
}

/// The work of finding type arguments grows with the calls and with the
/// size of the types, not faster. For one function it grows with the
/// square of the number of its type parameters where each is found from
/// the one after it, as in Go.
#[test]
fn the_steps_of_inference_grow_with_the_input_not_faster() {
    let steps = |text: String| {
        let loader = Loader::default();
        let checked = check_texts("p", &[("a.gopp", &text)], &loader, &ThreadedRodeo::new());
        assert_eq!(checked.messages, [] as [&str; 0]);
        checked.steps + loader.types.get().unwrap().steps()
    };
    let header = "package p\nfunc first[S ~[]E, E Clone](s S) E { return s[0] }\n\
                  func pair[K comparable, V any](k K, v V) map[K]V { return nil }\n";
    // Calls next to each other.
    let calls = |count: usize| {
        let all: String = (0..count)
            .map(|i| format!("var a{i} = first([]int{{{i}}})\nvar b{i} = pair({i}, a{i})\n"))
            .collect();
        format!("{header}{all}")
    };
    // A call inside a call inside a call.
    let nested = |depth: usize| {
        format!(
            "{header}var a = {}1{}\n",
            "first([]int{".repeat(depth),
            "})".repeat(depth)
        )
    };
    // A function with many type parameters, each found from its argument.
    let many_params = |count: usize| {
        let params: String = (0..count).map(|i| format!("P{i}, ")).collect();
        let list: String = (0..count).map(|i| format!("p{i} P{i}, ")).collect();
        let args: String = (0..count).map(|i| format!("{i}, ")).collect();
        format!("package p\nfunc f[{params}Q any]({list}q Q) {{}}\nfunc g() {{ f({args}1) }}\n")
    };
    let sizes = [250, 500, 1_000, 2_000];
    linear::assert_near_linear(&sizes, |n| steps(calls(n)));
    // The parser takes no deeper nesting.
    linear::assert_near_linear(&[50, 100, 200, 400], |n| steps(nested(n)));
    linear::assert_near_linear(&sizes, |n| steps(many_params(n)));
}

#[test]
fn values_of_type_parameters_are_used_as_go_checks_it() {
    let text = "package p

type Num interface{ ~int | ~float64 }
type Text interface{ ~string | ~[]byte }
type Any interface{ int | string | []int }
type Ptr interface{ *int | *string }
type Chans interface{ chan int | <-chan int }
type Empty interface {
	int
	string
}

func ops[N Num, T Text, A Any, B ~bool, S ~string](n N, t T, a A, b B, s S) {
	_ = n + n
	_ = n * 2
	_ = n < n
	_ = n == n
	_ = -n
	_ = s + s
	_ = b && b
	_ = !b
	_ = n % n
	_ = t + t
	_ = t == t
	_ = a + a
	_ = a == a
	_ = a < a
	_ = n + s
	_ = n == 1
	_ = n == \"x\"
	_ = !n
	_ = n && n
	_ = n == nil
}

func assign[N Num, I ~int, P ~*int, A any](n N, i I, p P, a A, x int, f float64) {
	n = 1
	n = 1.5
	i = 1
	i = 1.5
	n = \"s\"
	p = nil
	n = nil
	a = nil
	n = n
	n = i
	x = i
	x = n
	i = x
	var _ any = n
	var _ interface{ m() } = n
	p = &x
	var _ *int = p
	var _ float64 = n
}

func indirect[P ~*int, Q Ptr, R ~*int | ~*string, N Num, A any](p P, q Q, r R, n N, a A) {
	_ = *p
	_ = *q
	_ = *r
	_ = *n
	_ = *a
}

func chans[C chan int, D Chans, E chan int | chan string, F chan<- int | <-chan int, N Num, A any, Z Empty](c C, d D, e E, f F, n N, a A, z Z) {
	c <- 1
	_ = <-c
	_ = <-d
	d <- 1
	_ = <-e
	_ = <-f
	_ = <-n
	a <- 1
	_ = z == z
	_ = z + z
	<-z
}
";
    let expected = [
        "a.gopp:22:6: invalid operation: operator % not defined on n (variable of type N constrained by Num)",
        "a.gopp:23:6: invalid operation: operator + not defined on t (variable of type T constrained by Text)",
        "a.gopp:24:6: invalid operation: t == t (incomparable types in type set)",
        "a.gopp:25:6: invalid operation: operator + not defined on a (variable of type A constrained by Any)",
        "a.gopp:26:6: invalid operation: a == a (incomparable types in type set)",
        "a.gopp:27:6: invalid operation: a < a (type parameter A cannot use operator <)",
        "a.gopp:28:6: invalid operation: n + s (mismatched types N and S)",
        "a.gopp:30:11: invalid operation: n == \"x\" (mismatched types N and untyped string)",
        "a.gopp:31:7: invalid operation: operator ! not defined on n (variable of type N constrained by Num)",
        "a.gopp:32:6: invalid operation: operator && not defined on n (variable of type N constrained by Num)",
        "a.gopp:33:11: invalid operation: n == nil (mismatched types N and untyped nil)",
        "a.gopp:38:6: cannot use 1.5 (untyped float constant) as N value in assignment",
        "a.gopp:40:6: cannot use 1.5 (untyped float constant) as I value in assignment",
        "a.gopp:41:6: cannot use \"s\" (untyped string constant) as N value in assignment",
        "a.gopp:42:6: cannot use nil as P value in assignment",
        "a.gopp:43:6: cannot use nil as N value in assignment",
        "a.gopp:44:6: cannot use nil as A value in assignment",
        "a.gopp:46:6: cannot use i (variable of type I constrained by ~int) as N value in assignment",
        "a.gopp:47:6: cannot use i (variable of type I constrained by ~int) as int value in assignment",
        "a.gopp:48:6: cannot use n (variable of type N constrained by Num) as int value in assignment",
        "a.gopp:49:6: cannot use x (variable of type int) as I value in assignment",
        "a.gopp:51:27: cannot use n (variable of type N constrained by Num) as interface{m()} value in variable declaration: cannot assign int (in N) to interface{m()}",
        "a.gopp:54:18: cannot use n (variable of type N constrained by Num) as float64 value in variable declaration",
        "a.gopp:59:7: invalid operation: pointers of q (variable of type Q constrained by Ptr) must have identical base types",
        "a.gopp:60:7: invalid operation: pointers of r (variable of type R constrained by ~*int | ~*string) must have identical base types",
        "a.gopp:61:7: invalid operation: cannot indirect n (variable of type N constrained by Num)",
        "a.gopp:62:7: invalid operation: cannot indirect a (variable of type A constrained by any)",
        "a.gopp:69:4: invalid operation: cannot send to d (variable of type D constrained by Chans): receive-only channel <-chan int",
        "a.gopp:70:8: invalid operation: cannot receive from e (variable of type E constrained by chan int | chan string): channels chan int and chan string have different element types",
        "a.gopp:71:8: invalid operation: cannot receive from f (variable of type F constrained by chan<- int | <-chan int): send-only channel chan<- int",
        "a.gopp:72:8: invalid operation: cannot receive from n (variable of type N constrained by Num): non-channel int",
        "a.gopp:73:4: invalid operation: cannot send to a (variable of type A constrained by any): no specific channel type",
        "a.gopp:74:6: invalid operation: z == z (empty type set)",
        "a.gopp:75:6: invalid operation: operator + not defined on z (variable of type Z constrained by Empty with empty type set)",
        "a.gopp:76:4: invalid operation: cannot receive from z (variable of type Z constrained by Empty with empty type set): no specific channel type",
    ];
    assert_eq!(messages("a.gopp", text), expected);
    // `nil` is a value of a type parameter if it is one of every type
    // that the parameter may be.
    let good = "package p\nfunc clear[S ~[]int | ~[]string, M ~map[string]int](s S, m M) (S, M) \
                {\n\ts = nil\n\tm = nil\n\treturn s, m\n}\n";
    assert_eq!(messages("a.gopp", good), [] as [&str; 0]);
}

#[test]
fn a_generic_function_is_the_function_that_its_place_asks_for() {
    let text = "package p

func f1[P any](P)        {}
func f2[P any]() P       { var x P; return x }
func f3[P, Q any](P) Q   { var x Q; return x }
func f4[P any](P, P)     {}
func f5[P any](P) []P    { return nil }
func f6[P any](int) P    { var x P; return x }
func f7[P any](P) string { return \"\" }

// initialization expressions
var (
	v1           = f1 
	v2 func(int) = f2 

	v3 func(int)     = f1
	v4 func() int    = f2
	v5 func(int) int = f3
	_  func(int) int = f3[int]

	v6 func(int, int)     = f4
	v7 func(int, string)  = f4 
	v8 func(int) []int    = f5
	v9 func(string) []int = f5 

	_, _ func(int) = f1, f1
	_, _ func(int) = f1, f2 
)

// Regular assignments
func _() {
	v1 = f1 // no error here because v1 is invalid (we don't know its type) due to the error above
	var v1_ func() int
	_ = v1_
	v1_ = f1 
	v2 = f2  

	v3 = f1
	v4 = f2
	v5 = f3
	v5 = f3[int]

	v6 = f4
	v7 = f4 
	v8 = f5
	v9 = f5 

	// non-trivial LHS
	var a [2]func(string) []int
	a[0] = f5 
}

// Return statements
func _() func(int)     { return f1 }
func _() func() int    { return f2 }
func _() func(int) int { return f3 }
func _() func(int) int { return f3[int] }

func _() func(int, int) { return f4 }
func _() func(int, string) {
	return f4 
}
func _() func(int) []int { return f5 }
func _() func(string) []int {
	return f5 
}

func _() (_, _ func(int)) { return f1, f1 }
func _() (_, _ func(int)) { return f1, f2  }

// Argument passing
func g1(func(int))                           {}
func g2(func(int, int))                      {}
func g3(func(int) string)                    {}
func g4[P any](func(P) string)               {}
func g5[P, Q any](func(P) string, func(P) Q) {}
func g6(func(int), func(string))             {}

func _() {
	g1(f1)
	g1(f2 )
	g2(f4)
	g4(f6)
	g5(f6, f7)
	g6(f1, f1)
}

// Argument passing of partially instantiated functions
func h(func(int, string), func(string, int)) {}

func p[P, Q any](P, Q) {}

func _() {
	h(p, p)
	h(p[int], p[string])
}
";
    // Go names a result that has a name by it; it is a result variable
    // here.
    let expected = [
        "a.gopp:4:32: cannot declare x without a value: P may be a type with no zero value",
        "    give x a value, or declare it as Option[P]",
        "a.gopp:5:32: cannot declare x without a value: Q may be a type with no zero value",
        "    give x a value, or declare it as Option[Q]",
        "a.gopp:8:32: cannot declare x without a value: P may be a type with no zero value",
        "    give x a value, or declare it as Option[P]",
        "a.gopp:13:17: cannot use generic function f1 without instantiation",
        "a.gopp:14:17: type func(int) of v2 does not match func() P (cannot infer P)",
        "a.gopp:22:26: inferred type func(int, int) for func(P, P) does not match type func(int, string) of v7",
        "a.gopp:24:26: inferred type func(string) []string for func(P) []P does not match type func(string) []int of v9",
        "a.gopp:27:23: type func(int) of _ does not match func() P (cannot infer P)",
        "a.gopp:33:6: cannot declare v1_ without a value: func() int has no zero value (a func() int is never nil)",
        "    give v1_ a value, or declare it as Option[func() int]",
        "a.gopp:35:8: type func() int of v1_ does not match func(P) (cannot infer P)",
        "a.gopp:36:7: type func(int) of v2 does not match func() P (cannot infer P)",
        "a.gopp:44:7: inferred type func(int, int) for func(P, P) does not match type func(int, string) of v7",
        "a.gopp:46:7: inferred type func(string) []string for func(P) []P does not match type func(string) []int of v9",
        "a.gopp:49:6: cannot declare a without a value: [2]func(string) []int has no zero value (a func(string) []int is never nil)",
        "    give a a value, or declare it as Option[[2]func(string) []int]",
        "a.gopp:50:9: inferred type func(string) []string for func(P) []P does not match type func(string) []int of a[0]",
        "a.gopp:61:9: inferred type func(int, int) for func(P, P) does not match type func(int, string) of result variable",
        "a.gopp:65:9: inferred type func(string) []string for func(P) []P does not match type func(string) []int of result variable",
        "a.gopp:69:40: type func(int) of result variable does not match func() P (cannot infer P)",
        "a.gopp:81:5: in call to g1, type func[P any]() P of f2 does not match func(int) (cannot infer P)",
    ];
    assert_eq!(messages("a.gopp", text), expected);
}

#[test]
fn values_of_type_parameters_are_indexed_sliced_and_ranged_over_as_go_checks_it() {
    let text = "package p

type Bytes interface{ ~string | ~[]byte }
type Seq interface{ []int | [4]int | *[8]int }
type Maps interface{ map[string]int | map[string]int }
type Mixed interface{ []int | map[int]int }
type Other interface{ []int | []string }

func index[B Bytes, S Seq, M Maps, X Mixed, O Other, A any, N ~int](b B, s S, m M, x X, o O, a A, n N) {
	_ = b[0]
	_ = s[0]
	_ = s[3]
	_ = s[4]
	_ = m[\"k\"]
	_ = m[1]
	_ = x[0]
	_ = o[0]
	_ = a[0]
	_ = n[0]
	s[0] = 1
	b[0] = 1
	m[\"k\"] = 2
	_ = b[1:2]
	_ = b[1:2:3]
	_ = s[1:]
	_ = o[1:]
	_ = a[1:]
	_ = x[1:]
	var _ B = b[1:]
}

func ranges[B Bytes, S Seq, N ~int | ~int64, I ~int, C chan int | <-chan int, D chan<- int, A any, Z int | string](b B, s S, n N, i I, c C, d D, a A, z Z) {
	for range b {
	}
	for i, v := range s {
		_, _ = i, v
	}
	for range n {
	}
	for x := range i {
		var _ I = x
	}
	for v := range c {
		_ = v
	}
	for range d {
	}
	for range a {
	}
	for range z {
	}
}
";
    let expected = [
        "a.gopp:5:39: overlapping terms map[string]int and map[string]int",
        "a.gopp:15:8: cannot use 1 (untyped int constant) as string value in map index",
        "a.gopp:16:6: cannot index x (variable of type X constrained by Mixed)",
        "a.gopp:17:6: cannot index o (variable of type O constrained by Other)",
        "a.gopp:18:6: cannot index a (variable of type A constrained by any)",
        "a.gopp:19:6: cannot index n (variable of type N constrained by ~int)",
        "a.gopp:21:2: cannot assign to b[0] (neither addressable nor a map index expression)",
        "a.gopp:22:2: this write to m is lost: m is the function's own copy, and nothing reads it afterwards",
        "    declare m *M to change the value of the caller, or return m",
        "a.gopp:24:12: invalid operation: 3-index slice of string",
        "a.gopp:25:6: cannot slice s (variable of type S constrained by Seq): []int and [4]int have different underlying types",
        "a.gopp:26:6: cannot slice o (variable of type O constrained by Other): []int and []string have different underlying types",
        "a.gopp:27:6: cannot slice a (variable of type A constrained by any): no specific type in A",
        "a.gopp:28:6: cannot slice x (variable of type X constrained by Mixed): []int and map[int]int have different underlying types",
        "a.gopp:33:12: cannot range over b (variable of type B constrained by Bytes): string and []byte have different underlying types",
        "a.gopp:35:20: cannot range over s (variable of type S constrained by Seq): []int and [4]int have different underlying types",
        "a.gopp:38:12: cannot range over n (variable of type N constrained by ~int | ~int64): int and int64 have different underlying types",
        "a.gopp:46:12: cannot range over d (variable of type D constrained by chan<- int): receive from send-only channel chan<- int",
        "a.gopp:48:12: cannot range over a (variable of type A constrained by any): no specific type",
        "a.gopp:50:12: cannot range over z (variable of type Z constrained by int | string): int and string have different underlying types",
    ];
    assert_eq!(messages("a.gopp", text), expected);
}

#[test]
fn a_type_parameter_has_the_methods_of_its_constraint_and_no_fields() {
    let text = "package p

type S struct{ f int }

func (S) m() {}

type C interface {
	~struct{ f int }
	m()
}
type M interface{ m() }

func f[P C, Q M, R any, T S](p P, q Q, r R, t T, pp *P, pq *Q) {
	p.m()
	q.m()
	_ = P.m
	_ = Q.m
	var _ func() = q.m
	_ = p.f
	_ = q.f
	r.m()
	_ = r.f
	t.m()
	_ = t.f
	pp.m()
	pq.m()
	_ = pp.f
}
";
    let expected = [
        "a.gopp:19:8: p.f undefined (type P has no field or method f)",
        "a.gopp:20:8: q.f undefined (type Q has no field or method f)",
        "a.gopp:21:4: r.m undefined (type R has no field or method m)",
        "a.gopp:22:8: r.f undefined (type R has no field or method f)",
        "a.gopp:23:4: t.m undefined (type T has no field or method m)",
        "a.gopp:24:8: t.f undefined (type T has no field or method f)",
        "a.gopp:27:9: pp.f undefined (type P has no field or method f)",
    ];
    assert_eq!(messages("a.gopp", text), expected);
}

#[test]
fn conversions_and_built_in_functions_take_type_parameters_as_go_checks_it() {
    let text = "package p

type Int interface{ ~int | ~int64 }
type Num interface{ ~int | ~float64 }
type Text interface{ ~string | ~[]byte }
type MyStr string

func conv[I Int, N Num, T Text, S ~string, P ~*int, A any](i I, n N, t T, s S, p P, a A, x int, str string) {
	_ = I(n)
	_ = N(i)
	_ = float64(i)
	_ = int(n)
	_ = S(i)
	_ = string(t)
	_ = T(str)
	_ = []byte(t)
	_ = I(1)
	_ = N(1.5)
	_ = S(\"x\")
	_ = I(1.5)
	_ = I(\"x\")
	_ = A(1)
	_ = I(s)
	_ = S(n)
	_ = P(x)
	_ = (*string)(p)
	_ = int(s)
	_ = MyStr(s)
	_ = I(a)
	_ = A(i)
}

func built[S ~[]int, M ~map[string]int, C ~chan int, R ~<-chan int, B ~string | ~[]byte, X []int | map[int]int, Y map[int]int | map[string]int, N ~int, F ~float64, K ~complex128, A any](s S, m M, c C, r R, b B, x X, y Y, n N, f F, k K, a A) {
	_ = len(s) + len(m) + len(c) + len(b) + cap(s) + cap(c)
	_ = len(n)
	_ = len(a)
	_ = cap(m)
	_ = cap(b)
	clear(s)
	clear(m)
	clear(x)
	clear(b)
	clear(a)
	close(c)
	close(r)
	close(s)
	close(a)
	_ = copy(s, s)
	_ = copy(s, []int{})
	_ = copy([]byte{}, b)
	_ = copy(s, b)
	_ = copy(a, s)
	delete(m, \"k\")
	delete(m, 1)
	delete(x, 1)
	delete(y, 1)
	delete(s, 1)
	_ = make(S, 1)
	_ = make(M)
	_ = make(C, 2)
	_ = make(S)
	_ = make(X, 1)
	_ = make(N)
	_ = make(A)
	_ = min(n, n)
	_ = max(n, 1)
	_ = min(f, 2.5)
	_ = min(n, 2.5)
	_ = max(k, k)
	_ = min(n, f)
	_ = min(a, a)
	_ = real(k)
	_ = complex(f, f)
	_ = real(n)
}
";
    let expected = [
        "a.gopp:18:8: cannot convert 1.5 (untyped float value) to type N: cannot convert 1.5 (untyped float constant) to type int (in N)",
        "a.gopp:20:8: cannot convert 1.5 (untyped float value) to type I: cannot convert 1.5 (untyped float constant) to type int (in I)",
        "a.gopp:21:8: cannot convert \"x\" (untyped string value) to type I: cannot convert \"x\" (untyped string constant) to type int (in I)",
        "a.gopp:22:8: cannot convert 1 (untyped int value) to type A: A does not contain specific types",
        "a.gopp:23:8: cannot convert s (variable of type S constrained by ~string) to type I: cannot convert string (in S) to type int (in I)",
        "a.gopp:24:8: cannot convert n (variable of type N constrained by Num) to type S: cannot convert float64 (in N) to type string (in S)",
        "a.gopp:25:8: cannot convert x (variable of type int) to type P: cannot convert int to type *int (in P)",
        "a.gopp:26:16: cannot convert p (variable of type P constrained by ~*int) to type *string: cannot convert *int (in P) to type *string",
        "a.gopp:27:10: cannot convert s (variable of type S constrained by ~string) to type int: cannot convert string (in S) to type int",
        "a.gopp:29:8: cannot convert a (variable of type A constrained by any) to type I",
        "a.gopp:30:8: cannot convert i (variable of type I constrained by Int) to type A",
        "a.gopp:35:10: invalid argument: n (variable of type N constrained by ~int) for built-in len",
        "a.gopp:36:10: invalid argument: a (variable of type A constrained by any) for built-in len",
        "a.gopp:37:10: invalid argument: m (variable of type M constrained by ~map[string]int) for built-in cap",
        "a.gopp:38:10: invalid argument: b (variable of type B constrained by ~string | ~[]byte) for built-in cap",
        "a.gopp:40:2: this write to m is lost: m is the function's own copy, and nothing reads it afterwards",
        "    declare m *M to change the value of the caller, or return m",
        "a.gopp:42:8: invalid argument: cannot clear b (variable of type B constrained by ~string | ~[]byte): argument must be (or constrained by) map or slice",
        "a.gopp:43:8: invalid argument: cannot clear a (variable of type A constrained by any): argument must be (or constrained by) map or slice",
        "a.gopp:45:8: invalid operation: cannot close receive-only channel r (variable of type R constrained by ~<-chan int)",
        "a.gopp:46:8: invalid operation: cannot close non-channel s (variable of type S constrained by ~[]int)",
        "a.gopp:47:8: invalid operation: cannot close non-channel a (variable of type A constrained by any)",
        "a.gopp:50:11: invalid copy: the destination []byte{} (value of type []byte) is passed as a copy",
        "a.gopp:51:14: invalid copy: argument must be a slice; have b (variable of type B constrained by ~string | ~[]byte)",
        "a.gopp:52:11: invalid copy: argument must be a slice; have a (variable of type A constrained by any)",
        "a.gopp:54:12: cannot use 1 (untyped int constant) as string value in argument to delete",
        "a.gopp:55:9: invalid argument: x (variable of type X constrained by []int | map[int]int) is not a map",
        "a.gopp:56:9: invalid argument: maps of y (variable of type Y constrained by map[int]int | map[string]int) must have identical key types",
        "a.gopp:57:2: this write to s is lost: s is the function's own copy, and nothing reads it afterwards",
        "    declare s *S to change the value of the caller, or return s",
        "a.gopp:57:9: invalid argument: s (variable of type S constrained by ~[]int) is not a map",
        "a.gopp:61:6: invalid operation: make(S) expects 2 or 3 arguments; found 1",
        "a.gopp:62:11: invalid argument: cannot make X: []int and map[int]int have different underlying types",
        "a.gopp:63:11: invalid argument: cannot make N: type must be slice, map, or channel",
        "a.gopp:64:11: invalid argument: cannot make A: no specific type",
        "a.gopp:68:13: cannot convert 2.5 (untyped float constant) to type N",
        "a.gopp:69:10: invalid argument: k (variable of type K constrained by ~complex128) cannot be ordered",
        "a.gopp:70:13: invalid argument: mismatched types N (previous argument) and F (type of f)",
        "a.gopp:71:10: invalid argument: a (variable of type A constrained by any) cannot be ordered",
        "a.gopp:72:11: k (variable of type K constrained by ~complex128) not supported as argument to built-in real for go1.18 (see go.dev/issue/50937)",
        "a.gopp:73:14: f (variable of type F constrained by ~float64) not supported as argument to built-in complex for go1.18 (see go.dev/issue/50937)",
        "a.gopp:74:11: invalid argument: argument has type N, expected complex type",
    ];
    assert_eq!(messages("a.gopp", text), expected);
}

#[test]
fn a_generic_alias_with_type_arguments_is_what_it_stands_for() {
    let names = ThreadedRodeo::new();
    let mut loader = Loader::default();
    let lib = "package lib

type Pair[K comparable, V any] struct {
	Key K
	Val V
}
type Index[V any] = map[string]Pair[string, V]
type Set[K comparable] = map[K]bool
";
    let checked = check_texts("example.com/lib", &[("lib.gopp", lib)], &loader, &names);
    assert_eq!(checked.messages, [] as [&str; 0]);
    loader
        .packages
        .insert("example.com/lib".to_owned(), Arc::new(checked.package));
    let text = "package p

import \"example.com/lib\"

type T[P any] struct{ x P }
type List[P any] = []P
type Table[P any, Q ~int] = map[Q]List[P]
type Same[P any] = T[P]
type Int[P any] = int
type S struct{}
type Plain[P any] = S
type Ptr = *Plain[int]
type Far = Ptr

var a List[int] = []int{1}
var _ Table[string, int] = map[int][]string{}
var _ Same[int] = T[int]{x: 1}
var _ lib.Index[int] = map[string]lib.Pair[string, int]{}
var _ = lib.Index[int]{}[\"k\"].Val + 1

func first[P any](x List[P]) P { return x[0] }

var _ int = first(a)
var _ = struct{ List[int] }{}.List

func local[P any](p P) {
	type Node[Q any] struct {
		next *Node[Q]
		v    Q
	}
	type Row[Q ~int] = []Q
	var n Node[P]
	n.next.v = p
	var _ Row[int] = []int{}
	n.next.v = 1
	var _ Row[string]
	var _ Node
	type Loop[Q any] = *Loop[Q]
}

var _ List
var _ = List(nil)
var _ List[int, string]
var _ Table[int]
var _ Table[int, string]
var _ Int[string] = \"x\"
var _ string = first(a)
var _ lib.Set[func()]
var _ lib.Set

func (Same[P]) m() {}
func (Ptr) m()     {}
func (*Far) m()    {}

type Self[P any] = *Self[P]
";
    let checked = check_texts("p", &[("a.gopp", text)], &loader, &names);
    assert_eq!(
        checked.messages,
        [
            "a.gopp:21:41: cannot copy x[0] (variable of type P constrained by any) out of its place: P is not constrained by Clone",
            "    use &x[0] or mem.Replace(&x[0], v)",
            "a.gopp:24:17: struct embedding is not part of Go++: use `@Extends` to inherit from a struct, or give the field a name",
            "a.gopp:32:6: cannot declare n without a value: Node[P] has no zero value (a *Node[P] is never nil)",
            "    give n a value, or declare it as Option[Node[P]]",
            "a.gopp:33:13: cannot copy p (variable of type P constrained by any): P is not constrained by Clone",
            "    use move(p) to give the value, or &p to lend it",
            "a.gopp:35:13: cannot use 1 (untyped int constant) as P value in assignment",
            "a.gopp:36:12: string does not satisfy ~int (string missing in ~int)",
            "a.gopp:37:8: cannot use generic type Node[Q any] without instantiation",
            "a.gopp:38:7: invalid recursive type: Loop refers to itself",
            "a.gopp:41:7: cannot use generic type List[P any] without instantiation",
            "a.gopp:42:9: cannot use generic type List without instantiation",
            "a.gopp:43:7: too many type arguments for type List: have 2, want 1",
            "a.gopp:44:7: not enough type arguments for type Table: have 1, want 2",
            "a.gopp:45:18: string does not satisfy ~int (string missing in ~int)",
            "a.gopp:46:21: cannot use \"x\" (untyped string constant) as Int[string] value in variable declaration",
            "a.gopp:47:16: cannot use first(a) (value of type int) as string value in variable declaration",
            "a.gopp:48:15: func() does not satisfy comparable",
            "a.gopp:49:7: cannot use generic type lib.Set[K comparable] without instantiation",
            "a.gopp:51:7: cannot define new methods on generic alias type Same[P any]",
            "a.gopp:52:7: cannot define new methods on instantiated type Plain[int]",
            "a.gopp:53:8: cannot define new methods on instantiated type Plain[int]",
            "a.gopp:55:6: invalid recursive type: Self refers to itself",
        ]
    );
}

/// The category of each variable of a package, by its name.
fn categories(text: &str) -> Vec<String> {
    let names = ThreadedRodeo::new();
    let loader = Loader::default();
    let checked = check_texts("p", &[("a.gopp", text)], &loader, &names);
    // The variables only name their types: that some have no zero value
    // is said, and is not what is asked here.
    let unset = |message: &&String| {
        !(message.contains(" without a value: ") || message.starts_with("    give "))
    };
    let said: Vec<&String> = checked.messages.iter().filter(unset).collect();
    assert_eq!(said, [] as [&String; 0]);
    let types = loader.types.get().unwrap();
    let variables = checked.package.objects.iter();
    variables
        .filter(|object| matches!(object.kind, ObjKind::Var))
        .map(|object| {
            let name = names.resolve(&object.name);
            let owned = match types.holds_owned(object.ty) {
                true => ", owned",
                false => "",
            };
            let copy = types.copy_rule(object.ty);
            let copy = copy.map_or(String::new(), |rule| format!(", {rule:?}"));
            format!("{name}: {:?}{copy}{owned}", types.category(object.ty))
        })
        .collect()
}

// rule: OWN-C1, OWN-C2, OWN-C3, OWN-C4, OWN-R7
#[test]
fn a_type_is_copied_by_its_bytes_by_its_own_rule_or_not_at_all() {
    let text = "package p

type File struct{ fd int }

func (f *File) drop() {}

type Handle struct{ fd int }

func (h *Handle) drop()        {}
func (h Handle) Clone() Handle { return Handle{fd: h.fd} }

type Other struct{ fd int }

func (o *Other) drop()         {}
func (o *Other) Clone() *Other { return o }

@Pinned
type Mutex struct{ state int }

type Point struct{ X, Y int }
type Doc struct{ Title string }
type Node struct {
	next *Node
	kids []Node
}
type Tree struct {
	kids  []Tree
	files map[string]File
}
type Box[T any] struct{ v T }
type Reader interface{ Read() int }
type Bytes = []byte

var (
	b     bool
	n     int
	f64   float64
	p     *File
	arr   [3]Point
	pt    Point
	opt   Option[*File]
	s     string
	doc   Doc
	ints  []int
	m     map[string]int
	u     Unique[Doc]
	sh    Shared[File]
	w     Weak[File]
	ch    chan File
	fn    func(File)
	r     Reader
	e     error
	a     any
	os    Option[string]
	node  Node
	bytes Bytes
	bi    Box[int]
	bs    Box[string]
	file  File
	h     Handle
	other Other
	mu    Mutex
	files []File
	fm    map[int]File
	uf    Unique[File]
	of    Option[File]
	af    [2]File
	sf    struct{ f File }
	bf    Box[File]
	tree  Tree
)
";
    let expected = [
        "b: Trivial, Bytes",
        "n: Trivial, Bytes",
        "f64: Trivial, Bytes",
        "p: Trivial, Bytes",
        "arr: Trivial, Bytes",
        "pt: Trivial, Bytes",
        "opt: Trivial, Bytes",
        "s: Copyable, SameBytes, owned",
        "doc: Copyable, Parts, owned",
        "ints: Copyable, Elements, owned",
        "m: Copyable, Elements, owned",
        "u: Copyable, Pointee, owned",
        "sh: Copyable, Handle, owned",
        "w: Copyable, Handle, owned",
        "ch: Copyable, Handle, owned",
        "fn: Copyable, Handle, owned",
        "r: Copyable, Boxed, owned",
        "e: Copyable, Boxed, owned",
        "a: Copyable, Boxed, owned",
        "os: Copyable, Parts, owned",
        "node: Copyable, Parts, owned",
        "bytes: Copyable, Elements, owned",
        "bi: Trivial, Bytes",
        "bs: Copyable, Parts, owned",
        "file: NoCopy, owned",
        "h: Copyable, Method, owned",
        "other: NoCopy, owned",
        "mu: NoCopy",
        "files: NoCopy, owned",
        "fm: NoCopy, owned",
        "uf: NoCopy, owned",
        "of: NoCopy, owned",
        "af: NoCopy, owned",
        "sf: NoCopy, owned",
        "bf: NoCopy, owned",
        "tree: NoCopy, owned",
    ];
    assert_eq!(categories(text), expected);
}

#[test]
fn types_that_hold_each_other_are_copied_alike() {
    // `A`, `B` and `C` hold each other through slices and a map, and one
    // of them holds a file; `Plain` holds itself and nothing else.
    let text = "package p

type File struct{ fd int }

func (f *File) drop() {}

type A struct{ bs []B }
type B struct {
	cs map[string]C
	n  int
}
type C struct {
	as []A
	f  Unique[File]
}
type Plain struct{ more []Plain }
type Outer struct {
	a A
	p Plain
}

var (
	b     B
	a     A
	c     C
	plain Plain
	outer Outer
	as    []A
)
";
    let expected = [
        "b: NoCopy, owned",
        "a: NoCopy, owned",
        "c: NoCopy, owned",
        "plain: Copyable, Parts, owned",
        "outer: NoCopy, owned",
        "as: NoCopy, owned",
    ];
    assert_eq!(categories(text), expected);
}

#[test]
fn the_steps_of_categories_grow_with_the_types_not_faster() {
    let steps = |text: String| {
        let names = ThreadedRodeo::new();
        let loader = Loader::default();
        let checked = check_texts("p", &[("a.gopp", &text)], &loader, &names);
        assert_eq!(checked.messages, [] as [&str; 0]);
        let types = loader.types.get().unwrap();
        let before = types.steps();
        for object in &checked.package.objects {
            types.category(object.ty);
        }
        types.steps() - before
    };
    let declared = |count: usize, first: &str, next: &dyn Fn(usize) -> String| {
        let all: String = (1..count).map(next).collect();
        format!("package p\n{first}\n{all}")
    };
    // Each type holds the one before it, once and twice.
    let chain = |count: usize| {
        declared(count, "type T0 struct{ s string }", &|i| {
            format!(
                "type T{i} struct {{ t T{}; n int }}\nvar v{i} T{i}\n",
                i - 1
            )
        })
    };
    let diamonds = |count: usize| {
        declared(count, "type T0 struct{ s string }", &|i| {
            format!(
                "type T{i} struct {{ a, b T{}; c []T{} }}\nvar v{i} T{i}\n",
                i - 1,
                i - 1
            )
        })
    };
    // One type with many fields.
    let wide = |count: usize| {
        let types: String = (0..count)
            .map(|i| format!("type F{i} struct{{ n int }}\n"))
            .collect();
        let fields: String = (0..count).map(|i| format!("f{i} F{i}; ")).collect();
        format!("package p\n{types}type W struct {{ {fields} }}\nvar w W\n")
    };
    // Each type holds the next one through a slice, and the last the
    // first.
    let ring = |count: usize| {
        declared(
            count,
            &format!("type T0 struct{{ next []T{} }}", count - 1),
            &|i| format!("type T{i} struct {{ next []T{} }}\nvar v{i} T{i}\n", i - 1),
        )
    };
    let sizes = [250, 500, 1_000, 2_000];
    linear::assert_near_linear(&sizes, |n| steps(chain(n)));
    linear::assert_near_linear(&sizes, |n| steps(diamonds(n)));
    linear::assert_near_linear(&sizes, |n| steps(wide(n)));
    linear::assert_near_linear(&sizes, |n| steps(ring(n)));
}

// rule: OWN-C5, OWN-R7, GEN-1, GEN-2
#[test]
fn a_type_argument_is_copied_as_the_constraint_asks() {
    let names = ThreadedRodeo::new();
    let lib = "package lib

type File struct{ fd int }

func (f *File) drop() {}

type Count int

func (c *Count) drop() {}

func Dup[T ~int](x T) (T, T)      { return x, x }
func Both[T Copy](x T) (T, T)     { return x, x }
func Again[T Clone](x T) (T, T)   { return x, x }
";
    let loader = with_packages(&names, &[("lib", lib)]);
    let text = "package p

import \"lib\"

type Handle struct{ fd int }

func (h *Handle) drop()        {}
func (h Handle) Clone() Handle { return Handle{fd: h.fd} }

@Pinned
type Mutex struct{ state int }

type N int

func (n *N) drop() {}

type M int
type Doc struct{ Title string }
type Point struct{ X, Y int }
type Bad struct{ x int }

func (b Bad) drop() {}

type Pair[T Clone] struct{ a, b T }

func text[T ~string | ~int](x T) {}
func exact[T N | int](x T)       {}
func free[T any](x T)            {}
func plain[U any](u U)           { lib.Both(move(u)) }
func listed[U ~int](u U)         { lib.Both(u); lib.Dup(u) }
func copied[U Clone](u U)        { lib.Again(u); lib.Both(u) }
func mixed[U interface{ Clone; ~int | ~string }](u U) { lib.Again(u) }

func use(f lib.File, h Handle, m Mutex, n N, d Doc, p Point, s []lib.File, k M, c lib.Count) {
	lib.Dup(k)
	lib.Both(p)
	lib.Both(&f)
	lib.Again(d)
	lib.Again(h)
	lib.Again([]Doc{})
	text(\"x\")
	exact(move(n))
	free(move(f))
	var _ Pair[Doc]

	lib.Dup(move(n))
	lib.Dup(move(c))
	lib.Both(d)
	lib.Both(h)
	lib.Both([]int{})
	lib.Again(move(f))
	lib.Again(Mutex{})
	lib.Again(move(s))
	text(move(n))
	var _ Pair[lib.File]
	h.drop()
	_ = d.Clone()
	_ = h.Clone()
}
";
    let checked = check_texts("p", &[("a.gopp", text)], &loader, &names);
    let expected = [
        "a.gopp:22:14: invalid drop method: it has a pointer receiver, no parameters and no results",
        "a.gopp:29:36: U does not satisfy Copy (U is not constrained by Copy)",
        "a.gopp:31:50: U does not satisfy Copy (U is not constrained by Copy)",
        "a.gopp:46:2: N does not satisfy Copy, which the type set of T (~int) implies: N has a drop \
         method",
        "a.gopp:46:10: n is used after it was moved: it has no value until it is assigned again",
        "    a.gopp:42:8: n is moved here",
        "a.gopp:47:2: lib.Count does not satisfy Copy, which the type set of T (~int) implies: \
         lib.Count has a drop method",
        "a.gopp:48:2: Doc does not satisfy Copy (a copy of string is more than its bytes)",
        "a.gopp:49:2: Handle does not satisfy Copy (Handle has a drop method)",
        "a.gopp:50:2: []int does not satisfy Copy (a copy of []int is more than its bytes)",
        "a.gopp:51:2: lib.File does not satisfy Clone (lib.File has a drop method and no Clone() \
         lib.File)",
        "a.gopp:51:12: f is used after it was moved: it has no value until it is assigned again",
        "    a.gopp:43:7: f is moved here",
        "a.gopp:52:2: Mutex does not satisfy Clone (Mutex is @Pinned)",
        "a.gopp:53:2: []lib.File does not satisfy Clone (lib.File has a drop method and no Clone() \
         lib.File)",
        "a.gopp:54:2: N does not satisfy Clone, which the type set of T (~string | ~int) implies: N \
         has a drop method and no Clone() N",
        "a.gopp:54:7: n is used after it was moved: it has no value until it is assigned again",
        "    a.gopp:46:10: n is moved here",
        "a.gopp:55:13: lib.File does not satisfy Clone (lib.File has a drop method and no Clone() \
         lib.File)",
        "a.gopp:56:4: cannot use h.drop: drop runs when a value ends, and is not called",
        // Only a type that declares the copy method has it.
        "a.gopp:57:8: d.Clone undefined (type Doc has no field or method Clone)",
    ];
    assert_eq!(checked.messages, expected);
}

// rule: OWN-R3, OWN-R3a, OWN-R10, PAR-5, CLO-9
#[test]
fn a_value_that_is_not_copied_is_moved_or_stays_in_its_place() {
    let text = "package p

type File struct{ fd int }

func (f *File) drop() {}

type Files []File
type Doc struct{ Title string }
type Writer struct{ Out File }

@Pinned
type Mutex struct{ state int }

var global File

func openLog() File { return File{} }
func keep(f File)   {}
func check(f *File) {}
func show(d Doc)    {}

func locals(w Writer, files []File, m map[string]File, p *Writer, u Unique[File]) File {
	f := openLog()
	keep(move(f))
	keep(openLog())
	keep(Writer{}.Out)
	check(&f)
	check(&w.Out)
	check(u)
	files[0], files[1] = files[1], files[0]
	w.Out, global = global, w.Out
	w2 := Writer{Out: openLog()}
	out := []File(move(files))
	_, _ = w2, out

	g := f
	keep(f)
	h := w.Out
	i := files[0]
	j := m[\"a\"]
	k := global
	l := p.Out
	v, ok := m[\"a\"]
	w3 := Writer{Out: f}
	more := append(files, move(f))
	fs := Files(files)
	var o Option[File] = f
	var q File = u
	_, _, _, _, _, _, _, _, _, _, _, _, _ = g, h, i, j, k, l, v, ok, w3, more, fs, o, q
	return f
}

func results(w *Writer, files []File) (File, File) {
	return w.Out, files[0]
}

func pinned(mu Mutex, p *Mutex) Mutex {
	a := mu
	b := move(mu)
	c := p
	var d Mutex = p
	_, _, _, _ = a, b, c, d
	return mu
}

func captured(f File, d Doc) {
	_ = func() {
		keep(f)
		keep(move(f))
		show(d)
		g := openLog()
		keep(move(g))
	}
}

func channel(ch chan File, f File) {
	ch <- f
	ch <- move(f)
}
";
    let checked = check_texts(
        "p",
        &[("a.gopp", text)],
        &Loader::default(),
        &ThreadedRodeo::new(),
    );
    let expected = [
        "a.gopp:26:9: f is used after it was moved: it has no value until it is assigned again",
        "    a.gopp:23:7: f is moved here",
        "a.gopp:35:7: cannot copy f (variable of struct type File): File has a drop method and no \
         Clone() File",
        "    use move(f) to give the value, or &f to lend it",
        "a.gopp:36:7: cannot copy f (variable of struct type File): File has a drop method and no \
         Clone() File",
        "    use move(f) to give the value, or &f to lend it",
        "a.gopp:37:7: cannot copy w.Out (variable of struct type File) out of its place: File has \
         a drop method and no Clone() File",
        "    use &w.Out, mem.Take(&w.Out) or mem.Replace(&w.Out, v)",
        "a.gopp:38:7: cannot copy files[0] (variable of struct type File) out of its place: File \
         has a drop method and no Clone() File",
        "    use &files[0], mem.Take(&files[0]) or mem.Replace(&files[0], v)",
        "a.gopp:38:7: files is used after it was moved: it has no value until it is assigned again",
        "    a.gopp:32:16: files is moved here",
        "a.gopp:39:7: cannot copy m[\"a\"] (map index expression of struct type File) out of its \
         place: File has a drop method and no Clone() File",
        "a.gopp:40:7: cannot copy global (variable of struct type File) out of its place: File has \
         a drop method and no Clone() File",
        "    use &global, mem.Take(&global) or mem.Replace(&global, v)",
        "a.gopp:41:7: cannot copy p.Out (variable of struct type File) out of its place: File has \
         a drop method and no Clone() File",
        "    use &p.Out, mem.Take(&p.Out) or mem.Replace(&p.Out, v)",
        "a.gopp:42:11: cannot copy m[\"a\"] (map index expression of struct type File) out of its \
         place: File has a drop method and no Clone() File",
        "a.gopp:43:20: cannot copy f (variable of struct type File): File has a drop method and no \
         Clone() File",
        "    use move(f) to give the value, or &f to lend it",
        "a.gopp:44:17: cannot copy files (variable of type []File): File has a drop method and no \
         Clone() File",
        "    use move(files) to give the value, or &files to lend it",
        "a.gopp:45:14: cannot copy files (variable of type []File): File has a drop method and no \
         Clone() File",
        "    use move(files) to give the value, or &files to lend it",
        "a.gopp:46:23: f is used after it was moved: it has no value until it is assigned again",
        "    a.gopp:44:24: f is moved here",
        "a.gopp:46:23: cannot copy f (variable of struct type File): File has a drop method and no \
         Clone() File",
        "    use move(f) to give the value, or &f to lend it",
        "a.gopp:47:15: cannot copy u (variable of type Unique[File]) out of its place: File has a \
         drop method and no Clone() File",
        "    use mem.Take(u) or mem.Replace(u, v)",
        "a.gopp:53:9: cannot copy w.Out (variable of struct type File) out of its place: File has \
         a drop method and no Clone() File",
        "    use &w.Out, mem.Take(&w.Out) or mem.Replace(&w.Out, v)",
        "a.gopp:53:16: cannot copy files[0] (variable of struct type File) out of its place: File \
         has a drop method and no Clone() File",
        "    use &files[0], mem.Take(&files[0]) or mem.Replace(&files[0], v)",
        "a.gopp:57:7: cannot copy mu (variable of struct type Mutex): Mutex is @Pinned",
        "    use &mu",
        "a.gopp:58:12: cannot move mu (variable of struct type Mutex): Mutex is @Pinned, and stays \
         in its place",
        "a.gopp:60:16: cannot copy p (variable of type *Mutex): Mutex is @Pinned",
        "a.gopp:67:8: cannot copy f (variable of struct type File) out of its place: File has a \
         drop method and no Clone() File",
        "    use &f, mem.Take(&f) or mem.Replace(&f, v)",
        "a.gopp:68:13: cannot move f (variable of struct type File): move takes a local variable \
         or a parameter",
        "    use mem.Take(&f)",
        "a.gopp:76:8: cannot copy f (variable of struct type File): File has a drop method and no \
         Clone() File",
        "    use move(f) to give the value, or &f to lend it",
    ];
    assert_eq!(checked.messages, expected);
}

// rule: OWN-R3, OWN-R4, PAR-9
#[test]
fn a_place_says_how_each_use_gives_its_value() {
    let text = "package p

type File struct{ fd int }

func (f *File) drop() {}

type Doc struct {
	Title string
	Tags  []string
}

func (d Doc) Show() string  { return d.Title }
func (d *Doc) Set(t string) { d.Title = t }

type Holder struct {
	Docs []Doc
	M    map[string][]int
}

func grow(xs []int) []int { return xs }
func keep(f File)         {}

func use(h *Holder, xs []int, d Doc, fs []File, f File, n int, name string) (Doc, File) {
	m := n
	e := d
	title := d.Title
	_ = any(d)
	_ = d.Show()
	_ = d.Show
	d.Set(name)
	_ = len(xs) + m
	_ = title == name
	xs = append(xs, 1)
	xs = xs[1:]
	xs = grow(xs)
	h.Docs = append(h.Docs, d)
	h.M[\"a\"] = append(h.M[\"a\"], n)
	fs = append(fs, move(f))
	fs = fs[1:]
	ys := append(xs, 2)
	xs, ys = ys, xs
	h.Docs[0], h.Docs[n+1] = h.Docs[n+1], h.Docs[0]
	fs[0], fs[1] = fs[1], fs[0]
	g := func() Doc { return e }
	_ = g
	keep(move(fs[0]))
	return d, f
}
";
    let checked = check_texts(
        "p",
        &[("a.gopp", text)],
        &Loader::default(),
        &ThreadedRodeo::new(),
    );
    let expected = [
        "a.gopp:46:12: cannot move fs[0] (variable of struct type File): move takes a local \
         variable or a parameter",
        "    use mem.Take(&fs[0])",
        "a.gopp:47:12: f is used after it was moved: it has no value until it is assigned again",
        "    a.gopp:38:18: f is moved here",
    ];
    assert_eq!(checked.messages, expected);
    let expected = [
        "12:38 d.Title: copy",
        "13:41 t: copy, last",
        "20:36 xs: copy, last",
        "25:7 d: copy",
        "26:11 d.Title: copy",
        "27:10 d: copy",
        "28:6 d: copy",
        "29:6 d: copy",
        "30:8 name: copy",
        "33:14 xs: copy, over",
        "34:7 xs: over",
        "35:12 xs: copy, over, last",
        "36:18 h.Docs: copy, over",
        "36:26 d: copy",
        "37:20 h.M[\"a\"]: copy, over",
        "38:14 fs: move, over",
        "38:23 f: move",
        "39:7 fs: over",
        "40:15 xs: copy",
        "41:2 xs: swap",
        "41:6 ys: swap",
        "41:11 ys: move",
        "41:15 xs: move",
        "42:2 h.Docs[0]: swap",
        "42:13 h.Docs[n+1]: swap",
        "42:27 h.Docs[n+1]: move",
        "42:40 h.Docs[0]: move",
        "43:2 fs[0]: swap",
        "43:9 fs[1]: swap",
        "43:17 fs[1]: move",
        "43:24 fs[0]: move",
        "44:27 e: copy",
        "47:9 d: copy, last",
        "47:12 f: move",
    ];
    assert_eq!(transfers(&checked), expected);
}

// rule: OWN-R3
#[test]
fn slices_maps_and_strings_are_copied_where_they_are_given() {
    let text = "package p

type Table struct {
	rows  []int
	names map[string]int
}

func fill(rows []int, names map[string]int) {}

func use(t Table, n int, text string) ([]int, map[string]int) {
	rows := t.rows
	names := t.names
	var other map[string]int
	other = names
	fill(rows, names)
	fill(t.rows[1:], map[string]int{})
	count := n
	same := text
	_, _ = count, same
	p := &rows
	q := p
	_ = q
	_ = len(rows) + len(other)
	return rows, t.names
}
";
    let checked = check_texts(
        "p",
        &[("a.gopp", text)],
        &Loader::default(),
        &ThreadedRodeo::new(),
    );
    assert_eq!(checked.messages, [] as [&str; 0]);
    let expected = [
        "11:10 t.rows: copy",
        "12:11 t.names: copy",
        "14:10 names: copy",
        "15:7 rows: copy",
        "15:13 names: copy, last",
        "18:10 text: copy, last",
        "24:9 rows: copy",
        "24:15 t.names: copy",
    ];
    assert_eq!(transfers(&checked), expected);
}

// rule: OWN-R3b, RNG-1
#[test]
fn a_plain_range_variable_is_read_and_its_parts_are_not_changed() {
    let text = "package p

type File struct{ fd int }

func (f *File) drop() {}

type Doc struct {
	Title string
	Tags  []string
	N     int
	Next  *Doc
	Own   Unique[Doc]
}

type Key struct{ N int }

func openLog() File { return File{} }
func keep(f File)   {}
func check(f *File) {}
func show(d Doc)    {}

func good(docs []Doc, nums []int, grid [][]int, ptrs []*Doc, ch chan Doc, byName map[string]Doc) {
	var out []Doc
	for i, d := range docs {
		show(d)
		out = append(out, d)
		d = docs[i]
		d.Next.Title = \"x\"
		i++
	}
	for _, &d := range docs {
		d.Title = \"x\"
		d.Tags[0] = \"x\"
		d.N++
		_ = &d
	}
	for k, &d := range byName {
		d.N += len(k)
	}
	for _, x := range nums {
		x *= 2
		x++
	}
	for _, row := range grid {
		row = append(row, 1)
		_ = row[0]
	}
	for _, p := range ptrs {
		p.Title = \"x\"
		p.Tags[0] = \"x\"
	}
	for d := range ch {
		d.Title = \"x\"
		_ = &d
	}
	for i := range 3 {
		i++
	}
}

func bad(docs []Doc, grid [][]int, byName map[Key]map[string]int, arr [4]Doc) {
	for _, d := range docs {
		d.Title = \"x\"
		d.Tags[0] = \"x\"
		d.Tags = append(d.Tags, \"x\")
		d.N++
		d.N += 2
		d.Own.Title = \"x\"
		_ = &d
		_ = &d.Tags[0]
		_ = &d.Tags[1:]
		(d).Title, d.N = \"x\", 1
	}
	for _, row := range grid {
		row[0] = 1
		clear(row)
	}
	for k, v := range byName {
		k.N = 1
		v[\"a\"] = 1
		delete(v, \"a\")
		clear(v)
	}
	for _, d := range arr {
		d.N = 1
	}
}

func views(files []File, byName map[string]File) {
	for _, f := range files {
		_ = f.fd
		check(&f)
		keep(f)
		keep(move(f))
		f = openLog()
		f.fd = 1
	}
	for _, &f := range files {
		check(&f)
		f.fd = 1
		f = openLog()
		keep(f)
		keep(move(f))
	}
	for _, f := range byName {
		keep(f)
	}
	var f File
	for _, f = range files {
	}
	var name string
	for name, f = range byName {
	}
	_ = name
}
";
    let expected = [
        "a.gopp:63:3: cannot assign to d.Title: d is a copy of an element, and the change would be \
         lost",
        "    write &d in the range clause to name the element itself",
        "a.gopp:64:3: cannot assign to d.Tags[0]: d is a copy of an element, and the change would \
         be lost",
        "    write &d in the range clause to name the element itself",
        "a.gopp:65:3: cannot assign to d.Tags: d is a copy of an element, and the change would be \
         lost",
        "    write &d in the range clause to name the element itself",
        "a.gopp:66:3: cannot assign to d.N: d is a copy of an element, and the change would be \
         lost",
        "    write &d in the range clause to name the element itself",
        "a.gopp:67:3: cannot assign to d.N: d is a copy of an element, and the change would be \
         lost",
        "    write &d in the range clause to name the element itself",
        "a.gopp:68:3: cannot assign to d.Own.Title: d is a copy of an element, and the change \
         would be lost",
        "    write &d in the range clause to name the element itself",
        "a.gopp:69:8: cannot take the address of d: d is a copy of an element, and the change \
         would be lost",
        "    write &d in the range clause to name the element itself",
        "a.gopp:70:8: cannot take the address of d.Tags[0]: d is a copy of an element, and the \
         change would be lost",
        "    write &d in the range clause to name the element itself",
        "a.gopp:71:8: cannot take the address of d.Tags[1:]: d is a copy of an element, and the \
         change would be lost",
        "    write &d in the range clause to name the element itself",
        "a.gopp:72:3: cannot assign to (d).Title: d is a copy of an element, and the change would \
         be lost",
        "    write &d in the range clause to name the element itself",
        "a.gopp:72:14: cannot assign to d.N: d is a copy of an element, and the change would be \
         lost",
        "    write &d in the range clause to name the element itself",
        "a.gopp:75:3: cannot assign to row[0]: row is a copy of an element, and the change would \
         be lost",
        "    write &row in the range clause to name the element itself",
        "a.gopp:76:9: cannot clear row: row is a copy of an element, and the change would be lost",
        "    write &row in the range clause to name the element itself",
        "a.gopp:79:3: cannot assign to k.N: k is a copy of a key, and the change would be lost",
        "a.gopp:80:3: cannot assign to v[\"a\"]: v is a copy of an element, and the change would \
         be lost",
        "    write &v in the range clause to name the element itself",
        "a.gopp:81:10: cannot delete v: v is a copy of an element, and the change would be lost",
        "    write &v in the range clause to name the element itself",
        "a.gopp:82:9: cannot clear v: v is a copy of an element, and the change would be lost",
        "    write &v in the range clause to name the element itself",
        "a.gopp:85:3: cannot assign to d.N: d is a copy of an element, and the change would be \
         lost",
        "    write &d in the range clause to name the element itself",
        "a.gopp:92:10: cannot take the address of f: f is an element that the loop only reads",
        "    write &f in the range clause to name the element itself",
        "a.gopp:93:8: cannot copy f (variable of struct type File) out of what is ranged over: \
         File has a drop method and no Clone() File",
        "    write &f in the range clause, and use mem.Take(&f) or mem.Replace(&f, v)",
        "a.gopp:94:13: cannot move f (variable of struct type File): the loop only reads f",
        "    write &f in the range clause, and use mem.Take(&f) or mem.Replace(&f, v)",
        "a.gopp:95:3: cannot assign to f: f is an element that the loop only reads",
        "    write &f in the range clause to name the element itself",
        "a.gopp:96:3: cannot assign to f.fd: f is an element that the loop only reads",
        "    write &f in the range clause to name the element itself",
        "a.gopp:102:8: cannot copy f (variable of struct type File) out of its place: File has a \
         drop method and no Clone() File",
        "    use &f, mem.Take(&f) or mem.Replace(&f, v)",
        "a.gopp:103:13: cannot move f (variable of struct type File): move takes a local variable \
         or a parameter",
        "    use mem.Take(&f)",
        "a.gopp:106:8: cannot copy f (variable of struct type File) out of what is ranged over: \
         File has a drop method and no Clone() File",
        "    write &f in the range clause, and use mem.Take(&f) or mem.Replace(&f, v)",
        "a.gopp:109:9: cannot copy what files (variable of type []File) holds to f: File has a \
         drop method and no Clone() File",
        "    declare the variable in the range clause, with := and &",
        "a.gopp:112:12: cannot copy what byName (variable of type map[string]File) holds to f: \
         File has a drop method and no Clone() File",
        "    declare the variable in the range clause, with := and &",
    ];
    assert_eq!(messages("a.gopp", text), expected);
}

// rule: OWN-R4, OWN-R10
#[test]
fn what_is_taken_out_of_a_place_leaves_a_value_there() {
    let names = ThreadedRodeo::new();
    let mem = "package mem\nfunc Take[T any](p *T) T\nfunc Replace[T any](p *T, v T) T\n";
    let loader = with_packages(&names, &[("mem", mem)]);
    let text = "package p

import \"mem\"

type File struct{ fd int }

func (f *File) drop() {}

type Node struct {
	Name string @Required
}

@Pinned
type Mutex struct{ state int }

type Holder struct {
	Out   File
	Owner Unique[File]
	Opt   Option[File]
	Node  Node
	Run   func()
	mu    Mutex
}

func good(h *Holder, f File, files []File, byName map[string]File) {
	a := mem.Take(&h.Out)
	b := mem.Replace(&h.Owner, NewUnique(File{}))
	c := mem.Take(&h.Opt)
	d := mem.Take[File](&files[0])
	e := mem.Take(&f)
	g := mem.Replace(&h.Node, Node{Name: \"n\"})
	_, _, _, _, _, _ = a, b, c, d, e, g
	for _, &f := range files {
		_ = mem.Take(&f)
	}
	for _, &f := range byName {
		_ = mem.Replace(&f, File{})
	}
	_ = func() { _ = mem.Take(&f) }
}

func bad(h *Holder, mu *Mutex, files []File) {
	a := mem.Take(&h.Owner)
	b := mem.Take(&h.Node)
	c := mem.Take(&h.Run)
	d := mem.Take(mu)
	e := mem.Replace(&h.mu, Mutex{})
	_, _, _, _, _ = a, b, c, d, e
	for _, f := range files {
		_ = mem.Take(&f)
	}
}

func generic[T any, C Copy, N ~int | ~string](p *T, v T, c *C, n *N) T {
	_ = mem.Replace(p, move(v))
	_ = mem.Take(c)
	_ = mem.Take(n)
	return mem.Take(p)
}
";
    let checked = check_texts("p", &[("a.gopp", text)], &loader, &names);
    let expected = [
        "a.gopp:43:7: cannot use mem.Take(&h.Owner): Unique[File] has no zero value to leave in \
         the place",
        "    use mem.Replace, which leaves a value of yours",
        "a.gopp:44:7: cannot use mem.Take(&h.Node): Node has no zero value to leave in the place",
        "    use mem.Replace, which leaves a value of yours",
        "a.gopp:45:7: cannot use mem.Take(&h.Run): func() has no zero value to leave in the place",
        "    use mem.Replace, which leaves a value of yours",
        "a.gopp:46:7: cannot use mem.Take(mu): Mutex is @Pinned, and stays in its place",
        "a.gopp:47:7: cannot use mem.Replace(&h.mu, Mutex{}): Mutex is @Pinned, and stays in its \
         place",
        "a.gopp:50:17: cannot take the address of f: f is an element that the loop only reads",
        "    write &f in the range clause to name the element itself",
        "a.gopp:56:6: cannot use mem.Take(c): C may be a type with no zero value to leave in the \
         place",
        "    use mem.Replace, which leaves a value of yours",
        "a.gopp:58:9: cannot use mem.Take(p): T may be a type with no zero value to leave in the \
         place",
        "    use mem.Replace, which leaves a value of yours",
    ];
    assert_eq!(checked.messages, expected);
}

// rule: GEN-3, GEN-4, OWN-R3, OWN-R3a
#[test]
fn a_value_of_a_type_parameter_is_given_as_one_of_a_type_like_it() {
    // Each use in a function of its own, by the line of the function.
    let uses = [
        "a := v; _ = a",
        "keep(v)",
        "xs = append(xs, v); _ = xs",
        "m[\"a\"] = v; _ = len(m)",
        "_ = Cell[X]{f: v}",
        "ch <- v",
        "b := xs[0]; _ = b",
        "c := *p; _ = c",
        "d, ok := m[\"a\"]; if ok { borrow(&d) }",
        "for _, e := range xs { keep(e) }",
        "_ = func() { keep(v) }",
        "return xs[0]",
        "return *p",
        "return v",
        "c := move(v); _ = c",
        "keep(move(v))",
        "xs = append(xs, move(v)); _ = xs",
        "_ = &v",
        "borrow(&v); borrow(p); borrow(&xs[0])",
        "xs[0], xs[1] = xs[1], xs[0]; _ = len(xs)",
        "for _, &e := range xs { borrow(&e) }",
        "xs = xs[1:]; xs = append(xs, zero()); _ = len(xs)",
    ];
    let helpers =
        "type Cell[U any] struct{ f U }\nfunc keep[U any](u U) {}\nfunc borrow[U any](u *U) {}\n";
    // The lines whose function is refused, for a type that is declared
    // or for a type parameter with the constraint.
    let refused = |declared: &str, constraint: &str| {
        let mut text = format!("package p\n{declared}\n{helpers}");
        let first = text.lines().count() + 1;
        for (at, body) in uses.iter().enumerate() {
            let (generic, of) = match constraint {
                "" => (String::new(), ""),
                constraint => (format!("[X {constraint}]"), "[X]"),
            };
            let zero = format!("zero := func() X {{ return zero{of}() }}; _ = zero");
            text += &format!(
                "func f{at}{generic}(v X, xs []X, p *X, m map[string]X, ch chan X) X {{ {zero}; \
                 {body}; return zero() }}\n"
            );
        }
        let made = match constraint {
            "" => "func zero() X { panic(0) }\n".to_owned(),
            constraint => format!("func zero[X {constraint}]() X {{ panic(0) }}\n"),
        };
        text += &made;
        let checked = messages("a.gopp", &text);
        let mut lines: Vec<usize> = checked
            .iter()
            .filter(|message| !message.starts_with("    "))
            .map(|message| {
                let line: usize = message.split(':').nth(1).unwrap().parse().unwrap();
                assert!(line >= first && line < first + uses.len(), "{message}");
                line - first
            })
            .collect();
        lines.dedup();
        lines
    };
    let file = "type X struct{ fd int }\nfunc (x *X) drop() {}";
    let copies: Vec<usize> = (0..=12).collect();
    assert_eq!(refused(file, ""), copies);
    assert_eq!(refused("", "any"), copies);
    assert_eq!(refused("", "interface{ String() string }"), copies);
    let none: [usize; 0] = [];
    assert_eq!(refused("type X struct{ name string }", ""), none);
    assert_eq!(refused("", "Clone"), none);
    assert_eq!(refused("", "~string | ~[]int"), none);
    assert_eq!(refused("type X int", ""), none);
    assert_eq!(refused("", "Copy"), none);
    assert_eq!(refused("", "~int | ~uint8"), none);
}

// rule: CLO-5, CLO-5a
#[test]
fn a_function_literal_says_which_variables_it_uses_and_how() {
    let text = "package p

type Greeter struct{ Name string }

func (g *Greeter) Hello() string { return \"hi \" + g.Name }
func (g Greeter) Bye() string    { return \"bye \" + g.Name }

type Namer interface{ Bye() string }

func sortFunc(xs *[]int, less func(a, b int) int) {}
func show(args ...any)                           {}

func counter() (func() int, func() int) {
	n := 0
	inc := func() int { n++; return n }
	get := func() int { return n }
	return inc, get
}

func loops(xs []int) {
	var fs []func() int
	for i := 0; i < 3; i++ {
		fs = append(fs, func() int { return i })
	}
	total := 0
	sortFunc(&xs, func(a, b int) int {
		total++
		return a - b
	})
	for _, &x := range xs {
		_ = func() { x++ }
	}
	_ = func() {
		for _, &x := range xs {
			x++
		}
		total, fs := 1, fs
		_, _ = total, fs
	}
}

func methods(g Greeter, p *Greeter, n Namer) {
	h := g.Hello
	g.Name = \"bob\"
	show(h(), g.Bye(), (g.Hello)())
	a, b, c, d := g.Bye, p.Hello, p.Bye, n.Bye
	e := Greeter{}.Bye
	f := Greeter.Bye
	show(a, b, c, d, e, f)
	_ = func() { show(g.Hello, g.Bye, p.Hello) }
}

func recursion() {
	var fib Option[func(int) int]
	fib = func(n int) int {
		f, ok := fib.Get()
		if n < 2 || !ok {
			return n
		}
		return f(n-1) + f(n-2)
	}
	if fib != nil {
		show(fib(10))
	}
}

func uses(xs []int, g Greeter, p, q *Greeter, m map[string]int) (err error) {
	const limit = 3
	type Count int
	total := 0
	defer func() {
		if err != nil {
			err = nil
		}
	}()
	x := 1
	defer show(x)
	_ = func(a int) int {
		total += a
		local := a
		_ = func() {
			xs[0] = local
			g.Name = \"x\"
			p.Name = \"y\"
			p = g
			q = &g
			delete(m, \"a\")
			_ = &total
			_ = len(xs) + x + limit + int(Count(1))
		}
		return a
	}
	return nil
}
";
    let names = ThreadedRodeo::new();
    let checked = check_texts("p", &[("a.gopp", text)], &Loader::default(), &names);
    assert_eq!(checked.messages, [] as [&str; 0]);
    let expected = [
        "15:9: n write",
        "16:9: n read",
        "23:19: i read",
        "26:16: total write",
        "31:7: x write",
        "33:6: xs address, fs read",
        "43:7 g.Hello: address",
        "46:16 g.Bye: value",
        "46:23 p.Hello: value",
        "46:32 p.Bye: value",
        "46:39 n.Bye: value",
        "47:7 Greeter{}.Bye: value",
        "50:6: g address, p read",
        "50:20 g.Hello: address",
        "50:29 g.Bye: value",
        "50:36 p.Hello: value",
        "55:8: fib read",
        "71:8: err write",
        "78:6: xs write, g address, p read, q write, m write, total address, x read",
        "81:7: xs write, g address, p read, q write, m write, total address, x read, local read",
    ];
    assert_eq!(closures(&checked, &names), expected);
}

// rule: CLO-5a
#[test]
fn a_func_is_never_nil_and_a_method_value_holds_a_receiver_that_is_copied() {
    let text = "package p

type File struct{ fd int }

func (f *File) drop()  {}
func (f File) Fd() int { return f.fd }
func (f *File) Close() {}
func open() File       { return File{} }

type Server struct{ OnStop Option[func()] }

func funcs(s *Server, f, g func(), p *func()) {
	var b Option[func()]
	_ = b == nil
	b = f
	b = nil
	if h := s.OnStop; h != nil {
		h()
	}
	if b != nil {
		b()
	}
	p()

	_ = f == nil
	_ = nil != f
	_ = f == g
	f = nil
	s()
	b()
	s.OnStop()
	if s.OnStop != nil {
		s.OnStop()
	}
}

func values(f File) {
	a := f.Close
	b := open().Fd
	c := move(f).Fd
	d := f.Fd
	_, _, _, _ = a, b, c, d
}
";
    let expected = [
        "a.gopp:25:6: invalid operation: f == nil (a func() is never nil: use Option[func()])",
        "a.gopp:26:13: invalid operation: nil != f (a func() is never nil: use Option[func()])",
        "a.gopp:27:6: invalid operation: f == g (func values cannot be compared)",
        "a.gopp:28:6: cannot use nil as func() value in assignment",
        "    a func() is never nil: use Option[func()]",
        "a.gopp:29:2: invalid operation: cannot call s (variable of struct type Server): Server is \
         not a function",
        "a.gopp:30:2: invalid operation: cannot call b (variable of type Option[func()]): \
         Option[func()] is not a function",
        "a.gopp:31:2: invalid operation: cannot call s.OnStop (variable of type Option[func()]): \
         Option[func()] is not a function",
        "a.gopp:33:3: invalid operation: cannot call s.OnStop (variable of type Option[func()]): \
         Option[func()] is not a function",
        "a.gopp:41:7: f is used after it was moved: it has no value until it is assigned again",
        "    a.gopp:40:7: f is moved here",
        "a.gopp:41:7: cannot copy f (variable of struct type File): File has a drop method and no \
         Clone() File",
        "    use move(f) to give the value, or &f to lend it",
    ];
    assert_eq!(messages("a.gopp", text), expected);
}

#[test]
fn the_steps_of_captures_grow_with_the_input_not_faster() {
    let steps = |text: String| {
        let loader = Loader::default();
        let checked = check_texts("p", &[("a.gopp", &text)], &loader, &ThreadedRodeo::new());
        assert_eq!(checked.messages, [] as [&str; 0]);
        checked.steps
    };
    // Many literals that use one variable many times, and many variables
    // that one literal uses.
    let siblings = |count: usize| {
        let literals: String = (0..count)
            .map(|i| format!("\t_ = func() {{ n += {i}; n++; _ = &n }}\n"))
            .collect();
        format!("package p\nfunc f(n int) {{\n{literals}}}\n")
    };
    let wide = |count: usize| {
        let declared: String = (0..count).map(|i| format!("\tv{i} := {i}\n")).collect();
        let used: String = (0..count).map(|i| format!("\t\tv{i}++\n")).collect();
        format!("package p\nfunc f() {{\n{declared}\t_ = func() {{\n{used}\t}}\n}}\n")
    };
    // A literal in a literal: each uses a variable of the one around it,
    // and the innermost uses the outermost variable many times.
    let nested = |depth: usize| {
        let open: String = (0..depth)
            .map(|i| format!("v{} := v{i}\n_ = func() {{\n_ = v{}\n", i + 1, i + 1))
            .collect();
        let uses = "v0++\n".repeat(depth);
        format!(
            "package p\nfunc f(v0 int) {{\n{open}{uses}{}}}\n",
            "}\n".repeat(depth)
        )
    };
    let sizes = [250, 500, 1_000, 2_000];
    linear::assert_near_linear(&sizes, |n| steps(siblings(n)));
    linear::assert_near_linear(&sizes, |n| steps(wide(n)));
    // The parser takes no deeper nesting.
    linear::assert_near_linear(&[50, 100, 200, 400], |n| steps(nested(n)));
}

// rule: MAC-8, MAC-12, MAC-14
#[test]
fn a_built_in_attribute_is_written_where_it_goes_with_what_it_takes() {
    // For each attribute: a use that is taken, and uses that are refused
    // with what is said of each, at the attribute.
    let header = "package p\n\ntype T struct{ n int }\n\nconst Size = 64\n\nvar limit = 3\n\n";
    let rows: [(&str, &[(&str, &str)]); 19] = [
        (
            "type U struct {\n\tID int @Required\n}",
            &[
                (
                    "@Required\nvar x int",
                    "cannot use @Required on a variable: it is written on a field of a struct",
                ),
                (
                    "type U struct {\n\tID int @Required(1)\n}",
                    "@Required takes no arguments",
                ),
            ],
        ),
        (
            "@Extends(T)\ntype U struct{}",
            &[
                (
                    "@Extends(T)\nfunc f() {}",
                    "cannot use @Extends on a function: it is written on a struct type or an interface type",
                ),
                (
                    "@Extends(T, T)\ntype U struct{}",
                    "@Extends takes one parent",
                ),
                ("@Extends\ntype U struct{}", "@Extends takes one parent"),
            ],
        ),
        (
            "@Virtual\nfunc (t *T) M() {}",
            &[
                (
                    "@Virtual\nfunc (t T) M() {}",
                    "cannot use @Virtual on a method: the method needs a pointer receiver",
                ),
                (
                    "@Virtual\nfunc f() {}",
                    "cannot use @Virtual on a function: it is written on a method",
                ),
            ],
        ),
        (
            "@Abstract\nfunc (t *T) M()\n\n@Abstract\n@Extends(T)\ntype U struct{}",
            &[
                (
                    "@Abstract\nvar x int",
                    "cannot use @Abstract on a variable: it is written on a method or a type",
                ),
                (
                    "@Abstract\nfunc (t T) M()",
                    "cannot use @Abstract on a method: the method needs a pointer receiver",
                ),
            ],
        ),
        (
            "type V struct{}\n\n@Virtual\nfunc (v *V) M() {}\n\n@Extends(V)\ntype W struct{}\n\n@Override\nfunc (w *W) M() {}",
            &[(
                "@Override\nfunc (t *T) M() {}",
                "T.M is marked @Override, and no ancestor of T has a method M",
            )],
        ),
        (
            "@Operator\nfunc (t T) Add(u T) T { return t }\n\n@Operator\nfunc (t *T) AddAssign(u T) {}",
            &[
                (
                    "@Operator\nfunc (t T) Plus(u T) T { return t }",
                    "cannot use @Operator on a method: Plus is no operator method",
                ),
                (
                    "@Operator\nfunc Add(t, u T) T { return t }",
                    "cannot use @Operator on a function: it is written on a method",
                ),
            ],
        ),
        (
            "@Operator\n@Right\nfunc (t T) Mul(n int) T { return t }",
            &[
                (
                    "@Right\nfunc (t T) Mul(n int) T { return t }",
                    "cannot use @Right on a method: the method is no @Operator method",
                ),
                (
                    "@Operator\n@Right\nfunc (t T) Neg() T { return t }",
                    "cannot use @Right on a method: Neg takes no operand on its left",
                ),
            ],
        ),
        (
            "@Convert\nfunc (t T) Int() int { return t.n }",
            &[
                (
                    "@Convert\nfunc (t *T) Int() int { return t.n }",
                    "cannot use @Convert on a method: the method needs a value receiver, no parameters and one result",
                ),
                (
                    "@Convert\nfunc (t T) Int(n int) int { return n }",
                    "cannot use @Convert on a method: the method needs a value receiver, no parameters and one result",
                ),
                (
                    "@Convert\nfunc Int(t T) int { return t.n }",
                    "cannot use @Convert on a function: it is written on a method",
                ),
            ],
        ),
        (
            "@Const\nfunc square(n int) int { return n * n }\n\n@Const\nfunc (t T) N() int { return t.n }",
            &[
                (
                    "@Const\nconst c = 1",
                    "cannot use @Const on a constant: it is written on a function or a method",
                ),
                (
                    "@Const\n@Virtual\nfunc (t *T) M() {}",
                    "cannot use @Const on a method: the function is marked @Virtual",
                ),
                (
                    "@Const\n@Extern(\"C\")\nfunc f()",
                    "cannot use @Const on a function: the function is marked @Extern",
                ),
            ],
        ),
        (
            "@Pinned\ntype Lock struct{ state int }",
            &[
                (
                    "@Pinned\nfunc f() {}",
                    "cannot use @Pinned on a function: it is written on a type",
                ),
                (
                    "type U struct {\n\tn int @Pinned\n}",
                    "cannot use @Pinned on a field: it is written on a type",
                ),
                (
                    "@Pinned\ntype A = T",
                    "cannot use @Pinned on an alias: it is written on a type",
                ),
            ],
        ),
        (
            "@Extern(\"C\")\nfunc puts(s *byte) int",
            &[
                (
                    "@Extern(\"C\")\nfunc f() {}",
                    "cannot use @Extern on a function: a function that is defined in C has no body",
                ),
                (
                    "@Extern(\"C\")\nfunc f[P any](p *P)",
                    "cannot use @Extern on a function: a function that is defined in C has no type parameters",
                ),
                ("@Extern(\"D\")\nfunc f()", "@Extern takes the string \"C\""),
                ("@Extern\nfunc f()", "@Extern takes the string \"C\""),
                (
                    "@Extern(\"C\")\nfunc (t *T) M()",
                    "cannot use @Extern on a method: it is written on a function",
                ),
            ],
        ),
        (
            "@Export(\"C\")\nfunc add(a, b int32) int32 { return a + b }",
            &[
                (
                    "@Export(\"C\")\nfunc f()",
                    "cannot use @Export on a function: a function that C calls has a body",
                ),
                (
                    "@Export(\"C\")\nfunc f[P any](p *P) {}",
                    "cannot use @Export on a function: a function that C calls has no type parameters",
                ),
                (
                    "@Export(\"C\", 1)\nfunc f() {}",
                    "@Export takes the string \"C\"",
                ),
                (
                    "@Export(\"C\")\nvar x int",
                    "cannot use @Export on a variable: it is written on a function",
                ),
            ],
        ),
        (
            "@Unwind\nfunc callback(n int32) {}",
            &[(
                "@Unwind\ntype U int",
                "cannot use @Unwind on a type: it is written on a function",
            )],
        ),
        (
            "@Packed\ntype U struct{ a byte; b int }",
            &[
                (
                    "@Packed\ntype U int",
                    "cannot use @Packed on a type: it is written on a struct type",
                ),
                ("@Packed(1)\ntype U struct{}", "@Packed takes no arguments"),
            ],
        ),
        (
            "@Align(Size)\ntype U struct{ a byte }\n\n@Align(8)\ntype V int",
            &[
                (
                    "@Align(3)\ntype U int",
                    "@Align takes a constant power of two from 1 to 65536",
                ),
                (
                    "@Align(limit)\ntype U int",
                    "@Align takes a constant power of two from 1 to 65536",
                ),
                (
                    "@Align(131072)\ntype U int",
                    "@Align takes a constant power of two from 1 to 65536",
                ),
                (
                    "@Align\ntype U int",
                    "@Align takes a constant power of two from 1 to 65536",
                ),
                (
                    "@Align(8)\nvar x int",
                    "cannot use @Align on a variable: it is written on a type",
                ),
            ],
        ),
        (
            "@Inline\nfunc f() {}\n\n@NoInline\nfunc (t T) M() {}",
            &[
                (
                    "@Inline\n@NoInline\nfunc f() {}",
                    "cannot use @NoInline on a function: the function is marked @Inline too",
                ),
                (
                    "@Inline\nfunc f()",
                    "cannot use @Inline on a function: the function has no body",
                ),
                ("@Inline(1)\nfunc f() {}", "@Inline takes no arguments"),
                (
                    "@NoInline\ntype U int",
                    "cannot use @NoInline on a type: it is written on a function or a method",
                ),
            ],
        ),
        (
            "@NoBounds\nfunc first(xs []int) int { return xs[0] }",
            &[(
                "@NoBounds\nvar x int",
                "cannot use @NoBounds on a variable: it is written on a function or a method",
            )],
        ),
        (
            "@If(Size == 64)\nfunc f() {}\n\n@If(true)\ntype U int\n\n@If(false)\nvar x = 1\n\n@If(Size > 1)\nconst c = 1\n\n@If(true)\nfunc (t T) M() {}",
            &[
                ("@If(limit == 3)\nfunc f() {}", "@If takes a constant bool"),
                ("@If(1)\nfunc f() {}", "@If takes a constant bool"),
                ("@If\nfunc f() {}", "@If takes a constant bool"),
                (
                    "type U struct {\n\tn int @If(true)\n}",
                    "cannot use @If on a field: it is written on a declaration",
                ),
            ],
        ),
        // What is not built in is a macro's: its arguments are known at
        // compile time.
        (
            "@Derive(JSON, Equal)\n@Retry(Size, \"x\", -1)\ntype U struct {\n\tName string @JSON(\"name\") @Max(Size)\n}",
            &[
                (
                    "@Retry(limit)\nfunc f() {}",
                    "cannot use limit in @Retry: the arguments of an attribute are known at compile time, and limit is a variable",
                ),
                (
                    "type U struct {\n\tn int @Max(limit + 1)\n}",
                    "cannot use limit in @Max: the arguments of an attribute are known at compile time, and limit is a variable",
                ),
            ],
        ),
    ];
    let line = header.lines().count() + 1;
    for (taken, refused) in rows {
        let text = format!("{header}{taken}\n");
        assert_eq!(messages("a.gopp", &text), [] as [&str; 0], "{taken}");
        for (source, said) in refused {
            let text = format!("{header}{source}\n");
            let found = messages("a.gopp", &text);
            assert_eq!(found.len(), 1, "{source}: {found:?}");
            let (place, message) = found[0].split_once(": ").unwrap();
            assert_eq!(message, *said, "{source}");
            // At the attribute, which is written first or on a field.
            let at: usize = place.split(':').nth(1).unwrap().parse().unwrap();
            assert!(at == line || at == line + 1, "{source}: {place}");
        }
    }
}

// rule: PAR-5
#[test]
fn a_receiver_that_is_not_copied_is_given_unless_the_method_only_reads_it() {
    let text = "package p

type File struct{ fd int }

func (f *File) drop() {}

@Operator
func (f File) Eq(g File) bool { return f.fd == g.fd }

@Convert
func (f File) Int() int { return f.fd }

func (f File) Fd() int   { return f.fd }
func (f *File) Sync()    {}
func open() File         { return File{} }
func same(a, b File) bool { return false }

func use(f, g File, files []File) {
	_ = f.Eq(g)
	_ = files[0].Eq(files[1])
	_ = f.Int()
	f.Sync()
	_ = open().Fd()
	_ = move(g).Fd()

	_ = f.Fd()
	_ = files[0].Fd()
	_ = f.Eq(func() File { return files[0] }())
	_ = same(f, f)
}
";
    let expected = [
        "a.gopp:26:6: cannot copy f (variable of struct type File): File has a drop method and no \
         Clone() File",
        "    use move(f) to give the value, or &f to lend it",
        "a.gopp:27:6: cannot copy files[0] (variable of struct type File) out of its place: File \
         has a drop method and no Clone() File",
        "    use &files[0], mem.Take(&files[0]) or mem.Replace(&files[0], v)",
        "a.gopp:28:32: cannot copy files[0] (variable of struct type File) out of its place: File \
         has a drop method and no Clone() File",
        "    use &files[0], mem.Take(&files[0]) or mem.Replace(&files[0], v)",
        "a.gopp:29:11: cannot copy f (variable of struct type File): File has a drop method and no \
         Clone() File",
        "    use move(f) to give the value, or &f to lend it",
        "a.gopp:29:14: cannot copy f (variable of struct type File): File has a drop method and no \
         Clone() File",
        "    use move(f) to give the value, or &f to lend it",
    ];
    assert_eq!(messages("a.gopp", text), expected);
}

// rule: OWN-S2, PAR-5
#[test]
fn a_part_of_a_slice_is_a_copy_of_its_elements_unless_it_is_a_view() {
    let text = "package p

type File struct{ fd int }

func (f *File) drop() {}

type Pool struct{ files []File }

func open() []File          { return nil }
func checkAll(files []File) {}
func keep(f File)           {}
func view(files *[]File)    {}

func slices(files []File, p *Pool, arr [4]File, docs []string) []File {
	checkAll(files)
	checkAll(files[1:])
	checkAll(p.files)
	view(&files[1:])
	view(&arr[:2])
	files = files[1:]
	p.files = p.files[:2]
	_ = len(files[1:])
	for _, f := range files[1:] {
		_ = f.fd
	}
	tail := open()[1:]
	names := docs[1:]
	_, _ = tail, names

	a := files[1:]
	b := p.files[1:]
	c := arr[:]
	other := append(files[:1], open()...)
	_, _, _, _ = a, b, c, other
	return files[1:]
}

func generic[T any](xs []T, ys []T) []T {
	a := xs[1:]
	_ = a
	xs = xs[1:]
	return ys[:1]
}
";
    let checked = check_texts(
        "p",
        &[("a.gopp", text)],
        &Loader::default(),
        &ThreadedRodeo::new(),
    );
    let expected = [
        "a.gopp:30:7: cannot copy the elements of files[1:]: File has a drop method and no Clone() \
         File",
        "    use &files[1:] to name the same elements",
        "a.gopp:31:7: cannot copy the elements of p.files[1:]: File has a drop method and no \
         Clone() File",
        "    use &p.files[1:] to name the same elements",
        "a.gopp:32:7: cannot copy the elements of arr[:]: File has a drop method and no Clone() \
         File",
        "    use &arr[:] to name the same elements",
        "a.gopp:33:18: cannot copy the elements of files[:1]: File has a drop method and no \
         Clone() File",
        "    use &files[:1] to name the same elements",
        "a.gopp:39:7: cannot copy the elements of xs[1:]: T is not constrained by Clone",
        "    use &xs[1:] to name the same elements",
    ];
    assert_eq!(checked.messages, expected);
    // A slice that is passed is copied if the function does more than
    // read it, which is known with the function.
    let expected = [
        "15:11 files: copy",
        "16:11 files: copy",
        "17:11 p.files: copy",
        "20:10 files: over",
        "21:12 p.files: over",
        "35:9 files: move",
        "41:7 xs: over",
        "42:9 ys: move",
    ];
    assert_eq!(transfers(&checked), expected);
}

// rule: TYP-15, OWN-R9
#[test]
fn a_type_without_a_zero_value_is_never_left_unset() {
    let text = "package p

type User struct {
	ID    int    @Required
	Email string
}

type Team struct {
	Owner User
	Name  string
}

type Node struct {
	Next Option[*Node]
	Up   *Node
}

type Box[T any] struct{ v T }

type Handler interface{ Serve() }

type Bad struct {
	Nick Option[string] @Required
	Next *Node          @Required
	Run  func()         @Required
	H    Handler        @Required
	Err  error          @Required
	Tags []string       @Required
	U    User           @Required
}

var u User
var ok int
var boxed Box[User]
var plain Box[int]
var arr [0]User

func good(m map[string]User, ch chan User, users []User) {
	a := User{ID: 1}
	b := User{1, \"x\"}
	t := Team{Owner: a}
	xs := make([]User, 0, 10)
	ys := []User{a, b}
	zs := [2]User{a, b}
	var o Option[User]
	var err error
	v, found := m[\"a\"]
	w, open := <-ch
	m[\"b\"] = a
	delete(m, \"a\")
	clear(m)
	users = users[:0]
	for x := range ch {
		_ = x
	}
	if found && open {
		_, _ = v, w
	}
	_, _, _, _, _, _ = t, xs, ys, zs, o, err
}

func bad(m map[string]User, ch chan User, users []User, n int) (named User) {
	var a User
	var t Team
	var arr [3]User
	var f func()
	var h Handler
	var p *Node
	b := User{Email: \"x\"}
	c := User{}
	d := Team{Name: \"x\"}
	e := Node{}
	xs := make([]User, n)
	ys := []User{2: b}
	zs := [3]User{b}
	v := m[\"a\"]
	w := <-ch
	_ = m[\"a\"].Email
	clear(users)
	_, _, _, _, _, _, _, _, _, _, _, _, _, _ = a, t, arr, f, h, p, c, d, e, xs, ys, zs, v, w
	return b
}

func generic[T any, N ~int | ~string](n int) {
	var zero T
	var num N
	xs := make([]T, n)
	ys := make([]N, n)
	_, _, _, _ = zero, num, xs, ys
}
";
    let expected = [
        "a.gopp:23:22: cannot use @Required on a field of type Option[string]: an option may be \
         nil",
        "    use string @Required, or Option[string]",
        "a.gopp:24:22: cannot use @Required on a field of type *Node: a *Node is never nil, so the \
         field is set in every literal already",
        "a.gopp:25:22: cannot use @Required on a field of type func(): a func() is never nil, so \
         the field is set in every literal already",
        "a.gopp:26:22: cannot use @Required on a field of type Handler: a Handler is never nil, so \
         the field is set in every literal already",
        "a.gopp:32:5: cannot declare u without a value: User has no zero value (the field ID of \
         User is @Required)",
        "    give u a value, or declare it as Option[User]",
        "a.gopp:34:5: cannot declare boxed without a value: Box[User] has no zero value (the field \
         ID of User is @Required)",
        "    give boxed a value, or declare it as Option[Box[User]]",
        "a.gopp:49:2: this write to m is lost: m is the function's own copy, and nothing reads it \
         afterwards",
        "    declare m *map[string]User to change the value of the caller, or return m",
        "a.gopp:62:65: cannot name the result named: User has no zero value (the field ID of User \
         is @Required)",
        "    declare the result without a name",
        "a.gopp:63:6: cannot declare a without a value: User has no zero value (the field ID of \
         User is @Required)",
        "    give a a value, or declare it as Option[User]",
        "a.gopp:64:6: cannot declare t without a value: Team has no zero value (the field ID of \
         User is @Required)",
        "    give t a value, or declare it as Option[Team]",
        "a.gopp:65:6: cannot declare arr without a value: [3]User has no zero value (the field ID \
         of User is @Required)",
        "    give arr a value, or declare it as Option[[3]User]",
        "a.gopp:66:6: cannot declare f without a value: func() has no zero value (a func() is \
         never nil)",
        "    give f a value, or declare it as Option[func()]",
        "a.gopp:67:6: cannot declare h without a value: Handler has no zero value (a Handler is \
         never nil)",
        "    give h a value, or declare it as Option[Handler]",
        "a.gopp:68:6: cannot declare p without a value: *Node has no zero value (a *Node is never \
         nil)",
        "    give p a value, or declare it as Option[*Node]",
        "a.gopp:69:7: missing ID (@Required) in struct literal of type User",
        "a.gopp:70:7: missing ID (@Required) in struct literal of type User",
        "a.gopp:71:7: missing Owner (User has no zero value) in struct literal of type Team",
        "a.gopp:72:7: missing Up (*Node has no zero value) in struct literal of type Node",
        "a.gopp:73:8: cannot make a []User with a length: User has no zero value (the field ID of \
         User is @Required)",
        "    use make([]User, 0, n) and append the elements",
        "a.gopp:74:8: cannot leave out elements of the literal: User has no zero value (the field \
         ID of User is @Required)",
        "    write every element",
        "a.gopp:75:8: cannot leave out elements of the literal: User has no zero value (the field \
         ID of User is @Required)",
        "    write every element",
        "a.gopp:76:7: cannot read m[\"a\"] alone: the entry may be missing, and User has no zero \
         value (the field ID of User is @Required)",
        "    use v, ok := m[\"a\"]",
        "a.gopp:77:7: cannot read <-ch alone: the channel may be closed, and User has no zero \
         value (the field ID of User is @Required)",
        "    use v, ok := <-ch",
        "a.gopp:78:6: cannot read m[\"a\"] alone: the entry may be missing, and User has no zero \
         value (the field ID of User is @Required)",
        "    use v, ok := m[\"a\"]",
        "a.gopp:79:2: this write to users is lost: users is the function's own copy, and nothing \
         reads it afterwards",
        "    declare users *[]User to change the value of the caller, or return users",
        "a.gopp:79:8: cannot clear users: User has no zero value (the field ID of User is \
         @Required)",
        "    use users = users[:0], which drops the elements",
        "a.gopp:85:6: cannot declare zero without a value: T may be a type with no zero value",
        "    give zero a value, or declare it as Option[T]",
        "a.gopp:87:8: cannot make a []T with a length: T may be a type with no zero value",
        "    use make([]T, 0, n) and append the elements",
    ];
    assert_eq!(messages("a.gopp", text), expected);
}

// rule: TYP-15
#[test]
fn a_required_field_that_could_be_nil_is_never_nil() {
    let names = ThreadedRodeo::new();
    let mem = "package mem\nfunc Take[T any](p *T) T\nfunc Replace[T any](p *T, v T) T\n";
    let loader = with_packages(&names, &[("mem", mem)]);
    let text = "package p

import \"mem\"

type Fail struct{}

func (f Fail) Error() string { return \"\" }

type Job struct {
	ID    int            @Required
	Done  chan bool      @Required
	Seen  map[string]int @Required
	Err   error          @Required
	Extra any            @Required
	Tags  []string       @Required
	Later chan bool
}

func pick() chan bool { return nil }

func good(j *Job, other Job, ch chan bool) Job {
	j.Done = make(chan bool, 1)
	j.Seen = map[string]int{}
	j.Err = Fail{}
	j.Extra = 1
	j.Done = other.Done
	if ch != nil {
		j.Done = ch
	}
	made := make(chan bool)
	j.Done = made
	j.Tags = nil
	j.Later = nil
	j.Later = pick()
	old := mem.Replace(&j.Done, make(chan bool))
	later := mem.Take(&j.Later)
	_, _ = old, later
	return Job{ID: 1, Done: other.Done, Seen: map[string]int{}, Err: Fail{}, Extra: \"x\", Tags: nil}
}

func bad(j *Job, other Job, ch chan bool) {
	j.Done = ch
	j.Done = nil
	j.Done = pick()
	j.Done = other.Later
	j.Err = nil
	p := &j.Done
	a := mem.Take(&j.Seen)
	b := mem.Replace(&j.Done, pick())
	c := Job{ID: 1, Done: nil, Seen: map[string]int{}, Err: Fail{}, Extra: 1, Tags: nil}
	_, _, _, _ = p, a, b, c
}
";
    let checked = check_texts("p", &[("a.gopp", text)], &loader, &names);
    let expected = [
        "a.gopp:42:11: cannot store ch in j.Done: the field is @Required, and the value may be nil",
        "    put it in a variable and store it where that is not nil",
        "a.gopp:43:11: cannot store nil in j.Done: the field is @Required",
        "    store a value, or declare the field as Option[chan bool]",
        "a.gopp:44:11: cannot store pick() in j.Done: the field is @Required, and the value may be \
         nil",
        "    put it in a variable and store it where that is not nil",
        "a.gopp:45:11: cannot store other.Later in j.Done: the field is @Required, and the value \
         may be nil",
        "    put it in a variable and store it where that is not nil",
        "a.gopp:46:10: cannot store nil in j.Err: the field is @Required",
        "    store a value, or declare the field as Option[error]",
        "a.gopp:47:7: cannot take the address &j.Done: the field is @Required, so it is never nil",
        "    use mem.Replace to give it another value",
        "a.gopp:48:16: cannot take j.Seen: the field is @Required, so it is never nil",
        "    use mem.Replace(&j.Seen, v)",
        "a.gopp:49:28: cannot store pick() in j.Done: the field is @Required, and the value may be \
         nil",
        "    put it in a variable and store it where that is not nil",
        "a.gopp:50:24: cannot store nil in Done: the field is @Required",
        "    store a value, or declare the field as Option[chan bool]",
    ];
    assert_eq!(checked.messages, expected);
}

#[test]
fn the_steps_of_zero_values_grow_with_the_types_not_faster() {
    let steps = |text: String| {
        let loader = Loader::default();
        let checked = check_texts("p", &[("a.gopp", &text)], &loader, &ThreadedRodeo::new());
        assert_eq!(checked.messages, [] as [&str; 0]);
        checked.steps + loader.types.get().unwrap().steps()
    };
    // Each type holds the one before it, and each is asked: from the
    // first to the last, and from the last to the first.
    let chain = |count: usize, down: bool| {
        let types: String = (1..count)
            .map(|i| format!("type T{i} struct{{ a T{}; n int }}\n", i - 1))
            .collect();
        let order: Vec<usize> = match down {
            true => (0..count).rev().collect(),
            false => (0..count).collect(),
        };
        let asked: String = order
            .iter()
            .map(|i| format!("\t_ = make([]T{i}, 1)\n"))
            .collect();
        format!("package p\ntype T0 struct{{ n int }}\n{types}func f() {{\n{asked}}}\n")
    };
    // Each type holds the one before it twice.
    let diamonds = |count: usize| {
        let types: String = (1..count)
            .map(|i| format!("type T{i} struct{{ a, b T{}; c [2]T{} }}\n", i - 1, i - 1))
            .collect();
        format!(
            "package p\ntype T0 struct{{ n int }}\n{types}var x T{}\n",
            count - 1
        )
    };
    // One struct with many fields, asked many times.
    let wide = |count: usize| {
        let fields: String = (0..count).map(|i| format!("\tf{i} int\n")).collect();
        let asked = "\t_ = make([]T, 1)\n".repeat(count);
        format!("package p\ntype T struct {{\n{fields}}}\nfunc f() {{\n{asked}}}\n")
    };
    let sizes = [250, 500, 1_000, 2_000];
    linear::assert_near_linear(&sizes, |n| steps(chain(n, false)));
    linear::assert_near_linear(&sizes, |n| steps(chain(n, true)));
    linear::assert_near_linear(&sizes, |n| steps(diamonds(n)));
    linear::assert_near_linear(&sizes, |n| steps(wide(n)));
}

// rule: OWN-P5, OWN-P6, OWN-P8
#[test]
fn a_pointer_is_used_like_its_value_and_a_shared_value_is_only_read() {
    let names = ThreadedRodeo::new();
    let mem = "package mem\nfunc Take[T any](p *T) T\nfunc Replace[T any](p *T, v T) T\n";
    let loader = with_packages(&names, &[("mem", mem)]);
    let text = "package p

import \"mem\"

type Node struct {
	Name string
	Kids []int
	Tags map[string]int
	Next Option[*Node]
	N    int
}

func (n Node) Label() string { return n.Name }
func (n *Node) Rename(s string) { n.Name = s }

func open() Unique[Node]   { return NewUnique(Node{}) }
func share() Shared[Node]  { return NewShared(Node{}) }
func visit(n *Node)        {}
func maybe(n Option[*Node]) {}
func addOne(xs *[]int)     { xs[0]++ }

func pointers(p *Node, n *int, xs *[]int, m *map[string]int, q *Node) int {
	_ = p.Name
	_ = p.Label()
	p.Rename(\"x\")
	_ = xs[0]
	_ = xs[1:]
	_ = len(xs) + cap(xs) + len(m)
	for _, x := range xs {
		_ = x
	}
	delete(m, \"a\")
	clear(m)
	_ = n + 1
	var v int = n
	n = 9
	n++
	n += 2
	xs = append(xs, 4)
	xs = nil
	r := p
	p = q
	_ = p == q
	v = *n
	_, _ = r, v
	return n
}

func uniques(u Unique[Node], o Option[Unique[Node]]) {
	_ = u.Name
	u.Name = \"x\"
	u.Kids[0] = 1
	u.Rename(\"y\")
	_ = u.Label()
	visit(u)
	maybe(u)
	maybe(o)
	var p *Node = u
	var q Option[*Node] = u
	_, _ = p, q
	visit(open())
	var bad *Node = open()
	_ = bad
}

func shareds(s Shared[Node], w Weak[Node], o Option[Shared[Node]]) {
	_ = s.Name
	_ = s.Kids[0]
	_ = len(s.Kids)
	_ = s.Label()
	for _, k := range s.Kids {
		_ = k
	}
	kids := s.Kids
	visit(s)
	maybe(o)
	_ = kids

	s.Name = \"x\"
	s.Kids[0] = 1
	s.N++
	s.N += 1
	s.Kids = append(s.Kids, 1)
	delete(s.Tags, \"a\")
	clear(s.Kids)
	_ = mem.Take(&s.Name)
	_ = mem.Replace(&s.N, 2)
	p := &s.Name
	p = \"x\"
	addOne(&s.Kids)

	_ = w.Name
	w.Rename(\"x\")
	if t := w.Lock(); t != nil {
		_ = t.Name
	}
}
";
    let checked = check_texts("p", &[("a.gopp", text)], &loader, &names);
    let expected = [
        "a.gopp:61:8: cannot use open() as *Node value: the Unique[Node] is a temporary that ends \
         with the statement",
        "    store it in a variable first",
        "a.gopp:62:18: cannot use open() as *Node value: the Unique[Node] is a temporary that ends \
         with the statement",
        "    store it in a variable first",
        "a.gopp:79:2: cannot assign to s.Name: what a Shared holds is only read through it",
        "    put what changes in a Mutex or an Atomic, or own the value with a Unique",
        "a.gopp:80:2: cannot assign to s.Kids[0]: what a Shared holds is only read through it",
        "    put what changes in a Mutex or an Atomic, or own the value with a Unique",
        "a.gopp:81:2: cannot assign to s.N: what a Shared holds is only read through it",
        "    put what changes in a Mutex or an Atomic, or own the value with a Unique",
        "a.gopp:82:2: cannot assign to s.N: what a Shared holds is only read through it",
        "    put what changes in a Mutex or an Atomic, or own the value with a Unique",
        "a.gopp:83:2: cannot assign to s.Kids: what a Shared holds is only read through it",
        "    put what changes in a Mutex or an Atomic, or own the value with a Unique",
        "a.gopp:84:9: cannot delete from s.Tags: what a Shared holds is only read through it",
        "    put what changes in a Mutex or an Atomic, or own the value with a Unique",
        "a.gopp:85:8: cannot clear s.Kids: what a Shared holds is only read through it",
        "    put what changes in a Mutex or an Atomic, or own the value with a Unique",
        "a.gopp:86:16: cannot take s.Name: what a Shared holds is only read through it",
        "    put what changes in a Mutex or an Atomic, or own the value with a Unique",
        "a.gopp:87:19: cannot take s.N: what a Shared holds is only read through it",
        "    put what changes in a Mutex or an Atomic, or own the value with a Unique",
        "a.gopp:89:2: cannot assign to what p points to: what a Shared holds is only read",
        "a.gopp:92:8: w.Name undefined (type Weak[Node] has no field or method Name)",
        "    use w.Lock(), which gives an Option of a Shared",
        "a.gopp:93:4: w.Rename undefined (type Weak[Node] has no field or method Rename)",
        "    use w.Lock(), which gives an Option of a Shared",
    ];
    assert_eq!(checked.messages, expected);
}

// rule: TYP-3, OWN-P9
#[test]
fn an_option_is_nil_or_a_value_of_a_type_that_has_no_nil() {
    let names = ThreadedRodeo::new();
    let lib = "package lib\ntype Held[T any] struct{ v Option[T] }\nfunc Wrap[T any](v T) Option[T] { return v }\n";
    let loader = with_packages(&names, &[("lib", lib)]);
    let text = "package p

import \"lib\"

type User struct{ Name string }
type Bag struct{ s []int }
type Key struct {
	Nick Option[string]
	N    int
}
type Maybe Option[int]
type Shape interface{ Area() float64 }
type Fail struct{}

func (f Fail) Error() string { return \"\" }

type Box[T any] struct{ v Option[T] }

func wrap[T any](v T) Option[T] { return v }

func good(u User, p *User, f func(), s Shape) {
	var a Option[int]
	var b Option[int] = 5
	var c Option[*User] = p
	var d Option[User] = u
	var e Option[func()] = f
	var g Option[Shape] = s
	a = nil
	a = 7
	b = a
	_ = a == nil
	_ = nil != a
	_ = a == b
	_ = a == 5
	_ = 5 != a
	_ = c == p
	_ = a.IsNil()
	v, ok := a.Get()
	w, ok2 := c.Get()
	var k map[Option[string]]int
	var keys map[Key]int
	var bag Option[Bag]
	_ = bag == nil
	var m Maybe = 3
	_ = m == nil
	var x any = a
	var y any = d
	var box Box[int]
	_ = wrap(1)
	if ok2 {
		_ = w
	}
	_, _, _, _, _, _, _, _, _, _, _, _ = v, ok, ok2, k, keys, x, y, box, d, e, g, c
}

func bad(a Option[int], t Option[string], bag, bag2 Option[Bag], u User) {
	_ = lib.Wrap(1)
	_ = lib.Wrap(u.Name == \"\")
	_ = lib.Wrap(map[string]int{})
	var held lib.Held[chan int]
	_ = held
	_ = a == \"x\"
	_ = a == t
	_ = bag == bag2
	_ = a < 5
	_ = a + 1
	var n int = a
	var s string = a
	var e error = a
	_ = u.IsNil()
	_ = a.Missing()
	var o1 Option[[]int]
	var o2 Option[map[string]int]
	var o3 Option[chan int]
	var o4 Option[any]
	var o5 Option[error]
	var o6 Option[Option[int]]
	var o7 Option[Maybe]
	var b1 Box[[]int]
	_ = wrap([]int{})
	_, _, _, _, _, _, _, _, _, _, _ = n, s, e, o1, o2, o3, o4, o5, o6, o7, b1
}

func generic[T any, N ~int | ~string](v T, n N) {
	var o Option[T] = move(v)
	var p Option[N] = n
	_ = o == nil
	_ = p == n
	_, _ = o, p
}

type Circle struct{ r float64 }

func (c Circle) Area() float64 { return c.r }

func ifaces(f Option[Fail], c Option[Circle], a Option[int]) {
	var e error = f
	var x any = c
	var s Option[Shape] = c
	_ = e == nil
	_, _, _ = e, x, s
}
";
    let checked = check_texts("p", &[("a.gopp", text)], &loader, &names);
    let expected = [
        "a.gopp:59:6: cannot use map[string]int for T: an Option of it is written, and \
         map[string]int has nil already",
        "a.gopp:60:20: cannot use chan int for T: an Option of it is written, and chan int has nil \
         already",
        "a.gopp:62:11: invalid operation: a == \"x\" (mismatched types Option[int] and untyped \
         string)",
        "a.gopp:63:11: invalid operation: a == t (mismatched types Option[int] and Option[string])",
        "a.gopp:64:6: invalid operation: bag == bag2 (operator == not defined on Option[Bag])",
        "a.gopp:65:6: invalid operation: a < 5 (operator < not defined on Option[int])",
        "a.gopp:66:6: invalid operation: a + 1 (mismatched types Option[int] and int)",
        "a.gopp:67:14: cannot use a (variable of type Option[int]) as int value in variable \
         declaration: the option may be nil: test it against nil first",
        "a.gopp:68:17: cannot use a (variable of type Option[int]) as string value in variable \
         declaration",
        "a.gopp:69:16: cannot use a (variable of type Option[int]) as error value in variable \
         declaration: Option[int] does not implement error (missing method Error)",
        "a.gopp:70:8: u.IsNil undefined (type User has no field or method IsNil)",
        "a.gopp:71:8: a.Missing undefined (type Option[int] has no field or method Missing)",
        "    a may be nil: put it in a variable and test that against nil first",
        "a.gopp:72:9: invalid type Option[[]int]: []int has nil already",
        "    use []int",
        "a.gopp:73:9: invalid type Option[map[string]int]: map[string]int has nil already",
        "    use map[string]int",
        "a.gopp:74:9: invalid type Option[chan int]: chan int has nil already",
        "    use chan int",
        "a.gopp:75:9: invalid type Option[any]: any has nil already",
        "    use any",
        "a.gopp:76:9: invalid type Option[error]: error has nil already",
        "    use error",
        "a.gopp:77:9: invalid type Option[Option[int]]: Option[int] has nil already",
        "    use Option[int]",
        "a.gopp:78:9: invalid type Option[Maybe]: Maybe has nil already",
        "    use Maybe",
        "a.gopp:79:13: cannot use []int for T: an Option of it is written, and []int has nil \
         already",
        "a.gopp:80:6: cannot use []int for T: an Option of it is written, and []int has nil \
         already",
    ];
    assert_eq!(checked.messages, expected);
}

// rule: OWN-P10
#[test]
fn an_option_is_its_value_where_it_was_tested() {
    let text = "package p

type Tree struct {
	L, R Option[Unique[Tree]]
	V    int
}

type User struct {
	ID   int @Required
	Name string
}

type Server struct{ OnStop Option[func()] }

func find(id int) (*User, error) { panic(id) }
func show(u User)                {}
func next() Option[*Tree]        { return nil }

func sum(t Option[*Tree]) int {
	if t == nil {
		return 0
	}
	return t.V + sum(t.L) + sum(t.R)
}

func good(o Option[int], s *Server, m map[string]User, ch chan User, p Option[*Tree]) int {
	if o != nil {
		return o + 1
	}
	if !o.IsNil() {
		var n int = o
		return n
	}
	if o != nil && o > 3 {
		return 1
	}
	if o == nil || o < 0 {
		return 2
	}
	x := o
	for p != nil {
		_ = p.V
		p = next()
	}
	for q := next(); q != nil; q = next() {
		_ = q.V
	}
	if f := s.OnStop; f != nil {
		f()
	}
	u, ok := m[\"a\"]
	if !ok {
		return 0
	}
	show(u)
	if w, open := <-ch; open {
		show(w)
	}
	v, err := find(1)
	if err != nil {
		return 0
	}
	_ = v.Name
	o = nil
	o = 7
	return x
}

func bad(o Option[int], s *Server, m map[string]User, p Option[*Tree]) int {
	_ = o + 1
	var n int = o
	_ = p.V
	s.OnStop()
	if o != nil {
		o = nil
		_ = o + 1
	}
	if s.OnStop != nil {
		s.OnStop()
	}
	u, ok := m[\"a\"]
	show(u)
	v, err := find(1)
	_ = v.Name
	if p == nil {
		return 0
	}
	for i := 0; i < 3; i++ {
		_ = p.V
		p = next()
	}
	if o != nil {
		_ = func() int { return o + 1 }
	}
	_, _, _ = ok, err, n
	return 0
}
";
    let names = ThreadedRodeo::new();
    let checked = check_texts("p", &[("a.gopp", text)], &Loader::default(), &names);
    let expected = [
        "a.gopp:70:6: invalid operation: o + 1 (mismatched types Option[int] and int)",
        "a.gopp:71:14: cannot use o (variable of type Option[int]) as int value in variable \
         declaration: the option may be nil: test it against nil first",
        "a.gopp:72:8: p.V undefined (type Option[*Tree] has no field or method V)",
        "    p may be nil: put it in a variable and test that against nil first",
        "a.gopp:73:2: invalid operation: cannot call s.OnStop (variable of type Option[func()]): \
         Option[func()] is not a function",
        "a.gopp:76:7: invalid operation: o + 1 (mismatched types Option[int] and int)",
        "a.gopp:79:3: invalid operation: cannot call s.OnStop (variable of type Option[func()]): \
         Option[func()] is not a function",
        "a.gopp:82:7: cannot use u here: User has no zero value, so u is set only where ok is true",
        "    return or go on where !ok, and use u after that",
        "a.gopp:84:6: cannot use v here: *User has no zero value, so v is set only where err is \
         nil",
        "    return or go on where err != nil, and use v after that",
        "a.gopp:89:7: cannot rely on the test of p from before the loop: the loop assigns p",
        "    test p inside the loop",
        "a.gopp:93:27: invalid operation: o + 1 (mismatched types Option[int] and int)",
    ];
    assert_eq!(checked.messages, expected);
    // The uses that read what the option holds, by their lines.
    let file = &checked.files[0];
    let mut lines: Vec<u32> = Vec::new();
    for body in &checked.package.files[0].bodies {
        for &expr in &body.narrowed {
            let span = file.exprs[expr].span;
            lines.push(checked.map.file(span.file).line_col(span.lo).line);
        }
    }
    lines.sort_unstable();
    lines.dedup();
    assert_eq!(lines, [23, 28, 31, 34, 37, 40, 42, 46, 49, 89]);
}

#[test]
fn the_steps_of_what_is_known_grow_with_the_input_not_faster() {
    let steps = |text: String| {
        let loader = Loader::default();
        let checked = check_texts("p", &[("a.gopp", &text)], &loader, &ThreadedRodeo::new());
        assert_eq!(checked.messages, [] as [&str; 0]);
        checked.steps
    };
    let header = "package p\nfunc next() Option[int] { return nil }\n";
    // Tests one after the other, each with its own use and assignment.
    let siblings = |count: usize| {
        let all: String = (0..count)
            .map(|i| format!("\tif o != nil && o > {i} {{\n\t\ttotal += o\n\t}}\n\to = next()\n"))
            .collect();
        format!("{header}func f(o Option[int]) int {{\n\ttotal := 0\n{all}\treturn total\n}}\n")
    };
    // A test inside a test, each of a variable of its own, with the
    // uses of all of them at the bottom.
    let nested = |depth: usize| {
        let params: String = (0..depth).map(|i| format!("o{i} Option[int], ")).collect();
        let open: String = (0..depth).map(|i| format!("if o{i} != nil {{\n")).collect();
        let uses: String = (0..depth).map(|i| format!("total += o{i}\n")).collect();
        format!(
            "{header}func f({params}n int) int {{\ntotal := n\n{open}{uses}{}return total\n}}\n",
            "}\n".repeat(depth)
        )
    };
    // Early exits, and many variables that stay known to the end.
    let exits = |count: usize| {
        let params: String = (0..count).map(|i| format!("o{i} Option[int], ")).collect();
        let tests: String = (0..count)
            .map(|i| format!("\tif o{i} == nil {{\n\t\treturn 0\n\t}}\n"))
            .collect();
        let uses: String = (0..count).map(|i| format!("\ttotal += o{i}\n")).collect();
        format!(
            "{header}func f({params}n int) int {{\n\ttotal := n\n{tests}{uses}\treturn total\n}}\n"
        )
    };
    // Loops in loops that use what was known before the outermost.
    let loops = |depth: usize| {
        let open: String = (0..depth)
            .map(|i| format!("for i{i} := 0; i{i} < 2; i{i}++ {{\n"))
            .collect();
        let uses = "total += o\n".repeat(depth);
        format!(
            "{header}func f(o Option[int]) int {{\ntotal := 0\nif o == nil {{\nreturn 0\n}}\n{open}{uses}{}return total\n}}\n",
            "}\n".repeat(depth)
        )
    };
    let sizes = [250, 500, 1_000, 2_000];
    linear::assert_near_linear(&sizes, |n| steps(siblings(n)));
    linear::assert_near_linear(&sizes, |n| steps(exits(n)));
    // The parser takes no deeper nesting.
    linear::assert_near_linear(&[50, 100, 200, 400], |n| steps(nested(n)));
    linear::assert_near_linear(&[50, 100, 200, 400], |n| steps(loops(n)));
}

// rule: CON-6a
#[test]
fn nil_is_no_value_of_what_is_never_nil_and_a_lost_write_is_refused() {
    let text = "package p

type Node struct{ v int }
type Shape interface{ Area() float64 }

func get() map[string]int       { return nil }
func lists() map[string][]int   { return nil }
func made() chan int            { return nil }

func good(p *Node, o Option[*Node], e error, x any, m map[string]int, xs []int, ch chan int, s Shape, os Option[Shape]) {
	_ = o == nil
	_ = e == nil
	_ = x != nil
	_ = m == nil
	_ = xs == nil
	_ = ch == nil
	_ = os == nil
	o = nil
	e = nil
	x = nil
	m = nil
	var local map[string]int
	local[\"a\"] = 1
	m[\"a\"] = 1
	c := made()
	close(c)
	close(ch)
	var later chan int
	later = make(chan int)
	close(later)
	if v, ok := x.(map[string]int); ok {
		v[\"a\"] = 1
	}
	_ = get()[\"a\"]
	_, _ = p, s
}

func bad(p *Node, u Unique[Node], sh Shared[Node], w Weak[Node], f func(), s Shape, x any) {
	_ = p == nil
	_ = nil != u
	_ = sh == nil
	_ = w == nil
	_ = s == nil
	p = nil
	u = nil
	sh = nil
	w = nil
	s = nil
	var q *Node = nil
	get()[\"a\"] = 1
	get()[\"a\"]++
	lists()[\"a\"] = append(lists()[\"a\"], 1)
	delete(get(), \"a\")
	x.(map[string]int)[\"a\"] = 1
	var ch chan int
	close(ch)
	var nilch chan int = nil
	close(nilch)
	_ = q
}

func closes(param chan int, pick bool) (named chan int) {
	var a chan int
	if pick {
		a = make(chan int)
	}
	close(a)
	var b chan int
	if b != nil {
		close(b)
	}
	var c chan int
	for i := 0; i < 2; i++ {
		close(c)
		c = make(chan int)
	}
	var d chan int
	_ = func() { d = make(chan int) }
	close(d)
	var e chan int
Again:
	close(e)
	if pick {
		goto Again
	}
	close(param)
	close(named)

	var f chan int
	defer close(f)
	g := make(chan int)
	g = nil
	close(g)
	return nil
}
";
    let expected = [
        "a.gopp:24:2: this write to m is lost: m is the function's own copy, and nothing reads it \
         afterwards",
        "    declare m *map[string]int to change the value of the caller, or return m",
        "a.gopp:39:6: invalid operation: p == nil (a *Node is never nil: use Option[*Node])",
        "a.gopp:40:13: invalid operation: nil != u (a Unique[Node] is never nil: use \
         Option[Unique[Node]])",
        "a.gopp:41:6: invalid operation: sh == nil (a Shared[Node] is never nil: use \
         Option[Shared[Node]])",
        "a.gopp:42:6: invalid operation: w == nil (a Weak[Node] is never nil: use \
         Option[Weak[Node]])",
        "a.gopp:43:6: invalid operation: s == nil (a Shape is never nil: use Option[Shape])",
        "a.gopp:44:6: cannot use nil as *Node value in assignment",
        "    a *Node is never nil: use Option[*Node]",
        "a.gopp:45:6: cannot use nil as Unique[Node] value in assignment",
        "    a Unique[Node] is never nil: use Option[Unique[Node]]",
        "a.gopp:46:7: cannot use nil as Shared[Node] value in assignment",
        "    a Shared[Node] is never nil: use Option[Shared[Node]]",
        "a.gopp:47:6: cannot use nil as Weak[Node] value in assignment",
        "    a Weak[Node] is never nil: use Option[Weak[Node]]",
        "a.gopp:48:6: cannot use nil as Shape value in assignment",
        "    a Shape is never nil: use Option[Shape]",
        "a.gopp:49:16: cannot use nil as *Node value in variable declaration",
        "    a *Node is never nil: use Option[*Node]",
        "a.gopp:50:2: cannot change get()[\"a\"]: no variable holds the map, and the change would \
         be lost",
        "    put it in a variable first",
        "a.gopp:51:2: cannot change get()[\"a\"]: no variable holds the map, and the change would \
         be lost",
        "    put it in a variable first",
        "a.gopp:52:2: cannot change lists()[\"a\"]: no variable holds the map, and the change \
         would be lost",
        "    put it in a variable first",
        "a.gopp:53:9: cannot change get(): no variable holds the map, and the change would be lost",
        "    put it in a variable first",
        "a.gopp:54:2: cannot change x.(map[string]int)[\"a\"]: the assertion gives a copy, and the \
         change would be lost",
        "    put it in a variable first",
        "a.gopp:56:8: cannot close ch: ch is nil on every way here",
        "    make the channel first: ch = make(...)",
        "a.gopp:58:8: cannot close nilch: nilch is nil on every way here",
        "    make the channel first: nilch = make(...)",
        "a.gopp:90:14: cannot close f: f is nil on every way here",
        "    make the channel first: f = make(...)",
        "a.gopp:93:8: cannot close g: g is nil on every way here",
        "    make the channel first: g = make(...)",
    ];
    assert_eq!(messages("a.gopp", text), expected);
}

/// The labels of each variable of a package, by its name: `x: Send Sync`.
fn labels_of(text: &str) -> Vec<String> {
    let names = ThreadedRodeo::new();
    let loader = Loader::default();
    let checked = check_texts("p", &[("a.gopp", text)], &loader, &names);
    // The variables only name their types.
    let unset = |message: &&String| {
        !(message.contains(" without a value: ") || message.starts_with("    give "))
    };
    let said: Vec<&String> = checked.messages.iter().filter(unset).collect();
    assert_eq!(said, [] as [&String; 0]);
    let types = loader.types.get().unwrap();
    let variables = checked.package.objects.iter();
    variables
        .filter(|object| matches!(object.kind, ObjKind::Var))
        .map(|object| {
            let labels = types.labels(object.ty);
            let send = if labels.send { " Send" } else { "" };
            let sync = if labels.sync { " Sync" } else { "" };
            format!("{}:{send}{sync}", names.resolve(&object.name))
        })
        .collect()
}

// rule: CON-12, CON-19
#[test]
fn a_type_is_given_to_another_goroutine_or_shared_as_its_parts_allow() {
    let text = "package p

type Point struct{ X, Y int }
type Doc struct {
	Title string
	Tags  []string
}
type File struct{ fd int }

func (f *File) drop() {}

type Tree struct {
	kids []Tree
	name string
}
type Ring struct {
	next Option[Unique[Ring]]
	back *Ring
}
type Reader interface{ Read() int }
type Safe interface {
	Sync
	Read() int
}
type Holder struct {
	r Reader
	n int
}
type Box[T any] struct{ v T }

var (
	number  int
	text    string
	point   Point
	doc     Doc
	file    File
	array   [4]Doc
	slice   []Point
	table   map[string]Doc
	channel chan *Point
	fn      func(*Point)
	pointer *Point
	unique  Unique[Doc]
	shared  Shared[Doc]
	weak    Weak[Doc]
	option  Option[Doc]
	tree    Tree
	ring    Ring

	reader   Reader
	safe     Safe
	anything any
	failure  error
	holder   Holder
	local    Local[func()]
	localAny Local[any]

	sharedReader Shared[Reader]
	sharedSafe   Shared[Safe]
	sharedRing   Shared[Ring]
	uniqueRing   Unique[Ring]
	pointers     []*Point
	byPointer    map[string]*Point
	optPointer   Option[*Point]
	boxInt       Box[int]
	boxPointer   Box[*int]
	boxReader    Box[Reader]
)
";
    let expected = [
        "number: Send Sync",
        "text: Send Sync",
        "point: Send Sync",
        "doc: Send Sync",
        "file: Send Sync",
        "array: Send Sync",
        "slice: Send Sync",
        "table: Send Sync",
        "channel: Send Sync",
        "fn: Send Sync",
        "pointer:",
        "unique: Send Sync",
        "shared: Send Sync",
        "weak: Send Sync",
        "option: Send Sync",
        "tree: Send Sync",
        "ring:",
        "reader: Send",
        "safe: Send Sync",
        "anything: Send",
        "failure: Send Sync",
        "holder: Send",
        "local:",
        "localAny:",
        "sharedReader:",
        "sharedSafe: Send Sync",
        "sharedRing:",
        "uniqueRing:",
        "pointers:",
        "byPointer:",
        "optPointer:",
        "boxInt: Send Sync",
        "boxPointer:",
        "boxReader: Send",
    ];
    assert_eq!(labels_of(text), expected);
}

// rule: GEN-11
#[test]
fn a_type_argument_has_the_labels_that_the_constraint_names() {
    let text = "package p

type Node struct{ v int }
type Tree struct {
	kids []Tree
	name string
}
type Ring struct {
	next Option[Unique[Ring]]
	back *Ring
}
type I interface{ M() }
type Safe interface {
	Sync
	M()
}
type Plain struct{ n Node }

func (p Plain) M() {}

func spawn[T Send](x T)   {}
func share[T Sync](x T)   {}
func both[T interface{ Send; Clone }](x T) {}
func pass[U any](u U)     { spawn(move(u)) }
func sent[U Send](u U)    { spawn(move(u)) }
func listed[U ~int | ~string](u U) { spawn(u); share(u) }

var a Local[int]
var d Unique[*int]

func conv(f func(), lf Local[func()], i I, li Local[I], p Plain) {
	lf = f
	lf = func() {}
	li = i
	li = p
	lf()
	li.M()
	f = lf
	i = li
}

func labels(n Node, pn *Node, t Tree, r Ring, s Shared[Node], sp Shared[Ring], w Weak[Tree], i I, sf Safe, e error, x any, lf Local[func()], ch chan *Node, m map[string][]int, mp map[string]*Node, u Unique[Node], o Option[Tree], f func()) {
	spawn(n)
	spawn(t)
	spawn(s)
	spawn(w)
	spawn(i)
	spawn(e)
	spawn(x)
	spawn(ch)
	spawn(m)
	spawn(u)
	spawn(o)
	spawn(f)
	share(n)
	share(t)
	share(s)
	share(sf)
	share(e)
	share(m)
	share(f)
	both(n)

	spawn(pn)
	spawn(r)
	spawn(sp)
	spawn(lf)
	spawn(mp)
	share(i)
	share(x)
	share(pn)
	share(lf)
	spawn[*int](nil)
}
";
    let expected = [
        "a.gopp:24:29: U does not satisfy Send (U is not constrained by Send)",
        "a.gopp:28:7: invalid type Local[int]: Local is written for a func or an interface type, \
         and int is neither",
        "a.gopp:29:7: invalid type Unique[*int]: a *int borrows, and what borrows is not kept on \
         the heap",
        "a.gopp:38:6: cannot use lf (variable of type Local[func()]) as func() value in assignment",
        "a.gopp:39:6: cannot use li (variable of type Local[I]) as I value in assignment: Local[I] \
         does not implement I (missing method M)",
        "a.gopp:64:2: *Node does not satisfy Send (a *Node borrows what it points to)",
        "a.gopp:65:2: Ring does not satisfy Send (a *Ring borrows what it points to)",
        "a.gopp:66:2: Shared[Ring] does not satisfy Send (what a Shared[Ring] holds is not Sync)",
        "a.gopp:67:2: Local[func()] does not satisfy Send (a Local[func()] stays with one \
         goroutine)",
        "a.gopp:68:2: map[string]*Node does not satisfy Send (a *Node borrows what it points to)",
        "a.gopp:69:2: I does not satisfy Sync (I does not embed Sync)",
        "a.gopp:70:2: any does not satisfy Sync (any does not embed Sync)",
        "a.gopp:71:2: *Node does not satisfy Sync (a *Node borrows what it points to)",
        "a.gopp:72:2: Local[func()] does not satisfy Sync (a Local[func()] stays with one \
         goroutine)",
        "a.gopp:73:8: *int does not satisfy Send (a *int borrows what it points to)",
        "a.gopp:73:14: cannot use nil as *int value in argument to spawn[*int]",
        "    a *int is never nil: use Option[*int]",
    ];
    assert_eq!(messages("a.gopp", text), expected);
}

#[test]
fn the_steps_of_labels_grow_with_the_types_not_faster() {
    let steps = |text: String| {
        let loader = Loader::default();
        let checked = check_texts("p", &[("a.gopp", &text)], &loader, &ThreadedRodeo::new());
        assert_eq!(checked.messages, [] as [&str; 0]);
        checked.steps + loader.types.get().unwrap().steps()
    };
    let header = "package p\nfunc spawn[T Send](x Option[T]) {}\n";
    // Each type holds the one before it, and each is asked, the last
    // first.
    let chain = |count: usize| {
        let types: String = (1..count)
            .map(|i| format!("type T{i} struct{{ a T{}; b [2]T{} }}\n", i - 1, i - 1))
            .collect();
        let asked: String = (0..count)
            .rev()
            .map(|i| format!("\tspawn[T{i}](nil)\n"))
            .collect();
        format!("{header}type T0 struct{{ n int }}\n{types}func f() {{\n{asked}}}\n")
    };
    // Types that hold each other in a ring, each asked.
    let ring = |count: usize| {
        let types: String = (0..count)
            .map(|i| format!("type T{i} struct{{ next []T{}; n int }}\n", (i + 1) % count))
            .collect();
        let asked: String = (0..count)
            .map(|i| format!("\tspawn[T{i}](nil)\n"))
            .collect();
        format!("{header}{types}func f() {{\n{asked}}}\n")
    };
    let sizes = [250, 500, 1_000, 2_000];
    linear::assert_near_linear(&sizes, |n| steps(chain(n)));
    linear::assert_near_linear(&sizes, |n| steps(ring(n)));
}

// rule: TYP-5, TYP-6, TYP-7, TYP-9
#[test]
fn an_interface_value_holds_a_copy_that_may_go_to_another_goroutine() {
    let text = "package p

type File struct{ fd int }

func (f *File) drop() {}

type Point struct{ X, Y int }
type Reader interface{ Read() int }
type Buffer struct{ data []byte }

func (b *Buffer) Read() int { return len(b.data) }

type Fail struct{ msg string }

func (f Fail) Error() string { return f.msg }

type Soft struct{ n *int }

func (s *Soft) Error() string { return \"\" }

var saved any

func println(args ...any) {}
func log(v any)          { saved = v }
func open() File         { return File{} }

func good(name string, xs []int, f File, pt Point, buf Buffer, v any) (any, error) {
	var a any = name
	var c any = xs
	var b any = NewShared(move(f))
	var q Local[any] = &pt
	var r Local[Reader] = &buf
	var e error = Fail{msg: \"x\"}
	println(name, xs, &f, &pt, pt)
	log(name)
	if s, ok := v.(string); ok {
		_ = len(s)
	}
	switch x := v.(type) {
	case []int:
		x[0] = 1
	case map[string]int:
		x[\"b\"] = 2
	case *Point:
		_ = x
	}
	_, _, _, _, _, _ = a, b, c, q, r, e
	return pt, Fail{}
}

func bad(f File, pt Point, buf Buffer, v any, s Soft, lf Local[func()], files []File) (any, error, Reader) {
	var a any = f
	var b any = move(f)
	var c any = open()
	println(f)
	log(move(f))
	var d any = files
	var p any = &pt
	var r Reader = &buf
	var l any = lf
	saved = &pt
	_ = v.(File)
	switch v.(type) {
	case File:
	case []File:
	}
	_, _, _, _, _, _, _ = a, b, c, d, p, r, l
	return &pt, &s, &buf
}
";
    let names = ThreadedRodeo::new();
    let checked = check_texts("p", &[("a.gopp", text)], &Loader::default(), &names);
    let expected = [
        "a.gopp:34:21: f is used after it was moved: it has no value until it is assigned again",
        "    a.gopp:30:24: f is moved here",
        "a.gopp:52:14: cannot convert f (variable of struct type File) to any: an interface value \
         holds a copy, and File has a drop method and no Clone() File",
        "    put it in a Shared, or pass &f for the length of a call",
        "a.gopp:53:14: cannot convert move(f) (value of struct type File) to any: an interface \
         value holds a copy, and File has a drop method and no Clone() File",
        "    put it in a Shared: NewShared(move(f))",
        "a.gopp:54:14: cannot convert open() (value of struct type File) to any: an interface \
         value holds a copy, and File has a drop method and no Clone() File",
        "    put it in a Shared: NewShared(open())",
        "a.gopp:55:10: cannot convert f (variable of struct type File) to any: an interface value \
         holds a copy, and File has a drop method and no Clone() File",
        "    put it in a Shared, or pass &f for the length of a call",
        "a.gopp:55:10: f is used after it was moved: it has no value until it is assigned again",
        "    a.gopp:53:14: f is moved here",
        "a.gopp:56:6: cannot convert move(f) (value of struct type File) to any: an interface \
         value holds a copy, and File has a drop method and no Clone() File",
        "    put it in a Shared: NewShared(move(f))",
        "a.gopp:57:14: cannot convert files (variable of type []File) to any: an interface value \
         holds a copy, and File has a drop method and no Clone() File",
        "    put it in a Shared, or pass &files for the length of a call",
        "a.gopp:58:14: cannot keep &pt (value of type *Point) in a value of type any: an interface \
         value may be given to another goroutine, and a *Point borrows what it points to",
        "    keep it in a Local[any], or hold a Shared",
        "a.gopp:59:17: cannot keep &buf (value of type *Buffer) in a value of type Reader: an \
         interface value may be given to another goroutine, and a *Buffer borrows what it points \
         to",
        "    keep it in a Local[Reader], or hold a Shared",
        "a.gopp:60:14: cannot keep lf (variable of type Local[func()]) in a value of type any: an \
         interface value may be given to another goroutine, and a Local[func()] stays with one \
         goroutine",
        "    keep it in a Local[any], or hold a Shared",
        "a.gopp:61:10: cannot keep &pt (value of type *Point) in a value of type any: an interface \
         value may be given to another goroutine, and a *Point borrows what it points to",
        "    keep it in a Local[any], or hold a Shared",
        "a.gopp:62:6: impossible type assertion: no interface value holds a File: File has a drop \
         method and no Clone() File",
        "a.gopp:64:7: impossible type switch case: no interface value holds a File: File has a \
         drop method and no Clone() File",
        "a.gopp:65:7: impossible type switch case: no interface value holds a []File: File has a \
         drop method and no Clone() File",
        "a.gopp:68:9: cannot keep &pt (value of type *Point) in a value of type any: an interface \
         value may be given to another goroutine, and a *Point borrows what it points to",
        "    keep it in a Local[any], or hold a Shared",
        "a.gopp:68:14: cannot keep &s (value of type *Soft) in a value of type error: an interface \
         value may be given to another goroutine, and a *Soft borrows what it points to",
        "    keep it in a Local[error], or hold a Shared",
        "a.gopp:68:18: cannot keep &buf (value of type *Buffer) in a value of type Reader: an \
         interface value may be given to another goroutine, and a *Buffer borrows what it points \
         to",
        "    keep it in a Local[Reader], or hold a Shared",
    ];
    assert_eq!(checked.messages, expected);
    // The conversions that are arguments, by their lines: whether each is
    // boxed, the function that takes it says.
    let file = &checked.files[0];
    let mut lines: Vec<u32> = Vec::new();
    for body in &checked.package.files[0].bodies {
        for &expr in &body.iface_args {
            let span = file.exprs[expr].span;
            lines.push(checked.map.file(span.file).line_col(span.lo).line);
        }
    }
    lines.sort_unstable();
    assert_eq!(lines, [34, 34, 34, 34, 34, 35]);
}

// rule: TYP-16, TYP-17, TYP-18, TYP-19, TYP-20, TYP-21, EXP-13, EXP-15, GEN-12
#[test]
fn a_type_has_what_its_parents_have_and_replaces_what_they_let_it() {
    let text = "package p

type Payment struct {
	Amount int @Required
}

@Virtual
func (p *Payment) Fee() int { return 1 }

@Abstract
func (p *Payment) Name() string

func (p *Payment) Total() int { return p.Amount + p.Fee() }

type Stamps struct{ Created int }

func (s *Stamps) Touch() { s.Created++ }

@Extends(Payment)
@Extends(Stamps)
type Card struct {
	Number string
}

@Override
func (c *Card) Fee() int { return base.Fee() + base[Payment].Fee() + base.Created }

@Override
func (c *Card) Name() string { return \"card\" }

type Namer interface{ Name() string }

func good() {
	c := Card{Amount: 100, Created: 1, Number: \"4242\"}
	d := Card{Payment: Payment{Amount: 1}, Number: \"1\"}
	c.Touch()
	c.Amount = 3
	_ = c.Total() + c.Created + len(c.Number) + d.Amount
	var p *Payment = &c
	var n Local[Namer] = &c
	u := NewUnique(Card{Amount: 2})
	var up Unique[Payment] = move(u)
	q, ok := p.(*Card)
	switch x := p.(type) {
	case *Card:
		_ = x.Number
	}
	if ok {
		_ = q
	}
	_, _, _ = n, up, p
}

func bad(c Card, p *Payment) {
	_ = Card{1, 2, \"x\"}
	_ = Card{Number: \"x\"}
	_ = Card{Amount: 1, Payment: Payment{Amount: 2}}
	_ = Card{Amount: 1, Amount: 2}
	_ = Card{Amount: 1, Missing: 2}
	var v Payment = c
	var s *Stamps = &c
	var x Payment
	_ = Payment{Amount: 1}
	var xs []Payment
	_ = p.(*Stamps)
	st := &c.Created
	_ = st.(*int)
	_, _, _, _ = v, s, x, xs
}

type Box[T any] struct{ v T }
type Ptr[T any] struct{ p *T }

func feed[T any](p *T) {}
func keep[T any](v T)  {}

func generic(c *Card) {
	var a Ptr[Payment]
	feed[Payment](c)
	var b Box[Payment]
	keep[Payment](*c)
	_, _ = a, b
}

type Entity[T Clone] struct{ ID T }

@Virtual
func (e *Entity[T]) Key() T { return e.ID }

type Log[T any] struct{ lines []T }

@Extends(Entity[T])
@Extends(Log[T])
type Store[T Clone] struct{ Name string }

@Override
func (s *Store[T]) Key() T { return base.Key() }

@Extends(Payment)
type Paid interface{}

func fee[T Paid](p *T) int {
	var q *Payment = p
	return p.Fee() + p.Amount + q.Amount
}

@Extends(Payment)
@Abstract
type Wire struct{}

func more(s *Store[string], c *Card, st *Stamps, w *Wire) {
	_ = s.ID + s.Name + s.Key()
	_ = len(s.lines)
	var e *Entity[string] = s
	_ = fee(c) + fee[Card](c)
	_ = fee(st)
	var bad Paid
	_ = *e
	copied := *c
	_, _, _ = e, bad, copied
}

func (p Payment) ByValue() {}

func (c *Card) Alone() {
	x := base
	_ = x
}

func (s *Stamps) None() int { return base.Created }
";
    let expected = [
        "a.gopp:55:11: a literal of Card names its fields: the type has parents",
        "a.gopp:56:6: missing Amount (@Required) in struct literal of type Card",
        "a.gopp:57:22: Payment is set twice in the literal of Card",
        "a.gopp:58:22: Amount is set twice in the literal of Card",
        "a.gopp:59:22: unknown field Missing in struct literal of type Card",
        "a.gopp:60:8: cannot hold a value of Payment: the type is abstract; hold a pointer to a \
         child",
        "a.gopp:60:18: cannot use c (variable of struct type Card) as Payment value: a Card is \
         more than its parent Payment",
        "    use a pointer",
        "a.gopp:61:18: cannot use &c (value of type *Card) as *Stamps value: Stamps has no type \
         table, so nothing says what a *Stamps points to",
        "    add a @Virtual method to Stamps, or use the pointer to the child",
        "a.gopp:62:6: cannot declare x without a value: Payment has no zero value (the field \
         Amount of Payment is @Required)",
        "    give x a value, or declare it as Option[Payment]",
        "a.gopp:62:8: cannot hold a value of Payment: the type is abstract; hold a pointer to a \
         child",
        "a.gopp:63:6: cannot make a value of Payment: the type is abstract",
        "a.gopp:64:11: cannot hold a value of Payment: the type is abstract; hold a pointer to a \
         child",
        "a.gopp:65:6: impossible type assertion: *Stamps is no pointer to Payment or to a child of \
         it",
        "a.gopp:67:6: invalid operation: st (variable of type *int) is not an interface",
        "a.gopp:78:6: cannot declare a without a value: Ptr[Payment] has no zero value (a *Payment \
         is never nil)",
        "    give a a value, or declare it as Option[Ptr[Payment]]",
        "a.gopp:80:6: cannot declare b without a value: Box[Payment] has no zero value (the field \
         Amount of Payment is @Required)",
        "    give b a value, or declare it as Option[Box[Payment]]",
        "a.gopp:80:12: cannot use Payment for T: a value of it is held, and Payment is abstract",
        "a.gopp:81:7: cannot use Payment for T: a value of it is held, and Payment is abstract",
        "a.gopp:81:16: cannot use *c (variable of struct type Card) as Payment value: a Card is \
         more than its parent Payment",
        "    use a pointer",
        "a.gopp:116:6: Stamps does not satisfy Paid (Stamps does not extend Payment)",
        "a.gopp:117:10: cannot use Paid as the type of a value: an interface with @Extends is a \
         constraint",
        "a.gopp:119:12: cannot copy what *c points to: it may be a child of Card, and only the \
         parent part would be copied",
        "    use the pointer, or copy the whole object with Clone()",
        "a.gopp:123:18: cannot declare ByValue on a value of Payment: the type is abstract and has \
         no values",
        "a.gopp:126:7: base is no value: it selects a member of a parent, as in base.M()",
        "a.gopp:130:38: undefined: base (Stamps has no parents)",
    ];
    assert_eq!(messages("a.gopp", text), expected);
}

// rule: TYP-16, TYP-18, TYP-19
#[test]
fn a_hierarchy_has_each_ancestor_once_and_says_what_it_replaces() {
    let header = "package p\n\ntype A struct{ n int }\n\n@Virtual\nfunc (a *A) V() {}\nfunc (a *A) F() {}\n\n@Abstract\nfunc (a *A) Open()\n\ntype M struct{ m int }\n\nfunc (m *M) F() {}\n\n";
    let cases: [(&str, &[&str]); 16] = [
        (
            "@Extends(int)\ntype B struct{}",
            &["invalid parent int: a parent is a struct type"],
        ),
        (
            "type I interface{}\n\n@Extends(I)\ntype B struct{}",
            &["invalid parent I: a parent is a struct type"],
        ),
        (
            "@Extends(C)\n@Abstract\ntype B struct{}\n\n@Extends(B)\n@Abstract\ntype C struct{}",
            &[
                "invalid parents: B is its own ancestor",
                "invalid parents: C is its own ancestor",
            ],
        ),
        (
            "@Extends(M)\n@Extends(M)\ntype B struct{}",
            &["invalid parents of B: M is an ancestor twice"],
        ),
        (
            "@Extends(M)\ntype B struct{}\n\n@Extends(B)\n@Extends(M)\ntype C struct{}",
            &["invalid parents of C: M is an ancestor twice"],
        ),
        (
            "@Extends(A)\n@Abstract\ntype B struct{}\n\ntype C B",
            &[
                "cannot define C from B: B has parents or methods that may be replaced",
                "    write @Extends(B) before type C struct{}",
            ],
        ),
        (
            "@Abstract\ntype B struct{}",
            &["cannot use @Abstract on B: it is written on a type with parents, or on a method"],
        ),
        (
            "@Extends(M)\ntype B struct{ m int }",
            &["field m of B has the name of a member of its ancestor M"],
        ),
        (
            "@Extends(A)\n@Extends(M)\n@Abstract\ntype B struct{}",
            &["the parents A and M of B both have a member F"],
        ),
        (
            "@Extends(A)\ntype B struct{}",
            &[
                "B does not override the @Abstract method Open of A",
                "    declare the method with @Override, or mark the type @Abstract",
            ],
        ),
        (
            "@Extends(A)\n@Abstract\ntype B struct{}\n\nfunc (b *B) V() {}",
            &["B.V replaces A.V: mark it @Override"],
        ),
        (
            "@Extends(A)\n@Abstract\ntype B struct{}\n\n@Override\nfunc (b *B) F() {}",
            &["cannot replace A.F: it is final, because it is not marked @Virtual"],
        ),
        (
            "@Extends(A)\n@Abstract\ntype B struct{}\n\n@Override\nfunc (b *B) W() {}",
            &["B.W is marked @Override, and no ancestor of B has a method W"],
        ),
        (
            "@Extends(A)\n@Abstract\ntype B struct{}\n\n@Override\nfunc (b *B) V(n int) {}",
            &[
                "B.V does not have the signature of A.V",
                "    have func(int)",
                "    want func()",
            ],
        ),
        (
            "@Abstract\nfunc (m *M) Body() {}",
            &["the @Abstract method Body has a body: a child gives it one"],
        ),
        // What is right: a child of a child, an override of an override,
        // and an abstract method that a grandchild provides.
        (
            "@Extends(A)\n@Abstract\ntype B struct{}\n\n@Override\nfunc (b *B) V() {}\n\n@Extends(B)\ntype C struct{}\n\n@Override\nfunc (c *C) V() {}\n\n@Override\nfunc (c *C) Open() {}\n\nvar c = C{n: 1}\nvar p *A = &c",
            &[],
        ),
    ];
    for (source, expected) in cases {
        let text = format!("{header}{source}\n");
        let found: Vec<String> = messages("a.gopp", &text)
            .into_iter()
            .map(|message| match message.split_once(": ") {
                Some((place, rest)) if place.starts_with("a.gopp:") => rest.to_owned(),
                _ => message,
            })
            .collect();
        assert_eq!(found, *expected, "{source}");
    }
}

#[test]
fn the_steps_of_hierarchies_grow_with_the_types_not_faster() {
    let steps = |text: String| {
        let loader = Loader::default();
        let checked = check_texts("p", &[("a.gopp", &text)], &loader, &ThreadedRodeo::new());
        assert_eq!(checked.messages, [] as [&str; 0]);
        checked.steps + loader.types.get().unwrap().steps()
    };
    let header =
        "package p\ntype Root struct{ n int }\n\n@Virtual\nfunc (r *Root) M() int { return r.n }\n";
    // Many children of one parent, each with a method that replaces the
    // parent's, a literal and an upcast.
    let siblings = |count: usize| {
        let all: String = (0..count)
            .map(|i| format!(
                "@Extends(Root)\ntype C{i} struct{{ f{i} int }}\n\n@Override\nfunc (c *C{i}) M() int {{ return base.M() + c.f{i} + c.n }}\n\
                 func use{i}() int {{ c := C{i}{{n: 1, f{i}: 2}}; var r *Root = &c; return r.M() }}\n"
            ))
            .collect();
        format!("{header}{all}")
    };
    // Each type is a child of the one before it and replaces its method.
    let chain = |count: usize| {
        let all: String = (0..count)
            .map(|i| {
                let parent = match i {
                    0 => "Root".to_owned(),
                    _ => format!("C{}", i - 1),
                };
                format!("@Extends({parent})\ntype C{i} struct{{}}\n\n@Override\nfunc (c *C{i}) M() int {{ return base.M() + 1 }}\n")
            })
            .collect();
        format!("{header}{all}")
    };
    let sizes = [250, 500, 1_000, 2_000];
    linear::assert_near_linear(&sizes, |n| steps(siblings(n)));
    linear::assert_near_linear(&sizes, |n| steps(chain(n)));
}

/// What is said about the declarations, written after a clause that
/// imports package unsafe.
fn with_unsafe(file: &str, declarations: &str) -> Vec<String> {
    let names = ThreadedRodeo::new();
    let stub = "package unsafe\ntype Pointer *int\nfunc Sizeof(x any) uintptr\n";
    let loader = with_packages(&names, &[("unsafe", stub)]);
    let text = format!("package x; import \"unsafe\"; var _ unsafe.Pointer\n{declarations}\n");
    check_texts("x", &[(file, &text)], &loader, &names).messages
}

/// What is said about the file, which may import package unsafe.
fn with_unsafe_text(file: &str, text: &str) -> Vec<String> {
    let names = ThreadedRodeo::new();
    let stub = "package unsafe\ntype Pointer *int\nfunc Sizeof(x any) uintptr\n";
    let loader = with_packages(&names, &[("unsafe", stub)]);
    check_texts("p", &[(file, text)], &loader, &names).messages
}

// rule: GEN-5
#[test]
fn a_generic_that_would_need_instances_without_end_is_refused() {
    let goods = [
        "func F[T any](x T) { F(move(x)) }",
        "func F[T, U, V any]() { F[U, V, T](); F[V, T, U]() }",
        "type Ring[A, B, C any] struct { L *Ring[B, C, A]; R *Ring[C, A, B] }",
        "func F[T any]() { type U[T any] [unsafe.Sizeof(F[*T])]byte }",
        "func F[T any]() { type U[T any] [unsafe.Sizeof(F[*T])]byte; var _ U[int] }",
        "type U[T any] [unsafe.Sizeof(F[*T])]byte; func F[T any]() { var _ U[U[int]] }",
        "func F[T any]() { type A = int; F[A]() }",
    ];
    for good in goods {
        assert_eq!(with_unsafe("a.gopp", good), [] as [&str; 0], "{good}");
    }
    let bads = [
        "func F[T any](x T) { F(&x) }",
        "func F[T any]() { F[*T]() }",
        "func F[T any]() { F[[]T]() }",
        "func F[T any]() { F[[1]T]() }",
        "func F[T any]() { F[chan T]() }",
        "func F[T any]() { F[map[*T]int]() }",
        "func F[T any]() { F[map[error]T]() }",
        "func F[T any]() { F[func(T)]() }",
        "func F[T any]() { F[func() T]() }",
        "func F[T any]() { F[struct{ t T }]() }",
        "func F[T any]() { F[interface{ t() T }]() }",
        "type U[_ any] int; func F[T any]() { F[U[T]]() }",
        "func F[T any]() { type U int; F[U]() }",
        "func F[T any]() { type U int; F[*U]() }",
        "type U[T any] int; func (U[T]) m() { var _ U[*T] }",
        "type U[T any] int; func (*U[T]) m() { var _ U[*T] }",
        "type U[T1 any] [unsafe.Sizeof(F[*T1])]byte; func F[T2 any]() { var _ U[T2] }",
        "func F[A, B, C, D, E any]() { F[B, C, D, E, *A]() }",
        "type U[_ any] int; const X = unsafe.Sizeof(func() { type A[T any] U[A[*T]] })",
        "func F[T any]() { type A = *T; F[A]() }",
    ];
    for bad in bads {
        let found = with_unsafe("a.gopp", bad);
        let first = found.first().map(String::as_str).unwrap_or_default();
        assert!(
            first.ends_with(": instantiation cycle:"),
            "{bad}: {found:?}"
        );
    }

    // A type that holds such an instance of itself by value is refused
    // for that.
    assert_eq!(
        with_unsafe("a.gopp", "type A[T any] struct { _ A[*T] }"),
        ["a.gopp:2:26: invalid recursive type: A refers to itself"]
    );

    // The cycle is told at the first type parameter on it, with what
    // each one is instantiated as.
    let text = "package p
type Box[A any] struct{ value A }
func Nest[A interface{ Clone; Send }](b Box[A], n int) any {
	if n == 0 {
		return b
	}
	return Nest(Box[Box[A]]{b}, n-1)
}
";
    let expected = [
        "a.gopp:3:11: instantiation cycle:",
        "    a.gopp:7:9: A instantiated as Box[A]",
    ];
    assert_eq!(messages("a.gopp", text), expected);
    let text = "package p
func f[A, B any]() { g[B, *A]() }
func g[C, D any]() { f[D, C]() }
";
    let expected = [
        "a.gopp:2:8: instantiation cycle:",
        "    a.gopp:3:24: A instantiated as D",
        "    a.gopp:2:27: D instantiated as *A",
    ];
    assert_eq!(messages("a.gopp", text), expected);

    // Every type on a cycle is known before a body follows its
    // instances, not only those of the cycle that is told.
    let text = "package p
type T[P any] struct {
	s []T[[]P]
	m map[string]*T[*P]
}
type A[P any] struct{ next Option[Unique[B[[]P]]] }
type B[P any] struct {
	back []A[P]
	c    chan A[*P]
}

@Extends(Root[P])
type Kid[P any] struct{ more []Kid[*P] }
type Root[P any] struct{ v P }

func f(x T[int], y A[int], k Kid[int]) {
	ch := make(chan *A[int])
	go func() { ch <- &y }()
	w, kk := x, k
	_, _ = w, kk
}
";
    let expected = [
        "a.gopp:2:8: instantiation cycle:",
        "    a.gopp:3:8: P instantiated as []P",
    ];
    assert_eq!(messages("a.gopp", text), expected);

    // A type that is declared inside a generic function is another type
    // for each instance of the function: it stands for the type
    // parameters around it, those of a receiver and of a function
    // around a function literal too.
    let text = "package p
func F[T any]() {
	type U int
	F[*U]()
}
";
    let expected = [
        "a.gopp:2:8: instantiation cycle:",
        "    a.gopp:4:4: T instantiated as *U",
        "    a.gopp:3:7: U implicitly parameterized by T",
    ];
    assert_eq!(messages("a.gopp", text), expected);
    let text = "package p
type G[P any] struct{}

func (g G[P]) M() {
	f := func() {
		type L struct{ p P }
		var _ G[L]
	}
	f()
}
";
    let expected = [
        "a.gopp:2:8: instantiation cycle:",
        "    a.gopp:7:11: P instantiated as L",
        "    a.gopp:6:8: L implicitly parameterized by P",
    ];
    assert_eq!(messages("a.gopp", text), expected);
    let text = "package p
func H[A, B any]() {
	type L struct{}
	H[B, L]()
}
";
    let expected = [
        "a.gopp:2:8: instantiation cycle:",
        "    a.gopp:4:4: A instantiated as B",
        "    a.gopp:4:7: B instantiated as L",
        "    a.gopp:3:7: L implicitly parameterized by A",
    ];
    assert_eq!(messages("a.gopp", text), expected);
    // A local type that goes nowhere else, and one of a function without
    // type parameters, make no cycle.
    let text = "package p
type Box[T any] struct{ v T }

func F[T any](x T) {
	type L struct{ x T }
	var _ Box[L]
	var _ []L
}

func plain() {
	type L struct{}
	var _ Box[Box[L]]
}
";
    assert_eq!(messages("a.gopp", text), [] as [&str; 0]);

    // A package with another error is not told of its cycle.
    let text = "package p\nfunc f[T any]() { f[*T]() }\nvar x int = \"\"\n";
    let expected = [
        "a.gopp:3:13: cannot use \"\" (untyped string constant) as int value in variable declaration",
    ];
    assert_eq!(messages("a.gopp", text), expected);
}

// rule: GEN-13
#[test]
fn the_signature_of_a_method_instantiates_as_a_field_does() {
    let cycle = "package p
type G[T any] struct{ v T }
func (x G[T]) Next() Option[*G[*T]] { return nil }
";
    let expected = [
        "a.gopp:2:8: instantiation cycle:",
        "    a.gopp:3:32: T instantiated as *T",
    ];
    assert_eq!(messages("a.gopp", cycle), expected);

    // The same instance, the parameters in another order and another
    // type need no new instance; a type parameter of the method is not
    // one of the type.
    let fine = "package p
type G[T any] struct{ v T }
func (x G[T]) Same() Option[*G[T]] { return nil }
func (x G[T]) Other() G[int] { return G[int]{} }
type Two[A, B Clone] struct{ a A; b B }
func (x Two[A, B]) Swap() Two[B, A] { return Two[B, A]{a: x.b, b: x.a} }
type Pair[A, B any] struct{ a A; b B }
type List[T any] struct{ items []T }
func (l List[T]) Zip[U any](o List[U]) List[Pair[T, U]] { return List[Pair[T, U]]{} }
";
    assert_eq!(messages("a.gopp", fine), [] as [&str; 0]);
    let deep = "package p
type List[T Clone] struct{ items []T }
func (l List[T]) Deep[U any]() { l.Deep[*U]() }
";
    let expected = [
        "a.gopp:3:23: instantiation cycle:",
        "    a.gopp:3:41: U instantiated as *U",
    ];
    assert_eq!(messages("a.gopp", deep), expected);
    // A method with type parameters of its own is instantiated where it
    // is called, for the type arguments of the receiver too.
    let through_receiver = "package p
type List[T any] struct{ items []T }
func (l List[T]) Deep[U any]() {
	var x List[*T]
	x.Deep[U]()
}
";
    let expected = [
        "a.gopp:3:14: instantiation cycle:",
        "    a.gopp:5:2: T instantiated as *T",
    ];
    assert_eq!(messages("a.gopp", through_receiver), expected);
    let other_receiver = "package p
type List[T any] struct{ items []T }
func (l List[T]) Deep[U any]() {
	var x List[int]
	x.Deep[*U]()
}
";
    let expected = [
        "a.gopp:3:23: instantiation cycle:",
        "    a.gopp:5:9: U instantiated as *U",
    ];
    assert_eq!(messages("a.gopp", other_receiver), expected);
}

#[test]
fn the_steps_of_instantiation_cycles_grow_with_the_input_not_faster() {
    let steps = |text: String| {
        let loader = Loader::default();
        let checked = check_texts("p", &[("a.gopp", &text)], &loader, &ThreadedRodeo::new());
        assert_eq!(checked.messages, [] as [&str; 0]);
        checked.steps + loader.types.get().unwrap().steps()
    };
    // Each function instantiates the next with a type around its type
    // parameter, and the last one none: no cycle. Written last first,
    // each edge is recorded before the one that leads to it.
    let chain = |count: usize, forward: bool| {
        let function = |i: usize| match i + 1 == count {
            true => format!("func f{i}[T any]() {{}}\n"),
            false => format!("func f{i}[T any]() {{ f{}[*T]() }}\n", i + 1),
        };
        let all: String = match forward {
            true => (0..count).map(function).collect(),
            false => (0..count).rev().map(function).collect(),
        };
        format!("package p\n{all}")
    };
    // Rings of three types that name each other with their parameters
    // in another order: cycles of no weight.
    let rings = |count: usize| {
        let all: String = (0..count)
            .map(|i| {
                format!("type R{i}[A, B, C any] struct {{ l *R{i}[B, C, A]; r *R{i}[C, A, B] }}\n")
            })
            .collect();
        format!("package p\n{all}")
    };
    // Functions with a local type each, given on to the next function.
    let locals = |count: usize| {
        let function = |i: usize| match i + 1 == count {
            true => format!("func l{i}[T any]() {{}}\n"),
            false => format!(
                "func l{i}[T any]() {{ type L struct{{ x T }}; l{}[[]L]() }}\n",
                i + 1
            ),
        };
        let all: String = (0..count).map(function).collect();
        format!("package p\n{all}")
    };
    let sizes = [250, 500, 1_000, 2_000];
    linear::assert_near_linear(&sizes, |n| steps(chain(n, true)));
    linear::assert_near_linear(&sizes, |n| steps(chain(n, false)));
    linear::assert_near_linear(&sizes, |n| steps(rings(n)));
    linear::assert_near_linear(&sizes, |n| steps(locals(n)));
}

/// The methods that stand for operators in the first file, in the order
/// of the source: `line: what is written: Holder.Name(parameters) results`.
/// An entry says `op=` for a compound assignment, `store` for an index
/// expression that is assigned to, `case` for the value of a case, and
/// `right` for a method whose receiver is the operand on the right.
fn operator_uses(text: &str) -> Vec<String> {
    let names = ThreadedRodeo::new();
    let loader = Loader::default();
    let checked = check_texts("p", &[("a.gopp", text)], &loader, &names);
    assert_eq!(checked.messages, [] as [&str; 0]);
    let types = loader.types.get().unwrap();
    let tree = &checked.files[0];
    let mut found: Vec<(u32, u8, String)> = Vec::new();
    for body in &checked.package.files[0].bodies {
        let tables = [
            ("", &body.operators),
            ("op= ", &body.op_assigns),
            ("store ", &body.index_stores),
            ("case ", &body.case_operators),
        ];
        for (order, (kind, table)) in tables.into_iter().enumerate() {
            for (&expr, used) in table {
                let span = tree.exprs[expr].span;
                let line = text[..span.lo as usize].matches('\n').count() + 1;
                let written = &text[span.lo as usize..span.hi as usize];
                let show = |ty| types.display_in(ty, &names, names.get_or_intern("p"));
                let sig = show(used.sig);
                let right = if used.swapped { " right" } else { "" };
                let shown = format!(
                    "{line}: {kind}{written}: {}.{}{}{right}",
                    show(used.holder),
                    names.resolve(&used.name),
                    sig.strip_prefix("func").unwrap_or(&sig)
                );
                found.push((span.lo, order as u8, shown));
            }
        }
    }
    found.sort();
    found.into_iter().map(|(_, _, shown)| shown).collect()
}

// rule: EXP-16
#[test]
fn an_operator_method_has_the_form_of_its_operator() {
    let text = "package p

type V struct{ x int }

@Operator
func (a V) Add(b V, c V) V { return a }

@Operator
func (a V) Eq(b V) int { return 0 }

@Operator
func (a V) Cmp(b V) bool { return false }

@Operator
func (a *V) Sub(b V) V { return b }

@Operator
func (a V) MulAssign(b V) {}

@Operator
func (a V) SetIndex(i int, v int) {}

@Operator
func (a V) Iter() int { return 0 }

@Operator
func (a V) Mul[U any](b U) V { return a }

@Operator
func (a V) Div!(b V) V { return a }

@Operator
func (a V) Rem(b V) V { return a }

@Operator
func (a V) Rem(b V) V { return a }

func (a V) Neg() V { return a }

@Operator
func (a V) Neg() V { return a }

@Operator @Right
func (a V) BitAnd(b V) V { return a }

@Operator
func (a V) BitOr(b int) V { return a }

@Operator @Right
func (a V) BitOr(b int) V { return a }
";
    let expected = [
        "a.gopp:6:12: invalid @Operator method Add: its form is (b U) R",
        "a.gopp:9:12: invalid @Operator method Eq: its form is (b U) bool",
        "a.gopp:12:12: invalid @Operator method Cmp: its form is (b U) int",
        "a.gopp:15:13: invalid @Operator method Sub: it needs a value receiver: its operand may be \
         a temporary",
        "a.gopp:18:12: invalid @Operator method MulAssign: it needs a pointer receiver: it changes \
         its operand",
        "a.gopp:21:12: invalid @Operator method SetIndex: it needs a pointer receiver: it changes \
         its operand",
        "a.gopp:24:12: invalid @Operator method Iter: its form is () F, for a function type F that \
         range accepts",
        "a.gopp:27:12: invalid @Operator method Mul: an operator method has no type parameters of \
         its own",
        "a.gopp:30:12: invalid @Operator method Div: an operator method cannot fail",
        "a.gopp:36:12: method V.Rem already declared for these parameter types",
        "    a.gopp:33:12: other declaration of method Rem",
        "a.gopp:41:12: method V.Neg is declared with @Operator and without it",
        "    a.gopp:38:12: other declaration of method Neg",
        "a.gopp:44:12: invalid @Right: the parameter has the type of the receiver",
        "a.gopp:50:12: method V.BitOr already declared for these parameter types",
        "    a.gopp:47:12: other declaration of method BitOr",
    ];
    assert_eq!(messages("a.gopp", text), expected);

    // The forms that are right, and the names that only a plain method
    // has.
    let text = "package p

type V struct{ x int }

@Operator
func (a V) Add(b V) V { return a }

@Operator
func (a V) Add(b int) float64 { return 0 }

@Operator
func (a V) Shl(n int) V { return a }

@Operator @Right
func (a V) Shl(n int) V { return a }

@Operator
func (a V) Neg() V { return a }

@Operator
func (a V) Eq(b V) bool { return a.x == b.x }

@Operator
func (a V) Cmp(b V) int { return a.x - b.x }

@Operator
func (a *V) AddAssign(b V) { a.x += b.x }

@Operator
func (a V) Index(i, j int) int { return a.x }

@Operator
func (a *V) SetIndex(i, j int, v int) { a.x = v }

@Operator
func (a *V) Iter() func(yield func(int) bool) {
	return func(yield func(int) bool) { yield(a.x) }
}

func (a V) Plus(b V) V { return a }

type G[T any] struct{ items []T }

@Operator
func (g G[T]) Add(o G[T]) G[T] { return g }
";
    assert_eq!(messages("a.gopp", text), [] as [&str; 0]);
}

// rule: EXP-17
#[test]
fn a_type_has_no_second_meaning_of_what_go_gives_it() {
    let text = "package p

type Meters float64
type Seconds float64
type Speed float64

@Operator
func (a Meters) Add(b Meters) Meters { return a }

@Operator
func (a Meters) Div(b Seconds) Speed { return Speed(float64(a) / float64(b)) }

@Operator
func (a Meters) Neg() Meters { return a }

@Operator
func (a Meters) Eq(b Meters) bool { return true }

@Operator
func (a Meters) Cmp(b Meters) int { return 0 }

@Operator
func (a *Meters) SubAssign(b Meters) {}

@Operator
func (a Meters) Shl(n uint) Meters { return a }

type Bits uint8

@Operator
func (a Bits) Shl(n uint) Bits { return a }

@Operator
func (a Bits) BitNot() Bits { return a }

type List []int

@Operator
func (l List) Index(i int) int { return 0 }

@Operator
func (l *List) SetIndex(i int, v int) {}

@Operator
func (l List) Iter() func(func(int) bool) { return func(func(int) bool) {} }

type Grid [4]int

@Operator
func (g Grid) Eq(o Grid) bool { return true }

@Convert
func (a Meters) Float() float64 { return 0 }

@Convert
func (a Meters) Text() string { return \"\" }

@Convert
func (a Meters) Words() string { return \"\" }
";
    let expected = [
        "a.gopp:8:17: invalid @Operator method Add: Meters already has the operator +: its \
         underlying type float64 gives it",
        "a.gopp:14:17: invalid @Operator method Neg: Meters already has the operator -: its \
         underlying type float64 gives it",
        "a.gopp:17:17: invalid @Operator method Eq: Meters already has the operator ==: its \
         underlying type float64 gives it",
        "a.gopp:20:17: invalid @Operator method Cmp: Meters already has the operator <: its \
         underlying type float64 gives it",
        "a.gopp:23:18: invalid @Operator method SubAssign: Meters already has the operator -=: its \
         underlying type float64 gives it",
        "a.gopp:31:15: invalid @Operator method Shl: Bits already has the operator <<: its \
         underlying type uint8 gives it",
        "a.gopp:34:15: invalid @Operator method BitNot: Bits already has the operator ^: its \
         underlying type uint8 gives it",
        "a.gopp:39:15: invalid @Operator method Index: List already has the operator []: its \
         underlying type []int gives it",
        "a.gopp:42:16: invalid @Operator method SetIndex: List already has the operator []=: its \
         underlying type []int gives it",
        "a.gopp:45:15: invalid @Operator method Iter: List already has the operator range: its \
         underlying type []int gives it",
        "a.gopp:53:17: invalid @Convert method Float: a Meters converts to float64 already",
        "a.gopp:59:17: invalid @Convert method Words: Meters has a @Convert method to string \
         already",
        "    a.gopp:56:17: the other @Convert method",
    ];
    assert_eq!(messages("a.gopp", text), expected);
}

// rule: EXP-17
// rule: EXP-20
// rule: EXP-26
// rule: TYP-27
#[test]
fn an_operator_is_the_method_that_takes_the_operand() {
    // The example of ADR-0024.
    let declared = "package p

type Vec2 struct{ X, Y float64 }

@Operator
func (a Vec2) Add(b Vec2) Vec2 { return Vec2{a.X + b.X, a.Y + b.Y} }

@Operator
func (a Vec2) Sub(b Vec2) Vec2 { return Vec2{a.X - b.X, a.Y - b.Y} }

@Operator
func (a Vec2) Neg() Vec2 { return Vec2{-a.X, -a.Y} }

@Operator
func (a Vec2) Mul(b Vec2) Vec2 { return Vec2{a.X * b.X, a.Y * b.Y} }

@Operator @Right
func (a Vec2) Mul(s float64) Vec2 { return Vec2{a.X * s, a.Y * s} }

@Operator @Right
func (a Vec2) Div(s float64) Vec2 { return Vec2{s / a.X, s / a.Y} }

@Operator
func (a Vec2) Eq(b Vec2) bool { return a.X == b.X && a.Y == b.Y }

@Operator
func (a Vec2) Cmp(b Vec2) int { return 0 }
";
    let good = "
func use(a, b Vec2, p, q *Vec2, f float64) bool {
	c := a + b
	d := -a * 2.0
	e := 2.0 * a
	g := f * a
	h := a * f
	i := 1 / a
	j := p + q
	k := -p - a
	same := p == q
	_, _, _, _, _, _, _, _ = c, d, e, g, h, i, j, k
	switch a {
	case b, c:
	}
	return a < b || a >= c || a == b || a != c || same
}
";
    let expected = [
        "30: a + b: Vec2.Add(Vec2) Vec2",
        "31: -a * 2.0: Vec2.Mul(float64) Vec2",
        "31: -a: Vec2.Neg() Vec2",
        "32: 2.0 * a: Vec2.Mul(float64) Vec2 right",
        "33: f * a: Vec2.Mul(float64) Vec2 right",
        "34: a * f: Vec2.Mul(float64) Vec2",
        "35: 1 / a: Vec2.Div(float64) Vec2 right",
        "36: p + q: Vec2.Add(Vec2) Vec2",
        "37: -p - a: Vec2.Sub(Vec2) Vec2",
        "37: -p: Vec2.Neg() Vec2",
        "41: case b: Vec2.Eq(Vec2) bool",
        "41: case c: Vec2.Eq(Vec2) bool",
        "43: a < b: Vec2.Cmp(Vec2) int",
        "43: a >= c: Vec2.Cmp(Vec2) int",
        "43: a == b: Vec2.Eq(Vec2) bool",
        "43: a != c: Vec2.Eq(Vec2) bool",
    ];
    assert_eq!(operator_uses(&format!("{declared}{good}")), expected);

    let bad = "
type Other struct{}

func use(a Vec2, o Other, n int) {
	_ = a / 2.0
	_ = a + 1
	_ = a + n
	_ = n + a
	_ = a + o
	_ = o + a
	_ = !a
	_ = a << 2
	_ = a < o
	a++
}
";
    let expected = [
        "a.gopp:32:6: invalid operation: a / 2.0 (Vec2 has no operator / (@Operator Div) for \
         float64)",
        "a.gopp:33:6: invalid operation: a + 1 (Vec2 has no operator + (@Operator Add) for int)",
        "a.gopp:34:6: invalid operation: a + n (Vec2 has no operator + (@Operator Add) for int)",
        "a.gopp:35:6: invalid operation: n + a (the @Operator Add methods of Vec2 take a Vec2 as \
         the left operand)",
        "    declare an @Operator @Right Add method for the other order",
        "a.gopp:36:6: invalid operation: a + o (Vec2 has no operator + (@Operator Add) for Other)",
        "a.gopp:37:6: invalid operation: o + a (the @Operator Add methods of Vec2 take a Vec2 as \
         the left operand)",
        "    declare an @Operator @Right Add method for the other order",
        "a.gopp:38:7: invalid operation: operator ! not defined on a (variable of struct type \
         Vec2)",
        "a.gopp:39:6: invalid operation: shifted operand a (variable of struct type Vec2) must be \
         integer",
        "a.gopp:40:6: invalid operation: a < o (Vec2 has no operator < (@Operator Cmp) for Other)",
        "a.gopp:41:2: invalid operation: a++ (non-numeric type Vec2)",
    ];
    assert_eq!(messages("a.gopp", &format!("{declared}{bad}")), expected);
}

// rule: EXP-17
#[test]
fn a_constant_takes_the_method_of_its_default_type_or_the_only_one_that_holds_it() {
    let text = "package p

type Meters float64
type Seconds float64
type Speed float64
type Km int

@Operator
func (a Meters) Mul(b Seconds) Speed { return 0 }

@Operator
func (a Meters) Div(b int) Meters { return a }

@Operator
func (a Meters) Div(b float64) Speed { return 0 }

type Small struct{}

@Operator
func (a Small) Add(b int8) Small { return a }

@Operator
func (a Small) Add(b int16) Small { return a }

@Operator
func (a Small) Sub(b int8) Small { return a }

@Operator
func (a Small) Sub(b string) Small { return a }

@Operator
func (a Small) Eq(b string) bool { return true }

type Named struct{ n int }

@Operator
func (a Named) Eq(b int) bool { return a.n == b }

func use(m Meters, s Seconds, x Small, n, o Named, any1 any) {
	var speed Speed = m * s
	var still Meters = m * m
	var twice Meters = 2 * m
	var half Meters = m / 2
	var slow Speed = m / 2.5
	x = x - 1
	x = x - \"a\"
	_ = x == \"a\"
	_ = n == 3
	_ = n == o
	_ = n == any1
	_ = any1 == n
	_, _, _, _, _ = speed, still, twice, half, slow
}
";
    let expected = [
        "40: m * s: Meters.Mul(Seconds) Speed",
        "43: m / 2: Meters.Div(int) Meters",
        "44: m / 2.5: Meters.Div(float64) Speed",
        "45: x - 1: Small.Sub(int8) Small",
        "46: x - \"a\": Small.Sub(string) Small",
        "47: x == \"a\": Small.Eq(string) bool",
        "48: n == 3: Named.Eq(int) bool",
    ];
    assert_eq!(operator_uses(text), expected);

    let bad = text.replace("x = x - 1\n", "x = x + 1\n\tx = x - 1.5\n\t_ = 3 == n\n");
    let expected = [
        "a.gopp:45:6: invalid operation: x + 1 (more than one @Operator Add method takes these \
         operands)",
        "a.gopp:46:6: invalid operation: x - 1.5 (Small has no operator - (@Operator Sub) for \
         float64)",
        "a.gopp:47:6: invalid operation: 3 == n (the @Operator Eq methods of Named take a Named as \
         the left operand)",
        "    declare an @Operator @Right Eq method for the other order",
    ];
    assert_eq!(messages("a.gopp", &bad), expected);
}

// rule: EXP-19
// rule: EXP-22
// rule: EXP-25
// rule: RNG-6b
#[test]
fn a_method_changes_a_place_indexes_ranges_and_converts() {
    let declared = "package p

type Matrix struct {
	rows, cols int
	data       []float64
}

@Operator
func (m *Matrix) Index(r, c int) float64 { return m.data[r*m.cols+c] }

@Operator
func (m *Matrix) Index(name string) *float64 { return &m.data[0] }

@Operator
func (m *Matrix) SetIndex(r, c int, v float64) { m.data[r*m.cols+c] = v }

@Operator
func (m *Matrix) AddAssign(o Matrix) {}

@Operator
func (m Matrix) Add(o Matrix) Matrix { return m }

@Operator
func (m Matrix) Sub(o Matrix) Matrix { return m }

@Operator
func (m Matrix) Mul(o Matrix) float64 { return 0 }

@Operator
func (m *Matrix) Iter() func(yield func(int, float64) bool) {
	return func(yield func(int, float64) bool) {}
}

@Convert
func (m Matrix) Size() int { return m.rows * m.cols }

type Cells struct{ at map[string]Matrix }

func open() Matrix { return Matrix{} }
";
    let good = "
func use(m, n Matrix, pm *Matrix, cells Cells) float64 {
	m[0, 1] = 2.5
	m[1, 1] += 1
	x := m[0, 1] + pm[1, 0]
	pm[0, 0] = x
	x = m[\"first\"]
	m += n
	m -= n
	pm += n
	cells.at[\"a\"] += n
	for i, v := range m {
		_, _ = i, v
	}
	for i := range pm {
		_ = i
	}
	return float64(int(m)) + float64(m.Size())
}
";
    let expected = [
        "42: store m[0, 1]: Matrix.SetIndex(int, int, float64)",
        "43: m[1, 1]: Matrix.Index(int, int) float64",
        "43: store m[1, 1]: Matrix.SetIndex(int, int, float64)",
        "44: m[0, 1]: Matrix.Index(int, int) float64",
        "44: pm[1, 0]: Matrix.Index(int, int) float64",
        "45: store pm[0, 0]: Matrix.SetIndex(int, int, float64)",
        "46: m[\"first\"]: Matrix.Index(string) *float64",
        "47: op= m: Matrix.AddAssign(Matrix)",
        "48: op= m: Matrix.Sub(Matrix) Matrix",
        "49: op= pm: Matrix.AddAssign(Matrix)",
        "50: op= cells.at[\"a\"]: Matrix.Add(Matrix) Matrix",
        "51: m: Matrix.Iter() func(func(int, float64) bool)",
        "54: pm: Matrix.Iter() func(func(int, float64) bool)",
        "57: int(m): Matrix.Size() int",
    ];
    assert_eq!(operator_uses(&format!("{declared}{good}")), expected);

    let bad = "
func use(m, n Matrix, cells Cells) {
	m += m
	m.data[0] = 1
	m += open()
	m *= n
	p := &m[0, 0]
	open()[0, 0] = 1
	_ = open()[0, 0]
	_ = m[0]
	m[0, 0, 0] = 1
	m[\"first\"] = 3
	var k int = m
	var s string = string(m)
	for range open() {
	}
	_, _, _ = p, k, s
}

type Pair struct{ a, b int }
type Pairs struct{ all []Pair }

@Operator
func (p Pairs) Index(i int) Pair { return p.all[i] }

func change(p Pairs) {
	p[0].a = 1
	p[0] = Pair{}
}
";
    let expected = [
        "a.gopp:42:2: invalid operation: m += m (@Operator AddAssign changes m while it reads m)",
        "    write m = m + m",
        "a.gopp:45:2: invalid operation: m *= n (@Operator Mul gives a float64, which is no value \
         of the type Matrix of m)",
        "a.gopp:46:8: invalid operation: cannot take address of m[0, 0]: what @Operator Index \
         gives is no place",
        "a.gopp:47:2: invalid operation: open()[0, 0] (@Operator SetIndex has a pointer receiver, \
         and open() is no place)",
        "a.gopp:48:6: invalid operation: open()[0, 0] (@Operator Index has a pointer receiver, and \
         open() is no place)",
        "a.gopp:49:6: invalid operation: m[0] (Matrix has no @Operator Index method for int)",
        "a.gopp:50:2: invalid operation: m[0, 0, 0] (Matrix has no @Operator SetIndex method for \
         int, int, int)",
        "a.gopp:51:2: invalid operation: m[\"first\"] (Matrix has no @Operator SetIndex method for \
         string)",
        "a.gopp:52:14: cannot use m (variable of struct type Matrix) as int value in variable \
         declaration",
        "    write int(m)",
        "a.gopp:53:24: cannot convert m (variable of struct type Matrix) to type string",
        "a.gopp:54:12: cannot range over open() (value of struct type Matrix): @Operator Iter has \
         a pointer receiver, and it is no place",
        "a.gopp:66:2: cannot assign to p[0].a: what @Operator Index gives is no place",
        "a.gopp:67:2: invalid operation: p[0] (Pairs has no @Operator SetIndex method for int)",
    ];
    assert_eq!(messages("a.gopp", &format!("{declared}{bad}")), expected);
}

// rule: GEN-13
#[test]
fn a_constraint_lists_operators_that_the_types_of_go_have_too() {
    let declared = "package p

type Vec2 struct{ X, Y float64 }

@Operator
func (a Vec2) Add(b Vec2) Vec2 { return a }

@Operator
func (a Vec2) Mul(b Vec2) Vec2 { return a }

@Operator @Right
func (a Vec2) Mul(s float64) Vec2 { return a }

@Operator @Right
func (a Vec2) Div(s float64) Vec2 { return a }

@Operator
func (a *Vec2) SubAssign(b Vec2) {}

type Table struct{ rows []int }

@Operator
func (t *Table) Index(i int) int { return t.rows[i] }

@Operator
func (t *Table) SetIndex(i int, v int) { t.rows[i] = v }

type Named struct{ s []int }

func Sum[T interface {
	Add(T) T
	Clone
}](xs []T, zero T) T {
	acc := zero
	for _, x := range xs {
		acc = acc + x
		acc += x
	}
	return acc.Add(zero)
}

func Scale[T interface{ Mul(float64) T }](x T) T { return x * 2 }
func Halve[T interface{ Div(float64) T }](x T) T { return x / 2 }
func Same[T interface{ Eq(T) bool }](a, b T) bool { return a == b || a != b }
func Less[T interface{ Cmp(T) int }](a, b T) bool { return a < b || a >= b }
func Minus[T interface{ Neg() T }](a T) T { return -a }
func Drop[T interface{ SubAssign(T) }](a *T, b T) { a -= move(b) }
func Shift[T interface{ Shl(uint) T }](a T) T { return a << 2 }
func At[S interface{ Index(int) E }, E any](s S) E { return s[0] }
func Put[S interface{ SetIndex(int, E) }, E Copy](s S, v E) { s[0] = v }
";
    let good = "
func use(v Vec2, xs []int, arr [3]int, t Table) {
	_ = Sum([]int{1}, 0)
	_ = Sum([]float64{1}, 0)
	_ = Sum([]string{\"a\"}, \"\")
	_ = Sum([]Vec2{v}, v)
	_ = Scale(v)
	_ = Scale(1.5)
	_ = Same(1, 2)
	_ = Same(v, v)
	_ = Same(&v, &v)
	_ = Less(\"a\", \"b\")
	_ = Less(1.5, 2)
	_ = Minus(3)
	_ = Shift(uint8(1))
	n := 1
	Drop(&n, 2)
	Drop(&v, v)
	_ = At(xs)
	_ = At(arr)
	_ = At[string](\"x\")
	_ = At(&xs)
	_ = At(&t)
	Put(&xs, 1)
	Put(&arr, 1)
	Put(&t, 1)
}
";
    assert_eq!(
        messages("a.gopp", &format!("{declared}{good}")),
        [] as [&str; 0]
    );

    let bad = "
func Any[T any](a, b T) bool { return a + b == a }
func Other[T interface{ Add(T) T }](a T) bool { return a - a == a }

func use(v Vec2, xs []int, n Named, t Table) {
	_ = Sum([]bool{true}, false)
	_ = Scale(1)
	_ = Halve(v)
	_ = Same(n, n)
	_ = Less(v, v)
	_ = Less(1i, 2i)
	_ = Minus(\"a\")
	_ = Shift(1.5)
	Put(xs, 1)
	Put(t, 1)
	_ = At(t)
	_ = Sum([]*Vec2{&v}, &v)
}
";
    let expected = [
        "a.gopp:52:39: invalid operation: operator + not defined on a (variable of type T \
         constrained by any)",
        "a.gopp:53:56: invalid operation: operator - not defined on a (variable of type T \
         constrained by interface{Add(T) T})",
        "a.gopp:56:6: in call to Sum, T (type bool) does not satisfy interface{Add(T) T; Clone} \
         (missing method Add)",
        "a.gopp:57:6: int does not satisfy interface{Mul(float64) int} (missing method Mul)",
        "a.gopp:58:6: in call to Halve, T (type Vec2) does not satisfy interface{Div(float64) T} \
         (missing method Div)",
        "a.gopp:59:6: in call to Same, T (type Named) does not satisfy interface{Eq(T) bool} \
         (missing method Eq)",
        "a.gopp:60:6: in call to Less, T (type Vec2) does not satisfy interface{Cmp(T) int} \
         (missing method Cmp)",
        "a.gopp:61:6: complex128 does not satisfy interface{Cmp(complex128) int} (missing method \
         Cmp)",
        "a.gopp:62:6: string does not satisfy interface{Neg() string} (missing method Neg)",
        "a.gopp:63:6: float64 does not satisfy interface{Shl(uint) float64} (missing method Shl)",
        "a.gopp:64:2: in call to Put, S (type []int) does not satisfy interface{SetIndex(int, E)} \
         (missing method SetIndex)",
        "a.gopp:65:2: in call to Put, S (type Table) does not satisfy interface{SetIndex(int, E)} \
         (method SetIndex has pointer receiver)",
        "a.gopp:66:6: in call to At, S (type Table) does not satisfy interface{Index(int) E} \
         (method Index has pointer receiver)",
        "a.gopp:67:6: in call to Sum, T (type *Vec2) does not satisfy interface{Add(T) T; Clone} \
         (wrong type for method Add)",
        "    have Add(Vec2) Vec2",
        "    want Add(T) T",
    ];
    assert_eq!(messages("a.gopp", &format!("{declared}{bad}")), expected);
}

// rule: EXP-18
// rule: EXP-30
#[test]
fn an_operator_method_is_called_by_its_name_and_inherited() {
    let declared = "package p

type Big struct{ digits []uint32 }

func (b *Big) drop() {}

@Operator
func (a Big) Add(b Big) Big { return Big{} }

@Operator
func (a Big) Add(n int) Big { return Big{} }

@Operator @Right
func (a Big) Sub(n int) Big { return Big{} }

@Operator
func (a Big) Sub(b Big) Big { return Big{} }

@Operator
func (a Big) Eq(b Big) bool { return len(a.digits) == len(b.digits) }

type Base struct{ n int }

@Operator
func (b Base) Eq(o Base) bool { return b.n == o.n }

@Operator
func (b Base) Add(o Base) Base { return b }

@Extends(Base)
type Kid struct{ extra int }

@Operator
func (k Kid) Add(o Kid) Kid { return k }
";
    // The operands are read where they are, also those that are not
    // copied.
    let good = "
func use(a, b Big, k, k2 Kid, e Base) bool {
	c := a + b
	d := a + 1
	f := a.Add(b)
	g := a.Add(2)
	h := 3 - a
	i := a.Sub(b)
	_, _, _, _, _, _ = c, d, f, g, h, i
	_ = k + k2
	_ = e + e
	return a == b && e == e
}
";
    let expected = [
        "37: a + b: Big.Add(Big) Big",
        "38: a + 1: Big.Add(int) Big",
        "39: a.Add(b): Big.Add(Big) Big",
        "40: a.Add(2): Big.Add(int) Big",
        "41: 3 - a: Big.Sub(int) Big right",
        "42: a.Sub(b): Big.Sub(Big) Big",
        "44: k + k2: Kid.Add(Kid) Kid",
        "45: e + e: Base.Add(Base) Base",
        "46: a == b: Big.Eq(Big) bool",
        "46: e == e: Base.Eq(Base) bool",
    ];
    assert_eq!(operator_uses(&format!("{declared}{good}")), expected);

    let bad = "
func use(a Big, k, k2 Kid, e Base) {
	_ = a.Add(\"x\")
	_ = a.Sub(1)
	f := a.Add
	_ = f
	_ = k == k2
	_ = k + e
	_ = e + k
}
";
    let expected = [
        "a.gopp:37:6: no @Operator Add method of Big takes (string)",
        "a.gopp:38:6: no @Operator Sub method of Big takes (int)",
        "a.gopp:39:7: a.Add names more than one @Operator method: only a call says which",
        "a.gopp:39:7: cannot copy a (variable of struct type Big): Big has a drop method and no \
         Clone() Big",
        "    use move(a) to give the value, or &a to lend it",
        "a.gopp:41:6: invalid operation: k == k2 (Kid has no operator == (@Operator Eq) for Kid)",
        "    define @Operator Eq on Kid, or compare the parent parts",
        "a.gopp:43:6: invalid operation: e + k (Base has no operator + (@Operator Add) for Kid)",
    ];
    assert_eq!(messages("a.gopp", &format!("{declared}{bad}")), expected);
}

#[test]
fn the_steps_of_operators_grow_with_the_input_not_faster() {
    let steps = |text: String| {
        let loader = Loader::default();
        let checked = check_texts("p", &[("a.gopp", &text)], &loader, &ThreadedRodeo::new());
        assert_eq!(checked.messages, [] as [&str; 0]);
        checked.steps + loader.types.get().unwrap().steps()
    };
    // Many types, each with a few operator methods and uses of all the
    // forms.
    let types = |count: usize| {
        let all: String = (0..count)
            .map(|i| {
                format!(
                    "type V{i} struct{{ x int }}\n\n\
                     @Operator\nfunc (a V{i}) Add(b V{i}) V{i} {{ return a }}\n\n\
                     @Operator\nfunc (a V{i}) Add(b int) V{i} {{ return a }}\n\n\
                     @Operator @Right\nfunc (a V{i}) Mul(b float64) V{i} {{ return a }}\n\n\
                     @Operator\nfunc (a V{i}) Eq(b V{i}) bool {{ return a.x == b.x }}\n\n\
                     @Operator\nfunc (a *V{i}) SubAssign(b V{i}) {{}}\n\n\
                     @Operator\nfunc (a V{i}) Index(k int) int {{ return a.x }}\n\n\
                     func use{i}(a, b V{i}) bool {{\n\
                     \tc := a + b + 1\n\tc = 2.5 * c\n\tc -= a\n\t_ = c[0] + c.Add(3)[1]\n\
                     \treturn a == b\n}}\n"
                )
            })
            .collect();
        format!("package p\n{all}")
    };
    // One type and many uses of its operators in one function.
    let uses = |count: usize| {
        let all: String = (0..count)
            .map(|i| format!("\tc = c + a*{i}.5 + b\n\tc -= a\n"))
            .collect();
        format!(
            "package p\ntype V struct{{ x int }}\n\n\
             @Operator\nfunc (a V) Add(b V) V {{ return a }}\n\n\
             @Operator @Right\nfunc (a V) Mul(b float64) V {{ return a }}\n\n\
             @Operator\nfunc (a *V) SubAssign(b V) {{}}\n\n\
             func use(a, b V) V {{\n\tc := a\n{all}\treturn c\n}}\n"
        )
    };
    // A generic function over operator elements, instantiated for many
    // types.
    let instances = |count: usize| {
        let all: String = (0..count)
            .map(|i| {
                format!(
                    "type N{i} int\nvar _ = Sum([]N{i}{{1}})\ntype S{i} struct{{}}\n\n\
                     @Operator\nfunc (a S{i}) Add(b S{i}) S{i} {{ return a }}\n\n\
                     var _ = Sum([]S{i}{{}})\n"
                )
            })
            .collect();
        format!(
            "package p\nfunc Sum[T interface {{\n\tAdd(T) T\n\tCopy\n}}](xs []T) T \
             {{ return xs[0] + xs[0] }}\n{all}"
        )
    };
    let sizes = [250, 500, 1_000, 2_000];
    linear::assert_near_linear(&sizes, |n| steps(types(n)));
    linear::assert_near_linear(&sizes, |n| steps(uses(n)));
    linear::assert_near_linear(&sizes, |n| steps(instances(n)));
}

/// The constants of the package: `name type = value`, in the order of
/// their declarations.
fn constants(checked: &Checked, loader: &Loader, names: &ThreadedRodeo) -> Vec<String> {
    let types = loader.types.get().unwrap();
    let consts = checked.package.objects.iter();
    consts
        .filter(|object| matches!(object.kind, ObjKind::Const))
        .map(|object| {
            let ty = types.display_in(object.ty, names, names.get_or_intern("p"));
            format!("{} {ty} = {}", names.resolve(&object.name), object.value)
        })
        .collect()
}

// rule: EXP-31
#[test]
fn a_constant_is_made_of_constants() {
    let text = "package p

type Vec2 struct{ X, Y float64 }
type Line struct {
	From, To Vec2
	Name     string
}
type Pair struct{ a, b int }

const Origin = Vec2{0, 0}
const Unit = Vec2{X: 1, Y: 1}
const Half Vec2 = Vec2{Y: 0.5}
const Diag = Line{Origin, Unit, \"diag\"}
const X = Unit.X
const Name = Diag.Name
const ToY = Diag.To.Y
const Primes = [4]int{2, 3, 5, 7}
const Sparse = [...]int8{3: 1, 2, 0: 9}
const Third = Primes[2]
const Count = len(Primes) + len(Sparse)
const Letter = \"abc\"[1]
const None Option[int] = nil
const Some Option[int] = 3
const Pairs = [2]Pair{{1, 2}, {b: 4}}
const Last = Pairs[1].b
const Inner = struct{ n [2]bool }{[2]bool{true}}

var sized [Third + Last]int

func use(i int) float64 {
	const local = Vec2{2, 3}
	var grid [len(Pairs)][Primes[0]]int
	_ = grid
	return local.X + X + float64(Third) + float64(Letter) + ToY + float64(Primes[i])
}
";
    let names = ThreadedRodeo::new();
    let loader = Loader::default();
    let checked = check_texts("p", &[("a.gopp", text)], &loader, &names);
    assert_eq!(checked.messages, [] as [&str; 0]);
    let expected = [
        "Origin Vec2 = {0, 0}",
        "Unit Vec2 = {1, 1}",
        "Half Vec2 = {0, 0.5}",
        "Diag Line = {{0, 0}, {1, 1}, \"diag\"}",
        "X float64 = 1",
        "Name string = \"diag\"",
        "ToY float64 = 1",
        "Primes [4]int = {2, 3, 5, 7}",
        "Sparse [5]int8 = {9, 0, 0, 1, 2}",
        "Third int = 5",
        "Count int = 9",
        "Letter byte = 98",
        "None Option[int] = nil",
        "Some Option[int] = 3",
        "Pairs [2]Pair = {{1, 2}, {0, 4}}",
        "Last int = 4",
        "Inner struct{n [2]bool} = {{true, false}}",
    ];
    assert_eq!(constants(&checked, &loader, &names), expected);

    // The types that a constant does not have, and what a constant is
    // not: assigned, or the owner of an address.
    let bad = "package p

type Vec2 struct{ X, Y float64 }
type Buf struct{ n int }

func (b *Buf) drop() {}

@Pinned
type Pin struct{ n int }

type Root struct{ n int }

@Virtual
func (r *Root) M() {}

const A [2]*int = [2]*int{}
const B []int = nil
const C map[string]int = nil
const D Buf = Buf{}
const E Pin = Pin{}
const F Root = Root{}
const G struct{ f func() } = struct{ f func() }{}
const H any = 1
const I Option[Unique[int]] = nil
const J = []int{1}
var v = 3.5
const K = Vec2{v, 0}
const L Vec2 = nil

const Origin = Vec2{0, 0}
const Primes = [4]int{2, 3, 5, 7}

func use() {
	Origin.X = 1
	Primes[0] = 1
	p := &Origin
	q := &Primes[0]
	_, _ = p, q
	_ = Primes[4]
}
";
    let expected = [
        "a.gopp:16:9: invalid constant type [2]*int: a *int names a value of the running program",
        "a.gopp:17:9: invalid constant type []int: the buffer of a []int is made by the running \
         program",
        "a.gopp:18:9: invalid constant type map[string]int: the buffer of a map[string]int is made \
         by the running program",
        "a.gopp:19:9: invalid constant type Buf: Buf has a drop method",
        "a.gopp:20:9: invalid constant type Pin: Pin is @Pinned",
        "a.gopp:21:9: invalid constant type Root: Root has a type table",
        "a.gopp:22:9: invalid constant type struct{f func()}: func() is a func type",
        "a.gopp:23:9: invalid constant type any: any is an interface type",
        "a.gopp:24:9: invalid constant type Option[Unique[int]]: a Unique[int] names a value of \
         the running program",
        "a.gopp:25:11: []int{…} (value of type []int) is not constant",
        "a.gopp:27:11: Vec2{…} (value of struct type Vec2) is not constant",
        "a.gopp:28:16: nil is not constant",
        "a.gopp:34:2: cannot assign to Origin.X (neither addressable nor a map index expression)",
        "a.gopp:35:2: cannot assign to Primes[0] (neither addressable nor a map index expression)",
        "a.gopp:36:8: invalid operation: cannot take address of Origin (constant {0, 0} of struct \
         type Vec2)",
        "a.gopp:37:8: invalid operation: cannot take address of Primes[0] (constant 2 of type int)",
        "a.gopp:39:13: invalid argument: index 4 out of bounds [0:4]",
    ];
    assert_eq!(messages("a.gopp", bad), expected);
}

/// Runs the functions `double`, `Twice`, `table` and `fail`, and the
/// method `Add` of a struct of two numbers, as a `@Const` evaluator would.
struct Evaluator {
    double: Spur,
    twice: Spur,
    table: Spur,
    fail: Spur,
    add: Spur,
}

impl crate::ConstEvaluator for Evaluator {
    fn call(&self, call: &crate::ConstCall<'_>) -> Result<Value, crate::ConstFailure> {
        use crate::constant::{BinaryOp, Composite};
        let composite = |parts: Composite| Value::Composite(Arc::new(parts));
        let name = call.name;
        if name == self.double || name == self.twice {
            return Ok(call.args[0].binary(BinaryOp::Add, &call.args[0]));
        }
        if name == self.table {
            let squares = (0..4).map(|i| Value::Int64(i * i)).collect();
            return Ok(composite(Composite::Array(squares)));
        }
        if name == self.add {
            let fields = |value: &Value| match value {
                Value::Composite(parts) => match &**parts {
                    Composite::Struct(fields) => fields.clone(),
                    _ => Vec::new(),
                },
                _ => Vec::new(),
            };
            let (a, b) = (fields(&call.args[0]), fields(&call.args[1]));
            let sums = a.iter().zip(&b).map(|(a, b)| a.binary(BinaryOp::Add, b));
            return Ok(composite(Composite::Struct(sums.collect())));
        }
        assert_eq!(name, self.fail);
        Err(crate::ConstFailure {
            message: "panic: no value".to_owned(),
            stack: vec!["fail".to_owned(), "outer".to_owned()],
        })
    }
}

// rule: EXP-32
// rule: EXP-34
#[test]
fn a_const_function_is_called_in_a_constant_expression() {
    let text = "package p

import \"lib\"

type Vec2 struct{ X, Y int }

@Const @Operator
func (a Vec2) Add(b Vec2) Vec2 { return Vec2{a.X + b.X, a.Y + b.Y} }

@Const
func double(n int) int { return n + n }

@Const
func table() [4]int { return [4]int{0, 1, 4, 9} }

const Four = double(2)
const Eight = double(Four)
const Squares = table()
const Nine = Squares[3]
const Sum = Vec2{1, 2} + Vec2{10, 20}
const SumX = Sum.X
const Far = lib.Twice(4)

var sized [Eight + Nine + SumX]int
var direct [double(2)]int

func use(n int) int {
	const local = double(21)
	return double(n) + local
}
";
    let lib = "package lib\n\n@Const\nfunc Twice(n int) int { return 2 * n }\n\
               func Plain() int { return 1 }\n";
    let names = ThreadedRodeo::new();
    let mut loader = with_packages(&names, &[("lib", lib)]);
    // Without what runs them, the calls are constants of their types.
    let checked = check_texts("p", &[("a.gopp", text)], &loader, &names);
    assert_eq!(checked.messages, [] as [&str; 0]);
    let expected = [
        "Four int = unknown",
        "Eight int = unknown",
        "Squares [4]int = unknown",
        "Nine int = unknown",
        "Sum Vec2 = unknown",
        "SumX int = unknown",
        "Far int = unknown",
    ];
    assert_eq!(constants(&checked, &loader, &names), expected);

    loader.evaluator = Some(Box::new(Evaluator {
        double: names.get_or_intern("double"),
        twice: names.get_or_intern("Twice"),
        table: names.get_or_intern("table"),
        fail: names.get_or_intern("fail"),
        add: names.get_or_intern("Add"),
    }));
    let checked = check_texts("p", &[("a.gopp", text)], &loader, &names);
    assert_eq!(checked.messages, [] as [&str; 0]);
    let expected = [
        "Four int = 4",
        "Eight int = 8",
        "Squares [4]int = {0, 1, 4, 9}",
        "Nine int = 9",
        "Sum Vec2 = {11, 22}",
        "SumX int = 11",
        "Far int = 8",
    ];
    assert_eq!(constants(&checked, &loader, &names), expected);

    let bad = "package p

import \"lib\"

type Vec2 struct{ X, Y int }

@Operator
func (a Vec2) Add(b Vec2) Vec2 { return a }

func (a Vec2) Len() int { return a.X }

@Const
func fail(n int) int { panic(\"no value\") }

@Const
func slice() []int { return []int{1} }

func plain() int { return 1 }

var v = 2

const A = plain()
const B = lib.Plain()
const C = Vec2{1, 2} + Vec2{3, 4}
const D = Vec2{1, 2}.Len()
const E = fail(1)
const F = fail(v)
const G = slice()
const H = func() int { return 1 }()
var sized [plain()]int
";
    let checked = check_texts("p", &[("a.gopp", bad)], &loader, &names);
    let expected = [
        "a.gopp:22:11: cannot call non-@Const function plain in constant expression",
        "a.gopp:23:11: cannot call non-@Const function lib.Plain in constant expression",
        "a.gopp:24:11: cannot call non-@Const method Vec2.Add in constant expression",
        "a.gopp:25:11: cannot call non-@Const function Vec2{…}.Len in constant expression",
        "a.gopp:26:11: constant expression fails: panic: no value",
        "    in fail",
        "    in outer",
        "a.gopp:27:11: fail(v) (value of type int) is not constant",
        "a.gopp:28:11: invalid constant type []int: the buffer of a []int is made by the running \
         program",
        "a.gopp:29:11: cannot call non-@Const function (func() int literal) in constant expression",
        "a.gopp:30:12: cannot call non-@Const function plain in constant expression",
    ];
    assert_eq!(checked.messages, expected);
}

// rule: EXP-33
#[test]
fn the_body_of_a_const_function_does_what_a_constant_expression_may() {
    let declared = "package p

import \"unsafe\"

type Buf struct{ n int }

func (b *Buf) drop() {}

type Shape interface{ Area() int }

var counter int

func plain(x int) int { return x }

@Const
func pure(x int) int { return x + 1 }

type V struct{ x int }

@Operator
func (a V) Add(b V) V { return a }

@Const @Operator
func (a V) Sub(b V) V { return a }

func (a V) Plain() int { return a.x }

@Const
func (a V) Pure() int { return a.x }

const Limit = 4
";
    let bad = "
@Const
func bad(s Shape, f func() int, v V, ch chan int) int {
	counter++
	x := counter + plain(1) + pure(2)
	x += s.Area() + f() + v.Plain() + v.Pure()
	w := v + v - v
	m := map[string]int{}
	go pure(1)
	defer pure(1)
	select {}
	p := NewUnique(V{})
	var b Buf
	_ = unsafe.Sizeof(x)
	_ = recover()
	println(x)
	_, _, _, _ = w, m, p, b
	return x
}

@Const
func (v V) Count() int { return counter }
";
    let names = ThreadedRodeo::new();
    let stub = "package unsafe\nfunc Sizeof(x any) uintptr\n";
    let loader = with_packages(&names, &[("unsafe", stub)]);
    let text = format!("{declared}{bad}");
    let checked = check_texts("p", &[("a.gopp", &text)], &loader, &names);
    let expected = [
        "a.gopp:34:41: invalid @Const function bad: it uses a channel",
        "a.gopp:35:2: invalid @Const function bad: it uses the variable counter",
        "a.gopp:36:7: invalid @Const function bad: it uses the variable counter",
        "a.gopp:36:17: invalid @Const function bad: it calls plain, which is not @Const",
        "a.gopp:37:7: invalid @Const function bad: it calls a method of an interface",
        "a.gopp:37:18: invalid @Const function bad: it calls a func value",
        "a.gopp:37:24: invalid @Const function bad: it calls v.Plain, which is not @Const",
        "a.gopp:38:7: invalid @Const function bad: it uses the operator method V.Add, which is not \
         @Const",
        "a.gopp:39:7: invalid @Const function bad: it uses a map",
        "a.gopp:40:2: invalid @Const function bad: it has a go statement",
        "a.gopp:41:2: invalid @Const function bad: it has a defer statement",
        "a.gopp:42:2: invalid @Const function bad: it has a select statement",
        "a.gopp:43:7: invalid @Const function bad: it calls NewUnique",
        "a.gopp:44:8: invalid @Const function bad: it uses Buf, which has a drop method",
        "a.gopp:45:6: invalid @Const function bad: it uses package unsafe",
        "a.gopp:46:6: invalid @Const function bad: it calls recover",
        "a.gopp:47:2: invalid @Const function bad: it calls println",
        "a.gopp:53:33: invalid @Const function Count: it uses the variable counter",
    ];
    assert_eq!(checked.messages, expected);

    let good = "
@Const
func good(n int, v V) [Limit]int {
	var t [Limit]int
	for i := range t {
		t[i] = pure(i)*n + Limit
	}
	q := &t
	q[0] = v.Pure() + (v - v).x
	xs := make([]int, 3)
	xs = append(xs, len(xs), cap(xs), min(1, 2))
	clear(xs)
	var o Option[int] = 3
	if held, ok := o.Get(); ok && !o.IsNil() {
		t[1] += held
	}
	switch {
	case n > 3:
		return good(n-1, v)
	case n < 0:
		panic(\"negative\")
	}
	return t
}

@Const
func generic[T interface {
	Add(T) T
	Copy
}](a T) T {
	return a + a.Add(a)
}

func free() int { return counter + plain(1) }
";
    let text = format!("{declared}{good}").replace("import \"unsafe\"\n", "");
    assert_eq!(messages("a.gopp", &text), [] as [&str; 0]);
}

#[test]
fn the_steps_of_constants_grow_with_the_input_not_faster() {
    let steps = |text: String| {
        let loader = Loader::default();
        let checked = check_texts("p", &[("a.gopp", &text)], &loader, &ThreadedRodeo::new());
        assert_eq!(checked.messages, [] as [&str; 0]);
        checked.steps + loader.types.get().unwrap().steps()
    };
    // Many types, each with a constant made of the one before, a field
    // of it, and a `@Const` function that reads it.
    let chain = |count: usize| {
        let all: String = (0..count)
            .map(|i| {
                let inner = match i {
                    0 => "int".to_owned(),
                    _ => format!("T{}", i - 1),
                };
                let value = match i {
                    0 => "1".to_owned(),
                    _ => format!("C{}", i - 1),
                };
                format!(
                    "type T{i} struct{{ n int; in {inner} }}\nconst C{i} = T{i}{{{i}, {value}}}\n\
                     const N{i} = C{i}.n\n\n@Const\nfunc f{i}(k int) int {{ return k + N{i} + \
                     C{i}.n }}\n"
                )
            })
            .collect();
        format!("package p\n{all}")
    };
    // One array constant and many constants that are elements of it.
    let elements = |count: usize| {
        let all: String = (0..count)
            .map(|i| format!("const E{i} = Table[{}]\n", i % 8))
            .collect();
        format!("package p\nconst Table = [8]int{{1, 2, 3, 4, 5, 6, 7, 8}}\n{all}")
    };
    let sizes = [250, 500, 1_000, 2_000];
    linear::assert_near_linear(&sizes, |n| steps(chain(n)));
    linear::assert_near_linear(&sizes, |n| steps(elements(n)));
}

/// A header as what reads headers would give it: a few C functions,
/// variables, a constant and types, and C++ templates.
#[derive(Debug, Default)]
struct Header {
    file: std::sync::OnceLock<crate::types::TypeId>,
    /// How often each hook was asked.
    asked: std::sync::atomic::AtomicU64,
}

impl crate::ForeignPackage for Header {
    fn member(
        &self,
        name: &str,
        types: &Types,
        names: &ThreadedRodeo,
    ) -> Option<crate::ForeignMember> {
        use crate::ForeignMember::*;
        use crate::basic::Basic;
        self.asked
            .fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        let int32 = types.basic(Basic::Int32);
        // What reads the header says what a `const char*` is in Go++.
        let chars = types.basic(Basic::String);
        let numbers = types.pointer(int32);
        Some(match name {
            "puts" => Func {
                sig: types.func(&[chars], &[int32], false),
                read_only: vec![true],
            },
            "sum" => Func {
                sig: types.func(&[numbers, int32], &[int32], false),
                read_only: vec![true, false],
            },
            "fill" => Func {
                sig: types.func(&[numbers, int32], &[], false),
                read_only: vec![false, false],
            },
            "parse" => Func {
                sig: types.func(&[chars], &[int32], false),
                read_only: vec![true],
            },
            "errno" => Var {
                ty: int32,
                read_only: false,
            },
            "version" => Var {
                ty: int32,
                read_only: true,
            },
            "BUFSIZ" => Const {
                ty: int32,
                value: Value::Int64(8192),
            },
            "type_" => Type(int32),
            "FILE" => Type(*self.file.get_or_init(|| {
                let (name, package) = (names.get_or_intern("FILE"), names.get_or_intern("stdio"));
                let named = types.declare(name, package);
                types.set_underlying(named, int32);
                types.named(named, &[])
            })),
            "max" | "get" | "vector" | "pi_v" => Template,
            _ => return None,
        })
    }

    fn instance(
        &self,
        name: &str,
        explicit: &[crate::TemplateArg],
        args: Option<&[crate::types::TypeId]>,
        types: &Types,
        _: &ThreadedRodeo,
    ) -> Result<crate::ForeignMember, String> {
        use crate::ForeignMember::*;
        use crate::TemplateArg;
        use crate::basic::Basic;
        self.asked
            .fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        let int32 = types.basic(Basic::Int32);
        let first = |what: &str| match explicit.first() {
            Some(TemplateArg::Type(ty)) => Ok(*ty),
            Some(TemplateArg::Const(..)) => Err(format!("{what} takes a type")),
            None => Err(format!("{what} needs a template argument")),
        };
        match name {
            // `template<class T> T max(T, T)`.
            "max" => {
                // A constant without a type takes the type of the others.
                let untyped = |ty: &&crate::types::TypeId| types.default_type(**ty) != **ty;
                let given = args.unwrap_or_default();
                let typed: Vec<_> = given.iter().filter(|ty| !untyped(ty)).copied().collect();
                let of = match (explicit.first(), typed.first(), given.first()) {
                    (Some(_), ..) => first("max")?,
                    (None, Some(&ty), _) => ty,
                    (None, None, Some(&ty)) => types.default_type(ty),
                    (None, None, None) => return Err("no argument gives T".to_owned()),
                };
                if typed.iter().any(|&ty| !types.identical(ty, of)) {
                    return Err("the arguments give T two types".to_owned());
                }
                Ok(Func {
                    sig: types.func(&[of, of], &[of], false),
                    read_only: vec![false, false],
                })
            }
            // `template<size_t I> int32_t get(int32_t)`.
            "get" => match explicit.first() {
                Some(TemplateArg::Const(value, _)) if value.sign() >= 0 => Ok(Func {
                    sig: types.func(&[int32], &[int32], false),
                    read_only: vec![false],
                }),
                Some(TemplateArg::Const(value, _)) => {
                    Err(format!("{value} is no value of the template parameter I"))
                }
                _ => Err("get needs the template argument I".to_owned()),
            },
            "vector" => Ok(Type(types.slice(first("vector")?))),
            "pi_v" => Ok(Var {
                ty: first("pi_v")?,
                read_only: true,
            }),
            // `template<class T> T parse(const char*)`, next to the plain
            // function of that name.
            "parse" => Ok(Func {
                sig: types.func(&[types.basic(Basic::String)], &[first("parse")?], false),
                read_only: vec![true],
            }),
            _ => Err("it is no template".to_owned()),
        }
    }
}

/// A loader with the headers `<stdio.h>` and `<algorithm>`, both read by
/// one `Header`.
fn with_headers() -> (Loader, Arc<Header>) {
    let header = Arc::new(Header::default());
    let mut loader = Loader::default();
    for path in ["<stdio.h>", "<algorithm>"] {
        let package = Package::header(path, Arc::clone(&header) as Arc<dyn crate::ForeignPackage>);
        loader.packages.insert(path.to_owned(), Arc::new(package));
    }
    (loader, header)
}

// rule: FOR-3
#[test]
fn a_header_is_a_package_whose_names_are_asked() {
    let text = "package p

import (
	\"<algorithm>\"
	\"<stdio.h>\"
)

type Config struct{ level int32 }

func use(s Shared[Config], xs *[4]int32, n int32, file *stdio.FILE) int32 {
	stdio.puts(\"hi\")
	var buf [stdio.BUFSIZ]byte
	var kind stdio.type_ = 1
	stdio.errno = 0
	total := stdio.sum(&xs[0], 4) + stdio.errno + stdio.version + stdio.sum(&s.level, 1)
	stdio.fill(&xs[0], 4)
	a := algorithm.max(n, 2)
	b := algorithm.max[int32](1, 2)
	c := algorithm.get[1](n)
	var v algorithm.vector[int32]
	pi := algorithm.pi_v[float32]
	d := algorithm.parse(\"1\") + int32(algorithm.parse[float64](\"2\"))
	_, _, _, _ = buf, kind, file, pi
	return total + a + b + c + d + int32(len(v)) + stdio.puts(\"again\")
}
";
    let names = ThreadedRodeo::new();
    let (loader, header) = with_headers();
    let checked = check_texts("p", &[("a.gopp", text)], &loader, &names);
    assert_eq!(checked.messages, [] as [&str; 0]);
    // What an expression names in a header is kept for the lowering.
    let types = loader.types.get().unwrap();
    let tree = &checked.files[0];
    let bodies = checked.package.files[0].bodies.iter();
    let mut uses: Vec<(u32, String)> = bodies
        .flat_map(|body| &body.foreign_uses)
        .map(|(&expr, used)| {
            let span = tree.exprs[expr].span;
            let line = text[..span.lo as usize].matches('\n').count() + 1;
            let explicit = match used.explicit.len() {
                0 => String::new(),
                count => format!(" [{count}]"),
            };
            let shown = format!(
                "{line}: {}{explicit}: {}",
                names.resolve(&used.name),
                types.display(used.ty, &names)
            );
            (span.lo, shown)
        })
        .collect();
    uses.sort();
    let uses: Vec<String> = uses.into_iter().map(|(_, shown)| shown).collect();
    let expected = [
        "11: puts: func(string) int32",
        "14: errno: int32",
        "15: sum: func(*int32, int32) int32",
        "15: errno: int32",
        "15: version: int32",
        "15: sum: func(*int32, int32) int32",
        "16: fill: func(*int32, int32)",
        "17: max: func(int32, int32) int32",
        "18: max [1]: func(int32, int32) int32",
        "19: get [1]: func(int32) int32",
        "21: pi_v [1]: float32",
        "22: parse: func(string) int32",
        "22: parse [1]: func(string) float64",
        "24: puts: func(string) int32",
    ];
    assert_eq!(uses, expected);
    // Each name of a header is asked once, and each instance where it
    // is written.
    let asked = header.asked.load(std::sync::atomic::Ordering::Relaxed);
    assert_eq!(asked, 13 + 6);

    let bad = "package p

import (
	\"<algorithm>\"
	\"<stdio.h>\"
)

type Config struct{ level int32 }

func use(s Shared[Config], n int32) {
	stdio.nope()
	var x stdio.Nope
	stdio.version = 2
	_ = algorithm.max
	f := algorithm.max[int32]
	_ = algorithm.get[-1](n)
	_ = algorithm.get[n](n)
	_ = algorithm.max(n, \"x\")
	var w algorithm.vector
	var y algorithm.max[int32]
	var z stdio.BUFSIZ
	_ = stdio.puts[int32](\"x\")
	stdio.fill(&s.level, 1)
	_ = stdio.BUFSIZ[0]
	_, _, _, _, _ = x, f, w, y, z
}
";
    let (loader, _) = with_headers();
    let checked = check_texts("p", &[("a.gopp", bad)], &loader, &names);
    let expected = [
        "a.gopp:11:8: undefined: stdio.nope",
        "a.gopp:12:14: undefined: stdio.Nope",
        "a.gopp:13:2: cannot assign to stdio.version (neither addressable nor a map index \
         expression)",
        "a.gopp:14:6: cannot use algorithm.max without its template arguments: it is a template",
        "a.gopp:15:7: cannot use algorithm.max[int32] as a value: an instance of a function \
         template is only called",
        "    call it in a function literal",
        "a.gopp:16:6: cannot call algorithm.get[-1]: -1 is no value of the template parameter I",
        "a.gopp:17:20: invalid template argument n (variable of type int32): it is no type and no \
         constant",
        "a.gopp:18:23: cannot use \"x\" (untyped string constant) as int32 value in argument to \
         algorithm.max",
        "a.gopp:19:8: cannot use algorithm.vector without its template arguments",
        "a.gopp:20:8: algorithm.max[int32] is not a type",
        "a.gopp:21:8: stdio.BUFSIZ is not a type",
        "a.gopp:22:6: cannot call stdio.puts[int32]: it is no template",
        "a.gopp:23:13: cannot give &s.level to stdio.fill: the function may write through it, and \
         what a Shared holds is only read",
        "a.gopp:24:6: cannot index stdio.BUFSIZ (constant 8192 of type int32)",
    ];
    assert_eq!(checked.messages, expected);
}

#[test]
fn the_steps_of_header_names_grow_with_the_uses_not_faster() {
    let steps = |text: String| {
        let (loader, _) = with_headers();
        let checked = check_texts("p", &[("a.gopp", &text)], &loader, &ThreadedRodeo::new());
        assert_eq!(checked.messages, [] as [&str; 0]);
        checked.steps + loader.types.get().unwrap().steps()
    };
    let uses = |count: usize| {
        let all: String = (0..count)
            .map(|i| {
                format!(
                    "func f{i}(n int32) int32 {{\n\tvar v algorithm.vector[int32]\n\
                     \treturn algorithm.max(n, {i}) + algorithm.get[{i}](n) + stdio.puts(\"x\") + \
                     int32(len(v)) + stdio.BUFSIZ\n}}\n"
                )
            })
            .collect();
        format!("package p\n\nimport (\n\t\"<algorithm>\"\n\t\"<stdio.h>\"\n)\n\n{all}")
    };
    linear::assert_near_linear(&[250, 500, 1_000, 2_000], |n| steps(uses(n)));
}

/// A package that is large enough for several workers, with what the
/// workers must give back in the order of the source: errors, types
/// that only a body makes, calls of generic functions, operators, and
/// variables that need functions.
fn large_package(functions: usize) -> String {
    let mut text = "package p

type V struct{ x int }

@Operator
func (a V) Add(b V) V { return V{a.x + b.x} }

type Box[T Clone] struct{ v T }

func (b Box[T]) Get() T { return b.v }

func pair[A, B Clone](a A, b B) struct {
	a A
	b B
} {
	return struct {
		a A
		b B
	}{a, b}
}
"
    .to_owned();
    for i in 0..functions {
        let before = i.saturating_sub(1);
        text += &format!(
            "
var g{i} = f{before}(V{{{i}}}, V{{1}}, nil)

func f{i}(a, b V, xs []int) int {{
	type L{i} struct {{
		n    int
		next Option[*L{i}]
	}}
	type Pairs = map[string][{len}]L{i}
	c := a + b
	local := L{i}{{n: c.x}}
	boxed := Box[[]L{i}]{{v: []L{i}{{local}}}}
	total := len(boxed.Get()) + pair(c.x, \"s{i}\").a
	seen := Pairs{{}}
	for at, x := range xs {{
		if x > {i} {{
			total += x*at + len(seen)
		}}
	}}
	add := func(n int) int {{ return n + total }}
	var o Option[int] = add(total)
	if v, ok := o.Get(); ok {{
		return v + f{before}(c, a, xs)
	}}
	return total
}}
",
            len = i % 7 + 1
        );
        // Some bodies are wrong, each in its own way.
        match i % 9 {
            3 => {
                text += &format!("\nfunc wrong{i}() string {{ return f{i}(V{{}}, V{{}}, nil) }}\n")
            }
            6 => text += &format!("\nfunc wrong{i}(v V) {{ _ = v - v; _ = missing{i} }}\n"),
            _ => {}
        }
    }
    text
}

#[test]
fn the_bodies_of_a_package_are_checked_side_by_side_with_the_same_result() {
    let text = large_package(400);
    // Enough source for several workers.
    assert!(text.len() > 3 * 64 * 1024, "{}", text.len());
    let checked_with = |threads: usize| {
        let pool = rayon::ThreadPoolBuilder::new()
            .num_threads(threads)
            .build()
            .unwrap();
        pool.install(|| {
            let names = ThreadedRodeo::new();
            let loader = Loader::default();
            let checked = check_texts("p", &[("a.gopp", &text)], &loader, &names);
            let source = checked.map.file(checked.files[0].id);
            let files = [(&checked.files[0], &*source)];
            let types = loader.types.get().unwrap();
            let info = crate::info_text(&checked.package, &files, types, &names);
            let order: Vec<Vec<String>> = checked
                .package
                .init_order
                .iter()
                .map(|step| {
                    let name = |&object| names.resolve(&checked.package.object(object).name);
                    step.lhs.iter().map(name).map(str::to_owned).collect()
                })
                .collect();
            (checked.messages, info, order)
        })
    };
    let alone = checked_with(1);
    assert_eq!(alone.0.len(), 133, "{:?}", &alone.0[..4]);
    assert!(alone.1.contains("operator p.V.Add"), "{}", &alone.1[..2000]);
    assert_eq!(alone.2.len(), 400);
    for threads in [2, 5, 16] {
        let together = checked_with(threads);
        assert_eq!(together.0, alone.0, "{threads} threads");
        assert!(
            together.1 == alone.1,
            "{threads} threads: the tables differ"
        );
        assert_eq!(together.2, alone.2, "{threads} threads");
    }
}

#[test]
fn what_go_refuses_in_these_places_is_refused() {
    let text = "package p

import \"unsafe\"

const (
	_, _ = 1, 2
	_, _
	_
)

type RHS[P any] P

type (
	Sized [len(Sized{})]int
	Keyed map[[len(Keyed{1: 2})]int]int
	Self  struct{ f *[unsafe.Sizeof(Self{})]int }
)

type I interface{ ~int }
type C interface{ comparable }

func _(I)
func _() I
func _(_ comparable, _ C)
func _[P any]()
func external(x int) int

func _[T any](map[T]int) {}
func keys[T comparable](m map[T]int) {}

func local[P any]() {
	type (
		_[Q any] interface{ Q }
		_        interface{ int | ~P }
		_        struct{ ok int }
	)
	const _ P = 1
}

func uses(a []int) {
	_ = nil == nil
	_ = nil != nil
	_ = uses[0]
	_ = len[0]
	_ = *len
	var f func() []int
	_ = f[0]
	_ = f()[0]
	_ = a[::]
}

func dup[P int | float64 | complex128]() {
	_ = map[P]int{1: 1, 1.0: 2, 1 + 0i: 3, 2: 4}
}
";
    let found = with_unsafe_text("a.gopp", text);
    let expected = [
        "a.gopp:8:2: extra init expr at the declaration that is repeated",
        "a.gopp:11:17: cannot use a type parameter as RHS in type declaration",
        "a.gopp:14:13: invalid recursive type",
        "a.gopp:15:17: invalid recursive type",
        "a.gopp:16:34: invalid recursive type",
        "a.gopp:22:8: cannot use type I outside a type constraint: interface contains type constraints",
        "a.gopp:23:10: cannot use type I outside a type constraint: interface contains type constraints",
        "a.gopp:24:10: cannot use type comparable outside a type constraint: interface is (or embeds) comparable",
        "a.gopp:24:24: cannot use type C outside a type constraint: interface is (or embeds) comparable",
        "a.gopp:28:19: invalid map key type T (missing comparable constraint)",
        "a.gopp:33:23: term cannot be a type parameter",
        "a.gopp:34:30: type in term ~P cannot be a type parameter",
        "a.gopp:37:10: invalid constant type P: P is a type parameter",
        "a.gopp:41:13: invalid operation: nil == nil (operator == not defined on untyped nil)",
        "a.gopp:42:13: invalid operation: nil != nil (operator != not defined on untyped nil)",
        "a.gopp:43:6: cannot index uses (value of type func([]int))",
        "a.gopp:44:6: cannot index len (built-in)",
        "a.gopp:45:7: invalid operation: cannot indirect len (built-in)",
        "a.gopp:46:6: cannot declare f without a value: func() []int has no zero value (a func() []int is never nil)",
        "    give f a value, or declare it as Option[func() []int]",
        "a.gopp:47:6: cannot index f (variable of type func() []int)",
        "a.gopp:49:8: middle index required in 3-index slice",
        "a.gopp:49:9: final index required in 3-index slice",
        "a.gopp:53:22: duplicate key 1 in map literal",
        "a.gopp:53:30: duplicate key (1 + 0i) in map literal",
    ];
    assert_eq!(found, expected);
}

// rule: OWN-R3a
#[test]
fn the_steps_of_places_grow_with_the_input_not_faster() {
    let steps = |text: String| {
        let checked = check_texts(
            "p",
            &[("a.gopp", &text)],
            &Loader::default(),
            &ThreadedRodeo::new(),
        );
        assert_eq!(checked.messages, [] as [&str; 0]);
        checked.steps
    };
    let header = "package p\ntype Doc struct{ Title string; Tags []string; Next *Doc }\n\
                  func show(d Doc) {}\n";
    // Many uses that give a value, and many assignments over a place.
    let siblings = |count: usize| {
        let uses: String = (0..count)
            .map(|i| format!("\tshow(d)\n\tt{i} := docs[{i}].Title\n\tdocs[{i}].Tags = append(docs[{i}].Tags, t{i})\n"))
            .collect();
        format!("{header}func f(d Doc, docs []Doc) []Doc {{\n{uses}\treturn docs\n}}\n")
    };
    // One place under many fields, each in a struct of its own.
    let deep = |depth: usize| {
        let types: String = (0..depth)
            .map(|i| format!("type T{i} struct{{ f T{} }}\n", i + 1))
            .collect();
        let path = ".f".repeat(depth);
        format!(
            "{header}{types}type T{depth} struct{{ f Doc }}\n\
             func f(v T0, vs []T0) Doc {{\n\tv{path}.f.Title = \"x\"\n\tfor _, &e := range vs {{ \
             e{path}.f.Tags[0] = \"x\" }}\n\tshow(v{path}.f)\n\treturn built(){path}.f\n}}\n\
             func built() T0 {{ panic(0) }}\n"
        )
    };
    // One assignment that exchanges many places.
    let swap = |count: usize| {
        let left: Vec<String> = (0..count).map(|i| format!("docs[{i}]")).collect();
        let right: Vec<String> = (0..count).rev().map(|i| format!("docs[{i}]")).collect();
        format!(
            "{header}func f(docs []Doc) []Doc {{\n\t{} = {}\n\treturn docs\n}}\n",
            left.join(", "),
            right.join(", ")
        )
    };
    // A function literal in a function literal, each with a variable of
    // the one around it.
    let nested = |depth: usize| {
        let open: String = (0..depth)
            .map(|i| format!("d{} := d{i}\n_ = func() {{ show(d{})\n", i + 1, i + 1))
            .collect();
        format!(
            "{header}func f(d0 Doc) {{\n{open}{}\n}}\n",
            "}\n".repeat(depth)
        )
    };
    let sizes = [250, 500, 1_000, 2_000];
    linear::assert_near_linear(&sizes, |n| steps(siblings(n)));
    linear::assert_near_linear(&sizes, |n| steps(deep(n)));
    linear::assert_near_linear(&sizes, |n| steps(swap(n)));
    // The parser takes no deeper nesting.
    linear::assert_near_linear(&[50, 100, 200, 400], |n| steps(nested(n)));
}

/// Calls of a generic function build a type that holds another one twice
/// at every level, with no alias to name it by.
#[test]
fn a_type_built_by_generic_calls_is_written_out_in_part() {
    let build = |count: usize, last: &str| {
        let calls: String = (1..=count)
            .map(|i| format!("\ta{i} := dup(a{})\n", i - 1))
            .collect();
        format!(
            "package p\nfunc dup[T Clone](x T) struct{{ a, b T }} {{ return struct{{ a, b T }}{{x, x}} }}\n\
             func _() {{\n\ta0 := 1\n{calls}\t_ = a{count} == a{count}\n{last}}}\n"
        )
    };
    let check = |text: String| {
        let loader = Loader::default();
        let checked = check_texts("p", &[("a.gopp", &text)], &loader, &ThreadedRodeo::new());
        let steps = checked.steps + loader.types.get().unwrap().steps();
        (checked.messages, steps)
    };
    let (messages, steps) = check(build(200, ""));
    assert_eq!(messages, [] as [&str; 0]);
    assert!(steps < 20_000_000, "{steps} steps");
    let (messages, _) = check(build(40, "\tvar _ int = a40\n"));
    let [message] = &messages[..] else {
        panic!("{} messages", messages.len());
    };
    assert!(
        message.starts_with(
            "a.gopp:46:14: cannot use a40 (variable of type struct{a struct{a struct{a"
        )
    );
    assert!(message.ends_with("b …}; b …}) as int value in variable declaration"));
    assert!(message.len() < 100_000, "{} bytes", message.len());
}

#[test]
fn a_message_names_a_type_by_the_alias_it_was_written_with() {
    let text = "package p

func f[P any](x P) P { return x }

type T0 struct{}
type A10 = **T0
type Ai = int
type Pair = struct{ a, b Ai }

func (A10) m1() {}
func (Ai) m2()  {}

func _(p Pair) {
	type A = int
	var a A
	b := f(a)
	_ = b + \"foo\"
	var e interface{}
	var t any
	var _ float32 = e
	var _ float32 = t
	var by byte
	var _ string = by
	var u8 uint8
	var _ string = u8
	var _ string = \"x\"[0]
	for _, r := range \"x\" {
		var _ string = r
	}
	x := 'x'
	var _ string = x
	var bs []byte
	var _ string = bs
	var _ int = p
	_ = p.c
	var _ Ai = \"x\"
	var _ []byte = []uint8{}
	var _ struct{ a, b int } = p
}
";
    let expected = [
        "a.gopp:10:7: invalid receiver type A10",
        "a.gopp:11:7: cannot define new methods on non-local type Ai",
        "a.gopp:17:6: invalid operation: b + \"foo\" (mismatched types A and untyped string)",
        "a.gopp:20:18: cannot use e (variable of type interface{}) as float32 value in variable declaration: need type assertion",
        "a.gopp:21:18: cannot use t (variable of interface type any) as float32 value in variable declaration: need type assertion",
        "a.gopp:23:17: cannot use by (variable of type byte) as string value in variable declaration",
        "a.gopp:25:17: cannot use u8 (variable of type uint8) as string value in variable declaration",
        "a.gopp:26:17: cannot use \"x\"[0] (constant 120 of type byte) as string value in variable declaration",
        "a.gopp:28:18: cannot use r (variable of type rune) as string value in variable declaration",
        "a.gopp:31:17: cannot use x (variable of type rune) as string value in variable declaration",
        "a.gopp:33:17: cannot use bs (variable of type []byte) as string value in variable declaration",
        "a.gopp:34:14: cannot use p (variable of struct type Pair) as int value in variable declaration",
        "a.gopp:35:8: p.c undefined (type Pair has no field or method c)",
        "a.gopp:36:13: cannot use \"x\" (untyped string constant) as Ai value in variable declaration",
    ];
    assert_eq!(messages("a.gopp", text), expected);
}

/// Types that are nested, or hold declared types that hold others, a
/// thousand levels deep, with what a program does with a type. `first`
/// declares `S0` and `next` the type `S<i>` from `S<i-1>`.
fn deep_types(depth: usize, first: &str, next: &dyn Fn(usize) -> String, last: &str) -> String {
    let all: String = (1..=depth).map(next).collect();
    format!(
        "package p\nimport \"unsafe\"\n{first}\n{all}type T = S{depth}{last}\n\
         var v, w T\nvar _ = unsafe.Sizeof(v)\nvar _ any = v\nvar _ = any(v).(T)\n\
         func id[P any](x P) P {{ return x }}\nvar _ = id(v)\n\
         func _() {{ var u T = v; _ = u; _ = []T{{v}}; _ = &v; _ = func(x T) T {{ return x }}(w) }}\n\
         type W struct{{ t T }}\nvar _ = W{{}}\n\
         var _ int = v\nvar _ interface{{ none() }} = v\nvar _ = v.nothing\n"
    )
}

/// The walks over a type do not need a stack as deep as the type.
#[test]
fn a_deep_type_does_not_overflow_the_stack() {
    const DEPTH: usize = 600;
    let compared = "var _ = v == w\nvar _ = map[T]int{}\nfunc cmp[P comparable](x P) {}\n\
                    func _() { cmp(v) }\n";
    let shapes: Vec<String> = vec![
        deep_types(
            DEPTH,
            "type S0 struct{ x int }",
            &|i| format!("type S{i} struct{{ s S{} }}\n", i - 1),
            "",
        ) + compared,
        deep_types(
            DEPTH,
            "type S0 = int",
            &|i| format!("type S{i} = struct{{ s S{} }}\n", i - 1),
            "",
        ) + compared,
        deep_types(
            DEPTH,
            "type S0 = int",
            &|i| format!("type S{i} = [1]S{}\n", i - 1),
            "",
        ) + compared,
        deep_types(
            DEPTH,
            "type S0 = int",
            &|i| format!("type S{i} = *S{}\n", i - 1),
            "",
        ) + compared,
        // Each of these holds the one before it twice.
        deep_types(
            DEPTH,
            "type S0 = int",
            &|i| format!("type S{i} = func(S{}) S{}\n", i - 1, i - 1),
            "",
        ),
        deep_types(
            DEPTH,
            "type S0 interface{ m0() }",
            &|i| format!("type S{i} interface{{ S{}; m{i}() }}\n", i - 1),
            "",
        ) + compared,
        deep_types(
            DEPTH,
            "type Box[P any] struct{ x P }\ntype S0 = int",
            &|i| format!("type S{i} = Box[S{}]\n", i - 1),
            "",
        ) + compared,
        deep_types(
            DEPTH,
            "type S0[P any] = [1]P",
            &|i| format!("type S{i}[P any] = struct{{ s S{}[P] }}\n", i - 1),
            "[int]",
        ) + compared,
    ];
    // On a stack that a walk as deep as the types would overflow.
    let checking = std::thread::Builder::new().stack_size(192 * 1024);
    let checked = checking.spawn(move || {
        for text in shapes {
            let names = ThreadedRodeo::new();
            let stub = "package unsafe\nfunc Sizeof(x any) uintptr\n";
            let loader = with_packages(&names, &[("unsafe", stub)]);
            let checked = check_texts("p", &[("a.gopp", &text)], &loader, &names);
            // The last of the errors that every shape has.
            let messages = checked.messages;
            assert!(
                messages.iter().any(|m| m.contains("v.nothing undefined")),
                "{messages:?}"
            );
        }
    });
    checked.unwrap().join().unwrap();
}

#[test]
fn type_parameters_that_are_found_from_each_other_are_not_found() {
    // Each of the two would be replaced by a type with the other in it
    // without end.
    let text = "package p
func f[P, Q interface{ func(Q) P }]() {}
func g[P interface{ *Q }, Q interface{ func(P) }]() {}
func _() {
	f()
	g()
}
";
    let expected = [
        "a.gopp:5:2: in call to f, cannot infer P",
        "    a.gopp:2:8: P declared here",
        "a.gopp:6:2: in call to g, cannot infer P",
        "    a.gopp:3:8: P declared here",
    ];
    assert_eq!(messages("a.gopp", text), expected);
}

/// A type that holds the one before it twice is as large as two to the
/// number of such types when it is written out, and is walked once.
#[test]
fn a_type_that_stands_in_many_places_is_walked_once() {
    let steps = |text: String| {
        let names = ThreadedRodeo::new();
        let stub = "package unsafe\nfunc Sizeof(x any) uintptr\n";
        let loader = with_packages(&names, &[("unsafe", stub)]);
        let checked = check_texts("p", &[("a.gopp", &text)], &loader, &names);
        assert_eq!(checked.messages, [] as [&str; 0]);
        checked.steps + loader.types.get().unwrap().steps()
    };
    let chain = |count: usize, first: &str, next: &dyn Fn(usize) -> String| {
        let all: String = (1..=count).map(next).collect();
        format!("package p\nimport \"unsafe\"\n{first}\n{all}")
    };
    let aliases = |count: usize| {
        // Of no size, so that the last one has a size that can be told.
        let types = chain(count, "type A0 = [0]int", &|i| {
            format!("type A{i} = struct{{ a, b A{} }}\n", i - 1)
        });
        format!(
            "{types}var v, w A{count}\nvar _ = v == w\nvar _ = map[A{count}]int{{}}\n\
             var _ = unsafe.Sizeof(v)\nvar _ = any(v).(A{count})\n\
             type T struct{{ t A{count} }}\nvar _ = T{{}}.t.a.b\nvar _ = [2]A{count}{{v, w}}\n\
             func f[P any](x struct{{ a, b P }}) {{}}\nfunc g[P comparable](x P) {{}}\n\
             func _() {{ f(v); g(v) }}\n\
             var _ = struct{{ a, b A{} `x:\"y\"` }}(v)\n",
            count - 1
        )
    };
    let generic_types = |count: usize| {
        let types = chain(count, "type B0[P any] struct{ x P }", &|i| {
            format!("type B{i}[P any] struct{{ a, b B{}[P] }}\n", i - 1)
        });
        format!(
            "{types}var b B{count}[[0]int]\nvar _ = b == b\nvar _ = b.a.b\n\
             var _ = unsafe.Sizeof(b)\nvar _ = map[B{count}[string]]int{{}}\n"
        )
    };
    let generic_aliases = |count: usize| {
        let types = chain(count, "type C0[P any] = [0]P", &|i| {
            format!("type C{i}[P any] = struct{{ a, b C{}[P] }}\n", i - 1)
        });
        format!(
            "{types}var c C{count}[int]\nvar _ = c == c\nvar _ = c.a.b\n\
             var _ = unsafe.Sizeof(c)\n\
             type D[P any] struct{{ c C{count}[P] }}\nvar _ D[string]\n"
        )
    };
    let sizes = [20, 40, 80, 160];
    linear::assert_near_linear(&sizes, |n| steps(aliases(n)));
    linear::assert_near_linear(&sizes, |n| steps(generic_types(n)));
    // A generic alias is written out again for each alias that holds
    // it: the square of the number of aliases, not two to that number.
    let most = steps(generic_aliases(160));
    assert!(most < 100_000, "{most} steps");
    // A type argument is found through such aliases, as deep as
    // unification goes.
    let call = "func f[P Clone](x C40[P]) C38[P] { return x.a.b }\nvar _ C38[int] = f(c)\n";
    let found = steps(generic_aliases(40) + call);
    assert!(found < 100_000, "{found} steps");
}

/// What a generic alias stands for is written out with the type arguments
/// of each use, so a chain of aliases that each hold the one before costs
/// the square of its length; the chains here name the one before and add
/// nothing.
#[test]
fn the_steps_of_generic_aliases_grow_with_the_input_not_faster() {
    let steps = |text: String| {
        let loader = Loader::default();
        let checked = check_texts("p", &[("a.gopp", &text)], &loader, &ThreadedRodeo::new());
        assert_eq!(checked.messages, [] as [&str; 0]);
        checked.steps + loader.types.get().unwrap().steps()
    };
    let siblings = |count: usize| {
        let all: String = (0..count)
            .map(|i| format!("type A{i}[P any] = map[string][]P\nvar v{i} A{i}[int]\n"))
            .collect();
        format!("package p\n{all}")
    };
    // Each alias stands for the one before it.
    let chain = |count: usize| {
        let all: String = (1..count)
            .map(|i| format!("type A{i}[P any] = A{}[P]\nvar v{i} A{i}[int]\n", i - 1))
            .collect();
        format!("package p\ntype A0[P any] = []P\n{all}")
    };
    // Generic types and aliases inside a function, each used by the next.
    let local = |count: usize| {
        let all: String = (1..count)
            .map(|i| {
                let before = i - 1;
                format!(
                    "type T{i}[Q any] struct {{ next Option[*T{i}[Q]]; before Option[*T{before}[Q]] }}\n\
                     type A{i}[Q any] = Option[*T{i}[Q]]\nvar v{i} A{i}[P]\n_ = v{i}\n"
                )
            })
            .collect();
        format!(
            "package p\nfunc f[P any]() {{\ntype T0[Q any] struct{{ next Option[*T0[Q]] }}\n{all}}}\n"
        )
    };
    let sizes = [250, 500, 1_000, 2_000];
    linear::assert_near_linear(&sizes, |n| steps(siblings(n)));
    linear::assert_near_linear(&sizes, |n| steps(chain(n)));
    linear::assert_near_linear(&sizes, |n| steps(local(n)));
}

// rule: GEN-6, GEN-7, GEN-8, GEN-9, GEN-10
#[test]
fn a_method_may_have_type_parameters_of_its_own() {
    let text = "package p

type Set struct{ items []int }

func (s Set) Map[U any](f func(int) U) []U {
	var out []U
	for _, x := range s.items {
		out = append(out, f(x))
	}
	return out
}

func (s Set) Sum[N ~int | ~float64](zero N) N { return zero }

type List[T any] struct{ items []T }

func (l List[T]) Conv[U any](f func(T) U) List[U] { return List[U]{} }

type Mapper interface{ Map(f func(int) string) []string }

func use(s Set, l List[int]) {
	a := s.Map(func(x int) string { return \"\" })
	b := s.Map[bool](func(x int) bool { return true })
	var _ []string = a
	var _ []bool = b
	var _ float64 = s.Sum(1.5)
	_ = s.Sum(\"x\")
	_ = s.Sum[string](\"x\")
	c := l.Conv(func(x int) string { return \"\" })
	var _ List[string] = c
	var _ Mapper = s
	_ = s.Map
}

func (s Set) Parse[T any]!(text string) T {
	panic(text)
}

func parse!(s Set) int {
	n := s.Parse[int]!(\"5\")
	return n
}
";
    let expected = [
        "a.gopp:27:6: string does not satisfy ~int | ~float64 (string missing in ~int | ~float64)",
        "a.gopp:28:12: string does not satisfy ~int | ~float64 (string missing in ~int | \
         ~float64)",
        "a.gopp:31:17: cannot use s (variable of struct type Set) as Mapper value in variable \
         declaration: Set does not implement Mapper (method Map has type parameters)",
        "a.gopp:32:6: cannot use generic function s.Map without instantiation",
    ];
    assert_eq!(messages("a.gopp", text), expected);
}

#[test]
fn a_body_records_the_generic_functions_and_types_that_it_names() {
    let text = "package p

type List[T any] struct{ items []T }

func (l List[T]) Conv[U any](f func(T) U) List[U] { return List[U]{} }

func first[S ~[]E, E any](s S) E { return s[0] }
func pair[K comparable, V any](k K, v V) map[K]V { return nil }

var table List[string]

func use(l List[int], xs []float64) {
	_ = first(xs)
	_ = pair[string](\"a\", 1)
	_ = pair[int, bool]
	var f func([]int) int = first
	_ = f
	_ = l.Conv(func(x int) string { return \"\" })
}

func generic[P any](p []P) P { return first(p) }
";
    let names = ThreadedRodeo::new();
    let loader = Loader::default();
    let checked = check_texts("p", &[("a.gopp", text)], &loader, &names);
    let types = loader.types.get().unwrap();
    let tree = &checked.files[0];
    let bodies = checked.package.files[0].bodies.iter();
    let mut found: Vec<(u32, String)> = bodies
        .flat_map(|body| &body.instances)
        .map(|(&expr, instance)| {
            let span = tree.exprs[expr].span;
            let line = text[..span.lo as usize].matches('\n').count() + 1;
            let written = &text[span.lo as usize..span.hi as usize];
            let show =
                |ty: &crate::types::TypeId| types.display_in(*ty, &names, names.get_or_intern("p"));
            let args: Vec<String> = instance.args.iter().map(show).collect();
            let shown = format!(
                "{line}: {written}: [{}] {}",
                args.join(", "),
                show(&instance.ty)
            );
            (span.lo, shown)
        })
        .collect();
    found.sort();
    let found: Vec<String> = found.into_iter().map(|(_, shown)| shown).collect();
    let expected = [
        "5: List[T]: [T] List[T]",
        "5: List[U]: [U] List[U]",
        "5: List[U]: [U] List[U]",
        "10: List[string]: [string] List[string]",
        "12: List[int]: [int] List[int]",
        "13: first: [[]float64, float64] func([]float64) float64",
        "14: pair[string]: [string, int] func(string, int) map[string]int",
        "15: pair[int, bool]: [int, bool] func(int, bool) map[int]bool",
        "16: first: [[]int, int] func([]int) int",
        "18: l.Conv: [string] func(func(int) string) List[string]",
        "21: first: [[]P, P] func([]P) P",
    ];
    assert_eq!(found, expected);
}

#[test]
fn literals_and_calls_of_type_parameters_are_checked_as_go_checks_them() {
    let text = "package p

type S struct{ f int }

func lit[P ~[]int, Q ~struct{ f int }, R ~map[string]int, X []int | []string, A any, N ~int, Y S | struct{ f int }]() {
	_ = P{1, 2}
	_ = Q{f: 1}
	_ = R{\"a\": 1}
	_ = Y{f: 2}
	_ = X{}
	_ = A{}
	_ = N{}
	_ = []P{{1}, {2}}
	_ = []X{{}}
	_ = P{\"x\"}
	_ = Q{g: 1}
}

func call[F func(int) string, G func() | func(int), A any, H ~func()](f F, g G, a A, h H) {
	var _ string = f(1)
	h()
	f(\"x\")
	g()
	a()
}

func plain(n int, s struct{ x int }) {
	n()
	s.x(1)
}
";
    let expected = [
        "a.gopp:10:6: invalid composite literal type X (no common underlying type)",
        "a.gopp:11:6: invalid composite literal type A (no common underlying type)",
        "a.gopp:12:6: invalid composite literal type N",
        "a.gopp:14:10: invalid composite literal element type X (no common underlying type)",
        "a.gopp:15:8: cannot use \"x\" (untyped string constant) as int value in array or slice literal",
        "a.gopp:16:8: unknown field g in struct literal of type Q",
        "a.gopp:22:4: cannot use \"x\" (untyped string constant) as int value in argument to f",
        "a.gopp:23:2: invalid operation: cannot call g (variable of type G constrained by func() | func(int)): func() and func(int) have different underlying types",
        "a.gopp:24:2: invalid operation: cannot call a (variable of type A constrained by any): no specific type",
        "a.gopp:28:2: invalid operation: cannot call n (variable of type int): int is not a function",
        "a.gopp:29:2: invalid operation: cannot call s.x (variable of type int): int is not a function",
    ];
    assert_eq!(messages("a.gopp", text), expected);
}

#[test]
fn a_generic_function_as_an_argument_is_inferred_with_the_function_that_is_called() {
    let text = "package p

func apply[P, Q any](f func(P) Q, p P) Q    { return f(p) }
func ident[T any](t T) T                    { return t }
func both[P any](f func(P) P, g func(P) P)  {}
func text[P any](f func(P) string) P        { var p P; return p }
func show[T any](t T) string                { return \"\" }
func number(int) string                     { return \"\" }

func _() {
	var _ int = apply(ident, 1)
	var _ string = apply(show, 1.5)
	var _ int = text(number)
	both(ident[int], ident)
	both(ident, ident[string])
	var _ string = apply(ident, 1)
	both(ident, ident)
	both(ident[int], ident[string])
	_ = apply(ident, nil)
	_ = text(show)
}
";
    let expected = [
        "a.gopp:3:56: cannot copy p (variable of type P constrained by any): P is not constrained by Clone",
        "    use move(p) to give the value, or &p to lend it",
        "a.gopp:6:51: cannot declare p without a value: P may be a type with no zero value",
        "    give p a value, or declare it as Option[P]",
        "a.gopp:16:17: cannot use apply(ident, 1) (value of type int) as string value in variable declaration",
        "a.gopp:17:2: in call to both, cannot infer P",
        "    a.gopp:5:11: P declared here",
        "a.gopp:18:19: in call to both, type func(string) string of ident[string] does not match inferred type func(int) int for func(P) P",
        "a.gopp:19:6: in call to apply, cannot infer P",
        "    a.gopp:3:12: P declared here",
        "a.gopp:20:6: in call to text, cannot infer P",
        "    a.gopp:6:11: P declared here",
    ];
    assert_eq!(messages("a.gopp", text), expected);
}

// rule: RNG-8
#[test]
fn a_loop_variable_is_not_redeclared_in_the_body_of_its_loop() {
    let text = "package p

func use(int) {}

func f(xs []int, m map[string]int) {
	for i, x := range xs {
		x := x
		{
			var i = i
			use(i)
		}
		go func(x int) { use(x) }(x)
	}
	for i := 0; i < 3; i++ {
		for i := range xs {
			use(i)
		}
	}
	for k, v := range m {
		use(v)
		mine := k
		const v = 1
		_ = mine
	}
	i, x := 1, 2
	use(i + x)
}
";
    let expected = [
        "a.gopp:7:3: x redeclares a loop variable: use another name",
        "    a.gopp:6:9: loop variable x declared here",
        "a.gopp:9:8: i redeclares a loop variable: use another name",
        "    a.gopp:6:6: loop variable i declared here",
        "a.gopp:12:11: x redeclares a loop variable: use another name",
        "    a.gopp:6:9: loop variable x declared here",
        "a.gopp:15:7: i redeclares a loop variable: use another name",
        "    a.gopp:14:6: loop variable i declared here",
    ];
    assert_eq!(messages("a.gopp", text), expected);
}

// rule: RNG-1a, RNG-6a
#[test]
fn an_alias_variable_of_a_range_clause_names_an_element() {
    let text = r#"package p

type Doc struct{ N int }

func all(yield func(*Doc) bool)          {}
func vals(yield func(Doc) bool)          {}
func pairs(yield func(int, *Doc) bool)   {}
func plain(yield func(int, Doc) bool)    {}

func f(xs []Doc, arr [3]Doc, m map[string]Doc, s string, c chan Doc, p *[]Doc) {
	for _, &d := range xs {
		d.N++
	}
	for _, &d := range arr {
		d.N++
	}
	for _, &d := range m {
		d.N++
	}
	for _, &d := range p {
		d.N++
	}
	for &d := range all {
		d.N++
	}
	for _, &d := range pairs {
		d.N++
	}
	for _, &r := range s {
		_ = r
	}
	for &v := range c {
		_ = v
	}
	for &i := range 10 {
		_ = i
	}
	for &d := range vals {
		_ = d
	}
	for _, &d := range plain {
		_ = d
	}
}
"#;
    let expected = [
        "a.gopp:29:9: invalid & in range clause: s (variable of type string) has no elements to \
         alias",
        "a.gopp:32:6: invalid & in range clause: c (variable of type chan Doc) has no elements to \
         alias",
        "a.gopp:35:6: invalid & in range clause: 10 (untyped int constant) has no elements to alias",
        "a.gopp:38:6: invalid & in range clause: vals (value of type func(func(Doc) bool)) does \
         not yield pointers",
        "a.gopp:41:9: invalid & in range clause: plain (value of type func(func(int, Doc) bool)) \
         does not yield pointers",
    ];
    assert_eq!(messages("a.gopp", text), expected);
}

// rule: OWN-P5
#[test]
fn an_assignment_to_a_pointer_writes_through_it_or_points_it_elsewhere() {
    let text = "package p

type List struct {
	next *List
	v    int
}

func f(n *int, m *int, xs *[]int, l *List, o *List, v int, pair func() (int, *int)) {
	n = 9
	n = v
	n++
	n += 2
	xs = append(xs, 4)
	xs = nil
	l.v = 1
	n = m
	l = o
	l = l.next
	v = 3
	v, n = pair()
	n, m = m, n
	n, v = 1, 2
}
";
    let names = ThreadedRodeo::new();
    let loader = Loader::default();
    let checked = check_texts("p", &[("a.gopp", text)], &loader, &names);
    assert_eq!(checked.messages, [] as [&str; 0]);
    let tree = &checked.files[0];
    let mut written: Vec<(u32, u32)> = checked.package.files[0].bodies[1]
        .writes_through
        .iter()
        .map(|&place| (tree.exprs[place].span.lo, tree.exprs[place].span.hi))
        .collect();
    written.sort_unstable();
    let line = |&(lo, hi): &(u32, u32)| {
        let before = &text[..lo as usize];
        let line = before.matches('\n').count() + 1;
        format!("{line}: {}", &text[lo as usize..hi as usize])
    };
    let written: Vec<String> = written.iter().map(line).collect();
    // The other assignments to a pointer point it at something else.
    let expected = [
        "9: n", "10: n", "11: n", "12: n", "13: xs", "14: xs", "22: n",
    ];
    assert_eq!(written, expected);

    // With a star the place is what the pointer points to, as it is
    // written: nothing is left to find out.
    let starred = "package p
func g(n *int, xs *[]int, q **int, v int) {
	*n = 9
	*n += v
	*n++
	*xs = append(*xs, 4)
	*q = n
	**q = 3
	*n, v = v, *n
}
";
    let checked = check_texts("p", &[("a.gopp", starred)], &loader, &names);
    assert_eq!(checked.messages, [] as [&str; 0]);
    assert!(checked.package.files[0].bodies[0].writes_through.is_empty());
    // What is no pointer has nothing behind it.
    let found = messages("a.gopp", "package p\nfunc h(v int) { *v = 1 }\n");
    assert_eq!(
        found,
        ["a.gopp:2:18: invalid operation: cannot indirect v (variable of type int)"]
    );
}

#[test]
fn the_cases_of_a_type_switch_have_their_types_as_the_runtime_of_go_names_them() {
    let text = "package p

type point struct{ x, y int }

type shape interface{ Area() float64 }

func f(v any) int {
	switch x := v.(type) {
	case nil:
		return 0
	case int, point:
		return 1
	case []any, map[string]struct{}:
		return 2
	case struct {
		at point
		n  int `tag`
	}:
		return x.n
	case shape, interface{ B(); A(int) string }:
		return 3
	case func(int, ...string) (bool, error), [2]*point, <-chan byte:
		return 4
	}
	return 5
}
";
    {
        let name = "a.gopp";
        let names = ThreadedRodeo::new();
        let loader = Loader::default();
        let checked = check_texts("p", &[(name, text)], &loader, &names);
        assert_eq!(checked.messages, [] as [&str; 0]);
        let types = loader.types.get().unwrap();
        let tree = &checked.files[0];
        let bodies = checked.package.files[0].bodies.iter();
        let mut found: Vec<(u32, String)> = bodies
            .flat_map(|body| &body.case_types)
            .map(|(&case, &ty)| (tree.exprs[case].span.lo, types.runtime_name(ty, &names)))
            .collect();
        found.sort();
        let found: Vec<String> = found.into_iter().map(|(_, name)| name).collect();
        // The case `nil` is no type.
        assert_eq!(
            found,
            [
                "int",
                "p.point",
                "[]interface {}",
                "map[string]struct {}",
                "struct { at p.point; n int \"tag\" }",
                "p.shape",
                "interface { A(int) string; B() }",
                "func(int, ...string) (bool, error)",
                "[2]*p.point",
                "<-chan uint8",
            ],
            "{name}"
        );
    }
}

#[test]
fn an_interface_value_in_a_place_of_another_interface_type_is_noted() {
    let text = "package p

type Shape interface{ Area() int }

type Named interface{ Name() string }

type Both interface {
	Area() int
	Name() string
}

type Same interface{ Area() int }

type Square struct{ side int }

func (s Square) Area() int { return s.side }

func (s Square) Name() string { return \"square\" }

type holder struct {
	shape Shape
	any   any
}

func take(s Shape, rest []any) {}

func two() (Both, Square, Shape) { return Square{}, Square{}, Square{} }

func f(b Both, s Shape, shapes []Shape, byName map[string]any) (Shape, any, Same) {
	var named Named = b
	var same Same = s
	s = b
	shapes = append(shapes, b, s)
	shapes[0] = b
	byName[\"b\"] = b
	h := holder{b, s}
	h.any = named
	take(b, []any{b, s, 1})
	var held any
	named, s, held = two()
	s, held, same = two()
	_, _, _, _, _, _ = named, same, h, held, shapes, len(byName)
	if s == b {
		return two()
	}
	return b, b, s
}
";
    {
        let name = "a.gopp";
        let names = ThreadedRodeo::new();
        let loader = Loader::default();
        let checked = check_texts("p", &[(name, text)], &loader, &names);
        assert_eq!(checked.messages, [] as [&str; 0], "{name}");
        let types = loader.types.get().unwrap();
        let tree = &checked.files[0];
        let show = |ty| types.display_in(ty, &names, names.get_or_intern("p"));
        let at = |expr: gopp_syntax::ast::ExprId| {
            let span = tree.exprs[expr].span;
            let line = text[..span.lo as usize].matches('\n').count() + 1;
            (span.lo, line, &text[span.lo as usize..span.hi as usize])
        };
        let mut found: Vec<(u32, u32, String)> = Vec::new();
        for body in &checked.package.files[0].bodies {
            for (&expr, &(from, to)) in &body.iface_changes {
                let (lo, line, written) = at(expr);
                let shown = format!("{line}: {written}: {} to {}", show(from), show(to));
                found.push((lo, 0, shown));
            }
            for (&(expr, part), &(from, to)) in &body.part_conversions {
                let (lo, line, written) = at(expr);
                let shown = format!("{line}: {written} #{part}: {} to {}", show(from), show(to));
                found.push((lo, part + 1, shown));
            }
        }
        found.sort();
        let found: Vec<String> = found.into_iter().map(|(_, _, shown)| shown).collect();
        // An interface type with the same methods takes the value as
        // it is: `Same` and `Shape`. Two values that are compared stay
        // what they are: each says its type by itself.
        assert_eq!(
            found,
            [
                "30: b: Both to Named",
                "32: b: Both to Shape",
                "33: b: Both to Shape",
                "34: b: Both to Shape",
                "35: b: Both to any",
                "36: b: Both to Shape",
                "36: s: Shape to any",
                "37: named: Named to any",
                "38: b: Both to Shape",
                "38: b: Both to any",
                "38: s: Shape to any",
                "40: two() #0: Both to Named",
                "40: two() #1: Square to Shape",
                "40: two() #2: Shape to any",
                "41: two() #0: Both to Shape",
                "41: two() #1: Square to any",
                "44: two() #0: Both to Shape",
                "44: two() #1: Square to any",
                "46: b: Both to Shape",
                "46: b: Both to any",
            ],
            "{name}"
        );
    }
}

/// Where each file puts a value into an interface without a conversion
/// being written: `line: expression: its type to the interface type`.
fn iface_conversions(name: &str, text: &str) -> Vec<String> {
    let names = ThreadedRodeo::new();
    let loader = Loader::default();
    let checked = check_texts("p", &[(name, text)], &loader, &names);
    assert_eq!(checked.messages, [] as [&str; 0]);
    let types = loader.types.get().unwrap();
    let tree = &checked.files[0];
    let bodies = checked.package.files[0].bodies.iter();
    let mut found: Vec<(u32, String)> = bodies
        .flat_map(|body| &body.iface_conversions)
        .map(|(&expr, &(from, to))| {
            let span = tree.exprs[expr].span;
            let line = text[..span.lo as usize].matches('\n').count() + 1;
            let written = &text[span.lo as usize..span.hi as usize];
            let show = |ty| types.display_in(ty, &names, names.get_or_intern("p"));
            let shown = format!("{line}: {written}: {} to {}", show(from), show(to));
            (span.lo, shown)
        })
        .collect();
    found.sort();
    found.into_iter().map(|(_, shown)| shown).collect()
}

#[test]
fn a_body_records_where_a_value_is_put_into_an_interface() {
    let text = "package p

type I interface{ m() }
type T struct{}

func (T) m() {}

var global I = T{}

func use(i I, rest ...any) {}

func f(t T, i I, c chan I, m *map[any]I) I {
	var a I = t
	a = i
	use(t, 1, \"s\", i, nil)
	c <- t
	m[2.5] = t
	_ = []I{t, i}
	_ = struct{ i I }{t}
	_ = I(t)
	for _, a = range []T{} {
	}
	_ = a
	return t
}
";
    let expected = [
        "8: T{}: T to I",
        "13: t: T to I",
        "15: t: T to I",
        "15: 1: int to any",
        "15: \"s\": string to any",
        "16: t: T to I",
        "17: 2.5: float64 to any",
        "17: t: T to I",
        "18: t: T to I",
        "19: t: T to I",
        "21: a: T to I",
        "24: t: T to I",
    ];
    assert_eq!(iface_conversions("a.gopp", text), expected);
}

// rule: OWN-P5, OWN-P6
#[test]
fn a_body_records_what_of_a_pointer_or_a_handle_is_put_into_an_interface() {
    let text = "package p

type I interface{ m() }
type T struct{}

func (T) m() {}

type P struct{}

func (p *P) m() {}

func f(t T, pt *T, pp *P, u Unique[P], s Shared[T], o Option[I]) {
	var a Local[I] = t
	a = pt
	a = pp
	a = u
	a = s
	o = t
	o = o
	_ = a
}
";
    // A pointer stays a pointer where that fits, and is kept only by an
    // interface value that stays with one goroutine; a handle is held as
    // the handle.
    let expected = [
        "13: t: T to I",
        "14: pt: *T to I",
        "15: pp: *P to I",
        "16: u: Unique[P] to I",
        "17: s: Shared[T] to I",
        "18: t: T to I",
    ];
    assert_eq!(iface_conversions("a.gopp", text), expected);
}

/// The variables that each step of the start of the program sets.
fn init_order(text: &str) -> (Vec<String>, Vec<String>) {
    let names = ThreadedRodeo::new();
    let checked = check_texts("p", &[("a.gopp", text)], &Loader::default(), &names);
    let package = &checked.package;
    let step = |initializer: &crate::Initializer| {
        let name = |&object| names.resolve(&package.object(object).name);
        initializer
            .lhs
            .iter()
            .map(name)
            .collect::<Vec<_>>()
            .join(",")
    };
    (
        package.init_order.iter().map(step).collect(),
        checked.messages,
    )
}

#[test]
fn variables_are_initialized_after_what_they_need() {
    let cases: &[(&str, &[&str])] = &[
        ("var a = 1\nvar b = 2\nvar c = 3", &["a", "b", "c"]),
        ("var a = b + 1\nvar b = c\nvar c = 1", &["c", "b", "a"]),
        // What a function needs, its callers need.
        (
            "var a = f()\nvar b = 1\nfunc f() int { return g() }\nfunc g() int { return b + c }\nvar c = 2",
            &["b", "c", "a"],
        ),
        // Functions may call each other.
        (
            "var a = even(3)\nfunc even(n int) bool { return n == 0 || odd(n-1) }\n\
             func odd(n int) bool { return n != 0 && even(n-1) && limit > 0 }\nvar limit = 9",
            &["limit", "a"],
        ),
        // One value for two variables is one step; two values are two.
        (
            "var a, b = f()\nfunc f() (int, int) { return c, c }\nvar c = 1",
            &["c", "a,b"],
        ),
        ("var a, b = d, 1\nvar d = 2", &["b", "d", "a"]),
        // A variable without a value is no step, a blank one with a value is.
        ("var a int\nvar _ = b\nvar b = a", &["b", "_"]),
        // The first in the source that is ready goes first.
        (
            "var z = 1\nvar y = x\nvar x = 2\nvar w = 3",
            &["z", "x", "y", "w"],
        ),
        (
            "const k = 2\nvar a = k\nvar f = func() int { return b }\nvar b = 1",
            &["a", "b", "f"],
        ),
        // What a method needs, what names the method needs: called, or
        // as a function of its receiver.
        (
            "type T struct{}\nfunc (T) m() int { return b }\nvar a = T{}.m()\nvar b = 1",
            &["b", "a"],
        ),
        (
            "type T struct{}\nfunc (*T) m() int { return b }\nvar a = (*T).m\nvar b = 1",
            &["b", "a"],
        ),
        // Which method a call through an interface runs is not known.
        (
            "type I interface{ m() int }\ntype T struct{}\nfunc (T) m() int { return b }\n\
             var i I = T{}\nvar a = i.m()\nvar b = 1",
            &["i", "a", "b"],
        ),
    ];
    for (body, expected) in cases {
        let (order, messages) = init_order(&format!("package p\n{body}\n"));
        assert_eq!(messages, [] as [&str; 0], "{body}");
        assert_eq!(order, *expected, "{body}");
    }
}

#[test]
fn variables_that_need_each_other_are_an_error() {
    let cases: &[(&str, &[&str])] = &[
        (
            "var x = x",
            &["a.gopp:2:5: initialization cycle: x refers to itself"],
        ),
        (
            "var a = b\nvar b = a",
            &[
                "a.gopp:2:5: initialization cycle for a",
                "    a.gopp:2:5: a refers to b",
                "    a.gopp:3:5: b refers to a",
            ],
        ),
        (
            "var x = f()\nfunc f() int { return g() }\nfunc g() int { return x }",
            &[
                "a.gopp:2:5: initialization cycle for x",
                "    a.gopp:2:5: x refers to f",
                "    a.gopp:3:6: f refers to g",
                "    a.gopp:4:6: g refers to x",
            ],
        ),
        (
            "const a = b\nconst b = a",
            &[
                "a.gopp:2:7: initialization cycle for a",
                "    a.gopp:2:7: a refers to b",
                "    a.gopp:3:7: b refers to a",
            ],
        ),
        // A constant that repeats the value above it needs what that needs.
        (
            "const (\n\ta = c + iota\n\tb\n\tc\n)",
            &["a.gopp:5:2: initialization cycle: c refers to itself"],
        ),
        (
            "type T struct{}\nfunc (T) m() int { return x }\nvar x = T{}.m()",
            &[
                "a.gopp:4:5: initialization cycle for x",
                "    a.gopp:4:5: x refers to m",
                "    a.gopp:3:10: m refers to x",
            ],
        ),
        (
            "type T struct{}\nfunc (T) m() { _ = y }\nvar x = T.m\nvar y = x",
            &[
                "a.gopp:4:5: initialization cycle for x",
                "    a.gopp:4:5: x refers to m",
                "    a.gopp:3:10: m refers to y",
                "    a.gopp:5:5: y refers to x",
            ],
        ),
    ];
    for (body, expected) in cases {
        let (_, messages) = init_order(&format!("package p\n{body}\n"));
        assert_eq!(messages, *expected, "{body}");
    }
}

#[test]
fn init_and_main_are_plain_functions() {
    let check = |package: &str, body: &str| {
        let text = format!("package {package}\n{body}\n");
        let loader = Loader::default();
        check_texts(
            package,
            &[("a.gopp", &text)],
            &loader,
            &ThreadedRodeo::new(),
        )
        .messages
    };
    assert_eq!(
        check("main", "func init() {}\nfunc init() {}\nfunc main() {}"),
        [] as [&str; 0]
    );
    assert_eq!(
        check("p", "func main(x int) int { return x }\nvar _ = main"),
        [] as [&str; 0]
    );
    assert_eq!(
        check("main", "func init(x int) {}\nfunc main() int { return 0 }"),
        [
            "a.gopp:2:6: func init must have no arguments and no return values",
            "a.gopp:3:6: func main must have no arguments and no return values",
        ]
    );
    assert_eq!(
        check("main", "func init[T any]() {}\nfunc init()"),
        [
            "a.gopp:2:11: func init must have no type parameters",
            "a.gopp:3:6: func init must have a body",
        ]
    );
    assert_eq!(
        check("main", "var init = 1\ntype main int"),
        [
            "a.gopp:2:5: cannot declare init - must be func",
            "a.gopp:3:6: cannot declare main - must be func",
        ]
    );
}

#[test]
fn the_steps_of_ordering_grow_with_the_declarations_not_faster() {
    // Wall time is not measured here; this only has to end. Each program
    // is one long chain, the worst case for following references.
    let chain = |count: usize| {
        let vars: String = (0..count)
            .map(|i| format!("var v{i} = v{}\n", i + 1))
            .collect();
        let funcs: String = (0..count)
            .map(|i| format!("func f{i}() int {{ return f{}() }}\n", i + 1))
            .collect();
        let types: String = (0..count)
            .map(|i| format!("type T{i} struct{{ t T{} }}\n", i + 1))
            .collect();
        format!(
            "package p\n{vars}var v{count} = f0()\n{funcs}func f{count}() int {{ return 1 }}\n\
             {types}type T{count} int\nvar _ = [1]T0{{}}\n"
        )
    };
    let (order, messages) = init_order(&chain(20_000));
    assert_eq!(messages, [] as [&str; 0]);
    assert_eq!(order.len(), 20_002);
    assert_eq!(order[0], "v20000");
    assert_eq!(order[20_000], "v0");
}

/// The type of each package-level object of `text`, and the value of each
/// constant, as `name: type = value`, in the order of the names.
fn declared(file: &str, text: &str) -> (Vec<String>, Vec<String>) {
    let names = ThreadedRodeo::new();
    let loader = Loader::default();
    let checked = check_texts("p", &[(file, text)], &loader, &names);
    let types = loader.types.get().unwrap();
    let mut lines = Vec::new();
    for object in &checked.package.objects {
        let name = names.resolve(&object.name);
        let underlying = types.underlying(object.ty);
        let mut line = format!("{name}: {}", types.display(object.ty, &names));
        if underlying != object.ty {
            line += &format!(" ({})", types.display(underlying, &names));
        }
        if object.value != crate::constant::Value::Unknown {
            line += &format!(" = {}", object.value.exact_string());
        }
        lines.push(line);
    }
    lines.sort();
    (lines, checked.messages)
}

#[test]
fn declarations_get_their_types_and_constants_their_values() {
    let text = "package p

const (
	KB = 1 << (10 * (iota + 1))
	MB
	GB
)

const Third = 1.0 / 3
const Big = 1 << 100 >> 98
const Typed int8 = 100 + 27
const Name = \"go\" + \"pp\"
const Size = len(Name) + 1
const Mask uint8 = ^uint8(0x0f)
const Less = Third < 0.5

type Celsius float64
const Boiling Celsius = 100
const Twice = Boiling * 2

type Node struct {
	Name  string `json:\"name\"`
	next  Option[*Node]
	owner Shared[Node]
	Size  int @Required
}

type Reader interface {
	Read!(buf *[]byte) int
	Close()
}

type List[E any] struct {
	head Option[*List[E]]
	item E
}

type Bytes = []byte

var table map[string][]Node
var ints List[int]

func Open!(name string, flags ...int) Unique[Node]
func Map[T, U any](xs []T, f func(T) U) []U
";
    let (lines, messages) = declared("a.gopp", text);
    assert_eq!(messages, [] as [&str; 0]);
    let expected = [
        "Big: untyped int = 4",
        "Boiling: p.Celsius (float64) = 100",
        "Bytes: p.Bytes",
        "Celsius: p.Celsius (float64)",
        "GB: untyped int = 1073741824",
        "KB: untyped int = 1024",
        "Less: untyped bool = true",
        "List: p.List (struct{head Option[*p.List[E]]; item E})",
        "MB: untyped int = 1048576",
        "Map: func([]T, func(T) U) []U",
        "Mask: uint8 = 240",
        "Name: untyped string = \"gopp\"",
        "Node: p.Node (struct{Name string \"json:\\\"name\\\"\"; next Option[*p.Node]; owner \
         Shared[p.Node]; Size int @Required})",
        "Open: func(string, ...int) (Unique[p.Node], error)",
        "Reader: p.Reader (interface{Close(); Read(*[]byte) (int, error)})",
        "Size: int = 5",
        "Third: untyped float = 1/3",
        "Twice: p.Celsius (float64) = 200",
        "Typed: int8 = 127",
        "ints: p.List[int] (struct{head Option[*p.List[int]]; item int})",
        "table: map[string][]p.Node",
    ];
    assert_eq!(lines, expected);
}

#[test]
fn the_size_of_a_type_that_holds_itself_is_not_asked_forever() {
    let text = "package p\nimport \"unsafe\"\ntype T struct{ t T }\nvar t T\n\
                const size = unsafe.Sizeof(t)\nvar same = t == t\n";
    let names = ThreadedRodeo::new();
    let mut loader = Loader::default();
    let stub = "package unsafe\nfunc Sizeof(x any) uintptr\n";
    let stub = check_texts("unsafe", &[("unsafe.gopp", stub)], &loader, &names);
    loader
        .packages
        .insert("unsafe".to_owned(), Arc::new(stub.package));
    let found = check_texts("p", &[("a.gopp", text)], &loader, &names).messages;
    assert_eq!(
        found,
        ["a.gopp:3:6: invalid recursive type: T refers to itself"]
    );
}

#[test]
fn a_name_that_is_no_type_is_refused_as_one() {
    let text = "package p\nconst limit = 3\nvar x limit\nvar y len\nfunc f(a g) {}\nfunc g() {}\n";
    let (_, messages) = declared("a.gopp", text);
    assert_eq!(
        messages,
        [
            "a.gopp:3:7: limit is not a type",
            "a.gopp:4:7: len is not a type",
            "a.gopp:5:10: g is not a type",
        ]
    );
}

#[test]
fn the_methods_of_a_type_make_it_implement_an_interface() {
    let text = "package p
type File struct{}
func (f File) Name() string
func (f *File) Close()
func (f *File) Read!(buf *[]byte) int
type Reader interface {
	Read(buf *[]byte) (int, error)
	Close()
}
type Namer interface{ Name() string }
";
    let names = ThreadedRodeo::new();
    let loader = Loader::default();
    let checked = check_texts("p", &[("a.gopp", text)], &loader, &names);
    assert_eq!(checked.messages, [] as [&str; 0]);
    let types = loader.types.get().unwrap();
    let ty = |name: &str| {
        let object = checked.package.scope[&names.get(name).unwrap()];
        checked.package.object(object).ty
    };
    let (file, reader, namer) = (ty("File"), ty("Reader"), ty("Namer"));
    let pointer = types.pointer(file);
    assert!(types.implements(file, namer, &names));
    assert!(types.implements(pointer, namer, &names));
    // The methods of `Reader` have pointer receivers.
    assert!(!types.implements(file, reader, &names));
    assert!(types.implements(pointer, reader, &names));
}

#[test]
fn a_variable_without_a_type_has_the_type_of_its_value() {
    let text = "package p

type Point struct{ X, Y int }

func (p Point) Scale(k int) Point
func NewPoint() (Point, error)
func Pair[K comparable, V any](k K, v V) map[K]V
func First[E any](xs []E) E

var origin = Point{}
var ptr = &origin
var xs = []Point{origin}
var first = xs[0].X
var scaled = origin.Scale(2)
var viaPointer = ptr.Scale
var p2, err = NewPoint()
var table = Pair(\"a\", 1.5)
var v, ok = table[\"a\"]
var head = First(xs)
var n = len(xs) + 1
var big = 1 << 40
var name = \"go\" + \"pp\"
var less = first < n
var fn = func(a int) string { return \"\" }
var arr = [...]string{2: \"c\", \"d\"}
var size = len(arr)
var raw = []byte(name)
var opt Option[int]
var maybe = opt
var ch = make(chan Point, 1)
var got = <-ch
var more = append(xs, origin)
";
    let (lines, messages) = declared("a.gopp", text);
    assert_eq!(messages, [] as [&str; 0]);
    let vars: Vec<&str> = lines
        .iter()
        .map(String::as_str)
        .filter(|line| line.starts_with(char::is_lowercase))
        .collect();
    let expected = [
        "arr: [4]string",
        "big: int",
        "ch: chan p.Point",
        "err: error (interface{Error() string})",
        "first: int",
        "fn: func(int) string",
        "got: p.Point (struct{X int; Y int})",
        "head: p.Point (struct{X int; Y int})",
        "less: bool",
        "maybe: Option[int]",
        "more: []p.Point",
        "n: int",
        "name: string",
        "ok: bool",
        "opt: Option[int]",
        "origin: p.Point (struct{X int; Y int})",
        "p2: p.Point (struct{X int; Y int})",
        "ptr: *p.Point",
        "raw: []byte",
        "scaled: p.Point (struct{X int; Y int})",
        "size: int",
        "table: map[string]float64",
        "v: float64",
        "viaPointer: func(int) p.Point",
        "xs: []p.Point",
    ];
    assert_eq!(vars, expected);
}

#[test]
fn locals_have_the_types_of_their_values() {
    let text = "package p

type Point struct{ X, Y int }

func (p Point) Dist!(q Point) float64

func walk(points []Point, names map[string]*Point, done chan bool) (total float64, err error) {
	origin := Point{}
	for i, &p := range points {
		d, failed := p.Dist(origin)
		if failed != nil {
			return 0, failed
		}
		total += d * float64(i)
	}
	for name, at := range names {
		first := name[0]
		x := at.X
		_, _ = first, x
	}
	ok := <-done
	found, has := names[\"a\"]
	const limit = 10
	var grid [limit][2]Point
	row := grid[1]
	add := func(a, b int) int { sum := a + b; return sum }
	switch v := any(row).(type) {
	case [2]Point:
		_ = v
	case string, int:
		_ = v
	}
	if has {
		_ = found
	}
	_, _, _ = ok, has, add
	return total, nil
}
";
    let names = ThreadedRodeo::new();
    let loader = Loader::default();
    let checked = check_texts("p", &[("a.gopp", text)], &loader, &names);
    assert_eq!(checked.messages, [] as [&str; 0]);
    let types = loader.types.get().unwrap();
    let body = &checked.package.files[0].bodies[2];
    let locals: Vec<String> = body
        .locals
        .iter()
        .map(|local| {
            format!(
                "{}: {}",
                names.resolve(&local.name),
                types.display(local.ty, &names)
            )
        })
        .collect();
    let expected = [
        "points: []p.Point",
        "names: map[string]*p.Point",
        "done: chan bool",
        "total: float64",
        "err: error",
        "origin: p.Point",
        "i: int",
        "p: p.Point",
        "d: float64",
        "failed: error",
        "name: string",
        "at: *p.Point",
        "first: byte",
        "x: int",
        "ok: bool",
        "found: *p.Point",
        "has: bool",
        "limit: untyped int",
        "grid: [10][2]p.Point",
        "row: [2]p.Point",
        "a: int",
        "b: int",
        "sum: int",
        "add: func(int, int) int",
        "v: [2]p.Point",
        "v: any",
    ];
    assert_eq!(locals, expected);
}

#[test]
fn a_constant_of_no_type_gets_its_type_from_around_it() {
    let text = "package p

type Celsius float64

func take(n int8, rest ...float32) (uint16, any)

func f(c Celsius, xs []int32, m map[uint8]string, ch chan complex64) (float32, uint16) {
	var a float32 = 1
	b := 2
	d := c + 3
	e := xs[4] * 5
	m[6] = \"x\"
	ch <- 7
	g, h := take(8, 9, 10)
	i := []float64{11, 12}
	j := Celsius(13)
	k := (14 + 0.5) * c
	var l any = 15
	_ = 16.5
	return 17, 18
}
";
    let names = ThreadedRodeo::new();
    let loader = Loader::default();
    let checked = check_texts("p", &[("a.gopp", text)], &loader, &names);
    let types = loader.types.get().unwrap();
    let tree = &checked.files[0];
    let body = &checked.package.files[0].bodies[2];
    let mut literals: Vec<(u32, String)> = Vec::new();
    for (&expr, typed) in &body.types {
        let node = &tree.exprs[expr];
        if matches!(node.kind, ExprKind::BasicLit { .. }) {
            let literal = &text[node.span.lo as usize..node.span.hi as usize];
            let ty = types.display(typed.ty, &names);
            literals.push((
                node.span.lo,
                format!("{literal}: {ty} = {}", typed.value.exact_string()),
            ));
        }
    }
    literals.sort();
    let lines: Vec<&str> = literals.iter().map(|(_, line)| line.as_str()).collect();
    let expected = [
        "1: float32 = 1",
        "2: int = 2",
        "3: p.Celsius = 3",
        "4: int = 4",
        "5: int32 = 5",
        "6: uint8 = 6",
        "\"x\": string = \"x\"",
        "7: complex64 = (7 + 0i)",
        "8: int8 = 8",
        "9: float32 = 9",
        "10: float32 = 10",
        "11: float64 = 11",
        "12: float64 = 12",
        "13: p.Celsius = 13",
        // Inside a constant expression the parts keep their kind; the
        // whole takes the type.
        "14: untyped float = 14",
        "0.5: untyped float = 1/2",
        "15: int = 15",
        "16.5: float64 = 33/2",
        "17: float32 = 17",
        "18: uint16 = 18",
    ];
    assert_eq!(lines, expected);
}

#[test]
fn a_method_of_a_generic_type_is_checked_with_the_constraints_of_the_type() {
    let text = "package p

type Stringer interface{ String() string }

type Number interface{ ~int | ~float64 }

type Holder[T HELD, N Number] struct {
	item  T
	count N
}

func (h *Holder[S, C]) name() string {
	var held any = h.item
	_ = held
	h.count += 1
	return h.item.String()
}

func (h *Holder[S, C]) wrong() string {
	return h.item.Missing() + h.count.String()
}
";
    // A value is kept in an interface only if its type says that it is
    // copied and may be given to another goroutine.
    let held = [("a.gopp", "interface{ Stringer; Clone; Send }")];
    for (name, held) in held {
        let text = &text.replace("HELD", held);
        // The type parameters of the receiver have other names in the
        // body, and the constraints that the type gives them.
        assert_eq!(
            messages(name, text),
            [
                format!(
                    "{name}:20:16: h.item.Missing undefined (type S has no field or method Missing)"
                ),
                format!(
                    "{name}:20:36: h.count.String undefined (type C has no field or method String)"
                ),
            ],
            "{name}"
        );
        let names = ThreadedRodeo::new();
        let loader = Loader::default();
        let checked = check_texts("p", &[(name, text)], &loader, &names);
        let source = checked.map.file(checked.files[0].id);
        let files = [(&checked.files[0], &*source)];
        let types = loader.types.get().unwrap();
        let info = crate::info_text(&checked.package, &files, types, &names);
        assert!(
            info.contains("13:17 h.item: boxed S to any"),
            "{name}: {info}"
        );
        assert!(
            info.contains("16:9 h.item.String: selection Method of S in S by []"),
            "{name}: {info}"
        );
    }
}

#[test]
fn a_required_field_of_a_type_parameter_takes_no_option() {
    let text = "package p

type Box[T any] struct {
	v T @Required
	n int
}

type Plain[T any] struct{ v T }

type Maybe Option[int]

func good(p *int) {
	a := Box[int]{v: 1}
	b := Box[*int]{v: p}
	c := Plain[Option[int]]{}
	_, _, _ = a, b, c
}

func bad(o Option[int], m Maybe) {
	a := Box[Option[int]]{v: o}
	b := Box[Maybe]{v: m}
	_, _ = a, b
}
";
    // The field must be set and an option may be nil (ADR-0016): held
    // where the type argument is given.
    assert_eq!(
        messages("a.gopp", text),
        [
            "a.gopp:20:11: cannot use Option[int] for T: a field of it is @Required, and an option \
             may be nil",
            "a.gopp:21:11: cannot use Maybe for T: a field of it is @Required, and an option may be \
             nil",
        ]
    );
}

#[test]
fn a_value_that_an_error_guards_is_checked_in_a_generic_body_too() {
    let text = "package p

type Node struct{ v int }

func number(get func() (int, error)) int {
	v, _ := get()
	return v
}

func unnamed(get func() (*Node, error)) *Node {
	v, _ := get()
	return v
}

func unchecked[T any](get func() (T, error)) T {
	v, err := get()
	_ = err
	return v
}

func checked[T any]!(get func() (T, error)) T {
	v, err := get()
	if err != nil {
		return nil, err
	}
	return v
}

func use!() int {
	return checked!(func() (int, error) { return 1, nil })
}
";
    // A type parameter may be a type without a zero value: the body is
    // held to the rule once, for every type argument (OWN-P10). What
    // has a zero value needs no check, and a guard without a name says
    // so.
    assert_eq!(
        messages("a.gopp", text),
        [
            "a.gopp:12:9: cannot use v here: *Node has no zero value, and what says whether v is \
             set has no name",
            "    name the last value, and use v where it says that v is set",
            "a.gopp:18:9: cannot use v here: T has no zero value, so v is set only where err is nil",
            "    return or go on where err != nil, and use v after that",
        ]
    );
}

#[test]
fn a_variable_is_known_set_until_its_new_value_is_worked_out() {
    let text = "package p

type Node struct {
	value int
	next  Option[*Node]
}

func sum(head Option[*Node]) int {
	total := 0
	n := head
	for n != nil {
		total += n.value
		n = n.next
	}
	return total
}

func after(head Option[*Node]) int {
	n := head
	if n != nil {
		n = n.next
		return n.value
	}
	return 0
}
";
    // The value on the right of `n = n.next` reads the `n` that is
    // known to be set (OWN-P10); after the assignment nothing is known
    // of it.
    assert_eq!(
        messages("a.gopp", text),
        [
            "a.gopp:22:12: n.value undefined (type Option[*Node] has no field or method value)",
            "    n may be nil: put it in a variable and test that against nil first",
        ]
    );
}
