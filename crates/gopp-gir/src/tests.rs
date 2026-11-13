use gopp_target::TargetSpec;

use crate::*;

const HERE: Span = Span {
    file: 1,
    line: 7,
    col: 2,
};

/// A module with the function `write` of C declared in it.
fn module_with_write() -> (Module, FuncId) {
    let mut module = Module {
        name: "command-line-arguments".to_owned(),
        files: vec!["src/hello.gopp".to_owned()],
        ..Module::default()
    };
    let types = &mut module.types;
    let (int32, int64, uint64) = (
        types.int(32, true),
        types.int(64, true),
        types.int(64, false),
    );
    let byte = types.int(8, false);
    let bytes = types.ptr(byte);
    let sig = types.intern(Type::Func(FuncType {
        params: [int32, bytes, uint64].into(),
        ret: int64,
        has_env: false,
        cc: CallConv::C,
    }));
    let mut write = FuncBuilder::new(
        &mut module,
        "write",
        sig,
        Linkage::External,
        Span::default(),
    )
    .unwrap();
    write.set_no_unwind();
    let write = write.finish().unwrap();
    (module, write)
}

fn go_sig(module: &mut Module, params: &[TypeId], ret: TypeId) -> TypeId {
    module.types.intern(Type::Func(FuncType {
        params: params.into(),
        ret,
        has_env: false,
        cc: CallConv::Gopp,
    }))
}

#[test]
fn a_module_is_printed_as_the_design_writes_it() {
    let (mut module, write) = module_with_write();
    let sig = go_sig(&mut module, &[], TypeInterner::UNIT);
    let mut main =
        FuncBuilder::new(&mut module, "main.main", sig, Linkage::External, HERE).unwrap();
    let entry = main.new_block();
    let done = main.new_block();
    main.switch_to(entry);
    main.set_loc(HERE);
    let hello = main.const_str(b"hello\n");
    let text = main.assign_temp(Rvalue::Use(hello)).unwrap();
    let text = Place::local(text);
    let int32 = main.types().int(32, true);
    let int64 = main.types().int(64, true);
    let result = main.temp(int64);
    let args = vec![
        main.const_int(int32, 1).unwrap(),
        Operand::Copy(main.field(&text, 0).unwrap()),
        Operand::Copy(main.field(&text, 1).unwrap()),
    ];
    let callee = main.const_func(write);
    main.call(
        callee,
        args,
        Place::local(result),
        Some(done),
        UnwindAction::Unreachable,
    )
    .unwrap();
    main.switch_to(done);
    main.set_loc(Span {
        line: 8,
        col: 1,
        ..HERE
    });
    main.ret().unwrap();
    main.finish().unwrap();
    let expected = "module \"command-line-arguments\"
file 0 \"src/hello.gopp\"

declare fn write(_1: i32, _2: *u8, _3: u64) -> i64 [c, external, nounwind]

fn main.main() -> () [gopp, external] {
  let _0: ()
  let _1: string
  let _2: i64
  bb0:
    _1 = const str#0 \"hello\\0A\"  ; hello.gopp:7:2
    _2 = call const fn @write(const 1: i32, copy _1.0, copy _1.1) -> [return: bb1, unwind: unreachable]  ; hello.gopp:7:2
  bb1:
    return  ; hello.gopp:8:1
}\n";
    assert_eq!(print_module(&module), expected);
}

#[test]
fn every_kind_of_statement_and_end_has_its_text() {
    let (mut module, write) = module_with_write();
    let types = &mut module.types;
    let (int64, float64) = (types.int(64, true), types.float(64));
    let point = types.declare_struct("p.Point");
    let Type::Struct(point_id) = *types.get(point) else {
        panic!();
    };
    types
        .set_fields(point_id, &[("x", int64), ("y", int64)])
        .unwrap();
    let points = types.intern(Type::Slice(point));
    let counter = module.globals.alloc(Global {
        name: "p.counter".to_owned(),
        ty: int64,
        link: Linkage::Internal,
        mutable: true,
        parts: Vec::new(),
    });
    let sig = go_sig(&mut module, &[points, int64], float64);
    let mut f = FuncBuilder::new(&mut module, "p.f", sig, Linkage::LinkOnceOdr, HERE).unwrap();
    f.set_inline(Inline::Never);
    f.set_name(f.param(0), "ps");
    let [entry, checked, loop_, small, done, cleanup, panics] = [(); 7].map(|()| f.new_block());
    f.set_cleanup(cleanup);
    f.set_cold(panics);
    f.switch_to(entry);
    let (ps, at) = (Place::local(f.param(0)), f.param(1));
    let len = f.assign_temp(Rvalue::Len(ps.clone())).unwrap();
    let uint64 = f.types().int(64, false);
    let index = f
        .assign_temp(Rvalue::Cast(
            CastKind::IntToInt,
            Operand::Copy(Place::local(at)),
            uint64,
        ))
        .unwrap();
    let in_range = f
        .assign_temp(Rvalue::Binary(
            BinOp::Lt,
            Operand::Copy(Place::local(index)),
            Operand::Copy(Place::local(len)),
        ))
        .unwrap();
    let said = vec![
        Operand::Copy(Place::local(index)),
        Operand::Copy(Place::local(len)),
    ];
    f.assert(
        Operand::Copy(Place::local(in_range)),
        true,
        (AssertKind::Bounds, said),
        checked,
        UnwindAction::Cleanup(cleanup),
    )
    .unwrap();

    f.switch_to(checked);
    f.push_scope(
        ScopeKind::Block,
        Span {
            line: 9,
            col: 3,
            ..HERE
        },
    );
    let p = f.var("p", point, false);
    f.set_owned(p);
    f.storage_live(p).unwrap();
    let element = f.index(&ps, at).unwrap();
    f.assign(Place::local(p), Rvalue::Use(Operand::Copy(element)))
        .unwrap();
    let x = f.field(&Place::local(p), 0).unwrap();
    let address = f
        .assign_temp(Rvalue::AddrOf(x.clone(), Mutability::Mut))
        .unwrap();
    let through = f.deref(&Place::local(address)).unwrap();
    let one = f.const_int(int64, u64::MAX).unwrap();
    let sum = Rvalue::Binary(BinOp::Add, Operand::Copy(through.clone()), one);
    f.assign(through, sum).unwrap();
    let both = f
        .assign_temp(Rvalue::Aggregate(
            AggKind::Array(int64),
            vec![Operand::Copy(x.clone()), Operand::Move(x.clone())],
        ))
        .unwrap();
    let second = f.const_index(&Place::local(both), 1).unwrap();
    let made = Rvalue::Aggregate(
        AggKind::Of(point),
        vec![Operand::Copy(second.clone()), Operand::Copy(second)],
    );
    f.assign(Place::local(p), made).unwrap();
    let tuple = f
        .assign_temp(Rvalue::Aggregate(
            AggKind::Tuple,
            vec![Operand::Copy(Place::local(at)), f.const_bool(true)],
        ))
        .unwrap();
    let flag = f.field(&Place::local(tuple), 1).unwrap();
    let not = f
        .assign_temp(Rvalue::Unary(UnOp::Not, Operand::Copy(flag)))
        .unwrap();
    f.switch(Operand::Copy(Place::local(not)), vec![(0, loop_)], small)
        .unwrap();

    f.switch_to(loop_);
    let global = Operand::Const(Constant {
        kind: ConstantKind::Global(counter),
        ty: f.types().ptr(int64),
    });
    let counter = f.assign_temp(Rvalue::Use(global)).unwrap();
    let count = f.deref(&Place::local(counter)).unwrap();
    f.switch(Operand::Copy(count), vec![(1, small), (2, done)], loop_)
        .unwrap();

    f.switch_to(small);
    let as_float = Rvalue::Cast(CastKind::IntToFloat, Operand::Copy(x), float64);
    f.assign(Place::local(f.result()), as_float).unwrap();
    f.storage_dead(p).unwrap();
    f.pop_scope();
    f.drop(Place::local(p), done, UnwindAction::Terminate)
        .unwrap();

    f.switch_to(done);
    f.ret().unwrap();

    f.switch_to(cleanup);
    f.goto(panics).unwrap();
    f.switch_to(panics);
    let byte = f.types().int(8, false);
    let null = Operand::Const(Constant {
        kind: ConstantKind::Null,
        ty: f.types().ptr(byte),
    });
    let zero = f.const_int(uint64, 0).unwrap();
    let int32 = f.types().int(32, true);
    let fd = f.const_int(int32, 2).unwrap();
    let wrote = f.temp(int64);
    let callee = f.const_func(write);
    f.call(
        callee,
        vec![fd, null, zero],
        Place::local(wrote),
        None,
        UnwindAction::Continue,
    )
    .unwrap();
    f.finish().unwrap();
    let mut resumes = FuncBuilder::new(&mut module, "p.g", sig, Linkage::Internal, HERE).unwrap();
    resumes.set_no_return();
    let only = resumes.new_block();
    resumes.set_catch(only);
    resumes.switch_to(only);
    let zero = Operand::Const(Constant {
        kind: ConstantKind::Zero,
        ty: float64,
    });
    let half = Operand::Const(Constant {
        kind: ConstantKind::Float(0.5f64.to_bits()),
        ty: float64,
    });
    let sum = resumes
        .assign_temp(Rvalue::Binary(BinOp::Mul, zero, half))
        .unwrap();
    let negated = Rvalue::Unary(UnOp::Neg, Operand::Move(Place::local(sum)));
    resumes
        .assign(Place::local(resumes.result()), negated)
        .unwrap();
    resumes.resume().unwrap();
    resumes.finish().unwrap();
    let expected = "module \"command-line-arguments\"
file 0 \"src/hello.gopp\"

global var @p.counter: i64 [internal]

declare fn write(_1: i32, _2: *u8, _3: u64) -> i64 [c, external, nounwind]

fn p.f(_1: []p.Point, _2: i64) -> f64 [gopp, linkonce_odr, noinline] {
  scope 1: block in 0  ; hello.gopp:9:3
  let _0: f64
  let _1: []p.Point \"ps\"
  let _3: u64
  let _4: u64
  let _5: bool
  let _6: p.Point \"p\" [const, owned]
  let _7: *i64
  let _8: [2]i64
  let _9: (i64, bool)
  let _10: bool
  let _11: *i64
  let _12: i64
  bb0:
    _3 = len(_1)  ; hello.gopp:7:2
    _4 = int_to_int(copy _2) as u64  ; hello.gopp:7:2
    _5 = lt(copy _4, copy _3)  ; hello.gopp:7:2
    assert(copy _5 == true, Bounds(copy _4, copy _3)) -> [success: bb1, unwind: cleanup bb5]  ; hello.gopp:7:2
  bb1:
    storage_live(_6)  ; hello.gopp:7:2 scope 1
    _6 = copy _1[_2]  ; hello.gopp:7:2 scope 1
    _7 = &mut _6.0  ; hello.gopp:7:2 scope 1
    (*_7) = add(copy (*_7), const -1: i64)  ; hello.gopp:7:2 scope 1
    _8 = array i64 {copy _6.0, move _6.0}  ; hello.gopp:7:2 scope 1
    _6 = p.Point {copy _8[1], copy _8[1]}  ; hello.gopp:7:2 scope 1
    _9 = tuple {copy _2, const true}  ; hello.gopp:7:2 scope 1
    _10 = not(copy _9.1)  ; hello.gopp:7:2 scope 1
    switch copy _10 [0: bb2, otherwise: bb3]  ; hello.gopp:7:2 scope 1
  bb2:
    _11 = const global @p.counter  ; hello.gopp:7:2 scope 1
    switch copy (*_11) [1: bb3, 2: bb4, otherwise: bb2]  ; hello.gopp:7:2 scope 1
  bb3:
    _0 = int_to_float(copy _6.0) as f64  ; hello.gopp:7:2 scope 1
    storage_dead(_6)  ; hello.gopp:7:2 scope 1
    drop(_6) -> [return: bb4, unwind: terminate]  ; hello.gopp:7:2
  bb4:
    return  ; hello.gopp:7:2
  bb5: [cleanup]
    goto bb6  ; hello.gopp:7:2
  bb6: [cold]
    _12 = call const fn @write(const 2: i32, const null: *u8, const 0: u64) -> [unwind: continue]  ; hello.gopp:7:2
}

fn p.g(_1: []p.Point, _2: i64) -> f64 [gopp, internal, noreturn] {
  let _0: f64
  let _3: f64
  bb0: [catch]
    _3 = mul(const zero: f64, const 0x3fe0000000000000: f64)  ; hello.gopp:7:2
    _0 = neg(move _3)  ; hello.gopp:7:2
    resume  ; hello.gopp:7:2
}\n";
    assert_eq!(print_module(&module), expected);
}

#[test]
fn the_builder_says_the_type_of_every_rvalue_and_refuses_what_has_none() {
    let mut module = Module::default();
    let types = &mut module.types;
    let (int8, int64, uint64) = (
        types.int(8, true),
        types.int(64, true),
        types.int(64, false),
    );
    let (float32, float64) = (types.float(32), types.float(64));
    let string = types.intern(Type::String);
    let ints = types.intern(Type::Slice(int64));
    let pointer = types.ptr(int64);
    let option = types.intern(Type::Option(int8));
    let sig = go_sig(
        &mut module,
        &[int64, float64, string, ints, pointer, option, int8],
        int64,
    );
    let mut f = FuncBuilder::new(&mut module, "p.f", sig, Linkage::Internal, HERE).unwrap();
    let block = f.new_block();
    f.switch_to(block);
    let param = |f: &FuncBuilder, at: u32| Operand::Copy(Place::local(f.param(at)));
    let (i, x, s, xs, p, o, small) = (0, 1, 2, 3, 4, 5, 6);
    let place = |f: &FuncBuilder, at: u32| Place::local(f.param(at));
    let typed = |f: &mut FuncBuilder, rvalue: Rvalue| {
        let ty = f.rvalue_ty(&rvalue).map(|ty| f.types().display(ty));
        ty.map_err(|error| error.to_string())
    };
    let ok = |text: &str| Ok(text.to_owned());
    let no = |text: &str| Err(text.to_owned());
    use BinOp::*;
    let cases = [
        (Rvalue::Use(param(&f, s)), ok("string")),
        (Rvalue::AddrOf(place(&f, xs), Mutability::Not), ok("*[]i64")),
        (Rvalue::Len(place(&f, s)), ok("u64")),
        (Rvalue::Len(place(&f, xs)), ok("u64")),
        (Rvalue::Len(place(&f, i)), no("len of a i64")),
        (Rvalue::Binary(Add, param(&f, i), param(&f, i)), ok("i64")),
        (Rvalue::Binary(Div, param(&f, x), param(&f, x)), ok("f64")),
        (
            Rvalue::Binary(Rem, param(&f, x), param(&f, x)),
            no("binary operator of a f64"),
        ),
        (
            Rvalue::Binary(BitClear, param(&f, i), param(&f, i)),
            ok("i64"),
        ),
        (
            Rvalue::Binary(Shl, param(&f, i), param(&f, small)),
            ok("i64"),
        ),
        (
            Rvalue::Binary(Shr, param(&f, x), param(&f, i)),
            no("shift of a f64"),
        ),
        (Rvalue::Binary(Eq, param(&f, p), param(&f, p)), ok("bool")),
        (Rvalue::Binary(Lt, param(&f, x), param(&f, x)), ok("bool")),
        (
            Rvalue::Binary(Lt, param(&f, p), param(&f, p)),
            no("binary operator of a *i64"),
        ),
        (
            Rvalue::Binary(Eq, param(&f, s), param(&f, s)),
            no("binary operator of a string"),
        ),
        (
            Rvalue::Binary(Add, param(&f, i), param(&f, small)),
            no("binary operator: a i8 where a i64 is needed"),
        ),
        (Rvalue::Unary(UnOp::Neg, param(&f, x)), ok("f64")),
        (Rvalue::Unary(UnOp::BitNot, param(&f, i)), ok("i64")),
        (
            Rvalue::Unary(UnOp::BitNot, param(&f, x)),
            no("unary operator of a f64"),
        ),
        (Rvalue::Unary(UnOp::Not, f.const_bool(true)), ok("bool")),
        (
            Rvalue::Unary(UnOp::Not, param(&f, i)),
            no("unary operator of a i64"),
        ),
        (
            Rvalue::Cast(CastKind::IntToInt, param(&f, i), int8),
            ok("i8"),
        ),
        (
            Rvalue::Cast(CastKind::IntToFloat, param(&f, i), float32),
            ok("f32"),
        ),
        (
            Rvalue::Cast(CastKind::FloatToInt, param(&f, x), uint64),
            ok("u64"),
        ),
        (
            Rvalue::Cast(CastKind::FloatToFloat, param(&f, x), float32),
            ok("f32"),
        ),
        (
            Rvalue::Cast(CastKind::PtrToInt, param(&f, p), uint64),
            ok("u64"),
        ),
        (
            Rvalue::Cast(CastKind::IntToPtr, param(&f, i), pointer),
            ok("*i64"),
        ),
        (
            Rvalue::Cast(CastKind::IntToInt, param(&f, x), int8),
            no("cast: a f64 where a i8 is needed"),
        ),
        (
            Rvalue::Aggregate(AggKind::Tuple, vec![param(&f, i), param(&f, s)]),
            ok("(i64, string)"),
        ),
        (Rvalue::Aggregate(AggKind::Tuple, Vec::new()), ok("()")),
        (
            Rvalue::Aggregate(AggKind::Array(int64), vec![param(&f, i), param(&f, i)]),
            ok("[2]i64"),
        ),
        (
            Rvalue::Aggregate(AggKind::Array(int64), vec![param(&f, x)]),
            no("aggregate: a f64 where a i64 is needed"),
        ),
        (
            Rvalue::Aggregate(
                AggKind::Of(option),
                vec![param(&f, small), f.const_bool(true)],
            ),
            ok("?i8"),
        ),
        (
            Rvalue::Aggregate(AggKind::Of(option), vec![param(&f, small)]),
            no("1 values for 2"),
        ),
        (
            Rvalue::Aggregate(AggKind::Of(int64), Vec::new()),
            no("aggregate of a i64"),
        ),
    ];
    for (rvalue, expected) in cases {
        assert_eq!(typed(&mut f, rvalue.clone()), expected, "{rvalue:?}");
    }

    // A place says the type of each of its steps.
    let shown = |f: &mut FuncBuilder, place: Result<Place, BuildError>| match place {
        Ok(place) => {
            let ty = f.place_ty(&place);
            Ok(f.types().display(ty))
        }
        Err(error) => Err(error.to_string()),
    };
    let index = f.param(i);
    let places = [
        (f.deref(&place(&f, p)), ok("i64")),
        (f.deref(&place(&f, i)), no("deref of a i64")),
        (f.field(&place(&f, s), 1), ok("u64")),
        (f.field(&place(&f, xs), 0), ok("*i64")),
        (f.field(&place(&f, xs), 3), ok("*u8")),
        (f.field(&place(&f, o), 1), ok("bool")),
        (f.field(&place(&f, o), 2), no("field of a ?i8")),
        (f.index(&place(&f, xs), index), ok("i64")),
        (f.index(&place(&f, s), index), ok("u8")),
        (f.index(&place(&f, p), index), no("index of a *i64")),
        (
            f.const_index(&place(&f, xs), 0),
            no("constant index of a []i64"),
        ),
    ];
    for (place, expected) in places {
        assert_eq!(shown(&mut f, place), expected);
    }

    // A statement and an end are of the types that they join.
    let wrong = f.assign(place(&f, x), Rvalue::Use(param(&f, i)));
    assert_eq!(
        wrong.unwrap_err().to_string(),
        "assignment: a i64 where a f64 is needed"
    );
    let no_function = f.call(
        param(&f, i),
        Vec::new(),
        place(&f, i),
        None,
        UnwindAction::Continue,
    );
    assert_eq!(
        no_function.unwrap_err().to_string(),
        "i64 is no function type"
    );
    let on_float = f.switch(param(&f, x), Vec::new(), block);
    assert_eq!(on_float.unwrap_err().to_string(), "switch of a f64");
    assert_eq!(
        f.const_int(float64, 1).unwrap_err().to_string(),
        "integer constant of a f64"
    );
    let cut = f.const_int(int8, 0x1ff).unwrap();
    assert!(matches!(
        cut,
        Operand::Const(Constant {
            kind: ConstantKind::Int(0xff),
            ..
        })
    ));
    // A block has one end, and every block has one.
    f.ret().unwrap();
    assert_eq!(f.ret().unwrap_err(), BuildError::NoBlock);
    assert_eq!(f.storage_live(index).unwrap_err(), BuildError::NoBlock);
    let open = f.new_block();
    f.switch_to(open);
    assert_eq!(
        f.finish().unwrap_err().to_string(),
        "the block bb1 has no end"
    );
}

#[test]
fn one_type_is_one_id_and_a_type_that_holds_itself_is_one_by_its_shape() {
    let mut types = TypeInterner::new();
    let int = types.int(64, true);
    assert_eq!(types.int(64, true), int);
    assert_ne!(types.int(64, false), int);
    assert_eq!(types.tuple(&[]), TypeInterner::UNIT);
    let slice = types.intern(Type::Slice(int));
    assert_eq!(types.intern(Type::Slice(int)), slice);

    // `type S []S` and `type T []T` are one type; `type M map…` another.
    let s = types.rec("p.S", |types, me| types.intern(Type::Slice(me)));
    let t = types.rec("p.T", |types, me| types.intern(Type::Slice(me)));
    assert_eq!(s, t);
    assert_eq!(types.display(t), "p.S");
    let a = types.rec("p.A", |types, me| {
        let pointer = types.ptr(me);
        let option = types.intern(Type::Option(pointer));
        types.intern(Type::Array(option, 2))
    });
    assert_ne!(a, s);
    let b = types.rec("p.B", |types, me| {
        let pointer = types.ptr(me);
        let option = types.intern(Type::Option(pointer));
        types.intern(Type::Array(option, 3))
    });
    assert_ne!(a, b);
    // A shell that names another shell is told from one that names
    // itself.
    let of_s = types.rec("p.U", |types, _| types.intern(Type::Slice(s)));
    assert_ne!(of_s, s);
    let Type::Rec(rec) = *types.get(a) else {
        panic!();
    };
    assert_eq!(types.display(types.rec_body(rec).unwrap()), "[2]?*p.A");

    // The fields of a struct are said once, and may name the struct.
    let node = types.declare_struct("p.Node");
    let Type::Struct(id) = *types.get(node) else {
        panic!();
    };
    assert_eq!(types.struct_fields(id), None);
    let next = types.ptr(node);
    types
        .set_fields(id, &[("value", int), ("next", next)])
        .unwrap();
    assert_eq!(types.struct_fields(id), Some(&[int, next][..]));
    assert_eq!(types.field_names(id), ["value", "next"]);
    let again = types.set_fields(id, &[]);
    assert_eq!(
        again.unwrap_err().to_string(),
        "the struct p.Node has fields already"
    );
    let func = types.intern(Type::Func(FuncType {
        params: [node, slice].into(),
        ret: TypeInterner::BOOL,
        has_env: true,
        cc: CallConv::Gopp,
    }));
    assert_eq!(types.display(func), "fn env(p.Node, []i64) -> bool");
}

#[test]
fn a_type_is_as_large_as_the_target_lays_it_out() {
    let mut types = TypeInterner::new();
    let (byte, int32, int64) = (
        types.int(8, false),
        types.int(32, true),
        types.int(64, true),
    );
    let string = types.intern(Type::String);
    let node = types.declare_struct("p.Node");
    let Type::Struct(id) = *types.get(node) else {
        panic!();
    };
    let next = types.ptr(node);
    let option = types.intern(Type::Option(int32));
    types
        .set_fields(
            id,
            &[
                ("tag", byte),
                ("name", string),
                ("next", next),
                ("n", option),
            ],
        )
        .unwrap();
    let pair = types.tuple(&[byte, int64]);
    let grid = types.intern(Type::Array(pair, 3));
    let shell = types.rec("p.S", |types, me| types.intern(Type::Slice(me)));
    let func = types.intern(Type::Func(FuncType {
        params: [].into(),
        ret: TypeInterner::UNIT,
        has_env: false,
        cc: CallConv::Gopp,
    }));
    let table = |types: &TypeInterner, target: &str| {
        let target = TargetSpec::named(target).unwrap().layout();
        [node, pair, grid, shell, func, TypeInterner::UNIT]
            .map(|ty| {
                let layout = types.layout(ty, target).unwrap();
                format!("{} {}/{}", types.display(ty), layout.size, layout.align)
            })
            .join(", ")
    };
    assert_eq!(
        table(&types, "linux/amd64"),
        "p.Node 48/8, (u8, i64) 16/8, [3](u8, i64) 48/8, p.S 32/8, fn() -> () 8/8, () 0/1"
    );
    assert_eq!(
        table(&types, "wasip1/wasm32"),
        "p.Node 48/8, (u8, i64) 16/8, [3](u8, i64) 48/8, p.S 32/8, fn() -> () 4/4, () 0/1"
    );

    // What has no size says why.
    let target = TargetSpec::named("linux/amd64").unwrap().layout();
    let open = types.declare_struct("p.Open");
    assert_eq!(
        types.layout(open, target).unwrap_err().to_string(),
        "the type p.Open has no fields yet"
    );
    let own = types.declare_struct("p.Own");
    let Type::Struct(own_id) = *types.get(own) else {
        panic!();
    };
    let inner = types.tuple(&[int64, own]);
    types.set_fields(own_id, &[("inner", inner)]).unwrap();
    assert_eq!(
        types.layout(own, target).unwrap_err().to_string(),
        "the type p.Own holds itself by value"
    );
    let huge = types.intern(Type::Array(int64, u64::MAX));
    assert_eq!(
        types.layout(huge, target).unwrap_err().to_string(),
        "a value of the type [18446744073709551615]i64 would be too large"
    );
}

#[test]
fn a_deep_type_is_walked_without_a_deep_stack() {
    let mut types = TypeInterner::new();
    let mut ty = types.int(8, false);
    for _ in 0..200_000 {
        let pair = types.tuple(&[ty, TypeInterner::BOOL]);
        ty = types.intern(Type::Array(pair, 1));
    }
    let target = TargetSpec::named("linux/amd64").unwrap().layout();
    let layout = types.layout(ty, target).unwrap();
    assert_eq!((layout.size, layout.align), (200_001, 1));
    assert_eq!(
        types.display(ty).len(),
        200_000 * "[1](, bool)".len() + "u8".len()
    );
    let shell = types.rec("p.Deep", |types, me| {
        let mut ty = me;
        for _ in 0..200_000 {
            ty = types.ptr(ty);
        }
        ty
    });
    assert_eq!(types.layout(shell, target).unwrap().size, 8);
}
