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
use crate::types::{ChanDir, DeclaredMethod, Field, Method, Term, Type, TypeId, Types};
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

/// A constant operand: its type, which may be untyped, and its value.
struct Operand {
    ty: TypeId,
    value: Value,
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
    for object in 0..count {
        checker.resolve(object);
    }
    for (file, unit) in units.iter().enumerate() {
        checker.methods(file, *unit);
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
                DeclKind::Var { names, ty, .. } => {
                    for &name in names {
                        declare(name, body, Form::Var { ty: *ty });
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
    fn declare_params(&self, cx: &mut Cx<'a>, type_params: &[TypeParam]) {
        for param in type_params {
            if let Some(&local) = cx.body.defs.get(&param.name.span.lo) {
                cx.params.insert(local, self.types.param(param.name.name));
            }
        }
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
                let Some(operand) = value.and_then(|value| self.constant(&cx, value)) else {
                    self.incomplete = true;
                    return false;
                };
                let (ty, value) = match declared {
                    None => (operand.ty, Some(operand.value)),
                    Some(ty) => (ty, self.converted(&operand.value, ty)),
                };
                match value {
                    Some(value) => (self.ty[object], self.value[object]) = (ty, value),
                    None => self.incomplete = true,
                }
            }
            // The type of a variable without one is the type of its value.
            Form::Var { ty: None } => self.incomplete = true,
            Form::Var { ty: Some(ty) } => {
                let cx = self.cx(&decl, None);
                self.ty[object] = self.typ(&cx, ty);
            }
            Form::Type {
                alias,
                type_params,
                ty,
            } => {
                let mut cx = self.cx(&decl, None);
                self.declare_params(&mut cx, type_params);
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
                let underlying = self.typ(&cx, ty);
                if !self.incomplete {
                    self.types.set_underlying(named, underlying);
                }
                return true;
            }
            Form::Func { type_params, ty } => {
                let mut cx = self.cx(&decl, None);
                self.declare_params(&mut cx, type_params);
                self.ty[object] = self.typ(&cx, ty);
            }
        }
        false
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
                            if let (ExprKind::Name(name), Some(&local)) =
                                (&tree.exprs[*index].kind, body.defs.get(&span.lo))
                            {
                                cx.params.insert(local, self.types.param(*name));
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
                let len = self.constant(cx, *len).map(|len| len.value.to_int());
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

    // ---- constants

    fn basic_of(&self, ty: TypeId) -> Option<Basic> {
        match &*self.types.get(self.types.underlying(ty)) {
            Type::Basic(basic) => Some(*basic),
            _ => None,
        }
    }

    fn untyped(&self, basic: Basic, value: Value) -> Option<Operand> {
        Some(Operand {
            ty: self.types.basic(basic),
            value,
        })
    }

    /// `value` as a constant of the type `ty` holds it; `None` if the type
    /// cannot hold it.
    fn converted(&self, value: &Value, ty: TypeId) -> Option<Value> {
        representable(value, self.basic_of(ty)?)
    }

    /// The value of a constant expression; `None` for what is not constant
    /// or not worked out here yet.
    fn constant(&mut self, cx: &Cx<'a>, expr: ExprId) -> Option<Operand> {
        gopp_syntax::with_stack(|| self.constant_node(cx, expr))
    }

    fn constant_node(&mut self, cx: &Cx<'a>, expr: ExprId) -> Option<Operand> {
        let node = &cx.tree.exprs[expr];
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
            ExprKind::Paren(x) => self.constant(cx, *x),
            ExprKind::Name(_) | ExprKind::Selector { .. } => match *cx.body.uses.get(&expr)? {
                Ref::Universe(Predeclared::True) => {
                    self.untyped(Basic::UntypedBool, Value::Bool(true))
                }
                Ref::Universe(Predeclared::False) => {
                    self.untyped(Basic::UntypedBool, Value::Bool(false))
                }
                Ref::Universe(Predeclared::Iota) => {
                    self.untyped(Basic::UntypedInt, Value::Int64(cx.iota?))
                }
                Ref::Object(object)
                    if matches!(self.package.object(object).kind, ObjKind::Const) =>
                {
                    let ty = self.object_type(object);
                    let value = self.value[object.0 as usize].clone();
                    (value != Value::Unknown).then_some(Operand { ty, value })
                }
                Ref::Member { import, member } => {
                    let ObjKind::Import { package, .. } = &self.package.object(import).kind else {
                        return None;
                    };
                    let object = package.object(member);
                    let known =
                        matches!(object.kind, ObjKind::Const) && object.value != Value::Unknown;
                    known.then(|| Operand {
                        ty: object.ty,
                        value: object.value.clone(),
                    })
                }
                _ => None,
            },
            ExprKind::Unary { op, x } => self.unary(cx, *op, *x),
            ExprKind::Binary { op, x, y } => self.binary(cx, *op, *x, *y),
            ExprKind::Call { fun, args, .. } => self.call(cx, *fun, args),
            _ => None,
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
    /// is no value.
    fn fitted(&self, ty: TypeId, value: Value) -> Option<Operand> {
        let value = match self.is_untyped(ty) {
            true => value,
            false => self.converted(&value, ty)?,
        };
        (value != Value::Unknown).then_some(Operand { ty, value })
    }

    fn unary(&mut self, cx: &Cx<'a>, op: UnaryOp, x: ExprId) -> Option<Operand> {
        let x = self.constant(cx, x)?;
        let basic = self.basic_of(x.ty)?;
        let (op, bits) = match op {
            UnaryOp::Plus => (constant::UnaryOp::Plus, 0),
            UnaryOp::Neg => (constant::UnaryOp::Neg, 0),
            UnaryOp::Not => (constant::UnaryOp::Not, 0),
            // The complement of an unsigned number stays in its size.
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
            _ => return None,
        };
        self.fitted(x.ty, x.value.unary(op, bits))
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

    fn binary(&mut self, cx: &Cx<'a>, op: BinaryOp, x: ExprId, y: ExprId) -> Option<Operand> {
        let (mut x, mut y) = (self.constant(cx, x)?, self.constant(cx, y)?);
        if matches!(op, BinaryOp::Shl | BinaryOp::Shr) {
            // The count fits an unsigned integer and is small; a float
            // that is an integer shifts as one.
            let count = u32::try_from(y.value.to_int().as_i64()?).ok()?;
            let shifted = x.value.to_int();
            if count > 1023 - 1 + 52 || shifted.kind() != Kind::Int {
                return None;
            }
            let ty = match self.is_integer(x.ty) {
                true => x.ty,
                false if self.is_untyped(x.ty) => self.types.basic(Basic::UntypedInt),
                false => return None,
            };
            return self.fitted(ty, shifted.shift(op == BinaryOp::Shl, count));
        }
        // An untyped operand takes the type of the other; two typed ones
        // have the same type.
        let ty = match (self.is_untyped(x.ty), self.is_untyped(y.ty)) {
            (true, true) => self.larger_kind(x.ty, y.ty)?,
            (true, false) => {
                x.value = self.converted(&x.value, y.ty)?;
                y.ty
            }
            (false, true) => {
                y.value = self.converted(&y.value, x.ty)?;
                x.ty
            }
            (false, false) => (x.ty == y.ty).then_some(x.ty)?,
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
            let holds = Value::Bool(x.value.compare(compare, &y.value));
            return self.untyped(Basic::UntypedBool, holds);
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
            _ => return None,
        };
        self.fitted(ty, x.value.binary(op, &y.value))
    }

    /// A conversion of a constant, or a built-in function of constants.
    fn call(&mut self, cx: &Cx<'a>, fun: ExprId, args: &[ExprId]) -> Option<Operand> {
        let tree = cx.tree;
        let mut callee = fun;
        while let ExprKind::Paren(x) = tree.exprs[callee].kind {
            callee = x;
        }
        let &[arg] = args else { return None };
        let builtin = match cx.body.uses.get(&callee) {
            Some(Ref::Universe(name)) if name.kind() == PredeclaredKind::Func => Some(*name),
            _ => None,
        };
        if let Some(builtin) = builtin {
            let x = self.constant(cx, arg)?;
            return match (builtin, &x.value) {
                // The length of a constant string is a constant of type int.
                (Predeclared::Len, Value::Str(text)) => Some(Operand {
                    ty: self.types.basic(Basic::Int),
                    value: Value::Int64(text.bytes().len() as i64),
                }),
                (Predeclared::Real | Predeclared::Imag, _) if self.is_untyped(x.ty) => {
                    let complex = x.value.to_complex();
                    let part = match builtin {
                        Predeclared::Real => complex.real(),
                        _ => complex.imag(),
                    };
                    self.untyped(Basic::UntypedFloat, part)
                        .filter(|x| x.value != Value::Unknown)
                }
                _ => None,
            };
        }
        // `T(x)` for a type `T` of constants.
        let named = matches!(
            tree.exprs[callee].kind,
            ExprKind::Name(_) | ExprKind::Selector { .. }
        );
        let target = named.then(|| self.named_type(cx, callee)).flatten()?;
        let basic = self.basic_of(target)?;
        let x = self.constant(cx, arg)?;
        if let Some(value) = representable(&x.value, basic) {
            return (value != Value::Unknown).then_some(Operand { ty: target, value });
        }
        // An integer converts to the string of that character.
        if basic == Basic::String && self.is_integer(x.ty) {
            let code = x.value.as_i64().and_then(|code| u32::try_from(code).ok());
            let c = code
                .and_then(char::from_u32)
                .unwrap_or(char::REPLACEMENT_CHARACTER);
            let value = Value::string(c.encode_utf8(&mut [0; 4]).as_bytes());
            return Some(Operand { ty: target, value });
        }
        None
    }
}
