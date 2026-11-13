//! Static data: what a global holds when the program starts, written
//! as the initializer of its definition. Every part is a constant of C,
//! so the data is in the image and nothing runs to make it.

use std::fmt::Write;

use gopp_gir::{ConstantKind, Global, Type, TypeId, TypeInterner};

use super::number::int_literal;
use super::{Cx, EmitError, constness, global_name, linkage};

/// The definition of the global with the number `at`, which has static
/// data.
pub(super) fn global(
    cx: &Cx<'_>,
    at: usize,
    global: &Global,
    out: &mut String,
    steps: &mut u64,
) -> Result<(), EmitError> {
    // What is still to write: a value of a type, or the end of one.
    enum Piece {
        Value(TypeId),
        End(&'static str),
    }
    let _ = write!(
        out,
        "{}{}{} {} = ",
        linkage(global.link, true),
        constness(global),
        cx.spelled(global.ty),
        global_name(at)
    );
    let mut parts = global.parts.iter();
    let mut rest = vec![Piece::Value(global.ty)];
    while let Some(piece) = rest.pop() {
        *steps += 1;
        let ty = match piece {
            Piece::End(text) => {
                // The last part of a value is followed by nothing.
                out.truncate(out.trim_end_matches(", ").len());
                out.push_str(text);
                continue;
            }
            Piece::Value(ty) => ty,
        };
        // What takes no room is no field in C, and has no leaf.
        if cx.empty(ty) {
            continue;
        }
        let fields = match cx.kind(ty) {
            Type::Tuple(fields) => Some(fields.to_vec()),
            Type::Struct(id) => cx.module.types.struct_fields(*id).map(<[TypeId]>::to_vec),
            Type::Option(of) => Some(vec![*of, TypeInterner::BOOL]),
            // An array is a struct around an array of C.
            Type::Array(of, count) => {
                out.push_str("{{");
                rest.push(Piece::End("}}, "));
                rest.extend((0..*count).map(|_| Piece::Value(*of)));
                continue;
            }
            _ => None,
        };
        if let Some(fields) = fields {
            out.push('{');
            rest.push(Piece::End("}, "));
            rest.extend(fields.into_iter().rev().map(Piece::Value));
            continue;
        }
        let unsupported = EmitError::Unsupported {
            func: None,
            what: "static data of this kind",
        };
        let Some(part) = parts.next() else {
            return Err(unsupported);
        };
        let spelled = cx.spelled(ty);
        match (part.kind, cx.kind(ty)) {
            (ConstantKind::Zero, Type::String | Type::Slice(_)) => out.push_str("{}"),
            (ConstantKind::Zero, Type::Bool) => out.push_str("false"),
            (ConstantKind::Zero | ConstantKind::Null, _) => out.push('0'),
            (ConstantKind::Int(value), Type::Int { bits, signed }) => {
                out.push_str(&int_literal(value, *bits, *signed));
            }
            (ConstantKind::Bool(value), _) => out.push_str(&value.to_string()),
            (ConstantKind::Str(at), _) => {
                let text = cx.module.strings.get(at as usize);
                let len = text.map_or(0, |text| text.len());
                let _ = write!(out, "{{(uint8_t *)gopp_s{at}, {len}, 0}}");
            }
            (ConstantKind::Func(func), _) => {
                let name = cx.func_name(func.into_raw().into_u32() as usize);
                let _ = write!(out, "({spelled}){name}");
            }
            (ConstantKind::Global(named), _) => {
                let at = named.into_raw().into_u32() as usize;
                let _ = match cx.empty_global(at) {
                    true => write!(out, "({spelled})&gopp_empty"),
                    false => write!(out, "({spelled})&{}", global_name(at)),
                };
            }
            _ => return Err(unsupported),
        }
        out.push_str(", ");
    }
    out.truncate(out.trim_end_matches(", ").len());
    out.push_str(";\n");
    Ok(())
}
