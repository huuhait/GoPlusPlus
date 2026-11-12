//! The built-in attributes: what each is written on and what it takes
//! (MAC-8, MAC-14). A name that is not built in is left for the macros.

use gopp_syntax::constraint::is_build;

use super::*;
use crate::types::Marks;

/// What an attribute is written on.
#[derive(Clone, Copy)]
enum Target<'t> {
    Func(Shape<'t>),
    Method(Shape<'t>),
    Type(Declared),
    Var,
    Const,
    Field,
}

/// A function or a method as its attributes see it.
#[derive(Clone, Copy)]
struct Shape<'t> {
    name: &'t str,
    body: bool,
    generic: bool,
    pointer_receiver: bool,
    params: usize,
    results: usize,
}

/// What a type declaration declares.
#[derive(Clone, Copy, PartialEq)]
enum Declared {
    Struct,
    Interface,
    Other,
    Alias,
}

/// The built-in attributes that take no argument, and `@Required`, which
/// has an error of its own for one.
const BARE: [&str; 14] = [
    "Unwind", "Packed", "Inline", "NoInline", "NoBounds", "Pinned", "Required", "Virtual",
    "Abstract", "Override", "Operator", "Right", "Convert", "Const",
];

const TAKING: [&str; 5] = ["Extern", "Export", "Align", "Extends", "If"];

/// The largest alignment that `@Align` takes.
const ALIGN_AT_MOST: i64 = 65536;

impl<'a> Checker<'a> {
    fn attr_name(&self, cx: &Cx<'a>, attr: &Attribute) -> &'a str {
        let name = attr.path.name.span;
        &cx.text[name.lo as usize..name.hi as usize]
    }

    /// The marks of a method that is declared with `attrs`.
    pub(super) fn marks(&self, cx: &Cx<'a>, attrs: &[Attribute]) -> Marks {
        let local = attrs.iter().filter(|attr| attr.path.package.is_none());
        local
            .filter_map(|attr| Marks::of(self.attr_name(cx, attr)))
            .fold(Marks::default(), Marks::with)
    }

    /// Checks the attributes of the top-level declaration `decl`.
    pub(super) fn decl_attributes(&mut self, cx: &Cx<'a>, decl: &'a ast::Decl) {
        if decl.attrs.is_empty() {
            return;
        }
        let tree = cx.tree;
        let target = match &decl.kind {
            DeclKind::Func {
                is_macro: false,
                receiver,
                name,
                type_params,
                ty,
                body,
            } => {
                let (params, results) = match &tree.exprs[*ty].kind {
                    ExprKind::FuncType {
                        params, results, ..
                    } => (params.len(), results.len()),
                    _ => (0, 0),
                };
                let pointer = |receiver: &Param| {
                    let mut ty = receiver.ty;
                    while let ExprKind::Paren(inner) = tree.exprs[ty].kind {
                        ty = inner;
                    }
                    matches!(
                        tree.exprs[ty].kind,
                        ExprKind::Unary {
                            op: UnaryOp::Star,
                            ..
                        }
                    )
                };
                let shape = Shape {
                    name: &cx.text[name.span.lo as usize..name.span.hi as usize],
                    body: body.is_some(),
                    generic: !type_params.is_empty(),
                    pointer_receiver: receiver.as_ref().is_some_and(pointer),
                    params,
                    results,
                };
                match receiver {
                    Some(_) => Target::Method(shape),
                    None => Target::Func(shape),
                }
            }
            DeclKind::Type { alias, ty, .. } => {
                Target::Type(match (&tree.exprs[*ty].kind, alias) {
                    (_, true) => Declared::Alias,
                    (ExprKind::StructType { .. }, _) => Declared::Struct,
                    (ExprKind::InterfaceType { .. }, _) => Declared::Interface,
                    _ => Declared::Other,
                })
            }
            DeclKind::Var { .. } => Target::Var,
            DeclKind::Const { .. } => Target::Const,
            // A macro reads its attributes itself.
            _ => return,
        };
        self.attributes(cx, &decl.attrs, target);
    }

    /// Checks the attributes of a field of a struct type.
    pub(super) fn field_attributes(&mut self, cx: &Cx<'a>, attrs: &[Attribute]) {
        if !attrs.is_empty() {
            self.attributes(cx, attrs, Target::Field);
        }
    }

    fn attributes(&mut self, cx: &Cx<'a>, attrs: &[Attribute], target: Target<'a>) {
        let has = |this: &Self, name: &str| {
            attrs
                .iter()
                .any(|other| other.path.package.is_none() && this.attr_name(cx, other) == name)
        };
        for attr in attrs {
            let name = self.attr_name(cx, attr);
            // The words of a build condition are no expressions.
            if is_build(attr, cx.text) {
                continue;
            }
            let built_in =
                attr.path.package.is_none() && (BARE.contains(&name) || TAKING.contains(&name));
            if !built_in {
                self.compile_time(cx, attr);
                continue;
            }
            if let Some((code, why)) = self.misplaced(name, target, &|name| has(self, name)) {
                let on = match target {
                    Target::Func(_) => "a function",
                    Target::Method(_) => "a method",
                    Target::Type(Declared::Struct) => "a struct type",
                    Target::Type(Declared::Interface) => "an interface type",
                    Target::Type(Declared::Alias) => "an alias",
                    Target::Type(Declared::Other) => "a type",
                    Target::Var => "a variable",
                    Target::Const => "a constant",
                    Target::Field => "a field",
                };
                let message = match why.is_empty() {
                    true => format!("cannot use @{name} on {on}"),
                    false => format!("cannot use @{name} on {on}: {why}"),
                };
                self.error(code, attr.span, message);
                continue;
            }
            self.attr_arguments(cx, attr, name);
        }
    }

    /// Why the attribute `name` is not written on `target`, where `has`
    /// says which other attributes are; `None` if it is.
    fn misplaced(
        &self,
        name: &str,
        target: Target<'_>,
        has: &dyn Fn(&str) -> bool,
    ) -> Option<(Code, String)> {
        use Target::*;
        let placement = Code::AttrPlacement;
        let goes = |on: &str| Some((placement, format!("it is written on {on}")));
        let bodiless = |shape: Shape<'_>, what: &str| {
            (!shape.body).then(|| (placement, format!("the {what} has no body")))
        };
        let pointer_method = |code: Code, shape: Shape<'_>| {
            (!shape.pointer_receiver)
                .then(|| (code, "the method needs a pointer receiver".to_owned()))
        };
        match (name, target) {
            ("Extern", Func(shape)) => {
                let bad = |why: &str| Some((Code::GirExternBad, why.to_owned()));
                match (shape.body, shape.generic) {
                    (true, _) => bad("a function that is defined in C has no body"),
                    (_, true) => bad("a function that is defined in C has no type parameters"),
                    _ => None,
                }
            }
            ("Extern", _) => goes("a function"),
            ("Export", Func(shape)) => {
                let bad = |why: &str| Some((Code::GirExportBad, why.to_owned()));
                match (shape.body, shape.generic, has("Extern")) {
                    (false, ..) => bad("a function that C calls has a body"),
                    (_, true, _) => bad("a function that C calls has no type parameters"),
                    (.., true) => bad("the function is defined in C"),
                    _ => None,
                }
            }
            ("Export", _) => goes("a function"),
            ("Unwind", Func(_)) => None,
            ("Unwind", _) => goes("a function"),
            ("Packed", Type(Declared::Struct)) => None,
            ("Packed", _) => goes("a struct type"),
            ("Align" | "Pinned", Type(declared)) if declared != Declared::Alias => None,
            ("Align" | "Pinned", _) => goes("a type"),
            ("Inline" | "NoInline", Func(shape) | Method(shape)) => {
                let both = name == "NoInline" && has("Inline");
                let conflict = || {
                    let why = "the function is marked @Inline too";
                    (Code::InlineMarksConflict, why.to_owned())
                };
                bodiless(shape, "function").or_else(|| both.then(conflict))
            }
            ("NoBounds", Func(shape) | Method(shape)) => bodiless(shape, "function"),
            ("Inline" | "NoInline" | "NoBounds", _) => goes("a function or a method"),
            ("Required", Field) => None,
            ("Required", _) => Some((
                Code::RequiredPlacement,
                "it is written on a field of a struct".to_owned(),
            )),
            ("Extends", Type(Declared::Struct | Declared::Interface)) => None,
            ("Extends", _) => goes("a struct type or an interface type"),
            ("Virtual" | "Override", Method(shape)) => pointer_method(placement, shape),
            ("Virtual" | "Override", _) => goes("a method"),
            ("Abstract", Method(shape)) => pointer_method(Code::AbstractAttrPlacement, shape),
            ("Abstract", Type(declared)) if declared != Declared::Alias => None,
            ("Abstract", _) => Some((
                Code::AbstractAttrPlacement,
                "it is written on a method or a type".to_owned(),
            )),
            ("Operator", Method(shape)) => {
                let known = operators::shape_of(shape.name).is_some();
                (!known).then(|| (placement, format!("{} is no operator method", shape.name)))
            }
            ("Operator", _) => goes("a method"),
            ("Right", Method(shape)) => {
                let binary = operators::two_operands(shape.name);
                let why = match (has("Operator"), binary) {
                    (false, _) => "the method is no @Operator method".to_owned(),
                    (_, false) => format!("{} takes no operand on its left", shape.name),
                    _ => return None,
                };
                Some((Code::RightMisuse, why))
            }
            ("Right", _) => Some((Code::RightMisuse, "it is written on a method".to_owned())),
            ("Convert", Method(shape)) => {
                let plain = !shape.pointer_receiver && shape.params == 0 && shape.results == 1;
                let why = "the method needs a value receiver, no parameters and one result";
                (!plain).then(|| (Code::ConvertMisuse, why.to_owned()))
            }
            ("Convert", _) => Some((Code::ConvertMisuse, "it is written on a method".to_owned())),
            ("Const", Func(shape) | Method(shape)) => {
                let other = ["Extern", "Virtual", "Abstract", "Override"]
                    .into_iter()
                    .find(|other| has(other));
                match other {
                    Some(other) => Some((placement, format!("the function is marked @{other}"))),
                    None if matches!(target, Method(_)) && shape.name == "drop" => {
                        Some((placement, "drop runs when a value ends".to_owned()))
                    }
                    None => None,
                }
            }
            ("Const", _) => goes("a function or a method"),
            ("If", Field) => goes("a declaration"),
            _ => None,
        }
    }

    /// The arguments of the built-in attribute `name`.
    fn attr_arguments(&mut self, cx: &Cx<'a>, attr: &Attribute, name: &str) {
        let args = attr.args.as_deref();
        let wrong = |this: &mut Self, code: Code, takes: &str| {
            this.error(code, attr.span, format!("@{name} takes {takes}"));
        };
        match (name, args) {
            ("Required", Some(_)) => wrong(self, Code::RequiredArgs, "no arguments"),
            (_, Some(_)) if BARE.contains(&name) => wrong(self, Code::AttrNoArgs, "no arguments"),
            (_, None) if BARE.contains(&name) => {}
            ("Extern" | "Export", args) => {
                let is_c = match args {
                    Some([language]) => match self.constant(cx, *language) {
                        Some((_, language)) => language == Value::string(b"C"),
                        _ => false,
                    },
                    _ => false,
                };
                if !is_c {
                    let code = match name {
                        "Extern" => Code::GirExternBad,
                        _ => Code::GirExportBad,
                    };
                    wrong(self, code, "the string \"C\"");
                }
            }
            ("Align", args) => {
                let aligned = match args {
                    Some([size]) => self.constant(cx, *size).and_then(|(_, size)| size.as_i64()),
                    _ => None,
                };
                let fits =
                    |size: i64| (1..=ALIGN_AT_MOST).contains(&size) && size & (size - 1) == 0;
                if !aligned.is_some_and(fits) {
                    let takes = format!("a constant power of two from 1 to {ALIGN_AT_MOST}");
                    wrong(self, Code::AttrArgs, &takes);
                }
            }
            ("If", args) => {
                let known = match args {
                    Some([condition]) => {
                        let x = self.operand(cx, *condition);
                        let is_bool = self
                            .basic_of(x.ty)
                            .is_some_and(|basic| matches!(basic, Basic::Bool | Basic::UntypedBool));
                        // What a macro gives is known when macros are.
                        x.mode == Mode::Unknown || matches!(x.mode, Mode::Const(_)) && is_bool
                    }
                    _ => false,
                };
                if !known {
                    wrong(self, Code::AttrArgs, "a constant bool");
                }
            }
            // Which type the parent is, the type that extends it says.
            ("Extends", Some([_])) => {}
            ("Extends", _) => wrong(self, Code::ExtendsOneParent, "one parent"),
            _ => {}
        }
    }

    /// The arguments of an attribute that a macro reads are known at
    /// compile time: none names a variable (MAC-14).
    fn compile_time(&mut self, cx: &Cx<'a>, attr: &Attribute) {
        let mut rest: Vec<ExprId> = attr.args.clone().unwrap_or_default();
        while let Some(expr) = rest.pop() {
            self.steps += 1;
            match &cx.tree.exprs[expr].kind {
                ExprKind::Name(name) => {
                    let variable = match cx.body.uses.get(&expr) {
                        Some(&Ref::Local(local)) => {
                            use LocalKind::*;
                            let kind = cx.body.locals[local.0 as usize].kind;
                            matches!(kind, Var | Receiver | Param | Result)
                        }
                        Some(&Ref::Object(object)) => {
                            matches!(self.package.object(object).kind, ObjKind::Var)
                        }
                        _ => false,
                    };
                    if variable {
                        let message = format!(
                            "cannot use {} in @{}: the arguments of an attribute are known at \
                             compile time, and {0} is a variable",
                            self.names.resolve(name),
                            self.attr_name(cx, attr)
                        );
                        self.error(Code::AttrArgs, cx.tree.exprs[expr].span, message);
                    }
                }
                ExprKind::Selector { x, .. } => {
                    let variable = match cx.body.uses.get(&expr) {
                        Some(&Ref::Member { import, member }) => {
                            match &self.package.object(import).kind {
                                ObjKind::Import { package, .. } => {
                                    matches!(package.object(member).kind, ObjKind::Var)
                                }
                                _ => false,
                            }
                        }
                        _ => {
                            rest.push(*x);
                            false
                        }
                    };
                    if variable {
                        let message = format!(
                            "cannot use {} in @{}: the arguments of an attribute are known at \
                             compile time, and {0} is a variable",
                            self.expr_string(cx, expr),
                            self.attr_name(cx, attr)
                        );
                        self.error(Code::AttrArgs, cx.tree.exprs[expr].span, message);
                    }
                }
                ExprKind::Paren(x) | ExprKind::Unary { x, .. } => rest.push(*x),
                ExprKind::Binary { x, y, .. } => rest.extend([*x, *y]),
                ExprKind::KeyValue { key, value } => rest.extend([*key, *value]),
                ExprKind::Index { x, indices } => {
                    rest.push(*x);
                    rest.extend(indices);
                }
                ExprKind::Call { fun, args, .. } => {
                    rest.push(*fun);
                    rest.extend(args);
                }
                ExprKind::CompositeLit { elems, .. } => rest.extend(elems),
                ExprKind::MacroCall { args, .. } => rest.extend(args),
                _ => {}
            }
        }
    }
}
