//! The types of a module as types of C. A type of several parts is a
//! struct named after its number in the module; an array is a struct
//! around a C array, so that it is assigned and returned as a value. A
//! string is the struct that the header of the runtime declares.

use std::fmt::Write;

use gopp_gir::{CallConv, FuncType, Module, Type, TypeId};
use gopp_target::{Layout, Part, SLICE, STRING, TargetLayout};

use super::number::int_type;
use super::{Cx, EmitError};

/// How each type is written where a value of it is declared. A struct
/// that has a name for a debugger in `tags` is written by that name.
pub(super) fn spelled(
    module: &Module,
    layouts: &[Layout],
    tags: &[String],
    steps: &mut u64,
) -> Vec<String> {
    let types = &module.types;
    let mut spelled: Vec<Option<String>> = vec![None; types.count()];
    for ty in types.ids() {
        // The pointers on the way to a type that is no pointer, or to
        // one that is spelled already.
        let mut pointers = Vec::new();
        let mut at = ty;
        loop {
            *steps += 1;
            if spelled[at.index()].is_some() {
                break;
            }
            let own = match types.get(at) {
                Type::Ptr(to) => {
                    pointers.push((at, *to));
                    at = *to;
                    continue;
                }
                _ if layouts[at.index()].size == 0 => "void".to_owned(),
                Type::String => STRING.name.to_owned(),
                Type::Unit => "void".to_owned(),
                Type::Bool => "bool".to_owned(),
                Type::Int { bits, signed } => int_type(*bits, *signed),
                Type::Float { bits: 32 } => "float".to_owned(),
                Type::Float { .. } => "double".to_owned(),
                _ => match tags.get(at.index()).filter(|tag| !tag.is_empty()) {
                    Some(tag) => tag.clone(),
                    None => format!("T{}", at.index()),
                },
            };
            spelled[at.index()] = Some(own);
            break;
        }
        for (pointer, to) in pointers.into_iter().rev() {
            // What takes no room has no type of its own in C.
            let to = spelled[to.index()].as_deref().unwrap_or("void");
            spelled[pointer.index()] = Some(format!("{to} *"));
        }
    }
    spelled.into_iter().map(Option::unwrap_or_default).collect()
}

/// The type as GIR writes it, for a comment.
fn comment(cx: &Cx<'_>, ty: TypeId) -> String {
    cx.module.types.display(ty).replace("*/", "* /")
}

/// Whether the type is written as a struct of C.
fn is_struct(kind: &Type) -> bool {
    matches!(
        kind,
        Type::Slice(_) | Type::Array(..) | Type::Tuple(_) | Type::Option(_) | Type::Struct(_)
    )
}

/// The types that a value of the type holds as values, which C must
/// know whole before it.
fn held(cx: &Cx<'_>, ty: TypeId) -> Vec<TypeId> {
    match cx.kind(ty) {
        Type::Array(of, _) | Type::Option(of) => vec![*of],
        Type::Tuple(fields) => fields.to_vec(),
        Type::Struct(id) => cx
            .module
            .types
            .struct_fields(*id)
            .unwrap_or_default()
            .to_vec(),
        _ => Vec::new(),
    }
}

/// The types that the declaration of a function type names: its
/// parameters and its result, through the pointers on the way.
fn named(cx: &Cx<'_>, ty: TypeId) -> Vec<TypeId> {
    let Type::Func(func) = cx.kind(ty) else {
        return Vec::new();
    };
    let through = |mut part: TypeId| {
        while let Type::Ptr(to) = cx.kind(part) {
            part = *to;
        }
        part
    };
    func.params
        .iter()
        .chain([&func.ret])
        .map(|&part| through(part))
        .collect()
}

/// Every type in an order in which each comes after those that `before`
/// says it needs, among those that `wanted` picks.
fn ordered(
    cx: &Cx<'_>,
    steps: &mut u64,
    wanted: impl Fn(&Type) -> bool,
    before: impl Fn(&Cx<'_>, TypeId) -> Vec<TypeId>,
) -> Vec<TypeId> {
    let mut order = Vec::new();
    let mut placed = vec![false; cx.module.types.count()];
    for start in cx.module.types.ids() {
        let mut rest = vec![(start, false)];
        while let Some((ty, leaving)) = rest.pop() {
            *steps += 1;
            if leaving {
                order.push(ty);
                continue;
            }
            if std::mem::replace(&mut placed[ty.index()], true) || !wanted(cx.kind(ty)) {
                continue;
            }
            rest.push((ty, true));
            rest.extend(before(cx, ty).into_iter().map(|part| (part, false)));
        }
    }
    order
}

/// Which types a function of C takes or gives by value, with those
/// inside them: the code on the other side lays them out too.
fn crossing(cx: &Cx<'_>, steps: &mut u64) -> Vec<bool> {
    fn of(sig: &FuncType) -> impl Iterator<Item = TypeId> + '_ {
        sig.params.iter().chain([&sig.ret]).copied()
    }
    let mut rest: Vec<TypeId> = Vec::new();
    for func in cx.module.funcs.values() {
        match cx.kind(func.sig()) {
            Type::Func(sig) if sig.cc == CallConv::C && !cx.of_runtime(func.name()) => {
                rest.extend(of(sig));
            }
            _ => {}
        }
    }
    let mut crosses = vec![false; cx.module.types.count()];
    while let Some(ty) = rest.pop() {
        *steps += 1;
        if std::mem::replace(&mut crosses[ty.index()], true) {
            continue;
        }
        match cx.kind(ty) {
            Type::Func(sig) if sig.cc == CallConv::C => rest.extend(of(sig)),
            _ => rest.extend(held(cx, ty)),
        }
    }
    crosses
}

/// The declarations of the types, each after what it needs, and the
/// checks that C lays them out as the target does.
pub(super) fn definitions(
    cx: &Cx<'_>,
    target: TargetLayout,
    out: &mut String,
    steps: &mut u64,
) -> Result<(), EmitError> {
    let types = &cx.module.types;
    out.push('\n');
    // A name of the source is no macro of a header here.
    for name in cx.field_names() {
        let _ = writeln!(out, "#undef {name}");
    }
    for ty in types.ids() {
        *steps += 1;
        if let Type::Rec(_) = cx.kind(ty) {
            let what = "a type that holds itself and is no struct";
            return Err(EmitError::Unsupported { func: None, what });
        }
        if is_struct(cx.kind(ty)) && !cx.empty(ty) {
            let name = cx.spelled(ty);
            let _ = writeln!(
                out,
                "typedef struct {name} {name}; /* {} */",
                comment(cx, ty)
            );
        }
    }
    // A function type names its parameters, which may be incomplete; a
    // function type among them must have its name first.
    for ty in ordered(cx, steps, |kind| matches!(kind, Type::Func(_)), named) {
        let Type::Func(func) = cx.kind(ty) else {
            continue;
        };
        let params: Vec<&str> = func
            .params
            .iter()
            .filter(|&&param| !cx.empty(param))
            .map(|&param| cx.spelled(param))
            .collect();
        let params = match params.is_empty() {
            true => "void".to_owned(),
            false => params.join(", "),
        };
        // The environment is the first parameter, which is a pointer.
        let (call, env) = match (cx.go_call(func), func.has_env) {
            (true, true) => ("GOPP_CALL ", "GOPP_ENV "),
            (true, false) => ("GOPP_CALL ", ""),
            (false, _) => ("", ""),
        };
        let _ = writeln!(
            out,
            "typedef {call}{} (*{})({env}{params}); /* {} */",
            cx.spelled(func.ret),
            cx.spelled(ty),
            comment(cx, ty)
        );
    }
    let structs = ordered(cx, steps, is_struct, held);
    for &ty in &structs {
        if cx.empty(ty) {
            continue;
        }
        let name = cx.spelled(ty);
        let _ = writeln!(out, "struct {name} {{");
        match cx.kind(ty) {
            Type::Slice(of) => {
                let of = match cx.empty(*of) {
                    true => "void",
                    false => cx.spelled(*of),
                };
                for (field, part) in SLICE.fields {
                    let _ = match part {
                        Part::Bytes => writeln!(out, "  uint8_t *{field};"),
                        Part::Elems => writeln!(out, "  {of} *{field};"),
                        Part::U64 => writeln!(out, "  uint64_t {field};"),
                    };
                }
            }
            Type::Array(of, count) => {
                let _ = writeln!(out, "  {} a[{count}];", cx.spelled(*of));
            }
            Type::Option(of) => {
                if !cx.empty(*of) {
                    let _ = writeln!(out, "  {} value;", cx.spelled(*of));
                }
                out.push_str("  bool set;\n");
            }
            _ => {
                // A field that takes no room is not written, but it
                // says where what follows it may start: an array of no
                // elements starts where its elements would. The field
                // after it starts there too; after the last field, the
                // first one says it for the whole struct.
                let fields = held(cx, ty);
                let align = |field: &TypeId| cx.layout(*field).align;
                let empty_run = |fields: &[TypeId]| {
                    let run = fields.iter().rev().take_while(|&&field| cx.empty(field));
                    run.map(align).max().unwrap_or(1)
                };
                let last = empty_run(&fields);
                let mut first = true;
                for (at, &field) in fields.iter().enumerate() {
                    *steps += 1;
                    if cx.empty(field) {
                        continue;
                    }
                    let before = empty_run(&fields[..at]);
                    let wanted = match std::mem::replace(&mut first, false) {
                        true => before.max(last),
                        false => before,
                    };
                    if wanted > align(&field) {
                        let _ = write!(out, "  _Alignas({wanted})");
                    } else {
                        out.push(' ');
                    }
                    let _ = writeln!(out, " {} {};", cx.spelled(field), cx.field(ty, at));
                }
            }
        }
        out.push_str("};\n");
    }
    let crosses = crossing(cx, steps);
    for ty in structs {
        if cx.empty(ty) {
            continue;
        }
        let (name, layout) = (cx.spelled(ty), cx.layout(ty));
        let _ = writeln!(
            out,
            "_Static_assert(sizeof({name}) == {} && _Alignof({name}) == {}, \"the layout of T{}\");",
            layout.size,
            layout.align,
            ty.index()
        );
        // What crosses to C has each field where C code has it.
        if !crosses[ty.index()] || !matches!(cx.kind(ty), Type::Tuple(_) | Type::Struct(_)) {
            continue;
        }
        let fields = held(cx, ty);
        let layouts: Vec<Layout> = fields.iter().map(|&field| cx.layout(field)).collect();
        let Some((_, offsets)) = target.aggregate(&layouts) else {
            continue;
        };
        for (at, offset) in offsets.into_iter().enumerate() {
            *steps += 1;
            if !cx.empty(fields[at]) {
                let _ = writeln!(
                    out,
                    "_Static_assert(__builtin_offsetof({name}, {}) == {offset}, \"the layout of T{}\");",
                    cx.field(ty, at),
                    ty.index()
                );
            }
        }
    }
    Ok(())
}
