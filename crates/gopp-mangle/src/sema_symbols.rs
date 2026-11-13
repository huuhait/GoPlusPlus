//! The names of what the checker knows: one name for one type and one
//! entity, whatever the order of the work.

use std::collections::{BTreeSet, HashMap};
use std::sync::Arc;

use gopp_diag::Sink;
use gopp_sema::types::{Type, TypeId, Types};
use gopp_sema::{ObjId, ObjKind, Package, PackageLoader, Unit, check};
use gopp_span::{Dialect, SourceFile, SourceMap};
use gopp_syntax::parser::parse;
use gopp_testutil::linear::assert_near_linear;
use lasso::ThreadedRodeo;
use rand::rngs::ChaCha8Rng;
use rand::{RngExt, SeedableRng};

use crate::node::{Node, Required};
use crate::sema::{Entity, Namer, NoSymbol, symbol_of};
use crate::symbol_name::{Cx, bytes};
use crate::{Context, demangle};

struct NoPackages;

impl PackageLoader for NoPackages {
    fn load(&self, path: &str) -> Result<Arc<Package>, String> {
        Ok(Arc::new(Package::fake(path)))
    }
}

/// What the packages of a test are checked in.
pub(crate) struct World {
    map: SourceMap,
    sink: Sink,
    pub names: ThreadedRodeo,
    pub types: Types,
}

impl World {
    pub fn new() -> Self {
        let names = ThreadedRodeo::new();
        Self {
            map: SourceMap::new(),
            sink: Sink::new(100),
            types: Types::new(&names),
            names,
        }
    }

    /// The package at `path`, which is one file of Go++.
    fn check(&self, path: &str, text: &str) -> Package {
        let file = format!("{path}.gopp");
        let source = SourceFile::new(file.into(), text.to_owned(), Dialect::Gopp).unwrap();
        let id = self.map.add(source);
        let source = self.map.file(id);
        let file = parse(id, &source, &self.names, &self.sink);
        let unit = Unit {
            file: &file,
            source: &source,
        };
        check(
            path,
            &[unit],
            &NoPackages,
            &self.names,
            &self.types,
            &self.sink,
        )
    }

    /// The package, which is checked without a report.
    fn checked(path: &str, text: &str) -> (Self, Package) {
        let world = Self::new();
        let package = world.check(path, text);
        (world, package)
    }

    fn reported(self) -> Vec<String> {
        let reported = self.sink.finish();
        reported
            .iter()
            .map(|diagnostic| diagnostic.message.clone())
            .collect()
    }

    pub fn namer(&self) -> Namer<'_> {
        Namer::new(&self.types, &self.names)
    }
}

fn object(world: &World, package: &Package, name: &str) -> ObjId {
    let name = world.names.get(name).unwrap();
    *package
        .scope
        .get(&name)
        .unwrap_or_else(|| panic!("nothing is named so"))
}

fn ty(world: &World, package: &Package, name: &str) -> TypeId {
    package.object(object(world, package, name)).ty
}

const SAMPLE: &str = "package p

type Vec2 struct{ X, Y float64 }

func (v Vec2) Len() float64 { return v.X }

func (v *Vec2) Scale(k float64) { v.X = v.X * k }

func (v Vec2) hidden() {}

@Operator
func (a Vec2) Add(b Vec2) Vec2 { return a }

@Operator
func (a Vec2) Mul(k float64) Vec2 { return a }

@Operator
func (a Vec2) Mul(b Vec2) Vec2 { return a }

@Operator
func (a Vec2) Div(k float64) Vec2 { return a }

@Operator
@Right
func (a Vec2) Div(k float64) Vec2 { return a }

@Operator
func (a Vec2) Neg() Vec2 { return a }

type W[T any] struct{ v T }

@Operator
func (a W[T]) Add(b T) W[T] { return a }

@Operator
func (a W[T]) Add(b float64) W[T] { return a }

func (a W[T]) Get() T { return a.v }

type Animal struct{ Name string }

@Extends(Animal)
type Dog struct{ Tricks int }

func (d Dog) Speak() string { return d.Name }

type Shaper interface {
	Area() float64
	Name() string
}

func Map[T, U any](xs []T, f func(T) U) []U { return nil }

func Plain(x int) int {
	type local struct{ a int }
	var l local
	return x + l.a
}

func _() {}

var counter int

var Table [4]Vec2

var _ = Plain(1)

const Limit = 10

type Number interface{ ~int | ~float64 }

type Text = string
";

/// Every symbol of the package: its functions and variables, and for
/// each type its descriptor, its methods and its operator methods. A
/// generic type is taken with `float64` for each of its parameters.
fn all_names(world: &World, package: &Package) -> BTreeSet<String> {
    let mut namer = world.namer();
    let mut names = BTreeSet::new();
    let float64 = world.types.basic(gopp_sema::basic::Basic::Float64);
    for (at, object) in package.objects.iter().enumerate() {
        let mut add = |namer: &mut Namer<'_>, entity: Entity<'_>| match symbol_of(namer, entity) {
            Ok(name) => assert!(names.insert(name.clone()), "{name} names two things"),
            Err(none) => assert!(
                matches!(
                    none,
                    NoSymbol::Generic | NoSymbol::Blank | NoSymbol::Constraint
                ),
                "{none}"
            ),
        };
        match object.kind {
            ObjKind::Func | ObjKind::Var => {
                add(&mut namer, Entity::Object(package, ObjId(at as u32)))
            }
            ObjKind::Type => {
                let Type::Named { id, .. } = &*world.types.get(object.ty) else {
                    continue;
                };
                let args = vec![float64; world.types.type_params(*id).len()];
                let receiver = world.types.named(*id, &args);
                add(&mut namer, Entity::TypeInfo(receiver));
                for method in world.types.declared_methods(*id) {
                    let name = method.method.name;
                    add(&mut namer, Entity::Method { receiver, name });
                }
                for method in &world.types.declared_operators(*id) {
                    add(&mut namer, Entity::Operator { receiver, method });
                }
            }
            _ => {}
        }
    }
    names
}

#[test]
fn every_method_has_its_own_name() {
    let (world, package) = World::checked("p", SAMPLE);
    let names = all_names(&world, &package);
    let expected = [
        "_GPF1pE5Plain",
        "_GPM3GetIN1pE1WdE",
        "_GPM3LenN1pE4Vec2",
        "_GPM5ScaleN1pE4Vec2",
        "_GPM5SpeakN1pE3Dog",
        "_GPM6hiddenN1pE4Vec2",
        "_GPR1pE5Table",
        "_GPR1pE7counter",
        "_GPTiIN1pE1WdE",
        "_GPTiN1pE3Dog",
        "_GPTiN1pE4Vec2",
        "_GPTiN1pE6Animal",
        "_GPTiN1pE6Shaper",
        "_GPTo6op_AddIN1pE1WdELQ0_E",
        "_GPTo6op_AddIN1pE1WdELdE",
        "_GPTo6op_AddN1pE4Vec2LB12_E",
        "_GPTo6op_DivN1pE4Vec2LdE",
        "_GPTo6op_MulN1pE4Vec2LB12_E",
        "_GPTo6op_MulN1pE4Vec2LdE",
        "_GPTo6op_NegN1pE4Vec2LE",
        "_GPTo7opr_DivN1pE4Vec2LdE",
    ];
    // The alias `Text` is no type of its own, and `Number` and `Map`
    // are the types of no values.
    let expected: BTreeSet<String> = expected.into_iter().map(str::to_owned).collect();
    assert_eq!(names, expected);
    for name in &names {
        let readable = demangle(name.as_bytes(), 1000).unwrap();
        assert!(readable.contains("p."), "{readable}");
    }
    let shown = |name: &str| demangle(name.as_bytes(), 1000).unwrap();
    assert_eq!(shown("_GPTo6op_AddIN1pE1WdELQ0_E"), "p.W[float64].Add($0)");
    assert_eq!(
        shown("_GPTo6op_AddIN1pE1WdELdE"),
        "p.W[float64].Add(float64)"
    );
    assert_eq!(
        shown("_GPTo7opr_DivN1pE4Vec2LdE"),
        "p.Vec2.Div(float64) @Right"
    );
    assert_eq!(world.reported(), [] as [&str; 0]);
}

#[test]
fn a_table_of_methods_and_what_it_holds_have_their_names() {
    let (world, package) = World::checked("p", SAMPLE);
    let mut namer = world.namer();
    let (vec, shaper) = (ty(&world, &package, "Vec2"), ty(&world, &package, "Shaper"));
    let name = world.names.get_or_intern("Len");
    let thunk = |pointer: bool| Entity::IfaceThunk {
        receiver: vec,
        name,
        pointer,
    };
    let mut named = |entity: Entity<'_>| {
        let symbol = symbol_of(&mut namer, entity).unwrap();
        let readable = demangle(symbol.as_bytes(), 1000).unwrap();
        (symbol, readable)
    };
    let shown = |(symbol, readable): (String, String)| format!("{symbol}: {readable}");
    assert_eq!(
        shown(named(Entity::ITab(vec, shaper))),
        "_GPTaN1pE4Vec2N1pE6Shaper: itab for p.Vec2 as p.Shaper"
    );
    assert_eq!(
        shown(named(Entity::ITab(world.types.pointer(vec), shaper))),
        "_GPTaPN1pE4Vec2N1pE6Shaper: itab for *p.Vec2 as p.Shaper"
    );
    assert_eq!(
        shown(named(thunk(false))),
        "_GPTIM3LenN1pE4Vec2: interface thunk for p.Vec2.Len"
    );
    assert_eq!(
        shown(named(thunk(true))),
        "_GPTpM3LenN1pE4Vec2: interface thunk for (*p.Vec2).Len"
    );
}

#[test]
fn what_has_no_symbol_says_why() {
    let (world, package) = World::checked("p", SAMPLE);
    let mut namer = world.namer();
    let of = |name: &str| Entity::Object(&package, object(&world, &package, name));
    let cases = [
        (of("Map"), NoSymbol::Generic),
        (of("Limit"), NoSymbol::NotEmitted),
        (of("Vec2"), NoSymbol::NotEmitted),
        (
            Entity::TypeInfo(ty(&world, &package, "W")),
            NoSymbol::Generic,
        ),
        (
            Entity::TypeInfo(ty(&world, &package, "Number")),
            NoSymbol::Constraint,
        ),
        (
            Entity::TypeInfo(ty(&world, &package, "Limit")),
            NoSymbol::Untyped,
        ),
        (Entity::TypeInfo(Types::INVALID), NoSymbol::Invalid),
        (
            Entity::Method {
                receiver: world.types.basic(gopp_sema::basic::Basic::Int),
                name: world.names.get_or_intern("Len"),
            },
            NoSymbol::Shape(crate::BadNode(
                "the receiver of a method is no declared type",
            )),
        ),
        (
            Entity::Method {
                receiver: ty(&world, &package, "Vec2"),
                name: world.names.get_or_intern("_"),
            },
            NoSymbol::Blank,
        ),
    ];
    for (entity, why) in cases {
        assert_eq!(symbol_of(&mut namer, entity), Err(why));
        assert!(!why.to_string().is_empty());
    }
    // The blank function and the blank variable are no objects, or have
    // no symbol.
    for (at, object) in package.objects.iter().enumerate() {
        if world.names.resolve(&object.name) == "_" {
            let entity = Entity::Object(&package, ObjId(at as u32));
            assert_eq!(symbol_of(&mut namer, entity), Err(NoSymbol::Blank));
        }
    }
    // A type of a function has a name only with its function.
    let plain = object(&world, &package, "Plain");
    let body = package.files[0].bodies.iter().find(|body| {
        body.locals
            .iter()
            .any(|local| world.names.resolve(&local.name) == "local")
    });
    let local = body
        .unwrap()
        .locals
        .iter()
        .find(|local| world.names.resolve(&local.name) == "local");
    let local = local.unwrap().ty;
    assert_eq!(
        symbol_of(&mut namer, Entity::TypeInfo(local)),
        Err(NoSymbol::Local)
    );
    let slice = world.types.slice(local);
    assert_eq!(
        symbol_of(&mut namer, Entity::TypeInfo(slice)),
        Err(NoSymbol::Local)
    );
    let within = |_| Some((Entity::Object(&package, plain), 0));
    let mut namer = world.namer().with_locals(&within);
    let name = symbol_of(&mut namer, Entity::TypeInfo(slice)).unwrap();
    assert_eq!(name, "_GPTiSg0_5localF1pE5Plain");
    assert_eq!(
        demangle(name.as_bytes(), 100).unwrap(),
        "typeinfo for []p.Plain.local"
    );
}

#[test]
fn package_paths_with_dots_slashes_and_unicode() {
    let cases = [
        ("main", "_GPF4mainE1F", "main.F"),
        (
            "github.com/x/y",
            "_GPF13github$2e$com1x1yE1F",
            "github.com/x/y.F",
        ),
        ("a.b/c-d", "_GPF6a$2e$b6c$2d$dE1F", "a.b/c-d.F"),
        ("日本/語", "_GPF12$65e5$$672c$6$8a9e$E1F", "日本/語.F"),
        ("", "_GPF0_E1F", ".F"),
        (
            "command-line-arguments",
            "_GPF28command$2d$line$2d$argumentsE1F",
            "command-line-arguments.F",
        ),
    ];
    for (path, name, readable) in cases {
        let (world, package) =
            World::checked(path, "package p\n\ntype T struct{}\n\nfunc F() {}\n");
        let mut namer = world.namer();
        let entity = Entity::Object(&package, object(&world, &package, "F"));
        assert_eq!(symbol_of(&mut namer, entity).as_deref(), Ok(name));
        assert_eq!(demangle(name.as_bytes(), 100).unwrap(), readable);
        // A type is named by the path of its package, not by its name.
        let entity = Entity::TypeInfo(ty(&world, &package, "T"));
        let name = symbol_of(&mut namer, entity).unwrap();
        assert_eq!(
            demangle(name.as_bytes(), 100).unwrap(),
            format!("typeinfo for {path}.T")
        );
    }
}

/// Types, each written in some ways: every spelling in a row is the
/// same type, and no two rows are.
const SPELLINGS: &[&[&str]] = &[
    &["int"],
    &["uint8", "byte", "B"],
    &["int32", "rune"],
    &["string", "Text"],
    &["*int"],
    &["**int"],
    &["[]int"],
    &["[]uint8", "[]byte"],
    &["[3]int"],
    &["[4]int"],
    &["map[string]int"],
    &["map[int]string"],
    &["Option[int]"],
    &["Option[*int]"],
    &["Unique[int]"],
    &["Shared[int]"],
    &["Weak[int]"],
    &["chan int"],
    &["chan<- int"],
    &["<-chan int"],
    &["func()"],
    &["func(int)", "func(x int)"],
    &["func(int) int"],
    &["func(int, int)", "func(a, b int)"],
    &["func(...int)"],
    &["func([]int)"],
    &["func(int) (string, error)", "func!(int) string"],
    &["func() error", "func!()"],
    &["struct{}"],
    &["struct{ a int }"],
    &["struct{ b int }"],
    &["struct{ a int32 }", "struct{ a rune }"],
    &["struct{ A int }"],
    &["struct{ a int @Required }"],
    &["struct{ a int \"tag\" }", "struct{ a int `tag` }"],
    &["struct{ a int \"other\" }"],
    &["struct{ a, b int }", "struct{ a int; b int }"],
    &["struct{ b, a int }"],
    &["interface{}", "any", "interface{ any }"],
    &["interface{ M() }"],
    &["interface{ M() int }"],
    &[
        "interface{ M(); N() }",
        "interface{ N(); M() }",
        "interface{ HasM; N() }",
        "MN",
    ],
    &["interface{ m() }"],
    &["interface{ Sync; M() }", "interface{ M(); Sync }"],
    &["interface{ Sync }"],
    &["error"],
    &["interface{ Error() string }"],
    &["Vec2"],
    &["Named"],
    &["Box[int]"],
    &["Box[uint8]", "Box[byte]", "Box[B]", "ByteBox"],
    &["Box[Box[int]]"],
    &["Pair[int, string]"],
    &["Pair[string, int]"],
    &["*Vec2"],
    &["[]Vec2"],
    &["func(Vec2) Vec2"],
];

const DECLARED: &str = "
type B = byte
type Text = string
type Vec2 struct{ X, Y float64 }
type Named struct{ X, Y float64 }
type Box[T any] struct{ v Option[T] }
type Pair[K, V any] struct { k Option[K]; v Option[V] }
type ByteBox = Box[byte]
type HasM interface{ M() }
type MN = interface{ M(); N() }
";

/// A package that declares every spelling as an alias, and the type of
/// each: the row and the type.
fn spelled(world: &World) -> Vec<(usize, &'static str, TypeId)> {
    let mut text = format!("package p\n{DECLARED}");
    let mut names = Vec::new();
    for (row, spellings) in SPELLINGS.iter().enumerate() {
        for (at, spelling) in spellings.iter().enumerate() {
            text += &format!("type S{row}_{at} = {spelling}\n");
            names.push((row, *spelling, format!("S{row}_{at}")));
        }
    }
    let package = world.check("p", &text);
    names
        .into_iter()
        .map(|(row, spelling, name)| (row, spelling, ty(world, &package, &name)))
        .collect()
}

#[test]
fn one_name_exactly_when_the_checker_says_identical() {
    let world = World::new();
    let types = spelled(&world);
    let mut namer = world.namer();
    let names: Vec<String> = types
        .iter()
        .map(|&(_, spelling, ty)| {
            symbol_of(&mut namer, Entity::TypeInfo(ty))
                .unwrap_or_else(|none| panic!("{spelling}: {none}"))
        })
        .collect();
    for (a, &(row, spelling, ty)) in types.iter().enumerate() {
        for (b, &(other_row, other_spelling, other)) in types.iter().enumerate() {
            let identical = world.types.identical(ty, other);
            assert_eq!(
                identical,
                row == other_row,
                "{spelling} and {other_spelling}"
            );
            assert_eq!(
                names[a] == names[b],
                identical,
                "{spelling} and {other_spelling}"
            );
        }
    }
    assert_eq!(world.reported(), [] as [&str; 0]);
}

#[test]
fn readable_name_is_the_type_printers_text() {
    let world = World::new();
    let mut namer = world.namer();
    for (_, spelling, ty) in spelled(&world) {
        let name = symbol_of(&mut namer, Entity::TypeInfo(ty)).unwrap();
        let readable = demangle(name.as_bytes(), 1000).unwrap();
        let printed = world.types.canonical(ty, &world.names);
        assert_eq!(readable, format!("typeinfo for {printed}"), "{spelling}");
    }
}

#[test]
fn spelling_differs_only_for_one_type_written_two_ways() {
    let world = World::new();
    let mut namer = world.namer();
    // What the program wrote, by what a name is read as.
    let mut written: HashMap<String, BTreeSet<String>> = HashMap::new();
    for (_, _, ty) in spelled(&world) {
        let name = symbol_of(&mut namer, Entity::TypeInfo(ty)).unwrap();
        let readable = demangle(name.as_bytes(), 1000).unwrap();
        let readable = readable.strip_prefix("typeinfo for ").unwrap().to_owned();
        let declared_as = world.types.alias_target(ty).unwrap_or(ty);
        let shown = world.types.display(declared_as, &world.names);
        written.entry(readable).or_default().insert(shown);
    }
    let differs = |readable: &str, shown: &[&str]| {
        let all: BTreeSet<String> = shown.iter().map(|&text| text.to_owned()).collect();
        assert_eq!(written.get(readable), Some(&all), "{readable}");
    };
    differs("uint8", &["uint8", "byte", "p.B"]);
    differs("int32", &["int32", "rune"]);
    differs("string", &["string", "p.Text"]);
    differs("[]uint8", &["[]uint8", "[]byte"]);
    differs("func(int) (string, error)", &["func(int) (string, error)"]);
    differs("any", &["interface{}", "any", "interface{any}"]);
    differs(
        "interface{M(); N()}",
        &["interface{M(); N()}", "interface{N(); p.HasM}", "p.MN"],
    );
    differs(
        "p.Box[uint8]",
        &["p.Box[uint8]", "p.Box[byte]", "p.Box[p.B]", "p.ByteBox"],
    );
    differs("struct{a int32}", &["struct{a int32}", "struct{a rune}"]);
    // Every other type is read as it was written.
    let known = 9;
    let one_way = written
        .iter()
        .filter(|(readable, shown)| shown.len() == 1 && shown.contains(*readable));
    assert_eq!(one_way.count(), SPELLINGS.len() - known + 1);
}

#[test]
fn struct_identity_has_every_field_mark() {
    let (world, package) = World::checked(
        "p",
        "package p\n\ntype A = struct{ a int }\n\ntype R = struct{ a int @Required }\n",
    );
    let mut namer = world.namer();
    let (plain, required) = (ty(&world, &package, "A"), ty(&world, &package, "R"));
    assert!(!world.types.identical(plain, required));
    let plain = symbol_of(&mut namer, Entity::TypeInfo(plain)).unwrap();
    let required = symbol_of(&mut namer, Entity::TypeInfo(required)).unwrap();
    assert_eq!(plain, "_GPTiZ0_q2_1pE1aiE");
    assert_eq!(required, "_GPTiZ0_q10_1pE1aiE");
    // Each mark alone, and all that go together, give another type.
    let mut cx = Cx::default();
    let int = cx.int();
    let mut names = BTreeSet::new();
    for flags in 0..64u32 {
        let has = |bit: u32| flags & (1 << bit) != 0;
        let required = match (has(3), has(4)) {
            (false, false) => Required::No,
            (true, false) => Required::Yes,
            (false, true) => Required::MustSet,
            (true, true) => continue,
        };
        let field = cx.add(Node::Field {
            name: bytes("a"),
            package: has(0).then(|| bytes("p")),
            embedded: has(1),
            required,
            parent: has(2),
            tag: has(5).then(|| bytes("t")),
            ty: int,
        });
        let strukt = cx.add(Node::Struct(Box::new([field])));
        let entity = cx.typeinfo(strukt);
        let name = cx.symbol(entity);
        assert!(
            crate::parse(&mut Context::new(), name.as_bytes()).is_ok(),
            "{name}"
        );
        assert!(names.insert(name));
    }
    assert_eq!(names.len(), 48);
}

/// Packages that name each other's kinds of thing: generic types and
/// methods, overloads, parents.
fn packages() -> Vec<(&'static str, String)> {
    let other = "package q

type Vec2 struct{ X, Y float32 }

func (v Vec2) Len() float32 { return v.X }

@Operator
func (a Vec2) Add(b Vec2) Vec2 { return a }

type Stack[T any] struct{ items []T }

func (s Stack[T]) Top() Option[T] { return nil }

func Plain(x int) int { return x }

var counter uint8
";
    vec![
        ("p", SAMPLE.to_owned()),
        ("q", other.to_owned()),
        ("x.y/z", other.replace("package q", "package z")),
        ("deep/p", SAMPLE.to_owned()),
    ]
}

#[test]
fn names_do_not_depend_on_checking_order_or_threads() {
    let names_in = |order: &[usize], threads: usize| -> Vec<BTreeSet<String>> {
        let pool = rayon::ThreadPoolBuilder::new()
            .num_threads(threads)
            .build()
            .unwrap();
        pool.install(|| {
            let world = World::new();
            let sources = packages();
            let mut names = vec![BTreeSet::new(); sources.len()];
            for &at in order {
                let (path, text) = &sources[at];
                names[at] = all_names(&world, &world.check(path, text));
            }
            assert_eq!(world.reported(), [] as [&str; 0]);
            names
        })
    };
    let first = names_in(&[0, 1, 2, 3], 1);
    assert!(first.iter().all(|names| names.len() > 5));
    // Two packages of one text differ in every name.
    assert!(first[0].is_disjoint(&first[3]));
    assert!(first[1].is_disjoint(&first[2]));
    let mut rng = ChaCha8Rng::seed_from_u64(9);
    for round in 0..8 {
        let mut order = [0, 1, 2, 3];
        for at in (1..order.len()).rev() {
            order.swap(at, rng.random_range(0..=at));
        }
        let threads = [1, 16][round % 2];
        assert_eq!(
            names_in(&order, threads),
            first,
            "{order:?} on {threads} threads"
        );
    }
}

/// Types of the checker that grow with `size`.
pub(crate) fn checker_shapes(world: &World, size: usize) -> Vec<TypeId> {
    let types = &world.types;
    let int = types.basic(gopp_sema::basic::Basic::Int);
    let name = |text: String| world.names.get_or_intern(text);
    let mut chain = int;
    for level in 0..size {
        chain = match level % 6 {
            0 => types.pointer(chain),
            1 => types.slice(chain),
            2 => types.array(level as u64, chain),
            3 => types.map(int, chain),
            4 => types.option(chain),
            _ => types.chan(gopp_sema::types::ChanDir::Both, chain),
        };
    }
    let mut funcs = int;
    for _ in 0..size {
        funcs = types.func(&[funcs, int], &[funcs], false);
    }
    let field = |at: usize| gopp_sema::types::Field {
        name: name(format!("f{at}")),
        package: None,
        ty: types.array(at as u64, int),
        embedded: false,
        tag: None,
        required: at.is_multiple_of(2),
    };
    let strukt = types.strukt((0..size).map(field).collect());
    let wide: Vec<TypeId> = (0..size).map(|at| types.array(at as u64, int)).collect();
    let tuple = types.func(&wide, &wide, false);
    let mut halves = int;
    for _ in 0..size {
        halves = types.map(halves, halves);
    }
    vec![chain, funcs, strukt, tuple, halves]
}

#[test]
fn symbol_of_takes_linear_steps() {
    for shape in 0..5 {
        assert_near_linear(&[1_000, 4_000, 16_000], |size| {
            let world = World::new();
            let ty = checker_shapes(&world, size)[shape];
            let mut namer = world.namer();
            let name = symbol_of(&mut namer, Entity::TypeInfo(ty)).unwrap();
            assert!(name.len() > size);
            namer.steps()
        });
    }
}
