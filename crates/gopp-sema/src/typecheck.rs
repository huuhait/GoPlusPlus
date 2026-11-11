//! The types of the package-level declarations and the values of the
//! constants.
//!
//! A declaration is worked out when it is first needed, so the order of
//! the source does not matter. What this module cannot work out yet (a
//! value that needs the type of an expression that is not constant) is
//! left invalid, which fits everywhere and is no error.

use gopp_diag::{Code, Diagnostic, Sink};
use gopp_syntax::ast::{self, *};
use lasso::{Spur, ThreadedRodeo};
use rustc_hash::FxHashMap;

use crate::basic::{Basic, representable};
use crate::constant::{self, CompareOp, Kind, Value};
use crate::literal::unquote;
use crate::object::{Body, LocalId, ObjId, ObjKind, Ref};
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
    decls: Vec<Option<Decl<'a>>>,
    state: Vec<State>,
    ty: Vec<TypeId>,
    value: Vec<Value>,
    /// Whether the declaration being worked out needed something that
    /// this module does not work out yet.
    incomplete: bool,
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
        decls: vec![None; count],
        state: vec![State::New; count],
        ty: vec![Types::INVALID; count],
        value: vec![Value::Unknown; count],
        incomplete: false,
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
    let Checker { ty, value, .. } = checker;
    for ((object, ty), value) in package.objects.iter_mut().zip(ty).zip(value) {
        object.ty = ty;
        object.value = value;
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

    /// Gives each type parameter of a declaration a type.
    fn declare_params(&self, cx: &mut Cx<'a>, type_params: &[TypeParam]) -> Vec<TypeId> {
        let declare = |param: &TypeParam| {
            let ty = self.types.param(param.name.name);
            if let Some(&local) = cx.body.defs.get(&param.name.span.lo) {
                cx.params.insert(local, ty);
            }
            ty
        };
        type_params.iter().map(declare).collect()
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
            Ref::Local(local) => cx.params.get(&local).copied(),
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
                    let message = format!("{text} is not a type");
                    self.sink
                        .push(Diagnostic::new(Code::NotAType, node.span, message));
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
        gopp_syntax::with_stack(|| self.operand_node(cx, expr))
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
            ExprKind::Call { fun, args, .. } => self.call(cx, *fun, args),
            ExprKind::FuncLit { ty, .. } => Operand::value(self.typ(cx, *ty)),
            ExprKind::CompositeLit {
                ty: Some(ty),
                elems,
            } => Operand::value(self.literal_type(cx, *ty, elems)),
            ExprKind::Index { x, indices } => self.index(cx, expr, *x, indices),
            ExprKind::Slice { x, .. } => {
                let x = self.operand(cx, *x);
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
            ExprKind::Assert { ty, .. } => Operand::value(self.typ(cx, *ty)),
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
                None => Operand::UNKNOWN,
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

    fn binary(&mut self, cx: &Cx<'a>, op: BinaryOp, x: ExprId, y: ExprId) -> Operand {
        let (x, y) = (self.operand(cx, x), self.operand(cx, y));
        let comparison = matches!(
            op,
            BinaryOp::Eq | BinaryOp::Ne | BinaryOp::Lt | BinaryOp::Le | BinaryOp::Gt | BinaryOp::Ge
        );
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
    fn call(&mut self, cx: &Cx<'a>, fun: ExprId, args: &[ExprId]) -> Operand {
        let callee = self.operand(cx, fun);
        match callee.mode {
            Mode::Type => self.conversion(cx, callee.ty, args),
            Mode::Builtin(builtin) => self.builtin(cx, builtin, args),
            Mode::Unsafe(name) => self.unsafe_call(cx, name, args),
            Mode::Value => {
                // The type arguments of a generic function are found from
                // the arguments of the call.
                let sig = match self.types.generic_func(callee.ty) {
                    Some(params) => match self.inferred(cx, callee.ty, &params, args) {
                        Some(sig) => sig,
                        None => return Operand::UNKNOWN,
                    },
                    None => self.types.underlying(callee.ty),
                };
                let Type::Func { results, .. } = &*self.types.get(sig) else {
                    return Operand::UNKNOWN;
                };
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
    ) -> Option<TypeId> {
        let Type::Func {
            params: wanted,
            variadic,
            ..
        } = &*self.types.get(sig)
        else {
            return None;
        };
        // The type that the parameter list has for the argument at `at`.
        let wanted_at = |at: usize| match (*variadic, at + 1 >= wanted.len()) {
            (true, true) => match &*self.types.get(*wanted.last()?) {
                Type::Slice(elem) => Some(*elem),
                _ => None,
            },
            _ => wanted.get(at).copied(),
        };
        let mut bound = vec![None; params.len()];
        let mut untyped = Vec::new();
        for (at, &arg) in args.iter().enumerate() {
            let (operand, wanted) = (self.operand(cx, arg), wanted_at(at)?);
            if !operand.is_value() {
                return None;
            }
            match self.is_untyped(operand.ty) {
                true => untyped.push((wanted, operand.ty)),
                false if !self.types.unify(wanted, operand.ty, params, &mut bound) => return None,
                false => {}
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

    fn builtin(&mut self, cx: &Cx<'a>, builtin: Predeclared, args: &[ExprId]) -> Operand {
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
            Predeclared::Make => match self.type_argument(cx, first) {
                Some(ty) => Operand::value(ty),
                None => Operand::UNKNOWN,
            },
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
            // A size is a constant, but one that needs the layout.
            "Sizeof" | "Alignof" | "Offsetof" => self.value_of(Basic::Uintptr),
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
}
