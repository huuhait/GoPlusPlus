//! The names of what the checker knows: its types as nodes, and the
//! symbols of functions, methods and variables.

use std::fmt;
use std::sync::Arc;

use gopp_sema::basic::Basic as CheckedBasic;
use gopp_sema::types::{
    ChanDir, DeclaredMethod, Home, Marker, Marks, NamedId, Type, TypeId, TypeSet, Types,
};
use gopp_sema::{ObjId, ObjKind, Package};
use lasso::{Spur, ThreadedRodeo};
use rustc_hash::FxHashMap;

use crate::node::{BadNode, Basic, Context, Dir, Node, NodeId, Pair, Required, Wrap};

/// What a symbol is asked for.
#[derive(Clone, Copy)]
pub enum Entity<'a> {
    /// A function or a variable of the package.
    Object(&'a Package, ObjId),
    /// A generic function of the package with its type arguments.
    FuncInstance(&'a Package, ObjId, &'a [TypeId]),
    /// The method `name` of the declared type `receiver`, or of an
    /// instance of one.
    Method { receiver: TypeId, name: Spur },
    /// An `@Operator` method, as its type declares it.
    Operator {
        receiver: TypeId,
        method: &'a DeclaredMethod,
    },
    /// What runs the initializers and the `init` functions of the
    /// package.
    PkgInit(&'a Package),
    /// The `init` function with the number, from 0 in the order of the
    /// names of the files, then of the source.
    InitFunc(&'a Package, u64),
    /// The type descriptor.
    TypeInfo(TypeId),
    /// The function that makes a value of the type a copy of its own.
    CloneGlue(TypeId),
    /// The function that says whether two values of the type are equal.
    EqGlue(TypeId),
    /// The function that gives a value of the type its hash.
    HashGlue(TypeId),
    /// The function that ends a value of the type.
    DropGlue(TypeId),
    /// The code of a function of a package that is used as a value: the
    /// function with an environment in front, which it does not read.
    FuncValue(&'a Package, ObjId),
    /// The same for a generic function with its type arguments.
    InstanceValue(&'a Package, ObjId, &'a [TypeId]),
    /// The code of the method value `x.name` for an `x` of the declared
    /// type `receiver`: the method with the receiver in its environment.
    MethodValue { receiver: TypeId, name: Spur },
    /// The code of the method expression `T.name`: the method as a
    /// function that takes its receiver first. With `pointer`, that of
    /// `(*T).name` for a method that takes its receiver by value.
    MethodExpr {
        receiver: TypeId,
        name: Spur,
        pointer: bool,
    },
    /// The methods of a type that the tables of interfaces are made of
    /// while a program runs.
    MethodTable(TypeId),
    /// The methods of an interface type, for those tables.
    IfaceDesc(TypeId),
    /// The table of the methods of an interface type for the values of
    /// a type: the type, then the interface type.
    ITab(TypeId, TypeId),
    /// What a table of methods holds for the method `name` of the
    /// declared type `receiver`: in the table of the type itself, or in
    /// that of the pointers to it if `pointer` is set.
    IfaceThunk {
        receiver: TypeId,
        name: Spur,
        pointer: bool,
    },
}

/// For a type that is declared in a function: the function, and which
/// of the types of its name there it is, from 0. The checker keeps
/// neither.
pub type Locals<'a> = dyn Fn(NamedId) -> Option<(Entity<'a>, u64)> + Sync + 'a;

/// Why something has no symbol.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NoSymbol {
    /// Generic code: only its instances are named.
    Generic,
    /// A type declared in a function of which nothing is known.
    Local,
    /// A constraint, which is the type of no value.
    Constraint,
    /// Named `_`: there is none.
    Blank,
    /// A type in error, or one that is not checked.
    Invalid,
    /// The type of a constant without a type.
    Untyped,
    /// A type whose package has no import path.
    NoPath,
    /// A constant, a type, an import or a macro: nothing is emitted.
    NotEmitted,
    Shape(BadNode),
}

impl fmt::Display for NoSymbol {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            NoSymbol::Generic => "generic code has no symbol, only its instances have",
            NoSymbol::Local => "a type declared in a function is named with that function",
            NoSymbol::Constraint => "a constraint is the type of no value",
            NoSymbol::Blank => "what is named _ does not exist",
            NoSymbol::Invalid => "the type is in error",
            NoSymbol::Untyped => "a constant without a type has no type to name",
            NoSymbol::NoPath => "the package of the type has no import path",
            NoSymbol::NotEmitted => "only functions and variables have symbols",
            NoSymbol::Shape(bad) => bad.0,
        })
    }
}

impl std::error::Error for NoSymbol {}

/// A method of an interface: the path of the package of a name that is
/// not exported, the name, and the type.
type IMethod = (Option<Box<[u8]>>, Box<[u8]>, TypeId);

/// A type whose node is being made.
enum Visit {
    Enter(TypeId),
    /// Its parts are made.
    Leave(TypeId, Shape),
}

/// What is known of a type when its parts are asked for, so that the
/// table of the checker is asked once.
struct Shape {
    kind: Arc<Type>,
    /// For an interface: its methods in the order of a name, and whether
    /// it embeds `Sync`.
    interface: Option<(Vec<IMethod>, bool)>,
    /// For a declared type: where it is declared.
    home: Option<Home>,
}

/// Makes the nodes of the checker's types, each once.
pub struct Namer<'a> {
    types: &'a Types,
    names: &'a ThreadedRodeo,
    locals: Option<&'a Locals<'a>>,
    context: Context,
    made: FxHashMap<TypeId, Result<NodeId, NoSymbol>>,
    /// The text of the names that were asked for: the table of names is
    /// shared by all threads.
    texts: FxHashMap<Spur, Box<[u8]>>,
    /// The type parameters that are written by their index: those of
    /// the receiver of an `@Operator` method.
    params: Vec<TypeId>,
    steps: u64,
}

/// The symbol of the entity, or why it has none.
pub fn symbol_of(namer: &mut Namer<'_>, entity: Entity<'_>) -> Result<String, NoSymbol> {
    let node = namer.entity(entity)?;
    Ok(namer.context.symbol(node))
}

impl<'a> Namer<'a> {
    /// `names` is the interner of `types`.
    pub fn new(types: &'a Types, names: &'a ThreadedRodeo) -> Self {
        Self {
            types,
            names,
            locals: None,
            context: Context::new(),
            made: FxHashMap::default(),
            texts: FxHashMap::default(),
            params: Vec::new(),
            steps: 0,
        }
    }

    /// With what says where the types of functions are declared; without
    /// it they have no names.
    pub fn with_locals(mut self, locals: &'a Locals<'a>) -> Self {
        self.locals = Some(locals);
        self
    }

    /// How many steps making nodes took so far.
    pub fn steps(&self) -> u64 {
        self.steps
    }

    fn text(&mut self, name: Spur) -> Box<[u8]> {
        let names = self.names;
        let text = self.texts.entry(name);
        text.or_insert_with(|| names.resolve(&name).as_bytes().into())
            .clone()
    }

    fn add(&mut self, node: Node) -> Result<NodeId, NoSymbol> {
        self.context.add(node).map_err(NoSymbol::Shape)
    }

    /// The node of the entity.
    pub fn entity(&mut self, entity: Entity<'_>) -> Result<NodeId, NoSymbol> {
        let node = match entity {
            Entity::Object(package, id) => {
                let object = package.object(id);
                let (package, name) = (package.path.as_bytes().into(), self.text(object.name));
                if &*name == b"_" {
                    return Err(NoSymbol::Blank);
                }
                match object.kind {
                    ObjKind::Func if self.types.generic_func(object.ty).is_some() => {
                        return Err(NoSymbol::Generic);
                    }
                    ObjKind::Func => Node::Func { package, name },
                    ObjKind::Var => Node::Var { package, name },
                    _ => return Err(NoSymbol::NotEmitted),
                }
            }
            Entity::FuncInstance(package, id, args) => {
                let object = package.object(id);
                let (package, name) = (package.path.as_bytes().into(), self.text(object.name));
                if &*name == b"_" {
                    return Err(NoSymbol::Blank);
                }
                let of = self.add(Node::Func { package, name })?;
                let args: Result<Box<[NodeId]>, NoSymbol> =
                    args.iter().map(|&arg| self.ty(arg)).collect();
                Node::Instance { of, args: args? }
            }
            Entity::Method { receiver, name } => {
                let name = self.text(name);
                if &*name == b"_" {
                    return Err(NoSymbol::Blank);
                }
                let receiver = self.ty(receiver)?;
                Node::Method { name, receiver }
            }
            Entity::Operator { receiver, method } => {
                let receiver = self.ty(receiver)?;
                let params = match &*self.types.get(method.method.sig) {
                    Type::Func { params, .. } => params.clone(),
                    _ => return Err(NoSymbol::Invalid),
                };
                // Under the type parameters of the receiver a type has
                // another node than without them.
                let outer = std::mem::take(&mut self.made);
                self.params.clone_from(&method.receiver_params);
                let params: Result<Box<[NodeId]>, NoSymbol> =
                    params.iter().map(|&param| self.ty(param)).collect();
                self.params.clear();
                self.made = outer;
                Node::OpMethod {
                    right: method.marks.has(Marks::RIGHT),
                    name: self.text(method.method.name),
                    receiver,
                    params: self.add(Node::Tuple(params?))?,
                }
            }
            Entity::PkgInit(package) => Node::PkgInit(package.path.as_bytes().into()),
            Entity::InitFunc(package, ordinal) => Node::InitFunc {
                package: package.path.as_bytes().into(),
                ordinal,
            },
            Entity::TypeInfo(ty) => Node::Wrap(Wrap::TypeInfo, self.ty(ty)?),
            Entity::CloneGlue(ty) => Node::Wrap(Wrap::CloneGlue, self.ty(ty)?),
            Entity::EqGlue(ty) => Node::Wrap(Wrap::EqGlue, self.ty(ty)?),
            Entity::HashGlue(ty) => Node::Wrap(Wrap::HashGlue, self.ty(ty)?),
            Entity::DropGlue(ty) => Node::Wrap(Wrap::DropGlue, self.ty(ty)?),
            Entity::FuncValue(package, id) => {
                let function = self.entity(Entity::Object(package, id))?;
                Node::Wrap(Wrap::MethodExpr, function)
            }
            Entity::InstanceValue(package, id, args) => {
                let function = self.entity(Entity::FuncInstance(package, id, args))?;
                Node::Wrap(Wrap::MethodExpr, function)
            }
            Entity::MethodValue { receiver, name } => {
                let method = self.entity(Entity::Method { receiver, name })?;
                Node::Wrap(Wrap::MethodValue, method)
            }
            Entity::MethodExpr {
                receiver,
                name,
                pointer,
            } => {
                let mut method = self.entity(Entity::Method { receiver, name })?;
                if pointer {
                    method = self.add(Node::Wrap(Wrap::IfacePtrThunk, method))?;
                }
                Node::Wrap(Wrap::MethodExpr, method)
            }
            Entity::MethodTable(ty) => Node::Wrap(Wrap::MethodTable, self.ty(ty)?),
            Entity::IfaceDesc(ty) => Node::Wrap(Wrap::IfaceDesc, self.ty(ty)?),
            Entity::ITab(ty, iface) => Node::Pair(Pair::ITab, self.ty(ty)?, self.ty(iface)?),
            Entity::IfaceThunk {
                receiver,
                name,
                pointer,
            } => {
                let method = self.entity(Entity::Method { receiver, name })?;
                let wrap = match pointer {
                    true => Wrap::IfacePtrThunk,
                    false => Wrap::IfaceThunk,
                };
                Node::Wrap(wrap, method)
            }
        };
        self.add(node)
    }

    /// The node of the function literal with the number `ordinal`,
    /// from 1 in the order of the source, among those that are written
    /// directly in what `within` is the node of.
    pub fn closure(&mut self, within: NodeId, ordinal: u64) -> Result<NodeId, NoSymbol> {
        self.add(Node::Closure { ordinal, within })
    }

    /// The symbol of the entity with the node.
    pub fn symbol(&self, entity: NodeId) -> String {
        self.context.symbol(entity)
    }

    /// The node of the type: one for all identical types.
    pub fn ty(&mut self, ty: TypeId) -> Result<NodeId, NoSymbol> {
        let mut rest = vec![Visit::Enter(ty)];
        while let Some(visit) = rest.pop() {
            self.steps += 1;
            let made = match visit {
                Visit::Enter(ty) if self.made.contains_key(&ty) => continue,
                Visit::Enter(ty) => match self.parts(ty) {
                    Ok((parts, shape)) => {
                        rest.push(Visit::Leave(ty, shape));
                        rest.extend(parts.into_iter().map(Visit::Enter));
                        continue;
                    }
                    Err(none) => (ty, Err(none)),
                },
                Visit::Leave(ty, shape) => (ty, self.node(ty, shape)),
            };
            self.made.insert(made.0, made.1);
        }
        self.made
            .get(&ty)
            .copied()
            .unwrap_or(Err(NoSymbol::Invalid))
    }

    fn part(&self, ty: TypeId) -> Result<NodeId, NoSymbol> {
        self.made
            .get(&ty)
            .copied()
            .unwrap_or(Err(NoSymbol::Invalid))
    }

    /// The types whose nodes the node of `ty` is made of, with what
    /// was asked of the checker on the way.
    fn parts(&mut self, ty: TypeId) -> Result<(Vec<TypeId>, Shape), NoSymbol> {
        let kind = self.types.get(ty);
        let (mut interface, mut home) = (None, None);
        let parts = match &*kind {
            Type::Invalid => return Err(NoSymbol::Invalid),
            Type::Basic(basic) if basic.is_untyped() => return Err(NoSymbol::Untyped),
            Type::Union(_) | Type::Marker(_) => return Err(NoSymbol::Constraint),
            Type::Param(_) if !self.params.contains(&ty) => return Err(NoSymbol::Generic),
            Type::Func { type_params, .. } if !type_params.is_empty() => {
                return Err(NoSymbol::Generic);
            }
            Type::Interface { .. } => {
                let set = self.value_set(ty)?;
                let mut methods: Vec<IMethod> = Vec::new();
                set.methods.for_each(&mut |method| {
                    let package = method.package.map(|package| self.text(package));
                    methods.push((package, self.text(method.name), method.sig));
                });
                self.steps += methods.len() as u64;
                methods.sort();
                let parts = methods.iter().map(|&(_, _, sig)| sig).collect();
                interface = Some((methods, !set.markers.is_empty()));
                parts
            }
            Type::Named { id, args } => {
                let declared = self.types.home(*id);
                if declared.invalid {
                    return Err(NoSymbol::Invalid);
                }
                if declared.generic && args.is_empty() {
                    return Err(NoSymbol::Generic);
                }
                if self.types.is_interface(ty) {
                    self.value_set(ty)?;
                }
                home = Some(declared);
                args.to_vec()
            }
            other => {
                let mut parts = Vec::new();
                other.each_part(&mut |part| parts.push(part));
                parts
            }
        };
        let shape = Shape {
            kind,
            interface,
            home,
        };
        Ok((parts, shape))
    }

    /// The type set of an interface that is the type of values: it may
    /// embed `Sync` and nothing else that makes it a constraint.
    fn value_set(&self, ty: TypeId) -> Result<TypeSet, NoSymbol> {
        let set = self.types.type_set(ty).ok_or(NoSymbol::Invalid)?;
        let only_sync = set.markers.iter().all(|&marker| marker == Marker::Sync);
        match set.terms.is_some() || set.comparable || !only_sync {
            true => Err(NoSymbol::Constraint),
            false => Ok(set),
        }
    }

    /// The node of a type whose parts are made.
    fn node(&mut self, ty: TypeId, shape: Shape) -> Result<NodeId, NoSymbol> {
        let Shape {
            kind,
            interface,
            home,
        } = shape;
        let list = |namer: &Self, types: &[TypeId]| -> Result<Box<[NodeId]>, NoSymbol> {
            types.iter().map(|&part| namer.part(part)).collect()
        };
        let node = match &*kind {
            Type::Basic(basic) => Node::Basic(match basic {
                CheckedBasic::Bool => Basic::Bool,
                CheckedBasic::Int => Basic::Int,
                CheckedBasic::Int8 => Basic::Int8,
                CheckedBasic::Int16 => Basic::Int16,
                CheckedBasic::Int32 => Basic::Int32,
                CheckedBasic::Int64 => Basic::Int64,
                CheckedBasic::Uint => Basic::Uint,
                CheckedBasic::Uint8 => Basic::Uint8,
                CheckedBasic::Uint16 => Basic::Uint16,
                CheckedBasic::Uint32 => Basic::Uint32,
                CheckedBasic::Uint64 => Basic::Uint64,
                CheckedBasic::Uintptr => Basic::Uintptr,
                CheckedBasic::Float32 => Basic::Float32,
                CheckedBasic::Float64 => Basic::Float64,
                CheckedBasic::Complex64 => Basic::Complex64,
                CheckedBasic::Complex128 => Basic::Complex128,
                CheckedBasic::String => Basic::String,
                _ => return Err(NoSymbol::Untyped),
            }),
            Type::Pointer(of) => Node::Pointer(self.part(*of)?),
            Type::Slice(of) => Node::Slice(self.part(*of)?),
            Type::Option(of) => Node::Option(self.part(*of)?),
            Type::Array { len, elem } => Node::Array(*len, self.part(*elem)?),
            Type::Map { key, value } => Node::Map(self.part(*key)?, self.part(*value)?),
            Type::Chan { dir, elem } => {
                let dir = match dir {
                    ChanDir::Both => Dir::Both,
                    ChanDir::Send => Dir::Send,
                    ChanDir::Recv => Dir::Recv,
                };
                Node::Chan(dir, self.part(*elem)?)
            }
            Type::Unique(of) | Type::Shared(of) | Type::Weak(of) | Type::Local(of) => {
                let name: &[u8] = match &*kind {
                    Type::Unique(_) => b"Unique",
                    Type::Shared(_) => b"Shared",
                    Type::Weak(_) => b"Weak",
                    _ => b"Local",
                };
                let args = Box::new([self.part(*of)?]);
                let of = self.add(Node::Universe(name.into()))?;
                Node::Instance { of, args }
            }
            Type::Tuple(types) => Node::Tuple(list(self, types)?),
            Type::Func {
                params,
                results,
                variadic,
                ..
            } => {
                let (params, results) = (list(self, params)?, list(self, results)?);
                Node::Signature {
                    variadic: *variadic,
                    params: self.add(Node::Tuple(params))?,
                    results: self.add(Node::Tuple(results))?,
                }
            }
            Type::Struct(fields) => {
                let mut nodes = Vec::with_capacity(fields.len());
                for field in fields {
                    self.steps += 1;
                    let required = match field.required {
                        true => Required::Yes,
                        false => Required::No,
                    };
                    let node = Node::Field {
                        name: self.text(field.name),
                        package: field.package.map(|package| self.text(package)),
                        embedded: field.embedded,
                        required,
                        parent: false,
                        tag: field.tag.clone(),
                        ty: self.part(field.ty)?,
                    };
                    nodes.push(self.add(node)?);
                }
                Node::Struct(nodes.into())
            }
            Type::Interface { .. } => {
                let (methods, sync) = interface.ok_or(NoSymbol::Invalid)?;
                let mut nodes = Vec::with_capacity(methods.len());
                for (package, name, sig) in methods {
                    let sig = self.part(sig)?;
                    nodes.push(self.add(Node::IMethod { package, name, sig })?);
                }
                Node::Interface {
                    sync,
                    methods: nodes.into(),
                }
            }
            Type::Named { id, args } => {
                let args = list(self, args)?;
                let of = self.declared(*id, home.ok_or(NoSymbol::Invalid)?)?;
                match args.is_empty() {
                    true => return Ok(of),
                    false => Node::Instance { of, args },
                }
            }
            Type::Param(_) => {
                let index = self.params.iter().position(|&param| param == ty);
                Node::TypeParam(index.ok_or(NoSymbol::Generic)? as u64)
            }
            Type::Invalid | Type::Union(_) | Type::Marker(_) => return Err(NoSymbol::Invalid),
        };
        self.add(node)
    }

    /// The node of a declared type without its type arguments.
    fn declared(&mut self, named: NamedId, home: Home) -> Result<NodeId, NoSymbol> {
        let name = self.text(home.name);
        if home.package.is_none() {
            return self.add(Node::Universe(name));
        }
        if home.local {
            let (within, ordinal) = self
                .locals
                .and_then(|locals| locals(named))
                .ok_or(NoSymbol::Local)?;
            let within = self.entity(within)?;
            return self.add(Node::LocalNamed {
                ordinal,
                name,
                within,
            });
        }
        let package = self.text(home.path.ok_or(NoSymbol::NoPath)?);
        match (&*package, &*name) {
            (b"unsafe", b"Pointer") => self.add(Node::Basic(Basic::UnsafePointer)),
            _ => self.add(Node::Named { package, name }),
        }
    }
}
