//! The types of the package-level declarations and the values of the
//! constants.
//!
//! A declaration is worked out when it is first needed, so the order of
//! the source does not matter. What this module cannot work out yet (a
//! value that needs the type of an expression that is not constant) is
//! left invalid, which fits everywhere and is no error.

use gopp_diag::{Code, Diagnostic, Sink};
use gopp_span::Span;
use gopp_syntax::ast::{self, *};
use lasso::{Spur, ThreadedRodeo};
use rustc_hash::{FxHashMap, FxHashSet};

use crate::basic::{Basic, representable};
use crate::constant::{self, CompareOp, Kind, Value};
use crate::literal::unquote;
use crate::object::{Body, ExprType, LocalId, ObjId, ObjKind, Ref};
use crate::package::Package;
use crate::resolve::Unit;
use crate::types::{ChanDir, DeclaredMethod, Field, Member, Method, Term, Type, TypeId, Types};
use crate::universe::{Predeclared, PredeclaredKind};

/// Where an expression stands: its file, what its names denote, and the
/// type parameters in scope.
#[derive(Clone)]
struct Cx<'a> {
    tree: &'a File,
    body: &'a Body,
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
    Value,
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

    /// Whether the expression has a value: a constant or not.
    fn is_value(&self) -> bool {
        matches!(self.mode, Mode::Value | Mode::Const(_))
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
    /// Where an error was reported, so that none is reported twice.
    reported: FxHashSet<Span>,
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
    results: Vec<Vec<TypeId>>,
    /// While a body is walked: its groups of constants.
    local_groups: FxHashMap<u32, LocalGroup>,
}

/// What was found in one body: the type of each expression with a value,
/// and the type of each local.
type BodyTypes = (FxHashMap<ExprId, ExprType>, Vec<(LocalId, TypeId)>);

/// A group of constants inside a function: the index of its next constant,
/// and the type and values that a constant without a value repeats.
#[derive(Default)]
struct LocalGroup {
    next: i64,
    ty: Option<ExprId>,
    values: Vec<ExprId>,
}

/// Works out the type of every package-level object of `package` and the
/// value of every constant, and declares the methods of its types.
pub fn declarations(
    package: &mut Package,
    units: &[Unit<'_>],
    types: &Types,
    names: &ThreadedRodeo,
    sink: &Sink,
) {
    let count = package.objects.len();
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
        locals: FxHashMap::default(),
        memo: None,
        fresh: false,
        results: Vec::new(),
        local_groups: FxHashMap::default(),
    };
    for (file, unit) in units.iter().enumerate() {
        checker.collect(file, *unit);
    }
    // The methods first: the type of a variable may be the result of one.
    for (file, unit) in units.iter().enumerate() {
        checker.methods(file, *unit);
    }
    for object in 0..count {
        checker.resolve(object);
    }
    let types_of_files: Vec<_> = units
        .iter()
        .enumerate()
        .map(|(file, unit)| checker.bodies(file, *unit))
        .collect();
    let Checker { ty, value, .. } = checker;
    for ((object, ty), value) in package.objects.iter_mut().zip(ty).zip(value) {
        object.ty = ty;
        object.value = value;
    }
    for (file, types_of_bodies) in package.files.iter_mut().zip(types_of_files) {
        for (body, (exprs, locals)) in file.bodies.iter_mut().zip(types_of_bodies) {
            body.types = exprs;
            for (LocalId(local), ty) in locals {
                body.locals[local as usize].ty = ty;
            }
        }
    }
}

fn is_exported(name: &str) -> bool {
    name.starts_with(char::is_uppercase)
}

impl<'a> Checker<'a> {
    fn collect(&mut self, file: usize, unit: Unit<'a>) {
        let tree = unit.file;
        let bodies = &self.package.files[file].bodies;
        let by_span: FxHashMap<_, _> = self
            .package
            .objects
            .iter()
            .enumerate()
            .map(|(index, object)| (object.span, index))
            .collect();
        // The constant that a later one of its group repeats, and how many
        // constants each group has so far.
        let mut repeated: Option<Repeated<'a>> = None;
        let mut seen: FxHashMap<u32, i64> = FxHashMap::default();
        for (index, &decl) in tree.decls.iter().enumerate() {
            let node = &tree.decl_nodes[decl];
            let mut body = &bodies[index];
            let mut declare = |name: Ident, body: &'a Body, form: Form<'a>| {
                if let Some(&object) = by_span.get(&name.span) {
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
                    match repeated {
                        Some(last) if values.is_empty() && last.group == node.group => {
                            (ty, values, body) = (last.ty, last.values, last.body);
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
                        declare(name, body, Form::Const { ty, value, iota });
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
        for (param, &ty) in type_params.iter().zip(&params) {
            let constraint = self.typ(cx, param.bound);
            self.types.set_constraint(ty, constraint);
        }
        self.incomplete = incomplete;
        params
    }

    /// Works out the object, unless that is done or under way.
    fn resolve(&mut self, object: usize) {
        if self.state[object] != State::New {
            return;
        }
        self.state[object] = State::Busy;
        let outer = std::mem::replace(&mut self.incomplete, false);
        let defines_type = self.define(object);
        let incomplete = std::mem::replace(&mut self.incomplete, outer);
        // A declared type can be named before all of it is known; what
        // else is not fully known is not known at all.
        if incomplete && !defines_type {
            self.ty[object] = Types::INVALID;
            self.value[object] = Value::Unknown;
            self.incomplete = true;
        }
        self.state[object] = State::Done;
    }

    /// Works out one declaration; whether it declares a new type.
    fn define(&mut self, object: usize) -> bool {
        let Some(decl) = self.decls[object] else {
            return false;
        };
        match decl.form {
            Form::Const { ty, value, iota } => {
                let cx = self.cx(&decl, Some(iota));
                let declared = ty.map(|ty| self.typ(&cx, ty));
                let Some((own, value)) = value.and_then(|value| self.constant(&cx, value)) else {
                    self.incomplete = true;
                    return false;
                };
                let (ty, value) = match declared {
                    None => (own, Some(value)),
                    Some(ty) => (ty, self.converted(&value, ty)),
                };
                match value {
                    Some(value) => (self.ty[object], self.value[object]) = (ty, value),
                    None => self.incomplete = true,
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
                self.ty[object] = self.typ(&cx, ty);
                if !params.is_empty() {
                    self.types.set_generic_func(self.ty[object], params);
                }
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

    /// The type of the object `object` of this package.
    fn object_type(&mut self, object: ObjId) -> TypeId {
        self.resolve(object.0 as usize);
        self.ty[object.0 as usize]
    }

    /// Declares the methods of the file for their types.
    fn methods(&mut self, file: usize, unit: Unit<'a>) {
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
            let Type::Named { id: named, .. } = &*self.types.get(self.object_type(object)) else {
                continue;
            };
            let sig = self.typ(&cx, *ty);
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
                Some(self.object_type(object))
            }
            Ref::Member { import, member } => {
                let ObjKind::Import { package, .. } = &self.package.object(import).kind else {
                    return None;
                };
                let object = package.object(member);
                matches!(object.kind, ObjKind::Type).then_some(object.ty)
            }
            Ref::Local(local) => cx.params.get(&local).copied().or_else(|| {
                let declared = self.locals.get(&local)?;
                (declared.mode == Mode::Type).then_some(declared.ty)
            }),
            _ => None,
        }
    }

    /// The type that the expression `expr` writes.
    fn typ(&mut self, cx: &Cx<'a>, expr: ExprId) -> TypeId {
        gopp_syntax::with_stack(|| self.typ_node(cx, expr))
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
                    if self.reported.insert(node.span) {
                        let message = format!("{text} is not a type");
                        let diagnostic = Diagnostic::new(Code::NotAType, node.span, message);
                        self.sink.push(diagnostic);
                    }
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
            ExprKind::Index { x, indices } => self.instance(cx, *x, indices),
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
    fn instance(&mut self, cx: &Cx<'a>, x: ExprId, indices: &[ExprId]) -> TypeId {
        let args: Vec<TypeId> = indices.iter().map(|&index| self.typ(cx, index)).collect();
        let make: Option<fn(TypeId) -> Type> = match cx.body.uses.get(&x) {
            Some(Ref::Universe(Predeclared::Unique)) => Some(Type::Unique),
            Some(Ref::Universe(Predeclared::Shared)) => Some(Type::Shared),
            Some(Ref::Universe(Predeclared::Weak)) => Some(Type::Weak),
            Some(Ref::Universe(Predeclared::Local)) => Some(Type::Local),
            Some(Ref::Universe(Predeclared::Option)) => Some(Type::Option),
            _ => None,
        };
        if let (Some(make), &[arg]) = (make, &args[..]) {
            let made = match make(arg) {
                Type::Option(_) => Ok(self.types.option(arg)),
                _ => self.types.handle(make, arg),
            };
            return made.unwrap_or_else(|_| {
                self.incomplete = true;
                Types::INVALID
            });
        }
        let generic = self.typ(cx, x);
        match &*self.types.get(generic) {
            Type::Named { id, args: none } if none.is_empty() => self.types.named(*id, &args),
            _ => {
                self.incomplete = true;
                Types::INVALID
            }
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
            ExprKind::Paren(x) => self.operand(cx, *x),
            ExprKind::Name(_) => self.name(cx, expr),
            ExprKind::Selector { x, sel } => self.selector(cx, expr, *x, *sel),
            ExprKind::Unary { op, x } => self.unary(cx, *op, *x),
            ExprKind::Binary { op, x, y } => self.binary(cx, *op, *x, *y),
            ExprKind::Call {
                fun, args, spread, ..
            } => self.call(cx, *fun, args, *spread),
            ExprKind::FuncLit { ty, body } => {
                let sig = self.typ(cx, *ty);
                // Its body is walked with the body it is in.
                if self.memo.is_some() {
                    self.signature_locals(cx, *ty);
                    self.function_body(cx, sig, &body.stmts);
                }
                Operand::value(sig)
            }
            ExprKind::CompositeLit {
                ty: Some(ty),
                elems,
            } => {
                let literal = self.literal_type(cx, *ty, elems);
                if self.memo.is_some() {
                    self.elements(cx, literal, elems);
                }
                Operand::value(literal)
            }
            ExprKind::Index { x, indices } => self.index(cx, expr, *x, indices),
            ExprKind::Slice { x, lo, hi, max, .. } => {
                let x = self.operand(cx, *x);
                // A bound is an index: an `int` if it has no type.
                let int = self.types.basic(Basic::Int);
                for &bound in [lo, hi, max].into_iter().flatten() {
                    self.operand(cx, bound);
                    self.convert(cx, bound, int);
                }
                if !x.is_value() {
                    return Operand::UNKNOWN;
                }
                let sliced = match &*self.types.get(self.types.underlying(x.ty)) {
                    Type::Basic(Basic::String) => x.ty,
                    Type::Basic(Basic::UntypedString) => self.types.basic(Basic::String),
                    Type::Slice(_) => x.ty,
                    Type::Array { elem, .. } => self.types.slice(*elem),
                    Type::Pointer(array) => match &*self.types.get(self.types.underlying(*array)) {
                        Type::Array { elem, .. } => self.types.slice(*elem),
                        _ => return Operand::UNKNOWN,
                    },
                    _ => return Operand::UNKNOWN,
                };
                Operand::value(sliced)
            }
            ExprKind::Assert { x, ty } => {
                self.operand(cx, *x);
                Operand::value(self.typ(cx, *ty))
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

    fn name(&mut self, cx: &Cx<'a>, expr: ExprId) -> Operand {
        let Some(&denotes) = cx.body.uses.get(&expr) else {
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
                let ty = self.object_type(object);
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
            ObjKind::Var | ObjKind::Func => Operand::value(ty),
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
        let x = self.operand(cx, x);
        let found = self.types.member(x.ty, sel.name, self.path, self.names);
        match (&x.mode, found) {
            (Mode::Value, Some(Member::Field { ty })) => Operand::value(ty),
            (Mode::Value, Some(Member::Method { sig, .. })) => Operand::value(sig),
            // `T.m`: a function that takes the receiver first.
            (Mode::Type, Some(Member::Method { sig, .. })) => {
                let Type::Func {
                    params,
                    results,
                    variadic,
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

    /// Types the elements of a composite literal of the type `literal`.
    /// An element written without its type, `{1, 2}`, has the type that
    /// the literal gives its elements.
    fn elements(&mut self, cx: &Cx<'a>, literal: TypeId, elems: &[ExprId]) {
        let tree = cx.tree;
        let under = self.types.get(self.types.underlying(literal));
        // Whether keys are values, their type if they have one, and the
        // type of the elements.
        let (keys_are_values, key_type, elem_type) = match &*under {
            Type::Slice(elem) | Type::Array { elem, .. } => (true, None, Some(*elem)),
            Type::Map { key, value } => (true, Some(*key), Some(*value)),
            _ => (false, None, None),
        };
        for (position, &element) in elems.iter().enumerate() {
            let (key, value) = match tree.exprs[element].kind {
                ExprKind::KeyValue { key, value } => (Some(key), value),
                _ => (None, element),
            };
            // The type of the element: that of the field for a struct.
            let hint = match (&*under, key) {
                (Type::Struct(fields), None) => fields.get(position).map(|field| field.ty),
                (Type::Struct(_), Some(key)) => match tree.exprs[key].kind {
                    ExprKind::Name(name) => {
                        match self.types.member(literal, name, self.path, self.names) {
                            Some(Member::Field { ty }) => Some(ty),
                            _ => None,
                        }
                    }
                    _ => None,
                },
                _ => elem_type,
            };
            // The key of an array or a slice is an index.
            if let Some(key) = key.filter(|_| keys_are_values) {
                let int = self.types.basic(Basic::Int);
                self.element(cx, key, Some(key_type.unwrap_or(int)));
            }
            self.element(cx, value, hint);
        }
    }

    /// One element, or key, of a composite literal.
    fn element(&mut self, cx: &Cx<'a>, expr: ExprId, hint: Option<TypeId>) {
        let ExprKind::CompositeLit { ty: None, elems } = &cx.tree.exprs[expr].kind else {
            self.operand(cx, expr);
            if let Some(hint) = hint {
                self.convert(cx, expr, hint);
            }
            return;
        };
        let Some(hint) = hint else { return };
        // `{…}` for a pointer type is `&T{…}`.
        let literal = match &*self.types.get(self.types.underlying(hint)) {
            Type::Pointer(elem) => *elem,
            _ => hint,
        };
        self.elements(cx, literal, elems);
        if let Some(memo) = &mut self.memo {
            memo.insert(expr, Operand::value(hint));
        }
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

    fn index(&mut self, cx: &Cx<'a>, expr: ExprId, x: ExprId, indices: &[ExprId]) -> Operand {
        let x = self.operand(cx, x);
        match x.mode {
            // A generic type with its arguments.
            Mode::Type => Operand {
                mode: Mode::Type,
                ty: self.typ(cx, expr),
            },
            Mode::Value | Mode::Const(_) => {
                // An index of no type is an `int`; the key of a map has
                // the type of its keys.
                let wanted = match &*self.types.get(self.types.underlying(x.ty)) {
                    Type::Map { key, .. } => Some(*key),
                    Type::Func { .. } => None,
                    _ => Some(self.types.basic(Basic::Int)),
                };
                for &index in indices.iter().filter(|_| wanted.is_some()) {
                    self.operand(cx, index);
                    if let Some(wanted) = wanted {
                        self.convert(cx, index, wanted);
                    }
                }
                let elem = match &*self.types.get(self.types.underlying(x.ty)) {
                    Type::Basic(Basic::String | Basic::UntypedString) => {
                        self.types.basic(Basic::Uint8)
                    }
                    Type::Slice(elem) | Type::Array { elem, .. } => *elem,
                    Type::Map { value, .. } => *value,
                    Type::Pointer(array) => match &*self.types.get(self.types.underlying(*array)) {
                        Type::Array { elem, .. } => *elem,
                        _ => return Operand::UNKNOWN,
                    },
                    // A generic function with its type arguments.
                    Type::Func { .. } => {
                        let Some(params) = self.types.generic_func(x.ty) else {
                            return Operand::UNKNOWN;
                        };
                        let args: Vec<TypeId> = indices.iter().map(|&i| self.typ(cx, i)).collect();
                        if args.len() != params.len() {
                            return Operand::UNKNOWN;
                        }
                        self.types.substitute(x.ty, &params, &args)
                    }
                    _ => return Operand::UNKNOWN,
                };
                Operand::value(elem)
            }
            _ => Operand::UNKNOWN,
        }
    }

    fn is_untyped(&self, ty: TypeId) -> bool {
        self.types.is_untyped(ty)
    }

    /// The type that a value of the untyped kind `untyped` has where a
    /// value of the type `to` is wanted. `None` where it keeps its kind.
    fn target(&self, untyped: TypeId, to: TypeId) -> Option<TypeId> {
        if untyped == self.types.basic(Basic::UntypedNil) {
            return None;
        }
        match &*self.types.get(self.types.underlying(to)) {
            Type::Basic(Basic::UntypedNil) => None,
            Type::Basic(_) => Some(to),
            // An interface holds a value with a type of its own.
            Type::Interface { .. } => Some(self.types.default_type(untyped)),
            // A value is its own option (TYP-2).
            Type::Option(elem) => self.target(untyped, *elem),
            _ => None,
        }
    }

    /// Gives the expression `expr` the type `to`, if it has no type of its
    /// own yet: a constant, or a value made of constants of no type. Where
    /// the value is not a constant, its operands get the type too.
    fn convert(&mut self, cx: &Cx<'a>, expr: ExprId, to: TypeId) {
        self.retype(cx, expr, to, true);
    }

    /// `convert`; with `outermost`, a constant is also held as its type
    /// holds it. An expression in parentheses or under an operator keeps
    /// its exact value: only what is used as a value is rounded.
    fn retype(&mut self, cx: &Cx<'a>, expr: ExprId, to: TypeId, outermost: bool) {
        let Some(old) = self.memo.as_ref().and_then(|memo| memo.get(&expr)).cloned() else {
            return;
        };
        if !old.is_value() || !self.is_untyped(old.ty) {
            return;
        }
        let Some(ty) = self.target(old.ty, to) else {
            return;
        };
        let constant = matches!(old.mode, Mode::Const(_));
        match cx.tree.exprs[expr].kind {
            ExprKind::Paren(x) => self.retype(cx, x, to, false),
            ExprKind::Unary { x, .. } if !constant => self.retype(cx, x, to, false),
            ExprKind::Binary { op, x, y } if !constant => match op {
                // The result of a comparison says nothing of its operands.
                BinaryOp::Eq
                | BinaryOp::Ne
                | BinaryOp::Lt
                | BinaryOp::Le
                | BinaryOp::Gt
                | BinaryOp::Ge => {}
                BinaryOp::Shl | BinaryOp::Shr => self.retype(cx, x, to, false),
                _ => {
                    self.retype(cx, x, to, false);
                    self.retype(cx, y, to, false);
                }
            },
            _ => {}
        }
        // A constant is held as its type holds it.
        let fitted = match &old.mode {
            Mode::Const(value) if outermost && !self.is_untyped(ty) => self.converted(value, ty),
            _ => None,
        };
        if let Some(entry) = self.memo.as_mut().and_then(|memo| memo.get_mut(&expr)) {
            entry.ty = ty;
            if let Some(value) = fitted {
                entry.mode = Mode::Const(value);
            }
        }
    }

    /// Gives the expression `expr` the default type of its kind, if it has
    /// no type of its own yet.
    fn convert_default(&mut self, cx: &Cx<'a>, expr: ExprId) {
        let known = self.memo.as_ref().and_then(|memo| memo.get(&expr));
        if let Some(ty) = known
            .map(|operand| operand.ty)
            .filter(|&ty| self.is_untyped(ty))
        {
            self.convert(cx, expr, self.types.default_type(ty));
        }
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

    fn unary(&mut self, cx: &Cx<'a>, op: UnaryOp, x: ExprId) -> Operand {
        let operand = self.operand(cx, x);
        match (op, &operand.mode) {
            // `*T` is a type, `*p` what the pointer points to.
            (UnaryOp::Star, Mode::Type) => Operand {
                mode: Mode::Type,
                ty: self.types.pointer(operand.ty),
            },
            (UnaryOp::Star, Mode::Value) => {
                match &*self.types.get(self.types.underlying(operand.ty)) {
                    Type::Pointer(elem) => Operand::value(*elem),
                    _ => Operand::UNKNOWN,
                }
            }
            (UnaryOp::Addr, Mode::Value) => Operand::value(self.types.pointer(operand.ty)),
            (UnaryOp::Recv, Mode::Value) => {
                match &*self.types.get(self.types.underlying(operand.ty)) {
                    Type::Chan { elem, .. } => Operand::value(*elem),
                    _ => Operand::UNKNOWN,
                }
            }
            (UnaryOp::Plus | UnaryOp::Neg | UnaryOp::Not | UnaryOp::Xor, Mode::Value) => {
                Operand::value(operand.ty)
            }
            (_, Mode::Const(value)) => {
                let Some(basic) = self.basic_of(operand.ty) else {
                    return Operand::UNKNOWN;
                };
                let (op, bits) = match op {
                    UnaryOp::Plus => (constant::UnaryOp::Plus, 0),
                    UnaryOp::Neg => (constant::UnaryOp::Neg, 0),
                    UnaryOp::Not => (constant::UnaryOp::Not, 0),
                    // The complement of an unsigned number stays in its
                    // size.
                    UnaryOp::Xor => {
                        let bits = match basic {
                            Basic::Uint8 => 8,
                            Basic::Uint16 => 16,
                            Basic::Uint32 => 32,
                            Basic::Uint | Basic::Uint64 | Basic::Uintptr => 64,
                            _ => 0,
                        };
                        (constant::UnaryOp::Complement, bits)
                    }
                    _ => return Operand::UNKNOWN,
                };
                self.fitted(operand.ty, value.unary(op, bits))
            }
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

    fn binary(&mut self, cx: &Cx<'a>, op: BinaryOp, left: ExprId, right: ExprId) -> Operand {
        let (x, y) = (self.operand(cx, left), self.operand(cx, right));
        let comparison = matches!(
            op,
            BinaryOp::Eq | BinaryOp::Ne | BinaryOp::Lt | BinaryOp::Le | BinaryOp::Gt | BinaryOp::Ge
        );
        // An operand of no type takes the type of the other; of two such
        // numbers, the one of the smaller kind takes the larger kind.
        if !matches!(op, BinaryOp::Shl | BinaryOp::Shr) && x.is_value() && y.is_value() {
            match (self.is_untyped(x.ty), self.is_untyped(y.ty)) {
                (true, false) => self.convert(cx, left, y.ty),
                (false, true) => self.convert(cx, right, x.ty),
                (true, true) => {
                    if let Some(kind) = self.larger_kind(x.ty, y.ty) {
                        self.convert(cx, left, kind);
                        self.convert(cx, right, kind);
                    }
                }
                (false, false) => {}
            }
        }
        let (Mode::Const(a), Mode::Const(b)) = (&x.mode, &y.mode) else {
            if !x.is_value() || !y.is_value() {
                return Operand::UNKNOWN;
            }
            // A comparison of values is a boolean of no type yet; the
            // other results have the type of the operand that has one.
            return match (self.is_untyped(x.ty), self.is_untyped(y.ty)) {
                _ if comparison => self.value_of(Basic::UntypedBool),
                // A shift has the type of what is shifted; with a constant
                // of no type there, the type comes from around it.
                _ if matches!(op, BinaryOp::Shl | BinaryOp::Shr) => match self.is_untyped(x.ty) {
                    true => Operand::UNKNOWN,
                    false => Operand::value(x.ty),
                },
                (false, _) => Operand::value(x.ty),
                (true, false) => Operand::value(y.ty),
                (true, true) => match self.larger_kind(x.ty, y.ty) {
                    Some(ty) => Operand::value(ty),
                    None => Operand::UNKNOWN,
                },
            };
        };
        if matches!(op, BinaryOp::Shl | BinaryOp::Shr) {
            // The count fits an unsigned integer and is small; a float
            // that is an integer shifts as one.
            let count = b
                .to_int()
                .as_i64()
                .and_then(|count| u32::try_from(count).ok());
            let shifted = a.to_int();
            let ty = match self.is_integer(x.ty) {
                true => x.ty,
                false if self.is_untyped(x.ty) => self.types.basic(Basic::UntypedInt),
                false => return Operand::UNKNOWN,
            };
            return match count {
                Some(count) if count <= 1023 - 1 + 52 && shifted.kind() == Kind::Int => {
                    self.fitted(ty, shifted.shift(op == BinaryOp::Shl, count))
                }
                _ => Operand::UNKNOWN,
            };
        }
        // An untyped operand takes the type of the other; two typed ones
        // have the same type.
        let (mut a, mut b) = (a.clone(), b.clone());
        let ty = match (self.is_untyped(x.ty), self.is_untyped(y.ty)) {
            (true, true) => self.larger_kind(x.ty, y.ty),
            (true, false) => self.converted(&a, y.ty).map(|fit| {
                a = fit;
                y.ty
            }),
            (false, true) => self.converted(&b, x.ty).map(|fit| {
                b = fit;
                x.ty
            }),
            (false, false) => (x.ty == y.ty).then_some(x.ty),
        };
        let Some(ty) = ty else {
            return Operand::UNKNOWN;
        };
        let compare = match op {
            BinaryOp::Eq => Some(CompareOp::Eq),
            BinaryOp::Ne => Some(CompareOp::Ne),
            BinaryOp::Lt => Some(CompareOp::Lt),
            BinaryOp::Le => Some(CompareOp::Le),
            BinaryOp::Gt => Some(CompareOp::Gt),
            BinaryOp::Ge => Some(CompareOp::Ge),
            _ => None,
        };
        if let Some(compare) = compare {
            return self.untyped(Basic::UntypedBool, Value::Bool(a.compare(compare, &b)));
        }
        let op = match op {
            BinaryOp::OrOr => constant::BinaryOp::LogicalOr,
            BinaryOp::AndAnd => constant::BinaryOp::LogicalAnd,
            BinaryOp::Add => constant::BinaryOp::Add,
            BinaryOp::Sub => constant::BinaryOp::Sub,
            BinaryOp::Mul => constant::BinaryOp::Mul,
            // Two integers divide to an integer.
            BinaryOp::Div if self.is_integer(ty) => constant::BinaryOp::IntQuo,
            BinaryOp::Div => constant::BinaryOp::Quo,
            BinaryOp::Rem => constant::BinaryOp::Rem,
            BinaryOp::And => constant::BinaryOp::And,
            BinaryOp::Or => constant::BinaryOp::Or,
            BinaryOp::Xor => constant::BinaryOp::Xor,
            BinaryOp::AndNot => constant::BinaryOp::AndNot,
            _ => return Operand::UNKNOWN,
        };
        self.fitted(ty, a.binary(op, &b))
    }

    /// A call: of a function, a conversion, or a built-in function.
    fn call(&mut self, cx: &Cx<'a>, fun: ExprId, args: &[ExprId], spread: bool) -> Operand {
        let callee = self.operand(cx, fun);
        match callee.mode {
            Mode::Type => self.conversion(cx, callee.ty, args),
            Mode::Builtin(builtin) => self.builtin(cx, builtin, args, spread),
            Mode::Unsafe(name) => self.unsafe_call(cx, name, args),
            Mode::Value => {
                for &arg in args {
                    self.operand(cx, arg);
                }
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
                } = &*self.types.get(sig)
                else {
                    return Operand::UNKNOWN;
                };
                // An argument of no type takes the type of its parameter:
                // the element type for the arguments of `...T`.
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
        for (wanted, ty) in untyped {
            let at = params.iter().position(|&param| param == wanted);
            if let Some(at) = at.filter(|&at| bound[at].is_none()) {
                bound[at] = Some(self.types.default_type(ty));
            }
        }
        let bound: Vec<TypeId> = bound.into_iter().collect::<Option<_>>()?;
        Some(self.types.substitute(sig, params, &bound))
    }

    /// `T(x)`: a constant where `T` is a type of constants and holds `x`.
    fn conversion(&mut self, cx: &Cx<'a>, target: TypeId, args: &[ExprId]) -> Operand {
        let &[arg] = args else {
            return Operand::UNKNOWN;
        };
        let x = self.operand(cx, arg);
        // The argument takes the type it is converted to, if that is a
        // type of constants; an integer made a string keeps its kind.
        let to_string = self.basic_of(target) == Some(Basic::String) && self.is_integer(x.ty);
        match self.basic_of(target) {
            Some(_) if !to_string => self.convert(cx, arg, target),
            Some(_) => {}
            None => self.convert_default(cx, arg),
        }
        let (Mode::Const(value), Some(basic)) = (&x.mode, self.basic_of(target)) else {
            return match x.is_value() {
                true => Operand::value(target),
                false => Operand::UNKNOWN,
            };
        };
        if let Some(value) = representable(value, basic) {
            return match value {
                Value::Unknown => Operand::UNKNOWN,
                value => Operand::constant(target, value),
            };
        }
        // An integer converts to the string of that character.
        if basic == Basic::String && self.is_integer(x.ty) {
            let code = value.as_i64().and_then(|code| u32::try_from(code).ok());
            let c = code
                .and_then(char::from_u32)
                .unwrap_or(char::REPLACEMENT_CHARACTER);
            let value = Value::string(c.encode_utf8(&mut [0; 4]).as_bytes());
            return Operand::constant(target, value);
        }
        Operand::UNKNOWN
    }

    /// The type of `expr` as an argument that is a type: of `make`, `new`.
    fn type_argument(&mut self, cx: &Cx<'a>, expr: ExprId) -> Option<TypeId> {
        let operand = self.operand(cx, expr);
        matches!(operand.mode, Mode::Type).then_some(operand.ty)
    }

    fn builtin(
        &mut self,
        cx: &Cx<'a>,
        builtin: Predeclared,
        args: &[ExprId],
        spread: bool,
    ) -> Operand {
        let no_value = Operand {
            mode: Mode::NoValue,
            ty: Types::INVALID,
        };
        let Some(&first) = args.first() else {
            return match builtin {
                Predeclared::Recover => Operand::value(self.types.any()),
                Predeclared::Print | Predeclared::Println => no_value,
                _ => Operand::UNKNOWN,
            };
        };
        match builtin {
            Predeclared::New => match self.type_argument(cx, first) {
                Some(ty) => Operand::value(self.types.pointer(ty)),
                None => Operand::UNKNOWN,
            },
            Predeclared::Make => {
                // A size is an `int` if it has no type.
                let int = self.types.basic(Basic::Int);
                for &size in &args[1..] {
                    self.operand(cx, size);
                    self.convert(cx, size, int);
                }
                match self.type_argument(cx, first) {
                    Some(ty) => Operand::value(ty),
                    None => Operand::UNKNOWN,
                }
            }
            Predeclared::Delete
            | Predeclared::Clear
            | Predeclared::Close
            | Predeclared::Panic
            | Predeclared::Print
            | Predeclared::Println => no_value,
            Predeclared::CopyFunc => self.value_of(Basic::Int),
            Predeclared::Len | Predeclared::Cap => {
                let x = self.operand(cx, first);
                match &x.mode {
                    // The length of a constant string is a constant of
                    // type int.
                    Mode::Const(Value::Str(text)) if builtin == Predeclared::Len => {
                        let len = Value::Int64(text.bytes().len() as i64);
                        Operand::constant(self.types.basic(Basic::Int), len)
                    }
                    // The length of an array is constant if nothing is
                    // called or received to get the array.
                    Mode::Value if self.is_static(cx, first) => {
                        let array = match &*self.types.get(self.types.underlying(x.ty)) {
                            Type::Pointer(elem) => self.types.underlying(*elem),
                            _ => self.types.underlying(x.ty),
                        };
                        match &*self.types.get(array) {
                            Type::Array { len, .. } => {
                                let len = Value::Int64(*len as i64);
                                Operand::constant(self.types.basic(Basic::Int), len)
                            }
                            _ => self.value_of(Basic::Int),
                        }
                    }
                    Mode::Value | Mode::Const(_) => self.value_of(Basic::Int),
                    _ => Operand::UNKNOWN,
                }
            }
            Predeclared::Append => {
                let x = self.operand(cx, first);
                // What is appended has the type of the elements.
                if let Type::Slice(elem) = &*self.types.get(self.types.underlying(x.ty)) {
                    for &arg in &args[1..] {
                        self.operand(cx, arg);
                        // A slice or a string passed with `...` is no
                        // element.
                        if !spread {
                            self.convert(cx, arg, *elem);
                        }
                    }
                }
                match x.is_value() && !self.is_untyped(x.ty) {
                    true => Operand::value(x.ty),
                    false => Operand::UNKNOWN,
                }
            }
            Predeclared::Real | Predeclared::Imag => {
                let x = self.operand(cx, first);
                let part = match self.basic_of(x.ty) {
                    Some(Basic::Complex64) => Basic::Float32,
                    Some(Basic::Complex128) => Basic::Float64,
                    Some(basic) if basic.is_untyped() => Basic::UntypedFloat,
                    _ => return Operand::UNKNOWN,
                };
                match &x.mode {
                    Mode::Const(value) if part == Basic::UntypedFloat => {
                        let complex = value.to_complex();
                        let value = match builtin {
                            Predeclared::Real => complex.real(),
                            _ => complex.imag(),
                        };
                        self.fitted(self.types.basic(part), value)
                    }
                    Mode::Value => self.value_of(part),
                    _ => Operand::UNKNOWN,
                }
            }
            _ => Operand::UNKNOWN,
        }
    }

    /// Whether `expr` is evaluated without a call and without a receive
    /// from a channel.
    fn is_static(&self, cx: &Cx<'a>, expr: ExprId) -> bool {
        match &cx.tree.exprs[expr].kind {
            ExprKind::Name(_) | ExprKind::BasicLit { .. } => true,
            ExprKind::Paren(x) | ExprKind::Selector { x, .. } => self.is_static(cx, *x),
            ExprKind::Unary { op, x } => *op != UnaryOp::Recv && self.is_static(cx, *x),
            ExprKind::Index { x, indices } => {
                self.is_static(cx, *x) && indices.iter().all(|&index| self.is_static(cx, index))
            }
            ExprKind::CompositeLit { elems, .. } => elems.is_empty(),
            _ => false,
        }
    }

    /// A function of package unsafe, which takes a value of any type.
    fn unsafe_call(&mut self, cx: &Cx<'a>, name: Spur, args: &[ExprId]) -> Operand {
        let Some(&first) = args.first() else {
            return Operand::UNKNOWN;
        };
        let pointer = |this: &Self| {
            let object = this.package_unsafe_pointer(cx);
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
                    None => self.value_of(Basic::Uintptr),
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
    fn package_unsafe_pointer(&self, _cx: &Cx<'a>) -> Option<TypeId> {
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
        for (index, &decl) in tree.decls.iter().enumerate() {
            let body = &self.package.files[file].bodies[index];
            let form = Form::Var {
                ty: None,
                value: None,
            };
            let mut cx = self.cx(&Decl { unit, body, form }, None);
            self.locals.clear();
            self.local_groups.clear();
            self.memo = Some(FxHashMap::default());
            match &tree.decl_nodes[decl].kind {
                DeclKind::Func {
                    receiver,
                    type_params,
                    ty,
                    body: Some(block),
                    ..
                } => {
                    self.declare_params(&mut cx, type_params);
                    if let Some(receiver) = receiver {
                        self.receiver_params(&mut cx, receiver.ty);
                        self.params(&cx, std::slice::from_ref(receiver));
                    }
                    self.signature_locals(&cx, *ty);
                    let sig = self.typ(&cx, *ty);
                    self.function_body(&cx, sig, &block.stmts);
                }
                DeclKind::Var { ty, values, .. } | DeclKind::Const { ty, values, .. } => {
                    // The value of a constant is worked out with its
                    // declaration; here only variables are walked, for the
                    // function literals in their values.
                    if matches!(tree.decl_nodes[decl].kind, DeclKind::Var { .. }) {
                        let declared = ty.map(|ty| self.typ(&cx, ty));
                        for &value in values {
                            self.operand(&cx, value);
                            match declared {
                                Some(declared) => self.convert(&cx, value, declared),
                                None => self.convert_default(&cx, value),
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
                let value = match operand.mode {
                    Mode::Const(value) => value,
                    Mode::Value => Value::Unknown,
                    _ => return None,
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
            let locals = self
                .locals
                .drain()
                .map(|(local, operand)| (local, operand.ty));
            all.push((exprs, locals.collect()));
        }
        self.incomplete = false;
        all
    }

    /// Walks the statements of a function of the type `sig`.
    fn function_body(&mut self, cx: &Cx<'a>, sig: TypeId, stmts: &[StmtId]) {
        let results = match &*self.types.get(sig) {
            Type::Func { results, .. } => results.to_vec(),
            _ => Vec::new(),
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
                self.locals.insert(local, Operand::value(ty));
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
                Operand::value(self.types.default_type(ty))
            });
            self.locals.insert(local, operand);
        }
    }

    /// `lhs := rhs`, or `var lhs = rhs`: each new variable gets the type
    /// of its value.
    fn define_locals(&mut self, cx: &Cx<'a>, lhs: &[Option<LocalId>], rhs: &[ExprId]) {
        if rhs.len() == 1 && lhs.len() > 1 {
            for (position, &local) in lhs.iter().enumerate() {
                let ty = self.result_type(cx, rhs[0], position, lhs.len());
                self.set_local(local, ty);
            }
            return;
        }
        for (position, &local) in lhs.iter().enumerate() {
            let ty = rhs
                .get(position)
                .and_then(|&value| self.result_type(cx, value, 0, 1));
            self.set_local(local, ty);
            // A value of no type has its default type in the variable.
            if let Some(&value) = rhs.get(position) {
                self.convert_default(cx, value);
            }
        }
        for &extra in rhs.iter().skip(lhs.len()) {
            self.operand(cx, extra);
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
            StmtKind::Expr(x)
            | StmtKind::IncDec { x, .. }
            | StmtKind::Go(x)
            | StmtKind::Defer(x) => {
                self.operand(cx, *x);
            }
            StmtKind::Send { chan, value } => {
                let chan = self.operand(cx, *chan);
                self.operand(cx, *value);
                if let Type::Chan { elem, .. } = &*self.types.get(self.types.underlying(chan.ty)) {
                    self.convert(cx, *value, *elem);
                }
            }
            StmtKind::Return(results) => {
                for &result in results {
                    self.operand(cx, result);
                }
                // A value of no type takes the type of the result.
                let wanted = self.results.last().cloned().unwrap_or_default();
                if wanted.len() == results.len() {
                    for (&result, ty) in results.iter().zip(wanted) {
                        self.convert(cx, result, ty);
                    }
                }
            }
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
                self.define_locals(cx, &locals, rhs);
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
                }
                self.stmts(cx, &body.stmts);
            }
            StmtKind::Switch { init, tag, clauses } => {
                self.opt_stmt(cx, *init);
                let guard = tag.map(|tag| &tree.exprs[tag].kind);
                let switched = match guard {
                    Some(&ExprKind::TypeSwitchGuard { x, .. }) => Some(self.operand(cx, x)),
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
                        self.locals.insert(local, Operand::value(ty));
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
                true => (int, None),
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
                match ty {
                    Some(ty) => {
                        let ty = self.typ(cx, *ty);
                        for &value in values {
                            self.operand(cx, value);
                            self.convert(cx, value, ty);
                        }
                        for local in locals.into_iter().flatten() {
                            self.locals.insert(local, Operand::value(ty));
                        }
                    }
                    None => self.define_locals(cx, &locals, values),
                }
            }
            DeclKind::Const { names, ty, values } => {
                // A constant without a value repeats the one above it in
                // its group, with the next `iota`.
                let (mut ty, mut values, mut iota) = (*ty, values.clone(), 0);
                if let Some(group) = node.group {
                    let last = self.local_groups.entry(group).or_default();
                    if values.is_empty() {
                        (ty, values) = (last.ty, last.values.clone());
                    } else {
                        (last.ty, last.values) = (ty, values.clone());
                    }
                    iota = last.next;
                    last.next += 1;
                }
                let mut inner = cx.clone();
                inner.iota = Some(iota);
                let declared = ty.map(|ty| self.typ(&inner, ty));
                for (position, &name) in names.iter().enumerate() {
                    // A repeated value is worked out anew with its `iota`.
                    self.fresh = true;
                    let constant = values
                        .get(position)
                        .and_then(|&value| self.constant(&inner, value));
                    self.fresh = false;
                    let operand = match (constant, declared) {
                        (Some((own, value)), None) => Operand::constant(own, value),
                        (Some((_, value)), Some(ty)) => match self.converted(&value, ty) {
                            Some(value) => Operand::constant(ty, value),
                            None => Operand::UNKNOWN,
                        },
                        (None, _) => Operand::UNKNOWN,
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
