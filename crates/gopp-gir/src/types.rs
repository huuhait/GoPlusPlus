//! GIR types: of a fixed width on every target, interned so that one
//! type is one `TypeId`, with names only on structs and on the shells of
//! types that hold themselves (gir.md §3).

use gopp_target::{Layout, TargetLayout};
use rustc_hash::{FxHashMap, FxHashSet};

use crate::ir::CallConv;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct TypeId(u32);

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct StructId(u32);

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct RecId(u32);

#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub enum Type {
    /// The empty tuple: no result, `struct{}`.
    Unit,
    Bool,
    Int {
        bits: u8,
        signed: bool,
    },
    Float {
        bits: u8,
    },
    /// `{ptr *u8, len u64, backing *u8}`.
    String,
    Ptr(TypeId),
    /// `{ptr *T, len u64, cap u64, backing *u8}`.
    Slice(TypeId),
    Array(TypeId, u64),
    /// An anonymous struct, laid out as C lays one out.
    Tuple(Box<[TypeId]>),
    /// The value, then whether it is set.
    Option(TypeId),
    /// A struct with a name; its fields are said once.
    Struct(StructId),
    /// The shell of a type that is no struct and holds itself.
    Rec(RecId),
    Func(FuncType),
}

/// The type of the code of a function.
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct FuncType {
    pub params: Box<[TypeId]>,
    pub ret: TypeId,
    /// The code takes what a closure captured.
    pub has_env: bool,
    pub cc: CallConv,
}

#[derive(Clone, Debug)]
struct StructDef {
    name: String,
    /// `None` until the fields are said.
    fields: Option<Box<[TypeId]>>,
    /// The names of the fields, for the C that is written and for the
    /// debugger; no part of what the type is.
    field_names: Vec<String>,
}

#[derive(Clone, Debug)]
struct RecDef {
    name: String,
    body: Option<TypeId>,
}

/// What is wrong with a type that is asked for.
#[derive(Clone, Debug, PartialEq, Eq, thiserror::Error)]
pub enum TypeError {
    #[error("the struct {0} has fields already")]
    FieldsSetTwice(String),
    #[error("the type {0} has no fields yet")]
    NoFields(String),
    #[error("a value of the type {0} would be too large")]
    TooLarge(String),
    #[error("the type {0} holds itself by value")]
    HoldsItself(String),
}

/// All types of a module.
#[derive(Clone, Debug)]
pub struct TypeInterner {
    types: Vec<Type>,
    ids: FxHashMap<Type, TypeId>,
    structs: Vec<StructDef>,
    recs: Vec<RecDef>,
    /// The shell that stands for each shape of a type that holds itself.
    rec_shapes: FxHashMap<Vec<u64>, TypeId>,
    /// The structs that were asked for by their names.
    struct_names: FxHashMap<String, TypeId>,
}

impl Default for TypeInterner {
    fn default() -> Self {
        Self::new()
    }
}

impl TypeInterner {
    pub const UNIT: TypeId = TypeId(0);
    pub const BOOL: TypeId = TypeId(1);

    pub fn new() -> Self {
        let mut interner = Self {
            types: Vec::new(),
            ids: FxHashMap::default(),
            structs: Vec::new(),
            recs: Vec::new(),
            rec_shapes: FxHashMap::default(),
            struct_names: FxHashMap::default(),
        };
        interner.intern(Type::Unit);
        interner.intern(Type::Bool);
        interner
    }

    pub fn intern(&mut self, ty: Type) -> TypeId {
        if let Some(&id) = self.ids.get(&ty) {
            return id;
        }
        let id = TypeId(self.types.len() as u32);
        self.types.push(ty.clone());
        self.ids.insert(ty, id);
        id
    }

    /// Every type, in the order in which they were made.
    pub fn ids(&self) -> impl Iterator<Item = TypeId> + use<> {
        (0..self.types.len() as u32).map(TypeId)
    }

    pub fn get(&self, id: TypeId) -> &Type {
        &self.types[id.0 as usize]
    }

    pub fn int(&mut self, bits: u8, signed: bool) -> TypeId {
        self.intern(Type::Int { bits, signed })
    }

    pub fn float(&mut self, bits: u8) -> TypeId {
        self.intern(Type::Float { bits })
    }

    pub fn ptr(&mut self, to: TypeId) -> TypeId {
        self.intern(Type::Ptr(to))
    }

    pub fn tuple(&mut self, fields: &[TypeId]) -> TypeId {
        match fields {
            [] => Self::UNIT,
            fields => self.intern(Type::Tuple(fields.into())),
        }
    }

    /// A new struct of the name, whose fields are said with
    /// `set_fields`: its fields may name it.
    pub fn declare_struct(&mut self, name: &str) -> TypeId {
        let id = StructId(self.structs.len() as u32);
        self.structs.push(StructDef {
            name: name.to_owned(),
            fields: None,
            field_names: Vec::new(),
        });
        self.intern(Type::Struct(id))
    }

    /// The struct that was asked for by this name before, or a new one,
    /// and whether it is new: its fields are still to be said.
    pub fn struct_named(&mut self, name: &str) -> (TypeId, bool) {
        if let Some(&known) = self.struct_names.get(name) {
            return (known, false);
        }
        let new = self.declare_struct(name);
        self.struct_names.insert(name.to_owned(), new);
        (new, true)
    }

    /// Says the fields of a struct, each with its name, once.
    pub fn set_fields(&mut self, id: StructId, fields: &[(&str, TypeId)]) -> Result<(), TypeError> {
        let def = &mut self.structs[id.0 as usize];
        if def.fields.is_some() {
            return Err(TypeError::FieldsSetTwice(def.name.clone()));
        }
        def.fields = Some(fields.iter().map(|&(_, ty)| ty).collect());
        def.field_names = fields.iter().map(|&(name, _)| name.to_owned()).collect();
        Ok(())
    }

    pub fn struct_name(&self, id: StructId) -> &str {
        &self.structs[id.0 as usize].name
    }

    pub fn struct_fields(&self, id: StructId) -> Option<&[TypeId]> {
        self.structs[id.0 as usize].fields.as_deref()
    }

    pub fn field_names(&self, id: StructId) -> &[String] {
        &self.structs[id.0 as usize].field_names
    }

    pub fn rec_name(&self, id: RecId) -> &str {
        &self.recs[id.0 as usize].name
    }

    pub fn rec_body(&self, id: RecId) -> Option<TypeId> {
        self.recs[id.0 as usize].body
    }

    /// A type that is no struct and holds itself, as `type S []S`: `body`
    /// makes what it is from the shell that stands for it. Two such
    /// types of one shape are one type, with the name of the first.
    pub fn rec(&mut self, name: &str, body: impl FnOnce(&mut Self, TypeId) -> TypeId) -> TypeId {
        let id = RecId(self.recs.len() as u32);
        self.recs.push(RecDef {
            name: name.to_owned(),
            body: None,
        });
        let shell = self.intern(Type::Rec(id));
        let made = body(self, shell);
        let shape = self.shape(made, id);
        if let Some(&known) = self.rec_shapes.get(&shape) {
            return known;
        }
        self.recs[id.0 as usize].body = Some(made);
        self.rec_shapes.insert(shape, shell);
        shell
    }

    /// What the type `body` is made of, with `shell` named as itself:
    /// the same for two shells of one shape.
    fn shape(&self, body: TypeId, shell: RecId) -> Vec<u64> {
        const ITSELF: u64 = u64::MAX;
        let mut shape = Vec::new();
        let mut rest = vec![body];
        while let Some(ty) = rest.pop() {
            match self.get(ty) {
                Type::Rec(rec) if *rec == shell => shape.push(ITSELF),
                // What has a name is itself.
                Type::Unit
                | Type::Bool
                | Type::Int { .. }
                | Type::Float { .. }
                | Type::String
                | Type::Struct(_)
                | Type::Rec(_) => shape.extend([0, u64::from(ty.0)]),
                Type::Ptr(to) => {
                    shape.push(1);
                    rest.push(*to);
                }
                Type::Slice(of) => {
                    shape.push(2);
                    rest.push(*of);
                }
                Type::Array(of, count) => {
                    shape.extend([3, *count]);
                    rest.push(*of);
                }
                Type::Option(of) => {
                    shape.push(4);
                    rest.push(*of);
                }
                Type::Tuple(fields) => {
                    shape.extend([5, fields.len() as u64]);
                    rest.extend(fields.iter().rev());
                }
                Type::Func(func) => {
                    let kind = 6 + u64::from(func.has_env) + 2 * (func.cc as u64);
                    shape.extend([kind, func.params.len() as u64]);
                    rest.push(func.ret);
                    rest.extend(func.params.iter().rev());
                }
            }
        }
        shape
    }

    /// The types of the fields of a value of the type, for a type that
    /// has fields: a string, a slice, a tuple, an option, a struct.
    pub fn fields(&mut self, ty: TypeId) -> Option<Vec<TypeId>> {
        let node = self.get(ty).clone();
        let byte = |this: &mut Self| {
            let byte = this.int(8, false);
            this.ptr(byte)
        };
        Some(match node {
            Type::String => {
                let (bytes, len) = (byte(self), self.int(64, false));
                vec![bytes, len, bytes]
            }
            Type::Slice(of) => {
                let (elems, len, buffer) = (self.ptr(of), self.int(64, false), byte(self));
                vec![elems, len, len, buffer]
            }
            Type::Tuple(fields) => fields.to_vec(),
            Type::Option(of) => vec![of, Self::BOOL],
            Type::Struct(id) => self.struct_fields(id)?.to_vec(),
            _ => return None,
        })
    }

    /// The size and the alignment of a value of the type on a target.
    /// The walk keeps what it has to do in a list: a type may be nested
    /// deeper than a stack takes.
    pub fn layout(&self, ty: TypeId, target: TargetLayout) -> Result<Layout, TypeError> {
        let mut known: FxHashMap<TypeId, Layout> = FxHashMap::default();
        // A type is taken twice: to ask for its parts, and with them.
        let mut rest = vec![(ty, false)];
        let mut inside: FxHashSet<TypeId> = FxHashSet::default();
        while let Some((next, with_parts)) = rest.pop() {
            if known.contains_key(&next) {
                continue;
            }
            let parts: Vec<TypeId> = match self.get(next) {
                Type::Array(of, _) | Type::Option(of) => vec![*of],
                Type::Tuple(fields) => fields.to_vec(),
                Type::Struct(id) => match self.struct_fields(*id) {
                    Some(fields) => fields.to_vec(),
                    None => return Err(TypeError::NoFields(self.display(next))),
                },
                Type::Rec(id) => match self.rec_body(*id) {
                    Some(body) => vec![body],
                    None => return Err(TypeError::NoFields(self.display(next))),
                },
                _ => Vec::new(),
            };
            if !with_parts && !parts.is_empty() {
                // A value that holds a value of its own type has no size.
                if !inside.insert(next) {
                    return Err(TypeError::HoldsItself(self.display(next)));
                }
                rest.push((next, true));
                rest.extend(parts.iter().map(|&part| (part, false)));
                continue;
            }
            inside.remove(&next);
            let of = |part: &TypeId| known.get(part).copied();
            let layout = match self.get(next) {
                Type::Unit => Some(Layout::UNIT),
                Type::Bool => Some(target.bool()),
                Type::Int { bits, .. } | Type::Float { bits } => Some(target.scalar(*bits)),
                Type::String => Some(target.string()),
                Type::Slice(_) => Some(target.slice()),
                Type::Ptr(_) | Type::Func(_) => Some(target.pointer()),
                Type::Array(elem, count) => of(elem).and_then(|elem| elem.array(*count)),
                Type::Option(payload) => of(payload).and_then(|payload| target.option(payload)),
                Type::Rec(_) => parts.first().and_then(of),
                Type::Tuple(_) | Type::Struct(_) => {
                    let fields: Option<Vec<Layout>> = parts.iter().map(of).collect();
                    fields
                        .and_then(|fields| target.aggregate(&fields))
                        .map(|(whole, _)| whole)
                }
            };
            let Some(layout) = layout else {
                return Err(TypeError::TooLarge(self.display(next)));
            };
            known.insert(next, layout);
        }
        known
            .get(&ty)
            .copied()
            .ok_or_else(|| TypeError::TooLarge(self.display(ty)))
    }

    /// The type as the text of a module writes it.
    pub fn display(&self, ty: TypeId) -> String {
        let mut out = String::new();
        self.write(ty, &mut out);
        out
    }

    fn write(&self, ty: TypeId, out: &mut String) {
        // What is still to write: a type, or a piece of text.
        enum Piece {
            Type(TypeId),
            Text(&'static str),
        }
        let mut rest = vec![Piece::Type(ty)];
        while let Some(piece) = rest.pop() {
            let ty = match piece {
                Piece::Text(text) => {
                    out.push_str(text);
                    continue;
                }
                Piece::Type(ty) => ty,
            };
            let list = |rest: &mut Vec<Piece>, types: &[TypeId], close: &'static str| {
                rest.push(Piece::Text(close));
                for (at, &ty) in types.iter().enumerate().rev() {
                    rest.push(Piece::Type(ty));
                    if at > 0 {
                        rest.push(Piece::Text(", "));
                    }
                }
            };
            match self.get(ty) {
                Type::Unit => out.push_str("()"),
                Type::Bool => out.push_str("bool"),
                Type::Int { bits, signed } => {
                    out.push(if *signed { 'i' } else { 'u' });
                    out.push_str(&bits.to_string());
                }
                Type::Float { bits } => {
                    out.push('f');
                    out.push_str(&bits.to_string());
                }
                Type::String => out.push_str("string"),
                Type::Ptr(to) => {
                    out.push('*');
                    rest.push(Piece::Type(*to));
                }
                Type::Slice(of) => {
                    out.push_str("[]");
                    rest.push(Piece::Type(*of));
                }
                Type::Array(of, count) => {
                    out.push_str(&format!("[{count}]"));
                    rest.push(Piece::Type(*of));
                }
                Type::Option(of) => {
                    out.push('?');
                    rest.push(Piece::Type(*of));
                }
                Type::Tuple(fields) => {
                    out.push('(');
                    list(&mut rest, fields, ")");
                }
                Type::Struct(id) => out.push_str(self.struct_name(*id)),
                Type::Rec(id) => out.push_str(self.rec_name(*id)),
                Type::Func(func) => {
                    out.push_str(match (func.cc, func.has_env) {
                        (CallConv::C, _) => "fn \"C\"(",
                        (CallConv::Go, true) => "fn env(",
                        (CallConv::Go, false) => "fn(",
                    });
                    rest.push(Piece::Type(func.ret));
                    list(&mut rest, &func.params, ") -> ");
                }
            }
        }
    }
}
