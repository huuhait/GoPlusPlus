//! The types of the package-level declarations and the values of the
//! constants.
//!
//! A declaration is worked out when it is first needed, so the order of
//! the source does not matter. What this module cannot work out yet (a
//! value that needs the type of an expression that is not constant) is
//! left invalid, which fits everywhere and is no error.

mod assign;
mod attrs;
mod borrows;
mod branches;
mod builtins;
mod call;
mod constraints;
mod consts;
mod conversion;
mod flow;
mod foreign;
mod implements;
mod index;
mod infer;
mod inherit;
mod kept;
mod literal;
mod methods;
mod mono;
mod moves;
mod operators;
mod ops;
mod own;
mod place;
mod stmt;

use std::sync::Arc;

use gopp_diag::{Code, Diagnostic, Sink};
use gopp_span::Span;
use gopp_syntax::ast::{self, *};
use lasso::{Spur, ThreadedRodeo};
use rayon::prelude::*;
use rustc_hash::{FxHashMap, FxHashSet};

use crate::basic::{Basic, representable};
use crate::constant::{self, CompareOp, Kind, Value};
use crate::literal::unquote;
use crate::object::{
    Bang, Body, ExprType, LocalId, LocalKind, ObjId, ObjKind, Ref, Selection, SelectionKind,
};
use crate::package::Package;
use crate::resolve::Unit;
use crate::types::{
    ChanDir, DeclaredMethod, Field, Held, Imported, Lends, Member, Method, Term, Type, TypeId,
    Types,
};
use crate::universe::{Predeclared, PredeclaredKind};
use assign::Init;

/// Where an expression stands: its file, what its names denote, and the
/// type parameters in scope.
#[derive(Clone)]
struct Cx<'a> {
    tree: &'a File,
    body: &'a Body,
    source: &'a gopp_span::SourceFile,
    text: &'a str,
    /// The index of the constant in its group, inside a constant
    /// declaration.
    iota: Option<i64>,
    params: FxHashMap<LocalId, TypeId>,
}

#[derive(Clone, Copy)]
enum Form<'a> {
    Const {
        ty: Option<ExprId>,
        value: Option<ExprId>,
        iota: i64,
        /// The value is that of the constant above, written there.
        repeats: bool,
    },
    Var {
        ty: Option<ExprId>,
        /// The expression that gives the variable its value, which of the
        /// results of that expression it gets, and of how many.
        value: Option<(ExprId, usize, usize)>,
    },
    Type {
        alias: bool,
        type_params: &'a [TypeParam],
        ty: ExprId,
        /// Declared with `@Extends`.
        extends: bool,
        /// Declared with `@Pinned`.
        pinned: bool,
        attrs: &'a [Attribute],
    },
    Func {
        type_params: &'a [TypeParam],
        ty: ExprId,
    },
}

/// The declaration of a package-level object.
#[derive(Clone, Copy)]
struct Decl<'a> {
    unit: Unit<'a>,
    /// What the names of its expressions denote. For a constant that
    /// repeats the value above it, those of the declaration that wrote it.
    body: &'a Body,
    form: Form<'a>,
}

/// A constant of a group, which the ones after it repeat when they have no
/// value of their own.
#[derive(Clone, Copy)]
struct Repeated<'a> {
    group: Option<u32>,
    ty: Option<ExprId>,
    values: &'a [ExprId],
    body: &'a Body,
}

#[derive(Clone, Copy, PartialEq)]
enum State {
    New,
    Busy,
    Done,
}

/// What an expression is.
#[derive(Clone, Debug, PartialEq)]
enum Mode {
    /// Not worked out here yet, or in error.
    Unknown,
    /// A call of a function without a result.
    NoValue,
    /// A predeclared function, which is no value.
    Builtin(Predeclared),
    /// A function of package unsafe, by its name.
    Unsafe(Spur),
    Type,
    Const(Value),
    /// What can be assigned to and have its address taken.
    Variable,
    /// `m[k]`: can be assigned to, has no address.
    MapIndex,
    Value,
    /// A value that can give a second one that says whether it is there:
    /// `x.(T)`, `<-ch`.
    CommaOk,
}

/// An expression: what it is and its type, which may be untyped.
#[derive(Clone, Debug)]
struct Operand {
    mode: Mode,
    ty: TypeId,
}

impl Operand {
    const UNKNOWN: Self = Self {
        mode: Mode::Unknown,
        ty: Types::INVALID,
    };

    fn value(ty: TypeId) -> Self {
        let mode = match ty {
            invalid if invalid == Types::INVALID => Mode::Unknown,
            _ => Mode::Value,
        };
        Self { mode, ty }
    }

    fn constant(ty: TypeId, value: Value) -> Self {
        Self {
            mode: Mode::Const(value),
            ty,
        }
    }

    fn variable(ty: TypeId) -> Self {
        let mode = match ty {
            invalid if invalid == Types::INVALID => Mode::Unknown,
            _ => Mode::Variable,
        };
        Self { mode, ty }
    }

    /// Whether the expression has a value: a constant or not.
    fn is_value(&self) -> bool {
        use Mode::*;
        matches!(self.mode, Const(_) | Variable | MapIndex | Value | CommaOk)
    }
}

impl BodyTypes {
    /// Replaces every type in what was found by what `to` makes of it.
    fn each_type(&mut self, to: &mut impl FnMut(TypeId) -> TypeId) {
        for typed in self.exprs.values_mut() {
            typed.ty = to(typed.ty);
        }
        for (_, ty) in &mut self.locals {
            *ty = to(*ty);
        }
        for selection in self.selections.values_mut() {
            selection.recv = to(selection.recv);
            selection.holder = to(selection.holder);
        }
        for (from, into) in self.iface_conversions.values_mut() {
            (*from, *into) = (to(*from), to(*into));
        }
        for ty in self.case_types.values_mut() {
            *ty = to(*ty);
        }
        for (from, into) in self.iface_changes.values_mut() {
            (*from, *into) = (to(*from), to(*into));
        }
        for (from, into) in self.part_conversions.values_mut() {
            (*from, *into) = (to(*from), to(*into));
        }
        for instance in self.instances.values_mut() {
            instance.ty = to(instance.ty);
            for arg in &mut instance.args {
                *arg = to(*arg);
            }
        }
        for param in &mut self.type_params {
            *param = to(*param);
        }
        for given in self.given_types.values_mut() {
            *given = to(*given);
        }
        let tables = [
            &mut self.operators,
            &mut self.op_assigns,
            &mut self.index_stores,
            &mut self.case_operators,
        ];
        for used in tables.into_iter().flat_map(|table| table.values_mut()) {
            used.holder = to(used.holder);
            used.sig = to(used.sig);
        }
        for used in self.foreign_uses.values_mut() {
            used.ty = to(used.ty);
            for arg in &mut used.explicit {
                match arg {
                    crate::foreign::TemplateArg::Type(ty)
                    | crate::foreign::TemplateArg::Const(_, ty) => *ty = to(*ty),
                }
            }
        }
    }
}

/// A top-level declaration to check: its file, its place in the file,
/// and how many values a constant without values of its own repeats.
type Job = (usize, usize, usize);

/// What was found in one declaration that was checked aside, what it
/// reported and added, and the steps that took.
type Checked = (BodyTypes, Aside, u64);

/// What the check of one declaration reports and adds to what all the
/// declarations share.
#[derive(Clone, Default)]
struct Aside {
    diagnostics: Vec<Diagnostic>,
    /// What was reported once for this declaration.
    reported: FxHashSet<(Span, Code)>,
    /// What the declaration needs: each `(from, object)`.
    needs: Vec<(usize, usize)>,
    mono: Vec<mono::Event>,
}

#[derive(Clone)]
struct Checker<'a> {
    names: &'a ThreadedRodeo,
    types: &'a Types,
    sink: &'a Sink,
    package: &'a Package,
    /// The path and the name of the package, as names.
    path: Spur,
    name: Spur,
    blank: Spur,
    decls: Vec<Option<Decl<'a>>>,
    state: Vec<State>,
    ty: Vec<TypeId>,
    value: Vec<Value>,
    /// Whether the declaration being worked out needed something that
    /// this module does not work out yet.
    incomplete: bool,
    /// Where which error was reported, so that none is reported twice.
    reported: FxHashSet<(Span, Code)>,
    /// The constants of no type that are shifted by a count that is no
    /// constant: the type they get must be an integer type.
    shifted: FxHashSet<ExprId>,
    /// While a body is walked: what each local of it is.
    locals: FxHashMap<LocalId, Operand>,
    /// While a body is walked: what each expression of it is, so that
    /// none is worked out twice.
    memo: Option<FxHashMap<ExprId, Operand>>,
    /// While the value of a constant is worked out that another constant
    /// of its group may repeat: nothing is taken from `memo`.
    fresh: bool,
    /// While a body is walked: the results of the function that the
    /// statements are in, the innermost last; none for a signature that
    /// is in error.
    results: Vec<Option<Results>>,
    /// The call of the `go` or `defer` statement that is being checked,
    /// and whether it is that of `go`.
    suspended: Option<(ExprId, bool)>,
    bangs: FxHashMap<ExprId, Bang>,
    /// Whether a function was called or a channel received from since the
    /// flag was last cleared: the length of an array is a constant only
    /// if neither happens to get the array.
    called_or_received: bool,
    /// While the value that a constant repeats from the one above it is
    /// worked out: the name of the constant, where its errors are.
    error_at: Option<Span>,
    /// While a body is walked: its groups of constants.
    local_groups: FxHashMap<u32, LocalGroup>,
    /// While a body is walked: the variables that got no type from an
    /// error that is reported, which are not also unused.
    excused: Vec<LocalId>,
    /// While a body is walked: what each of its selectors reaches.
    selections: FxHashMap<ExprId, Selection>,
    /// While a body is walked: the left sides of its assignments that
    /// write what a pointer points to.
    writes_through: FxHashSet<ExprId>,
    /// While a body is walked: the selectors that name a method with
    /// a pointer receiver, whose call is given a pointer to it.
    pointer_methods: FxHashSet<ExprId>,
    /// While a body is walked: the values with a plain function that
    /// are given to a place which holds none, a `Local` one: there the
    /// function stays with one goroutine (CLO-2).
    localized: FxHashSet<ExprId>,
    /// While a body is walked: how its places give their values, the
    /// places that swaps assign to, and the uses that are assigned over.
    var_modes: FxHashMap<ExprId, crate::object::Transfer>,
    swaps: FxHashSet<ExprId>,
    self_moves: FxHashSet<ExprId>,
    /// While a body is walked: what the variables of its range clauses
    /// are.
    bindings: FxHashMap<LocalId, place::Binding>,
    /// The function literals that are being walked, the innermost last.
    literals: Vec<(ExprId, Span)>,
    /// While a body is walked: the most that each function literal does
    /// to each variable it captures.
    captures: FxHashMap<(ExprId, LocalId), crate::object::CaptureUse>,
    /// While a body is walked: its method values.
    method_values: FxHashMap<ExprId, crate::object::MethodValue>,
    /// The expression that is being called, while it is worked out.
    callee: Option<ExprId>,
    /// What runs the `@Const` functions, if the driver gave one.
    evaluator: Option<&'a dyn crate::consteval::ConstEvaluator>,
    /// While the value of a constant is worked out.
    in_const: bool,
    /// The `@Const` function whose signature and body are being checked.
    const_fn: Option<Spur>,
    /// The built-in attributes of each function of the package.
    func_marks: Vec<crate::types::Marks>,
    /// The `@Operator` methods of each type as they were declared, and
    /// the types that its `@Convert` methods convert to.
    operator_decls: FxHashMap<crate::types::NamedId, Vec<operators::OperatorDecl>>,
    convert_decls: FxHashMap<crate::types::NamedId, Vec<(TypeId, Span)>>,
    /// Where each `@Operator` method of a type is declared, by its place
    /// among them; and the names of all that are declared.
    operator_spans: FxHashMap<(crate::types::NamedId, u32), Span>,
    operator_names: FxHashSet<Span>,
    operators: FxHashMap<ExprId, crate::object::OperatorUse>,
    op_assigns: FxHashMap<ExprId, crate::object::OperatorUse>,
    index_stores: FxHashMap<ExprId, crate::object::OperatorUse>,
    case_operators: FxHashMap<ExprId, crate::object::OperatorUse>,
    /// The type parameters of the package and what each is instantiated
    /// as.
    mono: mono::Mono,
    kept: kept::Kept,
    /// What each function of the package does with the pointers that
    /// it is given.
    lending: FxHashMap<ObjId, Arc<Lends>>,
    /// The type parameters of the function whose body is being checked,
    /// those that its receiver names too.
    around: Vec<TypeId>,
    /// Where declarations are checked side by side: what the one being
    /// checked reports and adds to what all share, to be taken in the
    /// order of the source.
    aside: Option<Aside>,
    /// What each header says of each name that was asked.
    foreign_members: FxHashMap<(ObjId, Spur), Option<crate::foreign::ForeignMember>>,
    /// For an expression that names a function of a header: which of its
    /// parameters it does not write through.
    foreign_flags: FxHashMap<ExprId, Vec<bool>>,
    foreign_uses: FxHashMap<ExprId, crate::object::ForeignUse>,
    /// While the body of a method is walked: the type of its receiver.
    receiver_of: Option<TypeId>,
    /// The body that is checked takes an address or has a `range`
    /// loop: its pointers are followed (`borrows.rs`).
    lends: bool,
    /// Whether the body that is checked has a `go` statement.
    spawns: bool,
    /// While a part of a literal that is a parent is worked out: a
    /// literal of an abstract type is that part of a child, not a value.
    parent_part: bool,
    /// While a body is walked: the conversions to an interface that are
    /// arguments.
    iface_args: FxHashSet<ExprId>,
    /// While a body is walked: what is known of its variables.
    flow: flow::Flow,
    /// The name that is read as the variable it is, whatever is known of
    /// it, while it is worked out: what is assigned to, compared with nil
    /// or asked whether it is nil.
    unnarrowed: Option<ExprId>,
    /// While a body is walked: the option variables that are read as
    /// what they hold.
    narrowed: FxHashSet<ExprId>,
    /// The place that is assigned to, while it is worked out: an entry of
    /// a map is not read there.
    lookup_target: Option<ExprId>,
    /// While the arguments of a call are assigned: what the function
    /// that is called does with them.
    operands: place::Operands,
    /// While the value of an assignment to one place is walked: the
    /// place, as it is written.
    target: Option<String>,
    /// While the values of a swap are assigned.
    swapping: bool,
    /// The slice expression that `&` is written before: a view of the
    /// elements, and no slice of its own.
    viewed: Option<ExprId>,
    /// While a body is walked: the expressions whose values are put into
    /// an interface, each with its type and the interface type.
    iface_conversions: FxHashMap<ExprId, (TypeId, TypeId)>,
    /// While a body is walked: the cases of its type switches that are
    /// types.
    case_types: FxHashMap<ExprId, TypeId>,
    /// While a body is walked: the interface values that are given to a
    /// place of an interface type with other methods.
    iface_changes: FxHashMap<ExprId, (TypeId, TypeId)>,
    /// While a body is walked: the values among several of one
    /// expression that an interface takes or that change their interface
    /// type.
    part_conversions: FxHashMap<(ExprId, u32), (TypeId, TypeId)>,
    /// The methods and the embedded types of each interface type that was
    /// written, with where they are written: the names of its methods are
    /// checked once every type is known.
    interfaces: Vec<Vec<(Result<Method, TypeId>, Span)>>,
    /// The interface types that were written for variables, with where:
    /// that none is a constraint is checked once every type is known.
    var_types: Vec<(TypeId, Span)>,
    /// The terms of each union that was written, each with where: what
    /// may be a term is checked once every type is known.
    unions: Vec<Vec<(Term, Span)>>,
    /// The generic types and functions with type arguments that were
    /// written.
    instances: Vec<constraints::Instance>,
    /// Where each type parameter is declared.
    param_spans: FxHashMap<TypeId, Span>,
    /// The type arguments of each generic function that is written with
    /// only the first of them, `f[int]`, each with where it is written.
    partial: FxHashMap<ExprId, Vec<(TypeId, Span)>>,
    /// What the value that is assigned next is given to, as a message
    /// names it.
    assigned_to: Option<String>,
    /// While the place of an assignment is checked: the variables that
    /// it writes, each with where. What was known of them holds until
    /// the value on the right is worked out (`n = n.next`).
    written_later: Option<Vec<(LocalId, u32)>>,
    /// While a body is walked: the generic functions and types that it
    /// names, each with its type arguments.
    instantiated: FxHashMap<ExprId, crate::object::Instance>,
    /// The type parameters of the function whose body is checked, as
    /// the body names them: those of its receiver, then its own.
    body_params: Vec<TypeId>,
    /// In a generic body: the type that each place of `var_modes` is
    /// given as, where the type decides whether there is a mark.
    given_types: FxHashMap<ExprId, TypeId>,
    /// The selectors that reach a method of a type parameter, each with
    /// the name of the method.
    param_methods: FxHashMap<ExprId, Spur>,
    /// The receivers of the calls of such methods that are marked in
    /// `var_modes` for the instances, each with its selector.
    receiver_marks: FxHashMap<ExprId, ExprId>,
    /// The steps that the checks of labels and jumps took.
    steps: u64,
    /// The declared types of this package.
    declared: FxHashSet<crate::types::NamedId>,
    /// Where each method of a type of this package is declared.
    method_names: FxHashMap<(crate::types::NamedId, Spur, Option<Spur>), Span>,
    /// The methods of this package, each by where its name is declared:
    /// a method is what needs what its body uses, after the objects.
    method_nodes: FxHashMap<Span, usize>,
    /// The name of each method that is a node, in the order of the nodes.
    methods_declared: Vec<Ident>,
    /// The object that each declaring identifier declares.
    by_span: FxHashMap<Span, usize>,
    /// The constants and variables whose declaration is worked out: what
    /// is used as a value there, they need.
    current: Vec<usize>,
    /// For each object, the constants, variables and functions of the
    /// package that it needs, in the order of their first use.
    needs: Vec<Vec<usize>>,
    needed: FxHashSet<(usize, usize)>,
    /// The objects that are being worked out, the innermost last.
    under_way: Vec<usize>,
    /// The types that an invalid cycle goes through: they stay invalid.
    broken: Vec<bool>,
    /// While the constraints of type parameters are worked out.
    in_type_params: bool,
    /// The declared types and the generic types with arguments that are
    /// still to be checked for holding a value of themselves, in the order
    /// in which they were met.
    to_validate: Vec<TypeId>,
    /// What each `Option[T]` that was written is an option of, with where
    /// and whether in a body: that the type has no nil of its own is
    /// checked once every type is known.
    options: Vec<(TypeId, Span, bool)>,
    /// What each `Unique`, `Shared` and `Weak` type that is written
    /// holds, with where it is written and how: it may hold no pointer,
    /// which is known once every type is (OWN-B5).
    heaped: Vec<(TypeId, Span, String)>,
    /// The element type of each channel type that is written, with
    /// where: it is one whose values may go to another goroutine, which
    /// is known once every type is (CON-14).
    sent: Vec<(TypeId, Span)>,
    /// Whether the types that need instances without end are marked:
    /// what a type is made of is not walked to its end before.
    marked: bool,
    /// The type of each field that was written with `@Required`, with
    /// where the attribute is: what it may be on is checked once every
    /// type is known.
    required_fields: Vec<(TypeId, Span)>,
    /// The types with parents and where each is declared: how they are
    /// copied is asked once the types that need instances without end
    /// are known.
    children: Vec<(TypeId, Span)>,
    /// While a body is walked: the pointer variables that point into what
    /// a `Shared` holds, which is only read through them.
    read_only: FxHashSet<LocalId>,
    /// While a body is walked: the places `&x.f` where `f` is a field
    /// that is never nil, until what takes the address is known.
    required_addresses: Vec<ExprId>,
    /// The types that were written where a value is held, with where and
    /// whether in a body.
    value_types: Vec<(TypeId, Span, bool)>,
    /// The key type of each map type that was written, and where: whether
    /// it can be compared is known once every type is.
    map_keys: Vec<(TypeId, Span)>,
    /// Where each of them was written.
    written_at: FxHashMap<TypeId, Span>,
    /// The types that are known to hold no value of themselves.
    valid: FxHashSet<TypeId>,
    /// While that is asked: the types that were walked already inside
    /// the declared types that the walk is in, by how many those are.
    walked: Vec<FxHashSet<TypeId>>,
}

/// The results of a function.
#[derive(Clone)]
struct Results {
    types: Vec<TypeId>,
    /// The results have names: `return` alone returns them.
    named: bool,
    /// Declared with `!`: the last result is the error that `!` adds.
    fails: bool,
    /// For a function of a `.gopp` file that ends in `error` without
    /// `!`: the first result that may have no zero value, which the
    /// function cannot leave unset.
    unset: Option<TypeId>,
}

/// What was found in one body.
struct BodyTypes {
    exprs: FxHashMap<ExprId, ExprType>,
    locals: Vec<(LocalId, TypeId)>,
    /// The variables that are not also unused.
    excused: Vec<LocalId>,
    selections: FxHashMap<ExprId, Selection>,
    writes_through: FxHashSet<ExprId>,
    iface_conversions: FxHashMap<ExprId, (TypeId, TypeId)>,
    case_types: FxHashMap<ExprId, TypeId>,
    iface_changes: FxHashMap<ExprId, (TypeId, TypeId)>,
    part_conversions: FxHashMap<(ExprId, u32), (TypeId, TypeId)>,
    instances: FxHashMap<ExprId, crate::object::Instance>,
    type_params: Vec<TypeId>,
    given_types: FxHashMap<ExprId, TypeId>,
    param_methods: FxHashMap<ExprId, Spur>,
    receiver_marks: FxHashMap<ExprId, ExprId>,
    bangs: FxHashMap<ExprId, Bang>,
    var_modes: FxHashMap<ExprId, crate::object::Transfer>,
    swaps: FxHashSet<ExprId>,
    self_moves: FxHashSet<ExprId>,
    last_uses: FxHashSet<ExprId>,
    /// What the function and its function literals do with pointers,
    /// each with the number of the declaration; and the function as
    /// those who call it name it.
    lent: Vec<(usize, borrows::Problem)>,
    lender: Option<borrows::Callee>,
    captures: FxHashMap<ExprId, Vec<crate::object::Capture>>,
    method_values: FxHashMap<ExprId, crate::object::MethodValue>,
    narrowed: FxHashSet<ExprId>,
    iface_args: FxHashSet<ExprId>,
    operators: FxHashMap<ExprId, crate::object::OperatorUse>,
    op_assigns: FxHashMap<ExprId, crate::object::OperatorUse>,
    index_stores: FxHashMap<ExprId, crate::object::OperatorUse>,
    case_operators: FxHashMap<ExprId, crate::object::OperatorUse>,
    foreign_uses: FxHashMap<ExprId, crate::object::ForeignUse>,
}

/// A group of constants inside a function: the index of its next constant,
/// and the type and values that a constant without a value repeats.
#[derive(Clone, Default)]
struct LocalGroup {
    next: i64,
    ty: Option<ExprId>,
    values: Vec<ExprId>,
}

/// Works out the type of every package-level object of `package` and the
/// value of every constant, and declares the methods of its types. For
/// each object, the constants, variables and functions of the package that
/// its declaration uses as values.
pub fn declarations(
    package: &mut Package,
    units: &[Unit<'_>],
    types: &Types,
    names: &ThreadedRodeo,
    sink: &Sink,
    evaluator: Option<&dyn crate::consteval::ConstEvaluator>,
) -> (Vec<Vec<usize>>, Vec<Ident>, u64) {
    let count = package.objects.len();
    let by_span = package.objects.iter().enumerate();
    let by_span = by_span
        .map(|(index, object)| (object.span, index))
        .collect();
    let mut checker = Checker {
        names,
        types,
        sink,
        package,
        path: names.get_or_intern(&package.path),
        name: names.get_or_intern(&package.name),
        blank: names.get_or_intern_static("_"),
        decls: vec![None; count],
        state: vec![State::New; count],
        ty: vec![Types::INVALID; count],
        value: vec![Value::Unknown; count],
        incomplete: false,
        reported: FxHashSet::default(),
        shifted: FxHashSet::default(),
        locals: FxHashMap::default(),
        memo: None,
        fresh: false,
        results: Vec::new(),
        suspended: None,
        bangs: FxHashMap::default(),
        called_or_received: false,
        error_at: None,
        local_groups: FxHashMap::default(),
        excused: Vec::new(),
        selections: FxHashMap::default(),
        writes_through: FxHashSet::default(),
        pointer_methods: FxHashSet::default(),
        localized: FxHashSet::default(),
        var_modes: FxHashMap::default(),
        swaps: FxHashSet::default(),
        self_moves: FxHashSet::default(),
        bindings: FxHashMap::default(),
        literals: Vec::new(),
        captures: FxHashMap::default(),
        method_values: FxHashMap::default(),
        callee: None,
        evaluator,
        in_const: false,
        const_fn: None,
        func_marks: vec![crate::types::Marks::default(); count],
        operator_decls: FxHashMap::default(),
        convert_decls: FxHashMap::default(),
        operator_spans: FxHashMap::default(),
        operator_names: FxHashSet::default(),
        operators: FxHashMap::default(),
        op_assigns: FxHashMap::default(),
        index_stores: FxHashMap::default(),
        case_operators: FxHashMap::default(),
        mono: mono::Mono::default(),
        kept: kept::Kept::default(),
        lending: FxHashMap::default(),
        around: Vec::new(),
        aside: None,
        foreign_members: FxHashMap::default(),
        foreign_flags: FxHashMap::default(),
        foreign_uses: FxHashMap::default(),
        receiver_of: None,
        lends: false,
        spawns: false,
        parent_part: false,
        iface_args: FxHashSet::default(),
        flow: flow::Flow::default(),
        unnarrowed: None,
        narrowed: FxHashSet::default(),
        lookup_target: None,
        required_fields: Vec::new(),
        children: Vec::new(),
        value_types: Vec::new(),
        options: Vec::new(),
        heaped: Vec::new(),
        sent: Vec::new(),
        marked: false,
        required_addresses: Vec::new(),
        read_only: FxHashSet::default(),
        operands: place::Operands::Kept,
        target: None,
        swapping: false,
        viewed: None,
        iface_conversions: FxHashMap::default(),
        case_types: FxHashMap::default(),
        iface_changes: FxHashMap::default(),
        part_conversions: FxHashMap::default(),
        interfaces: Vec::new(),
        var_types: Vec::new(),
        unions: Vec::new(),
        instances: Vec::new(),
        param_spans: FxHashMap::default(),
        partial: FxHashMap::default(),
        assigned_to: None,
        written_later: None,
        instantiated: FxHashMap::default(),
        body_params: Vec::new(),
        given_types: FxHashMap::default(),
        param_methods: FxHashMap::default(),
        receiver_marks: FxHashMap::default(),
        steps: 0,
        declared: FxHashSet::default(),
        method_names: FxHashMap::default(),
        method_nodes: FxHashMap::default(),
        methods_declared: Vec::new(),
        by_span,
        current: Vec::new(),
        needs: vec![Vec::new(); count],
        needed: FxHashSet::default(),
        under_way: Vec::new(),
        broken: vec![false; count],
        in_type_params: false,
        to_validate: Vec::new(),
        map_keys: Vec::new(),
        written_at: FxHashMap::default(),
        valid: FxHashSet::default(),
        walked: Vec::new(),
    };
    for (file, unit) in units.iter().enumerate() {
        checker.collect(file, *unit);
    }
    // Go's order: the declared types, then the aliases, then the rest. The
    // methods come before the rest: the type of a variable may be the
    // result of one.
    let is_type = |checker: &Checker, object: usize, is_alias: bool| matches!(checker.decls[object], Some(Decl { form: Form::Type { alias, .. }, .. }) if alias == is_alias);
    for is_alias in [false, true] {
        for object in 0..count {
            if is_type(&checker, object, is_alias) {
                checker.resolve(object, false);
            }
        }
    }
    // Every method has its name before any has its signature, which may
    // name another method.
    for with_signatures in [false, true] {
        for (file, unit) in units.iter().enumerate() {
            checker.methods(file, *unit, with_signatures);
        }
    }
    for object in 0..count {
        checker.resolve(object, false);
    }
    checker.fields_and_methods();
    checker.hierarchies(units);
    checker.validate();
    // A type that needs instances without end is known before a body
    // follows its instances.
    checker.monomorph();
    checker.marked = true;
    checker.validate();
    let mut types_of_files = checker.bodies_aside(units);
    checker.lent_all(units, &mut types_of_files);
    checker.mono_report();
    checker.kept_report();
    let Checker {
        ty,
        value,
        needs,
        methods_declared,
        steps,
        func_marks,
        operator_spans,
        lending,
        ..
    } = checker;
    package.lends = lending;
    package.operators = (operator_spans.into_iter())
        .map(|(method, span)| (span.lo, method))
        .collect();
    for (((object, ty), value), marks) in package
        .objects
        .iter_mut()
        .zip(ty)
        .zip(value)
        .zip(func_marks)
    {
        object.ty = ty;
        object.value = value;
        object.marks = marks;
    }
    for (file, types_of_bodies) in package.files.iter_mut().zip(types_of_files) {
        for (body, found) in file.bodies.iter_mut().zip(types_of_bodies) {
            body.types = found.exprs;
            body.selections = found.selections;
            body.writes_through = found.writes_through;
            body.var_modes = found.var_modes;
            body.swaps = found.swaps;
            body.self_moves = found.self_moves;
            body.last_uses = found.last_uses;
            body.captures = found.captures;
            body.method_values = found.method_values;
            body.narrowed = found.narrowed;
            body.iface_args = found.iface_args;
            body.operators = found.operators;
            body.op_assigns = found.op_assigns;
            body.index_stores = found.index_stores;
            body.case_operators = found.case_operators;
            body.foreign_uses = found.foreign_uses;
            body.iface_conversions = found.iface_conversions;
            body.case_types = found.case_types;
            body.iface_changes = found.iface_changes;
            body.part_conversions = found.part_conversions;
            body.instances = found.instances;
            body.type_params = found.type_params;
            body.given_types = found.given_types;
            body.param_methods = found.param_methods;
            body.receiver_marks = found.receiver_marks;
            body.bangs = found.bangs;
            for (LocalId(local), ty) in found.locals {
                body.locals[local as usize].ty = ty;
            }
            for LocalId(local) in found.excused {
                body.locals[local as usize].used = true;
            }
        }
    }
    (needs, methods_declared, steps)
}

impl<'a> Checker<'a> {
    /// The type `declared` that `expr` writes for a type declaration: no
    /// type parameter, as in Go.
    fn no_param_declared(&mut self, cx: &Cx<'a>, expr: ExprId, declared: TypeId) -> TypeId {
        if !matches!(&*self.types.get(declared), Type::Param(_)) {
            return declared;
        }
        let message = "cannot use a type parameter as RHS in type declaration".to_owned();
        self.error(Code::TypeParamRhs, cx.tree.exprs[expr].span, message);
        Types::INVALID
    }

    /// Reports the diagnostic: at once, or with those of the declaration
    /// that is being checked.
    fn emit(&mut self, diagnostic: Diagnostic) {
        match &mut self.aside {
            Some(aside) => aside.diagnostics.push(diagnostic),
            None => self.sink.push(diagnostic),
        }
    }

    /// Whether nothing was said yet with the code at the place: once is
    /// enough.
    fn report_once(&mut self, key: (Span, Code)) -> bool {
        match &mut self.aside {
            Some(aside) => !self.reported.contains(&key) && aside.reported.insert(key),
            None => self.reported.insert(key),
        }
    }

    /// Checks the bodies of the top-level declarations of all files, side
    /// by side: each worker has a copy of what the declarations gave, and
    /// what a body reports and adds to what all share is taken in the
    /// order of the source, so the result is the same for any number of
    /// workers.
    fn bodies_aside(&mut self, units: &[Unit<'a>]) -> Vec<Vec<BodyTypes>> {
        /// The least source that one worker takes: checking it costs
        /// more than a copy of the checker.
        const SOURCE_EACH: usize = 64 * 1024;
        /// How many bytes of source are checked in the time that one
        /// type of the table is copied, with a margin.
        const COPY_COST: usize = 4;
        let jobs: Vec<Job> = units
            .iter()
            .enumerate()
            .flat_map(|(file, unit)| Self::body_jobs(file, *unit))
            .collect();
        self.foreign_names(units);
        let mut all: Vec<Vec<BodyTypes>> = units.iter().map(|_| Vec::new()).collect();
        // A worker starts with a copy of the checker and of the type
        // table, which costs as much as they are large; the bodies cost
        // as much as there is source. Each worker gets enough source to
        // be worth its copies, and little source has no worker.
        let length = |&(file, index, _): &Job| {
            let tree = units[file].file;
            let span = tree.decl_nodes[tree.decls[index]].span;
            (span.hi - span.lo) as usize
        };
        let source: usize = jobs.iter().map(length).sum();
        let each = SOURCE_EACH.max(COPY_COST * self.types.count());
        let workers = rayon::current_num_threads().min(source / each);
        if workers < 2 {
            for job in &jobs {
                let checked = self.body_aside(units, job);
                all[job.0].push(self.merged(checked));
            }
            self.incomplete = false;
            return all;
        }
        // The declarations in the order of the source, cut into as many
        // runs of about as much source as there are workers.
        let mut runs: Vec<&[Job]> = Vec::new();
        let (mut start, mut taken) = (0, 0);
        for (at, job) in jobs.iter().enumerate() {
            taken += length(job);
            if taken >= source / workers && runs.len() + 1 < workers {
                runs.push(&jobs[start..=at]);
                (start, taken) = (at + 1, 0);
            }
        }
        runs.push(&jobs[start..]);
        let shared = &*self;
        // No worker shares anything that is written: each has its own
        // type table, and what it made there comes into the table of all
        // in the order of the source.
        let mut done: Vec<(Types, Vec<Checked>)> = runs
            .par_iter()
            .map(|run| {
                let table = shared.types.fork();
                let checked = {
                    let mut worker: Checker<'_> = shared.clone();
                    worker.types = &table;
                    run.iter()
                        .map(|job| worker.body_aside(units, job))
                        .collect()
                };
                (table, checked)
            })
            .collect();
        let mut job = jobs.iter();
        for (table, checked) in &mut done {
            let mut kept = Imported::default();
            let main = self.types;
            for (mut types, mut aside, steps) in std::mem::take(checked) {
                let mut ty = |ty: TypeId| main.import(table, &mut kept, ty);
                types.each_type(&mut ty);
                for event in &mut aside.mono {
                    event.each_type(&mut ty);
                }
                let mut named = |named| main.import_named(table, &mut kept, named);
                for event in &mut aside.mono {
                    event.each_named(&mut named);
                }
                if let Some(&(file, ..)) = job.next() {
                    all[file].push(self.merged((types, aside, steps)));
                }
            }
            self.types.add_steps(table.steps());
        }
        // The copies are as large as the table: they end side by side.
        done.into_par_iter().for_each(drop);
        self.incomplete = false;
        all
    }

    /// Goes over what the functions do with pointers and says what is
    /// found. A function says what it does with the pointers that it is
    /// given, so each is looked at after those that it calls: functions
    /// that call each other together, until what they say stays the
    /// same, and all that wait for nothing side by side.
    fn lent_all(&mut self, units: &[Unit<'a>], bodies: &mut [Vec<BodyTypes>]) {
        use borrows::Callee;
        let mut problems = Vec::new();
        let mut lenders = Vec::new();
        // For each problem, which of the bodies of its file it is of.
        let mut checked = Vec::new();
        for (file, of_file) in bodies.iter_mut().enumerate() {
            for (at, body) in of_file.iter_mut().enumerate() {
                let lent = std::mem::take(&mut body.lent);
                checked.extend(lent.iter().map(|_| at));
                problems.extend(
                    lent.into_iter()
                        .map(|(decl, problem)| (file, decl, problem)),
                );
                lenders.extend(body.lender);
            }
        }
        let count = problems.len();
        let mut owned: FxHashMap<Callee, usize> = FxHashMap::default();
        for (at, (_, _, problem)) in problems.iter().enumerate() {
            owned.extend(problem.owner.map(|owner| (owner, at)));
        }
        // What is known before any problem is looked at: of a function
        // of another package, and of one of this package that does
        // nothing with pointers.
        let nothing = Arc::new(Lends::default());
        let mut done: FxHashMap<Callee, Arc<Lends>> = FxHashMap::default();
        for &lender in &lenders {
            if !owned.contains_key(&lender) {
                done.insert(lender, nothing.clone());
            }
        }
        let mut calls: Vec<Vec<usize>> = Vec::with_capacity(count);
        for (_, _, problem) in &problems {
            let mut called = Vec::new();
            for call in &problem.calls {
                self.steps += 1;
                if let Some(&at) = owned.get(&call.callee) {
                    called.push(at);
                    continue;
                }
                let known = match call.callee {
                    Callee::Imported(import, member) => {
                        match &self.package.objects[import.0 as usize].kind {
                            ObjKind::Import { package, .. } => package.lends.get(&member).cloned(),
                            _ => None,
                        }
                    }
                    Callee::Method(named, name) => self.types.lends(named, name),
                    _ => None,
                };
                if let Some(known) = known {
                    done.entry(call.callee).or_insert(known);
                }
            }
            calls.push(called);
        }
        let groups = borrows::groups(&calls, &mut self.steps);
        // The groups that wait for no other, then those that wait for
        // these only, and so on.
        let mut group_of = vec![0; count];
        for (at, group) in groups.iter().enumerate() {
            for &member in group {
                group_of[member] = at;
            }
        }
        let mut level = vec![0usize; groups.len()];
        let mut levels: Vec<Vec<usize>> = Vec::new();
        for (at, group) in groups.iter().enumerate() {
            let called = group.iter().flat_map(|&member| &calls[member]);
            let below = called.filter(|&&to| group_of[to] != at);
            level[at] = below.map(|&to| level[group_of[to]] + 1).max().unwrap_or(0);
            if levels.len() <= level[at] {
                levels.resize(level[at] + 1, Vec::new());
            }
            levels[level[at]].push(at);
        }
        let mut solved: Vec<Option<borrows::Lent>> = (0..count).map(|_| None).collect();
        for same in &levels {
            let found: Vec<Vec<(usize, borrows::Lent)>> = same
                .par_iter()
                .map(|&group| borrows::solve_together(&groups[group], &problems, &calls, &done))
                .collect();
            for (member, lent) in found.into_iter().flatten() {
                if let Some(owner) = problems[member].2.owner {
                    done.insert(owner, Arc::new(lent.lends.clone()));
                }
                solved[member] = Some(lent);
            }
        }
        for (((file, decl, problem), found), at) in problems.iter().zip(&solved).zip(checked) {
            let Some(found) = found else {
                continue;
            };
            self.steps += found.steps;
            let body = &self.package.files[*file].bodies[*decl];
            let text = units[*file].source.text();
            let types = &bodies[*file][at];
            let carried = |carrier| match carrier {
                borrows::Carrier::Expr(expr) => types.exprs.get(&expr).map(|typed| typed.ty),
                borrows::Carrier::Local(local) => {
                    let found = types.locals.iter().find(|(declared, _)| *declared == local);
                    found.map(|&(_, ty)| ty)
                }
            };
            self.lent((body, text), &carried, &problem.broken, found);
        }
        // For those who call the functions of this package.
        for lender in lenders {
            let Some(lends) = done.get(&lender).cloned() else {
                continue;
            };
            match lender {
                Callee::Func(object) => drop(self.lending.insert(object, lends)),
                Callee::Method(named, name) => self.types.set_lends(named, name, lends),
                _ => {}
            }
        }
    }

    /// Checks one declaration aside: what it reports and adds to what
    /// all share comes back with what was found in it.
    fn body_aside(&mut self, units: &[Unit<'a>], &(file, index, repeated): &Job) -> Checked {
        self.aside = Some(Aside::default());
        let before = self.steps;
        let types = self.body(file, units[file], index, repeated);
        let aside = self.aside.take().unwrap_or_default();
        (types, aside, self.steps - before)
    }

    /// Takes what a declaration that was checked aside reported and
    /// added. The declarations are taken in the order of the source.
    fn merged(&mut self, (types, aside, steps): Checked) -> BodyTypes {
        self.steps += steps;
        // What an earlier declaration said at a place is not said again.
        let mut said = Vec::new();
        for diagnostic in aside.diagnostics {
            let key = (diagnostic.span, diagnostic.code);
            if !self.reported.contains(&key) {
                said.push(key);
                self.sink.push(diagnostic);
            }
        }
        self.reported.extend(said);
        self.reported.extend(aside.reported);
        // Each was found once, by the declaration that needs it.
        for (from, object) in aside.needs {
            self.needed.insert((from, object));
            self.needs[from].push(object);
        }
        self.mono_replay(aside.mono);
        types
    }

    /// The type parameter `param` is declared at `span`; with `again`,
    /// also where it has a place already.
    fn param_span(&mut self, param: TypeId, span: Span, again: bool) {
        if again || !self.param_spans.contains_key(&param) {
            self.param_spans.insert(param, span);
            if let Some(aside) = &mut self.aside {
                aside.mono.push(mono::Event::Declared(param, span));
            }
        }
    }
}

fn is_exported(name: &str) -> bool {
    name.starts_with(char::is_uppercase)
}

fn is_value_kind(kind: &ObjKind) -> bool {
    matches!(kind, ObjKind::Const | ObjKind::Var)
}

impl<'a> Checker<'a> {
    fn collect(&mut self, file: usize, unit: Unit<'a>) {
        let tree = unit.file;
        let bodies = &self.package.files[file].bodies;
        // The constant that a later one of its group repeats, and how many
        // constants each group has so far.
        let mut repeated: Option<Repeated<'a>> = None;
        let mut seen: FxHashMap<u32, i64> = FxHashMap::default();
        for (index, &decl) in tree.decls.iter().enumerate() {
            let node = &tree.decl_nodes[decl];
            let mut body = &bodies[index];
            let mut declare = |name: Ident, body: &'a Body, form: Form<'a>| {
                if let Some(&object) = self.by_span.get(&name.span) {
                    self.decls[object] = Some(Decl { unit, body, form });
                }
            };
            match &node.kind {
                DeclKind::Const { names, ty, values } => {
                    let iota = match node.group {
                        Some(group) => {
                            let next = seen.entry(group).or_insert(0);
                            *next += 1;
                            *next - 1
                        }
                        None => 0,
                    };
                    let (mut ty, mut values) = (*ty, values.as_slice());
                    let mut repeats = false;
                    match repeated {
                        // Only inside one `const ( … )`.
                        Some(last)
                            if ty.is_none()
                                && values.is_empty()
                                && node.group.is_some()
                                && last.group == node.group =>
                        {
                            (ty, values, body) = (last.ty, last.values, last.body);
                            repeats = true;
                        }
                        _ => {
                            repeated = Some(Repeated {
                                group: node.group,
                                ty,
                                values,
                                body,
                            });
                        }
                    }
                    for (position, &name) in names.iter().enumerate() {
                        let value = values.get(position).copied();
                        let form = Form::Const {
                            ty,
                            value,
                            iota,
                            repeats,
                        };
                        declare(name, body, form);
                    }
                }
                DeclKind::Var { names, ty, values } => {
                    for (position, &name) in names.iter().enumerate() {
                        // `a, b = f()` gives each one result of the call.
                        let value = match values.len() {
                            1 => Some((values[0], position, names.len())),
                            _ => values.get(position).map(|&value| (value, 0, 1)),
                        };
                        declare(name, body, Form::Var { ty: *ty, value });
                    }
                }
                DeclKind::Type {
                    name,
                    type_params,
                    alias,
                    ty,
                } => {
                    let marked = |with: &str| {
                        node.attrs.iter().any(|attr| {
                            attr.path.package.is_none()
                                && self.names.resolve(&attr.path.name.name) == with
                        })
                    };
                    let form = Form::Type {
                        alias: *alias,
                        type_params,
                        ty: *ty,
                        extends: marked("Extends"),
                        pinned: marked("Pinned"),
                        attrs: &node.attrs,
                    };
                    declare(*name, body, form);
                }
                DeclKind::Func {
                    is_macro: false,
                    receiver: None,
                    name,
                    type_params,
                    ty,
                    ..
                } => {
                    let form = Form::Func {
                        type_params,
                        ty: *ty,
                    };
                    declare(*name, body, form);
                    let own = node.attrs.iter().filter(|attr| attr.path.package.is_none());
                    let marks = own
                        .filter_map(|attr| {
                            crate::types::Marks::of(self.names.resolve(&attr.path.name.name))
                        })
                        .fold(crate::types::Marks::default(), crate::types::Marks::with);
                    if let Some(&object) = self.by_span.get(&name.span) {
                        self.func_marks[object] = marks;
                    }
                }
                _ => {}
            }
        }
    }

    fn cx(&self, decl: &Decl<'a>, iota: Option<i64>) -> Cx<'a> {
        Cx {
            tree: decl.unit.file,
            body: decl.body,
            source: decl.unit.source,
            text: decl.unit.source.text(),
            iota,
            params: FxHashMap::default(),
        }
    }

    /// Gives each type parameter of a declaration a type, then each its
    /// constraint, which may name the others.
    pub(super) fn declare_params(
        &mut self,
        cx: &mut Cx<'a>,
        type_params: &[TypeParam],
    ) -> Vec<TypeId> {
        let params = self.new_params(type_params);
        for (param, &ty) in type_params.iter().zip(&params) {
            if let Some(&local) = cx.body.defs.get(&param.name.span.lo) {
                cx.params.insert(local, ty);
            }
        }
        self.constrain(cx, type_params, &params);
        params
    }

    fn new_params(&mut self, type_params: &[TypeParam]) -> Vec<TypeId> {
        let mut params = Vec::new();
        for param in type_params {
            let ty = self.types.param(param.name.name);
            self.param_span(ty, param.name.span, true);
            params.push(ty);
        }
        params
    }

    /// Gives the type parameters the constraints that are written for
    /// them.
    fn constrain(&mut self, cx: &Cx<'a>, type_params: &[TypeParam], params: &[TypeId]) {
        // A constraint that is not worked out leaves the declaration as
        // well known as it was.
        let incomplete = self.incomplete;
        let outer = std::mem::replace(&mut self.in_type_params, true);
        for (param, &ty) in type_params.iter().zip(params) {
            self.types.set_elements(ty);
            let mut constraint = self.typ(cx, param.bound);
            if matches!(&*self.types.get(constraint), Type::Param(_)) {
                let message = "cannot use a type parameter as constraint".to_owned();
                let span = cx.tree.exprs[param.bound].span;
                self.error(Code::TypeParamBoundNotInterface, span, message);
                constraint = self.types.any();
            }
            self.types.set_constraint(ty, constraint);
        }
        self.in_type_params = outer;
        self.incomplete = incomplete;
    }

    /// Works out the object, unless that is done. An object that is
    /// under way needs itself: `as_type` says that it is named as a type.
    fn resolve(&mut self, object: usize, as_type: bool) {
        match self.state[object] {
            State::Done => return,
            State::Busy => return self.cycle(object, as_type),
            State::New => {}
        }
        self.state[object] = State::Busy;
        self.under_way.push(object);
        // Only what the declaration of a constant or a variable uses is
        // needed when the program starts.
        let is_value = matches!(
            self.package.objects[object].kind,
            ObjKind::Const | ObjKind::Var
        );
        let current = match is_value {
            true => vec![object],
            false => Vec::new(),
        };
        let current = std::mem::replace(&mut self.current, current);
        let error_at = self.error_at.take();
        let in_type_params = std::mem::replace(&mut self.in_type_params, false);
        let outer = std::mem::replace(&mut self.incomplete, false);
        let defines_type = self.define(object);
        let incomplete = std::mem::replace(&mut self.incomplete, outer);
        self.in_type_params = in_type_params;
        self.error_at = error_at;
        self.current = current;
        self.under_way.pop();
        if self.broken[object] {
            self.ty[object] = Types::INVALID;
        }
        // A declared type can be named before all of it is known; what
        // else is not fully known is not known at all.
        if incomplete && !defines_type {
            self.ty[object] = Types::INVALID;
            self.value[object] = Value::Unknown;
            self.incomplete = true;
        }
        self.state[object] = State::Done;
    }

    /// The object `object`, which is being worked out, is needed for
    /// itself. That is an error unless the cycle is one of declared types
    /// alone, or one of constants and variables alone, which is reported
    /// with the order of initialization.
    fn cycle(&mut self, object: usize, as_type: bool) {
        let objects = &self.package.objects;
        let checked = match objects[object].kind {
            ObjKind::Const | ObjKind::Var => self.ty[object] == Types::INVALID,
            ObjKind::Type => as_type,
            _ => false,
        };
        let Some(start) = self.under_way.iter().rposition(|&member| member == object) else {
            return;
        };
        if !checked {
            return;
        }
        let cycle = &self.under_way[start..];
        let (mut values, mut defined, mut through_params) = (0, 0, false);
        for &member in cycle {
            match (
                &objects[member].kind,
                self.decls[member].map(|decl| decl.form),
            ) {
                (ObjKind::Const | ObjKind::Var, _) => values += 1,
                (
                    ObjKind::Type,
                    Some(Form::Type {
                        alias, type_params, ..
                    }),
                ) => {
                    // A generic type may not be needed for its own type
                    // parameters.
                    if self.in_type_params && !type_params.is_empty() {
                        through_params = true;
                        break;
                    }
                    defined += usize::from(!alias);
                }
                _ => {}
            }
        }
        let valid = values == cycle.len() || values == 0 && defined > 0;
        if valid && !through_params {
            return;
        }
        // Reported at the member that comes first in the source.
        let first = (0..cycle.len())
            .min_by_key(|&at| (objects[cycle[at]].span.file, objects[cycle[at]].span.lo))
            .unwrap_or(0);
        let name = |member: usize| self.names.resolve(&objects[member].name);
        let at = cycle[first];
        let is_type = matches!(objects[at].kind, ObjKind::Type);
        let diagnostic = match (cycle.len(), is_type) {
            (1, true) => {
                let message = format!("invalid recursive type: {} refers to itself", name(at));
                Diagnostic::new(Code::InvalidRecursiveTypeSelf, objects[at].span, message)
            }
            (1, false) => {
                let message = format!(
                    "invalid cycle in declaration: {} refers to itself",
                    name(at)
                );
                Diagnostic::new(Code::InvalidCycle, objects[at].span, message)
            }
            (len, _) => {
                let (code, message) = match is_type {
                    true => (
                        Code::InvalidRecursiveType,
                        format!("invalid recursive type {}", name(at)),
                    ),
                    false => (
                        Code::InvalidCycle,
                        format!("invalid cycle in declaration of {}", name(at)),
                    ),
                };
                let mut diagnostic = Diagnostic::new(code, objects[at].span, message);
                for step in 0..len {
                    let (from, to) = (cycle[(first + step) % len], cycle[(first + step + 1) % len]);
                    let note = format!("{} refers to {}", name(from), name(to));
                    diagnostic =
                        diagnostic.with_note(Code::NoteRefersTo, Some(objects[from].span), note);
                }
                diagnostic
            }
        };
        if self.report_once((objects[at].span, diagnostic.code)) {
            self.emit(diagnostic);
        }
        if is_value_kind(&objects[object].kind) {
            return;
        }
        self.broken[object] = true;
        self.ty[object] = Types::INVALID;
    }

    /// A declared type, or a generic type with arguments, was written at
    /// `span`: it is checked later for holding a value of itself.
    fn written(&mut self, ty: TypeId, span: Span) {
        if self.written_at.insert(ty, span).is_none() {
            self.to_validate.push(ty);
        }
    }

    /// Reports each type met so far that is declared as itself or holds a
    /// value of itself.
    fn validate(&mut self) {
        let mut next = 0;
        while let Some(&ty) = self.to_validate.get(next) {
            self.declared_as_itself(ty);
            self.walked.clear();
            self.holds_itself_not(ty, &mut Vec::new(), &mut Vec::new());
            next += 1;
        }
        self.to_validate.clear();
        for written in std::mem::take(&mut self.interfaces) {
            self.interface_methods(&written);
        }
        for (ty, span) in std::mem::take(&mut self.var_types) {
            self.no_constraint(ty, span);
        }
        for (ty, span, in_body) in std::mem::take(&mut self.value_types) {
            self.held_abstract(ty, span, in_body);
        }
        for terms in std::mem::take(&mut self.unions) {
            self.union_terms(&terms);
        }
        for (of, span, in_body) in std::mem::take(&mut self.options) {
            self.option_of(of, span, in_body);
        }
        if self.marked {
            for (elem, span) in std::mem::take(&mut self.sent) {
                self.sendable(elem, span);
            }
            for (ty, span) in std::mem::take(&mut self.children) {
                self.copied_as_parents(ty, span);
            }
        }
        for (held, span, written) in std::mem::take(&mut self.heaped) {
            match self.types.held(held) {
                Held::Pointer => {
                    let message = format!(
                        "invalid type {written}: a {} holds a pointer, and what holds one is not \
                         kept on the heap",
                        self.type_string(held)
                    );
                    self.error(Code::InvalidHandleType, span, message);
                }
                Held::Params(params) if params.is_empty() => {}
                Held::Params(params) => self.kept_where(&params, span, &own::kept_in(&written)),
            }
        }
        for (ty, span) in std::mem::take(&mut self.required_fields) {
            self.required_field(ty, span);
        }
        for instance in std::mem::take(&mut self.instances) {
            self.verify(&instance);
        }
        for (key, span) in std::mem::take(&mut self.map_keys) {
            // What a type parameter can be compared with is not decided
            // here yet.
            let unsettled =
                self.types.underlying(key) == Types::INVALID || self.types.has_param(key);
            if !unsettled && !self.types.comparable(key) {
                let message = format!("invalid map key type {}", self.type_string(key));
                self.error(Code::InvalidMapKey, span, message);
            }
            // A type parameter is a key if its constraint says that its
            // values compare.
            let is_param = matches!(&*self.types.get(key), Type::Param(_));
            if is_param && !self.types.comparable(key) {
                let message = format!(
                    "invalid map key type {} (missing comparable constraint)",
                    self.type_string(key)
                );
                self.error(Code::InvalidMapKey, span, message);
            }
        }
    }

    /// Where a generic type with arguments was written: one that came
    /// from the declaration of another, `from`, where that one was.
    fn place(&mut self, ty: TypeId, from: TypeId) {
        let is_instance =
            matches!(&*self.types.get(ty), Type::Named { args, .. } if !args.is_empty());
        if let (true, Some(&span)) = (is_instance, self.written_at.get(&from)) {
            self.written_at.entry(ty).or_insert(span);
        }
    }

    /// `type A B` with `type B A`: a declared type that is declared as
    /// itself, through others, is no type.
    fn declared_as_itself(&mut self, start: TypeId) {
        // A generic type may give itself other arguments at every step.
        const STEPS: usize = 64;
        let mut chain: Vec<TypeId> = Vec::new();
        let mut current = start;
        let cycle = loop {
            let Type::Named { id, args } = &*self.types.get(current) else {
                return;
            };
            if let Some(at) = chain.iter().position(|&seen| seen == current) {
                break chain.split_off(at);
            }
            if chain.len() == STEPS {
                break vec![start];
            }
            chain.push(current);
            let Some(declared_as) = self.types.declared_as(*id) else {
                return;
            };
            let params = self.types.type_params(*id);
            let next = match args.len() == params.len() {
                true => self.types.substitute(declared_as, &params, args),
                false => declared_as,
            };
            self.place(next, current);
            current = next;
        };
        if cycle.iter().any(|ty| self.is_invalid(*ty)) {
            return;
        }
        // Reported at the member that comes first in the source.
        let position = |ty: &TypeId| self.written_at.get(ty).map(|span| (span.file, span.lo));
        let first = (0..cycle.len()).min_by_key(|&at| position(&cycle[at]));
        let mut rotated = cycle.clone();
        rotated.rotate_left(first.unwrap_or(0));
        self.report_type_cycle(&rotated);
        for ty in cycle {
            if let Type::Named { id, .. } = &*self.types.get(ty) {
                self.types.invalidate(*id);
            }
        }
    }

    fn is_invalid(&self, ty: TypeId) -> bool {
        matches!(&*self.types.get(ty), Type::Named { id, .. } if self.types.is_invalid(*id))
    }

    /// Whether no value of the type `ty` holds a value of a type of
    /// `nest`, the declared types that the walk is inside of, nor of
    /// itself. `path` is all declared types that the walk went through.
    /// This is `validType` of Go's type checker.
    fn holds_itself_not(
        &mut self,
        ty: TypeId,
        nest: &mut Vec<TypeId>,
        path: &mut Vec<TypeId>,
    ) -> bool {
        gopp_syntax::with_stack(|| self.holds_itself_not_here(ty, nest, path))
    }

    fn holds_itself_not_here(
        &mut self,
        ty: TypeId,
        nest: &mut Vec<TypeId>,
        path: &mut Vec<TypeId>,
    ) -> bool {
        let node = self.types.get(ty);
        // What a type holds is the same wherever it is met, unless it is
        // written with a type parameter, which stands for another type
        // in each place; then it is the same while the walk is inside
        // the same declared types.
        let anywhere = !self.types.has_param(ty);
        let parts = matches!(
            &*node,
            Type::Array { .. }
                | Type::Option(_)
                | Type::Struct(_)
                | Type::Union(_)
                | Type::Interface { .. }
        );
        if parts {
            if self.valid.contains(&ty) {
                return true;
            }
            if self.walked.len() <= nest.len() {
                self.walked.resize_with(nest.len() + 1, FxHashSet::default);
            }
            if !anywhere && !self.walked[nest.len()].insert(ty) {
                return true;
            }
        }
        let valid = match &*node {
            Type::Array { elem, .. } | Type::Option(elem) => {
                self.holds_itself_not(*elem, nest, path)
            }
            Type::Struct(fields) => fields
                .iter()
                .all(|field| self.holds_itself_not(field.ty, nest, path)),
            Type::Union(terms) => terms
                .iter()
                .all(|term| self.holds_itself_not(term.ty, nest, path)),
            Type::Interface { embedded, .. } => embedded
                .iter()
                .all(|&embedded| self.holds_itself_not(embedded, nest, path)),
            _ => return self.named_holds_itself_not(ty, nest, path),
        };
        if valid && anywhere {
            self.valid.insert(ty);
        }
        valid
    }

    /// `holds_itself_not` for what has no parts of its own: a declared
    /// type, which holds what it is declared as, and a type parameter.
    fn named_holds_itself_not(
        &mut self,
        ty: TypeId,
        nest: &mut Vec<TypeId>,
        path: &mut Vec<TypeId>,
    ) -> bool {
        match &*self.types.get(ty) {
            Type::Named { id, .. } => {
                if self.types.underlying(ty) == Types::INVALID {
                    return false;
                }
                if nest.contains(&ty) {
                    self.types.invalidate(*id);
                    let start = path.iter().position(|&on_path| on_path == ty);
                    self.report_type_cycle(&path[start.unwrap_or(0)..]);
                    return false;
                }
                if self.valid.contains(&ty) {
                    return true;
                }
                let Some(declared_as) = self.types.declared_as(*id) else {
                    return false;
                };
                if let Some(&around) = nest.last() {
                    self.place(ty, around);
                }
                if self.walked.len() <= nest.len() + 1 {
                    self.walked.resize_with(nest.len() + 2, FxHashSet::default);
                }
                if !self.walked[nest.len()].insert(ty) {
                    return true;
                }
                nest.push(ty);
                path.push(ty);
                // Inside this type nothing was walked yet.
                self.walked[nest.len()].clear();
                let valid = self.holds_itself_not(declared_as, nest, path);
                nest.pop();
                path.pop();
                if valid && !self.types.has_param(ty) {
                    self.valid.insert(ty);
                }
                valid
            }
            // Inside a generic type with arguments, a type parameter
            // stands for its argument, which was written outside that type.
            Type::Param(_) => {
                let Some(&inside) = nest.last() else {
                    return true;
                };
                let Type::Named { id, args } = &*self.types.get(inside) else {
                    return true;
                };
                let params = self.types.type_params(*id);
                let position = params.iter().position(|&param| param == ty);
                let Some(&arg) = position.and_then(|position| args.get(position)) else {
                    return true;
                };
                nest.pop();
                let valid = self.holds_itself_not(arg, nest, path);
                nest.push(inside);
                valid
            }
            _ => true,
        }
    }

    /// `cycle` is types of this package, each of which holds a value of
    /// the next, and the last of the first.
    fn report_type_cycle(&mut self, cycle: &[TypeId]) {
        let described = |ty: &TypeId| {
            let Type::Named { id, args } = &*self.types.get(*ty) else {
                return None;
            };
            // A generic type that got its arguments from another one was
            // written where it was declared.
            let origin = self.types.named(*id, &[]);
            let span = self.written_at.get(ty).filter(|_| !args.is_empty());
            let span = span.or_else(|| self.written_at.get(&origin))?;
            Some((self.names.resolve(&self.types.name_of(*id)), *span))
        };
        let Some(members) = cycle.iter().map(described).collect::<Option<Vec<_>>>() else {
            return;
        };
        let Some(&(name, span)) = members.first() else {
            return;
        };
        if !self.report_once((span, Code::InvalidRecursiveType)) {
            return;
        }
        if members.len() == 1 {
            let message = format!("invalid recursive type: {name} refers to itself");
            let diagnostic = Diagnostic::new(Code::InvalidRecursiveTypeSelf, span, message);
            return self.emit(diagnostic);
        }
        let message = format!("invalid recursive type {name}");
        let mut diagnostic = Diagnostic::new(Code::InvalidRecursiveType, span, message);
        for (step, &(from, at)) in members.iter().enumerate() {
            let (to, _) = members[(step + 1) % members.len()];
            let note = format!("{from} refers to {to}");
            diagnostic = diagnostic.with_note(Code::NoteRefersTo, Some(at), note);
        }
        self.emit(diagnostic);
    }

    /// Works out one declaration; whether it declares a new type.
    fn define(&mut self, object: usize) -> bool {
        let Some(decl) = self.decls[object] else {
            return false;
        };
        match decl.form {
            Form::Const {
                ty,
                value,
                iota,
                repeats,
            } => {
                let cx = self.cx(&decl, Some(iota));
                let declared = ty.map(|ty| self.const_type(&cx, ty));
                let Some(value) = value.filter(|_| declared != Some(Types::INVALID)) else {
                    self.incomplete = true;
                    return false;
                };
                self.error_at = repeats.then_some(self.package.objects[object].span);
                let x = self.const_value(&cx, declared, value);
                self.error_at = None;
                match x.mode {
                    Mode::Const(value) => (self.ty[object], self.value[object]) = (x.ty, value),
                    _ => self.incomplete = true,
                }
            }
            Form::Var { ty: Some(ty), .. } => {
                let cx = self.cx(&decl, None);
                self.ty[object] = self.var_type(&cx, ty);
            }
            // The type of a variable without one is the type of its value.
            Form::Var { ty: None, value } => {
                let cx = self.cx(&decl, None);
                let ty = value.and_then(|(value, position, count)| {
                    self.result_type(&cx, value, position, count)
                });
                match ty {
                    // A generic function is no value without its type
                    // arguments.
                    Some(ty) if self.types.generic_func(ty).is_none() => {
                        self.ty[object] = self.types.default_type(ty);
                    }
                    _ => self.incomplete = true,
                }
            }
            Form::Type {
                alias,
                type_params,
                ty,
                extends,
                pinned,
                attrs,
            } => {
                let mut cx = self.cx(&decl, None);
                let params = self.declare_params(&mut cx, type_params);
                if alias {
                    let aliased = self.typ(&cx, ty);
                    let name = self.package.objects[object].name;
                    // What is not fully known is not known at all.
                    self.ty[object] = match self.incomplete {
                        true => aliased,
                        false => self.alias(name, params, aliased),
                    };
                    return false;
                }
                // The type exists before what it is declared as is known,
                // so that it can name itself.
                let named = self
                    .types
                    .declare(self.package.objects[object].name, self.name);
                self.types.set_home(named, self.path, false);
                self.declared.insert(named);
                self.ty[object] = self.types.named(named, &[]);
                self.written(self.ty[object], self.package.objects[object].span);
                self.mono_owner(&params, named);
                self.types.set_type_params(named, params);
                if extends {
                    self.types.set_extends(named);
                }
                if pinned {
                    self.types.set_pinned(named);
                }
                let underlying = self.typ(&cx, ty);
                let underlying = self.no_param_declared(&cx, ty, underlying);
                if !self.incomplete {
                    self.types.set_underlying(named, underlying);
                }
                self.inherit(&cx, named, attrs);
                return true;
            }
            Form::Func { type_params, ty } => {
                let mut cx = self.cx(&decl, None);
                let params = self.declare_params(&mut cx, type_params);
                let sig = self.typ(&cx, ty);
                self.ty[object] = self.types.generic_func_type(sig, &params);
            }
        }
        false
    }

    /// The alias `name` of `aliased`: the same type, written by the name
    /// of the alias; a generic alias without its type arguments if it
    /// has the type parameters `params`.
    fn alias(&mut self, name: Spur, params: Vec<TypeId>, aliased: TypeId) -> TypeId {
        if aliased == Types::INVALID {
            return aliased;
        }
        let named = self.types.declare(name, self.name);
        let generic = !params.is_empty();
        self.types.set_type_params(named, params);
        self.types.set_alias(named, aliased);
        match generic {
            true => self.types.named(named, &[]),
            false => self.types.alias(named, &[]).unwrap_or(aliased),
        }
    }

    /// `count` variables their values and the one at `position` this.
    fn result_type(
        &mut self,
        cx: &Cx<'a>,
        expr: ExprId,
        position: usize,
        count: usize,
    ) -> Option<TypeId> {
        let operand = self.operand(cx, expr);
        if !operand.is_value() || operand.ty == self.types.basic(Basic::UntypedNil) {
            return None;
        }
        let results = match &*self.types.get(operand.ty) {
            Type::Tuple(types) => types.to_vec(),
            _ => vec![operand.ty],
        };
        if results.len() == count {
            return results.get(position).copied();
        }
        // `v, ok = m[k]`, `v, ok = x.(T)`, `v, ok = <-ch`.
        let mut inner = expr;
        while let ExprKind::Paren(x) = cx.tree.exprs[inner].kind {
            inner = x;
        }
        let comma_ok = match &cx.tree.exprs[inner].kind {
            ExprKind::Assert { .. } => true,
            ExprKind::Unary { op, .. } => *op == UnaryOp::Recv,
            ExprKind::Index { x, .. } => {
                let indexed = self.operand(cx, *x);
                let under = self.types.get(self.types.underlying(indexed.ty));
                matches!(&*under, Type::Map { .. })
            }
            _ => false,
        };
        match (comma_ok && count == 2 && results.len() == 1, position) {
            (true, 0) => Some(results[0]),
            (true, _) => Some(self.types.basic(Basic::Bool)),
            _ => None,
        }
    }

    /// The type of the object `object` of this package, which is named
    /// as a type or as a value.
    pub(super) fn object_type(&mut self, object: ObjId, as_type: bool) -> TypeId {
        self.resolve(object.0 as usize, as_type);
        self.ty[object.0 as usize]
    }

    /// What is being declared needs the object `object`, or the method
    /// that is the node `object`, for its value.
    pub(super) fn need(&mut self, object: usize) {
        let kind = self.package.objects.get(object).map(|object| &object.kind);
        if !matches!(
            kind,
            None | Some(ObjKind::Const | ObjKind::Var | ObjKind::Func)
        ) {
            return;
        }
        for at in 0..self.current.len() {
            let from = self.current[at];
            if self.needed.insert((from, object)) {
                if let Some(aside) = &mut self.aside {
                    aside.needs.push((from, object));
                    continue;
                }
                self.needs[from].push(object);
            }
        }
    }

    /// The package of a field or method name: a name that is not exported
    /// belongs to its package.
    pub(super) fn package_of(&self, name: Spur) -> Option<Spur> {
        (!is_exported(self.names.resolve(&name))).then_some(self.path)
    }

    // ---- types

    fn predeclared(&mut self, name: Predeclared) -> TypeId {
        use crate::types::Marker;
        let basic = match name {
            Predeclared::Any => return self.types.any(),
            Predeclared::Byte => return self.types.byte(),
            Predeclared::Rune => return self.types.rune(),
            Predeclared::Error => return self.types.error(),
            Predeclared::Comparable => return self.types.intern(Type::Marker(Marker::Comparable)),
            Predeclared::Copy => return self.types.intern(Type::Marker(Marker::Copy)),
            Predeclared::Clone => return self.types.intern(Type::Marker(Marker::Clone)),
            Predeclared::Send => return self.types.intern(Type::Marker(Marker::Send)),
            Predeclared::Sync => return self.types.intern(Type::Marker(Marker::Sync)),
            Predeclared::Bool => Basic::Bool,
            Predeclared::Uint8 => Basic::Uint8,
            Predeclared::Complex64 => Basic::Complex64,
            Predeclared::Complex128 => Basic::Complex128,
            Predeclared::Float32 => Basic::Float32,
            Predeclared::Float64 => Basic::Float64,
            Predeclared::Int => Basic::Int,
            Predeclared::Int8 => Basic::Int8,
            Predeclared::Int16 => Basic::Int16,
            Predeclared::Int32 => Basic::Int32,
            Predeclared::Int64 => Basic::Int64,
            Predeclared::String => Basic::String,
            Predeclared::Uint => Basic::Uint,
            Predeclared::Uint16 => Basic::Uint16,
            Predeclared::Uint32 => Basic::Uint32,
            Predeclared::Uint64 => Basic::Uint64,
            Predeclared::Uintptr => Basic::Uintptr,
            // A generic type without its argument, or no type at all.
            _ => {
                self.incomplete = true;
                return Types::INVALID;
            }
        };
        self.types.basic(basic)
    }

    /// What a name or `pkg.Name` in a type position denotes, if it is a
    /// type.
    fn named_type(&mut self, cx: &Cx<'a>, expr: ExprId) -> Option<TypeId> {
        match *cx.body.uses.get(&expr)? {
            Ref::Universe(name) if name.kind() == PredeclaredKind::Type => {
                Some(self.predeclared(name))
            }
            Ref::Object(object) if matches!(self.package.object(object).kind, ObjKind::Type) => {
                Some(self.object_type(object, true))
            }
            Ref::Member { import, member } => {
                let ObjKind::Import { package, .. } = &self.package.object(import).kind else {
                    return None;
                };
                let object = package.object(member);
                matches!(object.kind, ObjKind::Type).then_some(object.ty)
            }
            Ref::Local(local) => cx.params.get(&local).copied().or_else(|| {
                match self.locals.get(&local) {
                    Some(declared) => (declared.mode == Mode::Type).then_some(declared.ty),
                    // An alias that names itself in its own declaration.
                    None => {
                        let declared = &cx.body.locals[local.0 as usize];
                        self.incomplete = true;
                        if declared.kind != LocalKind::Type {
                            return None;
                        }
                        let name = self.names.resolve(&declared.name);
                        let message = format!("invalid recursive type: {name} refers to itself");
                        self.error(Code::InvalidRecursiveTypeSelf, declared.span, message);
                        Some(Types::INVALID)
                    }
                }
            }),
            _ => None,
        }
    }

    /// The selector `x.sel` stands where a type is written and is none.
    fn selector_is_no_type(&mut self, cx: &Cx<'a>, expr: ExprId, x: ExprId, sel: Ident) {
        // A member of a package that is something else.
        if matches!(cx.body.uses.get(&expr), Some(Ref::Member { .. })) {
            let member = self.operand(cx, expr);
            if member.mode != Mode::Unknown {
                let message = format!("{} is not a type", self.describe(cx, expr, &member));
                self.error(Code::NotAType, cx.tree.exprs[expr].span, message);
            }
            return;
        }
        let base = self.operand(cx, x);
        let base = self.instantiated(cx, x, base);
        if base.mode == Mode::Unknown {
            return;
        }
        // `type T T.x`: the type is declared as a part of itself.
        let declared = self.under_way.last().copied().filter(|&object| {
            let form = self.decls[object].map(|decl| decl.form);
            base.mode == Mode::Type
                && self.ty[object] == base.ty
                && matches!(form, Some(Form::Type { ty, .. }) if ty == expr)
        });
        if let Some(object) = declared {
            let object = &self.package.objects[object];
            let name = self.names.resolve(&object.name);
            let message = format!("invalid recursive type: {name} refers to itself");
            self.error(Code::InvalidRecursiveTypeSelf, object.span, message);
            return;
        }
        let message = format!("{} is not a type", self.expr_string(cx, expr));
        self.error(Code::NotAType, sel.span, message);
    }

    /// The type that the expression `expr` writes. A generic type is
    /// written with its type arguments.
    fn typ(&mut self, cx: &Cx<'a>, expr: ExprId) -> TypeId {
        let ty = self.generic_or_type(cx, expr);
        self.pure_type(cx, expr, ty);
        if !self.is_generic_type(ty) {
            return ty;
        }
        let message = format!(
            "cannot use generic type {} without instantiation",
            self.generic_string(ty)
        );
        let span = cx.tree.exprs[expr].span;
        self.error(Code::GenericWithoutInstantiation, span, message);
        self.incomplete = true;
        Types::INVALID
    }

    /// The type that `expr` writes for a variable, a parameter, a field or
    /// an element, which is no constraint.
    pub(super) fn var_type(&mut self, cx: &Cx<'a>, expr: ExprId) -> TypeId {
        let ty = self.typ(cx, expr);
        if self.types.is_interface(ty) || matches!(&*self.types.get(ty), Type::Marker(_)) {
            self.var_types.push((ty, cx.tree.exprs[expr].span));
        }
        // A value is of no abstract type, and a type parameter that a
        // declaration holds by value takes none.
        if !matches!(&*self.types.get(ty), Type::Basic(_)) {
            self.value_types
                .push((ty, cx.tree.exprs[expr].span, self.memo.is_some()));
        }
        ty
    }

    /// The type that `expr` writes where no value of it is held by what
    /// is written around it: what a pointer points to, and a type
    /// argument, which is held as the generic type says.
    pub(super) fn pointee_type(&mut self, cx: &Cx<'a>, expr: ExprId) -> TypeId {
        let mark = self.value_types.len();
        let ty = self.var_type(cx, expr);
        let span = cx.tree.exprs[expr].span;
        if self.value_types.last().is_some_and(|last| last.1 == span)
            && mark < self.value_types.len()
        {
            self.value_types.pop();
        }
        ty
    }

    /// The operand `x` of `expr` where a generic type needs its type
    /// arguments: it is no operand without them.
    fn instantiated(&mut self, cx: &Cx<'a>, expr: ExprId, x: Operand) -> Operand {
        if x.mode != Mode::Type || !self.is_generic_type(x.ty) {
            return x;
        }
        let message = format!(
            "cannot use generic type {} without instantiation",
            self.expr_string(cx, expr)
        );
        let span = cx.tree.exprs[expr].span;
        self.error(Code::GenericWithoutInstantiation, span, message);
        Operand::UNKNOWN
    }

    /// `typ`, where a generic type may stand without its type arguments.
    fn generic_or_type(&mut self, cx: &Cx<'a>, expr: ExprId) -> TypeId {
        gopp_syntax::with_stack(|| self.typ_node(cx, expr))
    }

    /// Whether the type is a generic type without its type arguments.
    fn is_generic_type(&self, ty: TypeId) -> bool {
        matches!(&*self.types.get(ty), Type::Named { id, args }
            if args.is_empty() && !self.types.type_params(*id).is_empty())
    }

    fn typ_node(&mut self, cx: &Cx<'a>, expr: ExprId) -> TypeId {
        let tree = cx.tree;
        let node = &tree.exprs[expr];
        match &node.kind {
            ExprKind::Name(_) | ExprKind::Selector { .. } => {
                if let Some(ty) = self.foreign_type(cx, expr, expr, &[]) {
                    self.incomplete |= ty == Types::INVALID;
                    return ty;
                }
                if let Some(ty) = self.named_type(cx, expr) {
                    return ty;
                }
                // A name that denotes something else; an import that could
                // not be loaded denotes nothing that is known.
                if matches!(node.kind, ExprKind::Name(_)) && cx.body.uses.contains_key(&expr) {
                    let text = &cx.text[node.span.lo as usize..node.span.hi as usize];
                    // A signature is read once for its declaration and
                    // once for its body.
                    let message = format!("{text} is not a type");
                    self.error(Code::NotAType, node.span, message);
                }
                if let ExprKind::Selector { x, sel } = node.kind {
                    self.selector_is_no_type(cx, expr, x, sel);
                }
                self.incomplete = true;
                Types::INVALID
            }
            ExprKind::Paren(x) => self.typ(cx, *x),
            ExprKind::Unary {
                op: UnaryOp::Star,
                x,
            } => {
                let elem = self.pointee_type(cx, *x);
                self.types.pointer(elem)
            }
            ExprKind::SliceType { elem } => {
                let elem = self.var_type(cx, *elem);
                self.types.slice(elem)
            }
            ExprKind::ArrayType {
                len: Some(len),
                elem,
            } => {
                let elem = self.var_type(cx, *elem);
                match self.array_length(cx, *len) {
                    Some(len) => self.types.array(len, elem),
                    None => {
                        self.incomplete = true;
                        Types::INVALID
                    }
                }
            }
            // `[...]T` takes its length from the elements of a literal.
            ExprKind::ArrayType { len: None, elem } => {
                self.var_type(cx, *elem);
                let text = &cx.text[node.span.lo as usize..node.span.hi as usize];
                let lo = node.span.lo + text.find("...").unwrap_or(0) as u32;
                let dots = Span {
                    lo,
                    hi: lo + 3,
                    ..node.span
                };
                let message = "invalid use of [...] array (outside a composite literal)";
                self.error(Code::InvalidUseOfDots, dots, message.to_owned());
                self.incomplete = true;
                Types::INVALID
            }
            ExprKind::MapType { key, value } => {
                let key_span = tree.exprs[*key].span;
                let (key, value) = (self.var_type(cx, *key), self.var_type(cx, *value));
                self.map_keys.push((key, key_span));
                self.types.map(key, value)
            }
            ExprKind::ChanType { dir, elem } => {
                let elem = self.var_type(cx, *elem);
                self.sent.push((elem, cx.tree.exprs[expr].span));
                let dir = match dir {
                    ast::ChanDir::Both => ChanDir::Both,
                    ast::ChanDir::Send => ChanDir::Send,
                    ast::ChanDir::Recv => ChanDir::Recv,
                };
                self.types.chan(dir, elem)
            }
            ExprKind::FuncType {
                params,
                results,
                fails,
            } => {
                let variadic = params.last().is_some_and(|last| {
                    matches!(tree.exprs[last.ty].kind, ExprKind::DotsType { .. })
                });
                let mut list = |params: &[Param]| -> Vec<TypeId> {
                    params
                        .iter()
                        .map(|param| self.var_type(cx, param.ty))
                        .collect()
                };
                let (params, types) = (list(params), list(results));
                if !*fails {
                    return self.types.func(&params, &types, variadic);
                }
                if let Some(last) = results.last()
                    && types.last() == Some(&self.types.error())
                {
                    let message = "! already adds the error; remove it".to_owned();
                    let span = tree.exprs[last.ty].span;
                    self.error(Code::FailsReturnsError, span, message);
                }
                self.types.failing_func(&params, &types, variadic)
            }
            // `...T` as the type of the last parameter: a slice.
            ExprKind::DotsType { elem } => {
                let elem = self.var_type(cx, *elem);
                self.types.slice(elem)
            }
            ExprKind::StructType { fields } => {
                let mut all = Vec::new();
                let mut declared = FxHashMap::default();
                for field in fields {
                    let ty = self.var_type(cx, field.ty);
                    let literal = field.tag.map(|tag| tree.exprs[tag].span);
                    let tag = literal
                        .and_then(|span| unquote(&cx.text[span.lo as usize..span.hi as usize]));
                    self.field_attributes(cx, &field.attrs);
                    let marked = field.attrs.iter().find(|attr| {
                        let name = attr.path.name.span;
                        &cx.text[name.lo as usize..name.hi as usize] == "Required"
                    });
                    self.required_fields
                        .extend(marked.map(|attr| (ty, attr.span)));
                    let required = field.attrs.iter().any(|attr| {
                        let name = attr.path.name.span;
                        &cx.text[name.lo as usize..name.hi as usize] == "Required"
                    });
                    let embedded = field.names.is_empty();
                    let names: Vec<(Spur, Span)> = match embedded {
                        true => self.embedded(tree, field.ty).into_iter().collect(),
                        false => field
                            .names
                            .iter()
                            .map(|name| (name.name, name.span))
                            .collect(),
                    };
                    for (name, span) in names {
                        self.field_once(&mut declared, name, span);
                        all.push(Field {
                            name,
                            package: self.package_of(name),
                            ty,
                            embedded,
                            tag: tag.clone().map(Into::into),
                            required,
                        });
                    }
                }
                self.types.strukt(all)
            }
            ExprKind::InterfaceType { elems } => {
                let (mut methods, mut embedded) = (Vec::new(), Vec::new());
                let mut written = Vec::new();
                for elem in elems {
                    match elem {
                        InterfaceElem::Method { name, ty } => {
                            let method = Method {
                                name: name.name,
                                package: self.package_of(name.name),
                                sig: self.typ(cx, *ty),
                            };
                            written.push((Ok(method.clone()), name.span));
                            methods.push(method);
                        }
                        InterfaceElem::Type(ty) => {
                            let inner = self.typ(cx, *ty);
                            let inner = self.no_param_term(cx, *ty, inner, None);
                            written.push((Err(inner), tree.exprs[*ty].span));
                            embedded.push(inner);
                        }
                    }
                }
                if written.len() > 1 {
                    self.interfaces.push(written);
                }
                self.types.interface(methods, embedded, self.names)
            }
            // The type terms of a constraint.
            ExprKind::Unary {
                op: UnaryOp::Tilde, ..
            }
            | ExprKind::Binary {
                op: BinaryOp::Or, ..
            } => {
                let mut terms = Vec::new();
                self.terms(cx, expr, &mut terms);
                let union: Vec<Term> = terms.iter().map(|&(term, _)| term).collect();
                self.unions.push(terms);
                self.types.intern(Type::Union(union.into()))
            }
            ExprKind::Index { x, indices } => match self.foreign_type(cx, expr, *x, indices) {
                Some(ty) => {
                    self.incomplete |= ty == Types::INVALID;
                    ty
                }
                None => self.instance(cx, expr, *x, indices),
            },
            ExprKind::Bad | ExprKind::MacroCall { .. } => {
                self.incomplete = true;
                Types::INVALID
            }
            _ => {
                let message = format!("{} is not a type", self.expr_string(cx, expr));
                self.error(Code::NotAType, node.span, message);
                self.operand(cx, expr);
                self.incomplete = true;
                Types::INVALID
            }
        }
    }

    /// The name of an embedded field, which is that of its type, and
    /// where it is written.
    pub(super) fn embedded(&self, tree: &File, mut ty: ExprId) -> Option<(Spur, Span)> {
        loop {
            match &tree.exprs[ty].kind {
                ExprKind::Name(name) => return Some((*name, tree.exprs[ty].span)),
                ExprKind::Selector { sel, .. } => return Some((sel.name, sel.span)),
                ExprKind::Paren(x)
                | ExprKind::Index { x, .. }
                | ExprKind::Unary {
                    op: UnaryOp::Star,
                    x,
                } => ty = *x,
                _ => return None,
            }
        }
    }

    /// `x[indices]` in a type position: a generic type with its arguments.
    fn instance(&mut self, cx: &Cx<'a>, expr: ExprId, x: ExprId, indices: &[ExprId]) -> TypeId {
        let make: Option<fn(TypeId) -> Type> = match cx.body.uses.get(&x) {
            Some(Ref::Universe(Predeclared::Unique)) => Some(Type::Unique),
            Some(Ref::Universe(Predeclared::Shared)) => Some(Type::Shared),
            Some(Ref::Universe(Predeclared::Weak)) => Some(Type::Weak),
            Some(Ref::Universe(Predeclared::Local)) => Some(Type::Local),
            Some(Ref::Universe(Predeclared::Option)) => Some(Type::Option),
            _ => None,
        };
        // A type argument is held as the generic type holds its type
        // parameter, which the instance is asked.
        let holds = matches!(
            cx.body.uses.get(&x),
            Some(Ref::Universe(Predeclared::Option))
        );
        let arguments = |this: &mut Self| -> Vec<TypeId> {
            indices
                .iter()
                .map(|&index| match holds {
                    true => this.var_type(cx, index),
                    false => this.pointee_type(cx, index),
                })
                .collect()
        };
        if let Some(make) = make {
            let made = match arguments(self)[..] {
                [arg] => match make(arg) {
                    Type::Option(_) => {
                        let at = cx.tree.exprs[expr].span;
                        self.options.push((arg, at, self.memo.is_some()));
                        Some(self.types.option(arg))
                    }
                    _ => match self.types.handle(make, arg) {
                        Ok(made) => {
                            if !matches!(make(arg), Type::Local(_)) {
                                let at = cx.tree.exprs[expr].span;
                                let written = self.expr_string(cx, expr);
                                self.heaped.push((arg, at, written));
                            }
                            Some(made)
                        }
                        Err(bad) => {
                            self.bad_handle(cx, expr, arg, bad);
                            None
                        }
                    },
                },
                _ => None,
            };
            return made.unwrap_or_else(|| {
                self.incomplete = true;
                Types::INVALID
            });
        }
        // The arguments of what is no generic type are not looked at.
        let generic = self.generic_or_type(cx, x);
        if let Type::Named { id, .. } = &*self.types.get(generic)
            && self.is_generic_type(generic)
        {
            let args = arguments(self);
            let params = self.types.type_params(*id);
            let at = cx.tree.exprs[expr].span;
            if args.len() != params.len() {
                let message = format!(
                    "{} type arguments for type {}: have {}, want {}",
                    if args.len() < params.len() {
                        "not enough"
                    } else {
                        "too many"
                    },
                    self.names.resolve(&self.types.name_of(*id)),
                    args.len(),
                    params.len()
                );
                self.error(Code::GotNTypeArgs, at, message);
                self.incomplete = true;
                return Types::INVALID;
            }
            // An alias with type arguments is what it stands for.
            let instance = match self.types.alias(*id, &args) {
                Some(alias) => alias,
                None => {
                    let instance = self.types.named(*id, &args);
                    self.written(instance, cx.tree.exprs[x].span);
                    self.note_instance(expr, &args, instance);
                    instance
                }
            };
            let spans = indices.iter().map(|&index| cx.tree.exprs[index].span);
            self.instances.push(constraints::Instance {
                params,
                args,
                spans: spans.collect(),
                at,
            });
            return instance;
        }
        if generic != Types::INVALID {
            let message = format!(
                "invalid operation: {} ({} is not a generic type)",
                self.expr_string(cx, expr),
                self.type_string(generic)
            );
            self.error(Code::NotAGenericType, cx.tree.exprs[expr].span, message);
        }
        self.incomplete = true;
        Types::INVALID
    }

    /// A generic type as a message shows it without type arguments:
    /// `T[P any]`.
    fn generic_string(&self, ty: TypeId) -> String {
        let Type::Named { id, .. } = &*self.types.get(ty) else {
            return self.type_string(ty);
        };
        let params = self.type_params_string(&self.types.type_params(*id));
        format!("{}[{params}]", self.type_string(ty))
    }

    /// `P, Q any`: type parameters as they are declared; those with one
    /// constraint are written together.
    fn type_params_string(&self, params: &[TypeId]) -> String {
        let mut list = String::new();
        for (at, &param) in params.iter().enumerate() {
            let constraint = self.types.constraint(param);
            let next = params.get(at + 1).map(|&next| self.types.constraint(next));
            list += &self.type_string(param);
            match (next == Some(constraint), constraint) {
                (true, _) => list += ", ",
                (false, Some(constraint)) => {
                    list += " ";
                    list += &self.type_string(constraint);
                    if at + 1 < params.len() {
                        list += ", ";
                    }
                }
                (false, None) => {}
            }
        }
        list
    }

    /// A generic function type as a message shows it:
    /// `func[P any](P) []P`.
    pub(super) fn generic_func_string(&self, ty: TypeId) -> String {
        let shown = self.type_string(ty);
        match self.types.generic_func(ty) {
            Some(params) => format!(
                "func[{}]{}",
                self.type_params_string(&params),
                shown.strip_prefix("func").unwrap_or(&shown)
            ),
            None => shown,
        }
    }

    // ---- expressions

    fn basic_of(&self, ty: TypeId) -> Option<Basic> {
        match &*self.types.get(self.types.underlying(ty)) {
            Type::Basic(basic) => Some(*basic),
            _ => None,
        }
    }

    fn untyped(&self, basic: Basic, value: Value) -> Operand {
        Operand::constant(self.types.basic(basic), value)
    }

    fn value_of(&self, basic: Basic) -> Operand {
        Operand::value(self.types.basic(basic))
    }

    /// `value` as a constant of the type `ty` holds it; `None` if the type
    /// cannot hold it.
    fn converted(&self, value: &Value, ty: TypeId) -> Option<Value> {
        representable(value, self.basic_of(ty)?)
    }

    /// The length that `expr` gives an array: a constant that is an
    /// integer, not negative, and that an `int` holds.
    fn array_length(&mut self, cx: &Cx<'a>, expr: ExprId) -> Option<u64> {
        // What a name that is no constant is was said where the name was
        // found.
        let x = match cx.tree.exprs[expr].kind {
            ExprKind::Name(_) => {
                let x = self.operand(cx, expr);
                match x.mode {
                    Mode::Const(_) => x,
                    _ => return None,
                }
            }
            // The length of an array is a constant expression (EXP-32).
            _ => {
                let outer = std::mem::replace(&mut self.in_const, true);
                let x = self.value(cx, expr);
                self.in_const = outer;
                x
            }
        };
        let what = match &x.mode {
            Mode::Unknown => return None,
            // What a `@Const` function gives is known where it is run.
            Mode::Const(Value::Unknown) => return None,
            Mode::Const(value) => {
                let integer = self.is_integer(x.ty);
                if integer || self.is_untyped(x.ty) {
                    let value = value.to_int();
                    let fits = representable(&value, Basic::Int).is_some();
                    let length = value.as_i64().map(u64::try_from);
                    if let (true, Some(Ok(length))) = (fits, length) {
                        return Some(length);
                    }
                }
                match integer {
                    true => format!("invalid array length {}", self.describe(cx, expr, &x)),
                    false => format!(
                        "array length {} must be integer",
                        self.describe(cx, expr, &x)
                    ),
                }
            }
            _ => {
                let message = format!(
                    "array length {} must be constant",
                    self.describe(cx, expr, &x)
                );
                self.error(
                    Code::ArrayLengthNotConstant,
                    cx.tree.exprs[expr].span,
                    message,
                );
                return None;
            }
        };
        self.error(Code::InvalidArrayLength, cx.tree.exprs[expr].span, what);
        None
    }

    /// The value of a constant expression; `None` for what is not constant
    /// or not worked out here yet.
    fn constant(&mut self, cx: &Cx<'a>, expr: ExprId) -> Option<(TypeId, Value)> {
        match self.operand(cx, expr) {
            Operand {
                mode: Mode::Const(value),
                ty,
            } => Some((ty, value)),
            _ => None,
        }
    }

    /// What the expression `expr` is: a type, a constant, a value of some
    /// type. Unknown for what is not worked out here yet.
    fn operand(&mut self, cx: &Cx<'a>, expr: ExprId) -> Operand {
        let known = self.memo.as_ref().and_then(|memo| memo.get(&expr));
        if let Some(known) = known.filter(|_| !self.fresh) {
            return known.clone();
        }
        let operand = gopp_syntax::with_stack(|| self.operand_node(cx, expr));
        if let Some(memo) = &mut self.memo {
            memo.insert(expr, operand.clone());
        }
        operand
    }

    fn operand_node(&mut self, cx: &Cx<'a>, expr: ExprId) -> Operand {
        let tree = cx.tree;
        let node = &tree.exprs[expr];
        match &node.kind {
            ExprKind::BasicLit { kind, bad: false } => {
                let text = &cx.text[node.span.lo as usize..node.span.hi as usize];
                let basic = match kind {
                    LitKind::Int => Basic::UntypedInt,
                    LitKind::Float => Basic::UntypedFloat,
                    LitKind::Imag => Basic::UntypedComplex,
                    LitKind::Rune => Basic::UntypedRune,
                    LitKind::String => Basic::UntypedString,
                };
                let value = Value::from_literal(text, *kind);
                if value == Value::Unknown {
                    let message = format!("malformed constant: {text}");
                    self.error(Code::MalformedConstant, node.span, message);
                    return Operand::UNKNOWN;
                }
                self.untyped(basic, value)
            }
            ExprKind::Paren(x) => {
                let inner = self.operand(cx, *x);
                self.instantiated(cx, *x, inner)
            }
            ExprKind::Name(_) => self.name(cx, expr),
            ExprKind::Selector { x, sel } => self.selector(cx, expr, *x, *sel),
            ExprKind::Unary { op, x } => self.unary(cx, expr, *op, *x),
            ExprKind::Binary { op, x, y } => self.binary(cx, Some(expr), *op, *x, *y),
            ExprKind::Call {
                fun,
                args,
                spread,
                unwrap,
            } => {
                let called = self.call(cx, expr, *fun, args, *spread);
                match unwrap {
                    true => self.unwrapped(cx, expr, *fun, called),
                    false => called,
                }
            }
            ExprKind::FuncLit { ty, body } => {
                // What a function literal uses, it may lend.
                self.lends = true;
                let sig = self.typ(cx, *ty);
                // Its body is walked with the body it is in.
                if self.memo.is_some() {
                    self.signature_locals(cx, *ty);
                    self.literals.push((expr, cx.tree.exprs[expr].span));
                    let target = self.target.take();
                    self.function_body(cx, *ty, sig, body);
                    self.target = target;
                    self.literals.pop();
                }
                Operand::value(sig)
            }
            ExprKind::CompositeLit { ty, elems } => self.composite(cx, expr, *ty, elems, None),
            ExprKind::Index { x, indices } => self.index(cx, expr, *x, indices),
            ExprKind::Slice {
                x,
                lo,
                hi,
                max,
                full,
            } => self.slice(cx, expr, *x, [*lo, *hi, *max], *full),
            ExprKind::Assert { x, ty } => {
                let asserted = self.asserted(cx, *x, "invalid operation: ", "type assertion");
                let Some(from) = asserted else {
                    return Operand::UNKNOWN;
                };
                match self.var_type(cx, *ty) {
                    invalid if invalid == Types::INVALID => Operand::UNKNOWN,
                    ty => {
                        self.assertable(cx, expr, (*x, &from), ty, false);
                        Operand {
                            mode: Mode::CommaOk,
                            ty,
                        }
                    }
                }
            }
            // A type written out: `[]int`, `map[string]T`, `func()`, ...
            ExprKind::ArrayType { .. }
            | ExprKind::SliceType { .. }
            | ExprKind::MapType { .. }
            | ExprKind::ChanType { .. }
            | ExprKind::FuncType { .. }
            | ExprKind::StructType { .. }
            | ExprKind::InterfaceType { .. } => Operand {
                mode: Mode::Type,
                ty: self.typ(cx, expr),
            },
            // `x.(type)` belongs to a type switch, which reads it itself.
            ExprKind::TypeSwitchGuard { x, .. } => {
                if self.value(cx, *x).mode != Mode::Unknown {
                    let message = "use of .(type) outside type switch".to_owned();
                    self.error(Code::TypeSwitchOutside, node.span, message);
                }
                Operand::UNKNOWN
            }
            _ => Operand::UNKNOWN,
        }
    }

    /// What a type assertion or a type switch looks into, which is a
    /// value of an interface type: `None` if it is known to be none.
    fn asserted(&mut self, cx: &Cx<'a>, expr: ExprId, prefix: &str, what: &str) -> Option<Operand> {
        let x = self.value(cx, expr);
        if x.mode == Mode::Unknown || self.types.underlying(x.ty) == Types::INVALID {
            return Some(Operand::UNKNOWN);
        }
        let shown = self.describe(cx, expr, &x);
        let (code, message) = if matches!(&*self.types.get(x.ty), Type::Param(_)) {
            let message = format!("{prefix}cannot use {what} on type parameter value {shown}");
            (Code::AssertedTypeParam, message)
        } else if !self.types.is_interface(x.ty) {
            // A pointer to a type with a type table says what it points
            // to (EXP-15).
            match self.pointee_with_table(&x) {
                Some(true) => return Some(x),
                Some(false) => {
                    let message = format!(
                        "{prefix}{shown} is asserted, and the type it points to has no type \
                         table: nothing says what it points to"
                    );
                    (Code::DowncastNoTable, message)
                }
                None => {
                    let message = format!("{prefix}{shown} is not an interface");
                    (Code::NotAnInterface, message)
                }
            }
        } else {
            return Some(x);
        };
        self.error(code, cx.tree.exprs[expr].span, message);
        None
    }

    fn name(&mut self, cx: &Cx<'a>, expr: ExprId) -> Operand {
        // What a key of a composite literal denotes was found apart.
        let denotes = cx.body.uses.get(&expr).or_else(|| cx.body.keys.get(&expr));
        let Some(&denotes) = denotes else {
            self.base_alone(cx, expr);
            return Operand::UNKNOWN;
        };
        match denotes {
            Ref::Universe(Predeclared::True) => self.untyped(Basic::UntypedBool, Value::Bool(true)),
            Ref::Universe(Predeclared::False) => {
                self.untyped(Basic::UntypedBool, Value::Bool(false))
            }
            Ref::Universe(Predeclared::Iota) => match cx.iota {
                Some(iota) => self.untyped(Basic::UntypedInt, Value::Int64(iota)),
                None => {
                    let message = "cannot use iota outside constant declaration".to_owned();
                    self.error(Code::IotaOutsideConst, cx.tree.exprs[expr].span, message);
                    Operand::UNKNOWN
                }
            },
            Ref::Universe(Predeclared::Nil) => Operand {
                mode: Mode::Value,
                ty: self.types.basic(Basic::UntypedNil),
            },
            Ref::Universe(name) => match name.kind() {
                PredeclaredKind::Func => Operand {
                    mode: Mode::Builtin(name),
                    ty: Types::INVALID,
                },
                _ => match self.named_type(cx, expr) {
                    Some(ty) => Operand {
                        mode: Mode::Type,
                        ty,
                    },
                    None => Operand::UNKNOWN,
                },
            },
            Ref::Object(object) => {
                self.need(object.0 as usize);
                let ty = self.object_type(object, false);
                let kind = &self.package.object(object).kind;
                if matches!(kind, ObjKind::Var) {
                    let written = self.expr_string(cx, expr);
                    self.impure(self.span(cx, expr), &format!("uses the variable {written}"));
                }
                self.object_operand(kind, ty, self.value[object.0 as usize].clone())
            }
            // A local of the function literal that the expression is in.
            Ref::Local(local) => match cx.params.get(&local) {
                Some(&ty) => Operand {
                    mode: Mode::Type,
                    ty,
                },
                None => {
                    self.read_variable(cx, expr);
                    let x = self.locals.get(&local).cloned().unwrap_or(Operand::UNKNOWN);
                    match x.mode {
                        Mode::Variable => self.flow_read(cx, expr, local, x),
                        _ => x,
                    }
                }
            },
            Ref::Member { .. } => Operand::UNKNOWN,
        }
    }

    /// An object of the kind `kind` as an operand.
    fn object_operand(&self, kind: &ObjKind, ty: TypeId, value: Value) -> Operand {
        if ty == Types::INVALID {
            return Operand::UNKNOWN;
        }
        match kind {
            ObjKind::Const => Operand::constant(ty, value),
            ObjKind::Type => Operand {
                mode: Mode::Type,
                ty,
            },
            ObjKind::Var => Operand::variable(ty),
            ObjKind::Func => Operand::value(ty),
            _ => Operand::UNKNOWN,
        }
    }

    /// `x.sel`: a name of an imported package, a field, a method.
    fn selector(&mut self, cx: &Cx<'a>, expr: ExprId, x: ExprId, sel: Ident) -> Operand {
        if let Some(found) = self.foreign_selector(cx, expr) {
            return found;
        }
        if let Some(&Ref::Member { import, member }) = cx.body.uses.get(&expr) {
            let ObjKind::Import { package, .. } = &self.package.object(import).kind else {
                return Operand::UNKNOWN;
            };
            let object = package.object(member);
            // Package unsafe is built into the compiler: its functions take
            // any type.
            if package.path == "unsafe" {
                self.impure(self.span(cx, expr), "uses package unsafe");
            }
            if package.path == "unsafe" && matches!(object.kind, ObjKind::Func) {
                return Operand {
                    mode: Mode::Unsafe(object.name),
                    ty: Types::INVALID,
                };
            }
            if matches!(object.kind, ObjKind::Var) {
                let written = self.expr_string(cx, expr);
                self.impure(self.span(cx, expr), &format!("uses the variable {written}"));
            }
            return self.object_operand(&object.kind, object.ty, object.value.clone());
        }
        if let Some(found) = self.base_member(cx, expr, x, sel) {
            return found;
        }
        let base = x;
        // `o.IsNil()` and `o.Get()` ask the option, whatever is known.
        let asks = matches!(self.names.resolve(&sel.name), "IsNil" | "Get");
        let outer = match asks {
            true => self.unnarrowed.replace(self.unparen(cx, base)),
            false => self.unnarrowed,
        };
        let x = self.operand(cx, base);
        self.unnarrowed = outer;
        let x = self.instantiated(cx, base, x);
        if x.mode == Mode::Unknown {
            return Operand::UNKNOWN;
        }
        self.read_alone(cx, base, &x);
        if matches!(x.mode, Mode::Builtin(_)) {
            let message = format!(
                "invalid use of {} in selector expression",
                self.describe(cx, base, &x)
            );
            self.error(Code::BuiltinNotCalled, sel.span, message);
            return Operand::UNKNOWN;
        }
        if x.mode == Mode::NoValue {
            return self.no_value(cx, base, &x);
        }
        // In a `.gopp` file a field or a method is reached through every
        // kind of pointer.
        if x.is_value()
            && let Some(method) = self.handle_method(&x, sel.name)
        {
            self.whole_copied(&x, sel);
            return method;
        }
        let recv = x.ty;
        let x = self.read(x);
        let found = self
            .types
            .lookup(x.ty, sel.name, self.path, self.names, false);
        let folded = match (&x.mode, &found) {
            (Mode::Const(held), Ok(found)) if !found.indirect => {
                Some(self.constant_field(held, &found.path))
            }
            _ => None,
        };
        let found = match found {
            Ok(found) => {
                let kind = match (&x.mode, &found.member) {
                    (Mode::Type, Member::Method { .. }) => Some(SelectionKind::MethodExpr),
                    (Mode::Type, Member::Field { .. }) => None,
                    (_, Member::Field { .. }) => Some(SelectionKind::Field),
                    (_, Member::Method { .. }) => Some(SelectionKind::Method),
                };
                // The method that ends a value runs by itself (OWN-R7).
                let method = matches!(found.member, Member::Method { .. });
                if method && self.names.resolve(&sel.name) == "drop" {
                    let message = format!(
                        "cannot use {}: drop runs when a value ends, and is not called",
                        self.expr_string(cx, expr)
                    );
                    self.error(Code::OwnDropCalled, sel.span, message);
                    return Operand::UNKNOWN;
                }
                self.need_method(&found);
                if let (Some(kind), true) = (kind, self.memo.is_some()) {
                    // Which method of its type an instance reaches, the
                    // type argument decides.
                    if method && self.is_param(found.holder) {
                        self.param_methods.insert(expr, sel.name);
                    }
                    let selection = Selection {
                        kind,
                        recv,
                        holder: found.holder,
                        path: found.path,
                        indirect: found.indirect || x.ty != recv,
                    };
                    self.selections.insert(expr, selection);
                }
                Some(found.member)
            }
            Err(why) => {
                self.no_member(cx, base, &x, sel, why);
                None
            }
        };
        match (&x.mode, found) {
            // A field of a variable, or one behind a pointer, is a
            // variable.
            // A field of a constant is a constant (EXP-31).
            (Mode::Const(_), Some(Member::Field { ty, .. })) if folded.is_some() => {
                Operand::constant(ty, folded.unwrap_or(Value::Unknown))
            }
            (mode, Some(Member::Field { ty, indirect })) if x.is_value() => {
                match *mode == Mode::Variable || indirect {
                    true => Operand::variable(ty),
                    false => Operand::value(ty),
                }
            }
            (
                _,
                Some(Member::Method {
                    sig,
                    pointer_receiver,
                }),
            ) if x.is_value() => {
                self.method_selected(cx, (expr, sel.name), base, &x, pointer_receiver);
                Operand::value(sig)
            }
            // `T.m`: a function that takes the receiver first.
            (
                Mode::Type,
                Some(Member::Method {
                    sig,
                    pointer_receiver,
                }),
            ) => {
                let through_pointer = matches!(&*self.types.get(x.ty), Type::Pointer(_));
                if pointer_receiver && !through_pointer && !self.types.is_interface(x.ty) {
                    let text = self.expr_string(cx, expr);
                    let message = format!(
                        "invalid method expression {text} (needs pointer receiver (*{}).{})",
                        self.type_string(x.ty),
                        self.names.resolve(&sel.name)
                    );
                    self.error(Code::MethodExprPointerReceiver, sel.span, message);
                    return Operand::UNKNOWN;
                }
                let Type::Func {
                    params,
                    results,
                    variadic,
                    ..
                } = &*self.types.get(sig)
                else {
                    return Operand::UNKNOWN;
                };
                let params: Vec<TypeId> = std::iter::once(x.ty)
                    .chain(params.iter().copied())
                    .collect();
                Operand::value(self.types.func(&params, results, *variadic))
            }
            (Mode::Type, Some(Member::Field { .. })) => {
                let name = self.names.resolve(&sel.name);
                let message = format!(
                    "{}.{name} undefined (type {} has no method {name})",
                    self.expr_string(cx, base),
                    self.type_string(x.ty)
                );
                self.error(Code::UndefinedFieldOrMethod, sel.span, message);
                Operand::UNKNOWN
            }
            _ => Operand::UNKNOWN,
        }
    }

    /// `x.sel` reaches nothing in the type of `x`: says why, unless the
    /// type is in error, is written with a type parameter, or has members
    /// that the rules of a `.gopp` file give it.
    fn no_member(
        &mut self,
        cx: &Cx<'a>,
        base: ExprId,
        x: &Operand,
        sel: Ident,
        why: crate::types::NotFound,
    ) {
        let known = x.is_value() || x.mode == Mode::Type;
        let invalid = |ty: TypeId| self.types.underlying(ty) == Types::INVALID;
        let pointee_invalid = matches!(
            &*self.types.get(self.types.underlying(x.ty)),
            Type::Pointer(elem) if invalid(*elem)
        );
        let unsettled = invalid(x.ty) || pointee_invalid || self.open(x.ty);
        if !known || unsettled {
            return;
        }
        let (of, name) = (self.expr_string(cx, base), self.names.resolve(&sel.name));
        if why == crate::types::NotFound::Ambiguous {
            let message = format!("ambiguous selector {of}.{name}");
            return self.error(Code::AmbiguousSelector, sel.span, message);
        }
        let shown = self.type_string(x.ty);
        let pointee = match &*self.types.get(self.types.underlying(x.ty)) {
            Type::Pointer(elem) => Some(*elem),
            _ => None,
        };
        let pointer_to_param = pointee.is_some_and(|elem| self.is_param(elem));
        let pointer_to_interface = pointee.is_some_and(|elem| self.types.is_interface(elem));
        // A member whose name differs in its capitals only is the likely
        // one.
        let near = self
            .types
            .lookup(x.ty, sel.name, self.path, self.names, true);
        let reason = match near {
            _ if pointer_to_param => {
                format!("type {shown} is pointer to type parameter, not type parameter")
            }
            _ if pointer_to_interface => {
                format!("type {shown} is pointer to interface, not interface")
            }
            Err(_) => format!("type {shown} has no field or method {name}"),
            Ok(near) => {
                let what = match near.member {
                    Member::Field { .. } => "field",
                    Member::Method { .. } => "method",
                };
                let alt = self.names.resolve(&near.name);
                let exported = |name: &str| name.starts_with(char::is_uppercase);
                let same_tail = name.chars().skip(1).eq(alt.chars().skip(1));
                // A member of a type of this package is named as it is;
                // of another package, only what is exported can be meant.
                let here = near.owner.is_none_or(|owner| owner == self.name);
                if here && name != alt || exported(name) && exported(alt) {
                    format!(
                        "type {shown} has no field or method {name}, but does have {what} {alt}"
                    )
                } else if exported(name) && same_tail {
                    format!(
                        "type {shown} has no field or method {name}, but does have unexported \
                         {what} {alt}"
                    )
                } else if exported(alt) && same_tail {
                    format!(
                        "type {shown} has no field or method {name}, but does have {what} {alt}"
                    )
                } else if name == alt {
                    format!("cannot refer to unexported {what} {alt}")
                } else {
                    format!("type {shown} has no field or method {name}")
                }
            }
        };
        let message = format!("{of}.{name} undefined ({reason})");
        // What a `Weak` points to may be gone (ADR-0017), and an option
        // may be nil (OWN-P10).
        let fix = match &*self.types.get(self.types.underlying(x.ty)) {
            Type::Weak(_) => Some(format!(
                "use {of}.Lock(), which gives an Option of a Shared"
            )),
            Type::Option(_) => Some(format!(
                "{of} may be nil: put it in a variable and test that against nil first"
            )),
            _ => None,
        };
        match fix {
            Some(fix) => {
                let fix = (Code::NoteOwnFixOption, fix);
                self.error_with_fix(Code::UndefinedFieldOrMethod, sel.span, message, fix);
            }
            None => self.error(Code::UndefinedFieldOrMethod, sel.span, message),
        }
    }

    /// The type of the composite literal `ty{elems}`. `[...]T{…}` has as
    /// many elements as the literal says.
    fn literal_type(&mut self, cx: &Cx<'a>, ty: ExprId, elems: &[ExprId]) -> TypeId {
        let ExprKind::ArrayType { len: None, elem } = cx.tree.exprs[ty].kind else {
            return self.typ(cx, ty);
        };
        let elem = self.var_type(cx, elem);
        // An element with a key sits at that index; one without, after
        // the element before it.
        let (mut next, mut len) = (0u64, 0u64);
        for &element in elems {
            if let ExprKind::KeyValue { key, .. } = cx.tree.exprs[element].kind {
                let index = self.constant(cx, key).map(|(_, value)| value.to_int());
                match index.as_ref().and_then(Value::as_i64).map(u64::try_from) {
                    Some(Ok(index)) => next = index,
                    _ => {
                        self.incomplete = true;
                        return Types::INVALID;
                    }
                }
            }
            next += 1;
            len = len.max(next);
        }
        self.types.array(len, elem)
    }

    fn is_untyped(&self, ty: TypeId) -> bool {
        self.types.is_untyped(ty)
    }

    fn is_integer(&self, ty: TypeId) -> bool {
        use Basic::*;
        matches!(
            self.basic_of(ty),
            Some(
                Int | Int8
                    | Int16
                    | Int32
                    | Int64
                    | Uint
                    | Uint8
                    | Uint16
                    | Uint32
                    | Uint64
                    | Uintptr
                    | UntypedInt
                    | UntypedRune
            )
        )
    }

    /// A typed result is held as its type holds it; one that does not fit
    /// is not known.
    fn fitted(&self, ty: TypeId, value: Value) -> Operand {
        let value = match self.is_untyped(ty) {
            true => Some(value),
            false => self.converted(&value, ty),
        };
        match value {
            Some(value) if value != Value::Unknown => Operand::constant(ty, value),
            _ => Operand::UNKNOWN,
        }
    }

    /// The kind of the result for two untyped numbers: the later of
    /// integer, rune, float, complex.
    pub(super) fn larger_kind(&self, x: TypeId, y: TypeId) -> Option<TypeId> {
        let rank = |ty| match self.basic_of(ty) {
            Some(Basic::UntypedInt) => Some(0),
            Some(Basic::UntypedRune) => Some(1),
            Some(Basic::UntypedFloat) => Some(2),
            Some(Basic::UntypedComplex) => Some(3),
            _ => None,
        };
        match (rank(x), rank(y)) {
            (Some(a), Some(b)) => Some(if a >= b { x } else { y }),
            // Booleans and strings only go with their own kind.
            _ => (x == y).then_some(x),
        }
    }

    /// A call without a result stands where something is called or
    /// selected from.
    fn no_value(&mut self, cx: &Cx<'a>, expr: ExprId, x: &Operand) -> Operand {
        let message = format!("{} used as value or type", self.describe(cx, expr, x));
        self.error(Code::NoValue, cx.tree.exprs[expr].span, message);
        Operand::UNKNOWN
    }

    /// A call: of a function, a conversion, or a built-in function.
    fn call(
        &mut self,
        cx: &Cx<'a>,
        expr: ExprId,
        fun: ExprId,
        args: &[ExprId],
        spread: bool,
    ) -> Operand {
        // The calls in the arguments have operands of their own.
        let outer = std::mem::replace(&mut self.operands, place::Operands::Kept);
        let called = self.call_checked(cx, expr, fun, args, spread);
        self.operands = outer;
        called
    }

    fn call_checked(
        &mut self,
        cx: &Cx<'a>,
        expr: ExprId,
        fun: ExprId,
        args: &[ExprId],
        spread: bool,
    ) -> Operand {
        // A method that is called is no method value.
        let mut called = self.unparen(cx, fun);
        if let ExprKind::Index { x, .. } = cx.tree.exprs[called].kind {
            called = self.unparen(cx, x);
        }
        let outer = self.callee.replace(called);
        // A template of a header is the instance that the arguments
        // call.
        let callee = match self.foreign_callee(cx, fun, args) {
            Some(instance) => instance,
            None => self.operand(cx, fun),
        };
        self.callee = outer;
        self.method_values.remove(&called);
        let callee = self.operator_by_name(cx, expr, called, callee, args);
        let callee = self.instantiated(cx, fun, callee);
        // A function is called through a pointer; an option that is known
        // to be set is the function already.
        let callee = match &*self.types.get(self.types.underlying(callee.ty)) {
            Type::Pointer(_) | Type::Unique(_) | Type::Shared(_) | Type::Local(_) => {
                self.read(callee)
            }
            _ => callee,
        };
        match callee.mode {
            Mode::NoValue => {
                for &arg in args {
                    self.operand(cx, arg);
                }
                self.no_value(cx, fun, &callee)
            }
            Mode::Type => self.conversion(cx, expr, callee.ty, args),
            Mode::Builtin(_) | Mode::Unsafe(_) => {
                if let Mode::Builtin(builtin) = callee.mode {
                    self.pure_builtin(cx, fun, builtin);
                }
                let called = match callee.mode {
                    Mode::Builtin(builtin) => self.builtin(cx, expr, fun, builtin, args, spread),
                    Mode::Unsafe(name) => self.unsafe_call(cx, expr, name, args, spread),
                    _ => Operand::UNKNOWN,
                };
                self.called_or_received |= !matches!(called.mode, Mode::Const(_) | Mode::Unknown);
                called
            }
            _ if callee.is_value() => {
                self.called_or_received = true;
                self.operands = match self.reads_operands(cx, called) {
                    true => place::Operands::Read,
                    false => place::Operands::Passed,
                };
                // What is called is a function; of a type parameter, the
                // function type that all its types share.
                let shared = self.common_under(callee.ty, |this, of| match of {
                    Some((ty, under)) if !matches!(&*this.types.get(under), Type::Func { .. }) => {
                        Some(format!("{} is not a function", this.type_string(ty)))
                    }
                    _ => None,
                });
                let plain = match shared {
                    Ok(shared) => shared,
                    Err(cause) => {
                        for &arg in args {
                            self.operand(cx, arg);
                        }
                        if !self.open(callee.ty) {
                            let message = format!(
                                "invalid operation: cannot call {}: {cause}",
                                self.describe(cx, fun, &callee)
                            );
                            self.error(Code::CannotCall, cx.tree.exprs[fun].span, message);
                        }
                        return Operand::UNKNOWN;
                    }
                };
                let sig = match self.types.generic_func(callee.ty) {
                    // The type arguments that the call does not write
                    // are found from its arguments; in this call the
                    // function has the type that they give it.
                    Some(params) => {
                        let written = self.partial.get(&fun).cloned().unwrap_or_default();
                        let generic = infer::Generic {
                            sig: plain,
                            params,
                            written,
                        };
                        let sig = self.call_arguments(cx, expr, fun, plain, args, spread, generic);
                        if let Some(memo) = &mut self.memo {
                            memo.insert(fun, sig.map_or(Operand::UNKNOWN, Operand::value));
                        }
                        match sig {
                            Some(sig) => sig,
                            None => return Operand::UNKNOWN,
                        }
                    }
                    None => {
                        if let Type::Func {
                            params, variadic, ..
                        } = &*self.types.get(plain)
                        {
                            self.arguments(cx, expr, fun, (params, *variadic), args, spread);
                        }
                        plain
                    }
                };
                let Type::Func { results, .. } = &*self.types.get(sig) else {
                    return Operand::UNKNOWN;
                };
                let called = match &results[..] {
                    [] => Operand {
                        mode: Mode::NoValue,
                        ty: Types::INVALID,
                    },
                    [one] => Operand::value(*one),
                    many => Operand::value(self.types.intern(Type::Tuple(many.into()))),
                };
                self.taken(cx, (expr, args), fun, &called);
                self.foreign_arguments(cx, fun, args);
                self.pure_call(cx, fun);
                match self.in_const {
                    true => self.constant_call(cx, expr, fun, args, called),
                    false => called,
                }
            }
            _ => Operand::UNKNOWN,
        }
    }

    /// A function of package unsafe, which takes a value of any type.
    fn unsafe_call(
        &mut self,
        cx: &Cx<'a>,
        call: ExprId,
        name: Spur,
        args: &[ExprId],
        spread: bool,
    ) -> Operand {
        let text = self.names.resolve(&name);
        let count = match text {
            "Add" | "Slice" | "String" => 2,
            _ => 1,
        };
        // `Offsetof` takes a selector, which is looked at as it is written.
        let as_written = text == "Offsetof";
        let Some(given) =
            self.builtin_arguments(cx, call, text, (count, false), args, (spread, as_written))
        else {
            return Operand::UNKNOWN;
        };
        let context = format!("argument to unsafe.{text}");
        let byte_pointer = self.types.pointer(self.types.byte());
        match text {
            // The size and the alignment of the type of the argument are
            // constants, unless the type has a type parameter in it.
            "Sizeof" | "Alignof" => {
                let (at, x) = given[0].clone();
                let x = self.assignment(cx, at, x, None, &context);
                if x.mode == Mode::Unknown {
                    return Operand::UNKNOWN;
                }
                match self.types.layout(x.ty, self.names) {
                    Some((size, align)) => {
                        let bytes = if text == "Sizeof" { size } else { align };
                        let value = Value::Int64(bytes as i64);
                        Operand::constant(self.types.basic(Basic::Uintptr), value)
                    }
                    // The size of a type parameter is known when the
                    // function is used; a type in error has none.
                    None if self.types.has_param(x.ty) => self.value_of(Basic::Uintptr),
                    None => {
                        self.too_large(cx, at, &x);
                        Operand::UNKNOWN
                    }
                }
            }
            "Offsetof" => self.offset_of(cx, args[0]),
            "Add" => {
                let Some(pointer) = self.package_unsafe_pointer() else {
                    return Operand::UNKNOWN;
                };
                let (at, x) = given[0].clone();
                if self.assignment(cx, at, x, Some(pointer), &context).mode == Mode::Unknown {
                    return Operand::UNKNOWN;
                }
                match self.length(cx, &given[1], true) {
                    true => Operand::value(pointer),
                    false => Operand::UNKNOWN,
                }
            }
            "String" => {
                let (at, x) = given[0].clone();
                if self
                    .assignment(cx, at, x, Some(byte_pointer), &context)
                    .mode
                    == Mode::Unknown
                {
                    return Operand::UNKNOWN;
                }
                match self.length(cx, &given[1], false) {
                    true => self.value_of(Basic::String),
                    false => Operand::UNKNOWN,
                }
            }
            "StringData" => {
                let (at, x) = given[0].clone();
                let string = self.types.basic(Basic::String);
                match self.assignment(cx, at, x, Some(string), &context).mode {
                    Mode::Unknown => Operand::UNKNOWN,
                    _ => Operand::value(byte_pointer),
                }
            }
            "Slice" => {
                let (at, x) = &given[0];
                match &*self.types.get(self.types.underlying(x.ty)) {
                    Type::Pointer(elem) => match self.length(cx, &given[1], false) {
                        true => Operand::value(self.types.slice(*elem)),
                        false => Operand::UNKNOWN,
                    },
                    _ => self.not_a(cx, *at, x, "pointer"),
                }
            }
            "SliceData" => {
                let (at, x) = &given[0];
                match &*self.types.get(self.types.underlying(x.ty)) {
                    Type::Slice(elem) => Operand::value(self.types.pointer(*elem)),
                    _ => self.not_a(cx, *at, x, "slice"),
                }
            }
            _ => Operand::UNKNOWN,
        }
    }

    /// Reports the operand if its type has more bytes than can be
    /// counted.
    fn too_large(&mut self, cx: &Cx<'a>, expr: ExprId, x: &Operand) -> bool {
        let too_large = self.types.too_large(x.ty, self.names);
        if too_large {
            let message = format!("{} is too large", self.describe(cx, expr, x));
            self.error(Code::TypeTooLarge, cx.tree.exprs[expr].span, message);
        }
        too_large
    }

    /// The argument of a function of package unsafe is not of the kind
    /// that the function takes.
    fn not_a(&mut self, cx: &Cx<'a>, expr: ExprId, x: &Operand, kind: &str) -> Operand {
        if !self.types.has_param(x.ty) {
            let message = format!(
                "invalid argument: {} is not a {kind}",
                self.describe(cx, expr, x)
            );
            self.error(
                Code::BuiltinInvalidArgument,
                cx.tree.exprs[expr].span,
                message,
            );
        }
        Operand::UNKNOWN
    }

    /// Whether the argument is a length: an integer, and as a constant
    /// one that an `int` holds.
    fn length(&mut self, cx: &Cx<'a>, (at, x): &(ExprId, Operand), negative: bool) -> bool {
        self.valid_index(cx, *at, &mut x.clone(), "length", negative)
    }

    /// `unsafe.Offsetof(x.f)`: where the field `f` starts in `x`.
    fn offset_of(&mut self, cx: &Cx<'a>, arg: ExprId) -> Operand {
        let mut selector = arg;
        while let ExprKind::Paren(x) = cx.tree.exprs[selector].kind {
            selector = x;
        }
        let span = cx.tree.exprs[arg].span;
        let ExprKind::Selector { x, sel } = cx.tree.exprs[selector].kind else {
            let message = format!(
                "invalid argument: {} is not a selector expression",
                self.expr_string(cx, arg)
            );
            self.error(Code::BuiltinInvalidArgument, span, message);
            self.operand(cx, arg);
            return Operand::UNKNOWN;
        };
        let base = self.value(cx, x);
        if base.mode == Mode::Unknown || self.types.underlying(base.ty) == Types::INVALID {
            return Operand::UNKNOWN;
        }
        // The field may be that of a struct behind a pointer.
        let mut ty = base.ty;
        if let Type::Pointer(elem) = &*self.types.get(self.types.underlying(ty))
            && matches!(
                &*self.types.get(self.types.underlying(*elem)),
                Type::Struct(_)
            )
        {
            ty = *elem;
        }
        let name = self.names.resolve(&sel.name);
        let found = self
            .types
            .lookup(ty, sel.name, self.path, self.names, false);
        let (at, message) = match found.map(|found| found.member) {
            Ok(Member::Field {
                indirect: false, ..
            }) => {
                self.operand(cx, arg);
                return match self.types.offset_of(ty, sel.name, self.path, self.names) {
                    Some(offset) => {
                        let value = Value::Int64(offset as i64);
                        Operand::constant(self.types.basic(Basic::Uintptr), value)
                    }
                    None if self.too_large(cx, x, &base) => Operand::UNKNOWN,
                    None => self.value_of(Basic::Uintptr),
                };
            }
            Ok(Member::Field { indirect: true, .. }) => (
                x,
                format!(
                    "invalid argument: field {name} is embedded via a pointer in {}",
                    self.type_string(ty)
                ),
            ),
            Ok(Member::Method { .. }) => (
                arg,
                format!(
                    "invalid argument: {} is a method value",
                    self.expr_string(cx, arg)
                ),
            ),
            Err(_) if self.types.has_param(ty) => return self.value_of(Basic::Uintptr),
            Err(_) => (
                x,
                format!(
                    "invalid argument: {} has no single field {name}",
                    self.type_string(ty)
                ),
            ),
        };
        self.error(
            Code::BuiltinInvalidArgument,
            cx.tree.exprs[at].span,
            message,
        );
        Operand::UNKNOWN
    }

    /// The type `unsafe.Pointer`, found in a package that imports unsafe.
    fn package_unsafe_pointer(&self) -> Option<TypeId> {
        let pointer = self.names.get("Pointer")?;
        self.package
            .objects
            .iter()
            .find_map(|object| match &object.kind {
                ObjKind::Import { package, .. } if package.path == "unsafe" => {
                    let &member = package.scope.get(&pointer)?;
                    Some(package.object(member).ty)
                }
                _ => None,
            })
    }

    // ---- function bodies

    /// Types every expression of the bodies of the file, statement by
    /// statement, so that each local variable has its type where it is
    /// used.
    /// The top-level declarations of the file `file`, each with how many
    /// values a constant without values of its own repeats, in its group.
    fn body_jobs(file: usize, unit: Unit<'a>) -> Vec<Job> {
        let tree = unit.file;
        let mut repeated_values: Option<(Option<u32>, usize)> = None;
        let mut jobs = Vec::new();
        for (index, &decl) in tree.decls.iter().enumerate() {
            let mut repeated = 0;
            if let DeclKind::Const { ty, values, .. } = &tree.decl_nodes[decl].kind {
                let group = tree.decl_nodes[decl].group;
                repeated = match (ty.is_none() && values.is_empty(), repeated_values) {
                    (true, Some((last, count))) if last == group && group.is_some() => count,
                    (true, _) => 0,
                    (false, _) => {
                        repeated_values = Some((group, values.len()));
                        values.len()
                    }
                };
            }
            jobs.push((file, index, repeated));
        }
        jobs
    }

    /// Checks the top-level declaration `index` of the file `file`: the
    /// body of a function, the values of variables.
    fn body(&mut self, file: usize, unit: Unit<'a>, index: usize, repeated: usize) -> BodyTypes {
        let tree = unit.file;
        let decl = tree.decls[index];
        let body = &self.package.files[file].bodies[index];
        let form = Form::Var {
            ty: None,
            value: None,
        };
        let mut cx = self.cx(&Decl { unit, body, form }, None);
        self.locals.clear();
        self.pointer_methods.clear();
        self.localized.clear();
        self.bindings.clear();
        self.read_only.clear();
        self.flow = flow::Flow::default();
        self.local_groups.clear();
        self.shifted.clear();
        self.memo = Some(FxHashMap::default());
        self.current.clear();
        self.decl_attributes(&cx, &tree.decl_nodes[decl]);
        match &tree.decl_nodes[decl].kind {
            DeclKind::Func {
                receiver,
                name,
                type_params,
                ty,
                body: Some(block),
                ..
            } => {
                // What the body uses, the function or the method
                // needs; `init` is no object.
                let function = match receiver {
                    Some(_) => self.method_nodes.get(&name.span),
                    None => self.by_span.get(&name.span),
                };
                self.current.extend(function);
                let own = self.declare_params(&mut cx, type_params);
                // In the body the type parameters are those of the
                // function, by other names.
                let declared = self.by_span.get(&name.span).copied();
                let of_function = declared
                    .filter(|_| receiver.is_none())
                    .and_then(|object| self.types.generic_func(self.ty[object]));
                if let Some(of_function) = of_function {
                    self.mono_same(&own, &of_function);
                }
                self.receiver_of = None;
                self.body_params.clone_from(&own);
                if let Some(receiver) = receiver {
                    let named = self.receiver_params(&mut cx, receiver.ty);
                    self.body_params.splice(0..0, named.iter().copied());
                    self.around.extend(&named);
                    self.receiver_of = self.receiver_named(unit, receiver.ty);
                    if let Some(Type::Named { id, .. }) =
                        self.receiver_of.map(|ty| self.types.get(ty)).as_deref()
                    {
                        // In the body the type parameters of the type
                        // have other names, and the same constraints.
                        let declared = self.types.type_params(*id);
                        for (&param, &of) in named.iter().zip(&declared) {
                            if let Some(constraint) = self.types.constraint(of) {
                                let constraint =
                                    self.types.substitute(constraint, &declared, &named);
                                self.types.set_constraint(param, constraint);
                            }
                        }
                    }
                    self.params(&cx, std::slice::from_ref(receiver));
                    if let Some(Type::Named { id, .. }) =
                        self.receiver_of.map(|ty| self.types.get(ty)).as_deref()
                    {
                        for &param in &named {
                            self.param_span(param, name.span, false);
                        }
                        match own.is_empty() {
                            true => self.mono_canon(&named, *id),
                            false => self.mono_method(name.span, &named, &own),
                        }
                    }
                    self.base_declared(&cx);
                }
                // The body of a `@Const` function is held to what a
                // constant expression may do (EXP-33).
                let marks = self.marks(&cx, &tree.decl_nodes[decl].attrs);
                let is_const = marks.has(crate::types::Marks::CONST);
                self.const_fn = is_const.then_some(name.name);
                self.around.extend(&own);
                self.signature_locals(&cx, *ty);
                let sig = self.typ(&cx, *ty);
                self.function_body(&cx, *ty, sig, block);
                self.const_fn = None;
                self.around.clear();
                self.receiver_of = None;
            }
            // A function without a body: one with type parameters needs
            // one, and one without a name has its signature checked
            // nowhere else.
            DeclKind::Func {
                receiver: None,
                name,
                type_params,
                ty,
                body: None,
                ..
            } if !self.by_span.contains_key(&name.span) => {
                self.declare_params(&mut cx, type_params);
                self.signature_locals(&cx, *ty);
                self.typ(&cx, *ty);
            }
            // The value of a constant is worked out with its
            // declaration; variables are walked here too, for the
            // function literals in their values.
            DeclKind::Var { names, ty, values } => {
                let declared = ty.map(|ty| self.var_type(&cx, ty));
                self.arity(&cx, names, values, None);
                if let (Some(declared), true) = (declared, values.is_empty()) {
                    self.declared_unset(names, declared);
                }
                let objects = |this: &Self, names: &[Ident]| -> Vec<usize> {
                    let object = |name: &Ident| this.by_span.get(&name.span).copied();
                    names.iter().filter_map(object).collect()
                };
                // `a, b = f()` gives both the one value.
                if values.len() == 1 && names.len() > 1 {
                    self.current = objects(self, names);
                    self.var_decl(&cx, names, declared, values);
                } else {
                    for (position, &value) in values.iter().enumerate() {
                        let declared_here = names.get(position..=position).unwrap_or(&[]);
                        self.current = objects(self, declared_here);
                        match declared_here.is_empty() {
                            true => drop(self.operand(&cx, value)),
                            false => drop(self.var_decl(&cx, declared_here, declared, &[value])),
                        }
                    }
                }
                // A variable of the package lives as long as the
                // program: so does what its pointers point to (OWN-B5).
                let one_each = values.len() == names.len();
                for (position, name) in names.iter().enumerate() {
                    // What `_` gets is kept nowhere.
                    if name.name == self.blank {
                        continue;
                    }
                    let object = self.by_span.get(&name.span).copied();
                    let Some(ty) = object.and_then(|object| self.ty.get(object).copied()) else {
                        continue;
                    };
                    // Every goroutine reaches a variable of the package,
                    // and a `Local` function or interface stays with one.
                    let send = crate::types::Marker::Send;
                    if let Some((part, crate::types::Unlabeled::Local)) =
                        self.types.unlabeled(ty, send)
                    {
                        let message = format!(
                            "cannot keep a {} in a variable of the package: every goroutine may \
                             use the variable, and a {} stays with one goroutine",
                            self.type_string(ty),
                            self.type_string(part)
                        );
                        self.error(Code::FuncSharesCapture, name.span, message);
                        continue;
                    }
                    let value = values.get(position).copied().filter(|_| one_each);
                    let stays = match value {
                        Some(value) => self.points_to_packages(&cx, value),
                        None => values.is_empty(),
                    };
                    if !stays {
                        let at = value.map_or(name.span, |value| tree.exprs[value].span);
                        self.on_the_heap(ty, at, "a variable of the package");
                    }
                }
            }
            DeclKind::Const { names, values, .. } => {
                self.arity(&cx, names, values, Some(repeated));
                // A function literal in the value of a constant has a
                // body, which declares what the value does not use.
                if !cx.body.locals.is_empty() {
                    for name in names {
                        let object = self.by_span.get(&name.span).copied();
                        let Some(decl) = object.and_then(|object| self.decls[object]) else {
                            continue;
                        };
                        if let Form::Const {
                            value: Some(value),
                            iota,
                            ..
                        } = decl.form
                        {
                            let within = self.cx(&decl, Some(iota));
                            self.operand(&within, value);
                        }
                    }
                }
            }
            _ => {}
        }
        let typed = |(expr, operand): (ExprId, Operand)| {
            // A generic function that is not called has the type that
            // what it is assigned to gives it, which is not followed
            // yet.
            if self.types.generic_func(operand.ty).is_some() {
                return None;
            }
            if !operand.is_value() {
                return None;
            }
            let value = match operand.mode {
                Mode::Const(value) => value,
                _ => Value::Unknown,
            };
            Some((
                expr,
                ExprType {
                    ty: operand.ty,
                    value,
                },
            ))
        };
        self.addresses_of_required(&cx);
        let captures = self.captured_by_literal();
        let lender = self.lender(&cx, &tree.decl_nodes[decl]);
        let (last_uses, lent) = self.moves(&cx, &captures, (&tree.decl_nodes[decl], lender));
        let memo = self.memo.take().unwrap_or_default();
        let exprs = memo.into_iter().filter_map(typed).collect();
        self.validate();
        let locals = self
            .locals
            .drain()
            .map(|(local, operand)| (local, operand.ty));
        BodyTypes {
            exprs,
            locals: locals.collect(),
            excused: std::mem::take(&mut self.excused),
            selections: std::mem::take(&mut self.selections),
            writes_through: std::mem::take(&mut self.writes_through),
            var_modes: std::mem::take(&mut self.var_modes),
            swaps: std::mem::take(&mut self.swaps),
            self_moves: std::mem::take(&mut self.self_moves),
            last_uses,
            lent: lent.into_iter().map(|problem| (index, problem)).collect(),
            lender,
            captures,
            method_values: std::mem::take(&mut self.method_values),
            narrowed: std::mem::take(&mut self.narrowed),
            iface_args: std::mem::take(&mut self.iface_args),
            operators: std::mem::take(&mut self.operators),
            op_assigns: std::mem::take(&mut self.op_assigns),
            index_stores: std::mem::take(&mut self.index_stores),
            case_operators: std::mem::take(&mut self.case_operators),
            foreign_uses: std::mem::take(&mut self.foreign_uses),
            iface_conversions: std::mem::take(&mut self.iface_conversions),
            case_types: std::mem::take(&mut self.case_types),
            iface_changes: std::mem::take(&mut self.iface_changes),
            part_conversions: std::mem::take(&mut self.part_conversions),
            instances: std::mem::take(&mut self.instantiated),
            type_params: std::mem::take(&mut self.body_params),
            given_types: std::mem::take(&mut self.given_types),
            param_methods: std::mem::take(&mut self.param_methods),
            receiver_marks: std::mem::take(&mut self.receiver_marks),
            bangs: std::mem::take(&mut self.bangs),
        }
    }

    /// Walks the statements of a function of the type `sig`.
    fn function_body(&mut self, cx: &Cx<'a>, ty: ExprId, sig: TypeId, body: &ast::Block) {
        let (named, fails) = match &cx.tree.exprs[ty].kind {
            ExprKind::FuncType { results, fails, .. } => (
                results.first().is_some_and(|result| result.name.is_some()),
                *fails,
            ),
            _ => (false, false),
        };
        // The results of a signature that is in error are not known.
        let results = match &*self.types.get(sig) {
            Type::Func { results, .. } => {
                let unset = match &results[..] {
                    [rest @ .., last] if !fails && *last == self.types.error() => {
                        self.steps += rest.len() as u64;
                        rest.iter().copied().find(|&ty| self.may_lack_zero(ty))
                    }
                    _ => None,
                };
                Some(Results {
                    types: results.to_vec(),
                    named,
                    fails,
                    unset,
                })
            }
            _ => None,
        };
        // A function with results ends in a statement that does not go
        // on; the error that `!` adds is no result of its own.
        let returns = results
            .as_ref()
            .is_some_and(|results| results.types.len() > usize::from(fails));
        self.results.push(results);
        // Nothing that the function around knows is known in this one.
        let outer = self.flow_enter();
        self.stmts(cx, &body.stmts);
        self.flow_leave(outer);
        self.results.pop();
        self.branches(cx, body, returns);
    }

    /// `return results` in a function with the results `wanted`. A
    /// function that can fail returns its results and no error, or one
    /// value more, which is the error (ERR-3).
    fn returned(&mut self, cx: &Cx<'a>, whole: Span, wanted: &Results, results: &[ExprId]) {
        let types = &wanted.types;
        let given = match results {
            [one] => match self.operand(cx, *one) {
                Operand {
                    mode: Mode::NoValue,
                    ..
                } => 0,
                x => match &*self.types.get(x.ty) {
                    Type::Tuple(types) if x.mode == Mode::Value => types.len(),
                    _ => 1,
                },
            },
            many => many.len(),
        };
        let places =
            |types: &[TypeId]| -> Vec<Option<TypeId>> { types.iter().copied().map(Some).collect() };
        let whole = Init::Return(whole);
        let listed = results.len() == types.len();
        if wanted.fails && !listed && given == types.len() - 1 {
            self.init_vars(cx, &places(&types[..given]), results, whole);
            return;
        }
        if !listed || !wanted.fails && wanted.unset.is_none() {
            self.init_vars(cx, &places(types), results, whole);
            return;
        }
        let is_nil = |expr: ExprId| {
            let mut name = expr;
            while let ExprKind::Paren(inner) = cx.tree.exprs[name].kind {
                name = inner;
            }
            cx.body.uses.get(&name) == Some(&Ref::Universe(Predeclared::Nil))
        };
        let fails = wanted.fails || results.last().is_some_and(|&error| !is_nil(error));
        for (at, (&result, &ty)) in results.iter().zip(types).enumerate() {
            let unset = fails && at + 1 < types.len() && is_nil(result);
            // A failing return writes `nil` for a result that has no zero
            // value: that result is left unset.
            if unset && wanted.fails && self.may_lack_zero(ty) {
                self.operand(cx, result);
            } else if unset && !wanted.fails && self.never_nil(ty) {
                self.operand(cx, result);
                let shown = self.type_string(ty);
                let message = format!("cannot use nil as {shown} value in return statement");
                let fix = format!(
                    "a {shown} is never nil: declare the function with ! or return Option[{shown}]"
                );
                let span = cx.tree.exprs[result].span;
                self.error_with_fix(Code::FailReturnNil, span, message, (Code::NoteFixBang, fix));
            } else {
                self.init_var(cx, result, None, Some(ty), "return statement");
            }
        }
    }

    /// Declares the type parameters that the receiver `T[P, Q]` names.
    fn receiver_params(&mut self, cx: &mut Cx<'a>, mut base: ExprId) -> Vec<TypeId> {
        let tree = cx.tree;
        let mut named = Vec::new();
        loop {
            match &tree.exprs[base].kind {
                ExprKind::Paren(x)
                | ExprKind::Unary {
                    op: UnaryOp::Star,
                    x,
                } => base = *x,
                ExprKind::Index { indices, .. } => {
                    for index in indices {
                        let span = tree.exprs[*index].span;
                        if let (ExprKind::Name(name), Some(&local)) =
                            (&tree.exprs[*index].kind, cx.body.defs.get(&span.lo))
                        {
                            let param = self.types.param(*name);
                            cx.params.insert(local, param);
                            named.push(param);
                        }
                    }
                    return named;
                }
                _ => return named,
            }
        }
    }

    /// Gives each named parameter of the list its type.
    fn params(&mut self, cx: &Cx<'a>, params: &[Param]) {
        for param in params {
            let ty = self.typ(cx, param.ty);
            let local = param.name.and_then(|name| cx.body.defs.get(&name.span.lo));
            if let Some(&local) = local {
                self.locals.insert(local, Operand::variable(ty));
            }
        }
    }

    /// Gives the parameters and the named results of the function type
    /// `ty` their types.
    fn signature_locals(&mut self, cx: &Cx<'a>, ty: ExprId) {
        if let ExprKind::FuncType {
            params, results, ..
        } = &cx.tree.exprs[ty].kind
        {
            self.params(cx, params);
            self.params(cx, results);
            // A result with a name starts as the zero value of its type.
            for result in results {
                let Some(name) = result.name.filter(|name| name.name != self.blank) else {
                    continue;
                };
                let local = cx.body.defs.get(&name.span.lo);
                let Some(ty) = local.and_then(|local| self.locals.get(local)).map(|x| x.ty) else {
                    continue;
                };
                let text = self.names.resolve(&name.name);
                let doing = format!("cannot name the result {text}");
                self.zero_needed(name.span, ty, &doing, "declare the result without a name");
            }
        }
    }

    /// The local that the identifier at `expr` declares, if it declares
    /// one.
    fn declared_local(&self, cx: &Cx<'a>, expr: ExprId) -> Option<LocalId> {
        cx.body.defs.get(&cx.tree.exprs[expr].span.lo).copied()
    }

    fn set_local(&mut self, local: Option<LocalId>, ty: Option<TypeId>) {
        if let Some(local) = local {
            let operand = ty.map_or(Operand::UNKNOWN, |ty| {
                Operand::variable(self.types.default_type(ty))
            });
            self.locals.insert(local, operand);
        }
    }

    fn stmts(&mut self, cx: &Cx<'a>, stmts: &[StmtId]) {
        // What a block finds out, it knows to its end.
        let mark = self.flow_mark();
        for &stmt in stmts {
            gopp_syntax::with_stack(|| self.stmt(cx, stmt));
        }
        self.flow_back(mark);
    }

    fn opt_stmt(&mut self, cx: &Cx<'a>, stmt: Option<StmtId>) {
        if let Some(stmt) = stmt {
            gopp_syntax::with_stack(|| self.stmt(cx, stmt));
        }
    }

    fn stmt(&mut self, cx: &Cx<'a>, id: StmtId) {
        self.stmt_checked(cx, id);
        self.flow_left(cx, id);
    }

    fn stmt_checked(&mut self, cx: &Cx<'a>, id: StmtId) {
        let tree = cx.tree;
        match &tree.stmts[id].kind {
            StmtKind::Bad | StmtKind::Empty | StmtKind::Branch { .. } => {}
            StmtKind::Labeled { stmt, .. } => {
                self.flow_label();
                self.stmt(cx, *stmt);
            }
            StmtKind::Block(block) => self.stmts(cx, &block.stmts),
            StmtKind::Expr(x) => self.expr_stmt(cx, *x),
            StmtKind::Go(x) => {
                self.spawns = true;
                self.impure(tree.stmts[id].span, "has a go statement");
                self.suspended(cx, "go", *x);
            }
            StmtKind::Defer(x) => {
                self.impure(tree.stmts[id].span, "has a defer statement");
                self.suspended(cx, "defer", *x);
            }
            StmtKind::IncDec { x, inc } => {
                let operand = self.value(cx, *x);
                let operand = self.read(operand);
                if operand.mode == Mode::Unknown {
                    return;
                }
                if self.all_numeric(operand.ty) {
                    if let Some(to) = self.lhs_var(cx, *x) {
                        self.note_write(cx, *x, to, None);
                    }
                    return;
                }
                let message = format!(
                    "invalid operation: {}{} (non-numeric type {})",
                    self.expr_string(cx, *x),
                    if *inc { "++" } else { "--" },
                    self.type_string(operand.ty)
                );
                self.error(Code::IncDecNonNumeric, tree.exprs[*x].span, message);
            }
            StmtKind::Send { chan, value } => {
                let to = *chan;
                let chan = self.value(cx, to);
                // An error is at the arrow.
                let after = tree.exprs[to].span.hi;
                let rest = &cx.text[after as usize..tree.exprs[*value].span.lo as usize];
                let arrow = after + rest.find("<-").unwrap_or(0) as u32;
                let arrow = Span {
                    lo: arrow,
                    hi: arrow + 2,
                    ..tree.exprs[to].span
                };
                match self.chan_elem(cx, (to, arrow), &chan, false) {
                    Some(elem) => drop(self.assign(cx, *value, Some(elem), "send")),
                    None => drop(self.operand(cx, *value)),
                }
            }
            StmtKind::Return(results) => match self.results.last().cloned().flatten() {
                // `return` alone returns the results that have names.
                Some(wanted) if !(results.is_empty() && wanted.named) => {
                    self.returned(cx, tree.stmts[id].span, &wanted, results);
                }
                // The results are returned by their names, which still
                // denote them at the statement.
                Some(_) => {
                    let hidden = cx.body.hidden_results.get(&id);
                    for &(result, inner) in hidden.into_iter().flatten() {
                        let name = self.names.resolve(&cx.body.locals[result.0 as usize].name);
                        let ty = self.locals.get(&result).map_or(Types::INVALID, |x| x.ty);
                        let message = format!("result parameter {name} not in scope at return");
                        let note =
                            format!("inner declaration of var {name} {}", self.type_string(ty));
                        let span = tree.stmts[id].span;
                        if self.report_once((span, Code::ShadowedResult)) {
                            let diagnostic = Diagnostic::new(Code::ShadowedResult, span, message);
                            let inner = cx.body.locals[inner.0 as usize].span;
                            let diagnostic =
                                diagnostic.with_note(Code::NoteInnerDeclaration, Some(inner), note);
                            self.emit(diagnostic);
                        }
                    }
                }
                _ => {
                    for &result in results {
                        self.operand(cx, result);
                    }
                }
            },
            StmtKind::Decl(decls) => {
                for &decl in decls {
                    self.local_decl(cx, decl);
                }
            }
            StmtKind::Assign {
                op: AssignOp::Define,
                lhs,
                rhs,
            } => {
                let locals: Vec<_> = lhs.iter().map(|&x| self.declared_local(cx, x)).collect();
                // A name on the left that is declared already is assigned
                // to: it has its type.
                let mut places = Vec::new();
                for (&place, local) in lhs.iter().zip(&locals) {
                    let is_blank = matches!(tree.exprs[place].kind, ExprKind::Name(name) if name == self.blank);
                    if local.is_some() || is_blank {
                        places.push(None);
                        continue;
                    }
                    // What is declared in this block and is no variable
                    // is not assigned to.
                    let declared = self.operand(cx, place);
                    let assigned = matches!(declared.mode, Mode::Variable | Mode::Unknown);
                    if !assigned {
                        let message = format!("cannot assign to {}", self.expr_string(cx, place));
                        self.error(Code::CannotAssignTo, tree.exprs[place].span, message);
                    }
                    self.wrote(cx, place);
                    self.flow_written(cx, place);
                    places.push(assigned.then_some(declared.ty));
                }
                let types = self.init_vars(cx, &places, rhs, Init::Variables);
                self.only_read(cx, &locals, rhs);
                self.flow_defined(cx, lhs, rhs, &types);
                for (local, ty) in locals.into_iter().zip(types) {
                    if let Some(local) = local {
                        self.locals.insert(local, Operand::variable(ty));
                    }
                }
            }
            StmtKind::Assign {
                op: AssignOp::Assign,
                lhs,
                rhs,
            } => self.assign_vars(cx, lhs, rhs),
            // `x op= y` is `x = x op y`.
            StmtKind::Assign {
                op: AssignOp::Op(op),
                lhs,
                rhs,
            } if lhs.len() == 1 && rhs.len() == 1 => {
                if self.binary(cx, None, *op, lhs[0], rhs[0]).mode != Mode::Unknown
                    && let Some(to) = self.lhs_var(cx, lhs[0])
                {
                    self.note_write(cx, lhs[0], to, None);
                }
            }
            StmtKind::Assign { op, lhs, rhs } => {
                let targets: Vec<Operand> = lhs.iter().map(|&x| self.operand(cx, x)).collect();
                for &x in rhs {
                    self.operand(cx, x);
                }
                // A value of no type takes the type of what it is assigned
                // to; assigned to `_`, its default type. The count of a
                // shift keeps its own type.
                let shift = matches!(op, AssignOp::Op(BinaryOp::Shl | BinaryOp::Shr));
                if lhs.len() == rhs.len() && !shift {
                    for ((target, &place), &value) in targets.iter().zip(lhs).zip(rhs) {
                        let blank = matches!(tree.exprs[place].kind, ExprKind::Name(name) if name == self.blank);
                        match (target.is_value(), blank) {
                            (true, _) => self.convert(cx, value, target.ty),
                            (false, true) => self.convert_default(cx, value),
                            (false, false) => {}
                        }
                    }
                }
            }
            StmtKind::If {
                init,
                cond,
                then,
                otherwise,
            } => {
                // What the header declares and finds out ends with the
                // statement; what an arm that leaves rules out holds after.
                let outer = self.flow_mark();
                self.opt_stmt(cx, *init);
                self.condition(cx, *cond, "if");
                let (mut held, mut ruled_out) = (Vec::new(), Vec::new());
                self.flow_facts(cx, *cond, true, &mut held);
                self.flow_facts(cx, *cond, false, &mut ruled_out);
                let mark = self.flow_mark();
                self.flow_add(&held);
                self.stmts(cx, &then.stmts);
                self.flow_back(mark);
                self.flow_add(&ruled_out);
                self.opt_stmt(cx, *otherwise);
                self.flow_back(mark);
                let then_leaves = self.flow_block_leaves(&then.stmts);
                let else_leaves = otherwise.is_some_and(|arm| self.flow_leaves(arm));
                if init.is_some() {
                    self.flow_back(outer);
                }
                match (then_leaves, else_leaves) {
                    (true, false) => self.flow_add(&ruled_out),
                    (false, true) => self.flow_add(&held),
                    _ => {}
                }
            }
            StmtKind::For {
                init,
                cond,
                post,
                body,
            } => {
                let mark = self.flow_mark();
                self.opt_stmt(cx, *init);
                self.flow_loop(tree.stmts[id].span);
                let mut held = Vec::new();
                if let Some(cond) = cond {
                    self.condition(cx, *cond, "for");
                    self.flow_facts(cx, *cond, true, &mut held);
                }
                // The condition holds in the body, and after it where the
                // body left it alone.
                self.flow_add(&held);
                self.stmts(cx, &body.stmts);
                self.opt_stmt(cx, *post);
                self.flow_loop_end(cx);
                self.flow_back(mark);
            }
            StmtKind::Range {
                lhs,
                define,
                x,
                body,
            } => {
                self.lends = true;
                self.range_clause(cx, lhs, *define, *x);
                self.flow_loop(tree.stmts[id].span);
                self.stmts(cx, &body.stmts);
                self.flow_loop_end(cx);
            }
            StmtKind::Switch { init, tag, clauses } => {
                self.opt_stmt(cx, *init);
                let guard = tag.map(|tag| &tree.exprs[tag].kind);
                let switched = match guard {
                    Some(&ExprKind::TypeSwitchGuard { x, .. }) => {
                        let switched = self.asserted(cx, x, "", "type switch");
                        Some((x, switched.unwrap_or(Operand::UNKNOWN)))
                    }
                    _ => None,
                };
                let tagged = match switched {
                    Some(_) => Operand::UNKNOWN,
                    None => self.switch_tag(cx, *tag),
                };
                self.defaults(cx, clauses.iter().map(|c| (c.cases.is_none(), c.span)));
                let mut values = FxHashMap::default();
                let mut types = FxHashMap::default();
                let vars = cx.body.switch_vars.get(&id);
                for (index, clause) in clauses.iter().enumerate() {
                    let cases = clause.cases.as_deref().unwrap_or(&[]);
                    let mut case_types = Vec::new();
                    for &case in cases {
                        match &switched {
                            Some((of, switched)) => {
                                let case_type = self.type_or_nil(cx, case);
                                self.case_type(cx, case, &case_type, (*of, switched), &mut types);
                                if case_type.mode == Mode::Type && self.memo.is_some() {
                                    self.case_types.insert(case, case_type.ty);
                                }
                                case_types.push(case_type);
                            }
                            None => self.case_value(cx, &tagged, *tag, case, &mut values),
                        }
                    }
                    // The variable of a type switch has the type of the
                    // case when the clause has one type, else the type of
                    // what is switched on.
                    if let (Some((_, switched)), Some(&local)) =
                        (&switched, vars.and_then(|vars| vars.get(index)))
                    {
                        let ty = match &case_types[..] {
                            [one] if one.mode == Mode::Type => one.ty,
                            [one] if one.mode == Mode::Unknown => Types::INVALID,
                            _ => switched.ty,
                        };
                        self.locals.insert(local, Operand::variable(ty));
                    }
                    self.stmts(cx, &clause.body);
                }
            }
            StmtKind::Select { clauses } => {
                self.impure(tree.stmts[id].span, "has a select statement");
                self.defaults(cx, clauses.iter().map(|c| (c.comm.is_none(), c.span)));
                for clause in clauses {
                    self.opt_stmt(cx, clause.comm);
                    self.stmts(cx, &clause.body);
                }
            }
        }
    }

    /// A case of a type switch: a type, or the predeclared `nil`.
    fn type_or_nil(&mut self, cx: &Cx<'a>, case: ExprId) -> Operand {
        let mut name = case;
        while let ExprKind::Paren(x) = cx.tree.exprs[name].kind {
            name = x;
        }
        let nil = cx.body.uses.get(&name) == Some(&Ref::Universe(Predeclared::Nil));
        if !nil && self.var_type(cx, case) == Types::INVALID {
            return Operand::UNKNOWN;
        }
        self.operand(cx, case)
    }

    /// A `const`, `var` or `type` statement inside a function.
    fn local_decl(&mut self, cx: &Cx<'a>, decl: DeclId) {
        let tree = cx.tree;
        let node = &tree.decl_nodes[decl];
        let local = |name: Ident| cx.body.defs.get(&name.span.lo).copied();
        match &node.kind {
            DeclKind::Var { names, ty, values } => {
                let locals: Vec<_> = names.iter().map(|&name| local(name)).collect();
                let declared = ty.map(|ty| self.var_type(cx, ty));
                self.arity(cx, names, values, None);
                let types = self.var_decl(cx, names, declared, values);
                self.only_read(cx, &locals, values);
                for (local, ty) in locals.into_iter().zip(types) {
                    if let Some(local) = local {
                        self.locals.insert(local, Operand::variable(ty));
                    }
                }
            }
            DeclKind::Const { names, ty, values } => {
                // A constant without a value repeats the one above it in
                // its group, with the next `iota`.
                let own = values;
                let (mut ty, mut values, mut iota) = (*ty, values.clone(), 0);
                let mut repeats = false;
                if let Some(group) = node.group {
                    let last = self.local_groups.entry(group).or_default();
                    if ty.is_none() && values.is_empty() {
                        (ty, values) = (last.ty, last.values.clone());
                        repeats = true;
                    } else {
                        (last.ty, last.values) = (ty, values.clone());
                    }
                    iota = last.next;
                    last.next += 1;
                }
                self.arity(cx, names, own, Some(values.len()));
                let mut inner = cx.clone();
                inner.iota = Some(iota);
                let declared = ty.map(|ty| self.const_type(&inner, ty));
                for (position, &name) in names.iter().enumerate() {
                    // A repeated value is worked out anew with its `iota`,
                    // and what is wrong with it is said at the constant.
                    self.fresh = true;
                    self.error_at = repeats.then_some(name.span);
                    let operand = match values.get(position) {
                        Some(&value) => self.const_value(&inner, declared, value),
                        None => Operand::UNKNOWN,
                    };
                    self.error_at = None;
                    self.fresh = false;
                    let operand = match operand.mode {
                        Mode::Const(_) => operand,
                        _ => Operand::UNKNOWN,
                    };
                    if let Some(local) = local(name) {
                        self.locals.insert(local, operand);
                    }
                }
            }
            DeclKind::Type {
                name,
                alias,
                ty,
                type_params,
            } => {
                // A type that is named `_` is checked like any other.
                let this = local(*name);
                let params = self.new_params(type_params);
                for (param, &ty) in type_params.iter().zip(&params) {
                    if let Some(param) = local(param.name) {
                        let mode = Mode::Type;
                        self.locals.insert(param, Operand { mode, ty });
                    }
                }
                self.constrain(cx, type_params, &params);
                if *alias {
                    let aliased = self.typ(cx, *ty);
                    let ty = self.alias(name.name, params, aliased);
                    if let Some(this) = this {
                        let mode = Mode::Type;
                        self.locals.insert(this, Operand { mode, ty });
                    }
                    return;
                }
                // The type exists before what it is declared as is known,
                // so that it can name itself.
                let named = self.types.declare(name.name, self.name);
                self.types.set_home(named, self.path, true);
                self.types.set_type_params(named, params);
                let around = std::mem::take(&mut self.around);
                self.mono_local(named, name.span, &around);
                self.around = around;
                let declared = self.types.named(named, &[]);
                self.written(declared, name.span);
                if let Some(this) = this {
                    let mode = Mode::Type;
                    self.locals.insert(this, Operand { mode, ty: declared });
                }
                let underlying = self.typ(cx, *ty);
                let underlying = self.no_param_declared(cx, *ty, underlying);
                self.types.set_underlying(named, underlying);
            }
            _ => {}
        }
    }
}
