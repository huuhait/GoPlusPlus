use gopp_target::TargetSpec;

use super::*;
use crate::types::Type;

fn target() -> TargetLayout {
    TargetSpec::named("linux/amd64").unwrap().layout()
}

fn rules() -> CTypeRules {
    CTypeRules::new(Some(target()))
}

/// A struct of the fields, under a name that no other has.
fn strukt(types: &mut TypeInterner, name: &str, fields: &[TypeId]) -> TypeId {
    let ty = types.declare_struct(name);
    let Type::Struct(id) = types.get(ty).clone() else {
        unreachable!()
    };
    let named: Vec<(String, TypeId)> = (fields.iter().enumerate())
        .map(|(at, &field)| (format!("f{at}"), field))
        .collect();
    let named: Vec<(&str, TypeId)> = named.iter().map(|(name, ty)| (&name[..], *ty)).collect();
    types.set_fields(id, &named).unwrap();
    ty
}

/// A struct in a struct, `depth` deep, around an `i32`.
fn nested(types: &mut TypeInterner, name: &str, depth: u32) -> Vec<TypeId> {
    let mut all = vec![types.int(32, true)];
    for level in 1..=depth {
        let inner = all[all.len() - 1];
        all.push(strukt(types, &format!("{name}{level}"), &[inner]));
    }
    all
}

fn c_func(types: &mut TypeInterner, params: &[TypeId], ret: TypeId) -> TypeId {
    types.intern(Type::Func(FuncType {
        params: params.into(),
        ret,
        has_env: false,
        cc: CallConv::C,
    }))
}

#[test]
fn numbers_pointers_and_structs_of_them_cross() {
    let mut types = TypeInterner::new();
    let int = types.int(32, true);
    let byte = types.int(8, false);
    let float = types.float(64);
    let string = types.intern(Type::String);
    let pair = strukt(&mut types, "pair", &[int, float]);
    let four = types.intern(Type::Array(int, 4));
    let none = types.intern(Type::Array(int, 0));
    let with_arrays = strukt(&mut types, "arrays", &[four, none, pair]);
    let callback = c_func(&mut types, &[int, pair], TypeInterner::UNIT);
    let crossing = [
        TypeInterner::BOOL,
        int,
        byte,
        float,
        types.ptr(string),
        types.ptr(four),
        pair,
        with_arrays,
        callback,
    ];
    let mut rules = rules();
    for ty in crossing {
        assert_eq!(rules.param(&types, ty), Ok(()), "{}", types.display(ty));
        assert_eq!(rules.result(&types, ty), Ok(()), "{}", types.display(ty));
    }
    assert_eq!(rules.result(&types, TypeInterner::UNIT), Ok(()));
}

#[test]
fn what_c_has_no_type_for_does_not_cross() {
    let mut types = TypeInterner::new();
    let int = types.int(32, true);
    let string = types.intern(Type::String);
    let slice = types.intern(Type::Slice(int));
    let option = types.intern(Type::Option(int));
    let tuple = types.tuple(&[int, int]);
    let empty = strukt(&mut types, "empty", &[]);
    let holds_empty = strukt(&mut types, "holds_empty", &[int, empty]);
    let holds_string = strukt(&mut types, "holds_string", &[int, string]);
    let of_none = types.intern(Type::Array(int, 0));
    let only_none = strukt(&mut types, "only_none", &[of_none]);
    let of_empty = types.intern(Type::Array(of_none, 3));
    let holds_of_empty = strukt(&mut types, "holds_of_empty", &[int, of_empty]);
    let go_func = types.intern(Type::Func(FuncType {
        params: [int].into(),
        ret: int,
        has_env: false,
        cc: CallConv::Go,
    }));
    let takes_string = c_func(&mut types, &[string], int);
    let gives_array = types.intern(Type::Array(int, 4));
    let gives_array = c_func(&mut types, &[], gives_array);
    let refused = [
        (TypeInterner::UNIT, TypeInterner::UNIT),
        (string, string),
        (slice, slice),
        (option, option),
        (tuple, tuple),
        (empty, empty),
        (holds_empty, empty),
        (holds_string, string),
        (only_none, only_none),
        (holds_of_empty, of_empty),
        (go_func, go_func),
        (takes_string, takes_string),
        (gives_array, gives_array),
    ];
    let mut rules = rules();
    for (ty, part) in refused {
        let shown = types.display(ty);
        assert_eq!(
            rules.param(&types, ty),
            Err(CRefusal::NotC(part)),
            "{shown}"
        );
    }
    let array = types.intern(Type::Array(int, 4));
    assert_eq!(rules.param(&types, array), Err(CRefusal::BareArray));
    assert_eq!(rules.result(&types, array), Err(CRefusal::BareArray));
    let pair = strukt(&mut types, "pair", &[int, int]);
    let mixed = FuncType {
        params: [int, pair, string].into(),
        ret: slice,
        has_env: false,
        cc: CallConv::C,
    };
    let why = (Some(2), CRefusal::NotC(string));
    assert_eq!(rules.signature(&types, &mixed), Err(why));
    let gives = FuncType {
        params: [int, pair].into(),
        ..mixed
    };
    let why = (None, CRefusal::NotC(slice));
    assert_eq!(rules.signature(&types, &gives), Err(why));
}

#[test]
fn each_limit_holds_on_both_sides() {
    let mut types = TypeInterner::new();
    let deep = nested(&mut types, "deep", MAX_DEPTH + 1);
    let mut rules = rules();
    assert_eq!(rules.param(&types, deep[MAX_DEPTH as usize]), Ok(()));
    let too_deep = deep[MAX_DEPTH as usize + 1];
    assert_eq!(rules.param(&types, too_deep), Err(CRefusal::Depth));

    // A struct counts itself: 4095 fields are 4096 with it.
    let int = types.int(32, true);
    let wide = strukt(&mut types, "wide", &vec![int; MAX_FIELDS as usize - 1]);
    let wider = strukt(&mut types, "wider", &vec![int; MAX_FIELDS as usize]);
    assert_eq!(rules.param(&types, wide), Ok(()));
    assert_eq!(rules.param(&types, wider), Err(CRefusal::Fields));
    // An array counts its element once, however long it is.
    let many = types.intern(Type::Array(int, 1 << 20));
    let long = strukt(&mut types, "long", &[many]);
    assert_eq!(rules.param(&types, long), Ok(()));

    let byte = types.int(8, false);
    let most = types.intern(Type::Array(byte, MAX_BYTES));
    let more = types.intern(Type::Array(byte, MAX_BYTES + 1));
    let large = strukt(&mut types, "large", &[most]);
    let larger = strukt(&mut types, "larger", &[more]);
    assert_eq!(rules.param(&types, large), Ok(()));
    assert_eq!(rules.param(&types, larger), Err(CRefusal::Bytes));
    // Without a target the bytes are not counted.
    assert_eq!(CTypeRules::new(None).param(&types, larger), Ok(()));
}

#[test]
fn what_is_wrong_is_said_in_one_order() {
    let mut types = TypeInterner::new();
    let int = types.int(32, true);
    let string = types.intern(Type::String);
    // Too deep, with a string at the bottom, and too wide.
    let mut inner = strukt(&mut types, "bottom", &[string]);
    for level in 0..MAX_DEPTH {
        inner = strukt(&mut types, &format!("level{level}"), &[inner]);
    }
    let mut rules = rules();
    assert_eq!(rules.param(&types, inner), Err(CRefusal::Depth));
    let byte = types.int(8, false);
    let large = types.intern(Type::Array(byte, MAX_BYTES + 1));
    let mut fields = vec![int; MAX_FIELDS as usize];
    fields.push(large);
    let wide_and_large = strukt(&mut types, "wide_and_large", &fields);
    assert_eq!(rules.param(&types, wide_and_large), Err(CRefusal::Fields));
    fields.push(string);
    let all = strukt(&mut types, "all", &fields);
    assert_eq!(rules.param(&types, all), Err(CRefusal::NotC(string)));
}

/// The types of a module with deep, wide and shared structs, and what
/// rules that were asked nothing before say of each.
fn corpus() -> (TypeInterner, Vec<TypeId>) {
    let mut types = TypeInterner::new();
    let int = types.int(32, true);
    let string = types.intern(Type::String);
    let mut all = nested(&mut types, "chain", MAX_DEPTH + 6);
    // Each level holds the level below twice.
    let mut level = int;
    for at in 0..70 {
        level = strukt(&mut types, &format!("diamond{at}"), &[level, level]);
        all.push(level);
    }
    let mut level = strukt(&mut types, "bad", &[int, string]);
    for at in 0..70 {
        let array = types.intern(Type::Array(level, 3));
        level = strukt(&mut types, &format!("over{at}"), &[array]);
        all.extend([array, level]);
    }
    let callback = c_func(&mut types, &[all[3], all[70]], int);
    all.push(callback);
    (types, all)
}

#[test]
fn an_answer_does_not_depend_on_what_was_asked_before() {
    let (types, all) = corpus();
    let alone: Vec<_> = (all.iter()).map(|&ty| rules().param(&types, ty)).collect();
    let orders: [Vec<usize>; 4] = [
        (0..all.len()).collect(),
        (0..all.len()).rev().collect(),
        (0..all.len())
            .step_by(2)
            .chain((1..all.len()).step_by(2))
            .collect(),
        (0..all.len()).map(|at| at * 37 % all.len()).collect(),
    ];
    for order in orders {
        let mut rules = rules();
        for at in order {
            let shown = types.display(all[at]);
            assert_eq!(rules.param(&types, all[at]), alone[at], "{shown}");
            assert_eq!(rules.param(&types, all[at]), alone[at], "{shown} again");
        }
    }
    assert!(alone.contains(&Ok(())));
    assert!(alone.contains(&Err(CRefusal::Depth)));
    assert!(alone.contains(&Err(CRefusal::Fields)));
}

/// The steps for a module of `count` levels, each of which holds the
/// level below twice and an array of it, asked in the order.
fn steps_for(count: usize, order: impl Fn(usize) -> Vec<usize>) -> u64 {
    let mut types = TypeInterner::new();
    let mut all = vec![types.int(32, true)];
    for at in 0..count {
        let below = all[all.len() - 1];
        let array = types.intern(Type::Array(below, 2));
        all.push(strukt(
            &mut types,
            &format!("level{at}"),
            &[below, array, below],
        ));
    }
    let mut rules = CTypeRules::new(None);
    for at in order(all.len()) {
        let _ = rules.param(&types, all[at]);
    }
    rules.steps
}

#[test]
fn the_steps_grow_with_the_types_in_every_order() {
    let forward = |count| (0..count).collect::<Vec<_>>();
    let backward = |count| (0..count).rev().collect::<Vec<_>>();
    let far_first = |count| [count - 1].into_iter().chain(0..count).collect::<Vec<_>>();
    let orders: [&dyn Fn(usize) -> Vec<usize>; 3] = [&forward, &backward, &far_first];
    for order in orders {
        let small = steps_for(2_000, order);
        let large = steps_for(8_000, order);
        assert!(large <= small * 4 + 64, "{small} steps, then {large}");
        assert!(small <= 2_000 * 16, "{small} steps for 2000 levels");
    }
}
