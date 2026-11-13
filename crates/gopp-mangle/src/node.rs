//! What a name is made of: the nodes of appendix A.4, each kept once.

use std::sync::Arc;

use rustc_hash::FxHashMap;

use crate::ident;

/// A node of a [`Context`]. Two are equal exactly when they are the same
/// entity or the same type.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct NodeId(u32);

impl NodeId {
    fn index(self) -> usize {
        self.0 as usize
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Basic {
    Bool,
    Int,
    Int8,
    Int16,
    Int32,
    Int64,
    Uint,
    Uint8,
    Uint16,
    Uint32,
    Uint64,
    Uintptr,
    Float32,
    Float64,
    Complex64,
    Complex128,
    String,
    UnsafePointer,
}

impl Basic {
    pub const ALL: [Basic; 18] = [
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
        Basic::UnsafePointer,
    ];

    pub fn tag(self) -> char {
        match self {
            Basic::Bool => 'b',
            Basic::Int => 'i',
            Basic::Int8 => 'a',
            Basic::Int16 => 's',
            Basic::Int32 => 'l',
            Basic::Int64 => 'x',
            Basic::Uint => 'j',
            Basic::Uint8 => 'h',
            Basic::Uint16 => 't',
            Basic::Uint32 => 'm',
            Basic::Uint64 => 'y',
            Basic::Uintptr => 'z',
            Basic::Float32 => 'f',
            Basic::Float64 => 'd',
            Basic::Complex64 => 'c',
            Basic::Complex128 => 'e',
            Basic::String => 'r',
            Basic::UnsafePointer => 'p',
        }
    }

    pub fn name(self) -> &'static str {
        match self {
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
            Basic::UnsafePointer => "unsafe.Pointer",
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Dir {
    Both,
    Send,
    Recv,
}

/// Whether a field must be set in a literal.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Required {
    No,
    /// `@Required`.
    Yes,
    /// The field of an instance, declared with a type parameter: it must
    /// be set, and is no `@Required` field otherwise.
    MustSet,
}

/// What is made for a type, or around an entity: the nodes with one
/// child and no header.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Wrap {
    TypeInfo,
    VTable,
    EqGlue,
    HashGlue,
    DropGlue,
    CloneGlue,
    MethodTable,
    IfaceDesc,
    CabiThunk,
    MethodValue,
    MethodExpr,
    Promoted,
    IfaceThunk,
    IfacePtrThunk,
}

impl Wrap {
    pub const ALL: [Wrap; 14] = [
        Wrap::TypeInfo,
        Wrap::VTable,
        Wrap::EqGlue,
        Wrap::HashGlue,
        Wrap::DropGlue,
        Wrap::CloneGlue,
        Wrap::MethodTable,
        Wrap::IfaceDesc,
        Wrap::CabiThunk,
        Wrap::MethodValue,
        Wrap::MethodExpr,
        Wrap::Promoted,
        Wrap::IfaceThunk,
        Wrap::IfacePtrThunk,
    ];

    pub fn tag(self) -> &'static str {
        match self {
            Wrap::TypeInfo => "Ti",
            Wrap::VTable => "Tv",
            Wrap::EqGlue => "Te",
            Wrap::HashGlue => "Th",
            Wrap::DropGlue => "Td",
            Wrap::CloneGlue => "Tc",
            Wrap::MethodTable => "Tm",
            Wrap::IfaceDesc => "Tn",
            Wrap::CabiThunk => "TC",
            Wrap::MethodValue => "TV",
            Wrap::MethodExpr => "TE",
            Wrap::Promoted => "TP",
            Wrap::IfaceThunk => "TI",
            Wrap::IfacePtrThunk => "Tp",
        }
    }

    /// Whether the child is a type; else it is an entity.
    fn of_type(self) -> bool {
        use Wrap::*;
        matches!(
            self,
            TypeInfo | VTable | EqGlue | HashGlue | DropGlue | CloneGlue | MethodTable | IfaceDesc
        )
    }
}

/// The tables that two types have together.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Pair {
    /// The child type, then the parent.
    SecondaryVTable,
    /// The concrete type, then the interface.
    ITab,
}

impl Pair {
    pub fn tag(self) -> &'static str {
        match self {
            Pair::SecondaryVTable => "Ts",
            Pair::ITab => "Ta",
        }
    }
}

/// One node. Names and package paths are bytes: a name is any text, and
/// a path its segments with `/` between them.
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub enum Node {
    Basic(Basic),
    Pointer(NodeId),
    Slice(NodeId),
    Array(u64, NodeId),
    Map(NodeId, NodeId),
    Option(NodeId),
    Chan(Dir, NodeId),
    Tuple(Box<[NodeId]>),
    /// The parameters and the results, both tuples. The last parameter
    /// of a variadic one is its slice.
    Signature {
        variadic: bool,
        params: NodeId,
        results: NodeId,
    },
    /// Its fields.
    Struct(Box<[NodeId]>),
    Field {
        name: Box<[u8]>,
        /// The path of the package of a name that is not exported.
        package: Option<Box<[u8]>>,
        embedded: bool,
        required: Required,
        /// The hidden part of a parent in a type with parents.
        parent: bool,
        tag: Option<Box<[u8]>>,
        ty: NodeId,
    },
    /// Its methods, in the order of their packages, then their names.
    Interface {
        sync: bool,
        methods: Box<[NodeId]>,
    },
    IMethod {
        /// As for a field.
        package: Option<Box<[u8]>>,
        name: Box<[u8]>,
        sig: NodeId,
    },
    Named {
        package: Box<[u8]>,
        name: Box<[u8]>,
    },
    /// A type declared in the code of `within`: the `ordinal`th of its
    /// name there, from 0.
    LocalNamed {
        ordinal: u64,
        name: Box<[u8]>,
        within: NodeId,
    },
    /// A predeclared type.
    Universe(Box<[u8]>),
    /// A generic type, function or method with its type arguments.
    Instance {
        of: NodeId,
        args: Box<[NodeId]>,
    },
    TypeParam(u64),
    Func {
        package: Box<[u8]>,
        name: Box<[u8]>,
    },
    Method {
        name: Box<[u8]>,
        receiver: NodeId,
    },
    /// An `@Operator` method, with its declared parameters, a tuple.
    OpMethod {
        right: bool,
        name: Box<[u8]>,
        receiver: NodeId,
        params: NodeId,
    },
    Var {
        package: Box<[u8]>,
        name: Box<[u8]>,
    },
    /// The `ordinal`th function literal directly in `within`, from 1.
    Closure {
        ordinal: u64,
        within: NodeId,
    },
    PkgInit(Box<[u8]>),
    InitFunc {
        package: Box<[u8]>,
        ordinal: u64,
    },
    Wrap(Wrap, NodeId),
    Pair(Pair, NodeId, NodeId),
    Data(Box<[u8]>),
    BaseAdjustThunk(i64, NodeId),
}

/// Where a node may stand.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Class {
    Type,
    Entity,
    Field,
    IMethod,
}

/// Why a node is none that a name can hold.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct BadNode(pub &'static str);

impl std::fmt::Display for BadNode {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.0)
    }
}

impl std::error::Error for BadNode {}

struct Entry {
    node: Arc<Node>,
    /// How long the node is when it is written out with all below it.
    size: u64,
    class: Class,
}

/// The size from which a type that occurs again is written as a back
/// reference.
pub(crate) const REFERRED_FROM: u64 = 8;

/// The nodes of the names that are made together. A node is kept once,
/// so the order in which they were made is no part of a name.
#[derive(Default)]
pub struct Context {
    entries: Vec<Entry>,
    ids: FxHashMap<Arc<Node>, NodeId>,
}

impl Context {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn get(&self, id: NodeId) -> &Node {
        &self.entries[id.index()].node
    }

    pub(crate) fn class(&self, id: NodeId) -> Class {
        self.entries[id.index()].class
    }

    /// Whether the node is what a symbol names.
    pub fn is_entity(&self, id: NodeId) -> bool {
        self.class(id) == Class::Entity
    }

    fn is_type(&self, id: NodeId) -> bool {
        self.class(id) == Class::Type
    }

    /// Whether the node is written as a back reference where it occurs
    /// again in a name.
    pub(crate) fn is_referred(&self, id: NodeId) -> bool {
        let entry = &self.entries[id.index()];
        entry.class == Class::Type && entry.size >= REFERRED_FROM
    }

    /// The node, made once. Its children are of this context.
    pub fn add(&mut self, node: Node) -> Result<NodeId, BadNode> {
        if let Some(&known) = self.ids.get(&node) {
            return Ok(known);
        }
        let class = self.check(&node)?;
        let mut head = String::new();
        node.head(&mut head);
        let mut size = head.len() as u64 + u64::from(node.closes());
        node.each_child(&mut |child| size = size.saturating_add(self.entries[child.index()].size));
        let id = NodeId(self.entries.len() as u32);
        let node = Arc::new(node);
        self.entries.push(Entry {
            node: node.clone(),
            size,
            class,
        });
        self.ids.insert(node, id);
        Ok(id)
    }

    /// Whether code of its own can be inside the entity.
    fn holds_code(&self, id: NodeId) -> bool {
        self.is_entity(id)
            && matches!(
                self.get(id),
                Node::Func { .. }
                    | Node::Method { .. }
                    | Node::OpMethod { .. }
                    | Node::Instance { .. }
                    | Node::Var { .. }
                    | Node::Closure { .. }
                    | Node::InitFunc { .. }
                    | Node::PkgInit(_)
            )
    }

    fn is_method(&self, id: NodeId) -> bool {
        match self.get(id) {
            Node::Method { .. } | Node::OpMethod { .. } => true,
            Node::Instance { of, .. } => {
                matches!(self.get(*of), Node::Method { .. } | Node::OpMethod { .. })
            }
            _ => false,
        }
    }

    /// Whether a thunk can be made around the entity.
    fn is_wrapped(&self, id: NodeId) -> bool {
        use Wrap::*;
        match self.get(id) {
            Node::Func { .. }
            | Node::Method { .. }
            | Node::OpMethod { .. }
            | Node::Closure { .. }
            | Node::BaseAdjustThunk(..) => true,
            Node::Instance { .. } => self.is_entity(id),
            Node::Wrap(wrap, _) => matches!(
                wrap,
                CabiThunk | MethodValue | MethodExpr | Promoted | IfaceThunk | IfacePtrThunk
            ),
            _ => false,
        }
    }

    /// Whether methods are declared for the type.
    fn has_methods(&self, id: NodeId) -> bool {
        match self.get(id) {
            Node::Named { .. } | Node::LocalNamed { .. } => true,
            Node::Instance { .. } => self.is_type(id),
            _ => false,
        }
    }

    /// Where the node may stand, or why it is none.
    fn check(&self, node: &Node) -> Result<Class, BadNode> {
        let class = |id: NodeId| self.class(id);
        let ty = |id: NodeId| match class(id) {
            Class::Type => Ok(()),
            _ => Err(BadNode("a type is expected")),
        };
        let all = |ids: &[NodeId], wanted: Class, message: &'static str| match ids
            .iter()
            .all(|&id| class(id) == wanted)
        {
            true => Ok(()),
            false => Err(BadNode(message)),
        };
        let tuple = |id: NodeId, message: &'static str| match self.get(id) {
            Node::Tuple(_) => Ok(()),
            _ => Err(BadNode(message)),
        };
        let receiver = |id: NodeId| match self.has_methods(id) {
            true => Ok(()),
            false => Err(BadNode("the receiver of a method is no declared type")),
        };
        let code = |id: NodeId| match self.holds_code(id) {
            true => Ok(()),
            false => Err(BadNode("nothing is declared inside this entity")),
        };
        match node {
            Node::Basic(_) | Node::Named { .. } | Node::Universe(_) | Node::TypeParam(_) => {}
            Node::Pointer(of) | Node::Slice(of) | Node::Array(_, of) | Node::Option(of) => ty(*of)?,
            Node::Chan(_, of) => ty(*of)?,
            Node::Map(key, value) => {
                ty(*key)?;
                ty(*value)?;
            }
            Node::Tuple(types) => all(types, Class::Type, "a type is expected")?,
            Node::Signature {
                variadic,
                params,
                results,
            } => {
                tuple(*params, "the parameters of a function type are no tuple")?;
                tuple(*results, "the results of a function type are no tuple")?;
                let last_is_slice = match self.get(*params) {
                    Node::Tuple(params) => params
                        .last()
                        .is_some_and(|&last| matches!(self.get(last), Node::Slice(_))),
                    _ => false,
                };
                if *variadic && !last_is_slice {
                    return Err(BadNode("the last parameter of a variadic type is no slice"));
                }
            }
            Node::Struct(fields) => all(fields, Class::Field, "a field is expected")?,
            Node::Field { ty: of, .. } => {
                ty(*of)?;
                return Ok(Class::Field);
            }
            Node::Interface { methods, .. } => {
                all(
                    methods,
                    Class::IMethod,
                    "a method of an interface is expected",
                )?;
                let key = |id: &NodeId| match self.get(*id) {
                    Node::IMethod { package, name, .. } => (package.as_deref(), &**name),
                    _ => (None, &[][..]),
                };
                if !methods.is_sorted_by(|a, b| key(a) < key(b)) {
                    return Err(BadNode("the methods of an interface are out of order"));
                }
            }
            Node::IMethod { sig, .. } => {
                match self.get(*sig) {
                    Node::Signature { .. } => {}
                    _ => return Err(BadNode("the type of a method is no function type")),
                }
                return Ok(Class::IMethod);
            }
            Node::LocalNamed { within, .. } => code(*within)?,
            Node::Instance { of, args } => {
                if args.is_empty() {
                    return Err(BadNode("an instance has no type arguments"));
                }
                all(args, Class::Type, "a type is expected")?;
                return match self.get(*of) {
                    Node::Named { .. } | Node::LocalNamed { .. } | Node::Universe(_) => {
                        Ok(Class::Type)
                    }
                    Node::Func { .. } | Node::Method { .. } | Node::OpMethod { .. } => {
                        Ok(Class::Entity)
                    }
                    _ => Err(BadNode("an instance of what is not generic")),
                };
            }
            Node::Func { .. } | Node::Var { .. } | Node::PkgInit(_) | Node::InitFunc { .. } => {
                return Ok(Class::Entity);
            }
            Node::Data(_) => return Ok(Class::Entity),
            Node::Method { receiver: of, .. } => {
                receiver(*of)?;
                return Ok(Class::Entity);
            }
            Node::OpMethod {
                receiver: of,
                params,
                ..
            } => {
                receiver(*of)?;
                tuple(*params, "the parameters of an operator method are no tuple")?;
                return Ok(Class::Entity);
            }
            Node::Closure { within, .. } => {
                code(*within)?;
                return Ok(Class::Entity);
            }
            Node::Wrap(wrap, of) => {
                let fits = match wrap {
                    _ if wrap.of_type() => self.is_type(*of),
                    Wrap::IfacePtrThunk => self.is_method(*of),
                    _ => self.is_wrapped(*of),
                };
                return match fits {
                    true => Ok(Class::Entity),
                    false if wrap.of_type() => Err(BadNode("a type is expected")),
                    false => Err(BadNode("no thunk is made around this")),
                };
            }
            Node::Pair(_, first, second) => {
                ty(*first)?;
                ty(*second)?;
                return Ok(Class::Entity);
            }
            Node::BaseAdjustThunk(_, of) => {
                return match self.is_method(*of) {
                    true => Ok(Class::Entity),
                    false => Err(BadNode("no thunk is made around this")),
                };
            }
        }
        Ok(Class::Type)
    }
}

impl Node {
    /// What is written before the children: the tag and the header.
    pub(crate) fn head(&self, out: &mut String) {
        match self {
            Node::Basic(basic) => out.push(basic.tag()),
            Node::Pointer(_) => out.push('P'),
            Node::Slice(_) => out.push('S'),
            Node::Array(len, _) => {
                out.push('A');
                ident::number(*len, out);
            }
            Node::Map(..) => out.push('H'),
            Node::Option(_) => out.push('O'),
            Node::Chan(dir, _) => out.push_str(match dir {
                Dir::Both => "Xb",
                Dir::Send => "Xs",
                Dir::Recv => "Xr",
            }),
            Node::Tuple(_) => out.push('L'),
            Node::Signature { variadic, .. } => {
                out.push('G');
                ident::number(u64::from(*variadic), out);
                out.push_str("0_");
            }
            Node::Struct(_) => out.push_str("Z0_"),
            Node::Field {
                name,
                package,
                embedded,
                required,
                parent,
                tag,
                ..
            } => {
                let flags = u64::from(*embedded)
                    | u64::from(package.is_some()) << 1
                    | u64::from(tag.is_some()) << 2
                    | u64::from(*required == Required::Yes) << 3
                    | u64::from(*parent) << 5
                    | u64::from(*required == Required::MustSet) << 7;
                out.push('q');
                ident::number(flags, out);
                if let Some(package) = package {
                    ident::path(package, out);
                }
                ident::identifier(name, out);
                if let Some(tag) = tag {
                    ident::identifier(tag, out);
                }
            }
            Node::Interface { sync, .. } => out.push_str(match sync {
                true => "Y1_",
                false => "Y",
            }),
            Node::IMethod { package, name, .. } => {
                match package {
                    Some(package) => {
                        out.push_str("n1");
                        ident::path(package, out);
                    }
                    None => out.push_str("n0"),
                }
                ident::identifier(name, out);
            }
            Node::Named { package, name } => named('N', package, name, out),
            Node::LocalNamed { ordinal, name, .. } => {
                out.push('g');
                ident::number(*ordinal, out);
                ident::identifier(name, out);
            }
            Node::Universe(name) => {
                out.push('u');
                ident::identifier(name, out);
            }
            Node::Instance { .. } => out.push('I'),
            Node::TypeParam(index) => {
                out.push('Q');
                ident::number(*index, out);
            }
            Node::Func { package, name } => named('F', package, name, out),
            Node::Method { name, .. } => {
                out.push('M');
                ident::identifier(name, out);
            }
            Node::OpMethod { right, name, .. } => {
                out.push_str("To");
                let prefix: &[u8] = match right {
                    true => b"opr_",
                    false => b"op_",
                };
                ident::identifier(&[prefix, name].concat(), out);
            }
            Node::Var { package, name } => named('R', package, name, out),
            Node::Closure { ordinal, .. } => {
                out.push('C');
                ident::number(*ordinal, out);
            }
            Node::PkgInit(package) => {
                out.push('J');
                ident::path(package, out);
            }
            Node::InitFunc { package, ordinal } => {
                out.push('K');
                ident::path(package, out);
                ident::number(*ordinal, out);
            }
            Node::Wrap(wrap, _) => out.push_str(wrap.tag()),
            Node::Pair(pair, ..) => out.push_str(pair.tag()),
            Node::Data(key) => {
                out.push_str("TD");
                ident::identifier(key, out);
            }
            Node::BaseAdjustThunk(offset, _) => {
                out.push_str("Tt");
                ident::signed(*offset, out);
            }
        }
    }

    /// Whether an `E` ends the children.
    pub(crate) fn closes(&self) -> bool {
        matches!(
            self,
            Node::Tuple(_) | Node::Struct(_) | Node::Interface { .. } | Node::Instance { .. }
        )
    }

    /// The children in the order in which they are written.
    pub(crate) fn each_child(&self, each: &mut impl FnMut(NodeId)) {
        match self {
            Node::Basic(_)
            | Node::Named { .. }
            | Node::Universe(_)
            | Node::TypeParam(_)
            | Node::Func { .. }
            | Node::Var { .. }
            | Node::PkgInit(_)
            | Node::InitFunc { .. }
            | Node::Data(_) => {}
            Node::Pointer(of)
            | Node::Slice(of)
            | Node::Array(_, of)
            | Node::Option(of)
            | Node::Chan(_, of)
            | Node::Field { ty: of, .. }
            | Node::IMethod { sig: of, .. }
            | Node::LocalNamed { within: of, .. }
            | Node::Method { receiver: of, .. }
            | Node::Closure { within: of, .. }
            | Node::Wrap(_, of)
            | Node::BaseAdjustThunk(_, of) => each(*of),
            Node::Map(first, second)
            | Node::Signature {
                params: first,
                results: second,
                ..
            }
            | Node::OpMethod {
                receiver: first,
                params: second,
                ..
            }
            | Node::Pair(_, first, second) => {
                each(*first);
                each(*second);
            }
            Node::Tuple(all) | Node::Struct(all) | Node::Interface { methods: all, .. } => {
                all.iter().copied().for_each(each)
            }
            Node::Instance { of, args } => {
                each(*of);
                args.iter().copied().for_each(each);
            }
        }
    }
}

fn named(tag: char, package: &[u8], name: &[u8], out: &mut String) {
    out.push(tag);
    ident::path(package, out);
    ident::identifier(name, out);
}
