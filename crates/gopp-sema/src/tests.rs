use std::collections::{BTreeMap, HashMap};
use std::fs;
use std::path::PathBuf;
use std::process::Command;
use std::sync::Arc;

use gopp_diag::Sink;
use gopp_span::{Dialect, SourceFile, SourceMap};
use gopp_syntax::ast::{ExprId, ExprKind, File};
use gopp_syntax::parser::parse;
use gopp_testutil::{linear, samples, stress};
use lasso::ThreadedRodeo;

use crate::resolve::check_counted;
use crate::{LocalKind, ObjKind, Package, PackageLoader, Ref, Unit, check};

/// Packages by import path. With `fake_rest`, any other path is a package
/// about which nothing is known.
#[derive(Default)]
struct Loader {
    packages: HashMap<String, Arc<Package>>,
    fake_rest: bool,
}

impl PackageLoader for Loader {
    fn load(&self, path: &str) -> Result<Arc<Package>, String> {
        match self.packages.get(path) {
            Some(package) => Ok(Arc::clone(package)),
            None if self.fake_rest => Ok(Arc::new(Package::fake(path))),
            None => Err("package not found".to_owned()),
        }
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
        let dialect = Dialect::from_path(name.as_ref()).unwrap();
        let id = map.add(SourceFile::new(name.into(), text.to_owned(), dialect).unwrap());
        let source = map.file(id);
        files.push(parse(id, &source, names, &sink));
        sources.push(source);
    }
    let units: Vec<Unit> = files
        .iter()
        .zip(&sources)
        .map(|(file, source)| Unit { file, source })
        .collect();
    let (package, steps) = check_counted(path, &units, loader, names, &sink);
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

/// What Go's type checker or this one says about the names of one file:
/// for the offset of a declaring name its kind, and for the offset of
/// every other name what it denotes.
#[derive(Default)]
struct Names {
    defs: BTreeMap<u32, String>,
    uses: BTreeMap<u32, String>,
    /// Names that are keys of composite literals.
    keys: BTreeMap<u32, String>,
}

/// The names of each file of `package`, in the notation of
/// `tests/gotypes`.
fn names_of(package: &Package, files: &[File], names: &ThreadedRodeo) -> Vec<Names> {
    let index: HashMap<_, _> = files
        .iter()
        .enumerate()
        .map(|(i, file)| (file.id, i))
        .collect();
    let mut found: Vec<Names> = files.iter().map(|_| Names::default()).collect();
    for object in &package.objects {
        let kind = match &object.kind {
            ObjKind::Const => "const",
            ObjKind::Type => "type",
            // A blank variable is in no scope.
            ObjKind::Var if names.resolve(&object.name) == "_" => continue,
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

/// The packages of Go's standard library that are checked so far, found
/// through the import map of the package that imports them.
struct StdLoader<'a> {
    checked: &'a HashMap<String, Arc<Package>>,
    import_map: HashMap<String, String>,
}

impl PackageLoader for StdLoader<'_> {
    fn load(&self, path: &str) -> Result<Arc<Package>, String> {
        let path = self.import_map.get(path).map_or(path, String::as_str);
        let package = self.checked.get(path).ok_or("package not checked yet")?;
        Ok(Arc::clone(package))
    }
}

/// One package as `tests/gotypes` prints it.
#[derive(Default)]
struct Listed {
    path: String,
    import_map: HashMap<String, String>,
    files: Vec<(PathBuf, Names)>,
    /// The variables that each value initializes, in the order in which
    /// the values are computed.
    init_order: Vec<String>,
    /// Built into Go's type checker, which says nothing about its files.
    builtin: bool,
}

fn listed_packages(output: &str) -> Vec<Listed> {
    let mut packages: Vec<Listed> = Vec::new();
    for line in output.lines() {
        let mut words = line.split(' ');
        let record = words.next().unwrap();
        if record == "package" {
            packages.push(Listed {
                path: words.next().unwrap().to_owned(),
                ..Listed::default()
            });
            continue;
        }
        let package = packages.last_mut().unwrap();
        match record {
            "map" => {
                let written = words.next().unwrap().to_owned();
                package
                    .import_map
                    .insert(written, words.next().unwrap().to_owned());
            }
            "file" => package
                .files
                .push((words.next().unwrap().into(), Names::default())),
            "builtin" => package.builtin = true,
            "init" => package.init_order.push(words.next().unwrap().to_owned()),
            "def" | "use" => {
                let offset = words.next().unwrap().parse().unwrap();
                let rest = words.collect::<Vec<_>>().join(" ");
                let names = &mut package.files.last_mut().unwrap().1;
                let table = if record == "def" {
                    &mut names.defs
                } else {
                    &mut names.uses
                };
                table.insert(offset, rest);
            }
            _ => panic!("unknown record: {line}"),
        }
    }
    packages
}

/// The program that prints what Go's type checker resolves names to.
const GOTYPES: &str = concat!(env!("CARGO_MANIFEST_DIR"), "/../../tests/gotypes/main.go");

/// Every name in the packages of Go's standard library, as built for this
/// machine without cgo, denotes here what it denotes for Go's type checker,
/// and nothing is reported. Needs `go` on `PATH`.
#[test]
#[ignore = "long: type-checks the Go standard library with Go's checker too"]
fn names_of_the_go_standard_library_denote_what_go_s_checker_says() {
    let output = Command::new("go")
        .args(["run", GOTYPES])
        .env("CGO_ENABLED", "0")
        .output()
        .unwrap();
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
    let packages = listed_packages(std::str::from_utf8(&output.stdout).unwrap());
    assert!(packages.len() > 300, "only {} packages", packages.len());

    let names = ThreadedRodeo::new();
    let mut checked: HashMap<String, Arc<Package>> = HashMap::new();
    let mut failures = Vec::new();
    let (mut files_compared, mut names_compared, mut initializers_compared) = (0, 0, 0);
    let (mut packages_compared, mut same_order) = (0, 0);
    for listed in packages {
        let map = SourceMap::new();
        let sink = Sink::new(100);
        let mut sources = Vec::new();
        let mut files = Vec::new();
        for (path, _) in &listed.files {
            let bytes = fs::read(path).unwrap();
            let id = map.add(SourceFile::from_bytes(path.clone(), &bytes, Dialect::Go).unwrap());
            let source = map.file(id);
            files.push(parse(id, &source, &names, &sink));
            sources.push(source);
        }
        let units: Vec<Unit> = files
            .iter()
            .zip(&sources)
            .map(|(file, source)| Unit { file, source })
            .collect();
        let loader = StdLoader {
            checked: &checked,
            import_map: listed.import_map,
        };
        let package = check(&listed.path, &units, &loader, &names, &sink);
        for diagnostic in sink.finish() {
            let place = map.location(diagnostic.span);
            failures.push(format!("{place}: reported: {}", diagnostic.message));
        }

        let found = names_of(&package, &files, &names);
        let init_order: Vec<String> = package
            .init_order
            .iter()
            .map(|initializer| {
                let name = |&object| names.resolve(&package.object(object).name);
                initializer
                    .lhs
                    .iter()
                    .map(name)
                    .collect::<Vec<_>>()
                    .join(",")
            })
            .collect();
        let path = listed.path.clone();
        checked.insert(listed.path, Arc::new(package));
        if listed.builtin {
            continue;
        }
        // The same values are computed. Their order is Go's where no
        // variable needs another through a method or through the key of a
        // literal: what a selector and a key denote is not known yet.
        initializers_compared += init_order.len();
        same_order += usize::from(init_order == listed.init_order);
        packages_compared += 1;
        let sorted = |mut list: Vec<String>| {
            list.sort();
            list
        };
        if sorted(init_order.clone()) != sorted(listed.init_order.clone()) {
            failures.push(format!(
                "{path}: {} values are initialized, for go {}",
                init_order.len(),
                listed.init_order.len()
            ));
        }
        for (((path, go), ours), source) in listed.files.iter().zip(&found).zip(&sources) {
            files_compared += 1;
            names_compared += go.defs.len() + go.uses.len();
            let at = |offset: u32| {
                let place = source.line_col(offset);
                format!("{}:{}:{}", path.display(), place.line, place.column)
            };
            let none = String::from("nothing");
            for offset in go.defs.keys().chain(ours.defs.keys()) {
                let (go, ours) = (go.defs.get(offset), ours.defs.get(offset));
                if go != ours {
                    let (go, ours) = (go.unwrap_or(&none), ours.unwrap_or(&none));
                    failures.push(format!("{}: declares {ours}, for go {go}", at(*offset)));
                }
            }
            for (offset, denotes) in &go.uses {
                // Go's checker knows whether a key is a field.
                let ours = ours.uses.get(offset).or_else(|| ours.keys.get(offset));
                if ours != Some(denotes) {
                    let ours = ours.unwrap_or(&none);
                    failures.push(format!("{}: is {ours}, for go {denotes}", at(*offset)));
                }
            }
            for (offset, denotes) in &ours.uses {
                if !go.uses.contains_key(offset) {
                    failures.push(format!("{}: is {denotes}, for go nothing", at(*offset)));
                }
            }
        }
    }
    println!(
        "{files_compared} files, {names_compared} names, {initializers_compared} initial values; \
         {same_order} of {packages_compared} packages in Go's order of initialization"
    );
    assert!(
        same_order * 100 >= packages_compared * 95,
        "the order differs too often"
    );
    assert!(names_compared > 500_000, "only {names_compared} names");
    failures.dedup();
    let shown: Vec<&str> = failures.iter().take(40).map(String::as_str).collect();
    assert!(
        failures.is_empty(),
        "{} differences\n{}",
        failures.len(),
        shown.join("\n")
    );
}

#[test]
fn undefined_and_redeclared_names_are_reported_as_go_reports_them() {
    let cases: &[(&str, &[&str])] = &[
        ("func f() { _ = x }", &["a.go:2:16: undefined: x"]),
        // A name is not in scope before its declaration.
        (
            "func f() { _ = x; x := 1; _ = x }",
            &["a.go:2:16: undefined: x"],
        ),
        // A variable whose value is in error is not also reported as unused.
        ("func f() { x := x }", &["a.go:2:17: undefined: x"]),
        ("func f() { var x = y }", &["a.go:2:20: undefined: y"]),
        // The type of a parameter does not see the other parameters.
        ("func f(a int, b a) {}", &["a.go:2:17: undefined: a"]),
        (
            "var a int\nfunc a() {}",
            &[
                "a.go:3:6: a redeclared in this block",
                "    a.go:2:5: other declaration of a",
            ],
        ),
        (
            "func f(a int) (a string) { var a bool }",
            &[
                "a.go:2:16: a redeclared in this block",
                "    a.go:2:8: other declaration of a",
                "a.go:2:32: a redeclared in this block",
                "    a.go:2:8: other declaration of a",
            ],
        ),
        (
            "func f[T any](T int) {}",
            &[
                "a.go:2:15: T redeclared in this block",
                "    a.go:2:8: other declaration of T",
            ],
        ),
        (
            "func f() { a, b := 1, 2; a, b := b, a }",
            &["a.go:2:31: no new variables on left side of :="],
        ),
        (
            "func f() { a, a := 1, 2; _ = a }",
            &["a.go:2:15: a repeated on left side of :="],
        ),
        (
            "func f() { _ = _ }",
            &["a.go:2:16: cannot use _ as value or type"],
        ),
        (
            "func f(x any) { switch _ := x.(type) {} }",
            &["a.go:2:24: no new variable on left side of :="],
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
        assert_eq!(messages("a.go", &text), *expected, "{body}");
    }
}

#[test]
fn a_variable_that_is_never_read_is_reported() {
    let cases: &[(&str, &[&str])] = &[
        (
            "func f() { x := 1; var y, z int; _ = z }",
            &[
                "a.go:2:12: declared and not used: x",
                "a.go:2:24: declared and not used: y",
            ],
        ),
        // An assignment does not read the variable; `+=` and `++` do.
        (
            "func f() { x := 1; x = 2; (x) = 3 }",
            &["a.go:2:12: declared and not used: x"],
        ),
        ("func f() { x, y := 1, 2; x += 1; y++ }", &[]),
        // Parameters, results, constants and types need no use.
        (
            "func f(a int) (b int) { const c = 1; type T int; return }",
            &[],
        ),
        ("func f() { x := 1; _ = func() int { return x } }", &[]),
        (
            "func f(v any) { switch x := v.(type) { case int: case string: } }",
            &["a.go:2:24: x declared and not used"],
        ),
        (
            "func f(v any) { switch x := v.(type) { case int: case string: _ = x } }",
            &[],
        ),
        (
            "func f(xs []int) { for i, x := range xs { } }",
            &[
                "a.go:2:24: declared and not used: i",
                "a.go:2:27: declared and not used: x",
            ],
        ),
        // The key of a literal may be a variable.
        ("func f() { k := 1; _ = map[int]int{k: 2} }", &[]),
    ];
    for (body, expected) in cases {
        let text = format!("package p\n{body}\n");
        assert_eq!(messages("a.go", &text), *expected, "{body}");
    }
}

#[test]
fn an_assignment_in_a_gopp_file_counts_as_a_use() {
    // `p = 9` may write what the pointer `p` points to (ADR-0043).
    let text = "package p\nfunc f(n int) { p := &n; p = 9 }\n";
    assert_eq!(messages("a.gopp", text), [] as [&str; 0]);
}

/// A package `lib` with an exported and an unexported name.
fn with_lib(names: &ThreadedRodeo) -> Loader {
    let text = "package lib\nfunc Exported() {}\nfunc hidden() {}\nvar Value int\n";
    let lib = check_texts(
        "example.com/lib",
        &[("lib.go", text)],
        &Loader::default(),
        names,
    );
    assert_eq!(lib.messages, [] as [&str; 0]);
    let mut loader = Loader::default();
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
            &["a.go:2:8: \"example.com/lib\" imported and not used"],
        ),
        (
            "import l \"example.com/lib\"",
            &["a.go:2:8: \"example.com/lib\" imported as l and not used"],
        ),
        (
            "import . \"example.com/lib\"",
            &["a.go:2:8: \"example.com/lib\" imported and not used"],
        ),
        (
            "import \"example.com/lib\"\nvar _ = lib.Missing",
            &["a.go:3:13: undefined: lib.Missing"],
        ),
        (
            "import \"example.com/lib\"\nvar _ = lib.hidden",
            &["a.go:3:13: name hidden not exported by package lib"],
        ),
        // One error for a package that is not found, none for its names.
        (
            "import \"nowhere/x\"\nvar _ = x.Anything",
            &["a.go:2:8: could not import nowhere/x (package not found)"],
        ),
        (
            "import \"\"",
            &["a.go:2:8: invalid import path (empty string)"],
        ),
        (
            "import \"example.com/lib\"\nimport lib \"example.com/lib\"\nvar _ = lib.Value",
            &[
                "a.go:3:8: lib redeclared in this block",
                "    a.go:2:8: other declaration of lib",
                "a.go:3:8: \"example.com/lib\" imported and not used",
            ],
        ),
        (
            "import \"example.com/lib\"\nvar lib = 1",
            &[
                "a.go:2:8: \"example.com/lib\" imported and not used",
                "a.go:3:5: lib already declared through import of package lib (\"example.com/lib\")",
                "    a.go:2:8: other declaration of lib",
            ],
        ),
        (
            "import . \"example.com/lib\"\nvar Value = 1",
            &[
                "a.go:2:8: \"example.com/lib\" imported and not used",
                "a.go:3:5: Value already declared through dot-import of package lib \
                 (\"example.com/lib\")",
                "    a.go:2:8: other declaration of Value",
            ],
        ),
        // A local hides the import.
        (
            "import \"example.com/lib\"\nfunc f(lib struct{ Missing int }) int { return lib.Missing }",
            &["a.go:2:8: \"example.com/lib\" imported and not used"],
        ),
    ];
    let names = ThreadedRodeo::new();
    let loader = with_lib(&names);
    for (body, expected) in cases {
        let text = format!("package p\n{body}\n");
        let checked = check_texts("p", &[("a.go", &text)], &loader, &names);
        assert_eq!(checked.messages, *expected, "{body}");
    }
}

#[test]
fn an_import_belongs_to_its_file_and_a_declaration_to_the_package() {
    let names = ThreadedRodeo::new();
    let loader = with_lib(&names);
    let files = [
        (
            "a.go",
            "package p\nimport \"example.com/lib\"\nvar A = lib.Value + B\n",
        ),
        ("b.go", "package p\nvar B = 1\nvar _ = lib.Value\n"),
    ];
    let checked = check_texts("p", &files, &loader, &names);
    assert_eq!(checked.messages, ["b.go:3:9: undefined: lib"]);
}

#[test]
fn the_universe_of_a_gopp_file_is_not_that_of_a_go_file() {
    let body = "var a Option[int]\nvar b = NewUnique(1)\nfunc f[T Send](x Shared[T]) { move(x) }\n";
    let text = format!("package p\n{body}");
    assert_eq!(messages("a.gopp", &text), [] as [&str; 0]);
    assert_eq!(
        messages("a.go", &text),
        [
            "a.go:2:7: undefined: Option",
            "a.go:3:9: undefined: NewUnique",
            "a.go:4:10: undefined: Send",
            "a.go:4:18: undefined: Shared",
            "a.go:4:31: undefined: move",
        ]
    );

    let text = "package p\nvar p = new(int)\n";
    assert_eq!(messages("a.go", text), [] as [&str; 0]);
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
	next *Node/*@Node*/
}

type List/*=List*/[E/*=E*/ any] struct {
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
    linear::assert_near_linear(&[100, 200, 400, 800], |n| steps(nested(n)));
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
            let checked = check_texts("p", &[("a.go", &text)], &Loader::default(), &names);
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
    ];
    for (body, expected) in cases {
        let text = format!("package p\n{body}\n");
        assert_eq!(messages("a.gopp", &text), *expected, "{body}");
    }
}

/// The variables that each step of the start of the program sets.
fn init_order(text: &str) -> (Vec<String>, Vec<String>) {
    let names = ThreadedRodeo::new();
    let checked = check_texts("p", &[("a.go", text)], &Loader::default(), &names);
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
            &["a.go:2:5: initialization cycle: x refers to itself"],
        ),
        (
            "var a = b\nvar b = a",
            &[
                "a.go:2:5: initialization cycle for a",
                "    a.go:2:5: a refers to b",
                "    a.go:3:5: b refers to a",
            ],
        ),
        (
            "var x = f()\nfunc f() int { return g() }\nfunc g() int { return x }",
            &[
                "a.go:2:5: initialization cycle for x",
                "    a.go:2:5: x refers to f",
                "    a.go:3:6: f refers to g",
                "    a.go:4:6: g refers to x",
            ],
        ),
        (
            "const a = b\nconst b = a",
            &[
                "a.go:2:7: initialization cycle for a",
                "    a.go:2:7: a refers to b",
                "    a.go:3:7: b refers to a",
            ],
        ),
        // A constant that repeats the value above it needs what that needs.
        (
            "const (\n\ta = c + iota\n\tb\n\tc\n)",
            &["a.go:5:2: initialization cycle: c refers to itself"],
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
        check_texts(package, &[("a.go", &text)], &loader, &ThreadedRodeo::new()).messages
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
            "a.go:2:6: func init must have no arguments and no return values",
            "a.go:3:6: func main must have no arguments and no return values",
        ]
    );
    assert_eq!(
        check("main", "func init[T any]() {}\nfunc init()"),
        [
            "a.go:2:11: func init must have no type parameters",
            "a.go:3:6: func init must have a body",
        ]
    );
    assert_eq!(
        check("main", "var init = 1\ntype main int"),
        [
            "a.go:2:5: cannot declare init - must be func",
            "a.go:3:6: cannot declare main - must be func",
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
