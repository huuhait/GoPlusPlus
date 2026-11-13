use std::collections::BTreeSet;

use gopp_testutil::linear::assert_near_linear;

use super::initialized::Graph;
use super::verify_counted;
use crate::*;

const HERE: Span = Span {
    file: 1,
    line: 3,
    col: 1,
};

fn block(at: u32) -> BlockId {
    BlockId::from_raw(at.into())
}

fn local(at: u32) -> LocalId {
    LocalId::from_raw(at.into())
}

fn func(module: &mut Module, at: u32) -> &mut Function {
    &mut module.funcs[FuncId::from_raw(at.into())]
}

/// The functions of `base`, by their place in it.
const EXT: u32 = 0;
const PAIR: u32 = 3;
const STOP: u32 = 2;
const F: u32 = 4;

fn sig(module: &mut Module, cc: CallConv, params: &[TypeId], ret: TypeId) -> TypeId {
    module.types.intern(Type::Func(FuncType {
        params: params.into(),
        ret,
        has_env: false,
        cc,
    }))
}

fn declared<'m>(
    module: &'m mut Module,
    name: &str,
    params: &[TypeId],
    ret: TypeId,
) -> FuncBuilder<'m> {
    let sig = sig(module, CallConv::Go, params, ret);
    FuncBuilder::new(module, name, sig, Linkage::External, Span::default()).unwrap()
}

/// A module that keeps every rule, with one of each thing that a rule is
/// about; `BASE` is its text.
fn base() -> Module {
    let mut module = Module {
        name: "p".to_owned(),
        files: vec!["p.gopp".to_owned()],
        ..Module::default()
    };
    let types = &mut module.types;
    let (int64, uint64) = (types.int(64, true), types.int(64, false));
    let ints = types.intern(Type::Slice(int64));
    let pair = types.tuple(&[int64, TypeInterner::BOOL]);
    let point = types.declare_struct("p.Point");
    let Type::Struct(point_id) = *types.get(point) else {
        panic!();
    };
    types
        .set_fields(point_id, &[("x", int64), ("y", int64)])
        .unwrap();
    let counter = module.globals.alloc(Global {
        name: "p.counter".to_owned(),
        ty: int64,
        link: Linkage::Internal,
        mutable: true,
        parts: Vec::new(),
    });
    let ext = declared(&mut module, "p.ext", &[int64], int64)
        .finish()
        .unwrap();
    let mut quiet = declared(&mut module, "p.quiet", &[int64], int64);
    quiet.set_no_unwind();
    quiet.finish().unwrap();
    let mut stop = declared(&mut module, "p.stop", &[], TypeInterner::UNIT);
    stop.set_no_return();
    let stop = stop.finish().unwrap();
    let make_pair = declared(&mut module, "p.pair", &[], pair).finish().unwrap();

    let sig = sig(&mut module, CallConv::Go, &[ints, int64], int64);
    let mut f = FuncBuilder::new(&mut module, "p.f", sig, Linkage::External, HERE).unwrap();
    let [
        entry,
        checked,
        paired,
        calls,
        shifts,
        sums,
        drops,
        caught,
        resumes,
        back,
        stops,
    ] = [(); 11].map(|()| f.new_block());
    f.set_cleanup(drops);
    f.set_catch(caught);
    f.set_cleanup(resumes);
    let (xs, n) = (Place::local(f.param(0)), f.param(1));
    let copy = |local: LocalId| Operand::Copy(Place::local(local));

    f.switch_to(entry);
    let len = f.assign_temp(Rvalue::Len(xs.clone())).unwrap();
    let index = f
        .assign_temp(Rvalue::Cast(CastKind::IntToInt, copy(n), uint64))
        .unwrap();
    let in_range = f
        .assign_temp(Rvalue::Binary(BinOp::Lt, copy(index), copy(len)))
        .unwrap();
    f.assert(
        copy(in_range),
        true,
        (AssertKind::Bounds, vec![copy(index), copy(len)]),
        checked,
        UnwindAction::Cleanup(drops),
    )
    .unwrap();

    f.switch_to(checked);
    f.push_scope(ScopeKind::Block, HERE);
    let p = f.var("p", point, true);
    f.storage_live(p).unwrap();
    let element = f.index(&xs, n).unwrap();
    let made = Rvalue::Aggregate(AggKind::Of(point), vec![copy(n), Operand::Copy(element)]);
    f.assign(Place::local(p), made).unwrap();
    let x = f.field(&Place::local(p), 0).unwrap();
    let address = f.assign_temp(Rvalue::AddrOf(x, Mutability::Mut)).unwrap();
    let through = f.deref(&Place::local(address)).unwrap();
    let negated = Rvalue::Unary(UnOp::Neg, Operand::Copy(through.clone()));
    f.assign(through, negated).unwrap();
    f.pop_scope();
    let both = f.temp(pair);
    let callee = f.const_func(make_pair);
    f.call(
        callee,
        Vec::new(),
        Place::local(both),
        Some(paired),
        UnwindAction::Cleanup(drops),
    )
    .unwrap();

    f.switch_to(paired);
    let flag = f.field(&Place::local(both), 1).unwrap();
    f.assign_temp(Rvalue::Unary(UnOp::Not, Operand::Copy(flag)))
        .unwrap();
    f.switch(copy(n), vec![(0, calls), (1, shifts)], stops)
        .unwrap();

    f.switch_to(calls);
    let value = f.temp(int64);
    let first = f.field(&Place::local(both), 0).unwrap();
    let callee = f.const_func(ext);
    f.call(
        callee,
        vec![Operand::Copy(first)],
        Place::local(value),
        Some(sums),
        UnwindAction::Cleanup(caught),
    )
    .unwrap();

    f.switch_to(shifts);
    let shifted = Rvalue::Binary(BinOp::Shl, copy(n), copy(index));
    f.assign(Place::local(value), shifted).unwrap();
    let global = Operand::Const(Constant {
        kind: ConstantKind::Global(counter),
        ty: f.types().ptr(int64),
    });
    let counter = f.assign_temp(Rvalue::Use(global)).unwrap();
    let count = f.deref(&Place::local(counter)).unwrap();
    f.assign(count, Rvalue::Use(copy(value))).unwrap();
    let hi = f.const_str(b"hi");
    f.assign_temp(Rvalue::Use(hi)).unwrap();
    let seven = f.const_int(int64, 7).unwrap();
    f.assign_temp(Rvalue::Aggregate(
        AggKind::Array(int64),
        vec![copy(n), seven],
    ))
    .unwrap();
    f.goto(sums).unwrap();

    f.switch_to(sums);
    let y = f.field(&Place::local(p), 1).unwrap();
    let sum = Rvalue::Binary(BinOp::Add, copy(value), Operand::Copy(y));
    f.assign(Place::local(f.result()), sum).unwrap();
    f.storage_dead(p).unwrap();
    f.ret().unwrap();

    f.switch_to(drops);
    f.drop(Place::local(p), resumes, UnwindAction::Terminate)
        .unwrap();

    f.switch_to(caught);
    let none = f.const_int(int64, u64::MAX).unwrap();
    f.assign(Place::local(f.result()), Rvalue::Use(none))
        .unwrap();
    f.switch(copy(in_range), vec![(0, resumes)], back).unwrap();

    f.switch_to(resumes);
    f.resume().unwrap();

    f.switch_to(back);
    f.ret().unwrap();

    f.switch_to(stops);
    let nothing = f.temp(TypeInterner::UNIT);
    let callee = f.const_func(stop);
    f.call(
        callee,
        Vec::new(),
        Place::local(nothing),
        None,
        UnwindAction::Continue,
    )
    .unwrap();
    f.finish().unwrap();
    module
}

const BASE: &str = "module \"p\"
file 0 \"p.gopp\"

global var @p.counter: i64 [internal]

declare fn p.ext(_1: i64) -> i64 [go, external]

declare fn p.quiet(_1: i64) -> i64 [go, external, nounwind]

declare fn p.stop() -> () [go, external, noreturn]

declare fn p.pair() -> (i64, bool) [go, external]

fn p.f(_1: []i64, _2: i64) -> i64 [go, external] {
  scope 1: block in 0  ; p.gopp:3:1
  let _0: i64
  let _3: u64
  let _4: u64
  let _5: bool
  let _6: p.Point \"p\"
  let _7: *i64
  let _8: (i64, bool)
  let _9: bool
  let _10: i64
  let _11: *i64
  let _12: string
  let _13: [2]i64
  let _14: ()
  bb0:
    _3 = len(_1)  ; p.gopp:3:1
    _4 = int_to_int(copy _2) as u64  ; p.gopp:3:1
    _5 = lt(copy _4, copy _3)  ; p.gopp:3:1
    assert(copy _5 == true, Bounds(copy _4, copy _3)) -> [success: bb1, unwind: cleanup bb6]  ; p.gopp:3:1
  bb1:
    storage_live(_6)  ; p.gopp:3:1 scope 1
    _6 = p.Point {copy _2, copy _1[_2]}  ; p.gopp:3:1 scope 1
    _7 = &mut _6.0  ; p.gopp:3:1 scope 1
    (*_7) = neg(copy (*_7))  ; p.gopp:3:1 scope 1
    _8 = call const fn @p.pair() -> [return: bb2, unwind: cleanup bb6]  ; p.gopp:3:1
  bb2:
    _9 = not(copy _8.1)  ; p.gopp:3:1
    switch copy _2 [0: bb3, 1: bb4, otherwise: bb10]  ; p.gopp:3:1
  bb3:
    _10 = call const fn @p.ext(copy _8.0) -> [return: bb5, unwind: cleanup bb7]  ; p.gopp:3:1
  bb4:
    _10 = shl(copy _2, copy _4)  ; p.gopp:3:1
    _11 = const global @p.counter  ; p.gopp:3:1
    (*_11) = copy _10  ; p.gopp:3:1
    _12 = const str#0 \"hi\"  ; p.gopp:3:1
    _13 = array i64 {copy _2, const 7: i64}  ; p.gopp:3:1
    goto bb5  ; p.gopp:3:1
  bb5:
    _0 = add(copy _10, copy _6.1)  ; p.gopp:3:1
    storage_dead(_6)  ; p.gopp:3:1
    return  ; p.gopp:3:1
  bb6: [cleanup]
    drop(_6) -> [return: bb8, unwind: terminate]  ; p.gopp:3:1
  bb7: [catch]
    _0 = const -1: i64  ; p.gopp:3:1
    switch copy _5 [0: bb8, otherwise: bb9]  ; p.gopp:3:1
  bb8: [cleanup]
    resume  ; p.gopp:3:1
  bb9:
    return  ; p.gopp:3:1
  bb10:
    _14 = call const fn @p.stop() -> [unwind: continue]  ; p.gopp:3:1
}
";

#[test]
fn the_module_of_the_table_keeps_every_rule() {
    let module = base();
    assert_eq!(print_module(&module), BASE);
    assert_eq!(verify_module(&module), Ok(()));
}

fn f(module: &mut Module) -> &mut Function {
    func(module, F)
}

fn int64(module: &mut Module) -> TypeId {
    module.types.int(64, true)
}

fn stmt(module: &mut Module, bb: u32, at: usize) -> &mut Statement {
    &mut f(module).blocks[block(bb)].stmts[at]
}

fn assigned(module: &mut Module, bb: u32, at: usize) -> &mut (Place, Rvalue) {
    match &mut stmt(module, bb, at).kind {
        StatementKind::Assign(assigned) => assigned,
        other => panic!("{other:?}"),
    }
}

/// The parts of the aggregate that the statement makes.
fn parts(module: &mut Module, bb: u32, at: usize) -> &mut Vec<Operand> {
    match &mut assigned(module, bb, at).1 {
        Rvalue::Aggregate(_, parts) => parts,
        other => panic!("{other:?}"),
    }
}

/// One more assignment at the end of the block.
fn then(module: &mut Module, bb: u32, dest: Place, rvalue: Rvalue) {
    let mut last = stmt(module, 0, 0).clone();
    last.kind = StatementKind::Assign(Box::new((dest, rvalue)));
    f(module).blocks[block(bb)].stmts.push(last);
}

fn end(module: &mut Module, bb: u32) -> &mut TerminatorKind {
    &mut f(module).blocks[block(bb)].term.kind
}

fn unwind(module: &mut Module, bb: u32) -> &mut UnwindAction {
    match end(module, bb) {
        TerminatorKind::Call { unwind, .. }
        | TerminatorKind::Drop { unwind, .. }
        | TerminatorKind::Assert { unwind, .. } => unwind,
        other => panic!("{other:?}"),
    }
}

/// What a switch or an assert looks at.
fn on(module: &mut Module, bb: u32) -> &mut Operand {
    match end(module, bb) {
        TerminatorKind::Switch { on, .. } | TerminatorKind::Assert { cond: on, .. } => on,
        other => panic!("{other:?}"),
    }
}

fn cases(module: &mut Module, bb: u32) -> &mut Vec<(u64, BlockId)> {
    match end(module, bb) {
        TerminatorKind::Switch { cases, .. } => cases,
        other => panic!("{other:?}"),
    }
}

fn callee(module: &mut Module, bb: u32) -> &mut Operand {
    match end(module, bb) {
        TerminatorKind::Call { callee, .. } => callee,
        other => panic!("{other:?}"),
    }
}

fn args(module: &mut Module, bb: u32) -> &mut Vec<Operand> {
    match end(module, bb) {
        TerminatorKind::Call { args, .. } | TerminatorKind::Assert { args, .. } => args,
        other => panic!("{other:?}"),
    }
}

fn copy(at: u32) -> Operand {
    Operand::Copy(Place::local(local(at)))
}

/// One step into a local.
fn step(at: u32, kind: ProjectionKind, ty: TypeId) -> Place {
    Place {
        local: local(at),
        projections: [Projection { kind, ty }].into_iter().collect(),
    }
}

fn constant(kind: ConstantKind, ty: TypeId) -> Operand {
    Operand::Const(Constant { kind, ty })
}

/// A variable more, by its number.
fn variable(module: &mut Module, ty: TypeId) -> u32 {
    let mut new = f(module).locals[local(6)].clone();
    new.ty = ty;
    f(module).locals.alloc(new).into_raw().into_u32()
}

/// `bb5` adds what is at the place.
fn adds(module: &mut Module, place: Place) {
    assigned(module, 5, 0).1 = Rvalue::Binary(BinOp::Add, copy(10), Operand::Copy(place));
}

/// A struct with a part that `part` makes of the struct.
fn holding(module: &mut Module, part: fn(&mut TypeInterner, TypeId) -> TypeId) {
    let types = &mut module.types;
    let holder = types.declare_struct("p.Holder");
    let Type::Struct(id) = *types.get(holder) else {
        panic!();
    };
    let part = part(types, holder);
    types.set_fields(id, &[("part", part)]).unwrap();
}

fn global(module: &mut Module, name: &str, ty: TypeId) {
    module.globals.alloc(Global {
        name: name.to_owned(),
        ty,
        link: Linkage::Internal,
        mutable: false,
        parts: Vec::new(),
    });
}

/// A global of a number and a boolean with static data: `parts` makes
/// it from the two types.
fn data(module: &mut Module, parts: fn(TypeId, TypeId) -> Vec<Constant>) {
    let int = int64(module);
    let ty = module.types.tuple(&[int, TypeInterner::BOOL]);
    module.globals.alloc(Global {
        name: "p.data".to_owned(),
        ty,
        link: Linkage::Internal,
        mutable: false,
        parts: parts(int, TypeInterner::BOOL),
    });
}

/// `bb3` calls a function of C that a variable holds.
fn calls_c(module: &mut Module, param: TypeId, arg: Operand) {
    let ret = int64(module);
    let ty = sig(module, CallConv::C, &[param], ret);
    let held = variable(module, ty);
    *callee(module, 3) = copy(held);
    *args(module, 3) = vec![arg];
}

fn with_env(module: &mut Module, cc: CallConv, params: &[TypeId]) {
    module.types.intern(Type::Func(FuncType {
        params: params.into(),
        ret: TypeInterner::UNIT,
        has_env: true,
        cc,
    }));
}

fn bytes(module: &mut Module) -> TypeId {
    let byte = module.types.int(8, false);
    module.types.ptr(byte)
}

/// `bb2` makes a pair in `dest`, a place of the variable `at`.
fn makes_pair(module: &mut Module, dest: fn(u32, TypeId) -> Place) {
    let pair = f(module).locals[local(8)].ty;
    let to = module.types.ptr(pair);
    let at = variable(module, to);
    let parts = vec![
        copy(2),
        constant(ConstantKind::Bool(true), TypeInterner::BOOL),
    ];
    *assigned(module, 2, 0) = (dest(at, pair), Rvalue::Aggregate(AggKind::Tuple, parts));
}

fn keep(_: &mut Module) {}

type Change = fn(&mut Module);

/// For each rule a change of `base` that keeps it, one that breaks it,
/// and what the verifier says of the second.
const CASES: &[(u16, Change, Change, &str)] = &[
    (
        1,
        |m| {
            let mut twin = func(m, EXT).clone();
            twin.name = "p.other".to_owned();
            m.funcs.alloc(twin);
        },
        |m| {
            let twin = func(m, EXT).clone();
            m.funcs.alloc(twin);
        },
        "V001: fn p.ext: the symbol p.ext names two functions",
    ),
    (
        2,
        |m| _ = m.types.int(16, false),
        |m| _ = m.types.int(24, false),
        "V002: an integer of 24 bits",
    ),
    (
        2,
        |m| _ = m.types.float(32),
        |m| _ = m.types.float(16),
        "V002: a float of 16 bits",
    ),
    (
        2,
        |m| holding(m, |types, _| types.int(8, true)),
        |m| _ = m.types.declare_struct("p.Empty"),
        "V002: the struct p.Empty has no fields",
    ),
    (
        2,
        keep,
        |m| {
            let ty = m.types.int(128, false);
            let wide = variable(m, ty);
            let one = constant(ConstantKind::Int(1), ty);
            then(m, 9, Place::local(local(wide)), Rvalue::Use(one));
        },
        "V002: an integer of 128 bits",
    ),
    (
        3,
        |m| holding(m, |types, me| types.ptr(me)),
        |m| holding(m, |_, me| me),
        "V003: p.Holder holds itself by value",
    ),
    (
        3,
        |m| holding(m, |types, me| types.intern(Type::Slice(me))),
        |m| holding(m, |types, me| types.intern(Type::Array(me, 2))),
        "V003: p.Holder holds itself by value",
    ),
    (
        3,
        keep,
        |m| holding(m, |types, me| types.intern(Type::Option(me))),
        "V003: p.Holder holds itself by value",
    ),
    (
        3,
        keep,
        |m| {
            holding(m, |types, me| {
                let int64 = types.int(64, true);
                types.tuple(&[int64, me])
            });
        },
        "V003: p.Holder holds itself by value",
    ),
    (
        10,
        keep,
        |m| f(m).sig = int64(m),
        "V010: fn p.f: the signature i64 is no function type",
    ),
    (
        11,
        |m| f(m).locals[local(9)].kind = LocalKind::Var,
        |m| f(m).locals[local(1)].kind = LocalKind::Var,
        "V011: fn p.f: local _1 is not what the signature says",
    ),
    (
        11,
        keep,
        |m| f(m).locals[local(9)].kind = LocalKind::Arg,
        "V011: fn p.f: local _9 is not what the signature says",
    ),
    (
        12,
        keep,
        |m| {
            let result = f(m).locals[local(0)].clone();
            func(m, EXT).locals.alloc(result);
        },
        "V012: fn p.ext: a declaration has locals",
    ),
    (
        13,
        |m| *end(m, 4) = TerminatorKind::Goto(block(2)),
        |m| *end(m, 4) = TerminatorKind::Goto(block(0)),
        "V013: fn p.f: bb4 #5: an edge leads back to the first block",
    ),
    (
        14,
        |m| f(m).locals[local(6)].owned = true,
        |m| f(m).locals[local(0)].owned = true,
        "V014: fn p.f: the place of the result is owned",
    ),
    (
        21,
        keep,
        |m| *end(m, 4) = TerminatorKind::Goto(block(99)),
        "V021: fn p.f: bb4 #5: no block bb99",
    ),
    (
        21,
        keep,
        |m| *unwind(m, 3) = UnwindAction::Cleanup(block(99)),
        "V021: fn p.f: bb3 #0: no block bb99",
    ),
    (
        22,
        keep,
        |m| {
            let returns = f(m).blocks[block(9)].clone();
            f(m).blocks.alloc(returns);
        },
        "V022: fn p.f: bb11 #0: the block is not reached",
    ),
    (
        23,
        keep,
        |m| *end(m, 4) = TerminatorKind::Goto(block(6)),
        "V023: fn p.f: bb4 #5: the edge to bb6 crosses the cleanup",
    ),
    (
        23,
        keep,
        |m| {
            let TerminatorKind::Drop { target, .. } = end(m, 6) else {
                panic!();
            };
            *target = block(7);
        },
        "V023: fn p.f: bb6 #0: bb7 is entered by unwinding only",
    ),
    (
        24,
        |m| *unwind(m, 1) = UnwindAction::Cleanup(block(7)),
        |m| *unwind(m, 1) = UnwindAction::Cleanup(block(9)),
        "V024: fn p.f: bb1 #4: unwinding enters bb9, no cleanup block",
    ),
    (
        25,
        |m| *unwind(m, 6) = UnwindAction::Unreachable,
        |m| *unwind(m, 6) = UnwindAction::Continue,
        "V025: fn p.f: bb6 #0: a cleanup block lets a panic go on",
    ),
    (
        25,
        keep,
        |m| *end(m, 8) = TerminatorKind::Return,
        "V025: fn p.f: bb8 #0: a cleanup block returns",
    ),
    (
        25,
        keep,
        |m| *end(m, 9) = TerminatorKind::Resume,
        "V025: fn p.f: bb9 #0: resume outside a cleanup block",
    ),
    (
        26,
        |m| {
            func(m, PAIR).no_unwind = true;
            *unwind(m, 1) = UnwindAction::Unreachable;
        },
        |m| *unwind(m, 1) = UnwindAction::Unreachable,
        "V026: fn p.f: bb1 #4: the call may unwind",
    ),
    (
        26,
        keep,
        |m| func(m, PAIR).no_unwind = true,
        "V026: fn p.f: bb1 #4: a cleanup for a call that does not unwind",
    ),
    (
        26,
        keep,
        |m| f(m).no_unwind = true,
        "V026: fn p.f: bb8 #0: a function that does not unwind resumes\nV026: fn p.f: bb10 #0: a function that does not unwind lets a panic go on",
    ),
    (
        27,
        |m| cases(m, 2).push((5, block(4))),
        |m| cases(m, 2).push((0, block(4))),
        "V027: fn p.f: bb2 #1: duplicate switch case 0",
    ),
    (
        27,
        |m| cases(m, 7).push((1, block(9))),
        |m| cases(m, 7).push((2, block(9))),
        "V027: fn p.f: bb7 #1: switch case 2 is too wide",
    ),
    (
        28,
        keep,
        |m| func(m, STOP).no_return = false,
        "V028: fn p.f: bb10 #0: the call returns nowhere",
    ),
    (
        30,
        keep,
        |m| assigned(m, 0, 1).1 = Rvalue::Use(copy(99)),
        "V030: fn p.f: bb0 #1: no local _99",
    ),
    (
        32,
        |m| {
            let ty = int64(m);
            adds(m, step(6, ProjectionKind::Field(0), ty));
        },
        |m| {
            let ty = int64(m);
            adds(m, step(6, ProjectionKind::Field(5), ty));
        },
        "V032: fn p.f: bb5 #0: no such field of p.Point",
    ),
    (
        32,
        |m| {
            let ty = int64(m);
            let second = step(13, ProjectionKind::ConstIndex(1), ty);
            then(
                m,
                4,
                Place::local(local(10)),
                Rvalue::Use(Operand::Copy(second)),
            );
        },
        |m| {
            let ty = int64(m);
            let third = step(13, ProjectionKind::ConstIndex(2), ty);
            then(
                m,
                4,
                Place::local(local(10)),
                Rvalue::Use(Operand::Copy(third)),
            );
        },
        "V032: fn p.f: bb4 #5: constant index of [2]i64",
    ),
    (
        32,
        keep,
        |m| {
            let ty = int64(m);
            adds(m, step(6, ProjectionKind::Index(local(2)), ty));
        },
        "V032: fn p.f: bb5 #0: index of p.Point",
    ),
    (
        33,
        keep,
        |m| adds(m, step(6, ProjectionKind::Field(1), TypeInterner::BOOL)),
        "V033: fn p.f: bb5 #0: a step of p.Point says bool",
    ),
    (
        34,
        |m| {
            let ty = int64(m);
            parts(m, 1, 1)[1] = Operand::Copy(step(1, ProjectionKind::Index(local(4)), ty));
        },
        |m| {
            let ty = int64(m);
            parts(m, 1, 1)[1] = Operand::Copy(step(1, ProjectionKind::Index(local(5)), ty));
        },
        "V034: fn p.f: bb1 #1: index by no integer into []i64",
    ),
    (
        35,
        keep,
        |m| {
            let ty = int64(m);
            assigned(m, 1, 3).0 = step(2, ProjectionKind::Deref, ty);
        },
        "V035: fn p.f: bb1 #3: deref of i64",
    ),
    (
        36,
        keep,
        |m| stmt(m, 1, 0).kind = StatementKind::StorageLive(local(99)),
        "V036: fn p.f: bb1 #0: no local _99",
    ),
    (
        40,
        |m| {
            let ty = int64(m);
            assigned(m, 7, 0).1 = Rvalue::Use(constant(ConstantKind::Zero, ty));
        },
        |m| {
            let ty = int64(m);
            assigned(m, 7, 0).1 = Rvalue::Use(constant(ConstantKind::Bool(true), ty));
        },
        "V040: fn p.f: bb7 #0: the constant Bool(true) is no i64",
    ),
    (
        40,
        |m| {
            let ty = m.types.int(8, false);
            let small = variable(m, ty);
            let most = constant(ConstantKind::Int(255), ty);
            then(m, 9, Place::local(local(small)), Rvalue::Use(most));
        },
        |m| {
            let ty = m.types.int(8, false);
            let small = variable(m, ty);
            let more = constant(ConstantKind::Int(256), ty);
            then(m, 9, Place::local(local(small)), Rvalue::Use(more));
        },
        "V040: fn p.f: bb9 #0: the constant Int(256) is no u8",
    ),
    (
        40,
        keep,
        |m| {
            let ty = int64(m);
            assigned(m, 7, 0).1 = Rvalue::Use(constant(ConstantKind::Null, ty));
        },
        "V040: fn p.f: bb7 #0: the constant Null is no i64",
    ),
    (
        40,
        keep,
        |m| {
            let ty = f(m).locals[local(12)].ty;
            assigned(m, 4, 3).1 = Rvalue::Use(constant(ConstantKind::Str(9), ty));
        },
        "V040: fn p.f: bb4 #3: the constant Str(9) is no string",
    ),
    (
        40,
        keep,
        |m| {
            let ty = f(m).locals[local(11)].ty;
            let no_global = ConstantKind::Global(GlobalId::from_raw(9.into()));
            assigned(m, 4, 1).1 = Rvalue::Use(constant(no_global, ty));
        },
        "V040: fn p.f: bb4 #1: the constant Global(Idx::<Global>(9)) is no *i64",
    ),
    (
        40,
        |m| {
            let ty = func(m, EXT).sig;
            let held = variable(m, ty);
            let ext = constant(ConstantKind::Func(FuncId::from_raw(EXT.into())), ty);
            then(m, 9, Place::local(local(held)), Rvalue::Use(ext));
        },
        |m| {
            let ty = func(m, PAIR).sig;
            let held = variable(m, ty);
            let ext = constant(ConstantKind::Func(FuncId::from_raw(EXT.into())), ty);
            then(m, 9, Place::local(local(held)), Rvalue::Use(ext));
        },
        "V040: fn p.f: bb9 #0: the constant Func(Idx::<Function>(0)) is no fn() -> (i64, bool)",
    ),
    (
        50,
        keep,
        |m| assigned(m, 7, 0).1 = Rvalue::Use(copy(5)),
        "V050: fn p.f: bb7 #0: a bool is assigned to i64",
    ),
    (
        51,
        |m| {
            let ty = int64(m);
            let through = Operand::Copy(step(7, ProjectionKind::Deref, ty));
            assigned(m, 1, 3).1 = Rvalue::Unary(UnOp::BitNot, through);
        },
        |m| {
            let ty = int64(m);
            let through = Operand::Copy(step(7, ProjectionKind::Deref, ty));
            assigned(m, 1, 3).1 = Rvalue::Unary(UnOp::Not, through);
        },
        "V051: fn p.f: bb1 #3: Not of i64 as i64",
    ),
    (
        52,
        |m| assigned(m, 4, 0).1 = Rvalue::Binary(BinOp::Shr, copy(2), copy(4)),
        |m| assigned(m, 4, 0).1 = Rvalue::Binary(BinOp::Add, copy(2), copy(4)),
        "V052: fn p.f: bb4 #0: Add of i64 and u64 as i64",
    ),
    (
        52,
        |m| assigned(m, 0, 2).1 = Rvalue::Binary(BinOp::Eq, copy(4), copy(3)),
        |m| assigned(m, 0, 2).1 = Rvalue::Binary(BinOp::Sub, copy(4), copy(3)),
        "V052: fn p.f: bb0 #2: Sub of u64 and u64 as bool",
    ),
    (
        52,
        keep,
        |m| assigned(m, 2, 0).1 = Rvalue::Binary(BinOp::Lt, copy(5), copy(5)),
        "V052: fn p.f: bb2 #0: Lt of bool and bool as bool",
    ),
    (
        53,
        |m| {
            let ty = m.types.int(64, false);
            assigned(m, 0, 1).1 = Rvalue::Cast(CastKind::IntToInt, copy(3), ty);
        },
        |m| {
            let ty = m.types.int(64, false);
            assigned(m, 0, 1).1 = Rvalue::Cast(CastKind::IntToFloat, copy(2), ty);
        },
        "V053: fn p.f: bb0 #1: IntToFloat of i64 to u64 as u64",
    ),
    (
        54,
        |m| {
            let ty = int64(m);
            let y = step(6, ProjectionKind::Field(1), ty);
            assigned(m, 1, 2).1 = Rvalue::AddrOf(y, Mutability::Not);
        },
        |m| assigned(m, 1, 2).1 = Rvalue::AddrOf(Place::local(local(6)), Mutability::Mut),
        "V054: fn p.f: bb1 #2: the address of p.Point is no *i64",
    ),
    (
        55,
        |m| parts(m, 1, 1)[1] = copy(2),
        |m| drop(parts(m, 1, 1).pop()),
        "V055: fn p.f: bb1 #1: the parts are not those of p.Point",
    ),
    (
        55,
        keep,
        |m| parts(m, 4, 4)[1] = copy(5),
        "V055: fn p.f: bb4 #4: the parts are not those of [2]i64",
    ),
    (
        56,
        keep,
        |m| assigned(m, 0, 0).1 = Rvalue::Len(Place::local(local(2))),
        "V056: fn p.f: bb0 #0: the length of i64 as u64",
    ),
    (
        57,
        keep,
        |m| {
            let ty = int64(m);
            parts(m, 1, 1)[0] = Operand::Copy(step(6, ProjectionKind::Field(0), ty));
        },
        "V057: fn p.f: bb1 #1: an aggregate reads the local that it is put in",
    ),
    (
        57,
        |m| makes_pair(m, |_, _| Place::local(local(8))),
        |m| makes_pair(m, |at, pair| step(at, ProjectionKind::Deref, pair)),
        "V057: fn p.f: bb2 #0: an aggregate is put behind a pointer",
    ),
    (
        60,
        keep,
        |m| *on(m, 2) = copy(1),
        "V060: fn p.f: bb2 #1: a switch on []i64",
    ),
    (
        60,
        keep,
        |m| *on(m, 0) = copy(4),
        "V060: fn p.f: bb0 #3: an assert on u64",
    ),
    (
        61,
        keep,
        |m| *callee(m, 3) = copy(2),
        "V061: fn p.f: bb3 #0: a call of i64",
    ),
    (
        62,
        |m| *args(m, 3) = vec![copy(2)],
        |m| args(m, 3).clear(),
        "V062: fn p.f: bb3 #0: the arguments are not those of the function",
    ),
    (
        62,
        keep,
        |m| *args(m, 3) = vec![copy(4)],
        "V062: fn p.f: bb3 #0: the arguments are not those of the function",
    ),
    (
        63,
        keep,
        |m| {
            let TerminatorKind::Call { dest, .. } = end(m, 10) else {
                panic!();
            };
            *dest = Place::local(local(4));
        },
        "V063: fn p.f: bb10 #0: the result is put in u64",
    ),
    (
        64,
        keep,
        |m| {
            let nothing = variable(m, TypeInterner::UNIT);
            let TerminatorKind::Drop { place, .. } = end(m, 6) else {
                panic!();
            };
            *place = Place::local(local(nothing));
        },
        "V064: fn p.f: bb6 #0: a drop of what has no value",
    ),
    (
        65,
        |m| {
            let TerminatorKind::Assert { kind, .. } = end(m, 0) else {
                panic!();
            };
            *kind = AssertKind::Slice3C;
        },
        |m| {
            let TerminatorKind::Assert { kind, .. } = end(m, 0) else {
                panic!();
            };
            *kind = AssertKind::Nil;
        },
        "V065: fn p.f: bb0 #3: Nil names 0 integers",
    ),
    (
        65,
        keep,
        |m| args(m, 0).clear(),
        "V065: fn p.f: bb0 #3: Bounds names 2 integers",
    ),
    (
        65,
        keep,
        |m| args(m, 0)[0] = copy(5),
        "V065: fn p.f: bb0 #3: Bounds names 2 integers",
    ),
    (
        66,
        keep,
        |m| f(m).locals[local(8)].kind = LocalKind::Var,
        "V066: fn p.f: bb1 #4: the result needs a temporary of its own",
    ),
    (
        66,
        keep,
        |m| {
            let pair = f(m).locals[local(8)].ty;
            let to = m.types.ptr(pair);
            let at = variable(m, to);
            let address = Rvalue::AddrOf(Place::local(local(8)), Mutability::Not);
            *assigned(m, 2, 0) = (Place::local(local(at)), address);
        },
        "V066: fn p.f: bb1 #4: the result needs a temporary of its own",
    ),
    (
        70,
        |m| stmt(m, 5, 0).loc.file = 0,
        |m| stmt(m, 5, 0).loc.file = 2,
        "V070: fn p.f: bb5 #0: no file 1",
    ),
    (70, keep, |m| f(m).loc.file = 7, "V070: fn p.f: no file 6"),
    (
        71,
        |m| {
            let ty = int64(m);
            global(m, "p.other", ty);
        },
        |m| {
            let ty = int64(m);
            global(m, "p.counter", ty);
        },
        "V071: the symbol p.counter names two globals",
    ),
    (
        71,
        keep,
        |m| global(m, "p.nothing", TypeInterner::UNIT),
        "V071: the global p.nothing has no value",
    ),
    (
        72,
        |m| {
            data(m, |int, bool| {
                let number = ConstantKind::Int(7);
                let (kind, zero) = (number, ConstantKind::Zero);
                vec![
                    Constant { kind, ty: int },
                    Constant {
                        kind: zero,
                        ty: bool,
                    },
                ]
            });
        },
        |m| {
            data(m, |int, _| {
                let kind = ConstantKind::Int(7);
                vec![Constant { kind, ty: int }]
            });
        },
        "V072: the global p.data has 2 leaves and 1 parts",
    ),
    (
        72,
        keep,
        |m| {
            data(m, |int, _| {
                let kind = ConstantKind::Int(7);
                vec![Constant { kind, ty: int }, Constant { kind, ty: int }]
            });
        },
        "V072: part 1 of the global p.data is no bool",
    ),
    (
        73,
        |m| {
            let kind = ScopeKind::Inlined(FuncId::from_raw(F.into()));
            let (parent, loc) = (ScopeId(1), HERE);
            f(m).scopes.push(DebugScope { kind, parent, loc });
        },
        |m| {
            let kind = ScopeKind::Inlined(FuncId::from_raw(EXT.into()));
            let (parent, loc) = (ScopeId(1), HERE);
            f(m).scopes.push(DebugScope { kind, parent, loc });
        },
        "V073: fn p.f: scope 2 inlines no defined function",
    ),
    (
        73,
        keep,
        |m| f(m).scopes[0].parent = ScopeId(1),
        "V073: fn p.f: scope 1 comes before its parent",
    ),
    (
        74,
        |m| stmt(m, 5, 0).scope = ScopeId(1),
        |m| stmt(m, 5, 0).scope = ScopeId(2),
        "V074: fn p.f: bb5 #0: no scope 2",
    ),
    (
        74,
        keep,
        |m| f(m).locals[local(6)].scope = ScopeId(2),
        "V074: fn p.f: local _6 is in no scope",
    ),
    (
        75,
        keep,
        |m| f(m).cc = CallConv::C,
        "V075: fn p.f: the convention is not that of the signature",
    ),
    (
        76,
        |m| {
            let ty = int64(m);
            calls_c(m, ty, copy(2));
        },
        |m| {
            let ty = f(m).locals[local(12)].ty;
            calls_c(m, ty, constant(ConstantKind::Str(0), ty));
        },
        "V076: fn p.f: bb3 #0: a C function pointer takes scalars only",
    ),
    (
        78,
        |m| {
            let params = [bytes(m), int64(m)];
            with_env(m, CallConv::Go, &params);
        },
        |m| {
            let params = [int64(m)];
            with_env(m, CallConv::Go, &params);
        },
        "V078: fn env(i64) -> (): an environment needs the Go convention and a first *u8",
    ),
    (
        78,
        keep,
        |m| {
            let params = [bytes(m)];
            with_env(m, CallConv::C, &params);
        },
        "V078: fn \"C\"(*u8) -> (): an environment needs the Go convention and a first *u8",
    ),
    (
        79,
        |m| f(m).inline = Inline::Always,
        |m| func(m, EXT).inline = Inline::Never,
        "V079: fn p.ext: a declaration has an inline hint",
    ),
    (
        85,
        keep,
        |m| stmt(m, 4, 0).kind = StatementKind::Nop,
        "V085: fn p.f: bb4 #2: the temporary _10 may have no value",
    ),
    (
        85,
        keep,
        |m| f(m).blocks[block(0)].stmts.swap(0, 2),
        "V085: fn p.f: bb0 #0: the temporary _4 may have no value\nV085: fn p.f: bb0 #0: the temporary _3 may have no value",
    ),
    (
        85,
        keep,
        |m| assigned(m, 7, 0).1 = Rvalue::Use(copy(10)),
        "V085: fn p.f: bb7 #0: the temporary _10 may have no value",
    ),
    (
        85,
        |m| {
            let ty = int64(m);
            let first = step(8, ProjectionKind::Field(0), ty);
            assigned(m, 7, 0).1 = Rvalue::Use(Operand::Copy(first));
        },
        |m| {
            let ty = int64(m);
            let first = step(8, ProjectionKind::Field(0), ty);
            assigned(m, 7, 0).1 = Rvalue::Use(Operand::Copy(first));
            *unwind(m, 1) = UnwindAction::Cleanup(block(7));
        },
        "V085: fn p.f: bb7 #0: the temporary _8 may have no value",
    ),
    (
        85,
        keep,
        |m| {
            let ty = int64(m);
            parts(m, 1, 1)[1] = Operand::Copy(step(1, ProjectionKind::Index(local(10)), ty));
        },
        "V085: fn p.f: bb1 #1: the temporary _10 may have no value",
    ),
];

/// What the verifier says of the module, one line for each error.
fn said(module: &Module) -> String {
    let errors = verify_module(module).err().unwrap_or_default();
    let lines: Vec<String> = errors.iter().map(ToString::to_string).collect();
    lines.join("\n")
}

#[test]
fn each_rule_passes_what_keeps_it_and_names_what_breaks_it() {
    let mut wrong = Vec::new();
    for (at, &(rule, good, bad, says)) in CASES.iter().enumerate() {
        let mut module = base();
        good(&mut module);
        if verify_module(&module) != Ok(()) {
            wrong.push(format!("{at}: V{rule:03} good: {}", said(&module)));
        }
        let mut module = base();
        bad(&mut module);
        let found = said(&module);
        let only = !found.is_empty()
            && found
                .lines()
                .all(|line| line.starts_with(&format!("V{rule:03}: ")));
        if found != says || !only {
            wrong.push(format!("{at}: V{rule:03} bad: {found:?}"));
        }
    }
    assert!(wrong.is_empty(), "\n{}", wrong.join("\n"));
}

/// The rules that the data model holds by itself, and those about parts
/// that it does not have yet.
const HELD: [u16; 2] = [20, 31];
const NOT_CHECKED: [u16; 4] = [67, 72, 77, 81];

#[test]
fn every_rule_of_the_design_has_its_cases() {
    let design = include_str!("../../../../docs/design/gir.md");
    let listed: BTreeSet<u16> = design
        .lines()
        .filter_map(|line| line.strip_prefix("| `V")?.get(..3)?.parse().ok())
        .collect();
    let tested = CASES.iter().map(|&(rule, ..)| rule);
    let known: BTreeSet<u16> = tested.chain(HELD).chain(NOT_CHECKED).collect();
    assert_eq!(known, listed);
}

#[test]
fn the_dominators_are_those_that_taking_a_block_away_shows() {
    let mut seed = 7u64;
    let mut below = |limit: u32| {
        seed = seed
            .wrapping_mul(6_364_136_223_846_793_005)
            .wrapping_add(1_442_695_040_888_963_407);
        (seed >> 33) as u32 % limit
    };
    // The blocks that are reached when one is taken away.
    let reached = |succs: &[Vec<u32>], without: Option<u32>| {
        let mut seen = vec![false; succs.len()];
        let mut rest = vec![0];
        while let Some(at) = rest.pop() {
            if Some(at) == without || std::mem::replace(&mut seen[at as usize], true) {
                continue;
            }
            rest.extend(&succs[at as usize]);
        }
        seen
    };
    for _ in 0..400 {
        let count = 2 + below(14);
        let succs: Vec<Vec<u32>> = (0..count)
            .map(|_| (0..below(4)).map(|_| below(count)).collect())
            .collect();
        let graph = Graph::walk(&succs, &mut 0);
        let idom = graph.dominators(&mut 0);
        let all = reached(&succs, None);
        // The blocks that every way to each block goes through.
        let above: Vec<Vec<u32>> = (0..count)
            .map(|at| {
                let others = (0..count).filter(|&other| other != at && all[other as usize]);
                others
                    .filter(|&other| !reached(&succs, Some(other))[at as usize])
                    .collect()
            })
            .collect();
        for (at, &block) in graph.order.iter().enumerate().skip(1) {
            // The last of them is the one that all the others are above.
            let last = above[block as usize]
                .iter()
                .max_by_key(|&&other| above[other as usize].len())
                .unwrap();
            assert_eq!(
                graph.order[idom[at] as usize], *last,
                "{succs:?} at {block}"
            );
        }
        assert_eq!(graph.order.len(), all.iter().filter(|&&seen| seen).count());
    }
}

/// A module with a function `p.g(n i64) i64` that `body` fills, which
/// may call `p.ext`.
fn generated(body: impl FnOnce(&mut FuncBuilder, FuncId, TypeId)) -> Module {
    let mut module = Module::default();
    let int64 = module.types.int(64, true);
    let ext = declared(&mut module, "p.ext", &[int64], int64)
        .finish()
        .unwrap();
    let mut g = declared(&mut module, "p.g", &[int64], int64);
    let entry = g.new_block();
    g.switch_to(entry);
    body(&mut g, ext, int64);
    g.finish().unwrap();
    module
}

fn steps(module: &Module) -> u64 {
    let (errors, steps) = verify_counted(module);
    assert_eq!(errors, []);
    steps
}

fn use_of(local: LocalId) -> Rvalue {
    Rvalue::Use(Operand::Copy(Place::local(local)))
}

/// `n` calls one after another, which all unwind to one block.
fn calls_into_one_cleanup(n: usize) -> Module {
    generated(|g, ext, int64| {
        let cleanup = g.new_block();
        g.set_cleanup(cleanup);
        let mut last = g.param(0);
        for _ in 0..n {
            let (next, result) = (g.new_block(), g.temp(int64));
            let (callee, arg) = (g.const_func(ext), Operand::Copy(Place::local(last)));
            let unwind = UnwindAction::Cleanup(cleanup);
            g.call(callee, vec![arg], Place::local(result), Some(next), unwind)
                .unwrap();
            g.switch_to(next);
            last = result;
        }
        g.assign(Place::local(g.result()), use_of(last)).unwrap();
        g.ret().unwrap();
        g.switch_to(cleanup);
        g.resume().unwrap();
    })
}

/// Two ways that each assign `value` and meet in a new block, which
/// the builder is left in. `assigns` says whether they assign it.
fn two_ways(g: &mut FuncBuilder, value: LocalId, int64: TypeId, assigns: bool) {
    let [left, right, meet] = [(); 3].map(|()| g.new_block());
    let n = Operand::Copy(Place::local(g.param(0)));
    g.switch(n, vec![(0, left)], right).unwrap();
    for (way, number) in [(left, 1), (right, 2)] {
        g.switch_to(way);
        if assigns {
            let number = g.const_int(int64, number).unwrap();
            g.assign(Place::local(value), Rvalue::Use(number)).unwrap();
        }
        g.goto(meet).unwrap();
    }
    g.switch_to(meet);
}

/// `n` values that two ways assign, each read where the ways meet.
fn values_of_two_ways(n: usize) -> Module {
    generated(|g, _, int64| {
        for _ in 0..n {
            let value = g.temp(int64);
            two_ways(g, value, int64, true);
            g.assign(Place::local(g.result()), use_of(value)).unwrap();
        }
        g.ret().unwrap();
    })
}

/// `n` values, each read after `n` ways meet. With `before` they are
/// assigned before the ways part; without, two ways assign each, which
/// is farther back than the search walks.
fn values_read_after_many_ways(n: usize, before: bool) -> Module {
    generated(|g, _, int64| {
        let values: Vec<LocalId> = (0..n).map(|_| g.temp(int64)).collect();
        for &value in &values {
            match before {
                true => g.assign(Place::local(value), use_of(g.param(0))).unwrap(),
                false => two_ways(g, value, int64, true),
            }
        }
        let ways: Vec<BlockId> = (0..n).map(|_| g.new_block()).collect();
        let meet = g.new_block();
        let cases = ways.iter().enumerate();
        let cases = cases.map(|(at, &way)| (at as u64 + 1, way)).collect();
        let on = Operand::Copy(Place::local(g.param(0)));
        g.switch(on, cases, ways[0]).unwrap();
        for &way in &ways {
            g.switch_to(way);
            g.goto(meet).unwrap();
        }
        g.switch_to(meet);
        for value in values {
            g.assign(Place::local(g.result()), use_of(value)).unwrap();
        }
        g.ret().unwrap();
    })
}

/// A value that `n` ways assign, read where they meet.
fn value_of_ways(n: usize) -> Module {
    generated(|g, _, int64| {
        let value = g.temp(int64);
        let ways: Vec<BlockId> = (0..n).map(|_| g.new_block()).collect();
        let meet = g.new_block();
        let cases = ways.iter().enumerate();
        let cases = cases.map(|(at, &way)| (at as u64 + 1, way)).collect();
        let on = Operand::Copy(Place::local(g.param(0)));
        g.switch(on, cases, ways[0]).unwrap();
        for (at, &way) in ways.iter().enumerate() {
            g.switch_to(way);
            let number = g.const_int(int64, at as u64).unwrap();
            g.assign(Place::local(value), Rvalue::Use(number)).unwrap();
            g.goto(meet).unwrap();
        }
        g.switch_to(meet);
        g.assign(Place::local(g.result()), use_of(value)).unwrap();
        g.ret().unwrap();
    })
}

/// `n` loops in one another; each assigns a value at its start and
/// reads it and the value of the outermost where it is left.
fn loops_in_loops(n: usize) -> Module {
    generated(|g, _, _| {
        let outer = g.assign_temp(use_of(g.param(0))).unwrap();
        let heads: Vec<BlockId> = (0..n).map(|_| g.new_block()).collect();
        let exits: Vec<BlockId> = (0..n).map(|_| g.new_block()).collect();
        let innermost = g.new_block();
        g.goto(heads[0]).unwrap();
        for at in 0..n {
            g.switch_to(heads[at]);
            let own = g.assign_temp(use_of(outer)).unwrap();
            let inner = heads.get(at + 1).unwrap_or(&innermost);
            let on = Operand::Copy(Place::local(g.param(0)));
            g.switch(on, vec![(0, *inner)], exits[at]).unwrap();
            g.switch_to(exits[at]);
            let sum = Rvalue::Binary(
                BinOp::Add,
                Operand::Copy(Place::local(own)),
                Operand::Copy(Place::local(outer)),
            );
            g.assign(Place::local(g.result()), sum).unwrap();
            match at.checked_sub(1) {
                Some(around) => g.goto(heads[around]).unwrap(),
                None => g.ret().unwrap(),
            }
        }
        g.switch_to(innermost);
        g.goto(heads[n - 1]).unwrap();
    })
}

/// One block of `n` statements, each a step deeper into a value of
/// `n` tuples in one another.
fn steps_into_tuples(n: usize) -> Module {
    let depth = n.min(64);
    generated(|g, _, int64| {
        let mut nested = int64;
        for _ in 0..depth {
            nested = g.types().tuple(&[nested, TypeInterner::BOOL]);
        }
        let whole = g.var("whole", nested, true);
        for _ in 0..n / depth {
            let mut place = Place::local(whole);
            for _ in 0..depth {
                place = g.field(&place, 0).unwrap();
            }
            let read = Rvalue::Use(Operand::Copy(place));
            g.assign(Place::local(g.result()), read).unwrap();
        }
        g.ret().unwrap();
    })
}

/// `n` structs, each with the next as a part, and `n` functions.
fn structs_and_functions(n: usize) -> Module {
    let mut module = Module::default();
    let mut part = module.types.int(64, true);
    for at in 0..n {
        let holder = module.types.declare_struct(&format!("p.S{at}"));
        let Type::Struct(id) = *module.types.get(holder) else {
            panic!();
        };
        let array = module.types.intern(Type::Array(part, 2));
        let fields = [("one", part), ("two", array)];
        module.types.set_fields(id, &fields).unwrap();
        part = holder;
        let mut f = declared(&mut module, &format!("p.f{at}"), &[part], part);
        let entry = f.new_block();
        f.switch_to(entry);
        f.assign(Place::local(f.result()), use_of(f.param(0)))
            .unwrap();
        f.ret().unwrap();
        f.finish().unwrap();
    }
    module
}

#[test]
fn the_steps_grow_with_the_module() {
    let sizes = [250, 500, 1000, 2000];
    assert_near_linear(&sizes, |n| steps(&calls_into_one_cleanup(n)));
    assert_near_linear(&sizes, |n| steps(&values_of_two_ways(n)));
    assert_near_linear(&sizes, |n| steps(&values_read_after_many_ways(n, true)));
    // What breaks the rule is found in as few steps: every value is
    // reported once.
    assert_near_linear(&sizes, |n| {
        let (errors, steps) = verify_counted(&values_read_after_many_ways(n, false));
        assert_eq!(errors.len(), n);
        steps
    });
    assert_near_linear(&sizes, |n| steps(&loops_in_loops(n)));
    assert_near_linear(&sizes, |n| steps(&steps_into_tuples(n)));
    assert_near_linear(&sizes, |n| steps(&structs_and_functions(n)));
}

/// A value that two ways assign, read after `between` more places
/// where two ways meet.
fn read_after_meetings(between: usize) -> Module {
    generated(|g, _, int64| {
        let (value, other) = (g.temp(int64), g.temp(int64));
        two_ways(g, value, int64, true);
        for _ in 0..between {
            two_ways(g, other, int64, false);
        }
        g.assign(Place::local(g.result()), use_of(value)).unwrap();
        g.ret().unwrap();
    })
}

#[test]
fn a_value_of_several_ways_is_found_as_far_back_as_the_search_walks() {
    assert_eq!(verify_module(&read_after_meetings(0)), Ok(()));
    assert_eq!(verify_module(&read_after_meetings(15)), Ok(()));
    assert_eq!(
        said(&read_after_meetings(16)),
        "V085: fn p.g: bb51 #0: the temporary _2 may have no value"
    );
    assert_eq!(verify_module(&value_of_ways(32)), Ok(()));
    assert_eq!(
        said(&value_of_ways(33)),
        "V085: fn p.g: bb34 #0: the temporary _2 may have no value"
    );
    // Blocks that one way leads through do not count.
    let far = generated(|g, ext, int64| {
        let value = g.temp(int64);
        two_ways(g, value, int64, true);
        for _ in 0..100 {
            let (next, result) = (g.new_block(), g.temp(int64));
            let (callee, arg) = (g.const_func(ext), g.const_int(int64, 1).unwrap());
            g.call(
                callee,
                vec![arg],
                Place::local(result),
                Some(next),
                UnwindAction::Continue,
            )
            .unwrap();
            g.switch_to(next);
        }
        g.assign(Place::local(g.result()), use_of(value)).unwrap();
        g.ret().unwrap();
    });
    assert_eq!(verify_module(&far), Ok(()));
}

/// `count` functions of `calls` calls each; every third reads a value
/// that it has not got.
fn functions_of_calls(count: usize, calls: usize) -> Module {
    let mut module = Module::default();
    let int64 = module.types.int(64, true);
    let ext = declared(&mut module, "p.ext", &[int64], int64)
        .finish()
        .unwrap();
    for at in 0..count {
        let mut g = declared(&mut module, &format!("p.g{at}"), &[int64], int64);
        let (entry, cleanup) = (g.new_block(), g.new_block());
        g.set_cleanup(cleanup);
        g.switch_to(entry);
        let mut last = g.param(0);
        for _ in 0..calls {
            let (next, result) = (g.new_block(), g.temp(int64));
            let (callee, arg) = (g.const_func(ext), Operand::Copy(Place::local(last)));
            let unwind = UnwindAction::Cleanup(cleanup);
            g.call(callee, vec![arg], Place::local(result), Some(next), unwind)
                .unwrap();
            g.switch_to(next);
            last = result;
        }
        g.assign(Place::local(g.result()), use_of(last)).unwrap();
        g.ret().unwrap();
        g.switch_to(cleanup);
        if at % 3 == 0 {
            g.assign(Place::local(g.result()), use_of(last)).unwrap();
        }
        g.resume().unwrap();
        g.finish().unwrap();
    }
    module
}

#[test]
fn what_is_found_does_not_depend_on_the_number_of_threads() {
    let module = functions_of_calls(100, 8);
    let found_by = |threads: usize| {
        let pool = rayon::ThreadPoolBuilder::new()
            .num_threads(threads)
            .build()
            .unwrap();
        pool.install(|| verify_counted(&module))
    };
    let alone = found_by(1);
    let functions: Vec<&str> = alone.0.iter().filter_map(|e| e.func.as_deref()).collect();
    let expected: Vec<String> = (0..100).step_by(3).map(|at| format!("p.g{at}")).collect();
    assert_eq!(functions, expected);
    for threads in [2, 5, 16] {
        assert_eq!(found_by(threads), alone, "{threads} threads");
    }
}
