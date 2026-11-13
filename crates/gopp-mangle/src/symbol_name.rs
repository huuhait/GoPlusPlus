//! The names themselves: what the mangler writes, what the parser takes
//! and refuses, and how much work both are.

use std::collections::{HashMap, HashSet};

use gopp_testutil::linear::assert_near_linear;
use rand::rngs::ChaCha8Rng;
use rand::{RngExt, SeedableRng};

use crate::node::{Basic, Context, Dir, Node, NodeId, Pair, Required, Wrap};
use crate::parse::{ParseError, parse, parse_counted};
use crate::{PREFIX, demangle, filter, ident, is_reserved_name};

pub(crate) fn bytes(text: &str) -> Box<[u8]> {
    text.as_bytes().into()
}

/// A context with short ways to make the nodes that the tests need.
#[derive(Default)]
pub(crate) struct Cx(pub Context);

impl Cx {
    pub fn add(&mut self, node: Node) -> NodeId {
        self.0.add(node).unwrap()
    }

    pub fn basic(&mut self, basic: Basic) -> NodeId {
        self.add(Node::Basic(basic))
    }

    pub fn int(&mut self) -> NodeId {
        self.basic(Basic::Int)
    }

    pub fn named(&mut self, package: &str, name: &str) -> NodeId {
        let (package, name) = (bytes(package), bytes(name));
        self.add(Node::Named { package, name })
    }

    pub fn func(&mut self, package: &str, name: &str) -> NodeId {
        let (package, name) = (bytes(package), bytes(name));
        self.add(Node::Func { package, name })
    }

    pub fn method(&mut self, receiver: NodeId, name: &str) -> NodeId {
        let name = bytes(name);
        self.add(Node::Method { name, receiver })
    }

    pub fn tuple(&mut self, types: &[NodeId]) -> NodeId {
        self.add(Node::Tuple(types.into()))
    }

    pub fn instance(&mut self, of: NodeId, args: &[NodeId]) -> NodeId {
        let args = args.into();
        self.add(Node::Instance { of, args })
    }

    pub fn operator(
        &mut self,
        receiver: NodeId,
        name: &str,
        params: &[NodeId],
        right: bool,
    ) -> NodeId {
        let (name, params) = (bytes(name), self.tuple(params));
        self.add(Node::OpMethod {
            right,
            name,
            receiver,
            params,
        })
    }

    pub fn signature(&mut self, params: &[NodeId], results: &[NodeId], variadic: bool) -> NodeId {
        let (params, results) = (self.tuple(params), self.tuple(results));
        self.add(Node::Signature {
            variadic,
            params,
            results,
        })
    }

    pub fn wrap(&mut self, wrap: Wrap, of: NodeId) -> NodeId {
        self.add(Node::Wrap(wrap, of))
    }

    pub fn typeinfo(&mut self, of: NodeId) -> NodeId {
        self.wrap(Wrap::TypeInfo, of)
    }

    pub fn symbol(&self, entity: NodeId) -> String {
        self.0.symbol(entity)
    }
}

/// The rows of appendix A.7.
const GOLDENS: [(&str, &str); 27] = [
    ("_GPF4mainE3Foo", "main.Foo"),
    ("_GPF3std7stringsE5Index", "std/strings.Index"),
    ("_GPF13github$2e$com1x1yE3Run", "github.com/x/y.Run"),
    ("_GPF4mainE18$65e5$$672c$$8a9e$", "main.日本語"),
    ("_GPF1pE5$30$x", "p.0x"),
    ("_GPM3LenN4mainE4Vec2", "main.Vec2.Len"),
    ("_GPTo6op_AddN4mainE4Vec2LB12_E", "main.Vec2.Add(main.Vec2)"),
    ("_GPTo6op_NegN4mainE4Vec2LE", "main.Vec2.Neg()"),
    ("_GPTo6op_DivN4mainE4Vec2LdE", "main.Vec2.Div(float64)"),
    (
        "_GPTo7opr_DivN4mainE4Vec2LdE",
        "main.Vec2.Div(float64) @Right",
    ),
    ("_GPTo6op_AddIN4mainE1WdELQ0_E", "main.W[float64].Add($0)"),
    ("_GPIF3std6slicesE3MapirE", "std/slices.Map[int, string]"),
    ("_GPC2_M3GetIN4mainE3BoxiE", "main.Box[int].Get.func2"),
    ("_GPTiHrSi", "typeinfo for map[string][]int"),
    ("_GPTvN4mainE3Dog", "vtable for main.Dog"),
    ("_GPTmN4mainE3Dog", "method table for main.Dog"),
    (
        "_GPTnN4mainE6Shaper",
        "interface descriptor for main.Shaper",
    ),
    ("_GPJ4mainE", "main.init"),
    ("_GPK4mainE1_", "main.init.1"),
    ("_GPR4mainE7counter", "main.counter"),
    ("_GPTCF4mainE8Callback", "C-ABI thunk for main.Callback"),
    (
        "_GPTIM4BumpN4mainE3Box",
        "interface thunk for main.Box.Bump",
    ),
    (
        "_GPTpM3SumN4mainE5Adder",
        "interface thunk for (*main.Adder).Sum",
    ),
    (
        "_GPTiIN4mainE4PairIB6_iiEB18_E",
        "typeinfo for main.Pair[main.Pair[int, int], main.Pair[int, int]]",
    ),
    (
        "_GPTiG0_0_LiELru5errorE",
        "typeinfo for func(int) (string, error)",
    ),
    ("_GPTiZ0_q8_1aiE", "typeinfo for struct{a int @Required}"),
    ("_GPTiYE", "typeinfo for any"),
];

/// The entities of the rows, made from nodes.
fn golden_entities(cx: &mut Cx) -> Vec<NodeId> {
    let (int, string, float64) = (cx.int(), cx.basic(Basic::String), cx.basic(Basic::Float64));
    let vec2 = cx.named("main", "Vec2");
    let dog = cx.named("main", "Dog");
    let w = cx.named("main", "W");
    let w = cx.instance(w, &[float64]);
    let q0 = cx.add(Node::TypeParam(0));
    let map = cx.func("std/slices", "Map");
    let r#box = cx.named("main", "Box");
    let box_int = cx.instance(r#box, &[int]);
    let get = cx.method(box_int, "Get");
    let slice = cx.add(Node::Slice(int));
    let string_to_slice = cx.add(Node::Map(string, slice));
    let shaper = cx.named("main", "Shaper");
    let callback = cx.func("main", "Callback");
    let bump = cx.method(r#box, "Bump");
    let adder = cx.named("main", "Adder");
    let sum = cx.method(adder, "Sum");
    let pair = cx.named("main", "Pair");
    let inner = cx.instance(pair, &[int, int]);
    let outer = cx.instance(pair, &[inner, inner]);
    let error = cx.add(Node::Universe(bytes("error")));
    let sig = cx.signature(&[int], &[string, error], false);
    let field = cx.add(Node::Field {
        name: bytes("a"),
        package: None,
        embedded: false,
        required: Required::Yes,
        parent: false,
        tag: None,
        ty: int,
    });
    let strukt = cx.add(Node::Struct(Box::new([field])));
    let any = cx.add(Node::Interface {
        sync: false,
        methods: Box::new([]),
    });
    vec![
        cx.func("main", "Foo"),
        cx.func("std/strings", "Index"),
        cx.func("github.com/x/y", "Run"),
        cx.func("main", "日本語"),
        cx.func("p", "0x"),
        cx.method(vec2, "Len"),
        cx.operator(vec2, "Add", &[vec2], false),
        cx.operator(vec2, "Neg", &[], false),
        cx.operator(vec2, "Div", &[float64], false),
        cx.operator(vec2, "Div", &[float64], true),
        cx.operator(w, "Add", &[q0], false),
        cx.instance(map, &[int, string]),
        cx.add(Node::Closure {
            ordinal: 2,
            within: get,
        }),
        cx.typeinfo(string_to_slice),
        cx.wrap(Wrap::VTable, dog),
        cx.wrap(Wrap::MethodTable, dog),
        cx.wrap(Wrap::IfaceDesc, shaper),
        cx.add(Node::PkgInit(bytes("main"))),
        cx.add(Node::InitFunc {
            package: bytes("main"),
            ordinal: 1,
        }),
        cx.add(Node::Var {
            package: bytes("main"),
            name: bytes("counter"),
        }),
        cx.wrap(Wrap::CabiThunk, callback),
        cx.wrap(Wrap::IfaceThunk, bump),
        cx.wrap(Wrap::IfacePtrThunk, sum),
        cx.typeinfo(outer),
        cx.typeinfo(sig),
        cx.typeinfo(strukt),
        cx.typeinfo(any),
    ]
}

#[test]
fn goldens() {
    let mut cx = Cx::default();
    let entities = golden_entities(&mut cx);
    assert_eq!(entities.len(), GOLDENS.len());
    for (entity, (name, readable)) in entities.into_iter().zip(GOLDENS) {
        assert_eq!(cx.symbol(entity), name);
        assert_eq!(cx.0.readable(entity, usize::MAX), readable, "{name}");
        assert_eq!(demangle(name.as_bytes(), usize::MAX).unwrap(), readable);
        assert_eq!(parse(&mut cx.0, name.as_bytes()), Ok(entity), "{name}");
    }
}

#[test]
fn a_function_of_a_package_is_named_without_a_context() {
    let mut cx = Cx::default();
    let main = cx.func("x.y/main", "main");
    assert_eq!(crate::func_symbol("x.y/main", "main"), cx.symbol(main));
    let init = cx.add(Node::InitFunc {
        package: bytes("x.y/main"),
        ordinal: 12,
    });
    assert_eq!(crate::init_symbol("x.y/main", 12), cx.symbol(init));
}

fn appendix() -> String {
    let path = concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../../docs/spec/appendix-a-symbol-names.md"
    );
    std::fs::read_to_string(path).unwrap()
}

#[test]
fn the_appendix_is_the_code() {
    let text = appendix();
    let rows: Vec<(&str, &str)> = text
        .lines()
        .filter(|line| line.starts_with("| `_GP"))
        .map(|line| {
            let mut cells = line.split('|').map(|cell| cell.trim().trim_matches('`'));
            (cells.nth(1).unwrap(), cells.next().unwrap())
        })
        .collect();
    assert_eq!(rows, GOLDENS);
    // Every whole name in the running text is one too.
    let mut names = 0;
    for span in text.split('`').skip(1).step_by(2) {
        let word = |c: char| c.is_ascii_alphanumeric() || c == '_' || c == '$';
        if span.len() > PREFIX.len() && span.starts_with(PREFIX) && span.chars().all(word) {
            assert!(demangle(span.as_bytes(), usize::MAX).is_ok(), "{span}");
            names += 1;
        }
    }
    assert!(names > GOLDENS.len());
    // The examples of A.3.
    let written = |write: &dyn Fn(&mut String)| {
        let mut out = String::new();
        write(&mut out);
        out
    };
    let examples = [
        written(&|out| ident::identifier(b"Foo", out)),
        written(&|out| ident::identifier(b"0x", out)),
        written(&|out| ident::identifier("日本語".as_bytes(), out)),
        written(&|out| ident::path(b"std/strings", out)),
        written(&|out| ident::path(b"github.com/x/y", out)),
        written(&|out| ident::path(b"", out)),
        written(&|out| ident::number(12, out)),
        written(&|out| ident::signed(-3, out)),
    ];
    let expected = [
        "3Foo",
        "5$30$x",
        "18$65e5$$672c$$8a9e$",
        "3std7stringsE",
        "13github$2e$com1x1yE",
        "0_E",
        "12_",
        "n3_",
    ];
    assert_eq!(examples, expected);
    for example in expected {
        assert!(text.contains(&format!("`{example}`")), "{example}");
    }
}

/// Names that are hard to write: other scripts, punctuation, bytes that
/// are no text, nothing at all.
const NASTY: [&[u8]; 24] = [
    b"",
    b"_",
    b"a",
    b"E",
    b"0",
    b"0x",
    b"12",
    b"a1b",
    b"$",
    b"$30$",
    b"a$b",
    b"a.b",
    b"a/b",
    b"a b",
    b"\0",
    b"a\0b",
    "日本語".as_bytes(),
    "é".as_bytes(),
    "\u{10ffff}".as_bytes(),
    b"\xff",
    b"\xc3",
    b"a\xed\xa0\x80b",
    b"\xf0\x9f\x98",
    b"_GPF4mainE3Foo",
];

fn round_trip(cx: &Cx, entity: NodeId) -> String {
    let name = cx.symbol(entity);
    assert!(
        name.bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || byte == b'_' || byte == b'$')
    );
    let mut other = Context::new();
    let parsed = match parse(&mut other, name.as_bytes()) {
        Ok(parsed) => parsed,
        Err(error) => panic!("{name}: {error}"),
    };
    assert_eq!(other.symbol(parsed), name);
    let readable = cx.0.readable(entity, 1 << 16);
    assert_eq!(other.readable(parsed, 1 << 16), readable, "{name}");
    name
}

#[test]
fn every_identifier_round_trips() {
    let mut cx = Cx::default();
    let mut names = HashSet::new();
    let single: Vec<Vec<u8>> = (0..=255u8).map(|byte| vec![b'a', byte]).collect();
    let all = NASTY
        .iter()
        .copied()
        .chain(single.iter().map(|name| &name[..]));
    for name in all {
        let func = cx.add(Node::Func {
            package: bytes("p"),
            name: name.into(),
        });
        let data = cx.add(Node::Data(name.into()));
        let universe = cx.add(Node::Universe(name.into()));
        let universe = cx.typeinfo(universe);
        for entity in [func, data, universe] {
            assert!(names.insert(round_trip(&cx, entity)));
        }
        // In a path a `/` parts two segments.
        let path = cx.add(Node::PkgInit(name.into()));
        assert!(names.insert(round_trip(&cx, path)));
    }
}

#[test]
fn all_code_points_round_trip() {
    let mut cx = Cx::default();
    let all: Vec<char> = (0..=0x10ffffu32).filter_map(char::from_u32).collect();
    assert_eq!(all.len(), 0x110000 - 0x800);
    for chunk in all.chunks(8192) {
        let name: String = chunk.iter().collect();
        let func = cx.func("p", &name);
        let symbol = cx.symbol(func);
        let mut other = Context::new();
        let parsed = parse(&mut other, symbol.as_bytes()).unwrap();
        let Node::Func { name: read, .. } = other.get(parsed) else {
            panic!("{symbol}");
        };
        assert_eq!(&**read, name.as_bytes());
    }
}

#[test]
fn boundaries_between_paths_and_names_never_blur() {
    let parts = [
        "", "a", "b", "ab", "a/b", "a.b", "a//b", "/", "E", "1", "1a", "a1", "a/", "/a",
    ];
    let mut cx = Cx::default();
    let mut names = HashMap::new();
    for package in parts {
        for name in parts {
            let func = cx.func(package, name);
            let var = cx.add(Node::Var {
                package: bytes(package),
                name: bytes(name),
            });
            let named = cx.named(package, name);
            let named = cx.typeinfo(named);
            for entity in [func, var, named] {
                let symbol = round_trip(&cx, entity);
                assert_eq!(names.insert(symbol.clone(), entity), None, "{symbol}");
            }
        }
    }
}

/// One node of each kind that has a tag.
fn one_of_each(cx: &mut Cx) -> Vec<NodeId> {
    let int = cx.int();
    let named = cx.named("p", "T");
    let func = cx.func("p", "f");
    let method = cx.method(named, "m");
    let sig = cx.signature(&[], &[], false);
    let mut all: Vec<NodeId> = Basic::ALL.iter().map(|&basic| cx.basic(basic)).collect();
    all.extend(Wrap::ALL.iter().map(|&wrap| match wrap {
        Wrap::IfacePtrThunk | Wrap::CabiThunk | Wrap::MethodValue => cx.wrap(wrap, method),
        Wrap::MethodExpr | Wrap::Promoted | Wrap::IfaceThunk => cx.wrap(wrap, method),
        _ => cx.wrap(wrap, int),
    }));
    let field = Node::Field {
        name: bytes("x"),
        package: None,
        embedded: false,
        required: Required::No,
        parent: false,
        tag: None,
        ty: int,
    };
    let imethod = Node::IMethod {
        package: None,
        name: bytes("m"),
        sig,
    };
    let others = [
        Node::Pointer(int),
        Node::Slice(int),
        Node::Array(3, int),
        Node::Map(int, int),
        Node::Option(int),
        Node::Chan(Dir::Both, int),
        Node::Tuple(Box::new([])),
        Node::Struct(Box::new([])),
        field,
        Node::Interface {
            sync: false,
            methods: Box::new([]),
        },
        imethod,
        Node::LocalNamed {
            ordinal: 0,
            name: bytes("T"),
            within: func,
        },
        Node::Universe(bytes("error")),
        Node::Instance {
            of: named,
            args: Box::new([int]),
        },
        Node::TypeParam(0),
        Node::OpMethod {
            right: false,
            name: bytes("Add"),
            receiver: named,
            params: cx.tuple(&[]),
        },
        Node::Var {
            package: bytes("p"),
            name: bytes("v"),
        },
        Node::Closure {
            ordinal: 1,
            within: func,
        },
        Node::PkgInit(bytes("p")),
        Node::InitFunc {
            package: bytes("p"),
            ordinal: 0,
        },
        Node::Pair(Pair::SecondaryVTable, named, named),
        Node::Pair(Pair::ITab, named, named),
        Node::Data(bytes("k")),
        Node::BaseAdjustThunk(-8, method),
    ];
    all.extend([named, func, method, sig]);
    all.extend(others.map(|node| cx.add(node)));
    all
}

#[test]
fn tags_are_prefix_free_and_in_the_alphabet() {
    let mut cx = Cx::default();
    let mut tags: Vec<String> = one_of_each(&mut cx)
        .into_iter()
        .map(|id| {
            let mut head = String::new();
            cx.0.get(id).head(&mut head);
            let len = if head.starts_with('T') { 2 } else { 1 };
            head[..len].to_owned()
        })
        .collect();
    tags.extend(["E".to_owned(), "B".to_owned()]);
    let count = tags.len();
    tags.sort();
    tags.dedup();
    assert_eq!(tags.len(), count, "two kinds share a tag");
    // 18 basic types, 42 other kinds, the end and the back reference.
    assert_eq!(count, 18 + 42 + 2);
    for tag in &tags {
        assert!(tag.chars().all(|c| c.is_ascii_alphabetic()), "{tag}");
        let begins_another = tags
            .iter()
            .any(|other| other != tag && other.starts_with(&**tag));
        assert!(!begins_another, "{tag}");
        // A digit starts a length and never a node.
        assert!(!tag.starts_with(|c: char| c.is_ascii_digit()));
    }
}

#[test]
fn never_collides_with_a_c_name() {
    let mut cx = Cx::default();
    for entity in corpus(&mut cx, 500, 7) {
        let symbol = cx.symbol(entity);
        assert!(is_reserved_name(&symbol));
        assert!(symbol.starts_with("_GP"));
    }
    let foreign = [
        "main",
        "printf",
        "_start",
        "_Z3foov",
        "_ZN4core3fmt5Write9write_fmt17h0123456789abcdefE",
        "_RNvCs1234_7mycrate3foo",
        "$s4main3fooyyF",
        "_gopp_panic_bounds",
        "gopp_f1",
        "_G",
        "_Gp",
        "__GPF4mainE3Foo",
    ];
    for name in foreign {
        assert!(!is_reserved_name(name), "{name}");
        assert!(demangle(name.as_bytes(), 100).is_err(), "{name}");
    }
}

/// Makes entities of every kind, with types that occur again.
struct Generator<'a> {
    cx: &'a mut Cx,
    rng: ChaCha8Rng,
    types: Vec<NodeId>,
}

impl Generator<'_> {
    fn pick(&mut self, below: usize) -> usize {
        self.rng.random_range(0..below)
    }

    fn name(&mut self) -> Box<[u8]> {
        const PLAIN: [&str; 8] = ["T", "Vec2", "Get", "x", "main", "Box", "a1", "longer_name"];
        match self.pick(4) {
            0 => NASTY[self.pick(NASTY.len())].into(),
            _ => bytes(PLAIN[self.pick(PLAIN.len())]),
        }
    }

    fn path(&mut self) -> Box<[u8]> {
        const PATHS: [&str; 6] = ["main", "p", "std/strings", "github.com/x/y", "", "a/b/c"];
        bytes(PATHS[self.pick(PATHS.len())])
    }

    fn list(&mut self, depth: u32, at_least: usize) -> Vec<NodeId> {
        let count = at_least + self.pick(3);
        (0..count).map(|_| self.ty(depth)).collect()
    }

    fn signature(&mut self, depth: u32) -> NodeId {
        let mut params = self.list(depth, 0);
        let variadic = self.pick(4) == 0;
        if variadic {
            let of = self.ty(depth);
            params.push(self.cx.add(Node::Slice(of)));
        }
        let results = self.list(depth, 0);
        self.cx.signature(&params, &results, variadic)
    }

    fn ty(&mut self, depth: u32) -> NodeId {
        if !self.types.is_empty() && self.pick(3) == 0 {
            let at = self.pick(self.types.len());
            return self.types[at];
        }
        let leaf = depth == 0 || self.pick(4) == 0;
        let inner = depth.saturating_sub(1);
        let node = match (leaf, self.pick(if leaf { 4 } else { 14 })) {
            (true, 0) => Node::Basic(Basic::ALL[self.pick(Basic::ALL.len())]),
            (true, 1) => Node::Named {
                package: self.path(),
                name: self.name(),
            },
            (true, 2) => Node::Universe(self.name()),
            (true, _) => Node::TypeParam(self.pick(3) as u64),
            (false, 0) => Node::Pointer(self.ty(inner)),
            (false, 1) => Node::Slice(self.ty(inner)),
            (false, 2) => Node::Array(self.pick(1000) as u64, self.ty(inner)),
            (false, 3) => Node::Map(self.ty(inner), self.ty(inner)),
            (false, 4) => Node::Option(self.ty(inner)),
            (false, 5) => {
                let dir = [Dir::Both, Dir::Send, Dir::Recv][self.pick(3)];
                Node::Chan(dir, self.ty(inner))
            }
            (false, 6) => Node::Tuple(self.list(inner, 0).into()),
            (false, 7) => return self.signature(inner),
            (false, 8) => {
                let count = self.pick(4);
                let fields: Vec<NodeId> = (0..count).map(|_| self.field(inner)).collect();
                Node::Struct(fields.into())
            }
            (false, 9) => self.interface(inner),
            (false, 10) => Node::LocalNamed {
                ordinal: self.pick(3) as u64,
                name: self.name(),
                within: self.code(inner),
            },
            (false, _) => {
                let of = match self.pick(3) {
                    0 => Node::Universe(self.name()),
                    1 => Node::Named {
                        package: self.path(),
                        name: self.name(),
                    },
                    _ => Node::LocalNamed {
                        ordinal: 0,
                        name: self.name(),
                        within: self.code(inner),
                    },
                };
                Node::Instance {
                    of: self.cx.add(of),
                    args: self.list(inner, 1).into(),
                }
            }
        };
        let made = self.cx.add(node);
        self.types.push(made);
        made
    }

    fn field(&mut self, depth: u32) -> NodeId {
        let node = Node::Field {
            name: self.name(),
            package: (self.pick(3) == 0).then(|| self.path()),
            embedded: self.pick(4) == 0,
            required: [Required::No, Required::Yes, Required::MustSet][self.pick(3)],
            parent: self.pick(8) == 0,
            tag: (self.pick(4) == 0).then(|| self.name()),
            ty: self.ty(depth),
        };
        self.cx.add(node)
    }

    fn interface(&mut self, depth: u32) -> Node {
        let mut methods: Vec<_> = (0..self.pick(4))
            .map(|_| ((self.pick(3) == 0).then(|| self.path()), self.name()))
            .collect();
        methods.sort();
        methods.dedup();
        let methods = methods
            .into_iter()
            .map(|(package, name)| {
                let sig = self.signature(depth);
                self.cx.add(Node::IMethod { package, name, sig })
            })
            .collect();
        Node::Interface {
            sync: self.pick(4) == 0,
            methods,
        }
    }

    /// A type that methods are declared for.
    fn receiver(&mut self, depth: u32) -> NodeId {
        let named = Node::Named {
            package: self.path(),
            name: self.name(),
        };
        let named = self.cx.add(named);
        match self.pick(3) {
            0 => {
                let args = self.list(depth, 1);
                self.cx.instance(named, &args)
            }
            _ => named,
        }
    }

    fn method(&mut self, depth: u32) -> NodeId {
        let receiver = self.receiver(depth);
        let node = match self.pick(3) {
            0 => Node::OpMethod {
                right: self.pick(2) == 0,
                name: self.name(),
                receiver,
                params: {
                    let params = self.list(depth, 0);
                    self.cx.tuple(&params)
                },
            },
            _ => Node::Method {
                name: self.name(),
                receiver,
            },
        };
        let method = self.cx.add(node);
        match self.pick(5) {
            0 => {
                let args = self.list(depth, 1);
                self.cx.instance(method, &args)
            }
            _ => method,
        }
    }

    /// An entity that code is written in.
    fn code(&mut self, depth: u32) -> NodeId {
        let inner = depth.saturating_sub(1);
        let node = match self.pick(if depth == 0 { 3 } else { 6 }) {
            0 => Node::Func {
                package: self.path(),
                name: self.name(),
            },
            1 => Node::Var {
                package: self.path(),
                name: self.name(),
            },
            2 => Node::InitFunc {
                package: self.path(),
                ordinal: self.pick(4) as u64,
            },
            3 => return self.method(inner),
            4 => {
                let of = Node::Func {
                    package: self.path(),
                    name: self.name(),
                };
                Node::Instance {
                    of: self.cx.add(of),
                    args: self.list(inner, 1).into(),
                }
            }
            _ => Node::Closure {
                ordinal: 1 + self.pick(5) as u64,
                within: self.code(inner),
            },
        };
        self.cx.add(node)
    }

    fn entity(&mut self, depth: u32) -> NodeId {
        const OF_TYPES: [Wrap; 8] = [
            Wrap::TypeInfo,
            Wrap::VTable,
            Wrap::EqGlue,
            Wrap::HashGlue,
            Wrap::DropGlue,
            Wrap::CloneGlue,
            Wrap::MethodTable,
            Wrap::IfaceDesc,
        ];
        const THUNKS: [Wrap; 5] = [
            Wrap::CabiThunk,
            Wrap::MethodValue,
            Wrap::MethodExpr,
            Wrap::Promoted,
            Wrap::IfaceThunk,
        ];
        let node = match self.pick(9) {
            0 | 1 => return self.code(depth),
            2 | 3 => Node::Wrap(OF_TYPES[self.pick(OF_TYPES.len())], self.ty(depth)),
            4 => {
                let pair = [Pair::SecondaryVTable, Pair::ITab][self.pick(2)];
                Node::Pair(pair, self.ty(depth), self.ty(depth))
            }
            5 => Node::Data(self.name()),
            6 => Node::PkgInit(self.path()),
            7 => {
                let method = self.method(depth);
                match self.pick(2) {
                    0 => Node::Wrap(Wrap::IfacePtrThunk, method),
                    _ => Node::BaseAdjustThunk(self.pick(4096) as i64 - 2048, method),
                }
            }
            _ => {
                let mut of = match self.pick(2) {
                    0 => self.method(depth),
                    _ => self.code(0),
                };
                if matches!(self.cx.0.get(of), Node::Var { .. } | Node::InitFunc { .. }) {
                    of = self.method(depth);
                }
                Node::Wrap(THUNKS[self.pick(THUNKS.len())], of)
            }
        };
        self.cx.add(node)
    }
}

/// `count` entities of every kind, as deep as 4, the same for a seed.
pub(crate) fn corpus(cx: &mut Cx, count: usize, seed: u64) -> Vec<NodeId> {
    let mut generator = Generator {
        cx,
        rng: ChaCha8Rng::seed_from_u64(seed),
        types: Vec::new(),
    };
    (0..count).map(|_| generator.entity(4)).collect()
}

/// The readable text, written again without the printer.
fn oracle(context: &Context, id: NodeId) -> String {
    let show = |id: NodeId| oracle(context, id);
    let text = |name: &[u8]| String::from_utf8_lossy(name).into_owned();
    let list = |ids: &[NodeId], between: &str| {
        let shown: Vec<String> = ids.iter().map(|&id| show(id)).collect();
        shown.join(between)
    };
    let signature = |id: NodeId| {
        let Node::Signature {
            variadic,
            params,
            results,
        } = context.get(id)
        else {
            panic!("no function type");
        };
        let (Node::Tuple(params), Node::Tuple(results)) =
            (context.get(*params), context.get(*results))
        else {
            panic!("no tuples");
        };
        let mut shown: Vec<String> = params.iter().map(|&id| show(id)).collect();
        if *variadic {
            let last = shown.last_mut().unwrap();
            *last = format!("...{}", last.strip_prefix("[]").unwrap());
        }
        let results = match results.len() {
            0 => String::new(),
            1 => format!(" {}", show(results[0])),
            _ => format!(" ({})", list(results, ", ")),
        };
        format!("({}){results}", shown.join(", "))
    };
    match context.get(id) {
        Node::Basic(basic) => basic.name().to_owned(),
        Node::Pointer(of) => format!("*{}", show(*of)),
        Node::Slice(of) => format!("[]{}", show(*of)),
        Node::Array(len, of) => format!("[{len}]{}", show(*of)),
        Node::Map(key, value) => format!("map[{}]{}", show(*key), show(*value)),
        Node::Option(of) => format!("Option[{}]", show(*of)),
        Node::Chan(Dir::Both, of) => format!("chan {}", show(*of)),
        Node::Chan(Dir::Send, of) => format!("chan<- {}", show(*of)),
        Node::Chan(Dir::Recv, of) => format!("<-chan {}", show(*of)),
        Node::Tuple(types) => format!("({})", list(types, ", ")),
        Node::Signature { .. } => format!("func{}", signature(id)),
        Node::Struct(fields) => format!("struct{{{}}}", list(fields, "; ")),
        Node::Field {
            name,
            embedded,
            required,
            parent,
            tag,
            ty,
            ..
        } => {
            let mut shown = match embedded {
                true => show(*ty),
                false => format!("{} {}", text(name), show(*ty)),
            };
            shown += match required {
                Required::No => "",
                Required::Yes => " @Required",
                Required::MustSet => " @MustSet",
            };
            if *parent {
                shown += " @Parent";
            }
            if let Some(tag) = tag {
                shown += &format!(" {:?}", text(tag));
            }
            shown
        }
        Node::Interface { sync, methods } => {
            let mut parts: Vec<String> = methods.iter().map(|&id| show(id)).collect();
            if *sync {
                parts.push("Sync".to_owned());
            }
            match parts.is_empty() {
                true => "any".to_owned(),
                false => format!("interface{{{}}}", parts.join("; ")),
            }
        }
        Node::IMethod { name, sig, .. } => format!("{}{}", text(name), signature(*sig)),
        Node::Named { package, name }
        | Node::Func { package, name }
        | Node::Var { package, name } => {
            format!("{}.{}", text(package), text(name))
        }
        Node::LocalNamed {
            ordinal: 0,
            name,
            within,
        } => format!("{}.{}", show(*within), text(name)),
        Node::LocalNamed {
            ordinal,
            name,
            within,
        } => format!("{}.{}.{ordinal}", show(*within), text(name)),
        Node::Universe(name) => text(name),
        Node::Instance { of, args } => format!("{}[{}]", show(*of), list(args, ", ")),
        Node::TypeParam(index) => format!("${index}"),
        Node::Method { name, receiver } => format!("{}.{}", show(*receiver), text(name)),
        Node::OpMethod {
            right,
            name,
            receiver,
            params,
        } => {
            let right = if *right { " @Right" } else { "" };
            format!("{}.{}{}{right}", show(*receiver), text(name), show(*params))
        }
        Node::Closure { ordinal, within } => format!("{}.func{ordinal}", show(*within)),
        Node::PkgInit(package) => format!("{}.init", text(package)),
        Node::InitFunc { package, ordinal } => format!("{}.init.{ordinal}", text(package)),
        Node::Wrap(Wrap::IfacePtrThunk, of) => {
            let (method, args) = match context.get(*of) {
                Node::Instance { of, args } => (*of, format!("[{}]", list(args, ", "))),
                _ => (*of, String::new()),
            };
            let shown = match context.get(method) {
                Node::Method { name, receiver } => format!("(*{}).{}", show(*receiver), text(name)),
                Node::OpMethod {
                    right,
                    name,
                    receiver,
                    params,
                } => {
                    let right = if *right { " @Right" } else { "" };
                    format!(
                        "(*{}).{}{}{right}",
                        show(*receiver),
                        text(name),
                        show(*params)
                    )
                }
                _ => panic!("no method"),
            };
            format!("interface thunk for {shown}{args}")
        }
        Node::Wrap(wrap, of) => {
            let what = match wrap {
                Wrap::TypeInfo => "typeinfo for",
                Wrap::VTable => "vtable for",
                Wrap::EqGlue => "eq glue for",
                Wrap::HashGlue => "hash glue for",
                Wrap::DropGlue => "drop glue for",
                Wrap::CloneGlue => "clone glue for",
                Wrap::MethodTable => "method table for",
                Wrap::IfaceDesc => "interface descriptor for",
                Wrap::CabiThunk => "C-ABI thunk for",
                Wrap::MethodValue => "method value",
                Wrap::MethodExpr => "method expression",
                Wrap::Promoted => "promoted method",
                Wrap::IfaceThunk | Wrap::IfacePtrThunk => "interface thunk for",
            };
            format!("{what} {}", show(*of))
        }
        Node::Pair(Pair::SecondaryVTable, first, second) => {
            format!("vtable for {} as {}", show(*first), show(*second))
        }
        Node::Pair(Pair::ITab, first, second) => {
            format!("itab for {} as {}", show(*first), show(*second))
        }
        Node::Data(key) => format!("data {}", text(key)),
        Node::BaseAdjustThunk(offset, of) => format!("thunk at offset {offset} for {}", show(*of)),
    }
}

#[test]
fn corpus_round_trips() {
    let mut cx = Cx::default();
    let entities = corpus(&mut cx, 6000, 1);
    let mut names: HashMap<String, NodeId> = HashMap::new();
    let mut kinds = HashSet::new();
    let mut referred = 0;
    for &entity in &entities {
        let name = round_trip(&cx, entity);
        referred += usize::from(name.contains('B'));
        assert_eq!(
            cx.0.readable(entity, usize::MAX),
            oracle(&cx.0, entity),
            "{name}"
        );
        // One entity, one name.
        let known = *names.entry(name).or_insert(entity);
        assert_eq!(known, entity);
        kinds.insert(std::mem::discriminant(cx.0.get(entity)));
    }
    assert!(names.len() > 4000, "{}", names.len());
    assert!(referred > 1000, "{referred}");
    assert_eq!(kinds.len(), 12);
}

/// The node of `from` in `to`.
fn copy(from: &Context, to: &mut Context, id: NodeId) -> NodeId {
    let mut each = |id: NodeId| copy(from, to, id);
    fn all(ids: &[NodeId], each: &mut impl FnMut(NodeId) -> NodeId) -> Box<[NodeId]> {
        ids.iter().map(|&id| each(id)).collect()
    }
    let node = match from.get(id).clone() {
        Node::Pointer(of) => Node::Pointer(each(of)),
        Node::Slice(of) => Node::Slice(each(of)),
        Node::Array(len, of) => Node::Array(len, each(of)),
        Node::Map(key, value) => Node::Map(each(key), each(value)),
        Node::Option(of) => Node::Option(each(of)),
        Node::Chan(dir, of) => Node::Chan(dir, each(of)),
        Node::Tuple(types) => Node::Tuple(all(&types, &mut each)),
        Node::Signature {
            variadic,
            params,
            results,
        } => Node::Signature {
            variadic,
            params: each(params),
            results: each(results),
        },
        Node::Struct(fields) => Node::Struct(all(&fields, &mut each)),
        Node::Field {
            name,
            package,
            embedded,
            required,
            parent,
            tag,
            ty,
        } => Node::Field {
            name,
            package,
            embedded,
            required,
            parent,
            tag,
            ty: each(ty),
        },
        Node::Interface { sync, methods } => Node::Interface {
            sync,
            methods: all(&methods, &mut each),
        },
        Node::IMethod { package, name, sig } => Node::IMethod {
            package,
            name,
            sig: each(sig),
        },
        Node::LocalNamed {
            ordinal,
            name,
            within,
        } => Node::LocalNamed {
            ordinal,
            name,
            within: each(within),
        },
        Node::Instance { of, args } => Node::Instance {
            of: each(of),
            args: all(&args, &mut each),
        },
        Node::Method { name, receiver } => Node::Method {
            name,
            receiver: each(receiver),
        },
        Node::OpMethod {
            right,
            name,
            receiver,
            params,
        } => Node::OpMethod {
            right,
            name,
            receiver: each(receiver),
            params: each(params),
        },
        Node::Closure { ordinal, within } => Node::Closure {
            ordinal,
            within: each(within),
        },
        Node::Wrap(wrap, of) => Node::Wrap(wrap, each(of)),
        Node::Pair(pair, first, second) => Node::Pair(pair, each(first), each(second)),
        Node::BaseAdjustThunk(offset, of) => Node::BaseAdjustThunk(offset, each(of)),
        leaf => leaf,
    };
    to.add(node).unwrap()
}

#[test]
fn same_name_whatever_the_build_order() {
    let mut cx = Cx::default();
    let entities = corpus(&mut cx, 400, 2);
    let names: Vec<String> = entities.iter().map(|&entity| cx.symbol(entity)).collect();
    let mut rng = ChaCha8Rng::seed_from_u64(3);
    for round in 0..12 {
        // Other nodes first, more of them each round.
        let mut other = Cx::default();
        corpus(&mut other, round * 50, 100 + round as u64);
        let mut order: Vec<usize> = (0..entities.len()).collect();
        for at in (1..order.len()).rev() {
            order.swap(at, rng.random_range(0..=at));
        }
        for at in order {
            let copied = copy(&cx.0, &mut other.0, entities[at]);
            assert_eq!(other.symbol(copied), names[at]);
        }
    }
}

#[test]
fn a_type_is_referred_to_from_eight_characters_on() {
    let mut cx = Cx::default();
    // `u5error` is 7 characters, `u6errors` is 8.
    let short = cx.add(Node::Universe(bytes("error")));
    let long = cx.add(Node::Universe(bytes("errors")));
    let twice = cx.add(Node::Pair(Pair::ITab, short, short));
    assert_eq!(cx.symbol(twice), "_GPTau5erroru5error");
    let twice = cx.add(Node::Pair(Pair::ITab, long, long));
    assert_eq!(cx.symbol(twice), "_GPTau6errorsB5_");
    // What is short as it is written, but long written out, is referred
    // to as well.
    let pair = cx.named("main", "Pair");
    let inner = cx.instance(pair, &[pair]);
    let tuple = cx.tuple(&[inner, inner]);
    let entity = cx.typeinfo(tuple);
    assert_eq!(cx.symbol(entity), "_GPTiLIN4mainE4PairB7_EB6_E");
    // An entity is never referred to, however often it occurs.
    let func = cx.func("main", "generic");
    let local = cx.add(Node::LocalNamed {
        ordinal: 0,
        name: bytes("T"),
        within: func,
    });
    let local = cx.instance(func, &[local]);
    assert_eq!(cx.symbol(local), "_GPIF4mainE7genericg0_1TF4mainE7genericE");
    for entity in [twice, entity, local] {
        round_trip(&cx, entity);
    }
}

fn refused(text: &str) -> ParseError {
    match parse(&mut Context::new(), text.as_bytes()) {
        Ok(_) => panic!("{text} is taken for a name"),
        Err(error) => error,
    }
}

#[test]
fn only_what_the_mangler_writes_is_a_name() {
    // The text, and where it is wrong.
    let cases = [
        ("", 0),
        ("_G", 0),
        ("_GQF4mainE3Foo", 0),
        ("GPF4mainE3Foo", 0),
        ("_GP", 3),
        ("_GPF4mainE3Foo ", 14),
        ("_GPF4mainE3FooE", 14),
        ("_GPF4mainE03Foo", 10),
        ("_GPF4mainE3Fo", 10),
        ("_GPF4mainE", 10),
        ("_GPF4main3Foo", 13),
        ("_GPFE3Foo", 4),
        ("_GPF4mainE99999999999999999999999Foo", 10),
        // A character that is written as itself, escaped.
        ("_GPF4mainE4$41$", 11),
        ("_GPF4mainE6a$30$b", 11),
        ("_GPF4mainE5$041$", 11),
        ("_GPF4mainE6$65E5$", 11),
        ("_GPF4mainE6$d800$", 11),
        ("_GPF4mainE8$110000$", 11),
        ("_GPF4mainE5$x41$", 11),
        ("_GPF4mainE10$xc3$$xa9$", 12),
        ("_GPF4mainE1.", 11),
        ("_GPF4mainE2\u{e9}", 11),
        ("_GPF4mainE1$", 11),
        ("_GPF4mainE3$$$", 11),
        // A digit first, which is a length.
        ("_GPF4mainE10x", 10),
        ("_GPF6a$2f$bE1f", 4),
        // Numbers.
        ("_GPK4mainE01_", 10),
        ("_GPK4mainE1", 11),
        ("_GPK4mainE_", 10),
        ("_GPK4mainE18446744073709551616_", 10),
        ("_GPTtn0_M1mN1pE1T", 5),
        ("_GPTt9223372036854775808_M1mN1pE1T", 5),
        ("_GPTtn9223372036854775809_M1mN1pE1T", 5),
        // Children of the wrong kind.
        ("_GPi", 3),
        ("_GPN4mainE3Foo", 3),
        ("_GPM3Leni", 3),
        ("_GPM3LenF4mainE3Foo", 3),
        ("_GPTiF4mainE3Foo", 3),
        ("_GPTCi", 3),
        ("_GPTCR4mainE1v", 3),
        ("_GPTpF4mainE1f", 3),
        ("_GPIiiE", 3),
        ("_GPIN4mainE1TE", 3),
        ("_GPIE", 3),
        ("_GPIN4mainE1TF4mainE1fE", 3),
        ("_GPTiG0_0_iLE", 5),
        ("_GPTiG1_0_LiELE", 5),
        ("_GPTiG2_0_LELE", 6),
        ("_GPTiG0_1_LELE", 8),
        ("_GPTiZ1_E", 6),
        ("_GPTiZ0_iE", 5),
        ("_GPTiLq0_1aiE", 5),
        ("_GPTiYiE", 5),
        ("_GPTiY0_E", 6),
        ("_GPTiY2_E", 6),
        ("_GPTiXxi", 6),
        ("_GPTon3AddN1pE1TLE", 5),
        ("_GPTo6op_AddN1pE1Ti", 3),
        ("_GPTx", 3),
        ("_GPW", 3),
        ("_GPE", 3),
        // Field flags that are not known, or do not go together.
        ("_GPTiZ0_q16_1aiE", 9),
        ("_GPTiZ0_q64_1aiE", 9),
        ("_GPTiZ0_q136_1aiE", 9),
        ("_GPTiZ0_q256_1aiE", 9),
        ("_GPTiZ0_q2_1aiE", 13),
        // The methods of an interface out of order, or twice.
        ("_GPTiYn01bG0_0_LELEn01aB10_E", 5),
        ("_GPTiYn01aG0_0_LELEn01aB10_E", 5),
        ("_GPTiYn11pE1aG0_0_LELEn01aB13_E", 5),
        ("_GPTiYn01aG0_0_LELEn01bG0_0_LELEE", 23),
        ("_GPTiYn21aG0_0_LELEE", 7),
        // Back references.
        ("_GPTiB5_", 5),
        ("_GPTaN4mainE3FooN4mainE3Foo", 16),
        ("_GPTaN4mainE3FooB6_", 16),
        ("_GPTaN4mainE3FooB05_", 17),
        ("_GPTaN4mainE3FooB3_", 16),
        ("_GPTau5errorB5_", 12),
        ("_GPTiPPPPPPPPB5_", 13),
        ("_GPTiLPPPPPPPPiB14_E", 15),
    ];
    let mut wrong = Vec::new();
    for (text, offset) in cases {
        match parse(&mut Context::new(), text.as_bytes()) {
            Ok(_) => wrong.push(format!("{text}: a name")),
            Err(error) if error.offset != offset => wrong.push(format!("{text}: {error}")),
            Err(_) => {}
        }
    }
    assert!(wrong.is_empty(), "{wrong:#?}");
    // What is nearly the same and is a name.
    let names = [
        "_GPF4mainE3Foo",
        "_GPF4mainE5$30$0",
        "_GPF4mainE7$65e5$a",
        "_GPF4mainE6$xc3$a",
        "_GPF0_E0_",
        "_GPK4mainE0_",
        "_GPTtn9223372036854775808_M1mN1pE1T",
        "_GPTt9223372036854775807_M1mN1pE1T",
        "_GPTiG1_0_LSiELE",
        "_GPTiZ0_q167_1pE1a1tiE",
        "_GPTiZ0_q47_1pE1a1tiE",
        "_GPTiY1_E",
        "_GPTiYn01aG0_0_LELEn01bB10_n11pE1aB10_E",
        "_GPTaN4mainE3FooB5_",
        "_GPTaLN4mainE3FooEB5_",
        "_GPTaLN4mainE3FooEB6_",
        "_GPTiLPPPPPPPPiB6_B7_E",
    ];
    for name in names {
        let mut context = Context::new();
        let entity =
            parse(&mut context, name.as_bytes()).unwrap_or_else(|error| panic!("{name}: {error}"));
        assert_eq!(context.symbol(entity), name);
    }
    let error = refused("_GPF4mainE03Foo");
    assert_eq!(
        error.to_string(),
        "a number has a leading zero at offset 10"
    );
}

#[test]
fn every_prefix_of_a_valid_name_is_rejected() {
    let mut cx = Cx::default();
    for entity in corpus(&mut cx, 300, 4) {
        let name = cx.symbol(entity);
        for end in 0..name.len() {
            assert!(
                parse(&mut Context::new(), &name.as_bytes()[..end]).is_err(),
                "{name} cut at {end}"
            );
        }
    }
}

const ALPHABET: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_$";

/// The name with one to three small changes.
pub(crate) fn mutant(name: &[u8], donor: &[u8], rng: &mut ChaCha8Rng) -> Vec<u8> {
    let mut text = name.to_vec();
    for _ in 0..rng.random_range(1..=3) {
        let at = rng.random_range(0..=text.len());
        let letter = ALPHABET[rng.random_range(0..ALPHABET.len())];
        match rng.random_range(0..7) {
            0 => text.insert(at, letter),
            1 if at < text.len() => text[at] = letter,
            2 if at < text.len() => {
                text.remove(at);
            }
            3 if at < text.len() => {
                let other = rng.random_range(0..text.len());
                text.swap(at, other);
            }
            4 if at < text.len() => {
                let end = rng.random_range(at..=text.len());
                let piece = text[at..end].to_vec();
                text.splice(at..at, piece);
            }
            5 => {
                let from = rng.random_range(0..=donor.len());
                let end = rng.random_range(from..=donor.len());
                text.splice(at..at, donor[from..end].iter().copied());
            }
            _ => text.truncate(at),
        }
    }
    text
}

/// What must hold for any text: it is refused, or it is the one name of
/// what it names. Whether it was a name.
pub(crate) fn holds(text: &[u8]) -> bool {
    let mut context = Context::new();
    let Ok(entity) = parse(&mut context, text) else {
        return false;
    };
    assert_eq!(context.symbol(entity).as_bytes(), text);
    assert!(context.readable(entity, 4096).len() <= 4096);
    true
}

#[test]
fn mutated_names_never_crash() {
    let mut cx = Cx::default();
    let names: Vec<String> = corpus(&mut cx, 400, 5)
        .into_iter()
        .map(|entity| cx.symbol(entity))
        .collect();
    let mut rng = ChaCha8Rng::seed_from_u64(6);
    let (mut taken, total) = (0, 20_000);
    for round in 0..total {
        let name = names[round % names.len()].as_bytes();
        let donor = names[rng.random_range(0..names.len())].as_bytes();
        taken += usize::from(holds(&mutant(name, donor, &mut rng)));
    }
    // Some changes give another name, most give none.
    assert!(taken > total / 100 && taken < total / 2, "{taken}");
}

#[test]
fn random_text_never_crashes() {
    let mut rng = ChaCha8Rng::seed_from_u64(8);
    for round in 0..20_000 {
        let len = rng.random_range(0..40);
        let mut text: Vec<u8> = match round % 4 {
            0 => Vec::new(),
            _ => PREFIX.as_bytes().to_vec(),
        };
        for _ in 0..len {
            text.push(match round % 3 {
                0 => rng.random_range(0..=255u8),
                _ => ALPHABET[rng.random_range(0..ALPHABET.len())],
            });
        }
        holds(&text);
    }
}

#[test]
fn hostile_shapes() {
    let big = 100_000;
    let repeat = |piece: &str, count: usize| piece.repeat(count);
    let texts = [
        format!("_GPTi{}", repeat("P", big)),
        format!("_GPTi{}", repeat("L", big)),
        format!("_GPTi{}", repeat("I", big)),
        format!("_GPTi{}", repeat("E", big)),
        format!("_GPTi{}i", repeat("Tt1_", big)),
        format!("_GP{}F1pE1f", repeat("C1_", big)),
        format!("_GPTiL{}E", repeat("Z0_E", big)),
        format!("_GPTiLN4mainE3Foo{}E", repeat("B6_", big)),
        format!("_GPTiLN4mainE3Foo{}E", repeat("B7_", big)),
        format!("_GPTi{}", repeat("B5_", big)),
        format!("_GPF{}E1f", repeat("0_", big)),
        format!("_GPF{}", repeat("9", big)),
        format!("_GPF1pE{}{}", big, repeat("$", big)),
        format!("_GPTiZ0_{}E", repeat("q0_1ai", big)),
        format!("_GPTiY{}E", repeat("n01aG0_0_LELE", big)),
        format!("_GPTiG0_0_L{}ELE", repeat("G0_0_LELE", big)),
    ];
    for text in &texts {
        holds(text.as_bytes());
    }
    // Those of them that are names.
    let names = [5, 6, 7, 10, 13];
    for (at, text) in texts.iter().enumerate() {
        let is_name = parse(&mut Context::new(), text.as_bytes()).is_ok();
        assert_eq!(is_name, names.contains(&at), "shape {at}");
    }
}

/// `Pair[T, T]` over itself `depth` times: each level names the one
/// below twice.
fn shared_halves(cx: &mut Cx, depth: usize) -> NodeId {
    let pair = cx.named("main", "Pair");
    let mut ty = cx.int();
    for _ in 0..depth {
        ty = cx.instance(pair, &[ty, ty]);
    }
    cx.typeinfo(ty)
}

#[test]
fn exponential_sharing_stays_small() {
    let mut cx = Cx::default();
    let entity = shared_halves(&mut cx, 200);
    let name = cx.symbol(entity);
    // Written out it is 2^200 times `main.Pair`.
    assert!(name.len() < 200 * 12, "{}", name.len());
    assert_eq!(round_trip(&cx, entity), name);
    let readable = demangle(name.as_bytes(), 1000).unwrap();
    assert_eq!(readable.len(), 1000);
    assert!(readable.starts_with("typeinfo for main.Pair[main.Pair[main.Pair["));
    // The text is cut between two characters.
    let wide = cx.func("main", &"日".repeat(100));
    assert_eq!(cx.0.readable(wide, 8), "main.日");
    assert_eq!(cx.0.readable(wide, 0), "");
}

fn deep(cx: &mut Cx, depth: usize) -> NodeId {
    let mut ty = cx.int();
    for level in 0..depth {
        ty = match level % 4 {
            0 => cx.add(Node::Pointer(ty)),
            1 => cx.add(Node::Slice(ty)),
            2 => cx.add(Node::Option(ty)),
            _ => cx.tuple(&[ty]),
        };
    }
    cx.typeinfo(ty)
}

fn wide(cx: &mut Cx, count: usize) -> NodeId {
    let int = cx.int();
    let types: Vec<NodeId> = (0..count)
        .map(|at| cx.add(Node::Array(at as u64, int)))
        .collect();
    let tuple = cx.tuple(&types);
    cx.typeinfo(tuple)
}

/// Shapes of name that grow with `size`, for what the work is measured
/// on.
pub(crate) fn shapes(cx: &mut Cx, size: usize) -> Vec<NodeId> {
    let int = cx.int();
    let named = cx.named("main", "Vec2");
    let referred = vec![named; size];
    let referred = cx.tuple(&referred);
    let mut closure = cx.func("main", "f");
    for _ in 0..size {
        closure = cx.add(Node::Closure {
            ordinal: 1,
            within: closure,
        });
    }
    let fields: Vec<NodeId> = (0..size)
        .map(|at| {
            cx.add(Node::Field {
                name: bytes(&format!("f{at}")),
                package: None,
                embedded: false,
                required: Required::No,
                parent: false,
                tag: None,
                ty: int,
            })
        })
        .collect();
    let strukt = cx.add(Node::Struct(fields.into()));
    vec![
        deep(cx, size),
        wide(cx, size),
        shared_halves(cx, size),
        cx.func("main", &"long日".repeat(size / 4)),
        cx.func(&"seg/".repeat(size / 4), "f"),
        cx.typeinfo(referred),
        closure,
        cx.typeinfo(strukt),
    ]
}

#[test]
fn deep_and_wide_names_work() {
    let mut cx = Cx::default();
    for entity in shapes(&mut cx, 40_000) {
        let name = cx.symbol(entity);
        let mut other = Context::new();
        let parsed = parse(&mut other, name.as_bytes()).unwrap();
        assert_eq!(other.symbol(parsed), name);
        assert_eq!(
            other.readable(parsed, 1 << 20),
            cx.0.readable(entity, 1 << 20)
        );
    }
}

#[test]
fn long_names_take_linear_steps() {
    for shape in 0..8 {
        assert_near_linear(&[1_000, 4_000, 16_000], |size| {
            let mut cx = Cx::default();
            let entity = shapes(&mut cx, size)[shape];
            let (name, written) = cx.0.symbol_counted(entity);
            let (parsed, read) = parse_counted(&mut Context::new(), name.as_bytes()).unwrap();
            let _ = parsed;
            let (_, shown) = cx.0.readable_counted(entity, 1 << 20);
            written + read + shown
        });
    }
}

#[test]
fn a_word_that_is_a_name_is_replaced() {
    let text = "0x10 _GPF4mainE3Foo+0x4 (_GPM3LenN4mainE4Vec2) _GPF4mainE3Fo _Z3foov\n\t_GPJ4mainE";
    assert_eq!(
        filter(text, 100),
        "0x10 main.Foo+0x4 (main.Vec2.Len) _GPF4mainE3Fo _Z3foov\n\tmain.init"
    );
    assert_eq!(filter("", 100), "");
    assert_eq!(filter("_GPF4mainE3Foo", 6), "main.F");
}
