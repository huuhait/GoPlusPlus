//! The types of the package-level declarations and the values of the
//! constants.
//!
//! A declaration is worked out when it is first needed, so the order of
//! the source does not matter. What this module cannot work out yet (a
//! value that needs the type of an expression that is not constant) is
//! left invalid, which fits everywhere and is no error.

mod assign;
mod builtins;
mod call;
mod conversion;
mod index;
mod literal;
mod ops;

use gopp_diag::{Code, Diagnostic, Sink};
use gopp_span::{Dialect, Span};
use gopp_syntax::ast::{self, *};
use lasso::{Spur, ThreadedRodeo};
use rustc_hash::{FxHashMap, FxHashSet};

use crate::basic::{Basic, representable};
use crate::constant::{self, CompareOp, Kind, Value};
use crate::literal::unquote;
use crate::object::{Body, ExprType, LocalId, LocalKind, ObjId, ObjKind, Ref};
use crate::package::Package;
use crate::resolve::Unit;
use crate::types::{ChanDir, DeclaredMethod, Field, Member, Method, Term, Type, TypeId, Types};
use crate::universe::{Predeclared, PredeclaredKind};
use assign::Init;

/// Where an expression stands: its file, what its names denote, and the
/// type parameters in scope.
#[derive(Clone)]
struct Cx<'a> {
    tree: &'a File,
    body: &'a Body,
    text: &'a str,
    dialect: Dialect,
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
            Types::INVALID => Mode::Unknown,
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
            Types::INVALID => Mode::Unknown,
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
    /// While a body is walked: the types of the results of the function
    /// that the statements are in, the innermost last.
    /// With whether the results have names: `return` alone returns them.
    results: Vec<Option<(Vec<TypeId>, bool)>>,
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
    /// Where each of them was written.
    written_at: FxHashMap<TypeId, Span>,
    /// The types that are known to hold no value of themselves.
    valid: FxHashSet<TypeId>,
}

/// What was found in one body: the type of each expression with a value,
/// the type of each local, and the variables that are not also unused.
type BodyTypes = (
    FxHashMap<ExprId, ExprType>,
    Vec<(LocalId, TypeId)>,
    Vec<LocalId>,
);

/// A group of constants inside a function: the index of its next constant,
/// and the type and values that a constant without a value repeats.
#[derive(Default)]
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
) -> Vec<Vec<usize>> {
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
        called_or_received: false,
        error_at: None,
        local_groups: FxHashMap::default(),
        excused: Vec::new(),
        by_span,
        current: Vec::new(),
        needs: vec![Vec::new(); count],
        needed: FxHashSet::default(),
        under_way: Vec::new(),
        broken: vec![false; count],
        in_type_params: false,
        to_validate: Vec::new(),
        written_at: FxHashMap::default(),
        valid: FxHashSet::default(),
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
    checker.validate();
    let types_of_files: Vec<_> = units
        .iter()
        .enumerate()
        .map(|(file, unit)| checker.bodies(file, *unit))
        .collect();
    let Checker {
        ty, value, needs, ..
    } = checker;
    for ((object, ty), value) in package.objects.iter_mut().zip(ty).zip(value) {
        object.ty = ty;
        object.value = value;
    }
    for (file, types_of_bodies) in package.files.iter_mut().zip(types_of_files) {
        for (body, (exprs, locals, excused)) in file.bodies.iter_mut().zip(types_of_bodies) {
            body.types = exprs;
            for (LocalId(local), ty) in locals {
                body.locals[local as usize].ty = ty;
            }
            for LocalId(local) in excused {
                body.locals[local as usize].used = true;
            }
        }
    }
    needs
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
                    let form = Form::Type {
                        alias: *alias,
                        type_params,
                        ty: *ty,
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
                }
                _ => {}
            }
        }
    }

    fn cx(&self, decl: &Decl<'a>, iota: Option<i64>) -> Cx<'a> {
        Cx {
            tree: decl.unit.file,
            body: decl.body,
            text: decl.unit.source.text(),
            dialect: decl.unit.source.dialect(),
            iota,
            params: FxHashMap::default(),
        }
    }

    /// Gives each type parameter of a declaration a type, then each its
    /// constraint, which may name the others.
    fn declare_params(&mut self, cx: &mut Cx<'a>, type_params: &[TypeParam]) -> Vec<TypeId> {
        let mut params = Vec::new();
        for param in type_params {
            let ty = self.types.param(param.name.name);
            if let Some(&local) = cx.body.defs.get(&param.name.span.lo) {
                cx.params.insert(local, ty);
            }
            params.push(ty);
        }
        // A constraint that is not worked out leaves the declaration as
        // well known as it was.
        let incomplete = self.incomplete;
        let outer = std::mem::replace(&mut self.in_type_params, true);
        for (param, &ty) in type_params.iter().zip(&params) {
            let constraint = self.typ(cx, param.bound);
            self.types.set_constraint(ty, constraint);
        }
        self.in_type_params = outer;
        self.incomplete = incomplete;
        params
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
        if self.reported.insert((objects[at].span, diagnostic.code)) {
            self.sink.push(diagnostic);
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
            self.holds_itself_not(ty, &mut Vec::new(), &mut Vec::new());
            next += 1;
        }
        self.to_validate.clear();
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
        match &*self.types.get(ty) {
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
                nest.push(ty);
                path.push(ty);
                let valid = self.holds_itself_not(declared_as, nest, path);
                nest.pop();
                path.pop();
                // What a type holds is the same wherever it is met, unless
                // it is written with a type parameter, which stands for
                // another type in each place.
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
        if !self.reported.insert((span, Code::InvalidRecursiveType)) {
            return;
        }
        if members.len() == 1 {
            let message = format!("invalid recursive type: {name} refers to itself");
            let diagnostic = Diagnostic::new(Code::InvalidRecursiveTypeSelf, span, message);
            return self.sink.push(diagnostic);
        }
        let message = format!("invalid recursive type {name}");
        let mut diagnostic = Diagnostic::new(Code::InvalidRecursiveType, span, message);
        for (step, &(from, at)) in members.iter().enumerate() {
            let (to, _) = members[(step + 1) % members.len()];
            let note = format!("{from} refers to {to}");
            diagnostic = diagnostic.with_note(Code::NoteRefersTo, Some(at), note);
        }
        self.sink.push(diagnostic);
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
                    Mode::Const(value) if value != Value::Unknown => {
                        (self.ty[object], self.value[object]) = (x.ty, value);
                    }
                    _ => self.incomplete = true,
                }
            }
            Form::Var { ty: Some(ty), .. } => {
                let cx = self.cx(&decl, None);
                self.ty[object] = self.typ(&cx, ty);
            }
            // The type of a variable without one is the type of its value.
            Form::Var { ty: None, value } => {
                let cx = self.cx(&decl, None);
                let ty = value.and_then(|(value, position, count)| {
                    self.result_type(&cx, value, position, count)
                });
                match ty {
                    Some(ty) => self.ty[object] = self.types.default_type(ty),
                    None => self.incomplete = true,
                }
            }
            Form::Type {
                alias,
                type_params,
                ty,
            } => {
                let mut cx = self.cx(&decl, None);
                let params = self.declare_params(&mut cx, type_params);
                if alias {
                    self.ty[object] = self.typ(&cx, ty);
                    return false;
                }
                // The type exists before what it is declared as is known,
                // so that it can name itself.
                let named = self
                    .types
                    .declare(self.package.objects[object].name, self.name);
                self.ty[object] = self.types.named(named, &[]);
                self.written(self.ty[object], self.package.objects[object].span);
                self.types.set_type_params(named, params);
                let underlying = self.typ(&cx, ty);
                if !self.incomplete {
                    self.types.set_underlying(named, underlying);
                }
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

    /// The type of one result of the expression `expr`, which gives
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
    fn object_type(&mut self, object: ObjId, as_type: bool) -> TypeId {
        self.resolve(object.0 as usize, as_type);
        self.ty[object.0 as usize]
    }

    /// What is being declared needs the object `object` for its value.
    fn need(&mut self, object: usize) {
        let kind = &self.package.objects[object].kind;
        if !matches!(kind, ObjKind::Const | ObjKind::Var | ObjKind::Func) {
            return;
        }
        for at in 0..self.current.len() {
            let from = self.current[at];
            if self.needed.insert((from, object)) {
                self.needs[from].push(object);
            }
        }
    }

    /// Declares the methods of the file for their types.
    fn methods(&mut self, file: usize, unit: Unit<'a>, with_signatures: bool) {
        let tree = unit.file;
        for (index, &decl) in tree.decls.iter().enumerate() {
            let DeclKind::Func {
                receiver: Some(receiver),
                name,
                ty,
                ..
            } = &tree.decl_nodes[decl].kind
            else {
                continue;
            };
            let body = &self.package.files[file].bodies[index];
            // What was incomplete in the method before is not in this one.
            self.incomplete = false;
            let decl = Decl {
                unit,
                body,
                form: Form::Func {
                    type_params: &[],
                    ty: *ty,
                },
            };
            let mut cx = self.cx(&decl, None);
            // `func (r *T[P]) m()`: the base type, whether the receiver is
            // a pointer, and the type parameters that the brackets name.
            let mut base = receiver.ty;
            let mut pointer_receiver = false;
            let mut receiver_params = Vec::new();
            loop {
                match &tree.exprs[base].kind {
                    ExprKind::Paren(x) => base = *x,
                    ExprKind::Unary {
                        op: UnaryOp::Star,
                        x,
                    } => {
                        pointer_receiver = true;
                        base = *x;
                    }
                    ExprKind::Index { x, indices } => {
                        for index in indices {
                            let span = tree.exprs[*index].span;
                            let ExprKind::Name(name) = tree.exprs[*index].kind else {
                                continue;
                            };
                            let param = self.types.param(name);
                            receiver_params.push(param);
                            if let Some(&local) = body.defs.get(&span.lo) {
                                cx.params.insert(local, param);
                            }
                        }
                        base = *x;
                    }
                    _ => break,
                }
            }
            let Some(&Ref::Object(object)) = body.uses.get(&base) else {
                continue;
            };
            // The base type may be named through an alias of a pointer.
            let mut receiver_type = self.object_type(object, true);
            if let Type::Pointer(elem) = &*self.types.get(receiver_type) {
                (receiver_type, pointer_receiver) = (*elem, true);
            }
            let Type::Named { id: named, .. } = &*self.types.get(receiver_type) else {
                continue;
            };
            let sig = match with_signatures {
                true => self.typ(&cx, *ty),
                false => Types::INVALID,
            };
            let method = Method {
                name: name.name,
                package: self.package_of(name.name),
                sig,
            };
            self.types.add_method(
                *named,
                DeclaredMethod {
                    method,
                    pointer_receiver,
                    receiver_params,
                },
            );
        }
        // A method names no object, so nothing waits for what was
        // incomplete in it.
        self.incomplete = false;
    }

    /// The package of a field or method name: a name that is not exported
    /// belongs to its package.
    fn package_of(&self, name: Spur) -> Option<Spur> {
        (!is_exported(self.names.resolve(&name))).then_some(self.path)
    }

    // ---- types

    fn predeclared(&mut self, name: Predeclared) -> TypeId {
        use crate::types::Marker;
        let basic = match name {
            Predeclared::Any => return self.types.any(),
            Predeclared::Error => return self.types.error(),
            Predeclared::Comparable => return self.types.intern(Type::Marker(Marker::Comparable)),
            Predeclared::Copy => return self.types.intern(Type::Marker(Marker::Copy)),
            Predeclared::Clone => return self.types.intern(Type::Marker(Marker::Clone)),
            Predeclared::Send => return self.types.intern(Type::Marker(Marker::Send)),
            Predeclared::Sync => return self.types.intern(Type::Marker(Marker::Sync)),
            Predeclared::Bool => Basic::Bool,
            Predeclared::Byte | Predeclared::Uint8 => Basic::Uint8,
            Predeclared::Complex64 => Basic::Complex64,
            Predeclared::Complex128 => Basic::Complex128,
            Predeclared::Float32 => Basic::Float32,
            Predeclared::Float64 => Basic::Float64,
            Predeclared::Int => Basic::Int,
            Predeclared::Int8 => Basic::Int8,
            Predeclared::Int16 => Basic::Int16,
            Predeclared::Int32 | Predeclared::Rune => Basic::Int32,
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
                        let kind = cx.body.locals[local.0 as usize].kind;
                        self.incomplete = true;
                        (kind == LocalKind::Type).then_some(Types::INVALID)
                    }
                }
            }),
            _ => None,
        }
    }

    /// The type that the expression `expr` writes. A generic type is
    /// written with its type arguments.
    fn typ(&mut self, cx: &Cx<'a>, expr: ExprId) -> TypeId {
        let ty = self.generic_or_type(cx, expr);
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
                self.incomplete = true;
                Types::INVALID
            }
            ExprKind::Paren(x) => self.typ(cx, *x),
            ExprKind::Unary {
                op: UnaryOp::Star,
                x,
            } => {
                let elem = self.typ(cx, *x);
                self.types.pointer(elem)
            }
            ExprKind::SliceType { elem } => {
                let elem = self.typ(cx, *elem);
                self.types.slice(elem)
            }
            ExprKind::ArrayType {
                len: Some(len),
                elem,
            } => {
                let elem = self.typ(cx, *elem);
                let len = self.constant(cx, *len).map(|(_, len)| len.to_int());
                match len.as_ref().and_then(Value::as_i64).map(u64::try_from) {
                    Some(Ok(len)) => self.types.array(len, elem),
                    _ => {
                        self.incomplete = true;
                        Types::INVALID
                    }
                }
            }
            ExprKind::MapType { key, value } => {
                let (key, value) = (self.typ(cx, *key), self.typ(cx, *value));
                self.types.map(key, value)
            }
            ExprKind::ChanType { dir, elem } => {
                let elem = self.typ(cx, *elem);
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
                    params.iter().map(|param| self.typ(cx, param.ty)).collect()
                };
                let (params, results) = (list(params), list(results));
                match fails {
                    true => self.types.failing_func(&params, &results, variadic),
                    false => self.types.func(&params, &results, variadic),
                }
            }
            // `...T` as the type of the last parameter: a slice.
            ExprKind::DotsType { elem } => {
                let elem = self.typ(cx, *elem);
                self.types.slice(elem)
            }
            ExprKind::StructType { fields } => {
                let mut all = Vec::new();
                for field in fields {
                    let ty = self.typ(cx, field.ty);
                    let literal = field.tag.map(|tag| tree.exprs[tag].span);
                    let tag = literal
                        .and_then(|span| unquote(&cx.text[span.lo as usize..span.hi as usize]));
                    let required = field.attrs.iter().any(|attr| {
                        let name = attr.path.name.span;
                        &cx.text[name.lo as usize..name.hi as usize] == "Required"
                    });
                    let embedded = field.names.is_empty();
                    let names: Vec<Spur> = match embedded {
                        true => self.embedded_name(tree, field.ty).into_iter().collect(),
                        false => field.names.iter().map(|name| name.name).collect(),
                    };
                    for name in names {
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
                for elem in elems {
                    match elem {
                        InterfaceElem::Method { name, ty } => methods.push(Method {
                            name: name.name,
                            package: self.package_of(name.name),
                            sig: self.typ(cx, *ty),
                        }),
                        InterfaceElem::Type(ty) => embedded.push(self.typ(cx, *ty)),
                    }
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
                self.types.intern(Type::Union(terms.into()))
            }
            ExprKind::Index { x, indices } => self.instance(cx, expr, *x, indices),
            _ => {
                self.incomplete = true;
                Types::INVALID
            }
        }
    }

    fn terms(&mut self, cx: &Cx<'a>, expr: ExprId, terms: &mut Vec<Term>) {
        match cx.tree.exprs[expr].kind {
            ExprKind::Binary {
                op: BinaryOp::Or,
                x,
                y,
            } => {
                self.terms(cx, x, terms);
                self.terms(cx, y, terms);
            }
            ExprKind::Unary {
                op: UnaryOp::Tilde,
                x,
            } => {
                let ty = self.typ(cx, x);
                terms.push(Term { tilde: true, ty });
            }
            _ => {
                let ty = self.typ(cx, expr);
                terms.push(Term { tilde: false, ty });
            }
        }
    }

    /// The name of an embedded field: that of its type.
    fn embedded_name(&self, tree: &File, mut ty: ExprId) -> Option<Spur> {
        loop {
            match &tree.exprs[ty].kind {
                ExprKind::Name(name) => return Some(*name),
                ExprKind::Selector { sel, .. } => return Some(sel.name),
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
        let arguments = |this: &mut Self| -> Vec<TypeId> {
            indices.iter().map(|&index| this.typ(cx, index)).collect()
        };
        if let Some(make) = make {
            let made = match arguments(self)[..] {
                [arg] => match make(arg) {
                    Type::Option(_) => Some(self.types.option(arg)),
                    _ => self.types.handle(make, arg).ok(),
                },
                _ => None,
            };
            return made.unwrap_or_else(|| {
                self.incomplete = true;
                Types::INVALID
            });
        }
        // An alias with type parameters of its own is not followed yet.
        let generic_alias = match cx.body.uses.get(&x) {
            Some(Ref::Object(object)) => matches!(
                self.decls[object.0 as usize].map(|decl| decl.form),
                Some(Form::Type { alias: true, type_params, .. }) if !type_params.is_empty()
            ),
            _ => false,
        };
        if generic_alias {
            self.incomplete = true;
            return Types::INVALID;
        }
        // The arguments of what is no generic type are not looked at.
        let generic = self.generic_or_type(cx, x);
        if let Type::Named { id, .. } = &*self.types.get(generic)
            && self.is_generic_type(generic)
        {
            let instance = self.types.named(*id, &arguments(self));
            self.written(instance, cx.tree.exprs[x].span);
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
        let params = self.types.type_params(*id);
        let mut list = String::new();
        for (at, &param) in params.iter().enumerate() {
            // Parameters with one constraint are written together.
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
        format!("{}[{list}]", self.type_string(ty))
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
                self.untyped(basic, Value::from_literal(text, *kind))
            }
            ExprKind::Paren(x) => {
                let inner = self.operand(cx, *x);
                self.instantiated(cx, *x, inner)
            }
            ExprKind::Name(_) => self.name(cx, expr),
            ExprKind::Selector { x, sel } => self.selector(cx, expr, *x, *sel),
            ExprKind::Unary { op, x } => self.unary(cx, expr, *op, *x),
            ExprKind::Binary { op, x, y } => self.binary(cx, expr, *op, *x, *y),
            ExprKind::Call {
                fun,
                args,
                spread,
                unwrap,
            } => {
                let called = self.call(cx, expr, *fun, args, *spread);
                match unwrap {
                    true => self.unwrapped(called),
                    false => called,
                }
            }
            ExprKind::FuncLit { ty, body } => {
                let sig = self.typ(cx, *ty);
                // Its body is walked with the body it is in.
                if self.memo.is_some() {
                    self.signature_locals(cx, *ty);
                    self.function_body(cx, *ty, sig, &body.stmts);
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
                if self
                    .asserted(cx, *x, "invalid operation: ", "type assertion")
                    .is_none()
                {
                    return Operand::UNKNOWN;
                }
                match self.typ(cx, *ty) {
                    Types::INVALID => Operand::UNKNOWN,
                    ty => Operand {
                        mode: Mode::CommaOk,
                        ty,
                    },
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
            let message = format!("{prefix}{shown} is not an interface");
            (Code::NotAnInterface, message)
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
            return Operand::UNKNOWN;
        };
        match denotes {
            Ref::Universe(Predeclared::True) => self.untyped(Basic::UntypedBool, Value::Bool(true)),
            Ref::Universe(Predeclared::False) => {
                self.untyped(Basic::UntypedBool, Value::Bool(false))
            }
            Ref::Universe(Predeclared::Iota) => match cx.iota {
                Some(iota) => self.untyped(Basic::UntypedInt, Value::Int64(iota)),
                None => Operand::UNKNOWN,
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
                self.object_operand(kind, ty, self.value[object.0 as usize].clone())
            }
            // A local of the function literal that the expression is in.
            Ref::Local(local) => match cx.params.get(&local) {
                Some(&ty) => Operand {
                    mode: Mode::Type,
                    ty,
                },
                None => self.locals.get(&local).cloned().unwrap_or(Operand::UNKNOWN),
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
            ObjKind::Const if value != Value::Unknown => Operand::constant(ty, value),
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
        if let Some(&Ref::Member { import, member }) = cx.body.uses.get(&expr) {
            let ObjKind::Import { package, .. } = &self.package.object(import).kind else {
                return Operand::UNKNOWN;
            };
            let object = package.object(member);
            // Package unsafe is built into the compiler: its functions take
            // any type.
            if package.path == "unsafe" && matches!(object.kind, ObjKind::Func) {
                return Operand {
                    mode: Mode::Unsafe(object.name),
                    ty: Types::INVALID,
                };
            }
            return self.object_operand(&object.kind, object.ty, object.value.clone());
        }
        let base = x;
        let x = self.operand(cx, base);
        let x = self.instantiated(cx, base, x);
        if x.mode == Mode::Unknown {
            return Operand::UNKNOWN;
        }
        let found = self
            .types
            .lookup(x.ty, sel.name, self.path, self.names, false);
        let found = match found {
            Ok(found) => Some(found.member),
            Err(why) => {
                self.no_member(cx, base, &x, sel, why);
                None
            }
        };
        match (&x.mode, found) {
            // A field of a variable, or one behind a pointer, is a
            // variable.
            (mode, Some(Member::Field { ty, indirect })) if x.is_value() => {
                match *mode == Mode::Variable || indirect {
                    true => Operand::variable(ty),
                    false => Operand::value(ty),
                }
            }
            (
                mode,
                Some(Member::Method {
                    sig,
                    pointer_receiver,
                }),
            ) if x.is_value() => {
                // A method that needs the address of its receiver is
                // called on what has one.
                if pointer_receiver && *mode != Mode::Variable && !self.own_rules(cx) {
                    let message = format!(
                        "cannot call pointer method {} on {}",
                        self.names.resolve(&sel.name),
                        self.type_string(x.ty)
                    );
                    self.error(Code::MethodExprPointerReceiver, sel.span, message);
                    return Operand::UNKNOWN;
                }
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
        let unsettled = invalid(x.ty) || pointee_invalid || self.types.has_param(x.ty);
        if !known || unsettled || self.own_rules(cx) {
            return;
        }
        let (of, name) = (self.expr_string(cx, base), self.names.resolve(&sel.name));
        if why == crate::types::NotFound::Ambiguous {
            let message = format!("ambiguous selector {of}.{name}");
            return self.error(Code::AmbiguousSelector, sel.span, message);
        }
        let shown = self.type_string(x.ty);
        let pointer_to_interface = matches!(
            &*self.types.get(self.types.underlying(x.ty)),
            Type::Pointer(elem) if self.types.is_interface(*elem)
        );
        // A member whose name differs in its capitals only is the likely
        // one.
        let near = self
            .types
            .lookup(x.ty, sel.name, self.path, self.names, true);
        let reason = match near {
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
        self.error(Code::UndefinedFieldOrMethod, sel.span, message);
    }

    /// The type of the composite literal `ty{elems}`. `[...]T{…}` has as
    /// many elements as the literal says.
    fn literal_type(&mut self, cx: &Cx<'a>, ty: ExprId, elems: &[ExprId]) -> TypeId {
        let ExprKind::ArrayType { len: None, elem } = cx.tree.exprs[ty].kind else {
            return self.typ(cx, ty);
        };
        let elem = self.typ(cx, elem);
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
    fn larger_kind(&self, x: TypeId, y: TypeId) -> Option<TypeId> {
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

    /// The result of `f!(x)`: the results of `f(x)` without the error.
    fn unwrapped(&self, called: Operand) -> Operand {
        let Type::Tuple(results) = &*self.types.get(called.ty) else {
            return match called.ty == self.types.error() {
                true => Operand {
                    mode: Mode::NoValue,
                    ty: Types::INVALID,
                },
                false => called,
            };
        };
        match &results[..] {
            [rest @ .., last] if *last == self.types.error() => match rest {
                [one] => Operand::value(*one),
                many => Operand::value(self.types.intern(Type::Tuple(many.into()))),
            },
            _ => called,
        }
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
        let callee = self.operand(cx, fun);
        match callee.mode {
            Mode::Type => self.conversion(cx, expr, callee.ty, args),
            Mode::Builtin(_) | Mode::Unsafe(_) => {
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
                let generic = self.types.generic_func(callee.ty).is_some();
                // The type arguments of a generic function are found from
                // the arguments of the call.
                let sig = match self.types.generic_func(callee.ty) {
                    Some(params) => {
                        // In this call the function has the type that its
                        // type arguments give it.
                        let sig = self.inferred(cx, callee.ty, &params, args, spread);
                        if let Some(memo) = &mut self.memo {
                            memo.insert(fun, sig.map_or(Operand::UNKNOWN, Operand::value));
                        }
                        match sig {
                            Some(sig) => sig,
                            None => return Operand::UNKNOWN,
                        }
                    }
                    None => self.types.underlying(callee.ty),
                };
                let Type::Func {
                    params,
                    results,
                    variadic,
                    ..
                } = &*self.types.get(sig)
                else {
                    return Operand::UNKNOWN;
                };
                match generic {
                    // What the type arguments make of the parameters is
                    // not checked against the arguments yet: only a value
                    // of no type takes the type of its parameter.
                    true => {
                        for (at, &arg) in args.iter().enumerate() {
                            let in_rest = *variadic && at + 1 >= params.len() && !spread;
                            let wanted = match (in_rest, params.last()) {
                                (true, Some(&last)) => match &*self.types.get(last) {
                                    Type::Slice(elem) => Some(*elem),
                                    _ => None,
                                },
                                _ => params.get(at).copied(),
                            };
                            if let Some(wanted) = wanted {
                                self.convert(cx, arg, wanted);
                            }
                        }
                    }
                    false => self.arguments(cx, expr, fun, (params, *variadic), args, spread),
                }
                match &results[..] {
                    [] => Operand {
                        mode: Mode::NoValue,
                        ty: Types::INVALID,
                    },
                    [one] => Operand::value(*one),
                    many => Operand::value(self.types.intern(Type::Tuple(many.into()))),
                }
            }
            _ => Operand::UNKNOWN,
        }
    }

    /// The type of the generic function `sig` for the call with `args`:
    /// each type parameter is what the arguments make it. `None` if an
    /// argument does not fit or a parameter stays open.
    fn inferred(
        &mut self,
        cx: &Cx<'a>,
        sig: TypeId,
        params: &[TypeId],
        args: &[ExprId],
        spread: bool,
    ) -> Option<TypeId> {
        let Type::Func {
            params: wanted,
            variadic,
            ..
        } = &*self.types.get(sig)
        else {
            return None;
        };
        // The types of the arguments: a call with several results is all
        // the arguments at once.
        let mut given: Vec<TypeId> = Vec::new();
        for &arg in args {
            let operand = self.operand(cx, arg);
            if !operand.is_value() {
                return None;
            }
            match &*self.types.get(operand.ty) {
                Type::Tuple(results) if args.len() == 1 => given.extend(results.iter()),
                _ => given.push(operand.ty),
            }
        }
        // The type that the parameter list has for the argument at `at`:
        // the element type for the arguments of `...T`, unless the call
        // passes a slice with `...`.
        let wanted_at = |at: usize| {
            let in_rest = *variadic && at + 1 >= wanted.len();
            match (in_rest && !(spread && at + 1 == given.len()), wanted.last()) {
                (true, Some(&last)) => match &*self.types.get(last) {
                    Type::Slice(elem) => Some(*elem),
                    _ => None,
                },
                _ => wanted.get(at.min(wanted.len().saturating_sub(1))).copied(),
            }
        };
        let nil = self.types.basic(Basic::UntypedNil);
        let mut bound = vec![None; params.len()];
        let mut untyped = Vec::new();
        for (at, &ty) in given.iter().enumerate() {
            let wanted = wanted_at(at)?;
            // `nil` and a generic function say nothing by themselves.
            if ty == nil || self.types.generic_func(ty).is_some() {
                continue;
            }
            match self.is_untyped(ty) {
                true => untyped.push((wanted, ty)),
                false if !self.types.unify(wanted, ty, params, &mut bound) => return None,
                false => {}
            }
        }
        // A parameter whose constraint allows one type says what the type
        // parameters in that type are: `S ~[]E` with `S` known gives `E`.
        for _ in 0..params.len() {
            for (at, &param) in params.iter().enumerate() {
                let (Some(known), Some(core)) = (bound[at], self.types.core_type(param)) else {
                    continue;
                };
                let known = match core.tilde {
                    true => self.types.underlying(known),
                    false => known,
                };
                if !self.types.unify(core.ty, known, params, &mut bound) {
                    return None;
                }
            }
        }
        // A constant of no type gives a parameter its default type, if no
        // other argument said what the parameter is.
        // Of several, the one of the latest kind: `f(1, 2.5)` is a float.
        let mut kinds: Vec<Option<TypeId>> = vec![None; params.len()];
        for (wanted, ty) in untyped {
            let at = params.iter().position(|&param| param == wanted);
            if let Some(at) = at.filter(|&at| bound[at].is_none()) {
                kinds[at] = match kinds[at] {
                    Some(kind) => Some(self.larger_kind(kind, ty)?),
                    None => Some(ty),
                };
            }
        }
        for (slot, kind) in bound.iter_mut().zip(kinds) {
            if let Some(kind) = kind {
                *slot = Some(self.types.default_type(kind));
            }
        }
        let bound: Vec<TypeId> = bound.into_iter().collect::<Option<_>>()?;
        Some(self.types.substitute(sig, params, &bound))
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
        let checked =
            self.builtin_arguments(cx, call, text, (count, false), args, (spread, as_written));
        let (Some(_), Some(&first)) = (checked, args.first()) else {
            return Operand::UNKNOWN;
        };
        let pointer = |this: &Self| {
            let object = this.package_unsafe_pointer();
            object.map_or(Operand::UNKNOWN, Operand::value)
        };
        match self.names.resolve(&name) {
            // The size and the alignment of the type of the argument are
            // constants, unless the type has a type parameter in it.
            name @ ("Sizeof" | "Alignof") => {
                let x = self.operand(cx, first);
                self.convert_default(cx, first);
                let ty = self.types.default_type(x.ty);
                let layout = Some(ty).filter(|_| x.is_value());
                match layout.and_then(|ty| self.types.layout(ty, self.names)) {
                    Some((size, align)) => {
                        let bytes = if name == "Sizeof" { size } else { align };
                        let value = Value::Int64(bytes as i64);
                        Operand::constant(self.types.basic(Basic::Uintptr), value)
                    }
                    // The size of a type parameter is known when the
                    // function is used; a type in error has none.
                    None if self.types.has_param(ty) => self.value_of(Basic::Uintptr),
                    None => Operand::UNKNOWN,
                }
            }
            // `Offsetof(x.f)`: where the field `f` starts in `x`.
            "Offsetof" => {
                let mut selector = first;
                while let ExprKind::Paren(x) = cx.tree.exprs[selector].kind {
                    selector = x;
                }
                self.operand(cx, first);
                let offset = match cx.tree.exprs[selector].kind {
                    ExprKind::Selector { x, sel } => {
                        let base = self.operand(cx, x);
                        Some(base.ty).filter(|_| base.is_value()).and_then(|ty| {
                            self.types.offset_of(ty, sel.name, self.path, self.names)
                        })
                    }
                    _ => None,
                };
                match offset {
                    Some(offset) => {
                        let value = Value::Int64(offset as i64);
                        Operand::constant(self.types.basic(Basic::Uintptr), value)
                    }
                    None => self.value_of(Basic::Uintptr),
                }
            }
            "Add" => pointer(self),
            "String" => self.value_of(Basic::String),
            "StringData" => {
                let byte = self.types.basic(Basic::Uint8);
                Operand::value(self.types.pointer(byte))
            }
            "Slice" => {
                let x = self.operand(cx, first);
                match &*self.types.get(self.types.underlying(x.ty)) {
                    Type::Pointer(elem) if x.is_value() => Operand::value(self.types.slice(*elem)),
                    _ => Operand::UNKNOWN,
                }
            }
            "SliceData" => {
                let x = self.operand(cx, first);
                match &*self.types.get(self.types.underlying(x.ty)) {
                    Type::Slice(elem) if x.is_value() => Operand::value(self.types.pointer(*elem)),
                    _ => Operand::UNKNOWN,
                }
            }
            _ => Operand::UNKNOWN,
        }
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
    fn bodies(&mut self, file: usize, unit: Unit<'a>) -> Vec<BodyTypes> {
        let tree = unit.file;
        let mut all = Vec::new();
        // How many values a constant without values of its own repeats,
        // in its group.
        let mut repeated_values: Option<(Option<u32>, usize)> = None;
        for (index, &decl) in tree.decls.iter().enumerate() {
            let body = &self.package.files[file].bodies[index];
            let form = Form::Var {
                ty: None,
                value: None,
            };
            let mut cx = self.cx(&Decl { unit, body, form }, None);
            self.locals.clear();
            self.local_groups.clear();
            self.shifted.clear();
            self.memo = Some(FxHashMap::default());
            self.current.clear();
            match &tree.decl_nodes[decl].kind {
                DeclKind::Func {
                    receiver,
                    name,
                    type_params,
                    ty,
                    body: Some(block),
                    ..
                } => {
                    // What the body uses, the function needs; a method
                    // and `init` are no objects.
                    let function = self.by_span.get(&name.span).filter(|_| receiver.is_none());
                    self.current.extend(function);
                    self.declare_params(&mut cx, type_params);
                    if let Some(receiver) = receiver {
                        self.receiver_params(&mut cx, receiver.ty);
                        self.params(&cx, std::slice::from_ref(receiver));
                    }
                    self.signature_locals(&cx, *ty);
                    let sig = self.typ(&cx, *ty);
                    self.function_body(&cx, *ty, sig, &block.stmts);
                }
                // The value of a constant is worked out with its
                // declaration; variables are walked here too, for the
                // function literals in their values.
                DeclKind::Var { names, ty, values } => {
                    let declared = ty.map(|ty| self.typ(&cx, ty));
                    self.arity(&cx, names, values, None);
                    let objects = |this: &Self, names: &[Ident]| -> Vec<usize> {
                        let object = |name: &Ident| this.by_span.get(&name.span).copied();
                        names.iter().filter_map(object).collect()
                    };
                    // `a, b = f()` gives both the one value.
                    if values.len() == 1 && names.len() > 1 {
                        self.current = objects(self, names);
                        self.var_decl(&cx, names.len(), declared, values);
                    } else {
                        for (position, &value) in values.iter().enumerate() {
                            let declared_here = names.get(position..=position).unwrap_or(&[]);
                            self.current = objects(self, declared_here);
                            match declared_here.is_empty() {
                                true => drop(self.operand(&cx, value)),
                                false => drop(self.var_decl(&cx, 1, declared, &[value])),
                            }
                        }
                    }
                }
                DeclKind::Const { names, ty, values } => {
                    let group = tree.decl_nodes[decl].group;
                    let repeated = match (ty.is_none() && values.is_empty(), repeated_values) {
                        (true, Some((last, count))) if last == group && group.is_some() => count,
                        (true, _) => 0,
                        (false, _) => {
                            repeated_values = Some((group, values.len()));
                            values.len()
                        }
                    };
                    self.arity(&cx, names, values, Some(repeated));
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
            let memo = self.memo.take().unwrap_or_default();
            let exprs = memo.into_iter().filter_map(typed).collect();
            self.validate();
            let locals = self
                .locals
                .drain()
                .map(|(local, operand)| (local, operand.ty));
            all.push((exprs, locals.collect(), std::mem::take(&mut self.excused)));
        }
        self.incomplete = false;
        all
    }

    /// Walks the statements of a function of the type `sig`.
    fn function_body(&mut self, cx: &Cx<'a>, ty: ExprId, sig: TypeId, stmts: &[StmtId]) {
        let named = match &cx.tree.exprs[ty].kind {
            ExprKind::FuncType { results, .. } => {
                results.first().is_some_and(|result| result.name.is_some())
            }
            _ => false,
        };
        // The results of a signature that is in error are not known.
        let results = match &*self.types.get(sig) {
            Type::Func { results, .. } => Some((results.to_vec(), named)),
            _ => None,
        };
        self.results.push(results);
        self.stmts(cx, stmts);
        self.results.pop();
    }

    /// Declares the type parameters that the receiver `T[P, Q]` names.
    fn receiver_params(&mut self, cx: &mut Cx<'a>, mut base: ExprId) {
        let tree = cx.tree;
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
                            cx.params.insert(local, self.types.param(*name));
                        }
                    }
                    return;
                }
                _ => return,
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
        for &stmt in stmts {
            gopp_syntax::with_stack(|| self.stmt(cx, stmt));
        }
    }

    fn opt_stmt(&mut self, cx: &Cx<'a>, stmt: Option<StmtId>) {
        if let Some(stmt) = stmt {
            self.stmt(cx, stmt);
        }
    }

    fn stmt(&mut self, cx: &Cx<'a>, id: StmtId) {
        let tree = cx.tree;
        match &tree.stmts[id].kind {
            StmtKind::Bad | StmtKind::Empty | StmtKind::Branch { .. } => {}
            StmtKind::Labeled { stmt, .. } => self.stmt(cx, *stmt),
            StmtKind::Block(block) => self.stmts(cx, &block.stmts),
            StmtKind::Expr(x) => self.expr_stmt(cx, *x),
            StmtKind::IncDec { x, .. } | StmtKind::Go(x) | StmtKind::Defer(x) => {
                self.operand(cx, *x);
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
                Some((wanted, named)) if !(results.is_empty() && named) => {
                    let wanted: Vec<Option<TypeId>> = wanted.into_iter().map(Some).collect();
                    let whole = Init::Return(tree.stmts[id].span);
                    self.init_vars(cx, &wanted, results, whole);
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
                    places.push(match local.is_some() || is_blank {
                        true => None,
                        false => Some(self.operand(cx, place).ty),
                    });
                }
                let types = self.init_vars(cx, &places, rhs, Init::Variables);
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
                self.opt_stmt(cx, *init);
                self.operand(cx, *cond);
                self.stmts(cx, &then.stmts);
                self.opt_stmt(cx, *otherwise);
            }
            StmtKind::For {
                init,
                cond,
                post,
                body,
            } => {
                self.opt_stmt(cx, *init);
                if let Some(cond) = cond {
                    self.operand(cx, *cond);
                }
                self.opt_stmt(cx, *post);
                self.stmts(cx, &body.stmts);
            }
            StmtKind::Range {
                lhs,
                define,
                x,
                body,
            } => {
                let ranged = self.operand(cx, *x);
                let (key, value) = self.range_types(&ranged);
                for (position, &target) in lhs.iter().enumerate() {
                    if !*define {
                        self.operand(cx, target);
                        continue;
                    }
                    // `&v` declares `v` as another name of the element.
                    let named = match tree.exprs[target].kind {
                        ExprKind::Unary {
                            op: UnaryOp::Addr,
                            x,
                        } => x,
                        _ => target,
                    };
                    let ty = if position == 0 { key } else { value };
                    let local = self.declared_local(cx, named);
                    self.set_local(local, ty);
                    // What cannot be ranged over gives its variables no
                    // type.
                    self.excused.extend(local.filter(|_| ty.is_none()));
                }
                self.stmts(cx, &body.stmts);
            }
            StmtKind::Switch { init, tag, clauses } => {
                self.opt_stmt(cx, *init);
                let guard = tag.map(|tag| &tree.exprs[tag].kind);
                let switched = match guard {
                    Some(&ExprKind::TypeSwitchGuard { x, .. }) => Some(
                        self.asserted(cx, x, "", "type switch")
                            .unwrap_or(Operand::UNKNOWN),
                    ),
                    _ => {
                        if let Some(tag) = tag {
                            self.operand(cx, *tag);
                        }
                        None
                    }
                };
                let vars = cx.body.switch_vars.get(&id);
                for (index, clause) in clauses.iter().enumerate() {
                    let cases = clause.cases.as_deref().unwrap_or(&[]);
                    let mut case_types = Vec::new();
                    for &case in cases {
                        match switched {
                            Some(_) => case_types.push(self.operand(cx, case)),
                            None => {
                                self.operand(cx, case);
                            }
                        }
                    }
                    // The variable of a type switch has the type of the
                    // case when the clause has one type, else the type of
                    // what is switched on.
                    if let (Some(switched), Some(&local)) =
                        (&switched, vars.and_then(|vars| vars.get(index)))
                    {
                        let ty = match &case_types[..] {
                            [one] if one.mode == Mode::Type => one.ty,
                            _ => switched.ty,
                        };
                        self.locals.insert(local, Operand::variable(ty));
                    }
                    self.stmts(cx, &clause.body);
                }
            }
            StmtKind::Select { clauses } => {
                for clause in clauses {
                    self.opt_stmt(cx, clause.comm);
                    self.stmts(cx, &clause.body);
                }
            }
        }
    }

    /// The types of the two variables of `for k, v := range x`.
    fn range_types(&self, x: &Operand) -> (Option<TypeId>, Option<TypeId>) {
        if !x.is_value() {
            return (None, None);
        }
        let int = Some(self.types.basic(Basic::Int));
        match &*self.types.get(self.types.underlying(x.ty)) {
            Type::Basic(Basic::String | Basic::UntypedString) => {
                (int, Some(self.types.basic(Basic::Int32)))
            }
            // `range 10` counts with the type of the number.
            Type::Basic(basic) if self.is_integer(x.ty) => match basic.is_untyped() {
                true => (Some(self.types.default_type(x.ty)), None),
                false => (Some(x.ty), None),
            },
            Type::Slice(elem) | Type::Array { elem, .. } => (int, Some(*elem)),
            Type::Pointer(array) => match &*self.types.get(self.types.underlying(*array)) {
                Type::Array { elem, .. } => (int, Some(*elem)),
                _ => (None, None),
            },
            Type::Map { key, value } => (Some(*key), Some(*value)),
            Type::Chan { elem, .. } => (Some(*elem), None),
            // An iterator: `func(yield func(K, V) bool)`.
            Type::Func { params, .. } => match params.first().map(|&yield_| self.types.get(yield_))
            {
                Some(yield_) => match &*yield_ {
                    Type::Func { params, .. } => (params.first().copied(), params.get(1).copied()),
                    _ => (None, None),
                },
                None => (None, None),
            },
            _ => (None, None),
        }
    }

    /// A `const`, `var` or `type` statement inside a function.
    fn local_decl(&mut self, cx: &Cx<'a>, decl: DeclId) {
        let tree = cx.tree;
        let node = &tree.decl_nodes[decl];
        let local = |name: Ident| cx.body.defs.get(&name.span.lo).copied();
        match &node.kind {
            DeclKind::Var { names, ty, values } => {
                let locals: Vec<_> = names.iter().map(|&name| local(name)).collect();
                let declared = ty.map(|ty| self.typ(cx, ty));
                self.arity(cx, names, values, None);
                let types = self.var_decl(cx, names.len(), declared, values);
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
                let Some(local) = local(*name) else {
                    return;
                };
                if *alias || !type_params.is_empty() {
                    let ty = self.typ(cx, *ty);
                    let mode = match type_params.is_empty() {
                        true => Mode::Type,
                        false => Mode::Unknown,
                    };
                    self.locals.insert(local, Operand { mode, ty });
                    return;
                }
                // The type exists before what it is declared as is known,
                // so that it can name itself.
                let named = self.types.declare(name.name, self.name);
                let declared = self.types.named(named, &[]);
                self.written(declared, name.span);
                self.locals.insert(
                    local,
                    Operand {
                        mode: Mode::Type,
                        ty: declared,
                    },
                );
                let underlying = self.typ(cx, *ty);
                self.types.set_underlying(named, underlying);
            }
            _ => {}
        }
    }
}
