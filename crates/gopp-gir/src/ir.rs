//! The parts of a GIR module: functions of basic blocks over places and
//! rvalues (gir.md §2, §4 to §6, §14). Locals are slots, assigned as
//! often as the program assigns them; there is no SSA form.

use la_arena::{Arena, Idx};
use smallvec::SmallVec;

use crate::types::{TypeId, TypeInterner};

pub type LocalId = Idx<Local>;
pub type BlockId = Idx<BasicBlock>;
pub type FuncId = Idx<Function>;
pub type GlobalId = Idx<Global>;

/// A debug scope of a function: 0 is the function, `n` is `scopes[n-1]`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct ScopeId(pub u32);

/// A place in the source that needs no source map: the file as an index
/// into `Module::files` plus one, or 0 for none; the line; the column.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct Span {
    pub file: u32,
    pub line: u32,
    pub col: u32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum CallConv {
    /// Between Go++ functions.
    Go,
    /// The C convention of the target.
    C,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Linkage {
    External,
    Internal,
    /// Every package that needs it has a copy; the linker keeps one.
    LinkOnceOdr,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Inline {
    Default,
    Always,
    Never,
}

#[derive(Debug, Default)]
pub struct Module {
    pub types: TypeInterner,
    /// The path of the package.
    pub name: String,
    pub files: Vec<String>,
    /// The bytes of the string constants.
    pub strings: Vec<Box<[u8]>>,
    pub funcs: Arena<Function>,
    pub globals: Arena<Global>,
}

#[derive(Clone, Debug)]
pub struct Global {
    /// The symbol.
    pub name: String,
    pub ty: TypeId,
    pub link: Linkage,
    pub mutable: bool,
    /// What it holds when the program starts: a constant for each leaf
    /// of its type, in the order of `TypeInterner::leaves`. Empty for
    /// a global that starts as zeros.
    pub parts: Vec<Constant>,
}

/// A function: with blocks a definition, without a declaration. Its body
/// is made by `FuncBuilder` alone and is read through `&Function`.
#[derive(Clone, Debug)]
pub struct Function {
    /// The symbol.
    pub(crate) name: String,
    /// A `Func` type.
    pub(crate) sig: TypeId,
    pub(crate) link: Linkage,
    pub(crate) cc: CallConv,
    pub(crate) no_unwind: bool,
    pub(crate) no_return: bool,
    pub(crate) inline: Inline,
    pub(crate) loc: Span,
    /// 0 is the place of the result, then the parameters, then the rest.
    pub(crate) locals: Arena<Local>,
    /// 0 is where the function starts.
    pub(crate) blocks: Arena<BasicBlock>,
    pub(crate) scopes: Vec<DebugScope>,
}

impl Function {
    pub fn name(&self) -> &str {
        &self.name
    }

    pub fn sig(&self) -> TypeId {
        self.sig
    }

    pub fn link(&self) -> Linkage {
        self.link
    }

    pub fn cc(&self) -> CallConv {
        self.cc
    }

    pub fn no_unwind(&self) -> bool {
        self.no_unwind
    }

    pub fn no_return(&self) -> bool {
        self.no_return
    }

    pub fn inline(&self) -> Inline {
        self.inline
    }

    pub fn loc(&self) -> Span {
        self.loc
    }

    pub fn locals(&self) -> &Arena<Local> {
        &self.locals
    }

    pub fn blocks(&self) -> &Arena<BasicBlock> {
        &self.blocks
    }

    pub fn scopes(&self) -> &[DebugScope] {
        &self.scopes
    }

    /// Whether the function has no body here.
    pub fn is_declaration(&self) -> bool {
        self.blocks.is_empty()
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LocalKind {
    Return,
    Arg,
    /// A variable of the program.
    Var,
    /// What holds a value between two steps.
    Temp,
}

#[derive(Clone, Debug)]
pub struct Local {
    pub kind: LocalKind,
    pub ty: TypeId,
    /// The name in the source, of what has one.
    pub name: Option<String>,
    pub mutable: bool,
    /// The slot owns its value: the value is dropped when the slot ends.
    pub owned: bool,
    pub loc: Span,
    pub scope: ScopeId,
}

#[derive(Clone, Debug)]
pub struct BasicBlock {
    pub stmts: Vec<Statement>,
    pub term: Terminator,
    /// Runs only while a panic unwinds.
    pub cleanup: bool,
    /// A cleanup block that may stop the panic.
    pub catch: bool,
    /// Rarely reached.
    pub cold: bool,
}

#[derive(Clone, Debug)]
pub struct Statement {
    pub kind: StatementKind,
    pub loc: Span,
    pub scope: ScopeId,
}

#[derive(Clone, Debug)]
pub enum StatementKind {
    /// The place takes the value of the rvalue.
    Assign(Box<(Place, Rvalue)>),
    StorageLive(LocalId),
    StorageDead(LocalId),
    Nop,
}

#[derive(Clone, Debug)]
pub struct Terminator {
    pub kind: TerminatorKind,
    pub loc: Span,
    pub scope: ScopeId,
}

#[derive(Clone, Debug)]
pub enum TerminatorKind {
    Goto(BlockId),
    Switch {
        on: Operand,
        /// The bits of each value, with where it leads.
        cases: Vec<(u64, BlockId)>,
        otherwise: BlockId,
    },
    Return,
    Unreachable,
    Call {
        callee: Operand,
        args: Vec<Operand>,
        dest: Place,
        /// `None` after a call of a function that does not return.
        target: Option<BlockId>,
        unwind: UnwindAction,
    },
    Drop {
        place: Place,
        target: BlockId,
        unwind: UnwindAction,
    },
    Assert {
        cond: Operand,
        expected: bool,
        kind: AssertKind,
        /// What the message of the panic names.
        args: Vec<Operand>,
        target: BlockId,
        unwind: UnwindAction,
    },
    /// Goes on unwinding, at the end of a cleanup block.
    Resume,
}

/// Where a panic goes that leaves a call.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum UnwindAction {
    /// Nothing to clean up in this function: on to its caller.
    Continue,
    Cleanup(BlockId),
    /// A panic inside a cleanup ends the process.
    Terminate,
    /// What is called does not panic.
    Unreachable,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AssertKind {
    Bounds,
    SliceAlen,
    SliceAcap,
    SliceB,
    Slice3Alen,
    Slice3Acap,
    Slice3B,
    Slice3C,
    SliceConvert,
    Nil,
    DivByZero,
    NegativeShift,
}

/// A local, and the way from it to a part of it. Each step says the
/// type that it gives.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Place {
    pub local: LocalId,
    pub projections: SmallVec<[Projection; 2]>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Projection {
    pub kind: ProjectionKind,
    pub ty: TypeId,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ProjectionKind {
    Deref,
    Field(u32),
    /// The element at the index that an integer local holds; the check
    /// of the bounds is an `Assert` before it.
    Index(LocalId),
    ConstIndex(u64),
    /// The payload of a variant.
    Downcast(u32),
}

#[derive(Clone, Debug, PartialEq)]
pub enum Operand {
    /// Reads the place, which keeps its value.
    Copy(Place),
    /// Takes the value out of the place.
    Move(Place),
    Const(Constant),
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Constant {
    pub kind: ConstantKind,
    pub ty: TypeId,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum ConstantKind {
    /// The bits, cut to the width of the type.
    Int(u64),
    /// The bits of the IEEE-754 value.
    Float(u64),
    Bool(bool),
    /// An index into `Module::strings`.
    Str(u32),
    Func(FuncId),
    Null,
    /// The value of the type whose bytes are all zero.
    Zero,
    /// The address of a global.
    Global(GlobalId),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Mutability {
    Not,
    Mut,
}

#[derive(Clone, Debug, PartialEq)]
pub enum Rvalue {
    Use(Operand),
    AddrOf(Place, Mutability),
    Binary(BinOp, Operand, Operand),
    Unary(UnOp, Operand),
    /// The operand as a value of the type.
    Cast(CastKind, Operand, TypeId),
    Aggregate(AggKind, Vec<Operand>),
    /// The length of a string, a slice or an array.
    Len(Place),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BinOp {
    Add,
    Sub,
    Mul,
    Div,
    Rem,
    BitAnd,
    BitOr,
    BitXor,
    BitClear,
    Shl,
    Shr,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum UnOp {
    Neg,
    Not,
    BitNot,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CastKind {
    IntToInt,
    IntToFloat,
    /// NaN gives 0, a value out of range the nearest integer.
    FloatToInt,
    FloatToFloat,
    PtrToPtr,
    PtrToInt,
    IntToPtr,
}

/// What a value is put together as, from its fields in their order.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AggKind {
    Tuple,
    /// An array of elements of the type.
    Array(TypeId),
    /// A struct, an option that is set, a string or a slice: the type.
    Of(TypeId),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DebugScope {
    pub kind: ScopeKind,
    /// An earlier scope, or 0 for the function.
    pub parent: ScopeId,
    pub loc: Span,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ScopeKind {
    /// A `{}` of the source.
    Block,
    /// The body of the function that was inlined at `loc`.
    Inlined(FuncId),
}

impl TerminatorKind {
    /// Where the end of a block leads, and whether by an unwind edge.
    pub fn successors(&self) -> Vec<(BlockId, bool)> {
        let unwind = |action: &UnwindAction| match action {
            UnwindAction::Cleanup(block) => Some((*block, true)),
            _ => None,
        };
        match self {
            Self::Goto(target) => vec![(*target, false)],
            Self::Switch {
                cases, otherwise, ..
            } => {
                let mut all: Vec<_> = cases.iter().map(|&(_, target)| (target, false)).collect();
                all.push((*otherwise, false));
                all
            }
            Self::Call {
                target,
                unwind: action,
                ..
            } => {
                let target = target.map(|target| (target, false));
                target.into_iter().chain(unwind(action)).collect()
            }
            Self::Drop {
                target,
                unwind: action,
                ..
            }
            | Self::Assert {
                target,
                unwind: action,
                ..
            } => std::iter::once((*target, false))
                .chain(unwind(action))
                .collect(),
            Self::Return | Self::Unreachable | Self::Resume => Vec::new(),
        }
    }

    /// Every block that the end of a block names, to be changed.
    pub(crate) fn each_target(&mut self, mut each: impl FnMut(&mut BlockId)) {
        let unwind = |action: &mut UnwindAction, each: &mut dyn FnMut(&mut BlockId)| {
            if let UnwindAction::Cleanup(block) = action {
                each(block);
            }
        };
        match self {
            Self::Goto(target) => each(target),
            Self::Switch {
                cases, otherwise, ..
            } => {
                cases.iter_mut().for_each(|(_, target)| each(target));
                each(otherwise);
            }
            Self::Call {
                target,
                unwind: action,
                ..
            } => {
                target.iter_mut().for_each(&mut each);
                unwind(action, &mut each);
            }
            Self::Drop {
                target,
                unwind: action,
                ..
            }
            | Self::Assert {
                target,
                unwind: action,
                ..
            } => {
                each(target);
                unwind(action, &mut each);
            }
            Self::Return | Self::Unreachable | Self::Resume => {}
        }
    }
}

impl Place {
    pub fn local(local: LocalId) -> Self {
        Self {
            local,
            projections: SmallVec::new(),
        }
    }
}
