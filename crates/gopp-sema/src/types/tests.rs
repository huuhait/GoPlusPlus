use gopp_span::Dialect;
use lasso::ThreadedRodeo;
use rayon::prelude::*;

use super::*;

struct Fixture {
    names: ThreadedRodeo,
    types: Types,
}

impl Fixture {
    fn new() -> Self {
        let names = ThreadedRodeo::new();
        let types = Types::new(&names);
        Self { names, types }
    }

    fn basic(&self, basic: Basic) -> TypeId {
        self.types.basic(basic)
    }

    fn field(&self, name: &str, ty: TypeId) -> Field {
        Field {
            name: self.names.get_or_intern(name),
            package: None,
            ty,
            embedded: false,
            tag: None,
            required: false,
        }
    }

    fn method(&self, name: &str, params: &[TypeId], results: &[TypeId]) -> Method {
        Method {
            name: self.names.get_or_intern(name),
            package: None,
            sig: self.types.func(params, results, false),
        }
    }

    fn interface(&self, methods: Vec<Method>, embedded: Vec<TypeId>) -> TypeId {
        self.types.interface(methods, embedded, &self.names)
    }

    /// A declared type `name` of the package `p` with the underlying type
    /// `underlying`.
    fn declared(&self, name: &str, underlying: TypeId) -> (NamedId, TypeId) {
        let package = self.names.get_or_intern("p");
        let named = self.types.declare(self.names.get_or_intern(name), package);
        self.types.set_underlying(named, underlying);
        (named, self.types.named(named, &[]))
    }

    fn show(&self, id: TypeId) -> String {
        self.types.display(id, &self.names)
    }

    fn assignable(&self, from: TypeId, to: TypeId, dialect: Dialect) -> bool {
        self.types.assignable(from, None, to, dialect, &self.names)
    }
}

#[test]
fn a_basic_type_is_found_without_a_lookup() {
    let f = Fixture::new();
    for basic in BASICS {
        assert_eq!(*f.types.get(f.basic(basic)), Type::Basic(basic));
    }
    assert_eq!(*f.types.get(Types::INVALID), Type::Invalid);
}

#[test]
fn the_same_type_built_twice_is_one_type() {
    let f = Fixture::new();
    let (int, string) = (f.basic(Basic::Int), f.basic(Basic::String));
    assert_eq!(f.types.slice(int), f.types.slice(int));
    assert_ne!(f.types.slice(int), f.types.slice(string));
    assert_ne!(f.types.slice(int), f.types.array(3, int));
    assert_ne!(f.types.array(3, int), f.types.array(4, int));
    assert_eq!(
        f.types.map(string, f.types.slice(int)),
        f.types.map(string, f.types.slice(int))
    );
    assert_ne!(
        f.types.chan(ChanDir::Send, int),
        f.types.chan(ChanDir::Recv, int)
    );
    assert_ne!(f.types.pointer(int), f.types.option(int));
    // The names of parameters are no part of a function type.
    assert_eq!(
        f.types.func(&[int], &[string], false),
        f.types.func(&[int], &[string], false)
    );
    assert_ne!(
        f.types.func(&[f.types.slice(int)], &[], true),
        f.types.func(&[f.types.slice(int)], &[], false)
    );
}

#[test]
fn a_function_that_can_fail_is_the_function_that_returns_an_error() {
    let f = Fixture::new();
    let (int, string) = (f.basic(Basic::Int), f.basic(Basic::String));
    let error = f.types.error();
    assert_eq!(
        f.types.failing_func(&[int], &[string], false),
        f.types.func(&[int], &[string, error], false)
    );
    assert_eq!(
        f.types.failing_func(&[], &[], false),
        f.types.func(&[], &[error], false)
    );
    assert_eq!(
        f.show(f.types.failing_func(&[int], &[string], false)),
        "func(int) (string, error)"
    );
}

#[test]
fn struct_types_are_identical_field_for_field() {
    let f = Fixture::new();
    let int = f.basic(Basic::Int);
    let base = || vec![f.field("a", int), f.field("b", int)];
    let with = |change: &dyn Fn(&mut Field)| {
        let mut fields = base();
        change(&mut fields[0]);
        f.types.strukt(fields)
    };
    let same = f.types.strukt(base());
    assert_eq!(same, f.types.strukt(base()));
    let other_package = f.names.get_or_intern("other/p");
    let different = [
        with(&|field| field.name = f.names.get_or_intern("c")),
        with(&|field| field.ty = f.basic(Basic::String)),
        with(&|field| field.embedded = true),
        with(&|field| field.tag = Some(b"json:\"a\"".as_slice().into())),
        // `@Required` is part of the type (TYP-15).
        with(&|field| field.required = true),
        // A name that is not exported is a name of its package.
        with(&|field| field.package = Some(other_package)),
        f.types.strukt(vec![f.field("b", int), f.field("a", int)]),
        f.types.strukt(vec![f.field("a", int)]),
    ];
    for (index, &ty) in different.iter().enumerate() {
        assert_ne!(ty, same, "{index}");
        assert!(!f.types.identical(ty, same, &f.names), "{index}");
    }
    assert_eq!(
        f.show(with(&|field| {
            field.required = true;
            field.tag = Some(b"k".as_slice().into());
        })),
        "struct{a int @Required \"k\"; b int}"
    );
}

#[test]
fn interfaces_are_identical_when_their_methods_are() {
    let f = Fixture::new();
    let int = f.basic(Basic::Int);
    let read = || f.method("Read", &[int], &[int]);
    let close = || f.method("Close", &[], &[]);
    let both = f.interface(vec![read(), close()], vec![]);
    // The order of the methods does not matter.
    assert_eq!(both, f.interface(vec![close(), read()], vec![]));
    let (_, reader) = f.declared("Reader", f.interface(vec![read()], vec![]));
    let (_, closer) = f.declared("Closer", f.interface(vec![close()], vec![]));
    let embedding = f.interface(vec![close()], vec![reader]);
    let embedding_both = f.interface(vec![], vec![closer, reader, reader]);
    assert_ne!(both, embedding);
    for other in [embedding, embedding_both] {
        assert!(f.types.identical(both, other, &f.names));
        assert!(f.types.identical(other, both, &f.names));
    }
    assert!(
        !f.types
            .identical(both, f.interface(vec![read()], vec![]), &f.names)
    );
    assert!(!f.types.identical(both, f.types.any(), &f.names));
    assert!(!f.types.identical(both, int, &f.names));
    // A declared interface is its own type.
    assert!(
        !f.types
            .identical(reader, f.interface(vec![read()], vec![]), &f.names)
    );
    assert_eq!(f.show(embedding), "interface{Close(); p.Reader}");
    assert_eq!(f.show(f.types.any()), "any");
}

#[test]
fn a_declared_type_is_not_the_type_it_was_declared_as() {
    let f = Fixture::new();
    let int = f.basic(Basic::Int);
    let ints = f.types.slice(int);
    let (_, list) = f.declared("List", ints);
    let (_, other) = f.declared("Other", ints);
    let (_, list_of_lists) = f.declared("Lists", list);
    assert_ne!(list, other);
    assert_eq!(f.types.underlying(list), ints);
    // The underlying type of a type declared as a declared type is the
    // one at the end.
    assert_eq!(f.types.underlying(list_of_lists), ints);
    assert_eq!(f.types.underlying(ints), ints);
    assert_eq!(f.show(list), "p.List");
    // Until its declaration is checked, a declared type is in error.
    let pending = f
        .types
        .declare(f.names.get_or_intern("Pending"), f.names.get_or_intern("p"));
    assert_eq!(
        f.types.underlying(f.types.named(pending, &[])),
        Types::INVALID
    );
    // A generic type with different arguments is a different type.
    assert_ne!(
        f.types.named(pending, &[int]),
        f.types.named(pending, &[ints])
    );
    assert_eq!(
        f.show(f.types.named(pending, &[int, ints])),
        "p.Pending[int, []int]"
    );
    let (t, u) = (
        f.types.param(f.names.get_or_intern("T")),
        f.types.param(f.names.get_or_intern("T")),
    );
    assert_ne!(t, u);
    assert_eq!(f.show(t), "T");
}

#[test]
fn values_are_assignable_as_in_go() {
    let f = Fixture::new();
    let (int, string) = (f.basic(Basic::Int), f.basic(Basic::String));
    let ints = f.types.slice(int);
    let (_, list) = f.declared("List", ints);
    let (_, other) = f.declared("Other", ints);
    let (_, my_int) = f.declared("MyInt", int);
    let chan = |dir| f.types.chan(dir, int);
    let (_, my_chan) = f.declared("Chan", chan(ChanDir::Both));
    let (_, my_send) = f.declared("Send", chan(ChanDir::Send));
    let cases = [
        (int, int, true),
        (int, string, false),
        // Identical underlying types, and one side is not declared.
        (ints, list, true),
        (list, ints, true),
        (list, other, false),
        // `int` is a named type, so `MyInt` and `int` do not mix.
        (int, my_int, false),
        (my_int, int, false),
        (chan(ChanDir::Both), chan(ChanDir::Send), true),
        (chan(ChanDir::Both), chan(ChanDir::Recv), true),
        (chan(ChanDir::Send), chan(ChanDir::Both), false),
        (chan(ChanDir::Send), chan(ChanDir::Recv), false),
        (my_chan, chan(ChanDir::Send), true),
        (chan(ChanDir::Both), my_send, true),
        (my_chan, my_send, false),
        (int, f.types.any(), true),
        (list, f.types.any(), true),
        (f.types.any(), int, false),
        (Types::INVALID, int, true),
        (int, Types::INVALID, true),
    ];
    for (from, to, expected) in cases {
        for dialect in [Dialect::Go, Dialect::Gopp] {
            let (a, b) = (f.show(from), f.show(to));
            assert_eq!(f.assignable(from, to, dialect), expected, "{a} to {b}");
        }
    }
}

#[test]
fn nil_fits_fewer_types_in_a_gopp_file() {
    let f = Fixture::new();
    let int = f.basic(Basic::Int);
    let nil = f.basic(Basic::UntypedNil);
    let with_method = f.interface(vec![f.method("M", &[], &[])], vec![]);
    let (_, list) = f.declared("List", f.types.slice(int));
    let cases = [
        (f.types.slice(int), true, true),
        (list, true, true),
        (f.types.map(int, int), true, true),
        (f.types.chan(ChanDir::Both, int), true, true),
        (f.types.any(), true, true),
        (f.types.error(), true, true),
        (f.types.option(int), true, true),
        (f.types.option(f.types.pointer(int)), true, true),
        // Never nil in Go++ (TYP-2).
        (f.types.pointer(int), true, false),
        (f.types.func(&[], &[], false), true, false),
        (with_method, true, false),
        (int, false, false),
        (f.types.strukt(vec![]), false, false),
        (f.types.array(2, int), false, false),
    ];
    for (to, in_go, in_gopp) in cases {
        let shown = f.show(to);
        assert_eq!(f.assignable(nil, to, Dialect::Go), in_go, "{shown} in Go");
        assert_eq!(
            f.assignable(nil, to, Dialect::Gopp),
            in_gopp,
            "{shown} in Go++"
        );
    }
}

#[test]
fn an_untyped_constant_fits_a_type_that_can_hold_it() {
    let f = Fixture::new();
    let (_, my_byte) = f.declared("Byte", f.basic(Basic::Uint8));
    let with_method = f.interface(vec![f.method("M", &[], &[])], vec![]);
    let fits = |kind, value: &Value, to| {
        f.types
            .assignable(f.basic(kind), Some(value), to, Dialect::Gopp, &f.names)
    };
    let (small, large, half) = (Value::Int64(200), Value::Int64(300), Value::from_f64(0.5));
    assert!(fits(Basic::UntypedInt, &small, f.basic(Basic::Uint8)));
    assert!(fits(Basic::UntypedInt, &small, my_byte));
    assert!(!fits(Basic::UntypedInt, &large, my_byte));
    assert!(!fits(Basic::UntypedInt, &small, f.basic(Basic::Int8)));
    assert!(fits(Basic::UntypedInt, &small, f.basic(Basic::Float32)));
    assert!(!fits(Basic::UntypedFloat, &half, f.basic(Basic::Int)));
    assert!(!fits(Basic::UntypedInt, &small, f.basic(Basic::String)));
    assert!(fits(
        Basic::UntypedString,
        &Value::string(b"a"),
        f.basic(Basic::String)
    ));
    // An interface holds a value with a type: only the empty one takes a
    // constant that has none yet.
    assert!(fits(Basic::UntypedInt, &small, f.types.any()));
    assert!(!fits(Basic::UntypedInt, &small, with_method));
    // A value is its own option (TYP-2).
    assert!(fits(
        Basic::UntypedInt,
        &small,
        f.types.option(f.basic(Basic::Uint8))
    ));
    assert!(!fits(
        Basic::UntypedInt,
        &large,
        f.types.option(f.basic(Basic::Uint8))
    ));
    assert!(f.assignable(
        f.basic(Basic::Int),
        f.types.option(f.basic(Basic::Int)),
        Dialect::Gopp
    ));
    assert!(!f.assignable(
        f.types.option(f.basic(Basic::Int)),
        f.basic(Basic::Int),
        Dialect::Gopp
    ));
    // The result of a comparison is a boolean of no type yet.
    let untyped_bool = f.basic(Basic::UntypedBool);
    let (_, flag) = f.declared("Flag", f.basic(Basic::Bool));
    assert!(f.assignable(untyped_bool, flag, Dialect::Gopp));
    assert!(!f.assignable(untyped_bool, f.basic(Basic::Int), Dialect::Gopp));
    assert_eq!(
        f.types.default_type(f.basic(Basic::UntypedRune)),
        f.basic(Basic::Int32)
    );
    assert_eq!(f.types.default_type(flag), flag);
}

#[test]
fn a_type_implements_an_interface_whose_methods_it_has() {
    let f = Fixture::new();
    let (int, string) = (f.basic(Basic::Int), f.basic(Basic::String));
    let (named, file) = f.declared("File", f.types.strukt(vec![]));
    let declare = |method, pointer_receiver| {
        f.types.add_method(
            named,
            DeclaredMethod {
                method,
                pointer_receiver,
                receiver_params: Vec::new(),
            },
        );
    };
    declare(f.method("Name", &[], &[string]), false);
    declare(f.method("Write", &[int], &[]), true);
    declare(f.method("Error", &[], &[string]), false);
    let namer = f.interface(vec![f.method("Name", &[], &[string])], vec![]);
    let writer = f.interface(vec![f.method("Write", &[int], &[])], vec![]);
    let both = f.interface(vec![], vec![namer, writer]);
    let other_writer = f.interface(vec![f.method("Write", &[string], &[])], vec![]);
    let pointer = f.types.pointer(file);
    let cases = [
        (file, namer, true),
        (pointer, namer, true),
        // A method with a pointer receiver needs the pointer.
        (file, writer, false),
        (pointer, writer, true),
        (pointer, both, true),
        (file, both, false),
        (pointer, other_writer, false),
        (file, f.types.error(), true),
        (int, namer, false),
        (int, f.types.any(), true),
        // An interface with more methods is one with fewer.
        (both, namer, true),
        (namer, both, false),
    ];
    for (from, to, expected) in cases {
        let (a, b) = (f.show(from), f.show(to));
        assert_eq!(
            f.types.implements(from, to, &f.names),
            expected,
            "{a} as {b}"
        );
        assert_eq!(
            f.assignable(from, to, Dialect::Gopp),
            expected,
            "{a} to {b}"
        );
    }
}

#[test]
fn values_compare_when_all_their_parts_do() {
    let f = Fixture::new();
    let int = f.basic(Basic::Int);
    let ints = f.types.slice(int);
    let func = f.types.func(&[], &[], false);
    let unique = f.types.handle(Type::Unique, int).unwrap();
    let cases = [
        (int, true),
        (f.basic(Basic::String), true),
        (f.types.pointer(ints), true),
        (unique, true),
        (f.types.chan(ChanDir::Both, ints), true),
        (f.types.any(), true),
        (f.types.error(), true),
        (f.types.array(3, int), true),
        (
            f.types
                .strukt(vec![f.field("a", int), f.field("b", f.types.option(int))]),
            true,
        ),
        (f.types.option(int), true),
        (ints, false),
        (f.types.map(int, int), false),
        // A func is never nil in Go++, and never comparable (TYP-3).
        (func, false),
        (f.types.array(3, ints), false),
        (f.types.strukt(vec![f.field("a", ints)]), false),
        (f.types.option(ints), false),
        (f.basic(Basic::UntypedNil), false),
    ];
    for (ty, expected) in cases {
        assert_eq!(f.types.comparable(ty), expected, "{}", f.show(ty));
    }
    let (_, declared) = f.declared("Key", f.types.strukt(vec![f.field("a", ints)]));
    assert!(!f.types.comparable(declared));
}

#[test]
fn a_handle_takes_what_may_live_on_the_heap() {
    let f = Fixture::new();
    let int = f.basic(Basic::Int);
    let pointer = f.types.pointer(int);
    let (_, declared_pointer) = f.declared("Ref", pointer);
    let func = f.types.func(&[], &[], false);
    assert_eq!(
        f.show(f.types.handle(Type::Unique, int).unwrap()),
        "Unique[int]"
    );
    assert_eq!(
        f.show(f.types.handle(Type::Shared, f.types.slice(int)).unwrap()),
        "Shared[[]int]"
    );
    assert_eq!(
        f.show(f.types.handle(Type::Weak, int).unwrap()),
        "Weak[int]"
    );
    // A borrowed pointer may not live on the heap (OWN-B5).
    for make in [Type::Unique, Type::Shared, Type::Weak] {
        assert_eq!(
            f.types.handle(make, pointer),
            Err(BadArgument::PointerOnHeap)
        );
        assert_eq!(
            f.types.handle(make, declared_pointer),
            Err(BadArgument::PointerOnHeap)
        );
    }
    assert_eq!(
        f.show(f.types.handle(Type::Local, func).unwrap()),
        "Local[func()]"
    );
    assert_eq!(
        f.show(f.types.handle(Type::Local, f.types.any()).unwrap()),
        "Local[any]"
    );
    assert_eq!(
        f.types.handle(Type::Local, int),
        Err(BadArgument::LocalOfValue)
    );
    assert_eq!(
        f.types.handle(Type::Local, pointer),
        Err(BadArgument::LocalOfValue)
    );
}

#[test]
fn a_type_is_shown_as_a_program_writes_it() {
    let f = Fixture::new();
    let (int, string) = (f.basic(Basic::Int), f.basic(Basic::String));
    let strings = f.types.slice(string);
    let cases = [
        (f.types.map(string, f.types.slice(int)), "map[string][]int"),
        (f.types.pointer(f.types.array(4, int)), "*[4]int"),
        (f.types.chan(ChanDir::Recv, int), "<-chan int"),
        (f.types.chan(ChanDir::Send, int), "chan<- int"),
        (
            f.types.func(&[int, strings], &[], true),
            "func(int, ...string)",
        ),
        (
            f.types.func(&[int, strings], &[int], false),
            "func(int, []string) int",
        ),
        (f.types.option(f.types.pointer(int)), "Option[*int]"),
        (
            f.types.intern(Type::Tuple(Box::new([int, string]))),
            "(int, string)",
        ),
        (f.basic(Basic::UntypedFloat), "untyped float"),
        (Types::INVALID, "invalid type"),
    ];
    for (ty, text) in cases {
        assert_eq!(f.show(ty), text);
    }
}

#[test]
fn threads_that_build_the_same_type_get_the_same_id() {
    let f = Fixture::new();
    let int = f.basic(Basic::Int);
    let build = |depth: u64| {
        let mut ty = int;
        for level in 0..depth {
            ty = f.types.slice(f.types.array(level, ty));
        }
        ty
    };
    let ids: Vec<TypeId> = (0..64u64)
        .into_par_iter()
        .map(|i| build(i % 8 + 1))
        .collect();
    for (index, &id) in ids.iter().enumerate() {
        assert_eq!(id, build(index as u64 % 8 + 1));
    }
}
