//! What a name can denote.

use std::sync::Arc;

use gopp_span::Span;
use gopp_syntax::ast::{ExprId, StmtId};
use lasso::Spur;
use rustc_hash::{FxHashMap, FxHashSet};

use crate::constant::Value;
use crate::package::Package;
use crate::types::TypeId;
use crate::universe::Predeclared;

/// A package-level object or an import: an index into [`Package::objects`].
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct ObjId(pub u32);

/// An object declared inside a top-level declaration: an index into
/// [`Body::locals`].
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct LocalId(pub u32);

#[derive(Debug)]
pub struct Object {
    pub name: Spur,
    pub kind: ObjKind,
    /// The declaring identifier; for an import without a name, its path.
    pub span: Span,
    /// The type; invalid for what has none, and while it is not known.
    pub ty: TypeId,
    /// The value of a constant; unknown for anything else.
    pub value: Value,
}

#[derive(Debug)]
pub enum ObjKind {
    Const,
    Type,
    Var,
    Func,
    /// A declarative or procedural macro; its name is in a namespace of
    /// its own.
    Macro,
    /// The name one file gives to a package it imports.
    Import {
        package: Arc<Package>,
        /// Index of the file in [`Package::files`].
        file: u32,
        used: bool,
    },
}

#[derive(Debug)]
pub struct Local {
    pub name: Spur,
    pub kind: LocalKind,
    /// The declaring identifier.
    pub span: Span,
    /// Whether the value of a variable is read anywhere.
    pub used: bool,
    /// The type of a variable, constant or type; invalid while it is not
    /// known.
    pub ty: TypeId,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LocalKind {
    Const,
    Type,
    TypeParam,
    Var,
    Receiver,
    Param,
    Result,
}

/// What an identifier denotes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Ref {
    Universe(Predeclared),
    /// A package-level object of this package, or one of its imports.
    Object(ObjId),
    /// An object of an imported package: `member` is an index into the
    /// objects of the package that the import `import` names.
    Member {
        import: ObjId,
        member: ObjId,
    },
    Local(LocalId),
}

/// What was found inside one top-level declaration.
#[derive(Debug, Default)]
pub struct Body {
    pub locals: Vec<Local>,
    /// For a name, what it denotes. For `pkg.Name`, the selector is the
    /// member and `pkg` the import. For a macro call, the macro.
    pub uses: FxHashMap<ExprId, Ref>,
    /// What a name that is the key of a composite literal denotes, if it is
    /// not the name of a struct field: only the type of the literal tells.
    pub keys: FxHashMap<ExprId, Ref>,
    /// For the offset of a declaring identifier, the local it declares.
    pub defs: FxHashMap<u32, LocalId>,
    /// For a type switch with a variable, its variable in each clause.
    pub switch_vars: FxHashMap<StmtId, Vec<LocalId>>,
    /// The type of each expression that has a value, where it is worked
    /// out, and its value if it is a constant.
    pub types: FxHashMap<ExprId, ExprType>,
    /// For a selector `x.sel` that is no name of a package, what it
    /// reaches.
    pub selections: FxHashMap<ExprId, Selection>,
    /// In a `.gopp` file: the left sides of the assignments that write
    /// what a pointer points to. An assignment to a pointer that is not
    /// here points it at something else.
    pub writes_through: FxHashSet<ExprId>,
    /// The expressions whose values are put into an interface without a
    /// conversion being written: for each, the type that the value has
    /// there and the interface type. In a `.gopp` file the first may be
    /// what a pointer points to, or a pointer to what a handle owns.
    pub iface_conversions: FxHashMap<ExprId, (TypeId, TypeId)>,
    /// For a `return` without values in a function whose results have
    /// names: each result whose name denotes something else there, with
    /// what it denotes.
    pub hidden_results: FxHashMap<StmtId, Vec<(LocalId, LocalId)>>,
}

/// What a selector `x.sel` reaches, and the way to it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Selection {
    pub kind: SelectionKind,
    /// The type of `x`.
    pub recv: TypeId,
    /// The type that has the member: that of `x`, what it points to, or a
    /// type embedded in it.
    pub holder: TypeId,
    /// The index of each embedded field on the way to the holder and,
    /// for a field, its own index last.
    pub path: Vec<u32>,
    /// Whether a pointer is followed on the way.
    pub indirect: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SelectionKind {
    /// `x.f`: a field of a value.
    Field,
    /// `x.m`: a method of a value.
    Method,
    /// `T.m`: a method as a function that takes its receiver first.
    MethodExpr,
}

/// The type of an expression; and its value, if it is a constant.
#[derive(Clone, Debug, PartialEq)]
pub struct ExprType {
    pub ty: TypeId,
    pub value: Value,
}

/// What was found in one file of a package.
#[derive(Debug, Default)]
pub struct FileInfo {
    /// The file scope: import names, and the exported names of the packages
    /// imported with `.`.
    pub scope: FxHashMap<Spur, Ref>,
    /// One per top-level declaration of the file, in its order.
    pub bodies: Vec<Body>,
}
