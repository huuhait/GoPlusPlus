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
use crate::types::Types;
use crate::{LocalKind, ObjKind, Package, PackageLoader, Ref, Unit, check};

/// Packages by import path. With `fake_rest`, any other path is a package
/// about which nothing is known.
#[derive(Default)]
struct Loader {
    packages: HashMap<String, Arc<Package>>,
    fake_rest: bool,
    /// The types of all packages checked with this loader.
    types: std::sync::OnceLock<Types>,
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

/// What Go's type checker or this one says about the names of one file:
/// for the offset of a declaring name its kind, and for the offset of
/// every other name what it denotes.
#[derive(Default)]
struct Names {
    defs: BTreeMap<u32, String>,
    uses: BTreeMap<u32, String>,
    /// Names that are keys of composite literals.
    keys: BTreeMap<u32, String>,
    /// For the byte range of an expression with a value: its type in the
    /// one spelling for identical types, then its value if it is constant.
    types: HashMap<(u32, u32), Vec<String>>,
    /// For the byte range of a selector: the kind of what it reaches, the
    /// indices on the way to it, and whether a pointer is followed.
    selections: HashMap<(u32, u32), Vec<String>>,
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

/// The value of a constant in full: the bytes of a string, so that no rule
/// of quoting matters.
fn exact(value: &crate::constant::Value) -> String {
    match value {
        crate::constant::Value::Str(text) => {
            let hex: String = text.bytes().iter().map(|b| format!("{b:02x}")).collect();
            format!("bytes {hex}")
        }
        value => value.exact_string(),
    }
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
    /// Each package-level object: its name, its kind, its type in the one
    /// spelling for identical types, and the value of a constant.
    objects: Vec<Vec<String>>,
    /// Built into Go's type checker, which says nothing about its files.
    builtin: bool,
}

fn listed_packages(output: &str) -> Vec<Listed> {
    let mut packages: Vec<Listed> = Vec::new();
    for line in output.lines() {
        if let Some(typed) = line.strip_prefix("typ\t") {
            let mut fields = typed.split('\t');
            let mut number = || fields.next().unwrap().parse().unwrap();
            let range = (number(), number());
            let names = &mut packages.last_mut().unwrap().files.last_mut().unwrap().1;
            names
                .types
                .insert(range, fields.map(str::to_owned).collect());
            continue;
        }
        if let Some(selected) = line.strip_prefix("sel\t") {
            let mut fields = selected.split('\t');
            let mut number = || fields.next().unwrap().parse().unwrap();
            let range = (number(), number());
            let names = &mut packages.last_mut().unwrap().files.last_mut().unwrap().1;
            names
                .selections
                .insert(range, fields.map(str::to_owned).collect());
            continue;
        }
        if let Some(object) = line.strip_prefix("obj\t") {
            let fields = object.split('\t').map(str::to_owned).collect();
            packages.last_mut().unwrap().objects.push(fields);
            continue;
        }
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
    let types = Types::new(&names);
    let mut checked: HashMap<String, Arc<Package>> = HashMap::new();
    let mut failures = Vec::new();
    let (mut files_compared, mut names_compared, mut initializers_compared) = (0, 0, 0);
    let (mut packages_compared, mut same_order) = (0, 0);
    let mut objects: BTreeMap<(String, bool), usize> = BTreeMap::new();
    // How many expressions with a value Go has, and how many of them are
    // compared.
    let mut expressions = (0usize, 0usize, 0usize);
    let mut selections = (0usize, 0usize);
    let mut untyped_examples: Vec<String> = Vec::new();
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
        let package = check(&listed.path, &units, &loader, &names, &types, &sink);
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
        // The types of the declarations and the values of the constants,
        // where they are worked out already.
        for fields in listed.objects.iter().filter(|_| !listed.builtin) {
            let (name, kind) = (&fields[0], fields[1].as_str());
            let declared = names.get(name).and_then(|name| package.scope.get(&name));
            let Some(&object) = declared else {
                failures.push(format!("{path}.{name}: not declared"));
                continue;
            };
            let object = package.object(object);
            let ty = match kind {
                "type" => types.underlying(object.ty),
                _ => object.ty,
            };
            let unknown = ty == Types::INVALID
                || kind == "const" && object.value == crate::constant::Value::Unknown;
            *objects.entry((kind.to_owned(), !unknown)).or_insert(0usize) += 1;
            if unknown {
                continue;
            }
            let mut ours = vec![types.canonical(ty, &names)];
            if kind == "const" {
                ours.push(exact(&object.value));
            }
            if ours[..] != fields[2..] {
                let theirs = fields[2..].join(" = ");
                failures.push(format!(
                    "{path}.{name}: {} here, for go {theirs}",
                    ours.join(" = ")
                ));
            }
        }
        // The type of each expression, where it is worked out here and has
        // a type of its own already: a constant of no type gets its type
        // from what is around it, which is not followed yet.
        for ((info, tree), (file_path, go)) in package.files.iter().zip(&files).zip(&listed.files) {
            if listed.builtin {
                break;
            }
            // What each selector reaches. Go counts the methods of a type
            // and says of a method only sometimes whether a pointer is
            // followed, so a method is compared by the way to its type.
            selections.0 += go.selections.len();
            for (&expr, selection) in info.bodies.iter().flat_map(|body| &body.selections) {
                let span = tree.exprs[expr].span;
                let place = map.location(span);
                let Some(theirs) = go.selections.get(&(span.lo, span.hi)) else {
                    failures.push(format!("{place}: a selection here, for go none"));
                    continue;
                };
                selections.1 += 1;
                let is_field = selection.kind == crate::SelectionKind::Field;
                let kind = match selection.kind {
                    crate::SelectionKind::Field => "field",
                    crate::SelectionKind::Method => "method",
                    crate::SelectionKind::MethodExpr => "methodexpr",
                };
                let mut way: Vec<&str> = theirs[1].split(',').collect();
                if !is_field {
                    way.pop();
                }
                let path: Vec<String> = selection.path.iter().map(u32::to_string).collect();
                let same = kind == theirs[0]
                    && path == way
                    && (!is_field || selection.indirect.to_string() == theirs[2]);
                if !same {
                    failures.push(format!(
                        "{place}: {kind} {path:?} {} here, for go {theirs:?}",
                        selection.indirect
                    ));
                }
            }
            expressions.0 += go.types.len();
            for (&expr, typed) in info.bodies.iter().flat_map(|body| &body.types) {
                let span = tree.exprs[expr].span;
                if typed.ty == Types::INVALID {
                    continue;
                }
                let Some(theirs) = go.types.get(&(span.lo, span.hi)) else {
                    continue;
                };
                // A value of no type here that has one for Go: the place
                // that gives it its type is not followed yet.
                if types.is_untyped(typed.ty) && !theirs[0].starts_with("untyped ") {
                    expressions.2 += 1;
                    if untyped_examples.len() < 25 {
                        untyped_examples.push(format!("{}: {}", map.location(span), theirs[0]));
                    }
                    continue;
                }
                // The first of two results of `m[k]` is recorded by Go as
                // the pair.
                let ours = types.canonical(typed.ty, &names);
                if theirs[0].starts_with('(') && !ours.starts_with('(') {
                    continue;
                }
                // A part of the type is not worked out yet.
                if ours.contains("invalid type") {
                    continue;
                }
                expressions.1 += 1;
                let value = typed.value != crate::constant::Value::Unknown;
                let same_value = !value || theirs.get(1) == Some(&exact(&typed.value));
                if ours != theirs[0] || !same_value {
                    let place = map.location(span);
                    let (ours_value, theirs) = (exact(&typed.value), theirs.join(" = "));
                    let _ = file_path;
                    failures.push(format!(
                        "{place}: {ours} = {ours_value} here, for go {theirs}"
                    ));
                }
            }
        }
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
    assert_eq!(same_order, packages_compared, "the order differs");
    assert!(names_compared > 500_000, "only {names_compared} names");
    println!("objects by kind and whether their type is worked out: {objects:?}");
    println!("{} of {} selections compared", selections.1, selections.0);
    assert!(
        selections.1 * 100 >= selections.0 * 99,
        "too few selections are worked out"
    );
    println!(
        "{} of {} expressions compared; {} more have no type here yet, as: {untyped_examples:#?}",
        expressions.1, expressions.0, expressions.2
    );
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
                "a.go:2:39: missing return",
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
    let mut loader = Loader::default();
    let lib = check_texts("example.com/lib", &[("lib.go", text)], &loader, names);
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

// rule: LEX-7, LEX-8
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
        (
            "type B struct{ a *A }\ntype A B\nvar x A\nvar p *A = x.a",
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
    assert_eq!(messages("a.go", &text), [] as [&str; 0]);
}

#[test]
fn only_a_function_is_named_init_or_main() {
    let names = ThreadedRodeo::new();
    let text = "package main\nimport init \"example.com/lib\"\ntype main struct{}\n\
                var init int\nfunc main() {}\n";
    let found = check_texts("main", &[("a.go", text)], &with_lib(&names), &names).messages;
    assert_eq!(
        found,
        [
            "a.go:2:8: cannot import package as init - init must be a func",
            "a.go:3:6: cannot declare main - must be func",
            "a.go:4:5: cannot declare init - must be func",
        ]
    );
}

#[test]
fn an_import_path_of_a_go_file_has_go_s_characters() {
    let found = messages("a.go", "package p\nimport _ \"a!b\"\n");
    assert_eq!(
        found,
        ["a.go:2:10: invalid import path (invalid character U+0021 '!')"]
    );
    // A `.gopp` file imports headers too.
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
            &["a.go:4:7: select case must be send or receive (possibly with assignment)"],
        ),
        (
            "func f(c chan int) {\n\tselect {\n\tcase v := <-c:\n\t\t_ = v\n\tcase c <- 1:\n\t}\n}",
            &[],
        ),
        (
            "func f() {\n\tfor i := 0; i < 3; j := 1 {\n\t}\n}",
            &["a.go:3:21: syntax error: cannot declare in post statement of for loop"],
        ),
        // What cannot be ranged over gives its variable no type: the
        // variable is not also unused.
        (
            "func f() {\n\tfor x := range 1.5 {\n\t}\n}",
            &["a.go:3:17: cannot range over 1.5 (untyped float constant)"],
        ),
        (
            "func f(s []int) {\n\tfor _, x := range s {\n\t}\n}",
            &["a.go:3:9: declared and not used: x"],
        ),
    ];
    for (body, expected) in cases {
        let text = format!("package p\n{body}\n");
        assert_eq!(messages("a.go", &text), *expected, "{body}");
    }
}

#[test]
fn a_name_as_the_length_of_an_array_is_a_constant() {
    let text = "package p\nvar v int\nconst c = 2\nvar (\n\ta [n]int\n\tb [v]int\n\td [c]int\n)\n";
    assert_eq!(
        messages("a.go", text),
        [
            "a.go:5:5: undefined array length n or missing type constraint",
            "a.go:6:5: invalid array length v",
        ]
    );
}

#[test]
fn a_name_of_a_package_that_differs_in_its_capitals_is_suggested() {
    let names = ThreadedRodeo::new();
    let text = "package p\nimport \"example.com/lib\"\nvar _ = lib.EXPORTED\nvar _ = lib.Other\n";
    let found = check_texts("p", &[("a.go", text)], &with_lib(&names), &names).messages;
    assert_eq!(
        found,
        [
            "a.go:3:13: undefined: lib.EXPORTED (but have Exported)",
            "a.go:4:13: undefined: lib.Other",
        ]
    );
}

#[test]
fn the_signature_of_a_function_is_not_needed_at_the_start() {
    // `r` in the signature is an error of its own; `x` in the length of
    // the result is no need of `g`.
    let text = "package p\nvar r = newReader()\nfunc newReader() r\n\
                func g() [len(x)]int\nvar x = [2]int{}\nvar y = g\n";
    assert_eq!(messages("a.go", text), ["a.go:3:18: r is not a type"]);
}

#[test]
fn operators_are_checked_as_go_checks_them() {
    let declared = "var (\n\ta int\n\tb string\n\tt bool\n\tu uint\n\tp *int\n\tf func()\n\t\
                    s struct{ b []int }\n\ti any\n\ty uint8\n)\nconst c int8 = 100\n\
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
        (
            "p == 1",
            &["16:14: invalid operation: p == 1 (mismatched types *int and untyped int)"],
        ),
        (
            "f == f",
            &["16:9: invalid operation: f == f (func can only be compared to nil)"],
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
        ("p == nil || f != nil || i == 1 || i == nil || p != &a", &[]),
        ("c*1 - c/3 == -c && !t", &[]),
        ("y + 255 + 'a' + ^y", &[]),
        ("&struct{ x int }{1} != nil && s.b == nil", &[]),
        ("float64(a<<u) + 1.5/2 > 0", &[]),
    ];
    for (expr, expected) in cases {
        let text = format!("package p\n{declared}var _ = {expr}\n");
        let found = messages("a.go", &text);
        let found: Vec<&str> = found
            .iter()
            .map(|message| message.strip_prefix("a.go:").unwrap())
            .collect();
        assert_eq!(found, *expected, "{expr}");
    }
    // A constant of no type that is shifted gets its type from around it.
    let text = "package p\nfunc f(s uint) {\n\tvar x float64 = 1 << s\n\tvar y int64 = 1 << s\n\t\
                _, _ = x, y\n}\n";
    assert_eq!(
        messages("a.go", text),
        ["a.go:3:18: invalid operation: shifted operand 1 (type float64) must be integer"]
    );
}

#[test]
fn assignments_are_checked_as_go_checks_them() {
    let declared = "var (\n\ta int\n\tb string\n\tp *int\n\tc chan int\n\ts []int\n\t\
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
            &["16:10: invalid constant type []int"],
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
            "a, b = two()\n\ta, b = 1, \"x\"\n\tm[\"k\"] = struct{ x int }{1}\n\ts[0], *p = 1, 2",
            &[],
        ),
        (
            "c <- 1\n\ti = 1.5\n\ti = nil\n\tp = nil\n\tvar f float32 = 1\n\t_ = f",
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
        let found = messages("a.go", &text);
        let found: Vec<&str> = found
            .iter()
            .map(|message| message.strip_prefix("a.go:").unwrap())
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
        let found = messages("a.go", &text);
        let found: Vec<&str> = found
            .iter()
            .map(|message| message.strip_prefix("a.go:").unwrap_or(message))
            .collect();
        assert_eq!(found, *expected, "{body}");
    }
    // A function with named results returns them with `return` alone.
    let text = "package p\nfunc f() (n int, err error) {\n\treturn\n}\n";
    assert_eq!(messages("a.go", text), [] as [&str; 0]);
    // In a `.gopp` file, what a place takes follows other rules, which
    // are checked on their own: an assignment may write through a pointer.
    let text = "package p\nfunc f(p *int) {\n\tp = 9\n}\n";
    assert_eq!(messages("a.gopp", text), [] as [&str; 0]);
}

#[test]
fn calls_and_indexes_are_checked_as_go_checks_them() {
    let declared = "var (\n\ta int\n\tb string\n\ts []int\n\tarr [4]int\n\tm map[string]int\n\t\
                    p *[4]int\n)\nfunc f(x int, y string)\nfunc v(xs ...int)\n\
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
        let found = messages("a.go", &text);
        let found: Vec<&str> = found
            .iter()
            .map(|message| message.strip_prefix("a.go:").unwrap_or(message))
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
            "copy(s, b)",
            &[
                "15:7: invalid copy: arguments s (variable of type []int) and b (variable of type \
               string) have different element types int and uint8",
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
        ("_ = new(a)", &["15:10: a is not a type"]),
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
             _ = copy(s, s) + copy(bs, b)",
            &[],
        ),
        (
            "clear(m)\n\tclear(s)\n\tclose(c)\n\tdelete(m, b)\n\tpanic(b)\n\tprint(a, b)",
            &[],
        ),
        (
            "_, _, _ = make([]int, 2, 3), make(map[string]int), make(chan int, a)\n\t_ = new(int)",
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
        let found = messages("a.go", &text);
        let found: Vec<&str> = found
            .iter()
            .map(|message| message.strip_prefix("a.go:").unwrap_or(message))
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
        (
            "[]any{P{}, P{1, 2}, P{y: 2}, []P{{1, 2}, {x: 1}}, []*P{{1, 2}}}",
            &[],
        ),
        ("map[P]string{{1, 2}: \"a\", {3, 4}: \"b\"}", &[]),
        (
            "[]any{[...]int{5: 1, 2}, map[string]int{b: a, \"k\": 2}, []int{k: 1, 2}}",
            &[],
        ),
    ];
    for (literal, expected) in cases {
        let text = format!("package p\n{declared}func _() {{\n\t_ = {literal}\n}}\n");
        let found = messages("a.go", &text);
        let found: Vec<&str> = found
            .iter()
            .map(|message| message.strip_prefix("a.go:").unwrap_or(message))
            .collect();
        assert_eq!(found, *expected, "{literal}");
    }
}

#[test]
fn selectors_and_channels_are_checked_as_go_checks_them() {
    let declared = "type S struct {\n\tx  int\n\tUp int\n}\nfunc (S) val()  {}\nfunc (*S) ptr() {}\n\
                    type E1 struct{ f int }\ntype E2 struct{ f int }\ntype B struct {\n\tE1\n\tE2\n}\n\
                    var (\n\ts    S\n\tb    B\n\tp    *S\n\ta    int\n\tsend chan<- int\n\t\
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
        ("_ = b.f", &["24:8: ambiguous selector b.f"]),
        (
            "_ = a.x",
            &["24:8: a.x undefined (type int has no field or method x)"],
        ),
        ("S{}.ptr()", &["24:6: cannot call pointer method ptr on S"]),
        (
            "m[\"k\"].ptr()",
            &["24:9: cannot call pointer method ptr on S"],
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
        ("_ = s.x + s.Up + b.E1.f + b.E2.f + p.x + *&a", &[]),
        (
            "_ = <-recv\n\tsend <- 1\n\tf, g := S.val, (*S).ptr\n\t_, _ = f, g",
            &[],
        ),
    ];
    for (body, expected) in cases {
        let text = format!("package p\n{declared}func _() {{\n\t{body}\n}}\n");
        let found = messages("a.go", &text);
        let found: Vec<&str> = found
            .iter()
            .map(|message| message.strip_prefix("a.go:").unwrap_or(message))
            .collect();
        assert_eq!(found, *expected, "{body}");
    }
    // A method may be named in the signature of one that is declared
    // before it, and the receiver may be an alias of a pointer type.
    let text = "package p\nimport \"unsafe\"\ntype T struct{}\ntype P = *T\n\
                func (T) a([unsafe.Sizeof(T.b)]int) {}\nfunc (T) b()                       {}\n\
                func (P) c()                       {}\nfunc _() { (&T{}).c() }\n";
    let names = ThreadedRodeo::new();
    let mut loader = Loader::default();
    let stub = "package unsafe\nfunc Sizeof(x any) uintptr\n";
    let stub = check_texts("unsafe", &[("unsafe.go", stub)], &loader, &names);
    loader
        .packages
        .insert("unsafe".to_owned(), Arc::new(stub.package));
    let found = check_texts("p", &[("a.go", text)], &loader, &names).messages;
    assert_eq!(found, [] as [&str; 0]);
}

#[test]
fn what_is_asserted_or_switched_on_is_an_interface_value() {
    let declared = "type I interface{ m() }\nvar (\n\ta int\n\ti I\n\te any\n)\n";
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
        (
            "_ = i.(I)\n\t_, _ = e.(int)\n\t_ = any(a).(int)\n\t_ = any(p).(P)",
            &[],
        ),
        (
            "switch i.(type) {\n\t}\n\tswitch v := e.(type) {\n\tcase int:\n\t\t_ = v\n\t}",
            &[],
        ),
    ];
    for (body, expected) in cases {
        let text = format!("package p\n{declared}func _[P any](p P) {{\n\t{body}\n}}\n");
        let found = messages("a.go", &text);
        let found: Vec<&str> = found
            .iter()
            .map(|message| message.strip_prefix("a.go:").unwrap_or(message))
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
        messages("a.go", text),
        [
            "a.go:5:6: f() (no value) used as value",
            "a.go:6:4: too many arguments in call to f",
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
    let stub = check_texts("unsafe", &[("unsafe.go", stub)], &loader, &names);
    loader
        .packages
        .insert("unsafe".to_owned(), Arc::new(stub.package));
    let declared = "import \"unsafe\"\ntype S struct {\n\tf int\n\t*E\n}\ntype E struct{ g int }\n\
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
            "_ = unsafe.Offsetof(s.g)",
            &["17:22: invalid argument: field g is embedded via a pointer in S"],
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
                "17:20: cannot use 1 (untyped int constant) as *uint8 value in argument to \
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
        ("_ = new(1)", &["17:10: 1 is not a type"]),
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
        let found = check_texts("p", &[("a.go", &text)], &loader, &names).messages;
        let found: Vec<&str> = found
            .iter()
            .map(|message| message.strip_prefix("a.go:").unwrap_or(message))
            .collect();
        assert_eq!(found, *expected, "{body}");
    }
    assert_eq!(
        messages("a.go", "package p\ntype T T.x\n"),
        ["a.go:2:6: invalid recursive type: T refers to itself"]
    );
    // A size that an `int64` does not count, next to one that it does.
    let text = "package p\nimport \"unsafe\"\ntype E [1 << 30]complex128\nvar a [1 << 30]E\n\
                var _ = unsafe.Sizeof(a)\nvar b [1 << 20]E\nvar _ = unsafe.Sizeof(b)\n";
    assert_eq!(
        check_texts("p", &[("a.go", text)], &loader, &names).messages,
        ["a.go:5:23: a (variable of type [1073741824]E) is too large"]
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
        (
            "var _ [big]int",
            &["19:9: invalid array length big (constant unknown of int type myint)"],
        ),
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
            &["19:9: array length g() (value of type int) must be constant"],
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
        let found = messages("a.go", &text);
        let found: Vec<&str> = found
            .iter()
            .map(|message| message.strip_prefix("a.go:").unwrap_or(message))
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
        let found = messages("a.go", &text);
        let found: Vec<&str> = found
            .iter()
            .map(|message| message.strip_prefix("a.go:").unwrap_or(message))
            .filter(|&message| message != key)
            .collect();
        assert_eq!(found, *expected, "{body}");
    }
    assert_eq!(
        messages("a.go", "package p\ntype M map[M]int\n"),
        ["a.go:2:12: invalid map key type M"]
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
    // In a Go file `&T{…}` is what it is in Go.
    let text = "package p\ntype T struct{}\nvar _ = &T{}\n";
    assert_eq!(messages("a.go", text), [] as [&str; 0]);
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
		if n.next == nil {
			return total
		}
		n = n.next
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
fn a_place_of_a_gopp_file_takes_what_the_rules_of_gopp_say() {
    assert_eq!(messages("good.gopp", GOOD_GOPP), [] as [&str; 0]);
    let declared = "type User struct{ Name string }\ntype Vec struct{ X, Y int }\n\
                    func (a Vec) Add(b Vec) Vec { return a }\nfunc take(u User)    {}\n\
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
            "*n = 3",
            vec![
                "9:2: cannot assign to *n: a pointer is written without *".to_owned(),
                "    use n".to_owned(),
            ],
        ),
        (
            "*n++",
            vec![
                "9:2: cannot assign to *n: a pointer is written without *".to_owned(),
                "    use n".to_owned(),
            ],
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
    // In a Go file a member is reached through embedded fields.
    let embedded = "package p
type Inner struct{ a, b int }
func (*Inner) ptr() {}
type Outer struct {
	x int
	*Inner
}
func f(o Outer) {
	_ = o.b
	o.ptr()
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
    let expected = [
        "o.b Field from p.Outer in p.Inner by [1, 1] through a pointer",
        "o.ptr Method from p.Outer in p.Inner by [1] through a pointer",
    ];
    assert_eq!(selections("a.go", embedded, 3), expected);
}

#[test]
fn copy_writes_through_a_pointer_and_a_type_with_parents_is_left_alone() {
    let text = "package p

type Payment struct{ Amount int }

func (p *Payment) Total() int { return p.Amount }

@Extends(Payment)
type Card struct{ Number string }

func pay(p *Payment) {}

func mk() []int { return nil }

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
         type string) have different element types int and uint8",
    ];
    assert_eq!(messages("a.gopp", text), expected);
    // In a Go file `copy` takes the slice, and `&s[i:j]` has no address.
    let text = "package p\nfunc f(dst, src []int) {\n\t_ = copy(dst, src)\n\t_ = &dst[1:]\n}\n";
    assert_eq!(
        messages("a.go", text),
        ["a.go:4:7: invalid operation: cannot take address of dst[1:] (value of type []int)"]
    );
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
        "a.go:4:2: break not in for, switch, or select statement",
        "a.go:5:2: continue not in for statement",
        "a.go:6:2: fallthrough statement out of place",
        "a.go:7:7: label Missing not declared",
        "a.go:8:7: goto Inner jumps into block",
        "a.go:12:1: label Unused declared and not used",
        "a.go:14:1: label Twice already declared",
        "    a.go:13:1: other declaration of Twice",
        "a.go:16:7: goto Over jumps over variable declaration at line 17",
        "a.go:24:1: label Plain declared and not used",
        "a.go:27:9: invalid break label Plain",
        "a.go:28:12: invalid continue label Plain",
        "a.go:34:12: invalid continue label Sw",
        "a.go:37:3: cannot fallthrough final case in switch",
        "a.go:41:3: cannot fallthrough in type switch",
        "a.go:44:8: label Out not declared",
    ];
    assert_eq!(messages("a.go", text), expected);
    // The same rules hold in a `.gopp` file.
    let found = messages("a.gopp", text);
    let expected: Vec<String> = expected
        .iter()
        .map(|message| message.replace("a.go:", "a.gopp:"))
        .collect();
    assert_eq!(found, expected);
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
        let found = messages("a.go", &text);
        let found: Vec<&str> = found
            .iter()
            .map(|message| message.strip_prefix("a.go:").unwrap_or(message))
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
        "a.go:10:5: non-boolean condition in if statement",
        "a.go:12:5: non-boolean condition in if statement",
        "a.go:14:6: non-boolean condition in for statement",
        "a.go:16:8: non-boolean condition in for statement",
        "a.go:20:6: expression in go must be function call",
        "a.go:21:5: go discards result of len(sl) (value of type int)",
        "a.go:22:5: go requires function call, not conversion int(x) (value of type int)",
        "a.go:25:9: expression in defer must be function call",
        "a.go:26:8: defer discards result of len(sl) (value of type int)",
        "a.go:27:8: defer requires function call, not conversion int(x) (value of type int)",
        "a.go:32:7: duplicate case 1 (constant of type int) in expression switch",
        "    a.go:31:7: previous case",
        "a.go:33:7: duplicate case (1) (constant 1 of type int) in expression switch",
        "    a.go:31:7: previous case",
        "a.go:34:10: duplicate case 1 (constant of type int) in expression switch",
        "    a.go:31:7: previous case",
        "a.go:35:7: cannot convert \"a\" (untyped string constant) to type int",
        "a.go:36:7: invalid case s in switch on x (mismatched types string and int)",
        "a.go:38:7: duplicate case 2 (constant of type int) in expression switch",
        "    a.go:37:7: previous case",
        "a.go:42:2: multiple defaults (first at a.go:41:2)",
        "a.go:46:7: duplicate case \"foo\" (constant of type string) in expression switch",
        "    a.go:45:7: previous case",
        "a.go:47:7: cannot convert 1 (untyped int constant) to type string",
        "a.go:51:7: duplicate case 1 (constant of type int) in expression switch",
        "    a.go:50:7: previous case",
        "a.go:55:7: duplicate case byte(1) (constant 1 of type uint8) in expression switch",
        "    a.go:53:7: previous case",
        "a.go:57:7: invalid case sl in switch on i (slice can only be compared to nil)",
        "a.go:64:9: cannot switch on t (variable of struct type T) (T is not comparable)",
        "a.go:70:7: cannot convert 1 (untyped int constant) to type bool",
        "a.go:72:9: use of untyped nil in switch expression",
        "a.go:74:9: nop() (no value) used as value",
        "a.go:78:7: duplicate case 1.5 (constant of type float64) in expression switch",
        "    a.go:77:7: previous case",
        "a.go:82:7: duplicate case int in type switch",
        "    a.go:81:7: previous case",
        "a.go:84:7: duplicate case nil in type switch",
        "    a.go:83:7: previous case",
        "a.go:85:15: duplicate case int in type switch",
        "    a.go:81:7: previous case",
        "a.go:89:2: multiple defaults (first at a.go:88:2)",
    ];
    assert_eq!(messages("a.go", text), expected);
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
        "a.go:35:9: range over c (variable of type chan int) permits only one iteration variable",
        "a.go:38:12: cannot range over send (variable of type chan<- int): receive from send-only channel chan<- int",
        "a.go:50:9: range over x (variable of type int) permits only one iteration variable",
        "a.go:56:12: cannot range over 1.5 (untyped float constant)",
        "a.go:58:12: cannot range over fl (variable of type float64)",
        "a.go:60:12: cannot range over t (variable of type struct{})",
        "a.go:62:12: expected at most 2 expressions",
        "a.go:65:12: cannot use 1 << 100 (untyped int constant 1267650600228229401496703205376) as int value in range clause (overflows)",
        "a.go:70:6: range over seq0 (value of type func(func() bool)) permits no iteration variables",
        "a.go:78:9: range over seq1 (value of type func(func(int) bool)) permits only one iteration variable",
        "a.go:85:12: cannot range over seq3 (value of type func(func(int, int, int) bool)): func must be func(yield func(...) bool): yield func has too many parameters",
        "a.go:87:12: cannot range over bad1 (value of type func(func(int))): func must be func(yield func(...) bool): yield func does not return bool",
        "a.go:89:12: cannot range over bad2 (value of type func(func(int) MyBool)): func must be func(yield func(...) bool): yield func returns user-defined boolean, not bool",
        "a.go:91:12: cannot range over bad3 (value of type func(int, int)): func must be func(yield func(...) bool): wrong argument count",
        "a.go:93:12: cannot range over bad4 (value of type func(func(int) bool) int): func must be func(yield func(...) bool): unexpected results",
        "a.go:95:12: cannot range over bad5 (value of type func(int)): func must be func(yield func(...) bool): argument is not func",
        "a.go:105:6: cannot use str2 (value of type int) as string value in assignment",
        "a.go:107:9: cannot use i (value of type string) as int value in assignment",
        "a.go:111:6: cannot use iteration variable of type float64",
        "a.go:113:17: cannot use 300 (untyped int constant) as uint8 value in range clause (overflows)",
        "a.go:117:16: cannot range over fl (variable of type float64)",
        "a.go:119:6: cannot assign to 1 (neither addressable nor a map index expression)",
    ];
    assert_eq!(messages("a.go", text), expected);
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
        "a.go:11:8: ambiguous selector t.X",
        "a.go:12:10: ambiguous selector T{}.X",
        "a.go:13:6: use of .(type) outside type switch",
        "a.go:14:10: use of .(type) outside type switch",
        "a.go:16:8: no new variables on left side of :=",
        "a.go:18:6: invalid use of len (built-in) in selector expression",
        "a.go:19:10: invalid use of len (built-in) in selector expression",
        "a.go:23:3: result parameter a not in scope at return",
        "    a.go:21:3: inner declaration of var a int",
        "a.go:29:4: result parameter b not in scope at return",
        "    a.go:26:3: inner declaration of var b int",
        "a.go:32:9: not enough return values",
        "    have (int)",
        "    want (int, int)",
        "a.go:38:16: cannot use t (variable of type int) as bool value in variable declaration",
        "a.go:41:19: cannot use t (variable of type any) as float32 value in variable declaration: need type assertion",
        "a.go:44:19: cannot use t (variable of type any) as float32 value in variable declaration: need type assertion",
        "a.go:55:14: cannot use e (variable of type any) as int value in variable declaration: need type assertion",
        "a.go:56:12: cannot use i (variable of interface type I) as C value in variable declaration: need type assertion",
        "a.go:57:13: cannot use i (variable of interface type I) as *C value in variable declaration: need type assertion",
        "a.go:58:17: cannot use i (variable of interface type I) as string value in variable declaration",
        "a.go:59:10: cannot convert e (variable of type any) to type int: need type assertion",
        "a.go:60:8: cannot convert i (variable of interface type I) to type C: need type assertion",
        "a.go:61:6: cannot use i (variable of interface type I) as C value in assignment: need type assertion",
        "a.go:67:7: cannot use e (variable of type any) as C value in argument to take: need type assertion",
        "a.go:68:7: cannot use i (variable of interface type I) as C value in argument to take: need type assertion",
    ];
    assert_eq!(messages("a.go", text), expected);
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
        "a.go:42:12: cannot use T{} (value of struct type T) as J value in variable declaration: T does not implement J (missing method n)",
        "a.go:43:12: cannot use P{} (value of struct type P) as I value in variable declaration: P does not implement I (method m has pointer receiver)",
        "a.go:44:12: cannot use W{} (value of struct type W) as J value in variable declaration: W does not implement J (wrong type for method n)",
        "    have n() int",
        "    want n(int) string",
        "a.go:45:12: cannot use C{} (value of struct type C) as I value in variable declaration: C does not implement I (missing method m)",
        "    have M()",
        "    want m()",
        "a.go:46:12: cannot use F{} (value of struct type F) as I value in variable declaration: F does not implement I (F.m is a field, not a method)",
        "a.go:47:12: cannot use A{} (value of struct type A) as I value in variable declaration: A does not implement I (ambiguous selector A.m)",
        "a.go:48:12: cannot use 1 (constant of type int) as I value in variable declaration: int does not implement I (missing method m)",
        "a.go:49:12: cannot use i (variable of interface type I) as J value in variable declaration: I does not implement J (missing method n)",
        "a.go:50:13: cannot use T{} (value of struct type T) as *I value in variable declaration: T does not implement *I (type *I is pointer to interface, not interface)",
        "a.go:51:12: cannot use &i (value of type *I) as I value in variable declaration: *I does not implement I (type *I is pointer to interface, not interface)",
        "a.go:52:12: cannot use i (variable of interface type I) as T value in variable declaration: need type assertion",
        "a.go:53:12: cannot use i (variable of interface type I) as P value in variable declaration",
        "a.go:58:6: impossible type assertion: i.(P)",
        "    P does not implement I (method m has pointer receiver)",
        "a.go:59:6: impossible type assertion: j.(W)",
        "    W does not implement J (wrong type for method n)",
        "    have n() int",
        "    want n(int) string",
        "a.go:60:6: impossible type assertion: i.(C)",
        "    C does not implement I (missing method m)",
        "    have M()",
        "    want m()",
        "a.go:61:8: cannot convert P{} (value of struct type P) to type I: P does not implement I (method m has pointer receiver)",
        "a.go:62:8: cannot convert i (variable of interface type I) to type J: I does not implement J (missing method n)",
        "a.go:63:8: cannot convert i (variable of interface type I) to type T: need type assertion",
        "a.go:65:10: impossible type switch case: P",
        "    i (variable of interface type I) cannot have dynamic type P (method m has pointer receiver)",
        "a.go:65:17: impossible type switch case: C",
        "    i (variable of interface type I) cannot have dynamic type C (missing method m)",
        "    have M()",
        "    want m()",
        "a.go:66:10: impossible type switch case: F",
        "    i (variable of interface type I) cannot have dynamic type F (F.m is a field, not a method)",
        "a.go:69:6: cannot use P{} (value of struct type P) as I value in assignment: P does not implement I (method m has pointer receiver)",
        "a.go:70:4: cannot use P{} (value of struct type P) as I value in argument to g: P does not implement I (method m has pointer receiver)",
        "a.go:71:10: cannot use P{} (value of struct type P) as I value in array or slice literal: P does not implement I (method m has pointer receiver)",
        "a.go:72:11: invalid operation: i == P{} (mismatched types I and P)",
        "a.go:77:22: cannot use P{} (value of struct type P) as I value in return statement: P does not implement I (method m has pointer receiver)",
    ];
    assert_eq!(messages("a.go", text), expected);
}

// rule: OWN-P5
#[test]
fn a_pointer_and_a_handle_implement_what_their_methods_say_in_a_gopp_file() {
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
	var b I = pt
	var c I = pp
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
        "a.gopp:27:12: cannot use p (variable of struct type P) as I value in variable \
         declaration: P does not implement I (method m has pointer receiver)",
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
        "a.go:37:11: cannot use x (variable of interface type AB) as C value in variable \
         declaration: AB does not implement C (wrong type for method a)",
        "    have a() interface{A; B}",
        "    want a() interface{C}",
    ];
    assert_eq!(messages("a.go", text), expected);
}

/// The work of deciding what implements an interface grows with the
/// types, their methods and the depth of embedding, not faster.
#[test]
fn the_steps_of_looking_up_methods_grow_with_the_input_not_faster() {
    let steps = |text: String| {
        let loader = Loader::default();
        let checked = check_texts("p", &[("a.go", &text)], &loader, &ThreadedRodeo::new());
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
    // A type that has its method from a type embedded in a type embedded
    // in a type, and so on.
    let nested = |depth: usize| {
        let all: String = (1..depth)
            .map(|i| format!("type T{i} struct {{ T{} }}\n", i - 1))
            .collect();
        format!(
            "package p\ntype T0 struct{{}}\nfunc (T0) m() {{}}\n{all}\
             var _ interface {{ m() }} = T{}{{}}\n",
            depth - 1
        )
    };
    // An interface that embeds an interface that embeds an interface.
    let embedded_interfaces = |depth: usize| {
        let all: String = (1..depth)
            .map(|i| format!("type I{i} interface {{ I{}; m{i}() }}\n", i - 1))
            .collect();
        format!(
            "package p\ntype I0 interface {{ m0() }}\n{all}var x I{}\nvar _ I0 = x\n",
            depth - 1
        )
    };
    // Interfaces next to each other, each with a method and two embedded
    // interfaces.
    let sibling_interfaces = |count: usize| {
        let all: String = (0..count)
            .map(|i| format!("type I{i} interface {{ A; B; m{i}() }}\nvar _ A = I{i}(nil)\n"))
            .collect();
        format!("package p\ntype A interface {{ a() }}\ntype B interface {{ b() }}\n{all}")
    };
    let sizes = [500, 1_000, 2_000, 4_000];
    linear::assert_near_linear(&sizes, |n| steps(siblings(n)));
    linear::assert_near_linear(&sizes, |n| steps(many_methods(n)));
    linear::assert_near_linear(&sizes, |n| steps(nested(n)));
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
    // Go names an alias where it is written (`Ai`, `A10`); the type that
    // it stands for is named here.
    let expected = [
        "a.go:8:11: method T1.m already declared",
        "    a.go:7:11: other declaration of method m",
        "a.go:9:14: field and method with the same name f",
        "    a.go:4:2: other declaration of f",
        "a.go:15:12: field and method with the same name int",
        "    a.go:12:2: other declaration of int",
        "a.go:21:12: field and method with the same name T2",
        "    a.go:18:3: other declaration of T2",
        "a.go:23:12: field and method with the same name f",
        "    a.go:26:2: other declaration of f",
        "a.go:33:7: invalid receiver type T5 (pointer or interface type)",
        "a.go:34:7: invalid receiver type T5 (pointer or interface type)",
        "a.go:38:7: invalid receiver type ptr (pointer or interface type)",
        "a.go:39:8: invalid receiver type ptr (pointer or interface type)",
        "a.go:41:7: cannot define new methods on non-local type int",
        "a.go:42:7: invalid receiver type []int",
        "a.go:43:7: cannot define new methods on non-local type error",
        "a.go:44:8: cannot define new methods on non-local type error",
        "a.go:45:9: invalid receiver type any",
        "a.go:53:7: cannot define new methods on non-local type int",
        "a.go:55:11: method T0.m1 already declared",
        "    a.go:54:11: other declaration of method m1",
        "a.go:58:7: invalid receiver type **T0",
        "a.go:70:7: cannot define new methods on instantiated type G[int]",
        "a.go:71:8: cannot define new methods on instantiated type G[int]",
        "a.go:72:7: receiver declares 2 type parameters, but receiver base type declares 1",
        "a.go:73:9: receiver type parameter []int must be an identifier",
        "a.go:75:16: method G.m3 already declared",
        "    a.go:74:15: other declaration of method m3",
        "a.go:76:8: invalid receiver type **T0",
        "a.go:77:8: invalid receiver type *struct{}",
        "a.go:81:2: duplicate method m",
        "    a.go:80:2: other declaration of method m",
        "a.go:86:2: duplicate method m",
        "    a.go:87:2: other declaration of method m",
        "a.go:91:2: duplicate method m",
        "    a.go:90:2: other declaration of method m",
        "a.go:95:2: duplicate method m",
        "    a.go:94:2: other declaration of method m",
    ];
    assert_eq!(messages("a.go", text), expected);
}

// rule: TYP-25
#[test]
fn embedded_fields_and_the_names_of_fields_are_checked_as_go_checks_them() {
    let text = "package p

type T1 struct{ X int }
type T2 struct{ T1 }
type T3 struct{ T1 }
type T4 struct {
	T2
	T3
}
type U3 struct{ T2 }
type U4 struct{ T2 }
type U5 struct {
	U3
	U4
}
type V struct {
	T1
	T2
}
type W struct {
	*T2
	T3
	Y int
}

func (T1) m() {}

type I interface{ m() }

func f(t T4, u U5, v V, w W) {
	_ = v.X
	_ = w.Y
	_ = t.T2.X
	v.m()
	var _ I = v
	_ = t.X
	_ = u.X
	_ = w.X
	_ = t.T1
	_ = u.T2
	t.m()
	_ = T4.m
	var _ I = t
}

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

type I1 interface{}
type I2 interface{}
type P1 *int
type P2 *int
type L[P any] struct{}

type S1 struct {
	I1
	*T
	*I2
	*error
	P1
	*P2
}

type S2 struct {
	int8
	*int16
	L[int]
	a, b int
	_    int
	_    int

	int8
	*int16
	L[int]
	a string
}
";
    let expected = [
        "a.go:36:8: ambiguous selector t.X",
        "a.go:37:8: ambiguous selector u.X",
        "a.go:38:8: ambiguous selector w.X",
        "a.go:39:8: ambiguous selector t.T1",
        "a.go:40:8: ambiguous selector u.T2",
        "a.go:41:4: ambiguous selector t.m",
        "a.go:42:9: ambiguous selector T4.m",
        "a.go:43:12: cannot use t (variable of struct type T4) as I value in variable declaration: T4 does not implement I (ambiguous selector T4.m)",
        "a.go:55:8: T.a undefined (type T has no field or method a)",
        "a.go:56:8: T.x undefined (type T has no method x)",
        "a.go:57:8: invalid method expression T.n (needs pointer receiver (*T).n)",
        "a.go:58:8: T.y undefined (type T has no method y)",
        "a.go:59:11: (*T).y undefined (type *T has no method y)",
        "a.go:71:2: embedded field type cannot be a pointer to an interface",
        "a.go:72:2: embedded field type cannot be a pointer to an interface",
        "a.go:73:2: embedded field type cannot be a pointer",
        "a.go:74:2: embedded field type cannot be a pointer",
        "a.go:85:2: int8 redeclared",
        "    a.go:78:2: other declaration of int8",
        "a.go:86:3: int16 redeclared",
        "    a.go:79:3: other declaration of int16",
        "a.go:87:2: L redeclared",
        "    a.go:80:2: other declaration of L",
        "a.go:88:2: a redeclared",
        "    a.go:81:2: other declaration of a",
    ];
    assert_eq!(messages("a.go", text), expected);
    // A `.gopp` file refuses the embedded field itself.
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
        "a.go:15:4: cannot use type I1 outside a type constraint: interface is (or embeds) comparable",
        "a.go:16:4: cannot use type I2 outside a type constraint: interface contains type constraints",
        "a.go:17:4: cannot use type I3 outside a type constraint: interface contains type constraints",
        "a.go:18:4: cannot use type comparable outside a type constraint: interface is (or embeds) comparable",
        "a.go:19:6: cannot use type I1 outside a type constraint: interface is (or embeds) comparable",
        "a.go:20:8: cannot use type I2 outside a type constraint: interface contains type constraints",
        "a.go:21:5: cannot use type I3 outside a type constraint: interface contains type constraints",
        "a.go:22:8: cannot use type I1 outside a type constraint: interface is (or embeds) comparable",
        "a.go:22:11: cannot use type I2 outside a type constraint: interface contains type constraints",
        "a.go:23:9: cannot use type I3 outside a type constraint: interface contains type constraints",
        "a.go:24:9: cannot use type I1 outside a type constraint: interface is (or embeds) comparable",
        "a.go:25:11: cannot use type I2 outside a type constraint: interface contains type constraints",
        "a.go:27:6: cannot use type I2 outside a type constraint: interface contains type constraints",
        "a.go:28:9: cannot use type I2 outside a type constraint: interface contains type constraints",
        "a.go:29:11: cannot use type I3 outside a type constraint: interface contains type constraints",
        "a.go:30:8: cannot use type interface{int} outside a type constraint: interface contains type constraints",
        "a.go:31:6: cannot use interface I1 in conversion (contains specific type constraints or is comparable)",
        "a.go:32:6: cannot use interface I2 in conversion (contains specific type constraints or is comparable)",
        "a.go:37:2: cannot use type I2 outside a type constraint: interface contains type constraints",
        "a.go:38:4: cannot use type I1 outside a type constraint: interface is (or embeds) comparable",
        "a.go:39:2: embedded field type cannot be a pointer to an interface",
        "a.go:39:3: cannot use type I3 outside a type constraint: interface contains type constraints",
        "a.go:42:17: cannot use type I2 outside a type constraint: interface contains type constraints",
        "a.go:42:21: cannot use type I1 outside a type constraint: interface is (or embeds) comparable",
        "a.go:43:9: cannot use type I3 outside a type constraint: interface contains type constraints",
        "a.go:45:7: cannot use type I2 outside a type constraint: interface contains type constraints",
        "a.go:47:8: cannot use type I3 outside a type constraint: interface contains type constraints",
        "a.go:48:10: cannot use type I1 outside a type constraint: interface is (or embeds) comparable",
        "a.go:49:13: cannot use type I2 outside a type constraint: interface contains type constraints",
    ];
    assert_eq!(messages("a.go", text), expected);
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
        "a.go:10:15: invalid use of ~ (underlying type of MyInt is int)",
        "a.go:11:15: invalid use of ~ (M is an interface)",
        "a.go:12:21: cannot use p.M in union (p.M contains methods)",
        "a.go:13:21: cannot use comparable in union",
        "a.go:14:21: cannot use p.C in union (p.C embeds comparable)",
        "a.go:16:21: overlapping terms int and int",
        "a.go:17:21: overlapping terms ~int and int",
        "a.go:18:22: overlapping terms p.MyInt and ~int",
        "a.go:19:22: overlapping terms ~int and ~int",
        "a.go:21:15: invalid use of ~ (comparable is an interface)",
        "a.go:22:15: invalid use of ~ (any is an interface)",
        "a.go:25:17: cannot use a type parameter as constraint",
        "a.go:26:28: term cannot be a type parameter",
        "a.go:27:29: type in term ~P cannot be a type parameter",
        "a.go:28:34: term cannot be a type parameter",
        "a.go:29:35: type in term ~P cannot be a type parameter",
        "a.go:30:18: type in term ~P cannot be a type parameter",
        "a.go:31:23: term cannot be a type parameter",
        "a.go:33:10: invalid use of ~ (underlying type of MyInt is int)",
    ];
    assert_eq!(messages("a.go", text), expected);
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
    let checked = check_texts("p", &[("a.go", text)], &loader, &names);
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
            terms("~int | ~float64 | ~string | []uint8"),
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
        "a.go:36:6: string does not satisfy Num (string missing in ~int | ~float64)",
        "a.go:37:6: MyInt does not satisfy Exact (possibly missing ~ for int in Exact)",
        "a.go:38:11: []int does not satisfy comparable",
        "a.go:39:11: S does not satisfy comparable",
        "a.go:42:6: int does not satisfy Str (missing method String)",
        "a.go:43:6: MyInt does not satisfy NumStr (missing method String)",
        "a.go:44:6: cannot satisfy Nothing (empty type set)",
        "a.go:45:6: func() does not satisfy comparable",
        "a.go:46:4: not enough type arguments for type M: have 1, want 2",
        "a.go:47:4: too many type arguments for type M: have 3, want 2",
        "a.go:48:6: cannot use type Num outside a type constraint: interface contains type constraints",
        "a.go:50:6: T does not satisfy interface{m() T} (missing method m)",
        "a.go:59:10: P does not satisfy Exact",
        "a.go:61:10: P does not satisfy Str (missing method String)",
        "a.go:69:2: string does not satisfy Num (string missing in ~int | ~float64)",
        "a.go:70:4: string does not satisfy Num (string missing in ~int | ~float64)",
        "a.go:72:9: int does not satisfy Str (missing method String)",
        "a.go:73:14: got 3 type arguments but want 2",
        "a.go:74:2: []int does not satisfy comparable",
    ];
    assert_eq!(messages("a.go", text), expected);
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
        "a.go:22:8: in call to f1, type string of s does not match inferred type int for P",
        "a.go:24:8: in call to f1, mismatched types untyped int and untyped string (cannot infer P)",
        "a.go:26:5: cannot use 1.5 (untyped float constant) as int value in argument to f1 (truncated)",
        "a.go:27:2: in call to f1, cannot infer P",
        "    a.go:6:9: P declared here",
        "a.go:28:9: cannot use i (variable of type int) as MyInt value in argument to f1",
        "a.go:29:5: cannot use i (variable of type int) as MyInt value in argument to f1",
        "a.go:32:6: in call to f2, S (type int) does not satisfy ~[]E",
        "a.go:34:17: cannot use f2(l) (value of type int) as string value in variable declaration",
        "a.go:36:9: in call to f3, type List of l does not match map[K]V (cannot infer K and V)",
        "a.go:37:2: in call to f4, cannot infer Q",
        "    a.go:9:12: Q declared here",
        "a.go:39:6: in call to f4[int], cannot infer Q",
        "    a.go:9:12: Q declared here",
        "a.go:40:6: in call to f4[string], cannot infer Q",
        "    a.go:9:12: Q declared here",
        "a.go:42:8: in call to f5, type string of s does not match inferred type int for P",
        "a.go:44:2: in call to f5, cannot infer P",
        "    a.go:10:9: P declared here",
        "a.go:47:19: in call to f6, type string of s does not match inferred type int for P",
        "a.go:49:2: in call to f7, P (type int) does not satisfy interface{m() Q} (missing method m)",
        "a.go:51:2: in call to f8, P (type int) does not satisfy *Q",
        "a.go:54:2: float64 does not satisfy interface{~int | ~string} (float64 missing in ~int | ~string)",
        "a.go:57:10: cannot use s (variable of type string) as int value in argument to f1[int]",
        "a.go:57:13: cannot use s (variable of type string) as int value in argument to f1[int]",
    ];
    assert_eq!(messages("a.go", text), expected);
}

// rule: OWN-P5
#[test]
fn a_pointer_argument_is_read_as_what_it_points_to_where_its_type_does_not_fit() {
    let text = "package p

type List []int

func first[S ~[]E, E any](s S) E { return s[0] }
func same[P any](a, b P)         {}
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
        let checked = check_texts("p", &[("a.go", &text)], &loader, &ThreadedRodeo::new());
        assert_eq!(checked.messages, [] as [&str; 0]);
        checked.steps + loader.types.get().unwrap().steps()
    };
    let header = "package p\nfunc first[S ~[]E, E any](s S) E { return s[0] }\n\
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
        "a.go:22:6: invalid operation: operator % not defined on n (variable of type N constrained by Num)",
        "a.go:23:6: invalid operation: operator + not defined on t (variable of type T constrained by Text)",
        "a.go:24:6: invalid operation: t == t (incomparable types in type set)",
        "a.go:25:6: invalid operation: operator + not defined on a (variable of type A constrained by Any)",
        "a.go:26:6: invalid operation: a == a (incomparable types in type set)",
        "a.go:27:6: invalid operation: a < a (type parameter A cannot use operator <)",
        "a.go:28:6: invalid operation: n + s (mismatched types N and S)",
        "a.go:30:11: invalid operation: n == \"x\" (mismatched types N and untyped string)",
        "a.go:31:7: invalid operation: operator ! not defined on n (variable of type N constrained by Num)",
        "a.go:32:6: invalid operation: operator && not defined on n (variable of type N constrained by Num)",
        "a.go:33:11: invalid operation: n == nil (mismatched types N and untyped nil)",
        "a.go:38:6: cannot use 1.5 (untyped float constant) as N value in assignment",
        "a.go:40:6: cannot use 1.5 (untyped float constant) as I value in assignment",
        "a.go:41:6: cannot use \"s\" (untyped string constant) as N value in assignment",
        "a.go:43:6: cannot use nil as N value in assignment",
        "a.go:44:6: cannot use nil as A value in assignment",
        "a.go:46:6: cannot use i (variable of type I constrained by ~int) as N value in assignment",
        "a.go:47:6: cannot use i (variable of type I constrained by ~int) as int value in assignment",
        "a.go:48:6: cannot use n (variable of type N constrained by Num) as int value in assignment",
        "a.go:49:6: cannot use x (variable of type int) as I value in assignment",
        "a.go:51:27: cannot use n (variable of type N constrained by Num) as interface{m()} value in variable declaration: cannot assign int (in N) to interface{m()}",
        "a.go:54:18: cannot use n (variable of type N constrained by Num) as float64 value in variable declaration",
        "a.go:59:7: invalid operation: pointers of q (variable of type Q constrained by Ptr) must have identical base types",
        "a.go:60:7: invalid operation: pointers of r (variable of type R constrained by ~*int | ~*string) must have identical base types",
        "a.go:61:7: invalid operation: cannot indirect n (variable of type N constrained by Num)",
        "a.go:62:7: invalid operation: cannot indirect a (variable of type A constrained by any)",
        "a.go:69:4: invalid operation: cannot send to d (variable of type D constrained by Chans): receive-only channel <-chan int",
        "a.go:70:8: invalid operation: cannot receive from e (variable of type E constrained by chan int | chan string): channels chan int and chan string have different element types",
        "a.go:71:8: invalid operation: cannot receive from f (variable of type F constrained by chan<- int | <-chan int): send-only channel chan<- int",
        "a.go:72:8: invalid operation: cannot receive from n (variable of type N constrained by Num): non-channel int",
        "a.go:73:4: invalid operation: cannot send to a (variable of type A constrained by any): no specific channel type",
        "a.go:74:6: invalid operation: z == z (empty type set)",
        "a.go:75:6: invalid operation: operator + not defined on z (variable of type Z constrained by Empty with empty type set)",
        "a.go:76:4: invalid operation: cannot receive from z (variable of type Z constrained by Empty with empty type set): no specific channel type",
    ];
    assert_eq!(messages("a.go", text), expected);
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
        "a.go:13:17: cannot use generic function f1 without instantiation",
        "a.go:14:17: type func(int) of v2 does not match func() P (cannot infer P)",
        "a.go:22:26: inferred type func(int, int) for func(P, P) does not match type func(int, string) of v7",
        "a.go:24:26: inferred type func(string) []string for func(P) []P does not match type func(string) []int of v9",
        "a.go:27:23: type func(int) of _ does not match func() P (cannot infer P)",
        "a.go:35:8: type func() int of v1_ does not match func(P) (cannot infer P)",
        "a.go:36:7: type func(int) of v2 does not match func() P (cannot infer P)",
        "a.go:44:7: inferred type func(int, int) for func(P, P) does not match type func(int, string) of v7",
        "a.go:46:7: inferred type func(string) []string for func(P) []P does not match type func(string) []int of v9",
        "a.go:50:9: inferred type func(string) []string for func(P) []P does not match type func(string) []int of a[0]",
        "a.go:61:9: inferred type func(int, int) for func(P, P) does not match type func(int, string) of result variable",
        "a.go:65:9: inferred type func(string) []string for func(P) []P does not match type func(string) []int of result variable",
        "a.go:69:40: type func(int) of result variable does not match func() P (cannot infer P)",
        "a.go:81:5: in call to g1, type func[P any]() P of f2 does not match func(int) (cannot infer P)",
    ];
    assert_eq!(messages("a.go", text), expected);
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
        "a.go:5:39: overlapping terms map[string]int and map[string]int",
        "a.go:15:8: cannot use 1 (untyped int constant) as string value in map index",
        "a.go:16:6: cannot index x (variable of type X constrained by Mixed)",
        "a.go:17:6: cannot index o (variable of type O constrained by Other)",
        "a.go:18:6: cannot index a (variable of type A constrained by any)",
        "a.go:19:6: cannot index n (variable of type N constrained by ~int)",
        "a.go:21:2: cannot assign to b[0] (neither addressable nor a map index expression)",
        "a.go:24:12: invalid operation: 3-index slice of string",
        "a.go:25:6: cannot slice s (variable of type S constrained by Seq): []int and [4]int have different underlying types",
        "a.go:26:6: cannot slice o (variable of type O constrained by Other): []int and []string have different underlying types",
        "a.go:27:6: cannot slice a (variable of type A constrained by any): no specific type in A",
        "a.go:28:6: cannot slice x (variable of type X constrained by Mixed): []int and map[int]int have different underlying types",
        "a.go:33:12: cannot range over b (variable of type B constrained by Bytes): string and []uint8 have different underlying types",
        "a.go:35:20: cannot range over s (variable of type S constrained by Seq): []int and [4]int have different underlying types",
        "a.go:38:12: cannot range over n (variable of type N constrained by ~int | ~int64): int and int64 have different underlying types",
        "a.go:46:12: cannot range over d (variable of type D constrained by chan<- int): receive from send-only channel chan<- int",
        "a.go:48:12: cannot range over a (variable of type A constrained by any): no specific type",
        "a.go:50:12: cannot range over z (variable of type Z constrained by int | string): int and string have different underlying types",
    ];
    assert_eq!(messages("a.go", text), expected);
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
        "a.go:19:8: p.f undefined (type P has no field or method f)",
        "a.go:20:8: q.f undefined (type Q has no field or method f)",
        "a.go:21:4: r.m undefined (type R has no field or method m)",
        "a.go:22:8: r.f undefined (type R has no field or method f)",
        "a.go:23:4: t.m undefined (type T has no field or method m)",
        "a.go:24:8: t.f undefined (type T has no field or method f)",
        "a.go:25:5: pp.m undefined (type *P is pointer to type parameter, not type parameter)",
        "a.go:26:5: pq.m undefined (type *Q is pointer to type parameter, not type parameter)",
        "a.go:27:9: pp.f undefined (type *P is pointer to type parameter, not type parameter)",
    ];
    assert_eq!(messages("a.go", text), expected);
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
        "a.go:18:8: cannot convert 1.5 (untyped float value) to type N: cannot convert 1.5 (untyped float constant) to type int (in N)",
        "a.go:20:8: cannot convert 1.5 (untyped float value) to type I: cannot convert 1.5 (untyped float constant) to type int (in I)",
        "a.go:21:8: cannot convert \"x\" (untyped string value) to type I: cannot convert \"x\" (untyped string constant) to type int (in I)",
        "a.go:22:8: cannot convert 1 (untyped int value) to type A: A does not contain specific types",
        "a.go:23:8: cannot convert s (variable of type S constrained by ~string) to type I: cannot convert string (in S) to type int (in I)",
        "a.go:24:8: cannot convert n (variable of type N constrained by Num) to type S: cannot convert float64 (in N) to type string (in S)",
        "a.go:25:8: cannot convert x (variable of type int) to type P: cannot convert int to type *int (in P)",
        "a.go:26:16: cannot convert p (variable of type P constrained by ~*int) to type *string: cannot convert *int (in P) to type *string",
        "a.go:27:10: cannot convert s (variable of type S constrained by ~string) to type int: cannot convert string (in S) to type int",
        "a.go:29:8: cannot convert a (variable of type A constrained by any) to type I",
        "a.go:30:8: cannot convert i (variable of type I constrained by Int) to type A",
        "a.go:35:10: invalid argument: n (variable of type N constrained by ~int) for built-in len",
        "a.go:36:10: invalid argument: a (variable of type A constrained by any) for built-in len",
        "a.go:37:10: invalid argument: m (variable of type M constrained by ~map[string]int) for built-in cap",
        "a.go:38:10: invalid argument: b (variable of type B constrained by ~string | ~[]uint8) for built-in cap",
        "a.go:42:8: invalid argument: cannot clear b (variable of type B constrained by ~string | ~[]uint8): argument must be (or constrained by) map or slice",
        "a.go:43:8: invalid argument: cannot clear a (variable of type A constrained by any): argument must be (or constrained by) map or slice",
        "a.go:45:8: invalid operation: cannot close receive-only channel r (variable of type R constrained by ~<-chan int)",
        "a.go:46:8: invalid operation: cannot close non-channel s (variable of type S constrained by ~[]int)",
        "a.go:47:8: invalid operation: cannot close non-channel a (variable of type A constrained by any)",
        "a.go:51:14: invalid copy: argument must be a slice; have b (variable of type B constrained by ~string | ~[]uint8)",
        "a.go:52:11: invalid copy: argument must be a slice; have a (variable of type A constrained by any)",
        "a.go:54:12: cannot use 1 (untyped int constant) as string value in argument to delete",
        "a.go:55:9: invalid argument: x (variable of type X constrained by []int | map[int]int) is not a map",
        "a.go:56:9: invalid argument: maps of y (variable of type Y constrained by map[int]int | map[string]int) must have identical key types",
        "a.go:57:9: invalid argument: s (variable of type S constrained by ~[]int) is not a map",
        "a.go:61:6: invalid operation: make(S) expects 2 or 3 arguments; found 1",
        "a.go:62:11: invalid argument: cannot make X: []int and map[int]int have different underlying types",
        "a.go:63:11: invalid argument: cannot make N: type must be slice, map, or channel",
        "a.go:64:11: invalid argument: cannot make A: no specific type",
        "a.go:68:13: cannot convert 2.5 (untyped float constant) to type N",
        "a.go:69:10: invalid argument: k (variable of type K constrained by ~complex128) cannot be ordered",
        "a.go:70:13: invalid argument: mismatched types N (previous argument) and F (type of f)",
        "a.go:71:10: invalid argument: a (variable of type A constrained by any) cannot be ordered",
        "a.go:72:11: k (variable of type K constrained by ~complex128) not supported as argument to built-in real for go1.18 (see go.dev/issue/50937)",
        "a.go:73:14: f (variable of type F constrained by ~float64) not supported as argument to built-in complex for go1.18 (see go.dev/issue/50937)",
        "a.go:74:11: invalid argument: argument has type N, expected complex type",
    ];
    assert_eq!(messages("a.go", text), expected);
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
    // Go lets a name be declared again.
    assert_eq!(messages("a.go", text), [] as [&str; 0]);
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

func f(t T, p *T, i I, c chan I, m map[any]I) I {
	var a I = t
	a = p
	a = i
	a = nil
	use(t, 1, \"s\", i, nil)
	c <- t
	m[2.5] = p
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
        "14: p: *T to I",
        "17: t: T to I",
        "17: 1: int to any",
        "17: \"s\": string to any",
        "18: t: T to I",
        "19: 2.5: float64 to any",
        "19: p: *T to I",
        "20: t: T to I",
        "21: t: T to I",
        "23: a: T to I",
        "26: t: T to I",
    ];
    assert_eq!(iface_conversions("a.go", text), expected);
}

// rule: OWN-P5, OWN-P6
#[test]
fn a_gopp_body_records_what_of_a_pointer_or_a_handle_is_put_into_an_interface() {
    let text = "package p

type I interface{ m() }
type T struct{}

func (T) m() {}

type P struct{}

func (p *P) m() {}

func f(t T, pt *T, pp *P, u Unique[P], s Shared[T], o Option[I]) {
	var a I = t
	a = pt
	a = pp
	a = u
	a = s
	o = t
	o = a
	o = o
	_ = a
}
";
    // A pointer stays a pointer where that fits; a handle lends what it
    // owns.
    let expected = [
        "13: t: T to I",
        "14: pt: *T to I",
        "15: pp: *P to I",
        "16: u: *P to I",
        "17: s: *T to I",
        "18: t: T to I",
    ];
    assert_eq!(iface_conversions("a.gopp", text), expected);
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
        // What a method needs, what names the method needs: called, as a
        // value, as a function of its receiver, through an embedded field.
        (
            "type T struct{}\nfunc (T) m() int { return b }\nvar a = T{}.m()\nvar b = 1",
            &["b", "a"],
        ),
        (
            "type T struct{}\nfunc (*T) m() int { return b }\nvar a = (*T).m\nvar b = 1",
            &["b", "a"],
        ),
        (
            "type T struct{}\nfunc (T) m() int { return b }\ntype E struct{ T }\n\
             var a = E{}.m\nvar b = 1",
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
        (
            "type T struct{}\nfunc (T) m() int { return x }\nvar x = T{}.m()",
            &[
                "a.go:4:5: initialization cycle for x",
                "    a.go:4:5: x refers to m",
                "    a.go:3:10: m refers to x",
            ],
        ),
        (
            "type T struct{}\nfunc (T) m() { _ = y }\nvar x = T.m\nvar y = x",
            &[
                "a.go:4:5: initialization cycle for x",
                "    a.go:4:5: x refers to m",
                "    a.go:3:10: m refers to y",
                "    a.go:5:5: y refers to x",
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
	head *List[E]
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
        "Bytes: []uint8",
        "Celsius: p.Celsius (float64)",
        "GB: untyped int = 1073741824",
        "KB: untyped int = 1024",
        "Less: untyped bool = true",
        "List: p.List (struct{head *p.List[E]; item E})",
        "MB: untyped int = 1048576",
        "Map: func([]T, func(T) U) []U",
        "Mask: uint8 = 240",
        "Name: untyped string = \"gopp\"",
        "Node: p.Node (struct{Name string \"json:\\\"name\\\"\"; next Option[*p.Node]; owner \
         Shared[p.Node]; Size int @Required})",
        "Open: func(string, ...int) (Unique[p.Node], error)",
        "Reader: p.Reader (interface{Close(); Read(*[]uint8) (int, error)})",
        "Size: int = 5",
        "Third: untyped float = 1/3",
        "Twice: p.Celsius (float64) = 200",
        "Typed: int8 = 127",
        "ints: p.List[int] (struct{head *p.List[int]; item int})",
        "table: map[string][]p.Node",
    ];
    assert_eq!(lines, expected);
}

#[test]
fn the_size_of_a_type_that_holds_itself_is_not_asked_forever() {
    let text = "package p\nimport \"unsafe\"\ntype T struct{ T }\nvar t T\n\
                const size = unsafe.Sizeof(t)\nvar same = t == t\n";
    let names = ThreadedRodeo::new();
    let mut loader = Loader::default();
    let stub = "package unsafe\nfunc Sizeof(x any) uintptr\n";
    let stub = check_texts("unsafe", &[("unsafe.go", stub)], &loader, &names);
    loader
        .packages
        .insert("unsafe".to_owned(), Arc::new(stub.package));
    let found = check_texts("p", &[("a.go", text)], &loader, &names).messages;
    assert_eq!(
        found,
        ["a.go:3:6: invalid recursive type: T refers to itself"]
    );
}

#[test]
fn a_name_that_is_no_type_is_refused_as_one() {
    let text = "package p\nconst limit = 3\nvar x limit\nvar y len\nfunc f(a g) {}\nfunc g() {}\n";
    let (_, messages) = declared("a.go", text);
    assert_eq!(
        messages,
        [
            "a.go:3:7: limit is not a type",
            "a.go:4:7: len is not a type",
            "a.go:5:10: g is not a type",
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
        "raw: []uint8",
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
	_, _, _, _ = ok, found, has, add
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
        "first: uint8",
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
    let checked = check_texts("p", &[("a.go", text)], &loader, &names);
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

/// The packages of Go's standard library, as built for this machine without
/// cgo, checked here. Needs `go` on `PATH`.
fn standard_library(names: &ThreadedRodeo, types: &Types) -> HashMap<String, Arc<Package>> {
    let format = "{{.ImportPath}}\t{{.Dir}}\t{{join .GoFiles \",\"}}\t\
                  {{range $from, $to := .ImportMap}}{{$from}}={{$to}},{{end}}";
    let output = Command::new("go")
        .args(["list", "-deps", "-f", format, "std"])
        .env("CGO_ENABLED", "0")
        .output()
        .unwrap();
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
    let mut checked: HashMap<String, Arc<Package>> = HashMap::new();
    for line in std::str::from_utf8(&output.stdout).unwrap().lines() {
        let fields: Vec<&str> = line.split('\t').collect();
        let (path, dir) = (fields[0], PathBuf::from(fields[1]));
        let import_map = fields[3]
            .split_terminator(',')
            .filter_map(|pair| pair.split_once('='))
            .map(|(from, to)| (from.to_owned(), to.to_owned()))
            .collect();
        let map = SourceMap::new();
        let sink = Sink::new(100);
        let mut sources = Vec::new();
        let mut files = Vec::new();
        for name in fields[2].split_terminator(',') {
            let file = dir.join(name);
            let bytes = fs::read(&file).unwrap();
            let id = map.add(SourceFile::from_bytes(file, &bytes, Dialect::Go).unwrap());
            let source = map.file(id);
            files.push(parse(id, &source, names, &sink));
            sources.push(source);
        }
        let units: Vec<Unit> = files
            .iter()
            .zip(&sources)
            .map(|(file, source)| Unit { file, source })
            .collect();
        let loader = StdLoader {
            checked: &checked,
            import_map,
        };
        let package = check(path, &units, &loader, names, types, &sink);
        checked.insert(path.to_owned(), Arc::new(package));
    }
    checked
}

/// The program that compares reported errors with the errors that Go's test
/// files expect.
const GOERRORS: &str = concat!(env!("CARGO_MANIFEST_DIR"), "/../../tests/goerrors/main.go");

/// How many of the errors that the test files of Go's type checker expect
/// are reported here, at the least. It only grows.
const EXPECTED_ERRORS_REPORTED: usize = 2621;

/// The test files of Go's type checker say where Go reports which error.
/// Nothing is reported here that they do not expect; what they expect and
/// is not reported yet is counted. Needs `go` on `PATH`.
#[test]
#[ignore = "long: checks Go's standard library and the test files of Go's type checker"]
fn errors_are_reported_where_go_s_test_files_expect_them() {
    let names = ThreadedRodeo::new();
    let types = Types::new(&names);
    let loader = Loader {
        packages: standard_library(&names, &types),
        ..Loader::default()
    };
    let goroot = Command::new("go").args(["env", "GOROOT"]).output().unwrap();
    let goroot = String::from_utf8(goroot.stdout).unwrap();
    let testdata = PathBuf::from(goroot.trim()).join("src/internal/types/testdata");

    let sorted = |dir: &std::path::Path| {
        let mut entries: Vec<PathBuf> = fs::read_dir(dir)
            .unwrap()
            .map(|entry| entry.unwrap().path())
            .collect();
        entries.sort();
        entries
    };
    let mut input = String::new();
    let mut packages = 0;
    for dir in ["check", "examples", "fixedbugs", "spec"] {
        for entry in sorted(&testdata.join(dir)) {
            // A directory is one package.
            let paths = match entry.is_dir() {
                true => sorted(&entry),
                false => vec![entry.clone()],
            };
            let texts: Vec<Vec<u8>> = paths.iter().map(|path| fs::read(path).unwrap()).collect();
            // The first line may set what the file is checked with: an
            // older version of Go, or a switch that is off by default.
            let first = texts[0].split(|&byte| byte == b'\n').next().unwrap();
            let flags = String::from_utf8_lossy(first);
            let flags = flags.strip_prefix("//").map_or("", str::trim);
            let other_language = ["-lang=", "-gotypesalias=0", "-goexperiment", "-fakeImportC"]
                .iter()
                .any(|flag| flags.starts_with('-') && flags.contains(flag));
            if other_language {
                continue;
            }
            packages += 1;
            let map = SourceMap::new();
            let sink = Sink::new(10_000);
            let mut sources = Vec::new();
            let mut files = Vec::new();
            for (path, text) in paths.iter().zip(&texts) {
                input += &format!("file\t{}\n", path.display());
                let id = map.add(SourceFile::from_bytes(path.clone(), text, Dialect::Go).unwrap());
                let source = map.file(id);
                files.push(parse(id, &source, &names, &sink));
                sources.push(source);
            }
            let units: Vec<Unit> = files
                .iter()
                .zip(&sources)
                .map(|(file, source)| Unit { file, source })
                .collect();
            let name = entry.file_stem().unwrap().to_str().unwrap();
            check(name, &units, &loader, &names, &types, &sink);
            for diagnostic in sink.finish() {
                // `assert` and `trace` only exist in Go's own test run.
                if ["undefined: assert", "undefined: trace"].contains(&&*diagnostic.message) {
                    continue;
                }
                let source = map.file(diagnostic.span.file);
                let place = source.line_col(diagnostic.span.lo);
                // As Go prints an error: what belongs to it on lines of
                // its own, after a tab.
                let mut message = diagnostic.message.clone();
                for note in &diagnostic.notes {
                    message += "\n\t";
                    message += &note.message;
                }
                let escaped = message
                    .replace('\\', "\\\\")
                    .replace('\n', "\\n")
                    .replace('\t', "\\t");
                input += &format!(
                    "err\t{}\t{}\t{}\t{escaped}\n",
                    source.path().display(),
                    place.line,
                    place.column,
                );
            }
        }
    }

    let mut compare = Command::new("go")
        .args(["run", GOERRORS])
        .stdin(std::process::Stdio::piped())
        .stdout(std::process::Stdio::piped())
        .spawn()
        .unwrap();
    // The program reads all of its input before it prints.
    std::io::Write::write_all(&mut compare.stdin.take().unwrap(), input.as_bytes()).unwrap();
    let output = compare.wait_with_output().unwrap();
    assert!(output.status.success());
    let report = String::from_utf8(output.stdout).unwrap();
    // An alias is written here as the type it stands for, and a function
    // type without the names of its parameters.
    let known = [
        "fixedbugs/issue67628.go:16:6: invalid operation: b + \"foo\" (mismatched types int",
        "fixedbugs/issue46583.go:24:7: cannot use T1.m (value of type func(T1, int))",
        "fixedbugs/issue46583.go:25:7: cannot use T2.m (value of type func(T2, int))",
        "fixedbugs/issue46583.go:26:7: cannot use T3.m (value of type func(T3, int))",
        "fixedbugs/issue46583.go:27:7: cannot use T4.m (value of type func(T4, int))",
        "fixedbugs/issue69955.go:36:6: a (variable of struct type S) is not constant",
        "fixedbugs/issue69955.go:37:6: b (variable of type int) is not constant",
        "fixedbugs/issue69955.go:38:6: c (variable of int type N) is not constant",
        "fixedbugs/issue59740.go:24:25: in call to cons, type func(T, List[T]) T of nil[T]",
        "fixedbugs/issue60933.go:34:10: in call to g, type interface{Write([]uint8) (int, error)}",
        "fixedbugs/issue60933.go:35:12: in call to g, type *os.File of file does not match",
        // A type parameter that is named like the type next to it is not
        // told apart by where it is declared.
        "fixedbugs/issue58611.go:13:6: cannot use 0 (untyped int constant) as int value",
        "fixedbugs/issue58611.go:25:15: cannot use keys (variable of type []string) as []string",
        "fixedbugs/issue58611.go:26:61: cannot use keys (variable of type []string) as []string",
    ];
    let (mut ok, mut missing) = (0, 0);
    let mut wrong = Vec::new();
    for line in report.lines() {
        match line.split_once(' ').unwrap() {
            ("ok", count) => ok = count.parse().unwrap(),
            ("missing", _) => missing += 1,
            _ if known.iter().any(|known| line.contains(known)) => {}
            _ => wrong.push(line),
        }
    }
    if let Ok(path) = std::env::var("GOPP_ERRORS_REPORT") {
        fs::write(path, &report).unwrap();
    }
    println!("{packages} packages: {ok} expected errors reported, {missing} not yet");
    let shown: Vec<&str> = wrong.iter().take(40).copied().collect();
    assert!(
        wrong.is_empty(),
        "{} errors that Go does not report there\n{}",
        wrong.len(),
        shown.join("\n")
    );
    assert!(
        ok >= EXPECTED_ERRORS_REPORTED,
        "only {ok} expected errors reported"
    );
}
