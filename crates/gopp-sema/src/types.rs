//! Types and the relations between them.
//!
//! Every type is interned: building the same type twice gives the same
//! [`TypeId`], so two ids that are equal are identical types. The reverse
//! holds for all types but interfaces, which may be written in several ways
//! (`interface{ io.Reader }` and `interface{ Read(*[]byte) (int, error) }`);
//! [`Types::identical`] decides those.
//!
//! A function that can fail (`func!(int) T`) has no type of its own: it is
//! `func(int) (T, error)`.

use std::cell::{Cell, RefCell};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, PoisonError, RwLock};

use lasso::{Spur, ThreadedRodeo};
use rustc_hash::{FxHashMap, FxHashSet};

use crate::basic::{Basic, representable};
use crate::constant::Value;

/// A type. Two are equal if they are identical types: how the program
/// wrote a type, through which aliases, is kept beside it for the text of
/// a message and is no part of a comparison.
#[derive(Clone, Copy, Debug)]
pub struct TypeId {
    /// The type without aliases.
    canon: u32,
    /// The type as it was written: a type whose parts are as they were
    /// written, or an alias, which has the bit `ALIAS`.
    written: u32,
}

const ALIAS: u32 = 1 << 31;

impl TypeId {
    const fn plain(index: u32) -> Self {
        TypeId {
            canon: index,
            written: index,
        }
    }

    /// The type without the aliases it was written with.
    fn canonical(self) -> Self {
        Self::plain(self.canon)
    }

    /// The type and how it was written.
    fn spelled(self) -> (u32, u32) {
        (self.canon, self.written)
    }
}

impl PartialEq for TypeId {
    fn eq(&self, other: &Self) -> bool {
        self.canon == other.canon
    }
}

impl Eq for TypeId {}

impl std::hash::Hash for TypeId {
    fn hash<H: std::hash::Hasher>(&self, state: &mut H) {
        self.canon.hash(state);
    }
}

impl PartialOrd for TypeId {
    fn partial_cmp(&self, other: &Self) -> Option<std::cmp::Ordering> {
        Some(self.cmp(other))
    }
}

impl Ord for TypeId {
    fn cmp(&self, other: &Self) -> std::cmp::Ordering {
        self.canon.cmp(&other.canon)
    }
}

/// A declared type: an index into the table of the interner.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct NamedId(u32);

/// A type parameter. Two parameters are never the same type, whatever
/// their names.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct ParamId(u32);

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum ChanDir {
    Both,
    Send,
    Recv,
}

/// A constraint that the compiler knows and no program can write out.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Marker {
    Comparable,
    Copy,
    Clone,
    Send,
    Sync,
}

#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct Field {
    pub name: Spur,
    /// The path of the declaring package for a name that is not exported:
    /// such names of two packages are different names.
    pub package: Option<Spur>,
    pub ty: TypeId,
    pub embedded: bool,
    pub tag: Option<Box<[u8]>>,
    /// `@Required`: must be set in every literal.
    pub required: bool,
}

#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct Method {
    pub name: Spur,
    /// As for [`Field::package`].
    pub package: Option<Spur>,
    /// A [`Type::Func`].
    pub sig: TypeId,
}

/// One term of a union in a constraint: `T` or `~T`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Term {
    pub tilde: bool,
    pub ty: TypeId,
}

#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub enum Type {
    /// The type of what is in error. It fits everywhere, so that one
    /// mistake is reported once.
    Invalid,
    Basic(Basic),
    Pointer(TypeId),
    Unique(TypeId),
    Shared(TypeId),
    Weak(TypeId),
    Option(TypeId),
    /// `Local[T]`: a func or an interface that stays on one goroutine.
    Local(TypeId),
    Array {
        len: u64,
        elem: TypeId,
    },
    Slice(TypeId),
    Map {
        key: TypeId,
        value: TypeId,
    },
    Chan {
        dir: ChanDir,
        elem: TypeId,
    },
    /// Several values at once: the results of a call.
    Tuple(Box<[TypeId]>),
    Func {
        /// The type parameters of a generic function.
        type_params: Box<[TypeId]>,
        params: Box<[TypeId]>,
        results: Box<[TypeId]>,
        /// The last parameter is `...T`, held as `[]T`.
        variadic: bool,
    },
    Struct(Box<[Field]>),
    Interface {
        /// The methods written in the interface, in the order of their
        /// names.
        methods: Box<[Method]>,
        /// The embedded interfaces and the type terms of a constraint.
        embedded: Box<[TypeId]>,
    },
    Union(Box<[Term]>),
    Marker(Marker),
    /// A declared type, with its type arguments if it is generic.
    Named {
        id: NamedId,
        args: Box<[TypeId]>,
    },
    Param(ParamId),
}

impl Type {
    /// The types that the type is written with, each once for each place:
    /// the type arguments of a declared type, not what it is declared as.
    fn parts(&self) -> Vec<TypeId> {
        let mut parts = Vec::new();
        self.each_part(&mut |part| parts.push(part));
        parts
    }

    pub fn each_part(&self, each: &mut impl FnMut(TypeId)) {
        match self {
            Type::Param(_) | Type::Invalid | Type::Basic(_) | Type::Marker(_) => {}
            Type::Pointer(elem)
            | Type::Unique(elem)
            | Type::Shared(elem)
            | Type::Weak(elem)
            | Type::Option(elem)
            | Type::Local(elem)
            | Type::Slice(elem)
            | Type::Array { elem, .. }
            | Type::Chan { elem, .. } => each(*elem),
            Type::Map { key, value } => {
                each(*key);
                each(*value);
            }
            Type::Tuple(types) | Type::Named { args: types, .. } => {
                types.iter().copied().for_each(each);
            }
            Type::Func {
                params, results, ..
            } => params.iter().chain(results.iter()).copied().for_each(each),
            Type::Struct(fields) => fields.iter().for_each(|field| each(field.ty)),
            Type::Interface { methods, embedded } => {
                methods.iter().for_each(|method| each(method.sig));
                embedded.iter().copied().for_each(each);
            }
            Type::Union(terms) => terms.iter().for_each(|term| each(term.ty)),
        }
    }

    /// The type with each of its parts replaced by what `part` makes of
    /// it.
    fn map_parts(&self, part: &mut impl FnMut(TypeId) -> TypeId) -> Type {
        let mut list =
            |types: &[TypeId]| -> Box<[TypeId]> { types.iter().map(|&ty| part(ty)).collect() };
        match self {
            Type::Param(_) | Type::Invalid | Type::Basic(_) | Type::Marker(_) => self.clone(),
            Type::Pointer(elem) => Type::Pointer(part(*elem)),
            Type::Unique(elem) => Type::Unique(part(*elem)),
            Type::Shared(elem) => Type::Shared(part(*elem)),
            Type::Weak(elem) => Type::Weak(part(*elem)),
            Type::Option(elem) => Type::Option(part(*elem)),
            Type::Local(elem) => Type::Local(part(*elem)),
            Type::Slice(elem) => Type::Slice(part(*elem)),
            Type::Array { len, elem } => Type::Array {
                len: *len,
                elem: part(*elem),
            },
            Type::Map { key, value } => {
                let key = part(*key);
                Type::Map {
                    key,
                    value: part(*value),
                }
            }
            Type::Chan { dir, elem } => Type::Chan {
                dir: *dir,
                elem: part(*elem),
            },
            Type::Tuple(types) => Type::Tuple(list(types)),
            Type::Func {
                type_params,
                params,
                results,
                variadic,
            } => {
                let params = list(params);
                Type::Func {
                    type_params: type_params.clone(),
                    params,
                    results: list(results),
                    variadic: *variadic,
                }
            }
            Type::Struct(fields) => {
                let field = |field: &Field| Field {
                    ty: part(field.ty),
                    ..field.clone()
                };
                Type::Struct(fields.iter().map(field).collect())
            }
            Type::Interface { methods, embedded } => {
                let methods = methods
                    .iter()
                    .map(|method| Method {
                        sig: part(method.sig),
                        ..method.clone()
                    })
                    .collect();
                Type::Interface {
                    methods,
                    embedded: embedded.iter().map(|&ty| part(ty)).collect(),
                }
            }
            Type::Union(terms) => {
                let term = |term: &Term| Term {
                    ty: part(term.ty),
                    ..*term
                };
                Type::Union(terms.iter().map(term).collect())
            }
            Type::Named { id, args } => Type::Named {
                id: *id,
                args: list(args),
            },
        }
    }
}

/// A method of a declared type.
#[derive(Clone, Debug)]
pub struct DeclaredMethod {
    pub method: Method,
    /// The built-in attributes on the declaration.
    pub marks: Marks,
    /// Whether the receiver is a pointer.
    pub pointer_receiver: bool,
    /// The type parameters that the receiver names, `P` in
    /// `func (l *List[P]) Len() int`: the signature is written with them.
    pub receiver_params: Vec<TypeId>,
}

/// Says whether a type meets a method that is wanted in a way of its
/// own, given what says whether two method types are the same; `None`
/// if the methods of the type decide.
pub type Met<'m> = dyn FnMut(&Method, &mut dyn FnMut(TypeId, TypeId) -> bool) -> Option<bool> + 'm;

/// An `@Operator` method as a value of one type has it.
#[derive(Clone, Debug)]
pub struct Operator {
    pub name: Spur,
    /// The type of the method, written with the type arguments of the
    /// type that has it.
    pub sig: TypeId,
    pub marks: Marks,
    pub pointer_receiver: bool,
    /// The type that declares the method: the type itself or an
    /// ancestor.
    pub holder: TypeId,
    /// Which of the `@Operator` methods of the holder it is, in the order
    /// of their declarations.
    pub at: u32,
}

/// The built-in attributes that a method is declared with.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Marks(u16);

impl Marks {
    pub const VIRTUAL: Self = Self(1);
    pub const ABSTRACT: Self = Self(1 << 1);
    pub const OVERRIDE: Self = Self(1 << 2);
    pub const OPERATOR: Self = Self(1 << 3);
    pub const RIGHT: Self = Self(1 << 4);
    pub const CONVERT: Self = Self(1 << 5);
    pub const CONST: Self = Self(1 << 6);
    pub const INLINE: Self = Self(1 << 7);
    pub const NO_INLINE: Self = Self(1 << 8);
    pub const NO_BOUNDS: Self = Self(1 << 9);
    /// `@Extern("C")`: the function is defined in C.
    pub const EXTERN: Self = Self(1 << 10);

    /// The mark of the attribute `@name`, if a method may have it.
    pub fn of(name: &str) -> Option<Self> {
        Some(match name {
            "Virtual" => Self::VIRTUAL,
            "Abstract" => Self::ABSTRACT,
            "Override" => Self::OVERRIDE,
            "Operator" => Self::OPERATOR,
            "Right" => Self::RIGHT,
            "Convert" => Self::CONVERT,
            "Const" => Self::CONST,
            "Inline" => Self::INLINE,
            "NoInline" => Self::NO_INLINE,
            "NoBounds" => Self::NO_BOUNDS,
            "Extern" => Self::EXTERN,
            _ => return None,
        })
    }

    pub fn has(self, mark: Self) -> bool {
        self.0 & mark.0 != 0
    }

    pub fn with(self, mark: Self) -> Self {
        Self(self.0 | mark.0)
    }
}

/// What a selector reaches, with the name it has in the type.
#[derive(Clone, Debug)]
pub struct Found {
    pub member: Member,
    pub name: Spur,
    /// The name of the package of the declared type that has the member;
    /// `None` for a member of a type that is not declared.
    pub owner: Option<Spur>,
    /// The type that has the member: the one that was asked, what it
    /// points to, or a type embedded in it.
    pub holder: TypeId,
    /// The index of each embedded field on the way to the holder and,
    /// for a field, its own index last.
    pub path: Vec<u32>,
    /// Whether a pointer is followed on the way.
    pub indirect: bool,
}

/// What a type lacks of an interface that it does not implement.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Lacks {
    Method,
    /// It has this method, whose name differs in its capitals.
    Name(Method),
    /// Its method of the name belongs to another package.
    Exported,
    /// Its method of the name, which has another type.
    Type(Method),
    /// Two members of the name at one depth.
    OneMember,
    /// Its method needs the address of its receiver.
    ValueReceiver,
    /// Its method has type parameters of its own, which no method of an
    /// interface has.
    Generic,
    /// The name is a field.
    NoField,
}

/// What makes an interface a constraint. Of two, the later one says
/// more.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum Constraint {
    /// It embeds `Copy`, `Clone`, `Send` or `Sync`.
    Marker,
    /// It embeds `comparable`.
    Comparable,
    /// It has type terms.
    Terms,
}

/// Why a selector reaches nothing.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NotFound {
    Missing,
    /// Two members of the name at one depth.
    Ambiguous,
}

/// What a selector `x.name` reaches in the type of `x`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Member {
    Field {
        ty: TypeId,
        /// Reached through a pointer: the field is a variable whatever
        /// the value it was selected from is.
        indirect: bool,
    },
    Method {
        /// A [`Type::Func`], without the receiver.
        sig: TypeId,
        /// The method needs the address of its receiver, and the way to it
        /// went through no pointer.
        pointer_receiver: bool,
    },
}

/// Where a declared type is declared.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Home {
    pub name: Spur,
    /// The name of the declaring package; `None` for a predeclared type.
    pub package: Option<Spur>,
    /// Its import path; `None` for a predeclared type, and for one whose
    /// package has no path.
    pub path: Option<Spur>,
    /// Declared inside a function.
    pub local: bool,
    /// It has type parameters.
    pub generic: bool,
    /// Its declaration is in error.
    pub invalid: bool,
}

#[derive(Clone, Debug)]
struct Named {
    name: Spur,
    /// The name of the declaring package; `None` for a predeclared type.
    package: Option<Spur>,
    /// The import path of the declaring package, once it is said.
    path: Option<Spur>,
    /// Declared inside a function.
    local: bool,
    /// Known once it was asked for.
    underlying: Option<TypeId>,
    /// The type that the declaration wrote, which may be a declared type.
    /// Set once the declaration is checked.
    declared_as: Option<TypeId>,
    /// The declaration is in error.
    invalid: bool,
    /// The type parameters of a generic type.
    params: Vec<TypeId>,
    methods: Vec<DeclaredMethod>,
    /// Where each method is in `methods`, by its name and its package.
    method_index: FxHashMap<(Spur, Option<Spur>), u32>,
    /// Every `@Operator` method, in the order of the declarations: those
    /// of one name are told apart by their parameters.
    operators: Vec<DeclaredMethod>,
    /// Declared with `@Extends`: it has parents.
    extends: bool,
    /// The parents, in the order of the `@Extends` attributes.
    parents: Vec<TypeId>,
    /// Declared with `@Abstract`.
    abstract_marked: bool,
    /// On an instantiation cycle: its instances have no end, and no walk
    /// follows them.
    expanding: bool,
    /// Declared with `@Pinned`: a value stays where it is and is not
    /// copied.
    pinned: bool,
    /// Has a `drop` method, which ends a value of the type.
    drops: bool,
    /// For an alias: the type that it stands for, behind the aliases
    /// that it names in turn, written with its type parameters if it is
    /// generic.
    alias: Option<TypeId>,
}

#[derive(Debug, Default)]
struct Inner {
    types: Vec<Arc<Type>>,
    /// For each type of `types`, the type without aliases in its parts.
    canon: Vec<u32>,
    /// By the type without aliases, the ways it is written.
    ids: FxHashMap<Arc<Type>, Spellings>,
    aliases: Vec<Alias>,
    alias_ids: FxHashMap<AliasKey, u32>,
    /// For each type of `types`, whether a type parameter is written in
    /// it.
    has_param: Vec<bool>,
    named: Vec<Named>,
    /// The name of each type parameter, and its constraint once known.
    params: Vec<(Spur, Option<TypeId>)>,
    /// For a type parameter that stands for another in one place, the
    /// one that was declared.
    param_origins: FxHashMap<TypeId, TypeId>,
    /// The type parameters that a declaration writes: a method of their
    /// constraint may be met as an operator element (GEN-13).
    element_params: FxHashSet<TypeId>,
    /// By the first type parameter that a method with type parameters of
    /// its own got for one receiver: what the receiver names its type
    /// parameters, and the type arguments of that receiver.
    method_receivers: FxHashMap<TypeId, (Vec<TypeId>, Vec<TypeId>)>,
    /// The underlying type of each generic type with arguments that was
    /// asked for.
    instances: FxHashMap<TypeId, TypeId>,
    /// The type set of each interface type that was asked for it.
    type_sets: FxHashMap<TypeId, TypeSet>,
    /// How a value of each type that was asked for it is copied; with
    /// `true`, whether it holds what is ended with it.
    categories: FxHashMap<(TypeId, bool), Category>,
    /// For a type that was asked: whether it has a type table, and
    /// whether it is abstract.
    tables: FxHashMap<TypeId, bool>,
    abstracts: FxHashMap<TypeId, bool>,
    /// What a value of each type that was asked holds where it is, and
    /// into what a declaration puts a value of a type parameter on the
    /// heap.
    holding: FxHashMap<TypeId, Held>,
    leading: FxHashMap<TypeId, bool>,
    lending: FxHashMap<TypeId, bool>,
    kept: FxHashMap<TypeId, Arc<str>>,
    /// What each method of a declared type does with the pointers that
    /// it is given.
    lends: FxHashMap<(NamedId, Spur), Arc<Lends>>,
    open_abstracts: FxHashMap<TypeId, Arc<Vec<(TypeId, Method)>>>,
    /// For a type that was asked: whether it is `Send` and `Sync`.
    labels: FxHashMap<TypeId, Labels>,
    /// The type parameters that a declaration writes `Option[P]` for.
    optional: FxHashSet<TypeId>,
    /// The type parameters that a declaration holds a value of.
    held: FxHashSet<TypeId>,
    /// The type parameters that a declaration has a `@Required` field
    /// of.
    required: FxHashSet<TypeId>,
    /// For a type that was asked: the part of it without a zero value.
    no_zero: FxHashMap<TypeId, Option<(TypeId, NoZero)>>,
    /// For a type that was asked, what keeps it from being the type of
    /// a constant.
    not_constant: FxHashMap<TypeId, Option<(TypeId, NotConstant)>>,
}

/// A declared alias and how its type arguments are written.
type AliasKey = (NamedId, Box<[(u32, u32)]>);

/// One type as it is written without aliases, and as it is written with
/// them in its parts.
#[derive(Clone, Debug)]
struct Spellings {
    canonical: u32,
    written: Vec<u32>,
}

/// An alias as a program names it: `A`, or `A[int]` for a generic one.
#[derive(Clone, Debug)]
struct Alias {
    named: NamedId,
    args: Box<[TypeId]>,
    /// The type that the alias stands for, behind the aliases that it
    /// names in turn, as it is written.
    target: TypeId,
}

impl Inner {
    /// The type `ty` if it was made before, as it is written.
    fn find(&self, ty: &Type) -> Option<TypeId> {
        let spellings = self.ids.get(ty)?;
        let mut aliased = false;
        ty.each_part(&mut |part| aliased |= part.written != part.canon);
        if !aliased {
            return Some(TypeId::plain(spellings.canonical));
        }
        let parts = ty.parts();
        let same = |&&written: &&u32| {
            let other = self.types[written as usize].parts();
            other
                .iter()
                .zip(&parts)
                .all(|(a, b)| a.written == b.written)
        };
        spellings.written.iter().find(same).map(|&written| TypeId {
            canon: spellings.canonical,
            written,
        })
    }

    fn intern(&mut self, ty: Type) -> TypeId {
        if let Some(id) = self.find(&ty) {
            return id;
        }
        let mut aliased = false;
        ty.each_part(&mut |part| aliased |= part.written != part.canon);
        let canon = match aliased {
            true => Some(self.intern(ty.map_parts(&mut TypeId::canonical)).canon),
            false => None,
        };
        let index = self.types.len() as u32;
        let mut has_param = matches!(ty, Type::Param(_));
        ty.each_part(&mut |part| has_param |= self.has_param[part.canon as usize]);
        self.has_param.push(has_param);
        self.canon.push(canon.unwrap_or(index));
        let ty = Arc::new(ty);
        self.types.push(Arc::clone(&ty));
        match canon {
            Some(canon) => {
                // The type without aliases was made above.
                if let Some(spellings) = self.ids.get_mut(&ty) {
                    spellings.written.push(index);
                }
                TypeId {
                    canon,
                    written: index,
                }
            }
            None => {
                let spellings = Spellings {
                    canonical: index,
                    written: Vec::new(),
                };
                self.ids.insert(ty, spellings);
                TypeId::plain(index)
            }
        }
    }

    /// The alias `named` with the type arguments `args`, if it was made.
    fn find_alias(&self, named: NamedId, args: &[TypeId]) -> Option<TypeId> {
        let key = (named, args.iter().map(|arg| arg.spelled()).collect());
        let &index = self.alias_ids.get(&key)?;
        Some(TypeId {
            canon: self.aliases[index as usize].target.canon,
            written: ALIAS | index,
        })
    }

    /// The alias `named` with the type arguments `args`, which stands
    /// for `target`, a type that is not written as an alias.
    fn alias(&mut self, named: NamedId, args: &[TypeId], target: TypeId) -> TypeId {
        let key = (named, args.iter().map(|arg| arg.spelled()).collect());
        let next = self.aliases.len() as u32;
        let index = *self.alias_ids.entry(key).or_insert(next);
        if index == next {
            self.aliases.push(Alias {
                named,
                args: args.into(),
                target,
            });
        }
        TypeId {
            canon: self.aliases[index as usize].target.canon,
            written: ALIAS | index,
        }
    }

    /// The alias that the type is written as.
    fn alias_of(&self, id: TypeId) -> Option<&Alias> {
        (id.written & ALIAS != 0).then(|| &self.aliases[(id.written ^ ALIAS) as usize])
    }

    /// The type as it is written behind the aliases that name it.
    fn unalias(&self, id: TypeId) -> TypeId {
        self.alias_of(id).map_or(id, |alias| alias.target)
    }
}

/// Runs a step of a walk over a type on a stack that grows on demand: a
/// type may be nested, or hold declared types that hold others, deeper
/// than a stack of a fixed size takes.
fn deep<R>(step: impl FnOnce() -> R) -> R {
    gopp_syntax::with_stack(step)
}

/// How a type is written out.
#[derive(Clone, Copy)]
struct Spelling<'a> {
    /// One spelling for all identical types.
    canonical: bool,
    /// As the runtime of Go names a type in its messages: `interface {}`,
    /// `struct { x int }`.
    spaced: bool,
    /// The name of the package whose types are written without it.
    within: Option<Spur>,
    /// How many more types are written out before the rest is left out.
    left: &'a Cell<u32>,
}

/// The types that the text of one type names at most. A type that calls
/// of generic functions build may hold another one twice, which holds a
/// third one twice, and so on: written out it is as long as two to the
/// number of such types, and no message is read that far.
const WRITTEN_AT_MOST: u32 = 4096;

/// The layouts that one question about a layout has found: a type that
/// stands in several places is laid out once.
#[derive(Default)]
struct Layouts {
    /// The declared types that are being laid out, the innermost last.
    nest: Vec<TypeId>,
    known: FxHashMap<TypeId, Result<(u64, u64), NoLayout>>,
}

/// Why a type has no size here.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum NoLayout {
    /// A type parameter, a type of Go++ alone, a type in error.
    Unknown,
    TooLarge,
}

/// Why a predeclared generic type cannot take a type argument.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BadArgument {
    /// `Unique[*T]`: a borrowed pointer may not live on the heap (OWN-B5).
    PointerOnHeap,
    /// `Local[int]`: only a func or an interface is tied to a goroutine.
    LocalOfValue,
}

pub(crate) const BASICS: [Basic; 24] = [
    Basic::Bool,
    Basic::Int,
    Basic::Int8,
    Basic::Int16,
    Basic::Int32,
    Basic::Int64,
    Basic::Uint,
    Basic::Uint8,
    Basic::Uint16,
    Basic::Uint32,
    Basic::Uint64,
    Basic::Uintptr,
    Basic::Float32,
    Basic::Float64,
    Basic::Complex64,
    Basic::Complex128,
    Basic::String,
    Basic::UntypedBool,
    Basic::UntypedInt,
    Basic::UntypedRune,
    Basic::UntypedFloat,
    Basic::UntypedComplex,
    Basic::UntypedString,
    Basic::UntypedNil,
];

/// The types of one compilation. Shared by all packages and threads.
pub struct Types {
    inner: RwLock<Inner>,
    error: TypeId,
    /// The predeclared aliases `byte`, `rune` and `any`.
    byte: TypeId,
    rune: TypeId,
    any: TypeId,
    /// The name of the copy method.
    clone_name: Spur,
    /// The work of looking up members, counted for the tests of speed.
    steps: AtomicU64,
    /// For a copy of a table: how much of the table there was.
    forked: fork::Sizes,
}

/// How a value gets into a place of an interface type.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum IntoInterface {
    /// The value is put into an interface value.
    Put,
    /// The value is an interface value, and the interface type of the
    /// place has other methods than its own.
    Changed,
}

/// Type parameters that are replaced by types, in many types: a type
/// that stands in several of them is walked once. Declared types may
/// be replaced by others too (`rename`).
pub struct Replacing<'a> {
    types: &'a Types,
    from: &'a [TypeId],
    to: &'a [TypeId],
    renamed: FxHashMap<NamedId, NamedId>,
    steps: Cell<u64>,
    done: RefCell<FxHashMap<(u32, u32), TypeId>>,
}

impl Replacing<'_> {
    /// From here on the declared type `old` is replaced by `new`.
    pub fn rename(&mut self, old: NamedId, new: NamedId) {
        self.renamed.insert(old, new);
        self.done.borrow_mut().clear();
    }

    /// The type with what is replaced in it.
    pub fn ty(&self, ty: TypeId) -> TypeId {
        let both = (self.from, self.to);
        self.types
            .substitute_counted(ty, both, &self.renamed, &self.steps, &self.done)
    }
}

impl Drop for Replacing<'_> {
    fn drop(&mut self) {
        self.types
            .steps
            .fetch_add(self.steps.get(), Ordering::Relaxed);
    }
}

impl Types {
    /// `names` is the interner of the compilation.
    pub fn new(names: &ThreadedRodeo) -> Self {
        let mut inner = Inner::default();
        inner.intern(Type::Invalid);
        for basic in BASICS {
            inner.intern(Type::Basic(basic));
        }
        // type error interface { Error() string }
        let string = TypeId::plain(1 + Basic::String as u32);
        let sig = inner.intern(Type::Func {
            type_params: Box::new([]),
            params: Box::new([]),
            results: Box::new([string]),
            variadic: false,
        });
        let method = Method {
            name: names.get_or_intern_static("Error"),
            package: None,
            sig,
        };
        let underlying = inner.intern(Type::Interface {
            methods: Box::new([method]),
            embedded: Box::new([]),
        });
        inner.named.push(Named {
            name: names.get_or_intern_static("error"),
            package: None,
            path: None,
            local: false,
            underlying: Some(underlying),
            declared_as: Some(underlying),
            invalid: false,
            params: Vec::new(),
            methods: Vec::new(),
            method_index: FxHashMap::default(),
            operators: Vec::new(),
            extends: false,
            parents: Vec::new(),
            abstract_marked: false,
            expanding: false,
            pinned: false,
            drops: false,
            alias: None,
        });
        let error = inner.intern(Type::Named {
            id: NamedId(0),
            args: Box::new([]),
        });
        let empty = inner.intern(Type::Interface {
            methods: Box::new([]),
            embedded: Box::new([]),
        });
        let mut predeclared = |name: &'static str, target: TypeId| {
            let named = NamedId(inner.named.len() as u32);
            inner.named.push(Named {
                name: names.get_or_intern_static(name),
                package: None,
                path: None,
                local: false,
                underlying: None,
                declared_as: Some(target),
                invalid: false,
                params: Vec::new(),
                methods: Vec::new(),
                method_index: FxHashMap::default(),
                operators: Vec::new(),
                extends: false,
                parents: Vec::new(),
                abstract_marked: false,
                expanding: false,
                pinned: false,
                drops: false,
                alias: Some(target),
            });
            inner.alias(named, &[], target)
        };
        let byte = predeclared("byte", TypeId::plain(1 + Basic::Uint8 as u32));
        let rune = predeclared("rune", TypeId::plain(1 + Basic::Int32 as u32));
        let any = predeclared("any", empty);
        Self {
            inner: RwLock::new(inner),
            error,
            byte,
            rune,
            any,
            clone_name: names.get_or_intern_static("Clone"),
            steps: AtomicU64::new(0),
            forked: fork::Sizes::default(),
        }
    }

    /// How much work looking up members took so far.
    pub fn steps(&self) -> u64 {
        self.steps.load(Ordering::Relaxed)
    }

    fn read<R>(&self, f: impl FnOnce(&Inner) -> R) -> R {
        f(&self.inner.read().unwrap_or_else(PoisonError::into_inner))
    }

    fn write<R>(&self, f: impl FnOnce(&mut Inner) -> R) -> R {
        f(&mut self.inner.write().unwrap_or_else(PoisonError::into_inner))
    }

    pub fn intern(&self, ty: Type) -> TypeId {
        // Most types asked for exist already.
        if let Some(id) = self.read(|inner| inner.find(&ty)) {
            return id;
        }
        self.write(|inner| inner.intern(ty))
    }

    /// What the type is, behind the aliases it is written with; its parts
    /// are as they were written.
    pub fn get(&self, id: TypeId) -> Arc<Type> {
        self.read(|inner| Arc::clone(&inner.types[inner.unalias(id).written as usize]))
    }

    pub const INVALID: TypeId = TypeId::plain(0);

    pub fn basic(&self, basic: Basic) -> TypeId {
        TypeId::plain(1 + basic as u32)
    }

    /// The alias `named` with the type arguments `args`, none unless it
    /// is generic: the type that the alias stands for, written by the
    /// name of the alias. `None` if the alias is not declared yet.
    pub fn alias(&self, named: NamedId, args: &[TypeId]) -> Option<TypeId> {
        if let Some(known) = self.read(|inner| inner.find_alias(named, args)) {
            return Some(known);
        }
        let (target, params) = self.read(|inner| {
            let named = &inner.named[named.0 as usize];
            (named.alias, named.params.clone())
        });
        // A type parameter may be replaced by a type that is written as
        // an alias.
        let target = self.unalias(self.substitute(target?, &params, args));
        Some(self.write(|inner| inner.alias(named, args, target)))
    }

    /// The type as it is written behind the alias that names it, and
    /// behind the aliases that this one names.
    pub fn unalias(&self, id: TypeId) -> TypeId {
        self.read(|inner| inner.unalias(id))
    }

    /// What the alias that the type is written as was declared as, with
    /// its type arguments in the place of its type parameters: this may
    /// be written as another alias.
    pub fn alias_target(&self, id: TypeId) -> Option<TypeId> {
        let (declared, params, args) = self.read(|inner| {
            let alias = inner.alias_of(id)?;
            let named = &inner.named[alias.named.0 as usize];
            Some((named.declared_as, named.params.clone(), alias.args.to_vec()))
        })?;
        Some(self.substitute(declared?, &params, &args))
    }

    /// Whether the type is written by the name of an alias that a
    /// program declares, or by `any`: `byte` and `rune` are names of
    /// basic types.
    pub fn named_by_alias(&self, id: TypeId) -> bool {
        let predeclared = [self.byte, self.rune];
        id.written & ALIAS != 0 && predeclared.iter().all(|other| other.written != id.written)
    }

    /// The alias that the type is written as, with its type arguments.
    pub fn alias_of(&self, id: TypeId) -> Option<(NamedId, Vec<TypeId>)> {
        self.read(|inner| {
            let alias = inner.alias_of(id)?;
            Some((alias.named, alias.args.to_vec()))
        })
    }

    /// The predeclared type `error`.
    pub fn error(&self) -> TypeId {
        self.error
    }

    /// The predeclared alias `any` of `interface{}`.
    pub fn any(&self) -> TypeId {
        self.any
    }

    /// The predeclared alias `byte` of `uint8`.
    pub fn byte(&self) -> TypeId {
        self.byte
    }

    /// The predeclared alias `rune` of `int32`.
    pub fn rune(&self) -> TypeId {
        self.rune
    }

    pub fn pointer(&self, elem: TypeId) -> TypeId {
        self.intern(Type::Pointer(elem))
    }

    pub fn slice(&self, elem: TypeId) -> TypeId {
        self.intern(Type::Slice(elem))
    }

    pub fn array(&self, len: u64, elem: TypeId) -> TypeId {
        self.intern(Type::Array { len, elem })
    }

    pub fn map(&self, key: TypeId, value: TypeId) -> TypeId {
        self.intern(Type::Map { key, value })
    }

    pub fn chan(&self, dir: ChanDir, elem: TypeId) -> TypeId {
        self.intern(Type::Chan { dir, elem })
    }

    pub fn option(&self, elem: TypeId) -> TypeId {
        self.intern(Type::Option(elem))
    }

    pub fn func(&self, params: &[TypeId], results: &[TypeId], variadic: bool) -> TypeId {
        self.intern(Type::Func {
            type_params: Box::new([]),
            params: params.into(),
            results: results.into(),
            variadic,
        })
    }

    /// The type of a function that can fail: `func!(params) results` is
    /// `func(params) (results, error)`.
    pub fn failing_func(&self, params: &[TypeId], results: &[TypeId], variadic: bool) -> TypeId {
        let results: Vec<TypeId> = results.iter().copied().chain([self.error]).collect();
        self.func(params, &results, variadic)
    }

    pub fn strukt(&self, fields: Vec<Field>) -> TypeId {
        self.intern(Type::Struct(fields.into()))
    }

    /// An interface with the methods written in it and what it embeds, in
    /// any order.
    pub fn interface(
        &self,
        mut methods: Vec<Method>,
        mut embedded: Vec<TypeId>,
        names: &ThreadedRodeo,
    ) -> TypeId {
        // By the text of the names: the order of interning differs from
        // run to run.
        let key = |m: &Method| (names.resolve(&m.name), m.package.map(|p| names.resolve(&p)));
        methods.sort_by(|a, b| key(a).cmp(&key(b)));
        embedded.sort_unstable();
        embedded.dedup();
        self.intern(Type::Interface {
            methods: methods.into(),
            embedded: embedded.into(),
        })
    }

    /// `Unique[elem]`, `Shared[elem]`, `Weak[elem]` or `Local[elem]`, as
    /// `make` builds it from the argument, or why the argument is refused.
    pub fn handle(&self, make: fn(TypeId) -> Type, elem: TypeId) -> Result<TypeId, BadArgument> {
        let ty = make(elem);
        let under = self.get(self.underlying(elem));
        match (&ty, &*under) {
            (Type::Local(_), Type::Func { .. } | Type::Interface { .. } | Type::Invalid) => {}
            (Type::Local(_), _) => return Err(BadArgument::LocalOfValue),
            (_, Type::Pointer(_)) => return Err(BadArgument::PointerOnHeap),
            _ => {}
        }
        Ok(self.intern(ty))
    }

    /// Declares a type; its underlying type is set when its declaration is
    /// checked. `package` is the name of the declaring package.
    pub fn declare(&self, name: Spur, package: Spur) -> NamedId {
        self.write(|inner| {
            inner.named.push(Named {
                name,
                package: Some(package),
                path: None,
                local: false,
                underlying: None,
                declared_as: None,
                invalid: false,
                params: Vec::new(),
                methods: Vec::new(),
                method_index: FxHashMap::default(),
                operators: Vec::new(),
                extends: false,
                parents: Vec::new(),
                abstract_marked: false,
                expanding: false,
                pinned: false,
                drops: false,
                alias: None,
            });
            NamedId(inner.named.len() as u32 - 1)
        })
    }

    /// Says where `named` is declared: the import path of its package,
    /// and whether inside a function.
    pub fn set_home(&self, named: NamedId, path: Spur, local: bool) {
        self.write(|inner| {
            let named = &mut inner.named[named.0 as usize];
            (named.path, named.local) = (Some(path), local);
        });
    }

    /// Where `named` is declared.
    pub fn home(&self, named: NamedId) -> Home {
        self.read(|inner| {
            let named = &inner.named[named.0 as usize];
            Home {
                name: named.name,
                package: named.package,
                path: named.path,
                local: named.local,
                generic: !named.params.is_empty(),
                invalid: named.invalid,
            }
        })
    }

    /// Says what the declaration of `named` wrote.
    pub fn set_underlying(&self, named: NamedId, declared_as: TypeId) {
        self.write(|inner| inner.named[named.0 as usize].declared_as = Some(declared_as));
    }

    /// Says that `named` is an alias, declared as `aliased`.
    pub fn set_alias(&self, named: NamedId, aliased: TypeId) {
        self.write(|inner| {
            let target = inner.unalias(aliased);
            let named = &mut inner.named[named.0 as usize];
            named.alias = Some(target);
            named.declared_as = Some(aliased);
        });
    }

    /// Whether `named` is an alias.
    pub fn is_alias(&self, named: NamedId) -> bool {
        self.read(|inner| inner.named[named.0 as usize].alias.is_some())
    }

    /// The type that the declaration of `named` wrote, with the type
    /// parameters of a generic type in it.
    pub fn declared_as(&self, named: NamedId) -> Option<TypeId> {
        self.read(|inner| inner.named[named.0 as usize].declared_as)
    }

    pub fn type_params(&self, named: NamedId) -> Vec<TypeId> {
        self.read(|inner| inner.named[named.0 as usize].params.clone())
    }

    /// Whether the type is a declared type with methods of its own.
    pub fn declares_methods(&self, ty: TypeId) -> bool {
        match &*self.get(ty) {
            Type::Named { id, .. } => {
                self.read(|inner| !inner.named[id.0 as usize].methods.is_empty())
            }
            _ => false,
        }
    }

    pub fn name_of(&self, named: NamedId) -> Spur {
        self.read(|inner| inner.named[named.0 as usize].name)
    }

    /// Makes the declared type invalid: its declaration is in error, and
    /// nothing more is said about what is made of it.
    /// Whether the type is `unsafe.Pointer`, or declared as it.
    pub fn is_unsafe_pointer(&self, mut ty: TypeId, names: &ThreadedRodeo) -> bool {
        // A type that is declared as itself ends the walk.
        for _ in 0..64 {
            let Type::Named { id, .. } = &*self.get(ty) else {
                return false;
            };
            let (name, package, declared_as) = self.read(|inner| {
                let named = &inner.named[id.0 as usize];
                (named.name, named.package, named.declared_as)
            });
            let package = package.map(|package| names.resolve(&package));
            if names.resolve(&name) == "Pointer" && package == Some("unsafe") {
                return true;
            }
            match declared_as {
                Some(declared_as) => ty = declared_as,
                None => return false,
            }
        }
        false
    }

    pub fn is_invalid(&self, named: NamedId) -> bool {
        self.read(|inner| inner.named[named.0 as usize].invalid)
    }

    /// Says that `named` is declared with `@Extends`.
    pub fn set_extends(&self, named: NamedId) {
        self.write(|inner| inner.named[named.0 as usize].extends = true);
    }

    /// Says that `named` is declared with `@Pinned`.
    pub fn set_pinned(&self, named: NamedId) {
        self.write(|inner| inner.named[named.0 as usize].pinned = true);
    }

    /// Says that `named` has a `drop` method.
    pub fn set_drops(&self, named: NamedId) {
        self.write(|inner| inner.named[named.0 as usize].drops = true);
    }

    /// Whether `named` has a `drop` method, which ends its values
    /// (OWN-R7).
    pub fn drops(&self, named: NamedId) -> bool {
        self.read(|inner| inner.named[named.0 as usize].drops)
    }

    /// Whether the type is a declared type with parents: it also has
    /// their members and is a value of their types.
    pub fn extends(&self, ty: TypeId) -> bool {
        match &*self.get(ty) {
            Type::Named { id, .. } => self.read(|inner| inner.named[id.0 as usize].extends),
            _ => false,
        }
    }

    pub fn invalidate(&self, named: NamedId) {
        self.write(|inner| inner.named[named.0 as usize].invalid = true);
    }

    /// The constraint of the type parameter `param`, once it is known.
    pub fn constraint(&self, param: TypeId) -> Option<TypeId> {
        match &*self.get(param) {
            Type::Param(id) => self.read(|inner| inner.params[id.0 as usize].1),
            _ => None,
        }
    }

    /// Gives the type parameter `param` its constraint.
    pub fn set_constraint(&self, param: TypeId, constraint: TypeId) {
        if let Type::Param(id) = &*self.get(param) {
            self.write(|inner| inner.params[id.0 as usize].1 = Some(constraint));
        }
    }

    /// The type of a generic function with the type parameters
    /// `type_params` and the parameters and results of `sig`.
    pub fn generic_func_type(&self, sig: TypeId, type_params: &[TypeId]) -> TypeId {
        match &*self.get(sig) {
            Type::Func {
                params,
                results,
                variadic,
                ..
            } => self.intern(Type::Func {
                type_params: type_params.into(),
                params: params.clone(),
                results: results.clone(),
                variadic: *variadic,
            }),
            _ => sig,
        }
    }

    /// The type parameters of a generic function of the type `sig`.
    pub fn generic_func(&self, sig: TypeId) -> Option<Vec<TypeId>> {
        match &*self.get(sig) {
            Type::Func { type_params, .. } if !type_params.is_empty() => Some(type_params.to_vec()),
            _ => None,
        }
    }

    /// Says that `named` is generic, with the type parameters `params`.
    pub fn set_type_params(&self, named: NamedId, params: Vec<TypeId>) {
        self.write(|inner| inner.named[named.0 as usize].params = params);
    }

    /// `ty` with each type parameter of `from` replaced by the type at the
    /// same place of `to`.
    /// Whether a type parameter is written in the type.
    pub fn has_param(&self, ty: TypeId) -> bool {
        self.read(|inner| inner.has_param[ty.canon as usize])
    }

    /// Whether the type is written with one of the type parameters
    /// `params`.
    pub fn mentions(&self, ty: TypeId, params: &[TypeId]) -> bool {
        self.mentions_in(ty, params, &mut FxHashSet::default())
    }

    /// `mentions`; a type that was `seen` is not walked again.
    fn mentions_in(&self, ty: TypeId, params: &[TypeId], seen: &mut FxHashSet<TypeId>) -> bool {
        if !self.has_param(ty) {
            return false;
        }
        let node = self.get(ty);
        if matches!(&*node, Type::Param(_)) {
            return params.contains(&ty);
        }
        seen.insert(ty)
            && node
                .parts()
                .into_iter()
                .any(|part| deep(|| self.mentions_in(part, params, seen)))
    }

    /// The types that the type is written with, each once for each place:
    /// the type arguments of a declared type, not what it is declared as.
    pub fn parts(&self, ty: TypeId) -> Vec<TypeId> {
        self.get(ty).parts()
    }

    /// The name of a type parameter.
    pub fn param_name(&self, param: TypeId) -> Option<Spur> {
        match &*self.get(param) {
            Type::Param(id) => Some(self.read(|inner| inner.params[id.0 as usize].0)),
            _ => None,
        }
    }

    /// The type of the method of the name that values of the type `ty`
    /// have; `package` is that of a name that is not exported.
    pub fn method_type(
        &self,
        ty: TypeId,
        name: Spur,
        package: Option<Spur>,
        names: &ThreadedRodeo,
    ) -> Option<TypeId> {
        match self.find(ty, (name, package), names, false).ok()?.member {
            Member::Method {
                sig,
                pointer_receiver: false,
            } if self.generic_func(sig).is_none() => Some(sig),
            _ => None,
        }
    }

    pub fn substitute(&self, ty: TypeId, from: &[TypeId], to: &[TypeId]) -> TypeId {
        if from.is_empty() {
            return ty;
        }
        self.replacing(from, to).ty(ty)
    }

    /// What replaces the type parameters `from` by the types `to` in
    /// many types.
    pub fn replacing<'a>(&'a self, from: &'a [TypeId], to: &'a [TypeId]) -> Replacing<'a> {
        Replacing {
            types: self,
            from,
            to,
            renamed: FxHashMap::default(),
            steps: Cell::new(0),
            done: RefCell::default(),
        }
    }

    /// The type that a function declares and that is no alias and has
    /// no type parameters, if `ty` is one.
    pub fn local_type(&self, ty: TypeId) -> Option<NamedId> {
        let Type::Named { id, args } = &*self.get(ty) else {
            return None;
        };
        let plain = self.read(|inner| {
            let named = &inner.named[id.0 as usize];
            named.local && named.alias.is_none() && named.params.is_empty()
        });
        (args.is_empty() && plain).then_some(*id)
    }

    /// A new type with the name and the home of `named`, for a copy of
    /// a type that a generic function declares: each instance of the
    /// function has its own. What it is declared as is set next.
    pub fn local_copy(&self, named: NamedId) -> NamedId {
        self.write(|inner| {
            let of = &inner.named[named.0 as usize];
            let copy = Named {
                name: of.name,
                package: of.package,
                path: of.path,
                local: of.local,
                underlying: None,
                declared_as: None,
                invalid: of.invalid,
                params: Vec::new(),
                methods: Vec::new(),
                method_index: FxHashMap::default(),
                operators: Vec::new(),
                extends: false,
                parents: Vec::new(),
                abstract_marked: false,
                expanding: false,
                pinned: of.pinned,
                drops: false,
                alias: None,
            };
            inner.named.push(copy);
            NamedId(inner.named.len() as u32 - 1)
        })
    }

    /// `done` is what was replaced already, by how it is written: a type
    /// that is met again, as one that stands in several places of an
    /// alias is, is not walked again.
    /// `renamed` has the declared types that are replaced by others.
    fn substitute_counted(
        &self,
        ty: TypeId,
        (from, to): (&[TypeId], &[TypeId]),
        renamed: &FxHashMap<NamedId, NamedId>,
        steps: &Cell<u64>,
        done: &RefCell<FxHashMap<(u32, u32), TypeId>>,
    ) -> TypeId {
        steps.set(steps.get() + 1);
        if let Some(&replaced) = done.borrow().get(&ty.spelled()) {
            return replaced;
        }
        let mut sub =
            |ty: TypeId| deep(|| self.substitute_counted(ty, (from, to), renamed, steps, done));
        let node = self.get(ty);
        // A generic function becomes a plain one also where its type
        // parameters are written nowhere in it.
        let generic = matches!(&*node, Type::Func { type_params, .. } if !type_params.is_empty());
        // A declared type that is replaced may be anywhere in a type.
        if !generic && !self.has_param(ty) && renamed.is_empty() {
            return ty;
        }
        let replaced = match (self.alias_of(ty), &*node) {
            (None, Type::Named { id, args }) if renamed.contains_key(id) => {
                let args = args.iter().map(|&arg| sub(arg)).collect();
                self.intern(Type::Named {
                    id: renamed[id],
                    args,
                })
            }
            // An alias stays an alias, with its type arguments replaced.
            (Some((named, args)), _) => {
                let args: Vec<TypeId> = args.into_iter().map(&mut sub).collect();
                deep(|| self.alias(named, &args)).unwrap_or(ty)
            }
            (None, Type::Param(_)) => {
                let at = from.iter().position(|&param| param == ty);
                return at.and_then(|at| to.get(at).copied()).unwrap_or(ty);
            }
            // With its type arguments a function is generic no more.
            // A generic function keeps the type parameters that are not
            // replaced.
            (None, Type::Func { .. }) => {
                let mut replaced = node.map_parts(&mut sub);
                if let Type::Func { type_params, .. } = &mut replaced {
                    let kept = type_params.iter().copied();
                    *type_params = kept.filter(|param| !from.contains(param)).collect();
                }
                self.intern(replaced)
            }
            (None, node) => self.intern(node.map_parts(&mut sub)),
        };
        done.borrow_mut().insert(ty.spelled(), replaced);
        replaced
    }

    /// What `name` reaches in a value of the type `ty`: a field or a
    /// method of the type, or one reached through embedded fields, the
    /// shallowest first. `None` if there is none, or two at one depth.
    /// `package` is the path of the package that asks: a name that is not
    /// exported is found only in its own package.
    pub fn member(
        &self,
        ty: TypeId,
        name: Spur,
        package: Spur,
        names: &ThreadedRodeo,
    ) -> Option<Member> {
        let found = self.lookup(ty, name, package, names, false);
        found.ok().map(|found| found.member)
    }

    /// `member`, with the name that was found and why nothing was. With
    /// `any_case`, a name that differs in its capitals is found too,
    /// whatever its package: the one that was likely meant.
    pub fn lookup(
        &self,
        ty: TypeId,
        name: Spur,
        package: Spur,
        names: &ThreadedRodeo,
        any_case: bool,
    ) -> Result<Found, NotFound> {
        let exported = names.resolve(&name).starts_with(char::is_uppercase);
        let key = (name, (!exported).then_some(package));
        self.find(ty, key, names, any_case)
    }

    /// The type of the method `method` of a generic type for the type
    /// arguments `args`. A method with type parameters of its own gets
    /// new ones there, whose constraints are written with the type
    /// arguments too.
    fn method_of_instance(&self, method: &DeclaredMethod, args: &[TypeId]) -> TypeId {
        let sig = method.method.sig;
        let own = self.generic_func(sig).unwrap_or_default();
        if own.is_empty() || method.receiver_params.is_empty() {
            return self.substitute(sig, &method.receiver_params, args);
        }
        let fresh: Vec<TypeId> = own
            .iter()
            .filter_map(|&param| self.param_copy(param))
            .collect();
        if let Some(&first) = fresh.first() {
            let receiver = (method.receiver_params.clone(), args.to_vec());
            self.write(|inner| inner.method_receivers.insert(first, receiver));
        }
        let from: Vec<TypeId> = method.receiver_params.iter().chain(&own).copied().collect();
        let to: Vec<TypeId> = args.iter().chain(&fresh).copied().collect();
        for (&param, &new) in own.iter().zip(&fresh) {
            if let Some(constraint) = self.constraint(param) {
                self.set_constraint(new, self.substitute(constraint, &from, &to));
            }
        }
        self.generic_func_type(self.substitute(sig, &from, &to), &fresh)
    }

    /// `lookup` of a name with the package it belongs to, which is none
    /// for an exported name.
    fn find(
        &self,
        ty: TypeId,
        key: (Spur, Option<Spur>),
        names: &ThreadedRodeo,
        any_case: bool,
    ) -> Result<Found, NotFound> {
        let mut steps = 0;
        let found = self.find_counted(ty, key, names, any_case, &mut steps);
        self.steps.fetch_add(steps, Ordering::Relaxed);
        found
    }

    /// `find`; `steps` counts each type and each field that is looked at.
    fn find_counted(
        &self,
        ty: TypeId,
        key: (Spur, Option<Spur>),
        names: &ThreadedRodeo,
        any_case: bool,
        steps: &mut u64,
    ) -> Result<Found, NotFound> {
        let text = names.resolve(&key.0);
        let matches = |other: Spur, owner: Option<Spur>| match any_case {
            true => names.resolve(&other).to_lowercase() == text.to_lowercase(),
            false => (other, owner) == key,
        };
        // Through one pointer, the members of what it points to. A
        // declared pointer type has the fields, but no methods.
        let (start, through_pointer) = match &*self.get(self.underlying(ty)) {
            // A pointer to an interface or to a type parameter has no
            // members.
            Type::Pointer(elem)
                if !self.is_interface(*elem) && !matches!(&*self.get(*elem), Type::Param(_)) =>
            {
                (*elem, true)
            }
            _ => (ty, false),
        };
        let fields_only = through_pointer && matches!(&*self.get(ty), Type::Named { .. });
        // The types at one depth of embedding: each with whether a
        // pointer was followed to it, the way to it, and whether it is
        // embedded more than once at that depth.
        let mut level = vec![(start, through_pointer, Vec::new(), false)];
        let mut seen: FxHashSet<TypeId> = [start].into_iter().collect();
        while !level.is_empty() {
            let mut found = Vec::new();
            let mut next: Vec<(TypeId, bool, Vec<u32>, bool)> = Vec::new();
            let mut next_at: FxHashMap<TypeId, usize> = FxHashMap::default();
            // What is found in a type that is embedded twice is found
            // twice.
            let mut twice = false;
            for (current, indirect, path, multiple) in &level {
                let (current, indirect) = (*current, *indirect);
                *steps += 1;
                let before = found.len();
                let owner = match &*self.get(current) {
                    Type::Named { id, .. } => self.read(|inner| inner.named[id.0 as usize].package),
                    _ => None,
                };
                if let Type::Named { id, args } = &*self.get(current) {
                    let declared = self.read(|inner| {
                        let named = &inner.named[id.0 as usize];
                        let at = match any_case {
                            true => named
                                .methods
                                .iter()
                                .position(|m| matches(m.method.name, m.method.package)),
                            false => named.method_index.get(&key).map(|&at| at as usize),
                        };
                        at.map(|at| named.methods[at].clone())
                    });
                    if let Some(method) = declared {
                        let member = Member::Method {
                            sig: self.method_of_instance(&method, args),
                            pointer_receiver: method.pointer_receiver && !indirect,
                        };
                        found.push(Found {
                            member,
                            name: method.method.name,
                            owner,
                            holder: current,
                            path: path.clone(),
                            indirect,
                        });
                        twice |= *multiple;
                        continue;
                    }
                }
                // What a type does not declare, its parents may: each is
                // looked at like an embedded type, one level down.
                for (index, parent) in self.parents(current).into_iter().enumerate() {
                    let mut path = path.clone();
                    path.push(hierarchy::PARENT | index as u32);
                    if let Some(&at) = next_at.get(&parent) {
                        next[at].3 = true;
                    } else if seen.insert(parent) {
                        next_at.insert(parent, next.len());
                        next.push((parent, indirect, path, *multiple));
                    }
                }
                match &*self.get(self.underlying(current)) {
                    Type::Struct(fields) => {
                        *steps += fields.len() as u64;
                        for (index, field) in fields.iter().enumerate() {
                            let through = || {
                                let mut path = path.clone();
                                path.push(index as u32);
                                path
                            };
                            if matches(field.name, field.package) {
                                let member = Member::Field {
                                    ty: field.ty,
                                    indirect,
                                };
                                found.push(Found {
                                    member,
                                    name: field.name,
                                    owner,
                                    holder: current,
                                    path: through(),
                                    indirect,
                                });
                            } else if field.embedded {
                                let (inner, pointer) = match &*self.get(field.ty) {
                                    Type::Pointer(elem) => (*elem, true),
                                    _ => (field.ty, false),
                                };
                                // A type that is met at a smaller depth
                                // hides itself here.
                                if let Some(&at) = next_at.get(&inner) {
                                    next[at].3 = true;
                                } else if seen.insert(inner) {
                                    next_at.insert(inner, next.len());
                                    next.push((inner, indirect || pointer, through(), *multiple));
                                }
                            }
                        }
                    }
                    // A type parameter has the methods of its constraint.
                    Type::Interface { .. } | Type::Param(_) => {
                        let methods = match &*self.get(current) {
                            Type::Param(_) => self.param_set(current).map(|set| set.methods),
                            _ => self.method_set(current),
                        };
                        let methods = methods.unwrap_or_default();
                        let method = match any_case {
                            true => {
                                let mut near = None;
                                methods.for_each(&mut |m| {
                                    if matches(m.name, m.package) {
                                        near = Some(m.clone());
                                    }
                                });
                                near
                            }
                            false => methods.get(key).cloned(),
                        };
                        if let Some(method) = &method {
                            let member = Member::Method {
                                sig: method.sig,
                                pointer_receiver: false,
                            };
                            found.push(Found {
                                member,
                                name: method.name,
                                owner,
                                holder: current,
                                path: path.clone(),
                                indirect,
                            });
                        }
                    }
                    _ => {}
                }
                twice |= *multiple && found.len() > before;
            }
            let one = match found.len() {
                0 => {
                    level = next;
                    continue;
                }
                1 if !twice => found.pop(),
                _ => return Err(NotFound::Ambiguous),
            };
            return match one {
                Some(one) if fields_only && matches!(one.member, Member::Method { .. }) => {
                    Err(NotFound::Missing)
                }
                Some(one) => Ok(one),
                None => Err(NotFound::Missing),
            };
        }
        Err(NotFound::Missing)
    }

    /// The size and the alignment in bytes of a value of the type, as Go's
    /// compiler lays it out on a 64-bit machine. `None` for a type whose
    /// layout is not fixed here: a type parameter, a type of Go++ alone.
    pub fn layout(&self, ty: TypeId, names: &ThreadedRodeo) -> Option<(u64, u64)> {
        self.layout_in(ty, names, &mut Layouts::default()).ok()
    }

    /// Whether a value of the type has more bytes than an `int64` counts.
    pub fn too_large(&self, ty: TypeId, names: &ThreadedRodeo) -> bool {
        self.layout_in(ty, names, &mut Layouts::default()) == Err(NoLayout::TooLarge)
    }

    /// `layout` inside the declared types `nest`: a type that holds a
    /// value of itself, which is an error, has no layout.
    fn layout_in(
        &self,
        ty: TypeId,
        names: &ThreadedRodeo,
        layouts: &mut Layouts,
    ) -> Result<(u64, u64), NoLayout> {
        deep(|| self.layout_here(ty, names, layouts))
    }

    fn layout_here(
        &self,
        ty: TypeId,
        names: &ThreadedRodeo,
        layouts: &mut Layouts,
    ) -> Result<(u64, u64), NoLayout> {
        let node = self.get(ty);
        // What has no value of another type inside is not worth keeping.
        if !matches!(
            &*node,
            Type::Named { .. } | Type::Struct(_) | Type::Array { .. }
        ) {
            return self.layout_of(ty, names, layouts);
        }
        if let Some(&known) = layouts.known.get(&ty) {
            return known;
        }
        let named = matches!(&*node, Type::Named { .. });
        if named && layouts.nest.contains(&ty) {
            return Err(NoLayout::Unknown);
        }
        if named {
            layouts.nest.push(ty);
        }
        let layout = self.layout_of(ty, names, layouts);
        if named {
            layouts.nest.pop();
        }
        layouts.known.insert(ty, layout);
        self.steps.fetch_add(1, Ordering::Relaxed);
        layout
    }

    fn layout_of(
        &self,
        ty: TypeId,
        names: &ThreadedRodeo,
        layouts: &mut Layouts,
    ) -> Result<(u64, u64), NoLayout> {
        const WORD: u64 = 8;
        let sized = |size: Option<u64>| match size {
            Some(size) if size <= i64::MAX as u64 => Ok(size),
            _ => Err(NoLayout::TooLarge),
        };
        Ok(match &*self.get(self.underlying(ty)) {
            Type::Basic(basic) => match basic {
                Basic::Bool | Basic::Int8 | Basic::Uint8 => (1, 1),
                Basic::Int16 | Basic::Uint16 => (2, 2),
                Basic::Int32 | Basic::Uint32 | Basic::Float32 => (4, 4),
                // A complex number is laid out as its two floats.
                Basic::Complex64 => (8, 4),
                Basic::Complex128 => (16, 8),
                Basic::String => (2 * WORD, WORD),
                basic if basic.is_untyped() => return Err(NoLayout::Unknown),
                _ => (WORD, WORD),
            },
            Type::Pointer(_) | Type::Map { .. } | Type::Chan { .. } | Type::Func { .. } => {
                (WORD, WORD)
            }
            Type::Slice(_) => (3 * WORD, WORD),
            Type::Interface { .. } => (2 * WORD, WORD),
            Type::Array { len, elem } => {
                let (size, align) = self.layout_in(*elem, names, layouts)?;
                (sized(size.checked_mul(*len))?, align)
            }
            Type::Struct(fields) => {
                // An empty struct named `align64` in a package `atomic`
                // makes the struct around it 8-byte aligned.
                let marker = fields.is_empty()
                    && matches!(&*self.get(ty), Type::Named { id, .. } if self.read(|inner| {
                        let named = &inner.named[id.0 as usize];
                        let package = named.package.map(|package| names.resolve(&package));
                        names.resolve(&named.name) == "align64" && package == Some("atomic")
                    }));
                if marker {
                    return Ok((0, WORD));
                }
                let (offsets, end, align) = self.field_offsets(fields, names, layouts)?;
                // A struct that is not empty does not end in a field of
                // no size: a pointer past it would point outside it.
                let last_is_empty = match (offsets.last(), fields.last()) {
                    (Some(&offset), Some(_)) => offset == end && end > 0,
                    _ => false,
                };
                let size = if last_is_empty { end + 1 } else { end };
                (sized(size.checked_next_multiple_of(align))?, align)
            }
            _ => return Err(NoLayout::Unknown),
        })
    }

    /// The offset of each field of a struct, the offset after the last
    /// one, and the alignment of the struct.
    fn field_offsets(
        &self,
        fields: &[Field],
        names: &ThreadedRodeo,
        layouts: &mut Layouts,
    ) -> Result<(Vec<u64>, u64, u64), NoLayout> {
        let (mut offsets, mut next, mut max_align) = (Vec::new(), 0u64, 1u64);
        let sized = |size: Option<u64>| match size {
            Some(size) if size <= i64::MAX as u64 => Ok(size),
            _ => Err(NoLayout::TooLarge),
        };
        for field in fields {
            let (size, align) = self.layout_in(field.ty, names, layouts)?;
            next = sized(next.checked_next_multiple_of(align))?;
            offsets.push(next);
            next = sized(next.checked_add(size))?;
            max_align = max_align.max(align);
        }
        Ok((offsets, next, max_align))
    }

    /// The offset in bytes of the field `name` in a value of the struct
    /// type `ty`, through embedded structs that are held by value.
    pub fn offset_of(
        &self,
        ty: TypeId,
        name: Spur,
        package: Spur,
        names: &ThreadedRodeo,
    ) -> Option<u64> {
        let exported = names.resolve(&name).starts_with(char::is_uppercase);
        let mut level = vec![(ty, 0u64)];
        let mut seen = vec![ty];
        let mut layouts = Layouts::default();
        while !level.is_empty() {
            let (mut found, mut next) = (Vec::new(), Vec::new());
            for &(current, base) in &level {
                let Type::Struct(fields) = &*self.get(self.underlying(current)) else {
                    continue;
                };
                let (offsets, ..) = self.field_offsets(fields, names, &mut layouts).ok()?;
                for (field, offset) in fields.iter().zip(offsets) {
                    if field.name == name && (exported || field.package == Some(package)) {
                        found.push(base + offset);
                    } else if field.embedded && !seen.contains(&field.ty) {
                        seen.push(field.ty);
                        next.push((field.ty, base + offset));
                    }
                }
            }
            match found[..] {
                [] => level = next,
                [one] => return Some(one),
                _ => return None,
            }
        }
        None
    }

    /// Declares an `@Operator` method of `named`, next to the others of
    /// its name.
    pub fn add_operator(&self, named: NamedId, method: DeclaredMethod) {
        self.write(|inner| inner.named[named.0 as usize].operators.push(method));
    }

    /// The methods that the declared type declares itself, in the order
    /// of the declarations, without its `@Operator` methods.
    pub fn declared_methods(&self, named: NamedId) -> Vec<DeclaredMethod> {
        let operator = |method: &DeclaredMethod| method.marks.has(Marks::OPERATOR);
        self.read(|inner| {
            let methods = inner.named[named.0 as usize].methods.iter();
            methods
                .filter(|method| !operator(method))
                .cloned()
                .collect()
        })
    }

    /// The `@Operator` methods that the declared type declares itself,
    /// in the order of the declarations, written with the type
    /// parameters that each receiver names.
    pub fn declared_operators(&self, named: NamedId) -> Vec<DeclaredMethod> {
        self.read(|inner| inner.named[named.0 as usize].operators.clone())
    }

    /// The `@Operator` methods of the name that a value of the declared
    /// type `ty` has: its own, then those of its ancestors that none
    /// before replaces (EXP-30).
    pub fn operators(&self, ty: TypeId, name: Spur) -> Vec<Operator> {
        let mut found = Vec::new();
        self.operators_in(ty, name, &mut found);
        found
    }

    fn operators_in(&self, ty: TypeId, name: Spur, found: &mut Vec<Operator>) {
        let Type::Named { id, args } = &*self.get(self.unalias(ty)) else {
            return;
        };
        let own: Vec<(u32, DeclaredMethod)> = self.read(|inner| {
            let all = inner.named[id.0 as usize].operators.iter().enumerate();
            all.filter(|(_, method)| method.method.name == name)
                .map(|(at, method)| (at as u32, method.clone()))
                .collect()
        });
        self.steps
            .fetch_add(1 + own.len() as u64, Ordering::Relaxed);
        for (at, method) in own {
            let sig = self.substitute(method.method.sig, &method.receiver_params, args);
            let right = method.marks.has(Marks::RIGHT);
            let replaced = found.iter().any(|known: &Operator| {
                known.marks.has(Marks::RIGHT) == right && self.same_params(known.sig, sig)
            });
            if !replaced {
                found.push(Operator {
                    name,
                    sig,
                    marks: method.marks,
                    pointer_receiver: method.pointer_receiver,
                    holder: ty,
                    at,
                });
            }
        }
        for parent in self.parents(ty) {
            deep(|| self.operators_in(parent, name, found));
        }
    }

    /// The methods with the mark that a value of the declared type `ty`
    /// has, each with the type that declares it: its own, then those of
    /// its ancestors. The types of the methods are written with the type
    /// arguments.
    pub fn marked_methods(&self, ty: TypeId, mark: Marks) -> Vec<(Method, TypeId)> {
        let mut found = Vec::new();
        self.marked_in(ty, mark, &mut found);
        found
    }

    fn marked_in(&self, ty: TypeId, mark: Marks, found: &mut Vec<(Method, TypeId)>) {
        let Type::Named { id, args } = &*self.get(self.unalias(ty)) else {
            return;
        };
        let own: Vec<DeclaredMethod> = self.read(|inner| {
            let all = inner.named[id.0 as usize].methods.iter();
            all.filter(|method| method.marks.has(mark))
                .cloned()
                .collect()
        });
        self.steps
            .fetch_add(1 + own.len() as u64, Ordering::Relaxed);
        for method in own {
            let sig = self.substitute(method.method.sig, &method.receiver_params, args);
            found.push((
                Method {
                    sig,
                    ..method.method
                },
                ty,
            ));
        }
        for parent in self.parents(ty) {
            deep(|| self.marked_in(parent, mark, found));
        }
    }

    /// Whether the two function types take parameters of the same types.
    pub fn same_params(&self, a: TypeId, b: TypeId) -> bool {
        match (&*self.get(a), &*self.get(b)) {
            (Type::Func { params: a, .. }, Type::Func { params: b, .. }) => {
                a.len() == b.len() && a.iter().zip(b.iter()).all(|(&a, &b)| self.identical(a, b))
            }
            _ => false,
        }
    }

    /// Declares a method of `named`, or says what the method that was
    /// declared with the name is.
    pub fn add_method(&self, named: NamedId, method: DeclaredMethod) {
        self.write(|inner| {
            let named = &mut inner.named[named.0 as usize];
            let key = (method.method.name, method.method.package);
            match named.method_index.get(&key) {
                Some(&known) => named.methods[known as usize] = method,
                None => {
                    named.method_index.insert(key, named.methods.len() as u32);
                    named.methods.push(method);
                }
            }
        });
    }

    /// Notes that a declaration writes `Option[param]` for its type
    /// parameter `param`, so no type argument for it has nil.
    pub fn set_optional(&self, param: TypeId) {
        self.write(|inner| inner.optional.insert(param));
    }

    /// Says that the declared type is on an instantiation cycle.
    pub fn set_expanding(&self, named: NamedId) {
        self.write(|inner| inner.named[named.0 as usize].expanding = true);
    }

    /// Whether the type is an instance of a type that needs instances
    /// without end: what it is made of is not walked.
    pub fn is_expanding(&self, ty: TypeId) -> bool {
        match &*self.get(ty) {
            Type::Named { id, args } => {
                !args.is_empty() && self.read(|inner| inner.named[id.0 as usize].expanding)
            }
            _ => false,
        }
    }

    /// Notes that a declaration holds a value of its type parameter
    /// `param`, so no type argument for it is abstract.
    pub fn set_held(&self, param: TypeId) {
        self.write(|inner| inner.held.insert(param));
    }

    pub fn is_held(&self, param: TypeId) -> bool {
        self.read(|inner| inner.held.contains(&param))
    }

    /// Notes that a declaration has a `@Required` field of its type
    /// parameter `param`, so no type argument for it is an option.
    pub fn set_required(&self, param: TypeId) {
        self.write(|inner| inner.required.insert(param));
    }

    pub fn is_required(&self, param: TypeId) -> bool {
        self.read(|inner| inner.required.contains(&param))
    }

    /// Whether the type has nil of its own, so that an option of it
    /// says nothing more (OWN-P9).
    pub fn has_own_nil(&self, ty: TypeId, names: &ThreadedRodeo) -> bool {
        self.underlying(ty) != Types::INVALID
            && !matches!(&*self.get(ty), Type::Param(_))
            && (self.has_nil(ty) || self.is_unsafe_pointer(ty, names))
    }

    /// Whether the type is an option, or is declared as one.
    pub fn is_option(&self, ty: TypeId) -> bool {
        matches!(&*self.get(self.underlying(ty)), Type::Option(_))
    }

    pub fn is_optional(&self, param: TypeId) -> bool {
        self.read(|inner| inner.optional.contains(&param))
    }

    /// The marks of the method `name` that the declared type `holder`
    /// declares; none for any other type or name.
    pub fn method_marks(&self, holder: TypeId, name: Spur, package: Option<Spur>) -> Marks {
        let Type::Named { id, .. } = &*self.get(holder) else {
            return Marks::default();
        };
        self.read(|inner| {
            let named = &inner.named[id.0 as usize];
            let at = named.method_index.get(&(name, package));
            at.map_or(Marks::default(), |&at| named.methods[at as usize].marks)
        })
    }

    /// The declared type `named` with the type arguments `args`.
    pub fn named(&self, named: NamedId, args: &[TypeId]) -> TypeId {
        self.intern(Type::Named {
            id: named,
            args: args.into(),
        })
    }

    /// A new type parameter.
    pub fn param(&self, name: Spur) -> TypeId {
        let id = self.write(|inner| {
            inner.params.push((name, None));
            ParamId(inner.params.len() as u32 - 1)
        });
        self.intern(Type::Param(id))
    }

    /// A new type parameter that stands for `of` in one place.
    pub fn param_copy(&self, of: TypeId) -> Option<TypeId> {
        let copy = self.param(self.param_name(of)?);
        let origin = self.param_origin(of);
        self.write(|inner| inner.param_origins.insert(copy, origin));
        Some(copy)
    }

    /// The declared type parameter that `param` stands for.
    pub fn param_origin(&self, param: TypeId) -> TypeId {
        self.read(|inner| inner.param_origins.get(&param).copied())
            .unwrap_or(param)
    }

    /// The type parameter is one that a declaration writes.
    pub fn set_elements(&self, param: TypeId) {
        self.write(|inner| inner.element_params.insert(param));
    }

    /// Whether a method of the constraint of the type parameter may be
    /// met as an operator element.
    pub fn has_elements(&self, param: TypeId) -> bool {
        let origin = self.param_origin(param);
        self.read(|inner| inner.element_params.contains(&origin))
    }

    /// For the first type parameter of a method as one receiver has it:
    /// what the method names the type parameters of its receiver, and
    /// the type arguments of that receiver.
    pub fn method_receiver(&self, first: TypeId) -> Option<(Vec<TypeId>, Vec<TypeId>)> {
        self.read(|inner| inner.method_receivers.get(&first).cloned())
    }

    // ---- relations

    /// The underlying type: of a declared type the type it was declared
    /// as, of any other type itself. Invalid while the declaration of a
    /// declared type is not checked yet.
    pub fn underlying(&self, id: TypeId) -> TypeId {
        // The underlying type of a declared type is never a declared
        // type: it is that of what the type is declared as. `chain` is the
        // declared types that lead there, each declared as the next.
        let mut chain: Vec<(NamedId, TypeId, bool)> = Vec::new();
        let mut current = id;
        let underlying = loop {
            let Type::Named { id: named, args } = &*self.get(current) else {
                break current;
            };
            let (invalid, origin, declared_as, params, instance) = self.read(|inner| {
                let named = &inner.named[named.0 as usize];
                (
                    named.invalid,
                    named.underlying,
                    named.declared_as,
                    named.params.clone(),
                    inner.instances.get(&current).copied(),
                )
            });
            // A generic type with arguments is what it was declared as,
            // with the arguments in the place of its parameters.
            let generic = !args.is_empty() && params.len() == args.len();
            let known = match generic {
                true => instance,
                false => origin,
            };
            // A type that is declared as itself, through others, has no
            // underlying type.
            let again = chain.iter().any(|&(seen, ..)| seen == *named);
            match (known, declared_as) {
                _ if invalid => break Self::INVALID,
                (Some(known), _) => break known,
                (None, Some(declared_as)) if !again => {
                    chain.push((*named, current, generic));
                    current = match generic {
                        true => self.substitute(declared_as, &params, args),
                        false => declared_as,
                    };
                }
                _ => break Self::INVALID,
            }
        };
        if underlying != Self::INVALID && !chain.is_empty() {
            self.write(|inner| {
                for (named, ty, generic) in chain {
                    match generic {
                        true => drop(inner.instances.insert(ty, underlying)),
                        false => inner.named[named.0 as usize].underlying = Some(underlying),
                    }
                }
            });
        }
        underlying
    }

    pub fn is_untyped(&self, id: TypeId) -> bool {
        matches!(&*self.get(id), Type::Basic(basic) if basic.is_untyped())
    }

    pub fn is_interface(&self, id: TypeId) -> bool {
        matches!(&*self.get(self.underlying(id)), Type::Interface { .. })
    }

    /// How a value of the type `from` gets into a place of the type
    /// `to`, with the two types that say it: an option of an interface
    /// holds the interface. None if the place holds no interface, or
    /// takes the value as it is: an interface value of a type with the
    /// same methods.
    pub fn into_interface(
        &self,
        from: TypeId,
        to: TypeId,
        names: &ThreadedRodeo,
    ) -> Option<(IntoInterface, TypeId, TypeId)> {
        let held = |mut ty: TypeId| {
            while let Type::Option(elem) = &*self.get(self.underlying(ty)) {
                ty = *elem;
            }
            ty
        };
        let (from, to) = (held(from), held(to));
        if !self.is_interface(to) {
            return None;
        }
        if !self.is_interface(from) {
            return Some((IntoInterface::Put, from, to));
        }
        let changed = self.interface_methods(from, names) != self.interface_methods(to, names);
        changed.then_some((IntoInterface::Changed, from, to))
    }

    /// Whether the type has a name: a declared type, a type parameter or
    /// a basic type.
    pub fn is_named(&self, id: TypeId) -> bool {
        matches!(
            &*self.get(id),
            Type::Named { .. } | Type::Param(_) | Type::Basic(_)
        )
    }

    /// The methods of an interface with those of the interfaces it embeds,
    /// in the order of their names. `None` if `id` is no interface or
    /// holds type terms, which make it a constraint.
    pub fn interface_methods(&self, id: TypeId, names: &ThreadedRodeo) -> Option<Vec<Method>> {
        let mut all = Vec::new();
        self.method_set(id)?
            .for_each(&mut |method| all.push(method.clone()));
        all.sort_by_cached_key(|m| (names.resolve(&m.name), m.package.map(|p| names.resolve(&p))));
        Some(all)
    }

    /// Whether two types are identical.
    pub fn identical(&self, a: TypeId, b: TypeId) -> bool {
        a == b || self.identical_counted(a, b, true)
    }

    /// `identical`, but the tags of struct fields do not count: what a
    /// conversion asks of two struct types.
    pub fn identical_ignoring_tags(&self, a: TypeId, b: TypeId) -> bool {
        a == b || self.identical_counted(a, b, false)
    }

    fn identical_counted(&self, a: TypeId, b: TypeId, tags: bool) -> bool {
        let mut pairs = FxHashSet::default();
        let identical = self.identical_in(a, b, tags, &mut pairs);
        self.steps.fetch_add(pairs.len() as u64, Ordering::Relaxed);
        identical
    }

    /// Whether two types are identical, with or without the `tags` of
    /// struct fields. `pairs` is what was compared already: a pair that
    /// is met again is identical if nothing else differs. That is so for
    /// an interface that reaches itself through the types of its
    /// methods, and it compares a type that stands in several places
    /// once.
    fn identical_in(
        &self,
        a: TypeId,
        b: TypeId,
        tags: bool,
        pairs: &mut FxHashSet<(TypeId, TypeId)>,
    ) -> bool {
        a == b || deep(|| self.identical_here(a, b, tags, pairs))
    }

    fn identical_here(
        &self,
        a: TypeId,
        b: TypeId,
        tags: bool,
        pairs: &mut FxHashSet<(TypeId, TypeId)>,
    ) -> bool {
        let (x, y) = (self.get(a), self.get(b));
        // What has no parts is identical only to itself.
        let differs = match (&*x, &*y) {
            (Type::Basic(_), _)
            | (Type::Param(_), _)
            | (Type::Invalid, _)
            | (Type::Marker(_), _) => true,
            (Type::Named { id: a, .. }, Type::Named { id: b, .. }) => a != b,
            _ => std::mem::discriminant(&*x) != std::mem::discriminant(&*y),
        };
        if differs {
            return false;
        }
        if pairs.contains(&(b, a)) || !pairs.insert((a, b)) {
            return true;
        }
        // Only interfaces have more than one spelling: two types with
        // different ids are identical if they differ in interfaces alone.
        let mut same = |a: TypeId, b: TypeId| self.identical_in(a, b, tags, pairs);
        let mut all = |a: &[TypeId], b: &[TypeId]| {
            a.len() == b.len() && a.iter().zip(b).all(|(&a, &b)| same(a, b))
        };
        match (&*x, &*y) {
            (Type::Interface { .. }, Type::Interface { .. }) => {
                let (Some(x), Some(y)) = (self.method_set(a), self.method_set(b)) else {
                    return false;
                };
                let mut identical = x.len() == y.len();
                x.for_each(&mut |method| {
                    identical = identical
                        && y.get((method.name, method.package)).is_some_and(|other| {
                            self.identical_in(method.sig, other.sig, tags, pairs)
                        });
                });
                identical
            }
            (Type::Pointer(a), Type::Pointer(b))
            | (Type::Unique(a), Type::Unique(b))
            | (Type::Shared(a), Type::Shared(b))
            | (Type::Weak(a), Type::Weak(b))
            | (Type::Option(a), Type::Option(b))
            | (Type::Local(a), Type::Local(b))
            | (Type::Slice(a), Type::Slice(b)) => same(*a, *b),
            (Type::Array { len: m, elem: a }, Type::Array { len: n, elem: b }) => {
                m == n && same(*a, *b)
            }
            (Type::Map { key: k, value: v }, Type::Map { key: l, value: w }) => {
                same(*k, *l) && same(*v, *w)
            }
            (Type::Chan { dir: d, elem: a }, Type::Chan { dir: e, elem: b }) => {
                d == e && same(*a, *b)
            }
            (Type::Tuple(a), Type::Tuple(b)) => all(a, b),
            (
                Type::Func {
                    type_params: t,
                    params: p,
                    results: r,
                    variadic: v,
                },
                Type::Func {
                    type_params: u,
                    params: q,
                    results: s,
                    variadic: w,
                },
            ) => t == u && v == w && all(p, q) && all(r, s),
            (Type::Struct(a), Type::Struct(b)) => {
                a.len() == b.len()
                    && a.iter().zip(b.iter()).all(|(a, b)| {
                        a.name == b.name
                            && a.package == b.package
                            && a.embedded == b.embedded
                            && (a.tag == b.tag || !tags)
                            && a.required == b.required
                            && same(a.ty, b.ty)
                    })
            }
            (Type::Named { id: a, args: x }, Type::Named { id: b, args: y }) => a == b && all(x, y),
            _ => false,
        }
    }

    /// Whether values of the type can be compared with `==`.
    pub fn comparable(&self, id: TypeId) -> bool {
        self.comparable_counted(id, false)
    }

    /// Whether values of the type compare without a panic: no interface
    /// is among its parts.
    pub fn strictly_comparable(&self, id: TypeId) -> bool {
        self.comparable_counted(id, true)
    }

    fn comparable_counted(&self, id: TypeId, strict: bool) -> bool {
        let mut seen = FxHashSet::default();
        let comparable = self.comparable_in(id, strict, &mut seen);
        self.steps.fetch_add(seen.len() as u64, Ordering::Relaxed);
        comparable
    }

    /// `comparable`, or `strictly_comparable` if `strict`. A type that
    /// was `seen` says nothing against it: one that stands in several
    /// places is asked once, and so is one that holds a value of itself,
    /// which is an error.
    fn comparable_in(
        &self,
        id: TypeId,
        strict: bool,
        seen: &mut FxHashSet<(TypeId, bool)>,
    ) -> bool {
        deep(|| self.comparable_here(id, strict, seen))
    }

    fn comparable_here(
        &self,
        id: TypeId,
        strict: bool,
        seen: &mut FxHashSet<(TypeId, bool)>,
    ) -> bool {
        let under = self.get(self.underlying(id));
        let parts = matches!(
            &*under,
            Type::Param(_)
                | Type::Option(_)
                | Type::Local(_)
                | Type::Array { .. }
                | Type::Struct(_)
        );
        if parts && !seen.insert((id, strict)) {
            return true;
        }
        match &*under {
            Type::Basic(basic) => *basic != Basic::UntypedNil,
            // Pointers of every kind compare by address.
            Type::Pointer(_)
            | Type::Unique(_)
            | Type::Shared(_)
            | Type::Weak(_)
            | Type::Chan { .. }
            | Type::Invalid => true,
            Type::Interface { .. } => !strict,
            // A type parameter compares if every type that it may be
            // does.
            Type::Param(_) => self.param_set(id).is_some_and(|set| match &set.terms {
                Some(terms) => {
                    !terms.is_empty()
                        && terms
                            .iter()
                            .all(|term| self.comparable_in(term.ty, true, seen))
                }
                None => set.comparable,
            }),
            Type::Option(elem) | Type::Local(elem) | Type::Array { elem, .. } => {
                self.comparable_in(*elem, strict, seen)
            }
            Type::Struct(fields) => fields
                .iter()
                .all(|field| self.comparable_in(field.ty, strict, seen)),
            _ => false,
        }
    }

    /// Whether `nil` is a value of the type. A pointer, a func and an
    /// interface with methods are never nil (TYP-2).
    pub fn has_nil(&self, id: TypeId) -> bool {
        match &*self.get(self.underlying(id)) {
            Type::Pointer(_) | Type::Func { .. } => false,
            Type::Interface { methods, embedded } => {
                id == self.error || (methods.is_empty() && embedded.is_empty())
            }
            _ => self.nil_kind(id),
        }
    }

    /// Whether the type is of a kind that has a nil in Go: also a pointer,
    /// a func and an interface with methods, which are never nil here.
    pub fn nil_kind(&self, id: TypeId) -> bool {
        matches!(
            &*self.get(self.underlying(id)),
            Type::Slice(_)
                | Type::Map { .. }
                | Type::Chan { .. }
                | Type::Option(_)
                | Type::Basic(Basic::UntypedNil)
                | Type::Invalid
                | Type::Pointer(_)
                | Type::Func { .. }
                | Type::Interface { .. }
        )
    }

    /// The first method of the interface `iface`, in the order of the
    /// names, that values of the type `id` lack, and what they have in its
    /// place. `None` if they have every method, and where that is not
    /// known: `iface` is a constraint, or a type embedded in `id` is in
    /// error.
    pub fn missing_method(
        &self,
        id: TypeId,
        iface: TypeId,
        names: &ThreadedRodeo,
    ) -> Option<(Method, Lacks)> {
        self.lacks(id, &self.method_set(iface)?, names)
    }

    /// `missing_method` for the methods `wanted`. A type parameter has
    /// the methods of its constraint.
    pub fn lacks(
        &self,
        id: TypeId,
        wanted: &MethodSet,
        names: &ThreadedRodeo,
    ) -> Option<(Method, Lacks)> {
        self.lacks_with(
            id,
            wanted,
            names,
            &mut |a, b| self.identical(a, b),
            &mut |_, _| None,
        )
    }

    /// `lacks`, where `same` says whether the type of a method that the
    /// type has is the type that is wanted, and `met` whether the type
    /// meets a method in a way of its own.
    pub fn lacks_with(
        &self,
        id: TypeId,
        wanted: &MethodSet,
        names: &ThreadedRodeo,
        same: &mut dyn FnMut(TypeId, TypeId) -> bool,
        met: &mut Met<'_>,
    ) -> Option<(Method, Lacks)> {
        if wanted.is_empty() {
            return None;
        }
        self.steps.fetch_add(wanted.len() as u64, Ordering::Relaxed);
        let has = match &*self.get(id) {
            Type::Param(_) => Some(self.param_set(id)?.methods),
            _ if self.is_interface(id) => Some(self.method_set(id)?),
            _ => None,
        };
        // Of the methods that are lacking, the first by its name.
        let mut missing: Option<(Method, Lacks)> = None;
        let text = |method: &Method| {
            let package = method.package.map(|package| names.resolve(&package));
            (names.resolve(&method.name), package)
        };
        wanted.for_each(&mut |method| {
            let own = met(method, same);
            let lacks = match &has {
                _ if own == Some(true) => None,
                Some(has) => self.interface_lacks(has, method, same),
                None => self.type_lacks(id, method, names, same),
            };
            let lacks = lacks.or(own.filter(|met| !met).map(|_| Lacks::Method));
            if let Some(lacks) = lacks
                && missing
                    .as_ref()
                    .is_none_or(|(first, _)| text(method) < text(first))
            {
                missing = Some((method.clone(), lacks));
            }
        });
        missing.filter(|_| has.is_some() || !self.embeds_invalid(id, &mut FxHashSet::default()))
    }

    /// What an interface with the methods `has` lacks of `method`.
    fn interface_lacks(
        &self,
        has: &MethodSet,
        method: &Method,
        same: &mut dyn FnMut(TypeId, TypeId) -> bool,
    ) -> Option<Lacks> {
        match has.get((method.name, method.package)) {
            None => Some(Lacks::Method),
            Some(found) if same(found.sig, method.sig) => None,
            Some(found) => Some(Lacks::Type(found.clone())),
        }
    }

    /// What values of the type `id`, which is no interface, lack of
    /// `method`.
    fn type_lacks(
        &self,
        id: TypeId,
        method: &Method,
        names: &ThreadedRodeo,
        same: &mut dyn FnMut(TypeId, TypeId) -> bool,
    ) -> Option<Lacks> {
        let key = (method.name, method.package);
        let lacks = match self.find(id, key, names, false) {
            Err(NotFound::Ambiguous) => Lacks::OneMember,
            Err(NotFound::Missing) => match self.find(id, key, names, true) {
                Ok(Found {
                    member: Member::Method { sig, .. },
                    name,
                    ..
                }) => match name == method.name {
                    true => Lacks::Exported,
                    false => Lacks::Name(Method {
                        name,
                        package: None,
                        sig,
                    }),
                },
                _ => Lacks::Method,
            },
            Ok(found) => match found.member {
                Member::Field { .. } => Lacks::NoField,
                Member::Method {
                    pointer_receiver: true,
                    ..
                } => Lacks::ValueReceiver,
                Member::Method { sig, .. } if self.generic_func(sig).is_some() => Lacks::Generic,
                // The signature of a method is not known while the
                // types of its package are worked out.
                Member::Method { sig, .. } if sig == Self::INVALID || same(sig, method.sig) => {
                    return None;
                }
                Member::Method { sig, .. } => Lacks::Type(Method {
                    name: method.name,
                    package: method.package,
                    sig,
                }),
            },
        };
        Some(lacks)
    }

    /// Whether a struct of the type `id`, or one that it points to, has
    /// an embedded field whose type is in error, itself or through its
    /// embedded fields: what such a field would give is not known.
    fn embeds_invalid(&self, id: TypeId, seen: &mut FxHashSet<TypeId>) -> bool {
        let mut under = self.underlying(id);
        if let Type::Pointer(elem) = &*self.get(under) {
            under = self.underlying(*elem);
        }
        let Type::Struct(fields) = &*self.get(under) else {
            return false;
        };
        if !seen.insert(under) {
            return false;
        }
        fields.iter().filter(|field| field.embedded).any(|field| {
            self.underlying(field.ty) == Self::INVALID
                || deep(|| self.embeds_invalid(field.ty, seen))
        })
    }

    /// Whether the type `id` has every method of the interface `iface`.
    pub fn implements(&self, id: TypeId, iface: TypeId, names: &ThreadedRodeo) -> bool {
        match self.method_set(iface) {
            Some(wanted) => self.lacks(id, &wanted, names).is_none(),
            None => false,
        }
    }

    /// Whether a value of the type `from` may be assigned to a place of
    /// the type `to`. For a constant, `value` is its value: an untyped
    /// constant must fit the type. `nil` counts as a value of every kind
    /// that has one in Go ([`nil_kind`](Self::nil_kind)).
    pub fn assignable(
        &self,
        from: TypeId,
        value: Option<&Value>,
        to: TypeId,
        names: &ThreadedRodeo,
    ) -> bool {
        if from == Self::INVALID || to == Self::INVALID || self.identical(from, to) {
            return true;
        }
        let (from_under, to_under) = (self.underlying(from), self.underlying(to));
        let (from_type, to_type) = (self.get(from_under), self.get(to_under));
        if let Type::Basic(kind) = &*from_type
            && kind.is_untyped()
        {
            return self.untyped_assignable(*kind, value, to);
        }
        // Identical underlying types, and one side is not a declared type.
        let one_unnamed = !self.is_named(from) || !self.is_named(to);
        if one_unnamed && self.identical(from_under, to_under) {
            return true;
        }
        match (&*from_type, &*to_type) {
            (_, Type::Interface { .. }) => self.implements(from, to, names),
            // A channel for both directions is one for either.
            (
                Type::Chan {
                    dir: ChanDir::Both,
                    elem: a,
                },
                Type::Chan { elem: b, .. },
            ) => one_unnamed && a == b,
            // A value is its own option (TYP-2).
            (_, Type::Option(elem)) => self.assignable(from, value, *elem, names),
            _ => false,
        }
    }

    /// Whether an untyped value of the kind `kind` may be assigned to a
    /// place of the type `to`.
    fn untyped_assignable(&self, kind: Basic, value: Option<&Value>, to: TypeId) -> bool {
        if kind == Basic::UntypedNil {
            return self.nil_kind(to);
        }
        match &*self.get(self.underlying(to)) {
            Type::Basic(target) => match value {
                Some(value) => representable(value, *target).is_some(),
                // The result of a comparison, or a shift that is not
                // constant: only its kind is known.
                None => match kind {
                    Basic::UntypedBool => *target == Basic::Bool || *target == Basic::UntypedBool,
                    Basic::UntypedString => {
                        *target == Basic::String || *target == Basic::UntypedString
                    }
                    _ => !matches!(
                        target,
                        Basic::Bool
                            | Basic::String
                            | Basic::UntypedBool
                            | Basic::UntypedString
                            | Basic::UntypedNil
                    ),
                },
            },
            // A value in an interface has a type: only the empty
            // interface takes one that has none yet.
            Type::Interface { methods, embedded } => methods.is_empty() && embedded.is_empty(),
            Type::Option(elem) => self.untyped_assignable(kind, value, *elem),
            _ => false,
        }
    }

    /// The type that an untyped constant of the kind of `id` has where no
    /// type is asked for.
    pub fn default_type(&self, id: TypeId) -> TypeId {
        match &*self.get(id) {
            Type::Basic(Basic::UntypedRune) => self.rune,
            Type::Basic(basic) if basic.is_untyped() => self.basic(basic.default_type()),
            // A type that has a name keeps it.
            _ => id,
        }
    }

    // ---- text

    /// The type as a program writes it.
    pub fn display(&self, id: TypeId, names: &ThreadedRodeo) -> String {
        self.written(id, names, None)
    }

    fn written(&self, id: TypeId, names: &ThreadedRodeo, within: Option<Spur>) -> String {
        let left = Cell::new(WRITTEN_AT_MOST);
        let spelling = Spelling {
            canonical: false,
            spaced: false,
            within,
            left: &left,
        };
        let text = self.show(id, names, spelling);
        let steps = u64::from(WRITTEN_AT_MOST - left.get());
        self.steps.fetch_add(steps, Ordering::Relaxed);
        text
    }

    /// The type as a message of the package named `package` shows it: the
    /// types of that package are written without it.
    pub fn display_in(&self, id: TypeId, names: &ThreadedRodeo, package: Spur) -> String {
        self.written(id, names, Some(package))
    }

    /// The type in one spelling for all identical types: an interface is
    /// written with all its methods, in the order of their names, then its
    /// type terms.
    pub fn canonical(&self, id: TypeId, names: &ThreadedRodeo) -> String {
        // Compared with what Go writes: all of it.
        let spelling = Spelling {
            canonical: true,
            spaced: false,
            within: None,
            left: &Cell::new(u32::MAX),
        };
        self.show(id, names, spelling)
    }

    /// The type as the runtime of Go names it in a message: one spelling
    /// for all identical types, `interface {}` for the interface without
    /// methods, a struct with spaces inside its braces.
    pub fn runtime_name(&self, id: TypeId, names: &ThreadedRodeo) -> String {
        let spelling = Spelling {
            canonical: true,
            spaced: true,
            left: &Cell::new(u32::MAX),
            within: None,
        };
        self.show(id, names, spelling)
    }

    /// The methods and the type terms of an interface, each as text, with
    /// those of the interfaces it embeds.
    fn interface_parts(
        &self,
        id: TypeId,
        names: &ThreadedRodeo,
        spelling: Spelling,
    ) -> (Vec<String>, Vec<String>) {
        let (mut methods, mut terms) = (Vec::new(), Vec::new());
        let mut seen = vec![self.underlying(id)];
        let mut next = 0;
        while let Some(&current) = seen.get(next) {
            next += 1;
            let Type::Interface {
                methods: own,
                embedded,
            } = &*self.get(current)
            else {
                continue;
            };
            for method in own.iter() {
                let name = names.resolve(&method.name);
                methods.push(format!(
                    "{name}{}",
                    self.signature(method.sig, names, spelling)
                ));
            }
            for &inner in embedded.iter() {
                let under = self.underlying(inner);
                match &*self.get(under) {
                    Type::Interface { .. } if !seen.contains(&under) => seen.push(under),
                    Type::Interface { .. } => {}
                    _ => terms.push(self.show(inner, names, spelling)),
                }
            }
        }
        methods.sort();
        methods.dedup();
        terms.sort();
        terms.dedup();
        (methods, terms)
    }

    fn show(&self, id: TypeId, names: &ThreadedRodeo, spelling: Spelling) -> String {
        match spelling.left.get() {
            0 => "…".to_owned(),
            left => {
                spelling.left.set(left - 1);
                deep(|| self.show_here(id, names, spelling))
            }
        }
    }

    fn show_here(&self, id: TypeId, names: &ThreadedRodeo, spelling: Spelling) -> String {
        let show = |id: TypeId| self.show(id, names, spelling);
        let list = |ids: &[TypeId]| {
            ids.iter()
                .map(|&id| show(id))
                .collect::<Vec<_>>()
                .join(", ")
        };
        // A declared type or an alias by its name, with the name of its
        // package and its type arguments.
        let by_name = |named: NamedId, args: &[TypeId]| {
            let (name, package) = self.read(|inner| {
                let named = &inner.named[named.0 as usize];
                (named.name, named.package)
            });
            let package = package.filter(|&package| spelling.within != Some(package));
            let package = package.map_or(String::new(), |p| format!("{}.", names.resolve(&p)));
            let args = if args.is_empty() {
                String::new()
            } else {
                format!("[{}]", list(args))
            };
            format!("{package}{}{args}", names.resolve(&name))
        };
        // The one spelling for identical types names no alias.
        let id = match spelling.canonical {
            true => id.canonical(),
            false => id,
        };
        if let Some((named, args)) = self.alias_of(id) {
            return by_name(named, &args);
        }
        match &*self.get(id) {
            Type::Invalid => "invalid type".to_owned(),
            Type::Basic(basic) => basic_name(*basic).to_owned(),
            Type::Pointer(elem) => format!("*{}", show(*elem)),
            Type::Unique(elem) => format!("Unique[{}]", show(*elem)),
            Type::Shared(elem) => format!("Shared[{}]", show(*elem)),
            Type::Weak(elem) => format!("Weak[{}]", show(*elem)),
            Type::Option(elem) => format!("Option[{}]", show(*elem)),
            Type::Local(elem) => format!("Local[{}]", show(*elem)),
            Type::Array { len, elem } => format!("[{len}]{}", show(*elem)),
            Type::Slice(elem) => format!("[]{}", show(*elem)),
            Type::Map { key, value } => format!("map[{}]{}", show(*key), show(*value)),
            Type::Chan { dir, elem } => match dir {
                ChanDir::Both => format!("chan {}", show(*elem)),
                ChanDir::Send => format!("chan<- {}", show(*elem)),
                ChanDir::Recv => format!("<-chan {}", show(*elem)),
            },
            Type::Tuple(types) => format!("({})", list(types)),
            Type::Func { .. } => format!("func{}", self.signature(id, names, spelling)),
            Type::Struct(fields) => {
                let field = |f: &Field| {
                    let name = if f.embedded {
                        String::new()
                    } else {
                        format!("{} ", names.resolve(&f.name))
                    };
                    let required = if f.required { " @Required" } else { "" };
                    let tag = f.tag.as_ref().map_or(String::new(), |tag| {
                        format!(" {:?}", String::from_utf8_lossy(tag))
                    });
                    format!("{name}{}{required}{tag}", show(f.ty))
                };
                let fields: Vec<String> = fields.iter().map(field).collect();
                match (spelling.spaced, fields.is_empty()) {
                    (true, true) => "struct {}".to_owned(),
                    (true, false) => format!("struct {{ {} }}", fields.join("; ")),
                    (false, _) => format!("struct{{{}}}", fields.join("; ")),
                }
            }
            Type::Interface { methods, embedded } => {
                if methods.is_empty() && embedded.is_empty() {
                    return match (spelling.spaced, spelling.canonical) {
                        (true, _) => "interface {}".to_owned(),
                        (false, true) => "any".to_owned(),
                        (false, false) => "interface{}".to_owned(),
                    };
                }
                if spelling.canonical {
                    let (methods, terms) = self.interface_parts(id, names, spelling);
                    let parts: Vec<String> = methods.into_iter().chain(terms).collect();
                    return match (spelling.spaced, parts.is_empty()) {
                        (true, true) => "interface {}".to_owned(),
                        (true, false) => format!("interface {{ {} }}", parts.join("; ")),
                        (false, true) => "any".to_owned(),
                        (false, false) => format!("interface{{{}}}", parts.join("; ")),
                    };
                }
                let method = |m: &Method| {
                    let name = names.resolve(&m.name);
                    format!("{name}{}", self.signature(m.sig, names, spelling))
                };
                let parts: Vec<String> = methods
                    .iter()
                    .map(method)
                    .chain(embedded.iter().map(|&id| show(id)))
                    .collect();
                format!("interface{{{}}}", parts.join("; "))
            }
            Type::Union(terms) => {
                let term = |t: &Term| format!("{}{}", if t.tilde { "~" } else { "" }, show(t.ty));
                terms.iter().map(term).collect::<Vec<_>>().join(" | ")
            }
            Type::Marker(Marker::Comparable) => "comparable".to_owned(),
            Type::Marker(marker) => format!("{marker:?}"),
            Type::Named { id: named, args } => by_name(*named, args),
            Type::Param(param) => {
                let name = self.read(|inner| inner.params[param.0 as usize].0);
                names.resolve(&name).to_owned()
            }
        }
    }

    /// `(int, ...string) (bool, error)`: the parameters and results of the
    /// function type `id`.
    fn signature(&self, id: TypeId, names: &ThreadedRodeo, spelling: Spelling) -> String {
        let Type::Func {
            params,
            results,
            variadic,
            ..
        } = &*self.get(id)
        else {
            return String::new();
        };
        let mut shown: Vec<String> = params
            .iter()
            .map(|&p| self.show(p, names, spelling))
            .collect();
        if *variadic
            && let Some((last, &ty)) = shown.last_mut().zip(params.last())
            && let Type::Slice(elem) = &*self.get(ty)
        {
            *last = format!("...{}", self.show(*elem, names, spelling));
        }
        let results = match &results[..] {
            [] => String::new(),
            [one] => format!(" {}", self.show(*one, names, spelling)),
            many => {
                let many: Vec<String> = many
                    .iter()
                    .map(|&r| self.show(r, names, spelling))
                    .collect();
                format!(" ({})", many.join(", "))
            }
        };
        format!("({}){results}", shown.join(", "))
    }
}

fn basic_name(basic: Basic) -> &'static str {
    match basic {
        Basic::Bool => "bool",
        Basic::Int => "int",
        Basic::Int8 => "int8",
        Basic::Int16 => "int16",
        Basic::Int32 => "int32",
        Basic::Int64 => "int64",
        Basic::Uint => "uint",
        Basic::Uint8 => "uint8",
        Basic::Uint16 => "uint16",
        Basic::Uint32 => "uint32",
        Basic::Uint64 => "uint64",
        Basic::Uintptr => "uintptr",
        Basic::Float32 => "float32",
        Basic::Float64 => "float64",
        Basic::Complex64 => "complex64",
        Basic::Complex128 => "complex128",
        Basic::String => "string",
        Basic::UntypedBool => "untyped bool",
        Basic::UntypedInt => "untyped int",
        Basic::UntypedRune => "untyped rune",
        Basic::UntypedFloat => "untyped float",
        Basic::UntypedComplex => "untyped complex",
        Basic::UntypedString => "untyped string",
        Basic::UntypedNil => "untyped nil",
    }
}

mod category;
mod constant;
mod fork;
mod hierarchy;
mod label;
mod methodset;
#[cfg(test)]
mod tests;
mod typeset;
mod zero;

pub use category::{Category, CopyRule, NotCopied};
pub use constant::NotConstant;
pub use fork::Imported;
pub use hierarchy::{Held, Lends, PARENT};
pub use label::{Labels, Unlabeled};
pub use methodset::MethodSet;
pub use typeset::{NoCommon, TypeSet};
pub use zero::NoZero;
