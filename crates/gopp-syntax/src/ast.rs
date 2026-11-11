//! The syntax tree of one file.
//!
//! Types are expressions, as in Go's own syntax tree: `a[i]`, `*T` and `(T)`
//! read the same as a value and as a type, and only the checker can tell.

mod dump;

use gopp_span::{FileId, Span};
use la_arena::{Arena, Idx};
use lasso::Spur;

pub use dump::{dump, dump_json, dump_shape};

use crate::token::Comment;

pub type ExprId = Idx<Expr>;
pub type StmtId = Idx<Stmt>;
pub type DeclId = Idx<Decl>;

#[derive(Debug)]
pub struct File {
    pub id: FileId,
    pub package: Option<Ident>,
    /// Top-level declarations in source order, imports included.
    pub decls: Vec<DeclId>,
    pub comments: Vec<Comment>,
    pub exprs: Arena<Expr>,
    pub stmts: Arena<Stmt>,
    pub decl_nodes: Arena<Decl>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Ident {
    pub name: Spur,
    pub span: Span,
}

#[derive(Debug)]
pub struct Decl {
    pub kind: DeclKind,
    pub span: Span,
    pub attrs: Vec<Attribute>,
    /// Declarations of one parenthesized group share a number; `iota` and
    /// the implicit repetition of constants count within it.
    pub group: Option<u32>,
}

#[derive(Debug)]
pub enum DeclKind {
    Bad,
    Import {
        /// `.`, `_` or a package name.
        name: Option<Ident>,
        path: ExprId,
    },
    Const {
        names: Vec<Ident>,
        ty: Option<ExprId>,
        values: Vec<ExprId>,
    },
    Var {
        names: Vec<Ident>,
        ty: Option<ExprId>,
        values: Vec<ExprId>,
    },
    Type {
        name: Ident,
        type_params: Vec<TypeParam>,
        alias: bool,
        ty: ExprId,
    },
    Func {
        /// A procedural macro: `macro func Name(...)`.
        is_macro: bool,
        receiver: Option<Param>,
        name: Ident,
        type_params: Vec<TypeParam>,
        /// A [`ExprKind::FuncType`].
        ty: ExprId,
        body: Option<Block>,
    },
    /// A declarative macro: `macro Name(p kind, ...) { ... }`.
    Macro {
        name: Ident,
        params: Vec<MacroParam>,
        body: Block,
    },
    /// A macro invocation at top level; the expression is a
    /// [`ExprKind::MacroCall`].
    MacroCall(ExprId),
}

#[derive(Clone, Debug)]
pub struct Attribute {
    pub path: MacroRef,
    /// `None` without parentheses, which differs from `()`.
    pub args: Option<Vec<ExprId>>,
    pub span: Span,
}

/// `Name` or `pkg.Name` after `#` or `@`.
#[derive(Clone, Copy, Debug)]
pub struct MacroRef {
    pub package: Option<Ident>,
    pub name: Ident,
}

#[derive(Debug)]
pub struct MacroParam {
    pub name: Ident,
    pub variadic: bool,
    pub kind: Ident,
}

#[derive(Debug)]
pub struct TypeParam {
    pub name: Ident,
    pub bound: ExprId,
}

/// A parameter, result or receiver. Parameters declared together
/// (`a, b int`) are separate entries that share `ty`.
#[derive(Clone, Copy, Debug)]
pub struct Param {
    pub name: Option<Ident>,
    pub ty: ExprId,
}

#[derive(Debug)]
pub struct Field {
    /// Empty for an embedded field.
    pub names: Vec<Ident>,
    pub ty: ExprId,
    pub attrs: Vec<Attribute>,
    pub tag: Option<ExprId>,
    pub span: Span,
}

#[derive(Debug)]
pub enum InterfaceElem {
    Method {
        name: Ident,
        /// A [`ExprKind::FuncType`].
        ty: ExprId,
    },
    /// An embedded interface or a type set: `io.Reader`, `~int | string`.
    Type(ExprId),
}

#[derive(Debug)]
pub struct Expr {
    pub kind: ExprKind,
    pub span: Span,
}

#[derive(Debug)]
pub enum ExprKind {
    Bad,
    Name(Spur),
    /// The text is the span. `bad` marks a literal the lexer reported.
    BasicLit {
        kind: LitKind,
        bad: bool,
    },
    CompositeLit {
        ty: Option<ExprId>,
        elems: Vec<ExprId>,
    },
    KeyValue {
        key: ExprId,
        value: ExprId,
    },
    FuncLit {
        /// A [`ExprKind::FuncType`].
        ty: ExprId,
        body: Block,
    },
    Paren(ExprId),
    Selector {
        x: ExprId,
        sel: Ident,
    },
    /// `x[i]`, or `x[A, B]` with several indices: an index, an `Index`
    /// operator call or an instantiation.
    Index {
        x: ExprId,
        indices: Vec<ExprId>,
    },
    Slice {
        x: ExprId,
        lo: Option<ExprId>,
        hi: Option<ExprId>,
        max: Option<ExprId>,
        /// Written with two colons.
        full: bool,
    },
    Assert {
        x: ExprId,
        ty: ExprId,
    },
    /// `x.(type)` or `v := x.(type)` in a switch header.
    TypeSwitchGuard {
        lhs: Option<Ident>,
        x: ExprId,
    },
    Unary {
        op: UnaryOp,
        x: ExprId,
    },
    Binary {
        op: BinaryOp,
        x: ExprId,
        y: ExprId,
    },
    Call {
        fun: ExprId,
        args: Vec<ExprId>,
        /// The last argument is followed by `...`.
        spread: bool,
        /// `f!(x)`: the call unwraps its error.
        unwrap: bool,
    },
    MacroCall {
        path: MacroRef,
        args: Vec<ExprId>,
    },
    ArrayType {
        /// `None` for `[...]T`.
        len: Option<ExprId>,
        elem: ExprId,
    },
    SliceType {
        elem: ExprId,
    },
    /// `...T`, the type of a variadic parameter.
    DotsType {
        elem: ExprId,
    },
    StructType {
        fields: Vec<Field>,
    },
    InterfaceType {
        elems: Vec<InterfaceElem>,
    },
    FuncType {
        params: Vec<Param>,
        results: Vec<Param>,
        /// Written with `!`: the function can fail.
        fails: bool,
    },
    MapType {
        key: ExprId,
        value: ExprId,
    },
    ChanType {
        dir: ChanDir,
        elem: ExprId,
    },
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LitKind {
    Int,
    Float,
    Imag,
    Rune,
    String,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ChanDir {
    Both,
    Send,
    Recv,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum UnaryOp {
    Plus,
    Neg,
    Not,
    Xor,
    /// `*x`: a dereference or a pointer type.
    Star,
    Addr,
    Recv,
    /// `~T` in a type set.
    Tilde,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BinaryOp {
    OrOr,
    AndAnd,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    Add,
    Sub,
    /// `|`: bitwise or, or a union of type terms.
    Or,
    Xor,
    Mul,
    Div,
    Rem,
    Shl,
    Shr,
    And,
    AndNot,
}

#[derive(Debug)]
pub struct Block {
    pub stmts: Vec<StmtId>,
    pub span: Span,
}

#[derive(Debug)]
pub struct Stmt {
    pub kind: StmtKind,
    pub span: Span,
}

#[derive(Debug)]
pub enum StmtKind {
    Bad,
    Empty,
    Labeled {
        label: Ident,
        stmt: StmtId,
    },
    Block(Block),
    Expr(ExprId),
    Send {
        chan: ExprId,
        value: ExprId,
    },
    /// The declarations of one `const`, `var` or `type` statement.
    Decl(Vec<DeclId>),
    Assign {
        op: AssignOp,
        lhs: Vec<ExprId>,
        rhs: Vec<ExprId>,
    },
    IncDec {
        x: ExprId,
        inc: bool,
    },
    Branch {
        kind: BranchKind,
        label: Option<Ident>,
    },
    Go(ExprId),
    Defer(ExprId),
    Return(Vec<ExprId>),
    If {
        init: Option<StmtId>,
        cond: ExprId,
        then: Block,
        /// An `If` or a `Block` statement.
        otherwise: Option<StmtId>,
    },
    For {
        init: Option<StmtId>,
        cond: Option<ExprId>,
        post: Option<StmtId>,
        body: Block,
    },
    Range {
        /// With `define`, each is a name or `&name` (an alias variable).
        lhs: Vec<ExprId>,
        define: bool,
        x: ExprId,
        body: Block,
    },
    Switch {
        init: Option<StmtId>,
        /// A [`ExprKind::TypeSwitchGuard`] makes it a type switch.
        tag: Option<ExprId>,
        clauses: Vec<CaseClause>,
    },
    Select {
        clauses: Vec<CommClause>,
    },
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AssignOp {
    Assign,
    Define,
    Op(BinaryOp),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BranchKind {
    Break,
    Continue,
    Goto,
    Fallthrough,
}

#[derive(Debug)]
pub struct CaseClause {
    /// `None` for `default`.
    pub cases: Option<Vec<ExprId>>,
    pub body: Vec<StmtId>,
    pub span: Span,
}

#[derive(Debug)]
pub struct CommClause {
    /// `None` for `default`; else a send, or a receive as an expression or
    /// assignment statement.
    pub comm: Option<StmtId>,
    pub body: Vec<StmtId>,
    pub span: Span,
}
