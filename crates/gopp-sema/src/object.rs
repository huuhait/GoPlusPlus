//! What a name can denote.

use std::sync::Arc;

use gopp_span::Span;
use gopp_syntax::ast::{ExprId, StmtId};
use lasso::Spur;
use rustc_hash::{FxHashMap, FxHashSet};

use crate::constant::Value;
use crate::package::Package;
use crate::types::{Marks, TypeId};
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
    /// The built-in attributes of a function.
    pub marks: Marks,
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
    /// For an expression that names a generic function or type with type
    /// arguments, written or inferred, what they are: `f` in `f(x)`,
    /// `f[int]`, `List[int]`, a generic function that is given to a place
    /// of a function type. In a generic body they may be written with its
    /// type parameters.
    pub instances: FxHashMap<ExprId, Instance>,
    /// For a call `f!(x)`, what it does with an error.
    pub bangs: FxHashMap<ExprId, Bang>,
    /// In a `.gopp` file: for a place whose value is given to a new
    /// owner, how. A place of a type that is copied by its bytes has no
    /// entry, nor has a temporary, which is given as it is.
    pub var_modes: FxHashMap<ExprId, Transfer>,
    /// The expressions of `iface_conversions` that are arguments of a
    /// call: the value is boxed, and must be `Send`, only if the function
    /// keeps the parameter, which is known with its body.
    pub iface_args: FxHashSet<ExprId>,
    /// The names of option variables that are read as the value that the
    /// option holds, because the option is known to be set there.
    pub narrowed: FxHashSet<ExprId>,
    /// For a function literal, the variables of the functions around it
    /// that it uses, in the order of their declarations, each with the
    /// most that the literal or a literal inside it does to it.
    pub captures: FxHashMap<ExprId, Vec<Capture>>,
    /// The selectors `x.m` that are values, not called: what each holds of
    /// its receiver.
    pub method_values: FxHashMap<ExprId, MethodValue>,
    /// The places on the left of the assignments that only exchange the
    /// values of their places: each gets a value that was moved out of
    /// another, and its own was moved out too.
    pub swaps: FxHashSet<ExprId>,
    /// The copied places on the right of an assignment that are the place
    /// on its left, as in `xs = append(xs, v)` and `m[k] = f(m[k])`: the
    /// value is assigned over, so the one that is evaluated last may be
    /// given as it is.
    pub self_moves: FxHashSet<ExprId>,
    /// The names of variables in `var_modes` whose copy is the last
    /// that is read of the variable on every way from there: nothing
    /// reads the variable again before it is assigned. The value itself
    /// may be given, if the variable owns it and nothing else reaches
    /// it.
    pub last_uses: FxHashSet<ExprId>,
    /// The expressions that an `@Operator` or a `@Convert` method stands
    /// for, each with the method: `a + b`, `-a`, `a == b`, `a < b`, `a[i]`
    /// that is read, `T(a)`, and what a `range` clause ranges over.
    pub operators: FxHashMap<ExprId, OperatorUse>,
    /// For `x op= y`, by the place `x`: the method that is called, an
    /// in-place one (`AddAssign`) or the one of `x op y` (`Add`).
    pub op_assigns: FxHashMap<ExprId, OperatorUse>,
    /// The index expressions that are assigned to through an `@Operator`
    /// `SetIndex` method, each with the method.
    pub index_stores: FxHashMap<ExprId, OperatorUse>,
    /// The values of the cases of an expression switch that are compared
    /// with the tag by an `@Operator` `Eq` method, each with the method.
    pub case_operators: FxHashMap<ExprId, OperatorUse>,
    /// The expressions whose interface values are given to a place of an
    /// interface type with other methods: for each, the type of the value
    /// and the type of the place. The value says its type another way
    /// there.
    pub iface_changes: FxHashMap<ExprId, (TypeId, TypeId)>,
    /// For an expression that gives several values at once, the values
    /// that are put into an interface or given to a place of an interface
    /// type with other methods, each by its number: the type of the value
    /// and the type of the place.
    pub part_conversions: FxHashMap<(ExprId, u32), (TypeId, TypeId)>,
    /// The cases of the type switches that are types, each with its
    /// type; the case `nil` has none.
    pub case_types: FxHashMap<ExprId, TypeId>,
    /// The expressions that name what a C or C++ header declares, each
    /// with what it names: `stdio.puts`, `algorithm.max[int32]`, the
    /// function of a call that finds its template arguments.
    pub foreign_uses: FxHashMap<ExprId, ForeignUse>,
    /// For a `return` without values in a function whose results have
    /// names: each result whose name denotes something else there, with
    /// what it denotes.
    pub hidden_results: FxHashMap<StmtId, Vec<(LocalId, LocalId)>>,
}

/// How the value of a place gets to the new owner that one use gives it
/// to.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Transfer {
    /// The new owner gets a copy, made by the rule of the type. None is
    /// made where the program cannot tell, which is decided where the
    /// last read of each variable is known.
    Copy,
    /// The value itself: the operand of `move`, a returned variable that
    /// cannot be copied, a place of a swap.
    Move,
}

/// A variable that a function literal uses and does not declare.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Capture {
    pub local: LocalId,
    pub how: CaptureUse,
}

/// The most that a function literal does to a variable it captures.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum CaptureUse {
    Read,
    /// The variable or a part of it is assigned, counted, deleted from or
    /// cleared.
    Write,
    /// The address of the variable or of a part of it is taken: `&x`, a
    /// method with a pointer receiver, `&v` of a range clause over it.
    Address,
}

/// What an expression names in a C or C++ header.
#[derive(Clone, Debug, PartialEq)]
pub struct ForeignUse {
    /// The import that names the header.
    pub import: ObjId,
    pub name: Spur,
    /// The template arguments that are written, for a template.
    pub explicit: Vec<crate::foreign::TemplateArg>,
    /// The type of what is named: of a function, of the instance that is
    /// called.
    pub ty: TypeId,
}

/// The method that stands for an operator in one place (EXP-17).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct OperatorUse {
    /// The type that declares the method, with its type arguments; or
    /// the type parameter whose constraint lists it.
    pub holder: TypeId,
    pub name: Spur,
    /// Which of the `@Operator` methods of the holder, in the order of
    /// their declarations. `None` for a method that is found by its name
    /// alone: one of a constraint, or a `@Convert` method.
    pub method: Option<u32>,
    /// The type of the method as it is called here.
    pub sig: TypeId,
    /// The receiver is the operand on the right (`@Right`).
    pub swapped: bool,
}

/// What a method value `x.m` holds of `x` (CLO-5, CLO-5a).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MethodValue {
    /// The value that `x` has when the method value is made: a copy for a
    /// value receiver, the pointer or the interface value otherwise.
    Value,
    /// The address of the variable `x`, for a pointer receiver: the
    /// method sees what `x` is when it is called.
    Address,
}

/// What a call `f!(x)` does when `f` returns an error. The innermost
/// function around the call decides.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Bang {
    /// The function can fail: it returns the error.
    Return,
    /// The function cannot fail, or the call is that of a `go` statement
    /// or of a `defer` statement in such a function: it panics.
    Panic,
    /// The call of a `defer` statement in a function that can fail: the
    /// error becomes that of the function unless it has one.
    SetError,
}

/// A generic function or type as one use names it: its type arguments,
/// written or inferred, and the type that they make it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Instance {
    pub args: Vec<TypeId>,
    pub ty: TypeId,
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
    /// for a field, its own index last. A step to a parent is the index
    /// of the parent with the bit `types::PARENT`.
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
    /// `base.f`, `base[P].f`: a field of a parent part of the receiver.
    BaseField,
    /// `base.m`, `base[P].m`: the method of a parent, called as it is
    /// declared there, whatever replaces it.
    BaseMethod,
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
