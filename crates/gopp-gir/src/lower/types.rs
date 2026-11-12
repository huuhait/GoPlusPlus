//! The one place where a type of the checker becomes a type of GIR
//! (gir.md §3).

use gopp_sema::basic::Basic;
use gopp_sema::types::{Type as CheckedType, TypeId as CheckedTypeId};

use super::{Memo, Shared};
use crate::{CallConv, FuncType, Type, TypeId, TypeInterner};

impl Memo {
    /// The type of GIR that stands for `ty`, or what about it is not
    /// supported yet.
    pub fn lower_type(
        &mut self,
        shared: &Shared<'_>,
        types: &mut TypeInterner,
        ty: CheckedTypeId,
    ) -> Result<TypeId, &'static str> {
        if let Some(&known) = self.types.get(&ty) {
            return known;
        }
        self.steps += 1;
        // A struct in a struct in a struct: as deep as the program
        // declares them.
        let lowered =
            stacker::maybe_grow(64 * 1024, 1024 * 1024, || self.lower_new(shared, types, ty));
        self.types.insert(ty, lowered);
        lowered
    }

    /// The integer type of the width of a pointer.
    pub fn word(shared: &Shared<'_>, types: &mut TypeInterner, signed: bool) -> TypeId {
        types.int(shared.layout.pointer_bits(), signed)
    }

    fn bytes(types: &mut TypeInterner) -> TypeId {
        let byte = types.int(8, false);
        types.ptr(byte)
    }

    /// A struct of the runtime by its name, which has its parts when it
    /// is first asked for.
    fn runtime(types: &mut TypeInterner, name: &str, fields: &[(&str, TypeId)]) -> TypeId {
        let (ty, new) = types.struct_named(name);
        if let (true, Type::Struct(id)) = (new, types.get(ty).clone()) {
            // A struct that was just made has no fields yet.
            let _ = types.set_fields(id, fields);
        }
        ty
    }

    fn lower_new(
        &mut self,
        shared: &Shared<'_>,
        types: &mut TypeInterner,
        ty: CheckedTypeId,
    ) -> Result<TypeId, &'static str> {
        let checked = shared.types;
        if checked.is_unsafe_pointer(ty, shared.names) {
            return Ok(Self::bytes(types));
        }
        let kind = checked.get(ty);
        Ok(match &*kind {
            CheckedType::Invalid => return Err("a type that is in error"),
            CheckedType::Basic(basic) => match basic.default_type() {
                Basic::Bool => TypeInterner::BOOL,
                Basic::Int => Self::word(shared, types, true),
                Basic::Uint | Basic::Uintptr => Self::word(shared, types, false),
                Basic::Int8 => types.int(8, true),
                Basic::Int16 => types.int(16, true),
                Basic::Int32 => types.int(32, true),
                Basic::Int64 => types.int(64, true),
                Basic::Uint8 => types.int(8, false),
                Basic::Uint16 => types.int(16, false),
                Basic::Uint32 => types.int(32, false),
                Basic::Uint64 => types.int(64, false),
                Basic::Float32 => types.float(32),
                Basic::Float64 => types.float(64),
                Basic::Complex64 => {
                    let part = types.float(32);
                    types.tuple(&[part, part])
                }
                Basic::Complex128 => {
                    let part = types.float(64);
                    types.tuple(&[part, part])
                }
                Basic::String => types.intern(Type::String),
                _ => return Err("a value without a type"),
            },
            CheckedType::Pointer(to)
            | CheckedType::Unique(to)
            | CheckedType::Shared(to)
            | CheckedType::Weak(to) => {
                let to = self.lower_type(shared, types, *to)?;
                types.ptr(to)
            }
            CheckedType::Local(of) => self.lower_type(shared, types, *of)?,
            CheckedType::Option(of) => {
                let value = self.lower_type(shared, types, *of)?;
                // A pointer, an interface or a func is nil when it is
                // all zero, so the option needs no flag.
                let niche = matches!(
                    &*checked.get(checked.underlying(*of)),
                    CheckedType::Pointer(_)
                        | CheckedType::Unique(_)
                        | CheckedType::Shared(_)
                        | CheckedType::Weak(_)
                        | CheckedType::Interface { .. }
                        | CheckedType::Func { .. }
                );
                match niche {
                    true => value,
                    false => types.intern(Type::Option(value)),
                }
            }
            CheckedType::Array { len, elem } => {
                let elem = self.lower_type(shared, types, *elem)?;
                types.intern(Type::Array(elem, *len))
            }
            CheckedType::Slice(elem) => {
                let elem = self.lower_type(shared, types, *elem)?;
                types.intern(Type::Slice(elem))
            }
            CheckedType::Map { .. } => {
                let header = Self::runtime(types, "gopp.map", &[]);
                types.ptr(header)
            }
            CheckedType::Chan { .. } => {
                let header = Self::runtime(types, "gopp.chan", &[]);
                types.ptr(header)
            }
            CheckedType::Tuple(parts) => {
                let mut lowered = Vec::with_capacity(parts.len());
                for &part in parts.iter() {
                    lowered.push(self.lower_type(shared, types, part)?);
                }
                types.tuple(&lowered)
            }
            CheckedType::Func { type_params, .. } if !type_params.is_empty() => {
                return Err("a generic function as a value");
            }
            CheckedType::Func { .. } => {
                // The code, which takes the environment first, and the
                // environment.
                let bytes = Self::bytes(types);
                let plain = self.signature(shared, types, None, ty)?;
                let Type::Func(plain) = types.get(plain).clone() else {
                    return Err("a function type");
                };
                let mut params = vec![bytes];
                params.extend(plain.params.iter());
                let code = types.intern(Type::Func(FuncType {
                    params: params.into(),
                    ret: plain.ret,
                    has_env: true,
                    cc: CallConv::Go,
                }));
                types.tuple(&[code, bytes])
            }
            CheckedType::Struct(fields) if fields.is_empty() => TypeInterner::UNIT,
            CheckedType::Struct(fields) => {
                // A struct without a name is the struct of its fields,
                // as they are in GIR: two types of one name that two
                // functions declare are two types there.
                let mut parts = Vec::with_capacity(fields.len());
                for field in fields.iter() {
                    let part = self.lower_type(shared, types, field.ty)?;
                    parts.push((shared.names.resolve(&field.name), part));
                }
                let shown: Vec<String> = parts
                    .iter()
                    .map(|(name, part)| format!("{name} {}", types.display(*part)))
                    .collect();
                let (lowered, new) = types.struct_named(&format!("struct{{{}}}", shown.join("; ")));
                if let (true, Type::Struct(id)) = (new, types.get(lowered).clone()) {
                    // A struct that was just made has no fields yet.
                    let _ = types.set_fields(id, &parts);
                }
                lowered
            }
            CheckedType::Interface { .. } | CheckedType::Union(_) | CheckedType::Marker(_) => {
                let bytes = Self::bytes(types);
                Self::runtime(types, "gopp.iface", &[("itab", bytes), ("data", bytes)])
            }
            CheckedType::Named { id, args } => {
                let under = checked.underlying(ty);
                if !matches!(&*checked.get(under), CheckedType::Struct(_)) {
                    // What the type is declared as; a type that is met
                    // again on the way there holds itself.
                    if self.open.contains(&ty) {
                        return Err("a type that holds itself and is no struct");
                    }
                    self.open.push(ty);
                    let lowered = self.lower_type(shared, types, under);
                    self.open.pop();
                    return lowered;
                }
                if checked.extends(ty) {
                    return Err("a type with parents");
                }
                // The name that the package gave it, or the name that
                // it is written with elsewhere; the type arguments as
                // they are in GIR.
                let written = checked.display(ty, shared.names);
                let mut name = match shared.named.get(id) {
                    Some(name) => name.clone(),
                    None => written.split('[').next().unwrap_or(&written).to_owned(),
                };
                if !args.is_empty() {
                    let mut shown = Vec::with_capacity(args.len());
                    for &arg in args.iter() {
                        let arg = self.lower_type(shared, types, arg)?;
                        shown.push(types.display(arg));
                    }
                    name = format!("{name}[{}]", shown.join(", "));
                }
                self.strukt(shared, types, ty, under, &name)?
            }
            CheckedType::Param(_) => return Err("a type parameter"),
        })
    }

    /// The struct `ty` under the name, with the fields of `under`.
    fn strukt(
        &mut self,
        shared: &Shared<'_>,
        types: &mut TypeInterner,
        ty: CheckedTypeId,
        under: CheckedTypeId,
        name: &str,
    ) -> Result<TypeId, &'static str> {
        let (lowered, new) = types.struct_named(name);
        let Type::Struct(id) = types.get(lowered).clone() else {
            return Err("a struct");
        };
        if !new {
            return Ok(lowered);
        }
        // Known before its fields are: a field may point back to it.
        self.types.insert(ty, Ok(lowered));
        let kind = shared.types.get(under);
        let CheckedType::Struct(fields) = &*kind else {
            return Err("a struct");
        };
        let mut parts = Vec::with_capacity(fields.len());
        for field in fields.iter() {
            match self.lower_type(shared, types, field.ty) {
                Ok(part) => parts.push((shared.names.resolve(&field.name), part)),
                Err(what) => {
                    // What points to the struct already has it: it is
                    // left whole, without fields.
                    let _ = types.set_fields(id, &[]);
                    return Err(what);
                }
            }
        }
        // The fields of a struct that was just made are set once.
        let _ = types.set_fields(id, &parts);
        Ok(lowered)
    }

    /// The type of a function of GIR for the function type `sig` of the
    /// checker, after the receiver if it is a method.
    pub fn signature(
        &mut self,
        shared: &Shared<'_>,
        types: &mut TypeInterner,
        receiver: Option<CheckedTypeId>,
        sig: CheckedTypeId,
    ) -> Result<TypeId, &'static str> {
        let kind = shared.types.get(sig);
        let CheckedType::Func {
            params, results, ..
        } = &*kind
        else {
            return Err("a function type");
        };
        let mut lowered = Vec::with_capacity(params.len() + 1);
        for &param in receiver.iter().chain(params.iter()) {
            lowered.push(self.lower_type(shared, types, param)?);
        }
        let ret = match &results[..] {
            [] => TypeInterner::UNIT,
            [one] => self.lower_type(shared, types, *one)?,
            many => {
                let mut parts = Vec::with_capacity(many.len());
                for &part in many {
                    parts.push(self.lower_type(shared, types, part)?);
                }
                types.tuple(&parts)
            }
        };
        Ok(types.intern(Type::Func(FuncType {
            params: lowered.into(),
            ret,
            has_env: false,
            cc: CallConv::Go,
        })))
    }
}
